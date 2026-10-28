// System settings: USER PREFERENCES stored in NVS.
//
// This module keeps two concepts separate. Each app's tab5_profile_t says
// which hardware the app needs enabled: Grove, radio, or IMU. Those are app
// requirements. Brightness is different: it is a preference of the person
// holding the device, and no app has authority to override it. These values
// used to share one structure, so the app won.
//
// The system now overrides the profile's brightness when opening an app. The
// field remains in tab5_profile_t so the profile stays self-contained and
// portable, but this module owns the effective value here.
//
// Settings live in NVS rather than on the SD card so they work without a card
// and can be read during boot before any filesystem is mounted.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Brightness range in M5GFX units. A value from 0 to 255 becomes the duty
// cycle of a 9-bit, 44.1 kHz PWM signal on GPIO22. The minimum is deliberately
// nonzero: with the display completely dark, the user could not see the
// control needed to restore it. Perceived brightness is logarithmic (the
// Settings slider follows that curve), so the floor should be low but not too
// low. At 2 (0.9% duty), the display lit while dragging the slider but went
// dark again after sleep because the backlight driver could not start from
// zero with such a short pulse (see tab5_power_wake_screen). A value of 6 is
// 1.8% duty: still dark, but reliably visible.
#define SETTINGS_BRIGHTNESS_MIN  6
#define SETTINGS_BRIGHTNESS_MAX  255

// Load settings from NVS, initializing NVS if necessary. Call before tasks.
void settings_init(void);

uint8_t settings_brightness(void);

// Apply to the display immediately and save. Out-of-range values are clamped.
void settings_set_brightness(uint8_t brightness);

// Whether the user wants Wi-Fi enabled. The Wi-Fi service acts on this value;
// this module only stores the preference.
bool settings_wifi(void);
void settings_set_wifi(bool enabled);

// Whether the user wants the ExtPort1 keyboard enabled. When disabled, the
// system neither probes for the keyboard nor spends I2C transactions on it.
bool settings_keyboard(void);
void settings_set_keyboard(bool enabled);

// Keep Grove 5V enabled at all times.
//
// Without this, a module that must boot (the Unit C6L runs the full Meshtastic
// stack on an ESP32-C6) restarts whenever the rail is cut and never becomes
// ready to communicate. With this enabled, it remains powered like any other
// peripheral.
//
// The rail consumes power even with nothing connected, so this defaults off.
bool settings_grove(void);
void settings_set_grove(bool enabled);

// Time without a touch before the display turns off, in SECONDS.
//
// This is stored in seconds rather than milliseconds because it fits in an
// NVS uint8_t and matches how people think about the setting. The system does
// the conversion.
//
// The 15-second minimum is intentional: below that, the display turns off in
// the middle of reading and waking it becomes part of normal use. The
// 60-second maximum comes from the other direction: the display is the
// device's largest power consumer, so every extra second costs more than the
// savings available elsewhere.
#define SETTINGS_SCREEN_TIMEOUT_MIN_S  15
#define SETTINGS_SCREEN_TIMEOUT_MAX_S  60

uint8_t settings_screen_timeout_s(void);
void    settings_set_screen_timeout_s(uint8_t seconds);

// PANEL CLOCK (DPI/pixel clock), in MHz.
//
// Previously this could only be changed with `screen clock <mhz>` in the
// console, and the value disappeared at the next boot because the system
// always restored 60 MHz. Lowering the clock reduces the continuous PSRAM
// reads required by the panel (see tab5_power.cpp). This saves power from the
// demand side rather than by reducing supplied brightness. The value is
// persisted and exposed in Settings for users without a serial cable.
//
// This is the same range accepted by tab5_power_dpi_clock() (24..80), clamped
// here as well so an invalid persisted value cannot prevent the panel from
// starting. The ceiling was reduced from 100 to 80 because higher values use
// PSRAM bandwidth without a visible benefit. It also makes the Settings
// screen's 5-MHz steps clamp cleanly at both ends.
#define SETTINGS_PANEL_CLOCK_MIN_MHZ  24
#define SETTINGS_PANEL_CLOCK_MAX_MHZ  80

uint8_t settings_panel_clock_mhz(void);

