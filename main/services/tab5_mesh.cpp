#include "app.h"   // sistema_avisar
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes in PSRAM
#include "tab5_mesh.hpp"

#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "app_task.h"
#include "service_stop.hpp"      // parada cooperativa of tarefa (kernel)
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

// NVS of proprio servico: o que o radio contou (nosso numero, canais,
// favoritos), separado das preferences of pessoa (ajustes).
#define MESH_NVS "mesh"

#define MESH_UART   UART_NUM_1
#define MESH_RX     54
#define MESH_TX     53
#define MESH_BAUD   115200

// Sem frame por este time, consideramos o enlace caido.
// QUANTO SILENCIO ANTES DE DAR O ENLACE POR MUDO.
//
// Era 30s, e 30s e MENOS of que uma malha parada leva para falar. O radio nao
// diz nada por conta propria: telemetria dele exit a cada meia hora, e se
// ninguem transmite nada nao chega frame nenhum. Ou seja, numa malha quieta o
// enlace passava a maior parte of time marcado como "mudo" - com o C6L
// perfeitamente conectado. E "mudo" tinha consequencia: disparava one request of
// configuracao a cada 10s (ver a tarefa), e cada request REINICIA o download
// inteiro no radio. Ficavamos num ciclo of request configuracao para sempre, e
// enquanto o dump roda o trafego on vivo espera in queue of radio - que e
// pequena e descarta o excedente. Mensagem que chega nessa window some.
//
// 90s: ainda percebe one cabo arrancado em pouco mais of one minuto, e ja nao
// confunde "a malha esta quieta" com "o radio sumiu".
#define SILENCIO_MS 90000

// ---------------------------------------------------------------------------
// Protobuf: services/mesh_pb.hpp (o mesmo codec of ponte MQTT e of admin)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_trava = nullptr;

// VIDA DO SERVICO. Ate aqui ele so sabia nascer: uma vez of pe, segurava a UART
// of Grove para sempre. Trocar o C6L pelo GPS no cabo nao adiantava nada - o
// modulo novo estava la, mudo, com a porta in mao of quem ja tinha ido embora.
//
// Parada COOPERATIVA (kernel/service_stop.hpp), e nao vTaskDelete of fora: a
// tarefa pega mutex e escreve no cartao, e mata-la no middle disso deixaria o
// mutex preso e o arquivo open. Quem pede marca o request; a tarefa termina
// o que estava fazendo, confirma, e so entao a limpeza acontece.
static stop_t      s_parada = { };
static bool          s_servico_active = false;  // ha tarefa e enlace of pe
// POR ONDE: o cabo Grove ou o Bluetooth. Fixado no start, pela
// preferencia (settings_mesh_transport) - so muda stopping e subindo of novo.
static uint8_t       s_via = SETTINGS_MESH_TRANSPORT_SERIAL;
EXT_RAM_BSS_ATTR static mesh_no_t   s_nos[MESH_MAX_NOS];
static uint32_t    s_despejos = 0;   // quantas vezes achar_no() despejou o mais antigo since o boot
EXT_RAM_BSS_ATTR static mesh_msg_t  s_msgs[MESH_MAX_MSGS];
static int         s_n_msgs = 0;
// TOTAL DE VERDADE, NUNCA SATURA. Sintoma: message nova so aparecia depois
// of switch of screen e voltar. app_mesh.cpp detectava
// "chegou message nova" comparando tab5_mesh_num_msgs() (que devolve
// s_n_msgs) contra o valor of ultima pass - mas s_n_msgs e o tamanho of
// ANEL of 8 posicoes (MESH_MAX_MSGS), que para of crescer assim que 8
// mensagens (of QUALQUER conversa) ja passaram por ele. Depois disso o
// numero stays parado para sempre, e o comparador nunca mais via diferenca -
// so switch of screen forcava outros caminhos (s_hist_pronto=false direto) a
// reload. Este contador so cresce, nunca reseta a MESH_MAX_MSGS.
static uint32_t     s_total_msgs = 0;
static int         s_nao_lidas = 0;
static int         s_nao_lidas_dir = 0;
static mesh_saude_t s_saude = { };
static uint32_t    s_meu_num = 0;
static char        s_firmware[24] = { 0 };
static uint32_t    s_ultimo_frame_ms = 0;
static uint32_t    s_iniciado_em_ms   = 0;   // ver o aviso em tab5_mesh_silencio_ms()
static int         s_notif = -1;   // fonte no servico of notification

// DE QUAL REDE E O QUADRO QUE ESTA SENDO LIDO AGORA. So a tarefa of servico
// le quadros (sempre com a trava), e o of fila of internet e lido com isto em
// MESH_REDE_INTERNET: achar_no marca a rede of no, guardar_msg poe a message in
// conversa "LongFast Internet". Mais simples que levar a rede por cada funcao of
// caminho of leitura, que e o mesmo para as duas.
static uint8_t     s_rede_rx = MESH_REDE_NO;

// NOMES REAIS DOS CANAIS, ditos pelo proprio radio.
//
// Antes o canal 0 era "LongFast" cravado no code e o resto virava "Canal 1",
// "Canal 2". Mas o Meshtastic MANDA a configuracao dos canais no dump inicial
// (FromRadio field 10) - com o name que voce escolheu no celular. Ignorar isso
// era inventar one rotulo when o certo estava chegando pela serial.
static char s_canal_name[MESH_MAX_CANAIS][MESH_NOME_CONVERSA];

// SE O CANAL ESTA ATIVO AGORA, pela ULTIMA config que o radio mandou - nasceu
// junto of "canais disponiveis" (ver mais abaixo). Antes disto
// nada guardava esse estado: s_canal_nome[] so era ESCRITO when o papel
// vinha != 0 (ver o parser of field 10 abaixo), mas nunca ficava sabendo que
// one canal tinha sido DESLIGADO - o name antigo so ficava parado la,
// indistinguivel of "ainda active". Comeca everything false: so vira true when a
// PRIMEIRA config chegar, e nenhum canal deveria ser oferecido como
// "disponivel" antes disso (nao sabemos nada sobre ele ainda).
static bool s_canal_active[MESH_MAX_CANAIS];
static bool s_canais_conhecidos = false;   // ha configuracao (of radio ou of NVS)
static bool s_canais_lidos      = false;   // a NVS ja foi consultada
static bool s_canais_sujos      = false;   // changed e ainda nao foi para a NVS
static void recordar_canais(void);

// O radio avisou que reiniciou (FromRadio field 8). Sem isto, a gente so
// percebia pelo silencio - e esperava o prazo inteiro of 10s para so entao
// request a configuracao of novo.
static volatile bool s_radio_reiniciou = false;
static mesh_conversa_t s_conv[MESH_MAX_CONVERSAS];

// ORDEM DA COLUNA (ver tab5_mesh_conversas_versao no .hpp). 's_conv_seq' e a
// ultima atividade of conversa NESTA sessao, num contador que so cresce -
// nao o rx_time, que vale 0 when o clock ainda nao foi acertado e
// empataria everything. Conversa so lida of cartao stays com 0: vai para o end, in
// order of pasta.
static uint32_t s_conv_seq[MESH_MAX_CONVERSAS];
static uint32_t s_seq = 0;
static uint32_t s_conv_ver = 0;
static int      visivel_of(int interno);

// NAO LIDAS QUE SOBREVIVEM AO SERVICO. Parar o servico (troca of via, Wi-Fi
// caindo no modo internet, Bluetooth caindo, C6L achado no Grove) zera
// s_conv[] - e com ele as nao lidas. Visto on vivo: o aviso of barra SUMIA
// sem ninguem ter open o Mesh, so porque o servico subiu of novo por outra
// via. Aqui stays a contagem of cada conversa ate ela ser LIDA of verdade;
// achar_ou_criar_conversa() devolve o numero when a conversa renasce.
typedef struct { bool usada; bool channel; uint8_t idx; uint32_t no; uint16_t n; } nao_lida_guardada_t;
static nao_lida_guardada_t s_guardadas[MESH_MAX_CONVERSAS];

static nao_lida_guardada_t* guardada_of(bool canal, uint8_t idx, uint32_t no, bool create)
{
  nao_lida_guardada_t* livre = nullptr;
  for (auto& g : s_guardadas) {
    if (!g.usada) { if (!livre) { livre = &g; } continue; }
    if (canal && g.channel && g.idx == idx) { return &g; }
    if (!canal && !g.channel && g.no == no) { return &g; }
  }
  if (!create || !livre) { return nullptr; }
  *livre = { true, canal, idx, no, 0 };
  return livre;
}

// Com a trava tomada (ou com o servico parado). Fotografa as conversas vivas.
static void guardar_nao_lidas(void)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    const mesh_conversa_t& c = s_conv[i];
    if (!c.usada) { continue; }
    nao_lida_guardada_t* g = guardada_of(c.channel, c.idx, c.no, c.nao_lidas > 0);
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

// Contadores of diagnostics. Existem porque "nos 0/0" nao diz se o LocalStats
// nao CHEGA ou se chega e o filtro descarta - e sao perguntas com conserto oposto.
static uint32_t s_c_quadros = 0, s_c_pacotes = 0, s_c_telemetria = 0;
static uint32_t s_c_localstats = 0, s_c_text = 0, s_c_nodeinfo = 0;
static int      s_ultima_variante = -1;   // ultimo field visto dentro of Telemetry

// Quantas vezes one Routing chegou DEPOIS of pacote ja estar ENTREGUE (ver
// marcar_envio). Nao muda o estado mostrado - "confirmado" continua valendo -
// mas e one numero com finalidade propria: se ele cresce rapido, quer dizer
// que a confirmacao "alguem ouviu" esta vindo facil e a confirmacao forte of
// radio (end a end) esta vindo dificil - ou seja, a malha ESTA funcionando,
// so que no limit of alcance. E one termometro of qualidade of rota que nao
// existiria se o segundo Routing fosse so descartado sem count.
static uint32_t s_c_routing_extra = 0;


// ---------------------------------------------------------------------------
// OS ENVIOS EM VOO
// ---------------------------------------------------------------------------
// Anel dos ultimos pacotes que NOS mandamos, com o estado of cada one. Fica so
// em RAM of proposito: o cartao guarda o id junto of message, mas nao o
// estado - reabrir o app amanha e ver "enviando" num pacote of ontem seria
// mentira. Id que nao esta mais aqui devolve MESH_ENVIO_NA, e a screen nao
// desenha marca none.
#define MESH_MAX_ENVIOS 16

// Quanto wait pela confirmacao antes of abandon. O radio retransmite
// sozinho algumas vezes e, when desiste, ELE avisa com Routing.MAX_RETRANSMIT
// - este prazo e so a rede of seguranca para when o enlace morre no middle.
#define MESH_ACK_MS 60000

typedef struct {
  uint32_t id;
  uint32_t para;     // destino: decide o que one ACK quer dizer (ver marcar_envio)
  uint32_t em_ms;
  uint8_t  state;
  uint8_t  erro;
} send_state_t;

static send_state_t  s_envios[MESH_MAX_ENVIOS];
static int      s_prox_send_state = 0;
static uint32_t s_envios_ver = 0;

static void anotar_send_state(uint32_t id, uint32_t para)
{
  if (id == 0) { return; }

  // Ja acompanhado? Nao duplica. Importa porque now ha DOIS caminhos que
  // chamam isto - o nosso send_state e o eco of uma message que exited of celular -,
  // e a mesma message pode passar pelos dois.
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id == id) { return; }
  }

  send_state_t* e = &s_envios[s_prox_send_state];
  s_prox_send_state = (s_prox_send_state + 1) % MESH_MAX_ENVIOS;
  e->id     = id;
  e->para   = para;
  e->em_ms  = tab5_now_ms();
  e->state = MESH_ENVIANDO;
  e->erro   = 0;
  s_envios_ver++;
  system_notify(AVISO_MESH_ENVIO);
}

// ---------------------------------------------------------------------------
// O DESFECHO VAI PARA O CARTAO. O caminho, nao.
// ---------------------------------------------------------------------------
// A primeira versao guardava o estado so em RAM, com o argumento of que
// "reabrir amanha e ver ENVIANDO num pacote of ontem seria mentira". O
// argumento vale - para ENVIANDO. Nao vale para o resto.
//
// "Falhou por falta of rota" e one FATO FINAL: ele nao stays mais nem menos
// verdadeiro com o time, e some of screen so porque o anel of 16 envios girou ou
// o aparelho reiniciou. Quem mandou a message quer saber que ela nao chegou,
// hoje e semana que vem.
//
// Entao a divisao e por NATUREZA of estado, nao por comodidade:
//   ENVIANDO      - passageiro, so RAM. Nunca gravado.
//   ENTREGUE      - final, gravado.
//   FALHOU        - final, gravado, com o reason.
//   SEM_RESPOSTA  - final ("desisti of wait"), gravado.
//
// Arquivo PROPRIO e nao a line of message: a message e gravada no instante
// of send_state, when o desfecho ainda nao existe. Reescrever aquela line depois
// significaria reescrever o arquivo inteiro a cada ACK. Aqui e so acrescentar.
#define MESH_ARQ_ENVIOS "envios.txt"

static void gravar_desfecho(uint32_t id, uint8_t estado, uint8_t erro)
{
  if (id == 0) { return; }
  char line[40];
  snprintf(line, sizeof(line), "E,%08lX,%u,%u\n", (unsigned long)id, (unsigned)estado, (unsigned)erro);
  tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_ENVIOS, line);   // gravador of sistema (hal/tab5_sd.hpp)
}

// ---------------------------------------------------------------------------
// CARIMBO CORRIGIDO - when o clock salta (ver tab5_time.hpp)
// ---------------------------------------------------------------------------
// A message enviada leva a hora of NOSSO clock. Se ele estava errado e uma
// fonte o acerta depois, a message ficava no dia errado para sempre: a line
// dela ja esta no cartao. Reescrever o arquivo of conversa por isso seria
// caro; em vez disso vai uma line "H,<id>,<when>" para o envios.txt, o
// mesmo arquivo so of acrescentar dos desfechos, e o leitor of historico
// troca o carimbo of quem tem id la (ultima line vence, como no "E").
//
// So as mensagens DESTA sessao: as of antes foram carimbadas por outro
// clock (outro boot), que pode ter estado certo.
#define MESH_CARIMBADAS 16
typedef struct { uint32_t id; uint32_t when; } carimbada_t;
static carimbada_t s_carimbadas[MESH_CARIMBADAS];
static int         s_prox_carimbada = 0;

static void lock(void);
static void destravar(void);

// Com a trava tomada.
static void lembrar_carimbo(uint32_t id, uint32_t when)
{
  if (id == 0 || when == 0) { return; }
  s_carimbadas[s_prox_carimbada].id     = id;
  s_carimbadas[s_prox_carimbada].when = when;
  s_prox_carimbada = (s_prox_carimbada + 1) % MESH_CARIMBADAS;
}

// Tarefa of interface, logo depois of acerto of clock. So RAM e o gravador
// of sistema (tab5_sd_enqueue_append copia e iteration): nada of cartao aqui.
static void on_saltar_clock(int32_t delta)
{
  int n = 0;
  lock();
  for (int i = 0; i < MESH_CARIMBADAS; ++i) {
    carimbada_t* c = &s_carimbadas[i];
    if (c->id == 0) { continue; }
    c->when = (uint32_t)((int64_t)c->when + delta);
    for (int k = 0; k < s_n_msgs; ++k) {
      if (s_msgs[k].id == c->id) { s_msgs[k].when = c->when; }
    }
    char line[40];
    snprintf(line, sizeof(line), "H,%08lX,%lu\n", (unsigned long)c->id, (unsigned long)c->when);
    tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_ENVIOS, line);
    ++n;
  }
  // O app rele o historico when este contador muda: e o que tira of screen
  // o bubble no dia errado.
  if (n) { ++s_total_msgs; }
  destravar();
  if (n) {
    ESP_LOGI(TAG, "hora do aparelho corrigida em %+ld s: horario de %d mensagem(ns) enviada(s) "
                  "antes do acerto foi ajustado", (long)delta, n);
    system_notify(AVISO_MESH_MSG);
  }
}

// Desfechos lidos of cartao, UMA VEZ por carga of historico.
//
// A primeira versao chamava uma funcao "procura este id no arquivo" of DENTRO
// of loop que le as mensagens. Com 40 mensagens in conversa isso virava 40
// aberturas e 40 varreduras complete of envios.txt, no cartao, com dois
// arquivos abertos on mesmo time e o user esperando a conversa aparecer.
// Custo O(mensagens x desfechos) para responder uma pergunta que cabe em 1KB
// of RAM.
//
// Agora o arquivo e lido uma vez para esta tabela e a consulta e em memoria.
#define MESH_DESFECHOS 64

typedef struct {
  uint32_t id;
  uint8_t  state;
  uint8_t  erro;
  uint32_t when;   // carimbo corrigido ("H"); 0 = o of line of message vale
} desfecho_t;

// ULTIMO VENCE. O arquivo so cresce, entao a line mais recente of one id e a
// verdade current - one pacote pode ir of SEM_RESPOSTA para ENTREGUE when one ACK
// chega atrasado. Por isso sobrescrevemos a entrada em vez of stop in primeira.
//
// Tabela cheia: descarta a mais ANTIGA (anel). O arquivo e cronologico, entao o
// que sobra sao os 64 desfechos mais recentes - e o historico so mostra as
// ultimas dezenas of mensagens of qualquer jeito.
static int carregar_desfechos(desfecho_t* tab, int max)
{
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_ENVIOS, "r");
  if (!f) { return 0; }

  int n = 0, prox = 0;
  char line[64];
  int voltas = 0;
  while (fgets(line, sizeof(line), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // nao hold o core
    unsigned long lid = 0, lq = 0; unsigned est = 0, err = 0;
    const bool eh_hora = (sscanf(line, "H,%8lX,%lu", &lid, &lq) == 2);
    if (!eh_hora && sscanf(line, "E,%8lX,%u,%u", &lid, &est, &err) != 3) { continue; }

    int alvo = -1;
    for (int i = 0; i < n; ++i) { if (tab[i].id == (uint32_t)lid) { alvo = i; break; } }
    if (alvo < 0) {
      if (n < max) { alvo = n++; }
      else         { alvo = prox; prox = (prox + 1) % max; }
      tab[alvo].state = MESH_ENVIO_NA;
      tab[alvo].erro   = 0;
      tab[alvo].when = 0;
    }
    tab[alvo].id = (uint32_t)lid;
    if (eh_hora) { tab[alvo].when = (uint32_t)lq; continue; }
    tab[alvo].state = (uint8_t)est;
    tab[alvo].erro   = (uint8_t)err;
  }
  fclose(f);
  return n;
}

static uint32_t buscar_carimbo(const desfecho_t* tab, int n, uint32_t id)
{
  if (id == 0) { return 0; }
  for (int i = 0; i < n; ++i) { if (tab[i].id == id) { return tab[i].when; } }
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

// O QUE UM ROUTING QUER DIZER depende of QUEM o mandou ('of') e para onde ia
// o nosso pacote. Silencioso when o id nao e nosso: a malha esta cheia of
// roteamento que nao nos diz respeito.
//
// O no gera one ACK IMPLICITO when ouve one vizinho repetir o nosso pacote:
// one Routing com erro 0 vindo DELE MESMO (of == nosso numero). Ele prova que
// a malha ouviu - e, por economia of ar, o no PARA of retransmitir ali.
//   - DIFUSAO: nao ha destinatario para confirm; o implicito e o melhor que
//     existe, e vale ENTREGUE (o app oficial faz o mesmo).
//   - DIRETA: o implicito e middle caminho - vale OUVIDO. ENTREGUE so com o ACK
//     of PROPRIO destinatario (of == para).
//
// Antes os dois eram iguais: a direta virava "confirmada" no first eco of
// one vizinho, e when o no depois relatava que o destino nunca respondeu
// (erro 5, MAX_RETRANSMIT), a trava of "ENTREGUE e terminal" engolia o
// relato. A screen dizia entregue para message que nao chegou.
//
// O IMPLICITO DO MQTT NAO E "OUVIDO". Com o C6L enabled in internet, o
// firmware (src/mqtt/MQTT.cpp) gera one ACK implicito para si mesmo when o
// SERVIDOR devolve o nosso proprio pacote - igual on of one vizinho, so que
// marcado transport_mechanism = MQTT. Ele prova que o pacote chegou on
// servidor, nao que algum radio o ouviu. Contado como OUVIDO, uma direta para
// one no DESLIGADO ganhava o check cinza in hora e, como OUVIDO nao desce, o
// "tentou demais" que o radio manda depois era engolido: o X so aparecia com
// a internet desligada. O proprio firmware ja ignora esse ACK para stop as
// retransmissoes (ReliableRouter::sniffReceived); aqui ele tambem nao muda a
// marca of uma direta. Na difusao continua valendo ENTREGUE - quem esta no
// servidor recebe, e e a mesma regra of antes.
static void marcar_send_state(uint32_t id, int erro, uint32_t of, bool so_mqtt = false)
{
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    send_state_t* e = &s_envios[i];
    if (e->id != id || id == 0) { continue; }

    const bool difusao = (e->para == 0xFFFFFFFFu);
    if (so_mqtt && erro == 0 && !difusao && of != e->para) {
      ESP_LOGI(TAG, "message %08lX: reached the MQTT server - waiting for the destination", (unsigned long)id);
      return;
    }
    uint8_t novo;
    if (erro == 0) {
      novo = (difusao || of == e->para) ? MESH_ENTREGUE : MESH_OUVIDO;
    } else {
      novo = MESH_FALHOU;
    }

    // ENTREGUE e terminal: nada posterior o desfaz. OUVIDO so sobe (para
    // ENTREGUE); one erro depois dele nao vira "falhou" - a malha OUVIU, o que
    // faltou foi a confirmacao of destino, e a marca of OUVIDO ja diz isso.
    if (e->state == MESH_ENTREGUE || (e->state == MESH_OUVIDO && novo != MESH_ENTREGUE)) {
      if (erro != 0) {
        ++s_c_routing_extra;   // ver o comentario of declaracao - nao e so ruido
        ESP_LOGI(TAG, "mensagem %08lX: aviso '%s' chegou depois de %s - continua assim", (unsigned long)id,
                 mesh_erro_name((uint8_t)erro), e->state == MESH_ENTREGUE ? "entregue" : "ouvido");
      }
      return;
    }
    if (e->state == novo && e->erro == (uint8_t)erro) { return; }
    e->state = novo;
    e->erro   = (uint8_t)erro;
    s_envios_ver++;
    system_notify(AVISO_MESH_ENVIO);
    gravar_desfecho(id, novo, (uint8_t)erro);
    ESP_LOGI(TAG, "mensagem %08lX: %s%s", (unsigned long)id,
             novo == MESH_ENTREGUE ? "delivered to the destination" : novo == MESH_OUVIDO ? "heard by another node (destination not confirmed)" : "FAILED - ",
             novo == MESH_FALHOU ? mesh_erro_name((uint8_t)erro) : "");
    return;
  }
}

uint8_t tab5_mesh_estado_send_state(uint32_t id, uint8_t* erro)
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

const char* mesh_erro_name(uint8_t erro)
{
  // Routing.Error of meshtastic. So os que of para acontecer aqui; a lista
  // cresce a cada versao of firmware e o resto cai no "?".
  switch (erro) {
    case 0:   return "ok";
    case 1:   return "no route";
    case 2:   return "recusado";
    case 3:   return "no response";
    case 4:   return "no interface";
    case 5:   return "tentou demais";
    case 6:   return "wrong channel";
    case 7:   return "grande demais";
    case 8:   return "nobody answered";
    case 9:   return "transmission limit reached";
    case 32:  return "invalid request";
    case 33:  return "not authorized";
    case 34:  return "falha de chave";
    case 35:  return "chave desconhecida";
    case 38:  return "too fast";
    case MESH_ERRO_FILA: return "radio queue full";
    default:  return "?";
  }
}

// ---------------------------------------------------------------------------
// QUE HORAS SAO - emprestadas of radio
// ---------------------------------------------------------------------------
// O Tab5 acorda sem hora certa, mas todo NodeInfo traz last_heard e todo pacote
// traz rx_time, os dois em segundos of epoch e vindos of clock of C6L.
// Guardamos o maior carimbo ja visto junto com o nosso uptime naquele instante;
// dai em diante a hora anda com o nosso clock, e a conta of "ha quanto time"
// fecha com erro of segundos.
//
// O piso of 2020 e para nao aceitar carimbo of radio que ainda nao sincronizou
// (ele manda 0, ou one numero pequeno of uptime) e ancorar a referencia no ano
// of 1970 - o que faria todo no aparecer visto ha 56 anos.
#define EPOCH_MINIMO 1577836800u          // 2020-01-01

static uint32_t s_epoch_ref    = 0;       // carimbo of radio
static uint32_t s_epoch_ref_ms = 0;       // nosso uptime when ele chegou

uint32_t tab5_mesh_epoch(void)
{
  if (s_epoch_ref == 0) { return 0; }
  return s_epoch_ref + (tab5_now_ms() - s_epoch_ref_ms) / 1000;
}

// So aceita carimbo que ADIANTE o clock. Assim one last_heard antigo, of dump
// inicial, nunca puxa a referencia para tras.
static void marcar_epoch(uint32_t carimbo)
{
  if (carimbo < EPOCH_MINIMO) { return; }
  const uint32_t now = tab5_mesh_epoch();
  if (carimbo <= now) { return; }
  s_epoch_ref    = carimbo;
  s_epoch_ref_ms = tab5_now_ms();
}

