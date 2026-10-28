// APP: Ajustes - as preferencias do sistema.
//
// NOVA FORMA, a de qualquer celular:
//
//   - no TOPO, o que se mexe sem pensar: BRILHO e VOLUME, direto na barra;
//   - uma linha, e embaixo a LISTA das paginas - icone, nome, e uma linha de
//     estado ("Conectado a ...", "Desligado") para saber sem entrar;
//   - cada pagina tem VOLTAR e as opcoes dela. A lista rola (kernel/list):
//     opcao nova e UMA linha na tabela PAGINAS, nao um remendo de posicao.
//
// A versao anterior empilhava todas as secoes numa coluna de 1800 px que o
// sistema rolava; cada secao nova empurrava as outras, e as alturas eram
// numeros somados a mao (e erraram mais de uma vez).
//
// O que mora aqui e do USUARIO, nao do aplicativo: cada app continua
// declarando o hardware que precisa no perfil dele; este app controla o que
// atravessa todos (settings.h).

#include <math.h>
#include "tab5_time.hpp"   // tab5_agora_ms
#include "app.h"
#include <cmath>
#include "settings.h"
#include "tab5_wifi.hpp"
#include "tab5_ble.hpp"
#include "tab5_signal.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_lora.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_grove.hpp"
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "paint.hpp"
#include "list.hpp"
#include "conversation.hpp"          // conversa_quebrar_linhas (texto corrido)
#include "virtual_keyboard.hpp"   // o PIN do Meshtastic
#include "work.hpp"

#include "tab5_flashdrive.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// MEDIDAS
// ---------------------------------------------------------------------------
static const int MARGEM   = 24;
static const int ROTULO_PX = 30;      // "Brilho", "Volume"
static const int CTRL_A   = 64;       // altura da barra e dos botoes - e /+
static const int PM_L     = 88;       // largura dos botoes - e +
static const int RAPIDO_Y0 = 16;
static const int RAPIDO_A  = 44 + CTRL_A + 24;   // rotulo + controle + respiro
static const int DIVISOR_Y = RAPIDO_Y0 + 2 * RAPIDO_A + 4;
static const int LISTA_Y0  = DIVISOR_Y + 12;
static const int ITEM_A    = 104;
static const int ICONE_L   = 64;

// Paginas
static const int VOLTAR_L = 200, VOLTAR_A = 64;
static const int CONTEUDO_Y = 112;     // abaixo do VOLTAR/titulo
static const int BTN_A = 72;

static const int PASSO_BRILHO = 50;   // em BRILHO_POS_MAX: 20 toques do minimo ao maximo
static const int PASSO_VOLUME = 15;
static const int PASSO_TELA_S = 5;
static const int PASSO_RELOGIO = 5;

static uint16_t cor_linha(lgfx::LovyanGFX& d) { return d.color565(48, 52, 62); }
static uint16_t cor_sub(lgfx::LovyanGFX& d)   { return d.color565(150, 156, 168); }

// ---------------------------------------------------------------------------
// AS PAGINAS - a lista de baixo sai daqui
// ---------------------------------------------------------------------------
typedef enum {
  PAG_INICIO = -1,
  PAG_WIFI = 0, PAG_BLUETOOTH, PAG_ESPNOW, PAG_MESH, PAG_LORA, PAG_TELA, PAG_APPS, PAG_TECLADO, PAG_GROVE, PAG_USB, PAG_SOBRE,
  PAG_N
} pagina_t;

typedef void (*icone_fn)(lgfx::LovyanGFX& d, int x, int y, int l);
typedef void (*estado_fn)(char* out, size_t tam);

static void ic_wifi(lgfx::LovyanGFX&, int, int, int);
static void ic_bt(lgfx::LovyanGFX&, int, int, int);
static void ic_espnow(lgfx::LovyanGFX&, int, int, int);
static void ic_mesh(lgfx::LovyanGFX&, int, int, int);
static void ic_lora(lgfx::LovyanGFX&, int, int, int);
static void ic_tela(lgfx::LovyanGFX&, int, int, int);
static void ic_apps(lgfx::LovyanGFX&, int, int, int);
static void ic_teclado(lgfx::LovyanGFX&, int, int, int);
static void ic_grove(lgfx::LovyanGFX&, int, int, int);
static void ic_usb(lgfx::LovyanGFX&, int, int, int);
static void ic_sobre(lgfx::LovyanGFX&, int, int, int);
static void est_wifi(char*, size_t);
static void est_bt(char*, size_t);
static void est_espnow(char*, size_t);
static void est_mesh(char*, size_t);
static void est_lora(char*, size_t);
static void est_tela(char*, size_t);
static void est_apps(char*, size_t);
static void est_teclado(char*, size_t);
static void est_grove(char*, size_t);
static void est_usb(char*, size_t);
static void est_sobre(char*, size_t);

typedef struct { const char* name; icone_fn icone; estado_fn state; } pagina_def_t;
static const pagina_def_t PAGINAS[PAG_N] = {
  { "Wi-Fi",        ic_wifi,    est_wifi    },
  { "Bluetooth",    ic_bt,      est_bt      },
  { "ESP-NOW",      ic_espnow,  est_espnow  },
  { "Meshtastic",   ic_mesh,    est_mesh    },
  { "LoRa",         ic_lora,    est_lora    },
  { "Tela",         ic_tela,    est_tela    },
  { "Aplicativos",  ic_apps,    est_apps    },
  { "Teclado",      ic_teclado, est_teclado },
  { "Grove 5V",     ic_grove,   est_grove   },
  { "USB (pendrive)", ic_usb,   est_usb     },
  { "Sobre",        ic_sobre,   est_sobre   },
};

// ---------------------------------------------------------------------------
// ESTADO DA TELA
// ---------------------------------------------------------------------------
static pintura_t s_p;
static int       s_pag = PAG_INICIO;
static lista_t   s_lista;
static char      s_sub[PAG_N][48];     // a linha de estado de cada item, como desenhada
static int       s_ub = -1, s_uv = -1; // brilho/volume desenhados
static char      s_assin[512];         // "assinatura" da pagina aberta: mudou, redesenha
static char      s_aviso[96];          // recado curto numa pagina ("escolhido para o Mesh")

// Aplicativos: a lista completa e os nomes escondidos da tela inicial.
static lista_t   s_l_apps;
static char      s_apps_ocultos[1024];

// Bluetooth: a lista de aparelhos vistos
static lista_t   s_l_bt;
static uint32_t  s_bt_ver = 0;
static int       s_bt_n = 0;
static tab5_ble_seen_t s_bt[TAB5_BLE_SEEN_MAX];

// Meshtastic: o editor de PIN
static bool  s_pin_editando = false;
static char  s_pin_txt[8];
// O teclado virtual do sistema - um so, para a senha do Wi-Fi e o PIN (nunca
// abertos ao mesmo tempo: trocar de pagina fecha).
static tecvirt_t s_kb;

static void abrir_pagina(int p);

// ---------------------------------------------------------------------------
// TEXTO E BOTOES
// ---------------------------------------------------------------------------
static void texto(lgfx::LovyanGFX& d, int x, int y, int px, bool negrito, uint16_t cor,
                  const char* s, uint8_t datum = top_left)
{
  d.setFont(tab5_fonte_suave(negrito, px));
  d.setTextSize(1);
  d.setTextDatum(datum);
  d.setTextColor(cor, TFT_BLACK);
  d.drawString(s, x, y);
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
}

// Paragrafo quebrado na largura - o mesmo quebrador dos baloes de conversa.
static int paragrafo(lgfx::LovyanGFX& d, int x, int y, int largura, const char* s)
{
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextSize(1);
  char linhas[6][CONVERSA_LINHA_BYTES];
  const int n = conversa_quebrar_linhas(d, s, largura, linhas, 6);
  d.setTextDatum(top_left);
  d.setTextColor(cor_sub(d), TFT_BLACK);
  for (int k = 0; k < n; ++k) { d.drawString(linhas[k], x, y + k * 32); }
  tab5_fonte_corpo(d);
  return y + n * 32;
}


// Chevron ">" desenhado (a fonte teria outro peso a cada tamanho).
static void chevron(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  for (int k = 0; k < 3; ++k) {
    d.drawLine(cx - 6 + k, cy - 12, cx + 6 + k, cy, cor);
    d.drawLine(cx + 6 + k, cy, cx - 6 + k, cy + 12, cor);
  }
}

// Cabecalho de pagina: VOLTAR e o titulo.
static void cabecalho(lgfx::LovyanGFX& d, const area_t& a, const char* titulo)
{
  tab5_botao(d, a.x + 16, a.y + 16, VOLTAR_L, VOLTAR_A, "< VOLTAR", TFT_SKYBLUE);
  tab5_titulo(d, 40);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(titulo, a.x + 16 + VOLTAR_L + 28, a.y + 16 + VOLTAR_A / 2);
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
  d.drawFastHLine(a.x, a.y + CONTEUDO_Y - 16, a.largura, cor_linha(d));
}

// Linha de informacao: rotulo pequeno em cima, valor embaixo. 76 px.
static int info(lgfx::LovyanGFX& d, const area_t& a, int y, const char* rotulo,
                const char* valor, uint16_t cor = TFT_WHITE)
{
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), rotulo);
  texto(d, a.x + MARGEM, a.y + y + 28, 32, false, cor, valor);
  return y + 84;
}

// Botao da largura da pagina.
static int botao_largo(lgfx::LovyanGFX& d, const area_t& a, int y, const char* txt, uint16_t cor)
{
  tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, txt, cor);
  return y + BTN_A + 20;
}

// Par - / valor / + (tempo de tela, relogio, canal).
static const int PM_VALOR_L = 260;
static void menos_mais(lgfx::LovyanGFX& d, const area_t& a, int y, const char* valor)
{
  const int x = a.x + MARGEM;
  tab5_botao(d, x, a.y + y, PM_L + 40, BTN_A, "-", TFT_ORANGE);
  texto(d, x + PM_L + 40 + PM_VALOR_L / 2 + 20, a.y + y + BTN_A / 2, 34, true, TFT_WHITE, valor, middle_center);
  tab5_botao(d, x + PM_L + 40 + PM_VALOR_L + 40, a.y + y, PM_L + 40, BTN_A, "+", TFT_GREENYELLOW);
}
static int menos_mais_toque(int tx, int ty, int y)
{
  if (tab5_dentro(tx, ty, MARGEM, y, PM_L + 40, BTN_A)) { return -1; }
  if (tab5_dentro(tx, ty, MARGEM + PM_L + 40 + PM_VALOR_L + 40, y, PM_L + 40, BTN_A)) { return +1; }
  return 0;
}

// ---------------------------------------------------------------------------
// ICONES DA LISTA - quadrado colorido com um desenho simples, como qualquer
// tela de ajustes de celular. Tudo em fracao do lado (l).
// ---------------------------------------------------------------------------
static void fundo_icone(lgfx::LovyanGFX& d, int x, int y, int l, uint16_t cor)
{
  d.fillRoundRect(x, y, l, l, l / 5, cor);
}

static void ic_wifi(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(30, 120, 230));
  const int cx = x + l / 2, cy = y + l * 3 / 4;
  for (int k = 0; k < 3; ++k) {
    const int r = l * (18 + 12 * k) / 100;
    d.fillArc(cx, cy, r, r - l / 14, 225, 315, TFT_WHITE);
  }
  d.fillCircle(cx, cy - 1, l / 14, TFT_WHITE);
}

static void ic_bt(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(20, 80, 200));
  const int cx = x + l / 2, t = y + l / 6, b = y + l * 5 / 6, m = y + l / 2;
  const int dx = l / 5, q = (b - t) / 4;
  for (int k = -1; k <= 1; ++k) {
    d.drawLine(cx + k, t, cx + k, b, TFT_WHITE);
    d.drawLine(cx + k, t, cx + dx + k, t + q, TFT_WHITE);
    d.drawLine(cx + dx + k, t + q, cx - dx + k, m + q, TFT_WHITE);
    d.drawLine(cx + k, b, cx + dx + k, b - q, TFT_WHITE);
    d.drawLine(cx + dx + k, b - q, cx - dx + k, m - q, TFT_WHITE);
  }
}

static void ic_espnow(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(0, 150, 110));
  const int cy = y + l / 2;
  d.fillCircle(x + l / 4, cy, l / 9, TFT_WHITE);
  d.fillCircle(x + l * 3 / 4, cy, l / 9, TFT_WHITE);
  for (int k = 0; k < 3; ++k) {
    d.fillCircle(x + l * (38 + 12 * k) / 100, cy, l / 28, TFT_WHITE);
  }
}

static void ic_mesh(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(40, 170, 60));
  const int ax = x + l / 4, ay = y + l * 3 / 4, bx = x + l / 2, by = y + l / 4;
  const int cx = x + l * 3 / 4, cy = y + l * 3 / 4;
  for (int k = -1; k <= 1; ++k) {
    d.drawLine(ax, ay + k, bx, by + k, TFT_WHITE);
    d.drawLine(bx, by + k, cx, cy + k, TFT_WHITE);
    d.drawLine(ax, ay + k, cx, cy + k, TFT_WHITE);
  }
  d.fillCircle(ax, ay, l / 10, TFT_WHITE);
  d.fillCircle(bx, by, l / 10, TFT_WHITE);
  d.fillCircle(cx, cy, l / 10, TFT_WHITE);
}

static void ic_lora(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(190, 80, 35));
  const int cx = x + l / 2, cy = y + l * 3 / 4;
  d.fillRect(cx - 2, y + l / 4, 5, l / 2, TFT_WHITE);
  d.fillCircle(cx, cy, l / 14, TFT_WHITE);
  for (int k = 0; k < 2; ++k) {
    const int r = l * (22 + 14 * k) / 100;
    d.drawArc(cx, cy, r, r - 3, 205, 335, TFT_WHITE);
  }
}

