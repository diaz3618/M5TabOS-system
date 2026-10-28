#include "tab5_mesh_mqtt.hpp"
#include "tab5_mutex.hpp"
#include "mesh_pb.hpp"   // Mesh service codec (reading and writing)
#include "tab5_time.hpp"  // tab5_time_epoch

#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/aes.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include <cstring>
#include <cstdio>
#include <ctime>

static const char* TAG = "mesh_mqtt";

// ---------------------------------------------------------------------------
// CONFIGURACAO (aprendida of no, guardada in NVS)
// ---------------------------------------------------------------------------
#define MAX_CANAIS 8

typedef struct {
  uint8_t papel;         // 0 disabled, 1 primario, 2 secundario (Channel.Role)
  char    name[12];      // o name of canal e no maximum 11 bytes no Meshtastic
  uint8_t n_psk;
  uint8_t psk[32];
} canal_t;

typedef struct {
  canal_t canais[MAX_CANAIS];
  int32_t regiao;        // Config.LoRaConfig.RegionCode; 0 = nao sabe
  char    srv[64], usr[32], pwd[32], raiz[48];
} cfg_t;

static cfg_t             s_cfg;
static bool              s_cfg_lido = false;
static SemaphoreHandle_t s_trava_cfg = nullptr;

#define NVS_ESPACO "mesh"
#define NVS_CHAVE  "mqtt_cfg"

// Servidor publico oficial of Meshtastic, com as credenciais default publicadas
// pelo projeto. A raiz oficial default e "msh".
#define SRV_PADRAO "mqtt.meshtastic.org"
#define USR_PADRAO "meshdev"
#define PWD_PADRAO "large4cats"

// Config.LoRaConfig.RegionCode, in order of protobuf (meshtastic/config.proto).
static const char* const REGIOES[] = {
  "UNSET", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN",
  "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923",
  "PH_433", "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902",
};

static void lock_cfg(void)
{
  tab5_mutex_lock(&s_trava_cfg);
}
static void destravar_cfg(void) { xSemaphoreGive(s_trava_cfg); }

// Com a trava tomada.
static void garantir_cfg(void)
{
  if (s_cfg_lido) { return; }
  s_cfg_lido = true;
  memset(&s_cfg, 0, sizeof(s_cfg));
  nvs_handle_t h;
  if (nvs_open(NVS_ESPACO, NVS_READONLY, &h) == ESP_OK) {
    size_t size = sizeof(s_cfg);
    if (nvs_get_blob(h, NVS_CHAVE, &s_cfg, &size) != ESP_OK || size != sizeof(s_cfg)) {
      memset(&s_cfg, 0, sizeof(s_cfg));
    }
    nvs_close(h);
  }
  // Nenhum canal aprendido: o default of fabrica of todo no, LongFast com a
  // chave default (psk of 1 byte = 1).
  bool algum = false;
  for (int i = 0; i < MAX_CANAIS; ++i) { if (s_cfg.canais[i].papel) { algum = true; } }
  if (!algum) {
    s_cfg.canais[0].papel = 1;
    snprintf(s_cfg.canais[0].name, sizeof(s_cfg.canais[0].name), "LongFast");
    s_cfg.canais[0].n_psk = 1;
    s_cfg.canais[0].psk[0] = 1;
  }
}

