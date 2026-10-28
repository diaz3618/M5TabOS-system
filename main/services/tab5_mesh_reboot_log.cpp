// Diario de reinicios do no - ver o aviso em tab5_mesh_reboot_log.hpp.
#include "tab5_mesh_reboot_log.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tab5_sd.hpp"
#include "tab5_power.hpp"
#include "tab5_mesh.hpp"
#include "tab5_time.hpp"   // tab5_hora_epoch

static const char* TAG = "mesh_diario";

#define PASTA "Meshtastic"
#define ARQ   "reinicios.txt"
// A foto vale ate aqui: depois disso o contexto ja nao e o do reinicio.
#define FOTO_VALE_MS 180000u
// Janela do veredito: o no leva ~5 s para subir e o Tab5 percebe em seguida.
#define JANELA_S 30

EXT_RAM_BSS_ATTR static mesh_reinicio_t s_e[MESH_DIARIO_MAX];   // do mais velho (0) ao mais novo
static int      s_n = 0;
static uint32_t s_ult_cont = 0;
static uint32_t s_versao = 0;
static bool     s_carregado = false;
static volatile bool s_limpar_pend = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_envio_ms = 0;      // ultimo envio que nao foi batimento
static uint8_t  s_envio_tipo = 0;

typedef struct { bool tem; uint32_t ms; uint32_t up_no_s; } foto_t;
static foto_t s_foto;


void mesh_diario_nota_envio(uint8_t tipo)
{
  if (tipo == MESH_ENV_HEARTBEAT || tipo == MESH_ENV_NENHUM) { return; }   // a cada 30 s: so ruido
  s_envio_ms = tab5_agora_ms() | 1u;
  s_envio_tipo = tipo;
}

static int32_t ha_s(uint32_t ms_do_evento)
{
  if (ms_do_evento == 0) { return -1; }
  return (int32_t)((tab5_agora_ms() - ms_do_evento) / 1000u);
}

static void linha_de(const mesh_reinicio_t* r, char* o, size_t n)
{
  snprintf(o, n, "%lu|%lu|%lu|%lu|%u|%u|%u|%ld|%ld|%ld|%ld\n",
           (unsigned long)r->epoch, (unsigned long)r->up_tab5_s, (unsigned long)r->contador,
           (unsigned long)r->up_no_s, (unsigned)r->origem, (unsigned)r->envio_tipo,
           (unsigned)r->centivolts, (long)r->ma, (long)r->envio_s, (long)r->grove_s, (long)r->perfil_s);
}

static bool le_linha(const char* l, mesh_reinicio_t* r)
{
  unsigned long e, ut, c, un; unsigned o, et, cv; long ma, es, gs, ps;
  if (sscanf(l, "%lu|%lu|%lu|%lu|%u|%u|%u|%ld|%ld|%ld|%ld", &e, &ut, &c, &un, &o, &et, &cv, &ma, &es, &gs, &ps) != 11) { return false; }
  r->epoch = e; r->up_tab5_s = ut; r->contador = c; r->up_no_s = un; r->origem = (uint8_t)o;
  r->envio_tipo = (uint8_t)et; r->centivolts = (uint16_t)cv; r->ma = ma; r->envio_s = es; r->grove_s = gs; r->perfil_s = ps;
  return true;
}

static void juntar(const mesh_reinicio_t& r)
{
  taskENTER_CRITICAL(&s_mux);
  if (s_n >= MESH_DIARIO_MAX) { memmove(&s_e[0], &s_e[1], sizeof(s_e[0]) * (MESH_DIARIO_MAX - 1)); s_n = MESH_DIARIO_MAX - 1; }
  s_e[s_n++] = r;
  taskEXIT_CRITICAL(&s_mux);
  ++s_versao;
}

// Roda na tarefa "cartao" (carona): apaga o arquivo quando a tela pediu.
static void na_carona(void)
{
  if (!s_limpar_pend) { return; }
  s_limpar_pend = false;
  // Arquivo vazio pela porta do sistema: com o cartao emprestado, vai para o
  // diario - e o substituir grava antes as linhas que estavam para entrar.
  if (!tab5_sd_substituir(PASTA, ARQ, "", 0)) { s_limpar_pend = true; }
}

void mesh_diario_carregar(void)
{
  if (s_carregado) { return; }
  static bool carona = false;
  if (!carona) { carona = tab5_sd_carona(na_carona); }
  if (!tab5_sd_montar()) { s_carregado = true; return; }   // sem cartao: so a RAM desta sessao
  FILE* f = tab5_sd_abrir(PASTA, ARQ, "r");
  if (f) {
    char l[160];
    mesh_reinicio_t r;
    while (fgets(l, sizeof(l), f)) {
      if (!le_linha(l, &r)) { continue; }
      juntar(r);
      if (r.contador) { s_ult_cont = r.contador; }
    }
    fclose(f);
  }
  tab5_sd_desmontar();
  s_carregado = true;
  ESP_LOGI(TAG, "diario: %d registro(s), ultimo contador %lu", s_n, (unsigned long)s_ult_cont);
}