static void ic_tela(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(240, 150, 20));
  const int cx = x + l / 2, cy = y + l / 2;
  d.fillCircle(cx, cy, l / 6, TFT_WHITE);
  for (int k = 0; k < 8; ++k) {
    const float ang = k * (float)M_PI / 4.0f;
    const int r0 = l * 26 / 100, r1 = l * 36 / 100;
    for (int e = -1; e <= 1; ++e) {
      d.drawLine(cx + (int)(r0 * cosf(ang)) + e, cy + (int)(r0 * sinf(ang)),
                 cx + (int)(r1 * cosf(ang)) + e, cy + (int)(r1 * sinf(ang)), TFT_WHITE);
    }
  }
}

static void ic_apps(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(70, 130, 210));
  const int q = l / 4, vao = l / 10;
  const int x0 = x + (l - 2 * q - vao) / 2, y0 = y + (l - 2 * q - vao) / 2;
  for (int f = 0; f < 2; ++f) {
    for (int c = 0; c < 2; ++c) {
      d.fillRoundRect(x0 + c * (q + vao), y0 + f * (q + vao), q, q, q / 5, TFT_WHITE);
    }
  }
}

static void ic_teclado(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(110, 116, 128));
  const int kx = x + l / 8, ky = y + l * 3 / 10, kl = l * 3 / 4, ka = l * 2 / 5;
  d.fillRoundRect(kx, ky, kl, ka, 4, TFT_WHITE);
  const uint16_t tec = d.color565(110, 116, 128);
  const int t = kl / 7;
  for (int f = 0; f < 2; ++f) {
    for (int c = 0; c < 5; ++c) { d.fillRect(kx + t / 2 + c * (t + t / 3), ky + 5 + f * (t + 2), t, t - 2, tec); }
  }
  d.fillRect(kx + kl / 4, ky + ka - t / 2 - 5, kl / 2, t / 2, tec);
}

static void ic_grove(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(150, 70, 200));
  const int cx = x + l / 2;
  d.fillRoundRect(cx - l / 5, y + l * 35 / 100, l * 2 / 5, l * 3 / 10, 5, TFT_WHITE);
  d.fillRect(cx - l / 8, y + l / 6, l / 14, l / 5, TFT_WHITE);
  d.fillRect(cx + l / 8 - l / 14, y + l / 6, l / 14, l / 5, TFT_WHITE);
  d.fillRect(cx - l / 20, y + l * 65 / 100, l / 10, l / 6, TFT_WHITE);
}

// O simbolo do USB: haste, triangulo na ponta, e os dois ramos (bola e
// quadrado) - o mesmo desenho gravado no plugue.
static void ic_usb(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(40, 120, 170));
  const int cx = x + l / 2, topo = y + l / 6, base = y + l * 80 / 100, e = l / 24 + 1;
  d.fillRect(cx - e / 2, topo + l / 10, e, base - topo - l / 10, TFT_WHITE);
  d.fillTriangle(cx, topo, cx - l / 10, topo + l / 8, cx + l / 10, topo + l / 8, TFT_WHITE);
  d.fillCircle(cx, base, l / 14, TFT_WHITE);
  const int ry = y + l * 45 / 100;
  d.drawLine(cx, ry + l / 8, cx - l / 5, ry, TFT_WHITE);
  d.drawLine(cx + 1, ry + l / 8, cx - l / 5 + 1, ry, TFT_WHITE);
  d.fillCircle(cx - l / 5, ry - l / 22, l / 18, TFT_WHITE);
  const int ry2 = y + l * 55 / 100;
  d.drawLine(cx, ry2 + l / 8, cx + l / 5, ry2, TFT_WHITE);
  d.drawLine(cx - 1, ry2 + l / 8, cx + l / 5 - 1, ry2, TFT_WHITE);
  d.fillRect(cx + l / 5 - l / 20, ry2 - l / 10, l / 10, l / 10, TFT_WHITE);
}

static void ic_sobre(lgfx::LovyanGFX& d, int x, int y, int l)
{
  fundo_icone(d, x, y, l, d.color565(90, 96, 110));
  const int cx = x + l / 2;
  d.fillCircle(cx, y + l * 28 / 100, l / 12, TFT_WHITE);
  d.fillRoundRect(cx - l / 14, y + l * 42 / 100, l / 7, l * 38 / 100, 3, TFT_WHITE);
}

// ---------------------------------------------------------------------------
// LINHA DE ESTADO DE CADA PAGINA (o subtitulo na lista)
// ---------------------------------------------------------------------------
static void est_wifi(char* o, size_t t)
{
  const tab5_wifi_state_t e = tab5_wifi_state();
  if (e == TAB5_WIFI_CONNECTED)           { snprintf(o, t, "Conectado a %s", tab5_wifi_ssid()); }
  else if (e == TAB5_WIFI_NO_CREDENTIALS) { snprintf(o, t, "Nenhuma rede guardada"); }
  else if (e == TAB5_WIFI_C6_UNRESPONSIVE)        { snprintf(o, t, "O C6 não responde"); }
  else if (e == TAB5_WIFI_FAILED)         { snprintf(o, t, "Não conectou"); }
  else if (e == TAB5_WIFI_OFF)      { snprintf(o, t, ajustes_wifi() ? "Solto do roteador" : "Desligado"); }
  else                                    { snprintf(o, t, "Conectando..."); }
}

static void est_bt(char* o, size_t t)
{
  const tab5_ble_state_t e = tab5_ble_state();
  if (e == TAB5_BLE_READY)       { snprintf(o, t, "Ligado"); }
  else if (e == TAB5_BLE_STARTING) { snprintf(o, t, "Ligando..."); }
  // C6 com o firmware de fabrica (Wi-Fi funciona, Bluetooth nao): nao e
  // defeito, e uma atualizacao opcional - o texto tem que dizer isso.
  else if (e == TAB5_BLE_FAILED && tab5_wifi_c6_link() && !tab5_wifi_c6_is_ours())
                                  { snprintf(o, t, "Rádio C6 de fábrica: atualização opcional"); }
  else if (e == TAB5_BLE_FAILED)  { snprintf(o, t, "Falhou - o C6 não deu o Bluetooth"); }
  else if (e == TAB5_BLE_STOPPING) { snprintf(o, t, "Desligando..."); }
  else                            { snprintf(o, t, "Desligado"); }
}

static void est_espnow(char* o, size_t t)
{
  if (!tab5_espnow_c6_compativel()) {
    // Sem enlace ainda nao da para saber qual firmware o C6 tem.
    snprintf(o, t, tab5_wifi_c6_link() ? "Rádio C6 de fábrica: atualização opcional"
                                         : "Ligue o Wi-Fi");
    return;
  }
  const uint8_t f = tab5_espnow_canal_fixo();
  if (f) { snprintf(o, t, "Canal fixo %u (sem roteador)", (unsigned)f); }
  else   { snprintf(o, t, "Canal do roteador (%u)", (unsigned)tab5_espnow_canal()); }
}

static const char* nome_via_mesh(uint8_t via)
{
  return via == AJUSTES_MESH_BLUETOOTH ? "Bluetooth" : via == AJUSTES_MESH_INTERNET ? "Internet" : "Cabo Grove";
}

static void est_mesh(char* o, size_t t)
{
  snprintf(o, t, "%s - %s", nome_via_mesh(ajustes_mesh_via()), tab5_mesh_enlace_descricao());
}

static void est_lora(char* o, size_t t)
{
  const tab5_lora_status_t s = tab5_lora_status();
  if (!s.probed)             { snprintf(o, t, "Ainda não sondado"); }
  else if (!s.ioe_address)   { snprintf(o, t, "M5IOE1 não encontrado"); }
  else if (s.radio_present)  { snprintf(o, t, "SX1262 responde · M5IOE1 0x%02X", s.ioe_address); }
  else                        { snprintf(o, t, "M5IOE1 0x%02X · SX1262 não respondeu", s.ioe_address); }
}

static void est_tela(char* o, size_t t)
{
  snprintf(o, t, "Apaga em %u s  ·  painel a %d MHz", (unsigned)ajustes_tela_s(),
           tab5_power_dpi_clock());
}

static bool nome_em_lista(const char* lista, const char* nome)
{
  const size_t nome_n = strlen(nome);
  const char* p = lista ? lista : "";
  while (*p) {
    const char* fim = strchr(p, ',');
    const size_t n = fim ? (size_t)(fim - p) : strlen(p);
    if (n == nome_n && strncasecmp(p, nome, n) == 0) { return true; }
    if (!fim) { break; }
    p = fim + 1;
  }
  return false;
}

static void est_apps(char* o, size_t t)
{
  int visiveis = 0;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->name, "Ajustes") == 0
        || !nome_em_lista(s_apps_ocultos, SISTEMA_APPS[i]->name)) { ++visiveis; }
  }
  snprintf(o, t, "%d de %d visíveis", visiveis, SISTEMA_NUM_APPS);
}

static void est_teclado(char* o, size_t t)
{
  if (!ajustes_teclado())            { snprintf(o, t, "Desativado"); }
  else if (tab5_keyboard_present())  { snprintf(o, t, "Encaixado"); }
  else                               { snprintf(o, t, "Procurando no ExtPort1..."); }
}

static void est_usb(char* o, size_t t)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  if (e.fase == PENDRIVE_ATIVO)           { snprintf(o, t, "Cartão no computador"); }
  else if (e.fase != PENDRIVE_PARADO)     { snprintf(o, t, "Pendrive: aguarde..."); }
  else if (e.oferta)                      { snprintf(o, t, "Computador conectado"); }
  else if (ajustes_pendrive())            { snprintf(o, t, "Liga sozinho no computador"); }
  else                                    { snprintf(o, t, "Pendrive desligado"); }
}

static void est_grove(char* o, size_t t)
{
  const int dono = (int)tab5_grove_owner();
  if (sistema_grove_sondando())                 { snprintf(o, t, "Sondando o cabo..."); }
  else if (dono != (int)GROVE_OWNER_NONE)     { snprintf(o, t, "Em uso: %s", grove_owner_name((grove_owner_t)dono)); }
  else if (ajustes_grove())                     { snprintf(o, t, "Ligado - %s", grove_name(tab5_grove_type())); }
  else                                          { snprintf(o, t, "Desligado"); }
}

// O carimbo do build inteiro (main/carimbo.cmake), e nao o __DATE__ deste
// arquivo - que so mudava quando ESTE arquivo recompilava.
extern "C" const char* const m5tabos_compilado;

static void est_sobre(char* o, size_t t)
{
  snprintf(o, t, "M5tabOS %s", esp_app_get_description()->version);
}

// ---------------------------------------------------------------------------
// INICIO: BRILHO, VOLUME e a lista
// ---------------------------------------------------------------------------
static int barra_x(const area_t& a) { return a.x + MARGEM + PM_L + 16; }
static int barra_l(const area_t& a) { return a.largura - 2 * MARGEM - 2 * (PM_L + 16); }

// BRILHO NA ESCALA DO OLHO. O valor do PWM e linear na luz, mas o olho nao:
// 4% de duty ja parece "um quarto do brilho". Com a barra linear quase todo o
// curso ficava no claro e o primeiro degrau (10 -> 25) pulava de 4% para 10%.
// A barra anda numa posicao 0..1000 e o PWM sai dela pela curva gama 2,2 - a
// metade de baixo da barra cobre de 0,9% a ~22%, em degraus finos.
#define BRILHO_POS_MAX 1000
static int brilho_para_pos(int b)
{
  const float f = (float)(b - AJUSTES_BRILHO_MIN) / (float)(AJUSTES_BRILHO_MAX - AJUSTES_BRILHO_MIN);
  return (int)(powf(f < 0 ? 0 : f, 1.0f / 2.2f) * BRILHO_POS_MAX + 0.5f);
}
static int pos_para_brilho(int pos)
{
  const float f = (float)pos / (float)BRILHO_POS_MAX;
  return AJUSTES_BRILHO_MIN + (int)(powf(f, 2.2f) * (AJUSTES_BRILHO_MAX - AJUSTES_BRILHO_MIN) + 0.5f);
}

static void desenhar_rapido(lgfx::LovyanGFX& d, const area_t& a, int y0, const char* nome,
                            int valor, int minimo, int maximo, bool tudo, int anterior)
{
  const int x = a.x + MARGEM;
  const int pct = ((valor - minimo) * 100) / (maximo - minimo);
  char buf[16];
  if (tudo) {
    texto(d, x, a.y + y0, ROTULO_PX, true, TFT_WHITE, nome);
    tab5_botao(d, x, a.y + y0 + 44, PM_L, CTRL_A, "-", TFT_ORANGE);
    tab5_botao(d, a.x + a.largura - MARGEM - PM_L, a.y + y0 + 44, PM_L, CTRL_A, "+", TFT_GREENYELLOW);
  }
  // Valor a direita do rotulo (apaga o anterior: largura variavel)
  d.fillRect(a.x + a.largura - MARGEM - 160, a.y + y0, 160, 40, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%d%%", pct);
  texto(d, a.x + a.largura - MARGEM, a.y + y0, ROTULO_PX, false, TFT_LIGHTGREY, buf, top_right);

  // Barra: moldura so na passada completa; o preenchimento so no que mudou.
  const int bx = barra_x(a), bl = barra_l(a), by = a.y + y0 + 44;
  const int dentro_l = bl - 8;
  const int cheio = ((valor - minimo) * dentro_l) / (maximo - minimo);
  const int antes = (tudo || anterior < 0) ? -1 : ((anterior - minimo) * dentro_l) / (maximo - minimo);
  if (tudo) {
    d.fillRoundRect(bx, by, bl, CTRL_A, CTRL_A / 2, d.color565(34, 38, 46));
  }
  if (antes < 0) {
    d.fillRoundRect(bx + 4, by + 4, dentro_l, CTRL_A - 8, (CTRL_A - 8) / 2, d.color565(34, 38, 46));
    if (cheio > 0) { d.fillRoundRect(bx + 4, by + 4, cheio < CTRL_A - 8 ? CTRL_A - 8 : cheio, CTRL_A - 8, (CTRL_A - 8) / 2, TFT_SKYBLUE); }
  } else if (cheio != antes) {
    // Redesenha a barra inteira por dentro - os cantos redondos nao deixam
    // pintar so a diferenca sem sobrar serrilhado.
    d.fillRoundRect(bx + 4, by + 4, dentro_l, CTRL_A - 8, (CTRL_A - 8) / 2, d.color565(34, 38, 46));
    if (cheio > 0) { d.fillRoundRect(bx + 4, by + 4, cheio < CTRL_A - 8 ? CTRL_A - 8 : cheio, CTRL_A - 8, (CTRL_A - 8) / 2, TFT_SKYBLUE); }
  }
}

static int item_altura(int, void*) { return ITEM_A; }

static void item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= PAG_N) { return; }
  d.fillRect(x0, y, largura, ITEM_A, TFT_BLACK);
  const int ix = x0 + MARGEM, iy = y + (ITEM_A - ICONE_L) / 2;
  PAGINAS[i].icone(d, ix, iy, ICONE_L);
  const int tx = ix + ICONE_L + 24;
  texto(d, tx, y + 18, 32, true, TFT_WHITE, PAGINAS[i].name);
  // Estado cortado MEDINDO, para nao passar por baixo do chevron.
  char sub[48];
  snprintf(sub, sizeof(sub), "%s", s_sub[i]);
  d.setFont(tab5_fonte_suave(false, 24));
  const int max_l = x0 + largura - MARGEM - 40 - tx;
  tab5_cortar_ao_caber(d, sub, max_l);
  tab5_fonte_corpo(d);
  texto(d, tx, y + 60, 24, false, cor_sub(d), sub);
  chevron(d, x0 + largura - MARGEM - 12, y + ITEM_A / 2, cor_sub(d));
  if (i + 1 < PAG_N) { d.drawFastHLine(tx, y + ITEM_A - 1, x0 + largura - tx, cor_linha(d)); }
}

