// Configuracao do no Meshtastic - ver o aviso grande em tab5_mesh_admin.hpp.
#include "tab5_mesh_admin.hpp"
#include "mesh_pb.hpp"   // o codec do servico, um so para leitura e escrita

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app.h"          // sistema_avisar
#include "tab5_gps.hpp"
#include "tab5_time.hpp"   // tab5_agora_ms
#include "settings.h"       // AJUSTES_MESH_BLUETOOTH

static const char* TAG = "mesh_admin";

// ---------------------------------------------------------------------------
// Fornecidos pelo servico (tab5_mesh.cpp) - nao fazem parte da API publica.
// ---------------------------------------------------------------------------
bool tab5_mesh_interno_admin(const uint8_t* adm, size_t n);   // AdminMessage para o nosso no
bool tab5_mesh_interno_pode_admin(void);                      // no do outro lado, dump completo
void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou);   // pedir a configuracao daqui a pouco

// AdminMessage - numeros de campo conferidos no admin.pb.h (2.5 a atual).
#define ADM_SET_OWNER          32
#define ADM_SET_CHANNEL        33
#define ADM_SET_CONFIG         34
#define ADM_SET_MODULE_CONFIG  35
#define ADM_SET_FIXED_POSITION 41
#define ADM_REMOVE_FIXED_POS   42
#define ADM_SET_TIME_ONLY      43
#define ADM_BEGIN_EDIT         64
#define ADM_COMMIT_EDIT        65
#define ADM_REBOOT_SECONDS     97

// Data.payload max_size (mesh.pb.h): o AdminMessage inteiro tem que caber.
#define ADM_MAX 233

// Secoes: Config 1..10 (device, position, power, network, display, lora,
// bluetooth, security, sessionkey, device_ui) e ModuleConfig 1..17.
#define CFG_SECOES 11
#define MOD_SECOES 18
#define BRUTO_MAX  256

typedef struct { bool tem; uint16_t n; uint8_t b[BRUTO_MAX]; } bruto_t;

// O que cada campo editavel e no protobuf. Todos varint.
typedef struct { bool modulo; uint8_t secao; uint8_t num; } campo_def_t;
static const campo_def_t CAMPOS[MESH_CFG_N] = {
  /* PAPEL       */ { false, 1,  1 },   // DeviceConfig.role
  /* NODEINFO_S  */ { false, 1,  7 },   // DeviceConfig.node_info_broadcast_secs
  /* POS_S       */ { false, 2,  1 },   // PositionConfig.position_broadcast_secs
  /* SALTOS      */ { false, 6,  8 },   // LoRaConfig.hop_limit
  /* TX_DBM      */ { false, 6, 10 },   // LoRaConfig.tx_power (int32)
  /* BT          */ { false, 7,  1 },   // BluetoothConfig.enabled
  /* MQTT        */ { true,  1,  1 },   // ModuleConfig.MQTTConfig.enabled
  /* TELE_S      */ { true,  6,  1 },   // ModuleConfig.TelemetryConfig.device_update_interval
  /* MQTT_PROXY  */ { true,  1,  9 },   // ModuleConfig.MQTTConfig.proxy_to_client_enabled
  /* POS_INTELIG */ { false, 2,  2 },   // PositionConfig.position_broadcast_smart_enabled
  /* GPS_MODO    */ { false, 2, 13 },   // PositionConfig.gps_mode
};

// Secoes que NUNCA sao reenviadas (ver o .hpp): power 3, network 4,
// security 8, sessionkey 9; modulo serial 2.
static bool secao_proibida(bool modulo, int secao)
{
  return modulo ? (secao == 2) : (secao == 3 || secao == 4 || secao == 8 || secao == 9);
}

// Escrito pela tarefa do servico, lido pela interface: spinlock, porque as
// secoes criticas sao copias e varreduras de algumas centenas de bytes.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static bruto_t s_cfg[CFG_SECOES];
EXT_RAM_BSS_ATTR static bruto_t s_mod[MOD_SECOES];
EXT_RAM_BSS_ATTR static bruto_t s_user, s_meta, s_myinfo;
EXT_RAM_BSS_ATTR static bruto_t s_canal0;   // o Channel PRIMARIO inteiro (indice 0)
static char     s_aviso[96];
static uint32_t s_aviso_ms = 0;
static bool     s_dump_ok = false;
static bool     s_gravando = false;
static uint32_t s_gravando_ms = 0;
static uint32_t s_versao = 0;   // lido sem trava: so "mudou?", como as outras versoes do servico
static char     s_motivo[72];   // por que o ultimo mesh_cfg_gravar nao mandou nada (para a tela)

