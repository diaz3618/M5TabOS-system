// Servico of WiFi of sistema - NAO e one aplicativo.
//
// Por que service e nao app: a barra of status precisa of state o time todo,
// e one app futuro (Meshtastic, impressao por network, NTP) vai querer a connection ja
// of pe. Se o WiFi morresse junto com a screen of app que o ligou, cada app teria
// que reconectar - segundos of espera a cada troca of screen.
//
// A vontade of user mora em settings_wifi() (NVS). Este modulo e o BRACO que
// realiza essa vontade; ele nao decide nada sozinho.
//
// -------------------------------------------------------------------------
// DUAS TAREFAS, E A DIVISAO NAO E ARBITRARIA
// -------------------------------------------------------------------------
// O trilho of C6 (WLAN_PWR_EN) stays num expansor I2C, e neste sistema o I2C
// tem owner unico: a tarefa of interface (ver o header of kernel/core.cpp - o
// I2C_Class of M5Unified nao tem mutex). Por isso:
//
//   - tab5_wifi_turn_on()/turn_off() TOCAM I2C -> so podem ser chamadas of
//     dentro of tarefa of interface. Elas so mexem no trilho e avisam.
//   - todo o resto (wait o C6 bootar, SDIO, esp_wifi, DHCP) roda in tarefa
//     "wifi" deste modulo, que nao encosta em I2C nenhum.
//
// Chamar tab5_wifi_turn_on() of outra tarefa nao of erro - of corrida no I2C, que
// e pior, porque aparece one dia qualquer como leitura errada of battery.
//
// -------------------------------------------------------------------------
// NIVEIS DE ECONOMIA - hoje o 1, of proposito
// -------------------------------------------------------------------------
// Combinado: comecar conservador e measure antes of apertar. Os niveis sao:
//
//   1 (ATUAL)  turn_off = esp_wifi_stop(). O radio of C6 desliga, o link SDIO
//              e a stack continuam of pe. Religar e instantaneo e nunca failure.
//   2          + esp_wifi_deinit() + esp_hosted_deinit(). Libera RAM, mas
//              religar tem que renegociar o SDIO.
//   3          + cortar WLAN_PWR_EN. Consumo zero no C6. E o unico que apaga
//              o BLE junto, e o unico onde religar pode simplesmente falhar.
//
// Medir cada one com o app Energia antes of start of level. O plano e comparar
// numeros, nao adivinhar - of mesmo jeito que foi feito com a frequencia.

#pragma once

#include <stdint.h>
#include <stddef.h>

enum tab5_wifi_state_t
{
  TAB5_WIFI_OFF = 0,
  TAB5_WIFI_STARTING,      // ligando o C6 e negociando o SDIO
  TAB5_WIFI_CONNECTING,   // searching/autenticando no point of acesso
  TAB5_WIFI_CONNECTED,    // tem IP
  TAB5_WIFI_NO_CREDENTIALS,
  TAB5_WIFI_FAILED,

  // O ENLACE COM O C6 EXISTE, MAS ELE NAO RESPONDE A RPC.
  //
  // Estado proprio porque e one defeito of OUTRA natureza: nao e password errada
  // nem network fora of ar, e nao adianta tentar of novo. O coprocessador esta com
  // firmware ruim e precisa ser regravado - fisicamente, pelo conector
  // reservado of placa.
  //
  // Merece name proprio in screen: "failed" mandaria o user conferir a password
  // pelo resto of vida.
  TAB5_WIFI_C6_UNRESPONSIVE
};

// Cria a tarefa of service. NAO liga o radio. Chamar uma vez, no boot, depois
// of settings_init() e of tab5_sd_init().
void tab5_wifi_init(void);

// SOMENTE of tarefa of interface (mexem no expansor I2C). Nao bloqueiam: o
// work demorado acontece in tarefa of service.
void tab5_wifi_turn_on(void);
void tab5_wifi_turn_off(void);

// Sobe SO o link com o C6 (esp_hosted), sem turn_on o WiFi nem search
// roteador. Para who usa o C6 por outro reason - o Bluetooth. Liga o trilho
// of radio (I2C): chamar of tarefa of interface, como tab5_wifi_turn_on().
// Assincrono: tab5_wifi_stack_ready() diz when became ready.
void tab5_wifi_ensure_stack(void);