void mesh_diario_foto(uint32_t up_no_s)
{
  s_foto.tem = true; s_foto.ms = tab5_agora_ms() | 1u; s_foto.up_no_s = up_no_s;
  // O contexto do Tab5 e lido na hora do REGISTRO, mas o tempo dos eventos e
  // relativo a ela: a foto so marca quando o aviso chegou.
}

void mesh_diario_contador(uint32_t contador, uint32_t up_no_s, bool conectados_antes, bool pedido_nosso)
{
  if (contador == 0 || !s_carregado) { return; }
  const uint32_t ant = s_ult_cont;
  if (contador == ant) { return; }

  mesh_reinicio_t r;
  memset(&r, 0, sizeof(r));
  r.epoch = tab5_hora_epoch();
  r.up_tab5_s = tab5_agora_ms() / 1000u;
  r.contador = contador;

  const bool foto_fresca = s_foto.tem && (tab5_agora_ms() - s_foto.ms) < FOTO_VALE_MS;
  // Uptime de ANTES de cair: o da foto (tirada ao chegar o aviso, antes da
  // telemetria nova) ou, sem foto, o que o servico ainda guarda.
  r.up_no_s = foto_fresca ? s_foto.up_no_s : up_no_s;

  if (ant == 0 || contador < ant) { r.origem = MESH_REI_BASE; }   // sem referencia, ou o no foi zerado/regravado
  else if (pedido_nosso)          { r.origem = MESH_REI_PEDIDO; }
  else if (!conectados_antes)     { r.origem = MESH_REI_AUSENTE; }
  else                            { r.origem = MESH_REI_SOZINHO; }

  const tab5_medida_t m = tab5_power_ultima();
  r.centivolts = (uint16_t)(m.volts > 0 ? m.volts * 100.0f : 0);
  r.ma = (int32_t)m.mA;
  r.envio_s = ha_s(s_envio_ms);
  r.envio_tipo = s_envio_tipo;
  r.grove_s = ha_s(tab5_power_grove_toque_ms());
  r.perfil_s = ha_s(tab5_power_aplicar_ms());
  // O aviso chegou "foto" depois do reinicio: os tempos acima contam desde o
  // REGISTRO; tira o intervalo ja corrido para ficarem relativos ao reinicio.
  if (foto_fresca) {
    const int32_t atraso = (int32_t)((tab5_agora_ms() - s_foto.ms) / 1000u);
    if (r.envio_s >= 0) { r.envio_s = r.envio_s > atraso ? r.envio_s - atraso : 0; }
    if (r.grove_s >= 0) { r.grove_s = r.grove_s > atraso ? r.grove_s - atraso : 0; }
    if (r.perfil_s >= 0) { r.perfil_s = r.perfil_s > atraso ? r.perfil_s - atraso : 0; }
  }
  s_foto.tem = false;
  s_ult_cont = contador;
  juntar(r);

  char linha[160];
  linha_de(&r, linha, sizeof(linha));
  tab5_sd_anexar(PASTA, ARQ, linha);
  ESP_LOGW(TAG, "no reiniciou (contador %lu -> %lu, origem %u): %s", (unsigned long)ant, (unsigned long)contador,
           (unsigned)r.origem, mesh_diario_veredito(&r));
}

int mesh_diario_ler(mesh_reinicio_t* o, int max)
{
  int k = 0;
  taskENTER_CRITICAL(&s_mux);
  for (int i = s_n - 1; i >= 0 && k < max; --i) { o[k++] = s_e[i]; }
  taskEXIT_CRITICAL(&s_mux);
  return k;
}

uint32_t mesh_diario_versao(void) { return s_versao; }

void mesh_diario_limpar(void)
{
  taskENTER_CRITICAL(&s_mux);
  s_n = 0;
  taskEXIT_CRITICAL(&s_mux);
  s_limpar_pend = true;
  ++s_versao;
  // o contador de referencia fica: apagar o diario nao faz o proximo reinicio virar "base"
}

const char* mesh_diario_veredito(const mesh_reinicio_t* r)
{
  switch (r->origem) {
    case MESH_REI_BASE:    return "ponto de partida (primeira leitura)";
    case MESH_REI_PEDIDO:  return "pedido pelo Tab5 (gravar configuração ou reiniciar)";
    case MESH_REI_AUSENTE: return "reiniciou com o Tab5 desconectado - sem como saber";
    default: break;
  }
  if (r->grove_s >= 0 && r->grove_s <= JANELA_S) { return "o Tab5 mexeu no 5V do Grove na hora - provável causa"; }
  if (r->perfil_s >= 0 && r->perfil_s <= JANELA_S) { return "o Tab5 reaplicou o perfil de energia na hora - suspeito"; }
  if (r->envio_s >= 0 && r->envio_s <= 10) {
    return r->envio_tipo == MESH_ENV_ADMIN ? "logo depois de um comando de admin do Tab5 - suspeito"
         : r->envio_tipo == MESH_ENV_CONFIG ? "logo depois do pedido de configuração do Tab5 - suspeito"
         : "logo depois de um pacote enviado pelo Tab5 - suspeito";
  }
  if (r->centivolts && r->centivolts < 650) { return "bateria do Tab5 baixa na hora - veja a alimentação"; }
  return "sem indício do lado do Tab5: falha do próprio nó (firmware ou energia do módulo)";
}
