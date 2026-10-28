// APP: Exemplo - one cartao para cada API of sistema.
//
// E o mostruario e o molde: cada line of tabela usa UMA API of sistema
// (API.md) of jeito certo, com o minimum of code. Para write one app
// novo, copie esta pasta para main/app_<name>/, renomeie APP_EXAMPLE para
// APP_<NOME> e apague os cartoes que nao interessam.
//
// O QUE ESTE ARQUIVO MOSTRA, ALEM DE CADA CARTAO:
//   - a regra of ouro: so a tarefa of interface desenha. A tarefa of app
//     (cartao TAREFA), o worker (TRABALHO), a parte of fundo (FUNDO) e o
//     console (CONSOLE) so mudam DADO e pedem redraw;
//   - ciclo of vida: everything o que ao_abrir() cria, ao_fechar() destroi;
//   - a lista rolavel of sistema (kernel/list): cada cartao so e repintado
//     when o valor dele muda;
//   - commands of console of app: 'app exemplo count 5'.

#include "app.h"
#include "list.hpp"
#include "app_task.h"
#include "work.hpp"
#include "background.h"
#include "notification.h"
#include "tab5_ui.hpp"
#include "tab5_sd.hpp"
#include "tab5_time.hpp"
#include "tab5_imu.hpp"
#include "tab5_gps.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "tab5_keyboard.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// ESTADO
// ---------------------------------------------------------------------------
// Tudo o que outra tarefa escreve e a interface le e atomico (one numero) ou
// passa por fila. Texto vindo of outra tarefa nunca e lido "no middle".

static list_t s_lista;

// TAREFA: one contador que a tarefa of app produz e entrega por fila of 1.
static app_task_t  s_tarefa = { };
static QueueHandle_t s_fila   = nullptr;
static uint32_t      s_tarefa_n = 0;

// TOQUE e TECLADO.
static int  s_touches = 0, s_touch_x = -1, s_touch_y = -1;
static char s_key[24] = "";

// TRABALHO: o worker escreve no resultado, a interface so le no ao_aviso.
static std::atomic<int> s_trab_arquivos{-1};   // -1 = nunca rodou
static bool s_trab_running = false;

// CARTAO.
static int s_lines_gravadas = 0;

// NOTIFICACAO.
static int  s_notif = -1;
static bool s_notif_active = false;

// ACORDADO.
static bool s_acordado = false;

// CONSOLE: o comando 'app exemplo count N' soma aqui (outra tarefa).
static std::atomic<int> s_console_soma{0};

// FUNDO: one cronometro que continua com o app closed.
static std::atomic<uint32_t> s_fundo_s{0};

// ---------------------------------------------------------------------------
// OS CARTOES
// ---------------------------------------------------------------------------
// Cada one: titulo, a API que ele usa, como write o valor of now, e one
// botao opcional. A tabela abaixo e a screen inteira.
typedef struct {
  const char* titulo;
  const char* api;
  void (*valor)(char* out, size_t n);
  const char* botao;            // nullptr = sem botao
  void (*on_botao)(void);
} cartao_t;

#define VALOR_MAX 64

// ---- TELA -------------------------------------------------------------------
static void v_screen(char* o, size_t n)
{
  const area_t a = system_area();
  snprintf(o, n, "area do app: %d x %d", a.width, a.height);
}

// ---- TOQUE ------------------------------------------------------------------
static void v_touch(char* o, size_t n)
{
  if (s_touches == 0) { snprintf(o, n, "toque em qualquer cartao"); return; }
  snprintf(o, n, "%d toque(s), ultimo em %d,%d", s_touches, s_touch_x, s_touch_y);
}

// ---- TAREFA -----------------------------------------------------------------
// A tarefa of app NAO desenha: produz, entrega pela fila e pede redraw.
static void laco_tarefa(app_task_t* t)
{
  uint32_t n = 0;
  while (app_task_active(t)) {
    ++n;
    xQueueOverwrite(s_fila, &n);       // fila of 1: so o mais recente importa
    system_request_redraw();
    vTaskDelay(pdMS_TO_TICKS(1000));   // tarefa_app_encerrar acorda daqui
  }
}

