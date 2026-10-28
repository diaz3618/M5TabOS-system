// APP: ESP-NOW - a janela do servico services/tab5_espnow.
//
// Uma barra fina de informacoes em cima (estado, canal, MAC, quem ja foi
// ouvido) e a conversa embaixo - baloes, toque para escrever, teclado. A
// conversa e o componente do sistema (kernel/conversation): este arquivo so diz
// QUAIS mensagens existem e COMO enviar. O radio, o historico no cartao e o
// aviso com o app fechado sao do servico.
//
// Envio em DIFUSAO: todo aparelho no mesmo canal ouve (o Cardputer com o
// firmware oficial de ESP-NOW, por exemplo). Em difusao nao existe ACK - o
// visto verde diz "saiu pela antena", nao "alguem recebeu".
#include "app.h"
#include "conversation.hpp"
#include "paint.hpp"
#include "tab5_espnow.hpp"
#include "tab5_wifi.hpp"
#include "tab5_ui.hpp"
#include "tab5_screen.hpp"
#include <cstdio>
#include <cstring>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// Na PSRAM: ~1 KB (rascunho, copia dele para o "mudou", teclado) que so a
// tarefa de interface toca, e nao precisa morar na RAM interna com o app fechado.
EXT_RAM_BSS_ATTR static conversa_t s_c;
static pintura_t  s_p;
static char       s_barra_txt[128];
static int        s_barra_estado = -1;

// 88 px, "ESP-NOW" em 3.6 e a pilula em 3.2 (era 56/2.2, depois 72/3.0 -
// pequeno demais para ler a distancia de uso).
static const int BARRA_A = 88;

// O SELETOR DE CANAL, na barra: cada toque passa para o proximo. 0 = o do
// roteador; 1, 6 e 11 sao os canais que nao se sobrepoem em 2,4 GHz - e os
// padroes da maioria dos aparelhos (um ESP32 sozinho fica no 1).
static const uint8_t CANAIS[] = { 0, 1, 6, 11 };
static int s_pilula_x0 = -1, s_pilula_x1 = -1;   // onde ela foi desenhada (x da area)

// Cor estavel por aparelho: o mesmo MAC tem sempre a mesma cor no rotulo, e
// numa mesa com tres Cardputers da para saber quem e quem de relance.
static uint16_t cor_do_mac(const uint8_t mac[6])
{
  static const uint16_t CORES[] = { TFT_SKYBLUE, TFT_ORANGE, TFT_GREENYELLOW, TFT_PINK,
                                    TFT_GOLD, TFT_CYAN, TFT_VIOLET, TFT_SILVER };
  const unsigned h = (unsigned)mac[3] * 31u + (unsigned)mac[4] * 7u + (unsigned)mac[5];
  return CORES[h % (sizeof(CORES) / sizeof(CORES[0]))];
}

// ---- o que o componente de conversa pergunta -------------------------------
static int contar(void*) { return tab5_espnow_n(); }

static bool ler(int i, conversa_msg_t* out, void*)
{
  static espnow_msg_t m, ant;   // fora da stack da interface
  if (!tab5_espnow_msg(i, &m)) { return false; }
  snprintf(out->texto, sizeof(out->texto), "%s", m.texto);
  out->minha  = m.minha;
  out->quando = m.quando;
  out->envio  = m.envio;   // espnow_envio_t e conversa_envio_t tem a mesma ordem
  out->name[0] = 0;
  // Rotulo so quando o remetente muda - uma sequencia do mesmo aparelho nao
  // repete o nome em cada balao.
  if (!m.minha) {
    const bool mesmo = (i > 0) && tab5_espnow_msg(i - 1, &ant) && !ant.minha
                    && memcmp(ant.mac, m.mac, 6) == 0;
    if (!mesmo) {
      snprintf(out->name, sizeof(out->name), "%02X%02X", m.mac[4], m.mac[5]);
      out->cor_nome = cor_do_mac(m.mac);
    }
  }
  // O DIALETO de cada recebida, pequeno embaixo: com Cardputer, StickC e Basic
  // na mesa, diz quem fala o formato do componente esp-now e quem manda texto puro.
  out->meta[0] = 0;
  if (!m.minha) {
    snprintf(out->meta, sizeof(out->meta), "%s", m.espressif ? "esp-now Espressif" : "texto puro");
    out->cor_meta = m.espressif ? TFT_SKYBLUE : TFT_DARKGREY;
  }
  return true;
}