static void desenhar_inicio(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  const int b = ajustes_brilho(), v = ajustes_volume();
  const int b_ant = s_ub, v_ant = s_uv;
  if (pintura_mudou_i(&s_p, &s_ub, b) || tudo) {
    desenhar_rapido(d, a, RAPIDO_Y0, "Brilho", brilho_para_pos(b), 0, BRILHO_POS_MAX, tudo,
                    b_ant < 0 ? -1 : brilho_para_pos(b_ant));
  }
  if (pintura_mudou_i(&s_p, &s_uv, v) || tudo) {
    desenhar_rapido(d, a, RAPIDO_Y0 + RAPIDO_A, "Volume", v, AJUSTES_VOLUME_MIN, AJUSTES_VOLUME_MAX, tudo, v_ant);
  }
  if (tudo) { d.drawFastHLine(a.x, a.y + DIVISOR_Y, a.largura, cor_linha(d)); }

  // Estado de cada pagina: so o item cujo texto mudou e redesenhado.
  for (int i = 0; i < PAG_N; ++i) {
    char agora[48];
    PAGINAS[i].state(agora, sizeof(agora));
    if (strcmp(agora, s_sub[i]) != 0) {
      snprintf(s_sub[i], sizeof(s_sub[i]), "%s", agora);
      lista_item_mudou(&s_lista, i);
    }
  }
  const area_t janela = { a.x, a.y + LISTA_Y0, a.largura, a.altura - LISTA_Y0 };
  lista_desenhar(&s_lista, d, janela, TFT_BLACK, tudo);
}

static void mexer_rapido(int y0, int tx, int ty, bool so_barra)
{
  const area_t a = sistema_area();
  const bool brilho = (y0 == RAPIDO_Y0);
  // O brilho anda na posicao perceptual (ver brilho_para_pos).
  const int minimo = brilho ? 0 : AJUSTES_VOLUME_MIN;
  const int maximo = brilho ? BRILHO_POS_MAX : AJUSTES_VOLUME_MAX;
  const int atual  = brilho ? brilho_para_pos(ajustes_brilho()) : ajustes_volume();
  const int passo  = brilho ? PASSO_BRILHO : PASSO_VOLUME;
  const int cy = y0 + 44;
  int novo = atual;
  const int bx = barra_x(a) - a.x, bl = barra_l(a);
  if (tab5_dentro(tx, ty, bx - 8, cy, bl + 16, CTRL_A)) {
    int rel = tx - bx; if (rel < 0) { rel = 0; } if (rel > bl) { rel = bl; }
    novo = minimo + (rel * (maximo - minimo)) / bl;
  } else if (so_barra) {
    return;
  } else if (tab5_dentro(tx, ty, MARGEM, cy, PM_L, CTRL_A)) {
    novo = atual - passo;
  } else if (tab5_dentro(tx, ty, a.largura - MARGEM - PM_L, cy, PM_L, CTRL_A)) {
    novo = atual + passo;
  } else {
    return;
  }
  if (novo < minimo) { novo = minimo; }
  if (novo > maximo) { novo = maximo; }
  if (novo == atual) { return; }
  if (brilho) {
    int b = pos_para_brilho(novo);
    // No pe da curva dois toques de "-"/"+" podem cair no mesmo valor de PWM:
    // o botao sempre anda pelo menos um.
    const int b_atual = ajustes_brilho();
    if (b == b_atual) { b += (novo > atual) ? 1 : -1; }
    if (b < AJUSTES_BRILHO_MIN) { b = AJUSTES_BRILHO_MIN; }
    if (b > AJUSTES_BRILHO_MAX) { b = AJUSTES_BRILHO_MAX; }
    if (b == b_atual) { return; }
    ajustes_definir_brilho((uint8_t)b);
  }
  else {
    ajustes_definir_volume((uint8_t)novo);
    if (!so_barra) { sistema_beep(880.0f, 80); }   // ouvir o volume novo
  }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// PAGINAS
// ---------------------------------------------------------------------------
// Cada pagina: desenhar(tudo) e tocar(x,y). A pagina redesenha o conteudo
// inteiro quando a ASSINATURA (os valores que ela mostra) muda - e so entao.
static void conteudo_limpo(lgfx::LovyanGFX& d, const area_t& a)
{
  tab5_screen_clear(a.x, a.y + CONTEUDO_Y, a.largura, a.altura - CONTEUDO_Y, TFT_BLACK);
}

// ---- Wi-Fi -----------------------------------------------------------------
// Estado, ligar/desligar, procurar, e a LISTA de redes em volta. Tocar numa
// rede conhecida conecta; numa nova, pede a senha (teclado da tela ou do
// ExtPort1), guarda cifrada no cartao e conecta. Tocar duas vezes na rede em
// uso a esquece.
static const int WF_ITEM_A = 92;
static lista_t   s_l_wifi;
static uint32_t  s_wf_ver = 0;
static int       s_wf_n = 0;
static tab5_wifi_seen_t s_wf[TAB5_WIFI_MAX_SEEN];
static bool      s_wf_senha = false;          // editando a senha de s_wf_alvo
static char      s_wf_alvo[33];
static char      s_wf_txt[65];
static char      s_wf_campo[70];          // o que esta pintado no campo da senha
static char      s_wf_esquecer[33];           // 1o toque na rede em uso
static uint32_t  s_wf_esquecer_ms = 0;

static int wf_y_lista(void) { return CONTEUDO_Y + 84 + (BTN_A + 20) * 2 + 44; }

static bool wf_em_uso(const char* ssid)
{
  return tab5_wifi_state() == TAB5_WIFI_CONNECTED && strcmp(tab5_wifi_ssid(), ssid) == 0;
}

static int wf_item_altura(int, void*) { return WF_ITEM_A; }
static void wf_item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= s_wf_n) { return; }
  const tab5_wifi_seen_t& v = s_wf[i];
  d.fillRect(x0, y, largura, WF_ITEM_A, TFT_BLACK);
  const bool uso = wf_em_uso(v.ssid);
  // Sinal em 4 barras, a esquerda.
  const int nb = v.rssi > -55 ? 4 : v.rssi > -65 ? 3 : v.rssi > -75 ? 2 : 1;
  for (int b = 0; b < 4; ++b) {
    const int h = 8 + b * 8;
    d.fillRect(x0 + MARGEM + b * 10, y + 56 - h, 7, h, b < nb ? (uso ? TFT_GREENYELLOW : TFT_WHITE) : cor_linha(d));
  }
  const int tx = x0 + MARGEM + 56;
  texto(d, tx, y + 12, 30, true, uso ? TFT_GREENYELLOW : TFT_WHITE, v.ssid);
  char linha[80], dist[12];
  tab5_signal_distance_text(v.rssi, TAB5_SIGNAL_P1M_WIFI, dist, sizeof(dist));
  snprintf(linha, sizeof(linha), "%s%s%d dBm %s  ·  canal %u  ·  %s", uso ? "conectada  ·  " : "",
           (!uso && v.known) ? "conhecida  ·  " : "", (int)v.rssi, dist, (unsigned)v.channel,
           v.open ? "aberta" : "com senha");
  texto(d, tx, y + 52, 22, false, cor_sub(d), linha);
  d.drawFastHLine(tx, y + WF_ITEM_A - 1, x0 + largura - tx, cor_linha(d));
}

static void wf_recarregar(void)
{
  const int n = tab5_wifi_seen_count();
  int k = 0;
  for (int i = 0; i < n && k < TAB5_WIFI_MAX_SEEN; ++i) { if (tab5_wifi_get_seen(i, &s_wf[k])) { ++k; } }
  s_wf_n = k;
  lista_itens_mudaram(&s_l_wifi, s_wf_n);
}

static void pag_wifi(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[160];
  est_wifi(sub, sizeof(sub));
  const bool con = tab5_wifi_state() == TAB5_WIFI_CONNECTED;
  const uint32_t ver = tab5_wifi_networks_version();
  if (ver != s_wf_ver) { s_wf_ver = ver; wf_recarregar(); }
  char det[48];
  const int r = tab5_wifi_rssi();
  if (con && r != TAB5_WIFI_RSSI_UNKNOWN) { snprintf(det, sizeof(det), "%s  ·  %d dBm", tab5_wifi_ip(), r); }
  else if (con)                                { snprintf(det, sizeof(det), "%s", tab5_wifi_ip()); }
  else                                         { snprintf(det, sizeof(det), "%d rede(s) conhecida(s)", tab5_wifi_known_count()); }
  // O texto digitado NAO entra na assinatura: cada letra repintava a pagina
  // inteira e o teclado junto. Agora a letra repinta so o campo.
  snprintf(assin, sizeof(assin), "%s|%s|%d|%d|%d|%s|%d", sub, det, (int)ajustes_wifi(), (int)tab5_wifi_searching(),
           s_wf_n, s_aviso, (int)s_wf_senha);
  const bool mudou = pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) || tudo;
  if (s_wf_senha) {
    const int y_campo = CONTEUDO_Y + 36;
    tecvirt_posicionar(&s_kb, a);
    if (mudou) {
      lista_esconder(&s_l_wifi);
      conteudo_limpo(d, a);
      char rot[64];
      snprintf(rot, sizeof(rot), "Senha de \"%s\"", s_wf_alvo);
      texto(d, a.x + MARGEM, a.y + CONTEUDO_Y, 24, false, cor_sub(d), rot);
      paragrafo(d, a.x + MARGEM, a.y + y_campo + 92, a.largura - 2 * MARGEM,
                "Toque ENVIAR para guardar e conectar. A senha fica cifrada no cartão. X cancela.");
    }
    char campo[70];
    snprintf(campo, sizeof(campo), "%s_", s_wf_txt);
    if (pintura_mudou_s(&s_p, s_wf_campo, sizeof(s_wf_campo), campo) || mudou) {
      d.fillRoundRect(a.x + MARGEM, a.y + y_campo, a.largura - 2 * MARGEM, 72, 12, d.color565(40, 44, 54));
      d.setFont(tab5_fonte_suave(false, 34));
      d.setTextSize(1);
      d.setTextDatum(middle_left);
      d.setTextColor(TFT_WHITE, d.color565(40, 44, 54));
      d.drawString(campo, a.x + MARGEM + 16, a.y + y_campo + 36);
      d.setTextDatum(top_left);
      tab5_fonte_corpo(d);
    }
    tecvirt_pintar(&s_kb, d, mudou);
    return;
  }
  if (mudou) {
    conteudo_limpo(d, a);
    int y = CONTEUDO_Y;
    texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Estado");
    texto(d, a.x + MARGEM, a.y + y + 28, 32, false, con ? TFT_GREENYELLOW : TFT_WHITE, sub);
    texto(d, a.x + a.largura - MARGEM, a.y + y + 34, 22, false, cor_sub(d), det, top_right);
    y += 84;
    y = botao_largo(d, a, y, ajustes_wifi() ? "DESLIGAR O WI-FI" : "LIGAR O WI-FI",
                    ajustes_wifi() ? TFT_ORANGE : TFT_GREENYELLOW);
    y = botao_largo(d, a, y, tab5_wifi_searching() ? "PROCURANDO..." : "PROCURAR REDES",
                    tab5_wifi_searching() ? TFT_DARKGREY : TFT_SKYBLUE);
    texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d),
          s_aviso[0] ? s_aviso : (s_wf_n ? "Toque numa rede para conectar" : "Toque PROCURAR REDES"));
  }
  const area_t janela = { a.x, a.y + wf_y_lista(), a.largura, a.altura - wf_y_lista() };
  lista_desenhar(&s_l_wifi, d, janela, TFT_BLACK, mudou);
}

// Gravar no cartao e conectar: no worker (o cartao demora), com a senha
// numa copia propria que o trabalho apaga ao terminar.
typedef struct { char ssid[33]; char senha[65]; bool esquecer; } wf_pedido_t;

static void wf_trabalho(void* arg)
{
  wf_pedido_t* p = (wf_pedido_t*)arg;
  // A gravacao monta o cartao sozinha (e, com ele emprestado ao computador,
  // vai para o diario - a rede nova nao se perde).
  if (p->esquecer) { tab5_wifi_forget(p->ssid); }
  else             { tab5_wifi_save_credentials(p->ssid, p->senha[0] ? p->senha : "-"); }
  if (!p->esquecer) { tab5_wifi_connect(p->ssid); }
  memset(p, 0, sizeof(*p));
  free(p);
}