static void v_tarefa(char* o, size_t n)
{
  uint32_t v;
  if (s_fila && xQueueReceive(s_fila, &v, 0) == pdTRUE) { s_tarefa_n = v; }
  snprintf(o, n, "a tarefa contou %lu", (unsigned long)s_tarefa_n);
}

// ---- TRABALHO (worker of sistema) -------------------------------------------
// Roda in tarefa of worker, nunca in interface: pode read o cartao a vontade.
// Termina e o app recebe AVISO_TRABALHO (declarado em .notices).
static void trab_count(void*)
{
  int n = 0;
  char pasta[96];
  if (tab5_sd_mounted() && tab5_sd_folder("Example", pasta, sizeof(pasta))) {
    DIR* d = opendir(pasta);
    if (d) {
      while (readdir(d) && !system_work_cancelled()) { ++n; }
      closedir(d);
    }
  }
  s_trab_arquivos = n;
}

static void v_work(char* o, size_t n)
{
  const int a = s_trab_arquivos;
  if (s_trab_running) { snprintf(o, n, "contando..."); }
  else if (a < 0)     { snprintf(o, n, "conta os arquivos da pasta do app"); }
  else                { snprintf(o, n, "%d arquivo(s) em apps/Exemplo", a); }
}

static void b_work(void)
{
  if (!s_trab_running && system_work(trab_count, nullptr)) { s_trab_running = true; }
}

// ---- CARTAO -----------------------------------------------------------------
// tab5_sd_enqueue_append nao espera: o sistema grava em lote, e guarda no diario se o
// cartao estiver emprestado on computador.
static void v_cartao(char* o, size_t n)
{
  snprintf(o, n, s_lines_gravadas ? "%d linha(s) em apps/Exemplo/diario.csv"
                                   : "grava uma linha no CSV do app", s_lines_gravadas);
}

static void b_cartao(void)
{
  char line[48];
  snprintf(line, sizeof(line), "%lu,%d\n", (unsigned long)tab5_time_epoch(), s_lines_gravadas + 1);
  if (tab5_sd_enqueue_append("Example", "diario.csv", line, "epoch,n\n")) { ++s_lines_gravadas; }
}

// ---- HORA -------------------------------------------------------------------
static void v_hora(char* o, size_t n)
{
  const uint32_t e = tab5_time_epoch();
  if (!e) { snprintf(o, n, "relogio ainda sem acerto"); return; }
  struct tm t;
  tab5_local_time(e, &t);
  snprintf(o, n, "%02d/%02d/%04d %02d:%02d:%02d (%s)", t.tm_mday, t.tm_mon + 1,
           t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec, tab5_time_source());
}

// ---- ENERGIA ----------------------------------------------------------------
// tab5_power_latest(): a measurement que a interface ja fez, sem touch no I2C.
static void v_energia(char* o, size_t n)
{
  const tab5_measurement_t m = tab5_power_latest();
  if (m.pct >= 0) { snprintf(o, n, "%d%%  %.2f V  %+.0f mA", m.pct, (double)m.volts, (double)m.mA); }
  else            { snprintf(o, n, "%.2f V  %+.0f mA", (double)m.volts, (double)m.mA); }
}

// ---- IMU --------------------------------------------------------------------
// Exige .profile.imu = true (o sistema so liga a IMU of quem pede).
static void v_imu(char* o, size_t n)
{
  tab5_imu_data_t d;
  if (!tab5_imu_latest(&d) || !d.valid) { snprintf(o, n, "IMU sem leitura"); return; }
  snprintf(o, n, "x %+.2f g  y %+.2f g  z %+.2f g", (double)d.ax, (double)d.ay, (double)d.az);
}

// ---- GPS --------------------------------------------------------------------
static void v_gps(char* o, size_t n)
{
  if (!tab5_gps_on_air()) { snprintf(o, n, "sem GPS no Grove"); return; }
  const nmea_position_t p = tab5_gps_position();
  if (p.qualidade == 0) { snprintf(o, n, "GPS sem fix (%u sat)", (unsigned)p.sats_usados); return; }
  snprintf(o, n, "%.5f, %.5f  (%u sat)", p.lat, p.lon, (unsigned)p.sats_usados);
}

