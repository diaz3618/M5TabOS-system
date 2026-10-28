// Servico de WiFi do sistema - NAO e um aplicativo.
//
// Por que service e nao app: a barra de status precisa do state o time todo,
// e um app futuro (Meshtastic, impressao por network, NTP) vai querer a conexao ja
// de pe. Se o WiFi morresse junto com a tela do app que o ligou, cada app teria
// que reconectar - segundos de espera a cada troca de tela.
//
// A vontade do usuario mora em settings_wifi() (NVS). Este modulo e o BRACO que
// realiza essa vontade; ele nao decide nada sozinho.
//
// -------------------------------------------------------------------------
// DUAS TAREFAS, E A DIVISAO NAO E ARBITRARIA
// -------------------------------------------------------------------------
// O trilho do C6 (WLAN_PWR_EN) fica num expansor I2C, e neste sistema o I2C
// tem owner unico: a tarefa de interface (ver o header de kernel/core.cpp - o
// I2C_Class do M5Unified nao tem mutex). Por isso:
//
//   - tab5_wifi_turn_on()/turn_off() TOCAM I2C -> so podem ser chamadas de
//     dentro da tarefa de interface. Elas so mexem no trilho e avisam.
//   - todo o resto (esperar o C6 bootar, SDIO, esp_wifi, DHCP) roda na tarefa
//     "wifi" deste modulo, que nao encosta em I2C nenhum.
//
// Chamar tab5_wifi_turn_on() de outra tarefa nao da erro - da corrida no I2C, que
// e pior, porque aparece um dia qualquer como leitura errada de bateria.
//
// -------------------------------------------------------------------------
// NIVEIS DE ECONOMIA - hoje o 1, de proposito
// -------------------------------------------------------------------------
// Combinado: comecar conservador e measure antes de apertar. Os niveis sao:
//
//   1 (ATUAL)  turn_off = esp_wifi_stop(). O radio do C6 desliga, o link SDIO
//              e a stack continuam de pe. Religar e instantaneo e nunca failure.
//   2          + esp_wifi_deinit() + esp_hosted_deinit(). Libera RAM, mas
//              religar tem que renegociar o SDIO.
//   3          + cortar WLAN_PWR_EN. Consumo zero no C6. E o unico que apaga
//              o BLE junto, e o unico onde religar pode simplesmente falhar.
//
// Medir cada um com o app Energia antes de start de nivel. O plano e comparar
// numeros, nao adivinhar - do mesmo jeito que foi feito com a frequencia.

#pragma once

#include <stdint.h>
#include <stddef.h>

enum tab5_wifi_state_t
{
  TAB5_WIFI_OFF = 0,
  TAB5_WIFI_STARTING,      // ligando o C6 e negociando o SDIO
  TAB5_WIFI_CONNECTING,   // searching/autenticando no point de acesso
  TAB5_WIFI_CONNECTED,    // tem IP
  TAB5_WIFI_NO_CREDENTIALS,
  TAB5_WIFI_FAILED,

  // O ENLACE COM O C6 EXISTE, MAS ELE NAO RESPONDE A RPC.
  //
  // Estado proprio porque e um defeito de OUTRA natureza: nao e password errada
  // nem network fora do ar, e nao adianta tentar de novo. O coprocessador esta com
  // firmware ruim e precisa ser regravado - fisicamente, pelo conector
  // reservado da placa.
  //
  // Merece name proprio na tela: "failed" mandaria o usuario conferir a password
  // pelo resto da vida.
  TAB5_WIFI_C6_UNRESPONSIVE
};

// Cria a tarefa do service. NAO liga o radio. Chamar uma vez, no boot, depois
// de settings_init() e de tab5_sd_init().
void tab5_wifi_init(void);

// SOMENTE da tarefa de interface (mexem no expansor I2C). Nao bloqueiam: o
// trabalho demorado acontece na tarefa do service.
void tab5_wifi_turn_on(void);
void tab5_wifi_turn_off(void);

// Sobe SO o link com o C6 (esp_hosted), sem turn_on o WiFi nem search
// roteador. Para who usa o C6 por outro motivo - o Bluetooth. Liga o trilho
// do radio (I2C): chamar da tarefa de interface, como tab5_wifi_turn_on().
// Assincrono: tab5_wifi_stack_ready() diz quando ficou ready.
void tab5_wifi_ensure_stack(void);