static void wf_pedir(const char* ssid, const char* senha, bool esquecer)
{
  wf_pedido_t* p = (wf_pedido_t*)calloc(1, sizeof(wf_pedido_t));
  if (!p) { return; }
  snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
  snprintf(p->senha, sizeof(p->senha), "%s", senha ? senha : "");
  p->esquecer = esquecer;
  if (!sistema_trabalho_solto(wf_trabalho, p)) { memset(p, 0, sizeof(*p)); free(p); }
}

// Conectar exige o Wi-Fi ligado: liga (e grava a vontade) se estiver desligado.
static void wf_ligar_se_preciso(void)
{
  if (!ajustes_wifi()) { ajustes_definir_wifi(true); tab5_wifi_turn_on(); }
}

static void wf_fechar_senha(void)
{
  s_wf_senha = false;
  memset(s_wf_txt, 0, sizeof(s_wf_txt));
  tecvirt_fechar(&s_kb);
  sistema_pedir_redesenho_completo();
}

static void wf_enviar_senha(void)
{
  if (!s_wf_txt[0]) { return; }
  wf_ligar_se_preciso();
  wf_pedir(s_wf_alvo, s_wf_txt, false);
  snprintf(s_aviso, sizeof(s_aviso), "conectando a %s...", s_wf_alvo);
  wf_fechar_senha();
}

static void wf_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR) { wf_enviar_senha(); return; }
  if (ac == TECVIRT_FECHAR) { wf_fechar_senha(); return; }
  // Senha e ASCII (como no teclado fisico): o acento da pagina de simbolos nao entra.
  tecvirt_editar(s_wf_txt, sizeof(s_wf_txt), ac, true);
  sistema_pedir_redesenho();
}

static void toque_wifi(int tx, int ty)
{
  const area_t a = sistema_area();
  if (s_wf_senha) {
    if (tecvirt_contem(&s_kb, a.x + tx, a.y + ty)) {
      const int ac = tecvirt_clique(&s_kb, a.x + tx, a.y + ty);
      if (ac) { wf_tecla(ac); }
    }
    return;
  }
  const int l = a.largura - 2 * MARGEM;
  int y = CONTEUDO_Y + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    const bool ligar = !ajustes_wifi();
    ajustes_definir_wifi(ligar);
    if (ligar) { tab5_wifi_turn_on(); } else { tab5_wifi_turn_off(); }
    s_aviso[0] = 0;
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    if (!tab5_wifi_searching()) { tab5_wifi_search(); }
    s_aviso[0] = 0;
    sistema_pedir_redesenho();
    return;
  }
  int dy = 0;
  const int i = lista_item_tocado(&s_l_wifi, tx, ty, &dy);
  if (i < 0 || i >= s_wf_n) { return; }
  const tab5_wifi_seen_t& v = s_wf[i];
  const uint32_t agora = tab5_agora_ms();
  if (wf_em_uso(v.ssid)) {
    // Em uso: o primeiro toque avisa, o segundo (em 4 s) esquece a rede.
    if (strcmp(s_wf_esquecer, v.ssid) == 0 && agora - s_wf_esquecer_ms < 4000) {
      wf_pedir(v.ssid, nullptr, true);
      snprintf(s_aviso, sizeof(s_aviso), "%s esquecida", v.ssid);
      s_wf_esquecer[0] = 0;
    } else {
      snprintf(s_wf_esquecer, sizeof(s_wf_esquecer), "%s", v.ssid);
      s_wf_esquecer_ms = agora;
      snprintf(s_aviso, sizeof(s_aviso), "Conectada. Toque de novo para esquecer");
    }
  } else if (v.known || v.open) {
    wf_ligar_se_preciso();
    if (v.known) { tab5_wifi_connect(v.ssid); }
    else             { wf_pedir(v.ssid, nullptr, false); }   // aberta: guarda sem senha e conecta
    snprintf(s_aviso, sizeof(s_aviso), "conectando a %s...", v.ssid);
  } else {
    snprintf(s_wf_alvo, sizeof(s_wf_alvo), "%s", v.ssid);
    memset(s_wf_txt, 0, sizeof(s_wf_txt));
    s_wf_senha = true;
    tecvirt_abrir(&s_kb);
    sistema_pedir_redesenho_completo();
    return;
  }
  sistema_pedir_redesenho();
}

static void teclar_wifi(const char* txt, uint8_t codigo)
{
  if (!s_wf_senha) { return; }
  if (codigo == TAB5_KEY_ENTER)  { wf_enviar_senha(); return; }
  if (codigo == TAB5_KEY_BACKSPACE) { wf_tecla(TECVIRT_APAGAR); return; }
  if (codigo == TAB5_KEY_ESC)    { wf_fechar_senha(); return; }
  // Senha e ASCII: a letra que a tecla morta compos volta a ser as duas teclas.
  char cru[8];
  tab5_keyboard_ascii(txt, cru, sizeof(cru));
  for (const char* c = cru; *c; ++c) { if ((unsigned char)*c >= 0x20) { wf_tecla((unsigned char)*c); } }
}

// ---- Bluetooth -------------------------------------------------------------
static const int BT_ITEM_A = 92;
static int bt_lista_y0(void) { return CONTEUDO_Y + 84 + 84 + BTN_A + 20 + BTN_A + 20 + 44; }

static int bt_item_altura(int, void*) { return BT_ITEM_A; }
static void bt_item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= s_bt_n) { return; }
  const tab5_ble_seen_t& v = s_bt[i];
  d.fillRect(x0, y, largura, BT_ITEM_A, TFT_BLACK);
  char end[18], linha[80], dist[12];
  tab5_ble_address_text(v.addr, end);
  // Rastreador em laranja: e o que se procura quando se quer saber "tem uma
  // etiqueta perto de mim?".
  const uint16_t cor = v.meshtastic ? TFT_GREENYELLOW
                     : v.class_id == TAB5_BLE_TRACKER ? TFT_ORANGE : TFT_WHITE;
  char titulo[48];
  if (v.name[0])       { snprintf(titulo, sizeof(titulo), "%s", v.name); }
  else if (v.brand[0]) { snprintf(titulo, sizeof(titulo), "%s %s", v.class_id == TAB5_BLE_TRACKER ? "Rastreador" : "Beacon", v.brand); }
  else                 { snprintf(titulo, sizeof(titulo), "(sem nome)"); }
  texto(d, x0 + MARGEM, y + 12, 30, true, cor, titulo);
  tab5_signal_distance_text(v.rssi, v.p1m ? v.p1m : TAB5_SIGNAL_P1M_BLE, dist, sizeof(dist));
  const char* tipo = v.meshtastic ? "  ·  Meshtastic" : "";
  char marca[24] = "";
  if (!v.meshtastic && v.brand[0] && v.name[0]) { snprintf(marca, sizeof(marca), "  ·  %s", v.brand); }
  snprintf(linha, sizeof(linha), "%s  ·  %d dBm %s%s%s", end, (int)v.rssi, dist, tipo, marca);
  texto(d, x0 + MARGEM, y + 52, 22, false, cor_sub(d), linha);
  if (v.meshtastic) { chevron(d, x0 + largura - MARGEM - 12, y + BT_ITEM_A / 2, cor_sub(d)); }
  d.drawFastHLine(x0 + MARGEM, y + BT_ITEM_A - 1, largura - MARGEM, cor_linha(d));
}

static void bt_recarregar(void)
{
  // Meshtastic primeiro, depois por sinal: o que interessa fica no alto.
  const int n = tab5_ble_seen_count();
  int k = 0;
  for (int i = 0; i < n && k < TAB5_BLE_SEEN_MAX; ++i) { if (tab5_ble_get_seen(i, &s_bt[k])) { ++k; } }
  for (int i = 1; i < k; ++i) {
    tab5_ble_seen_t t = s_bt[i];
    int j = i - 1;
    // Meshtastic, depois rastreadores, depois o resto - cada grupo por sinal.
    auto grupo = [](const tab5_ble_seen_t& x) {
      return x.meshtastic ? 0 : x.class_id == TAB5_BLE_TRACKER ? 1 : 2;
    };
    auto antes = [grupo](const tab5_ble_seen_t& x, const tab5_ble_seen_t& y) {
      if (grupo(x) != grupo(y)) { return grupo(x) < grupo(y); }
      return x.rssi > y.rssi;
    };
    while (j >= 0 && antes(t, s_bt[j])) { s_bt[j + 1] = s_bt[j]; --j; }
    s_bt[j + 1] = t;
  }
  s_bt_n = k;
  lista_itens_mudaram(&s_l_bt, s_bt_n);
}

static void pag_bt(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[160];
  est_bt(sub, sizeof(sub));
  const bool pronto = tab5_ble_ready();
  const bool varrendo = tab5_ble_scanning();
  const uint32_t ver = tab5_ble_version();
  if (ver != s_bt_ver) { s_bt_ver = ver; bt_recarregar(); }
  const bool via_bt = ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH;
  const char* enlace = via_bt ? mesh_ble_descricao()
                     : ajustes_mesh_via() == AJUSTES_MESH_INTERNET ? "Mesh pela internet" : "Mesh pelo cabo Grove";
  snprintf(assin, sizeof(assin), "%s|%d|%d|%d|%s|%s", sub, (int)ajustes_bluetooth(), (int)varrendo, s_bt_n, s_aviso, enlace);
  const bool mudou = pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) || tudo;
  if (mudou) {
    conteudo_limpo(d, a);
    int y = CONTEUDO_Y;
    y = info(d, a, y, "Estado", sub, pronto ? TFT_GREENYELLOW : TFT_WHITE);
    y = info(d, a, y, "Conexão do Mesh", enlace, mesh_ble_conectado() ? TFT_GREENYELLOW : TFT_WHITE);
    const bool ligado = tab5_ble_state() != TAB5_BLE_OFF;
    const bool descendo = tab5_ble_state() == TAB5_BLE_STOPPING;
    y = botao_largo(d, a, y, descendo ? "DESLIGANDO..." : ligado ? "DESLIGAR O BLUETOOTH" : "LIGAR O BLUETOOTH",
                    descendo ? TFT_DARKGREY : ligado ? TFT_ORANGE : TFT_GREENYELLOW);
    y = botao_largo(d, a, y, varrendo ? "PROCURANDO..." : "PROCURAR APARELHOS",
                    pronto && !varrendo ? TFT_SKYBLUE : TFT_DARKGREY);
    texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d),
          s_aviso[0] ? s_aviso : (s_bt_n ? "Toque num Meshtastic (verde) para conectar o Mesh" : "Nenhum aparelho visto ainda"));
  }
  const area_t janela = { a.x, a.y + bt_lista_y0(), a.largura, a.altura - bt_lista_y0() };
  lista_desenhar(&s_l_bt, d, janela, TFT_BLACK, mudou);
}

static void reiniciar_mesh_trabalho(void*)
{
  tab5_mesh_parar();
  // Bluetooth e internet sobem direto (a internet espera o Wi-Fi por conta
  // propria). O cabo: a sondagem acha o C6L e sobe a serial.
  if (ajustes_mesh_via() != AJUSTES_MESH_SERIAL) { tab5_mesh_iniciar(); }
  else { sistema_grove_redetectar(); }
}

static void toque_bt(int tx, int ty)
{
  const int l = sistema_area().largura - 2 * MARGEM;
  int y = CONTEUDO_Y + 84 + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    // Descendo: o botao espera a pilha terminar (o toque repetido de quem
    // achou que nao pegou nao empilha pedidos).
    if (tab5_ble_state() == TAB5_BLE_STOPPING) { return; }
    sistema_bluetooth(tab5_ble_state() == TAB5_BLE_OFF);
    s_aviso[0] = 0;
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    if (tab5_ble_scan(6000)) { s_aviso[0] = 0; }
    sistema_pedir_redesenho();
    return;
  }
  int dy = 0;
  const int i = lista_item_tocado(&s_l_bt, tx, ty, &dy);
  if (i < 0 || i >= s_bt_n) { return; }
  char end[18];
  tab5_ble_address_text(s_bt[i].addr, end);
  const char* nome = s_bt[i].name[0] ? s_bt[i].name : end;
  // Todo toque responde: um aparelho que o sistema nao sabe usar diz isso, em
  // vez de o toque parecer perdido.
  if (!s_bt[i].meshtastic) {
    char dist[12];
    tab5_signal_distance_text(s_bt[i].rssi, s_bt[i].p1m ? s_bt[i].p1m : TAB5_SIGNAL_P1M_BLE, dist, sizeof(dist));
    if (s_bt[i].brand[0]) {
      snprintf(s_aviso, sizeof(s_aviso), "%s (%s) a %s - só leitura", nome, s_bt[i].brand, dist);
    } else {
      snprintf(s_aviso, sizeof(s_aviso), "%s a %s: sem uso ainda (só nós Meshtastic)", nome, dist);
    }
    ESP_LOGI("ajustes", "bluetooth: %s (%s) nao e Meshtastic - nada a fazer", nome, end);
    sistema_pedir_redesenho();
    return;
  }
  // Meshtastic: vira o no do Mesh, e o Mesh passa para o Bluetooth - tocar e
  // o pedido de conectar, e a linha "Conexão" acompanha o que acontece.
  ajustes_definir_mesh_ble_endereco(end);
  if (ajustes_mesh_via() != AJUSTES_MESH_BLUETOOTH) { ajustes_definir_mesh_via(AJUSTES_MESH_BLUETOOTH); }
  // Conectar liga o Bluetooth - e fica gravado, como no botao.
  if (!ajustes_bluetooth()) { ajustes_definir_bluetooth(true); }
  tab5_ble_turn_on();   // I2C do trilho: aqui, na interface
  sistema_trabalho_solto(reiniciar_mesh_trabalho, nullptr);
  snprintf(s_aviso, sizeof(s_aviso), "conectando a %s...", nome);
  ESP_LOGI("ajustes", "bluetooth: conectar o Mesh a %s (%s)", nome, end);
  sistema_pedir_redesenho();
}