// SAVE ONLY; of not apply to the panel. tab5_power_dpi_clock() changes the
// hardware clock. The caller (console or Settings app) controls the order of
// applying and saving because only it knows whether the change succeeded.
void    settings_set_panel_clock_mhz(uint8_t mhz);

// Internal speaker volume (ES8388), using M5.Speaker.setVolume()'s native
// 0..255 range. Unlike brightness, zero is a valid muted state; no nonzero
// floor is needed because a silent speaker does not hide its own control.
//
// Applied immediately, like settings_set_brightness(), but with no hardware
// cost while nothing is playing: setVolume() only updates the Speaker object.
// The ES8388 remains off until an app first calls M5.Speaker.tone() or
// playWav(). M5Unified initializes the codec lazily; see the volume warning in
// app_config.cpp. This is therefore safe to apply during boot.
#define SETTINGS_VOLUME_MIN  0
#define SETTINGS_VOLUME_MAX  255

uint8_t settings_volume(void);
void    settings_set_volume(uint8_t volume);

// Time-zone offset in whole hours from UTC, set by the user (default 0/UTC).
// This affects display only (tab5_local_time() in tab5_time.hpp). The RTC,
// synchronization, and logs deliberately remain in UTC.
#define SETTINGS_UTC_OFFSET_MIN  -12
#define SETTINGS_UTC_OFFSET_MAX   14

int8_t settings_utc_offset_hours(void);
void   settings_set_utc_offset_hours(int8_t hours);

// Home reference position used to calculate distance and direction to a
// received location (Mesh, app_mesh.cpp). This belongs to the user rather than
// specifically to Mesh: it is not an app requirement, but a preference that
// can serve any app that needs it in the future.
//
// Returns false while unset. Latitude and longitude both equal to zero are the
// factory value and a safe sentinel for "empty" without another NVS flag.
bool settings_home(float* lat, float* lon);
void settings_set_home(float lat, float lon);

// TRAIL SOUND ALERT: mute only Trail while allowing Mesh to sound. This lives
// here as a user preference, not in notification.h, which stores only runtime
// state (see notif_silenciar()). The default is enabled to preserve existing
// behavior. The control remains in the Trail app for now. Each future source
// should get an analogous pair, such as settings_mesh_beep(), rather than an
// unvalidated generic mechanism (see ARCHITECTURE.md section 1.1).
bool settings_trail_beep(void);
void settings_set_trail_beep(bool play);

// MESH SOUND ALERT: the analogous pair described above, with the same format
// and enabled-by-default behavior.
bool settings_mesh_beep(void);
void settings_set_mesh_beep(bool play);

// ESP-NOW channel. Zero follows the router; 1..13 selects a fixed channel and
// releases the router. This used to be RAM-only, so every restart returned to
// the router and a Cardputer on channel 1 disappeared until the app was opened
// and reconfigured. The ESP-NOW service applies the value; this module stores
// the choice.
uint8_t settings_espnow_channel(void);
void    settings_set_espnow_channel(uint8_t channel);

// Transport used by the Tab5 to communicate with the Meshtastic mesh. Serial
// uses the Grove cable (PROTO/UART); Bluetooth uses BLE through the internal
// C6; Internet uses MQTT over Wi-Fi with no node. The PIN is the Meshtastic BLE
// pairing PIN (123456 by default on devices without a screen).
//
// GROVE HAS PRIORITY: when probing finds a C6L on the cable, the transport
// automatically becomes Serial (kernel/core.cpp). A manual choice then remains
// in effect until the next discovery. Internet can also be selected manually
// and provides a fallback when no node is available (settings_mesh_internet).
#define SETTINGS_MESH_TRANSPORT_SERIAL     0
#define SETTINGS_MESH_TRANSPORT_BLUETOOTH  1
#define SETTINGS_MESH_TRANSPORT_INTERNET   2
uint8_t  settings_mesh_transport(void);

// MESH OVER THE INTERNET WHEN THE NODE IS DISCONNECTED: with Wi-Fi connected
// and no C6L available by cable or Bluetooth, messages arrive through MQTT.
// Enabled by default.
bool     settings_mesh_internet(void);
void     settings_set_mesh_internet(bool enabled);

