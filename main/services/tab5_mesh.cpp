#include "app.h"   // sistema_avisar
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM
#include "tab5_mesh.hpp"

#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "app_task.h"
#include "service_stop.hpp"      // parada cooperativa da tarefa (kernel)
#include "driver/uart.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "tab5_grove.hpp"
#include "tab5_power.hpp"
#include "notification.h"
#include "settings.h"
#include "tab5_mesh_ble.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_mesh_admin.hpp"
#include "tab5_mesh_reboot_log.hpp"
#include "tab5_mesh_wifi_node.hpp"
#include "mesh_pb.hpp"
#include "tab5_wifi.hpp"
#include "tab5_gps.hpp"
#include "tab5_time.hpp"

static const char *TAG = "mesh";

// NVS do proprio servico: o que o radio contou (nosso numero, canais,
// favoritos), separado das preferencias da pessoa (ajustes).
#define MESH_NVS "mesh"

#define MESH_UART   UART_NUM_1
#define MESH_RX     54
#define MESH_TX     53
#define MESH_BAUD   115200

// Sem frame por este tempo, consideramos o enlace caido.
// QUANTO SILENCIO ANTES DE DAR O ENLACE POR MUDO.
//
// Era 30s, e 30s e MENOS do que uma malha parada leva para falar. O radio nao
// diz nada por conta propria: telemetria dele sai a cada meia hora, e se
// ninguem transmite nada nao chega frame nenhum. Ou seja, numa malha quieta o
// enlace passava a maior parte do tempo marcado como "mudo" - com o C6L
// perfeitamente conectado. E "mudo" tinha consequencia: disparava um pedido de
// configuracao a cada 10s (ver a tarefa), e cada pedido REINICIA o download
// inteiro no radio. Ficavamos num ciclo de pedir configuracao para sempre, e
// enquanto o dump roda o trafego ao vivo espera na queue do radio - que e
// pequena e descarta o excedente. Mensagem que chega nessa janela some.
//
// 90s: ainda percebe um cabo arrancado em pouco mais de um minuto, e ja nao
// confunde "a malha esta quieta" com "o radio sumiu".
#define SILENCIO_MS 90000

// ---------------------------------------------------------------------------
// Protobuf: services/mesh_pb.hpp (o mesmo codec da ponte MQTT e do admin)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_trava = nullptr;

// VIDA DO SERVICO. Ate aqui ele so sabia nascer: uma vez de pe, segurava a UART
// do Grove para sempre. Trocar o C6L pelo GPS no cabo nao adiantava nada - o
// modulo novo estava la, mudo, com a porta na mao de quem ja tinha ido embora.
//
// Parada COOPERATIVA (kernel/service_stop.hpp), e nao vTaskDelete de fora: a
// tarefa pega mutex e escreve no cartao, e mata-la no meio disso deixaria o
// mutex preso e o arquivo aberto. Quem pede marca o pedido; a tarefa termina
// o que estava fazendo, confirma, e so entao a limpeza acontece.
static parada_t      s_parada = { };
static bool          s_servico_ativo = false;  // ha tarefa e enlace de pe
// POR ONDE: o cabo Grove ou o Bluetooth. Fixado no iniciar, pela
// preferencia (settings_mesh_transport) - so muda parando e subindo de novo.
static uint8_t       s_via = SETTINGS_MESH_TRANSPORT_SERIAL;
EXT_RAM_BSS_ATTR static mesh_no_t   s_nos[MESH_MAX_NOS];
static uint32_t    s_despejos = 0;   // quantas vezes achar_no() despejou o mais antigo desde o boot
EXT_RAM_BSS_ATTR static mesh_msg_t  s_msgs[MESH_MAX_MSGS];
static int         s_n_msgs = 0;
// TOTAL DE VERDADE, NUNCA SATURA. Sintoma: mensagem nova so aparecia depois
// de trocar de tela e voltar. app_mesh.cpp detectava
// "chegou mensagem nova" comparando tab5_mesh_num_msgs() (que devolve
// s_n_msgs) contra o valor da ultima passada - mas s_n_msgs e o tamanho do
// ANEL de 8 posicoes (MESH_MAX_MSGS), que para de crescer assim que 8
// mensagens (de QUALQUER conversa) ja passaram por ele. Depois disso o
// numero fica parado para sempre, e o comparador nunca mais via diferenca -
// so trocar de tela forcava outros caminhos (s_hist_pronto=false direto) a
// recarregar. Este contador so cresce, nunca reseta a MESH_MAX_MSGS.
static uint32_t     s_total_msgs = 0;
static int         s_nao_lidas = 0;
static int         s_nao_lidas_dir = 0;
static mesh_saude_t s_saude = { };
static uint32_t    s_meu_num = 0;
static char        s_firmware[24] = { 0 };
static uint32_t    s_ultimo_quadro_ms = 0;
static uint32_t    s_iniciado_em_ms   = 0;   // ver o aviso em tab5_mesh_silencio_ms()
static int         s_notif = -1;   // fonte no servico de notificacao

// DE QUAL REDE E O QUADRO QUE ESTA SENDO LIDO AGORA. So a tarefa do servico
// le quadros (sempre com a trava), e o da fila da internet e lido com isto em
// MESH_REDE_INTERNET: achar_no marca a rede do no, guardar_msg poe a mensagem na
// conversa "LongFast Internet". Mais simples que levar a rede por cada funcao do
// caminho de leitura, que e o mesmo para as duas.
static uint8_t     s_rede_rx = MESH_REDE_NO;

// NOMES REAIS DOS CANAIS, ditos pelo proprio radio.
//
// Antes o canal 0 era "LongFast" cravado no codigo e o resto virava "Canal 1",
// "Canal 2". Mas o Meshtastic MANDA a configuracao dos canais no dump inicial
// (FromRadio campo 10) - com o nome que voce escolheu no celular. Ignorar isso
// era inventar um rotulo quando o certo estava chegando pela serial.
static char s_canal_nome[MESH_MAX_CANAIS][MESH_NOME_CONVERSA];

// SE O CANAL ESTA ATIVO AGORA, pela ULTIMA config que o radio mandou - nasceu
// junto de "canais disponiveis" (ver mais abaixo). Antes disto
// nada guardava esse estado: s_canal_nome[] so era ESCRITO quando o papel
// vinha != 0 (ver o parser do campo 10 abaixo), mas nunca ficava sabendo que
// um canal tinha sido DESLIGADO - o nome antigo so ficava parado la,
// indistinguivel de "ainda ativo". Comeca tudo false: so vira true quando a
// PRIMEIRA config chegar, e nenhum canal deveria ser oferecido como
// "disponivel" antes disso (nao sabemos nada sobre ele ainda).
static bool s_canal_ativo[MESH_MAX_CANAIS];
static bool s_canais_conhecidos = false;   // ha configuracao (do radio ou da NVS)
static bool s_canais_lidos      = false;   // a NVS ja foi consultada
static bool s_canais_sujos      = false;   // mudou e ainda nao foi para a NVS
static void recordar_canais(void);

// O radio avisou que reiniciou (FromRadio campo 8). Sem isto, a gente so
// percebia pelo silencio - e esperava o prazo inteiro de 10s para so entao
// pedir a configuracao de novo.
static volatile bool s_radio_reiniciou = false;
static mesh_conversa_t s_conv[MESH_MAX_CONVERSAS];

// ORDEM DA COLUNA (ver tab5_mesh_conversas_versao no .hpp). 's_conv_seq' e a
// ultima atividade da conversa NESTA sessao, num contador que so cresce -
// nao o rx_time, que vale 0 quando o relogio ainda nao foi acertado e
// empataria tudo. Conversa so lida do cartao fica com 0: vai para o fim, na
// ordem da pasta.
static uint32_t s_conv_seq[MESH_MAX_CONVERSAS];
static uint32_t s_seq = 0;
static uint32_t s_conv_ver = 0;
static int      visivel_de(int interno);

// NAO LIDAS QUE SOBREVIVEM AO SERVICO. Parar o servico (troca de via, Wi-Fi
// caindo no modo internet, Bluetooth caindo, C6L achado no Grove) zera
// s_conv[] - e com ele as nao lidas. Visto ao vivo: o aviso da barra SUMIA
// sem ninguem ter aberto o Mesh, so porque o servico subiu de novo por outra
// via. Aqui fica a contagem de cada conversa ate ela ser LIDA de verdade;
// achar_ou_criar_conversa() devolve o numero quando a conversa renasce.
typedef struct { bool usada; bool channel; uint8_t idx; uint32_t no; uint16_t n; } nao_lida_guardada_t;
static nao_lida_guardada_t s_guardadas[MESH_MAX_CONVERSAS];

static nao_lida_guardada_t* guardada_de(bool canal, uint8_t idx, uint32_t no, bool criar)
{
  nao_lida_guardada_t* livre = nullptr;
  for (auto& g : s_guardadas) {
    if (!g.usada) { if (!livre) { livre = &g; } continue; }
    if (canal && g.channel && g.idx == idx) { return &g; }
    if (!canal && !g.channel && g.no == no) { return &g; }
  }
  if (!criar || !livre) { return nullptr; }
  *livre = { true, canal, idx, no, 0 };
  return livre;
}

// Com a trava tomada (ou com o servico parado). Fotografa as conversas vivas.
static void guardar_nao_lidas(void)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    const mesh_conversa_t& c = s_conv[i];
    if (!c.usada) { continue; }
    nao_lida_guardada_t* g = guardada_de(c.channel, c.idx, c.no, c.nao_lidas > 0);
    if (!g) { continue; }
    if (c.nao_lidas > 0) { g->n = (uint16_t)(c.nao_lidas > 0xFFFF ? 0xFFFF : c.nao_lidas); }
    else                 { g->usada = false; }
  }
}

static bool ha_nao_lidas_guardadas(void)
{
  for (const auto& g : s_guardadas) { if (g.usada && g.n) { return true; } }
  return false;
}

#include <dirent.h>
#include "nvs.h"
#include "tab5_monitor.hpp"   // tab5_mem_marco

#define MESH_PASTA "Meshtastic"

// Contadores de diagnostico. Existem porque "nos 0/0" nao diz se o LocalStats
// nao CHEGA ou se chega e o filtro descarta - e sao perguntas com conserto oposto.
static uint32_t s_c_quadros = 0, s_c_pacotes = 0, s_c_telemetria = 0;
static uint32_t s_c_localstats = 0, s_c_texto = 0, s_c_nodeinfo = 0;
static int      s_ultima_variante = -1;   // ultimo campo visto dentro de Telemetry

// Quantas vezes um Routing chegou DEPOIS do pacote ja estar ENTREGUE (ver
// marcar_envio). Nao muda o estado mostrado - "confirmado" continua valendo -
// mas e um numero com finalidade propria: se ele cresce rapido, quer dizer
// que a confirmacao "alguem ouviu" esta vindo facil e a confirmacao forte do
// radio (fim a fim) esta vindo dificil - ou seja, a malha ESTA funcionando,
// so que no limite do alcance. E um termometro de qualidade de rota que nao
// existiria se o segundo Routing fosse so descartado sem contar.
static uint32_t s_c_routing_extra = 0;


// ---------------------------------------------------------------------------
// OS ENVIOS EM VOO
// ---------------------------------------------------------------------------
// Anel dos ultimos pacotes que NOS mandamos, com o estado de cada um. Fica so
// em RAM de proposito: o cartao guarda o id junto da mensagem, mas nao o
// estado - reabrir o app amanha e ver "enviando" num pacote de ontem seria
// mentira. Id que nao esta mais aqui devolve MESH_ENVIO_NA, e a tela nao
// desenha marca nenhuma.
#define MESH_MAX_ENVIOS 16

// Quanto esperar pela confirmacao antes de desistir. O radio retransmite
// sozinho algumas vezes e, quando desiste, ELE avisa com Routing.MAX_RETRANSMIT
// - este prazo e so a rede de seguranca para quando o enlace morre no meio.
#define MESH_ACK_MS 60000

typedef struct {
  uint32_t id;
  uint32_t para;     // destino: decide o que um ACK quer dizer (ver marcar_envio)
  uint32_t em_ms;
  uint8_t  state;
  uint8_t  erro;
} envio_t;

static envio_t  s_envios[MESH_MAX_ENVIOS];
static int      s_prox_envio = 0;
static uint32_t s_envios_ver = 0;

static void anotar_envio(uint32_t id, uint32_t para)
{
  if (id == 0) { return; }

  // Ja acompanhado? Nao duplica. Importa porque agora ha DOIS caminhos que
  // chamam isto - o nosso envio e o eco de uma mensagem que saiu do celular -,
  // e a mesma mensagem pode passar pelos dois.
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id == id) { return; }
  }

  envio_t* e = &s_envios[s_prox_envio];
  s_prox_envio = (s_prox_envio + 1) % MESH_MAX_ENVIOS;
  e->id     = id;
  e->para   = para;
  e->em_ms  = tab5_now_ms();
  e->state = MESH_ENVIANDO;
  e->erro   = 0;
  s_envios_ver++;
  sistema_avisar(AVISO_MESH_ENVIO);
}

// ---------------------------------------------------------------------------
// O DESFECHO VAI PARA O CARTAO. O caminho, nao.
// ---------------------------------------------------------------------------
// A primeira versao guardava o estado so em RAM, com o argumento de que
// "reabrir amanha e ver ENVIANDO num pacote de ontem seria mentira". O
// argumento vale - para ENVIANDO. Nao vale para o resto.
//
// "Falhou por falta de rota" e um FATO FINAL: ele nao fica mais nem menos
// verdadeiro com o tempo, e some da tela so porque o anel de 16 envios girou ou
// o aparelho reiniciou. Quem mandou a mensagem quer saber que ela nao chegou,
// hoje e semana que vem.
//
// Entao a divisao e por NATUREZA do estado, nao por comodidade:
//   ENVIANDO      - passageiro, so RAM. Nunca gravado.
//   ENTREGUE      - final, gravado.
//   FALHOU        - final, gravado, com o motivo.
//   SEM_RESPOSTA  - final ("desisti de esperar"), gravado.
//
// Arquivo PROPRIO e nao a linha da mensagem: a mensagem e gravada no instante
// do envio, quando o desfecho ainda nao existe. Reescrever aquela linha depois
// significaria reescrever o arquivo inteiro a cada ACK. Aqui e so acrescentar.
#define MESH_ARQ_ENVIOS "envios.txt"

static void gravar_desfecho(uint32_t id, uint8_t estado, uint8_t erro)
{
  if (id == 0) { return; }
  char linha[40];
  snprintf(linha, sizeof(linha), "E,%08lX,%u,%u\n", (unsigned long)id, (unsigned)estado, (unsigned)erro);
  tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_ENVIOS, linha);   // gravador do sistema (hal/tab5_sd.hpp)
}

// ---------------------------------------------------------------------------
// CARIMBO CORRIGIDO - quando o relogio salta (ver tab5_time.hpp)
// ---------------------------------------------------------------------------
// A mensagem enviada leva a hora do NOSSO relogio. Se ele estava errado e uma
// fonte o acerta depois, a mensagem ficava no dia errado para sempre: a linha
// dela ja esta no cartao. Reescrever o arquivo da conversa por isso seria
// caro; em vez disso vai uma linha "H,<id>,<quando>" para o envios.txt, o
// mesmo arquivo so de acrescentar dos desfechos, e o leitor do historico
// troca o carimbo de quem tem id la (ultima linha vence, como no "E").
//
// So as mensagens DESTA sessao: as de antes foram carimbadas por outro
// relogio (outro boot), que pode ter estado certo.
#define MESH_CARIMBADAS 16
typedef struct { uint32_t id; uint32_t quando; } carimbada_t;
static carimbada_t s_carimbadas[MESH_CARIMBADAS];
static int         s_prox_carimbada = 0;

static void travar(void);
static void destravar(void);

// Com a trava tomada.
static void lembrar_carimbo(uint32_t id, uint32_t quando)
{
  if (id == 0 || quando == 0) { return; }
  s_carimbadas[s_prox_carimbada].id     = id;
  s_carimbadas[s_prox_carimbada].quando = quando;
  s_prox_carimbada = (s_prox_carimbada + 1) % MESH_CARIMBADAS;
}

// Tarefa de interface, logo depois do acerto do relogio. So RAM e o gravador
// do sistema (tab5_sd_enqueue_append copia e volta): nada de cartao aqui.
static void ao_saltar_relogio(int32_t delta)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_CARIMBADAS; ++i) {
    carimbada_t* c = &s_carimbadas[i];
    if (c->id == 0) { continue; }
    c->quando = (uint32_t)((int64_t)c->quando + delta);
    for (int k = 0; k < s_n_msgs; ++k) {
      if (s_msgs[k].id == c->id) { s_msgs[k].quando = c->quando; }
    }
    char linha[40];
    snprintf(linha, sizeof(linha), "H,%08lX,%lu\n", (unsigned long)c->id, (unsigned long)c->quando);
    tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_ENVIOS, linha);
    ++n;
  }
  // O app rele o historico quando este contador muda: e o que tira da tela
  // o balao no dia errado.
  if (n) { ++s_total_msgs; }
  destravar();
  if (n) {
    ESP_LOGI(TAG, "hora do aparelho corrigida em %+ld s: horario de %d mensagem(ns) enviada(s) "
                  "antes do acerto foi ajustado", (long)delta, n);
    sistema_avisar(AVISO_MESH_MSG);
  }
}

// Desfechos lidos do cartao, UMA VEZ por carga de historico.
//
// A primeira versao chamava uma funcao "procura este id no arquivo" de DENTRO
// do loop que le as mensagens. Com 40 mensagens na conversa isso virava 40
// aberturas e 40 varreduras completas do envios.txt, no cartao, com dois
// arquivos abertos ao mesmo tempo e o usuario esperando a conversa aparecer.
// Custo O(mensagens x desfechos) para responder uma pergunta que cabe em 1KB
// de RAM.
//
// Agora o arquivo e lido uma vez para esta tabela e a consulta e em memoria.
#define MESH_DESFECHOS 64

typedef struct {
  uint32_t id;
  uint8_t  state;
  uint8_t  erro;
  uint32_t quando;   // carimbo corrigido ("H"); 0 = o da linha da mensagem vale
} desfecho_t;

// ULTIMO VENCE. O arquivo so cresce, entao a linha mais recente de um id e a
// verdade atual - um pacote pode ir de SEM_RESPOSTA para ENTREGUE quando um ACK
// chega atrasado. Por isso sobrescrevemos a entrada em vez de parar na primeira.
//
// Tabela cheia: descarta a mais ANTIGA (anel). O arquivo e cronologico, entao o
// que sobra sao os 64 desfechos mais recentes - e o historico so mostra as
// ultimas dezenas de mensagens de qualquer jeito.
static int carregar_desfechos(desfecho_t* tab, int max)
{
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_ENVIOS, "r");
  if (!f) { return 0; }

  int n = 0, prox = 0;
  char linha[64];
  int voltas = 0;
  while (fgets(linha, sizeof(linha), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // nao segurar o core
    unsigned long lid = 0, lq = 0; unsigned est = 0, err = 0;
    const bool eh_hora = (sscanf(linha, "H,%8lX,%lu", &lid, &lq) == 2);
    if (!eh_hora && sscanf(linha, "E,%8lX,%u,%u", &lid, &est, &err) != 3) { continue; }

    int alvo = -1;
    for (int i = 0; i < n; ++i) { if (tab[i].id == (uint32_t)lid) { alvo = i; break; } }
    if (alvo < 0) {
      if (n < max) { alvo = n++; }
      else         { alvo = prox; prox = (prox + 1) % max; }
      tab[alvo].state = MESH_ENVIO_NA;
      tab[alvo].erro   = 0;
      tab[alvo].quando = 0;
    }
    tab[alvo].id = (uint32_t)lid;
    if (eh_hora) { tab[alvo].quando = (uint32_t)lq; continue; }
    tab[alvo].state = (uint8_t)est;
    tab[alvo].erro   = (uint8_t)err;
  }
  fclose(f);
  return n;
}

static uint32_t buscar_carimbo(const desfecho_t* tab, int n, uint32_t id)
{
  if (id == 0) { return 0; }
  for (int i = 0; i < n; ++i) { if (tab[i].id == id) { return tab[i].quando; } }
  return 0;
}

static uint8_t buscar_desfecho(const desfecho_t* tab, int n, uint32_t id, uint8_t* erro)
{
  if (id == 0) { return MESH_ENVIO_NA; }
  for (int i = 0; i < n; ++i) {
    if (tab[i].id != id) { continue; }
    if (erro) { *erro = tab[i].erro; }
    return tab[i].state;
  }
  return MESH_ENVIO_NA;
}

// O QUE UM ROUTING QUER DIZER depende de QUEM o mandou ('de') e para onde ia
// o nosso pacote. Silencioso quando o id nao e nosso: a malha esta cheia de
// roteamento que nao nos diz respeito.
//
// O no gera um ACK IMPLICITO quando ouve um vizinho repetir o nosso pacote:
// um Routing com erro 0 vindo DELE MESMO (de == nosso numero). Ele prova que
// a malha ouviu - e, por economia de ar, o no PARA de retransmitir ali.
//   - DIFUSAO: nao ha destinatario para confirmar; o implicito e o melhor que
//     existe, e vale ENTREGUE (o app oficial faz o mesmo).
//   - DIRETA: o implicito e meio caminho - vale OUVIDO. ENTREGUE so com o ACK
//     do PROPRIO destinatario (de == para).
//
// Antes os dois eram iguais: a direta virava "confirmada" no primeiro eco de
// um vizinho, e quando o no depois relatava que o destino nunca respondeu
// (erro 5, MAX_RETRANSMIT), a trava de "ENTREGUE e terminal" engolia o
// relato. A tela dizia entregue para mensagem que nao chegou.
//
// O IMPLICITO DO MQTT NAO E "OUVIDO". Com o C6L ligado na internet, o
// firmware (src/mqtt/MQTT.cpp) gera um ACK implicito para si mesmo quando o
// SERVIDOR devolve o nosso proprio pacote - igual ao de um vizinho, so que
// marcado transport_mechanism = MQTT. Ele prova que o pacote chegou ao
// servidor, nao que algum radio o ouviu. Contado como OUVIDO, uma direta para
// um no DESLIGADO ganhava o check cinza na hora e, como OUVIDO nao desce, o
// "tentou demais" que o radio manda depois era engolido: o X so aparecia com
// a internet desligada. O proprio firmware ja ignora esse ACK para parar as
// retransmissoes (ReliableRouter::sniffReceived); aqui ele tambem nao muda a
// marca de uma direta. Na difusao continua valendo ENTREGUE - quem esta no
// servidor recebe, e e a mesma regra de antes.
static void marcar_envio(uint32_t id, int erro, uint32_t de, bool so_mqtt = false)
{
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    envio_t* e = &s_envios[i];
    if (e->id != id || id == 0) { continue; }

    const bool difusao = (e->para == 0xFFFFFFFFu);
    if (so_mqtt && erro == 0 && !difusao && de != e->para) {
      ESP_LOGI(TAG, "mensagem %08lX: chegou ao servidor MQTT - esperando o destino", (unsigned long)id);
      return;
    }
    uint8_t novo;
    if (erro == 0) {
      novo = (difusao || de == e->para) ? MESH_ENTREGUE : MESH_OUVIDO;
    } else {
      novo = MESH_FALHOU;
    }

    // ENTREGUE e terminal: nada posterior o desfaz. OUVIDO so sobe (para
    // ENTREGUE); um erro depois dele nao vira "falhou" - a malha OUVIU, o que
    // faltou foi a confirmacao do destino, e a marca de OUVIDO ja diz isso.
    if (e->state == MESH_ENTREGUE || (e->state == MESH_OUVIDO && novo != MESH_ENTREGUE)) {
      if (erro != 0) {
        ++s_c_routing_extra;   // ver o comentario da declaracao - nao e so ruido
        ESP_LOGI(TAG, "mensagem %08lX: aviso '%s' chegou depois de %s - continua assim", (unsigned long)id,
                 mesh_erro_nome((uint8_t)erro), e->state == MESH_ENTREGUE ? "entregue" : "ouvido");
      }
      return;
    }
    if (e->state == novo && e->erro == (uint8_t)erro) { return; }
    e->state = novo;
    e->erro   = (uint8_t)erro;
    s_envios_ver++;
    sistema_avisar(AVISO_MESH_ENVIO);
    gravar_desfecho(id, novo, (uint8_t)erro);
    ESP_LOGI(TAG, "mensagem %08lX: %s%s", (unsigned long)id,
             novo == MESH_ENTREGUE ? "entregue ao destino" : novo == MESH_OUVIDO ? "ouvida por outro no (sem confirmacao do destino)" : "FALHOU - ",
             novo == MESH_FALHOU ? mesh_erro_nome((uint8_t)erro) : "");
    return;
  }
}

uint8_t tab5_mesh_estado_envio(uint32_t id, uint8_t* erro)
{
  if (id == 0) { return MESH_ENVIO_NA; }
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id != id) { continue; }
    if (erro) { *erro = s_envios[i].erro; }
    return s_envios[i].state;
  }
  return MESH_ENVIO_NA;
}

uint32_t tab5_mesh_envios_versao(void) { return s_envios_ver; }

const char* mesh_erro_nome(uint8_t erro)
{
  // Routing.Error do meshtastic. So os que da para acontecer aqui; a lista
  // cresce a cada versao do firmware e o resto cai no "?".
  switch (erro) {
    case 0:   return "ok";
    case 1:   return "sem rota";
    case 2:   return "recusado";
    case 3:   return "sem resposta";
    case 4:   return "sem interface";
    case 5:   return "tentou demais";
    case 6:   return "canal errado";
    case 7:   return "grande demais";
    case 8:   return "ninguém respondeu";
    case 9:   return "limite de transmissão";
    case 32:  return "pedido inválido";
    case 33:  return "sem autorização";
    case 34:  return "falha de chave";
    case 35:  return "chave desconhecida";
    case 38:  return "rápido demais";
    case MESH_ERRO_FILA: return "fila do rádio cheia";
    default:  return "?";
  }
}

// ---------------------------------------------------------------------------
// QUE HORAS SAO - emprestadas do radio
// ---------------------------------------------------------------------------
// O Tab5 acorda sem hora certa, mas todo NodeInfo traz last_heard e todo pacote
// traz rx_time, os dois em segundos de epoch e vindos do relogio do C6L.
// Guardamos o maior carimbo ja visto junto com o nosso uptime naquele instante;
// dai em diante a hora anda com o nosso relogio, e a conta de "ha quanto tempo"
// fecha com erro de segundos.
//
// O piso de 2020 e para nao aceitar carimbo de radio que ainda nao sincronizou
// (ele manda 0, ou um numero pequeno de uptime) e ancorar a referencia no ano
// de 1970 - o que faria todo no aparecer visto ha 56 anos.
#define EPOCH_MINIMO 1577836800u          // 2020-01-01

static uint32_t s_epoch_ref    = 0;       // carimbo do radio
static uint32_t s_epoch_ref_ms = 0;       // nosso uptime quando ele chegou

uint32_t tab5_mesh_epoch(void)
{
  if (s_epoch_ref == 0) { return 0; }
  return s_epoch_ref + (tab5_now_ms() - s_epoch_ref_ms) / 1000;
}

// So aceita carimbo que ADIANTE o relogio. Assim um last_heard antigo, do dump
// inicial, nunca puxa a referencia para tras.
static void marcar_epoch(uint32_t carimbo)
{
  if (carimbo < EPOCH_MINIMO) { return; }
  const uint32_t agora = tab5_mesh_epoch();
  if (carimbo <= agora) { return; }
  s_epoch_ref    = carimbo;
  s_epoch_ref_ms = tab5_now_ms();
}