// Com a trava tomada.
static void gravar_cfg(void)
{
  nvs_handle_t h;
  if (nvs_open(NVS_ESPACO, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_blob(h, NVS_CHAVE, &s_cfg, sizeof(s_cfg));
  nvs_commit(h);
  nvs_close(h);
}

void mesh_mqtt_aprender_canal(int idx, int papel, const char* name, const uint8_t* psk, size_t n_psk)
{
  if (idx < 0 || idx >= MAX_CANAIS) { return; }
  canal_t novo = { };
  novo.papel = (uint8_t)papel;
  snprintf(novo.name, sizeof(novo.name), "%s", name ? name : "");
  novo.n_psk = (uint8_t)(n_psk > sizeof(novo.psk) ? sizeof(novo.psk) : n_psk);
  if (psk && novo.n_psk) { memcpy(novo.psk, psk, novo.n_psk); }
  lock_cfg();
  garantir_cfg();
  if (memcmp(&s_cfg.canais[idx], &novo, sizeof(novo)) != 0) {
    s_cfg.canais[idx] = novo;
    gravar_cfg();
    ESP_LOGI(TAG, "channel %d learned from the node: \"%s\" (%u-byte key)", idx, novo.name, (unsigned)novo.n_psk);
  }
  destravar_cfg();
}

void mesh_mqtt_aprender_regiao(int regiao)
{
  lock_cfg();
  garantir_cfg();
  if (s_cfg.regiao != regiao) { s_cfg.regiao = regiao; gravar_cfg(); }
  destravar_cfg();
}

void mesh_mqtt_aprender_servidor(const char* endereco, const char* user,
                                 const char* senha, const char* raiz)
{
  cfg_t c;
  lock_cfg();
  garantir_cfg();
  c = s_cfg;
  snprintf(c.srv,  sizeof(c.srv),  "%s", endereco ? endereco : "");
  snprintf(c.usr,  sizeof(c.usr),  "%s", user ? user : "");
  snprintf(c.pwd,  sizeof(c.pwd),  "%s", senha ? senha : "");
  snprintf(c.raiz, sizeof(c.raiz), "%s", raiz ? raiz : "");
  if (memcmp(&c, &s_cfg, sizeof(c)) != 0) { s_cfg = c; gravar_cfg(); ESP_LOGI(TAG, "servidor aprendido do no"); }
  destravar_cfg();
}

// Raiz oficial of servidor default (ver SRV_PADRAO).
#define RAIZ_PADRAO "msh"

// "msh/BR_902": a raiz que o no usa. O firmware poe a regiao in raiz when a
// regiao e escolhida; se o no mandou uma raiz, vale ela.
static void raiz_efetiva(char* out, size_t size)
{
  // Sem servidor aprendido of no vale o default, com a raiz dele.
  if (!s_cfg.srv[0]) { snprintf(out, size, "%s", RAIZ_PADRAO); return; }
  if (s_cfg.raiz[0]) { snprintf(out, size, "%s", s_cfg.raiz); return; }
  const int r = s_cfg.regiao;
  if (r > 0 && r < (int)(sizeof(REGIOES) / sizeof(REGIOES[0]))) { snprintf(out, size, "msh/%s", REGIOES[r]); }
  else { snprintf(out, size, "%s", RAIZ_PADRAO); }
}

// ---------------------------------------------------------------------------
// CRIPTOGRAFIA DO CANAL - AES-CTR, a mesma of firmware (CryptoEngine.cpp)
// ---------------------------------------------------------------------------
// psk of 1 byte e one INDICE: 0 = sem cifra, 1 = a chave default, N = a default
// com o ultimo byte somado of N-1. 16 ou 32 bytes = AES-128/256 com ela.
static const uint8_t CHAVE_PADRAO[16] = {
  0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static size_t chave_of_canal(const canal_t& c, uint8_t chave[32])
{
  if (c.n_psk == 1) {
    if (c.psk[0] == 0) { return 0; }
    memcpy(chave, CHAVE_PADRAO, 16);
    chave[15] = (uint8_t)(chave[15] + c.psk[0] - 1);
    return 16;
  }
  if (c.n_psk == 16 || c.n_psk == 32) { memcpy(chave, c.psk, c.n_psk); return c.n_psk; }
  return 0;
}

// O "channel" of one pacote cifrado nao e o indice, e este hash: XOR dos bytes
// of name com o XOR dos bytes of chave.
static uint8_t hash_of_canal(const canal_t& c)
{
  uint8_t h = 0, chave[32];
  for (const char* p = c.name; *p; ++p) { h ^= (uint8_t)*p; }
  const size_t n = chave_of_canal(c, chave);
  for (size_t i = 0; i < n; ++i) { h ^= chave[i]; }
  return h;
}

// Nonce: id of pacote (64 bits, LE) + no of origem (32 bits, LE) + zeros. O
// contador of CTR anda nos ultimos bytes - igual para cifrar e decifrar.
static bool cifrar(const canal_t& c, uint32_t id, uint32_t of, const uint8_t* in, size_t n, uint8_t* out)
{
  uint8_t chave[32];
  const size_t nk = chave_of_canal(c, chave);
  if (nk == 0) { memcpy(out, in, n); return true; }
  uint8_t nonce[16] = { }, bloco[16] = { };
  const uint64_t id64 = id;
  memcpy(nonce, &id64, 8);
  memcpy(nonce + 8, &of, 4);
  size_t desl = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, chave, (unsigned)(nk * 8)) == 0
         && mbedtls_aes_crypt_ctr(&aes, n, &desl, nonce, bloco, in, out) == 0;
  mbedtls_aes_free(&aes);
  return ok;
}

// ---------------------------------------------------------------------------
// ESTADO DO ENLACE
// ---------------------------------------------------------------------------
#define QUADRO_MAX 600
typedef struct { uint16_t n; uint8_t d[QUADRO_MAX]; } frame_t;

static esp_mqtt_client_handle_t s_cli = nullptr;
static QueueHandle_t   s_fila = nullptr;
static volatile bool   s_conectado = false, s_conectou_now = false;
static uint32_t        s_num = 0;
static char            s_curto[8] = "", s_longo[40] = "";
static char            s_descr[96] = "Internet: stopped";

// PROXY (ver o .hpp). O que o no disse of propria configuracao MQTT vem of
// dump e mora so em RAM; a raiz e aprendida of PRIMEIRO topico que o no
// publica - e a fonte mais certa, porque e literalmente o que ele usa.
static volatile bool s_proxy = false;   // o cliente of pe e o of proxy
static bool     s_no_mqtt = false, s_no_proxy = false, s_no_tls = false;
static bool     s_downlink[MAX_CANAIS];
static char     s_px_raiz[48] = "";       // a assinada now ("" = none)
static char     s_px_raiz_no[48] = "";    // a aprendida of topico publicado pelo no
static uint32_t s_px_subiu = 0, s_px_desceu = 0, s_px_perdeu = 0;

// Envio publicado com QoS 1 -> o PUBACK of servidor vira o "confirmado" of screen.
#define PENDENTES 8
typedef struct { int msg; uint32_t id; } pend_t;
static pend_t s_pend[PENDENTES];
static int s_prox_pend = 0;

// Duas tarefas entregam (a of cliente MQTT e a of servico, respondendo on
// want_config): o frame of montagem e unico e estatico (600 bytes in stack
// of servico pesariam), entao passa por uma trava.
static SemaphoreHandle_t s_trava_ent = nullptr;
static SemaphoreHandle_t s_trava_env = nullptr;   // ver mesh_mqtt_enviar
// FILA CHEIA: one aviso a cada 10 s, com o total e QUAL fila - one por pacote
// inundava o log numa rajada, e "queue cheia" sozinho nao dizia of que rede.
static QueueHandle_t s_rede_internet_fila_ref = nullptr;   // ver s_rede_internet_fila, mais abaixo
static uint32_t s_perdidos[2] = { 0, 0 };        // 0 = no (internet/proxy), 1 = rede publica
static void perdeu(QueueHandle_t fila)
{
  const int k = (fila && fila == s_rede_internet_fila_ref) ? 1 : 0;
  ++s_perdidos[k];
  static int64_t ultimo_us[2] = { 0, 0 };
  const int64_t now = esp_timer_get_time();
  if (now - ultimo_us[k] >= 10000000) {
    ultimo_us[k] = now;
    ESP_LOGW(TAG, "queue %s full - %lu packet(s) discarded since boot",
             k ? "internet" : "no", (unsigned long)s_perdidos[k]);
  }
}

// MONTAGEM SEM PILHA. As tarefas dos clientes MQTT tem stack justa (3,5 KB
// no of internet, pico measured of 2,6 KB) e as duas chegam aqui: o frame e
// montado DIRETO no frame estatico que vai para a fila, sob s_trava_ent -
// nenhum buffer of 600 bytes in stack of ninguem.
static frame_t s_q;                     // o que vai para a fila (xQueueSend copia)
static uint8_t  s_pacote[QUADRO_MAX];    // MeshPacket em montagem (entregar_pacote)

// Com s_trava_ent tomada.
static bool enfileirar(QueueHandle_t fila, size_t n)
{
  if (!n) { return false; }
  s_q.n = (uint16_t)n;
  const bool ok = xQueueSend(fila, &s_q, 0) == pdTRUE;
  if (!ok) { perdeu(fila); }
  return ok;
}

static bool entregar_em(QueueHandle_t fila, const uint8_t* d, size_t n)
{
  if (!fila || !s_trava_ent || n > QUADRO_MAX) { return false; }
  xSemaphoreTake(s_trava_ent, portMAX_DELAY);
  memcpy(s_q.d, d, n);
  const bool ok = enfileirar(fila, n);
  xSemaphoreGive(s_trava_ent);
  return ok;
}
static bool entregar(const uint8_t* d, size_t n) { return entregar_em(s_fila, d, n); }

// FromRadio{ field: body } para o servico, como o radio entregaria.
static bool entregar_from_radio(QueueHandle_t fila, int field, const pb_escritor_t* body)
{
  if (!fila || !s_trava_ent) { return false; }
  xSemaphoreTake(s_trava_ent, portMAX_DELAY);
  pb_escritor_t f = pb_escritor(s_q.d, sizeof(s_q.d));
  pb_w_sub(&f, field, body);
  const bool ok = enfileirar(fila, pb_tamanho(&f));
  xSemaphoreGive(s_trava_ent);
  return ok;
}

// UM PACOTE COMO O RADIO O ENTREGARIA, a partir of one Data ja montado:
// FromRadio{ 2: packet MeshPacket{ 1: from, 2: to, 4: decoded, 6: id,
// 7: rx_time, 14: via_mqtt } }. 'da_internet' carimba a hora of chegada e o
// via_mqtt (o que veio of servidor); a confirmacao of PUBACK vai sem os dois,
// como a of radio.
static bool entregar_pacote(QueueHandle_t fila, uint32_t of, uint32_t para,
                            const pb_escritor_t* data, bool of_internet)
{
  if (!fila || !s_trava_ent) { return false; }
  xSemaphoreTake(s_trava_ent, portMAX_DELAY);
  pb_escritor_t w = pb_escritor(s_pacote, sizeof(s_pacote));
  pb_w_fixo32(&w, 1, of);
  pb_w_fixo32(&w, 2, para);
  pb_w_sub(&w, 4, data);
  pb_w_fixo32(&w, 6, esp_random() | 1u);
  if (of_internet) {
    if (const uint32_t now = tab5_time_epoch()) { pb_w_fixo32(&w, 7, now); }
    pb_w_num(&w, 14, 1);
  }
  pb_escritor_t f = pb_escritor(s_q.d, sizeof(s_q.d));
  pb_w_sub(&f, 2, &w);
  const bool ok = enfileirar(fila, pb_tamanho(&f));
  xSemaphoreGive(s_trava_ent);
  return ok;
}

// ---------------------------------------------------------------------------
// A REDE PUBLICA PELA INTERNET - one segundo cliente, em paralelo (ver o .hpp)
// ---------------------------------------------------------------------------
// O servidor publico of projeto, com a raiz of regiao US e o LongFast of
// fabrica (chave default). O resto of enlace e o mesmo of modo internet: a
// mesma decifragem, o mesmo envelope, a mesma confirmacao pelo PUBACK - so a
// fila e outra, e o servico marca o que exit dela como of rede of internet.
#define INTERNET_SRV  "mqtt.meshtastic.org"
#define INTERNET_RAIZ "msh/US"
static const canal_t CANAL_INTERNET = { 1, "LongFast", 1, { 1 } };

static esp_mqtt_client_handle_t s_rede_internet_cli = nullptr;
static QueueHandle_t   s_rede_internet_fila = nullptr;
static volatile bool   s_rede_internet_conectado = false;
static pend_t          s_rede_internet_pend[8];
static int             s_rede_internet_prox_pend = 0;
static uint32_t        s_rede_internet_recebidos = 0;

static int canal_por_name(const char* name, size_t n)
{
  for (int i = 0; i < MAX_CANAIS; ++i) {
    const canal_t& c = s_cfg.canais[i];
    if (c.papel && strlen(c.name) == n && memcmp(c.name, name, n) == 0) { return i; }
  }
  return -1;
}

// Relatorio of mapa (<raiz>/2/map/): vai sem cifra e traz name, papel, modelo e
// position of no - o jeito of conhecer os nos que nao mandam NodeInfo pelo canal.
// Vira one NodeInfo e uma Position, como se o radio os tivesse ouvido.
static void processar_mapa(const uint8_t* d, size_t n)
{
  pb_leitor_t l = pb_leitor(d, n);
  pb_field_t c;
  const uint8_t* mp = nullptr; size_t n_mp = 0;
  while (pb_field(&l, &c)) { if (c.num == 1 && c.type == 2) { mp = c.val; n_mp = c.n_val; } }
  if (!mp) { return; }
  uint32_t of = 0;
  const uint8_t* dec = nullptr; size_t n_dec = 0;
  l = pb_leitor(mp, n_mp);
  while (pb_field(&l, &c)) {
    if (c.num == 1 && c.type == 5) { of = (uint32_t)c.v; }
    else if (c.num == 4 && c.type == 2) { dec = c.val; n_dec = c.n_val; }
  }
  if (!dec || !of || of == s_num) { return; }
  const uint8_t* rel = nullptr; size_t n_rel = 0;
  uint64_t porta = 0;
  l = pb_leitor(dec, n_dec);
  while (pb_field(&l, &c)) {
    if (c.num == 1 && c.type == 0) { porta = c.v; }
    else if (c.num == 2 && c.type == 2) { rel = c.val; n_rel = c.n_val; }
  }
  if (porta != 73 || !rel) { return; }   // MAP_REPORT_APP

  const uint8_t* longo = nullptr; size_t n_longo = 0;
  const uint8_t* curto = nullptr; size_t n_curto = 0;
  uint64_t papel = 0, modelo = 0;
  uint32_t lat = 0, lon = 0;
  l = pb_leitor(rel, n_rel);
  while (pb_field(&l, &c)) {
    if      (c.num == 1  && c.type == 2) { longo = c.val; n_longo = c.n_val; }
    else if (c.num == 2  && c.type == 2) { curto = c.val; n_curto = c.n_val; }
    else if (c.num == 3  && c.type == 0) { papel = c.v; }
    else if (c.num == 4  && c.type == 0) { modelo = c.v; }
    else if (c.num == 9  && c.type == 5) { lat = (uint32_t)c.v; }
    else if (c.num == 10 && c.type == 5) { lon = (uint32_t)c.v; }
  }
  if (!n_longo && !n_curto) { return; }

  char id[12];
  snprintf(id, sizeof(id), "!%08lx", (unsigned long)of);

  // NodeInfo: Data{ 1: NODEINFO_APP, 2: User{ 1: id, 2: long_name,
  // 3: short_name, 5: hw_model, 7: role } }
  static uint8_t user[128], data[160];   // so o cliente of no le mapa: estatico, fora of stack
  pb_escritor_t wu = pb_escritor(user, sizeof(user));
  pb_w_text(&wu, 1, id, sizeof(id));
  if (n_longo) { pb_w_bytes(&wu, 2, longo, n_longo); }
  if (n_curto) { pb_w_bytes(&wu, 3, curto, n_curto); }
  if (modelo)  { pb_w_num(&wu, 5, modelo); }
  if (papel)   { pb_w_num(&wu, 7, papel); }
  pb_escritor_t wd = pb_escritor(data, sizeof(data));
  pb_w_num(&wd, 1, 4);
  pb_w_sub(&wd, 2, &wu);
  entregar_pacote(s_fila, of, MESH_PB_DIFUSAO, &wd, true);

  // Position: Data{ 1: POSITION_APP, 2: Position{ 1: lat, 2: lon } }
  if (lat || lon) {
    uint8_t pos[24];
    pb_escritor_t wp = pb_escritor(pos, sizeof(pos));
    pb_w_fixo32(&wp, 1, lat);
    pb_w_fixo32(&wp, 2, lon);
    wd = pb_escritor(data, sizeof(data));
    pb_w_num(&wd, 1, 3);
    pb_w_sub(&wd, 2, &wp);
    entregar_pacote(s_fila, of, MESH_PB_DIFUSAO, &wd, true);
  }
}

// ServiceEnvelope{1: packet, 2: channel_id, 3: gateway_id} -> FromRadio.
// Anel of (origem ^ id) ja vistos, one por cliente (cada one so e mexido pela
// tarefa dele). O of internet e maior: ~40 pacotes novos por segundo la, e as
// copias chegam on longo of alguns segundos.
#define VISTOS_NO  128
#define VISTOS_INTERNET 512
static uint32_t s_vistos_no[VISTOS_NO], s_vistos_internet[VISTOS_INTERNET];
static int      s_prox_visto[2] = { 0, 0 };

static bool ja_visto(int k, uint32_t of, uint32_t id)
{
  if (id == 0) { return false; }
  const uint32_t ch = (of * 2654435761u) ^ id;
  uint32_t* v = k ? s_vistos_internet : s_vistos_no;
  const int n = k ? VISTOS_INTERNET : VISTOS_NO;
  for (int i = 0; i < n; ++i) { if (v[i] == ch) { return true; } }
  v[s_prox_visto[k]] = ch;
  s_prox_visto[k] = (s_prox_visto[k] + 1) % n;
  return false;
}

static void processar(const char* topico, int n_top, const uint8_t* d, size_t n,
                      QueueHandle_t fila, bool internet)
{
  for (int i = 0; i + 7 <= n_top; ++i) {
    if (memcmp(topico + i, "/2/map/", 7) == 0) { if (!internet) { processar_mapa(d, n); } return; }
  }
  // O canal pelo topico: <raiz>/2/e/<canal>/!<gateway>
  const char* e = nullptr;
  for (int i = 0; i + 5 <= n_top; ++i) { if (memcmp(topico + i, "/2/e/", 5) == 0) { e = topico + i + 5; } }
  if (!e) { return; }
  const char* end_name = e;
  while (end_name < topico + n_top && *end_name != '/') { ++end_name; }

  // UM PAR DE BUFFERS POR CLIENTE. As tarefas of cliente of no e of cliente
  // of internet chegam aqui AO MESMO TEMPO (cada uma com o seu pacote); com one
  // par so, uma decifrava por top of pacote que a outra montava. Estatico,
  // e nao stack: a of cliente of internet tem 3,5 KB.
  static uint8_t s_claro[2][QUADRO_MAX], s_pkt[2][QUADRO_MAX];
  uint8_t* const claro = s_claro[internet ? 1 : 0];
  uint8_t* const pkt   = s_pkt[internet ? 1 : 0];
  int idx;
  canal_t canal;
  if (internet) {
    // Na rede of internet so o LongFast of fabrica: os canais aprendidos of no sao daqui.
    const size_t nn = (size_t)(end_name - e);
    idx = (nn == strlen(CANAL_INTERNET.name) && memcmp(e, CANAL_INTERNET.name, nn) == 0) ? 0 : -1;
    canal = CANAL_INTERNET;
  } else {
    lock_cfg();
    idx = canal_por_name(e, (size_t)(end_name - e));
    canal = idx >= 0 ? s_cfg.canais[idx] : canal_t{ };
    destravar_cfg();
  }
  if (idx < 0) { return; }

  pb_leitor_t l = pb_leitor(d, n);
  pb_field_t c;
  const uint8_t* mp = nullptr; size_t n_mp = 0;
  while (pb_field(&l, &c)) { if (c.num == 1 && c.type == 2) { mp = c.val; n_mp = c.n_val; } }
  if (!mp) { return; }

  uint32_t of = 0, id = 0;
  const uint8_t* cif = nullptr; size_t n_cif = 0;
  const uint8_t* dec = nullptr; size_t n_dec = 0;
  l = pb_leitor(mp, n_mp);
  while (pb_field(&l, &c)) {
    if      (c.num == 1 && c.type == 5) { of = (uint32_t)c.v; }
    else if (c.num == 6 && c.type == 5) { id = (uint32_t)c.v; }
    else if (c.num == 5 && c.type == 2) { cif = c.val; n_cif = c.n_val; }
    else if (c.num == 4 && c.type == 2) { dec = c.val; n_dec = c.n_val; }
  }
  if (of == s_num && s_num) { return; }   // o nosso proprio send_state voltando pelo servidor
  // O MESMO PACOTE DE VARIOS GATEWAYS: cada no com MQTT que o ouviu publica a
  // sua copia. No servidor publico sao 3 of cada 4 mensagens (measured: 153/s,
  // 75% repetidas) - e cada uma era decifrada, enfileirada e lida pelo
  // servico a toa. Corta aqui, antes of AES, pelo par (origem, id).
  if (ja_visto(internet ? 1 : 0, of, id)) { return; }
  if (cif) {
    if (n_cif > QUADRO_MAX || !cifrar(canal, id, of, cif, n_cif, claro)) { return; }
    // Chave errada (ou message direta com PKI) of lixo: one Data of verdade
    // comeca pelo field 1 (portnum, varint).
    if (n_cif == 0 || claro[0] != 0x08) { return; }
    dec = claro; n_dec = n_cif;
  }
  if (!dec) { return; }
  // DA INTERNET SO TEXTO (1) E APRESENTACAO (4, o name of quem fala). O resto -
  // position, telemetria, traceroute - e a maior parte of LongFast of la, nao
  // aparece em lugar nenhum daqui e enchia a fila. Data comeca pelo portnum
  // (0x08 <varint>), e as duas portas cabem num byte.
  if (internet && n_dec >= 2 && dec[1] != 1 && dec[1] != 4) { return; }

  // MeshPacket of novo, como o radio entregaria: o resto dos campos cru, o
  // canal como INDICE, o Data decifrado, a hora of chegada e via_mqtt.
  pb_escritor_t w = pb_escritor(pkt, QUADRO_MAX);
  l = pb_leitor(mp, n_mp);
  while (pb_field(&l, &c)) {
    if (c.num == 3 || c.num == 4 || c.num == 5 || c.num == 7 || c.num == 14) { continue; }
    pb_w_cru(&w, c.ini, c.n_total);
  }
  pb_w_num(&w, 3, (uint64_t)idx);
  pb_w_bytes(&w, 4, dec, n_dec);
  if (const uint32_t now = tab5_time_epoch()) { pb_w_fixo32(&w, 7, now); }
  pb_w_num(&w, 14, 1);                 // via_mqtt (o 13 e o obsoleto 'delayed')
  if (entregar_from_radio(fila, 2, &w) && internet) { ++s_rede_internet_recebidos; }   // FromRadio.packet
}

// O PUBACK of nosso send_state: one Routing com erro 0 para o id, como o radio
// manda when one vizinho repete o pacote - aqui quer dizer "o servidor
// recebeu".
static void confirm(uint32_t id, QueueHandle_t fila)
{
  uint8_t data[24];
  pb_escritor_t d = pb_escritor(data, sizeof(data));
  pb_w_num(&d, 1, 5);                                // portnum ROUTING_APP
  const uint8_t routing[] = { 0x18, 0x00 };          // Routing{ 3: error_reason = NONE }
  pb_w_bytes(&d, 2, routing, sizeof(routing));
  pb_w_fixo32(&d, 6, id);                            // request_id
  entregar_pacote(fila, s_num, s_num, &d, false);
}

static void assinar(void)
{
  char raiz[48], topico[96];
  lock_cfg();
  raiz_efetiva(raiz, sizeof(raiz));
  for (int i = 0; i < MAX_CANAIS; ++i) {
    if (!s_cfg.canais[i].papel || !s_cfg.canais[i].name[0]) { continue; }
    snprintf(topico, sizeof(topico), "%s/2/e/%s/#", raiz, s_cfg.canais[i].name);
    esp_mqtt_client_subscribe(s_cli, topico, 0);
    ESP_LOGI(TAG, "assinando %s", topico);
    // Sub-raizes (msh/BR/SP/2/e/...): one level abaixo, onde os estados e
    // cidades publicam. Nao tem cifra diferente: e o mesmo canal.
    snprintf(topico, sizeof(topico), "%s/+/2/e/%s/#", raiz, s_cfg.canais[i].name);
    esp_mqtt_client_subscribe(s_cli, topico, 0);
    snprintf(topico, sizeof(topico), "%s/+/+/2/e/%s/#", raiz, s_cfg.canais[i].name);   // meshdev/<pais>/<regiao>/2/e/...
    esp_mqtt_client_subscribe(s_cli, topico, 0);
  }
  // Relatorios of mapa: nomes e posicoes of nos que nunca mandaram NodeInfo.
  snprintf(topico, sizeof(topico), "%s/2/map/", raiz);
  esp_mqtt_client_subscribe(s_cli, topico, 0);
  snprintf(topico, sizeof(topico), "%s/+/2/map/", raiz);
  esp_mqtt_client_subscribe(s_cli, topico, 0);
  snprintf(topico, sizeof(topico), "%s/+/+/2/map/", raiz);
  esp_mqtt_client_subscribe(s_cli, topico, 0);
  destravar_cfg();
}

// AS ASSINATURAS DO PROXY: as mesmas of app of celular - cada canal com
// downlink enabled no no, mais o topico PKI das mensagens diretas (o no
// decifra; para o Tab5 sao bytes). Canal sem downlink nao e assinado: o no
// descartaria o que viesse dele, e o trafego entraria a toa.
static void assinar_proxy(const char* raiz, bool desfazer)
{
  char topico[96];
  lock_cfg();
  for (int i = 0; i < MAX_CANAIS; ++i) {
    if (!s_cfg.canais[i].papel || !s_cfg.canais[i].name[0] || !s_downlink[i]) { continue; }
    snprintf(topico, sizeof(topico), "%s/2/e/%s/+", raiz, s_cfg.canais[i].name);
    if (desfazer) { esp_mqtt_client_unsubscribe(s_cli, topico); }
    else          { esp_mqtt_client_subscribe(s_cli, topico, 0); ESP_LOGI(TAG, "proxy assinando %s", topico); }
  }
  destravar_cfg();
  snprintf(topico, sizeof(topico), "%s/2/e/PKI/+", raiz);
  if (desfazer) { esp_mqtt_client_unsubscribe(s_cli, topico); }
  else          { esp_mqtt_client_subscribe(s_cli, topico, 0); }
}

// A raiz of proxy: a que o no usa of fato (aprendida of topico), senao a of
// dump, senao "msh" - o default of firmware.
static void raiz_proxy(char* out, size_t size)
{
  if (s_px_raiz_no[0]) { snprintf(out, size, "%s", s_px_raiz_no); return; }
  lock_cfg();
  snprintf(out, size, "%s", s_cfg.raiz[0] ? s_cfg.raiz : "msh");
  destravar_cfg();
}

static void assinar_proxy_now(void)
{
  char raiz[48];
  raiz_proxy(raiz, sizeof(raiz));
  if (s_px_raiz[0] && strcmp(s_px_raiz, raiz) != 0) { assinar_proxy(s_px_raiz, true); }
  snprintf(s_px_raiz, sizeof(s_px_raiz), "%s", raiz);
  assinar_proxy(raiz, false);
}

// DESCIDA: o que chegou of servidor vira ToRadio{ 6: MqttClientProxyMessage
// { 1 topic, 2 data, 4 retained } } e vai para a fila; o servico entrega on
// no. 512 e o teto of one ToRadio no firmware (MAX_TO_FROM_RADIO_SIZE), pelo
// cabo e pelo Bluetooth.
static void proxy_lower(const char* topico, int n_top, const uint8_t* d, size_t n, bool retido)
{
  static uint8_t msg[QUADRO_MAX], saida[QUADRO_MAX];   // so a tarefa of cliente MQTT desce
  pb_escritor_t m = pb_escritor(msg, sizeof(msg));
  pb_w_bytes(&m, 1, topico, (size_t)n_top);
  pb_w_bytes(&m, 2, d, n);
  if (retido) { pb_w_num(&m, 4, 1); }
  pb_escritor_t f = pb_escritor(saida, 512);
  pb_w_sub(&f, 6, &m);
  const size_t k = pb_tamanho(&f);
  if (k && entregar(saida, k)) { ++s_px_desceu; } else { ++s_px_perdeu; }
}

static void on_evento(void*, esp_event_base_t, int32_t id_ev, void* data)
{
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id_ev) {
    case MQTT_EVENT_CONNECTED:
      if (!s_conectado) { tab5_memory_marco("MQTT conectado"); }
      s_conectado = true;
      s_conectou_now = true;
      if (s_proxy) { s_px_raiz[0] = 0; assinar_proxy_now(); }   // sessao nova: nada assinado
      else         { assinar(); }
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_conectado = false;
      break;
    case MQTT_EVENT_DATA:
      // Pacote em pedacos (maior que o buffer) nao e Meshtastic of verdade.
      if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
        if (s_proxy) { proxy_lower(ev->topic, ev->topic_len, (const uint8_t*)ev->data, (size_t)ev->data_len, ev->retain); }
        else         { processar(ev->topic, ev->topic_len, (const uint8_t*)ev->data, (size_t)ev->data_len, s_fila, false); }
      } else if (s_proxy && ev->current_data_offset == 0) { ++s_px_perdeu; }
      break;
    case MQTT_EVENT_PUBLISHED:
      for (int i = 0; i < PENDENTES; ++i) {
        if (s_pend[i].msg == ev->msg_id && s_pend[i].id) { confirm(s_pend[i].id, s_fila); s_pend[i].id = 0; }
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void mesh_mqtt_set_no(uint32_t num, const char* curto, const char* longo)
{
  s_num = num;
  snprintf(s_curto, sizeof(s_curto), "%s", curto ? curto : "");
  snprintf(s_longo, sizeof(s_longo), "%s", longo ? longo : "");
}

// Os dois papeis sobem pelo mesmo caminho; muda o servidor, a identidade e o
// que se assina (no CONNECTED). Com o cliente of pe no OUTRO papel, recusa:
// quem troca of papel e o laco of servico, stopping antes.
static bool start_cliente(bool proxy)
{
  if (s_cli) { return s_proxy == proxy; }
  // 16 quadros: a press_start of proxy chega em rajada (one servidor publico
  // entrega o canal inteiro), e o enlace com o no esvazia a fila a cada iteration.
  if (!s_fila) { s_fila = xQueueCreateWithCaps(32, sizeof(frame_t), MALLOC_CAP_SPIRAM); }
  tab5_mutex_create(&s_trava_ent);
  tab5_mutex_create(&s_trava_env);
  if (!s_fila || !s_trava_ent || !s_trava_env) { return false; }
  xQueueReset(s_fila);

  static char uri[96], usr[32], pwd[32], raiz[48], srv[64];
  const bool tls = proxy && s_no_tls;
  lock_cfg();
  garantir_cfg();
  // O SERVIDOR: no proxy e o of NO, exatamente - endereco vazio e o default of
  // firmware (mqtt.meshtastic.org, meshdev/large4cats). No modo internet,
  // vazio e o default oficial usado por este sistema.
  const char* default_server = proxy ? "mqtt.meshtastic.org" : SRV_PADRAO;
  snprintf(srv, sizeof(srv), "%s", s_cfg.srv[0] ? s_cfg.srv : default_server);
  // "host" ou "host:porta", como o app of Meshtastic grava.
  snprintf(uri, sizeof(uri), strchr(srv, ':') ? (tls ? "mqtts://%s" : "mqtt://%s") : (tls ? "mqtts://%s:8883" : "mqtt://%s:1883"), srv);
  snprintf(usr, sizeof(usr), "%s", s_cfg.srv[0] ? s_cfg.usr : USR_PADRAO);
  snprintf(pwd, sizeof(pwd), "%s", s_cfg.srv[0] ? s_cfg.pwd : PWD_PADRAO);
  if (proxy) { destravar_cfg(); raiz_proxy(raiz, sizeof(raiz)); lock_cfg(); }
  else       { raiz_efetiva(raiz, sizeof(raiz)); }
  destravar_cfg();

  // Identidade propria of proxy: dois clientes com o mesmo id no servidor
  // derrubariam one on outro, e o modo internet pode ter ficado registrado.
  static char cliente[24];
  snprintf(cliente, sizeof(cliente), proxy ? "tab5px-%08lx" : "tab5-%08lx", (unsigned long)s_num);
  esp_mqtt_client_config_t c = { };
  c.broker.address.uri = uri;
  c.credentials.username = usr;
  c.credentials.authentication.password = pwd;
  c.credentials.client_id = cliente;
  c.buffer.size = 1024;
  // Pico measured of ~2,6 KB com 6 KB (pilhas-*.csv, horas conectado recebendo
  // a malha). Os quadros e a decifragem usam buffers static, nao stack.
  c.task.stack_size = 4096;
  c.network.reconnect_timeout_ms = 15000;
  if (tls) { c.broker.verification.crt_bundle_attach = esp_crt_bundle_attach; }
  s_proxy = proxy;
  s_cli = esp_mqtt_client_init(&c);
  if (!s_cli) { return false; }
  esp_mqtt_client_register_event(s_cli, MQTT_EVENT_ANY, on_evento, nullptr);
  if (esp_mqtt_client_start(s_cli) != ESP_OK) {
    esp_mqtt_client_destroy(s_cli);
    s_cli = nullptr;
    return false;
  }
  tab5_memory_marco("MQTT (cliente + tarefa)");
  snprintf(s_descr, sizeof(s_descr), "%s: %s (%s)", proxy ? "proxy" : "internet", srv, raiz);
  ESP_LOGI(TAG, "conectando a %s, raiz %s, como !%08lx%s", uri, raiz, (unsigned long)s_num,
           proxy ? " - proxy do no" : "");
  return true;
}

bool mesh_mqtt_start(void)       { return start_cliente(false); }
bool mesh_mqtt_start_proxy(void) { return start_cliente(true); }
bool mesh_mqtt_em_proxy(void)      { return s_cli && s_proxy; }

void mesh_mqtt_stop(void)
{
  if (!s_cli) { return; }
  esp_mqtt_client_stop(s_cli);
  esp_mqtt_client_destroy(s_cli);
  s_cli = nullptr;
  s_conectado = false;
  s_proxy = false;
  s_px_raiz[0] = 0;
  if (s_fila) { xQueueReset(s_fila); }   // press_start of proxy para one no que nao vai mais receber
  snprintf(s_descr, sizeof(s_descr), "Internet: stopped");
  ESP_LOGI(TAG, "disconnected from the server");
}

bool mesh_mqtt_active(void)     { return s_cli != nullptr; }
bool mesh_mqtt_conectado(void) { return s_cli && s_conectado; }
bool mesh_mqtt_conectou_now(void)
{
  if (!s_conectou_now) { return false; }
  s_conectou_now = false;
  return true;
}

const char* mesh_mqtt_descricao(void)
{
  static char d[112];
  snprintf(d, sizeof(d), "%s%s", s_descr, !s_cli ? "" : s_conectado ? " - conectado" : " - conectando");
  return d;
}

size_t mesh_mqtt_receber(uint8_t* buf, size_t max, uint32_t espera_ms)
{
  if (!s_fila || s_proxy) { if (espera_ms) { vTaskDelay(pdMS_TO_TICKS(espera_ms)); } return 0; }
  static frame_t q;   // so a tarefa of servico recebe
  if (xQueueReceive(s_fila, &q, pdMS_TO_TICKS(espera_ms)) != pdTRUE) { return 0; }
  const size_t n = q.n < max ? q.n : max;
  memcpy(buf, q.d, n);
  return n;
}

// O want_config of one radio: o que o dump dele traria of essencial - o nosso
// numero, o nosso NodeInfo, os canais e o end of configuracao.
static void responder_config(void)
{
  // FromRadio{ 3: my_info MyNodeInfo{ 1: my_node_num } }
  uint8_t a[160];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, 1, s_num);
  entregar_from_radio(s_fila, 3, &w);

  // FromRadio{ 4: node_info NodeInfo{ 1: num, 2: user User{ 1: id, 2: long, 3: short } } }
  char id[12];
  snprintf(id, sizeof(id), "!%08lx", (unsigned long)s_num);
  uint8_t u[96];
  pb_escritor_t wu = pb_escritor(u, sizeof(u));
  pb_w_text(&wu, 1, id, sizeof(id));
  if (s_longo[0]) { pb_w_text(&wu, 2, s_longo, sizeof(s_longo)); }
  if (s_curto[0]) { pb_w_text(&wu, 3, s_curto, sizeof(s_curto)); }
  w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, 1, s_num);
  pb_w_sub(&w, 2, &wu);
  entregar_from_radio(s_fila, 4, &w);

  lock_cfg();
  garantir_cfg();
  const cfg_t cfg = s_cfg;
  destravar_cfg();
  // FromRadio{ 10: channel Channel{ 1: index, 2: settings{ 2: psk, 3: name }, 3: role } }
  for (int i = 0; i < MAX_CANAIS; ++i) {
    const canal_t& c = cfg.canais[i];
    if (!c.papel) { continue; }
    uint8_t cs[64];
    pb_escritor_t ws = pb_escritor(cs, sizeof(cs));
    if (c.n_psk) { pb_w_bytes(&ws, 2, c.psk, c.n_psk); }
    pb_w_text(&ws, 3, c.name, sizeof(c.name));
    w = pb_escritor(a, sizeof(a));
    pb_w_num(&w, 1, (uint64_t)i);
    pb_w_sub(&w, 2, &ws);
    pb_w_num(&w, 3, c.papel);
    entregar_from_radio(s_fila, 10, &w);
  }

  // FromRadio{ 7: config_complete_id = 42 } - o mesmo id que o servico pediu.
  uint8_t b[8];
  pb_escritor_t f = pb_escritor(b, sizeof(b));
  pb_w_num(&f, 7, 42);
  entregar(b, pb_tamanho(&f));
}

// Chamada of tarefa of servico (want_config, batimento) E of of interface
// (o app enviando): os buffers of montagem sao estaticos, uma of cada vez.
static bool send_travado(const uint8_t* toradio, size_t n);
static bool publicar_data(esp_mqtt_client_handle_t cli, pend_t* pend, int* prox, int n_pend,
                          const canal_t& canal, const char* raiz, uint32_t para,
                          const uint8_t* dec, size_t n_dec, uint32_t id, uint64_t saltos);

// O QUE O SERVICO MANDA, lido of iteration: ToRadio{ 1: packet MeshPacket{ 2: to,
// 3: channel, 4: decoded, 6: id, 9: hop_limit }, 3: want_config_id }. As
// duas saidas (no e internet) leem o mesmo envelope - one leitor so.
typedef struct {
  bool           quer_config;   // want_config_id: o servico pedindo o "dump"
  uint32_t       para, id;
  uint64_t       channel, saltos;
  const uint8_t* data;          // Data ja serializado (nullptr = sem pacote)
  size_t         n_data;
} to_radio_t;

static void read_to_radio(const uint8_t* b, size_t n, to_radio_t* t)
{
  *t = { false, MESH_PB_DIFUSAO, 0, 0, 3, nullptr, 0 };
  pb_leitor_t l = pb_leitor(b, n);
  pb_field_t c;
  const uint8_t* mp = nullptr; size_t n_mp = 0;
  while (pb_field(&l, &c)) {
    if      (c.num == 1 && c.type == PB_LEN)    { mp = c.val; n_mp = c.n_val; }
    else if (c.num == 3 && c.type == PB_VARINT) { t->quer_config = true; }
  }
  if (!mp) { return; }
  l = pb_leitor(mp, n_mp);
  while (pb_field(&l, &c)) {
    if      (c.num == 2 && c.type == PB_FIXO32) { t->para = (uint32_t)c.v; }
    else if (c.num == 3 && c.type == PB_VARINT) { t->channel = c.v; }
    else if (c.num == 4 && c.type == PB_LEN)    { t->data = c.val; t->n_data = c.n_val; }
    else if (c.num == 6 && c.type == PB_FIXO32) { t->id = (uint32_t)c.v; }
    else if (c.num == 9 && c.type == PB_VARINT) { t->saltos = c.v; }
  }
  if (t->id == 0) { t->id = esp_random() | 1u; }
}

bool mesh_mqtt_send(const uint8_t* toradio, size_t n)
{
  // NO PROXY O CLIENTE E DO NO: nada of Tab5 exit por ele como se fosse one
  // radio (cifrar com a chave of canal e publicar em name of no daria a
  // mesma message duas vezes, when o no publicar a dele).
  if (!s_trava_env || s_proxy) { return false; }
  xSemaphoreTake(s_trava_env, portMAX_DELAY);
  const bool ok = send_travado(toradio, n);
  xSemaphoreGive(s_trava_env);
  return ok;
}

static bool send_travado(const uint8_t* toradio, size_t n)
{
  // SEM CLIENTE NAO HA PARA QUEM RESPONDER. O servico continua pedindo a
  // configuracao (a cada 2 min) mesmo com o Wi-Fi fora; a resposta ia para a
  // fila, que so e lida com o cliente of pe, e enchia ("queue cheia").
  if (!s_cli) { return false; }
  to_radio_t t;
  read_to_radio(toradio, n, &t);
  if (t.quer_config) { responder_config(); return true; }
  if (!t.data) { return true; }                  // batimento e afins: nada a fazer aqui
  if (!mesh_mqtt_conectado() || s_num == 0) { return false; }
  if (t.channel >= MAX_CANAIS) { return false; }
  // MENSAGEM DIRETA NAO SAI POR AQUI. Sem a chave publica (PKI) so daria para
  // cifrar com a chave of CANAL, que todo assinante of topico tem: a conversa
  // particular apareceria para a malha inteira (e no bot of Telegram). Melhor
  // recusar - o send_state falha in screen - of que vazar.
  if (t.para != MESH_PB_DIFUSAO) {
    ESP_LOGW(TAG, "direct message rejected: Internet supports channel messages only (PKI unavailable)");
    return false;
  }

  lock_cfg();
  const canal_t canal = s_cfg.canais[t.channel];
  char raiz[48];
  raiz_efetiva(raiz, sizeof(raiz));
  destravar_cfg();
  if (!canal.papel) { return false; }
  return publicar_data(s_cli, s_pend, &s_prox_pend, PENDENTES, canal, raiz, t.para, t.data, t.n_data, t.id, t.saltos);
}

// Data -> cifra -> MeshPacket -> ServiceEnvelope -> publica em
// <raiz>/2/e/<canal>/!<no>. Os dois clientes saem por aqui (buffers estaticos:
// com s_trava_env tomada).
static bool publicar_data(esp_mqtt_client_handle_t cli, pend_t* pend, int* prox, int n_pend,
                          const canal_t& canal, const char* raiz, uint32_t para,
                          const uint8_t* dec, size_t n_dec, uint32_t id, uint64_t saltos)
{
  // Data + bitfield OK_TO_MQTT (field 9, bit 0): sem ele os gateways que
  // respeitam a privacidade nao descem a message para o radio.
  static uint8_t data[QUADRO_MAX], cif[QUADRO_MAX], pkt[QUADRO_MAX], env[QUADRO_MAX];
  pb_escritor_t wdata = pb_escritor(data, sizeof(data));
  pb_w_cru(&wdata, dec, n_dec);
  pb_w_num(&wdata, 9, 1);                        // bitfield: OK_TO_MQTT
  const size_t n_data = pb_tamanho(&wdata);
  if (!n_data) { return false; }
  uint8_t chave[32];
  const bool cifrado = chave_of_canal(canal, chave) > 0;
  if (!cifrar(canal, id, s_num, data, n_data, cif)) { return false; }

  // MeshPacket{ 1: from, 2: to, 3: channel (hash), 4: decoded | 5: encrypted,
  // 6: id, 9: hop_limit, 15: hop_start }
  pb_escritor_t w = pb_escritor(pkt, sizeof(pkt));
  pb_w_fixo32(&w, 1, s_num);
  pb_w_fixo32(&w, 2, para);
  pb_w_num(&w, 3, hash_of_canal(canal));
  if (cifrado) { pb_w_bytes(&w, 5, cif, n_data); } else { pb_w_bytes(&w, 4, data, n_data); }
  pb_w_fixo32(&w, 6, id);
  pb_w_num(&w, 9, saltos);
  pb_w_num(&w, 15, saltos);                      // hop_start: a malha conta os saltos por ele

  // ServiceEnvelope{ 1: packet, 2: channel_id, 3: gateway_id }
  char gw[12];
  snprintf(gw, sizeof(gw), "!%08lx", (unsigned long)s_num);
  pb_escritor_t e = pb_escritor(env, sizeof(env));
  pb_w_sub(&e, 1, &w);
  pb_w_text(&e, 2, canal.name, sizeof(canal.name));
  pb_w_text(&e, 3, gw, sizeof(gw));
  const size_t n_env = pb_tamanho(&e);
  if (!n_env) { return false; }

  char topico[112];
  snprintf(topico, sizeof(topico), "%s/2/e/%s/%s", raiz, canal.name, gw);
  // enqueue, e nao publish: o publish escreve no socket NA HORA e pode
  // hold quem chamou (a tarefa of interface) ate o prazo of rede. O
  // enqueue so poe in fila of cliente, que manda in tarefa dele.
  const int msg = esp_mqtt_client_enqueue(cli, topico, (const char*)env, (int)n_env, 1, 0, true);
  if (msg < 0) { return false; }
  pend[*prox].msg = msg;
  pend[*prox].id  = id;
  *prox = (*prox + 1) % n_pend;
  ESP_LOGI(TAG, "publicado em %s (id %08lx)", topico, (unsigned long)id);
  return true;
}

// ---------------------------------------------------------------------------
// PROXY - API
// ---------------------------------------------------------------------------
void mesh_mqtt_aprender_no_mqtt(bool enabled, bool proxy, bool tls)
{
  s_no_mqtt = enabled; s_no_proxy = proxy; s_no_tls = tls;
}

void mesh_mqtt_aprender_downlink(int idx, bool downlink)
{
  if (idx >= 0 && idx < MAX_CANAIS) { s_downlink[idx] = downlink; }
}

bool mesh_mqtt_no_tem_proxy(void) { return s_no_mqtt && s_no_proxy; }

// "msh/BR/2/e/LongFast/!a1b2c3d4" -> "msh/BR": everything antes of first "/2/".
static bool raiz_of_topico(const char* t, char* out, size_t size)
{
  const char* p = strstr(t, "/2/");
  if (!p || p == t || (size_t)(p - t) >= size) { return false; }
  memcpy(out, t, (size_t)(p - t));
  out[p - t] = 0;
  return true;
}

bool mesh_mqtt_proxy_publicar(const char* topico, const uint8_t* d, size_t n, bool retido)
{
  if (!s_proxy || !mesh_mqtt_conectado() || !topico || !topico[0]) { ++s_px_perdeu; return false; }
  // A raiz que o no usa of verdade. Mudou (ou era o palpite of dump): refaz
  // as assinaturas in raiz certa.
  char raiz[48];
  if (raiz_of_topico(topico, raiz, sizeof(raiz)) && strcmp(raiz, s_px_raiz_no) != 0) {
    snprintf(s_px_raiz_no, sizeof(s_px_raiz_no), "%s", raiz);
    if (strcmp(raiz, s_px_raiz) != 0) {
      ESP_LOGI(TAG, "proxy: o no publica na raiz %s - assinando nela", raiz);
      assinar_proxy_now();
    }
  }
  // enqueue: nao segura a tarefa of servico esperando a rede (ver enviar_travado).
  const int msg = esp_mqtt_client_enqueue(s_cli, topico, (const char*)d, (int)n, 1, retido ? 1 : 0, true);
  if (msg < 0) { ++s_px_perdeu; return false; }
  ++s_px_subiu;
  return true;
}

size_t mesh_mqtt_proxy_receber(uint8_t* buf, size_t max)
{
  if (!s_fila || !s_proxy) { return 0; }
  static frame_t q;   // so a tarefa of servico recebe
  if (xQueueReceive(s_fila, &q, 0) != pdTRUE) { return 0; }
  if (q.n > max) { ++s_px_perdeu; return 0; }
  memcpy(buf, q.d, q.n);
  return q.n;
}

void mesh_mqtt_proxy_contadores(uint32_t* subiu, uint32_t* desceu, uint32_t* perdeu)
{
  if (subiu)  { *subiu  = s_px_subiu; }
  if (desceu) { *desceu = s_px_desceu; }
  if (perdeu) { *perdeu = s_px_perdeu; }
}

// ---------------------------------------------------------------------------
// REDE PUBLICA PELA INTERNET - API
// ---------------------------------------------------------------------------
static void assinar_internet(void)
{
  // Os gateways desta raiz publicam por estado e cidade (msh/US/GA/2/e/...,
  // msh/US/KS/Topeka/2/e/...): ate tres niveis abaixo of raiz. Sem os relatorios
  // of mapa - la eles trariam o pais inteiro para a tabela of nos.
  static const char* const NIVEL[] = { "", "/+", "/+/+", "/+/+/+" };
  char topico[96];
  for (size_t i = 0; i < sizeof(NIVEL) / sizeof(NIVEL[0]); ++i) {
    snprintf(topico, sizeof(topico), "%s%s/2/e/%s/#", INTERNET_RAIZ, NIVEL[i], CANAL_INTERNET.name);
    esp_mqtt_client_subscribe(s_rede_internet_cli, topico, 0);
  }
  ESP_LOGI(TAG, "internet: assinando %s/.../2/e/%s", INTERNET_RAIZ, CANAL_INTERNET.name);
}

static void on_evento_internet(void*, esp_event_base_t, int32_t id_ev, void* data)
{
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id_ev) {
    case MQTT_EVENT_CONNECTED:    s_rede_internet_conectado = true; assinar_internet(); break;
    case MQTT_EVENT_DISCONNECTED: s_rede_internet_conectado = false; break;
    case MQTT_EVENT_DATA:
      if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
        processar(ev->topic, ev->topic_len, (const uint8_t*)ev->data, (size_t)ev->data_len, s_rede_internet_fila, true);
      }
      break;
    case MQTT_EVENT_PUBLISHED:
      for (size_t i = 0; i < sizeof(s_rede_internet_pend) / sizeof(s_rede_internet_pend[0]); ++i) {
        if (s_rede_internet_pend[i].msg == ev->msg_id && s_rede_internet_pend[i].id) { confirm(s_rede_internet_pend[i].id, s_rede_internet_fila); s_rede_internet_pend[i].id = 0; }
      }
      break;
    default: break;
  }
}