// ---- ESP-NOW ---------------------------------------------------------------
static void pag_espnow(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[160], val[24];
  est_espnow(sub, sizeof(sub));
  const uint8_t f = tab5_espnow_canal_fixo();
  snprintf(assin, sizeof(assin), "%s|%u|%u", sub, (unsigned)f, (unsigned)tab5_espnow_canal());
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  y = info(d, a, y, "Canal agora", sub);
  y = botao_largo(d, a, y, "USAR O CANAL DO ROTEADOR", f ? TFT_SKYBLUE : TFT_DARKGREY);
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Canal fixo");
  y += 34;
  if (f) { snprintf(val, sizeof(val), "%u", (unsigned)f); } else { snprintf(val, sizeof(val), "-"); }
  menos_mais(d, a, y, val);
  y += BTN_A + 24;
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "O rádio fica em um canal por vez. No canal do roteador o Wi-Fi e o ESP-NOW "
            "funcionam juntos, mas os outros aparelhos precisam estar nesse canal. "
            "Canal fixo solta o roteador. A escolha fica salva.");
}
static void toque_espnow(int tx, int ty)
{
  int y = CONTEUDO_Y + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, sistema_area().largura - 2 * MARGEM, BTN_A)) {
    tab5_espnow_definir_canal(0);
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 20 + 34;
  const int s = menos_mais_toque(tx, ty, y);
  if (s) {
    int c = tab5_espnow_canal_fixo();
    c = (c == 0) ? (s > 0 ? 1 : 13) : c + s;
    if (c < 1) { c = 13; } if (c > 13) { c = 1; }
    tab5_espnow_definir_canal((uint8_t)c);
    sistema_pedir_redesenho();
  }
}

// ---- Meshtastic ------------------------------------------------------------
static int mesh_y_via(void)    { return CONTEUDO_Y + 30; }
static int mesh_y_pin(void)    { return mesh_y_via() + BTN_A + 24 + 84 + 84; }
// A posicao do GPS para a malha saiu daqui: e do grupo Localizacao do app
// Mesh (Configurar o no), junto do que o NO faz com ela - dois lugares para a
// mesma coisa deixavam um contradizer o outro.
static int mesh_y_net(void)    { return mesh_y_pin() + 34 + BTN_A + 24; }   // bloco da internet (MQTT)

static bool s_pin_para_no = false;   // o editor abriu porque o no esta pedindo o PIN agora
static char s_pin_pintado[16];       // o numero que esta na tela

// O numero do PIN, sozinho: limpa so a faixa dele (ate o botao) e escreve.
static void pin_valor(lgfx::LovyanGFX& d, const area_t& a, bool forcar)
{
  char pin[16];
  if (s_pin_editando) { snprintf(pin, sizeof(pin), "%s_", s_pin_txt); }
  else                { snprintf(pin, sizeof(pin), "%06lu", (unsigned long)ajustes_mesh_pin()); }
  if (!pintura_mudou_s(&s_p, s_pin_pintado, sizeof(s_pin_pintado), pin) && !forcar) { return; }
  const int y = mesh_y_pin() + 34;
  if (!forcar) { d.fillRect(a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM - 260 - 8, BTN_A, TFT_BLACK); }
  texto(d, a.x + MARGEM, a.y + y + BTN_A / 2, 40, true, s_pin_editando ? TFT_YELLOW : TFT_WHITE, pin, middle_left);
}

static void pag_mesh(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  // O no pediu o PIN (modo aleatorio): o teclado abre sozinho.
  if (mesh_ble_esperando_pin() && !s_pin_editando) {
    s_pin_editando = true;
    s_pin_para_no = true;
    s_pin_txt[0] = 0;
    tecvirt_abrir(&s_kb);
    tudo = true;
  }
  char assin[320], salvo[18];
  ajustes_mesh_ble_endereco(salvo);
  snprintf(assin, sizeof(assin), "%d|%s|%s|%lu|%u|%d|%s|%d|%s", (int)ajustes_mesh_via(), tab5_mesh_enlace_descricao(), salvo,
           (unsigned long)ajustes_mesh_pin(), 0u, (int)s_pin_editando, "",
           (int)ajustes_mesh_internet(), tab5_mesh_internet_estado());
  // A memoria da assinatura (s_assin) guarda 159 caracteres: o que muda
  // depois disso (os estados, que vem no fim) nao repintaria. Vai o HASH da
  // assinatura inteira (FNV-1a), que cabe e muda com qualquer caractere.
  uint32_t h = 2166136261u;
  for (const char* c = assin; *c; ++c) { h = (h ^ (uint8_t)*c) * 16777619u; }
  snprintf(assin, sizeof(assin), "mesh|%08lx", (unsigned long)h);
  // O PIN digitado fica FORA da assinatura: a letra repinta so o numero.
  if (s_pin_editando) { tecvirt_posicionar(&s_kb, a); }
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) {
    if (s_pin_editando) { pin_valor(d, a, false); tecvirt_pintar(&s_kb, d, false); }
    return;
  }
  conteudo_limpo(d, a);
  const int l3 = (a.largura - 2 * MARGEM - 32) / 3;

  // TRES VIAS. O Grove ganha sozinho quando o C6L e achado no cabo; depois
  // disso a escolha aqui vale ate o proximo achado (ver settings.h).
  texto(d, a.x + MARGEM, a.y + CONTEUDO_Y, 22, false, cor_sub(d), "Falar com a malha por (o Grove tem prioridade)");
  int y = mesh_y_via();
  static const char* const ROTULO[3] = { "CABO GROVE", "BLUETOOTH", "INTERNET" };
  for (int k = 0; k < 3; ++k) {
    const bool sel = ajustes_mesh_via() == k;
    tab5_botao(d, a.x + MARGEM + k * (l3 + 16), a.y + y, l3, BTN_A, ROTULO[k],
               sel ? TFT_GREENYELLOW : d.color565(50, 55, 65), sel ? TFT_BLACK : TFT_LIGHTGREY);
  }
  y += BTN_A + 24;
  y = info(d, a, y, "Enlace", tab5_mesh_enlace_descricao(),
           tab5_mesh_no_ar() ? TFT_GREENYELLOW : TFT_WHITE);
  y = info(d, a, y, "Nó Bluetooth", salvo[0] ? salvo : "o primeiro que aparecer");

  // PIN
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "PIN do pareamento Bluetooth");
  y += 34;
  pin_valor(d, a, true);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A,
             s_pin_editando ? "CANCELAR" : "ALTERAR", s_pin_editando ? TFT_ORANGE : TFT_SKYBLUE);
  y += BTN_A + 24;

  if (s_pin_editando) {
    paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
              s_pin_para_no ? "O nó está pedindo o PIN agora: digite o que ele mostra e toque ENVIAR (30 s)."
                            : "Digite o PIN fixo do nó (123456 nos sem tela) e toque ENVIAR.");
    tecvirt_pintar(&s_kb, d, true);
    return;
  }

  // Internet (MQTT pelo Wi-Fi): so roda sem o no; o Grove tem prioridade.
  const bool net = ajustes_mesh_internet();
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Internet também quando faltar o nó (MQTT pelo Wi-Fi)");
  y += 34;
  texto(d, a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, net ? TFT_GREENYELLOW : TFT_WHITE,
        net ? "Ligada" : "Desligada", middle_left);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A, net ? "DESLIGAR" : "LIGAR",
             net ? TFT_ORANGE : TFT_SKYBLUE);
  y += BTN_A + 12;
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, tab5_mesh_internet_estado());
  y += 100;
  if (salvo[0]) { y = botao_largo(d, a, y, "ESQUECER O NÓ BLUETOOTH", d.color565(50, 55, 65)); }
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "Com o nó no Bluetooth o cabo Grove fica livre para o GPS. O que o nó faz "
            "(localização, internet do Tab5 para ele, rádio): app Mesh, página Nó.");
}

static void aplicar_pin(void)
{
  if (s_pin_txt[0]) {
    const uint32_t pin = (uint32_t)atol(s_pin_txt);
    if (s_pin_para_no) {
      // PIN PEDIDO AGORA PELO NO (o que aparece na tela dele): vai so para
      // este pareamento. Nao e salvo - o no gera outro na proxima vez, e um
      // numero velho salvo so faria o pareamento seguinte falhar.
      if (mesh_ble_esperando_pin()) { mesh_ble_digitar_pin(pin); }
    } else {
      // PIN FIXO, editado por conta propria (ALTERAR): fica salvo e e tentado
      // primeiro nas proximas conexoes.
      ajustes_definir_mesh_pin(pin);
      ajustes_definir_mesh_pin_na_tela(false);
      mesh_ble_tentar_de_novo();
    }
  }
  s_pin_para_no = false;
  s_pin_editando = false;
  tecvirt_fechar(&s_kb);
  sistema_pedir_redesenho_completo();
}

static void pin_tecla(int ac)
{
  const size_t n = strlen(s_pin_txt);
  if (ac >= '0' && ac <= '9' && n < 6) { s_pin_txt[n] = (char)ac; s_pin_txt[n + 1] = 0; }
  else if (ac == TECVIRT_APAGAR && n) { s_pin_txt[n - 1] = 0; }
  else if (ac == TECVIRT_ENVIAR) { aplicar_pin(); return; }
  else if (ac == TECVIRT_FECHAR) { s_pin_editando = false; tecvirt_fechar(&s_kb); sistema_pedir_redesenho_completo(); return; }
  sistema_pedir_redesenho();
}