#define GRAVANDO_PRAZO_MS 90000   // o no reinicia em ~5 s; 90 s cobre um boot lento


// ---------------------------------------------------------------------------
// Ganchos do servico
// ---------------------------------------------------------------------------
static void guardar(bruto_t* d, const uint8_t* b, size_t n)
{
  if (n > BRUTO_MAX) {
    // Maior que o buffer: guardar truncado e reenviar depois perderia campos
    // no no. Marca como desconhecida - a edicao dela fica bloqueada.
    ESP_LOGD(TAG, "secao de %u bytes nao cabe - edicao dela desligada", (unsigned)n);
    taskENTER_CRITICAL(&s_mux); d->tem = false; d->n = 0; taskEXIT_CRITICAL(&s_mux);
    return;
  }
  taskENTER_CRITICAL(&s_mux);
  memcpy(d->b, b, n); d->n = (uint16_t)n; d->tem = true;
  taskEXIT_CRITICAL(&s_mux);
  ++s_versao;
}

void mesh_admin_guardar_secao(bool modulo, int secao, const uint8_t* b, size_t n)
{
  if (modulo) { if (secao >= 1 && secao < MOD_SECOES) { guardar(&s_mod[secao], b, n); } }
  else        { if (secao >= 1 && secao < CFG_SECOES) { guardar(&s_cfg[secao], b, n); } }
}
void mesh_admin_guardar_user(const uint8_t* b, size_t n)     { guardar(&s_user, b, n); }
void mesh_admin_guardar_metadata(const uint8_t* b, size_t n) { guardar(&s_meta, b, n); }
void mesh_admin_guardar_my_info(const uint8_t* b, size_t n)  { guardar(&s_myinfo, b, n); }
void mesh_admin_guardar_canal(int idx, const uint8_t* b, size_t n) { if (idx == 0) { guardar(&s_canal0, b, n); } }

void mesh_admin_guardar_aviso(const uint8_t* b, size_t n)
{
  char msg[sizeof(s_aviso)] = "";
  if (!pb_achar_texto(b, n, 4, msg, sizeof(msg)) || !msg[0]) { return; }   // ClientNotification.message
  ESP_LOGW(TAG, "aviso do no: %s", msg);
  taskENTER_CRITICAL(&s_mux);
  memcpy(s_aviso, msg, sizeof(s_aviso));
  s_aviso_ms = tab5_agora_ms() | 1u;
  taskEXIT_CRITICAL(&s_mux);
  ++s_versao;
  sistema_avisar(AVISO_MESH_NOS);
}

void mesh_admin_inicio_dump(void) { s_dump_ok = false; ++s_versao; }

void mesh_admin_fim_dump(void)
{
  s_dump_ok = true;
  if (s_gravando) { ESP_LOGI(TAG, "no se reapresentou - valores novos confirmados"); }
  s_gravando = false;
  ++s_versao;
  sistema_avisar(AVISO_MESH_NOS);
}

// INSISTE enquanto o no nao volta. O primeiro pedido de configuracao sai 15 s
// depois de gravar (tab5_mesh_interno_reapresentar_em); um boot mais lento
// perde esse pedido, e sem isto o proximo so sairia no ritmo de silencio do
// servico (minutos). De 20 em 20 s ate o dump chegar ou o prazo acabar.
void mesh_admin_tique(uint32_t t)
{
  static uint32_t ultimo = 0;
  if (!s_gravando) { ultimo = 0; return; }
  // O PRAZO VALE AQUI TAMBEM, e nao so quando a tela pergunta (estado()):
  // com a pagina fechada, um no que nunca volta seria pedido de 20 em 20 s
  // para sempre.
  if (t - s_gravando_ms > GRAVANDO_PRAZO_MS) {
    ESP_LOGW(TAG, "o no nao se reapresentou em %d s depois de gravar", GRAVANDO_PRAZO_MS / 1000);
    s_gravando = false;
    ++s_versao;
    return;
  }
  if (t - s_gravando_ms < 35000) { return; }
  if (ultimo == 0 || t - ultimo >= 20000) {
    ultimo = t;
    ESP_LOGI(TAG, "o no ainda nao voltou - pedindo a configuracao de novo");
    tab5_mesh_interno_reapresentar_em(0, true);
  }
}