// ---- WIFI -------------------------------------------------------------------
static void v_wifi(char* o, size_t n)
{
  const char* ip = tab5_wifi_ip();
  if (ip && ip[0]) { snprintf(o, n, "%s  %s  %d dBm", tab5_wifi_ssid(), ip, (int)tab5_wifi_rssi()); }
  else             { snprintf(o, n, "%s", tab5_wifi_description()); }
}

// ---- ESP-NOW ----------------------------------------------------------------
static void v_espnow(char* o, size_t n)
{
  if (!tab5_espnow_pronto()) { snprintf(o, n, "fora do ar (radio C6 de fabrica ou Wi-Fi desligado)"); return; }
  const int k = tab5_espnow_n();
  espnow_msg_t m;
  if (k > 0 && tab5_espnow_msg(k - 1, &m)) { snprintf(o, n, "canal %u, ultima: %.28s", tab5_espnow_canal(), m.text); }
  else                                     { snprintf(o, n, "canal %u, nada recebido", tab5_espnow_canal()); }
}

static void b_espnow(void) { tab5_espnow_send("ola do Exemplo"); }

// ---- MESH -------------------------------------------------------------------
// So leitura: one app of exemplo nao deve exit falando num canal publico. Para
// send: tab5_mesh_enviar(0xFFFFFFFF, canal, text) ou para one no.
static void v_mesh(char* o, size_t n)
{
  if (!tab5_mesh_servico_active()) { snprintf(o, n, "servico desligado (Ajustes > Mesh)"); return; }
  snprintf(o, n, "%s, %d no(s), %d nao lida(s)", tab5_mesh_no_ar() ? "no ar" : "mudo",
           tab5_mesh_num_nos(), tab5_mesh_nao_lidas());
}

// ---- NOTIFICACAO ------------------------------------------------------------
// Estado, nao evento: publicar of novo SUBSTITUI. Quantidade 0 apaga.
static void v_notif(char* o, size_t n)
{
  snprintf(o, n, s_notif_active ? "aviso na barra - toque para tirar" : "poe um aviso na barra de status");
}

static void b_notif(void)
{
  s_notif_active = !s_notif_active;
  if (s_notif_active) { notification_publicar(s_notif, 1, NOTIF_ATENCAO, "aviso do Exemplo"); }
  else               { notification_limpar(s_notif); }
}

// ---- SOM --------------------------------------------------------------------
// Exige .profile.speaker = true. Sempre pelo sistema_beep (o codec frio
// engoliria one tone() direto).
static void v_sound(char* o, size_t n) { snprintf(o, n, "sistema_beep(880 Hz, 120 ms)"); }
static void b_sound(void) { system_beep(880.0f, 120); }

// ---- TECLADO ----------------------------------------------------------------
static void v_keyboard(char* o, size_t n)
{
  if (!tab5_keyboard_present()) { snprintf(o, n, "encaixe o teclado no ExtPort1"); return; }
  snprintf(o, n, s_key[0] ? "ultima tecla: %s" : "digite algo", s_key);
}

// ---- ACORDADO ---------------------------------------------------------------
static void v_acordado(char* o, size_t n)
{
  snprintf(o, n, s_acordado ? "tela nao apaga enquanto o app estiver aberto"
                            : "a tela apaga sozinha (padrao)");
}

static void b_acordado(void)
{
  s_acordado = !s_acordado;
  system_manter_acordado(SISTEMA_ACORDADO_APP, s_acordado);   // released sozinho on close
}

// ---- FUNDO ------------------------------------------------------------------
// O loop roda numa tarefa of KERNEL e continua com o app closed; enquanto
// roda, a barra mostra uma pastilha com o summary. Nao desenha nada.
static void laco_fundo(background_t* f)
{
  s_fundo_s = 0;
  while (background_wait(f, 1000)) {
    const uint32_t s = ++s_fundo_s;
    char r[NOTIF_RESUMO];
    snprintf(r, sizeof(r), "%lu s", (unsigned long)s);
    background_estado(f, (int)s, r);
    system_request_redraw();         // so tem efeito com a window open
  }
}