static void toque_mesh(int tx, int ty)
{
  const area_t a = sistema_area();
  if (s_pin_editando && tecvirt_contem(&s_kb, a.x + tx, a.y + ty)) {
    const int ac = tecvirt_clique(&s_kb, a.x + tx, a.y + ty);
    if (ac) { pin_tecla(ac); }
    return;
  }
  const int l3 = (a.largura - 2 * MARGEM - 32) / 3;
  int y = mesh_y_via();
  int via = -1;
  for (int k = 0; k < 3; ++k) { if (tab5_dentro(tx, ty, MARGEM + k * (l3 + 16), y, l3, BTN_A)) { via = k; } }
  if (via >= 0 && via != ajustes_mesh_via()) {
    ajustes_definir_mesh_via((uint8_t)via);
    if (via == AJUSTES_MESH_BLUETOOTH) {   // liga o Bluetooth, e fica gravado como no botao
      if (!ajustes_bluetooth()) { ajustes_definir_bluetooth(true); }
      tab5_ble_turn_on();   // I2C do trilho: aqui, na interface
    }
    sistema_trabalho_solto(reiniciar_mesh_trabalho, nullptr);
    sistema_pedir_redesenho();
    return;
  }
  y = mesh_y_pin() + 34;
  if (tab5_dentro(tx, ty, a.largura - MARGEM - 260, y, 260, BTN_A)) {
    s_pin_editando = !s_pin_editando;
    s_pin_txt[0] = 0;
    if (s_pin_editando) { tecvirt_abrir(&s_kb); } else { tecvirt_fechar(&s_kb); }
    sistema_pedir_redesenho_completo();
    return;
  }
  if (s_pin_editando) { return; }
  y = mesh_y_net() + 34;
  if (tab5_dentro(tx, ty, a.largura - MARGEM - 260, y, 260, BTN_A)) {
    ajustes_definir_mesh_internet(!ajustes_mesh_internet());
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 12 + 100;
  char salvo[18];
  ajustes_mesh_ble_endereco(salvo);
  if (salvo[0] && tab5_dentro(tx, ty, MARGEM, y, a.largura - 2 * MARGEM, BTN_A)) {
    ajustes_definir_mesh_ble_endereco("");
    if (ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH) { sistema_trabalho_solto(reiniciar_mesh_trabalho, nullptr); }
    sistema_pedir_redesenho();
  }
}

static void teclar_mesh(const char* txt, uint8_t codigo)
{
  if (!s_pin_editando) { return; }
  const size_t n = strlen(s_pin_txt);
  if (codigo == TAB5_KEY_ENTER) { aplicar_pin(); return; }
  if (codigo == TAB5_KEY_BACKSPACE) { if (n) { s_pin_txt[n - 1] = 0; } sistema_pedir_redesenho(); return; }
  if (codigo == TAB5_KEY_ESC) { s_pin_editando = false; tecvirt_fechar(&s_kb); sistema_pedir_redesenho_completo(); return; }
  if (txt && txt[0] >= '0' && txt[0] <= '9' && n < 6) { s_pin_txt[n] = txt[0]; s_pin_txt[n + 1] = 0; sistema_pedir_redesenho(); }
}

// ---- LoRa ------------------------------------------------------------------
// Diagnostico do transceptor NU no M5-Bus. Nao usa nem altera o servico
// Meshtastic: sao aparelhos e caminhos eletricos diferentes.
static const int LORA_BOTAO_Y = 840;

// Uma linha "sw2=G4* · sw3=G2 · sw4=G47" a partir da tabela do HAL - gerada,
// nao copiada a mao, para nunca mais destoar dela (era assim que o "24/G35"
// de NSS, que o esquematico provou nunca ter existido, tinha sobrevivido).
// '*' marca o switch que bateria com a rota tentada agora (gpio_ativo).
static void lora_linha_candidatos(char* out, size_t cap, const tab5_lora_pin_t* pinos, size_t n,
                                  tab5_lora_signal_t sinal, int gpio_ativo)
{
  size_t k = 0;
  out[0] = 0;
  for (size_t i = 0; i < n; ++i) {
    if (pinos[i].signal != sinal) { continue; }
    char item[24];
    const int len = snprintf(item, sizeof(item), "%ssw%u=G%d%s", k ? " · " : "",
                             (unsigned)pinos[i].switch_number, pinos[i].gpio,
                             pinos[i].gpio == gpio_ativo ? "*" : "");
    if (len <= 0 || k + (size_t)len + 1 > cap) { break; }
    memcpy(out + k, item, (size_t)len + 1);
    k += (size_t)len;
  }
}

static void pag_lora(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  const tab5_lora_status_t s = tab5_lora_status();
  tab5_lora_pin_t pinos[16];
  const size_t n = tab5_lora_pins(pinos, sizeof(pinos) / sizeof(pinos[0]));
  char assin[384];
  int pos = snprintf(assin, sizeof(assin), "%d|%u|%u|%04X|%02X|%d|%d|%d",
                     (int)s.probed, s.ioe_address, s.ioe_mask, s.ioe_uid, s.ioe_revision,
                     (int)s.radio_present, s.radio_error, s.state_error);
  for (size_t i = 0; i < n && pos > 0 && pos < (int)sizeof(assin); ++i) {
    pos += snprintf(assin + pos, sizeof(assin) - (size_t)pos, "|%d", (int)pinos[i].state);
  }
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }

  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  char valor[96];
  if (!s.ioe_address) {
    snprintf(valor, sizeof(valor), "M5IOE1 não encontrado · SX1262 não testado");
  } else if (s.radio_present) {
    snprintf(valor, sizeof(valor), "SX1262 respondeu · M5IOE1 0x%02X rev %02X",
             s.ioe_address, s.ioe_revision);
  } else {
    snprintf(valor, sizeof(valor), "M5IOE1 0x%02X · SX1262 não respondeu (%d)",
             s.ioe_address, s.radio_error);
  }
  y = info(d, a, y, "Estado ao vivo", valor,
           s.radio_present ? TFT_GREENYELLOW : (s.ioe_address ? TFT_ORANGE : TFT_WHITE));

  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "SW2 · endereço do M5IOE1 (verde = respondeu)");
  y += 32;
  static const uint8_t END[] = { 0x74, 0x73, 0x72, 0x71 };
  static const char* const AB[] = { "A0 B0", "A1 B0", "A0 B1", "A1 B1" };
  const int caixa_l = (a.largura - 2 * MARGEM - 24) / 4;
  for (int i = 0; i < 4; ++i) {
    const int x = a.x + MARGEM + i * (caixa_l + 8);
    const bool vivo = (s.ioe_mask & (1u << (END[i] - 0x71))) != 0;
    d.fillRoundRect(x, a.y + y, caixa_l, 60, 8,
                    vivo ? d.color565(35, 105, 45) : d.color565(38, 42, 50));
    snprintf(valor, sizeof(valor), "%s  0x%02X", AB[i], END[i]);
    texto(d, x + caixa_l / 2, a.y + y + 30, 20, vivo, TFT_WHITE, valor, middle_center);
  }
  y += 84;

  // CONFERIDO no esquematico do modulo e cruzado com o do Tab5 (M5_BUS) -
  // ver o aviso grande em hal/tab5_lora.hpp. O switch 1 do SW1 (rotulado
  // "ISO_G0" no modulo) nao aparece aqui de proposito: nao vai a pino
  // nenhum do M5-Bus, so NSS tem 3 candidatos de verdade, nao 4.
  char linha[128];
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "SW1 · switches 2-4 = NSS, 5-8 = BUSY");
  y += 30;
  lora_linha_candidatos(valor, sizeof(valor), pinos, n, TAB5_LORA_SIGNAL_NSS, s.gpio_nss);
  snprintf(linha, sizeof(linha), "NSS   %s", valor);
  texto(d, a.x + MARGEM, a.y + y, 24, false, TFT_WHITE, linha);
  y += 30;
  lora_linha_candidatos(valor, sizeof(valor), pinos, n, TAB5_LORA_SIGNAL_BUSY, s.gpio_busy);
  snprintf(linha, sizeof(linha), "BUSY  %s", valor);
  texto(d, a.x + MARGEM, a.y + y, 24, false, TFT_WHITE, linha);
  y += 30;
  texto(d, a.x + MARGEM, a.y + y, 20, false, cor_sub(d), "switch 1 do SW1 é interno do módulo - sem pino aqui");
  y += 38;

  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "SW3 · switches 9-11 = IRQ");
  y += 30;
  lora_linha_candidatos(valor, sizeof(valor), pinos, n, TAB5_LORA_SIGNAL_IRQ, s.gpio_irq);
  snprintf(linha, sizeof(linha), "IRQ   %s", valor);
  texto(d, a.x + MARGEM, a.y + y, 24, false, TFT_WHITE, linha);
  y += 30;
  texto(d, a.x + MARGEM, a.y + y, 20, false, cor_sub(d), "* é a rota tentada agora - confira contra os seus switches");
  y += 46;

  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Sonda elétrica · pino M5-Bus / GPIO");
  y += 30;
  const int col_l = (a.largura - 2 * MARGEM) / 2;
  for (size_t i = 0; i < n; ++i) {
    const int col = i >= (n + 1) / 2;
    const int lin = col ? (int)i - (int)((n + 1) / 2) : (int)i;
    snprintf(valor, sizeof(valor), "%2u / G%-2d  %s", pinos[i].mbus, pinos[i].gpio,
             tab5_lora_pin_state_name(pinos[i].state));
    texto(d, a.x + MARGEM + col * col_l, a.y + y + lin * 30, 21, false,
          pinos[i].state == TAB5_LORA_PIN_FLOATING ? TFT_DARKGREY : TFT_LIGHTGREY, valor);
  }

  tab5_botao(d, a.x + MARGEM, a.y + LORA_BOTAO_Y,
             a.largura - 2 * MARGEM, BTN_A, "SONDAR E TENTAR O SX1262", TFT_SKYBLUE);
  texto(d, a.x + MARGEM, a.y + LORA_BOTAO_Y + BTN_A + 12, 20, false, cor_sub(d),
        "SDA/SCL são lidos sem mudar os pulls, para preservar o I2C interno.");
}

static void toque_lora(int tx, int ty)
{
  const area_t a = sistema_area();
  if (!tab5_dentro(tx, ty, MARGEM, LORA_BOTAO_Y, a.largura - 2 * MARGEM, BTN_A)) { return; }
  tab5_lora_iniciar();
  sistema_pedir_redesenho();
}

// ---- Tela ------------------------------------------------------------------
static void pag_tela(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char assin[64], v1[16], v2[16];
  snprintf(v1, sizeof(v1), "%u s", (unsigned)ajustes_tela_s());
  // O relogio EM USO, e nao o gravado: na tela TDDI ele fica no de fabrica
  // (tab5_painel em hal/tab5_power.hpp) e o gravado nao vale.
  snprintf(v2, sizeof(v2), "%d MHz", tab5_power_dpi_clock());
  snprintf(assin, sizeof(assin), "%s|%s|%d|%d", v1, v2, (int)ajustes_bloqueio(), (int)ajustes_parede());
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Apagar a tela depois de, sem toque");
  y += 34; menos_mais(d, a, y, v1); y += BTN_A + 12;
  char ajuda[64];
  snprintf(ajuda, sizeof(ajuda), "De %d a %d segundos.", AJUSTES_TELA_MIN_S, AJUSTES_TELA_MAX_S);
  y = paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, ajuda) - a.y + 24;
  // Bloqueio: alturas fixas, o toque (toque_tela) repete esta conta.
  const bool bq = ajustes_bloqueio();
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Tela de bloqueio ao ligar e ao acordar");
  y += 34;
  texto(d, a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, bq ? TFT_GREENYELLOW : TFT_WHITE,
        bq ? "Ligada" : "Desligada", middle_left);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A, bq ? "DESLIGAR" : "LIGAR",
             bq ? TFT_ORANGE : TFT_SKYBLUE);
  y += BTN_A + 24;
  // Papel de parede do inicio: TROCAR passa pelas opcoes em roda.
  static const char* const PAREDE[] = { "Sorteio", "Fios", "Ondas", "Teia", "Raios", "Círculos", "Nenhum" };
  const uint8_t pp = ajustes_parede();
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Papel de parede do início");
  y += 34;
  texto(d, a.x + MARGEM, a.y + y + BTN_A / 2, 32, true,
        pp == AJUSTES_PAREDE_NENHUM ? TFT_WHITE : TFT_GREENYELLOW, PAREDE[pp], middle_left);
  tab5_botao(d, a.x + a.largura - MARGEM - 260, a.y + y, 260, BTN_A, "TROCAR", TFT_SKYBLUE);
  y += BTN_A + 24;
  texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), "Relógio do painel");
  y += 34; menos_mais(d, a, y, v2); y += BTN_A + 12;
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "Menor economiza bateria (menos leitura da memória pelo painel), mas reduz "
            "os quadros por segundo. 60 MHz é o padrão.");
}
static void toque_tela(int tx, int ty)
{
  int y = CONTEUDO_Y + 34;
  int s = menos_mais_toque(tx, ty, y);
  if (s) { ajustes_definir_tela_s((uint8_t)(ajustes_tela_s() + s * PASSO_TELA_S)); sistema_pedir_redesenho(); return; }
  y += BTN_A + 12 + 32 + 24 + 34;   // linha do botao do bloqueio
  if (tab5_dentro(tx, ty, sistema_area().largura - MARGEM - 260, y, 260, BTN_A)) {
    ajustes_definir_bloqueio(!ajustes_bloqueio());
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 24 + 34;   // linha do papel de parede
  if (tab5_dentro(tx, ty, sistema_area().largura - MARGEM - 260, y, 260, BTN_A)) {
    ajustes_definir_parede((uint8_t)((ajustes_parede() + 1) % (AJUSTES_PAREDE_NENHUM + 1)));
    sistema_pedir_redesenho();
    return;
  }
  y += BTN_A + 24 + 34;
  s = menos_mais_toque(tx, ty, y);
  if (s) {
    int v = tab5_power_dpi_clock() + s * PASSO_RELOGIO;
    if (v < AJUSTES_RELOGIO_MIN) { v = AJUSTES_RELOGIO_MIN; }
    if (v > AJUSTES_RELOGIO_MAX) { v = AJUSTES_RELOGIO_MAX; }
    // Aplica primeiro, grava so se deu certo: um valor que o painel recusou
    // nao pode virar o do proximo boot.
    if (tab5_power_dpi_clock(v)) {
      ajustes_definir_relogio_painel((uint8_t)v);
      sistema_pedir_redesenho_completo();
    }
  }
}

// ---- Aplicativos -----------------------------------------------------------
// A lista vem da tabela completa de apps, nunca da home ja filtrada.
static const int APPS_ITEM_A = 92;
static const int APPS_LISTA_Y = CONTEUDO_Y + 52;

static int apps_item_altura(int, void*) { return APPS_ITEM_A; }

static void apps_item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= SISTEMA_NUM_APPS) { return; }
  const char* nome = SISTEMA_APPS[i]->name;
  const bool ajustes = strcasecmp(nome, "Ajustes") == 0;
  const bool oculto = !ajustes && nome_em_lista(s_apps_ocultos, nome);
  d.fillRect(x0, y, largura, APPS_ITEM_A, TFT_BLACK);
  texto(d, x0 + MARGEM, y + APPS_ITEM_A / 2, 30, true,
        oculto ? cor_sub(d) : TFT_WHITE, nome, middle_left);
  const int bl = 230, ba = 60;
  tab5_botao(d, x0 + largura - MARGEM - bl, y + (APPS_ITEM_A - ba) / 2, bl, ba,
             ajustes ? "SEMPRE VISÍVEL" : oculto ? "MOSTRAR" : "OCULTAR",
             ajustes ? TFT_DARKGREY : oculto ? TFT_SKYBLUE : TFT_ORANGE);
  d.drawFastHLine(x0 + MARGEM, y + APPS_ITEM_A - 1, largura - 2 * MARGEM, cor_linha(d));
}

static void apps_alternar(int alvo)
{
  if (alvo < 0 || alvo >= SISTEMA_NUM_APPS) { return; }
  // Ajustes fica sempre visivel para a pessoa nao perder o caminho de volta
  // ate esta pagina.
  if (strcasecmp(SISTEMA_APPS[alvo]->name, "Ajustes") == 0) { return; }

  char novos[sizeof(s_apps_ocultos)];
  size_t k = 0;
  novos[0] = 0;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const char* nome = SISTEMA_APPS[i]->name;
    if (strcasecmp(nome, "Ajustes") == 0) { continue; }
    bool oculto = nome_em_lista(s_apps_ocultos, nome);
    if (i == alvo) { oculto = !oculto; }
    if (!oculto) { continue; }
    const size_t n = strlen(nome);
    if (k + n + 2 > sizeof(novos)) { return; }
    if (k) { novos[k++] = ','; }
    memcpy(novos + k, nome, n);
    k += n;
    novos[k] = 0;
  }
  snprintf(s_apps_ocultos, sizeof(s_apps_ocultos), "%s", novos);
  ajustes_definir_apps_ocultos(s_apps_ocultos);
  lista_item_mudou(&s_l_apps, alvo);
  sistema_home_apps_recarregar();
}

static void pag_apps(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  if (tudo) {
    conteudo_limpo(d, a);
    texto(d, a.x + MARGEM, a.y + CONTEUDO_Y + 10, 22, false, cor_sub(d),
          "Escolha quais aplicativos aparecem na tela inicial.");
  }
  const area_t janela = { a.x, a.y + APPS_LISTA_Y, a.largura, a.altura - APPS_LISTA_Y };
  lista_desenhar(&s_l_apps, d, janela, TFT_BLACK, tudo);
}

static void toque_apps(int tx, int ty)
{
  int dy = 0;
  const int i = lista_item_tocado(&s_l_apps, tx, ty, &dy);
  if (i >= 0) { apps_alternar(i); }
}

// ---- Teclado ---------------------------------------------------------------
static void pag_teclado(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[64];
  est_teclado(sub, sizeof(sub));
  if (ajustes_teclado() && tab5_keyboard_present()) {
    snprintf(sub, sizeof(sub), "Encaixado (firmware %u)", (unsigned)tab5_keyboard_version());
  }
  snprintf(assin, sizeof(assin), "%s|%d", sub, (int)ajustes_teclado());
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = info(d, a, CONTEUDO_Y, "Estado", sub, tab5_keyboard_present() ? TFT_GREENYELLOW : TFT_WHITE);
  y = botao_largo(d, a, y, ajustes_teclado() ? "DESATIVAR" : "ATIVAR",
                  ajustes_teclado() ? TFT_ORANGE : TFT_GREENYELLOW);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "O teclado encaixa no ExtPort1. Desativado, o sistema não procura por ele "
            "e o teclado da tela é usado nas conversas.");
}
static void toque_teclado(int tx, int ty)
{
  if (tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + 84, sistema_area().largura - 2 * MARGEM, BTN_A)) {
    const bool novo = !ajustes_teclado();
    ajustes_definir_teclado(novo);
    tab5_keyboard_enable(novo);
    sistema_pedir_redesenho();
  }
}