bool mesh_mqtt_internet_start(void)
{
  if (s_rede_internet_cli) { return true; }
  if (!s_rede_internet_fila) { s_rede_internet_fila = xQueueCreateWithCaps(32, sizeof(frame_t), MALLOC_CAP_SPIRAM); s_rede_internet_fila_ref = s_rede_internet_fila; }
  tab5_mutex_create(&s_trava_ent);
  tab5_mutex_create(&s_trava_env);
  if (!s_rede_internet_fila || !s_trava_ent || !s_trava_env) { return false; }
  xQueueReset(s_rede_internet_fila);
  static char cliente[24];
  snprintf(cliente, sizeof(cliente), "tab5net-%08lx", (unsigned long)(s_num ? s_num : esp_random()));
  esp_mqtt_client_config_t c = { };
  c.broker.address.uri = "mqtt://" INTERNET_SRV ":1883";
  c.credentials.username = USR_PADRAO;
  c.credentials.authentication.password = PWD_PADRAO;
  c.credentials.client_id = cliente;
  // NO TAMANHO DO QUE PASSA: o envelope maior e ~400 bytes com o topico (o
  // MeshPacket tem teto of 256), e o que exit e so text nosso. Tudo isto e
  // RAM interna (alocacoes < 4 KB nao vao para a PSRAM, e a stack of tarefa of
  // biblioteca e sempre interna). Maior que o buffer chega em pedacos e e
  // descartado em ao_evento_internet - nao e Meshtastic.
  c.buffer.size = 768;
  c.buffer.out_size = 512;
  c.task.stack_size = 3584;   // pico measured of ~2,6 KB no cliente igual a este
  c.network.reconnect_timeout_ms = 15000;
  s_rede_internet_cli = esp_mqtt_client_init(&c);
  if (!s_rede_internet_cli) { return false; }
  esp_mqtt_client_register_event(s_rede_internet_cli, MQTT_EVENT_ANY, on_evento_internet, nullptr);
  if (esp_mqtt_client_start(s_rede_internet_cli) != ESP_OK) {
    esp_mqtt_client_destroy(s_rede_internet_cli);
    s_rede_internet_cli = nullptr;
    return false;
  }
  tab5_memory_marco("MQTT internet (cliente + tarefa)");
  ESP_LOGI(TAG, "Internet: connecting to %s, root %s", INTERNET_SRV, INTERNET_RAIZ);
  return true;
}

