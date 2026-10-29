#pragma once

// CONTAINED WORKAROUND - ONE CONSOLIDATED POINT.
//
// tab5_screen.cpp (double buffering) and tab5_power.cpp (sleep/panel teardown)
// need access to PROTECTED lgfx::Panel_DSI members for which M5GFX exposes no
// public getter: the DPI panel handle (_disp_panel_handle), line-pointer array
// (_lines_buffer), buffer stored in _config_detail, and dimensions in _cfg.
// Each file used to have ITS OWN subclass doing the same thing (PainelAberto in
// one, PanelDSIAberto in the other): two copies of one trick, each exposing a
// different subset of the same fields. The risk is not theoretical. If M5GFX
// rearranges Panel_DSI, the two can break at DIFFERENT times (one may compile
// "by accident" after the other has stopped), hiding the problem behind a
// partial migration instead of one immediate, clean compile error.
//
// One point shared by both files: if it breaks, it does so at COMPILE TIME,
// not silently at runtime. Both users fail together in the same way, and the
// fix is done once.
//
// This class is never instantiated. It exists only as a "key" for static_cast,
// just like the two classes it replaces.
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
#include "esp_lcd_mipi_dsi.h"

namespace {
  struct InternalDSIPanel : public lgfx::Panel_DSI {
    esp_lcd_panel_handle_t get_handle(void)  { return _disp_panel_handle; }
    void      clear_handle(void)               { _disp_panel_handle = nullptr; }

    uint8_t** get_lines(void)                { return _lines_buffer; }
    void      set_lines(uint8_t** v)        { _lines_buffer = v; }
    void      clear_lines(void)                { _lines_buffer = nullptr; }

    void      clear_buffer(void)               { _config_detail.buffer = nullptr; }

    int       width(void)                    { return _cfg.panel_width; }
    int       height(void)                     { return _cfg.panel_height; }
  };

  // Returns nullptr if the current panel is not a Panel_DSI (during sleep, for
  // example). Every caller already handles that case.
  inline InternalDSIPanel* tab5_internal_dsi_panel(void)
  {
    auto* p = M5.Display.panel();
    if (p == nullptr) { return nullptr; }
    return static_cast<InternalDSIPanel*>(static_cast<lgfx::Panel_DSI*>(p));
  }
}