// ---------------------------------------------------------------------------
// Leitura
// ---------------------------------------------------------------------------
// PRECISAO DA POSICAO NO CANAL: Channel{ 2: settings{ 7: module_settings{
// 1: position_precision } } }. Sem module_settings o firmware trata o
// primario como os canais de antes do campo existir: 13 bits (~3 km) - ver
// PositionModule::allocPositionPacket. Com trava tomada.
static int32_t precisao_de(const bruto_t& c)
{
  if (!c.tem) { return -1; }
  pb_campo_t st, mod;
  if (!pb_achar(c.b, c.n, 2, PB_LEN, &st)) { return 13; }
  if (!pb_achar(st.val, st.n_val, 7, PB_LEN, &mod)) { return 13; }
  uint64_t v;
  return pb_achar_varint(mod.val, mod.n_val, 1, &v) ? (int32_t)v : 0;
}

static mesh_cfg_estado_t estado(void)
{
  if (s_gravando && (tab5_agora_ms() - s_gravando_ms) > GRAVANDO_PRAZO_MS) {
    ESP_LOGW(TAG, "o no nao se reapresentou em %d s depois de gravar", GRAVANDO_PRAZO_MS / 1000);
    s_gravando = false;
  }
  if (s_gravando) { return MESH_CFG_GRAVANDO; }
  return (s_dump_ok && tab5_mesh_interno_pode_admin()) ? MESH_CFG_PRONTO : MESH_CFG_SEM_NO;
}

uint32_t mesh_cfg_versao(void) { return s_versao; }

void mesh_cfg_ler(mesh_cfg_t* o)
{
  memset(o, 0, sizeof(*o));
  o->state = (uint8_t)estado();
  o->regiao = -1;
  o->nos_no_radio = -1;
  uint64_t v;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < MESH_CFG_N; ++i) {
    const campo_def_t& c = CAMPOS[i];
    const bruto_t& b = c.modulo ? s_mod[c.secao] : s_cfg[c.secao];
    o->conhecido[i] = b.tem;
    o->v[i] = (b.tem && pb_achar_varint(b.b, b.n, c.num, &v)) ? (int32_t)v : 0;
  }
  if (s_cfg[2].tem && pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 3, &v)) { o->pos_fixa = (v != 0); }
  o->precisao = precisao_de(s_canal0);
  if (s_cfg[6].tem) {
    o->regiao     = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 7, &v) ? (int32_t)v : 0;
    o->preset     = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 2, &v) ? (int32_t)v : 0;
    o->usa_preset = pb_achar_varint(s_cfg[6].b, s_cfg[6].n, 1, &v) && v != 0;
  }
  if (s_cfg[4].tem) {
    o->wifi_conhecido = true;
    o->wifi_ligado = pb_achar_varint(s_cfg[4].b, s_cfg[4].n, 1, &v) && v != 0;
    pb_achar_texto(s_cfg[4].b, s_cfg[4].n, 3, o->wifi_ssid, sizeof(o->wifi_ssid));
  }
  if (s_meta.tem) {
    pb_achar_texto(s_meta.b, s_meta.n, 1, o->firmware, sizeof(o->firmware));
    if (pb_achar_varint(s_meta.b, s_meta.n, 9, &v)) { o->hw = (uint8_t)v; }
    o->tem_wifi = pb_achar_varint(s_meta.b, s_meta.n, 4, &v) && v;
    o->tem_bt   = pb_achar_varint(s_meta.b, s_meta.n, 5, &v) && v;
  }
  if (s_myinfo.tem) {
    if (pb_achar_varint(s_myinfo.b, s_myinfo.n, 8, &v))  { o->reinicios = (uint32_t)v; }
    // nodedb_count (15) so existe no firmware novo: ausente fica -1, e a tela
    // nao inventa um numero.
    if (pb_achar_varint(s_myinfo.b, s_myinfo.n, 15, &v)) { o->nos_no_radio = (int32_t)v; }
  }
  memcpy(o->aviso, s_aviso, sizeof(o->aviso));
  o->aviso_ms = s_aviso_ms;
  taskEXIT_CRITICAL(&s_mux);
}