// ---------------------------------------------------------------------------
// HORA AO VIVO - a unica deste arquivo que acerta o relogio do sistema
// ---------------------------------------------------------------------------
// tab5_mesh_epoch() acima serve para "ha quanto tempo", e so. Ancorada no
// MAIOR last_heard da lista de nos, ela fica ATRASADA quanto for o silencio
// da malha - o no mais recente pode ter falado ontem. Usada como "agora" ela
// acertava o relogio com esse atraso: a mensagem enviada saia carimbada no
// dia errado, e o "HOJE" da conversa pulava para o dia certo so quando uma
// resposta de verdade chegava.
//
// Aqui entra so carimbo que e "agora" por construcao:
//   - rx_time de pacote que chegou DEPOIS de o no terminar o dump e esvaziar
//     a queue do que guardou enquanto estavamos desconectados (esses tem o
//     rx_time de quando ELE recebeu, minutos ou horas atras);
//   - o campo 'time' da telemetria do nosso proprio no, pelo mesmo filtro.
// O rx_time e sempre do relogio do NOSSO no (ele carimba ao receber), mesmo
// para pacote que atravessou a malha ou veio pela ponte MQTT.
#define QUEUE_DRENADA_MS 10000u            // depois do fim do dump
#define HORA_VIVA_VALE_MS (60u * 60u * 1000u)

static uint32_t s_config_ok_ms = 0;        // fim do dump; 0 = ainda rodando
static uint32_t s_vivo_ref     = 0;
static uint32_t s_vivo_ref_ms  = 0;

static bool trafego_ao_vivo(void)
{
  return s_config_ok_ms != 0 && (tab5_now_ms() - s_config_ok_ms) > QUEUE_DRENADA_MS;
}

static void marcar_hora_viva(uint32_t carimbo)
{
  if (carimbo < EPOCH_MINIMO || !trafego_ao_vivo()) { return; }
  s_vivo_ref    = carimbo;
  s_vivo_ref_ms = tab5_now_ms();
}

uint32_t tab5_mesh_hora_viva(void)
{
  const uint32_t ref = s_vivo_ref, ms = s_vivo_ref_ms;
  if (ref == 0 || (tab5_now_ms() - ms) > HORA_VIVA_VALE_MS) { return 0; }
  return ref + (tab5_now_ms() - ms) / 1000;
}

static void travar(void)   { if (s_trava) xSemaphoreTake(s_trava, portMAX_DELAY); }
static void destravar(void){ if (s_trava) xSemaphoreGive(s_trava); }

bool     tab5_mesh_no_ar(void)       { return (tab5_now_ms() - s_ultimo_quadro_ms) < SILENCIO_MS
                                           && s_ultimo_quadro_ms != 0; }

// HA QUANTO TEMPO O RADIO NAO FALA, em ms. UINT32_MAX = nunca falou.
//
// Existe porque "o enlace esta vivo?" e "alguem arrancou o modulo do cabo?" sao
// perguntas diferentes e querem prazos diferentes. O no_ar() e generoso de
// proposito (90s) para nao chamar de morta uma malha que so esta quieta. A
// vigia do Grove precisa do numero CRU para escolher o proprio prazo - e, com
// ela apoiada no no_ar(), subir o SILENCIO_MS de 30s para 90s empurrou a
// deteccao de troca de modulo de 90s para 150s sem ninguem pedir.
uint32_t tab5_mesh_silencio_ms(void)
{
  if (s_ultimo_quadro_ms == 0) {
    // NENHUM frame AINDA - mas pode ser so porque o servico acabou de
    // subir, nao porque o radio sumiu. MESMO BUG do GPS (ver o aviso grande
    // em tab5_gps_silence_ms(), achado em hardware):
    // UINT32_MAX incondicional faz a vigia do Grove (kernel/core.cpp) enxergar
    // "mudo ha 60s" no PRIMEIRO tique (ate 2s) depois de QUALQUER reinicio,
    // sem dar ao radio nem os 60s de MUDO_MS que o codigo pretendia dar. Na
    // pratica o C6L costuma responder rapido o bastante para nunca bater
    // nessa janela - mas a malha jamais teve garantia disso, e um radio
    // lento para reconectar (boot frio, ruido) cairia direto no mesmo loop
    // de reinicio a cada 2s que o GPS mostrou. Contar a partir de QUANDO O
    // SERVICO SUBIU, e nao um sentinela fixo, fecha o buraco.
    return s_servico_ativo ? (tab5_now_ms() - s_iniciado_em_ms) : UINT32_MAX;
  }
  return tab5_now_ms() - s_ultimo_quadro_ms;
}
uint32_t tab5_mesh_meu_numero(void)  { return s_meu_num; }
const char* tab5_mesh_firmware(void) { return s_firmware; }

void tab5_mesh_definir_beep(bool tocar)
{
  settings_set_mesh_beep(tocar);
  // Se a fonte ainda nao existe (servico nunca subiu nesta sessao), nao ha
  // bandeira para atualizar agora - tab5_mesh_iniciar() aplica a preferencia
  // gravada no instante em que a fonte nascer, ver acima.
  if (s_notif >= 0) { notif_silenciar(s_notif, !tocar); }
}

bool tab5_mesh_beep_ligado(void) { return settings_mesh_beep(); }
int      tab5_mesh_nao_lidas(void)   { return s_nao_lidas; }
int      tab5_mesh_nao_lidas_diretas(void) { return s_nao_lidas_dir; }
static void publicar_notif(void);   // definida adiante, junto das outras


void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* texto, uint32_t* nodeinfo,
                          int* ultima_variante)
{
  if (quadros)    *quadros    = s_c_quadros;
  if (pacotes)    *pacotes    = s_c_pacotes;
  if (telemetria) *telemetria = s_c_telemetria;
  if (localstats) *localstats = s_c_localstats;
  if (texto)      *texto      = s_c_texto;
  if (nodeinfo)   *nodeinfo   = s_c_nodeinfo;
  if (ultima_variante) *ultima_variante = s_ultima_variante;
}

// Ver o comentario da declaracao de s_c_routing_extra: nao e ruido, e um
// termometro de qualidade de rota - cresce quando "alguem ouviu" chega facil
// mas a confirmacao forte do radio nao chega.
uint32_t tab5_mesh_routing_extra(void) { return s_c_routing_extra; }

void tab5_mesh_saude(mesh_saude_t* s) { if (s) { travar(); *s = s_saude; destravar(); } }

int tab5_mesh_num_nos(void)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_NOS; ++i) { if (s_nos[i].usado) { ++n; } }
  destravar();
  return n;
}

uint32_t tab5_mesh_despejos(void) { return s_despejos; }

// Ha quantos segundos o no foi ouvido; -1 = nao da para saber. Duas fontes,
// nesta ordem:
//   1. nosso proprio avistamento - exato, nao depende de relogio nenhum;
//   2. o last_heard do radio contra a hora emprestada dele.
static int32_t idade_do_no(const mesh_no_t& no)
{
  const uint32_t ep = tab5_mesh_epoch();
  if (no.visto_local_ms != 0) { return (int32_t)((tab5_now_ms() - no.visto_local_ms) / 1000); }
  if (no.visto_em >= EPOCH_MINIMO && ep > no.visto_em) { return (int32_t)(ep - no.visto_em); }
  return -1;
}

// A ORDEM DA TABELA, a mesma do cliente web: o nosso no primeiro, depois do
// ouvido mais recentemente ao mais antigo, e os sem idade conhecida no fim.
// Antes era a ordem das vagas do vetor - um no que acabou de falar podia
// estar no fim da lista, e a lista "embaralhava" a cada despejo. SEM TRAVA.
static uint32_t s_prox_fixado = 0;
static int      fav_pos(uint32_t num);   // favoritos, mais abaixo

// LUGAR FIXO dado NA CHEGADA do dado (pacote ou NodeInfo com SNR de radio), e
// nao quando a tela pergunta: e ele que protege o no do despejo (achar_no),
// e o despejo acontece com a tela fechada tambem. SEM TRAVA.
static void fixar_se_pelo_ar(mesh_no_t* no)
{
  if (no && !no->fixado && no->num != s_meu_num && tab5_mesh_no_pelo_ar(no)) {
    no->fixado = ++s_prox_fixado;
  }
}

// ORDEM EM CACHE. Com 256 vagas, ordenar a cada tab5_mesh_no(i) - uma vez por
// linha desenhada - viraria milhoes de comparacoes por segundo. A ordem so
// muda quando a tabela muda (s_nos_ver) ou quando o tempo passa (a idade da
// faixa de baixo): refeita no maximo uma vez por segundo, ou na hora se algo
// chegou. Com a trava tomada.
static uint32_t s_nos_ver = 0;
EXT_RAM_BSS_ATTR static int s_ordem[MESH_MAX_NOS];
static int      s_ordem_n = 0;
EXT_RAM_BSS_ATTR static int s_ordem_rede[2][MESH_MAX_NOS];   // a mesma ordem, separada por rede
static int      s_ordem_rede_n[2] = { 0, 0 };
static uint32_t s_ordem_ver = UINT32_MAX;
static uint32_t s_ordem_ms = 0;

static int ordem_nos(void)
{
  if (s_ordem_ver == s_nos_ver && (tab5_now_ms() - s_ordem_ms) < 1000) { return s_ordem_n; }
  // MUDOU, MAS ACABOU DE ORDENAR: espera 250 ms. Com trafego denso da internet a
  // versao muda a cada pacote, e a pagina de nos pede uma linha de cada vez -
  // eram varias ordenacoes de 256 nos por desenho. Um quarto de segundo de
  // atraso na ordem nao se ve; o processamento, sim.
  if (s_ordem_ver != UINT32_MAX && (tab5_now_ms() - s_ordem_ms) < 250) { return s_ordem_n; }
  s_ordem_ver = s_nos_ver;
  s_ordem_ms  = tab5_now_ms();

  // Tres faixas:
  //   0 - o nosso no;
  //   F - os FAVORITOS (estrela), na ordem em que foram marcados;
  //   1 - os ouvidos PELO AR, FIXOS: na ordem em que apareceram com SNR pela
  //       primeira vez - nao descem quando outro fala;
  //   2 - o resto (MQTT, ou sem SNR), do ouvido mais recentemente ao mais
  //       antigo. Quem ganha SNR sobe para a faixa 1 e fica.
  EXT_RAM_BSS_ATTR static int64_t chave[MESH_MAX_NOS];
  int n = 0;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    mesh_no_t& no = s_nos[i];
    if (!no.usado) { continue; }
    fixar_se_pelo_ar(&no);
    int64_t k;
    const int fp = fav_pos(no.num);
    if (no.num == s_meu_num) { k = -1; }
    else if (fp >= 0)        { k = fp; }                         // favoritos, na ordem em que foram marcados
    else if (no.fixado)      { k = ((int64_t)1 << 20) + no.fixado; }
    else {
      int64_t idade = idade_do_no(no);
      if (idade < 0) { idade = INT32_MAX; }
      k = ((int64_t)1 << 40) + idade;
    }
    int j = n++;
    while (j > 0 && k < chave[j - 1]) { s_ordem[j] = s_ordem[j - 1]; chave[j] = chave[j - 1]; --j; }
    s_ordem[j] = i; chave[j] = k;
  }
  s_ordem_n = n;
  s_ordem_rede_n[0] = s_ordem_rede_n[1] = 0;
  for (int k = 0; k < n; ++k) {
    const int r = s_nos[s_ordem[k]].rede == MESH_REDE_INTERNET ? 1 : 0;
    s_ordem_rede[r][s_ordem_rede_n[r]++] = s_ordem[k];
  }
  return n;
}

bool tab5_mesh_no_pelo_ar(const mesh_no_t* no)
{
  return !no->via_mqtt && no->snr > -128.0f;
}

// ---------------------------------------------------------------------------
// FAVORITOS - a estrela, a mesma do no e do cliente web
// ---------------------------------------------------------------------------
// O Meshtastic ja tem favoritos: NodeInfo.is_favorite (campo 10) no dump, e o
// AdminMessage set_favorite_node (39) / remove_favorite_node (40) para mudar.
// Usando os dele, a estrela marcada aqui aparece no cliente web e na tela do
// no, e o no protege o favorito do despejo da NodeDB dele tambem.
//
// Guardados TAMBEM aqui (NVS), na ordem em que foram marcados - e a ordem no
// topo da tabela. Sem o radio (modo internet) a lista local vale sozinha; a
// mudanca feita assim fica PENDENTE e vai para o no no proximo dump. Com o
// radio, o dump e a verdade: estrela tirada no cliente web some daqui tambem.
#define MESH_MAX_FAVS  32
#define MESH_NVS_FAVS  "favs"
static uint32_t s_favs[MESH_MAX_FAVS];
static int      s_n_favs = 0;
static bool     s_favs_lidos = false;
typedef struct { uint32_t num; bool quer; } fav_pend_t;
#define MESH_FAV_PEND 8
static fav_pend_t s_fav_pend[MESH_FAV_PEND];
static int        s_n_fav_pend = 0;

static bool       enlace_enviar(const uint8_t* corpo, size_t n);
static mesh_no_t* achar_no(uint32_t num);

static void recordar_favs(void)
{
  if (s_favs_lidos) { return; }
  s_favs_lidos = true;
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t n = sizeof(s_favs);
  if (nvs_get_blob(h, MESH_NVS_FAVS, s_favs, &n) == ESP_OK) { s_n_favs = (int)(n / sizeof(uint32_t)); }
  nvs_close(h);
}

static void salvar_favs(void)
{
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (s_n_favs) { nvs_set_blob(h, MESH_NVS_FAVS, s_favs, s_n_favs * sizeof(uint32_t)); }
  else          { nvs_erase_key(h, MESH_NVS_FAVS); }
  nvs_commit(h);
  nvs_close(h);
}

// Posicao na lista de favoritos; -1 = nao e. SEM TRAVA.
static int fav_pos(uint32_t num)
{
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { if (s_favs[i] == num) { return i; } }
  return -1;
}

// So a lista local (e a NVS, se mudou). SEM TRAVA.
static void fav_aplicar(uint32_t num, bool quer)
{
  const int pos = fav_pos(num);
  if (quer == (pos >= 0)) { return; }
  if (quer) {
    if (s_n_favs >= MESH_MAX_FAVS) { ESP_LOGW(TAG, "favoritos cheios (%d)", MESH_MAX_FAVS); return; }
    s_favs[s_n_favs++] = num;
    achar_no(num);   // favorito fica na tabela mesmo sem ter falado nesta sessao
  } else {
    for (int i = pos; i < s_n_favs - 1; ++i) { s_favs[i] = s_favs[i + 1]; }
    --s_n_favs;
  }
  salvar_favs();
  ++s_nos_ver;
}

static int fav_pendente(uint32_t num)
{
  for (int i = 0; i < s_n_fav_pend; ++i) { if (s_fav_pend[i].num == num) { return i; } }
  return -1;
}

// O no esta do outro lado e ja terminou o dump: da para mandar admin.
static bool radio_aceita_admin(void)
{
  return s_servico_ativo && s_meu_num != 0 && s_config_ok_ms != 0
      && (s_via == SETTINGS_MESH_TRANSPORT_SERIAL || (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && mesh_ble_conectado()));
}

// ToRadio{ packet: MeshPacket{ to: nos, decoded: Data{ portnum: ADMIN(6),
// payload: <AdminMessage> }, id } }. Sem 'from' (= 0): e o que o no
// reconhece como admin LOCAL, de um cliente conectado - aceito sem chave de
// sessao (AdminModule.cpp, ramo mp.from == 0, da 2.5 ate a atual).
// Unico caminho de AdminMessage do servico: favoritos e tab5_mesh_admin.
#define PORT_ADMIN 6
bool tab5_mesh_interno_admin(const uint8_t* adm, size_t n_adm)
{
  if (!adm || n_adm == 0 || n_adm > MESH_PAYLOAD_MAX + 33) { return false; }   // Data.payload: 233
  if (s_meu_num == 0) { return false; }   // sem o nosso numero, o 'to' iria 0 - e 0 nao e o no
  const mesh_pb_envelope_t e = { s_meu_num, -1, PORT_ADMIN, adm, n_adm, esp_random() | 1u, 0, false };
  uint8_t quadro[MESH_PAYLOAD_MAX + 88];
  const size_t n = mesh_pb_to_radio(&e, quadro, sizeof(quadro));
  const bool ok = n && enlace_enviar(quadro, n);
  if (ok) { mesh_diario_nota_envio(MESH_ENV_ADMIN); }
  return ok;
}

bool tab5_mesh_interno_pode_admin(void) { return radio_aceita_admin(); }

static bool enviar_admin_favorito(uint32_t num, bool quer)
{
  // AdminMessage{ 39: set_favorite_node | 40: remove_favorite_node = num }
  uint8_t adm[12];
  pb_escritor_t w = pb_escritor(adm, sizeof(adm));
  pb_w_num(&w, quer ? 39 : 40, num);
  const bool ok = tab5_mesh_interno_admin(adm, pb_tamanho(&w));
  ESP_LOGI(TAG, "favorito %08lX -> %s no no: %s", (unsigned long)num, quer ? "marcar" : "tirar", ok ? "enviado" : "falhou");
  return ok;
}

// Fim do dump: o que foi mudado sem o radio vai agora. Com a trava.
static void enviar_favs_pendentes(void)
{
  if (!radio_aceita_admin()) { return; }
  for (int i = 0; i < s_n_fav_pend; ++i) { enviar_admin_favorito(s_fav_pend[i].num, s_fav_pend[i].quer); }
  s_n_fav_pend = 0;
}

bool tab5_mesh_favorito(uint32_t num)
{
  travar();
  const bool f = fav_pos(num) >= 0;
  destravar();
  return f;
}

void tab5_mesh_definir_favorito(uint32_t num, bool quer)
{
  if (num == 0) { return; }
  travar();
  if (num != s_meu_num) {
    fav_aplicar(num, quer);
    // Para o no: agora, se ele esta ai; senao fica pendente para o dump.
    int k = fav_pendente(num);
    if (radio_aceita_admin() && enviar_admin_favorito(num, quer)) {
      if (k >= 0) { s_fav_pend[k] = s_fav_pend[--s_n_fav_pend]; }
    } else {
      if (k < 0 && s_n_fav_pend < MESH_FAV_PEND) { k = s_n_fav_pend++; }
      if (k >= 0) { s_fav_pend[k] = fav_pend_t{ num, quer }; }
    }
  }
  destravar();
  sistema_avisar(AVISO_MESH_NOS);
}

// O que o dump disse de um no (campo 10 ausente = false, e proto3). Pendente
// nosso vence: e uma mudanca que o no ainda nao recebeu.
static void fav_do_radio(uint32_t num, bool fav)
{
  if (num == s_meu_num || s_via == SETTINGS_MESH_TRANSPORT_INTERNET || fav_pendente(num) >= 0) { return; }
  fav_aplicar(num, fav);
}

bool tab5_mesh_no(int idx, mesh_no_t* saida)
{
  bool ok = false;
  travar();
  const int n = ordem_nos();
  if (idx >= 0 && idx < n && s_nos[s_ordem[idx]].usado) {   // ordem de ate 250 ms atras
    *saida = s_nos[s_ordem[idx]];
    // Campo DERIVADO, preenchido so na copia: sem isso envelheceria entre a
    // leitura e o desenho. Sem fonte nenhuma fica -1, e a tela escreve "?".
    saida->idade_s = idade_do_no(*saida);
    saida->favorito = fav_pos(saida->num) >= 0;
    ok = true;
  }
  destravar();
  return ok;
}

int tab5_mesh_num_nos_rede(uint8_t rede)
{
  travar();
  ordem_nos();
  const int n = s_ordem_rede_n[rede == MESH_REDE_INTERNET ? 1 : 0];
  destravar();
  return n;
}

bool tab5_mesh_no_rede(uint8_t rede, int idx, mesh_no_t* saida)
{
  bool ok = false;
  travar();
  ordem_nos();
  const int r = rede == MESH_REDE_INTERNET ? 1 : 0;
  if (idx >= 0 && idx < s_ordem_rede_n[r] && s_nos[s_ordem_rede[r][idx]].usado) {
    *saida = s_nos[s_ordem_rede[r][idx]];
    saida->idade_s = idade_do_no(*saida);
    saida->favorito = fav_pos(saida->num) >= 0;
    ok = true;
  }
  destravar();
  return ok;
}

int tab5_mesh_num_msgs(void) { return s_n_msgs; }

// NUNCA SATURA - ver o aviso grande junto de s_total_msgs. Existe SO para
// quem precisa saber "chegou (ou saiu) mensagem desde a ultima vez que
// olhei" (comparacao direta, nao pintura_mudou_*): tab5_mesh_num_msgs()
// parece servir para isso mas nao serve, trava em MESH_MAX_MSGS (8) para
// sempre.
uint32_t tab5_mesh_total_msgs(void) { return s_total_msgs; }

bool tab5_mesh_msg(int i, mesh_msg_t* saida)
{
  if (i < 0 || i >= s_n_msgs) { return false; }
  travar();
  *saida = s_msgs[i];
  destravar();
  // Derivado na copia, igual ao idade_s dos nos: o estado muda o tempo todo e
  // guardar na mensagem faria a copia envelhecer entre a leitura e o uso.
  saida->envio = tab5_mesh_estado_envio(saida->id, &saida->erro);
  return true;
}

// ---------------------------------------------------------------------------
// Conversas e historico no cartao
// ---------------------------------------------------------------------------
// Nome do arquivo de uma conversa. Curto e sem acento de proposito: e nome de
// arquivo em FAT, e o cartao ja nos ensinou que 8.3 morde (ver o caso do
// satelites.csv que nao abria por falta de LFN).
// ---------------------------------------------------------------------------
// CACHE DE NOMES DOS NOS
// ---------------------------------------------------------------------------
// COMO O MESHTASTIC IDENTIFICA UM NO. O identificador e o "num" - um uint32
// derivado dos ultimos 4 bytes do MAC do radio. Ele NAO muda: e ele que vai no
// campo 1 do MeshPacket e e por ele que se endereca uma mensagem direta.
//
// O nome longo e o curto sao ETIQUETAS que a pessoa escolhe e pode trocar
// quando quiser. Trocar o nome NAO cria outro no - o num continua o mesmo, e
// para o sistema e o mesmo interlocutor com rotulo novo. Um no novo de verdade
// e um num novo, e ai e outra conversa mesmo.
//
// Por isso o arquivo da conversa sempre foi nomeado pelo NUM (no-A1B2C3D4.txt),
// e isso estava certo desde o inicio. O que faltava era guardar a ETIQUETA: sem
// o C6L plugado, s_nos esta vazio e a conversa nascia chamada "A1B2C3D4".
//
// Este cache resolve isso. Vive em nos.txt, uma linha por avistamento, e o
// leitor fica com a ULTIMA de cada num - assim renomear e so acrescentar uma
// linha, sem reescrever arquivo nenhum.
//
// HISTORICO, NAO "AO VIVO" - de proposito DESACOPLADO de MESH_MAX_NOS (32,
// o tamanho da tabela AO VIVO em s_nos - ver achar_no() e o despejo do mais
// antigo). Um historico de nos que so cresce - MESH_MAX_NOS era pequeno demais para
// isso TAMBEM (o despejo da tabela ao vivo resolve "quem esta ativo agora",
// mas nao "quem ja vi alguma vez"). Cada entrada e so 4+24+1 bytes; 256
// delas sao ~8KB - nada para um ESP32-P4 com PSRAM.
//
// SOBRE A BUSCA ANTES DE ACRESCENTAR (a duvida): sim, precisa
// - e uma busca LINEAR, sem indice nenhum, exatamente como ja era com 32.
// Ela so roda quando uma NodeInfo chega (nao a cada pacote - NodeInfo e
// esporadica) e uma vez por linha ao carregar o arquivo no boot. Com 256
// entradas isso e no maximo 256 comparacoes de inteiro - microssegundos,
// mesmo no pior caso. Nao compensa uma estrutura mais esperta (hash, arvore)
// para um numero deste tamanho; so compensaria na casa dos milhares.
#define MESH_ARQ_NOS "nos.txt"
// 1024: o cache de nomes enchia com os nos da internet e, cheio, recusava
// nome novo - inclusive o de um vizinho de radio. ~53 KB na PSRAM.
#define MESH_MAX_CONHECIDOS 1024

// CURTO E LONGO, os dois - a tabela de nos saia toda sem nome. Antes so o curto persistia aqui
// (ver o aviso grande junto de lembrar_nome() mais abaixo).
typedef struct { uint32_t num; char curto[MESH_NOME_CURTO]; char longo[MESH_NOME_LONGO]; bool sujo; } no_conhecido_t;
EXT_RAM_BSS_ATTR static no_conhecido_t s_conhecidos[MESH_MAX_CONHECIDOS];
static int s_n_conhecidos = 0;

// O ARQUIVO JA FOI LIDO NESTA SESSAO. Antes a guarda era "s_n_conhecidos ==
// 0": se o cartao nao montava na hora da leitura, a tabela ficava vazia, o
// dump do no a enchia, e da primeira vez que o cartao voltava TODOS os nos
// iam de novo para o fim do arquivo - que so crescia, com as mesmas linhas,
// a cada boot assim. Agora ninguem grava antes de ler (descarregar_nomes).
static bool s_nomes_lidos = false;

static void carregar_nomes(void);

// SEM TRAVA: chamadas de dentro dela. Nulo (nao string vazia) quando nao ha
// cache - "sem entrada" e "entrada vazia" sao a mesma coisa aqui, os dois
// jeitos de dizer "nada para mostrar".
static const char* curto_conhecido(uint32_t num)
{
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (s_conhecidos[i].num == num) { return s_conhecidos[i].curto[0] ? s_conhecidos[i].curto : nullptr; }
  }
  return nullptr;
}

static const char* longo_conhecido(uint32_t num)
{
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (s_conhecidos[i].num == num) { return s_conhecidos[i].longo[0] ? s_conhecidos[i].longo : nullptr; }
  }
  return nullptr;
}

// O QUE AS FONTES DO SISTEMA DESENHAM. As fontes suaves cobrem ASCII e o
// Latin-1 que o portugues usa (2 bytes em UTF-8, lider 0xC2/0xC3); emoji e
// simbolos (3-4 bytes) viram caixinha. Copia so o desenhavel, sem espacos
// nas pontas. "?" sozinho e o placeholder do Meshtastic para "ninguem
// escolheu um curto" - vale como vazio.
static void nome_legivel(const char* in, char* out, size_t tam)
{
  if (!tam) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)(in ? in : "");
  while (*p && o + 1 < tam) {
    if (*p >= 0x20 && *p <= 0x7e) { out[o++] = (char)*p++; continue; }
    if ((*p == 0xC2 || *p == 0xC3) && (p[1] & 0xC0) == 0x80) {
      if (o + 2 >= tam) { break; }
      out[o++] = (char)p[0]; out[o++] = (char)p[1]; p += 2; continue;
    }
    // Sequencia que a fonte nao tem: pula ela inteira.
    const int n = ((*p & 0xE0) == 0xC0) ? 2 : ((*p & 0xF0) == 0xE0) ? 3 : ((*p & 0xF8) == 0xF0) ? 4 : 1;
    for (int k = 0; k < n && *p; ++k) { ++p; }
  }
  out[o] = 0;
  size_t ini = 0;
  while (out[ini] == ' ') { ++ini; }
  if (ini) { memmove(out, out + ini, strlen(out + ini) + 1); }
  size_t n = strlen(out);
  while (n && out[n - 1] == ' ') { out[--n] = 0; }
  if (!strcmp(out, "?")) { out[0] = 0; }
}

static bool curto_utilizavel(const char* s)
{
  char t[MESH_NOME_CURTO * 2];
  nome_legivel(s, t, sizeof(t));
  return t[0] != 0;
}