// ---- Grove -----------------------------------------------------------------
static void pag_grove(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char sub[48], assin[64];
  est_grove(sub, sizeof(sub));
  snprintf(assin, sizeof(assin), "%s|%d", sub, (int)ajustes_grove());
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = info(d, a, CONTEUDO_Y, "Estado", sub);
  y = botao_largo(d, a, y, ajustes_grove() ? "DESLIGAR O 5V" : "LIGAR O 5V",
                  ajustes_grove() ? TFT_ORANGE : TFT_GREENYELLOW);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "Ligado, o módulo do cabo (C6L, GPS) fica sempre alimentado e não reinicia "
            "a cada uso. Desligado economiza bateria sem nada plugado.");
}
static void toque_grove(int tx, int ty)
{
  if (tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + 84, sistema_area().largura - 2 * MARGEM, BTN_A)) {
    ajustes_definir_grove(!ajustes_grove());
    sistema_reaplicar_perfil();
    sistema_grove_redetectar();
    sistema_pedir_redesenho();
  }
}

// ---- Sobre -----------------------------------------------------------------
static void pag_sobre(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  char assin[96], mem[48], ps[48], ativo[32];
  const size_t li = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t lp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  snprintf(mem, sizeof(mem), "%u KB livres", (unsigned)(li / 1024));
  snprintf(ps, sizeof(ps), "%u KB livres", (unsigned)(lp / 1024));
  const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
  snprintf(ativo, sizeof(ativo), "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
  snprintf(assin, sizeof(assin), "%s|%s|%s", mem, ps, ativo);
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  int y = CONTEUDO_Y;
  char sistema[48];
  snprintf(sistema, sizeof(sistema), "M5tabOS %s", esp_app_get_description()->version);
  y = info(d, a, y, "Sistema", sistema);
  y = info(d, a, y, "Compilado em", m5tabos_compilado);
  y = info(d, a, y, "ESP-IDF", esp_get_idf_version());
  y = info(d, a, y, "Memória interna", mem);
  y = info(d, a, y, "PSRAM", ps);
  y = info(d, a, y, "Ligado há", ativo);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "M5Stack Tab5: ESP32-P4 (sistema) e ESP32-C6 (Wi-Fi, Bluetooth e ESP-NOW).");
}

// ---- USB (pendrive) --------------------------------------------------------
// A tela do pendrive inteira: estado, transferencia, o diario das gravacoes
// guardadas, o resultado da ultima volta e o ajuste de ligar sozinho. O
// trabalho mora no servico (services/tab5_flashdrive) - esta pagina so pede e
// mostra, e a pessoa pode sair dela a vontade: o pendrive segue em segundo
// plano. A CONFIRMACAO de devolver sem o computador ter ejetado e daqui.
typedef struct { int x, y, l, a; } usb_ret_t;
static usb_ret_t s_usb_bt[3];          // os botoes como desenhados (coordenadas da area)
static bool      s_usb_confirmar = false;

static void kb_txt(char* o, size_t t, uint64_t b)
{
  if (b < 1024ULL * 1024)             { snprintf(o, t, "%u KB", (unsigned)(b / 1024)); }
  else if (b < 1024ULL * 1024 * 1024) { snprintf(o, t, "%.1f MB", (double)b / (1024.0 * 1024.0)); }
  else                                { snprintf(o, t, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0)); }
}

static int usb_botao(lgfx::LovyanGFX& d, const area_t& a, int i, int y, const char* txt, uint16_t cor)
{
  s_usb_bt[i] = { MARGEM, y, a.largura - 2 * MARGEM, BTN_A };
  tab5_botao(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM, BTN_A, txt, cor);
  return y + BTN_A + 20;
}

static void pag_usb(lgfx::LovyanGFX& d, const area_t& a, bool tudo)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  tab5_sd_summary_t r;
  tab5_sd_summary(&r);
  if (e.fase != PENDRIVE_ATIVO) { s_usb_confirmar = false; }
  // Aberta com um resultado esperando: a pessoa viu - o aviso da barra sai.
  if (e.fase == PENDRIVE_PARADO) { tab5_pendrive_resultado_visto(); }

  // O QUE MOSTRAR
  char estado[64], l2[96] = "", l3[80] = "", fim[96] = "";
  uint16_t cor = TFT_WHITE;
  switch (e.fase) {
    case PENDRIVE_PREPARANDO: snprintf(estado, sizeof(estado), "Preparando o cartão..."); break;
    case PENDRIVE_DEVOLVENDO: snprintf(estado, sizeof(estado), "Devolvendo o cartão..."); break;
    case PENDRIVE_ATIVO:
      if (!e.usb.montado)       { snprintf(estado, sizeof(estado), "Esperando o computador"); cor = TFT_YELLOW; }
      else if (e.usb.suspenso)  { snprintf(estado, sizeof(estado), "Computador parado ou cabo solto"); cor = TFT_ORANGE; }
      else if (e.usb.ativo)     { snprintf(estado, sizeof(estado), "Copiando..."); cor = TFT_SKYBLUE; }
      else                      { snprintf(estado, sizeof(estado), "Cartão no computador"); cor = TFT_GREENYELLOW; }
      break;
    default:
      snprintf(estado, sizeof(estado), "%s", e.oferta ? "Computador conectado" : "Desligado");
      cor = e.oferta ? TFT_YELLOW : TFT_WHITE;
      break;
  }
  if (e.fase == PENDRIVE_ATIVO) {
    char li[24], gr[24];
    kb_txt(li, sizeof(li), e.usb.lidos);
    kb_txt(gr, sizeof(gr), e.usb.gravados);
    snprintf(l2, sizeof(l2), "Lidos %s  ·  gravados %s%s", li, gr, e.usb.erros ? "  ·  ERROS" : "");
  }
  if (e.fase != PENDRIVE_PARADO || r.in_journal) {
    snprintf(l3, sizeof(l3), "%u gravação(ões) guardadas (%u de %u KB)",
             (unsigned)r.in_journal, (unsigned)(r.journal_used / 1024), (unsigned)(r.journal_total / 1024));
  }
  // DIAGNOSTICO: ate onde o computador chegou, na ordem em que ele faz. E o
  // que diz, num disco que nao aparece, se o problema e no USB (nem
  // reconheceu), no pedido do disco ou na leitura (o sistema dele desistiu).
  char diag[96] = "";
  if (e.fase == PENDRIVE_ATIVO) {
    const tab5_pendrive_usb_t& u = e.usb;
    if (!u.n_config)            { snprintf(diag, sizeof(diag), "USB: o computador não reconheceu o aparelho"); }
    else if (!u.n_inquiry)      { snprintf(diag, sizeof(diag), "USB: reconheceu, mas não pediu o disco"); }
    else if (!u.n_respostas)    { snprintf(diag, sizeof(diag), "USB: pediu o disco %ux, a resposta não chegou",
                                           (unsigned)u.n_inquiry); }
    else if (!u.n_capacidade)   { snprintf(diag, sizeof(diag), "USB: pediu o disco, não o tamanho (resp. %u, últ. 0x%02X)",
                                           (unsigned)u.n_respostas, (unsigned)u.op_respondido); }
    else if (!u.n_leituras)     { snprintf(diag, sizeof(diag), "USB: viu o disco, ainda não leu"); }
    else                        { snprintf(diag, sizeof(diag), "USB: %u leitura(s), %u entregue(s), a mais lenta %u ms",
                                           (unsigned)u.n_cmd_leitura, (unsigned)u.n_leituras_ok,
                                           (unsigned)u.leitura_max_ms); }
    // Parou ha mais de 3 s: diz ONDE - e o que separa um travamento do USB
    // (ou do cartao) do computador que simplesmente parou de pedir.
    if (u.n_config && u.parado_ms >= 3000) {
      switch (u.onde) {
        case PENDRIVE_ESPERA_CARTAO:
          snprintf(diag, sizeof(diag), "USB: PARADO no cartão (setor %u)", (unsigned)u.ultimo_setor); break;
        case PENDRIVE_ESPERA_USB:
          snprintf(diag, sizeof(diag), "USB: PARADO no USB, entregando o setor %u (%u/%u)",
                   (unsigned)u.ultimo_setor, (unsigned)u.n_leituras_ok, (unsigned)u.n_cmd_leitura); break;
        default:
          if (u.n_cmd_leitura) {
            snprintf(diag, sizeof(diag), "USB: o computador parou de pedir (%u/%u leituras)",
                     (unsigned)u.n_leituras_ok, (unsigned)u.n_cmd_leitura);
          }
          break;
      }
    }
    if (u.op_recusado) {
      const size_t k = strlen(diag);
      snprintf(diag + k, sizeof(diag) - k, " · recusou 0x%02X", (unsigned)u.op_recusado);
    }
  }
  char perdas[64] = "";
  if (r.lost) {
    snprintf(perdas, sizeof(perdas), "%u gravação(ões) perdidas: memória cheia", (unsigned)r.lost);
  }
  if (e.fase == PENDRIVE_PARADO) {
    switch (e.motivo) {
      case PENDRIVE_RECUSA_APP:        snprintf(fim, sizeof(fim), "Feche o app que usa o cartão (Arquivos, Câmera)."); break;
      case PENDRIVE_RECUSA_EM_USO:     snprintf(fim, sizeof(fim), "O cartão estava em uso. Tente de novo."); break;
      case PENDRIVE_RECUSA_SEM_CARTAO: snprintf(fim, sizeof(fim), "Sem cartão SD, ou ele não respondeu."); break;
      case PENDRIVE_RECUSA_SEM_DIARIO: snprintf(fim, sizeof(fim), "Falta a memória interna do pendrive: grave o firmware de novo."); break;
      case PENDRIVE_RECUSA_USB:        snprintf(fim, sizeof(fim), "O USB não iniciou. O cartão continua no Tab5."); break;
      default:
        if (e.sem_ejetar) {
          // Ver tab5_flashdrive.cpp (s_saiu_sem_ejetar): o disco nao aparecer
          // logo da proxima vez e o computador verificando o cartao.
          snprintf(fim, sizeof(fim), "Saiu sem ejetar: o computador verifica o cartão antes de abrir.");
        } else if (r.complete) {
          snprintf(fim, sizeof(fim), "Última vez: %u gravação(ões) aplicada(s)%s%s",
                   (unsigned)r.applied, r.conflicts ? ", conflito em " : "",
                   r.conflicts ? "arquivo(s) .tab5" : "");
        }
        break;
    }
  }

  char assin[sizeof(s_assin)];
  snprintf(assin, sizeof(assin), "%d|%d|%d|%s|%s|%s|%s|%d|%u|%u|%s", (int)e.fase, (int)e.motivo, (int)s_usb_confirmar,
           estado, l2, l3, fim, (int)ajustes_pendrive(), (unsigned)r.conflicts, (unsigned)r.lost, diag);
  if (!pintura_mudou_s(&s_p, s_assin, sizeof(s_assin), assin) && !tudo) { return; }
  conteudo_limpo(d, a);
  memset(s_usb_bt, 0, sizeof(s_usb_bt));

  int y = info(d, a, CONTEUDO_Y, "Cartão SD como pendrive (USB-C)", estado, cor);
  if (l2[0])  { texto(d, a.x + MARGEM, a.y + y, 26, false, TFT_WHITE, l2); y += 44; }
  if (l3[0])  { texto(d, a.x + MARGEM, a.y + y, 24, false, cor_sub(d), l3); y += 44; }
  if (diag[0])   { texto(d, a.x + MARGEM, a.y + y, 22, false, cor_sub(d), diag); y += 40; }
  if (perdas[0]) { texto(d, a.x + MARGEM, a.y + y, 24, false, TFT_RED, perdas); y += 44; }
  if (fim[0]) { texto(d, a.x + MARGEM, a.y + y, 24, false, TFT_ORANGE, fim); y += 44; }
  y += 8;

  switch (e.fase) {
    case PENDRIVE_PARADO:
      y = usb_botao(d, a, 0, y, "USAR COMO PENDRIVE", TFT_SKYBLUE);
      break;
    case PENDRIVE_ATIVO:
      if (s_usb_confirmar) {
        y = paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
                      "O computador não avisou que ejetou. Se ele tiver algo em cache, arquivos "
                      "podem ficar incompletos. Ejete no computador e o cartão volta sozinho. Já "
                      "usou \"Remover com segurança\" no Windows? Pode devolver.") - a.y + 16;
        y = usb_botao(d, a, 0, y, "VOLTAR", TFT_SKYBLUE);
        y = usb_botao(d, a, 1, y, "DEVOLVER MESMO ASSIM", TFT_RED);
      } else {
        y = usb_botao(d, a, 0, y, "DEVOLVER O CARTÃO AO TAB5", TFT_ORANGE);
      }
      break;
    default:
      y = usb_botao(d, a, 0, y, "AGUARDE...", TFT_DARKGREY);
      s_usb_bt[0] = { };   // nao responde
      break;
  }
  y = usb_botao(d, a, 2, y, ajustes_pendrive() ? "LIGAR SOZINHO NO COMPUTADOR: SIM" : "LIGAR SOZINHO NO COMPUTADOR: NÃO",
                ajustes_pendrive() ? TFT_GREENYELLOW : TFT_LIGHTGREY);
  paragrafo(d, a.x + MARGEM, a.y + y, a.largura - 2 * MARGEM,
            "O Tab5 segue usável enquanto isso. O que ele gravar fica na memória interna e "
            "vai para o cartão na volta. Arquivos e Câmera esperam o cartão. Para terminar, "
            "ejete o disco no computador.");
}

static bool usb_em(int i, int tx, int ty)
{
  const usb_ret_t& b = s_usb_bt[i];
  return b.l > 0 && tab5_dentro(tx, ty, b.x, b.y, b.l, b.a);
}