bool mesh_admin_no_tem_posicao_propria(void)
{
  uint64_t fixa = 0, gps = 0;
  taskENTER_CRITICAL(&s_mux);
  if (s_cfg[2].tem) {
    pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 3, &fixa);    // fixed_position
    pb_achar_varint(s_cfg[2].b, s_cfg[2].n, 13, &gps);    // gps_mode (1 = ENABLED)
  }
  taskEXIT_CRITICAL(&s_mux);
  return fixa != 0 || gps == 1;
}

int32_t mesh_admin_precisao_primario(void)
{
  taskENTER_CRITICAL(&s_mux);
  const int32_t p = precisao_de(s_canal0);
  taskEXIT_CRITICAL(&s_mux);
  return p;
}

// ---------------------------------------------------------------------------
// Escrita
// ---------------------------------------------------------------------------
// AdminMessage{ campo: valor varint }
static bool admin_varint(int campo, uint64_t valor)
{
  uint8_t a[16];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_num(&w, campo, valor);
  return tab5_mesh_interno_admin(a, pb_tamanho(&w));
}

// AdminMessage{ campo: <submensagem ja montada> } em 'out'. 0 = nao cabe
// no buffer OU passa do que um pacote leva (ADM_MAX) - nos dois casos a
// secao nao pode ir, e quem chama recusa a gravacao inteira.
static size_t admin_msg(uint8_t* out, size_t tam, int campo, const pb_escritor_t* corpo)
{
  pb_escritor_t w = pb_escritor(out, tam);
  pb_w_sub(&w, campo, corpo);
  const size_t n = pb_tamanho(&w);
  return n <= ADM_MAX ? n : 0;
}

// O BRUTO GUARDADO NO DUMP, como inicio de uma secao a reenviar. Os campos
// mudados vao DEPOIS dele: o nanopb fica com a ultima ocorrencia de cada
// campo, entao o valor novo vence o antigo sem precisar reescrever a secao.
// nullptr = pronto para receber os campos; senao, POR QUE nao (para o log).
static const char* comecar_do_bruto(pb_escritor_t* w, const bruto_t& b)
{
  taskENTER_CRITICAL(&s_mux);
  const bool tem = b.tem;
  if (tem) { pb_w_cru(w, b.b, b.n); }
  taskEXIT_CRITICAL(&s_mux);
  if (!tem)   { return "nao veio no dump"; }
  if (!w->ok) { return "e grande demais para um pacote"; }
  return nullptr;
}

// O CANAL PRIMARIO com outra precisao: o Channel guardado campo a campo, cru,
// menos settings.module_settings.position_precision, que vai com o valor
// novo. Aqui nao da para so acrescentar no fim, como nas secoes: module_settings
// e SUBMENSAGEM, e duas ocorrencias dela nao garantem a ultima vencendo campo
// a campo - entao a submensagem e remontada (o is_muted dela segue igual).
static bool montar_canal(pb_escritor_t* w, int32_t precisao)
{
  EXT_RAM_BSS_ATTR static bruto_t c;   // so a interface grava
  taskENTER_CRITICAL(&s_mux);
  c = s_canal0;
  taskEXIT_CRITICAL(&s_mux);
  if (!c.tem) { return false; }
  pb_leitor_t l = pb_leitor(c.b, c.n);
  pb_campo_t f;
  bool tem_settings = false;
  while (pb_campo(&l, &f)) {
    if (!(f.num == 2 && f.type == PB_LEN)) { pb_w_cru(w, f.ini, f.n_total); continue; }
    tem_settings = true;
    uint8_t sb[160];
    pb_escritor_t ws = pb_escritor(sb, sizeof(sb));
    pb_leitor_t ls = pb_leitor(f.val, f.n_val);
    pb_campo_t g;
    uint8_t mb[16];
    pb_escritor_t wm = pb_escritor(mb, sizeof(mb));
    pb_w_num(&wm, 1, (uint64_t)precisao);                       // position_precision
    while (pb_campo(&ls, &g)) {
      if (g.num == 7 && g.type == PB_LEN) {                     // module_settings: o resto dela
        pb_leitor_t lm = pb_leitor(g.val, g.n_val);
        pb_campo_t h;
        while (pb_campo(&lm, &h)) { if (h.num != 1) { pb_w_cru(&wm, h.ini, h.n_total); } }
      } else {
        pb_w_cru(&ws, g.ini, g.n_total);
      }
    }
    pb_w_sub(&ws, 7, &wm);
    if (!ws.ok || !wm.ok) { return false; }
    pb_w_sub(w, 2, &ws);
  }
  return tem_settings && w->ok;
}