// SEM TRAVA: chamada de dentro dela (lembrar_nome, achar_ou_criar_conversa).
// Ao vivo primeiro (mais fresco), depois o cache do cartao.
static void nome_exibicao(uint32_t num, char* out, size_t out_tam)
{
  const char* curto = nullptr;
  const char* longo = nullptr;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) {
      if (s_nos[i].curto[0]) { curto = s_nos[i].curto; }
      if (s_nos[i].longo[0]) { longo = s_nos[i].longo; }
      break;
    }
  }
  char t[MESH_NOME_LONGO];
  nome_legivel(curto ? curto : curto_conhecido(num), t, sizeof(t));
  if (!t[0]) { nome_legivel(longo ? longo : longo_conhecido(num), t, sizeof(t)); }
  if (t[0]) { snprintf(out, out_tam, "%s", t); return; }
  snprintf(out, out_tam, "!%08lx", (unsigned long)num);
}

void tab5_mesh_nome_exibicao(uint32_t num, char* out, size_t out_tam)
{
  travar();
  nome_exibicao(num, out, out_tam);
  destravar();
}

// Ver o aviso completo em tab5_mesh.hpp. Wrapper fino sobre curto_conhecido()
// (privada deste arquivo) - existe so para expor "ha cache, sem hex" para
// quem, diferente de tab5_mesh_nome_exibicao(), NAO quer o fallback de hex.
bool tab5_mesh_nome_cache(uint32_t num, char* out, size_t out_tam)
{
  const char* cache = curto_conhecido(num);
  if (!cache) { return false; }
  snprintf(out, out_tam, "%s", cache);
  return true;
}

// Irma de tab5_mesh_nome_cache(), para o nome LONGO - ver o aviso grande no
// .hpp.
bool tab5_mesh_nome_longo_cache(uint32_t num, char* out, size_t out_tam)
{
  const char* cache = longo_conhecido(num);
  if (!cache) { return false; }
  snprintf(out, out_tam, "%s", cache);
  return true;
}

// Grava so quando MUDA. O node_info chega a cada apresentacao e a cada
// telemetria; anotar sempre encheria o cartao com a mesma linha.
//
// 'curto'/'longo': cada um NULO independentemente quando este NodeInfo nao
// trouxe (ou trouxe algo inutilizavel) daquele campo - so atualiza o que
// veio, nunca apaga o outro que ja estava no cache.
static void lembrar_nome(uint32_t num, const char* curto, const char* longo)
{
  const bool tem_curto = curto && curto[0];
  const bool tem_longo = longo && longo[0];
  if (num == 0 || (!tem_curto && !tem_longo)) { return; }
  // OS DA INTERNET NAO VAO PARA O CACHE. Sao centenas, de passagem: gravavam o
  // cartao de 15 em 15 s e tomavam as 1024 vagas dos nomes daqui. Enquanto
  // estao na tabela o nome deles aparece igual (ela e consultada primeiro).
  if (s_rede_rx == MESH_REDE_INTERNET) { return; }

  int idx = -1;
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (s_conhecidos[i].num == num) { idx = i; break; }
  }
  if (idx < 0) {
    if (s_n_conhecidos >= MESH_MAX_CONHECIDOS) { return; }
    idx = s_n_conhecidos++;
    s_conhecidos[idx].num = num;
    s_conhecidos[idx].curto[0] = 0;
    s_conhecidos[idx].longo[0] = 0;
  }

  bool mudou = false;
  if (tem_curto && strcmp(s_conhecidos[idx].curto, curto) != 0) {
    snprintf(s_conhecidos[idx].curto, sizeof(s_conhecidos[idx].curto), "%s", curto);
    mudou = true;
  }
  if (tem_longo && strcmp(s_conhecidos[idx].longo, longo) != 0) {
    snprintf(s_conhecidos[idx].longo, sizeof(s_conhecidos[idx].longo), "%s", longo);
    mudou = true;
  }
  if (!mudou) { return; }

  // NAO GRAVA AGORA - so marca. Gravar aqui montava e desmontava o cartao uma
  // vez POR NO, e o radio despeja a lista inteira de uma vez na apresentacao:
  // no boot sairam oito montagens em um segundo, cada uma com o aviso do LDO
  // do SDMMC atras. A escrita vale um lote, nao um evento.
  s_conhecidos[idx].sujo = true;

  // A conversa que ja exista passa a usar o nome novo, pela MESMA regra da
  // exibicao (curto, longo, id). O ARQUIVO dela e nomeado pelo num, entao
  // renomear aqui nao desgruda o historico.
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada && !s_conv[i].channel && s_conv[i].no == num) {
      nome_exibicao(num, s_conv[i].name, sizeof(s_conv[i].name));
    }
  }
}

// Descarrega os nomes pendentes. UMA montagem para todos.
//
// Chamada pela tarefa do mesh, espacada: o cartao e caro de montar e nome de no
// nao e informacao urgente - se o aparelho reiniciar antes da descarga, o
// proximo node_info reensina.
static void descarregar_nomes(void)
{
  int pendentes = 0;
  for (int i = 0; i < s_n_conhecidos; ++i) { if (s_conhecidos[i].sujo) { ++pendentes; } }
  if (pendentes == 0) { return; }

  // Sem ter lido o arquivo, nao da para saber se o nome e novo: tenta ler
  // agora; sem cartao, os pendentes esperam a proxima volta.
  if (!s_nomes_lidos) { carregar_nomes(); }
  if (!s_nomes_lidos) { return; }
  pendentes = 0;
  for (int i = 0; i < s_n_conhecidos; ++i) { if (s_conhecidos[i].sujo) { ++pendentes; } }
  if (pendentes == 0) { return; }

  // FORMATO GANHOU UM CAMPO: "hex,curto,longo" - era so
  // "hex,curto". carregar_nomes() (abaixo) le os dois formatos: uma linha
  // antiga sem o segundo virgula so deixa o longo vazio, sem quebrar nada.
  // Pelo gravador do sistema: entregar e na hora, gravar e em lote.
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (!s_conhecidos[i].sujo) { continue; }
    char linha[96];
    snprintf(linha, sizeof(linha), "%08lX,%s,%s\n", (unsigned long)s_conhecidos[i].num,
             s_conhecidos[i].curto, s_conhecidos[i].longo);
    tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_NOS, linha);
    s_conhecidos[i].sujo = false;
  }
  ESP_LOGI(TAG, "%d nome(s) de no gravados", pendentes);
}

// Le o cache do cartao. Ultima linha de cada num vence.
//
// DOIS FORMATOS aceitos - ver o aviso em descarregar_nomes(): "hex,curto"
// (antigo) e "hex,curto,longo" (atual). A quebra de linha e removida ANTES
// de procurar a segunda virgula - ela so pode estar no fim de verdade,
// nao importa quantas virgulas vierem antes.
static void carregar_nomes(void)
{
  if (!tab5_sd_mount()) { return; }
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_NOS, "r");
  if (!f) { s_nomes_lidos = true; tab5_sd_unmount(); return; }   // ainda nao existe

  // Quem ja estava na tabela antes da leitura veio do no ao vivo.
  EXT_RAM_BSS_ATTR static bool ao_vivo[MESH_MAX_CONHECIDOS];
  for (int i = 0; i < MESH_MAX_CONHECIDOS; ++i) { ao_vivo[i] = (i < s_n_conhecidos); }

  int linhas = 0;
  char linha[96];
  while (fgets(linha, sizeof(linha), f)) {
    char* resto = strchr(linha, ',');
    if (!resto) { continue; }
    *resto++ = 0;   // linha = so o hex agora
    ++linhas;

    char* fim = strpbrk(resto, "\r\n");
    if (fim) { *fim = 0; }   // resto = "curto" ou "curto,longo", sem quebra

    char* curto = resto;
    char* longo = strchr(resto, ',');
    if (longo) { *longo++ = 0; }   // formato novo: separa os dois campos

    if (!curto[0] && (!longo || !longo[0])) { continue; }

    const uint32_t num = (uint32_t)strtoul(linha, nullptr, 16);
    if (num == 0) { continue; }

    int idx = -1;
    for (int i = 0; i < s_n_conhecidos; ++i) {
      if (s_conhecidos[i].num == num) { idx = i; break; }
    }
    // O no ao vivo ja ensinou o nome antes desta leitura: ele vence, e so
    // precisa ir para o arquivo se for diferente da ULTIMA linha dele la.
    if (idx >= 0 && ao_vivo[idx]) {
      s_conhecidos[idx].sujo = strcmp(s_conhecidos[idx].curto, curto) != 0
                            || strcmp(s_conhecidos[idx].longo, longo ? longo : "") != 0;
      continue;
    }
    if (idx < 0) {
      if (s_n_conhecidos >= MESH_MAX_CONHECIDOS) { continue; }
      idx = s_n_conhecidos++;
      s_conhecidos[idx].num = num;
      s_conhecidos[idx].curto[0] = 0;
      s_conhecidos[idx].longo[0] = 0;
      s_conhecidos[idx].sujo = false;
    }
    if (curto[0])         { snprintf(s_conhecidos[idx].curto, sizeof(s_conhecidos[idx].curto), "%s", curto); }
    if (longo && longo[0]) { snprintf(s_conhecidos[idx].longo, sizeof(s_conhecidos[idx].longo), "%s", longo); }
  }
  fclose(f);
  s_nomes_lidos = true;

  // COMPACTA. O arquivo e "so acrescentar" (renomear = uma linha nova), entao
  // cresce com linhas que a leitura acima ja descartou; e arquivos gravados
  // antes da guarda s_nomes_lidos tem o mesmo no repetido dezenas de vezes.
  // Com sobra demais, reescreve com uma linha por no - e ja leva junto o que
  // estava pendente.
  if (linhas > s_n_conhecidos + 32) {
    // O arquivo novo inteiro na PSRAM e UMA troca pela porta do sistema
    // (tab5_sd_replace) - nunca meio arquivo.
    const size_t por = 8 + 3 + MESH_NOME_CURTO + MESH_NOME_LONGO;
    const size_t cap = (size_t)s_n_conhecidos * por + 1;
    char* txt = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (txt) {
      size_t k = 0;
      for (int i = 0; i < s_n_conhecidos && k < cap; ++i) {
        k += (size_t)snprintf(txt + k, cap - k, "%08lX,%s,%s\n", (unsigned long)s_conhecidos[i].num,
                              s_conhecidos[i].curto, s_conhecidos[i].longo);
      }
      if (k < cap && tab5_sd_replace(MESH_PASTA, MESH_ARQ_NOS, txt, k)) {
        for (int i = 0; i < s_n_conhecidos; ++i) { s_conhecidos[i].sujo = false; }
        ESP_LOGI(TAG, "cartao: nos.txt compactado de %d para %d linha(s)", linhas, s_n_conhecidos);
      }
      heap_caps_free(txt);
    }
  }
  tab5_sd_unmount();
  ESP_LOGI(TAG, "cartao: %d no(s) conhecido(s)", s_n_conhecidos);
}

// ---------------------------------------------------------------------------
// CONVERSAS APAGADAS - A FLAG QUE FALTAVA.
// ---------------------------------------------------------------------------
// O BUG: apagar uma conversa (tab5_mesh_esquecer_conversa) sempre so mexeu na
// RAM, de proposito - "o cartao nao e tocado" era a regra, para o historico
// sobreviver caso voces voltassem a se falar. So que tab5_mesh_carregar_do_
// cartao() RELE A PASTA INTEIRA sempre que roda (abrir o app, sair do
// repouso), e o arquivo no-XXXXXXXX.txt continuava la - entao a conversa
// "apagada" voltava sozinha na proxima varredura. Nao era a mensagem
// reaparecendo, era a PASTA sendo lida nova de novo sem lembrar da decisao.
//
// A CORRECAO E A FLAG QUE FOI PEDIDA: um arquivo pequeno, apagadas.txt, uma
// linha por numero de no escondido. tab5_mesh_carregar_do_cartao() para de
// recriar a conversa de quem esta nesta lista - o historico continua no
// cartao (apagar so o REGISTRO, nao o passado, segue sendo a regra), so que
// agora ele fica invisivel de proposito, nao por acidente.
//
// UM NO SAI DA LISTA SOZINHO quando voces voltam a se falar de verdade - toque
// deliberado em "conversar com este no" (tab5_mesh_conversa_com) ou mensagem
// nova chegando (guardar_msg). Reler o cartao NAO conta como "voltar a se
// falar"; so conteudo novo conta. UM CANAL sai igual - toque em "conversar com
// este canal" (tab5_mesh_conversa_do_canal) ou mensagem nova nele.
//
// CANAL PASSOU A ENTRAR AQUI - ate entao so no direto podia ser
// apagado ("canal nao se apaga, e devolver false e mais honesto que fingir",
// dizia o comentario velho de tab5_mesh_esquecer_conversa). O pedido de
// oferecer "canais disponiveis" para reabrir depois exigia justamente a
// mesma flag que ja existia para no - so precisava aprender a guardar QUAL
// TIPO de chave e essa, ja que indice de canal (0..7) e numero de no (sempre
// um valor grande, derivado de MAC) nao podem se confundir no MESMO campo.
typedef struct { bool channel; uint32_t key; } apagado_t;
#define MESH_ARQ_APAGADAS "apagadas.txt"

#define MESH_MAX_APAGADOS 64   // conversas escondidas; nada a ver com a tabela de nos
static apagado_t s_apagados[MESH_MAX_APAGADOS];
static int       s_n_apagados = 0;
static bool      s_apagados_carregado = false;

// FORMATO DO ARQUIVO: uma linha por entrada.
//   no direto:  "%08lX"   (como sempre foi - RETROCOMPATIVEL de proposito,
//               nenhum arquivo apagadas.txt ja gravado precisa mudar)
//   canal:      "K%u"     ('K' nunca e digito hexadecimal (0-9A-F), entao
//               nunca se confunde com um numero de no legitimo lido do
//               formato antigo)
static void carregar_apagados(void)
{
  // Sem cartao NAO conta como carregado: a lista da RAM nao pode virar o
  // arquivo (desmarcar_apagada reescreve). Tenta de novo no proximo uso.
  if (!tab5_sd_mount()) { return; }
  s_apagados_carregado = true;   // com o cartao: uma leitura por sessao, mesmo se o arquivo faltar
  // O ARQUIVO E A VERDADE: o que foi marcado antes desta leitura ja esta nele
  // (tab5_sd_open aplica o diario e o pendente antes de abrir).
  s_n_apagados = 0;
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_APAGADAS, "r");
  if (!f) { tab5_sd_unmount(); return; }

  char linha[16];
  while (fgets(linha, sizeof(linha), f) && s_n_apagados < MESH_MAX_APAGADOS) {
    if (linha[0] == 'K') {
      const uint32_t idx = (uint32_t)strtoul(linha + 1, nullptr, 10);
      if (idx < MESH_MAX_CANAIS || idx == MESH_CANAL_INTERNET) {
        s_apagados[s_n_apagados].channel = true;
        s_apagados[s_n_apagados].key = idx;
        ++s_n_apagados;
      }
      continue;
    }
    const uint32_t num = (uint32_t)strtoul(linha, nullptr, 16);
    if (num != 0) {
      s_apagados[s_n_apagados].channel = false;
      s_apagados[s_n_apagados].key = num;
      ++s_n_apagados;
    }
  }
  fclose(f);
  tab5_sd_unmount();
  ESP_LOGI(TAG, "cartao: %d conversa(s) apagada(s) - ficam escondidas", s_n_apagados);
}

// SEM TRAVA: chamada de dentro dela.
static bool esta_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].channel == canal && s_apagados[i].key == chave) { return true; }
  }
  return false;
}

// Marca uma conversa como apagada: RAM primeiro (vale na hora, mesmo sem
// cartao), depois grava a linha - a linha extra no arquivo, sem reescrever
// tudo, e o mesmo truque de nos.txt.
static void marcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (esta_apagada(canal, chave)) { return; }
  if (s_n_apagados < MESH_MAX_APAGADOS) {
    s_apagados[s_n_apagados].channel = canal;
    s_apagados[s_n_apagados].key = chave;
    ++s_n_apagados;
  }

  char linha[16];
  if (canal) { snprintf(linha, sizeof(linha), "K%u\n", (unsigned)chave); }
  else       { snprintf(linha, sizeof(linha), "%08lX\n", (unsigned long)chave); }
  tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_APAGADAS, linha);
}

// Tira uma conversa da lista de apagados - chamado quando voces voltam a se
// falar de verdade (ver o comentario grande acima). Reescreve o arquivo
// inteiro: e raro (so acontece ao reatar algo que voce tinha apagado), e a
// lista e pequena.
static void desmarcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (!s_apagados_carregado) { return; }   // sem a lista do cartao nao se reescreve o arquivo
  int achou = -1;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].channel == canal && s_apagados[i].key == chave) { achou = i; break; }
  }
  if (achou < 0) { return; }   // nao estava apagado - nada a fazer, nada a reescrever

  for (int i = achou; i < s_n_apagados - 1; ++i) { s_apagados[i] = s_apagados[i+1]; }
  --s_n_apagados;

  // Reescreve do zero pela porta do sistema: o substituir grava antes as
  // linhas de "apagada" que ainda estavam para entrar, e com o cartao
  // emprestado ao computador vai para o diario.
  char txt[MESH_MAX_APAGADOS * 12 + 1];
  size_t k = 0;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].channel) { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "K%u\n", (unsigned)s_apagados[i].key); }
    else                     { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "%08lX\n", (unsigned long)s_apagados[i].key); }
  }
  tab5_sd_replace(MESH_PASTA, MESH_ARQ_APAGADAS, txt, k);
}

static void arquivo_da_conversa(const mesh_conversa_t* c, char* out, size_t tam)
{
  if (c->channel && mesh_canal_internet(c->idx)) { snprintf(out, tam, "internet-%u.txt", (unsigned)(c->idx & 0x7F)); }
  else if (c->channel) { snprintf(out, tam, "canal-%u.txt", (unsigned)c->idx); }
  else          { snprintf(out, tam, "no-%08lX.txt", (unsigned long)c->no); }
}

static int achar_ou_criar_conversa(bool canal, uint8_t idx, uint32_t no)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    if (canal && s_conv[i].channel && s_conv[i].idx == idx) { return i; }
    if (!canal && !s_conv[i].channel && s_conv[i].no == no) { return i; }
  }
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada) { continue; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));
    s_conv_seq[i] = 0;
    ++s_conv_ver;   // a ordem da coluna mudou
    s_conv[i].usada = true;
    s_conv[i].channel = canal;
    s_conv[i].idx = idx;
    s_conv[i].no  = no;
    if (const nao_lida_guardada_t* g = guardada_de(canal, idx, no, false)) { s_conv[i].nao_lidas = g->n; }
    if (canal) {
      // Nome REAL, se o radio ja nos contou (FromRadio campo 10). O rotulo
      // inventado abaixo e so o que resta enquanto a configuracao nao chegou.
      if (mesh_canal_internet(idx)) {
        snprintf(s_conv[i].name, sizeof(s_conv[i].name), "LongFast Internet");
      } else if (idx < MESH_MAX_CANAIS && s_canal_nome[idx][0]) {
        snprintf(s_conv[i].name, sizeof(s_conv[i].name), "%s", s_canal_nome[idx]);
      } else {
        snprintf(s_conv[i].name, sizeof(s_conv[i].name),
                 idx == 0 ? "LongFast" : "Canal %u", (unsigned)idx);
      }
    } else {
      // Nome do no - curto, longo, e o id so em ultimo caso (nome_exibicao).
      nome_exibicao(no, s_conv[i].name, sizeof(s_conv[i].name));
    }
    return i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// GRAVACAO DAS MENSAGENS
// ---------------------------------------------------------------------------
// O Mesh tinha tarefa e queue PROPRIAS de gravacao ("mesh_grava",
// apertar enviar travava a tela montando o cartao). A ideia era
// certa e virou do SISTEMA: tab5_sd_enqueue_append (hal/tab5_sd.hpp) copia a linha e
// volta na hora, e a tarefa "cartao" grava em lote. A mensagem vai como
// URGENTE: gravada em milissegundos, nao no lote de 5 s.
// Acrescenta ao arquivo da conversa. Formato de UMA linha, com o texto POR
// ULTIMO para que virgula dentro da mensagem nao quebre a leitura:
//   M,<quando>,<de>,<snr>,<rssi>,<texto>
//
// NAO FAZ MAIS E/S AQUI - ver o aviso grande acima, junto de laco_gravar().
// Isto roda com s_trava preso (chamado de dentro de guardar_msg()) e as
// vezes na tarefa de INTERFACE - tem que ser rapido incondicionalmente.
static void gravar_msg(int ci, const mesh_msg_t* m)
{
  if (ci < 0) { return; }
  char arquivo[32];
  arquivo_da_conversa(&s_conv[ci], arquivo, sizeof(arquivo));
  // "M4" e nao "M3": o registro ganhou os saltos - etiqueta nova
  // em vez de campo a mais, e o leitor sabe o que esperar antes de ler.
  char linha[sizeof(((mesh_msg_t*)0)->texto) + 96];
  snprintf(linha, sizeof(linha), "M4,%lu,%08lX,%.1f,%ld,%d,%u,%08lX,%s\n",
           (unsigned long)m->quando, (unsigned long)m->de,
           m->snr, (long)m->rssi, (int)m->via_mqtt, (unsigned)m->saltos,
           (unsigned long)m->id, m->texto);
  if (!tab5_sd_enqueue_append(MESH_PASTA, arquivo, linha, nullptr, true)) {
    ESP_LOGW(TAG, "gravador cheio - mensagem NAO foi salva no cartao");
  }
}

// COMECAR A FALAR COM ALGUEM QUE AINDA NAO FALOU CONOSCO.
//
// Ate aqui uma conversa direta so nascia quando a mensagem CHEGAVA
// (achar_ou_criar_conversa no caminho de recepcao). Ou seja: dava para
// responder, nunca para puxar assunto. Esta funcao abre o outro sentido, a
// partir da lista de nos.
//
// Devolve o indice VISIVEL - o mesmo que a coluna de conversas usa - e nao o
// interno, porque quem chama e a tela e e com esse que ela sabe trabalhar. Os
// dois divergem assim que uma conversa e liberada no meio do vetor.
// O NOSSO NUMERO SOBREVIVE AO DESLIGAMENTO.
//
// Ele so chegava pelo my_info do radio. Sem o C6L plugado valia 0 - e como a
// tela decide o lado do balao por (m->de == meu_numero), TODA mensagem do
// arquivo aparecia como recebida, inclusive as que voce mandou. O arquivo
// abria contando a conversa errada.
//
// Fica na NVS do proprio mesh, e nao em ajustes: ajustes guarda o que a PESSOA
// escolheu, e isto e coisa que o radio contou. Misturar os dois faria o modulo
// de preferencias saber de coisas que nao sao preferencia.

static void lembrar_meu_numero(uint32_t num)
{
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u32(h, "meu_num", num);
  nvs_commit(h);
  nvs_close(h);
}

static uint32_t recordar_meu_numero(void)
{
  nvs_handle_t h;
  uint32_t v = 0;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return 0; }
  if (nvs_get_u32(h, "meu_num", &v) != ESP_OK) { v = 0; }
  nvs_close(h);
  return v;
}

bool tab5_mesh_servico_ativo(void) { return s_servico_ativo; }

// AS CONVERSAS QUE ESTAO NO CARTAO, sem precisar do radio.
//
// O historico sempre esteve gravado, mas a LISTA de conversas so existia na
// memoria - e ela so era povoada por mensagem chegando. Sem o C6L plugado, o
// app abria dizendo "sem conversas" com o cartao cheio delas. O arquivo estava
// la; faltava alguem ler o indice.
//
// A pasta e o proprio indice: cada conversa e um arquivo, e o nome dele carrega
// a chave (canal-3.txt, no-A1B2C3D4.txt). Nao precisa de catalogo a parte, que
// seria mais um estado para ficar dessincronizado do que descreve.
//
// Serve TAMBEM com o radio no ar: conversas antigas passam a aparecer na coluna
// desde a abertura, em vez de so quando alguem volta a falar. Renomear e
// seguro - o nome do canal chega depois no dump de configuracao e sobrescreve
// o rotulo inventado, porque o arquivo e nomeado pelo INDICE e nao pelo nome.
int tab5_mesh_carregar_do_cartao(void)
{
  // Sem radio, o numero vem da NVS. E o que permite ao arquivo saber quais
  // mensagens sao suas.
  if (s_meu_num == 0) { s_meu_num = recordar_meu_numero(); }

  // Nomes ANTES das conversas: e o cache que da nome a elas quando o radio nao
  // esta plugado. Na ordem inversa, toda conversa nasceria chamada pelo numero.
  if (!s_nomes_lidos) { carregar_nomes(); }

  if (!tab5_sd_mount()) { return 0; }

  char pasta[128];
  if (!tab5_sd_folder(MESH_PASTA, pasta, sizeof(pasta))) {
    tab5_sd_unmount();
    return 0;
  }

  DIR* dir = opendir(pasta);
  if (!dir) { tab5_sd_unmount(); return 0; }

  int achadas = 0;
  struct dirent* e;
  travar();
  recordar_canais();
  while ((e = readdir(dir)) != NULL) {
    const char* n = e->d_name;
    if (strcmp(n, MESH_ARQ_ENVIOS) == 0) { continue; }   // desfechos, nao conversa

    if (strncmp(n, "canal-", 6) == 0) {
      const unsigned long idx = strtoul(n + 6, nullptr, 10);
      // !esta_apagada(true, idx) ACRESCENTADO - faltava aqui, e
      // era a SEGUNDA causa (independente da config do radio) de um canal
      // apagado voltar sozinho: esta varredura roda ao abrir o app e nao
      // sabia nada sobre "apagada", so olhava se o arquivo existia no
      // cartao. Mesma logica que "no-" ja usava, so nunca tinha sido
      // estendida para canal.
      // Canal DESABILITADO no no (configuracao conhecida, ver
      // recordar_canais): o arquivo fica no cartao, a conversa nao volta.
      const bool desligado = s_canais_conhecidos && idx < MESH_MAX_CANAIS && !s_canal_ativo[idx];
      if (idx < MESH_MAX_CANAIS && !desligado && !esta_apagada(true, (uint8_t)idx)
       && achar_ou_criar_conversa(true, (uint8_t)idx, 0) >= 0) { ++achadas; }
    }
    else if (strncmp(n, "internet-", 9) == 0 || strncmp(n, "eua-", 4) == 0) {   // segundo nome: historico antigo
      // A conversa da internet, com historico no cartao: volta mesmo com a rede
      // desligada (so leitura), como as outras.
      if (!esta_apagada(true, MESH_CANAL_INTERNET)
       && achar_ou_criar_conversa(true, MESH_CANAL_INTERNET, 0) >= 0) { ++achadas; }
    }
    else if (strncmp(n, "no-", 3) == 0) {
      // Hexadecimal, como foi escrito. Zero nao e no - e o que o strtoul
      // devolve quando o nome nao presta, entao serve de peneira tambem.
      const uint32_t num = (uint32_t)strtoul(n + 3, nullptr, 16);
      // Apagada de proposito: nao recriar so porque o arquivo ainda existe no
      // cartao (ver o comentario grande em carregar_apagados, acima). O
      // historico continua salvo - so nao volta a aparecer sozinho.
      if (num != 0 && !esta_apagada(false, num) && achar_ou_criar_conversa(false, 0, num) >= 0) { ++achadas; }
    }
  }
  destravar();
  closedir(dir);
  tab5_sd_unmount();

  ESP_LOGI(TAG, "cartao: %d conversa(s) no arquivo", achadas);
  return achadas;
}

int tab5_mesh_conversa_com(uint32_t no)
{
  // Numero zero nao e no; e o preenchimento de quem nao sabe. E conversa com o proprio no
  // mesmo nao existe no protocolo - o radio nao entrega a si proprio.
  if (no == 0) { return -1; }

  travar();
  if (no == s_meu_num) { destravar(); return -1; }

  const int interno = achar_ou_criar_conversa(false, 0, no);
  const int visivel = (interno >= 0) ? visivel_de(interno) : -1;
  destravar();

  // Toque deliberado em "conversar com este no": e a volta a se falar que
  // desfaz um apagar anterior (ver o comentario grande acima da flag).
  if (interno >= 0) { desmarcar_apagada(false, no); }
  return visivel;
}