static void toque_usb(int tx, int ty)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  if (usb_em(2, tx, ty)) {
    ajustes_definir_pendrive(!ajustes_pendrive());
    sistema_pedir_redesenho();
    return;
  }
  if (e.fase == PENDRIVE_PARADO && usb_em(0, tx, ty)) {
    tab5_pendrive_iniciar();
    sistema_pedir_redesenho();
    return;
  }
  if (e.fase == PENDRIVE_ATIVO) {
    const bool montado = e.usb.montado && !e.usb.ejetado;
    if (!s_usb_confirmar && usb_em(0, tx, ty)) {
      if (montado) { s_usb_confirmar = true; } else { tab5_pendrive_devolver(); }
    } else if (s_usb_confirmar && usb_em(0, tx, ty)) {
      s_usb_confirmar = false;
    } else if (s_usb_confirmar && usb_em(1, tx, ty)) {
      ESP_LOGW("ajustes", "cartao devolvido sem ejecao do computador");
      s_usb_confirmar = false;
      tab5_pendrive_devolver();
    }
    sistema_pedir_redesenho();
  }
}

// ---------------------------------------------------------------------------
// DESPACHO
// ---------------------------------------------------------------------------
typedef void (*pag_desenhar_fn)(lgfx::LovyanGFX&, const area_t&, bool);
typedef void (*pag_tocar_fn)(int, int);
static const pag_desenhar_fn PAG_DESENHAR[PAG_N] = {
  pag_wifi, pag_bt, pag_espnow, pag_mesh, pag_lora, pag_tela, pag_apps, pag_teclado, pag_grove, pag_usb, pag_sobre };
static const pag_tocar_fn PAG_TOCAR[PAG_N] = {
  toque_wifi, toque_bt, toque_espnow, toque_mesh, toque_lora, toque_tela, toque_apps, toque_teclado, toque_grove, toque_usb, nullptr };

static void abrir_pagina(int p)
{
  if (s_pin_editando) { s_pin_editando = false; tecvirt_fechar(&s_kb); }
  s_pag = p;
  s_assin[0] = 0;
  s_aviso[0] = 0;
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); tecvirt_fechar(&s_kb); }
  if (p == PAG_WIFI) {
    lista_definir(&s_l_wifi, ROLAGEM_INICIO, 0, wf_item_altura, wf_item_desenhar, nullptr);
    s_wf_ver = 0;
    tab5_wifi_search();   // entrar na pagina ja procura
  } else {
    lista_esconder(&s_l_wifi);
  }
  if (p == PAG_BLUETOOTH) {
    lista_definir(&s_l_bt, ROLAGEM_INICIO, 0, bt_item_altura, bt_item_desenhar, nullptr);
    s_bt_ver = 0;
    // Entrar na pagina ja procura: e o que se quer ver ao abrir.
    if (tab5_ble_ready()) { tab5_ble_scan(6000); }
  } else {
    lista_esconder(&s_l_bt);
  }
  if (p == PAG_APPS) {
    ajustes_apps_ocultos(s_apps_ocultos, sizeof(s_apps_ocultos));
    lista_definir(&s_l_apps, ROLAGEM_INICIO, SISTEMA_NUM_APPS,
                  apps_item_altura, apps_item_desenhar, nullptr);
  } else {
    lista_esconder(&s_l_apps);
  }
  if (p == PAG_LORA) { tab5_lora_iniciar(); }
  if (p == PAG_INICIO) { s_ub = s_uv = -1; }
  else { lista_esconder(&s_lista); }
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

static bool ao_abrir(void)
{
  s_pag = PAG_INICIO;
  s_ub = s_uv = -1;
  ajustes_apps_ocultos(s_apps_ocultos, sizeof(s_apps_ocultos));
  memset(s_sub, 0, sizeof(s_sub));
  for (int i = 0; i < PAG_N; ++i) { PAGINAS[i].state(s_sub[i], sizeof(s_sub[i])); }
  lista_definir(&s_lista, ROLAGEM_INICIO, PAG_N, item_altura, item_desenhar, nullptr);
  pintura_pedir(&s_p);
  // O aviso do pendrive (barra de status) abre Ajustes: cai direto na pagina.
  if (tab5_pendrive_pede_atencao()) { abrir_pagina(PAG_USB); }
  return true;
}

static void ao_fechar(void)
{
  if (s_pin_editando) { s_pin_editando = false; tecvirt_fechar(&s_kb); }
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); tecvirt_fechar(&s_kb); }
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  // O NO PEDIU O PIN: em qualquer pagina de Ajustes, vai para a do
  // Meshtastic, que abre o teclado (o pareamento espera ~30 s).
  if (mesh_ble_esperando_pin() && s_pag != PAG_MESH) { abrir_pagina(PAG_MESH); }
  pintura_iniciar(&s_p);
  const bool tudo = pintura_completa(&s_p);
  if (s_pag == PAG_INICIO) { desenhar_inicio(d, a, tudo); return; }
  if (tudo) { cabecalho(d, a, PAGINAS[s_pag].name); }
  PAG_DESENHAR[s_pag](d, a, tudo);
}

static void ao_tocar(int tx, int ty)
{
  if (s_pag == PAG_INICIO) {
    if (ty < DIVISOR_Y) {
      mexer_rapido(ty < RAPIDO_Y0 + RAPIDO_A ? RAPIDO_Y0 : RAPIDO_Y0 + RAPIDO_A, tx, ty, false);
      return;
    }
    int dy = 0;
    const int i = lista_item_tocado(&s_lista, tx, ty, &dy);
    if (i >= 0 && i < PAG_N) { abrir_pagina(i); }
    return;
  }
  if (!s_pin_editando && !s_wf_senha && tab5_dentro(tx, ty, 16, 16, VOLTAR_L, VOLTAR_A)) { abrir_pagina(PAG_INICIO); return; }
  if (PAG_TOCAR[s_pag]) { PAG_TOCAR[s_pag](tx, ty); }
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  // Teclado aberto (senha do Wi-Fi ou PIN): os dedos sao dele.
  if (tecvirt_aberto(&s_kb)) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb, acoes, 8);
    for (int i = 0; i < n && tecvirt_aberto(&s_kb); ++i) {
      if (s_wf_senha)          { wf_tecla(acoes[i]); }
      else if (s_pin_editando) { pin_tecla(acoes[i]); }
    }
    return;
  }
  if (s_pag == PAG_INICIO) {
    // Arrastar o dedo na barra de brilho/volume ajusta ao vivo.
    if (pressionado && y >= 0 && y < DIVISOR_Y) {
      mexer_rapido(y < RAPIDO_Y0 + RAPIDO_A ? RAPIDO_Y0 : RAPIDO_Y0 + RAPIDO_A, x, y, true);
    }
    if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
  } else if (s_pag == PAG_BLUETOOTH) {
    if (lista_arrastar(&s_l_bt, x, y, pressionado)) { sistema_pedir_redesenho(); }
  } else if (s_pag == PAG_WIFI && !s_wf_senha) {
    if (lista_arrastar(&s_l_wifi, x, y, pressionado)) { sistema_pedir_redesenho(); }
  } else if (s_pag == PAG_APPS) {
    if (lista_arrastar(&s_l_apps, x, y, pressionado)) { sistema_pedir_redesenho(); }
  }
}

static void ao_teclar(const char* txt, uint8_t codigo, uint8_t)
{
  if (s_pag == PAG_MESH) { teclar_mesh(txt, codigo); }
  if (s_pag == PAG_WIFI) { teclar_wifi(txt, codigo); }
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao do lado, como a marca do Mesh: sai limpo em qualquer
// tamanho, nao gasta flash e nao serrilha. A letra grande que estava aqui era
// a fonte embutida ampliada 6x - cada pixel dela virava um quadrado de 6.

// ENGRENAGENS QUE ENGRENAM DE VERDADE (terceira versao).
//
// A anterior tinha a composicao certa - grande, e duas menores ao
// lado, como na referencia do icone de ajustes classico - mas as tres se
// SOBREPUNHAM: a distancia entre centros era bem menor que a soma dos raios, e
// viravam um borrao so. Agora e engrenagem de verdade:
//   - MESMO PASSO de dente nas tres (PASSO, % do lado): so assim o dente de
//     uma cabe no vao da outra; o raio de cada uma sai do numero de dentes;
//   - distancia entre centros = soma dos raios primitivos (onde os dentes se
//     encontram);
//   - FASE calculada: a grande aponta um dente para a media, a media poe um
//     VAO ali; para a pequena, a fase compensa onde o dente da grande caiu.
// Dente TRAPEZIOIDAL (mais largo na raiz que na ponta), baixo e largo - o
// dente fino e comprido da primeira prova lia como sol, nao engrenagem.
// Prototipado em imagem antes (a mesma conta), conferido tambem a 96 px.
static const float ENG_PASSO = 10.5f;   // passo circular, % do lado
static const float ENG_DENTE = 6.0f;    // altura do dente, % do lado

static float eng_raio(int n) { return (float)n * ENG_PASSO / (2.0f * (float)M_PI); }

static void desenhar_engrenagem(lgfx::LovyanGFX& d, int x0, int y0, int lado,
                                float cx, float cy, int n, float fase_graus,
                                uint16_t escuro, uint16_t fundo)
{
  const float k  = (float)lado / 100.0f;   // % do lado -> pixels
  const float rp = eng_raio(n);
  const float rr = rp - ENG_DENTE / 2.0f;  // raiz
  const float rt = rp + ENG_DENTE / 2.0f;  // ponta
  auto px = [&](float v) { return x0 + (int)lroundf(v * k); };
  auto py = [&](float v) { return y0 + (int)lroundf(v * k); };

  d.fillCircle(px(cx), py(cy), (int)lroundf(rr * k), escuro);
  const float passo = 360.0f / (float)n;
  const float hb = passo * 0.36f * (float)M_PI / 180.0f;   // meia largura na raiz
  const float ht = passo * 0.22f * (float)M_PI / 180.0f;   // meia largura na ponta
  for (int i = 0; i < n; ++i) {
    const float a = (fase_graus + (float)i * passo) * (float)M_PI / 180.0f;
    const int x1 = px(cx + rr * cosf(a - hb)), y1 = py(cy + rr * sinf(a - hb));
    const int x2 = px(cx + rt * cosf(a - ht)), y2 = py(cy + rt * sinf(a - ht));
    const int x3 = px(cx + rt * cosf(a + ht)), y3 = py(cy + rt * sinf(a + ht));
    const int x4 = px(cx + rr * cosf(a + hb)), y4 = py(cy + rr * sinf(a + hb));
    d.fillTriangle(x1, y1, x2, y2, x3, y3, escuro);
    d.fillTriangle(x1, y1, x3, y3, x4, y4, escuro);
  }
  // Furo do meio, na cor do fundo.
  d.fillCircle(px(cx), py(cy), (int)lroundf(rr * 0.40f * k), fundo);
}

static void icone_config(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo  = d.color565(250, 205, 60);
  const uint16_t escuro = d.color565(50, 42, 10);
  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  // Grande embaixo a esquerda; a media engrena nela para cima-direita (-40
  // graus), a pequena para baixo-direita (+20). Folgas conferidas: nenhuma
  // encosta na borda arredondada, e media e pequena nao se tocam.
  const int   nG = 14, nM = 9, nP = 6;
  const float gx = 40.0f, gy = 58.0f;
  const float t1 = -40.0f, t2 = 20.0f;
  const float rG = eng_raio(nG), rM = eng_raio(nM), rP = eng_raio(nP);
  const float r2g = (float)M_PI / 180.0f;

  const float mx = gx + (rG + rM) * cosf(t1 * r2g), my = gy + (rG + rM) * sinf(t1 * r2g);
  const float sx = gx + (rG + rP) * cosf(t2 * r2g), sy = gy + (rG + rP) * sinf(t2 * r2g);

  const float fG = t1;                              // dente da grande apontando para a media
  const float fM = t1 + 180.0f + 180.0f / nM;       // vao da media no mesmo ponto
  const float pG = 360.0f / nG;
  const float dlt = t2 - (fG + roundf((t2 - fG) / pG) * pG);   // onde o dente da grande caiu
  const float fP = (t2 + 180.0f - dlt * rG / rP) + 180.0f / nP;

  desenhar_engrenagem(d, x, y, lado, gx, gy, nG, fG, escuro, fundo);
  desenhar_engrenagem(d, x, y, lado, mx, my, nM, fM, escuro, fundo);
  desenhar_engrenagem(d, x, y, lado, sx, sy, nP, fP, escuro, fundo);
}


// 'extern' e OBRIGATORIO: em C++ um const no escopo de arquivo tem ligacao
// interna, e o apps.cpp nao enxergaria este simbolo.
// O RITMO DE CADA PAGINA. Era 500 ms em todas: a interface acordava 2x/s com
// Ajustes aberto, mesmo parada na pagina da Tela (que so muda pelo toque, e o
// toque ja pede redesenho). Agora: 0 onde nada muda sozinho, 500 ms so onde
// algo anda depressa (procura de redes, varredura BLE, pendrive em uso,
// sondagem do Grove), 1 s no resto (estados de Wi-Fi/BLE/Mesh, relogio do
// Sobre). Cada pagina continua comparando a assinatura antes de pintar.
static uint32_t periodo_agora(void)
{
  if (sistema_grove_sondando()) { return 500; }
  switch (s_pag) {
    case PAG_TELA:      return 0;
    case PAG_APPS:      return 0;
    case PAG_WIFI:      return tab5_wifi_searching() ? 500 : 1000;
    case PAG_BLUETOOTH: return tab5_ble_scanning() ? 500 : 1000;
    case PAG_USB: {
      tab5_pendrive_estado_t e;
      tab5_pendrive_estado(&e);
      return e.fase != PENDRIVE_PARADO ? 500 : 1000;
    }
    default:            return 1000;
  }
}

extern const app_t APP_CONFIG = {
  .name  = "Ajustes",
  .cor   = TFT_YELLOW,
  .perfil = { .speaker = true },
  .usa_sd = false,
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .ao_teclar = ao_teclar,
  .periodo_ms = 1000,                // o padrao; periodo_agora decide por pagina
  .periodo_agora = periodo_agora,
  .altura_conteudo = 0,   // nao rola inteiro: a lista e as paginas rolam por dentro
  .icone = icone_config,
};
