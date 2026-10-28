// Servico de WiFi do sistema - NAO e um aplicativo.
//
// Por que servico e nao app: a barra de status precisa do estado o tempo todo,
// e um app futuro (Meshtastic, impressao por rede, NTP) vai querer a conexao ja
// de pe. Se o WiFi morresse junto com a tela do app que o ligou, cada app teria
// que reconectar - segundos de espera a cada troca de tela.
//
// A vontade do usuario mora em ajustes_wifi() (NVS). Este modulo e o BRACO que
// realiza essa vontade; ele nao decide nada sozinho.
//
// -------------------------------------------------------------------------
// DUAS TAREFAS, E A DIVISAO NAO E ARBITRARIA
// -------------------------------------------------------------------------
// O trilho do C6 (WLAN_PWR_EN) fica num expansor I2C, e neste sistema o I2C
// tem dono unico: a tarefa de interface (ver o cabecalho de kernel/core.cpp - o
// I2C_Class do M5Unified nao tem mutex). Por isso:
//
//   - tab5_wifi_ligar()/desligar() TOCAM I2C -> so podem ser chamadas de
//     dentro da tarefa de interface. Elas so mexem no trilho e avisam.
//   - todo o resto (esperar o C6 bootar, SDIO, esp_wifi, DHCP) roda na tarefa
//     "wifi" deste modulo, que nao encosta em I2C nenhum.
//
// Chamar tab5_wifi_ligar() de outra tarefa nao da erro - da corrida no I2C, que
// e pior, porque aparece um dia qualquer como leitura errada de bateria.
//
// -------------------------------------------------------------------------
// NIVEIS DE ECONOMIA - hoje o 1, de proposito
// -------------------------------------------------------------------------
// Combinado: comecar conservador e medir antes de apertar. Os niveis sao:
//
//   1 (ATUAL)  desligar = esp_wifi_stop(). O radio do C6 desliga, o link SDIO
//              e a stack continuam de pe. Religar e instantaneo e nunca falha.
//   2          + esp_wifi_deinit() + esp_hosted_deinit(). Libera RAM, mas
//              religar tem que renegociar o SDIO.
//   3          + cortar WLAN_PWR_EN. Consumo zero no C6. E o unico que apaga
//              o BLE junto, e o unico onde religar pode simplesmente falhar.
//
// Medir cada um com o app Energia antes de subir de nivel. O plano e comparar
// numeros, nao adivinhar - do mesmo jeito que foi feito com a frequencia.

#pragma once

#include <stdint.h>
#include <stddef.h>

enum tab5_wifi_estado_t
{
  TAB5_WIFI_DESLIGADO = 0,
  TAB5_WIFI_SUBINDO,      // ligando o C6 e negociando o SDIO
  TAB5_WIFI_CONECTANDO,   // procurando/autenticando no ponto de acesso
  TAB5_WIFI_CONECTADO,    // tem IP
  TAB5_WIFI_SEM_CREDENCIAL,
  TAB5_WIFI_FALHOU,

  // O ENLACE COM O C6 EXISTE, MAS ELE NAO RESPONDE A RPC.
  //
  // Estado proprio porque e um defeito de OUTRA natureza: nao e senha errada
  // nem rede fora do ar, e nao adianta tentar de novo. O coprocessador esta com
  // firmware ruim e precisa ser regravado - fisicamente, pelo conector
  // reservado da placa.
  //
  // Merece nome proprio na tela: "falhou" mandaria o usuario conferir a senha
  // pelo resto da vida.
  TAB5_WIFI_C6_MUDO
};

// Cria a tarefa do servico. NAO liga o radio. Chamar uma vez, no boot, depois
// de ajustes_iniciar() e de tab5_sd_iniciar().
void tab5_wifi_iniciar(void);

// SOMENTE da tarefa de interface (mexem no expansor I2C). Nao bloqueiam: o
// trabalho demorado acontece na tarefa do servico.
void tab5_wifi_ligar(void);
void tab5_wifi_desligar(void);

// Sobe SO o enlace com o C6 (esp_hosted), sem ligar o WiFi nem procurar
// roteador. Para quem usa o C6 por outro motivo - o Bluetooth. Liga o trilho
// do radio (I2C): chamar da tarefa de interface, como tab5_wifi_ligar().
// Assincrono: tab5_wifi_pilha_de_pe() diz quando ficou pronto.
void tab5_wifi_garantir_pilha(void);

