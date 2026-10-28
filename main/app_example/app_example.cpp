// APP: Exemplo - um cartao para cada API do sistema.
//
// E o mostruario e o molde: cada linha da tabela usa UMA API do sistema
// (API.md) do jeito certo, com o minimo de codigo. Para escrever um app
// novo, copie esta pasta para main/app_<nome>/, renomeie APP_EXAMPLE para
// APP_<NOME> e apague os cartoes que nao interessam.
//
// O QUE ESTE ARQUIVO MOSTRA, ALEM DE CADA CARTAO:
//   - a regra de ouro: so a tarefa de interface desenha. A tarefa do app
//     (cartao TAREFA), o worker (TRABALHO), a parte de fundo (FUNDO) e o
//     console (CONSOLE) so mudam DADO e pedem redesenho;
//   - ciclo de vida: tudo o que ao_abrir() cria, ao_fechar() destroi;
//   - a lista rolavel do sistema (kernel/list): cada cartao so e repintado
//     quando o valor dele muda;
//   - comandos de console do app: 'app exemplo contar 5'.

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
// Tudo o que outra tarefa escreve e a interface le e atomico (um numero) ou
// passa por fila. Texto vindo de outra tarefa nunca e lido "no meio".

static lista_t s_lista;

// TAREFA: um contador que a tarefa do app produz e entrega por fila de 1.
static tarefa_app_t  s_tarefa = { };
static QueueHandle_t s_fila   = nullptr;
static uint32_t      s_tarefa_n = 0;

// TOQUE e TECLADO.
static int  s_toques = 0, s_toque_x = -1, s_toque_y = -1;
static char s_tecla[24] = "";

// TRABALHO: o worker escreve no resultado, a interface so le no ao_aviso.
static std::atomic<int> s_trab_arquivos{-1};   // -1 = nunca rodou
static bool s_trab_rodando = false;

// CARTAO.
static int s_linhas_gravadas = 0;

// NOTIFICACAO.
static int  s_notif = -1;
static bool s_notif_ativa = false;

// ACORDADO.
static bool s_acordado = false;

// CONSOLE: o comando 'app exemplo contar N' soma aqui (outra tarefa).
static std::atomic<int> s_console_soma{0};

// FUNDO: um cronometro que continua com o app fechado.
static std::atomic<uint32_t> s_fundo_s{0};

// ---------------------------------------------------------------------------
// OS CARTOES
// ---------------------------------------------------------------------------
// Cada um: titulo, a API que ele usa, como escrever o valor de agora, e um
// botao opcional. A tabela abaixo e a tela inteira.
typedef struct {
  const char* titulo;
  const char* api;
  void (*valor)(char* out, size_t n);
  const char* botao;            // nullptr = sem botao
  void (*ao_botao)(void);
} cartao_t;

#define VALOR_MAX 64

// ---- TELA -------------------------------------------------------------------
static void v_tela(char* o, size_t n)
{
  const area_t a = sistema_area();
  snprintf(o, n, "area do app: %d x %d", a.largura, a.altura);
}

// ---- TOQUE ------------------------------------------------------------------
static void v_toque(char* o, size_t n)
{
  if (s_toques == 0) { snprintf(o, n, "toque em qualquer cartao"); return; }
  snprintf(o, n, "%d toque(s), ultimo em %d,%d", s_toques, s_toque_x, s_toque_y);
}

// ---- TAREFA -----------------------------------------------------------------
// A tarefa do app NAO desenha: produz, entrega pela fila e pede redesenho.
static void laco_tarefa(tarefa_app_t* t)
{
  uint32_t n = 0;
  while (tarefa_app_ativa(t)) {
    ++n;
    xQueueOverwrite(s_fila, &n);       // fila de 1: so o mais recente importa
    sistema_pedir_redesenho();
    vTaskDelay(pdMS_TO_TICKS(1000));   // tarefa_app_encerrar acorda daqui
  }
}

static void v_tarefa(char* o, size_t n)
{
  uint32_t v;
  if (s_fila && xQueueReceive(s_fila, &v, 0) == pdTRUE) { s_tarefa_n = v; }
  snprintf(o, n, "a tarefa contou %lu", (unsigned long)s_tarefa_n);
}