// Mesmo papel de tab5_mesh_conversa_com(), para CANAL - criado
// junto da opcao de apagar canal, para "canais disponiveis" poder reabrir um
// que foi escondido (ver o aviso grande da flag de apagados, e
// tab5_mesh_num_canais_disponiveis()/tab5_mesh_canal_disponivel() no fim
// deste arquivo).
int tab5_mesh_conversa_do_canal(uint8_t idx)
{
  if (idx >= MESH_MAX_CANAIS && idx != MESH_CANAL_INTERNET) { return -1; }

  travar();
  const int interno = achar_ou_criar_conversa(true, idx, 0);
  const int visivel = (interno >= 0) ? visivel_de(interno) : -1;
  destravar();

  // Toque deliberado em "conversar com este canal": desfaz um apagar
  // anterior, mesmo raciocinio de conversa_com() para no.
  if (interno >= 0) { desmarcar_apagada(true, idx); }
  return visivel;
}

// ---------------------------------------------------------------------------
// A ORDEM DA COLUNA
// ---------------------------------------------------------------------------
// Canais primeiro, pelo indice; depois as diretas, mais recente em cima. O
// vetor s_conv e um conjunto de vagas, nao uma ordem: uma conversa nova
// ocupa a primeira vaga livre, que pode ficar ANTES da conversa aberta. Com o
// indice visivel sendo "a n-esima vaga usada", isso deslocava a conversa
// aberta uma casa - o app passava a desenhar o historico de uma e a enviar
// para outra. Agora a ordem e explicita e a mudanca dela e anunciada
// (s_conv_ver), para o app reencontrar a sua pela chave.
//
// SEM TRAVA: chamadas de dentro dela. Insercao direta: sao no maximo 12.
static bool vem_antes(int a, int b)
{
  const mesh_conversa_t& x = s_conv[a];
  const mesh_conversa_t& y = s_conv[b];
  // A internet embaixo de tudo, depois das diretas: e abaixo do traco na coluna.
  const bool ex = x.channel && mesh_canal_internet(x.idx), ey = y.channel && mesh_canal_internet(y.idx);
  if (ex != ey) { return ey; }
  if (x.channel != y.channel) { return x.channel; }
  if (x.channel)            { return x.idx < y.idx; }
  if (s_conv_seq[a] != s_conv_seq[b]) { return s_conv_seq[a] > s_conv_seq[b]; }
  return a < b;
}

static int ordem_conversas(int out[MESH_MAX_CONVERSAS])
{
  int n = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    int j = n++;
    while (j > 0 && vem_antes(i, out[j - 1])) { out[j] = out[j - 1]; --j; }
    out[j] = i;
  }
  return n;
}

static int interno_de(int visivel)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = ordem_conversas(o);
  return (visivel >= 0 && visivel < n) ? o[visivel] : -1;
}

static int visivel_de(int interno)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = ordem_conversas(o);
  for (int k = 0; k < n; ++k) { if (o[k] == interno) { return k; } }
  return -1;
}

uint32_t tab5_mesh_conversas_versao(void) { return s_conv_ver; }

int tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no)
{
  int v = -1;
  travar();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada || s_conv[i].channel != canal) { continue; }
    if (canal ? (s_conv[i].idx == idx) : (s_conv[i].no == no)) { v = visivel_de(i); break; }
  }
  destravar();
  return v;
}

// ---------------------------------------------------------------------------
// OS CANAIS DO NO - ativos, com nome, e lembrados entre um boot e outro
// ---------------------------------------------------------------------------
// A configuracao dos canais so chega no dump do radio. Sem guarda-la, com o no
// fora (so consulta, ou modo internet) nao havia como saber que um canal
// tinha sido DESABILITADO: o arquivo canal-N.txt continuava no cartao e a
// conversa voltava na varredura da pasta. Fica na NVS do proprio mesh, ao
// lado do nosso numero - e informacao que o radio contou, nao preferencia.
//
// Gravada em LOTE (s_canais_sujos), no fim do dump ou na volta de 15 s da
// tarefa: o dump traz os 8 canais em sequencia e cada um seria uma escrita.
typedef struct { uint8_t ativo; char name[MESH_NOME_CONVERSA]; } canal_salvo_t;
#define MESH_NVS_CANAIS "canais"

static void recordar_canais(void)
{
  if (s_canais_lidos) { return; }
  s_canais_lidos = true;
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return; }
  canal_salvo_t tab[MESH_MAX_CANAIS];
  size_t n = sizeof(tab);
  if (nvs_get_blob(h, MESH_NVS_CANAIS, tab, &n) == ESP_OK && n == sizeof(tab)) {
    for (int i = 0; i < MESH_MAX_CANAIS; ++i) {
      s_canal_ativo[i] = tab[i].ativo != 0;
      tab[i].name[sizeof(tab[i].name) - 1] = 0;
      snprintf(s_canal_nome[i], sizeof(s_canal_nome[i]), "%s", tab[i].ativo ? tab[i].name : "");
    }
    s_canais_conhecidos = true;
  }
  nvs_close(h);
}

// A COPIA sob a trava, a GRAVACAO fora dela: a NVS grava flash (o cache
// para, ms), e com a trava presa o radio esperava junto para guardar o
// proximo quadro. Roda na carona da tarefa "cartao" (ver mesh_na_carona).
static void salvar_canais(void)
{
  canal_salvo_t tab[MESH_MAX_CANAIS];
  travar();
  if (!s_canais_sujos) { destravar(); return; }
  s_canais_sujos = false;
  memset(tab, 0, sizeof(tab));
  for (int i = 0; i < MESH_MAX_CANAIS; ++i) {
    tab[i].ativo = s_canal_ativo[i] ? 1 : 0;
    snprintf(tab[i].name, sizeof(tab[i].name), "%s", s_canal_nome[i]);
  }
  destravar();
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_blob(h, MESH_NVS_CANAIS, tab, sizeof(tab));
  nvs_commit(h);
  nvs_close(h);
}

// Nome de um canal para mostrar. Nome vazio no Meshtastic quer dizer "o nome
// do preset de modem" - LongFast no padrao de fabrica.
static void nome_do_canal(int idx, char* out, size_t tam)
{
  if (idx == MESH_CANAL_INTERNET) { snprintf(out, tam, "LongFast Internet"); }
  else if (idx >= 0 && idx < MESH_MAX_CANAIS && s_canal_nome[idx][0]) { snprintf(out, tam, "%s", s_canal_nome[idx]); }
  else if (idx == 0) { snprintf(out, tam, "LongFast"); }
  else               { snprintf(out, tam, "Canal %d", idx); }
}

// Some com a conversa de um canal da coluna, SEM marcar como apagada: quem a
// tirou foi a configuracao do no, nao voce. Reativando o canal no no, ela
// volta sozinha no proximo dump, com o historico do cartao.
static void esconder_conversa_canal(int idx)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada && s_conv[i].channel && s_conv[i].idx == idx) {
      memset(&s_conv[i], 0, sizeof(s_conv[i]));
      s_conv_seq[i] = 0;
      ++s_conv_ver;
      publicar_notif();   // as nao lidas dela saem do total
    }
  }
}

// Um canal da configuracao (FromRadio campo 10), JA com os zeros do proto3
// tratados (ver o leitor). Com a trava tomada.
static void aplicar_canal(int idx, int papel, const char* nome_radio, const uint8_t* psk, size_t n_psk)
{
  recordar_canais();   // antes de comparar: a NVS nao pode sobrescrever o que chegou agora
  const bool ativo = (papel != 0);
  char nome[MESH_NOME_CONVERSA] = "";
  if (ativo) {
    if (nome_radio[0]) { snprintf(nome, sizeof(nome), "%s", nome_radio); }
    else if (idx == 0) { snprintf(nome, sizeof(nome), "LongFast"); }
    else               { snprintf(nome, sizeof(nome), "Canal %d", idx); }
  }
  mesh_mqtt_aprender_canal(idx, papel, nome, ativo ? psk : nullptr, ativo ? n_psk : 0);

  if (s_canal_ativo[idx] != ativo || strcmp(s_canal_nome[idx], nome) != 0 || !s_canais_conhecidos) {
    s_canais_sujos = true;
  }
  s_canal_ativo[idx] = ativo;
  snprintf(s_canal_nome[idx], sizeof(s_canal_nome[idx]), "%s", nome);
  s_canais_conhecidos = true;

  if (!ativo) {
    // DESABILITADO NO NO: sai da coluna. Era o caso do canal desligado no
    // cliente web que continuava aparecendo aqui.
    esconder_conversa_canal(idx);
    return;
  }
  ESP_LOGI(TAG, "canal %d: \"%s\"", idx, nome);

  // CANAL EXISTE, ENTAO A CONVERSA EXISTE - salvo se voce a apagou. Sem isso
  // um canal criado no celular ficava invisivel ate alguem falar nele.
  if (!esta_apagada(true, (uint8_t)idx)) { achar_ou_criar_conversa(true, (uint8_t)idx, 0); }
  // Renomeia a que ja exista. O arquivo e pelo INDICE (canal-0.txt), entao
  // renomear nao desgruda o historico.
  for (int k = 0; k < MESH_MAX_CONVERSAS; ++k) {
    if (s_conv[k].usada && s_conv[k].channel && s_conv[k].idx == idx) {
      snprintf(s_conv[k].name, sizeof(s_conv[k].name), "%s", nome);
    }
  }
}

// As conversas dos canais ativos (sem as apagadas), ou so o primario quando
// nenhuma configuracao e conhecida ainda. Com a trava tomada.
static void criar_conversas_dos_canais(void)
{
  recordar_canais();
  bool algum = false;
  for (int idx = 0; idx < MESH_MAX_CANAIS; ++idx) {
    if (!s_canais_conhecidos || !s_canal_ativo[idx]) { continue; }
    algum = true;
    if (!esta_apagada(true, (uint8_t)idx)) { achar_ou_criar_conversa(true, (uint8_t)idx, 0); }
  }
  if (!algum && !esta_apagada(true, 0)) { achar_ou_criar_conversa(true, 0, 0); }
  if (settings_mesh_public_network() && !esta_apagada(true, MESH_CANAL_INTERNET)) { achar_ou_criar_conversa(true, MESH_CANAL_INTERNET, 0); }
}

// Indices dos canais a OFERECER: os ativos; sem nenhum conhecido, o primario.
// O da internet vem por ultimo, com a rede ligada.
static int lista_canais(uint8_t out[MESH_MAX_CANAIS + 1])
{
  int n = 0;
  for (int idx = 0; idx < MESH_MAX_CANAIS; ++idx) {
    if (s_canais_conhecidos && s_canal_ativo[idx]) { out[n++] = (uint8_t)idx; }
  }
  if (n == 0) { out[n++] = 0; }
  if (settings_mesh_public_network()) { out[n++] = MESH_CANAL_INTERNET; }
  return n;
}

int tab5_mesh_num_canais(void)
{
  uint8_t l[MESH_MAX_CANAIS + 1];
  travar();
  recordar_canais();
  const int n = lista_canais(l);
  destravar();
  return n;
}

bool tab5_mesh_canal(int i, uint8_t* idx_saida, char* nome, size_t tam, bool* visivel)
{
  uint8_t l[MESH_MAX_CANAIS + 1];
  bool ok = false;
  travar();
  recordar_canais();
  const int n = lista_canais(l);
  if (i >= 0 && i < n) {
    const uint8_t idx = l[i];
    if (idx_saida) { *idx_saida = idx; }
    if (nome)      { nome_do_canal(idx, nome, tam); }
    if (visivel) {
      *visivel = false;
      for (int k = 0; k < MESH_MAX_CONVERSAS; ++k) {
        if (s_conv[k].usada && s_conv[k].channel && s_conv[k].idx == idx) { *visivel = true; break; }
      }
    }
    ok = true;
  }
  destravar();
  return ok;
}

// APAGAR UMA CONVERSA - CANAL TAMBEM.
//
// Nasceu de um efeito colateral do "tocar no no para falar com ele": bastava
// TOCAR para a conversa existir, e ela ficava la vazia para sempre. Criar sem
// oferecer como desfazer e um caminho so de ida.
//
// CANAL NAO SE APAGAVA - "devolver false e mais honesto que
// fingir", dizia o comentario velho aqui, porque nao havia flag nenhuma que
// impedisse o canal de voltar sozinho na proxima mensagem ou na proxima
// config. Agora ha (a mesma "apagados" que ja servia no direto, extendida
// para guardar tambem indice de canal - ver o aviso grande junto dela) e os
// dois pontos que recriavam a conversa sozinhos (a varredura do cartao e o
// parser da config, ver os avisos la) ja checam essa flag. Falta so o "voltar
// a falar de proposito" - tab5_mesh_conversa_do_canal() cuida disso.
//
// O CARTAO NAO E TOCADO, de proposito. O que se apaga e o registro em memoria,
// nao o historico. Se voces voltarem a se falar, a conversa renasce com o que
// ja tinha sido dito - que e o que se espera de uma lista, e nao de um
// expurgo. Uma conversa vazia, que e o caso que motivou isto, nao tem nada no
// cartao mesmo.
bool tab5_mesh_esquecer_conversa(int idx)
{
  bool ok = false, canal_apagado = false;
  uint32_t chave_apagada = 0;
  travar();
  const int i = interno_de(idx);
  if (i >= 0) {
    // As nao lidas dela saem do total sozinhas: publicar_notif() reconta.
    // Guardado antes do memset apagar os proprios campos.
    canal_apagado = s_conv[i].channel;
    chave_apagada = canal_apagado ? s_conv[i].idx : s_conv[i].no;
    if (nao_lida_guardada_t* g = guardada_de(s_conv[i].channel, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));   // usada volta a false
    s_conv_seq[i] = 0;
    ++s_conv_ver;
    ok = true;
  }
  if (ok) { publicar_notif(); }
  destravar();

  // Fora da trava: e I/O de cartao (fopen/fclose), nao deve segurar o mutex
  // que o resto do servico (recepcao, envio) tambem disputa. Canal-0 e chave
  // valida (LongFast e o canal 0), entao so no direto tem "zero = invalido".
  if (ok && (canal_apagado || chave_apagada != 0)) {
    marcar_apagada(canal_apagado, chave_apagada);
  }
  return ok;
}

int tab5_mesh_num_conversas(void)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) { if (s_conv[i].usada) { ++n; } }
  destravar();
  return n;
}

bool tab5_mesh_conversa(int idx, mesh_conversa_t* saida)
{
  bool ok = false;
  travar();
  const int i = interno_de(idx);
  if (i >= 0) {
    *saida = s_conv[i];
    // A segunda linha, derivada agora (ver mesh_conversa_t.sub): o nome
    // longo de quem e a conversa direta, se ele disser algo que o nome da
    // primeira linha ainda nao disse.
    saida->sub[0] = 0;
    if (saida->channel) {
      if (mesh_canal_internet(saida->idx)) { snprintf(saida->sub, sizeof(saida->sub), "rede internet"); }
      else { snprintf(saida->sub, sizeof(saida->sub), saida->idx == 0 ? "canal primário" : "canal %u", (unsigned)saida->idx); }
    } else {
      const char* longo = nullptr;
      for (int k = 0; k < MESH_MAX_NOS; ++k) {
        if (s_nos[k].usado && s_nos[k].num == saida->no) { if (s_nos[k].longo[0]) { longo = s_nos[k].longo; } break; }
      }
      nome_legivel(longo ? longo : longo_conhecido(saida->no), saida->sub, sizeof(saida->sub));
      if (!strcmp(saida->sub, saida->name)) { saida->sub[0] = 0; }
    }
    ok = true;
  }
  destravar();
  return ok;
}

// Um lugar so para publicar, chamado de todos os pontos que mexem nos
// contadores. Espalhar a publicacao pelos pontos de alteracao seria repetir a
// regra de gravidade em cinco lugares - e esquecer num deles.
//
// SEM TRAVA: quem chama ja esta dentro dela. Tomar de novo travaria o mutex
// contra si mesmo (nao e recursivo).
static void publicar_notif(void)
{
  // O TOTAL E DERIVADO, NUNCA MANTIDO A PARTE. Antes eram tres
  // contadores mexidos por aritmetica em quatro lugares (+1 ao chegar, -N ao
  // ler/apagar, zerar ao abrir o app) - e zerar o total ao abrir o app SEM
  // zerar as conversas os descolava: dali em diante, ler uma conversa antiga
  // subtraia mais do que o total tinha, o piso em zero engolia a diferenca e
  // uma mensagem NOVA de outra conversa sumia da barra. Somando as conversas
  // aqui, a barra e as bolinhas de cada conversa sao a mesma verdade.
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    // OS DA INTERNET NAO APITAM. O LongFast publico fala o dia inteiro: a bolinha
    // da conversa conta, a barra de status e o bipe ficam com o que e daqui.
    if (s_conv[i].channel && mesh_canal_internet(s_conv[i].idx)) { continue; }
    s_nao_lidas += s_conv[i].nao_lidas;
    if (!s_conv[i].channel) { s_nao_lidas_dir += s_conv[i].nao_lidas; }
  }
  if (s_notif < 0) { return; }
  char resumo[NOTIF_RESUMO];
  if (s_nao_lidas_dir > 0) {
    snprintf(resumo, sizeof(resumo), "%d direta%s", s_nao_lidas_dir,
             s_nao_lidas_dir > 1 ? "s" : "");
  } else {
    // QUAL CANAL, e nao so "quantas mensagens" - "1 no canal" nao dizia nada
    // quando ha mais de um canal na lista (LongFast, BR...). Sabendo o NOME
    // do unico canal com novidade, a barra de status ja diz onde olhar sem
    // precisar abrir o app so para descobrir.
    //
    // CHAMADA SEMPRE COM O MUTEX JA TOMADO pelo chamador (ver os quatro
    // pontos que chamam publicar_notif) - por isso le s_conv[] direto, sem
    // travar() de novo (destravaria duas vezes o mesmo lugar - deadlock).
    const char* nome_unico = nullptr;
    int canais_com_novidade = 0;
    for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
      if (s_conv[i].usada && s_conv[i].channel && !mesh_canal_internet(s_conv[i].idx) && s_conv[i].nao_lidas > 0) {
        ++canais_com_novidade;
        nome_unico = s_conv[i].name;
      }
    }
    if (canais_com_novidade == 1 && nome_unico && nome_unico[0]) {
      snprintf(resumo, sizeof(resumo), "%d em %s", s_nao_lidas, nome_unico);
    } else if (canais_com_novidade > 1) {
      snprintf(resumo, sizeof(resumo), "%d em %d canais", s_nao_lidas, canais_com_novidade);
    } else {
      // Rede de seguranca: s_nao_lidas>0 sem nenhum canal marcado nao deveria
      // acontecer, mas sem isto a barra ficaria muda em vez de generica.
      snprintf(resumo, sizeof(resumo), "%d no canal", s_nao_lidas);
    }
  }
  // Direta e ATENCAO: e endereçada a voce. Trafego de canal e INFO - vale
  // saber, nao exige nada.
  notif_publicar(s_notif, s_nao_lidas,
                 s_nao_lidas_dir > 0 ? NOTIF_ATENCAO : NOTIF_INFO, resumo);
}

void tab5_mesh_marcar_conversa_lida(int idx)
{
  travar();
  const int i = interno_de(idx);
  // O total e as diretas saem sozinhos: publicar_notif() reconta.
  if (i >= 0) {
    s_conv[i].nao_lidas = 0;
    // Lida de verdade: a guardada some junto, para nao voltar no proximo reinicio.
    if (nao_lida_guardada_t* g = guardada_de(s_conv[i].channel, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
  }
  publicar_notif();
  destravar();
}

// Le as ULTIMAS 'max' mensagens do arquivo. Le o arquivo inteiro mantendo uma
// janela circular em vez de tentar posicionar no fim: linha tem tamanho
// variavel, entao nao ha como saltar direto - e o arquivo e pequeno.
int tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(conversa, &c)) { return 0; }
  return tab5_mesh_historico_de(c.channel, c.idx, c.no, saida, max);
}

int tab5_mesh_historico_de(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c = { };
  c.channel = canal; c.idx = idx; c.no = no;
  if (!tab5_sd_mount()) { return 0; }

  char nome[32];
  arquivo_da_conversa(&c, nome, sizeof(nome));

  // Volta a usar o servico: quem sabe distinguir "ainda nao existe" de "deu
  // errado" e o tab5_sd, nao cada app. Conversa sem historico devolve 0
  // mensagens sem alarme no log.
  FILE* f = tab5_sd_open(MESH_PASTA, nome, "r");
  if (!f) { tab5_sd_unmount(); return 0; }

  // Desfechos carregados UMA VEZ, antes do loop. Ver o comentario da tabela.
  static desfecho_t desf[MESH_DESFECHOS];
  const int n_desf = carregar_desfechos(desf, MESH_DESFECHOS);

  int n = 0, primeiro = 0, voltas = 0;
  // TEM QUE CABER O PREFIXO (~60 bytes: "M4,quando,de,snr,rssi,vmqtt,saltos,
  // id,") MAIS O TEXTO INTEIRO (ate MESH_TEXTO-1=237 bytes, ver o aviso la)
  // MAIS a quebra de linha - senao esta funcao vira o MESMO tipo de
  // truncamento que a leitura ao vivo tinha (MESH_TEXTO pequeno demais),
  // so que ao reler do cartao em vez de ao receber.
  char linha[MESH_TEXTO + 80];
  while (fgets(linha, sizeof(linha), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // nao segurar o core
    const bool fmt_m4 = (strncmp(linha, "M4,", 3) == 0);
    const bool fmt_m3 = !fmt_m4 && (strncmp(linha, "M3,", 3) == 0);
    const bool fmt_m2 = !fmt_m4 && !fmt_m3 && (strncmp(linha, "M2,", 3) == 0);
    if (!fmt_m4 && !fmt_m3 && !fmt_m2 && strncmp(linha, "M,", 2) != 0) { continue; }

    mesh_msg_t m; memset(&m, 0, sizeof(m));
    unsigned long quando = 0, de = 0, id = 0; float snr = 0; long rssi = 0;
    int vmqtt = 0;
    unsigned saltos = MESH_SALTOS_DESCONHECIDO;
    // %n devolve onde o texto comeca: tudo dali ate o fim da linha e a mensagem,
    // inclusive virgulas.
    int pos = 0;
    if (fmt_m4) {
      if (sscanf(linha, "M4,%lu,%8lX,%f,%ld,%d,%u,%8lX,%n",
                 &quando, &de, &snr, &rssi, &vmqtt, &saltos, &id, &pos) < 7) { continue; }
    } else if (fmt_m3) {
      // M3 tem via_mqtt mas nao saltos (campo chegou logo depois, mesma leva
      // ) - fica MESH_SALTOS_DESCONHECIDO, o sentinela de
      // "nao da para saber", nunca um numero inventado.
      if (sscanf(linha, "M3,%lu,%8lX,%f,%ld,%d,%8lX,%n",
                 &quando, &de, &snr, &rssi, &vmqtt, &id, &pos) < 6) { continue; }
    } else if (fmt_m2) {
      // M2 nao tem via_mqtt nem saltos - via_mqtt fica false, que e a
      // suposicao mais segura: mensagem antiga muito provavelmente veio pelo
      // radio mesmo, e um valor errado aqui so troca um icone, nunca perde
      // texto nem confunde entrega.
      if (sscanf(linha, "M2,%lu,%8lX,%f,%ld,%8lX,%n",
                 &quando, &de, &snr, &rssi, &id, &pos) < 5) { continue; }
    } else {
      // Formato antigo, sem id. A mensagem aparece igual; so nao ha marca de
      // entrega - e nao ha mesmo, porque o estado morreu no reinicio.
      if (sscanf(linha, "M,%lu,%8lX,%f,%ld,%n",
                 &quando, &de, &snr, &rssi, &pos) < 4) { continue; }
    }
    m.quando = (uint32_t)quando; m.de = (uint32_t)de; m.snr = snr; m.rssi = (int32_t)rssi;
    m.via_mqtt = (vmqtt != 0);
    m.saltos = (uint8_t)saltos;
    m.id = (uint32_t)id;
    // ANEL PRIMEIRO, CARTAO DEPOIS. O anel tem o estado ao vivo, inclusive o
    // ENVIANDO que nunca vai para o cartao; o cartao tem os desfechos de todos
    // os envios que ja sairam do anel ou de antes do ultimo reinicio.
    m.envio = tab5_mesh_estado_envio(m.id, &m.erro);
    if (m.envio == MESH_ENVIO_NA) { m.envio = buscar_desfecho(desf, n_desf, m.id, &m.erro); }
    const uint32_t corrigido = buscar_carimbo(desf, n_desf, m.id);
    if (corrigido) { m.quando = corrigido; }
    snprintf(m.texto, sizeof(m.texto), "%s", linha + pos);
    m.texto[strcspn(m.texto, "\r\n")] = 0;

    saida[(primeiro + n) % max] = m;
    if (n < max) { ++n; } else { primeiro = (primeiro + 1) % max; }
  }
  fclose(f);
  tab5_sd_unmount();

  // Desenrola a janela para a ordem cronologica.
  if (primeiro != 0 && n == max) {
    mesh_msg_t tmp[1];
    for (int i = 0; i < n; ++i) {
      // rotacao simples: barata para dezenas de itens
      tmp[0] = saida[0];
      for (int k = 0; k < n - 1; ++k) { saida[k] = saida[k+1]; }
      saida[n-1] = tmp[0];
      if (--primeiro == 0) { break; }
    }
  }
  return n;
}

// ---------------------------------------------------------------------------

// Acha ou cria a entrada do no. A tabela e pequena e a busca linear: com 32
// entradas isso e mais rapido que qualquer indice, e nao ha alocacao.
static mesh_no_t* achar_no(uint32_t num)
{
  // O nosso no e sempre daqui - a confirmacao de um envio pela internet chega
  // "de nos" pela fila de la.
  const uint8_t rede = (s_meu_num && num == s_meu_num) ? MESH_REDE_NO : s_rede_rx;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) {
      // Ouvido aqui tambem: e daqui (um no local que aparece no servidor
      // publico continua sendo local). O contrario nao rebaixa.
      if (rede == MESH_REDE_NO && s_nos[i].rede != MESH_REDE_NO) { s_nos[i].rede = MESH_REDE_NO; ++s_nos_ver; }
      return &s_nos[i];
    }
  }
  // TETO DA INTERNET. O LongFast publico tem milhares de nos; sem teto eles
  // empurrariam os daqui para fora da tabela. Cheio o teto, o novo no remoto
  // toma a vaga do remoto ouvido ha mais tempo (favorito nunca sai).
  if (rede == MESH_REDE_INTERNET) {
    int n_internet = 0, velho = -1;
    int64_t maior = -1;
    for (int i = 0; i < MESH_MAX_NOS; ++i) {
      const mesh_no_t& c = s_nos[i];
      if (!c.usado || c.rede != MESH_REDE_INTERNET) { continue; }
      ++n_internet;
      if (fav_pos(c.num) >= 0) { continue; }
      int64_t idade = idade_do_no(c);
      if (idade < 0) { idade = INT64_MAX / 2; }
      if (idade > maior) { maior = idade; velho = i; }
    }
    if (n_internet >= MESH_MAX_NOS_INTERNET && velho >= 0) {
      ++s_despejos;
      ++s_nos_ver;
      memset(&s_nos[velho], 0, sizeof(s_nos[velho]));
      s_nos[velho].usado = true;
      s_nos[velho].num = num;
      s_nos[velho].snr = -128.0f;
      s_nos[velho].saltos = MESH_SALTOS_DESCONHECIDO;
      s_nos[velho].rede = MESH_REDE_INTERNET;
      return &s_nos[velho];
    }
  }
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (!s_nos[i].usado) {
      memset(&s_nos[i], 0, sizeof(s_nos[i]));
      s_nos[i].usado = true;
      s_nos[i].num = num;
      s_nos[i].snr = -128.0f;
      // SALTOS COMECA DESCONHECIDO, nao 0 (nos apareciam sem nome e sem
      // saltos). memset zera para 0, que e
      // EXATAMENTE o valor de "vizinho direto" - um no que so foi criado por
      // achar_no() (visto num pacote, mas ainda sem NodeInfo) mostraria "0
      // saltos" como se fosse confirmado, quando na verdade e so "ainda nao
      // sei". So ler_node_info() (NodeInfo, campo 9) escreve um saltos de
      // verdade; ate la, o sentinela evita a mentira por omissao.
      s_nos[i].saltos = MESH_SALTOS_DESCONHECIDO;
      s_nos[i].rede = rede;
      ++s_nos_ver;
      return &s_nos[i];
    }
  }

  // TABELA CHEIA - isto devolvia nullptr e o no NOVO era
  // ignorado para sempre: quem enchesse as vagas primeiro (nem sempre os
  // nos mais relevantes - so os primeiros ouvidos desde o boot) trancava a
  // tabela, e ninguem mais entrava, nem gente conversando via MQTT que nunca
  // tinha aparecido antes. A tabela nunca trocava nos antigos por novos.
  //
  // A SAIDA: despejar alguem (criterio logo abaixo). Isso e seguro porque mesh_no_t e so um CACHE
  // de metadados ao vivo (nome, bateria, posicao) - o historico de mensagens
  // mora no cartao, indexado pelo NUMERO do no, e sobrevive intacto ao no
  // sair da tabela. Se o no despejado voltar a falar, ele reaparece na
  // proxima NodeInfo, como um no "novo" comum.
  // QUEM SAI, em tres passadas - a primeira que achar candidato decide:
  //   0 - os da INTERNET (via_mqtt) nao fixos: sao muitos, voltam no proximo
  //       pacote e nao sao alcancaveis sem a internet;
  //   1 - qualquer nao fixo;
  //   2 - qualquer um (tabela inteira de fixos - raro com 256).
  // O nosso nunca. Dentro da passada, o de MAIOR idade - pela idade completa
  // (avistamento nosso OU last_heard do radio), e nao so pelo avistamento:
  // no do dump tem avistamento 0 e era despejado primeiro mesmo tendo sido
  // ouvido ha um minuto, enquanto o resto do dump girava numa vaga so.
  int mais_velho = -1;
  int64_t maior = -1;
  for (int pass = 0; pass < 3 && mais_velho < 0; ++pass) {
    for (int i = 0; i < MESH_MAX_NOS; ++i) {
      const mesh_no_t& c = s_nos[i];
      if (c.num == s_meu_num || fav_pos(c.num) >= 0) { continue; }   // nem o nosso nem favorito
      if (pass == 0 && (c.fixado || !c.via_mqtt)) { continue; }
      if (pass == 1 && c.fixado) { continue; }
      int64_t idade = idade_do_no(c);
      if (idade < 0) { idade = INT64_MAX / 2; }   // nunca ouvido: o mais velho de todos
      if (idade > maior) { maior = idade; mais_velho = i; }
    }
  }
  if (mais_velho < 0) { mais_velho = 0; }
  // SEM LOG AQUI DE PROPOSITO (poluia o log). Isto e estado normal de regime (a tabela FICA cheia numa malha com
  // mais de 32 nos, e cada despejo e esperado, nao um erro) - so a CONTAGEM
  // fica guardada, para consultar quando precisar em vez de aparecer sozinha
  // toda hora. Ver tab5_mesh_despejos() e o comando "mesh" no console.
  ++s_despejos;
  ++s_nos_ver;
  memset(&s_nos[mais_velho], 0, sizeof(s_nos[mais_velho]));
  s_nos[mais_velho].usado = true;
  s_nos[mais_velho].num = num;
  s_nos[mais_velho].snr = -128.0f;
  s_nos[mais_velho].saltos = MESH_SALTOS_DESCONHECIDO;   // ver o aviso no ramo de cima
  s_nos[mais_velho].rede = rede;
  return &s_nos[mais_velho];
}