// Join the public internet network (mqtt.meshtastic.org, LongFast) in parallel
// while Wi-Fi is connected, with separate nodes and conversations. Disabled
// by default because it creates unsolicited internet traffic. This is toggled
// from the Mesh app's nodes page.
bool     settings_mesh_public_network(void);
void     settings_set_mesh_public_network(bool enabled);
void     settings_set_mesh_transport(uint8_t transport);
uint32_t settings_mesh_pin(void);
void     settings_set_mesh_pin(uint32_t pin);

// The selected node displays a new PIN for every pairing, so a saved PIN is
// not valid and the system must always ask. This is learned after the first
// rejection of the saved PIN and resets when another node is selected.
bool     settings_mesh_pin_on_screen(void);
void     settings_set_mesh_pin_on_screen(bool shown);

// BLE address of the selected node ("" means the first Meshtastic found).
void     settings_mesh_ble_address(char out[18]);
void     settings_set_mesh_ble_address(const char* address);

// TAB5 GPS TO MESH: interval in MINUTES at which the Tab5 forwards its Grove
// GPS position. Zero means never, the default, because position is personal
// data and is sent only when the user requests it. With a node connected, the
// position goes only to that node, which broadcasts it at its own cadence and
// precision (see enviar_posicao in tab5_mesh.cpp). Over the internet without a
// node, the Tab5 broadcasts it directly. Edited under Mesh > Configure node >
// Location.
uint8_t settings_mesh_position_interval_min(void);
void    settings_set_mesh_position_interval_min(uint8_t minutes);

// Whether Bluetooth is enabled. Selecting Bluetooth as the Mesh transport
// also enables it (see the Settings app).
bool settings_bluetooth(void);
void settings_set_bluetooth(bool enabled);

// Lock screen: after waking from sleep, show the time and date and require an
// upward swipe. Disabled by default.
bool settings_lock_screen(void);
void settings_set_lock_screen(bool enabled);

// FLASH DRIVE ON CONNECTION: when connected to a computer over USB-C, open
// the Flash Drive app so the SD card becomes a computer disk. Disabled by
// default because mass storage takes the console off the cable and firmware
// flashing should not trigger it automatically. An open app is not
// interrupted; the notification center reports the connection instead. The
// Flash Drive app can always be opened manually.
bool settings_flash_drive(void);
void settings_set_flash_drive(bool enabled);

// Gyroscope scale by BMI270 axis (0=x, 1=y, 2=z). The sensor can ship with a
// sensitivity error of a few percent, which accumulates in Radar heading on
// every turn. Measure this in Radar using full turns against a reference.
// 1.0 means no correction; accepted range is 0.90..1.10.
float settings_gyro_scale(int axis);
void  settings_set_gyro_scale(int axis, float scale);

// Home-screen icon order as comma-separated app NAMES
// ("Mesh,Trilha,Radar"). Names rather than indices keep firmware additions or
// removals from shuffling the rest. Apps absent from the list follow in
// apps.cpp order, and unknown names are ignored. Empty means apps.cpp order.
// The shell interprets this value (sistema_home_*, app.h).
void settings_apps_order(char* out, size_t n);
void settings_set_apps_order(const char* names);   // "" or nullptr = default

// Apps hidden from the home screen as comma-separated NAMES. Unknown names
// are ignored. Empty means all apps are visible. The shell interprets this
// alongside the icon order.
void settings_apps_hidden(char* out, size_t n);
void settings_set_apps_hidden(const char* names);   // "" or nullptr = none

// HOME-SCREEN WALLPAPER: thin abstract lines over black, using the same
// designs as the lock-screen background. RANDOM (the default) chooses one at
// each boot and keeps it until the next; 1..5 selects a fixed design; NONE
// restores the original black background.
#define SETTINGS_WALLPAPER_RANDOM       0
#define SETTINGS_WALLPAPER_STYLE_COUNT  5   // 1..5 = threads, waves, web, rays, circles
#define SETTINGS_WALLPAPER_NONE         6

uint8_t settings_wallpaper(void);
void    settings_set_wallpaper(uint8_t option);