static bool enviar(const char* texto, void*) { return tab5_espnow_enviar(texto); }

// ---- barra de informacoes ---------------------------------------------------
// 0 = sem o firmware nosso no C6, 1 = C6 sem enlace (WiFi nunca ligado neste
// boot), 2 = ligando, 3 = no ar, 4 = radio parado (WiFi desligado em Ajustes,
// ou a desistencia fora do alcance do roteador) - o enlace segue de pe, o radio nao.
static int estado_agora(void)
{
  if (!tab5_espnow_c6_compativel()) {
    return tab5_wifi_c6_link() ? 0 : 1;   // sem enlace ainda nao da para saber o firmware
  }
  if (!tab5_wifi_c6_link()) { return 1; }
  if (!tab5_wifi_radio_active()) { return 4; }
  return tab5_espnow_pronto() ? 3 : 2;
}

static const char* motivo(int est)
{
  switch (est) {
    case 0:  return "rádio C6 de fábrica: ESP-NOW pede atualização (opcional)";
    case 1:  return "ligue o Wi-Fi em Ajustes (o C6 só tem energia com ele)";
    case 2:  return "ligando o ESP-NOW...";
    case 4:  return "rádio parado - ligue o Wi-Fi ou fixe um canal em Ajustes";
    default: return "";
  }
}