static bool background_sem_sound(void) { return false; }

static const background_def_t EXEMPLO_FUNDO = {
  .name  = "Example",
  .body = laco_fundo,
  .stack = 3072,
  .profile = { },
  .piso_sleep_mhz = 0,
  .on_start = nullptr,
  .on_stop   = nullptr,
  .com_sound    = background_sem_sound,
};

static void v_fundo(char* o, size_t n)
{
  if (system_fundo_active(&EXEMPLO_FUNDO)) {
    snprintf(o, n, "cronometro: %lu s (feche o app e veja a barra)", (unsigned long)s_fundo_s.load());
  } else {
    snprintf(o, n, "um cronometro que sobrevive ao app fechado");
  }
}

static void b_fundo(void)
{
  if (system_fundo_active(&EXEMPLO_FUNDO)) { system_fundo_stop(&EXEMPLO_FUNDO); }
  else                                     { system_fundo_start(&EXEMPLO_FUNDO); }
}

// ---- CONSOLE ----------------------------------------------------------------
static void v_console(char* o, size_t n)
{
  snprintf(o, n, "'app exemplo contar 5' na serial: soma = %d", s_console_soma.load());
}

static const cartao_t CARTOES[] = {
  { "TELA",        "sistema_area, tab5_ui, pintura",       v_screen,     nullptr,   nullptr },
  { "TOQUE",       "ao_tocar(x, y) - na soltura",           v_touch,    nullptr,   nullptr },
  { "TAREFA",      "tarefa_app + fila",                     v_tarefa,   nullptr,   nullptr },
  { "TRABALHO",    "sistema_trabalho + AVISO_TRABALHO",     v_work, "contar",  b_work },
  { "CARTAO",      "tab5_sd_enqueue_append",                        v_cartao,   "gravar",  b_cartao },
  { "HORA",        "tab5_time_epoch + tab5_local_time",     v_hora,     nullptr,   nullptr },
  { "ENERGIA",     "tab5_power_latest",                     v_energia,  nullptr,   nullptr },
  { "IMU",         "tab5_imu_latest (perfil.imu)",          v_imu,      nullptr,   nullptr },
  { "GPS",         "tab5_gps_position",                      v_gps,      nullptr,   nullptr },
  { "WIFI",        "tab5_wifi_ssid / _ip / _rssi",          v_wifi,     nullptr,   nullptr },
  { "ESP-NOW",     "tab5_espnow_enviar + AVISO_ESPNOW",     v_espnow,   "difundir", b_espnow },
  { "MESH",        "tab5_mesh_* + AVISO_MESH_*",            v_mesh,     nullptr,   nullptr },
  { "NOTIFICACAO", "notif_publicar / notif_limpar",         v_notif,    "avisar",  b_notif },
  { "SOM",         "sistema_beep (perfil.speaker)",    v_sound,      "bip",     b_sound },
  { "TECLADO",     "ao_teclar(texto, codigo, mod)",         v_keyboard,  nullptr,   nullptr },
  { "ACORDADO",    "sistema_manter_acordado",               v_acordado, "trocar",  b_acordado },
  { "FUNDO",       "background.h: sistema_fundo_iniciar",        v_fundo,    "ligar",   b_fundo },
  { "CONSOLE",     "app_t.comandos",                        v_console,  nullptr,   nullptr },
};
#define N_CARTOES ((int)(sizeof(CARTOES) / sizeof(CARTOES[0])))

// ---------------------------------------------------------------------------
// DESENHO
// ---------------------------------------------------------------------------
static const int CARTAO_A = 132;          // height of cada cartao
static const int BOTAO_L = 170, BOTAO_A = 64;
static const int MARGEM = 16;

static char s_valor[N_CARTOES][VALOR_MAX];   // o que esta in screen now

static int cartao_height(int, void*) { return CARTAO_A; }