// ---------------------------------------------------------------------------
// HORA AO VIVO - a unica deste arquivo que acerta o clock of sistema
// ---------------------------------------------------------------------------
// tab5_mesh_epoch() acima serve para "ha quanto time", e so. Ancorada no
// MAIOR last_heard of lista of nos, ela stays ATRASADA quanto for o silencio
// of malha - o no mais recente pode ter falado ontem. Usada como "now" ela
// acertava o clock com esse atraso: a message enviada saia carimbada no
// dia errado, e o "HOJE" of conversa pulava para o dia certo so when uma
// resposta of verdade chegava.
//
// Aqui entra so carimbo que e "now" por construcao:
//   - rx_time of pacote que chegou DEPOIS of o no terminar o dump e esvaziar
//     a queue of que guardou enquanto estavamos desconectados (esses tem o
//     rx_time of when ELE recebeu, minutos ou horas atras);
//   - o field 'time' of telemetria of nosso proprio no, pelo mesmo filtro.
// O rx_time e sempre of clock of NOSSO no (ele carimba on receber), mesmo
// para pacote que atravessou a malha ou veio pela ponte MQTT.
#define QUEUE_DRENADA_MS 10000u            // depois of end of dump
#define HORA_VIVA_VALE_MS (60u * 60u * 1000u)

static uint32_t s_config_ok_ms = 0;        // end of dump; 0 = ainda running
static uint32_t s_vivo_ref     = 0;
static uint32_t s_vivo_ref_ms  = 0;

static bool trafego_on_vivo(void)
{
  return s_config_ok_ms != 0 && (tab5_now_ms() - s_config_ok_ms) > QUEUE_DRENADA_MS;
}

static void marcar_hora_alive(uint32_t carimbo)
{
  if (carimbo < EPOCH_MINIMO || !trafego_on_vivo()) { return; }
  s_vivo_ref    = carimbo;
  s_vivo_ref_ms = tab5_now_ms();
}

uint32_t tab5_mesh_hora_alive(void)
{
  const uint32_t ref = s_vivo_ref, ms = s_vivo_ref_ms;
  if (ref == 0 || (tab5_now_ms() - ms) > HORA_VIVA_VALE_MS) { return 0; }
  return ref + (tab5_now_ms() - ms) / 1000;
}

static void lock(void)   { if (s_trava) xSemaphoreTake(s_trava, portMAX_DELAY); }
static void destravar(void){ if (s_trava) xSemaphoreGive(s_trava); }

bool     tab5_mesh_no_ar(void)       { return (tab5_now_ms() - s_ultimo_frame_ms) < SILENCIO_MS
                                           && s_ultimo_frame_ms != 0; }

// HA QUANTO TEMPO O RADIO NAO FALA, em ms. UINT32_MAX = nunca falou.
//
// Existe porque "o enlace esta vivo?" e "alguem arrancou o modulo of cabo?" sao
// perguntas diferentes e querem prazos diferentes. O no_ar() e generoso of
// proposito (90s) para nao chamar of morta uma malha que so esta quieta. A
// vigia of Grove precisa of numero CRU para escolher o proprio prazo - e, com
// ela apoiada no no_ar(), raise o SILENCIO_MS of 30s para 90s empurrou a
// deteccao of troca of modulo of 90s para 150s sem ninguem request.
uint32_t tab5_mesh_silencio_ms(void)
{
  if (s_ultimo_frame_ms == 0) {
    // NENHUM frame AINDA - mas pode ser so porque o servico acabou of
    // raise, nao porque o radio sumiu. MESMO BUG of GPS (ver o aviso grande
    // em tab5_gps_silence_ms(), achado em hardware):
    // UINT32_MAX incondicional faz a vigia of Grove (kernel/core.cpp) enxergar
    // "mudo ha 60s" no PRIMEIRO tique (ate 2s) depois of QUALQUER reinicio,
    // sem dar on radio nem os 60s of MUDO_MS que o code pretendia dar. Na
    // pratica o C6L costuma responder rapido o bastante para nunca bater
    // nessa window - mas a malha jamais teve garantia disso, e one radio
    // lento para reconectar (boot frio, ruido) cairia direto no mesmo loop
    // of reinicio a cada 2s que o GPS mostrou. Contar a partir of QUANDO O
    // SERVICO SUBIU, e nao one sentinela fixo, fecha o buraco.
    return s_servico_active ? (tab5_now_ms() - s_iniciado_em_ms) : UINT32_MAX;
  }
  return tab5_now_ms() - s_ultimo_frame_ms;
}
uint32_t tab5_mesh_meu_numero(void)  { return s_meu_num; }
const char* tab5_mesh_firmware(void) { return s_firmware; }

void tab5_mesh_set_beep(bool touch)
{
  settings_set_mesh_beep(touch);
  // Se a fonte ainda nao existe (servico nunca subiu nesta sessao), nao ha
  // bandeira para atualizar now - tab5_mesh_iniciar() aplica a preferencia
  // gravada no instante em que a fonte nascer, ver acima.
  if (s_notif >= 0) { notification_silenciar(s_notif, !touch); }
}

bool tab5_mesh_beep_enabled(void) { return settings_mesh_beep(); }
int      tab5_mesh_nao_lidas(void)   { return s_nao_lidas; }
int      tab5_mesh_nao_lidas_diretas(void) { return s_nao_lidas_dir; }
static void publicar_notif(void);   // definida adiante, junto das outras


void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* text, uint32_t* nodeinfo,
                          int* ultima_variante)
{
  if (quadros)    *quadros    = s_c_quadros;
  if (pacotes)    *pacotes    = s_c_pacotes;
  if (telemetria) *telemetria = s_c_telemetria;
  if (localstats) *localstats = s_c_localstats;
  if (text)      *text      = s_c_text;
  if (nodeinfo)   *nodeinfo   = s_c_nodeinfo;
  if (ultima_variante) *ultima_variante = s_ultima_variante;
}

// Ver o comentario of declaracao of s_c_routing_extra: nao e ruido, e one
// termometro of qualidade of rota - cresce when "alguem ouviu" chega facil
// mas a confirmacao forte of radio nao chega.
uint32_t tab5_mesh_routing_extra(void) { return s_c_routing_extra; }

void tab5_mesh_saude(mesh_saude_t* s) { if (s) { lock(); *s = s_saude; destravar(); } }

int tab5_mesh_num_nos(void)
{
  int n = 0;
  lock();
  for (int i = 0; i < MESH_MAX_NOS; ++i) { if (s_nos[i].usado) { ++n; } }
  destravar();
  return n;
}

uint32_t tab5_mesh_despejos(void) { return s_despejos; }

// Ha quantos segundos o no foi ouvido; -1 = nao of para saber. Duas fontes,
// nesta order:
//   1. nosso proprio avistamento - exato, nao depende of clock nenhum;
//   2. o last_heard of radio contra a hora emprestada dele.
static int32_t idade_of_no(const mesh_no_t& no)
{
  const uint32_t ep = tab5_mesh_epoch();
  if (no.visto_local_ms != 0) { return (int32_t)((tab5_now_ms() - no.visto_local_ms) / 1000); }
  if (no.visto_em >= EPOCH_MINIMO && ep > no.visto_em) { return (int32_t)(ep - no.visto_em); }
  return -1;
}

// A ORDEM DA TABELA, a mesma of cliente web: o nosso no first, depois of
// ouvido mais recentemente on mais antigo, e os sem idade conhecida no end.
// Antes era a order das vagas of vetor - one no que acabou of falar podia
// estar no end of lista, e a lista "embaralhava" a cada despejo. SEM TRAVA.
static uint32_t s_prox_fixado = 0;
static int      fav_pos(uint32_t num);   // favoritos, mais abaixo

// LUGAR FIXO dado NA CHEGADA of dado (pacote ou NodeInfo com SNR of radio), e
// nao when a screen pergunta: e ele que protege o no of despejo (achar_no),
// e o despejo acontece com a screen closed tambem. SEM TRAVA.
static void fixar_se_pelo_ar(mesh_no_t* no)
{
  if (no && !no->fixado && no->num != s_meu_num && tab5_mesh_no_pelo_ar(no)) {
    no->fixado = ++s_prox_fixado;
  }
}

// ORDEM EM CACHE. Com 256 vagas, ordenar a cada tab5_mesh_no(i) - uma vez por
// line desenhada - viraria milhoes of comparacoes por segundo. A order so
// muda when a tabela muda (s_nos_ver) ou when o time passa (a idade of
// faixa of bottom): refeita no maximum uma vez por segundo, ou in hora se algo
// chegou. Com a trava tomada.
static uint32_t s_nos_ver = 0;
EXT_RAM_BSS_ATTR static int s_order[MESH_MAX_NOS];
static int      s_order_n = 0;
EXT_RAM_BSS_ATTR static int s_order_rede[2][MESH_MAX_NOS];   // a mesma order, separada por rede
static int      s_order_rede_n[2] = { 0, 0 };
static uint32_t s_order_ver = UINT32_MAX;
static uint32_t s_order_ms = 0;

static int order_nos(void)
{
  if (s_order_ver == s_nos_ver && (tab5_now_ms() - s_order_ms) < 1000) { return s_order_n; }
  // MUDOU, MAS ACABOU DE ORDENAR: espera 250 ms. Com trafego denso of internet a
  // versao muda a cada pacote, e a pagina of nos pede uma line of cada vez -
  // eram varias ordenacoes of 256 nos por drawing. Um quarto of segundo of
  // atraso in order nao se ve; o processamento, sim.
  if (s_order_ver != UINT32_MAX && (tab5_now_ms() - s_order_ms) < 250) { return s_order_n; }
  s_order_ver = s_nos_ver;
  s_order_ms  = tab5_now_ms();

  // Tres faixas:
  //   0 - o nosso no;
  //   F - os FAVORITOS (estrela), in order em que foram marcados;
  //   1 - os ouvidos PELO AR, FIXOS: in order em que apareceram com SNR pela
  //       primeira vez - nao descem when outro fala;
  //   2 - o resto (MQTT, ou sem SNR), of ouvido mais recentemente on mais
  //       antigo. Quem ganha SNR sobe para a faixa 1 e stays.
  EXT_RAM_BSS_ATTR static int64_t chave[MESH_MAX_NOS];
  int n = 0;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    mesh_no_t& no = s_nos[i];
    if (!no.usado) { continue; }
    fixar_se_pelo_ar(&no);
    int64_t k;
    const int fp = fav_pos(no.num);
    if (no.num == s_meu_num) { k = -1; }
    else if (fp >= 0)        { k = fp; }                         // favoritos, in order em que foram marcados
    else if (no.fixado)      { k = ((int64_t)1 << 20) + no.fixado; }
    else {
      int64_t idade = idade_of_no(no);
      if (idade < 0) { idade = INT32_MAX; }
      k = ((int64_t)1 << 40) + idade;
    }
    int j = n++;
    while (j > 0 && k < chave[j - 1]) { s_order[j] = s_order[j - 1]; chave[j] = chave[j - 1]; --j; }
    s_order[j] = i; chave[j] = k;
  }
  s_order_n = n;
  s_order_rede_n[0] = s_order_rede_n[1] = 0;
  for (int k = 0; k < n; ++k) {
    const int r = s_nos[s_order[k]].rede == MESH_REDE_INTERNET ? 1 : 0;
    s_order_rede[r][s_order_rede_n[r]++] = s_order[k];
  }
  return n;
}

bool tab5_mesh_no_pelo_ar(const mesh_no_t* no)
{
  return !no->via_mqtt && no->snr > -128.0f;
}

// ---------------------------------------------------------------------------
// FAVORITOS - a estrela, a mesma of no e of cliente web
// ---------------------------------------------------------------------------
// O Meshtastic ja tem favoritos: NodeInfo.is_favorite (field 10) no dump, e o
// AdminMessage set_favorite_node (39) / remove_favorite_node (40) para mudar.
// Usando os dele, a estrela marcada aqui aparece no cliente web e in screen of
// no, e o no protege o favorito of despejo of NodeDB dele tambem.
//
// Guardados TAMBEM aqui (NVS), in order em que foram marcados - e a order no
// topo of tabela. Sem o radio (modo internet) a lista local vale sozinha; a
// mudanca feita assim stays PENDENTE e vai para o no no next dump. Com o
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

static bool       enlace_send(const uint8_t* body, size_t n);
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

// Posicao in lista of favoritos; -1 = nao e. SEM TRAVA.
static int fav_pos(uint32_t num)
{
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { if (s_favs[i] == num) { return i; } }
  return -1;
}

// So a lista local (e a NVS, se changed). SEM TRAVA.
static void fav_aplicar(uint32_t num, bool quer)
{
  const int pos = fav_pos(num);
  if (quer == (pos >= 0)) { return; }
  if (quer) {
    if (s_n_favs >= MESH_MAX_FAVS) { ESP_LOGW(TAG, "favoritos cheios (%d)", MESH_MAX_FAVS); return; }
    s_favs[s_n_favs++] = num;
    achar_no(num);   // favorito stays in tabela mesmo sem ter falado nesta sessao
  } else {
    for (int i = pos; i < s_n_favs - 1; ++i) { s_favs[i] = s_favs[i + 1]; }
    --s_n_favs;
  }
  salvar_favs();
  ++s_nos_ver;
}

static int fav_pending(uint32_t num)
{
  for (int i = 0; i < s_n_fav_pend; ++i) { if (s_fav_pend[i].num == num) { return i; } }
  return -1;
}

// O no esta of outro lado e ja terminou o dump: of para mandar admin.
static bool radio_aceita_admin(void)
{
  return s_servico_active && s_meu_num != 0 && s_config_ok_ms != 0
      && (s_via == SETTINGS_MESH_TRANSPORT_SERIAL || (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && mesh_ble_conectado()));
}

// ToRadio{ packet: MeshPacket{ to: nos, decoded: Data{ portnum: ADMIN(6),
// payload: <AdminMessage> }, id } }. Sem 'from' (= 0): e o que o no
// reconhece como admin LOCAL, of one cliente conectado - aceito sem chave of
// sessao (AdminModule.cpp, ramo mp.from == 0, of 2.5 ate a current).
// Unico caminho of AdminMessage of servico: favoritos e tab5_mesh_admin.
#define PORT_ADMIN 6
bool tab5_mesh_interno_admin(const uint8_t* adm, size_t n_adm)
{
  if (!adm || n_adm == 0 || n_adm > MESH_PAYLOAD_MAX + 33) { return false; }   // Data.payload: 233
  if (s_meu_num == 0) { return false; }   // sem o nosso numero, o 'to' iria 0 - e 0 nao e o no
  const mesh_pb_envelope_t e = { s_meu_num, -1, PORT_ADMIN, adm, n_adm, esp_random() | 1u, 0, false };
  uint8_t frame[MESH_PAYLOAD_MAX + 88];
  const size_t n = mesh_pb_to_radio(&e, frame, sizeof(frame));
  const bool ok = n && enlace_send(frame, n);
  if (ok) { mesh_diario_nota_send_state(MESH_ENV_ADMIN); }
  return ok;
}

bool tab5_mesh_interno_pode_admin(void) { return radio_aceita_admin(); }

static bool send_admin_favorito(uint32_t num, bool quer)
{
  // AdminMessage{ 39: set_favorite_node | 40: remove_favorite_node = num }
  uint8_t adm[12];
  pb_escritor_t w = pb_escritor(adm, sizeof(adm));
  pb_w_num(&w, quer ? 39 : 40, num);
  const bool ok = tab5_mesh_interno_admin(adm, pb_tamanho(&w));
  ESP_LOGI(TAG, "favorite %08lX -> %s node: %s", (unsigned long)num, quer ? "mark" : "remove", ok ? "sent" : "failed");
  return ok;
}

// Fim of dump: o que foi mudado sem o radio vai now. Com a trava.
static void send_favs_pendentes(void)
{
  if (!radio_aceita_admin()) { return; }
  for (int i = 0; i < s_n_fav_pend; ++i) { send_admin_favorito(s_fav_pend[i].num, s_fav_pend[i].quer); }
  s_n_fav_pend = 0;
}

bool tab5_mesh_favorito(uint32_t num)
{
  lock();
  const bool f = fav_pos(num) >= 0;
  destravar();
  return f;
}

void tab5_mesh_set_favorito(uint32_t num, bool quer)
{
  if (num == 0) { return; }
  lock();
  if (num != s_meu_num) {
    fav_aplicar(num, quer);
    // Para o no: now, se ele esta ai; senao stays pending para o dump.
    int k = fav_pending(num);
    if (radio_aceita_admin() && send_admin_favorito(num, quer)) {
      if (k >= 0) { s_fav_pend[k] = s_fav_pend[--s_n_fav_pend]; }
    } else {
      if (k < 0 && s_n_fav_pend < MESH_FAV_PEND) { k = s_n_fav_pend++; }
      if (k >= 0) { s_fav_pend[k] = fav_pend_t{ num, quer }; }
    }
  }
  destravar();
  system_notify(AVISO_MESH_NOS);
}

// O que o dump disse of one no (field 10 ausente = false, e proto3). Pendente
// nosso vence: e uma mudanca que o no ainda nao recebeu.
static void fav_of_radio(uint32_t num, bool fav)
{
  if (num == s_meu_num || s_via == SETTINGS_MESH_TRANSPORT_INTERNET || fav_pending(num) >= 0) { return; }
  fav_aplicar(num, fav);
}

bool tab5_mesh_no(int idx, mesh_no_t* saida)
{
  bool ok = false;
  lock();
  const int n = order_nos();
  if (idx >= 0 && idx < n && s_nos[s_order[idx]].usado) {   // order of ate 250 ms atras
    *saida = s_nos[s_order[idx]];
    // Campo DERIVADO, preenchido so in copia: sem isso envelheceria entre a
    // leitura e o drawing. Sem fonte none stays -1, e a screen escreve "?".
    saida->idade_s = idade_of_no(*saida);
    saida->favorito = fav_pos(saida->num) >= 0;
    ok = true;
  }
  destravar();
  return ok;
}

int tab5_mesh_num_nos_rede(uint8_t rede)
{
  lock();
  order_nos();
  const int n = s_order_rede_n[rede == MESH_REDE_INTERNET ? 1 : 0];
  destravar();
  return n;
}

bool tab5_mesh_no_rede(uint8_t rede, int idx, mesh_no_t* saida)
{
  bool ok = false;
  lock();
  order_nos();
  const int r = rede == MESH_REDE_INTERNET ? 1 : 0;
  if (idx >= 0 && idx < s_order_rede_n[r] && s_nos[s_order_rede[r][idx]].usado) {
    *saida = s_nos[s_order_rede[r][idx]];
    saida->idade_s = idade_of_no(*saida);
    saida->favorito = fav_pos(saida->num) >= 0;
    ok = true;
  }
  destravar();
  return ok;
}

int tab5_mesh_num_msgs(void) { return s_n_msgs; }

// NUNCA SATURA - ver o aviso grande junto of s_total_msgs. Existe SO para
// quem precisa saber "chegou (ou exited) message since a ultima vez que
// olhei" (comparacao direta, nao pintura_mudou_*): tab5_mesh_num_msgs()
// parece servir para isso mas nao serve, trava em MESH_MAX_MSGS (8) para
// sempre.
uint32_t tab5_mesh_total_msgs(void) { return s_total_msgs; }

bool tab5_mesh_msg(int i, mesh_msg_t* saida)
{
  if (i < 0 || i >= s_n_msgs) { return false; }
  lock();
  *saida = s_msgs[i];
  destravar();
  // Derivado in copia, igual on idade_s dos nos: o estado muda o time todo e
  // guardar in message faria a copia envelhecer entre a leitura e o uso.
  saida->send_state = tab5_mesh_estado_send_state(saida->id, &saida->erro);
  return true;
}

// ---------------------------------------------------------------------------
// Conversas e historico no cartao
// ---------------------------------------------------------------------------
// Nome of arquivo of uma conversa. Curto e sem acento of proposito: e name of
// arquivo em FAT, e o cartao ja nos ensinou que 8.3 morde (ver o caso of
// satelites.csv que nao abria por falta of LFN).
// ---------------------------------------------------------------------------
// CACHE DE NOMES DOS NOS
// ---------------------------------------------------------------------------
// COMO O MESHTASTIC IDENTIFICA UM NO. O identificador e o "num" - one uint32
// derivado dos ultimos 4 bytes of MAC of radio. Ele NAO muda: e ele que vai no
// field 1 of MeshPacket e e por ele que se endereca uma message direta.
//
// O name longo e o curto sao ETIQUETAS que a pessoa escolhe e pode switch
// when quiser. Trocar o name NAO cria outro no - o num continua o mesmo, e
// para o sistema e o mesmo interlocutor com rotulo novo. Um no novo of verdade
// e one num novo, e ai e outra conversa mesmo.
//
// Por isso o arquivo of conversa sempre foi nomeado pelo NUM (no-A1B2C3D4.txt),
// e isso estava certo since o start. O que faltava era guardar a ETIQUETA: sem
// o C6L plugado, s_nos esta vazio e a conversa nascia chamada "A1B2C3D4".
//
// Este cache resolve isso. Vive em nos.txt, uma line por avistamento, e o
// leitor stays com a ULTIMA of cada num - assim renomear e so acrescentar uma
// line, sem reescrever arquivo nenhum.
//
// HISTORICO, NAO "AO VIVO" - of proposito DESACOPLADO of MESH_MAX_NOS (32,
// o tamanho of tabela AO VIVO em s_nos - ver achar_no() e o despejo of mais
// antigo). Um historico of nos que so cresce - MESH_MAX_NOS era pequeno demais para
// isso TAMBEM (o despejo of tabela on vivo resolve "quem esta active now",
// mas nao "quem ja vi alguma vez"). Cada entrada e so 4+24+1 bytes; 256
// delas sao ~8KB - nada para one ESP32-P4 com PSRAM.
//
// SOBRE A BUSCA ANTES DE ACRESCENTAR (a duvida): sim, precisa
// - e uma busca LINEAR, sem indice nenhum, exatamente como ja era com 32.
// Ela so roda when uma NodeInfo chega (nao a cada pacote - NodeInfo e
// esporadica) e uma vez por line on carregar o arquivo no boot. Com 256
// entries isso e no maximum 256 comparacoes of inteiro - microssegundos,
// mesmo no pior caso. Nao compensa uma estrutura mais esperta (hash, arvore)
// para one numero deste tamanho; so compensaria in casa dos milhares.
#define MESH_ARQ_NOS "nos.txt"
// 1024: o cache of nomes enchia com os nos of internet e, cheio, recusava
// name novo - inclusive o of one vizinho of radio. ~53 KB in PSRAM.
#define MESH_MAX_CONHECIDOS 1024

// CURTO E LONGO, os dois - a tabela of nos saia toda sem name. Antes so o curto persistia aqui
// (ver o aviso grande junto of lembrar_nome() mais abaixo).
typedef struct { uint32_t num; char curto[MESH_NOME_CURTO]; char longo[MESH_NOME_LONGO]; bool dirty; } no_conhecido_t;
EXT_RAM_BSS_ATTR static no_conhecido_t s_conhecidos[MESH_MAX_CONHECIDOS];
static int s_n_conhecidos = 0;

// O ARQUIVO JA FOI LIDO NESTA SESSAO. Antes a guarda era "s_n_conhecidos ==
// 0": se o cartao nao montava in hora of leitura, a tabela ficava vazia, o
// dump of no a enchia, e of primeira vez que o cartao voltava TODOS os nos
// iam of novo para o end of arquivo - que so crescia, com as mesmas lines,
// a cada boot assim. Agora ninguem grava antes of read (descarregar_nomes).
static bool s_nomes_lidos = false;

static void carregar_nomes(void);

// SEM TRAVA: chamadas of dentro dela. Nulo (nao string vazia) when nao ha
// cache - "sem entrada" e "entrada vazia" sao a mesma coisa aqui, os dois
// jeitos of dizer "nada para mostrar".
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
// nas pontas. "?" sozinho e o placeholder of Meshtastic para "ninguem
// escolheu one curto" - vale como vazio.
static void name_legivel(const char* in, char* out, size_t size)
{
  if (!size) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)(in ? in : "");
  while (*p && o + 1 < size) {
    if (*p >= 0x20 && *p <= 0x7e) { out[o++] = (char)*p++; continue; }
    if ((*p == 0xC2 || *p == 0xC3) && (p[1] & 0xC0) == 0x80) {
      if (o + 2 >= size) { break; }
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
  name_legivel(s, t, sizeof(t));
  return t[0] != 0;
}

// SEM TRAVA: chamada of dentro dela (lembrar_nome, achar_ou_criar_conversa).
// Ao vivo first (mais fresco), depois o cache of cartao.
static void name_exibicao(uint32_t num, char* out, size_t out_size)
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
  name_legivel(curto ? curto : curto_conhecido(num), t, sizeof(t));
  if (!t[0]) { name_legivel(longo ? longo : longo_conhecido(num), t, sizeof(t)); }
  if (t[0]) { snprintf(out, out_size, "%s", t); return; }
  snprintf(out, out_size, "!%08lx", (unsigned long)num);
}

void tab5_mesh_name_exibicao(uint32_t num, char* out, size_t out_size)
{
  lock();
  name_exibicao(num, out, out_size);
  destravar();
}

// Ver o aviso complete em tab5_mesh.hpp. Wrapper fino sobre curto_conhecido()
// (privada deste arquivo) - existe so para expor "ha cache, sem hex" para
// quem, diferente of tab5_mesh_nome_exibicao(), NAO quer o fallback of hex.
bool tab5_mesh_name_cache(uint32_t num, char* out, size_t out_size)
{
  const char* cache = curto_conhecido(num);
  if (!cache) { return false; }
  snprintf(out, out_size, "%s", cache);
  return true;
}