static void desenhar_barra(lgfx::LovyanGFX& d, const area_t& a, int est)
{
  const uint8_t fixo = tab5_espnow_canal_fixo();
  char canal[32];
  // Fixo: o canal que o C6 DISSE que esta (o servico confere) - se ele
  // recusou a troca, a pilula mostra o pedido e o real, em vez de mentir.
  const uint8_t real = tab5_espnow_canal();
  // Em pe a barra tem 720 px: a pilula vai curta. "fixo" x "roteador" continua
  // dito pela COR dela (laranja = fixo), so sem a palavra.
  const bool pe = a.largura < a.altura;
  if (fixo && est == 3 && real && real != fixo) {
    if (pe) { snprintf(canal, sizeof(canal), "ch %u? (%u)", (unsigned)real, (unsigned)fixo); }
    else    { snprintf(canal, sizeof(canal), "canal %u?  fixo %u", (unsigned)real, (unsigned)fixo); }
  } else if (fixo) { snprintf(canal, sizeof(canal), pe ? "ch %u" : "canal %u  fixo", (unsigned)fixo); }
  else if (est == 3) { snprintf(canal, sizeof(canal), pe ? "ch %u" : "canal %u  roteador", (unsigned)tab5_espnow_canal()); }
  else { snprintf(canal, sizeof(canal), pe ? "ch auto" : "canal: roteador"); }

  char txt[80];
  if (est == 3) {
    uint8_t mac[6];
    tab5_espnow_meu_mac(mac);
    const uint32_t ms = tab5_espnow_ms_desde_rx();
    char ultimo[24];
    if (ms == UINT32_MAX)  { snprintf(ultimo, sizeof(ultimo), "nada ouvido"); }
    else if (ms < 60000)   { snprintf(ultimo, sizeof(ultimo), "último %lus", (unsigned long)(ms / 1000)); }
    else                   { snprintf(ultimo, sizeof(ultimo), "último %lumin", (unsigned long)(ms / 60000)); }
    // EM PE NAO CABE O MAC. Depois do titulo e da pilula sobram ~150 px, e
    // a linha inteira encolhida ate o minimo ainda passava da borda. O MAC e
    // o dado que menos muda (e o do proprio Tab5): sai ele, fica o que diz
    // se ha alguem por perto.
    if (pe) {
      snprintf(txt, sizeof(txt), "%d ouvido(s)  %s", tab5_espnow_vistos(), ultimo);
    } else {
      snprintf(txt, sizeof(txt), "%02X:%02X:%02X:%02X:%02X:%02X   %d ouvido(s)   %s",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tab5_espnow_vistos(), ultimo);
    }
  } else {
    snprintf(txt, sizeof(txt), "%s", est == 2 ? "ligando..." : est == 4 ? "rádio parado" : "fora do ar");
  }

  char tudo[128];
  snprintf(tudo, sizeof(tudo), "%s|%s", canal, txt);
  const bool m1 = pintura_mudou_i(&s_p, &s_barra_estado, est);
  const bool m2 = pintura_mudou_s(&s_p, s_barra_txt, sizeof(s_barra_txt), tudo);
  if (!m1 && !m2) { return; }

  const uint16_t FUNDO = d.color565(13, 17, 23);
  tab5_screen_clear(a.x, a.y, a.largura, BARRA_A, FUNDO);
  const int cy = a.y + BARRA_A / 2;
  const uint16_t cor = (est == 3) ? TFT_GREENYELLOW : (est == 2) ? TFT_ORANGE : TFT_DARKGREY;
  d.fillCircle(a.x + 34, cy, 15, cor);

  d.setTextDatum(middle_left);
  tab5_texto(d, 3.6f);
  d.setTextColor(TFT_WHITE, FUNDO);
  d.drawString("ESP-NOW", a.x + 64, cy);
  int x = a.x + 64 + (int)d.textWidth("ESP-NOW") + 28;

  // Pilula do canal: e um botao (toque troca), entao tem cara de botao.
  tab5_texto(d, 3.2f);
  const int pl = (int)d.textWidth(canal) + 44;
  const int pa = BARRA_A - 22;
  const uint16_t cor_p = fixo ? d.color565(120, 70, 10) : d.color565(38, 60, 90);
  d.fillRoundRect(x, cy - pa / 2, pl, pa, pa / 2, cor_p);
  d.setTextColor(TFT_WHITE, cor_p);
  d.drawString(canal, x + 22, cy);
  s_pilula_x0 = x - a.x;
  s_pilula_x1 = x - a.x + pl;
  x += pl + 24;

  // O resto (MAC, ouvidos, ultimo) no MAIOR tamanho que caiba no que sobrou -
  // medido, nao chutado: em retrato ou com MAC comprido, encolhe sozinho em
  // vez de passar da borda.
  const int sobra = a.x + a.largura - 16 - x;
  float tam = 3.2f;
  tab5_texto(d, tam);
  while (tam > 1.8f && (int)d.textWidth(txt) > sobra) {
    tam -= 0.2f;
    tab5_texto(d, tam);
  }
  // No menor tamanho e ainda sem caber: corta no fim, por caractere (UTF-8),
  // em vez de passar da borda da tela.
  tab5_cortar_ao_caber(d, txt, sobra);
  d.setTextColor(TFT_LIGHTGREY, FUNDO);
  d.drawString(txt, x, cy);
  d.drawFastHLine(a.x, a.y + BARRA_A - 1, a.largura, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

// ---- icone da tela inicial ------------------------------------------------
// Na linha da marca da Espressif (dona do ESP-NOW, que nao tem logotipo
// proprio - a pagina oficial usa a marca da empresa): o vermelho dela, e em
// branco o ponto de origem com tres ondas abrindo para cima e para a direita -
// o sinal saindo de um aparelho, sem roteador no meio. Tudo proporcional ao
// lado: o mesmo desenho serve em qualquer tamanho de grade.
static void marca_espnow(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t vermelho = d.color565(231, 53, 44);   // o vermelho da Espressif
  d.fillRoundRect(x, y, lado, lado, lado / 6, vermelho);

  const int cx = x + (lado * 28) / 100;
  const int cy = y + (lado * 72) / 100;
  int grosso = (lado * 7) / 100;
  if (grosso < 3) { grosso = 3; }

  d.fillCircle(cx, cy, (lado * 8) / 100, TFT_WHITE);
  // 270..360 graus = o quadrante de cima-direita (y cresce para baixo).
  static const int RAIO[3] = { 24, 40, 56 };   // % do lado, ate a borda de fora da onda
  for (int k = 0; k < 3; ++k) {
    const int r1 = (lado * RAIO[k]) / 100;
    d.fillArc(cx, cy, r1 - grosso, r1, 270, 360, TFT_WHITE);
  }
}

// ---- ciclo de vida ---------------------------------------------------------
static bool ao_abrir(void)
{
  s_barra_estado = -1;
  s_barra_txt[0] = 0;
  conversa_abrir(&s_c, &s_p, contar, ler, enviar, nullptr);
  tab5_espnow_marcar_lidas();
  tab5_espnow_janela(true);    // conversa na frente: o servico pede o receptor ligado
  return true;
}

static void ao_fechar(void) { tab5_espnow_janela(false); conversa_fechar(&s_c); }

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);
  const int est = estado_agora();
  desenhar_barra(d, a, est);
  const area_t r = { a.x, a.y + BARRA_A, a.largura, a.altura - BARRA_A };
  conversa_desenhar(&s_c, d, r, pintura_completa(&s_p), est == 3, motivo(est));
}