tab5_wifi_state_t tab5_wifi_state(void);

// A stack do esp_hosted (e do esp_wifi) ja subiu neste boot?
//
// Diferente de "conectado": o link SDIO com o C6 pode estar de pe com o WiFi
// sem network nenhuma. Quem quiser falar com o coprocessador - consultar version,
// pedir MAC, mandar dado pelo channel peer_data - precisa disto, nao do state da
// conexao.
bool tab5_wifi_stack_ready(void);
// Avisa quando a stack (esp_hosted + esp_wifi) acabou de start - para who
// depende do link com o C6 (o ESP-NOW) dormir sem perguntar a cada segundo.
// Uma funcao so; chamada da tarefa do Wi-Fi, nao pode bloquear.
void tab5_wifi_on_stack_ready(void (*fn)(void));

// O C6 roda o NOSSO firmware (folder c6/) - ele respondeu ao aviso pelo channel
// peer_data neste boot. So entao existe ESP-NOW (services/tab5_espnow): o
// firmware de fabrica nao tem esse channel.
bool tab5_wifi_c6_is_ours(void);

// MODO SO RADIO - o radio fica de pe SEM roteador, para o ESP-NOW usar um
// channel proprio (o radio so fica num channel por vez, e associado ele e do
// roteador). Sem reconexao, e sem a desistencia que para o radio longe de
// casa. false = volta a connect no roteador. Nao bloqueia (queue do service).
void tab5_wifi_radio_only_mode(bool turn_on);
bool tab5_wifi_radio_only(void);

// O radio esta INICIADO (com roteador, sem roteador, ou scanning). Falso depois
// de turn_off o WiFi em Ajustes ou da desistencia fora do alcance do roteador - que param o
// radio mas deixam o C6 alimentado e o ENLACE de pe. E a pergunta do ESP-NOW.
bool tab5_wifi_radio_active(void);

// ECONOMIA DO RADIO. O padrao e modem sleep: o C6 desliga o receptor entre os
// beacons do roteador (economia grande, e para WiFi comum nao custa nada).
// Quem precisa do receptor on o TEMPO TODO - o ESP-NOW, que perde o frame
// que chega durante o cochilo - pede com o proprio bit, e desfaz quando nao
// precisar mais. O radio so economiza sem pedido nenhum. Nao bloqueia.
#define TAB5_AWAKE_ESPNOW  (1u << 0)
// Um bit POR QUEM PEDE: o service ESP-NOW reafirma o dele de tempos em tempos,
// e com um bit shared ele desfazia o pedido do farejo.
#define TAB5_AWAKE_SNIFFING  (1u << 1)
#define TAB5_AWAKE_TEST   (1u << 2)   // comando radio do console
#define TAB5_AWAKE_CSI     (1u << 3)   // service CSI (tab5_csi)
void tab5_wifi_keep_awake(uint32_t who, bool sim);

// O ENLACE SDIO com o C6 esta de pe? Mais fraco que a pergunta acima, e de
// proposito: aqui basta o transporte ter negociado. Um C6 com firmware
// quebrado pode ter link SEM ter RPC funcionando - e e nesse state que a
// gravacao de firmware novo precisa poder acontecer.
bool tab5_wifi_c6_link(void);

// REMOVIDA (auditoria de camadas): fazia "s_pilha_de_pe ||
// settings_wifi()" aqui dentro - HAL perguntando a um service (settings.h),
// sentido errado da dependencia (ver WRITING_APPS.md secao 8). O unico
// chamador era sistema_impor_preferencias() no kernel, que ja tem
// settings_wifi() disponivel legitimamente - a conta virou
// "tab5_wifi_stack_ready() || settings_wifi()" direto la, sem perder nada.

// Varre e imprime as networks visiveis no console. Diagnostico: separa "o radio
// nao funciona" (lista vazia) de "a network nao aparece" (lista com outras networks,
// sem a sua). Exige que o WiFi ja tenha sido on uma vez neste boot - a
// stack precisa estar de pe.
void tab5_wifi_scan_console(void);