void mesh_mqtt_internet_stop(void)
{
  if (!s_rede_internet_cli) { return; }
  esp_mqtt_client_stop(s_rede_internet_cli);
  esp_mqtt_client_destroy(s_rede_internet_cli);
  s_rede_internet_cli = nullptr;
  s_rede_internet_conectado = false;
  if (s_rede_internet_fila) { xQueueReset(s_rede_internet_fila); }
  ESP_LOGI(TAG, "internet: desconectado");
}

bool mesh_mqtt_internet_active(void)     { return s_rede_internet_cli != nullptr; }
bool mesh_mqtt_internet_conectado(void) { return s_rede_internet_cli && s_rede_internet_conectado; }
uint32_t mesh_mqtt_internet_recebidos(void) { return s_rede_internet_recebidos; }

size_t mesh_mqtt_internet_receber(uint8_t* buf, size_t max)
{
  if (!s_rede_internet_fila) { return 0; }
  static frame_t q;   // so a tarefa of servico recebe
  if (xQueueReceive(s_rede_internet_fila, &q, 0) != pdTRUE) { return 0; }
  const size_t n = q.n < max ? q.n : max;
  memcpy(buf, q.d, n);
  return n;
}

bool mesh_mqtt_internet_send(const uint8_t* toradio, size_t n)
{
  if (!s_trava_env || !mesh_mqtt_internet_conectado() || s_num == 0) { return false; }
  to_radio_t t;
  read_to_radio(toradio, n, &t);
  // So canal: a direta precisaria of PKI (ver mesh_mqtt_enviar).
  if (!t.data || t.para != MESH_PB_DIFUSAO) { return false; }
  xSemaphoreTake(s_trava_env, portMAX_DELAY);
  const bool ok = publicar_data(s_rede_internet_cli, s_rede_internet_pend, &s_rede_internet_prox_pend,
                                (int)(sizeof(s_rede_internet_pend) / sizeof(s_rede_internet_pend[0])),
                                CANAL_INTERNET, INTERNET_RAIZ, t.para, t.data, t.n_data, t.id, t.saltos);
  xSemaphoreGive(s_trava_env);
  return ok;
}