// ---------------------------------------------------------------------------
// A NODEDB DO TAB5, NO CARTAO
// ---------------------------------------------------------------------------
// O rádio guarda no maximo MAX_NUM_NODES (100 no C6L ate a 2.7, 120 na 2.8):
// constante do firmware dele. A tabela daqui tem 256 vagas e aprende tambem
// pelo trafego ao vivo - e, guardada no cartao, sobrevive ao reinicio: o Tab5
// passa a conhecer mais nos do que o radio consegue lembrar.
//
// Binario com cabecalho (magia, versao, tamanho do registro): se mesh_no_t
// mudar de forma numa versao nova, o arquivo velho e ignorado em vez de lido
// torto. Gravado a cada 10 min se a tabela mudou, e ao parar o servico.
//
// Ao carregar, o que e "desta sessao" volta zerado: avistamento local, SNR e o
// lugar fixo no topo. Um no ouvido ontem nao esta "no ar" hoje; ele volta ao
// topo quando for ouvido de novo.
#define MESH_ARQ_NODEDB "nodedb.bin"
typedef struct { char magia[4]; uint16_t versao; uint16_t tam; uint32_t n; } nodedb_cab_t;
// A COPIA SO EXISTE ENQUANTO GRAVA OU LE (31 KB de PSRAM): era um vetor fixo,
// usado duas vezes por boot e de 10 em 10 minutos.
static mesh_no_t* nodedb_copia(void)
{
  return (mesh_no_t*)heap_caps_malloc(sizeof(mesh_no_t) * MESH_MAX_NOS, MALLOC_CAP_SPIRAM);
}
static uint32_t s_nodedb_ver_salva = 0;
// A NodeDB do cartao foi lida nesta subida do servico (ou, com o cartao
// presente, nao existia). So entao a tabela pode substitui-la - uma tabela que
// comecou vazia por falta de cartao apagaria os nos guardados.
static bool s_nodedb_lida = false;

static void salvar_nodedb(void)
{
  mesh_no_t* s_nodedb_copia = nodedb_copia();
  if (!s_nodedb_copia) { return; }
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num != 0) { s_nodedb_copia[n++] = s_nos[i]; }
  }
  const uint32_t ver = s_nos_ver;
  destravar();
  if (n == 0 || !s_nodedb_lida) { heap_caps_free(s_nodedb_copia); return; }
  // Cabecalho e tabela num bloco so, e UMA troca pela porta do sistema: um
  // reinicio no meio deixa a NodeDB antiga inteira, nunca meia - e com o
  // cartao emprestado ao computador ela vai para o diario.
  const size_t tam = sizeof(nodedb_cab_t) + sizeof(mesh_no_t) * (size_t)n;
  uint8_t* bloco = (uint8_t*)heap_caps_malloc(tam, MALLOC_CAP_SPIRAM);
  if (bloco) {
    const nodedb_cab_t cab = { { 'M', 'N', 'D', 'B' }, 1, (uint16_t)sizeof(mesh_no_t), (uint32_t)n };
    memcpy(bloco, &cab, sizeof(cab));
    memcpy(bloco + sizeof(cab), s_nodedb_copia, sizeof(mesh_no_t) * (size_t)n);
    if (tab5_sd_replace(MESH_PASTA, MESH_ARQ_NODEDB, bloco, tam)) {
      s_nodedb_ver_salva = ver;
      ESP_LOGI(TAG, "cartao: %d no(s) na NodeDB do Tab5", n);
    }
    heap_caps_free(bloco);
  }
  heap_caps_free(s_nodedb_copia);
}

// Com a tabela vazia (logo depois do memset do iniciar). Le o cartao SEM a
// trava (E/S lenta) para a copia, e so a passagem para a tabela e travada - a
// tela pode estar lendo a tabela ao mesmo tempo.
static void carregar_nodedb(void)
{
  s_nodedb_lida = false;
  if (!tab5_sd_mount()) { return; }
  s_nodedb_lida = true;   // com o cartao: ou le, ou o arquivo nao existe (o primeiro salvar cria)
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_NODEDB, "rb");
  if (!f) { tab5_sd_unmount(); return; }
  mesh_no_t* s_nodedb_copia = nodedb_copia();
  if (!s_nodedb_copia) { fclose(f); tab5_sd_unmount(); return; }
  nodedb_cab_t cab;
  int lidos = 0;
  if (fread(&cab, sizeof(cab), 1, f) == 1 && !memcmp(cab.magia, "MNDB", 4)
   && cab.versao == 1 && cab.tam == sizeof(mesh_no_t)) {
    const int total = (cab.n < MESH_MAX_NOS) ? (int)cab.n : MESH_MAX_NOS;
    lidos = (int)fread(s_nodedb_copia, sizeof(mesh_no_t), (size_t)total, f);
  } else {
    ESP_LOGW(TAG, "cartao: nodedb.bin de outra versao - ignorado");
  }
  fclose(f);
  tab5_sd_unmount();

  int n = 0;
  travar();
  for (int i = 0; i < lidos && n < MESH_MAX_NOS; ++i) {
    mesh_no_t no = s_nodedb_copia[i];
    if (no.num == 0) { continue; }
    no.usado = true;
    no.visto_local_ms = 0;
    no.idade_s = -1;
    no.fixado = 0;
    no.snr = -128.0f;
    no.favorito = false;
    s_nos[n++] = no;
  }
  ++s_nos_ver;
  s_nodedb_ver_salva = s_nos_ver;
  destravar();
  heap_caps_free(s_nodedb_copia);
  ESP_LOGI(TAG, "cartao: %d no(s) lembrado(s) da NodeDB do Tab5", n);
}

// Ver o aviso completo em tab5_mesh.hpp. So LEITURA - nunca cria entrada
// (diferente de achar_no): perguntar "qual o enlace deste no" nao pode ter o
// efeito colateral de inventar um no na tabela.
bool tab5_mesh_no_enlace(uint32_t num, bool* via_mqtt, uint8_t* saltos)
{
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) {
      if (via_mqtt) { *via_mqtt = s_nos[i].via_mqtt; }
      if (saltos)   { *saltos   = s_nos[i].saltos;   }
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Interpretacao das mensagens que interessam
// ---------------------------------------------------------------------------
// DeviceMetrics: 1=bateria 2=tensao 3=uso_canal 4=airtime 5=uptime
//
// 'nosso' distingue a telemetria do NOSSO no da telemetria que chega da malha.
// Sem essa distincao (foi o bug) qualquer no vizinho sobrescrevia
// a nossa saude: aparecia "uptime 1136229s" - 13 dias - num C6L que tinha
// reiniciado 35 vezes. Bateria e tensao SEMPRE vao para o no dono da metrica;
// so os indicadores de saude e que sao exclusivos do nosso.
static void ler_metrics(pb_leitor_t l, mesh_no_t* no, bool nosso)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v; pb_varint(&l, &v); if (no) no->bateria = (uint8_t)v; }
    else if (n == 2 && t == 5) { float f; pb_float(&l, &f); if (no) no->tensao = f; }
    else if (n == 3 && t == 5) { float f; pb_float(&l, &f); if (nosso) s_saude.uso_canal = f; }
    else if (n == 4 && t == 5) { float f; pb_float(&l, &f); if (nosso) s_saude.airtime_tx = f; }
    else if (n == 5 && t == 0) { uint64_t v; pb_varint(&l, &v); if (nosso) s_saude.uptime_s = (uint32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
  if (nosso) { s_saude.tem_metricas = true; }
}

// LocalStats - a saude da malha vista pelo NOSSO no, e so por ele.
//   1=uptime 2=uso_canal 3=airtime 4..6=contadores de pacote
//   7=nos_online 8=nos_total
// Versoes mais novas acrescentam campos (piso de ruido, por exemplo); o pb_pular()
// cuida deles sem a gente precisar saber quais sao.
static void ler_local_stats(pb_leitor_t l)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.uptime_s   = (uint32_t)v; }
    else if (n == 2 && t == 5) { float f; pb_float(&l,&f);     s_saude.uso_canal  = f; }
    else if (n == 3 && t == 5) { float f; pb_float(&l,&f);     s_saude.airtime_tx = f; }
    else if (n == 4 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.pkt_tx      = (uint32_t)v; }
    else if (n == 5 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.pkt_rx      = (uint32_t)v; }
    else if (n == 6 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.pkt_ruins   = (uint32_t)v; }
    else if (n == 7 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.nos_online = (uint16_t)v; }
    else if (n == 8 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.nos_total  = (uint16_t)v; }
    else if (n == 9 && t == 0) { uint64_t v; pb_varint(&l,&v); s_saude.pkt_repetidos = (uint32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
  s_saude.tem_local = true;
  // SEM LOG AQUI. Os
  // valores ja ficam em s_saude, consultaveis sob demanda pelo comando "mesh"
  // do console (imprime canal/airtime/uptime/nos) sempre que precisar olhar.
}

// HardwareModel (mesh.proto) - TABELA COMPLETA, gerada, nao mais adivinhada.
//
// A versao anterior desta funcao era um switch com uma duzia de modelos
// escolhidos a mao, com o argumento de que copiar a lista oficial inteira
// "envelheceria e mentiria". O argumento nao se sustentou no primeiro uso de
// verdade: uma malha real trouxe "modelo 110", "modelo 88",
// "modelo 81" - nos genuinos que a tabela adivinhada simplesmente nao previa,
// entre eles o 111 = M5Stack C6L, o proprio radio que este sistema usa.
//
// A correcao nao e adivinhar mais - e parar de adivinhar. TAB5_MESH_HW_NOMES
// (tab5_mesh_hw_dados.cpp) e gerada pelo generate_hw.py direto do enum oficial
// do Meshtastic; "envelhece e mente" deixa de ser risco porque reexportar e
// um comando, nao uma memoria de quais numeros existem.
const char* mesh_hw_nome(uint8_t hw)
{
  return TAB5_MESH_HW_NOMES[hw];   // hw e uint8_t: sempre dentro da tabela
}

// User: 1=id 2=nome_longo 3=nome_curto 4=macaddr 5=hw_model
//
// O 'id' e o 'macaddr' sao o que faltava: nome longo e
// curto sao ETIQUETAS que quem renomeia no celular troca a vontade, e por elas
// nao da para saber com que APARELHO se esta falando. O id ("!a1b2c3d4") e
// derivado do MAC e acompanha o radio para sempre.
static void ler_user(pb_leitor_t l, mesh_no_t* no)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 2) { pb_texto(&l, no->id,    sizeof(no->id)); }
    else if (n == 2 && t == 2) { pb_texto(&l, no->longo, sizeof(no->longo)); }
    else if (n == 3 && t == 2) { pb_texto(&l, no->curto, sizeof(no->curto)); }
    else if (n == 4 && t == 2) {
      // macaddr e 'bytes', nao texto - nao da para usar ler_texto (ele
      // terminaria a string no primeiro byte 0x00 do endereco).
      uint64_t k;
      if (!pb_varint(&l, &k)) { break; }
      if (k > pb_restam(&l)) { break; }
      if (k == 6) { memcpy(no->mac, l.p, 6); no->tem_mac = true; }
      l.p += (size_t)k;
    }
    else if (n == 5 && t == 0) { uint64_t v; pb_varint(&l,&v); no->hw = (uint8_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
}

// Position: 1=lat_i 2=lon_i (sfixed32)
static void ler_position(pb_leitor_t l, mesh_no_t* no)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 5) { uint32_t v; pb_fixo32(&l, &v); no->lat_i = (int32_t)v; no->tem_pos = true; }
    else if (n == 2 && t == 5) { uint32_t v; pb_fixo32(&l, &v); no->lon_i = (int32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
}

// NodeInfo: 1=num 2=user 3=position 4=snr 5=last_heard 6=device_metrics 9=hops
static void ler_node_info(pb_leitor_t l)
{
  bool fav_radio = false;   // is_favorite: proto3, ausente = false
  uint32_t num = 0;
  mesh_no_t* no = nullptr;
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if (n == 1 && t == 0) {
      uint64_t v; pb_varint(&l, &v); num = (uint32_t)v;
      no = achar_no(num);
      if (!no) { return; }
    }
    else if (!no) { if (!pb_pular(&l, t)) { break; } }         // num sempre vem antes
    else if (n == 2 && t == 2) {
      pb_leitor_t s;
      if (pb_sub(&l,&s)) {
        // O User do NOSSO no, cru: o set_owner o reenvia inteiro (tab5_mesh_admin).
        if (num == s_meu_num && s_meu_num) { mesh_admin_guardar_user(s.p, (size_t)(s.fim - s.p)); }
        ler_user(s, no);
      }
    }
    else if (n == 3 && t == 2) { pb_leitor_t s; if (pb_sub(&l,&s)) ler_position(s, no); }
    else if (n == 4 && t == 5) { pb_float(&l, &no->snr); }
    else if (n == 5 && t == 5) { pb_fixo32(&l, &no->visto_em); marcar_epoch(no->visto_em); }
    else if (n == 6 && t == 2) { pb_leitor_t s; if (pb_sub(&l,&s)) ler_metrics(s, no, num == s_meu_num); }
    // via_mqtt: o no chegou pela INTERNET, nao pelo radio. Muda o significado
    // de tudo que se ve dele - o SNR nao e dele, e nao da para alcanca-lo por
    // RF. Merece aparecer na lista.
    else if (n == 8 && t == 0) { uint64_t v; pb_varint(&l,&v); no->via_mqtt = (v != 0); }
    else if (n == 9 && t == 0) { uint64_t v; pb_varint(&l,&v); no->saltos = (uint8_t)v; }
    else if (n == 10 && t == 0) { uint64_t v; pb_varint(&l,&v); fav_radio = (v != 0); }
    else if (!pb_pular(&l, t)) { break; }
  }

  // Guarda a etiqueta para quando o radio nao estiver aqui. Renomear no
  // aplicativo do celular chega por este mesmo caminho, com o MESMO num - e o
  // cache se atualiza sozinho, sem virar um interlocutor novo.
  //
  // OS DOIS AGORA, curto E longo - so o curto usavel entrava
  // aqui (era o unico que tab5_mesh_nome_exibicao() promete mostrar). Mas a
  // tabela de nos (montar_linha(), app_mesh.cpp) sempre mostrou o longo
  // tambem, e SO da entrada ao vivo - sem cache, ficava "(sem nome)" ate
  // ESTA SESSAO ver uma NodeInfo daquele no de novo, o que e raro (a maioria
  // dos nos e aprendida por telemetria/posicao/texto, nunca por NodeInfo).
  // Ver o aviso grande junto de tab5_mesh_nome_longo_cache() no .hpp.
  if (no) {
    lembrar_nome(num, curto_utilizavel(no->curto) ? no->curto : nullptr,
                       no->longo[0] ? no->longo : nullptr);
    fixar_se_pelo_ar(no);
    fav_do_radio(num, fav_radio);
    ++s_nos_ver;
  }
  sistema_avisar(AVISO_MESH_NOS);
}

// ANEL DE IDS JA VISTOS - dedup PROPRIO, separado do s_msgs.
//
// Ate aqui, a deteccao de eco olhava dentro de s_msgs[MESH_MAX_MSGS] - e
// MESH_MAX_MSGS e 8. Nasceu para o caso simples (voce manda, o radio devolve
// o mesmo pacote segundos depois) e nesse caso 8 sempre bastou: dificilmente
// chegam 8 mensagens de OUTRAS pessoas no intervalo entre o envio e o eco.
//
// COM MQTT ISSO MUDA. O mesmo pacote pode chegar DUAS vezes por caminhos com
// atraso bem diferente - uma copia pelo LoRA local, outra via broker MQTT
// (uplink de outro no, ou o proprio downlink de volta) - e o atraso entre as
// duas nao e mais "alguns segundos previsiveis", e "o que a internet e a queue
// do broker demorarem". Numa malha com ponte ativa, o trafego tambem fica mais
// denso. As duas coisas juntas empurram a segunda copia para FORA da janela de
// 8 mensagens, e ela passa a entrar como se fosse nova - a mesma mensagem
// duas vezes na conversa.
//
// A correcao e um anel SO PARA ISSO, independente do que aparece na tela: 256
// ids, 1 KB (na PSRAM, como o resto do .bss do servico), nada alem de um
// uint32_t cada. Eram 64; com o LongFast da internet passando pelo mesmo anel, a
// janela de um texto daqui encolhia - a copia atrasada pela ponte entrava de
// novo. Nao guarda conteudo - so "este id ja passou por aqui".
#define MESH_DEDUP_N 256
static uint32_t s_ids_vistos[MESH_DEDUP_N];
static int      s_prox_id_visto = 0;

static bool id_ja_visto(uint32_t id)
{
  if (id == 0) { return false; }   // 0 e "sem id" (ver o id em tab5_mesh_enviar) - nunca dedup
  for (int i = 0; i < MESH_DEDUP_N; ++i) {
    if (s_ids_vistos[i] == id) { return true; }
  }
  return false;
}

static void marcar_id_visto(uint32_t id)
{
  if (id == 0) { return; }
  s_ids_vistos[s_prox_id_visto] = id;
  s_prox_id_visto = (s_prox_id_visto + 1) % MESH_DEDUP_N;
}

static void guardar_msg(uint32_t de, uint32_t para, uint8_t canal, const char* txt,
                        uint32_t quando, float snr, int32_t rssi, uint32_t id,
                        bool via_mqtt, uint8_t saltos)
{
  // ECO DA PROPRIA MENSAGEM, E DUPLICATA POR PONTE MQTT: as duas descartam
  // pelo mesmo id, ver o comentario do anel logo acima.
  //
  // Ao enviar, guardamos a mensagem aqui na hora. Segundos depois o radio
  // DEVOLVE o mesmo pacote como recebido (from = nosso numero) - e antes de
  // lermos o campo 6 do MeshPacket ele chegava com id 0, virava uma SEGUNDA
  // copia, e era essa copia que a tela mostrava. Sem id, sem marca de entrega:
  // era por isso que o tique verde e o triangulo vermelho nunca apareciam nas
  // mensagens enviadas.
  //
  // O id do pacote e o mesmo que mandamos, entao ele identifica o eco sem
  // ambiguidade. Vale tambem para mensagem enviada pelo CELULAR atraves do
  // C6L: chega uma vez so, com o id que o radio deu a ela.
  // DA INTERNET: so canal (a direta viria cifrada com PKI, que nao abrimos), e
  // sempre na conversa "LongFast Internet".
  if (s_rede_rx == MESH_REDE_INTERNET) {
    if (para != 0xFFFFFFFFu) { return; }
    canal = MESH_CANAL_INTERNET;
  }
  if (id_ja_visto(id)) { return; }
  marcar_id_visto(id);

  // Empurra as antigas para o fim: indice 0 e sempre a mais recente, que e
  // como a tela quer ler.
  if (s_n_msgs < MESH_MAX_MSGS) { s_n_msgs++; }
  for (int i = s_n_msgs - 1; i > 0; --i) { s_msgs[i] = s_msgs[i-1]; }
  memset(&s_msgs[0], 0, sizeof(s_msgs[0]));
  s_msgs[0].de = de;
  s_msgs[0].para = para;
  s_msgs[0].channel = canal;
  // 0xFFFFFFFF e o endereco de difusao do Meshtastic. Qualquer outro valor
  // igual ao nosso numero significa mensagem enderecada A VOCE.
  s_msgs[0].direta = (para != 0xFFFFFFFFu) && (para == s_meu_num);
  snprintf(s_msgs[0].texto, sizeof(s_msgs[0].texto), "%s", txt);
  s_msgs[0].quando = quando;
  s_msgs[0].snr = snr;
  s_msgs[0].rssi = rssi;
  s_msgs[0].via_mqtt = via_mqtt;
  s_msgs[0].saltos = saltos;
  s_msgs[0].id = id;

  // MENSAGEM NOSSA NAO E "NAO LIDA". Parece obvio e nao era: o contador subia
  // para tudo, e o tab5_mesh_enviar corrigia DEPOIS decrementando s_nao_lidas -
  // mas esquecia o contador DA CONVERSA. Resultado: mandar no LongFast deixava
  // um "1" na conversa que voce estava olhando.
  //
  // A decisao tem que morar aqui, onde se sabe de quem e a mensagem, e nao
  // num remendo do lado de quem envia.
  const bool minha = (de == s_meu_num);

  // ACOMPANHA TAMBEM O QUE SAIU DO CELULAR.
  //
  // Ate agora so entrava no anel o que o Tab5 mandava - entao mensagem enviada
  // pelo app do celular atraves do C6L aparecia na conversa SEM marca nenhuma,
  // e voce ficava sem saber se chegou.
  //
  // Mas o ACK dela CHEGA ATE NOS: o C6L repassa todo o trafego de FromRadio
  // para cada cliente conectado, e nos somos um. A gente so o descartava por
  // nao ter o id na tabela. Registrando aqui, o mesmo Routing que marca as
  // nossas passa a marcar as dela.
  //
  // A condicao e "veio do nosso no e tem id": e a definicao de "mensagem que
  // este radio transmitiu", tenha sido esta tela ou o celular a pedir.
  //
  // SO TRAFEGO AO VIVO. Durante o dump o no pode reentregar uma mensagem nossa
  // antiga: anotada aqui ela virava ENVIANDO e, 60 s depois, "sem resposta" -
  // gravado no envios.txt POR CIMA do desfecho verdadeiro que ela ja tinha.
  if (minha && id != 0 && trafego_ao_vivo()) { anotar_envio(id, para); }

  // A CONVERSA E SEMPRE "O OUTRO LADO".
  //
  // Difusao vai para o canal. Direta vai para o no do OUTRO - que e o
  // remetente quando recebemos, e o DESTINATARIO quando somos nos que
  // enviamos. Sem essa distincao, mensagem direta enviada por nos caia na
  // conversa do canal, porque o teste de "direta" olhava so o destinatario.
  const bool difusao = (para == 0xFFFFFFFFu);
  const uint32_t outro = (de == s_meu_num) ? para : de;
  const int ci = difusao ? achar_ou_criar_conversa(true, canal, 0)
                         : achar_ou_criar_conversa(false, 0, outro);
  if (ci >= 0) {
    if (!minha) { s_conv[ci].nao_lidas++; }
    s_conv[ci].ultima_em = quando;
    // Sobe para o topo das diretas. Canal tem lugar fixo (pelo indice).
    if (!difusao) { s_conv_seq[ci] = ++s_seq; ++s_conv_ver; }
    gravar_msg(ci, &s_msgs[0]);
  }

  // Mensagem de verdade (enviada ou recebida) e a volta a se falar que desfaz
  // um apagar anterior - ver o comentario grande na flag, acima. VALE PARA OS
  // DOIS (canal tambem se apaga agora): a linha de cima ja
  // recria a conversa do canal incondicionalmente (e deveria - uma mensagem
  // de verdade e o proprio sinal de "voltou"), mas SEM desmarcar aqui a flag
  // continuaria escrita no cartao e o canal sumiria nao no proximo boot.
  if (difusao) { desmarcar_apagada(true, canal); }
  else         { desmarcar_apagada(false, outro); }

  // O CONTADOR SOBE SO DEPOIS DE A LINHA ESTAR NO GRAVADOR (gravar_msg, acima).
  // A tela le este numero sem trava: subindo antes, ela podia pedir a releitura
  // do cartao com a linha ainda nem entregue - e nada mais a fazia reler
  // depois. Este NUNCA satura - ver o aviso grande junto da declaracao.
  ++s_total_msgs;

  publicar_notif();
  // O ID VAI NO LOG: mensagem repetida que passa por aqui ja passou pelo
  // descarte por id (id_ja_visto). Com o id a vista, da para saber de cara se
  // a repeticao e da REDE (id novo - o remetente ou uma ponte reenviou) ou
  // nossa (id igual - o descarte falhou). E a origem: radio ou internet.
  ESP_LOGI(TAG, "mensagem %s de %08lX (id %08lX, %s%s): %s",
           s_msgs[0].direta ? "DIRETA" : "de canal", (unsigned long)de, (unsigned long)id,
           via_mqtt ? "internet" : "radio", s_rede_rx == MESH_REDE_INTERNET ? " publica" : "", txt);

  sistema_avisar(AVISO_MESH_MSG);
}

