#pragma once

// GAMBIARRA CONTIDA - PONTO UNICO, consolidado.
//
// tab5_screen.cpp (double buffer) e tab5_power.cpp (repouso/desmontagem do
// painel) precisam chegar em membros PROTEGIDOS de lgfx::Panel_DSI que o
// M5GFX nao expoe por getter publico: o handle do painel DPI
// (_disp_panel_handle), o vetor de ponteiros de linha (_lines_buffer), o
// buffer guardado em _config_detail e as dimensoes em _cfg.
// cada arquivo tinha a SUA PROPRIA subclasse fazendo a mesma coisa
// (PainelAberto num, PainelDSIAberto no outro) - duas copias do mesmo truque,
// cada uma expondo um subconjunto diferente dos mesmos campos. O risco disso
// nao e teorico: se o M5GFX reorganizar Panel_DSI, as duas podem quebrar em
// momentos DIFERENTES (uma ainda compila "por acaso" enquanto a outra ja
// parou), escondendo o problema atras de um comportamento parcialmente
// migrado em vez de um erro de compilacao limpo e imediato.
//
// Um ponto so, usado pelos dois arquivos: quebra (em tempo de COMPILACAO, nao
// silenciosamente em runtime - a mesma garantia de antes) atinge os dois ao
// mesmo tempo, do mesmo jeito, e o conserto e feito uma vez so.
//
// Nao instanciamos esta classe nunca - ela so serve de "chave" no
// static_cast, igual as duas que ela substitui.
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
#include "esp_lcd_mipi_dsi.h"

namespace {
  struct PainelDSIInterno : public lgfx::Panel_DSI {
    esp_lcd_panel_handle_t pegar_handle(void)  { return _disp_panel_handle; }
    void      zerar_handle(void)               { _disp_panel_handle = nullptr; }

    uint8_t** pegar_lines(void)                { return _lines_buffer; }
    void      trocar_lines(uint8_t** v)        { _lines_buffer = v; }
    void      zerar_lines(void)                { _lines_buffer = nullptr; }

    void      zerar_buffer(void)               { _config_detail.buffer = nullptr; }

    int       largura(void)                    { return _cfg.panel_width; }
    int       altura(void)                     { return _cfg.panel_height; }
  };

  // Devolve nullptr se o painel atual nao for um Panel_DSI (repouso em
  // andamento, por exemplo) - todo chamador ja trata esse caso.
  inline PainelDSIInterno* tab5_painel_dsi_interno(void)
  {
    auto* p = M5.Display.panel();
    if (p == nullptr) { return nullptr; }
    return static_cast<PainelDSIInterno*>(static_cast<lgfx::Panel_DSI*>(p));
  }
}