static void ao_aviso(uint32_t)
{
  // Com a janela aberta, o que chega ja nasce lido.
  tab5_espnow_marcar_lidas();
  conversa_mudou(&s_c);
}

static void ao_tocar(int x, int y)
{
  // A pilula do canal passa ao proximo: roteador -> 1 -> 6 -> 11 -> roteador.
  if (y < BARRA_A && s_pilula_x0 >= 0 && x >= s_pilula_x0 && x < s_pilula_x1) {
    const uint8_t atual = tab5_espnow_canal_fixo();
    int k = 0;
    while (k < (int)sizeof(CANAIS) && CANAIS[k] != atual) { ++k; }
    tab5_espnow_definir_canal(CANAIS[(k + 1) % (int)sizeof(CANAIS)]);
    sistema_pedir_redesenho();
    return;
  }
  if (y < BARRA_A) { return; }
  conversa_tocar(&s_c, x, y);
}
static void ao_tocar_nivel(int x, int y, bool p)       { conversa_tocar_nivel(&s_c, x, y, p); }
static void ao_teclar(const char* t, uint8_t c, uint8_t m) { conversa_teclar(&s_c, t, c, m); }

extern const app_t APP_ESPNOW = {
  .name  = "ESP-NOW",
  .cor   = TFT_CYAN,
  // O radio e do servico de WiFi (o ajuste do sistema mantem). O IMU e para
  // a tela seguir o aparelho.
  .perfil = { .imu = true },
  .usa_sd = false,          // o historico e do servico
  // Em pe ou deitado conforme o aparelho; com o teclado encaixado, deitado
  // (igual ao Mesh). A conversa e o componente do sistema e ja se mede pela
  // largura que chega - so a barra de cima precisa saber do retrato.
  .orientacao = ORI_SEGUE_IMU,
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,   // tecla acende sob o dedo; rolagem
  .ao_teclar = ao_teclar,             // teclado fisico do ExtPort1
  .periodo_ms = 1000,                 // o "ultimo ha Ns" da barra
  .avisos = AVISO_ESPNOW,
  .ao_aviso = ao_aviso,
  .icone = marca_espnow,
};