// Irma of tab5_mesh_nome_cache(), para o name LONGO - ver o aviso grande no
// .hpp.
bool tab5_mesh_name_longo_cache(uint32_t num, char* out, size_t out_size)
{
  const char* cache = longo_conhecido(num);
  if (!cache) { return false; }
  snprintf(out, out_size, "%s", cache);
  return true;
}

// Grava so when MUDA. O node_info chega a cada apresentacao e a cada
// telemetria; anotar sempre encheria o cartao com a mesma line.
//
// 'curto'/'longo': cada one NULO independentemente when este NodeInfo nao
// trouxe (ou trouxe algo inutilizavel) daquele field - so atualiza o que
// veio, nunca apaga o outro que ja estava no cache.
static void lembrar_name(uint32_t num, const char* curto, const char* longo)
{
  const bool tem_curto = curto && curto[0];
  const bool tem_longo = longo && longo[0];
  if (num == 0 || (!tem_curto && !tem_longo)) { return; }
  // OS DA INTERNET NAO VAO PARA O CACHE. Sao centenas, of passagem: gravavam o
  // cartao of 15 em 15 s e tomavam as 1024 vagas dos nomes daqui. Enquanto
  // estao in tabela o name deles aparece igual (ela e consultada first).
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

  bool changed = false;
  if (tem_curto && strcmp(s_conhecidos[idx].curto, curto) != 0) {
    snprintf(s_conhecidos[idx].curto, sizeof(s_conhecidos[idx].curto), "%s", curto);
    changed = true;
  }
  if (tem_longo && strcmp(s_conhecidos[idx].longo, longo) != 0) {
    snprintf(s_conhecidos[idx].longo, sizeof(s_conhecidos[idx].longo), "%s", longo);
    changed = true;
  }
  if (!changed) { return; }

  // NAO GRAVA AGORA - so marca. Gravar aqui montava e desmontava o cartao uma
  // vez POR NO, e o radio despeja a lista inteira of uma vez in apresentacao:
  // no boot sairam oito montagens em one segundo, cada uma com o aviso of LDO
  // of SDMMC atras. A escrita vale one lote, nao one evento.
  s_conhecidos[idx].dirty = true;

  // A conversa que ja exista passa a usar o name novo, pela MESMA regra of
  // exibicao (curto, longo, id). O ARQUIVO dela e nomeado pelo num, entao
  // renomear aqui nao desgruda o historico.
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada && !s_conv[i].channel && s_conv[i].no == num) {
      name_exibicao(num, s_conv[i].name, sizeof(s_conv[i].name));
    }
  }
}

// Descarrega os nomes pendentes. UMA montagem para todos.
//
// Chamada pela tarefa of mesh, espacada: o cartao e caro of montar e name of no
// nao e informacao urgente - se o aparelho reiniciar antes of descarga, o
// next node_info reensina.
static void descarregar_nomes(void)
{
  int pendentes = 0;
  for (int i = 0; i < s_n_conhecidos; ++i) { if (s_conhecidos[i].dirty) { ++pendentes; } }
  if (pendentes == 0) { return; }

  // Sem ter lido o arquivo, nao of para saber se o name e novo: tenta read
  // now; sem cartao, os pendentes esperam a next iteration.
  if (!s_nomes_lidos) { carregar_nomes(); }
  if (!s_nomes_lidos) { return; }
  pendentes = 0;
  for (int i = 0; i < s_n_conhecidos; ++i) { if (s_conhecidos[i].dirty) { ++pendentes; } }
  if (pendentes == 0) { return; }

  // FORMATO GANHOU UM CAMPO: "hex,curto,longo" - era so
  // "hex,curto". carregar_nomes() (abaixo) le os dois formatos: uma line
  // antiga sem o segundo virgula so deixa o longo vazio, sem break nada.
  // Pelo gravador of sistema: entregar e in hora, gravar e em lote.
  for (int i = 0; i < s_n_conhecidos; ++i) {
    if (!s_conhecidos[i].dirty) { continue; }
    char line[96];
    snprintf(line, sizeof(line), "%08lX,%s,%s\n", (unsigned long)s_conhecidos[i].num,
             s_conhecidos[i].curto, s_conhecidos[i].longo);
    tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_NOS, line);
    s_conhecidos[i].dirty = false;
  }
  ESP_LOGI(TAG, "%d nome(s) de no gravados", pendentes);
}

// Le o cache of cartao. Ultima line of cada num vence.
//
// DOIS FORMATOS aceitos - ver o aviso em descarregar_nomes(): "hex,curto"
// (antigo) e "hex,curto,longo" (current). A quebra of line e removida ANTES
// of procurar a segunda virgula - ela so pode estar no end of verdade,
// nao importa quantas virgulas vierem antes.
static void carregar_nomes(void)
{
  if (!tab5_sd_mount()) { return; }
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_NOS, "r");
  if (!f) { s_nomes_lidos = true; tab5_sd_unmount(); return; }   // ainda nao existe

  // Quem ja estava in tabela antes of leitura veio of no on vivo.
  EXT_RAM_BSS_ATTR static bool on_vivo[MESH_MAX_CONHECIDOS];
  for (int i = 0; i < MESH_MAX_CONHECIDOS; ++i) { on_vivo[i] = (i < s_n_conhecidos); }

  int lines = 0;
  char line[96];
  while (fgets(line, sizeof(line), f)) {
    char* resto = strchr(line, ',');
    if (!resto) { continue; }
    *resto++ = 0;   // line = so o hex now
    ++lines;

    char* end = strpbrk(resto, "\r\n");
    if (end) { *end = 0; }   // resto = "curto" ou "curto,longo", sem quebra

    char* curto = resto;
    char* longo = strchr(resto, ',');
    if (longo) { *longo++ = 0; }   // formato novo: separa os dois campos

    if (!curto[0] && (!longo || !longo[0])) { continue; }

    const uint32_t num = (uint32_t)strtoul(line, nullptr, 16);
    if (num == 0) { continue; }

    int idx = -1;
    for (int i = 0; i < s_n_conhecidos; ++i) {
      if (s_conhecidos[i].num == num) { idx = i; break; }
    }
    // O no on vivo ja ensinou o name antes desta leitura: ele vence, e so
    // precisa ir para o arquivo se for diferente of ULTIMA line dele la.
    if (idx >= 0 && on_vivo[idx]) {
      s_conhecidos[idx].dirty = strcmp(s_conhecidos[idx].curto, curto) != 0
                            || strcmp(s_conhecidos[idx].longo, longo ? longo : "") != 0;
      continue;
    }
    if (idx < 0) {
      if (s_n_conhecidos >= MESH_MAX_CONHECIDOS) { continue; }
      idx = s_n_conhecidos++;
      s_conhecidos[idx].num = num;
      s_conhecidos[idx].curto[0] = 0;
      s_conhecidos[idx].longo[0] = 0;
      s_conhecidos[idx].dirty = false;
    }
    if (curto[0])         { snprintf(s_conhecidos[idx].curto, sizeof(s_conhecidos[idx].curto), "%s", curto); }
    if (longo && longo[0]) { snprintf(s_conhecidos[idx].longo, sizeof(s_conhecidos[idx].longo), "%s", longo); }
  }
  fclose(f);
  s_nomes_lidos = true;

  // COMPACTA. O arquivo e "so acrescentar" (renomear = uma line nova), entao
  // cresce com lines que a leitura acima ja descartou; e arquivos gravados
  // antes of guarda s_nomes_lidos tem o mesmo no repetido dezenas of vezes.
  // Com sobra demais, reescreve com uma line por no - e ja leva junto o que
  // estava pending.
  if (lines > s_n_conhecidos + 32) {
    // O arquivo novo inteiro in PSRAM e UMA troca pela porta of sistema
    // (tab5_sd_replace) - nunca middle arquivo.
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
        for (int i = 0; i < s_n_conhecidos; ++i) { s_conhecidos[i].dirty = false; }
        ESP_LOGI(TAG, "card: nos.txt compacted from %d to %d line(s)", lines, s_n_conhecidos);
      }
      heap_caps_free(txt);
    }
  }
  tab5_sd_unmount();
  ESP_LOGI(TAG, "card: %d known node(s)", s_n_conhecidos);
}

// ---------------------------------------------------------------------------
// CONVERSAS APAGADAS - A FLAG QUE FALTAVA.
// ---------------------------------------------------------------------------
// O BUG: erase uma conversa (tab5_mesh_esquecer_conversa) sempre so mexeu in
// RAM, of proposito - "o cartao nao e touched" era a regra, para o historico
// sobreviver caso voces voltassem a se falar. So que tab5_mesh_carregar_do_
// cartao() RELE A PASTA INTEIRA sempre que roda (open o app, exit of
// sleep), e o arquivo no-XXXXXXXX.txt continuava la - entao a conversa
// "apagada" voltava sozinha in next varredura. Nao era a message
// reaparecendo, era a PASTA sendo lida nova of novo sem lembrar of decisao.
//
// A CORRECAO E A FLAG QUE FOI PEDIDA: one arquivo pequeno, apagadas.txt, uma
// line por numero of no escondido. tab5_mesh_carregar_do_cartao() para of
// recriar a conversa of quem esta nesta lista - o historico continua no
// cartao (erase so o REGISTRO, nao o passado, segue sendo a regra), so que
// now ele stays invisivel of proposito, nao por acidente.
//
// UM NO SAI DA LISTA SOZINHO when voces voltam a se falar of verdade - touch
// deliberado em "conversar com este no" (tab5_mesh_conversa_com) ou message
// nova chegando (guardar_msg). Reler o cartao NAO conta como "voltar a se
// falar"; so content novo conta. UM CANAL exit igual - touch em "conversar com
// este canal" (tab5_mesh_conversa_do_canal) ou message nova nele.
//
// CANAL PASSOU A ENTRAR AQUI - ate entao so no direto podia ser
// apagado ("canal nao se apaga, e return false e mais honesto que fingir",
// dizia o comentario velho of tab5_mesh_esquecer_conversa). O request of
// oferecer "canais disponiveis" para reabrir depois exigia justamente a
// mesma flag que ja existia para no - so precisava aprender a guardar QUAL
// TIPO of chave e essa, ja que indice of canal (0..7) e numero of no (sempre
// one valor grande, derivado of MAC) nao podem se confundir no MESMO field.
typedef struct { bool channel; uint32_t key; } apagado_t;
#define MESH_ARQ_APAGADAS "apagadas.txt"

#define MESH_MAX_APAGADOS 64   // conversas escondidas; nada a ver com a tabela of nos
static apagado_t s_apagados[MESH_MAX_APAGADOS];
static int       s_n_apagados = 0;
static bool      s_apagados_carregado = false;

// FORMATO DO ARQUIVO: uma line por entrada.
//   no direto:  "%08lX"   (como sempre foi - RETROCOMPATIVEL of proposito,
//               nenhum arquivo apagadas.txt ja gravado precisa mudar)
//   canal:      "K%u"     ('K' nunca e digito hexadecimal (0-9A-F), entao
//               nunca se confunde com one numero of no legitimo lido of
//               formato antigo)
static void carregar_apagados(void)
{
  // Sem cartao NAO conta como carregado: a lista of RAM nao pode virar o
  // arquivo (desmarcar_apagada reescreve). Tenta of novo no next uso.
  if (!tab5_sd_mount()) { return; }
  s_apagados_carregado = true;   // com o cartao: uma leitura por sessao, mesmo se o arquivo faltar
  // O ARQUIVO E A VERDADE: o que foi marcado antes desta leitura ja esta nele
  // (tab5_sd_open aplica o diario e o pending antes of open).
  s_n_apagados = 0;
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_APAGADAS, "r");
  if (!f) { tab5_sd_unmount(); return; }

  char line[16];
  while (fgets(line, sizeof(line), f) && s_n_apagados < MESH_MAX_APAGADOS) {
    if (line[0] == 'K') {
      const uint32_t idx = (uint32_t)strtoul(line + 1, nullptr, 10);
      if (idx < MESH_MAX_CANAIS || idx == MESH_CANAL_INTERNET) {
        s_apagados[s_n_apagados].channel = true;
        s_apagados[s_n_apagados].key = idx;
        ++s_n_apagados;
      }
      continue;
    }
    const uint32_t num = (uint32_t)strtoul(line, nullptr, 16);
    if (num != 0) {
      s_apagados[s_n_apagados].channel = false;
      s_apagados[s_n_apagados].key = num;
      ++s_n_apagados;
    }
  }
  fclose(f);
  tab5_sd_unmount();
  ESP_LOGI(TAG, "card: %d conversation(s) deleted - hidden", s_n_apagados);
}

// SEM TRAVA: chamada of dentro dela.
static bool esta_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].channel == canal && s_apagados[i].key == chave) { return true; }
  }
  return false;
}

// Marca uma conversa como apagada: RAM first (vale in hora, mesmo sem
// cartao), depois grava a line - a line extra no arquivo, sem reescrever
// everything, e o mesmo truque of nos.txt.
static void marcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (esta_apagada(canal, chave)) { return; }
  if (s_n_apagados < MESH_MAX_APAGADOS) {
    s_apagados[s_n_apagados].channel = canal;
    s_apagados[s_n_apagados].key = chave;
    ++s_n_apagados;
  }

  char line[16];
  if (canal) { snprintf(line, sizeof(line), "K%u\n", (unsigned)chave); }
  else       { snprintf(line, sizeof(line), "%08lX\n", (unsigned long)chave); }
  tab5_sd_enqueue_append(MESH_PASTA, MESH_ARQ_APAGADAS, line);
}

// Tira uma conversa of lista of apagados - chamado when voces voltam a se
// falar of verdade (ver o comentario grande acima). Reescreve o arquivo
// inteiro: e raro (so acontece on reatar algo que voce tinha apagado), e a
// lista e pequena.
static void desmarcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (!s_apagados_carregado) { return; }   // sem a lista of cartao nao se reescreve o arquivo
  int achou = -1;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].channel == canal && s_apagados[i].key == chave) { achou = i; break; }
  }
  if (achou < 0) { return; }   // nao estava apagado - nada a fazer, nada a reescrever

  for (int i = achou; i < s_n_apagados - 1; ++i) { s_apagados[i] = s_apagados[i+1]; }
  --s_n_apagados;

  // Reescreve of zero pela porta of sistema: o substituir grava antes as
  // lines of "apagada" que ainda estavam para entrar, e com o cartao
  // emprestado on computador vai para o diario.
  char txt[MESH_MAX_APAGADOS * 12 + 1];
  size_t k = 0;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].channel) { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "K%u\n", (unsigned)s_apagados[i].key); }
    else                     { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "%08lX\n", (unsigned long)s_apagados[i].key); }
  }
  tab5_sd_replace(MESH_PASTA, MESH_ARQ_APAGADAS, txt, k);
}

static void arquivo_of_conversa(const mesh_conversa_t* c, char* out, size_t size)
{
  if (c->channel && mesh_canal_internet(c->idx)) { snprintf(out, size, "internet-%u.txt", (unsigned)(c->idx & 0x7F)); }
  else if (c->channel) { snprintf(out, size, "canal-%u.txt", (unsigned)c->idx); }
  else          { snprintf(out, size, "no-%08lX.txt", (unsigned long)c->no); }
}

static int achar_ou_create_conversa(bool canal, uint8_t idx, uint32_t no)
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
    ++s_conv_ver;   // a order of coluna changed
    s_conv[i].usada = true;
    s_conv[i].channel = canal;
    s_conv[i].idx = idx;
    s_conv[i].no  = no;
    if (const nao_lida_guardada_t* g = guardada_of(canal, idx, no, false)) { s_conv[i].nao_lidas = g->n; }
    if (canal) {
      // Nome REAL, se o radio ja nos contou (FromRadio field 10). O rotulo
      // inventado abaixo e so o que resta enquanto a configuracao nao chegou.
      if (mesh_canal_internet(idx)) {
        snprintf(s_conv[i].name, sizeof(s_conv[i].name), "LongFast Internet");
      } else if (idx < MESH_MAX_CANAIS && s_canal_name[idx][0]) {
        snprintf(s_conv[i].name, sizeof(s_conv[i].name), "%s", s_canal_name[idx]);
      } else {
        snprintf(s_conv[i].name, sizeof(s_conv[i].name),
                 idx == 0 ? "LongFast" : "Canal %u", (unsigned)idx);
      }
    } else {
      // Nome of no - curto, longo, e o id so em ultimo caso (nome_exibicao).
      name_exibicao(no, s_conv[i].name, sizeof(s_conv[i].name));
    }
    return i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// GRAVACAO DAS MENSAGENS
// ---------------------------------------------------------------------------
// O Mesh tinha tarefa e queue PROPRIAS of gravacao ("mesh_grava",
// apertar send travava a screen montando o cartao). A ideia era
// certa e virou of SISTEMA: tab5_sd_enqueue_append (hal/tab5_sd.hpp) copia a line e
// iteration in hora, e a tarefa "cartao" grava em lote. A message vai como
// URGENTE: gravada em milissegundos, nao no lote of 5 s.
// Acrescenta on arquivo of conversa. Formato of UMA line, com o text POR
// ULTIMO para que virgula dentro of message nao quebre a leitura:
//   M,<when>,<of>,<snr>,<rssi>,<text>
//
// NAO FAZ MAIS E/S AQUI - ver o aviso grande acima, junto of laco_gravar().
// Isto roda com s_trava preso (chamado of dentro of guardar_msg()) e as
// vezes in tarefa of INTERFACE - tem que ser rapido incondicionalmente.
static void gravar_msg(int ci, const mesh_msg_t* m)
{
  if (ci < 0) { return; }
  char arquivo[32];
  arquivo_of_conversa(&s_conv[ci], arquivo, sizeof(arquivo));
  // "M4" e nao "M3": o registro ganhou os saltos - etiqueta nova
  // em vez of field a mais, e o leitor sabe o que wait antes of read.
  char line[sizeof(((mesh_msg_t*)0)->text) + 96];
  snprintf(line, sizeof(line), "M4,%lu,%08lX,%.1f,%ld,%d,%u,%08lX,%s\n",
           (unsigned long)m->when, (unsigned long)m->of,
           m->snr, (long)m->rssi, (int)m->via_mqtt, (unsigned)m->saltos,
           (unsigned long)m->id, m->text);
  if (!tab5_sd_enqueue_append(MESH_PASTA, arquivo, line, nullptr, true)) {
  ESP_LOGW(TAG, "writer full - message was NOT saved to the card");
  }
}

// COMECAR A FALAR COM ALGUEM QUE AINDA NAO FALOU CONOSCO.
//
// Ate aqui uma conversa direta so nascia when a message CHEGAVA
// (achar_ou_criar_conversa no caminho of recepcao). Ou seja: dava para
// responder, nunca para puxar assunto. Esta funcao abre o outro sentido, a
// partir of lista of nos.
//
// Devolve o indice VISIVEL - o mesmo que a coluna of conversas usa - e nao o
// interno, porque quem chama e a screen e e com esse que ela sabe trabalhar. Os
// dois divergem assim que uma conversa e liberada no middle of vetor.
// O NOSSO NUMERO SOBREVIVE AO DESLIGAMENTO.
//
// Ele so chegava pelo my_info of radio. Sem o C6L plugado valia 0 - e como a
// screen decide o lado of bubble por (m->of == meu_numero), TODA message of
// arquivo aparecia como recebida, inclusive as que voce mandou. O arquivo
// abria contando a conversa errada.
//
// Fica in NVS of proprio mesh, e nao em ajustes: ajustes guarda o que a PESSOA
// escolheu, e isto e coisa que o radio contou. Misturar os dois faria o modulo
// of preferences saber of coisas que nao sao preferencia.

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

bool tab5_mesh_servico_active(void) { return s_servico_active; }

// AS CONVERSAS QUE ESTAO NO CARTAO, sem precisar of radio.
//
// O historico sempre esteve gravado, mas a LISTA of conversas so existia in
// memoria - e ela so era povoada por message chegando. Sem o C6L plugado, o
// app abria dizendo "sem conversas" com o cartao cheio delas. O arquivo estava
// la; faltava alguem read o indice.
//
// A pasta e o proprio indice: cada conversa e one arquivo, e o name dele carrega
// a chave (canal-3.txt, no-A1B2C3D4.txt). Nao precisa of catalogo a parte, que
// seria mais one estado para ficar dessincronizado of que descreve.
//
// Serve TAMBEM com o radio no ar: conversas antigas passam a aparecer in coluna
// since a abertura, em vez of so when alguem iteration a falar. Renomear e
// seguro - o name of canal chega depois no dump of configuracao e sobrescreve
// o rotulo inventado, porque o arquivo e nomeado pelo INDICE e nao pelo name.
int tab5_mesh_carregar_of_cartao(void)
{
  // Sem radio, o numero vem of NVS. E o que permite on arquivo saber quais
  // mensagens sao suas.
  if (s_meu_num == 0) { s_meu_num = recordar_meu_numero(); }

  // Nomes ANTES das conversas: e o cache que of name a elas when o radio nao
  // esta plugado. Na order inversa, toda conversa nasceria chamada pelo numero.
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
  lock();
  recordar_canais();
  while ((e = readdir(dir)) != NULL) {
    const char* n = e->d_name;
    if (strcmp(n, MESH_ARQ_ENVIOS) == 0) { continue; }   // desfechos, nao conversa

    if (strncmp(n, "canal-", 6) == 0) {
      const unsigned long idx = strtoul(n + 6, nullptr, 10);
      // !esta_apagada(true, idx) ACRESCENTADO - faltava aqui, e
      // era a SEGUNDA causa (independente of config of radio) of one canal
      // apagado voltar sozinho: esta varredura roda on open o app e nao
      // sabia nada sobre "apagada", so olhava se o arquivo existia no
      // cartao. Mesma logica que "no-" ja usava, so nunca tinha sido
      // estendida para canal.
      // Canal DESABILITADO no no (configuracao conhecida, ver
      // recordar_canais): o arquivo stays no cartao, a conversa nao iteration.
      const bool disabled = s_canais_conhecidos && idx < MESH_MAX_CANAIS && !s_canal_active[idx];
      if (idx < MESH_MAX_CANAIS && !disabled && !esta_apagada(true, (uint8_t)idx)
       && achar_ou_create_conversa(true, (uint8_t)idx, 0) >= 0) { ++achadas; }
    }
    else if (strncmp(n, "internet-", 9) == 0 || strncmp(n, "eua-", 4) == 0) {   // segundo name: historico antigo
      // A conversa of internet, com historico no cartao: iteration mesmo com a rede
      // desligada (so leitura), como as outras.
      if (!esta_apagada(true, MESH_CANAL_INTERNET)
       && achar_ou_create_conversa(true, MESH_CANAL_INTERNET, 0) >= 0) { ++achadas; }
    }
    else if (strncmp(n, "no-", 3) == 0) {
      // Hexadecimal, como foi escrito. Zero nao e no - e o que o strtoul
      // devolve when o name nao presta, entao serve of peneira tambem.
      const uint32_t num = (uint32_t)strtoul(n + 3, nullptr, 16);
      // Apagada of proposito: nao recriar so porque o arquivo ainda existe no
      // cartao (ver o comentario grande em carregar_apagados, acima). O
      // historico continua salvo - so nao iteration a aparecer sozinho.
      if (num != 0 && !esta_apagada(false, num) && achar_ou_create_conversa(false, 0, num) >= 0) { ++achadas; }
    }
  }
  destravar();
  closedir(dir);
  tab5_sd_unmount();

  ESP_LOGI(TAG, "card: %d conversation(s) in the file", achadas);
  return achadas;
}

int tab5_mesh_conversa_com(uint32_t no)
{
  // Numero zero nao e no; e o preenchimento of quem nao sabe. E conversa com o proprio no
  // mesmo nao existe no protocolo - o radio nao entrega a si proprio.
  if (no == 0) { return -1; }

  lock();
  if (no == s_meu_num) { destravar(); return -1; }

  const int interno = achar_ou_create_conversa(false, 0, no);
  const int visivel = (interno >= 0) ? visivel_of(interno) : -1;
  destravar();

  // Toque deliberado em "conversar com este no": e a iteration a se falar que
  // desfaz one erase anterior (ver o comentario grande acima of flag).
  if (interno >= 0) { desmarcar_apagada(false, no); }
  return visivel;
}

// Mesmo papel of tab5_mesh_conversa_com(), para CANAL - criado
// junto of opcao of erase canal, para "canais disponiveis" poder reabrir one
// que foi escondido (ver o aviso grande of flag of apagados, e
// tab5_mesh_num_canais_disponiveis()/tab5_mesh_canal_disponivel() no end
// deste arquivo).
int tab5_mesh_conversa_of_canal(uint8_t idx)
{
  if (idx >= MESH_MAX_CANAIS && idx != MESH_CANAL_INTERNET) { return -1; }

  lock();
  const int interno = achar_ou_create_conversa(true, idx, 0);
  const int visivel = (interno >= 0) ? visivel_of(interno) : -1;
  destravar();

  // Toque deliberado em "conversar com este canal": desfaz one erase
  // anterior, mesmo raciocinio of conversa_com() para no.
  if (interno >= 0) { desmarcar_apagada(true, idx); }
  return visivel;
}