tab5_wifi_state_t tab5_wifi_state(void);

// A stack of esp_hosted (e of esp_wifi) ja subiu neste boot?
//
// Diferente of "conectado": o link SDIO com o C6 pode estar of pe com o WiFi
// sem network none. Quem quiser falar com o coprocessador - consultar version,
// request MAC, mandar dado pelo channel peer_data - precisa disto, nao of state of
// connection.
bool tab5_wifi_stack_ready(void);
// Avisa when a stack (esp_hosted + esp_wifi) acabou of start - para who
// depende of link com o C6 (o ESP-NOW) dormir sem perguntar a cada segundo.
// Uma funcao so; chamada of tarefa of Wi-Fi, nao pode bloquear.
void tab5_wifi_on_stack_ready(void (*fn)(void));

// O C6 roda o NOSSO firmware (folder c6/) - ele respondeu on aviso pelo channel
// peer_data neste boot. So entao existe ESP-NOW (services/tab5_espnow): o
// firmware of fabrica nao tem esse channel.
bool tab5_wifi_c6_is_ours(void);

// MODO SO RADIO - o radio stays of pe SEM roteador, para o ESP-NOW usar one
// channel proprio (o radio so stays num channel por vez, e associado ele e of
// roteador). Sem reconexao, e sem a desistencia que para o radio longe of
// casa. false = iteration a connect no roteador. Nao bloqueia (queue of service).
void tab5_wifi_radio_only_mode(bool turn_on);
bool tab5_wifi_radio_only(void);

// O radio esta INICIADO (com roteador, sem roteador, ou scanning). Falso depois
// of turn_off o WiFi em Ajustes ou of desistencia fora of alcance of roteador - que param o
// radio mas deixam o C6 alimentado e o ENLACE of pe. E a pergunta of ESP-NOW.
bool tab5_wifi_radio_active(void);

// ECONOMIA DO RADIO. O default e modem sleep: o C6 desliga o receptor entre os
// beacons of roteador (economia grande, e para WiFi comum nao custa nada).
// Quem precisa of receptor on o TEMPO TODO - o ESP-NOW, que perde o frame
// que chega durante o cochilo - pede com o proprio bit, e desfaz when nao
// precisar mais. O radio so economiza sem request nenhum. Nao bloqueia.
#define TAB5_AWAKE_ESPNOW  (1u << 0)
// Um bit POR QUEM PEDE: o service ESP-NOW reafirma o dele of tempos em tempos,
// e com one bit shared ele desfazia o request of farejo.
#define TAB5_AWAKE_SNIFFING  (1u << 1)
#define TAB5_AWAKE_TEST   (1u << 2)   // comando radio of console
#define TAB5_AWAKE_CSI     (1u << 3)   // service CSI (tab5_csi)
void tab5_wifi_keep_awake(uint32_t who, bool sim);

// O ENLACE SDIO com o C6 esta of pe? Mais fraco que a pergunta acima, e of
// proposito: aqui basta o transporte ter negociado. Um C6 com firmware
// quebrado pode ter link SEM ter RPC funcionando - e e nesse state que a
// gravacao of firmware novo precisa poder acontecer.
bool tab5_wifi_c6_link(void);

// REMOVIDA (auditoria of camadas): fazia "s_pilha_de_pe ||
// settings_wifi()" aqui dentro - HAL perguntando a one service (settings.h),
// sentido errado of dependencia (ver WRITING_APPS.md secao 8). O unico
// chamador era sistema_impor_preferencias() no kernel, que ja tem
// settings_wifi() disponivel legitimamente - a conta virou
// "tab5_wifi_stack_ready() || settings_wifi()" direto la, sem perder nada.

// Varre e imprime as networks visiveis no console. Diagnostico: separa "o radio
// nao funciona" (lista vazia) of "a network nao aparece" (lista com outras networks,
// sem a sua). Exige que o WiFi ja tenha sido on uma vez neste boot - a
// stack precisa estar of pe.
void tab5_wifi_scan_console(void);