// ---- TRABALHO (worker do sistema) -------------------------------------------
// Roda na tarefa do worker, nunca na interface: pode ler o cartao a vontade.
// Termina e o app recebe AVISO_TRABALHO (declarado em .avisos).
static void trab_contar(void*)
{
  int n = 0;
  char pasta[96];
  if (tab5_sd_mounted() && tab5_sd_folder("Example", pasta, sizeof(pasta))) {
    DIR* d = opendir(pasta);
    if (d) {
      while (readdir(d) && !sistema_trabalho_cancelado()) { ++n; }
      closedir(d);
    }
  }
  s_trab_arquivos = n;
}

static void v_trabalho(char* o, size_t n)
{
  const int a = s_trab_arquivos;
  if (s_trab_rodando) { snprintf(o, n, "contando..."); }
  else if (a < 0)     { snprintf(o, n, "conta os arquivos da pasta do app"); }
  else                { snprintf(o, n, "%d arquivo(s) em apps/Exemplo", a); }
}

static void b_trabalho(void)
{
  if (!s_trab_rodando && sistema_trabalho(trab_contar, nullptr)) { s_trab_rodando = true; }
}

// ---- CARTAO -----------------------------------------------------------------
// tab5_sd_enqueue_append nao espera: o sistema grava em lote, e guarda no diario se o
// cartao estiver emprestado ao computador.
static void v_cartao(char* o, size_t n)
{
  snprintf(o, n, s_linhas_gravadas ? "%d linha(s) em apps/Exemplo/diario.csv"
                                   : "grava uma linha no CSV do app", s_linhas_gravadas);
}

static void b_cartao(void)
{
  char linha[48];
  snprintf(linha, sizeof(linha), "%lu,%d\n", (unsigned long)tab5_hora_epoch(), s_linhas_gravadas + 1);
  if (tab5_sd_enqueue_append("Example", "diario.csv", linha, "epoch,n\n")) { ++s_linhas_gravadas; }
}

// ---- HORA -------------------------------------------------------------------
static void v_hora(char* o, size_t n)
{
  const uint32_t e = tab5_hora_epoch();
  if (!e) { snprintf(o, n, "relogio ainda sem acerto"); return; }
  struct tm t;
  tab5_hora_local(e, &t);
  snprintf(o, n, "%02d/%02d/%04d %02d:%02d:%02d (%s)", t.tm_mday, t.tm_mon + 1,
           t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec, tab5_hora_fonte());
}

// ---- ENERGIA ----------------------------------------------------------------
// tab5_power_latest(): a medida que a interface ja fez, sem tocar no I2C.
static void v_energia(char* o, size_t n)
{
  const tab5_measurement_t m = tab5_power_latest();
  if (m.pct >= 0) { snprintf(o, n, "%d%%  %.2f V  %+.0f mA", m.pct, (double)m.volts, (double)m.mA); }
  else            { snprintf(o, n, "%.2f V  %+.0f mA", (double)m.volts, (double)m.mA); }
}

// ---- IMU --------------------------------------------------------------------
// Exige .perfil.imu = true (o sistema so liga a IMU de quem pede).
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
  if (k > 0 && tab5_espnow_msg(k - 1, &m)) { snprintf(o, n, "canal %u, ultima: %.28s", tab5_espnow_canal(), m.texto); }
  else                                     { snprintf(o, n, "canal %u, nada recebido", tab5_espnow_canal()); }
}

static void b_espnow(void) { tab5_espnow_enviar("ola do Exemplo"); }

// ---- MESH -------------------------------------------------------------------
// So leitura: um app de exemplo nao deve sair falando num canal publico. Para
// enviar: tab5_mesh_enviar(0xFFFFFFFF, canal, texto) ou para um no.
static void v_mesh(char* o, size_t n)
{
  if (!tab5_mesh_servico_ativo()) { snprintf(o, n, "servico desligado (Ajustes > Mesh)"); return; }
  snprintf(o, n, "%s, %d no(s), %d nao lida(s)", tab5_mesh_no_ar() ? "no ar" : "mudo",
           tab5_mesh_num_nos(), tab5_mesh_nao_lidas());
}

// ---- NOTIFICACAO ------------------------------------------------------------
// Estado, nao evento: publicar de novo SUBSTITUI. Quantidade 0 apaga.
static void v_notif(char* o, size_t n)
{
  snprintf(o, n, s_notif_ativa ? "aviso na barra - toque para tirar" : "poe um aviso na barra de status");
}