// ---------------------------------------------------------------------------
// A ORDEM DA COLUNA
// ---------------------------------------------------------------------------
// Canais first, pelo indice; depois as diretas, mais recente em top. O
// vetor s_conv e one conjunto of vagas, nao uma order: uma conversa nova
// ocupa a primeira vaga livre, que pode ficar ANTES of conversa open. Com o
// indice visivel sendo "a n-esima vaga usada", isso deslocava a conversa
// open uma casa - o app passava a draw o historico of uma e a send
// para outra. Agora a order e explicita e a mudanca dela e anunciada
// (s_conv_ver), para o app reencontrar a sua pela chave.
//
// SEM TRAVA: chamadas of dentro dela. Insercao direta: sao no maximum 12.
static bool vem_antes(int a, int b)
{
  const mesh_conversa_t& x = s_conv[a];
  const mesh_conversa_t& y = s_conv[b];
  // A internet embaixo of everything, depois das diretas: e abaixo of traco in coluna.
  const bool ex = x.channel && mesh_canal_internet(x.idx), ey = y.channel && mesh_canal_internet(y.idx);
  if (ex != ey) { return ey; }
  if (x.channel != y.channel) { return x.channel; }
  if (x.channel)            { return x.idx < y.idx; }
  if (s_conv_seq[a] != s_conv_seq[b]) { return s_conv_seq[a] > s_conv_seq[b]; }
  return a < b;
}

static int order_conversas(int out[MESH_MAX_CONVERSAS])
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

static int interno_of(int visivel)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = order_conversas(o);
  return (visivel >= 0 && visivel < n) ? o[visivel] : -1;
}

static int visivel_of(int interno)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = order_conversas(o);
  for (int k = 0; k < n; ++k) { if (o[k] == interno) { return k; } }
  return -1;
}

uint32_t tab5_mesh_conversas_versao(void) { return s_conv_ver; }

int tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no)
{
  int v = -1;
  lock();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada || s_conv[i].channel != canal) { continue; }
    if (canal ? (s_conv[i].idx == idx) : (s_conv[i].no == no)) { v = visivel_of(i); break; }
  }
  destravar();
  return v;
}

// ---------------------------------------------------------------------------
// OS CANAIS DO NO - ativos, com name, e lembrados entre one boot e outro
// ---------------------------------------------------------------------------
// A configuracao dos canais so chega no dump of radio. Sem guarda-la, com o no
// fora (so consulta, ou modo internet) nao havia como saber que one canal
// tinha sido DESABILITADO: o arquivo canal-N.txt continuava no cartao e a
// conversa voltava in varredura of pasta. Fica in NVS of proprio mesh, on
// lado of nosso numero - e informacao que o radio contou, nao preferencia.
//
// Gravada em LOTE (s_canais_sujos), no end of dump ou in iteration of 15 s of
// tarefa: o dump traz os 8 canais em sequencia e cada one seria uma escrita.
typedef struct { uint8_t active; char name[MESH_NOME_CONVERSA]; } canal_salvo_t;
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
      s_canal_active[i] = tab[i].active != 0;
      tab[i].name[sizeof(tab[i].name) - 1] = 0;
      snprintf(s_canal_name[i], sizeof(s_canal_name[i]), "%s", tab[i].active ? tab[i].name : "");
    }
    s_canais_conhecidos = true;
  }
  nvs_close(h);
}

// A COPIA sob a trava, a GRAVACAO fora dela: a NVS grava flash (o cache
// para, ms), e com a trava presa o radio esperava junto para guardar o
// next frame. Roda in carona of tarefa "cartao" (ver mesh_na_carona).
static void salvar_canais(void)
{
  canal_salvo_t tab[MESH_MAX_CANAIS];
  lock();
  if (!s_canais_sujos) { destravar(); return; }
  s_canais_sujos = false;
  memset(tab, 0, sizeof(tab));
  for (int i = 0; i < MESH_MAX_CANAIS; ++i) {
    tab[i].active = s_canal_active[i] ? 1 : 0;
    snprintf(tab[i].name, sizeof(tab[i].name), "%s", s_canal_name[i]);
  }
  destravar();
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_blob(h, MESH_NVS_CANAIS, tab, sizeof(tab));
  nvs_commit(h);
  nvs_close(h);
}

// Nome of one canal para mostrar. Nome vazio no Meshtastic quer dizer "o name
// of preset of modem" - LongFast no default of fabrica.
static void name_of_canal(int idx, char* out, size_t size)
{
  if (idx == MESH_CANAL_INTERNET) { snprintf(out, size, "LongFast Internet"); }
  else if (idx >= 0 && idx < MESH_MAX_CANAIS && s_canal_name[idx][0]) { snprintf(out, size, "%s", s_canal_name[idx]); }
  else if (idx == 0) { snprintf(out, size, "LongFast"); }
  else               { snprintf(out, size, "Canal %d", idx); }
}

// Some com a conversa of one canal of coluna, SEM marcar como apagada: quem a
// tirou foi a configuracao of no, nao voce. Reativando o canal no no, ela
// iteration sozinha no next dump, com o historico of cartao.
static void hide_conversa_canal(int idx)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada && s_conv[i].channel && s_conv[i].idx == idx) {
      memset(&s_conv[i], 0, sizeof(s_conv[i]));
      s_conv_seq[i] = 0;
      ++s_conv_ver;
      publicar_notif();   // as nao lidas dela saem of total
    }
  }
}

// Um canal of configuracao (FromRadio field 10), JA com os zeros of proto3
// tratados (ver o leitor). Com a trava tomada.
static void aplicar_canal(int idx, int papel, const char* name_radio, const uint8_t* psk, size_t n_psk)
{
  recordar_canais();   // antes of comparar: a NVS nao pode sobrescrever o que chegou now
  const bool active = (papel != 0);
  char name[MESH_NOME_CONVERSA] = "";
  if (active) {
    if (name_radio[0]) { snprintf(name, sizeof(name), "%s", name_radio); }
    else if (idx == 0) { snprintf(name, sizeof(name), "LongFast"); }
    else               { snprintf(name, sizeof(name), "Canal %d", idx); }
  }
  mesh_mqtt_aprender_canal(idx, papel, name, active ? psk : nullptr, active ? n_psk : 0);

  if (s_canal_active[idx] != active || strcmp(s_canal_name[idx], name) != 0 || !s_canais_conhecidos) {
    s_canais_sujos = true;
  }
  s_canal_active[idx] = active;
  snprintf(s_canal_name[idx], sizeof(s_canal_name[idx]), "%s", name);
  s_canais_conhecidos = true;

  if (!active) {
    // DESABILITADO NO NO: exit of coluna. Era o caso of canal disabled no
    // cliente web que continuava aparecendo aqui.
    hide_conversa_canal(idx);
    return;
  }
  ESP_LOGI(TAG, "canal %d: \"%s\"", idx, name);

  // CANAL EXISTE, ENTAO A CONVERSA EXISTE - salvo se voce a apagou. Sem isso
  // one canal criado no celular ficava invisivel ate alguem falar nele.
  if (!esta_apagada(true, (uint8_t)idx)) { achar_ou_create_conversa(true, (uint8_t)idx, 0); }
  // Renomeia a que ja exista. O arquivo e pelo INDICE (canal-0.txt), entao
  // renomear nao desgruda o historico.
  for (int k = 0; k < MESH_MAX_CONVERSAS; ++k) {
    if (s_conv[k].usada && s_conv[k].channel && s_conv[k].idx == idx) {
      snprintf(s_conv[k].name, sizeof(s_conv[k].name), "%s", name);
    }
  }
}

// As conversas dos canais ativos (sem as apagadas), ou so o primario when
// none configuracao e conhecida ainda. Com a trava tomada.
static void create_conversas_dos_canais(void)
{
  recordar_canais();
  bool algum = false;
  for (int idx = 0; idx < MESH_MAX_CANAIS; ++idx) {
    if (!s_canais_conhecidos || !s_canal_active[idx]) { continue; }
    algum = true;
    if (!esta_apagada(true, (uint8_t)idx)) { achar_ou_create_conversa(true, (uint8_t)idx, 0); }
  }
  if (!algum && !esta_apagada(true, 0)) { achar_ou_create_conversa(true, 0, 0); }
  if (settings_mesh_public_network() && !esta_apagada(true, MESH_CANAL_INTERNET)) { achar_ou_create_conversa(true, MESH_CANAL_INTERNET, 0); }
}

// Indices dos canais a OFERECER: os ativos; sem nenhum conhecido, o primario.
// O of internet vem por ultimo, com a rede enabled.
static int list_canais(uint8_t out[MESH_MAX_CANAIS + 1])
{
  int n = 0;
  for (int idx = 0; idx < MESH_MAX_CANAIS; ++idx) {
    if (s_canais_conhecidos && s_canal_active[idx]) { out[n++] = (uint8_t)idx; }
  }
  if (n == 0) { out[n++] = 0; }
  if (settings_mesh_public_network()) { out[n++] = MESH_CANAL_INTERNET; }
  return n;
}

int tab5_mesh_num_canais(void)
{
  uint8_t l[MESH_MAX_CANAIS + 1];
  lock();
  recordar_canais();
  const int n = list_canais(l);
  destravar();
  return n;
}

bool tab5_mesh_canal(int i, uint8_t* idx_saida, char* name, size_t size, bool* visivel)
{
  uint8_t l[MESH_MAX_CANAIS + 1];
  bool ok = false;
  lock();
  recordar_canais();
  const int n = list_canais(l);
  if (i >= 0 && i < n) {
    const uint8_t idx = l[i];
    if (idx_saida) { *idx_saida = idx; }
    if (name)      { name_of_canal(idx, name, size); }
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
// Nasceu of one efeito colateral of "touch no no para falar com ele": bastava
// TOCAR para a conversa existir, e ela ficava la vazia para sempre. Criar sem
// oferecer como desfazer e one caminho so of ida.
//
// CANAL NAO SE APAGAVA - "return false e mais honesto que
// fingir", dizia o comentario velho aqui, porque nao havia flag none que
// impedisse o canal of voltar sozinho in next message ou in next
// config. Agora ha (a mesma "apagados" que ja servia no direto, extendida
// para guardar tambem indice of canal - ver o aviso grande junto dela) e os
// dois pontos que recriavam a conversa sozinhos (a varredura of cartao e o
// parser of config, ver os notices la) ja checam essa flag. Falta so o "voltar
// a falar of proposito" - tab5_mesh_conversa_do_canal() cuida disso.
//
// O CARTAO NAO E TOCADO, of proposito. O que se apaga e o registro em memoria,
// nao o historico. Se voces voltarem a se falar, a conversa renasce com o que
// ja tinha sido dito - que e o que se espera of uma lista, e nao of one
// expurgo. Uma conversa vazia, que e o caso que motivou isto, nao tem nada no
// cartao mesmo.
bool tab5_mesh_esquecer_conversa(int idx)
{
  bool ok = false, canal_apagado = false;
  uint32_t chave_apagada = 0;
  lock();
  const int i = interno_of(idx);
  if (i >= 0) {
    // As nao lidas dela saem of total sozinhas: publicar_notif() reconta.
    // Guardado antes of memset erase os proprios campos.
    canal_apagado = s_conv[i].channel;
    chave_apagada = canal_apagado ? s_conv[i].idx : s_conv[i].no;
    if (nao_lida_guardada_t* g = guardada_of(s_conv[i].channel, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));   // usada iteration a false
    s_conv_seq[i] = 0;
    ++s_conv_ver;
    ok = true;
  }
  if (ok) { publicar_notif(); }
  destravar();

  // Fora of trava: e I/O of cartao (fopen/fclose), nao deve hold o mutex
  // que o resto of servico (recepcao, send_state) tambem disputa. Canal-0 e chave
  // valida (LongFast e o canal 0), entao so no direto tem "zero = invalido".
  if (ok && (canal_apagado || chave_apagada != 0)) {
    marcar_apagada(canal_apagado, chave_apagada);
  }
  return ok;
}

int tab5_mesh_num_conversas(void)
{
  int n = 0;
  lock();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) { if (s_conv[i].usada) { ++n; } }
  destravar();
  return n;
}

bool tab5_mesh_conversa(int idx, mesh_conversa_t* saida)
{
  bool ok = false;
  lock();
  const int i = interno_of(idx);
  if (i >= 0) {
    *saida = s_conv[i];
    // A segunda line, derivada now (ver mesh_conversa_t.sub): o name
    // longo of quem e a conversa direta, se ele disser algo que o name of
    // primeira line ainda nao disse.
    saida->sub[0] = 0;
    if (saida->channel) {
      if (mesh_canal_internet(saida->idx)) { snprintf(saida->sub, sizeof(saida->sub), "rede internet"); }
      else { snprintf(saida->sub, sizeof(saida->sub), saida->idx == 0 ? "primary channel" : "channel %u", (unsigned)saida->idx); }
    } else {
      const char* longo = nullptr;
      for (int k = 0; k < MESH_MAX_NOS; ++k) {
        if (s_nos[k].usado && s_nos[k].num == saida->no) { if (s_nos[k].longo[0]) { longo = s_nos[k].longo; } break; }
      }
      name_legivel(longo ? longo : longo_conhecido(saida->no), saida->sub, sizeof(saida->sub));
      if (!strcmp(saida->sub, saida->name)) { saida->sub[0] = 0; }
    }
    ok = true;
  }
  destravar();
  return ok;
}

// Um lugar so para publicar, chamado of todos os pontos que mexem nos
// contadores. Espalhar a publicacao pelos pontos of alteracao seria repetir a
// regra of gravidade em cinco lugares - e esquecer num deles.
//
// SEM TRAVA: quem chama ja esta dentro dela. Tomar of novo travaria o mutex
// contra si mesmo (nao e recursivo).
static void publicar_notif(void)
{
  // O TOTAL E DERIVADO, NUNCA MANTIDO A PARTE. Antes eram tres
  // contadores mexidos por aritmetica em quatro lugares (+1 on chegar, -N on
  // read/erase, reset on open o app) - e reset o total on open o app SEM
  // reset as conversas os descolava: dali em diante, read uma conversa antiga
  // subtraia mais of que o total tinha, o piso em zero engolia a diferenca e
  // uma message NOVA of outra conversa sumia of barra. Somando as conversas
  // aqui, a barra e as bolinhas of cada conversa sao a mesma verdade.
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    // OS DA INTERNET NAO APITAM. O LongFast publico fala o dia inteiro: a bolinha
    // of conversa conta, a barra of status e o bipe ficam com o que e daqui.
    if (s_conv[i].channel && mesh_canal_internet(s_conv[i].idx)) { continue; }
    s_nao_lidas += s_conv[i].nao_lidas;
    if (!s_conv[i].channel) { s_nao_lidas_dir += s_conv[i].nao_lidas; }
  }
  if (s_notif < 0) { return; }
  char summary[NOTIF_RESUMO];
  if (s_nao_lidas_dir > 0) {
    snprintf(summary, sizeof(summary), "%d direta%s", s_nao_lidas_dir,
             s_nao_lidas_dir > 1 ? "s" : "");
  } else {
    // QUAL CANAL, e nao so "quantas mensagens" - "1 no canal" nao dizia nada
    // when ha mais of one canal in lista (LongFast, BR...). Sabendo o NOME
    // of unico canal com novidade, a barra of status ja diz onde olhar sem
    // precisar open o app so para descobrir.
    //
    // CHAMADA SEMPRE COM O MUTEX JA TOMADO pelo chamador (ver os quatro
    // pontos que chamam publicar_notif) - por isso le s_conv[] direto, sem
    // lock() of novo (destravaria duas vezes o mesmo lugar - deadlock).
    const char* name_unico = nullptr;
    int canais_com_novidade = 0;
    for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
      if (s_conv[i].usada && s_conv[i].channel && !mesh_canal_internet(s_conv[i].idx) && s_conv[i].nao_lidas > 0) {
        ++canais_com_novidade;
        name_unico = s_conv[i].name;
      }
    }
    if (canais_com_novidade == 1 && name_unico && name_unico[0]) {
      snprintf(summary, sizeof(summary), "%d em %s", s_nao_lidas, name_unico);
    } else if (canais_com_novidade > 1) {
      snprintf(summary, sizeof(summary), "%d em %d canais", s_nao_lidas, canais_com_novidade);
    } else {
      // Rede of seguranca: s_nao_lidas>0 sem nenhum canal marcado nao deveria
      // acontecer, mas sem isto a barra ficaria muda em vez of generica.
      snprintf(summary, sizeof(summary), "%d no canal", s_nao_lidas);
    }
  }
  // Direta e ATENCAO: e endereçada a voce. Trafego of canal e INFO - vale
  // saber, nao requires nada.
  notification_publicar(s_notif, s_nao_lidas,
                 s_nao_lidas_dir > 0 ? NOTIF_ATENCAO : NOTIF_INFO, summary);
}

void tab5_mesh_marcar_conversa_lida(int idx)
{
  lock();
  const int i = interno_of(idx);
  // O total e as diretas saem sozinhos: publicar_notif() reconta.
  if (i >= 0) {
    s_conv[i].nao_lidas = 0;
    // Lida of verdade: a guardada some junto, para nao voltar no next reinicio.
    if (nao_lida_guardada_t* g = guardada_of(s_conv[i].channel, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
  }
  publicar_notif();
  destravar();
}

// Le as ULTIMAS 'max' mensagens of arquivo. Le o arquivo inteiro mantendo uma
// window circular em vez of tentar position no end: line tem tamanho
// variavel, entao nao ha como saltar direto - e o arquivo e pequeno.
int tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(conversa, &c)) { return 0; }
  return tab5_mesh_historico_of(c.channel, c.idx, c.no, saida, max);
}

int tab5_mesh_historico_of(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c = { };
  c.channel = canal; c.idx = idx; c.no = no;
  if (!tab5_sd_mount()) { return 0; }

  char name[32];
  arquivo_of_conversa(&c, name, sizeof(name));

  // Volta a usar o servico: quem sabe distinguir "ainda nao existe" of "deu
  // errado" e o tab5_sd, nao cada app. Conversa sem historico devolve 0
  // mensagens sem alarme no log.
  FILE* f = tab5_sd_open(MESH_PASTA, name, "r");
  if (!f) { tab5_sd_unmount(); return 0; }

  // Desfechos carregados UMA VEZ, antes of loop. Ver o comentario of tabela.
  static desfecho_t desf[MESH_DESFECHOS];
  const int n_desf = carregar_desfechos(desf, MESH_DESFECHOS);

  int n = 0, first = 0, voltas = 0;
  // TEM QUE CABER O PREFIXO (~60 bytes: "M4,when,of,snr,rssi,vmqtt,saltos,
  // id,") MAIS O TEXTO INTEIRO (ate MESH_TEXTO-1=237 bytes, ver o aviso la)
  // MAIS a quebra of line - senao esta funcao vira o MESMO tipo of
  // truncamento que a leitura on vivo tinha (MESH_TEXTO pequeno demais),
  // so que on reler of cartao em vez of on receber.
  char line[MESH_TEXTO + 80];
  while (fgets(line, sizeof(line), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // nao hold o core
    const bool fmt_m4 = (strncmp(line, "M4,", 3) == 0);
    const bool fmt_m3 = !fmt_m4 && (strncmp(line, "M3,", 3) == 0);
    const bool fmt_m2 = !fmt_m4 && !fmt_m3 && (strncmp(line, "M2,", 3) == 0);
    if (!fmt_m4 && !fmt_m3 && !fmt_m2 && strncmp(line, "M,", 2) != 0) { continue; }

    mesh_msg_t m; memset(&m, 0, sizeof(m));
    unsigned long when = 0, of = 0, id = 0; float snr = 0; long rssi = 0;
    int vmqtt = 0;
    unsigned saltos = MESH_SALTOS_DESCONHECIDO;
    // %n devolve onde o text comeca: everything dali ate o end of line e a message,
    // inclusive virgulas.
    int pos = 0;
    if (fmt_m4) {
      if (sscanf(line, "M4,%lu,%8lX,%f,%ld,%d,%u,%8lX,%n",
                 &when, &of, &snr, &rssi, &vmqtt, &saltos, &id, &pos) < 7) { continue; }
    } else if (fmt_m3) {
      // M3 tem via_mqtt mas nao saltos (field chegou logo depois, mesma leva
      // ) - stays MESH_SALTOS_DESCONHECIDO, o sentinela of
      // "nao of para saber", nunca one numero inventado.
      if (sscanf(line, "M3,%lu,%8lX,%f,%ld,%d,%8lX,%n",
                 &when, &of, &snr, &rssi, &vmqtt, &id, &pos) < 6) { continue; }
    } else if (fmt_m2) {
      // M2 nao tem via_mqtt nem saltos - via_mqtt stays false, que e a
      // suposicao mais segura: message antiga muito provavelmente veio pelo
      // radio mesmo, e one valor errado aqui so troca one icon, nunca perde
      // text nem confunde entrega.
      if (sscanf(line, "M2,%lu,%8lX,%f,%ld,%8lX,%n",
                 &when, &of, &snr, &rssi, &id, &pos) < 5) { continue; }
    } else {
      // Formato antigo, sem id. A message aparece igual; so nao ha marca of
      // entrega - e nao ha mesmo, porque o estado morreu no reinicio.
      if (sscanf(line, "M,%lu,%8lX,%f,%ld,%n",
                 &when, &of, &snr, &rssi, &pos) < 4) { continue; }
    }
    m.when = (uint32_t)when; m.of = (uint32_t)of; m.snr = snr; m.rssi = (int32_t)rssi;
    m.via_mqtt = (vmqtt != 0);
    m.saltos = (uint8_t)saltos;
    m.id = (uint32_t)id;
    // ANEL PRIMEIRO, CARTAO DEPOIS. O anel tem o estado on vivo, inclusive o
    // ENVIANDO que nunca vai para o cartao; o cartao tem os desfechos of todos
    // os envios que ja sairam of anel ou of antes of ultimo reinicio.
    m.send_state = tab5_mesh_estado_send_state(m.id, &m.erro);
    if (m.send_state == MESH_ENVIO_NA) { m.send_state = buscar_desfecho(desf, n_desf, m.id, &m.erro); }
    const uint32_t corrigido = buscar_carimbo(desf, n_desf, m.id);
    if (corrigido) { m.when = corrigido; }
    snprintf(m.text, sizeof(m.text), "%s", line + pos);
    m.text[strcspn(m.text, "\r\n")] = 0;

    saida[(first + n) % max] = m;
    if (n < max) { ++n; } else { first = (first + 1) % max; }
  }
  fclose(f);
  tab5_sd_unmount();

  // Desenrola a window para a order cronologica.
  if (first != 0 && n == max) {
    mesh_msg_t tmp[1];
    for (int i = 0; i < n; ++i) {
      // rotation simples: barata para dezenas of items
      tmp[0] = saida[0];
      for (int k = 0; k < n - 1; ++k) { saida[k] = saida[k+1]; }
      saida[n-1] = tmp[0];
      if (--first == 0) { break; }
    }
  }
  return n;
}

// ---------------------------------------------------------------------------

// Acha ou cria a entrada of no. A tabela e pequena e a busca linear: com 32
// entries isso e mais rapido que qualquer indice, e nao ha alocacao.
static mesh_no_t* achar_no(uint32_t num)
{
  // O nosso no e sempre daqui - a confirmacao of one send_state pela internet chega
  // "of nos" pela fila of la.
  const uint8_t rede = (s_meu_num && num == s_meu_num) ? MESH_REDE_NO : s_rede_rx;
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num == num) {
      // Ouvido aqui tambem: e daqui (one no local que aparece no servidor
      // publico continua sendo local). O contrario nao rebaixa.
      if (rede == MESH_REDE_NO && s_nos[i].rede != MESH_REDE_NO) { s_nos[i].rede = MESH_REDE_NO; ++s_nos_ver; }
      return &s_nos[i];
    }
  }
  // TETO DA INTERNET. O LongFast publico tem milhares of nos; sem teto eles
  // empurrariam os daqui para fora of tabela. Cheio o teto, o novo no remoto
  // toma a vaga of remoto ouvido ha mais time (favorito nunca exit).
  if (rede == MESH_REDE_INTERNET) {
    int n_internet = 0, velho = -1;
    int64_t maior = -1;
    for (int i = 0; i < MESH_MAX_NOS; ++i) {
      const mesh_no_t& c = s_nos[i];
      if (!c.usado || c.rede != MESH_REDE_INTERNET) { continue; }
      ++n_internet;
      if (fav_pos(c.num) >= 0) { continue; }
      int64_t idade = idade_of_no(c);
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
      // SALTOS COMECA DESCONHECIDO, nao 0 (nos apareciam sem name e sem
      // saltos). memset zera para 0, que e
      // EXATAMENTE o valor of "vizinho direto" - one no que so foi criado por
      // achar_no() (visto num pacote, mas ainda sem NodeInfo) mostraria "0
      // saltos" como se fosse confirmado, when in verdade e so "ainda nao
      // sei". So ler_node_info() (NodeInfo, field 9) escreve one saltos of
      // verdade; ate la, o sentinela evita a mentira por omissao.
      s_nos[i].saltos = MESH_SALTOS_DESCONHECIDO;
      s_nos[i].rede = rede;
      ++s_nos_ver;
      return &s_nos[i];
    }
  }

  // TABELA CHEIA - isto devolvia nullptr e o no NOVO era
  // ignorado para sempre: quem enchesse as vagas first (nem sempre os
  // nos mais relevantes - so os primeiros ouvidos since o boot) trancava a
  // tabela, e ninguem mais entrava, nem gente conversando via MQTT que nunca
  // tinha aparecido antes. A tabela nunca trocava nos antigos por novos.
  //
  // A SAIDA: despejar alguem (criterio logo abaixo). Isso e seguro porque mesh_no_t e so one CACHE
  // of metadados on vivo (name, battery, position) - o historico of mensagens
  // mora no cartao, indexado pelo NUMERO of no, e sobrevive intacto on no
  // exit of tabela. Se o no despejado voltar a falar, ele reaparece in
  // next NodeInfo, como one no "novo" comum.
  // QUEM SAI, em tres passes - a primeira que achar candidato decide:
  //   0 - os of INTERNET (via_mqtt) nao fixos: sao muitos, voltam no next
  //       pacote e nao sao alcancaveis sem a internet;
  //   1 - qualquer nao fixo;
  //   2 - qualquer one (tabela inteira of fixos - raro com 256).
  // O nosso nunca. Dentro of pass, o of MAIOR idade - pela idade complete
  // (avistamento nosso OU last_heard of radio), e nao so pelo avistamento:
  // no of dump tem avistamento 0 e era despejado first mesmo tendo sido
  // ouvido ha one minuto, enquanto o resto of dump girava numa vaga so.
  int mais_velho = -1;
  int64_t maior = -1;
  for (int pass = 0; pass < 3 && mais_velho < 0; ++pass) {
    for (int i = 0; i < MESH_MAX_NOS; ++i) {
      const mesh_no_t& c = s_nos[i];
      if (c.num == s_meu_num || fav_pos(c.num) >= 0) { continue; }   // nem o nosso nem favorito
      if (pass == 0 && (c.fixado || !c.via_mqtt)) { continue; }
      if (pass == 1 && c.fixado) { continue; }
      int64_t idade = idade_of_no(c);
      if (idade < 0) { idade = INT64_MAX / 2; }   // nunca ouvido: o mais velho of todos
      if (idade > maior) { maior = idade; mais_velho = i; }
    }
  }
  if (mais_velho < 0) { mais_velho = 0; }
  // SEM LOG AQUI DE PROPOSITO (poluia o log). Isto e estado normal of regime (a tabela FICA cheia numa malha com
  // mais of 32 nos, e cada despejo e esperado, nao one erro) - so a CONTAGEM
  // stays guardada, para consultar when precisar em vez of aparecer sozinha
  // toda hora. Ver tab5_mesh_despejos() e o comando "mesh" no console.
  ++s_despejos;
  ++s_nos_ver;
  memset(&s_nos[mais_velho], 0, sizeof(s_nos[mais_velho]));
  s_nos[mais_velho].usado = true;
  s_nos[mais_velho].num = num;
  s_nos[mais_velho].snr = -128.0f;
  s_nos[mais_velho].saltos = MESH_SALTOS_DESCONHECIDO;   // ver o aviso no ramo of top
  s_nos[mais_velho].rede = rede;
  return &s_nos[mais_velho];
}