// "OS NOS MUDARAM" PARA A TELA. Da internet, no maximo um por segundo: la chegam
// dezenas de pacotes por segundo, e cada aviso fazia a pagina de nos e a
// coluna se conferirem de novo. A tabela muda na hora; so o aviso espera.
static void avisar_nos(void)
{
  // E na rede do no ate 4 por segundo: a ponte MQTT tambem chega em rajada.
  static uint32_t ultimo_ms[2] = { 0, 0 };
  const int k = (s_rede_rx == MESH_REDE_INTERNET) ? 1 : 0;
  const uint32_t t = tab5_now_ms();
  if (t - ultimo_ms[k] < (k ? 1000u : 250u)) { return; }
  ultimo_ms[k] = t;
  sistema_avisar(AVISO_MESH_NOS);
}

// Data (dentro do MeshPacket): 1=portnum 2=payload
#define PORT_TEXTO      1
#define PORT_POSICAO    3
#define PORT_NODEINFO   4
#define PORT_ROTEAMENTO 5
#define PORT_TELEMETRIA 67

static void ler_data(pb_leitor_t l, uint32_t de, uint32_t para, uint8_t canal,
                     uint32_t quando, float snr, int32_t rssi, uint32_t id,
                     bool via_mqtt, uint8_t saltos, bool transp_mqtt)
{
  int porta = 0;
  uint32_t pedido = 0;                 // request_id: de qual pacote NOSSO isto fala
  const uint8_t* pay = nullptr; size_t pay_n = 0;
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v; pb_varint(&l,&v); porta = (int)v; }
    else if (n == 2 && t == 2) {
      uint64_t k; if (!pb_varint(&l,&k)) break;
      if (k > pb_restam(&l)) break;              // ver pb_restam(): somar no ponteiro estoura
      pay = l.p; pay_n = (size_t)k; l.p += (size_t)k;
    }
    else if (n == 6 && t == 5) { pb_fixo32(&l, &pedido); }   // request_id
    else if (!pb_pular(&l, t)) { break; }
  }

  // A CONFIRMACAO NAO TEM CORPO OBRIGATORIO.
  //
  // Um ACK e um Routing com error_reason AUSENTE - e campo ausente em protobuf
  // nao ocupa byte nenhum, entao o payload vem VAZIO. Testar "if (!pay) return"
  // antes daqui, como o codigo fazia, descartaria justamente o caso de sucesso
  // e so o erro apareceria na tela.
  if (porta == PORT_ROTEAMENTO) {
    if (pedido == 0) { return; }
    int erro = 0;
    if (pay) {
      pb_leitor_t rl = { pay, pay + pay_n };
      int rn, rt;
      while (pb_proximo_campo(&rl, &rn, &rt)) {
        if (rn == 3 && rt == 0) { uint64_t v; pb_varint(&rl,&v); erro = (int)v; }
        else if (!pb_pular(&rl, rt)) { break; }
      }
    }
    marcar_envio(pedido, erro, de, transp_mqtt);
    return;
  }

  if (!pay) { return; }

  // A APRESENTACAO DE UM NO, AO VIVO (NODEINFO_APP, payload = User).
  //
  // O FromRadio.node_info so vem no DUMP de configuracao - o no NAO o reenvia
  // quando um vizinho se apresenta depois; ele repassa o PACOTE, e o cliente
  // e quem le o User de dentro dele. Sem ler esta porta, todo no que
  // aparecesse depois do dump (e TODO no no modo internet, que nao tem dump
  // de verdade) ficava sem nome, sem modelo e sem MAC ate o proximo reinicio
  // do radio - e a conversa com ele aparecia pelo id em hexa.
  if (porta == PORT_NODEINFO) {
    mesh_no_t* no = achar_no(de);
    if (de == s_meu_num && s_meu_num) { mesh_admin_guardar_user(pay, pay_n); }
    if (no) {
      ler_user(pb_leitor_t{ pay, pay + pay_n }, no);
      lembrar_nome(de, curto_utilizavel(no->curto) ? no->curto : nullptr,
                   no->longo[0] ? no->longo : nullptr);
      ++s_nos_ver;
      avisar_nos();
    }
    return;
  }

  // POSICAO AO VIVO (POSITION_APP, payload = Position). Mesmo motivo: sem
  // isto a posicao de um no so era a do dump, e envelhecia para sempre.
  if (porta == PORT_POSICAO) {
    mesh_no_t* no = achar_no(de);
    if (no) { ler_position(pb_leitor_t{ pay, pay + pay_n }, no); }
    return;
  }

  if (porta == PORT_TEXTO) {
    char txt[MESH_TEXTO];
    size_t c = (pay_n < sizeof(txt) - 1) ? pay_n : sizeof(txt) - 1;
    memcpy(txt, pay, c); txt[c] = 0;
    // QUEBRA DE LINHA VIRA ESPACO. O historico no cartao e uma mensagem por
    // linha: um "\n" no meio (comum no que vem da ponte do Telegram) cortava a
    // mensagem ali, e o resto virava uma linha sem "M4," que a leitura pula.
    for (size_t k = 0; k < c; ++k) { if (txt[k] == '\n' || txt[k] == '\r' || txt[k] == '\t') { txt[k] = ' '; } }
    s_c_texto++;
    // SEM LOG AQUI. O
    // via_mqtt/saltos DESTE pacote (antes de qualquer fallback da tela) fica
    // gravado em s_msgs[0] normalmente - de "mesh" no console consegue ler
    // a mensagem mais recente com tab5_mesh_msg(0, ...) e ver os dois campos
    // crus, sob demanda, sem precisar de log automatico nenhum.
    guardar_msg(de, para, canal, txt, quando, snr, rssi, id, via_mqtt, saltos);
  }
  else if (porta == PORT_TELEMETRIA) {
    // Telemetry: 2=device_metrics, 6=local_stats. As outras variantes
    // (ambiente, qualidade do ar, energia) ficam para quando houver no com
    // sensor - pular e seguro e nao custa nada.
    s_c_telemetria++;
    pb_leitor_t tl = { pay, pay + pay_n };
    int tn, tt;
    while (pb_proximo_campo(&tl, &tn, &tt)) {
      s_ultima_variante = tn;    // qual variante de Telemetry veio por ultimo
      // Campo 1 = time: a hora em que o NOSSO no gerou a telemetria.
      if      (tn == 1 && tt == 5) { uint32_t v = 0; pb_fixo32(&tl, &v); if (de == s_meu_num) { marcar_hora_viva(v); } }
      else if (tn == 2 && tt == 2) { pb_leitor_t s; if (pb_sub(&tl,&s)) ler_metrics(s, achar_no(de), de == s_meu_num); }
      // LocalStats so faz sentido do proprio no: e a visao DELE da malha.
      else if (tn == 6 && tt == 2 && de == s_meu_num) { s_c_localstats++; pb_leitor_t s; if (pb_sub(&tl,&s)) ler_local_stats(s); }
      else if (!pb_pular(&tl, tt)) { break; }
    }
  }
}

// MeshPacket: 1=from 2=to 3=channel 4=decoded 6=id 7=rx_time 8=rx_snr
// 9=hop_limit 12=rx_rssi 13=via_mqtt (bool, varint) 14=hop_start - os dois
// ultimos lidos para a tela distinguir mensagem ouvida no ar
// (com o numero de saltos) de mensagem que entrou pela ponte MQTT.
static void ler_packet(pb_leitor_t l)
{
  uint32_t de = 0, quando = 0, para = 0xFFFFFFFFu, id = 0;
  uint8_t canal = 0;
  float snr = 0; int32_t rssi = 0;
  bool via_mqtt = false;
  // hop_limit: quanto SOBROU do orcamento de saltos quando o pacote chegou
  // aqui. hop_start: o orcamento ORIGINAL que o remetente armou - viaja
  // junto, sem mudar, pacote a pacote. A diferenca e quantos repetidores ele
  // passou. Zero em hop_start e o valor de fabrica do protobuf (campo
  // ausente) - firmware antigo nao manda, entao a conta fica indefinida.
  uint32_t hop_limit = 0, hop_start = 0;
  // transport_mechanism (21): por onde o pacote entrou no radio. So o MQTT (5)
  // interessa aqui - ver "O IMPLICITO DO MQTT" em marcar_envio().
  bool transp_mqtt = false;
  pb_leitor_t dados = { nullptr, nullptr };
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 5) { pb_fixo32(&l, &de); }
    else if (n == 2 && t == 5) { pb_fixo32(&l, &para); }
    else if (n == 3 && t == 0) { uint64_t v; pb_varint(&l,&v); canal = (uint8_t)v; }
    else if (n == 6 && t == 5) { pb_fixo32(&l, &id); }       // id do pacote
    else if (n == 7 && t == 5) { pb_fixo32(&l, &quando); }
    else if (n == 8 && t == 5) { pb_float(&l, &snr); }
    else if (n == 9 && t == 0) { uint64_t v; pb_varint(&l,&v); hop_limit = (uint32_t)v; }
    else if (n == 12 && t == 0){ uint64_t v; pb_varint(&l,&v); rssi = (int32_t)v; }
    // CAMPOS 14 E 15, conferidos contra o mesh.pb.h do firmware do C6L
    // via_mqtt = 14, hop_start = 15. Ate aqui eram lidos 13 e 14
    // - o 13 e o 'delayed' (obsoleto, sempre 0) e o 14 era o via_mqtt tomado
    // por hop_start. Resultado: MQTT nunca era reconhecido pelo pacote e o
    // numero de saltos saia errado ou "--" (dai o fallback pelo NodeInfo).
    else if (n == 14 && t == 0){ uint64_t v; pb_varint(&l,&v); via_mqtt = (v != 0); }
    else if (n == 15 && t == 0){ uint64_t v; pb_varint(&l,&v); hop_start = (uint32_t)v; }
    else if (n == 21 && t == 0){ uint64_t v; pb_varint(&l,&v); transp_mqtt = (v == 5); }
    else if (n == 4 && t == 2) { if (!pb_sub(&l, &dados)) break; }
    else if (!pb_pular(&l, t)) { break; }
  }
  // ZERO SIGNIFICA "EU". O Meshtastic usa from=0 em pacote gerado localmente -
  // aparece como "fr=0x00000000" no log do proprio radio. Sem traduzir isso:
  //   - o LocalStats era descartado (o filtro exigia de == meu numero), e os
  //     contadores de nos ficavam em 0/0;
  //   - e um no fantasma de numero 0 entrava na tabela.
  if (de == 0) { de = s_meu_num; }

  // SALTOS = hop_start - hop_limit, so quando hop_start veio preenchido (>0).
  // hop_start == 0 e o padrao de fabrica do protobuf (campo ausente,
  // firmware antigo) - nesse caso a conta fica indefinida, e o sentinela diz
  // isso em vez de inventar um "0 saltos" que seria mentira.
  uint8_t saltos = MESH_SALTOS_DESCONHECIDO;
  if (hop_start > 0 && hop_start >= hop_limit) {
    saltos = (uint8_t)(hop_start - hop_limit);
  }

  // Interpretado no fim porque os metadados (quem, quando, com que sinal) podem
  // vir DEPOIS do bloco de dados na serializacao.
  if (dados.p) { ler_data(dados, de, para, canal, quando, snr, rssi, id, via_mqtt, saltos, transp_mqtt); }

  marcar_epoch(quando);
  marcar_hora_viva(quando);

  if (de != 0) {
    mesh_no_t* no = achar_no(de);
    if (no) {
      if (snr != 0) { no->snr = snr; }
      // SALTOS/VIA_MQTT TAMBEM AQUI - a tela mostrava pouco mais que o ID,
      // comparada com o cliente web. Os dois numeros ja eram calculados ALI EM CIMA para TODO
      // pacote (ver 'saltos'/'via_mqtt' no comeco desta funcao) e ja iam
      // para a MENSAGEM (ler_data) - so nunca tinham sido aplicados ao NO.
      // A tabela de nos so aprendia essa coluna quando um NodeInfo chegava
      // (campo 9/8, ler_node_info() mais acima) - um pacote raro (rajada no
      // dump inicial, depois so de vez em quando), enquanto QUALQUER
      // pacote - mensagem, telemetria, posicao - ja carrega hop_start/
      // hop_limit/via_mqtt na propria borda (MeshPacket). O "Desconhecido"
      // ficava sozinho ate o proximo NodeInfo, mesmo com dezenas de outros
      // pacotes do mesmo no passando por aqui antes disso.
      //
      // So sobrescreve saltos quando ESTE pacote realmente trouxe hop_start
      // (saltos != DESCONHECIDO) - um pacote sem essa info nao deve apagar
      // um valor bom que outro pacote ja tinha ensinado. via_mqtt e sempre o
      // caminho MAIS RECENTE (um no pode ser ouvido direto numa hora e via
      // ponte na outra), entao sobrescreve sem guarda.
      if (saltos != MESH_SALTOS_DESCONHECIDO) { no->saltos = saltos; }
      no->via_mqtt = via_mqtt;
      // AVISTAMENTO AO VIVO. Marcado aqui e nao no ler_node_info de proposito:
      // o node_info chega em rajada no dump de configuracao, para TODOS os nos
      // de uma vez, e carimbar todos ali faria a malha inteira parecer ter sido
      // ouvida agora. Pacote e diferente - ele so chega quando o no falou.
      no->visto_local_ms = tab5_now_ms();
      if (quando >= EPOCH_MINIMO) { no->visto_em = quando; }
      fixar_se_pelo_ar(no);   // ouvido pelo ar agora: ganha o lugar no topo e a protecao do despejo
      ++s_nos_ver;
    }
  }
  avisar_nos();
}

// LocalStats vem como telemetria do proprio no: 1=uptime 2=uso 3=airtime
// 4..=contagens, 8=nos_online, 9=nos_total, 10=piso_ruido (varia com a versao;
// pulamos o que nao reconhecemos).
static void ler_from_radio(const uint8_t* buf, size_t n)
{
  pb_leitor_t l = { buf, buf + n };
  int f, t;

  // LOOP, e nao um unico campo. O FromRadio nao e so a variante: ele tem
  // "uint32 id = 1" FORA do oneof, e protobuf serializa em ordem de numero de
  // campo. Quando o radio preenche esse id - e ele preenche em alguns caminhos
  // do firmware - o primeiro campo do frame e o 1, nenhum ramo abaixo casa, e
  // a MENSAGEM INTEIRA ERA DESCARTADA em silencio.
  //
  // Ler so o primeiro campo era apostar que o id viria sempre zerado (e campo
  // zerado o nanopb omite). A aposta funciona quase sempre, que e a pior
  // especie de erro: some do teste e aparece no uso.
  while (pb_proximo_campo(&l, &f, &t))
  {
  // CAMPO 2, nao 1. O campo 1 do FromRadio e "id"; o pacote e o 2.
  // Conferido no protobuf oficial (meshtastic.protobuf.mesh_pb2).
  // Com o numero errado, TODO pacote chegava e era descartado como campo
  // desconhecido - o contador dizia "0 pacotes" com o enlace perfeito.
  if (f == 2 && t == 2) { s_c_pacotes++; pb_leitor_t s; if (pb_sub(&l,&s)) ler_packet(s); }
  else if (f == 3 && t == 2) {           // my_info
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      mesh_admin_guardar_my_info(s.p, (size_t)(s.fim - s.p));   // reboot_count, nodedb_count
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 0) {
          uint64_t v; pb_varint(&s,&v);
          const uint32_t novo = (uint32_t)v;
          // So escreve quando MUDA: a NVS tem ciclos de escrita finitos e o
          // my_info chega a cada apresentacao.
          if (novo != 0 && novo != s_meu_num) { lembrar_meu_numero(novo); }
          s_meu_num = novo;
        }
        else if (n2 == 8 && t2 == 0) {   // reboot_count: o diario de reinicios compara com o anterior
          uint64_t v; pb_varint(&s, &v);
          // s_ultimo_quadro_ms ainda e o do quadro ANTERIOR (so atualiza depois
          // de ler_from_radio): zero = primeiro contato desta sessao.
          mesh_diario_contador((uint32_t)v, s_saude.uptime_s, s_ultimo_quadro_ms != 0, mesh_admin_gravando());
        }
        else if (!pb_pular(&s, t2)) { break; }
      }
    }
  }
  else if (f == 4 && t == 2) { s_c_nodeinfo++; pb_leitor_t s; if (pb_sub(&l,&s)) ler_node_info(s); }
  else if (f == 11 && t == 2) {          // queueStatus: 1=res 4=mesh_packet_id
    // Falha IMEDIATA, antes de o pacote sair pelo radio: a queue de transmissao
    // nao aceitou. Sem isto o envio ficaria "enviando" ate o prazo estourar,
    // quando o radio ja tinha dito que nem tentou.
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      int32_t res = 0; uint32_t pid = 0;
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if      (n2 == 1 && t2 == 0) { uint64_t v; pb_varint(&s,&v); res = (int32_t)v; }
        // mesh_packet_id e UINT32 (varint), nao fixed32 - conferido no
        // mesh.pb.h. Lido como fixed32 ele nunca casava, e a
        // recusa da queue do radio nunca marcava o envio como falho.
        else if (n2 == 4 && t2 == 0) { uint64_t v; pb_varint(&s, &v); pid = (uint32_t)v; }
        else if (!pb_pular(&s, t2)) { break; }
      }
      if (pid != 0 && res != 0) { marcar_envio(pid, MESH_ERRO_FILA, s_meu_num); }
    }
  }
  else if (f == 7 && t == 0) {           // configCompleteId
    // FIM DO DOWNLOAD DE CONFIGURACAO. E daqui em diante que o radio comeca a
    // entregar o trafego AO VIVO - antes disso ele esta drenando a queue de
    // configuracao. Nao ha nada a fazer alem de registrar, mas registrar
    // importa: sem esta linha no log nao da para distinguir "o dump nunca
    // terminou" de "terminou e nao chega mensagem", e o conserto e outro.
    uint64_t v;
    if (pb_varint(&l, &v)) {
      s_config_ok_ms = tab5_now_ms();   // a hora ao vivo conta daqui (ver marcar_hora_viva)
      // A configuracao dos canais chegou inteira: s_canais_sujos ja esta de
      // pe, e a carona da tarefa "cartao" grava em ate 1 s (fora do radio).
      mesh_admin_fim_dump();         // a configuracao do no tambem
      enviar_favs_pendentes();       // estrelas mudadas sem o radio
      ESP_LOGI(TAG, "configuracao completa (id %lu) - trafego ao vivo daqui",
               (unsigned long)v);
    }
  }
  else if (f == 8 && t == 0) {           // rebooted
    uint64_t v;
    if (pb_varint(&l, &v) && v != 0) {
      // TENSAO DO BARRAMENTO NO INSTANTE DO REINICIO
      // ("tem como saber se os 5V do Grove estao chegando?"). Nao medimos o
      // Grove especificamente (so existe o INA226 do barramento da BATERIA,
      // 2S Li-Po ~7.4V - ver tab5_power.hpp), e nao dá pra medir agora
      // mesmo (tab5_power_latest() so, nunca _medir() daqui: esta e a
      // tarefa do Mesh, nao a de interface, e o barramento I2C interno nao
      // tem mutex - ver o aviso grande em tab5_power.hpp). O que "ultima"
      // devolve e uma leitura em cache de ATE 1s atras, atualizada pela
      // interface - resolucao grosseira para um afundamento rapido de
      // tensao, mas se o barramento estiver visivelmente baixo bem na hora
      // do reinicio, e um indicio real de energia, nao so uma suspeita.
      mesh_diario_foto(s_saude.uptime_s);   // contexto do Tab5 neste instante
      const tab5_measurement_t m = tab5_power_latest();
      ESP_LOGW(TAG, "o radio reiniciou - vou pedir a configuracao de novo "
                    "(barramento no instante: %.2fV %.0fmA, ate 1s atras)",
               (double)m.volts, (double)m.mA);
      s_radio_reiniciou = true;
    }
  }
  // CONFIG E MODULECONFIG - so o que o modo internet precisa: a regiao
  // (Config.lora.region, que vai na raiz do topico) e o servidor MQTT do no
  // (ModuleConfig.mqtt: 2=endereco 3=usuario 4=senha 8=raiz). Endereco vazio
  // = o servidor publico, que e o padrao do firmware.
  else if (f == 5 && t == 2) {           // config: oneof 1..10 = secao; 6=lora { 7=region }
    pb_leitor_t s, sec;
    int n2, t2;
    if (pb_sub(&l, &s)) {
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (t2 != 2) { if (!pb_pular(&s, t2)) { break; } continue; }
        if (!pb_sub(&s, &sec)) { break; }
        // A SECAO CRUA vai para a configuracao do no (tab5_mesh_admin): e ela
        // que volta, inteira, quando um campo e alterado.
        mesh_admin_guardar_secao(false, n2, sec.p, (size_t)(sec.fim - sec.p));
        if (n2 != 6) { continue; }
        int n3, t3;
        while (pb_proximo_campo(&sec, &n3, &t3)) {
          if (n3 == 7 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); mesh_mqtt_aprender_regiao((int)v); }
          else if (!pb_pular(&sec, t3)) { break; }
        }
      }
    }
  }
  else if (f == 9 && t == 2) {           // moduleConfig: oneof 1..17 = modulo; 1=mqtt
    pb_leitor_t s, sec;
    int n2, t2;
    if (pb_sub(&l, &s)) {
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (t2 != 2) { if (!pb_pular(&s, t2)) { break; } continue; }
        if (!pb_sub(&s, &sec)) { break; }
        mesh_admin_guardar_secao(true, n2, sec.p, (size_t)(sec.fim - sec.p));
        if (n2 != 1) { continue; }
        char end[64] = "", usr[32] = "", pwd[32] = "", raiz[48] = "";
        bool mqtt_lig = false, tls = false, proxy = false;   // proto3: ausente = false
        int n3, t3;
        while (pb_proximo_campo(&sec, &n3, &t3)) {
          if      (n3 == 1 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); mqtt_lig = (v != 0); }
          else if (n3 == 7 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); tls = (v != 0); }
          else if (n3 == 9 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); proxy = (v != 0); }
          else if (n3 == 2 && t3 == 2) { pb_texto(&sec, end,  sizeof(end)); }
          else if (n3 == 3 && t3 == 2) { pb_texto(&sec, usr,  sizeof(usr)); }
          else if (n3 == 4 && t3 == 2) { pb_texto(&sec, pwd,  sizeof(pwd)); }
          else if (n3 == 8 && t3 == 2) { pb_texto(&sec, raiz, sizeof(raiz)); }
          else if (!pb_pular(&sec, t3)) { break; }
        }
        mesh_mqtt_aprender_servidor(end, usr, pwd, raiz);
        mesh_mqtt_aprender_no_mqtt(mqtt_lig, proxy, tls);
      }
    }
  }
  else if (f == 10 && t == 2) {          // channel: 1=index 2=settings 3=role
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      const uint8_t* ch_ini = s.p;
      const size_t   ch_n = (size_t)(s.fim - s.p);
      // OS DOIS COMECAM EM ZERO, e isso e o conserto de dois bugs.
      //
      // 'index' (int32) e 'role' (enum) sao campos proto3 SINGULAR: o nanopb
      // NAO os escreve quando valem zero. Comecando em -1 e 1, como era:
      //   - o canal 0 (o primario, LongFast) chegava SEM o campo 1, ficava com
      //     idx -1 e a configuracao inteira dele era ignorada - nunca entrava
      //     como ativo, e por isso nao aparecia para reabrir depois de apagado;
      //   - um canal DESABILITADO (role 0) chegava SEM o campo 3 e era lido como
      //     PRIMARY - continuava "ativo", com o nome antigo, na coluna.
      // O valor ausente e o zero do protobuf: e isso que o firmware quis dizer.
      int idx = 0, papel = 0;
      bool downlink = false;
      char nome[MESH_NOME_CONVERSA] = { 0 };
      uint8_t psk[32];
      size_t  n_psk = 0;
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if      (n2 == 1 && t2 == 0) { uint64_t v; pb_varint(&s,&v); idx = (int)v; }
        else if (n2 == 3 && t2 == 0) { uint64_t v; pb_varint(&s,&v); papel = (int)v; }
        else if (n2 == 2 && t2 == 2) {           // ChannelSettings: 2=psk 3=name
          pb_leitor_t cs;
          if (pb_sub(&s,&cs)) {
            int n3, t3;
            while (pb_proximo_campo(&cs, &n3, &t3)) {
              if (n3 == 3 && t3 == 2) { pb_texto(&cs, nome, sizeof(nome)); }
              else if (n3 == 6 && t3 == 0) { uint64_t v; pb_varint(&cs, &v); downlink = (v != 0); }   // o proxy assina
              else if (n3 == 2 && t3 == 2) {   // psk: a chave, para o modo internet
                uint64_t k;
                if (!pb_varint(&cs, &k) || k > pb_restam(&cs)) { break; }
                n_psk = (k <= sizeof(psk)) ? (size_t)k : 0;
                if (n_psk) { memcpy(psk, cs.p, n_psk); }
                cs.p += (size_t)k;
              }
              else if (!pb_pular(&cs, t3)) { break; }
            }
          }
        }
        else if (!pb_pular(&s, t2)) { break; }
      }
      // O Channel CRU vai para a configuracao do no (a precisao da posicao do
      // primario e editada la, e volta com o resto dele intacto).
      mesh_admin_guardar_canal(idx, ch_ini, ch_n);
      if (idx >= 0 && idx < MESH_MAX_CANAIS) {
        mesh_mqtt_aprender_downlink(idx, papel != 0 && downlink);   // antes: aplicar_canal ensina o nome
        aplicar_canal(idx, papel, nome, psk, n_psk);
      }
    }
  }
  else if (f == 14 && t == 2) {          // mqttClientProxyMessage: 1=topic 2=data 3=text 4=retained
    // O NO PEDINDO PARA PUBLICAR (proxy ligado nele). Sem o proxy do lado do
    // Tab5 a mensagem e so descartada - o no nao espera resposta.
    pb_leitor_t s;
    if (pb_sub(&l, &s)) {
      static char topico[128];   // so a tarefa do servico le FromRadio
      topico[0] = 0;
      const uint8_t* dado = nullptr; size_t n_dado = 0;
      bool retido = false;
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 2) { pb_texto(&s, topico, sizeof(topico)); }
        else if ((n2 == 2 || n2 == 3) && t2 == 2) {   // data (bytes) ou text: os dois vao crus
          uint64_t k;
          if (!pb_varint(&s, &k) || k > pb_restam(&s)) { break; }
          dado = s.p; n_dado = (size_t)k; s.p += (size_t)k;
        }
        else if (n2 == 4 && t2 == 0) { uint64_t v; pb_varint(&s, &v); retido = (v != 0); }
        else if (!pb_pular(&s, t2)) { break; }
      }
      if (mesh_mqtt_em_proxy() && topico[0] && dado) { mesh_mqtt_proxy_publicar(topico, dado, n_dado, retido); }
    }
  }
  else if (f == 16 && t == 2) {          // clientNotification: aviso do firmware para quem usa
    pb_leitor_t s;
    if (pb_sub(&l, &s)) { mesh_admin_guardar_aviso(s.p, (size_t)(s.fim - s.p)); }
  }
  else if (f == 13 && t == 2) {          // metadata: 1=firmware_version
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      mesh_admin_guardar_metadata(s.p, (size_t)(s.fim - s.p));
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 2) { pb_texto(&s, s_firmware, sizeof(s_firmware)); }
        else if (!pb_pular(&s, t2)) { break; }
      }
    }
  }
  // config e o resto: pulados, para o loop chegar ao campo seguinte em vez de
  // parar no primeiro desconhecido.
  else if (!pb_pular(&l, t)) { break; }
  }
}