// A lista reserva 12 px a right para a barra of rolagem: a width que o
// cartao recebe e esta (a MESMA conta no drawing e no touch).
static int width_cartao(const area_t& a) { return a.width - 12; }

// Onde stays o botao of one cartao (x relativo a window, y relativo on cartao).
static int botao_x(int width) { return width - MARGEM - BOTAO_L - 8; }
static int botao_y(void)        { return (CARTAO_A - BOTAO_A) / 2; }

static void cartao_draw(lgfx::LovyanGFX& d, int i, int x, int y, int width, void*)
{
  const cartao_t& c = CARTOES[i];
  const lgfx::TextStyle estilo = d.getTextStyle();   // copia: devolvemos no end
  // (o fundo of cartao a lista ja pintou - nao limpar of novo)
  d.drawFastHLine(x + MARGEM, y + CARTAO_A - 1, width - 2 * MARGEM, SYSTEM_LINE);

  d.setTextDatum(top_left);
  tab5_fonte_body_negrito(d);
  tab5_text(d, 3.0f);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString(c.titulo, x + MARGEM, y + 14);

  tab5_fonte_body(d);
  tab5_text(d, 2.2f);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString(c.api, x + MARGEM, y + 52);

  // O valor: cortado com "..." se nao couber antes of botao.
  char v[VALOR_MAX];
  snprintf(v, sizeof(v), "%s", s_valor[i]);
  tab5_text(d, 2.6f);
  const int lim = (c.botao ? botao_x(width) : width - MARGEM) - MARGEM - 8;
  tab5_cortar_com_reticencias(d, v, sizeof(v), lim);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(v, x + MARGEM, y + 84);

  if (c.botao) {
    tab5_botao(d, x + botao_x(width), y + botao_y(), BOTAO_L, BOTAO_A, c.botao, TFT_SKYBLUE);
  }
  d.setTextStyle(estilo);
}

// Recalcula o valor of cada cartao e marca para repintar SO os que changed.
static void atualizar_valores(bool todos)
{
  for (int i = 0; i < N_CARTOES; ++i) {
    char v[VALOR_MAX];
    CARTOES[i].valor(v, sizeof(v));
    if (todos || strcmp(v, s_valor[i]) != 0) {
      memcpy(s_valor[i], v, sizeof(v));
      if (!todos) { list_item_changed(&s_lista, i); }
    }
  }
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  const bool everything = system_area_foi_limpa();
  atualizar_valores(everything);
  list_draw(&s_lista, d, a, TFT_BLACK, everything);
}

// ---------------------------------------------------------------------------
// TOQUE, TECLADO, AVISOS
// ---------------------------------------------------------------------------
static void on_touch_level(int x, int y, bool pressed)
{
  if (list_drag(&s_lista, x, y, pressed)) { system_request_redraw(); }
}

static void on_touch(int x, int y)
{
  int dy = 0;
  const int i = list_item_touched(&s_lista, x, y, &dy);
  if (i < 0) { return; }                        // fora of lista, ou end of arraste
  ++s_touches; s_touch_x = x; s_touch_y = y;
  const cartao_t& c = CARTOES[i];
  const area_t a = system_area();
  if (c.botao && tab5_dentro(x, dy, botao_x(width_cartao(a)), botao_y(), BOTAO_L, BOTAO_A)) {
    c.on_botao();
  }
  system_request_redraw();
}

static void on_type(const char* text, uint8_t code, uint8_t)
{
  if (text[0]) { snprintf(s_key, sizeof(s_key), "\"%s\"", text); }
  else          { snprintf(s_key, sizeof(s_key), "codigo %u", (unsigned)code); }
}

// Os servicos avisam (AVISO_*) e o app redesenha in hora, sem perguntar a
// cada segundo. AVISO_TRABALHO e o end of sistema_trabalho().
static void on_aviso(uint32_t notices)
{
  if (notices & AVISO_TRABALHO) { s_trab_running = false; }
  system_request_redraw();
}