const char* mesh_cfg_motivo(void) { return s_motivo; }

bool mesh_cfg_gravar(const mesh_cfg_mudanca_t* m)
{
  s_motivo[0] = 0;
  if (estado() != MESH_CFG_PRONTO) {
    ESP_LOGW(TAG, "gravar recusado: sem no ou dump incompleto");
    snprintf(s_motivo, sizeof(s_motivo), "sem o nó, ou a configuração ainda chegando");
    return false;
  }

  // MONTA TUDO ANTES DE MANDAR QUALQUER COISA. Se uma secao nao puder ir
  // (desconhecida, proibida, grande demais), nada sai - meia transacao
  // gravada seria uma configuracao que ninguem pediu.
  // device, position, lora, bluetooth, mqtt, telemetry + network + o canal
  // primario + o dono = 9.
  enum { MAX_MSGS = 10 };
  EXT_RAM_BSS_ATTR static uint8_t msgs[MAX_MSGS][ADM_MAX + 16];   // so a interface grava
  size_t tam[MAX_MSGS];
  int n_msgs = 0;

  bool feito[MESH_CFG_N] = { };
  int msg_mqtt = -1;   // indice, em msgs[], da secao MQTT (ver a regra do Bluetooth abaixo)
  for (int i = 0; i < MESH_CFG_N; ++i) {
    if (!m->muda[i] || feito[i]) { continue; }
    const campo_def_t& c = CAMPOS[i];
    if (secao_proibida(c.modulo, c.secao)) { ESP_LOGE(TAG, "secao proibida"); return false; }

    // A secao guardada + TODOS os campos alterados dela, no fim.
    EXT_RAM_BSS_ATTR static uint8_t corpo[ADM_MAX];
    pb_escritor_t wc = pb_escritor(corpo, sizeof(corpo));
    if (const char* porque = comecar_do_bruto(&wc, c.modulo ? s_mod[c.secao] : s_cfg[c.secao])) {
      ESP_LOGW(TAG, "secao %s %d %s - nao da para gravar", c.modulo ? "modulo" : "config", c.secao, porque);
      return false;
    }
    for (int k = i; k < MESH_CFG_N; ++k) {
      if (!m->muda[k] || CAMPOS[k].modulo != c.modulo || CAMPOS[k].secao != c.secao) { continue; }
      pb_w_int32(&wc, CAMPOS[k].num, m->v[k]);
      feito[k] = true;
    }

    // Config{ secao: corpo } ou ModuleConfig{ secao: corpo }, dentro do
    // AdminMessage{ set_config | set_module_config }.
    EXT_RAM_BSS_ATTR static uint8_t sec[ADM_MAX];
    pb_escritor_t ws = pb_escritor(sec, sizeof(sec));
    pb_w_sub(&ws, c.secao, &wc);
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), c.modulo ? ADM_SET_MODULE_CONFIG : ADM_SET_CONFIG, &ws);
    if (!tam[n_msgs]) {
      ESP_LOGE(TAG, "secao grande demais para um pacote");
      snprintf(s_motivo, sizeof(s_motivo), "configuração grande demais para um pacote");
      return false;
    }
    if (c.modulo && c.secao == 1) { msg_mqtt = n_msgs; }
    ++n_msgs;
  }

  // WI-FI DO NO: Config.network = secao guardada + wifi_enabled(1) + ssid(3)
  // + psk(4) no fim. A senha nao volta do no, entao so vai com nome E senha.
  if (m->wifi_ssid[0] || m->wifi_psk[0] || m->wifi_muda) {
    const bool tem_rede = m->wifi_ssid[0] || m->wifi_psk[0];
    if (tem_rede && (!m->wifi_ssid[0] || (!m->wifi_psk[0] && !m->wifi_aberta))) { ESP_LOGW(TAG, "wifi: precisa de nome e senha juntos"); return false; }
    EXT_RAM_BSS_ATTR static uint8_t corpo[ADM_MAX];
    pb_escritor_t wc = pb_escritor(corpo, sizeof(corpo));
    if (const char* porque = comecar_do_bruto(&wc, s_cfg[4])) { ESP_LOGW(TAG, "wifi: a secao network %s - nada enviado", porque); return false; }
    pb_w_num(&wc, 1, m->wifi_liga ? 1 : 0);                    // wifi_enabled
    if (tem_rede) {
      pb_w_texto(&wc, 3, m->wifi_ssid, 32);                    // wifi_ssid
      pb_w_texto(&wc, 4, m->wifi_psk, 64);                     // wifi_psk
    }
    EXT_RAM_BSS_ATTR static uint8_t sec[ADM_MAX];
    pb_escritor_t ws = pb_escritor(sec, sizeof(sec));
    pb_w_sub(&ws, 4, &wc);                                     // Config.network = 4
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_CONFIG, &ws);
    if (!tam[n_msgs]) { ESP_LOGE(TAG, "wifi: secao grande demais para um pacote"); return false; }
    ++n_msgs;
  }

  // LOCALIZACAO NA MALHA: set_channel do primario com a precisao nova.
  if (m->precisao_muda) {
    EXT_RAM_BSS_ATTR static uint8_t corpo[ADM_MAX];
    pb_escritor_t wc = pb_escritor(corpo, sizeof(corpo));
    if (!montar_canal(&wc, m->precisao)) {
      ESP_LOGW(TAG, "canal primario: nao veio no dump ou nao cabe - nada enviado");
      snprintf(s_motivo, sizeof(s_motivo), "o canal principal do nó ainda não chegou");
      return false;
    }
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_CHANNEL, &wc);
    if (!tam[n_msgs]) { ESP_LOGE(TAG, "canal grande demais para um pacote"); return false; }
    ++n_msgs;
  }

  // NOMES: o User guardado do nosso no + os nomes no fim. Reenviar o User
  // inteiro preserva o que o set_owner tambem le (is_licensed,
  // is_unmessagable) - so com os nomes, um no radioamador perderia a licenca.
  if (m->longo[0] || m->curto[0]) {
    EXT_RAM_BSS_ATTR static uint8_t u[ADM_MAX];
    pb_escritor_t wu = pb_escritor(u, sizeof(u));
    // Sem o User do no nao vai: so com os nomes, o set_owner desligaria o
    // is_licensed de um no radioamador (ele compara o campo, ausente = false).
    if (const char* porque = comecar_do_bruto(&wu, s_user)) { ESP_LOGW(TAG, "nomes: o User do nosso no %s - nada enviado", porque); return false; }
    if (m->longo[0]) { pb_w_texto(&wu, 2, m->longo, MESH_NOME_LONGO - 1); }   // long_name
    if (m->curto[0]) { pb_w_texto(&wu, 3, m->curto, MESH_NOME_CURTO - 1); }   // short_name
    if (n_msgs >= MAX_MSGS) { return false; }
    tam[n_msgs] = admin_msg(msgs[n_msgs], sizeof(msgs[0]), ADM_SET_OWNER, &wu);
    if (!tam[n_msgs]) { ESP_LOGE(TAG, "nome grande demais para um pacote"); return false; }
    ++n_msgs;
  }

  if (n_msgs == 0) { return false; }

  // A SECAO MQTT PELO BLUETOOTH VAI SOZINHA, SEM TRANSACAO. O firmware
  // (AdminModule::handleSetModuleConfig, caso mqtt) chama disableBluetooth()
  // ASSIM QUE recebe a secao MQTT - mesmo com a transacao aberta. Dentro de
  // begin/commit, o BLE caia antes do commit: a transacao ficava aberta (o
  // firmware nao a expira), nada era gravado e o no nao reiniciava, agora sem
  // Bluetooth ate alguem desliga-lo da tomada.
  // Fora da transacao o mesmo set_module_config valida, grava no disco e
  // reinicia o no sozinho (saveChanges com shouldReboot) - o BLE cair logo
  // em seguida nao atrapalha mais nada. Por isso pelo Bluetooth a secao MQTT
  // precisa ser a UNICA mudanca: qualquer outra, depois dela, nao chegaria.
  // Pelo cabo nao ha Bluetooth a cair, e vale a transacao normal.
  if (msg_mqtt >= 0 && tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH) {
    if (n_msgs > 1) {
      ESP_LOGW(TAG, "pelo Bluetooth a secao MQTT grava sozinha - outras mudancas pendentes junto");
      snprintf(s_motivo, sizeof(s_motivo), "pelo Bluetooth, MQTT/proxy grava sozinho: grave o resto antes");
      return false;
    }
    if (!tab5_mesh_interno_admin(msgs[msg_mqtt], tam[msg_mqtt])) {
      snprintf(s_motivo, sizeof(s_motivo), "o enlace com o nó recusou o envio");
      return false;
    }
    ESP_LOGI(TAG, "secao MQTT enviada sem transacao (Bluetooth) - o no grava, desliga o BLE e reinicia");
    s_gravando = true;
    s_gravando_ms = tab5_agora_ms();
    ++s_versao;
    tab5_mesh_interno_reapresentar_em(15000, true);
    return true;
  }

  // begin -> secoes -> commit. O commit grava e reinicia o no.
  bool ok = admin_varint(ADM_BEGIN_EDIT, 1);
  for (int i = 0; ok && i < n_msgs; ++i) { ok = tab5_mesh_interno_admin(msgs[i], tam[i]); }
  if (ok) { ok = admin_varint(ADM_COMMIT_EDIT, 1); }
  if (!ok) {
    // Transacao aberta sem commit. O firmware NAO a expira: as mudancas ficam
    // so na RAM do no, sem gravar, ate ele reiniciar.
    ESP_LOGE(TAG, "enlace recusou no meio da transacao - o no fica sem gravar ate reiniciar");
    snprintf(s_motivo, sizeof(s_motivo), "o enlace caiu no meio da gravação - reinicie o nó");
    return false;
  }
  ESP_LOGI(TAG, "transacao enviada (%d mensagem(ns)) - o no grava e reinicia", n_msgs);
  s_gravando = true;
  s_gravando_ms = tab5_agora_ms();
  ++s_versao;
  // Pela serial o no reiniciado nao fala sozinho: pedir a configuracao depois
  // do boot (o BLE reconecta e pede sozinho; pedir de novo nao faz mal).
  tab5_mesh_interno_reapresentar_em(15000, true);
  return true;
}