// ---------------------------------------------------------------------------
// Tarefa
// ---------------------------------------------------------------------------
// ToRadio{ heartbeat: {} } enquadrado.
//   campo 7, tipo 2 -> tag (7<<3)|2 = 0x3A, comprimento 0
//
// O radio espera que o cliente se manifeste de tempos em tempos; o log do C6L
// mostra "Got client heartbeat" vindo do celular. Sem isto ele pode considerar
// o cliente ausente e parar de entregar trafego - e a ponte fica viva mas muda.
// O ENLACE: entrega o corpo de um ToRadio pela via do servico. Na serial ele
// vai num frame 0x94 0xC3 <tamanho>; no BLE e o proprio valor da
// caracteristica toRadio (ver tab5_mesh_ble.hpp). E o UNICO lugar que sabe
// qual e a via - o resto do servico so monta protobuf.
// Com o no fora e o Wi-Fi de pe, a internet assume (ver usar_internet no
// laco do servico): o que sairia pelo radio vai pelo MQTT.
// Criada uma vez e nunca apagada: o enlace pode ser chamado da interface
// enquanto o servico para e sobe de novo.
static SemaphoreHandle_t s_tx = nullptr;

// Um ToRadio do proxy (a descida): o teto do firmware, pelo cabo e pelo BLE.
#define QUADRO_PROXY_MAX 512

static bool enlace_enviar(const uint8_t* corpo, size_t n)
{
  // Para o diario de reinicios: o que o Tab5 mandou por ultimo. ToRadio:
  // 1 = pacote, 3 = want_config_id, 7 = batimento.
  if (n > 0) {
    const int campo = corpo[0] >> 3;
    mesh_diario_nota_envio(campo == 1 ? MESH_ENV_PACOTE : campo == 3 ? MESH_ENV_CONFIG
                         : campo == 7 ? MESH_ENV_HEARTBEAT : MESH_ENV_NENHUM);
  }
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { return mesh_mqtt_enviar(corpo, n); }
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    if (!mesh_ble_conectado() && mesh_mqtt_conectado()) { return mesh_mqtt_enviar(corpo, n); }
    return mesh_ble_enviar(corpo, n);
  }
  // UM QUADRO POR VEZ NA UART. Cabecalho e corpo sao duas escritas, e
  // escrevem aqui a tarefa do Mesh (batimento, configuracao) e a de interface
  // (texto, favorito, admin). Sem a trava, um batimento podia cair entre o
  // cabecalho e o corpo de uma mensagem: o no lia um tamanho e recebia outro
  // quadro no lugar - as duas se perdiam sem erro nenhum.
  if (s_tx) { xSemaphoreTake(s_tx, portMAX_DELAY); }
  uint8_t cab[4] = { 0x94, 0xC3, (uint8_t)(n >> 8), (uint8_t)(n & 0xFF) };
  uart_write_bytes(MESH_UART, cab, sizeof(cab));
  uart_write_bytes(MESH_UART, corpo, n);
  if (s_tx) { xSemaphoreGive(s_tx); }
  return true;
}

static void mandar_heartbeat(void)
{
  const uint8_t q[] = { 0x3A, 0x00 };
  enlace_enviar(q, sizeof(q));
}

// PEDIR A CONFIGURACAO DE NOVO daqui a pouco - depois de gravar algo no no
// (tab5_mesh_admin): ele reinicia e, pela serial, nao fala sozinho.
static volatile uint32_t s_reapresentar_ms = 0;
static volatile bool     s_acordar_proxima = false;

static volatile bool     s_so_config = false;

void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou)
{
  // Reiniciado, ele pode estar no modo de baixo consumo da serial e precisar
  // da rajada de 0x94. Sem reinicio, NAO: a rajada no meio de uma conversa e
  // ruido na maquina de quadros dele (ver pedir_apresentacao).
  // Sem reinicio (posicao fixa posta ou tirada) so a CONFIGURACAO mudou: o
  // pedido e o "so configuracao", sem a NodeDB inteira atras.
  if (reiniciou) { s_acordar_proxima = true; s_so_config = false; }
  else           { s_so_config = true; }
  s_reapresentar_ms = (tab5_now_ms() + ms) | 1u;
}

static void pedir_apresentacao(void)
{
  // A RAJADA DE 0x94 SO NA PRIMEIRA VEZ.
  //
  // Ela existe para tirar o radio do modo de baixo consumo da serial - e o
  // procedimento documentado de acordar. Mas sao 32 bytes de lixo entrando na
  // maquina de enquadramento DELE, e mandar isso com a conversa ja estabelecida
  // e injetar ruido no meio de um frame que pode estar chegando ao mesmo
  // tempo. Depois do primeiro contato o radio esta acordado por definicao: ele
  // acabou de falar conosco.
  if ((s_meu_num == 0 || s_acordar_proxima) && s_via == SETTINGS_MESH_TRANSPORT_SERIAL) {   // BLE nao tem modo de baixo consumo da serial
    s_acordar_proxima = false;
    uint8_t acorda[32];
    memset(acorda, 0x94, sizeof(acorda));
    if (s_tx) { xSemaphoreTake(s_tx, portMAX_DELAY); }
    uart_write_bytes(MESH_UART, acorda, sizeof(acorda));
    if (s_tx) { xSemaphoreGive(s_tx); }
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  // SO A CONFIGURACAO quando so ela mudou: want_config_id 69420 e o
  // SPECIAL_NONCE_ONLY_CONFIG do firmware (PhoneAPI) - canais, config e
  // modulos, sem a NodeDB, que pelo Bluetooth com uma malha cheia sao
  // centenas de quadros so para saber que um campo virou. Firmware antigo nao
  // conhece o numero e manda tudo, como antes. Qualquer outro motivo (primeiro
  // contato, reinicio, silencio) pede tudo.
  const bool so_config = s_so_config && s_meu_num != 0;
  s_so_config = false;
  static const uint8_t TUDO_Q[]   = { 0x18, 0x2A };               // ToRadio{ want_config_id: 42 }
  static const uint8_t CONFIG_Q[] = { 0x18, 0xAC, 0x9E, 0x04 };   // ToRadio{ want_config_id: 69420 }
  s_config_ok_ms = 0;   // dump novo: o que vier ate o fim dele e passado
  mesh_admin_inicio_dump();
  if (so_config) { enlace_enviar(CONFIG_Q, sizeof(CONFIG_Q)); }
  else           { enlace_enviar(TUDO_Q, sizeof(TUDO_Q)); }
}

// ---------------------------------------------------------------------------
// Envio
// ---------------------------------------------------------------------------
// Texto, admin e posicao saem no MESMO envelope ToRadio{ packet: MeshPacket{
// to, channel, decoded: Data{ portnum, payload }, id, hop_limit, want_ack } },
// montado num lugar so: mesh_pb_to_radio() (services/mesh_pb.hpp).

// ---------------------------------------------------------------------------
// FILA DE ENVIO DE TEXTO
// ---------------------------------------------------------------------------
typedef struct { uint16_t n; bool internet; uint32_t id; uint8_t d[MESH_TEXTO + 96]; } envio_fila_t;
static QueueHandle_t s_fila_envio = nullptr;
#define MESH_FILA_ENVIO 8

// Ja da para dizer que nao sai? (ver tab5_mesh_enviar)
static bool pode_sair(bool internet, uint32_t para)
{
  const bool difusao = (para == 0xFFFFFFFFu);
  if (internet) { return difusao && mesh_mqtt_internet_conectado(); }
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { return difusao && mesh_mqtt_conectado(); }
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && !mesh_ble_conectado()) {
    return difusao && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();   // a internet cobrindo a falta dele
  }
  return true;
}

// DOIS SEGUNDOS ENTRE TEXTOS. O firmware descarta o texto que chega do
// cliente menos de 2 s depois do anterior (PhoneAPI, rate limit da porta TEXT,
// da 2.5 ate a atual): na 2.7 sem erro nenhum - a mensagem ficava "enviando" e
// virava "sem resposta". A fila espera o que falta; a internet nao tem esse
// limite (e o servidor, nao o no). Chamada pela tarefa do servico, sem trava.
static void laco_envios(void)
{
  static uint32_t ultimo_ms = 0;
  static envio_fila_t e;
  if (!s_fila_envio) { return; }
  for (int k = 0; k < 4; ++k) {
    if (xQueuePeek(s_fila_envio, &e, 0) != pdTRUE) { return; }
    if (!e.internet && ultimo_ms && tab5_now_ms() - ultimo_ms < 2100) { return; }
    xQueueReceive(s_fila_envio, &e, 0);
    const bool ok = e.internet ? mesh_mqtt_internet_enviar(e.d, e.n) : enlace_enviar(e.d, e.n);
    if (!e.internet) { ultimo_ms = tab5_now_ms(); }
    if (!ok) {
      // O enlace caiu entre o toque e a vez dela: falhou, e a tela diz.
      ESP_LOGW(TAG, "pacote %08lX: o enlace recusou", (unsigned long)e.id);
      travar();
      marcar_envio(e.id, MESH_ERRO_FILA, s_meu_num);
      destravar();
    }
  }
}

bool tab5_mesh_enviar(uint32_t para, uint8_t canal, const char* texto)
{
  if (!texto || !texto[0]) { return false; }

  // SEM SERVICO NAO HA PARA ONDE ESCREVER. A tela ja impede chegar aqui, mas a
  // guarda mora TAMBEM aqui de proposito: sem servico nao ha UART instalada, e
  // um uart_write_bytes numa porta que nao existe nao e "nao enviou" - e
  // comportamento indefinido. Quem protege o hardware e a camada do hardware.
  if (!s_servico_ativo) {
    ESP_LOGW(TAG, "envio recusado: servico parado (sem no Meshtastic)");
    return false;
  }
  // ENLACE MUDO NAO E MOTIVO PARA RECUSAR, e a recusa era pior que o problema.
  //
  // "no ar" aqui significa so "chegou algum frame do radio nos ultimos 30s".
  // Numa malha parada isso e o estado NORMAL: o radio nao fala sozinho, e nao
  // ha nada de errado com o cabo. Recusando, o botao ENV nao fazia nada, sem
  // mensagem de erro e sem entrada no historico - do lado de fora e
  // indistinguivel de um sistema quebrado, e foi assim que ficou o tempo todo.
  //
  // Mandar e o certo: se o radio nao estiver mesmo la, o queueStatus nao vem, o
  // ACK nao vem, e o prazo de 60s marca a mensagem como "sem resposta" na
  // propria tela. Isso e informacao; o silencio nao era.
  if (!tab5_mesh_no_ar()) {
    ESP_LOGW(TAG, "enlace sem quadro recente - enviando assim mesmo");
  }

  // No maximo MESH_PAYLOAD_MAX (233) bytes - acima disso o no descarta o
  // pacote inteiro em silencio (ver o .hpp). Corta na fronteira de caractere:
  // um acento partido ao meio sai como lixo na tela de quem recebe.
  size_t n_txt = strnlen(texto, MESH_PAYLOAD_MAX + 1);
  if (n_txt > MESH_PAYLOAD_MAX) {
    n_txt = MESH_PAYLOAD_MAX;
    while (n_txt > 0 && ((unsigned char)texto[n_txt] & 0xC0) == 0x80) { --n_txt; }
    ESP_LOGW(TAG, "texto acima de %d bytes - enviado cortado", MESH_PAYLOAD_MAX);
  }
  char enviado[MESH_PAYLOAD_MAX + 1];
  memcpy(enviado, texto, n_txt);
  enviado[n_txt] = 0;

  // Pela internet o pacote vai no canal 0 DE LA (o LongFast publico); o indice
  // MESH_CANAL_INTERNET e so a chave da conversa daqui. E la so ha canal (sem PKI).
  const bool internet = mesh_canal_internet(canal);
  if (internet && para != MESH_PB_DIFUSAO) { return false; }

  mesh_pb_envelope_t env = { };
  env.para    = para;
  env.channel   = internet ? 0 : canal;
  env.porta   = PORT_TEXTO;
  env.carga   = (const uint8_t*)texto;
  env.n_carga = n_txt;
  // ID PROPRIO, e e ele que torna a confirmacao possivel. Sem id o radio
  // inventa um, e nunca ficamos sabendo qual - o ACK chegaria com um
  // request_id que nao bate com nada nosso. Zero e reservado, por isso o |1.
  env.id = esp_random() | 1u;
  // hop_limit 3. Sem ele o pacote sai com ZERO SALTOS - so quem ouve a nossa
  // antena diretamente recebe, e nada e repetido adiante. 3 e o padrao do
  // proprio Meshtastic (nao lemos a Config do radio para herdar o dele).
  env.saltos = 3;
  // want_ack nos DOIS casos - tambem na difusao. La nao ha destinatario para
  // confirmar, mas ha a CONFIRMACAO IMPLICITA: quando um vizinho repete o
  // pacote, o firmware devolve um Routing com o nosso request_id. E a unica
  // prova de que alguem ouviu - o mesmo que o app oficial faz num canal.
  env.quer_ack = true;
  const uint32_t id = env.id;

  // O enquadramento (se houver) e do enlace.
  uint8_t quadro[MESH_TEXTO + 96];
  const size_t n_quadro = mesh_pb_to_radio(&env, quadro, sizeof(quadro));
  if (!n_quadro) { ESP_LOGE(TAG, "texto nao coube no quadro"); return false; }

  // A RECUSA QUE DA PARA SABER AGORA sai agora, com o envio falhando na
  // tela: a rede publica fora, a internet (so canal - a direta precisa de
  // PKI), o Bluetooth sem conexao. O resto e decidido pelo enlace, na fila.
  if (!pode_sair(internet, para)) {
    ESP_LOGW(TAG, "envio recusado: %s", internet ? tab5_mesh_rede_internet_estado() : tab5_mesh_enlace_descricao());
    return false;
  }

  // NA FILA, NAO NA HORA. Quem chama e a tarefa de INTERFACE, e o envio pode
  // ter de esperar o limite de 2 s do no entre textos (ver laco_envios) -
  // esperar aqui congelava a tela. A mensagem aparece na conversa ja como
  // "enviando"; a tarefa do servico a entrega no ritmo que o no aceita.
  if (!s_fila_envio) { return false; }
  static envio_fila_t e;   // so a interface envia texto; 300 bytes fora da pilha dela
  e.n = (uint16_t)n_quadro;
  e.internet = internet;
  e.id = id;
  memcpy(e.d, quadro, e.n);
  if (xQueueSend(s_fila_envio, &e, 0) != pdTRUE) {
    ESP_LOGW(TAG, "fila de envio cheia - mensagem nao saiu");
    return false;
  }

  // Guarda a nossa propria mensagem: a tela mostra as suas a direita, e sem
  // isto ela sumiria - o radio nao devolve o que voce mesmo enviou.
  travar();
  anotar_envio(id, para);
  // Sem remendo no contador: o guardar_msg ja sabe que a mensagem e nossa
  // (de == s_meu_num) e nao a conta como nao lida, nem aqui nem na conversa.
  // via_mqtt = false: e a NOSSA transmissao saindo pelo nosso proprio radio,
  // nunca chega pela ponte.
  // saltos = desconhecido: e a NOSSA mensagem, ainda nao viajou nenhum salto
  // do ponto de vista de quem recebe - e o meta-line nem aparece para
  // mensagem MINHA de qualquer forma (ver desenhar_msgs em app_mesh.cpp).
  // A HORA DO ENVIO, em UTC como a rx_time do radio. Era 0 - e
  // 0 no fuso -3 vira 31/12/1969 na tela. Relogio ainda nao acertado (sem GPS
  // nem malha desde o boot) continua 0 = "sem data", que a tela sabe tratar.
  const uint32_t quando = tab5_time_epoch();
  guardar_msg(s_meu_num, para, canal, enviado, quando, 0, 0, id, false, MESH_SALTOS_DESCONHECIDO);
  lembrar_carimbo(id, quando);   // corrigido se o relogio saltar (ao_saltar_relogio)
  destravar();

  ESP_LOGI(TAG, "enviado para %08lX (canal %u) id=%08lX: %s",
           (unsigned long)para, (unsigned)canal, (unsigned long)id, enviado);
  return true;
}

// ---------------------------------------------------------------------------
// POSICAO DO GPS PARA A MALHA
// ---------------------------------------------------------------------------
// Com o C6L no Bluetooth o Grove fica livre para o GPS. A posicao vai num
// MeshPacket, porta POSITION_APP (3), com um Position dentro:
//   1: latitude_i  (sfixed32, graus x 1e7)
//   2: longitude_i (sfixed32, graus x 1e7)
//   3: altitude    (int32, metros)
//   4: time        (fixed32, epoch UTC)
//
// PARA QUEM, e a diferenca e de tempo de ar.
//   - COM O NO do outro lado: SO PARA ELE (to = o numero dele), como faz o
//     app do celular. Ele passa a usa-la como a propria posicao e a difunde
//     no ritmo dele (Localizacao > "enviar a cada", "ao se mover"), com a
//     precisao do canal. Antes ia em DIFUSAO, 3 saltos: a malha recebia a
//     mesma posicao duas vezes - a do Tab5 e, logo depois, a do no repetindo
//     o que ja sabia, cada uma no ritmo de um.
//   - O no JA SABE onde esta (posicao fixa, ou o GPS dele ligado): nada a
//     mandar. A fixa ignoraria a nossa; o GPS dele a atropelaria.
//   - PELA INTERNET (sem no, ou o Bluetooth caiu e o MQTT cobre): o Tab5 e o
//     no, entao difunde ele mesmo - com a precisao do canal primario que o
//     no deixou configurada (0 = nao manda; menos de 32 = so a regiao).
// Sem fix, nao manda nada (devolve false e a tarefa tenta de novo em 30 s);
// true = resolvido ate a proxima volta (mandou, ou nao ha o que mandar).
static bool enviar_posicao(void)
{
  if (!s_servico_ativo || !tab5_gps_service_active()) { return false; }
  const bool com_no = radio_aceita_admin();
  const bool pela_net = !com_no && (s_via == SETTINGS_MESH_TRANSPORT_INTERNET || s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH)
                     && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();
  if (!com_no && !pela_net) { return false; }
  if (com_no && mesh_admin_no_tem_posicao_propria()) { return true; }
  const int32_t prec = mesh_admin_precisao_primario();
  if (pela_net && prec == 0) { return true; }

  const nmea_position_t g = tab5_gps_position();
  if (!g.valid || g.qualidade == 0) { return false; }
  int32_t lat = (int32_t)(g.lat * 1e7), lon = (int32_t)(g.lon * 1e7);
  if (pela_net && prec > 0 && prec < 32) {
    // O mesmo corte do firmware (PositionModule): os bits que sobram vao
    // para o meio da celula, nao para o canto.
    const uint32_t mascara = 0xFFFFFFFFu << (32 - prec);
    lat = (int32_t)(((uint32_t)lat & mascara) + (1u << (31 - prec)));
    lon = (int32_t)(((uint32_t)lon & mascara) + (1u << (31 - prec)));
  }

  uint8_t pos[32];
  pb_escritor_t w = pb_escritor(pos, sizeof(pos));
  mesh_pb_posicao(&w, lat, lon, (int32_t)g.altitude_m, tab5_time_epoch());

  // Para o no: sem saltos (nao sai dele). Difusao: 3 saltos (ver tab5_mesh_enviar).
  const mesh_pb_envelope_t env = { com_no ? s_meu_num : MESH_PB_DIFUSAO, -1, PORT_POSICAO, pos,
                                   pb_tamanho(&w), esp_random() | 1u, (uint8_t)(com_no ? 0 : 3), false };
  uint8_t quadro[96];
  const size_t n = pb_tamanho(&w) ? mesh_pb_to_radio(&env, quadro, sizeof(quadro)) : 0;
  const bool ok = n && enlace_enviar(quadro, n);
  if (ok) {
    ESP_LOGI(TAG, "posicao do GPS %s (%.5f, %.5f)", com_no ? "passada ao no" : "difundida pela internet",
             g.lat, g.lon);
  }
  return ok;
}

// Quem somos, para os clientes MQTT (no e internet): eles publicam e
// respondem o want_config em nome deste numero e destes nomes. Chamado antes
// de subir qualquer um dos dois.
static void informar_no_ao_mqtt(void)
{
  char curto[MESH_NOME_CURTO] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(s_meu_num, curto, sizeof(curto));
  tab5_mesh_nome_longo_cache(s_meu_num, longo, sizeof(longo));
  mesh_mqtt_definir_no(s_meu_num, curto, longo);
}

// AS GRAVACOES LENTAS DO MESH, NA CARONA DA TAREFA "cartao" (tab5_sd_carona).
// Eram feitas no laco do RADIO: enquanto a NodeDB ia para o cartao (montar,
// escrever dezenas de KB, fsync) ninguem lia a UART, e o buffer de 4 KB (~350
// ms a 115200) transbordava num cartao lento - quadro do no perdido. A regra
// do sistema (hal/tab5_sd.hpp) e esta: quem so esvazia dados no cartao pega
// carona na tarefa dele. Canais: a cada 15 s se mudaram (e logo depois do
// dump, que os marca). NodeDB: a cada 10 min se mudou.
static void mesh_na_carona(void)
{
  if (!s_servico_ativo) { return; }
  const uint32_t t = tab5_now_ms();
  static uint32_t proximos_canais = 0, proxima_nodedb = 600000;
  if (s_canais_sujos && (int32_t)(t - proximos_canais) >= 0) {
    proximos_canais = t + 15000;
    salvar_canais();
  }
  if ((int32_t)(t - proxima_nodedb) >= 0) {
    proxima_nodedb = t + 600000;
    if (s_nos_ver != s_nodedb_ver_salva) { salvar_nodedb(); }
  }
}