// ---------------------------------------------------------------------------
// CICLO DE VIDA
// ---------------------------------------------------------------------------
static bool on_open(void)
{
  s_fila = xQueueCreate(1, sizeof(uint32_t));
  if (!s_fila) { return false; }
  if (!app_task_create(&s_tarefa, laco_tarefa, "exemplo", 3072, 2)) {
    vQueueDelete(s_fila);
    s_fila = nullptr;
    return false;
  }
  s_notif = notification_registrar("Example");    // idempotente: a mesma vaga a cada abertura
  notification_associar_app(s_notif, "Example");  // touch no aviso abre este app
  tab5_espnow_window(true);                // receptor of ESP-NOW enabled com o app in screen
  list_set(&s_lista, ROLAGEM_INICIO, N_CARTOES, cartao_height, cartao_draw, nullptr);
  return true;
}

static void on_close(void)
{
  // A tarefa SAI antes of a fila dela ser destruida: stop espera.
  app_task_stop(&s_tarefa, 200);
  if (s_fila) { vQueueDelete(s_fila); s_fila = nullptr; }
  tab5_espnow_window(false);
  // O aviso e o fundo continuam of proposito: sobrevivem a window.
  s_acordado = false;                      // o sistema ja soltou o request
  s_trab_running = false;                  // work pending foi descartado
}

// ---------------------------------------------------------------------------
// CONSOLE: 'app exemplo count [n]' e 'app exemplo reset'
// ---------------------------------------------------------------------------
// Rodam in tarefa of console: so mexem em dado atomico e pedem redraw.
static void cmd_count(int argc, char** argv)
{
  const int n = argc >= 2 ? atoi(argv[1]) : 1;
  printf("soma = %d\n", s_console_soma += n);
  system_request_redraw();
}

static void cmd_reset(int, char**)
{
  s_console_soma = 0;
  printf("soma zerada\n");
  system_request_redraw();
}

static const app_comando_t COMANDOS[] = {
  { "contar", "[n]\tsoma n (padrao 1) no cartao CONSOLE", cmd_count, false },
  { "zerar",  "\tzera a soma (so com o app aberto)",      cmd_reset,  true },
  { nullptr },
};

// ---------------------------------------------------------------------------
// ICONE: quadrado tracejado com one "+" - o molde of one app.
// ---------------------------------------------------------------------------
static void icon_exemplo(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t traco = TFT_CYAN;
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(20, 50, 55));
  const int m = (int)(0.24f * lado), L = lado - 2 * m, passo = L / 7, e = lado / 40;
  for (int i = 0; i < 7; i += 2) {
    d.fillRect(x + m + i * passo, y + m,             passo, e, traco);
    d.fillRect(x + m + i * passo, y + m + L,         passo, e, traco);
    d.fillRect(x + m,             y + m + i * passo, e, passo, traco);
    d.fillRect(x + m + L,         y + m + i * passo, e, passo, traco);
  }
  const int g = lado / 28;
  d.fillRect(x + lado / 2 - L / 6, y + lado / 2 - g, L / 3, 2 * g, traco);
  d.fillRect(x + lado / 2 - g, y + lado / 2 - L / 6, 2 * g, L / 3, traco);
}

// 'extern' e OBRIGATORIO: em C++ one const of arquivo tem ligacao interna, e o
// apps.cpp nao enxergaria o simbolo (o erro so aparece no link).
extern const app_t APP_EXAMPLE = {
  .name  = "Example",
  .color   = TFT_CYAN,
  // So o que os cartoes usam. O resto stays disabled enquanto o app esta in
  // screen - e o que impede o aparelho of alimentar o que ninguem usa.
  .profile = { .imu = true, .speaker = true },
  .uses_sd = true,               // monta o cartao antes of ao_abrir (TRABALHO le a pasta)
  .orientation = ORIENTATION_PORTRAIT,
  .piso_sleep_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,
  .on_type = on_type,
  .periodo_ms = 500,            // hora, energia e IMU mudam sozinhos
  .notices = AVISO_TRABALHO | AVISO_ESPNOW | AVISO_MESH_MSG | AVISO_MESH_NOS | AVISO_GPS,
  .on_aviso = on_aviso,
  .icon = icon_exemplo,
  .commands = COMANDOS,
};