static void b_notif(void)
{
  s_notif_ativa = !s_notif_ativa;
  if (s_notif_ativa) { notif_publicar(s_notif, 1, NOTIF_ATENCAO, "aviso do Exemplo"); }
  else               { notif_limpar(s_notif); }
}

// ---- SOM --------------------------------------------------------------------
// Exige .perfil.speaker = true. Sempre pelo sistema_beep (o codec frio
// engoliria um tone() direto).
static void v_som(char* o, size_t n) { snprintf(o, n, "sistema_beep(880 Hz, 120 ms)"); }
static void b_som(void) { sistema_beep(880.0f, 120); }

// ---- TECLADO ----------------------------------------------------------------
static void v_teclado(char* o, size_t n)
{
  if (!tab5_keyboard_present()) { snprintf(o, n, "encaixe o teclado no ExtPort1"); return; }
  snprintf(o, n, s_tecla[0] ? "ultima tecla: %s" : "digite algo", s_tecla);
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
  sistema_manter_acordado(SISTEMA_ACORDADO_APP, s_acordado);   // solto sozinho ao fechar
}

// ---- FUNDO ------------------------------------------------------------------
// O loop roda numa tarefa do KERNEL e continua com o app fechado; enquanto
// roda, a barra mostra uma pastilha com o resumo. Nao desenha nada.
static void laco_fundo(fundo_t* f)
{
  s_fundo_s = 0;
  while (fundo_esperar(f, 1000)) {
    const uint32_t s = ++s_fundo_s;
    char r[NOTIF_RESUMO];
    snprintf(r, sizeof(r), "%lu s", (unsigned long)s);
    fundo_estado(f, (int)s, r);
    sistema_pedir_redesenho();         // so tem efeito com a janela aberta
  }
}

static bool fundo_sem_som(void) { return false; }

static const fundo_def_t EXEMPLO_FUNDO = {
  .name  = "Example",
  .corpo = laco_fundo,
  .pilha = 3072,
  .perfil = { },
  .piso_repouso_mhz = 0,
  .ao_iniciar = nullptr,
  .ao_parar   = nullptr,
  .com_som    = fundo_sem_som,
};

static void v_fundo(char* o, size_t n)
{
  if (sistema_fundo_ativo(&EXEMPLO_FUNDO)) {
    snprintf(o, n, "cronometro: %lu s (feche o app e veja a barra)", (unsigned long)s_fundo_s.load());
  } else {
    snprintf(o, n, "um cronometro que sobrevive ao app fechado");
  }
}

static void b_fundo(void)
{
  if (sistema_fundo_ativo(&EXEMPLO_FUNDO)) { sistema_fundo_parar(&EXEMPLO_FUNDO); }
  else                                     { sistema_fundo_iniciar(&EXEMPLO_FUNDO); }
}

// ---- CONSOLE ----------------------------------------------------------------
static void v_console(char* o, size_t n)
{
  snprintf(o, n, "'app exemplo contar 5' na serial: soma = %d", s_console_soma.load());
}

static const cartao_t CARTOES[] = {
  { "TELA",        "sistema_area, tab5_ui, pintura",       v_tela,     nullptr,   nullptr },
  { "TOQUE",       "ao_tocar(x, y) - na soltura",           v_toque,    nullptr,   nullptr },
  { "TAREFA",      "tarefa_app + fila",                     v_tarefa,   nullptr,   nullptr },
  { "TRABALHO",    "sistema_trabalho + AVISO_TRABALHO",     v_trabalho, "contar",  b_trabalho },
  { "CARTAO",      "tab5_sd_enqueue_append",                        v_cartao,   "gravar",  b_cartao },
  { "HORA",        "tab5_hora_epoch + tab5_hora_local",     v_hora,     nullptr,   nullptr },
  { "ENERGIA",     "tab5_power_latest",                     v_energia,  nullptr,   nullptr },
  { "IMU",         "tab5_imu_latest (perfil.imu)",          v_imu,      nullptr,   nullptr },
  { "GPS",         "tab5_gps_position",                      v_gps,      nullptr,   nullptr },
  { "WIFI",        "tab5_wifi_ssid / _ip / _rssi",          v_wifi,     nullptr,   nullptr },
  { "ESP-NOW",     "tab5_espnow_enviar + AVISO_ESPNOW",     v_espnow,   "difundir", b_espnow },
  { "MESH",        "tab5_mesh_* + AVISO_MESH_*",            v_mesh,     nullptr,   nullptr },
  { "NOTIFICACAO", "notif_publicar / notif_limpar",         v_notif,    "avisar",  b_notif },
  { "SOM",         "sistema_beep (perfil.speaker)",    v_som,      "bip",     b_som },
  { "TECLADO",     "ao_teclar(texto, codigo, mod)",         v_teclado,  nullptr,   nullptr },
  { "ACORDADO",    "sistema_manter_acordado",               v_acordado, "trocar",  b_acordado },
  { "FUNDO",       "background.h: sistema_fundo_iniciar",        v_fundo,    "ligar",   b_fundo },
  { "CONSOLE",     "app_t.comandos",                        v_console,  nullptr,   nullptr },
};
#define N_CARTOES ((int)(sizeof(CARTOES) / sizeof(CARTOES[0])))