static void vTaskMesh(void*)
{
  static uint8_t quadro[1024];
  size_t usado = 0;
  int    estado = 0;          // 0=procura 0x94  1=viu 0x94  2=tamanho  3=corpo
  size_t falta = 0;
  uint8_t tam_hi = 0;
  uint32_t proxima_apresentacao = 0;
  uint32_t proximo_heartbeat = 15000;   // primeiro so depois do dump inicial

  // WATCHDOG: um deadlock aqui e SILENCIOSO e caro de um jeito especifico -
  // esta tarefa segura o mutex do mesh (travar()/destravar()) em varios
  // trechos, e travada DENTRO de um deles o mutex nunca seria devolvido. A
  // tela nao trava (ela so pediria o mutex e ficaria esperando, mas o loop de
  // interface segue vivo por fora), entao o sintoma seria "mensagens paradas
  // de chegar" sem nenhum log dizendo por que - dificil de distinguir de um
  // radio mudo de verdade. Ver o comentario igual em kernel/core.cpp para o
  // raciocinio completo.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "nao consegui inscrever a tarefa no watchdog");
  }

  for (;;)
  {
    // PONTO DE SAIDA. No topo do loop, fora de qualquer secao critica: aqui a
    // tarefa nao segura mutex nem arquivo, entao sumir daqui nao deixa nada
    // pela metade. Como o uart_read_bytes la embaixo espera no maximo 200ms, a
    // parada custa isso no pior caso.
    if (parada_pedida(&s_parada)) {
      // DESINSCREVE ANTES DE MORRER. O watchdog guarda o HANDLE desta tarefa;
      // deletando sem avisar ele antes, o handle fica pendurado la dentro e o
      // proximo ciclo de verificacao do watchdog encontraria uma tarefa que
      // nao existe mais - o tipo de "fantasma" que este sistema anda cacando
      // em outros lugares (Grove, apresentacao do radio). A ordem importa:
      // desinscrever so funciona enquanto a tarefa ainda existe para fazer a
      // chamada.
      esp_task_wdt_delete(NULL);
      parada_confirmar(&s_parada);
      return;   // a vaga do pool devolve o dono e volta a dormir (kernel/task_pool.h)
    }

    // PROVA DE VIDA. Chegar aqui de novo significa que a volta anterior
    // terminou - nenhum uart_read_bytes, trava de mutex ou I/O de cartao
    // ficou preso. 200ms e o maior bloqueio que este loop faz de proposito
    // (o prazo do uart_read_bytes la embaixo); bem dentro dos 5s do watchdog.
    esp_task_wdt_reset();

    const uint32_t t = tab5_now_ms();

    // Apresentacao SO quando o enlace esta mudo. Pedir de novo com o enlace
    // vivo faz o radio REINICIAR o download de configuracao inteiro - foi o que
    // produziu 11 node_info para 6 nos: o dump rodou duas vezes.
    // O radio avisou que reiniciou: pede a configuracao NA HORA. Sem isto a
    // gente esperava o enlace ficar mudo e o prazo de 10s vencer - e nesse
    // meio-tempo a lista de nos e os canais ficavam do reinicio anterior.
    mesh_admin_tique(t);
    if (s_reapresentar_ms && (int32_t)(t - s_reapresentar_ms) >= 0) {
      s_reapresentar_ms = 0;
      proxima_apresentacao = t + 10000;
      pedir_apresentacao();
    }
    else if (s_radio_reiniciou) {
      s_radio_reiniciou = false;
      proxima_apresentacao = t + 10000;
      pedir_apresentacao();
    }
    else if (!tab5_mesh_no_ar() && t > proxima_apresentacao) {
      // DOIS RITMOS, e a diferenca entre eles e o que resolve.
      //
      // Sem o my_info nao sabemos nem o nosso proprio numero: nada funciona, e
      // insistir de 10 em 10 segundos e o certo - e o primeiro contato.
      //
      // COM ele, ja conversamos com este radio. Um silencio agora quer dizer
      // "a malha esta quieta", nao "o radio sumiu" - e pedir a configuracao de
      // novo faz o C6L REINICIAR o download inteiro, jogando fora o trafego ao
      // vivo que estiver na queue dele. Era um dump a cada 30s, para sempre, na
      // maior parte do tempo. De dois em dois minutos ainda recupera um radio
      // que travou, sem transformar a conversa num ciclo de reapresentacoes.
      const uint32_t espera = (s_meu_num == 0) ? 10000 : 120000;
      proxima_apresentacao = t + espera;
      ESP_LOGI(TAG, "sem quadro ha %d s - pedindo a configuracao ao radio",
               SILENCIO_MS / 1000);
      pedir_apresentacao();
    }

    // Nomes de no pendentes, de 15 em 15 segundos. Espacado de proposito: e o
    // que transforma a rajada de montagens do boot numa unica escrita.
    static uint32_t proxima_descarga = 15000;
    if (t > proxima_descarga) {
      proxima_descarga = t + 15000;
      descarregar_nomes();   // so entrega ao gravador (tab5_sd_enqueue_append): nao bloqueia
    }
    // Canais (NVS) e NodeDB (cartao) NAO sao gravados aqui: ver mesh_na_carona.

    // Prazo dos envios sem resposta. Rede de seguranca: quando o radio desiste
    // por conta propria ele AVISA (Routing.MAX_RETRANSMIT), entao chegar aqui
    // significa que o enlace morreu no meio - e "nao sei" e o que a tela deve
    // dizer, nao "falhou".
    for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
      if (s_envios[i].state == MESH_ENVIANDO
       && (t - s_envios[i].em_ms) > MESH_ACK_MS) {
        s_envios[i].state = MESH_SEM_RESPOSTA;
        s_envios_ver++;
        sistema_avisar(AVISO_MESH_ENVIO);
        gravar_desfecho(s_envios[i].id, MESH_SEM_RESPOSTA, 0);
        ESP_LOGW(TAG, "pacote %08lX: sem confirmacao em %d s",
                 (unsigned long)s_envios[i].id, MESH_ACK_MS / 1000);
      }
    }

    laco_envios();   // texto na fila (ver tab5_mesh_enviar)

    // Batimento periodico. Mantem o radio convencido de que ha cliente do outro
    // lado; sem ele o trafego ao vivo pode simplesmente parar de vir.
    if (t > proximo_heartbeat) {
      proximo_heartbeat = t + 30000;
      mandar_heartbeat();
    }

    // LEITURA EM BLOCO, e a diferenca e de energia, nao de elegancia.
    //
    // Antes era uma chamada por BYTE, com prazo de 200ms. A 115200 baud isso
    // chega a 11.520 chamadas por segundo, cada uma com travessia de queue e
    // troca de contexto - e roda o tempo todo, porque isto e servico, nao app.
    //
    // Lendo em bloco, um pacote inteiro sai de UMA chamada. A tarefa acorda
    // poucas vezes por segundo em vez de milhares, e o core volta a ficar
    // ocioso entre elas - que e o que permite ao DFS trabalhar.
    // POSICAO DO GPS PARA A MALHA - ver enviar_posicao().
    static uint32_t proxima_posicao = 60000;
    if (settings_mesh_position_interval_min() && t > proxima_posicao) {
      proxima_posicao = t + (enviar_posicao() ? settings_mesh_position_interval_min() * 60000u : 30000u);
    }

    // A REDE PUBLICA PELA INTERNET, em paralelo a tudo o resto (ver tab5_mesh_mqtt.hpp):
    // sobe com o Wi-Fi e o ajuste ligado, desce sem um ou outro. Ate 8
    // pacotes por volta, lidos como da rede INTERNET (s_rede_rx) - nao contam como
    // quadro do no (s_ultimo_quadro_ms e a vida do RADIO).
    {
      const bool quer_internet = settings_mesh_public_network() && tab5_wifi_state() == TAB5_WIFI_CONNECTED;
      if (quer_internet && !mesh_mqtt_internet_ativo()) {
        informar_no_ao_mqtt();
        mesh_mqtt_internet_iniciar();
        travar();
        if (!esta_apagada(true, MESH_CANAL_INTERNET)) { achar_ou_criar_conversa(true, MESH_CANAL_INTERNET, 0); }
        destravar();
      } else if (!quer_internet && mesh_mqtt_internet_ativo()) {
        mesh_mqtt_internet_parar();
      }
      for (int k = 0; k < 32; ++k) {
        const size_t n = mesh_mqtt_internet_receber(quadro, sizeof(quadro));
        if (n == 0) { break; }
        travar();
        s_rede_rx = MESH_REDE_INTERNET;
        ler_from_radio(quadro, n);
        s_rede_rx = MESH_REDE_NO;
        destravar();
      }
    }

    // O CLIENTE MQTT, EM UM DE DOIS PAPEIS (ver services/tab5_mesh_mqtt.hpp):
    //   INTERNET - o Tab5 fala com a malha pelo servidor, quando o no nao esta
    //              (ou quando a via escolhida e a Internet);
    //   PROXY    - o no esta conectado e com o proxy ligado: o Tab5 so carrega
    //              o que ELE publica e assina (proxy ligado no no).
    // Os dois sao exclusivos por construcao: o proxy exige o no presente, a
    // internet como tapa-buraco exige a falta dele. Com folga nas duas
    // direcoes: o BLE cai e volta sozinho numa reconexao, e o servidor nao
    // deve ser aberto e fechado a cada piscada dele.
    {
      enum { MQ_NADA = 0, MQ_NET, MQ_PROXY };
      static int      modo_pedido = MQ_NADA;
      static uint32_t modo_desde = 0;
      // DOIS JEITOS DE A INTERNET ESTAR AQUI:
      //   - ela E a via (escolhida em Ajustes, ou o kernel subiu o servico so
      //     por ela): roda com o Wi-Fi, e ponto. O C6L no cabo nao a derruba
      //     daqui - quem decide trocar para o Grove e a sondagem do kernel, ao
      //     ACHAR o no (a prioridade do Grove), nao este laco a cada volta. Com
      //     o teste do Grove aqui, escolher "internet" com o C6L plugado
      //     derrubava o MQTT logo depois de subir.
      //   - a via e o no (cabo ou Bluetooth): a internet so cobre a falta
      //     dele, se o ajuste "internet quando sem o no" estiver ligado.
      const bool wifi = tab5_wifi_state() == TAB5_WIFI_CONNECTED;
      const bool no_ok = (s_via == SETTINGS_MESH_TRANSPORT_SERIAL)
                      || (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && mesh_ble_conectado())
                      || tab5_grove_type() == GROVE_MESHTASTIC;
      const bool quer_net = wifi && ((s_via == SETTINGS_MESH_TRANSPORT_INTERNET)
                                  || (settings_mesh_internet() && !no_ok));
      // O proxy pede o no CONVERSANDO (nao so a via escolhida) e com o dump
      // completo: e o dump que diz se o proxy esta ligado nele e quais canais
      // descem.
      const bool no_vivo = (s_via == SETTINGS_MESH_TRANSPORT_SERIAL && tab5_mesh_no_ar())
                        || (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && mesh_ble_conectado());
      // A CHAVE E DO NO: liga-se o proxy na pagina "No" do app Mesh, que grava
      // proxy_to_client_enabled nele. Ligado no no, o Tab5 empresta a internet.
      const bool quer_proxy = wifi && no_vivo && s_config_ok_ms != 0 && mesh_mqtt_no_tem_proxy();
      const int modo = quer_proxy ? MQ_PROXY : quer_net ? MQ_NET : MQ_NADA;
      int atual = !mesh_mqtt_ativo() ? MQ_NADA : mesh_mqtt_em_proxy() ? MQ_PROXY : MQ_NET;
      if (modo != modo_pedido) { modo_pedido = modo; modo_desde = t; }
      // Descer: o no que acabou de voltar tira a internet NA HORA (o proxy vai
      // subir); o resto espera 10 s firmes, para nao derrubar numa piscada.
      if (atual != MQ_NADA && atual != modo
       && (modo == MQ_PROXY || t - modo_desde >= 10000)) {
        mesh_mqtt_parar();
        atual = MQ_NADA;
      }
      // Subir: a internet como tapa-buraco espera 20 s (o BLE costuma voltar
      // antes disso); a via Internet e o proxy sobem logo.
      const uint32_t folga = (modo == MQ_NET && s_via != SETTINGS_MESH_TRANSPORT_INTERNET) ? 20000 : 0;
      if (atual == MQ_NADA && modo != MQ_NADA && t - modo_desde >= folga) {
        informar_no_ao_mqtt();
        if (modo == MQ_PROXY) { mesh_mqtt_iniciar_proxy(); } else { mesh_mqtt_iniciar(); }
        atual = !mesh_mqtt_ativo() ? MQ_NADA : modo;
      }
      if (atual == MQ_NET) {
        if (mesh_mqtt_conectou_agora() && s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { s_radio_reiniciou = true; }
        // ESVAZIA A FILA, nao um por volta: com o no no cabo a volta espera ate
        // 200 ms na serial, e um pacote por volta (5/s) perdia a rajada do
        // servidor. So o primeiro espera (na via Internet e o ritmo do laco).
        for (int k = 0; k < 16; ++k) {
          const size_t n = mesh_mqtt_receber(quadro, sizeof(quadro),
                                             (k == 0 && s_via == SETTINGS_MESH_TRANSPORT_INTERNET) ? 200 : 0);
          if (n == 0) { break; }
          travar();
          s_c_quadros++;
          ler_from_radio(quadro, n);
          s_ultimo_quadro_ms = tab5_now_ms();
          destravar();
        }
      } else if (atual == MQ_PROXY) {
        // DESCIDA DO PROXY: o que veio do servidor vai ao no. Ate 8 por volta -
        // a volta dura no maximo os 200 ms da leitura do enlace, e a fila (32)
        // segura a rajada no meio-tempo. Eram 4 (20/s), e um servidor publico
        // entregando o canal inteiro enchia a fila. Mais que 8 seguraria a
        // volta escrevendo na serial/Bluetooth.
        static uint8_t desce[QUADRO_PROXY_MAX];
        for (int k = 0; k < 8; ++k) {
          const size_t n = mesh_mqtt_proxy_receber(desce, sizeof(desce));
          if (n == 0) { break; }
          enlace_enviar(desce, n);
        }
      }
      if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) {
        if (!mesh_mqtt_ativo()) { vTaskDelay(pdMS_TO_TICKS(200)); }
        continue;
      }
    }

    // VIA BLUETOOTH: o enlace ja entrega FromRadio inteiro, sem frame - nao
    // ha maquina de enquadramento. Mesmo prazo de 200 ms da serial.
    if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
      if (mesh_ble_conectou_agora()) { s_radio_reiniciou = true; }   // pede a configuracao ja
      // O que ja chegou de uma vez (o dump vem em rajada); so o primeiro espera.
      for (int k = 0; k < 16; ++k) {
        const size_t n = mesh_ble_receber(quadro, sizeof(quadro), k == 0 ? 200 : 0);
        if (n == 0) { break; }
        travar();
        s_c_quadros++;
        ler_from_radio(quadro, n);
        s_ultimo_quadro_ms = tab5_now_ms();
        destravar();
      }
      continue;
    }

    // ACORDA NO PRIMEIRO BYTE, LE O RESTO NA HORA. uart_read_bytes so volta
    // com o tamanho pedido OU no prazo: pedindo 256, um quadro de 60 bytes
    // ficava esperando os 200 ms inteiros - cada mensagem chegava atrasada, e o
    // laco inteiro (fila de envio e os dois clientes de internet) andava nesse passo. Esperar
    // UM byte dorme igual quando a serial esta quieta e acorda quando ela fala.
    static uint8_t entrada[512];
    int lidos = uart_read_bytes(MESH_UART, entrada, 1, pdMS_TO_TICKS(200));
    if (lidos <= 0) { continue; }
    size_t pronto = 0;
    if (uart_get_buffered_data_len(MESH_UART, &pronto) == ESP_OK && pronto > 0) {
      if (pronto > sizeof(entrada) - 1) { pronto = sizeof(entrada) - 1; }
      const int mais = uart_read_bytes(MESH_UART, entrada + 1, pronto, 0);
      if (mais > 0) { lidos += mais; }
    }

    for (int k = 0; k < lidos; ++k) {
    const uint8_t b = entrada[k];

    switch (estado) {
      case 0: if (b == 0x94) { estado = 1; } break;
      case 1: estado = (b == 0xC3) ? 2 : (b == 0x94 ? 1 : 0); break;
      case 2: tam_hi = b; estado = 3; break;
      case 3: {
        falta = ((size_t)tam_hi << 8) | b;
        usado = 0;
        if (falta == 0) { estado = 0; }
        else if (falta > sizeof(quadro)) {
          // PULA O CORPO, nao volta a procurar cabecalho dentro dele.
          //
          // Voltando ao estado 0 a maquina passava a varrer os proprios bytes
          // do frame descartado atras de 0x94 0xC3 - e essa dupla aparece por
          // acaso em dado binario. Encontrando uma, ela lia um "tamanho" que e
          // lixo e engolia o frame BOM que vinha depois. Uma mensagem some, e
          // nada no log diz por que.
          ESP_LOGW(TAG, "quadro de %u bytes nao cabe - pulando", (unsigned)falta);
          estado = 5;
        }
        else { estado = 4; }
        break;
      }
      case 5:                      // engolindo um frame grande demais
        if (--falta == 0) { estado = 0; }
        break;
      case 4:
        quadro[usado++] = b;
        if (usado >= falta) {
          travar();
          s_c_quadros++;
          ler_from_radio(quadro, usado);
          s_ultimo_quadro_ms = tab5_now_ms();
          destravar();
          estado = 0;
        }
        break;
    }
    }   // for k
  }
}

// PARA O SERVICO E DEVOLVE O GROVE.
//
// Existe para o caso que o sistema simplesmente nao cobria: trocar o modulo do
// cabo com o aparelho ligado. Sem isto o servico ficava de pe conversando com
// um C6L que nao estava mais la, e o GPS recem-plugado nunca era procurado
// porque a porta tinha dono.
//
// Devolve false quando NAO conseguiu parar - e nesse caso nao limpa nada. Se a
// tarefa nao confirmou que saiu, ela ainda pode estar viva: apagar a UART e o
// mutex debaixo dela trocaria um servico teimoso por uma pane.
bool tab5_mesh_parar(void)
{
  if (!s_servico_ativo) { return true; }   // ja parado; pedir de novo nao e erro

  ESP_LOGI(TAG, "parando o servico...");
  // A tarefa espera no maximo 200 ms na UART (ou na fila do enlace): a saida
  // custa uma volta. 1 s e folga larga. Sem confirmacao, o pedido FICA DE PE
  // e nada e desmontado - a proxima chamada termina (ver parada_pedir).
  if (!parada_pedir(&s_parada, 1000)) {
    ESP_LOGE(TAG, "a tarefa nao confirmou em 1s - segue parando, nada foi desmontado");
    return false;
  }

  // Daqui para baixo a tarefa NAO EXISTE MAIS. So agora e seguro desmontar o
  // que ela usava.

  // TAREFA DE GRAVACAO PRIMEIRO - independente da UART/mutex acima (so
  // depende da propria queue), mas rodando ate aqui. 300ms de folga: uma
  // escrita costuma levar bem menos, e nao ha nada em andamento na tarefa
  // "mesh" que ainda vá enfileirar mais nada (ja confirmou saida). Qualquer
  // pedido ainda na queue quando ela sair e descartado - mesmo trade-off
  // aceito em toda queue deste sistema que enfileira E/S (ver o aviso grande
  // junto de MESH_FILA_GRAVACAO).

  // O que estava pendente vai agora: a tarefa que faria a descarga acabou de
  // morrer, e sem isto os nomes aprendidos nesta sessao se perderiam.
  descarregar_nomes();

  mesh_mqtt_parar();
  mesh_mqtt_internet_parar();   // a fila dele e lida so por esta tarefa, que acabou de sair
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    mesh_ble_parar();
  } else if (s_via == SETTINGS_MESH_TRANSPORT_SERIAL) {
    uart_driver_delete(MESH_UART);
    tab5_grove_release(GROVE_OWNER_MESH);
  }

  // O AVISO SO SAI SE NAO HA NADA PARA LER. Antes ele era apagado a cada
  // parada - e o Mesh para sozinho (troca de via, Wi-Fi caindo): o aviso
  // sumia sem o usuario ter lido nada. As nao lidas ficam guardadas e voltam
  // com as conversas; o que esta publicado agora continua valendo.
  guardar_nao_lidas();
  if (s_notif >= 0 && !ha_nao_lidas_guardadas()) { notif_limpar(s_notif); }
  salvar_canais();   // o que a carona ainda nao levou (ela para com o servico)
  salvar_nodedb();   // antes do memset da tabela, logo abaixo
  // O MUTEX FICA. Ele era apagado aqui e recriado no proximo iniciar - mas
  // as consultas publicas (a barra da home, o app Mesh) o tomam de OUTRAS
  // tarefas a qualquer momento: apagar um mutex preso ou com alguem
  // esperando nele e memoria liberada em uso, e entre o apagar e o recriar
  // as leituras corriam sem exclusao nenhuma. Criado uma vez, vale para
  // sempre (o mesmo de tab5_sd).

  // Enlace zerado: sem isto o tab5_mesh_no_ar() continuaria respondendo pelo
  // ultimo frame recebido, e a barra diria "conectado" a um radio ausente.
  s_ultimo_quadro_ms = 0;
  s_config_ok_ms = 0;
  // s_meu_num NAO e zerado: nosso numero nao muda porque o cabo saiu, e o
  // arquivo precisa dele para saber de quem e cada mensagem.
  s_n_msgs = 0;   // a contagem de NOS nao tem variavel: sai do memset abaixo
  s_total_msgs = 0;
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  // Sob a trava: a interface pode estar lendo as tabelas agora mesmo (a
  // barra da home, o app Mesh) - antes isto rodava com o mutex ja apagado.
  travar();
  memset(s_nos,  0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  ++s_conv_ver;
  destravar();

  s_servico_ativo = false;
  sistema_avisar(AVISO_MESH_NOS);   // a barra de escrita passa a "so consulta"
  ESP_LOGI(TAG, "servico parado%s", s_via == SETTINGS_MESH_TRANSPORT_SERIAL ? ", Grove livre" : "");
  return true;
}

bool tab5_mesh_soltar_grove(void)
{
  // Via Bluetooth o servico nao esta no Grove: trocar o modulo do cabo nao e
  // motivo para derrubar a conversa com o no.
  if (s_servico_ativo && s_via != SETTINGS_MESH_TRANSPORT_SERIAL) { return true; }
  return tab5_mesh_parar();
}

uint8_t tab5_mesh_via(void) { return s_via; }

const char* tab5_mesh_enlace_descricao(void)
{
  if (!s_servico_ativo) { return "parado"; }
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { return mesh_mqtt_descricao(); }
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    return (!mesh_ble_conectado() && mesh_mqtt_ativo()) ? mesh_mqtt_descricao() : mesh_ble_descricao();
  }
  return tab5_mesh_no_ar() ? "cabo Grove, conversando" : "cabo Grove, esperando o no";
}

static void iniciar_via(uint8_t via);

void tab5_mesh_iniciar(void)
{
  // SOBE NA VIA ESCOLHIDA. Se o servico esta de pe por OUTRA via (internet
  // cobrindo a falta do no, ou o Bluetooth quando o Grove acabou de ganhar a
  // prioridade), para antes: sem isto o iniciar_via via "ja esta de pe" e nao
  // fazia nada, e o Mesh seguia na via velha.
  const uint8_t via = settings_mesh_transport();
  if (s_servico_ativo && s_via != via && !parada_em_curso(&s_parada)) { tab5_mesh_parar(); }
  iniciar_via(via);
}

const char* tab5_mesh_rede_internet_estado(void)
{
  static char t[72];
  if (!settings_mesh_public_network())                          { return "desligada"; }
  if (tab5_wifi_state() != TAB5_WIFI_CONNECTED)    { return "esperando o Wi-Fi"; }
  if (!s_servico_ativo)                             { return "serviço Mesh parado"; }
  if (!mesh_mqtt_internet_conectado())                   { return "conectando a mqtt.meshtastic.org"; }
  snprintf(t, sizeof(t), "conectada · %lu pacotes", (unsigned long)mesh_mqtt_internet_recebidos());
  return t;
}

const char* tab5_mesh_internet_estado(void)
{
  if (settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_INTERNET) {
    if (tab5_wifi_state() != TAB5_WIFI_CONNECTED) { return "Internet escolhida: esperando o Wi-Fi conectar"; }
    if (!mesh_mqtt_ativo())                        { return "Internet escolhida: vai conectar em instantes"; }
    return mesh_mqtt_descricao();
  }
  if (!settings_mesh_internet())                   { return "Desligada"; }
  if (tab5_grove_type() == GROVE_MESHTASTIC
   || (s_servico_ativo && s_via == SETTINGS_MESH_TRANSPORT_SERIAL)) { return "Em espera: o nó no Grove tem prioridade"; }
  if (tab5_wifi_state() != TAB5_WIFI_CONNECTED)  { return "Esperando o Wi-Fi conectar"; }
  if (!mesh_mqtt_ativo())                         { return "Ligada, vai conectar em instantes"; }
  return mesh_mqtt_descricao();
}

const char* tab5_mesh_proxy_estado(void)
{
  // Curto: vai ao lado de "ligado" numa linha da pagina "No".
  static char d[48];
  if (tab5_wifi_state() != TAB5_WIFI_CONNECTED) { return "sem Wi-Fi no Tab5"; }
  if (!mesh_mqtt_no_tem_proxy())                 { return "o nó ainda não confirmou"; }
  if (!mesh_mqtt_em_proxy())                     { return "vai conectar"; }
  if (!mesh_mqtt_conectado())                    { return "conectando ao servidor"; }
  uint32_t sobe, desce, perde;
  mesh_mqtt_proxy_contadores(&sobe, &desce, &perde);
  snprintf(d, sizeof(d), "conectado (%lu/%lu)", (unsigned long)sobe, (unsigned long)desce);
  return d;
}

void tab5_mesh_iniciar_internet(void)
{
  if (s_servico_ativo) { return; }
  iniciar_via(SETTINGS_MESH_TRANSPORT_INTERNET);
}

static void iniciar_via(uint8_t via)
{
  // IDEMPOTENTE. Sem esta guarda, chamar duas vezes criaria um segundo mutex
  // (vazando o primeiro, com quem as tarefas ainda estariam sincronizando) e
  // uma segunda tarefa disputando a mesma UART. Nunca acontecia porque o unico
  // chamador era o boot; agora existe o caminho de re-sondagem.
  if (s_servico_ativo) {
    // Uma parada em andamento nao pode ser atropelada por um inicio: a tarefa
    // antiga ainda esta viva e a UART ainda e dela.
    if (parada_em_curso(&s_parada)) {
      ESP_LOGW(TAG, "ainda parando o servico anterior - nao vou subir por cima");
    } else {
      ESP_LOGI(TAG, "servico ja esta de pe - nada a fazer");
    }
    return;
  }

  tab5_mutex_criar(&s_trava);   // uma vez por boot - ver tab5_mesh_parar
  tab5_sd_piggyback(mesh_na_carona);   // idempotente: a carona e registrada uma vez
  tab5_mutex_criar(&s_tx);
  if (!s_fila_envio) { s_fila_envio = xQueueCreateWithCaps(MESH_FILA_ENVIO, sizeof(envio_fila_t), MALLOC_CAP_SPIRAM); }
  if (s_fila_envio) { xQueueReset(s_fila_envio); }
  tab5_time_on_jump(ao_saltar_relogio);   // idempotente


  s_notif = notif_registrar("Meshtastic");
  notif_associar_app(s_notif, "Mesh");   // tocar o aviso abre o app Mesh
  // A preferencia (ajustes) e quem manda desde o INSTANTE em que a fonte
  // nasce - este registro acontece de novo a cada sondagem que reencontra o
  // modulo (troca de cabo), entao sem isto o silencio "esqueceria" a cada
  // hot-swap.
  if (s_notif >= 0) { notif_silenciar(s_notif, !settings_mesh_beep()); }
  memset(s_nos, 0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  // O ANEL DE IDS VISTOS NAO E ZERADO: trocar de via (Grove -> internet) ou
  // reconectar o cabo nao muda quais mensagens ja estao no cartao. Zerado, a
  // mesma mensagem ouvida pelo radio e depois pela ponte MQTT entrava duas
  // vezes no historico.
  memset(&s_saude, 0, sizeof(s_saude));   // a saude e do no desta via, nao da anterior

  ++s_conv_ver;   // a coluna recomeca

  // OS CANAIS EXISTEM DESDE O INICIO, sem esperar mensagem: os ativos da
  // ultima configuracao conhecida (NVS), ou o primario sem nenhuma. Numa malha
  // quieta, sem isto, nao havia para onde escrever. Os apagados ficam de fora
  // - e eram recriados aqui, o canal 0 sempre, mesmo apagado.
  criar_conversas_dos_canais();

  // Os nos que o Tab5 ja conhecia (alem do teto do radio).
  carregar_nodedb();
  mesh_diario_carregar();   // reinicios do no ja anotados (cartao)
  mesh_wifi_no_carregar();  // redes do Wi-Fi do no ja usadas (cartao)

  // FAVORITOS NA TABELA DESDE O INICIO, mesmo sem ter falado: estao fixos.
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { achar_no(s_favs[i]); }

  // RESTAURA O QUE O memset() ACIMA ACABOU DE APAGAR - defeito observado em
  // abrir o app ANTES de plugar o C6L jah carrega tudo do cartao
  // (ao_abrir() chama tab5_mesh_carregar_do_cartao()), mas a sondagem do Grove
  // que chega depois, achando o modulo, zera s_conv[] alguns milissegundos
  // apos - a lista fica so com o canal 0 (linha acima) ate o usuario sair e
  // voltar ao app, o que forca um novo ao_abrir(). Chamar de novo aqui, logo
  // apos o memset, fecha a mesma lacuna sem depender do usuario reabrir nada.
  // Idempotente (achar_ou_criar_conversa acha o que ja existe em vez de
  // duplicar) e seguro nesta tarefa: tab5_mesh_iniciar() so roda dentro de
  // vTaskSondagemGrove (kernel/core.cpp), a tarefa de prioridade mais baixa do
  // sistema, feita para aguentar E/S lenta (aqui, montar o cartao) sem travar
  // o toque.
  tab5_mesh_carregar_do_cartao();

  // As conversas renasceram com as nao lidas guardadas (guardar_nao_lidas):
  // a barra passa a contar a partir delas, e nao do que sobrou publicado.
  travar();
  publicar_notif();
  destravar();

  s_via = via;
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) {
    // Sem cabo e sem radio: o laco do servico abre o MQTT (usar_internet).
  } else if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    // Sem Grove e sem UART: o enlace BLE procura o no, conecta e entrega o
    // que chegar. O Bluetooth ja foi ligado por quem chamou.
    if (!mesh_ble_iniciar()) { ESP_LOGE(TAG, "enlace Bluetooth nao subiu"); return; }
  } else {
  uart_config_t cfg = { };
  cfg.baud_rate = MESH_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (!tab5_grove_acquire(GROVE_OWNER_MESH)) {
    ESP_LOGE(TAG, "Grove ocupado - servico nao vai subir");
    return;
  }

  esp_log_level_set("uart", ESP_LOG_ERROR);   // reserva antiga do Ex_I2C, ver tab5_grove
  if (uart_driver_install(MESH_UART, 4096, 512, 0, NULL, 0) != ESP_OK
   || uart_param_config(MESH_UART, &cfg) != ESP_OK
   || uart_set_pin(MESH_UART, MESH_TX, MESH_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    esp_log_level_set("uart", ESP_LOG_WARN);
    ESP_LOGE(TAG, "nao consegui abrir a UART do Grove");
    return;
  }
  esp_log_level_set("uart", ESP_LOG_WARN);
  }   // via serial

  // Prioridade 3, igual ao WiFi: abaixo da interface (4), para nunca engasgar o
  // toque; acima do registro (1), porque perder byte da serial e irreversivel.
  // ATIVO ANTES DE A TAREFA EXISTIR: com prioridade 3 ela comeca na hora,
  // antes de quem chamou (a sondagem, prioridade 1) voltar - e ja envia.
  s_iniciado_em_ms = tab5_now_ms();   // ver o aviso em tab5_mesh_silencio_ms()
  s_servico_ativo  = true;
  // 6 KB: com 4 KB o monitor registrou no minimo 264 bytes livres em uso real.
  if (!parada_preparar(&s_parada)
   || !pool_criar(nullptr, vTaskMesh, nullptr, "mesh", 6144, 3, OWNER_SERVICE, "Mesh")) {
    // DEVOLVE O QUE O INICIAR TOMOU. Antes so a bandeira voltava: a UART e o
    // Grove ficavam com o Mesh para sempre, e nenhuma sondagem achava mais
    // nada no cabo ate reiniciar.
    parada_desistir(&s_parada);
    s_servico_ativo = false;
    if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { mesh_ble_parar(); }
    else if (s_via == SETTINGS_MESH_TRANSPORT_SERIAL) { uart_driver_delete(MESH_UART); tab5_grove_release(GROVE_OWNER_MESH); }
    ESP_LOGE(TAG, "sem memoria para a tarefa do Mesh");
    return;
  }
  sistema_avisar(AVISO_MESH_NOS);
  tab5_mem_marco("Mesh (servico)");
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { ESP_LOGI(TAG, "servico no ar (so internet, sem o no)"); }
  else if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { ESP_LOGI(TAG, "servico no ar (Bluetooth)"); }
  else { ESP_LOGI(TAG, "servico no ar (Grove %d baud, RX=%d TX=%d)", MESH_BAUD, MESH_RX, MESH_TX); }
}