// Troca a network em uso sem reler o cartao. Chamada por dentro de
// tab5_wifi_save_credentials() logo apos write o file - read o file de
// volta com o link do C6 de pe derruba o WiFi (ver o comentario em
// ler_credenciais). Exposta tambem para who so quer atualizar a RAM sem
// tocar no cartao (nenhum chamador faz isso hoje, mas write/atualizar sao
// operacoes distintas o bastante para nao forcar as duas juntas sempre).
void tab5_wifi_set_credentials(const char* ssid, const char* password);

// Acrescenta (ou atualiza) a network no wifi.txt, cifrando a password, e atualiza a
// RAM (tab5_wifi_set_credentials). As outras networks guardadas ficam.
// Grava pela porta do sistema (tab5_sd_replace): monta o cartao sozinho,
// e com ele lent ao computador vai para o diario. Bloqueia - do worker,
// nunca da tarefa de interface. password "-" ou vazia grava network open.
bool tab5_wifi_save_credentials(const char* ssid, const char* password);

// ---------------------------------------------------------------------------
// REDES: varias conhecidas, procura e troca
// ---------------------------------------------------------------------------
// O wifi.txt guarda varias networks (pares SSID/password, senhas cifradas). Ao turn_on,
// o sistema procura em volta e conecta na known de signal mais forte.
#define TAB5_WIFI_MAX_NETWORKS   8
#define TAB5_WIFI_MAX_SEEN  24

typedef struct { char ssid[33]; bool open; } tab5_wifi_network_t;

typedef struct {
  char   ssid[33];
  int8_t rssi;
  uint8_t channel;
  bool   open;
  bool   known;   // esta no wifi.txt
} tab5_wifi_seen_t;

// Procura as networks em volta (assincrono; liga o C6 se preciso - I2C: chamar
// da tarefa de interface). O resultado sai em tab5_wifi_get_seen().
void     tab5_wifi_search(void);
bool     tab5_wifi_searching(void);
int      tab5_wifi_seen_count(void);
bool     tab5_wifi_get_seen(int i, tab5_wifi_seen_t* out);   // do signal mais forte ao mais fraco
int      tab5_wifi_known_count(void);
bool     tab5_wifi_is_known(const char* ssid);
// A network guardada numero 'i' (0..n_conhecidas-1), com a password em text (so RAM;
// vazia = network open). Serve a who quer COPIAR as networks (o Mesh traz as do
// Tab5 para o Wi-Fi do no). false = indice fora da lista.
bool     tab5_wifi_saved_network(int i, char* ssid, size_t tam_ssid, char* password, size_t tam_senha);
// A mesma cifra do wifi.txt (AES com a switch_number do aparelho), para outros
// arquivos de credenciais guardarem password sem text puro no cartao.
// 'hex' precisa de TAB5_WIFI_CIPHER_HEX bytes.
#define TAB5_WIFI_CIPHER_HEX 200
bool     tab5_wifi_encrypt_text(const char* plaintext, char* hex, size_t hex_size);
bool     tab5_wifi_decrypt_text(const char* hex, char* plaintext, size_t plaintext_size);
uint32_t tab5_wifi_networks_version(void);   // muda a cada procura ou network guardada/esquecida

// Conecta numa network JA CONHECIDA (gravada com tab5_wifi_save_credentials).
void     tab5_wifi_connect(const char* ssid);
// Tira a network do wifi.txt (mesma porta e mesmas regras de write acima).
bool     tab5_wifi_forget(const char* ssid);

// Texto curto para a barra de status ("WiFi", "conectando", o SSID...).
const char* tab5_wifi_description(void);

const char* tab5_wifi_ssid(void);
const char* tab5_wifi_ip(void);

// Sinal indisponivel: sem conexao, ou o coprocessador nao preencheu o campo.
// NAO use 0 como "sem signal" - zero e um value de RSSI valido na teoria e
// significaria signal maximo, o oposto do pretendido.
#define TAB5_WIFI_RSSI_UNKNOWN  (-127)

// TAB5_WIFI_RSSI_UNKNOWN quando nao ha conexao. Le do cache alimentado pela tarefa do service:
// esp_wifi_sta_get_ap_info() e uma ida ao C6 pelo SDIO, cara demais para
// chamar a cada redesenho da barra.
int8_t tab5_wifi_rssi(void);

// Caminho do file de credenciais, para o app Ajustes mostrar quando faltar.
#define TAB5_WIFI_FILE "/sdcard/apps/Sistema/wifi.txt"