// ---------------------------------------------------------------------------
// DESENHO
// ---------------------------------------------------------------------------
static const int CARTAO_A = 132;          // altura de cada cartao
static const int BOTAO_L = 170, BOTAO_A = 64;
static const int MARGEM = 16;

static char s_valor[N_CARTOES][VALOR_MAX];   // o que esta na tela agora

static int cartao_altura(int, void*) { return CARTAO_A; }

// A lista reserva 12 px a direita para a barra de rolagem: a largura que o
// cartao recebe e esta (a MESMA conta no desenho e no toque).
static int largura_cartao(const area_t& a) { return a.largura - 12; }

// Onde fica o botao de um cartao (x relativo a janela, y relativo ao cartao).
static int botao_x(int largura) { return largura - MARGEM - BOTAO_L - 8; }
static int botao_y(void)        { return (CARTAO_A - BOTAO_A) / 2; }

static void cartao_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void*)
{
  const cartao_t& c = CARTOES[i];
  const lgfx::TextStyle estilo = d.getTextStyle();   // copia: devolvemos no fim
  // (o fundo do cartao a lista ja pintou - nao limpar de novo)
  d.drawFastHLine(x + MARGEM, y + CARTAO_A - 1, largura - 2 * MARGEM, SIS_LINHA);

  d.setTextDatum(top_left);
  tab5_fonte_corpo_negrito(d);
  tab5_texto(d, 3.0f);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString(c.titulo, x + MARGEM, y + 14);

  tab5_fonte_corpo(d);
  tab5_texto(d, 2.2f);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString(c.api, x + MARGEM, y + 52);

  // O valor: cortado com "..." se nao couber antes do botao.
  char v[VALOR_MAX];
  snprintf(v, sizeof(v), "%s", s_valor[i]);
  tab5_texto(d, 2.6f);
  const int lim = (c.botao ? botao_x(largura) : largura - MARGEM) - MARGEM - 8;
  tab5_cortar_com_reticencias(d, v, sizeof(v), lim);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(v, x + MARGEM, y + 84);

  if (c.botao) {
    tab5_botao(d, x + botao_x(largura), y + botao_y(), BOTAO_L, BOTAO_A, c.botao, TFT_SKYBLUE);
  }
  d.setTextStyle(estilo);
}

// Recalcula o valor de cada cartao e marca para repintar SO os que mudaram.
static void atualizar_valores(bool todos)
{
  for (int i = 0; i < N_CARTOES; ++i) {
    char v[VALOR_MAX];
    CARTOES[i].valor(v, sizeof(v));
    if (todos || strcmp(v, s_valor[i]) != 0) {
      memcpy(s_valor[i], v, sizeof(v));
      if (!todos) { lista_item_mudou(&s_lista, i); }
    }
  }
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  const bool tudo = sistema_area_foi_limpa();
  atualizar_valores(tudo);
  lista_desenhar(&s_lista, d, a, TFT_BLACK, tudo);
}

// ---------------------------------------------------------------------------
// TOQUE, TECLADO, AVISOS
// ---------------------------------------------------------------------------
static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  int dy = 0;
  const int i = lista_item_tocado(&s_lista, x, y, &dy);
  if (i < 0) { return; }                        // fora da lista, ou fim de arraste
  ++s_toques; s_toque_x = x; s_toque_y = y;
  const cartao_t& c = CARTOES[i];
  const area_t a = sistema_area();
  if (c.botao && tab5_dentro(x, dy, botao_x(largura_cartao(a)), botao_y(), BOTAO_L, BOTAO_A)) {
    c.ao_botao();
  }
  sistema_pedir_redesenho();
}