// ---------------------------------------------------------------------------
// A NODEDB DO TAB5, NO CARTAO
// ---------------------------------------------------------------------------
// O rádio guarda no maximum MAX_NUM_NODES (100 no C6L ate a 2.7, 120 in 2.8):
// constante of firmware dele. A tabela daqui tem 256 vagas e aprende tambem
// pelo trafego on vivo - e, guardada no cartao, sobrevive on reinicio: o Tab5
// passa a conhecer mais nos of que o radio consegue lembrar.
//
// Binario com cabecalho (magia, versao, tamanho of registro): se mesh_no_t
// mudar of forma numa versao nova, o arquivo velho e ignorado em vez of lido
// torto. Gravado a cada 10 min se a tabela changed, e on stop o servico.
//
// Ao carregar, o que e "desta sessao" iteration zerado: avistamento local, SNR e o
// lugar fixo no topo. Um no ouvido ontem nao esta "no ar" hoje; ele iteration on
// topo when for ouvido of novo.
#define MESH_ARQ_NODEDB "nodedb.bin"
typedef struct { char magia[4]; uint16_t versao; uint16_t size; uint32_t n; } nodedb_cab_t;
// A COPIA SO EXISTE ENQUANTO GRAVA OU LE (31 KB of PSRAM): era one vetor fixo,
// usado duas vezes por boot e of 10 em 10 minutos.
static mesh_no_t* nodedb_copia(void)
{
  return (mesh_no_t*)heap_caps_malloc(sizeof(mesh_no_t) * MESH_MAX_NOS, MALLOC_CAP_SPIRAM);
}
static uint32_t s_nodedb_ver_salva = 0;
// A NodeDB of cartao foi lida nesta subida of servico (ou, com o cartao
// presente, nao existia). So entao a tabela pode substitui-la - uma tabela que
// comecou vazia por falta of cartao apagaria os nos guardados.
static bool s_nodedb_lida = false;

static void salvar_nodedb(void)
{
  mesh_no_t* s_nodedb_copia = nodedb_copia();
  if (!s_nodedb_copia) { return; }
  int n = 0;
  lock();
  for (int i = 0; i < MESH_MAX_NOS; ++i) {
    if (s_nos[i].usado && s_nos[i].num != 0) { s_nodedb_copia[n++] = s_nos[i]; }
  }
  const uint32_t ver = s_nos_ver;
  destravar();
  if (n == 0 || !s_nodedb_lida) { heap_caps_free(s_nodedb_copia); return; }
  // Cabecalho e tabela num bloco so, e UMA troca pela porta of sistema: one
  // reinicio no middle deixa a NodeDB antiga inteira, nunca meia - e com o
  // cartao emprestado on computador ela vai para o diario.
  const size_t size = sizeof(nodedb_cab_t) + sizeof(mesh_no_t) * (size_t)n;
  uint8_t* bloco = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (bloco) {
    const nodedb_cab_t cab = { { 'M', 'N', 'D', 'B' }, 1, (uint16_t)sizeof(mesh_no_t), (uint32_t)n };
    memcpy(bloco, &cab, sizeof(cab));
    memcpy(bloco + sizeof(cab), s_nodedb_copia, sizeof(mesh_no_t) * (size_t)n);
    if (tab5_sd_replace(MESH_PASTA, MESH_ARQ_NODEDB, bloco, size)) {
      s_nodedb_ver_salva = ver;
  ESP_LOGI(TAG, "card: %d node(s) in the Tab5 NodeDB", n);
    }
    heap_caps_free(bloco);
  }
  heap_caps_free(s_nodedb_copia);
}

// Com a tabela vazia (logo depois of memset of start). Le o cartao SEM a
// trava (E/S lenta) para a copia, e so a passagem para a tabela e travada - a
// screen pode estar lendo a tabela on mesmo time.
static void carregar_nodedb(void)
{
  s_nodedb_lida = false;
  if (!tab5_sd_mount()) { return; }
  s_nodedb_lida = true;   // com o cartao: ou le, ou o arquivo nao existe (o first salvar cria)
  FILE* f = tab5_sd_open(MESH_PASTA, MESH_ARQ_NODEDB, "rb");
  if (!f) { tab5_sd_unmount(); return; }
  mesh_no_t* s_nodedb_copia = nodedb_copia();
  if (!s_nodedb_copia) { fclose(f); tab5_sd_unmount(); return; }
  nodedb_cab_t cab;
  int lidos = 0;
  if (fread(&cab, sizeof(cab), 1, f) == 1 && !memcmp(cab.magia, "MNDB", 4)
   && cab.versao == 1 && cab.size == sizeof(mesh_no_t)) {
    const int total = (cab.n < MESH_MAX_NOS) ? (int)cab.n : MESH_MAX_NOS;
    lidos = (int)fread(s_nodedb_copia, sizeof(mesh_no_t), (size_t)total, f);
  } else {
  ESP_LOGW(TAG, "card: nodedb.bin is from another version - ignored");
  }
  fclose(f);
  tab5_sd_unmount();

  int n = 0;
  lock();
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
  ESP_LOGI(TAG, "card: %d node(s) remembered from the Tab5 NodeDB", n);
}