tab5_wifi_estado_t tab5_wifi_estado(void);

// A stack do esp_hosted (e do esp_wifi) ja subiu neste boot?
//
// Diferente de "conectado": o enlace SDIO com o C6 pode estar de pe com o WiFi
// sem rede nenhuma. Quem quiser falar com o coprocessador - consultar versao,
// pedir MAC, mandar dado pelo canal peer_data - precisa disto, nao do estado da
// conexao.
bool tab5_wifi_pilha_de_pe(void);
// Avisa quando a pilha (esp_hosted + esp_wifi) acabou de subir - para quem
// depende do enlace com o C6 (o ESP-NOW) dormir sem perguntar a cada segundo.
// Uma funcao so; chamada da tarefa do Wi-Fi, nao pode bloquear.
void tab5_wifi_ao_subir(void (*fn)(void));

// O C6 roda o NOSSO firmware (pasta c6/) - ele respondeu ao aviso pelo canal
// peer_data neste boot. So entao existe ESP-NOW (services/tab5_espnow): o
// firmware de fabrica nao tem esse canal.
bool tab5_wifi_c6_nosso(void);

// MODO SO RADIO - o radio fica de pe SEM roteador, para o ESP-NOW usar um
// canal proprio (o radio so fica num canal por vez, e associado ele e do
// roteador). Sem reconexao, e sem a desistencia que para o radio longe de
// casa. false = volta a conectar no roteador. Nao bloqueia (queue do servico).
void tab5_wifi_modo_so_radio(bool ligar);
bool tab5_wifi_so_radio(void);

// O radio esta INICIADO (com roteador, sem roteador, ou varrendo). Falso depois
// de desligar o WiFi em Ajustes ou da desistencia fora do alcance do roteador - que param o
// radio mas deixam o C6 alimentado e o ENLACE de pe. E a pergunta do ESP-NOW.
bool tab5_wifi_radio_ativo(void);

// ECONOMIA DO RADIO. O padrao e modem sleep: o C6 desliga o receptor entre os
// beacons do roteador (economia grande, e para WiFi comum nao custa nada).
// Quem precisa do receptor ligado o TEMPO TODO - o ESP-NOW, que perde o frame
// que chega durante o cochilo - pede com o proprio bit, e desfaz quando nao
// precisar mais. O radio so economiza sem pedido nenhum. Nao bloqueia.
#define TAB5_ACORDADO_ESPNOW  (1u << 0)
// Um bit POR QUEM PEDE: o servico ESP-NOW reafirma o dele de tempos em tempos,
// e com um bit shared ele desfazia o pedido do farejo.
#define TAB5_ACORDADO_FAREJO  (1u << 1)
#define TAB5_ACORDADO_TESTE   (1u << 2)   // comando radio do console
#define TAB5_ACORDADO_CSI     (1u << 3)   // servico CSI (tab5_csi)
void tab5_wifi_manter_acordado(uint32_t quem, bool sim);

// O ENLACE SDIO com o C6 esta de pe? Mais fraco que a pergunta acima, e de
// proposito: aqui basta o transporte ter negociado. Um C6 com firmware
// quebrado pode ter enlace SEM ter RPC funcionando - e e nesse estado que a
// gravacao de firmware novo precisa poder acontecer.
bool tab5_wifi_c6_enlace(void);

// REMOVIDA (auditoria de camadas): fazia "s_pilha_de_pe ||
// ajustes_wifi()" aqui dentro - HAL perguntando a um servico (settings.h),
// sentido errado da dependencia (ver WRITING_APPS.md secao 8). O unico
// chamador era sistema_impor_preferencias() no kernel, que ja tem
// ajustes_wifi() disponivel legitimamente - a conta virou
// "tab5_wifi_pilha_de_pe() || ajustes_wifi()" direto la, sem perder nada.

// Varre e imprime as redes visiveis no console. Diagnostico: separa "o radio
// nao funciona" (lista vazia) de "a rede nao aparece" (lista com outras redes,
// sem a sua). Exige que o WiFi ja tenha sido ligado uma vez neste boot - a
// stack precisa estar de pe.
void tab5_wifi_varrer(void);

// Troca a rede em uso sem reler o cartao. Chamada por dentro de
// tab5_wifi_gravar_credenciais() logo apos gravar o arquivo - ler o arquivo de
// volta com o link do C6 de pe derruba o WiFi (ver o comentario em
// ler_credenciais). Exposta tambem para quem so quer atualizar a RAM sem
// tocar no cartao (nenhum chamador faz isso hoje, mas gravar/atualizar sao
// operacoes distintas o bastante para nao forcar as duas juntas sempre).
void tab5_wifi_definir_credenciais(const char* ssid, const char* senha);