bool mesh_cfg_posicao_fixa_do_gps(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const nmea_position_t g = tab5_gps_position();
  if (!g.valid || g.qualidade == 0) { ESP_LOGW(TAG, "posicao fixa: GPS do Tab5 sem fix"); return false; }
  // A mesma Position que o servico manda para a malha (mesh_pb_posicao).
  uint8_t pos[32];
  pb_escritor_t wp = pb_escritor(pos, sizeof(pos));
  mesh_pb_posicao(&wp, (int32_t)(g.lat * 1e7), (int32_t)(g.lon * 1e7), (int32_t)g.altitude_m,
                  tab5_hora_epoch());
  uint8_t a[48];
  const size_t n = admin_msg(a, sizeof(a), ADM_SET_FIXED_POSITION, &wp);
  const bool ok = n && tab5_mesh_interno_admin(a, n);
  // A secao position no dump guardado ainda diz "nao fixa": pede de novo.
  if (ok) { ESP_LOGI(TAG, "posicao fixa enviada (%.5f, %.5f)", g.lat, g.lon); tab5_mesh_interno_reapresentar_em(3000, false); }
  return ok;
}

bool mesh_cfg_tirar_posicao_fixa(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const bool ok = admin_varint(ADM_REMOVE_FIXED_POS, 1);
  if (ok) { tab5_mesh_interno_reapresentar_em(3000, false); }
  return ok;
}

bool mesh_cfg_acertar_hora(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const uint32_t agora = tab5_hora_epoch();
  if (!agora) { ESP_LOGW(TAG, "hora do Tab5 ainda nao acertada"); return false; }
  uint8_t a[8];
  pb_escritor_t w = pb_escritor(a, sizeof(a));
  pb_w_fixo32(&w, ADM_SET_TIME_ONLY, agora);
  return tab5_mesh_interno_admin(a, pb_tamanho(&w));
}

bool mesh_cfg_reiniciar(void)
{
  if (estado() != MESH_CFG_PRONTO) { return false; }
  const bool ok = admin_varint(ADM_REBOOT_SECONDS, 5);
  if (ok) {
    s_gravando = true;   // mesma espera: o no some e volta
    s_gravando_ms = tab5_agora_ms();
    ++s_versao;
    tab5_mesh_interno_reapresentar_em(15000, true);
  }
  return ok;
}

bool mesh_admin_gravando(void) { return s_gravando; }