// Troca a network em uso sem reler o cartao. Chamada por dentro of
// tab5_wifi_save_credentials() logo apos write o file - read o file of
// iteration com o link of C6 of pe derruba o WiFi (ver o comentario em
// ler_credenciais). Exposta tambem para who so quer atualizar a RAM sem
// touch no cartao (nenhum chamador faz isso hoje, mas write/atualizar sao
// operacoes distintas o bastante para nao forcar as duas juntas sempre).
void tab5_wifi_set_credentials(const char* ssid, const char* password);

// Acrescenta (ou atualiza) a network no wifi.txt, cifrando a password, e atualiza a
// RAM (tab5_wifi_set_credentials). As outras networks guardadas ficam.
// Grava pela porta of sistema (tab5_sd_replace): monta o cartao sozinho,
// e com ele lent on computador vai para o diario. Bloqueia - of worker,
// nunca of tarefa of interface. password "-" ou vazia grava network open.
bool tab5_wifi_save_credentials(const char* ssid, const char* password);

// ---------------------------------------------------------------------------
// REDES: varias conhecidas, procura e troca
// ---------------------------------------------------------------------------
// O wifi.txt guarda varias networks (pares SSID/password, senhas cifradas). Ao turn_on,
// o sistema procura em iteration e conecta in known of signal mais forte.
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

// Procura as networks em iteration (assincrono; liga o C6 se preciso - I2C: chamar
// of tarefa of interface). O resultado exit em tab5_wifi_get_seen().
void     tab5_wifi_search(void);
bool     tab5_wifi_searching(void);
int      tab5_wifi_seen_count(void);
bool     tab5_wifi_get_seen(int i, tab5_wifi_seen_t* out);   // of signal mais forte on mais fraco
int      tab5_wifi_known_count(void);
bool     tab5_wifi_is_known(const char* ssid);
// A network guardada numero 'i' (0..n_conhecidas-1), com a password em text (so RAM;
// vazia = network open). Serve a who quer COPIAR as networks (o Mesh traz as of
// Tab5 para o Wi-Fi of no). false = indice fora of lista.
bool     tab5_wifi_saved_network(int i, char* ssid, size_t size_ssid, char* password, size_t size_senha);
// A mesma cifra of wifi.txt (AES com a switch_number of aparelho), para outros
// arquivos of credenciais guardarem password sem text puro no cartao.
// 'hex' precisa of TAB5_WIFI_CIPHER_HEX bytes.
#define TAB5_WIFI_CIPHER_HEX 200
bool     tab5_wifi_encrypt_text(const char* plaintext, char* hex, size_t hex_size);
bool     tab5_wifi_decrypt_text(const char* hex, char* plaintext, size_t plaintext_size);
uint32_t tab5_wifi_networks_version(void);   // muda a cada procura ou network guardada/esquecida

// Conecta numa network JA CONHECIDA (gravada com tab5_wifi_save_credentials).
void     tab5_wifi_connect(const char* ssid);
// Tira a network of wifi.txt (mesma porta e mesmas regras of write acima).
bool     tab5_wifi_forget(const char* ssid);

// Texto curto para a barra of status ("WiFi", "conectando", o SSID...).
const char* tab5_wifi_description(void);

const char* tab5_wifi_ssid(void);
const char* tab5_wifi_ip(void);

// Sinal indisponivel: sem connection, ou o coprocessador nao preencheu o field.
// NAO use 0 como "sem signal" - zero e one value of RSSI valido in teoria e
// significaria signal maximum, o oposto of pretendido.
#define TAB5_WIFI_RSSI_UNKNOWN  (-127)

// TAB5_WIFI_RSSI_UNKNOWN when nao ha connection. Le of cache alimentado pela tarefa of service:
// esp_wifi_sta_get_ap_info() e uma ida on C6 pelo SDIO, cara demais para
// chamar a cada redraw of barra.
int8_t tab5_wifi_rssi(void);

// Caminho of file of credenciais, para o app Ajustes mostrar when faltar.
#define TAB5_WIFI_FILE "/sdcard/apps/Sistema/wifi.txt"
