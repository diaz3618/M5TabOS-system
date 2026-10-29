// M5tabOS - skeleton system with applications for the M5Stack Tab5.
//
// app_main = SETUP. Boots, applies the boot energy profile, creates the
// task of interface and returns. When it returns, FreeRTOS destroys the
// "main" task and returns its stack to the heap - the interface continues running.

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <M5Unified.h>

#include "app.h"
#include "tab5_power.hpp"
#include "settings.h"
#include "tab5_emoji.hpp"
#include "tab5_ui.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_marco - extract of internal RAM
#include "tab5_flashdrive.hpp"

static const char *TAG = "main";


// extern "C" is REQUIRED: the startup of IDF and C and looks for the "app_main" symbol
// crude. Without this, C++ decorates to _Z8app_mainv and the link fails.
extern "C" void app_main(void)
{
    // The of boot profile and the UNION of that apps need to EXIST (IMU, mic,
    // speaker, RTC) - decisions that can only be made before the beginning.
    // Everything that is keyable then stays disabled here and is enabled per app.
    // Adjustments need to come BEFORE begin: the initial brightness of screen already exit
    // of that the user chose, without flashing the default value and correcting it later.
    // Extract of internal RAM (tab5_monitor.hpp): each landmark notes as to
    // previous step consumed. The first is the line of base of heap.
    tab5_memory_marco("boot: app_main");
    settings_init();
    tab5_memory_marco("ajustes (NVS)");

    tab5_profile_t boot = system_profile_of_boot();

    // The preferences of user are valid NOW ON BOOT, not from the first app
    // open. Before, only brightness was imposed here, and the 5V of Grove was born
    // disabled even with the preference set to enabled.
    system_apply_preferences(boot);

    auto cfg = M5.config();
    tab5_power_config(cfg, boot);     // phase 1: before begin
    M5.begin(cfg);                    // rails, MIPI-DSI panel, touch
    tab5_power_apply(boot);         // phase 2: after begin
    tab5_memory_marco("M5.begin (painel, toque)");
    tab5_panel();                     // which of the three screens (tab5_power.hpp) - before changing the clock

    // VOLUME saved, applied JA - before any app touches the first sound
    // (M5.Speaker.tone()/playWav() raises the ES8388 alone, in time, see the
    // big warning in settings.h). Without this, the first sound of a session
    // would come out in the volume of factory of object Speaker, only correcting it later if
    // the user went through Settings.
    M5.Speaker.setVolume(settings_volume());

    // EMOJI, ONCE, FOR THE ENTIRE SYSTEM. It needs to come after of begin
    // (the callback is one method of panel, which only exists from now on) and
    // before any app can draw text - give it the place, right at boot.
    // No app needs to know this exists: and the same drawing of rest of
    // kernel, "those who draw text shouldn't worry about this".
    tab5_emoji_start();
    tab5_memory_marco("emoji");

    // SYSTEM-WIDE DEFAULT FONT. setFont() and GLOBAL STATE of display
    // (not part of TextStyle - see identical warning in tab5_ui.hpp), so
    // set once here, before any app draw, goes for kernel and
    // apps without any need to call setFont() alone.
    //
    // TAB5_FONTE_CORPO lives in tab5_ui.hpp (swap in progress,: a
    // Built-in Font0 became Font4, which did not provide reading comfort and became
    // DejaVu24 - it only changes there, not here). Whoever draws a TITLE uses the
    // Orbitron_Light by top and restores the body afterwards (see tab5_fonte_title
    // / tab5_source_body in tab5_ui.hpp); everything else of system stays in it.
    tab5_fontes_start();   // soft fonts (tab5_ui.hpp) - before first text
    M5.Display.setFont(TAB5_FONTE_CORPO);
    tab5_memory_marco("fontes");

    ESP_LOGI(TAG, "display %dx%d", M5.Display.width(), M5.Display.height());
    tab5_power_log("boot");

    // Right AFTER M5.begin(), which is when the MIPI-DSI panel is initialized:
    // If it is the panel that holds the CPU at maximum, its lock appears here and not
    // before. It is the measurement that replaces our deduction about the stuck clock.
    tab5_power_locks("apos M5.begin");

    // SPLASH ("M5Tab OS" in screen) DOES NOT draw here - moved inside of
    // vTaskInterface (kernel/core.cpp), after tab5_power_dpi_clock(). See the
    // warning la: draw before that point means draw before of
    // panel takes over the final clock, and the clock swap reassembles the DSI and
    // erases whatever is on screen - it was exactly the blink observed in
    // .
    system_start();

    // PENDRIVE MODE: warning and cable watch (connects the pendrive alone on
    // connect to a computer, only with the setting enabled). After the system: she
    // checks the open app and the lock screen.
    tab5_pendrive_servico_start();

    tab5_memory_marco("sistema_iniciar (fim)");
    ESP_LOGI(TAG, "M5tabOS %s iniciado - boot concluido", esp_app_get_description()->version);
}