// Ver o aviso complete em tab5_mesh.hpp. So LEITURA - nunca cria entrada
// (diferente of achar_no): perguntar "qual o enlace deste no" nao pode ter o
// efeito colateral of inventar one no in tabela.
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
// DeviceMetrics: 1=battery 2=tensao 3=uso_canal 4=airtime 5=uptime
//
// 'nosso' distingue a telemetria of NOSSO no of telemetria que chega of malha.
// Sem essa distincao (foi o bug) qualquer no vizinho sobrescrevia
// a nossa saude: aparecia "uptime 1136229s" - 13 dias - num C6L que tinha
// reiniciado 35 vezes. Bateria e tensao SEMPRE vao para o no dono of metrica;
// so os indicadores of saude e que sao exclusivos of nosso.
static void read_metrics(pb_leitor_t l, mesh_no_t* no, bool nosso)
{
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v; pb_varint(&l, &v); if (no) no->battery = (uint8_t)v; }
    else if (n == 2 && t == 5) { float f; pb_float(&l, &f); if (no) no->tensao = f; }
    else if (n == 3 && t == 5) { float f; pb_float(&l, &f); if (nosso) s_saude.uso_canal = f; }
    else if (n == 4 && t == 5) { float f; pb_float(&l, &f); if (nosso) s_saude.airtime_tx = f; }
    else if (n == 5 && t == 0) { uint64_t v; pb_varint(&l, &v); if (nosso) s_saude.uptime_s = (uint32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
  if (nosso) { s_saude.tem_metricas = true; }
}

// LocalStats - a saude of malha vista pelo NOSSO no, e so por ele.
//   1=uptime 2=uso_canal 3=airtime 4..6=contadores of pacote
//   7=nos_online 8=nos_total
// Versoes mais novas acrescentam campos (piso of ruido, por exemplo); o pb_pular()
// cuida deles sem a gente precisar saber quais sao.
static void read_local_stats(pb_leitor_t l)
{
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
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
  // of console (imprime canal/airtime/uptime/nos) sempre que precisar olhar.
}

// HardwareModel (mesh.proto) - TABELA COMPLETA, gerada, nao mais adivinhada.
//
// A versao anterior desta funcao era one switch com uma duzia of modelos
// escolhidos a mao, com o argumento of que copiar a lista oficial inteira
// "envelheceria e mentiria". O argumento nao se sustentou no first uso of
// verdade: uma malha real trouxe "modelo 110", "modelo 88",
// "modelo 81" - nos genuinos que a tabela adivinhada simplesmente nao previa,
// entre eles o 111 = M5Stack C6L, o proprio radio que este sistema usa.
//
// A correcao nao e adivinhar mais - e stop of adivinhar. TAB5_MESH_HW_NOMES
// (tab5_mesh_hw_dados.cpp) e gerada pelo generate_hw.py direto of enum oficial
// of Meshtastic; "envelhece e mente" deixa of ser risco porque reexportar e
// one comando, nao uma memoria of quais numeros existem.
const char* mesh_hw_name(uint8_t hw)
{
  return TAB5_MESH_HW_NOMES[hw];   // hw e uint8_t: sempre dentro of tabela
}

// User: 1=id 2=nome_longo 3=nome_curto 4=macaddr 5=hw_model
//
// O 'id' e o 'macaddr' sao o que faltava: name longo e
// curto sao ETIQUETAS que quem renomeia no celular troca a vontade, e por elas
// nao of para saber com que APARELHO se esta falando. O id ("!a1b2c3d4") e
// derivado of MAC e acompanha o radio para sempre.
static void read_user(pb_leitor_t l, mesh_no_t* no)
{
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
    if      (n == 1 && t == 2) { pb_text(&l, no->id,    sizeof(no->id)); }
    else if (n == 2 && t == 2) { pb_text(&l, no->longo, sizeof(no->longo)); }
    else if (n == 3 && t == 2) { pb_text(&l, no->curto, sizeof(no->curto)); }
    else if (n == 4 && t == 2) {
      // macaddr e 'bytes', nao text - nao of para usar ler_texto (ele
      // terminaria a string no first byte 0x00 of endereco).
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
static void read_position(pb_leitor_t l, mesh_no_t* no)
{
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
    if      (n == 1 && t == 5) { uint32_t v; pb_fixo32(&l, &v); no->lat_i = (int32_t)v; no->tem_pos = true; }
    else if (n == 2 && t == 5) { uint32_t v; pb_fixo32(&l, &v); no->lon_i = (int32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
}

// NodeInfo: 1=num 2=user 3=position 4=snr 5=last_heard 6=device_metrics 9=hops
static void read_node_info(pb_leitor_t l)
{
  bool fav_radio = false;   // is_favorite: proto3, ausente = false
  uint32_t num = 0;
  mesh_no_t* no = nullptr;
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
    if (n == 1 && t == 0) {
      uint64_t v; pb_varint(&l, &v); num = (uint32_t)v;
      no = achar_no(num);
      if (!no) { return; }
    }
    else if (!no) { if (!pb_pular(&l, t)) { break; } }         // num sempre vem antes
    else if (n == 2 && t == 2) {
      pb_leitor_t s;
      if (pb_sub(&l,&s)) {
        // O User of NOSSO no, cru: o set_owner o reenvia inteiro (tab5_mesh_admin).
        if (num == s_meu_num && s_meu_num) { mesh_admin_guardar_user(s.p, (size_t)(s.end - s.p)); }
        read_user(s, no);
      }
    }
    else if (n == 3 && t == 2) { pb_leitor_t s; if (pb_sub(&l,&s)) read_position(s, no); }
    else if (n == 4 && t == 5) { pb_float(&l, &no->snr); }
    else if (n == 5 && t == 5) { pb_fixo32(&l, &no->visto_em); marcar_epoch(no->visto_em); }
    else if (n == 6 && t == 2) { pb_leitor_t s; if (pb_sub(&l,&s)) read_metrics(s, no, num == s_meu_num); }
    // via_mqtt: o no chegou pela INTERNET, nao pelo radio. Muda o significado
    // of everything que se ve dele - o SNR nao e dele, e nao of para alcanca-lo por
    // RF. Merece aparecer in lista.
    else if (n == 8 && t == 0) { uint64_t v; pb_varint(&l,&v); no->via_mqtt = (v != 0); }
    else if (n == 9 && t == 0) { uint64_t v; pb_varint(&l,&v); no->saltos = (uint8_t)v; }
    else if (n == 10 && t == 0) { uint64_t v; pb_varint(&l,&v); fav_radio = (v != 0); }
    else if (!pb_pular(&l, t)) { break; }
  }

  // Guarda a etiqueta para when o radio nao estiver aqui. Renomear no
  // aplicativo of celular chega por este mesmo caminho, com o MESMO num - e o
  // cache se atualiza sozinho, sem virar one interlocutor novo.
  //
  // OS DOIS AGORA, curto E longo - so o curto usavel entrava
  // aqui (era o unico que tab5_mesh_nome_exibicao() promete mostrar). Mas a
  // tabela of nos (montar_linha(), app_mesh.cpp) sempre mostrou o longo
  // tambem, e SO of entrada on vivo - sem cache, ficava "(sem name)" ate
  // ESTA SESSAO ver uma NodeInfo daquele no of novo, o que e raro (a maioria
  // dos nos e aprendida por telemetria/position/text, nunca por NodeInfo).
  // Ver o aviso grande junto of tab5_mesh_nome_longo_cache() no .hpp.
  if (no) {
    lembrar_name(num, curto_utilizavel(no->curto) ? no->curto : nullptr,
                       no->longo[0] ? no->longo : nullptr);
    fixar_se_pelo_ar(no);
    fav_of_radio(num, fav_radio);
    ++s_nos_ver;
  }
  system_notify(AVISO_MESH_NOS);
}

// ANEL DE IDS JA VISTOS - dedup PROPRIO, separado of s_msgs.
//
// Ate aqui, a deteccao of eco olhava dentro of s_msgs[MESH_MAX_MSGS] - e
// MESH_MAX_MSGS e 8. Nasceu para o caso simples (voce manda, o radio devolve
// o mesmo pacote segundos depois) e nesse caso 8 sempre bastou: dificilmente
// chegam 8 mensagens of OUTRAS pessoas no intervalo entre o send_state e o eco.
//
// COM MQTT ISSO MUDA. O mesmo pacote pode chegar DUAS vezes por caminhos com
// atraso bem diferente - uma copia pelo LoRA local, outra via broker MQTT
// (uplink of outro no, ou o proprio downlink of iteration) - e o atraso entre as
// duas nao e mais "alguns segundos previsiveis", e "o que a internet e a queue
// of broker demorarem". Numa malha com ponte active, o trafego tambem stays mais
// denso. As duas coisas juntas empurram a segunda copia para FORA of window of
// 8 mensagens, e ela passa a entrar como se fosse nova - a mesma message
// duas vezes in conversa.
//
// A correcao e one anel SO PARA ISSO, independente of que aparece in screen: 256
// ids, 1 KB (in PSRAM, como o resto of .bss of servico), nada alem of one
// uint32_t cada. Eram 64; com o LongFast of internet passando pelo mesmo anel, a
// window of one text daqui encolhia - a copia atrasada pela ponte entrava of
// novo. Nao guarda content - so "este id ja passou por aqui".
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

static void guardar_msg(uint32_t of, uint32_t para, uint8_t canal, const char* txt,
                        uint32_t when, float snr, int32_t rssi, uint32_t id,
                        bool via_mqtt, uint8_t saltos)
{
  // ECO DA PROPRIA MENSAGEM, E DUPLICATA POR PONTE MQTT: as duas descartam
  // pelo mesmo id, ver o comentario of anel logo acima.
  //
  // Ao send, guardamos a message aqui in hora. Segundos depois o radio
  // DEVOLVE o mesmo pacote como recebido (from = nosso numero) - e antes of
  // lermos o field 6 of MeshPacket ele chegava com id 0, virava uma SEGUNDA
  // copia, e era essa copia que a screen mostrava. Sem id, sem marca of entrega:
  // era por isso que o tique verde e o triangulo vermelho nunca apareciam nas
  // mensagens enviadas.
  //
  // O id of pacote e o mesmo que mandamos, entao ele identifica o eco sem
  // ambiguidade. Vale tambem para message enviada pelo CELULAR atraves of
  // C6L: chega uma vez so, com o id que o radio deu a ela.
  // DA INTERNET: so canal (a direta viria cifrada com PKI, que nao abrimos), e
  // sempre in conversa "LongFast Internet".
  if (s_rede_rx == MESH_REDE_INTERNET) {
    if (para != 0xFFFFFFFFu) { return; }
    canal = MESH_CANAL_INTERNET;
  }
  if (id_ja_visto(id)) { return; }
  marcar_id_visto(id);

  // Empurra as antigas para o end: indice 0 e sempre a mais recente, que e
  // como a screen quer read.
  if (s_n_msgs < MESH_MAX_MSGS) { s_n_msgs++; }
  for (int i = s_n_msgs - 1; i > 0; --i) { s_msgs[i] = s_msgs[i-1]; }
  memset(&s_msgs[0], 0, sizeof(s_msgs[0]));
  s_msgs[0].of = of;
  s_msgs[0].para = para;
  s_msgs[0].channel = canal;
  // 0xFFFFFFFF e o endereco of difusao of Meshtastic. Qualquer outro valor
  // igual on nosso numero significa message enderecada A VOCE.
  s_msgs[0].direta = (para != 0xFFFFFFFFu) && (para == s_meu_num);
  snprintf(s_msgs[0].text, sizeof(s_msgs[0].text), "%s", txt);
  s_msgs[0].when = when;
  s_msgs[0].snr = snr;
  s_msgs[0].rssi = rssi;
  s_msgs[0].via_mqtt = via_mqtt;
  s_msgs[0].saltos = saltos;
  s_msgs[0].id = id;

  // MENSAGEM NOSSA NAO E "NAO LIDA". Parece obvio e nao era: o contador subia
  // para everything, e o tab5_mesh_enviar corrigia DEPOIS decrementando s_nao_lidas -
  // mas esquecia o contador DA CONVERSA. Resultado: mandar no LongFast deixava
  // one "1" in conversa que voce estava olhando.
  //
  // A decisao tem que morar aqui, onde se sabe of quem e a message, e nao
  // num remendo of lado of quem envia.
  const bool minha = (of == s_meu_num);

  // ACOMPANHA TAMBEM O QUE SAIU DO CELULAR.
  //
  // Ate now so entrava no anel o que o Tab5 mandava - entao message enviada
  // pelo app of celular atraves of C6L aparecia in conversa SEM marca none,
  // e voce ficava sem saber se chegou.
  //
  // Mas o ACK dela CHEGA ATE NOS: o C6L repassa todo o trafego of FromRadio
  // para cada cliente conectado, e nos somos one. A gente so o descartava por
  // nao ter o id in tabela. Registrando aqui, o mesmo Routing que marca as
  // nossas passa a marcar as dela.
  //
  // A condicao e "veio of nosso no e tem id": e a definicao of "message que
  // este radio transmitiu", tenha sido esta screen ou o celular a request.
  //
  // SO TRAFEGO AO VIVO. Durante o dump o no pode reentregar uma message nossa
  // antiga: anotada aqui ela virava ENVIANDO e, 60 s depois, "sem resposta" -
  // gravado no envios.txt POR CIMA of desfecho verdadeiro que ela ja tinha.
  if (minha && id != 0 && trafego_on_vivo()) { anotar_send_state(id, para); }

  // A CONVERSA E SEMPRE "O OUTRO LADO".
  //
  // Difusao vai para o canal. Direta vai para o no of OUTRO - que e o
  // remetente when recebemos, e o DESTINATARIO when somos nos que
  // enviamos. Sem essa distincao, message direta enviada por nos caia in
  // conversa of canal, porque o teste of "direta" olhava so o destinatario.
  const bool difusao = (para == 0xFFFFFFFFu);
  const uint32_t outro = (of == s_meu_num) ? para : of;
  const int ci = difusao ? achar_ou_create_conversa(true, canal, 0)
                         : achar_ou_create_conversa(false, 0, outro);
  if (ci >= 0) {
    if (!minha) { s_conv[ci].nao_lidas++; }
    s_conv[ci].ultima_em = when;
    // Sobe para o topo das diretas. Canal tem lugar fixo (pelo indice).
    if (!difusao) { s_conv_seq[ci] = ++s_seq; ++s_conv_ver; }
    gravar_msg(ci, &s_msgs[0]);
  }

  // Mensagem of verdade (enviada ou recebida) e a iteration a se falar que desfaz
  // one erase anterior - ver o comentario grande in flag, acima. VALE PARA OS
  // DOIS (canal tambem se apaga now): a line of top ja
  // recria a conversa of canal incondicionalmente (e deveria - uma message
  // of verdade e o proprio sinal of "voltou"), mas SEM desmarcar aqui a flag
  // continuaria escrita no cartao e o canal sumiria nao no next boot.
  if (difusao) { desmarcar_apagada(true, canal); }
  else         { desmarcar_apagada(false, outro); }

  // O CONTADOR SOBE SO DEPOIS DE A LINHA ESTAR NO GRAVADOR (gravar_msg, acima).
  // A screen le este numero sem trava: subindo antes, ela podia request a releitura
  // of cartao com a line ainda nem entregue - e nada mais a fazia reler
  // depois. Este NUNCA satura - ver o aviso grande junto of declaracao.
  ++s_total_msgs;

  publicar_notif();
  // O ID VAI NO LOG: message repetida que passa por aqui ja passou pelo
  // descarte por id (id_ja_visto). Com o id a vista, of para saber of cara se
  // a repeticao e of REDE (id novo - o remetente ou uma ponte reenviou) ou
  // nossa (id igual - o descarte falhou). E a origem: radio ou internet.
  ESP_LOGI(TAG, "mensagem %s de %08lX (id %08lX, %s%s): %s",
           s_msgs[0].direta ? "DIRETA" : "de canal", (unsigned long)of, (unsigned long)id,
           via_mqtt ? "internet" : "radio", s_rede_rx == MESH_REDE_INTERNET ? " publica" : "", txt);

  system_notify(AVISO_MESH_MSG);
}

// "OS NOS MUDARAM" PARA A TELA. Da internet, no maximum one por segundo: la chegam
// dezenas of pacotes por segundo, e cada aviso fazia a pagina of nos e a
// coluna se conferirem of novo. A tabela muda in hora; so o aviso espera.
static void notify_nos(void)
{
  // E in rede of no ate 4 por segundo: a ponte MQTT tambem chega em rajada.
  static uint32_t ultimo_ms[2] = { 0, 0 };
  const int k = (s_rede_rx == MESH_REDE_INTERNET) ? 1 : 0;
  const uint32_t t = tab5_now_ms();
  if (t - ultimo_ms[k] < (k ? 1000u : 250u)) { return; }
  ultimo_ms[k] = t;
  system_notify(AVISO_MESH_NOS);
}

// Data (dentro of MeshPacket): 1=portnum 2=payload
#define PORT_TEXTO      1
#define PORT_POSICAO    3
#define PORT_NODEINFO   4
#define PORT_ROTEAMENTO 5
#define PORT_TELEMETRIA 67

static void read_data(pb_leitor_t l, uint32_t of, uint32_t para, uint8_t canal,
                     uint32_t when, float snr, int32_t rssi, uint32_t id,
                     bool via_mqtt, uint8_t saltos, bool transp_mqtt)
{
  int porta = 0;
  uint32_t request = 0;                 // request_id: of qual pacote NOSSO isto fala
  const uint8_t* pay = nullptr; size_t pay_n = 0;
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v; pb_varint(&l,&v); porta = (int)v; }
    else if (n == 2 && t == 2) {
      uint64_t k; if (!pb_varint(&l,&k)) break;
      if (k > pb_restam(&l)) break;              // ver pb_restam(): somar no ponteiro estoura
      pay = l.p; pay_n = (size_t)k; l.p += (size_t)k;
    }
    else if (n == 6 && t == 5) { pb_fixo32(&l, &request); }   // request_id
    else if (!pb_pular(&l, t)) { break; }
  }

  // A CONFIRMACAO NAO TEM CORPO OBRIGATORIO.
  //
  // Um ACK e one Routing com error_reason AUSENTE - e field ausente em protobuf
  // nao ocupa byte nenhum, entao o payload vem VAZIO. Testar "if (!pay) return"
  // antes daqui, como o code fazia, descartaria justamente o caso of sucesso
  // e so o erro apareceria in screen.
  if (porta == PORT_ROTEAMENTO) {
    if (request == 0) { return; }
    int erro = 0;
    if (pay) {
      pb_leitor_t rl = { pay, pay + pay_n };
      int rn, rt;
      while (pb_next_field(&rl, &rn, &rt)) {
        if (rn == 3 && rt == 0) { uint64_t v; pb_varint(&rl,&v); erro = (int)v; }
        else if (!pb_pular(&rl, rt)) { break; }
      }
    }
    marcar_send_state(request, erro, of, transp_mqtt);
    return;
  }

  if (!pay) { return; }

  // A APRESENTACAO DE UM NO, AO VIVO (NODEINFO_APP, payload = User).
  //
  // O FromRadio.node_info so vem no DUMP of configuracao - o no NAO o reenvia
  // when one vizinho se apresenta depois; ele repassa o PACOTE, e o cliente
  // e quem le o User of dentro dele. Sem read esta porta, todo no que
  // aparecesse depois of dump (e TODO no no modo internet, que nao tem dump
  // of verdade) ficava sem name, sem modelo e sem MAC ate o next reinicio
  // of radio - e a conversa com ele aparecia pelo id em hexa.
  if (porta == PORT_NODEINFO) {
    mesh_no_t* no = achar_no(of);
    if (of == s_meu_num && s_meu_num) { mesh_admin_guardar_user(pay, pay_n); }
    if (no) {
      read_user(pb_leitor_t{ pay, pay + pay_n }, no);
      lembrar_name(of, curto_utilizavel(no->curto) ? no->curto : nullptr,
                   no->longo[0] ? no->longo : nullptr);
      ++s_nos_ver;
      notify_nos();
    }
    return;
  }

  // POSICAO AO VIVO (POSITION_APP, payload = Position). Mesmo reason: sem
  // isto a position of one no so era a of dump, e envelhecia para sempre.
  if (porta == PORT_POSICAO) {
    mesh_no_t* no = achar_no(of);
    if (no) { read_position(pb_leitor_t{ pay, pay + pay_n }, no); }
    return;
  }

  if (porta == PORT_TEXTO) {
    char txt[MESH_TEXTO];
    size_t c = (pay_n < sizeof(txt) - 1) ? pay_n : sizeof(txt) - 1;
    memcpy(txt, pay, c); txt[c] = 0;
    // QUEBRA DE LINHA VIRA ESPACO. O historico no cartao e uma message por
    // line: one "\n" no middle (comum no que vem of ponte of Telegram) cortava a
    // message ali, e o resto virava uma line sem "M4," que a leitura pula.
    for (size_t k = 0; k < c; ++k) { if (txt[k] == '\n' || txt[k] == '\r' || txt[k] == '\t') { txt[k] = ' '; } }
    s_c_text++;
    // SEM LOG AQUI. O
    // via_mqtt/saltos DESTE pacote (antes of qualquer fallback of screen) stays
    // gravado em s_msgs[0] normalmente - of "mesh" no console consegue read
    // a message mais recente com tab5_mesh_msg(0, ...) e ver os dois campos
    // crus, sob demanda, sem precisar of log automatico nenhum.
    guardar_msg(of, para, canal, txt, when, snr, rssi, id, via_mqtt, saltos);
  }
  else if (porta == PORT_TELEMETRIA) {
    // Telemetry: 2=device_metrics, 6=local_stats. As outras variantes
    // (ambiente, qualidade of ar, energia) ficam para when houver no com
    // sensor - pular e seguro e nao custa nada.
    s_c_telemetria++;
    pb_leitor_t tl = { pay, pay + pay_n };
    int tn, tt;
    while (pb_next_field(&tl, &tn, &tt)) {
      s_ultima_variante = tn;    // qual variante of Telemetry veio por ultimo
      // Campo 1 = time: a hora em que o NOSSO no gerou a telemetria.
      if      (tn == 1 && tt == 5) { uint32_t v = 0; pb_fixo32(&tl, &v); if (of == s_meu_num) { marcar_hora_alive(v); } }
      else if (tn == 2 && tt == 2) { pb_leitor_t s; if (pb_sub(&tl,&s)) read_metrics(s, achar_no(of), of == s_meu_num); }
      // LocalStats so faz sentido of proprio no: e a visao DELE of malha.
      else if (tn == 6 && tt == 2 && of == s_meu_num) { s_c_localstats++; pb_leitor_t s; if (pb_sub(&tl,&s)) read_local_stats(s); }
      else if (!pb_pular(&tl, tt)) { break; }
    }
  }
}

// MeshPacket: 1=from 2=to 3=channel 4=decoded 6=id 7=rx_time 8=rx_snr
// 9=hop_limit 12=rx_rssi 13=via_mqtt (bool, varint) 14=hop_start - os dois
// ultimos lidos para a screen distinguir message ouvida no ar
// (com o numero of saltos) of message que entrou pela ponte MQTT.
static void read_packet(pb_leitor_t l)
{
  uint32_t of = 0, when = 0, para = 0xFFFFFFFFu, id = 0;
  uint8_t canal = 0;
  float snr = 0; int32_t rssi = 0;
  bool via_mqtt = false;
  // hop_limit: quanto SOBROU of orcamento of saltos when o pacote chegou
  // aqui. hop_start: o orcamento ORIGINAL que o remetente armou - viaja
  // junto, sem mudar, pacote a pacote. A diferenca e quantos repetidores ele
  // passou. Zero em hop_start e o valor of fabrica of protobuf (field
  // ausente) - firmware antigo nao manda, entao a conta stays indefinida.
  uint32_t hop_limit = 0, hop_start = 0;
  // transport_mechanism (21): por onde o pacote entrou no radio. So o MQTT (5)
  // interessa aqui - ver "O IMPLICITO DO MQTT" em marcar_envio().
  bool transp_mqtt = false;
  pb_leitor_t data = { nullptr, nullptr };
  int n, t;
  while (pb_next_field(&l, &n, &t)) {
    if      (n == 1 && t == 5) { pb_fixo32(&l, &of); }
    else if (n == 2 && t == 5) { pb_fixo32(&l, &para); }
    else if (n == 3 && t == 0) { uint64_t v; pb_varint(&l,&v); canal = (uint8_t)v; }
    else if (n == 6 && t == 5) { pb_fixo32(&l, &id); }       // id of pacote
    else if (n == 7 && t == 5) { pb_fixo32(&l, &when); }
    else if (n == 8 && t == 5) { pb_float(&l, &snr); }
    else if (n == 9 && t == 0) { uint64_t v; pb_varint(&l,&v); hop_limit = (uint32_t)v; }
    else if (n == 12 && t == 0){ uint64_t v; pb_varint(&l,&v); rssi = (int32_t)v; }
    // CAMPOS 14 E 15, conferidos contra o mesh.pb.h of firmware of C6L
    // via_mqtt = 14, hop_start = 15. Ate aqui eram lidos 13 e 14
    // - o 13 e o 'delayed' (obsoleto, sempre 0) e o 14 era o via_mqtt tomado
    // por hop_start. Resultado: MQTT nunca era reconhecido pelo pacote e o
    // numero of saltos saia errado ou "--" (dai o fallback pelo NodeInfo).
    else if (n == 14 && t == 0){ uint64_t v; pb_varint(&l,&v); via_mqtt = (v != 0); }
    else if (n == 15 && t == 0){ uint64_t v; pb_varint(&l,&v); hop_start = (uint32_t)v; }
    else if (n == 21 && t == 0){ uint64_t v; pb_varint(&l,&v); transp_mqtt = (v == 5); }
    else if (n == 4 && t == 2) { if (!pb_sub(&l, &data)) break; }
    else if (!pb_pular(&l, t)) { break; }
  }
  // ZERO SIGNIFICA "EU". O Meshtastic usa from=0 em pacote gerado localmente -
  // aparece como "fr=0x00000000" no log of proprio radio. Sem traduzir isso:
  //   - o LocalStats era descartado (o filtro exigia of == meu numero), e os
  //     contadores of nos ficavam em 0/0;
  //   - e one no fantasma of numero 0 entrava in tabela.
  if (of == 0) { of = s_meu_num; }

  // SALTOS = hop_start - hop_limit, so when hop_start veio preenchido (>0).
  // hop_start == 0 e o default of fabrica of protobuf (field ausente,
  // firmware antigo) - nesse caso a conta stays indefinida, e o sentinela diz
  // isso em vez of inventar one "0 saltos" que seria mentira.
  uint8_t saltos = MESH_SALTOS_DESCONHECIDO;
  if (hop_start > 0 && hop_start >= hop_limit) {
    saltos = (uint8_t)(hop_start - hop_limit);
  }

  // Interpretado no end porque os metadados (quem, when, com que sinal) podem
  // vir DEPOIS of bloco of data in serializacao.
  if (data.p) { read_data(data, of, para, canal, when, snr, rssi, id, via_mqtt, saltos, transp_mqtt); }

  marcar_epoch(when);
  marcar_hora_alive(when);

  if (of != 0) {
    mesh_no_t* no = achar_no(of);
    if (no) {
      if (snr != 0) { no->snr = snr; }
      // SALTOS/VIA_MQTT TAMBEM AQUI - a screen mostrava pouco mais que o ID,
      // comparada com o cliente web. Os dois numeros ja eram calculados ALI EM CIMA para TODO
      // pacote (ver 'saltos'/'via_mqtt' no comeco desta funcao) e ja iam
      // para a MENSAGEM (ler_data) - so nunca tinham sido aplicados on NO.
      // A tabela of nos so aprendia essa coluna when one NodeInfo chegava
      // (field 9/8, ler_node_info() mais acima) - one pacote raro (rajada no
      // dump inicial, depois so of vez em when), enquanto QUALQUER
      // pacote - message, telemetria, position - ja carrega hop_start/
      // hop_limit/via_mqtt in propria borda (MeshPacket). O "Desconhecido"
      // ficava sozinho ate o next NodeInfo, mesmo com dezenas of outros
      // pacotes of mesmo no passando por aqui antes disso.
      //
      // So sobrescreve saltos when ESTE pacote realmente trouxe hop_start
      // (saltos != DESCONHECIDO) - one pacote sem essa info nao deve erase
      // one valor bom que outro pacote ja tinha ensinado. via_mqtt e sempre o
      // caminho MAIS RECENTE (one no pode ser ouvido direto numa hora e via
      // ponte in outra), entao sobrescreve sem guarda.
      if (saltos != MESH_SALTOS_DESCONHECIDO) { no->saltos = saltos; }
      no->via_mqtt = via_mqtt;
      // AVISTAMENTO AO VIVO. Marcado aqui e nao no ler_node_info of proposito:
      // o node_info chega em rajada no dump of configuracao, para TODOS os nos
      // of uma vez, e carimbar todos ali faria a malha inteira parecer ter sido
      // ouvida now. Pacote e diferente - ele so chega when o no falou.
      no->visto_local_ms = tab5_now_ms();
      if (when >= EPOCH_MINIMO) { no->visto_em = when; }
      fixar_se_pelo_ar(no);   // ouvido pelo ar now: ganha o lugar no topo e a protecao of despejo
      ++s_nos_ver;
    }
  }
  notify_nos();
}

// LocalStats vem como telemetria of proprio no: 1=uptime 2=uso 3=airtime
// 4..=contagens, 8=nos_online, 9=nos_total, 10=piso_ruido (varia com a versao;
// pulamos o que nao reconhecemos).
static void read_from_radio(const uint8_t* buf, size_t n)
{
  pb_leitor_t l = { buf, buf + n };
  int f, t;

  // LOOP, e nao one unico field. O FromRadio nao e so a variante: ele tem
  // "uint32 id = 1" FORA of oneof, e protobuf serializa em order of numero of
  // field. Quando o radio preenche esse id - e ele preenche em alguns caminhos
  // of firmware - o first field of frame e o 1, nenhum ramo abaixo casa, e
  // a MENSAGEM INTEIRA ERA DESCARTADA em silencio.
  //
  // Ler so o first field era apostar que o id viria sempre zerado (e field
  // zerado o nanopb omite). A aposta funciona quase sempre, que e a pior
  // especie of erro: some of teste e aparece no uso.
  while (pb_next_field(&l, &f, &t))
  {
  // CAMPO 2, nao 1. O field 1 of FromRadio e "id"; o pacote e o 2.
  // Conferido no protobuf oficial (meshtastic.protobuf.mesh_pb2).
  // Com o numero errado, TODO pacote chegava e era descartado como field
  // desconhecido - o contador dizia "0 pacotes" com o enlace perfeito.
  if (f == 2 && t == 2) { s_c_pacotes++; pb_leitor_t s; if (pb_sub(&l,&s)) read_packet(s); }
  else if (f == 3 && t == 2) {           // my_info
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      mesh_admin_guardar_my_info(s.p, (size_t)(s.end - s.p));   // reboot_count, nodedb_count
      int n2, t2;
      while (pb_next_field(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 0) {
          uint64_t v; pb_varint(&s,&v);
          const uint32_t novo = (uint32_t)v;
          // So escreve when MUDA: a NVS tem ciclos of escrita finitos e o
          // my_info chega a cada apresentacao.
          if (novo != 0 && novo != s_meu_num) { lembrar_meu_numero(novo); }
          s_meu_num = novo;
        }
        else if (n2 == 8 && t2 == 0) {   // reboot_count: o diario of reinicios compara com o anterior
          uint64_t v; pb_varint(&s, &v);
          // s_ultimo_quadro_ms ainda e o of frame ANTERIOR (so atualiza depois
          // of ler_from_radio): zero = first contato desta sessao.
          mesh_diario_contador((uint32_t)v, s_saude.uptime_s, s_ultimo_frame_ms != 0, mesh_admin_gravando());
        }
        else if (!pb_pular(&s, t2)) { break; }
      }
    }
  }
  else if (f == 4 && t == 2) { s_c_nodeinfo++; pb_leitor_t s; if (pb_sub(&l,&s)) read_node_info(s); }
  else if (f == 11 && t == 2) {          // queueStatus: 1=res 4=mesh_packet_id
    // Falha IMEDIATA, antes of o pacote exit pelo radio: a queue of transmissao
    // nao aceitou. Sem isto o send_state ficaria "enviando" ate o prazo estourar,
    // when o radio ja tinha dito que nem tentou.
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      int32_t res = 0; uint32_t pid = 0;
      int n2, t2;
      while (pb_next_field(&s, &n2, &t2)) {
        if      (n2 == 1 && t2 == 0) { uint64_t v; pb_varint(&s,&v); res = (int32_t)v; }
        // mesh_packet_id e UINT32 (varint), nao fixed32 - conferido no
        // mesh.pb.h. Lido como fixed32 ele nunca casava, e a
        // recusa of queue of radio nunca marcava o send_state como falho.
        else if (n2 == 4 && t2 == 0) { uint64_t v; pb_varint(&s, &v); pid = (uint32_t)v; }
        else if (!pb_pular(&s, t2)) { break; }
      }
      if (pid != 0 && res != 0) { marcar_send_state(pid, MESH_ERRO_FILA, s_meu_num); }
    }
  }
  else if (f == 7 && t == 0) {           // configCompleteId
    // FIM DO DOWNLOAD DE CONFIGURACAO. E daqui em diante que o radio comeca a
    // entregar o trafego AO VIVO - antes disso ele esta drenando a queue of
    // configuracao. Nao ha nada a fazer alem of registrar, mas registrar
    // importa: sem esta line no log nao of para distinguir "o dump nunca
    // terminou" of "terminou e nao chega message", e o conserto e outro.
    uint64_t v;
    if (pb_varint(&l, &v)) {
      s_config_ok_ms = tab5_now_ms();   // a hora on vivo conta daqui (ver marcar_hora_viva)
      // A configuracao dos canais chegou inteira: s_canais_sujos ja esta of
      // pe, e a carona of tarefa "cartao" grava em ate 1 s (fora of radio).
      mesh_admin_end_dump();         // a configuracao of no tambem
      send_favs_pendentes();       // estrelas mudadas sem o radio
      ESP_LOGI(TAG, "configuracao completa (id %lu) - trafego ao vivo daqui",
               (unsigned long)v);
    }
  }
  else if (f == 8 && t == 0) {           // rebooted
    uint64_t v;
    if (pb_varint(&l, &v) && v != 0) {
      // TENSAO DO BARRAMENTO NO INSTANTE DO REINICIO
      // ("tem como saber se os 5V of Grove estao chegando?"). Nao medimos o
      // Grove especificamente (so existe o INA226 of barramento of BATERIA,
      // 2S Li-Po ~7.4V - ver tab5_power.hpp), e nao dá pra medir now
      // mesmo (tab5_power_latest() so, nunca _medir() daqui: esta e a
      // tarefa of Mesh, nao a of interface, e o barramento I2C interno nao
      // tem mutex - ver o aviso grande em tab5_power.hpp). O que "ultima"
      // devolve e uma leitura em cache of ATE 1s atras, atualizada pela
      // interface - resolucao grosseira para one afundamento rapido of
      // tensao, mas se o barramento estiver visivelmente bottom bem in hora
      // of reinicio, e one indicio real of energia, nao so uma suspeita.
      mesh_diario_foto(s_saude.uptime_s);   // context of Tab5 neste instante
      const tab5_measurement_t m = tab5_power_latest();
      ESP_LOGW(TAG, "o radio reiniciou - vou pedir a configuracao de novo "
                    "(barramento no instante: %.2fV %.0fmA, ate 1s atras)",
               (double)m.volts, (double)m.mA);
      s_radio_reiniciou = true;
    }
  }
  // CONFIG E MODULECONFIG - so o que o modo internet precisa: a regiao
  // (Config.lora.region, que vai in raiz of topico) e o servidor MQTT of no
  // (ModuleConfig.mqtt: 2=endereco 3=user 4=senha 8=raiz). Endereco vazio
  // = o servidor publico, que e o default of firmware.
  else if (f == 5 && t == 2) {           // config: oneof 1..10 = secao; 6=lora { 7=region }
    pb_leitor_t s, sec;
    int n2, t2;
    if (pb_sub(&l, &s)) {
      while (pb_next_field(&s, &n2, &t2)) {
        if (t2 != 2) { if (!pb_pular(&s, t2)) { break; } continue; }
        if (!pb_sub(&s, &sec)) { break; }
        // A SECAO CRUA vai para a configuracao of no (tab5_mesh_admin): e ela
        // que iteration, inteira, when one field e alterado.
        mesh_admin_guardar_secao(false, n2, sec.p, (size_t)(sec.end - sec.p));
        if (n2 != 6) { continue; }
        int n3, t3;
        while (pb_next_field(&sec, &n3, &t3)) {
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
      while (pb_next_field(&s, &n2, &t2)) {
        if (t2 != 2) { if (!pb_pular(&s, t2)) { break; } continue; }
        if (!pb_sub(&s, &sec)) { break; }
        mesh_admin_guardar_secao(true, n2, sec.p, (size_t)(sec.end - sec.p));
        if (n2 != 1) { continue; }
        char end[64] = "", usr[32] = "", pwd[32] = "", raiz[48] = "";
        bool mqtt_lig = false, tls = false, proxy = false;   // proto3: ausente = false
        int n3, t3;
        while (pb_next_field(&sec, &n3, &t3)) {
          if      (n3 == 1 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); mqtt_lig = (v != 0); }
          else if (n3 == 7 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); tls = (v != 0); }
          else if (n3 == 9 && t3 == 0) { uint64_t v; pb_varint(&sec, &v); proxy = (v != 0); }
          else if (n3 == 2 && t3 == 2) { pb_text(&sec, end,  sizeof(end)); }
          else if (n3 == 3 && t3 == 2) { pb_text(&sec, usr,  sizeof(usr)); }
          else if (n3 == 4 && t3 == 2) { pb_text(&sec, pwd,  sizeof(pwd)); }
          else if (n3 == 8 && t3 == 2) { pb_text(&sec, raiz, sizeof(raiz)); }
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
      const size_t   ch_n = (size_t)(s.end - s.p);
      // OS DOIS COMECAM EM ZERO, e isso e o conserto of dois bugs.
      //
      // 'index' (int32) e 'role' (enum) sao campos proto3 SINGULAR: o nanopb
      // NAO os escreve when valem zero. Comecando em -1 e 1, como era:
      //   - o canal 0 (o primario, LongFast) chegava SEM o field 1, ficava com
      //     idx -1 e a configuracao inteira dele era ignorada - nunca entrava
      //     como active, e por isso nao aparecia para reabrir depois of apagado;
      //   - one canal DESABILITADO (role 0) chegava SEM o field 3 e era lido como
      //     PRIMARY - continuava "active", com o name antigo, in coluna.
      // O valor ausente e o zero of protobuf: e isso que o firmware quis dizer.
      int idx = 0, papel = 0;
      bool downlink = false;
      char name[MESH_NOME_CONVERSA] = { 0 };
      uint8_t psk[32];
      size_t  n_psk = 0;
      int n2, t2;
      while (pb_next_field(&s, &n2, &t2)) {
        if      (n2 == 1 && t2 == 0) { uint64_t v; pb_varint(&s,&v); idx = (int)v; }
        else if (n2 == 3 && t2 == 0) { uint64_t v; pb_varint(&s,&v); papel = (int)v; }
        else if (n2 == 2 && t2 == 2) {           // ChannelSettings: 2=psk 3=name
          pb_leitor_t cs;
          if (pb_sub(&s,&cs)) {
            int n3, t3;
            while (pb_next_field(&cs, &n3, &t3)) {
              if (n3 == 3 && t3 == 2) { pb_text(&cs, name, sizeof(name)); }
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
      // O Channel CRU vai para a configuracao of no (a precisao of position of
      // primario e editada la, e iteration com o resto dele intacto).
      mesh_admin_guardar_canal(idx, ch_ini, ch_n);
      if (idx >= 0 && idx < MESH_MAX_CANAIS) {
        mesh_mqtt_aprender_downlink(idx, papel != 0 && downlink);   // antes: aplicar_canal ensina o name
        aplicar_canal(idx, papel, name, psk, n_psk);
      }
    }
  }
  else if (f == 14 && t == 2) {          // mqttClientProxyMessage: 1=topic 2=data 3=text 4=retained
    // O NO PEDINDO PARA PUBLICAR (proxy enabled nele). Sem o proxy of lado of
    // Tab5 a message e so descartada - o no nao espera resposta.
    pb_leitor_t s;
    if (pb_sub(&l, &s)) {
      static char topico[128];   // so a tarefa of servico le FromRadio
      topico[0] = 0;
      const uint8_t* dado = nullptr; size_t n_dado = 0;
      bool retido = false;
      int n2, t2;
      while (pb_next_field(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 2) { pb_text(&s, topico, sizeof(topico)); }
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
  else if (f == 16 && t == 2) {          // clientNotification: aviso of firmware para quem usa
    pb_leitor_t s;
    if (pb_sub(&l, &s)) { mesh_admin_guardar_aviso(s.p, (size_t)(s.end - s.p)); }
  }
  else if (f == 13 && t == 2) {          // metadata: 1=firmware_version
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      mesh_admin_guardar_metadata(s.p, (size_t)(s.end - s.p));
      int n2, t2;
      while (pb_next_field(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 2) { pb_text(&s, s_firmware, sizeof(s_firmware)); }
        else if (!pb_pular(&s, t2)) { break; }
      }
    }
  }
  // config e o resto: pulados, para o loop chegar on field seguinte em vez of
  // stop no first desconhecido.
  else if (!pb_pular(&l, t)) { break; }
  }
}

// ---------------------------------------------------------------------------
// Tarefa
// ---------------------------------------------------------------------------
// ToRadio{ heartbeat: {} } enquadrado.
//   field 7, tipo 2 -> tag (7<<3)|2 = 0x3A, comprimento 0
//
// O radio espera que o cliente se manifeste of tempos em tempos; o log of C6L
// mostra "Got client heartbeat" vindo of celular. Sem isto ele pode considerar
// o cliente ausente e stop of entregar trafego - e a ponte stays alive mas muda.
// O ENLACE: entrega o body of one ToRadio pela via of servico. Na serial ele
// vai num frame 0x94 0xC3 <tamanho>; no BLE e o proprio valor of
// caracteristica toRadio (ver tab5_mesh_ble.hpp). E o UNICO lugar que sabe
// qual e a via - o resto of servico so monta protobuf.
// Com o no fora e o Wi-Fi of pe, a internet assume (ver usar_internet no
// laco of servico): o que sairia pelo radio vai pelo MQTT.
// Criada uma vez e nunca apagada: o enlace pode ser chamado of interface
// enquanto o servico para e sobe of novo.
static SemaphoreHandle_t s_tx = nullptr;

// Um ToRadio of proxy (a press_start): o teto of firmware, pelo cabo e pelo BLE.
#define QUADRO_PROXY_MAX 512

static bool enlace_send(const uint8_t* body, size_t n)
{
  // Para o diario of reinicios: o que o Tab5 mandou por ultimo. ToRadio:
  // 1 = pacote, 3 = want_config_id, 7 = batimento.
  if (n > 0) {
    const int field = body[0] >> 3;
    mesh_diario_nota_send_state(field == 1 ? MESH_ENV_PACOTE : field == 3 ? MESH_ENV_CONFIG
                         : field == 7 ? MESH_ENV_HEARTBEAT : MESH_ENV_NENHUM);
  }
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { return mesh_mqtt_send(body, n); }
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    if (!mesh_ble_conectado() && mesh_mqtt_conectado()) { return mesh_mqtt_send(body, n); }
    return mesh_ble_send(body, n);
  }
  // UM QUADRO POR VEZ NA UART. Cabecalho e body sao duas escritas, e
  // escrevem aqui a tarefa of Mesh (batimento, configuracao) e a of interface
  // (text, favorito, admin). Sem a trava, one batimento podia cair entre o
  // cabecalho e o body of uma message: o no lia one tamanho e recebia outro
  // frame no lugar - as duas se perdiam sem erro nenhum.
  if (s_tx) { xSemaphoreTake(s_tx, portMAX_DELAY); }
  uint8_t cab[4] = { 0x94, 0xC3, (uint8_t)(n >> 8), (uint8_t)(n & 0xFF) };
  uart_write_bytes(MESH_UART, cab, sizeof(cab));
  uart_write_bytes(MESH_UART, body, n);
  if (s_tx) { xSemaphoreGive(s_tx); }
  return true;
}

static void mandar_heartbeat(void)
{
  const uint8_t q[] = { 0x3A, 0x00 };
  enlace_send(q, sizeof(q));
}

// PEDIR A CONFIGURACAO DE NOVO daqui a pouco - depois of gravar algo no no
// (tab5_mesh_admin): ele reinicia e, pela serial, nao fala sozinho.
static volatile uint32_t s_reapresentar_ms = 0;
static volatile bool     s_wake_next = false;

static volatile bool     s_so_config = false;

void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou)
{
  // Reiniciado, ele pode estar no modo of bottom consumo of serial e precisar
  // of rajada of 0x94. Sem reinicio, NAO: a rajada no middle of uma conversa e
  // ruido in maquina of quadros dele (ver pedir_apresentacao).
  // Sem reinicio (position fixa posta ou tirada) so a CONFIGURACAO changed: o
  // request e o "so configuracao", sem a NodeDB inteira atras.
  if (reiniciou) { s_wake_next = true; s_so_config = false; }
  else           { s_so_config = true; }
  s_reapresentar_ms = (tab5_now_ms() + ms) | 1u;
}

static void request_apresentacao(void)
{
  // A RAJADA DE 0x94 SO NA PRIMEIRA VEZ.
  //
  // Ela existe para tirar o radio of modo of bottom consumo of serial - e o
  // procedimento documentado of wake. Mas sao 32 bytes of lixo entrando in
  // maquina of enquadramento DELE, e mandar isso com a conversa ja estabelecida
  // e injetar ruido no middle of one frame que pode estar chegando on mesmo
  // time. Depois of first contato o radio esta acordado por definicao: ele
  // acabou of falar conosco.
  if ((s_meu_num == 0 || s_wake_next) && s_via == SETTINGS_MESH_TRANSPORT_SERIAL) {   // BLE nao tem modo of bottom consumo of serial
    s_wake_next = false;
    uint8_t acorda[32];
    memset(acorda, 0x94, sizeof(acorda));
    if (s_tx) { xSemaphoreTake(s_tx, portMAX_DELAY); }
    uart_write_bytes(MESH_UART, acorda, sizeof(acorda));
    if (s_tx) { xSemaphoreGive(s_tx); }
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  // SO A CONFIGURACAO when so ela changed: want_config_id 69420 e o
  // SPECIAL_NONCE_ONLY_CONFIG of firmware (PhoneAPI) - canais, config e
  // modulos, sem a NodeDB, que pelo Bluetooth com uma malha cheia sao
  // centenas of quadros so para saber que one field virou. Firmware antigo nao
  // conhece o numero e manda everything, como antes. Qualquer outro reason (first
  // contato, reinicio, silencio) pede everything.
  const bool so_config = s_so_config && s_meu_num != 0;
  s_so_config = false;
  static const uint8_t TUDO_Q[]   = { 0x18, 0x2A };               // ToRadio{ want_config_id: 42 }
  static const uint8_t CONFIG_Q[] = { 0x18, 0xAC, 0x9E, 0x04 };   // ToRadio{ want_config_id: 69420 }
  s_config_ok_ms = 0;   // dump novo: o que vier ate o end dele e passado
  mesh_admin_start_dump();
  if (so_config) { enlace_send(CONFIG_Q, sizeof(CONFIG_Q)); }
  else           { enlace_send(TUDO_Q, sizeof(TUDO_Q)); }
}

// ---------------------------------------------------------------------------
// Envio
// ---------------------------------------------------------------------------
// Texto, admin e position saem no MESMO envelope ToRadio{ packet: MeshPacket{
// to, channel, decoded: Data{ portnum, payload }, id, hop_limit, want_ack } },
// montado num lugar so: mesh_pb_to_radio() (services/mesh_pb.hpp).

// ---------------------------------------------------------------------------
// FILA DE ENVIO DE TEXTO
// ---------------------------------------------------------------------------
typedef struct { uint16_t n; bool internet; uint32_t id; uint8_t d[MESH_TEXTO + 96]; } send_state_fila_t;
static QueueHandle_t s_fila_send_state = nullptr;
#define MESH_FILA_ENVIO 8

// Ja of para dizer que nao exit? (ver tab5_mesh_enviar)
static bool pode_exit(bool internet, uint32_t para)
{
  const bool difusao = (para == 0xFFFFFFFFu);
  if (internet) { return difusao && mesh_mqtt_internet_conectado(); }
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { return difusao && mesh_mqtt_conectado(); }
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && !mesh_ble_conectado()) {
    return difusao && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();   // a internet cobrindo a falta dele
  }
  return true;
}

// DOIS SEGUNDOS ENTRE TEXTOS. O firmware descarta o text que chega of
// cliente menos of 2 s depois of anterior (PhoneAPI, rate limit of porta TEXT,
// of 2.5 ate a current): in 2.7 sem erro nenhum - a message ficava "enviando" e
// virava "sem resposta". A fila espera o que falta; a internet nao tem esse
// limit (e o servidor, nao o no). Chamada pela tarefa of servico, sem trava.
static void laco_envios(void)
{
  static uint32_t ultimo_ms = 0;
  static send_state_fila_t e;
  if (!s_fila_send_state) { return; }
  for (int k = 0; k < 4; ++k) {
    if (xQueuePeek(s_fila_send_state, &e, 0) != pdTRUE) { return; }
    if (!e.internet && ultimo_ms && tab5_now_ms() - ultimo_ms < 2100) { return; }
    xQueueReceive(s_fila_send_state, &e, 0);
    const bool ok = e.internet ? mesh_mqtt_internet_send(e.d, e.n) : enlace_send(e.d, e.n);
    if (!e.internet) { ultimo_ms = tab5_now_ms(); }
    if (!ok) {
      // O enlace caiu entre o touch e a vez dela: falhou, e a screen diz.
      ESP_LOGW(TAG, "pacote %08lX: o enlace recusou", (unsigned long)e.id);
      lock();
      marcar_send_state(e.id, MESH_ERRO_FILA, s_meu_num);
      destravar();
    }
  }
}

bool tab5_mesh_send(uint32_t para, uint8_t canal, const char* text)
{
  if (!text || !text[0]) { return false; }

  // SEM SERVICO NAO HA PARA ONDE ESCREVER. A screen ja impede chegar aqui, mas a
  // guarda mora TAMBEM aqui of proposito: sem servico nao ha UART instalada, e
  // one uart_write_bytes numa porta que nao existe nao e "nao enviou" - e
  // comportamento indefinido. Quem protege o hardware e a camada of hardware.
  if (!s_servico_active) {
    ESP_LOGW(TAG, "send rejected: service stopped (no Meshtastic node)");
    return false;
  }
  // ENLACE MUDO NAO E MOTIVO PARA RECUSAR, e a recusa era pior que o problema.
  //
  // "no ar" aqui significa so "chegou algum frame of radio nos ultimos 30s".
  // Numa malha parada isso e o estado NORMAL: o radio nao fala sozinho, e nao
  // ha nada of errado com o cabo. Recusando, o botao ENV nao fazia nada, sem
  // message of erro e sem entrada no historico - of lado of fora e
  // indistinguivel of one sistema quebrado, e foi assim que became o time todo.
  //
  // Mandar e o certo: se o radio nao estiver mesmo la, o queueStatus nao vem, o
  // ACK nao vem, e o prazo of 60s marca a message como "sem resposta" in
  // propria screen. Isso e informacao; o silencio nao era.
  if (!tab5_mesh_no_ar()) {
    ESP_LOGW(TAG, "no recent frame on the link - sending anyway");
  }

  // No maximum MESH_PAYLOAD_MAX (233) bytes - acima disso o no descarta o
  // pacote inteiro em silencio (ver o .hpp). Corta in fronteira of caractere:
  // one acento partido on middle exit como lixo in screen of quem recebe.
  size_t n_txt = strnlen(text, MESH_PAYLOAD_MAX + 1);
  if (n_txt > MESH_PAYLOAD_MAX) {
    n_txt = MESH_PAYLOAD_MAX;
    while (n_txt > 0 && ((unsigned char)text[n_txt] & 0xC0) == 0x80) { --n_txt; }
    ESP_LOGW(TAG, "text exceeds %d bytes - sending truncated", MESH_PAYLOAD_MAX);
  }
  char enviado[MESH_PAYLOAD_MAX + 1];
  memcpy(enviado, text, n_txt);
  enviado[n_txt] = 0;

  // Pela internet o pacote vai no canal 0 DE LA (o LongFast publico); o indice
  // MESH_CANAL_INTERNET e so a chave of conversa daqui. E la so ha canal (sem PKI).
  const bool internet = mesh_canal_internet(canal);
  if (internet && para != MESH_PB_DIFUSAO) { return false; }

  mesh_pb_envelope_t env = { };
  env.para    = para;
  env.channel   = internet ? 0 : canal;
  env.porta   = PORT_TEXTO;
  env.carga   = (const uint8_t*)text;
  env.n_carga = n_txt;
  // ID PROPRIO, e e ele que torna a confirmacao possivel. Sem id o radio
  // inventa one, e nunca ficamos sabendo qual - o ACK chegaria com one
  // request_id que nao bate com nada nosso. Zero e reservado, por isso o |1.
  env.id = esp_random() | 1u;
  // hop_limit 3. Sem ele o pacote exit com ZERO SALTOS - so quem ouve a nossa
  // antena diretamente recebe, e nada e repetido adiante. 3 e o default of
  // proprio Meshtastic (nao lemos a Config of radio para herdar o dele).
  env.saltos = 3;
  // want_ack nos DOIS casos - tambem in difusao. La nao ha destinatario para
  // confirm, mas ha a CONFIRMACAO IMPLICITA: when one vizinho repete o
  // pacote, o firmware devolve one Routing com o nosso request_id. E a unica
  // prova of que alguem ouviu - o mesmo que o app oficial faz num canal.
  env.quer_ack = true;
  const uint32_t id = env.id;

  // O enquadramento (se houver) e of enlace.
  uint8_t frame[MESH_TEXTO + 96];
  const size_t n_frame = mesh_pb_to_radio(&env, frame, sizeof(frame));
  if (!n_frame) { ESP_LOGE(TAG, "text did not fit in the frame"); return false; }

  // A RECUSA QUE DA PARA SABER AGORA exit now, com o send_state falhando in
  // screen: a rede publica fora, a internet (so canal - a direta precisa of
  // PKI), o Bluetooth sem connection. O resto e decidido pelo enlace, in fila.
  if (!pode_exit(internet, para)) {
    ESP_LOGW(TAG, "envio recusado: %s", internet ? tab5_mesh_rede_internet_estado() : tab5_mesh_enlace_descricao());
    return false;
  }

  // NA FILA, NAO NA HORA. Quem chama e a tarefa of INTERFACE, e o send_state pode
  // ter of wait o limit of 2 s of no entre textos (ver laco_envios) -
  // wait aqui congelava a screen. A message aparece in conversa ja como
  // "enviando"; a tarefa of servico a entrega no ritmo que o no aceita.
  if (!s_fila_send_state) { return false; }
  static send_state_fila_t e;   // so a interface envia text; 300 bytes fora of stack dela
  e.n = (uint16_t)n_frame;
  e.internet = internet;
  e.id = id;
  memcpy(e.d, frame, e.n);
  if (xQueueSend(s_fila_send_state, &e, 0) != pdTRUE) {
    ESP_LOGW(TAG, "send queue full - message was not sent");
    return false;
  }

  // Guarda a nossa propria message: a screen mostra as suas a right, e sem
  // isto ela sumiria - o radio nao devolve o que voce mesmo enviou.
  lock();
  anotar_send_state(id, para);
  // Sem remendo no contador: o guardar_msg ja sabe que a message e nossa
  // (of == s_meu_num) e nao a conta como nao lida, nem aqui nem in conversa.
  // via_mqtt = false: e a NOSSA transmissao saindo pelo nosso proprio radio,
  // nunca chega pela ponte.
  // saltos = desconhecido: e a NOSSA message, ainda nao viajou nenhum salto
  // of ponto of vista of quem recebe - e o meta-line nem aparece para
  // message MINHA of qualquer forma (ver desenhar_msgs em app_mesh.cpp).
  // A HORA DO ENVIO, em UTC como a rx_time of radio. Era 0 - e
  // 0 no fuso -3 vira 31/12/1969 in screen. Relogio ainda nao acertado (sem GPS
  // nem malha since o boot) continua 0 = "sem data", que a screen sabe tratar.
  const uint32_t when = tab5_time_epoch();
  guardar_msg(s_meu_num, para, canal, enviado, when, 0, 0, id, false, MESH_SALTOS_DESCONHECIDO);
  lembrar_carimbo(id, when);   // corrigido se o clock saltar (ao_saltar_relogio)
  destravar();

  ESP_LOGI(TAG, "enviado para %08lX (canal %u) id=%08lX: %s",
           (unsigned long)para, (unsigned)canal, (unsigned long)id, enviado);
  return true;
}

// ---------------------------------------------------------------------------
// POSICAO DO GPS PARA A MALHA
// ---------------------------------------------------------------------------
// Com o C6L no Bluetooth o Grove stays livre para o GPS. A position vai num
// MeshPacket, porta POSITION_APP (3), com one Position dentro:
//   1: latitude_i  (sfixed32, graus x 1e7)
//   2: longitude_i (sfixed32, graus x 1e7)
//   3: altitude    (int32, metros)
//   4: time        (fixed32, epoch UTC)
//
// PARA QUEM, e a diferenca e of time of ar.
//   - COM O NO of outro lado: SO PARA ELE (to = o numero dele), como faz o
//     app of celular. Ele passa a usa-la como a propria position e a difunde
//     no ritmo dele (Localizacao > "send a cada", "on se move"), com a
//     precisao of canal. Antes ia em DIFUSAO, 3 saltos: a malha recebia a
//     mesma position duas vezes - a of Tab5 e, logo depois, a of no repetindo
//     o que ja sabia, cada uma no ritmo of one.
//   - O no JA SABE onde esta (position fixa, ou o GPS dele enabled): nada a
//     mandar. A fixa ignoraria a nossa; o GPS dele a atropelaria.
//   - PELA INTERNET (sem no, ou o Bluetooth caiu e o MQTT cobre): o Tab5 e o
//     no, entao difunde ele mesmo - com a precisao of canal primario que o
//     no deixou configurada (0 = nao manda; menos of 32 = so a regiao).
// Sem fix, nao manda nada (devolve false e a tarefa tenta of novo em 30 s);
// true = resolvido ate a next iteration (mandou, ou nao ha o que mandar).
static bool send_position(void)
{
  if (!s_servico_active || !tab5_gps_service_active()) { return false; }
  const bool com_no = radio_aceita_admin();
  const bool pela_net = !com_no && (s_via == SETTINGS_MESH_TRANSPORT_INTERNET || s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH)
                     && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();
  if (!com_no && !pela_net) { return false; }
  if (com_no && mesh_admin_no_tem_position_propria()) { return true; }
  const int32_t prec = mesh_admin_precisao_primario();
  if (pela_net && prec == 0) { return true; }

  const nmea_position_t g = tab5_gps_position();
  if (!g.valid || g.qualidade == 0) { return false; }
  int32_t lat = (int32_t)(g.lat * 1e7), lon = (int32_t)(g.lon * 1e7);
  if (pela_net && prec > 0 && prec < 32) {
    // O mesmo corte of firmware (PositionModule): os bits que sobram vao
    // para o middle of celula, nao para o canto.
    const uint32_t mascara = 0xFFFFFFFFu << (32 - prec);
    lat = (int32_t)(((uint32_t)lat & mascara) + (1u << (31 - prec)));
    lon = (int32_t)(((uint32_t)lon & mascara) + (1u << (31 - prec)));
  }

  uint8_t pos[32];
  pb_escritor_t w = pb_escritor(pos, sizeof(pos));
  mesh_pb_position(&w, lat, lon, (int32_t)g.altitude_m, tab5_time_epoch());

  // Para o no: sem saltos (nao exit dele). Difusao: 3 saltos (ver tab5_mesh_enviar).
  const mesh_pb_envelope_t env = { com_no ? s_meu_num : MESH_PB_DIFUSAO, -1, PORT_POSICAO, pos,
                                   pb_tamanho(&w), esp_random() | 1u, (uint8_t)(com_no ? 0 : 3), false };
  uint8_t frame[96];
  const size_t n = pb_tamanho(&w) ? mesh_pb_to_radio(&env, frame, sizeof(frame)) : 0;
  const bool ok = n && enlace_send(frame, n);
  if (ok) {
    ESP_LOGI(TAG, "posicao do GPS %s (%.5f, %.5f)", com_no ? "passada ao no" : "difundida pela internet",
             g.lat, g.lon);
  }
  return ok;
}

// Quem somos, para os clientes MQTT (no e internet): eles publicam e
// respondem o want_config em name deste numero e destes nomes. Chamado antes
// of raise qualquer one dos dois.
static void informar_no_on_mqtt(void)
{
  char curto[MESH_NOME_CURTO] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_name_cache(s_meu_num, curto, sizeof(curto));
  tab5_mesh_name_longo_cache(s_meu_num, longo, sizeof(longo));
  mesh_mqtt_set_no(s_meu_num, curto, longo);
}

// AS GRAVACOES LENTAS DO MESH, NA CARONA DA TAREFA "cartao" (tab5_sd_carona).
// Eram feitas no laco of RADIO: enquanto a NodeDB ia para o cartao (montar,
// write dezenas of KB, fsync) ninguem lia a UART, e o buffer of 4 KB (~350
// ms a 115200) transbordava num cartao lento - frame of no perdido. A regra
// of sistema (hal/tab5_sd.hpp) e esta: quem so esvazia data no cartao pega
// carona in tarefa dele. Canais: a cada 15 s se changed (e logo depois of
// dump, que os marca). NodeDB: a cada 10 min se changed.
static void mesh_in_carona(void)
{
  if (!s_servico_active) { return; }
  const uint32_t t = tab5_now_ms();
  static uint32_t proximos_canais = 0, next_nodedb = 600000;
  if (s_canais_sujos && (int32_t)(t - proximos_canais) >= 0) {
    proximos_canais = t + 15000;
    salvar_canais();
  }
  if ((int32_t)(t - next_nodedb) >= 0) {
    next_nodedb = t + 600000;
    if (s_nos_ver != s_nodedb_ver_salva) { salvar_nodedb(); }
  }
}

static void vTaskMesh(void*)
{
  static uint8_t frame[1024];
  size_t usado = 0;
  int    estado = 0;          // 0=procura 0x94  1=viu 0x94  2=tamanho  3=body
  size_t falta = 0;
  uint8_t size_hi = 0;
  uint32_t next_apresentacao = 0;
  uint32_t next_heartbeat = 15000;   // first so depois of dump inicial

  // WATCHDOG: one deadlock aqui e SILENCIOSO e caro of one jeito especifico -
  // esta tarefa segura o mutex of mesh (lock()/destravar()) em varios
  // trechos, e travada DENTRO of one deles o mutex nunca seria devolvido. A
  // screen nao trava (ela so pediria o mutex e ficaria esperando, mas o loop of
  // interface segue vivo por fora), entao o sintoma seria "mensagens paradas
  // of chegar" sem nenhum log dizendo por que - dificil of distinguir of one
  // radio mudo of verdade. Ver o comentario igual em kernel/core.cpp para o
  // raciocinio complete.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "nao consegui inscrever a tarefa no watchdog");
  }

  for (;;)
  {
    // PONTO DE SAIDA. No topo of loop, fora of qualquer secao critica: aqui a
    // tarefa nao segura mutex nem arquivo, entao sumir daqui nao deixa nada
    // pela metade. Como o uart_read_bytes la embaixo espera no maximum 200ms, a
    // parada custa isso no pior caso.
    if (stop_requested(&s_parada)) {
      // DESINSCREVE ANTES DE MORRER. O watchdog guarda o HANDLE desta tarefa;
      // deletando sem notify ele antes, o handle stays pendurado la dentro e o
      // next ciclo of verificacao of watchdog encontraria uma tarefa que
      // nao existe mais - o tipo of "fantasma" que este sistema anda cacando
      // em outros lugares (Grove, apresentacao of radio). A order importa:
      // desinscrever so funciona enquanto a tarefa ainda existe para fazer a
      // chamada.
      esp_task_wdt_delete(NULL);
      stop_confirm(&s_parada);
      return;   // a vaga of pool devolve o dono e iteration a dormir (kernel/task_pool.h)
    }

    // PROVA DE VIDA. Chegar aqui of novo significa que a iteration anterior
    // terminou - nenhum uart_read_bytes, trava of mutex ou I/O of cartao
    // became preso. 200ms e o maior bloqueio que este loop faz of proposito
    // (o prazo of uart_read_bytes la embaixo); bem dentro dos 5s of watchdog.
    esp_task_wdt_reset();

    const uint32_t t = tab5_now_ms();

    // Apresentacao SO when o enlace esta mudo. Pedir of novo com o enlace
    // vivo faz o radio REINICIAR o download of configuracao inteiro - foi o que
    // produziu 11 node_info para 6 nos: o dump rodou duas vezes.
    // O radio avisou que reiniciou: pede a configuracao NA HORA. Sem isto a
    // gente esperava o enlace ficar mudo e o prazo of 10s vencer - e nesse
    // meio-tempo a lista of nos e os canais ficavam of reinicio anterior.
    mesh_admin_tique(t);
    if (s_reapresentar_ms && (int32_t)(t - s_reapresentar_ms) >= 0) {
      s_reapresentar_ms = 0;
      next_apresentacao = t + 10000;
      request_apresentacao();
    }
    else if (s_radio_reiniciou) {
      s_radio_reiniciou = false;
      next_apresentacao = t + 10000;
      request_apresentacao();
    }
    else if (!tab5_mesh_no_ar() && t > next_apresentacao) {
      // DOIS RITMOS, e a diferenca entre eles e o que resolve.
      //
      // Sem o my_info nao sabemos nem o nosso proprio numero: nada funciona, e
      // insistir of 10 em 10 segundos e o certo - e o first contato.
      //
      // COM ele, ja conversamos com este radio. Um silencio now quer dizer
      // "a malha esta quieta", nao "o radio sumiu" - e request a configuracao of
      // novo faz o C6L REINICIAR o download inteiro, jogando fora o trafego on
      // vivo que estiver in queue dele. Era one dump a cada 30s, para sempre, in
      // maior parte of time. De dois em dois minutos ainda recupera one radio
      // que travou, sem transformar a conversa num ciclo of reapresentacoes.
      const uint32_t espera = (s_meu_num == 0) ? 10000 : 120000;
      next_apresentacao = t + espera;
      ESP_LOGI(TAG, "sem quadro ha %d s - pedindo a configuracao ao radio",
               SILENCIO_MS / 1000);
      request_apresentacao();
    }

    // Nomes of no pendentes, of 15 em 15 segundos. Espacado of proposito: e o
    // que transforma a rajada of montagens of boot numa unica escrita.
    static uint32_t next_descarga = 15000;
    if (t > next_descarga) {
      next_descarga = t + 15000;
      descarregar_nomes();   // so entrega on gravador (tab5_sd_enqueue_append): nao bloqueia
    }
    // Canais (NVS) e NodeDB (cartao) NAO sao gravados aqui: ver mesh_na_carona.

    // Prazo dos envios sem resposta. Rede of seguranca: when o radio desiste
    // por conta propria ele AVISA (Routing.MAX_RETRANSMIT), entao chegar aqui
    // significa que o enlace morreu no middle - e "nao sei" e o que a screen deve
    // dizer, nao "falhou".
    for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
      if (s_envios[i].state == MESH_ENVIANDO
       && (t - s_envios[i].em_ms) > MESH_ACK_MS) {
        s_envios[i].state = MESH_SEM_RESPOSTA;
        s_envios_ver++;
        system_notify(AVISO_MESH_ENVIO);
        gravar_desfecho(s_envios[i].id, MESH_SEM_RESPOSTA, 0);
        ESP_LOGW(TAG, "packet %08lX: no confirmation after %d s",
                 (unsigned long)s_envios[i].id, MESH_ACK_MS / 1000);
      }
    }

    laco_envios();   // text in fila (ver tab5_mesh_enviar)

    // Batimento periodico. Mantem o radio convencido of que ha cliente of outro
    // lado; sem ele o trafego on vivo pode simplesmente stop of vir.
    if (t > next_heartbeat) {
      next_heartbeat = t + 30000;
      mandar_heartbeat();
    }

    // LEITURA EM BLOCO, e a diferenca e of energia, nao of elegancia.
    //
    // Antes era uma chamada por BYTE, com prazo of 200ms. A 115200 baud isso
    // chega a 11.520 chamadas por segundo, cada uma com travessia of queue e
    // troca of context - e roda o time todo, porque isto e servico, nao app.
    //
    // Lendo em bloco, one pacote inteiro exit of UMA chamada. A tarefa acorda
    // poucas vezes por segundo em vez of milhares, e o core iteration a ficar
    // ocioso entre elas - que e o que permite on DFS trabalhar.
    // POSICAO DO GPS PARA A MALHA - ver enviar_posicao().
    static uint32_t next_position = 60000;
    if (settings_mesh_position_interval_min() && t > next_position) {
      next_position = t + (send_position() ? settings_mesh_position_interval_min() * 60000u : 30000u);
    }

    // A REDE PUBLICA PELA INTERNET, em paralelo a everything o resto (ver tab5_mesh_mqtt.hpp):
    // sobe com o Wi-Fi e o ajuste enabled, desce sem one ou outro. Ate 8
    // pacotes por iteration, lidos como of rede INTERNET (s_rede_rx) - nao contam como
    // frame of no (s_ultimo_quadro_ms e a vida of RADIO).
    {
      const bool quer_internet = settings_mesh_public_network() && tab5_wifi_state() == TAB5_WIFI_CONNECTED;
      if (quer_internet && !mesh_mqtt_internet_active()) {
        informar_no_on_mqtt();
        mesh_mqtt_internet_start();
        lock();
        if (!esta_apagada(true, MESH_CANAL_INTERNET)) { achar_ou_create_conversa(true, MESH_CANAL_INTERNET, 0); }
        destravar();
      } else if (!quer_internet && mesh_mqtt_internet_active()) {
        mesh_mqtt_internet_stop();
      }
      for (int k = 0; k < 32; ++k) {
        const size_t n = mesh_mqtt_internet_receber(frame, sizeof(frame));
        if (n == 0) { break; }
        lock();
        s_rede_rx = MESH_REDE_INTERNET;
        read_from_radio(frame, n);
        s_rede_rx = MESH_REDE_NO;
        destravar();
      }
    }

    // O CLIENTE MQTT, EM UM DE DOIS PAPEIS (ver services/tab5_mesh_mqtt.hpp):
    //   INTERNET - o Tab5 fala com a malha pelo servidor, when o no nao esta
    //              (ou when a via escolhida e a Internet);
    //   PROXY    - o no esta conectado e com o proxy enabled: o Tab5 so carrega
    //              o que ELE publica e assina (proxy enabled no no).
    // Os dois sao exclusivos por construcao: o proxy requires o no presente, a
    // internet como tapa-buraco requires a falta dele. Com folga nas duas
    // direcoes: o BLE cai e iteration sozinho numa reconexao, e o servidor nao
    // deve ser open e closed a cada piscada dele.
    {
      enum { MQ_NADA = 0, MQ_NET, MQ_PROXY };
      static int      modo_request = MQ_NADA;
      static uint32_t modo_since = 0;
      // DOIS JEITOS DE A INTERNET ESTAR AQUI:
      //   - ela E a via (escolhida em Ajustes, ou o kernel subiu o servico so
      //     por ela): roda com o Wi-Fi, e ponto. O C6L no cabo nao a derruba
      //     daqui - quem decide switch para o Grove e a sondagem of kernel, on
      //     ACHAR o no (a priority of Grove), nao este laco a cada iteration. Com
      //     o teste of Grove aqui, escolher "internet" com o C6L plugado
      //     derrubava o MQTT logo depois of raise.
      //   - a via e o no (cabo ou Bluetooth): a internet so cobre a falta
      //     dele, se o ajuste "internet when sem o no" estiver enabled.
      const bool wifi = tab5_wifi_state() == TAB5_WIFI_CONNECTED;
      const bool no_ok = (s_via == SETTINGS_MESH_TRANSPORT_SERIAL)
                      || (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && mesh_ble_conectado())
                      || tab5_grove_type() == GROVE_MESHTASTIC;
      const bool quer_net = wifi && ((s_via == SETTINGS_MESH_TRANSPORT_INTERNET)
                                  || (settings_mesh_internet() && !no_ok));
      // O proxy pede o no CONVERSANDO (nao so a via escolhida) e com o dump
      // complete: e o dump que diz se o proxy esta enabled nele e quais canais
      // descem.
      const bool no_vivo = (s_via == SETTINGS_MESH_TRANSPORT_SERIAL && tab5_mesh_no_ar())
                        || (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH && mesh_ble_conectado());
      // A CHAVE E DO NO: liga-se o proxy in pagina "No" of app Mesh, que grava
      // proxy_to_client_enabled nele. Ligado no no, o Tab5 empresta a internet.
      const bool quer_proxy = wifi && no_vivo && s_config_ok_ms != 0 && mesh_mqtt_no_tem_proxy();
      const int modo = quer_proxy ? MQ_PROXY : quer_net ? MQ_NET : MQ_NADA;
      int current = !mesh_mqtt_active() ? MQ_NADA : mesh_mqtt_em_proxy() ? MQ_PROXY : MQ_NET;
      if (modo != modo_request) { modo_request = modo; modo_since = t; }
      // Descer: o no que acabou of voltar tira a internet NA HORA (o proxy vai
      // raise); o resto espera 10 s firmes, para nao derrubar numa piscada.
      if (current != MQ_NADA && current != modo
       && (modo == MQ_PROXY || t - modo_since >= 10000)) {
        mesh_mqtt_stop();
        current = MQ_NADA;
      }
      // Subir: a internet como tapa-buraco espera 20 s (o BLE costuma voltar
      // antes disso); a via Internet e o proxy sobem logo.
      const uint32_t folga = (modo == MQ_NET && s_via != SETTINGS_MESH_TRANSPORT_INTERNET) ? 20000 : 0;
      if (current == MQ_NADA && modo != MQ_NADA && t - modo_since >= folga) {
        informar_no_on_mqtt();
        if (modo == MQ_PROXY) { mesh_mqtt_start_proxy(); } else { mesh_mqtt_start(); }
        current = !mesh_mqtt_active() ? MQ_NADA : modo;
      }
      if (current == MQ_NET) {
        if (mesh_mqtt_conectou_now() && s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { s_radio_reiniciou = true; }
        // ESVAZIA A FILA, nao one por iteration: com o no no cabo a iteration espera ate
        // 200 ms in serial, e one pacote por iteration (5/s) perdia a rajada of
        // servidor. So o first espera (in via Internet e o ritmo of laco).
        for (int k = 0; k < 16; ++k) {
          const size_t n = mesh_mqtt_receber(frame, sizeof(frame),
                                             (k == 0 && s_via == SETTINGS_MESH_TRANSPORT_INTERNET) ? 200 : 0);
          if (n == 0) { break; }
          lock();
          s_c_quadros++;
          read_from_radio(frame, n);
          s_ultimo_frame_ms = tab5_now_ms();
          destravar();
        }
      } else if (current == MQ_PROXY) {
        // DESCIDA DO PROXY: o que veio of servidor vai on no. Ate 8 por iteration -
        // a iteration dura no maximum os 200 ms of leitura of enlace, e a fila (32)
        // segura a rajada no meio-tempo. Eram 4 (20/s), e one servidor publico
        // entregando o canal inteiro enchia a fila. Mais que 8 seguraria a
        // iteration escrevendo in serial/Bluetooth.
        static uint8_t desce[QUADRO_PROXY_MAX];
        for (int k = 0; k < 8; ++k) {
          const size_t n = mesh_mqtt_proxy_receber(desce, sizeof(desce));
          if (n == 0) { break; }
          enlace_send(desce, n);
        }
      }
      if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) {
        if (!mesh_mqtt_active()) { vTaskDelay(pdMS_TO_TICKS(200)); }
        continue;
      }
    }

    // VIA BLUETOOTH: o enlace ja entrega FromRadio inteiro, sem frame - nao
    // ha maquina of enquadramento. Mesmo prazo of 200 ms of serial.
    if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
      if (mesh_ble_conectou_now()) { s_radio_reiniciou = true; }   // pede a configuracao ja
      // O que ja chegou of uma vez (o dump vem em rajada); so o first espera.
      for (int k = 0; k < 16; ++k) {
        const size_t n = mesh_ble_receber(frame, sizeof(frame), k == 0 ? 200 : 0);
        if (n == 0) { break; }
        lock();
        s_c_quadros++;
        read_from_radio(frame, n);
        s_ultimo_frame_ms = tab5_now_ms();
        destravar();
      }
      continue;
    }

    // ACORDA NO PRIMEIRO BYTE, LE O RESTO NA HORA. uart_read_bytes so iteration
    // com o tamanho request OU no prazo: pedindo 256, one frame of 60 bytes
    // ficava esperando os 200 ms inteiros - cada message chegava atrasada, e o
    // laco inteiro (fila of send_state e os dois clientes of internet) andava nesse passo. Esperar
    // UM byte dorme igual when a serial esta quieta e acorda when ela fala.
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
      case 2: size_hi = b; estado = 3; break;
      case 3: {
        falta = ((size_t)size_hi << 8) | b;
        usado = 0;
        if (falta == 0) { estado = 0; }
        else if (falta > sizeof(frame)) {
          // PULA O CORPO, nao iteration a procurar cabecalho dentro dele.
          //
          // Voltando on estado 0 a maquina passava a varrer os proprios bytes
          // of frame descartado atras of 0x94 0xC3 - e essa dupla aparece por
          // acaso em dado binario. Encontrando uma, ela lia one "tamanho" que e
          // lixo e engolia o frame BOM que vinha depois. Uma message some, e
          // nada no log diz por que.
          ESP_LOGW(TAG, "frame of %u bytes does not fit - skipping", (unsigned)falta);
          estado = 5;
        }
        else { estado = 4; }
        break;
      }
      case 5:                      // engolindo one frame grande demais
        if (--falta == 0) { estado = 0; }
        break;
      case 4:
        frame[usado++] = b;
        if (usado >= falta) {
          lock();
          s_c_quadros++;
          read_from_radio(frame, usado);
          s_ultimo_frame_ms = tab5_now_ms();
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
// Existe para o caso que o sistema simplesmente nao cobria: switch o modulo of
// cabo com o aparelho enabled. Sem isto o servico ficava of pe conversando com
// one C6L que nao estava mais la, e o GPS recem-plugado nunca era procurado
// porque a porta tinha dono.
//
// Devolve false when NAO conseguiu stop - e nesse caso nao limpa nada. Se a
// tarefa nao confirmou que exited, ela ainda pode estar alive: erase a UART e o
// mutex debaixo dela trocaria one servico teimoso por uma pane.
bool tab5_mesh_stop(void)
{
  if (!s_servico_active) { return true; }   // ja parado; request of novo nao e erro

  ESP_LOGI(TAG, "stopping the service...");
  // A tarefa espera no maximum 200 ms in UART (ou in fila of enlace): a saida
  // custa uma iteration. 1 s e folga larga. Sem confirmacao, o request FICA DE PE
  // e nada e desmontado - a next chamada termina (ver parada_pedir).
  if (!stop_request(&s_parada, 1000)) {
    ESP_LOGE(TAG, "the task did not confirm within 1 s - continuing to stop; nothing was dismantled");
    return false;
  }

  // Daqui para bottom a tarefa NAO EXISTE MAIS. So now e seguro desmontar o
  // que ela usava.

  // TAREFA DE GRAVACAO PRIMEIRO - independente of UART/mutex acima (so
  // depende of propria queue), mas running ate aqui. 300ms of folga: uma
  // escrita costuma levar bem menos, e nao ha nada em andamento in tarefa
  // "mesh" que ainda vá enfileirar mais nada (ja confirmou saida). Qualquer
  // request ainda in queue when ela exit e descartado - mesmo trade-off
  // aceito em toda queue deste sistema que enfileira E/S (ver o aviso grande
  // junto of MESH_FILA_GRAVACAO).

  // O que estava pending vai now: a tarefa que faria a descarga acabou of
  // morrer, e sem isto os nomes aprendidos nesta sessao se perderiam.
  descarregar_nomes();

  mesh_mqtt_stop();
  mesh_mqtt_internet_stop();   // a fila dele e lida so por esta tarefa, que acabou of exit
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    mesh_ble_stop();
  } else if (s_via == SETTINGS_MESH_TRANSPORT_SERIAL) {
    uart_driver_delete(MESH_UART);
    tab5_grove_release(GROVE_OWNER_MESH);
  }

  // O AVISO SO SAI SE NAO HA NADA PARA LER. Antes ele era apagado a cada
  // parada - e o Mesh para sozinho (troca of via, Wi-Fi caindo): o aviso
  // sumia sem o user ter lido nada. As nao lidas ficam guardadas e voltam
  // com as conversas; o que esta publicado now continua valendo.
  guardar_nao_lidas();
  if (s_notif >= 0 && !ha_nao_lidas_guardadas()) { notification_limpar(s_notif); }
  salvar_canais();   // o que a carona ainda nao levou (ela para com o servico)
  salvar_nodedb();   // antes of memset of tabela, logo abaixo
  // O MUTEX FICA. Ele era apagado aqui e recriado no next start - mas
  // as consultas publicas (a barra of home, o app Mesh) o tomam of OUTRAS
  // tarefas a qualquer momento: erase one mutex preso ou com alguem
  // esperando nele e memoria liberada em uso, e entre o erase e o recriar
  // as leituras corriam sem exclusao none. Criado uma vez, vale para
  // sempre (o mesmo of tab5_sd).

  // Enlace zerado: sem isto o tab5_mesh_no_ar() continuaria respondendo pelo
  // ultimo frame recebido, e a barra diria "conectado" a one radio ausente.
  s_ultimo_frame_ms = 0;
  s_config_ok_ms = 0;
  // s_meu_num NAO e zerado: nosso numero nao muda porque o cabo exited, e o
  // arquivo precisa dele para saber of quem e cada message.
  s_n_msgs = 0;   // a contagem of NOS nao tem variavel: exit of memset abaixo
  s_total_msgs = 0;
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  // Sob a trava: a interface pode estar lendo as tabelas now mesmo (a
  // barra of home, o app Mesh) - antes isto rodava com o mutex ja apagado.
  lock();
  memset(s_nos,  0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  ++s_conv_ver;
  destravar();

  s_servico_active = false;
  system_notify(AVISO_MESH_NOS);   // a barra of escrita passa a "so consulta"
  ESP_LOGI(TAG, "service stopped%s", s_via == SETTINGS_MESH_TRANSPORT_SERIAL ? ", Grove released" : "");
  return true;
}

bool tab5_mesh_release_grove(void)
{
  // Via Bluetooth o servico nao esta no Grove: switch o modulo of cabo nao e
  // reason para derrubar a conversa com o no.
  if (s_servico_active && s_via != SETTINGS_MESH_TRANSPORT_SERIAL) { return true; }
  return tab5_mesh_stop();
}

uint8_t tab5_mesh_via(void) { return s_via; }

const char* tab5_mesh_enlace_descricao(void)
{
  if (!s_servico_active) { return "stopped"; }
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { return mesh_mqtt_descricao(); }
  if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    return (!mesh_ble_conectado() && mesh_mqtt_active()) ? mesh_mqtt_descricao() : mesh_ble_descricao();
  }
  return tab5_mesh_no_ar() ? "Grove cable, communicating" : "Grove cable, waiting for the node";
}

static void start_via(uint8_t via);

void tab5_mesh_start(void)
{
  // SOBE NA VIA ESCOLHIDA. Se o servico esta of pe por OUTRA via (internet
  // cobrindo a falta of no, ou o Bluetooth when o Grove acabou of ganhar a
  // priority), para antes: sem isto o iniciar_via via "ja esta of pe" e nao
  // fazia nada, e o Mesh seguia in via velha.
  const uint8_t via = settings_mesh_transport();
  if (s_servico_active && s_via != via && !stop_em_progress(&s_parada)) { tab5_mesh_stop(); }
  start_via(via);
}

const char* tab5_mesh_rede_internet_estado(void)
{
  static char t[72];
  if (!settings_mesh_public_network())                          { return "disabled"; }
  if (tab5_wifi_state() != TAB5_WIFI_CONNECTED)    { return "waiting for Wi-Fi"; }
  if (!s_servico_active)                             { return "Mesh service stopped"; }
  if (!mesh_mqtt_internet_conectado())                   { return "conectando a mqtt.meshtastic.org"; }
  snprintf(t, sizeof(t), "conectada · %lu pacotes", (unsigned long)mesh_mqtt_internet_recebidos());
  return t;
}

const char* tab5_mesh_internet_estado(void)
{
  if (settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_INTERNET) {
    if (tab5_wifi_state() != TAB5_WIFI_CONNECTED) { return "Internet selected: waiting for Wi-Fi to connect"; }
    if (!mesh_mqtt_active())                        { return "Internet escolhida: vai conectar em instantes"; }
    return mesh_mqtt_descricao();
  }
  if (!settings_mesh_internet())                   { return "Disabled"; }
  if (tab5_grove_type() == GROVE_MESHTASTIC
   || (s_servico_active && s_via == SETTINGS_MESH_TRANSPORT_SERIAL)) { return "Waiting: the node on Grove has priority"; }
  if (tab5_wifi_state() != TAB5_WIFI_CONNECTED)  { return "Esperando o Wi-Fi conectar"; }
  if (!mesh_mqtt_active())                         { return "Ligada, vai conectar em instantes"; }
  return mesh_mqtt_descricao();
}

const char* tab5_mesh_proxy_estado(void)
{
  // Curto: vai on lado of "enabled" numa line of pagina "No".
  static char d[48];
  if (tab5_wifi_state() != TAB5_WIFI_CONNECTED) { return "no Wi-Fi on the Tab5"; }
  if (!mesh_mqtt_no_tem_proxy())                 { return "the node has not confirmed yet"; }
  if (!mesh_mqtt_em_proxy())                     { return "vai conectar"; }
  if (!mesh_mqtt_conectado())                    { return "connecting to the server"; }
  uint32_t sobe, desce, perde;
  mesh_mqtt_proxy_contadores(&sobe, &desce, &perde);
  snprintf(d, sizeof(d), "conectado (%lu/%lu)", (unsigned long)sobe, (unsigned long)desce);
  return d;
}

void tab5_mesh_start_internet(void)
{
  if (s_servico_active) { return; }
  start_via(SETTINGS_MESH_TRANSPORT_INTERNET);
}

static void start_via(uint8_t via)
{
  // IDEMPOTENTE. Sem esta guarda, chamar duas vezes criaria one segundo mutex
  // (vazando o first, com quem as tarefas ainda estariam sincronizando) e
  // uma segunda tarefa disputando a mesma UART. Nunca acontecia porque o unico
  // chamador era o boot; now existe o caminho of re-sondagem.
  if (s_servico_active) {
    // Uma parada em andamento nao pode ser atropelada por one start: a tarefa
    // antiga ainda esta alive e a UART ainda e dela.
    if (stop_em_progress(&s_parada)) {
      ESP_LOGW(TAG, "the previous service is still stopping - will not start over it");
    } else {
      ESP_LOGI(TAG, "service is already running - nothing to do");
    }
    return;
  }

  tab5_mutex_create(&s_trava);   // uma vez por boot - ver tab5_mesh_parar
  tab5_sd_piggyback(mesh_in_carona);   // idempotente: a carona e registrada uma vez
  tab5_mutex_create(&s_tx);
  if (!s_fila_send_state) { s_fila_send_state = xQueueCreateWithCaps(MESH_FILA_ENVIO, sizeof(send_state_fila_t), MALLOC_CAP_SPIRAM); }
  if (s_fila_send_state) { xQueueReset(s_fila_send_state); }
  tab5_time_on_jump(on_saltar_clock);   // idempotente


  s_notif = notification_registrar("Meshtastic");
  notification_associar_app(s_notif, "Mesh");   // touch o aviso abre o app Mesh
  // A preferencia (ajustes) e quem manda since o INSTANTE em que a fonte
  // nasce - este registro acontece of novo a cada sondagem que reencontra o
  // modulo (troca of cabo), entao sem isto o silencio "esqueceria" a cada
  // hot-swap.
  if (s_notif >= 0) { notification_silenciar(s_notif, !settings_mesh_beep()); }
  memset(s_nos, 0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  // O ANEL DE IDS VISTOS NAO E ZERADO: switch of via (Grove -> internet) ou
  // reconectar o cabo nao muda quais mensagens ja estao no cartao. Zerado, a
  // mesma message ouvida pelo radio e depois pela ponte MQTT entrava duas
  // vezes no historico.
  memset(&s_saude, 0, sizeof(s_saude));   // a saude e of no desta via, nao of anterior

  ++s_conv_ver;   // a coluna recomeca

  // OS CANAIS EXISTEM DESDE O INICIO, sem wait message: os ativos of
  // ultima configuracao conhecida (NVS), ou o primario sem none. Numa malha
  // quieta, sem isto, nao havia para onde write. Os apagados ficam of fora
  // - e eram recriados aqui, o canal 0 sempre, mesmo apagado.
  create_conversas_dos_canais();

  // Os nos que o Tab5 ja conhecia (alem of teto of radio).
  carregar_nodedb();
  mesh_diario_carregar();   // reinicios of no ja anotados (cartao)
  mesh_wifi_no_carregar();  // redes of Wi-Fi of no ja usadas (cartao)

  // FAVORITOS NA TABELA DESDE O INICIO, mesmo sem ter falado: estao fixos.
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { achar_no(s_favs[i]); }

  // RESTAURA O QUE O memset() ACIMA ACABOU DE APAGAR - defeito observado em
  // open o app ANTES of plugar o C6L jah carrega everything of cartao
  // (ao_abrir() chama tab5_mesh_carregar_do_cartao()), mas a sondagem of Grove
  // que chega depois, achando o modulo, zera s_conv[] alguns milissegundos
  // apos - a lista stays so com o canal 0 (line acima) ate o user exit e
  // voltar on app, o que forca one novo ao_abrir(). Chamar of novo aqui, logo
  // apos o memset, fecha a mesma lacuna sem depender of user reabrir nada.
  // Idempotente (achar_ou_criar_conversa acha o que ja existe em vez of
  // duplicar) e seguro nesta tarefa: tab5_mesh_iniciar() so roda dentro of
  // vTaskSondagemGrove (kernel/core.cpp), a tarefa of priority mais baixa of
  // sistema, feita para aguentar E/S lenta (aqui, montar o cartao) sem lock
  // o touch.
  tab5_mesh_carregar_of_cartao();

  // As conversas renasceram com as nao lidas guardadas (guardar_nao_lidas):
  // a barra passa a count a partir delas, e nao of que sobrou publicado.
  lock();
  publicar_notif();
  destravar();

  s_via = via;
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) {
    // Sem cabo e sem radio: o laco of servico abre o MQTT (usar_internet).
  } else if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {
    // Sem Grove e sem UART: o enlace BLE procura o no, conecta e entrega o
    // que chegar. O Bluetooth ja foi enabled por quem chamou.
    if (!mesh_ble_start()) { ESP_LOGE(TAG, "Bluetooth link did not start"); return; }
  } else {
  uart_config_t cfg = { };
  cfg.baud_rate = MESH_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (!tab5_grove_acquire(GROVE_OWNER_MESH)) {
    ESP_LOGE(TAG, "Grove is busy - service will not start");
    return;
  }

  esp_log_level_set("uart", ESP_LOG_ERROR);   // reserva antiga of Ex_I2C, ver tab5_grove
  if (uart_driver_install(MESH_UART, 4096, 512, 0, NULL, 0) != ESP_OK
   || uart_param_config(MESH_UART, &cfg) != ESP_OK
   || uart_set_pin(MESH_UART, MESH_TX, MESH_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    esp_log_level_set("uart", ESP_LOG_WARN);
    ESP_LOGE(TAG, "could not open the Grove UART");
    return;
  }
  esp_log_level_set("uart", ESP_LOG_WARN);
  }   // via serial

  // Prioridade 3, igual on WiFi: abaixo of interface (4), para nunca engasgar o
  // touch; acima of registro (1), porque perder byte of serial e irreversivel.
  // ATIVO ANTES DE A TAREFA EXISTIR: com priority 3 ela comeca in hora,
  // antes of quem chamou (a sondagem, priority 1) voltar - e ja envia.
  s_iniciado_em_ms = tab5_now_ms();   // ver o aviso em tab5_mesh_silencio_ms()
  s_servico_active  = true;
  // 6 KB: com 4 KB o monitor registrou no minimum 264 bytes livres em uso real.
  if (!stop_prepare(&s_parada)
   || !pool_create(nullptr, vTaskMesh, nullptr, "mesh", 6144, 3, OWNER_SERVICE, "Mesh")) {
    // DEVOLVE O QUE O INICIAR TOMOU. Antes so a bandeira voltava: a UART e o
    // Grove ficavam com o Mesh para sempre, e none sondagem achava mais
    // nada no cabo ate reiniciar.
    stop_abandon(&s_parada);
    s_servico_active = false;
    if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { mesh_ble_stop(); }
    else if (s_via == SETTINGS_MESH_TRANSPORT_SERIAL) { uart_driver_delete(MESH_UART); tab5_grove_release(GROVE_OWNER_MESH); }
    ESP_LOGE(TAG, "not enough memory for the Mesh task");
    return;
  }
  system_notify(AVISO_MESH_NOS);
  tab5_memory_marco("Mesh (servico)");
  if (s_via == SETTINGS_MESH_TRANSPORT_INTERNET) { ESP_LOGI(TAG, "service running (Internet only, without the node)"); }
  else if (s_via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { ESP_LOGI(TAG, "service running (Bluetooth)"); }
  else { ESP_LOGI(TAG, "service running (Grove %d baud, RX=%d TX=%d)", MESH_BAUD, MESH_RX, MESH_TX); }
}