// Acrescenta (ou atualiza) a rede no wifi.txt, cifrando a senha, e atualiza a
// RAM (tab5_wifi_definir_credenciais). As outras redes guardadas ficam.
// Grava pela porta do sistema (tab5_sd_substituir): monta o cartao sozinho,
// e com ele emprestado ao computador vai para o diario. Bloqueia - do worker,
// nunca da tarefa de interface. senha "-" ou vazia grava rede aberta.
bool tab5_wifi_gravar_credenciais(const char* ssid, const char* senha);

// ---------------------------------------------------------------------------
// REDES: varias conhecidas, procura e troca
// ---------------------------------------------------------------------------
// O wifi.txt guarda varias redes (pares SSID/senha, senhas cifradas). Ao ligar,
// o sistema procura em volta e conecta na conhecida de sinal mais forte.
#define TAB5_WIFI_MAX_REDES   8
#define TAB5_WIFI_MAX_VISTAS  24

typedef struct { char ssid[33]; bool aberta; } tab5_wifi_rede_t;

typedef struct {
  char   ssid[33];
  int8_t rssi;
  uint8_t canal;
  bool   aberta;
  bool   conhecida;   // esta no wifi.txt
} tab5_wifi_visto_t;

// Procura as redes em volta (assincrono; liga o C6 se preciso - I2C: chamar
// da tarefa de interface). O resultado sai em tab5_wifi_vista().
void     tab5_wifi_procurar(void);
bool     tab5_wifi_procurando(void);
int      tab5_wifi_n_vistas(void);
bool     tab5_wifi_vista(int i, tab5_wifi_visto_t* out);   // do sinal mais forte ao mais fraco
int      tab5_wifi_n_conhecidas(void);
bool     tab5_wifi_conhecida(const char* ssid);
// A rede guardada numero 'i' (0..n_conhecidas-1), com a senha em texto (so RAM;
// vazia = rede aberta). Serve a quem quer COPIAR as redes (o Mesh traz as do
// Tab5 para o Wi-Fi do no). false = indice fora da lista.
bool     tab5_wifi_rede_guardada(int i, char* ssid, size_t tam_ssid, char* senha, size_t tam_senha);
// A mesma cifra do wifi.txt (AES com a chave do aparelho), para outros
// arquivos de credenciais guardarem senha sem texto puro no cartao.
// 'hex' precisa de TAB5_WIFI_CIFRA_HEX bytes.
#define TAB5_WIFI_CIFRA_HEX 200
bool     tab5_wifi_cifrar_texto(const char* claro, char* hex, size_t tam_hex);
bool     tab5_wifi_decifrar_texto(const char* hex, char* claro, size_t tam_claro);
uint32_t tab5_wifi_redes_versao(void);   // muda a cada procura ou rede guardada/esquecida

// Conecta numa rede JA CONHECIDA (gravada com tab5_wifi_gravar_credenciais).
void     tab5_wifi_conectar(const char* ssid);
// Tira a rede do wifi.txt (mesma porta e mesmas regras de gravar acima).
bool     tab5_wifi_esquecer(const char* ssid);

// Texto curto para a barra de status ("WiFi", "conectando", o SSID...).
const char* tab5_wifi_descricao(void);

const char* tab5_wifi_ssid(void);
const char* tab5_wifi_ip(void);

// Sinal indisponivel: sem conexao, ou o coprocessador nao preencheu o campo.
// NAO use 0 como "sem sinal" - zero e um valor de RSSI valido na teoria e
// significaria sinal maximo, o oposto do pretendido.
#define TAB5_WIFI_RSSI_DESCONHECIDO  (-127)

// TAB5_WIFI_RSSI_DESCONHECIDO quando nao ha conexao. Le do cache alimentado pela tarefa do servico:
// esp_wifi_sta_get_ap_info() e uma ida ao C6 pelo SDIO, cara demais para
// chamar a cada redesenho da barra.
int8_t tab5_wifi_rssi(void);

// Caminho do arquivo de credenciais, para o app Ajustes mostrar quando faltar.
#define TAB5_WIFI_ARQUIVO "/sdcard/apps/Sistema/wifi.txt"