static void ao_teclar(const char* texto, uint8_t codigo, uint8_t)
{
  if (texto[0]) { snprintf(s_tecla, sizeof(s_tecla), "\"%s\"", texto); }
  else          { snprintf(s_tecla, sizeof(s_tecla), "codigo %u", (unsigned)codigo); }
}

// Os servicos avisam (AVISO_*) e o app redesenha na hora, sem perguntar a
// cada segundo. AVISO_TRABALHO e o fim do sistema_trabalho().
static void ao_aviso(uint32_t avisos)
{
  if (avisos & AVISO_TRABALHO) { s_trab_rodando = false; }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// CICLO DE VIDA
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  s_fila = xQueueCreate(1, sizeof(uint32_t));
  if (!s_fila) { return false; }
  if (!tarefa_app_criar(&s_tarefa, laco_tarefa, "exemplo", 3072, 2)) {
    vQueueDelete(s_fila);
    s_fila = nullptr;
    return false;
  }
  s_notif = notif_registrar("Example");    // idempotente: a mesma vaga a cada abertura
  notif_associar_app(s_notif, "Example");  // tocar no aviso abre este app
  tab5_espnow_janela(true);                // receptor do ESP-NOW ligado com o app na tela
  lista_definir(&s_lista, ROLAGEM_INICIO, N_CARTOES, cartao_altura, cartao_desenhar, nullptr);
  return true;
}

static void ao_fechar(void)
{
  // A tarefa SAI antes de a fila dela ser destruida: encerrar espera.
  tarefa_app_encerrar(&s_tarefa, 200);
  if (s_fila) { vQueueDelete(s_fila); s_fila = nullptr; }
  tab5_espnow_janela(false);
  // O aviso e o fundo continuam de proposito: sobrevivem a janela.
  s_acordado = false;                      // o sistema ja soltou o pedido
  s_trab_rodando = false;                  // trabalho pendente foi descartado
}

// ---------------------------------------------------------------------------
// CONSOLE: 'app exemplo contar [n]' e 'app exemplo zerar'
// ---------------------------------------------------------------------------
// Rodam na tarefa do console: so mexem em dado atomico e pedem redesenho.
static void cmd_contar(int argc, char** argv)
{
  const int n = argc >= 2 ? atoi(argv[1]) : 1;
  printf("soma = %d\n", s_console_soma += n);
  sistema_pedir_redesenho();
}

static void cmd_zerar(int, char**)
{
  s_console_soma = 0;
  printf("soma zerada\n");
  sistema_pedir_redesenho();
}

static const app_comando_t COMANDOS[] = {
  { "contar", "[n]\tsoma n (padrao 1) no cartao CONSOLE", cmd_contar, false },
  { "zerar",  "\tzera a soma (so com o app aberto)",      cmd_zerar,  true },
  { nullptr },
};

// ---------------------------------------------------------------------------
// ICONE: quadrado tracejado com um "+" - o molde de um app.
// ---------------------------------------------------------------------------
static void icone_exemplo(lgfx::LovyanGFX& d, int x, int y, int lado)
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

// 'extern' e OBRIGATORIO: em C++ um const de arquivo tem ligacao interna, e o
// apps.cpp nao enxergaria o simbolo (o erro so aparece no link).
extern const app_t APP_EXAMPLE = {
  .name  = "Example",
  .cor   = TFT_CYAN,
  // So o que os cartoes usam. O resto fica desligado enquanto o app esta na
  // tela - e o que impede o aparelho de alimentar o que ninguem usa.
  .perfil = { .imu = true, .speaker = true },
  .usa_sd = true,               // monta o cartao antes do ao_abrir (TRABALHO le a pasta)
  .orientacao = ORI_EM_PE,
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_ms = 500,            // hora, energia e IMU mudam sozinhos
  .avisos = AVISO_TRABALHO | AVISO_ESPNOW | AVISO_MESH_MSG | AVISO_MESH_NOS | AVISO_GPS,
  .ao_aviso = ao_aviso,
  .icone = icone_exemplo,
  .comandos = COMANDOS,
};
