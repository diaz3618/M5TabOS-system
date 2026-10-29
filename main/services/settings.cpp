#include "settings.h"

#include <M5Unified.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdio>
#include <cmath>

static const char *TAG = "ajustes";
static const char *NAMESPACE = "sistema";

static uint8_t s_brightness = 80;
static uint8_t s_volume = 128;   // Midscale: neither muted nor full volume on first use.
static bool    s_wifi   = false;
static bool    s_keyboard = false;
static bool    s_grove   = false;
static uint8_t s_screen_timeout_s = 30;   // Previous system default, in seconds.
static uint8_t s_panel_clock_mhz = 60;    // M5GFX factory default.
static int8_t  s_utc_offset_h = 0;   // UTC by default - stored biased by 12 (0..26) to fit nvs_get_u8
static bool    s_ready = false;

// Degrees times 1,000,000 (microdegrees), stored as an integer. This fits in an
// NVS i32 without a blob and gives about 11 cm of precision at the equator,
// more than enough for an address. The factory value 0.0 in the Gulf of Guinea
// is a safe "not configured" sentinel without a separate NVS flag.
static int32_t s_home_lat_ue6 = 0;
static int32_t s_home_lon_ue6 = 0;

static bool s_trail_beep = true;   // Enabled by default; see the note in settings.h.
static bool s_mesh_beep  = true;   // Same default for Mesh.
static uint8_t  s_espnow_channel = 0;
static uint8_t  s_mesh_transport = SETTINGS_MESH_TRANSPORT_SERIAL;
static uint32_t s_mesh_pin = 123456;
static char     s_mesh_address[18] = "";
static bool     s_mesh_pin_on_screen = false;
static uint8_t  s_mesh_position_interval_min = 0;
static bool     s_mesh_internet = true;
static bool     s_mesh_public_network = false;
static bool     s_bluetooth = false;
static uint8_t  s_wallpaper    = SETTINGS_WALLPAPER_RANDOM;
static bool     s_lock_screen = false;   // Lock screen after waking up; disabled by default.
static bool     s_flash_drive = false;   // Auto-open on computer connection; disabled by default.
// Gyroscope scale in parts per million (1,000,000 = 1.0), per axis. Store it
// as an NVS i32, like the home coordinates, without a blob.
static int32_t  s_gyro_scale_ppm[3] = { 1000000, 1000000, 1000000 };
static const char* const GYRO_SCALE_KEY[3] = { "giro_esc_x", "giro_esc_y", "giro_esc_z" };
#define GYRO_SCALE_MIN_PPM  900000
#define GYRO_SCALE_MAX_PPM 1100000

static void write_u8(const char* key, uint8_t value)
{
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u8(h, key, value);
  nvs_commit(h);      // Without commit, the value disappears at the next boot.
  nvs_close(h);
}

// DEFERRED WRITES for values ​​changed by dragging (brightness and volume). The
// Settings slider produces a new value every frame. Each value used to cause
// an nvs_commit in the UI task, writing flash with the cache disabled, pausing
// both colors for several milliseconds, making the drag stutter, and storing
// dozens of unwanted intermediate values. The hardware now changes
// immediately while NVS receives only the final value, one second after the
// last movement, from the esp_timer task.
#define DEFERRED_WRITE_DELAY_US 1000000
static esp_timer_handle_t s_deferred_write_timer = nullptr;
static volatile bool s_brightness_pending = false, s_volume_pending = false;

static void write_deferred_values(void*)
{
  if (s_brightness_pending) { s_brightness_pending = false; write_u8("brilho", s_brightness); }
  if (s_volume_pending) { s_volume_pending = false; write_u8("volume", s_volume); }
}

static void defer_write(void)
{
  if (!s_deferred_write_timer) {
    const esp_timer_create_args_t a = { .callback = write_deferred_values, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "settings_write",
                                        .skip_unhandled_events = true };
    if (esp_timer_create(&a, &s_deferred_write_timer) != ESP_OK) { s_deferred_write_timer = nullptr; write_deferred_values(nullptr); return; }
  }
  esp_timer_stop(s_deferred_write_timer);   // Each new value postpones the deadline.
  esp_timer_start_once(s_deferred_write_timer, DEFERRED_WRITE_DELAY_US);
}

void settings_init(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Worn or obsolete NVS partition. Erasing is safe here because it contains
    // only our settings, with no irreplaceable user data.
    ESP_LOGW(TAG, "NVS precisou ser reinicializada");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS indisponivel (%s) - ajustes so valem nesta sessao",
             esp_err_to_name(err));
    return;
  }
  s_ready = true;

  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v;
    if (nvs_get_u8(h, "brilho", &v) == ESP_OK) { s_brightness = v; }
    if (nvs_get_u8(h, "volume", &v) == ESP_OK) { s_volume = v; }
    if (nvs_get_u8(h, "wifi",   &v) == ESP_OK) { s_wifi = (v != 0); }
    if (nvs_get_u8(h, "teclado",&v) == ESP_OK) { s_keyboard = (v != 0); }
    if (nvs_get_u8(h, "grove",  &v) == ESP_OK) { s_grove = (v != 0); }
    if (nvs_get_u8(h, "tela_s", &v) == ESP_OK) {
      // Clamp on read as well as write: an old or corrupt persisted value must
      // does not create a display that never turns off.
      s_screen_timeout_s = (v < SETTINGS_SCREEN_TIMEOUT_MIN_S) ? SETTINGS_SCREEN_TIMEOUT_MIN_S
               : (v > SETTINGS_SCREEN_TIMEOUT_MAX_S) ? SETTINGS_SCREEN_TIMEOUT_MAX_S : v;
    }
    if (nvs_get_u8(h, "relogio_p", &v) == ESP_OK) {
      s_panel_clock_mhz = (v < SETTINGS_PANEL_CLOCK_MIN_MHZ) ? SETTINGS_PANEL_CLOCK_MIN_MHZ
                       : (v > SETTINGS_PANEL_CLOCK_MAX_MHZ) ? SETTINGS_PANEL_CLOCK_MAX_MHZ : v;
    }
    if (nvs_get_u8(h, "utc_off", &v) == ESP_OK) {
      const int decoded = (int)v - 12;
      s_utc_offset_h = (decoded < SETTINGS_UTC_OFFSET_MIN) ? SETTINGS_UTC_OFFSET_MIN
                     : (decoded > SETTINGS_UTC_OFFSET_MAX) ? SETTINGS_UTC_OFFSET_MAX : (int8_t)decoded;
    }
    int32_t vi;
    if (nvs_get_i32(h, "casa_lat", &vi) == ESP_OK) { s_home_lat_ue6 = vi; }
    if (nvs_get_i32(h, "casa_lon", &vi) == ESP_OK) { s_home_lon_ue6 = vi; }
    for (int axis = 0; axis < 3; ++axis) {
      if (nvs_get_i32(h, GYRO_SCALE_KEY[axis], &vi) == ESP_OK
          && vi >= GYRO_SCALE_MIN_PPM && vi <= GYRO_SCALE_MAX_PPM) { s_gyro_scale_ppm[axis] = vi; }
    }
    if (nvs_get_u8(h, "beep_trilha", &v) == ESP_OK) { s_trail_beep = (v != 0); }
    if (nvs_get_u8(h, "beep_mesh", &v) == ESP_OK) { s_mesh_beep = (v != 0); }
    if (nvs_get_u8(h, "espnow_c", &v) == ESP_OK && v <= 13) { s_espnow_channel = v; }
    if (nvs_get_u8(h, "mesh_via", &v) == ESP_OK && v <= SETTINGS_MESH_TRANSPORT_INTERNET) { s_mesh_transport = v; }
    if (nvs_get_u8(h, "mesh_pos", &v) == ESP_OK) { s_mesh_position_interval_min = v; }
    if (nvs_get_u8(h, "mesh_net", &v) == ESP_OK) { s_mesh_internet = (v != 0); }
    if (nvs_get_u8(h, "mesh_rede", &v) == ESP_OK
     || nvs_get_u8(h, "mesh_eua", &v) == ESP_OK) {   // Second key preserves compatibility with old writes.
      s_mesh_public_network = (v != 0);
    }
    if (nvs_get_u8(h, "bluetooth", &v) == ESP_OK) { s_bluetooth = (v != 0); }
    if (nvs_get_u8(h, "bloqueio", &v) == ESP_OK) { s_lock_screen = (v != 0); }
    if (nvs_get_u8(h, "pendrive", &v) == ESP_OK) { s_flash_drive = (v != 0); }
    if (nvs_get_u8(h, "parede", &v) == ESP_OK && v <= SETTINGS_WALLPAPER_NONE) { s_wallpaper = v; }
    uint32_t vu;
    if (nvs_get_u32(h, "mesh_pin", &vu) == ESP_OK && vu <= 999999) { s_mesh_pin = vu; }
    if (nvs_get_u8(h, "mesh_pin_t", &v) == ESP_OK) { s_mesh_pin_on_screen = (v != 0); }
    size_t size = sizeof(s_mesh_address);
    if (nvs_get_str(h, "mesh_end", s_mesh_address, &size) != ESP_OK) { s_mesh_address[0] = 0; }
    nvs_close(h);
  }
  // An invalid value saved by mistake must not leave the display unusable.
  if (s_brightness < SETTINGS_BRIGHTNESS_MIN) { s_brightness = SETTINGS_BRIGHTNESS_MIN; }

  ESP_LOGI(TAG, "ajustes lidos: brilho %d, WiFi %s", (int)s_brightness, s_wifi ? "ligado" : "desligado");
}

uint8_t settings_brightness(void) { return s_brightness; }

void settings_set_brightness(uint8_t brightness)
{
  if (brightness < SETTINGS_BRIGHTNESS_MIN) { brightness = SETTINGS_BRIGHTNESS_MIN; }
  s_brightness = brightness;

  // Apply immediately: moving a slider without seeing a response is confusing.
  // Use the normal API rather than accessing the panel directly because this
  // is the authoritative brightness restored by tab5_power after sleep.
  M5.Display.setBrightness(s_brightness);

  s_brightness_pending = true;
  defer_write();   // NVS receives only the final value; see DEFERRED_WRITE_DELAY_US.
}

uint8_t settings_volume(void) { return s_volume; }

void settings_set_volume(uint8_t volume)
{
  if (volume < SETTINGS_VOLUME_MIN) { volume = SETTINGS_VOLUME_MIN; }
  if (volume > SETTINGS_VOLUME_MAX) { volume = SETTINGS_VOLUME_MAX; }
  s_volume = volume;

  // No I2C cost here: setVolume() only updates the Speaker object and does not
  // power up the ES8388 (see the note in settings.h). Unlike camera operations,
  // this is safe from any task because the UI-task-only I2C rule does not apply.
  M5.Speaker.setVolume(s_volume);

  s_volume_pending = true;
  defer_write();   // Likewise, only the final value reaches NVS.
}

int8_t settings_utc_offset_hours(void) { return s_utc_offset_h; }

void settings_set_utc_offset_hours(int8_t hours)
{
  if (hours < SETTINGS_UTC_OFFSET_MIN) { hours = SETTINGS_UTC_OFFSET_MIN; }
  if (hours > SETTINGS_UTC_OFFSET_MAX) { hours = SETTINGS_UTC_OFFSET_MAX; }
  s_utc_offset_h = hours;
  write_u8("utc_off", (uint8_t)(hours + 12));
}

bool settings_wifi(void) { return s_wifi; }

void settings_set_wifi(bool enabled)
{
  s_wifi = enabled;
  write_u8("wifi", enabled ? 1 : 0);
}

bool settings_keyboard(void) { return s_keyboard; }

void settings_set_keyboard(bool enabled)
{
  s_keyboard = enabled;
  write_u8("teclado", enabled ? 1 : 0);
  ESP_LOGI(TAG, "teclado: %s", enabled ? "ativado" : "desativado");
}

bool settings_grove(void) { return s_grove; }

uint8_t settings_screen_timeout_s(void) { return s_screen_timeout_s; }

void settings_set_screen_timeout_s(uint8_t seconds)
{
  s_screen_timeout_s = (seconds < SETTINGS_SCREEN_TIMEOUT_MIN_S) ? SETTINGS_SCREEN_TIMEOUT_MIN_S
           : (seconds > SETTINGS_SCREEN_TIMEOUT_MAX_S) ? SETTINGS_SCREEN_TIMEOUT_MAX_S : seconds;
  write_u8("tela_s", s_screen_timeout_s);
}

void settings_set_grove(bool enabled)
{
  s_grove = enabled;
  write_u8("grove", enabled ? 1 : 0);
  ESP_LOGI(TAG, "5V do Grove sempre ligado: %s", enabled ? "sim" : "nao");
}

uint8_t settings_panel_clock_mhz(void) { return s_panel_clock_mhz; }

void settings_set_panel_clock_mhz(uint8_t mhz)
{
  s_panel_clock_mhz = (mhz < SETTINGS_PANEL_CLOCK_MIN_MHZ) ? SETTINGS_PANEL_CLOCK_MIN_MHZ
                   : (mhz > SETTINGS_PANEL_CLOCK_MAX_MHZ) ? SETTINGS_PANEL_CLOCK_MAX_MHZ : mhz;
  write_u8("relogio_p", s_panel_clock_mhz);
  ESP_LOGI(TAG, "relogio do painel = %d MHz", (int)s_panel_clock_mhz);
}

bool settings_home(float* lat, float* lon)
{
  if (s_home_lat_ue6 == 0 && s_home_lon_ue6 == 0) { return false; }
  if (lat) { *lat = (float)s_home_lat_ue6 / 1000000.0f; }
  if (lon) { *lon = (float)s_home_lon_ue6 / 1000000.0f; }
  return true;
}

void settings_set_home(float lat, float lon)
{
  s_home_lat_ue6 = (int32_t)(lat * 1000000.0f);
  s_home_lon_ue6 = (int32_t)(lon * 1000000.0f);
  ESP_LOGI(TAG, "casa gravada: %.6f, %.6f", (double)lat, (double)lon);

  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_i32(h, "casa_lat", s_home_lat_ue6);
  nvs_set_i32(h, "casa_lon", s_home_lon_ue6);
  nvs_commit(h);
  nvs_close(h);
}

bool settings_trail_beep(void) { return s_trail_beep; }

void settings_set_trail_beep(bool play)
{
  s_trail_beep = play;
  write_u8("beep_trilha", play ? 1 : 0);
  ESP_LOGI(TAG, "beep da Trilha: %s", play ? "ligado" : "desligado");
}

bool settings_mesh_beep(void) { return s_mesh_beep; }

void settings_set_mesh_beep(bool play)
{
  s_mesh_beep = play;
  write_u8("beep_mesh", play ? 1 : 0);
  ESP_LOGI(TAG, "beep do Mesh: %s", play ? "ligado" : "desligado");
}

uint8_t settings_espnow_channel(void) { return s_espnow_channel; }

void settings_set_espnow_channel(uint8_t channel)
{
  if (channel > 13) { channel = 0; }
  if (channel == s_espnow_channel) { return; }   // The service reapplies it at boot; avoid needless flash wear.
  s_espnow_channel = channel;
  write_u8("espnow_c", channel);
}

uint8_t settings_mesh_transport(void) { return s_mesh_transport; }

void settings_set_mesh_transport(uint8_t transport)
{
  if (transport > SETTINGS_MESH_TRANSPORT_INTERNET) { transport = SETTINGS_MESH_TRANSPORT_SERIAL; }
  if (transport == s_mesh_transport) { return; }   // NVS has a finite write lifetime.
  s_mesh_transport = transport;
  write_u8("mesh_via", transport);
  ESP_LOGI(TAG, "Mesh vai pelo %s", transport == SETTINGS_MESH_TRANSPORT_BLUETOOTH ? "Bluetooth"
                                  : transport == SETTINGS_MESH_TRANSPORT_INTERNET  ? "internet" : "cabo Grove");
}

uint32_t settings_mesh_pin(void) { return s_mesh_pin; }

void settings_set_mesh_pin(uint32_t pin)
{
  if (pin > 999999) { return; }
  s_mesh_pin = pin;
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u32(h, "mesh_pin", pin);
  nvs_commit(h);
  nvs_close(h);
}

void settings_mesh_ble_address(char out[18]) { memcpy(out, s_mesh_address, sizeof(s_mesh_address)); }

bool settings_mesh_pin_on_screen(void) { return s_mesh_pin_on_screen; }

void settings_set_mesh_pin_on_screen(bool shown)
{
  if (shown == s_mesh_pin_on_screen) { return; }
  s_mesh_pin_on_screen = shown;
  write_u8("mesh_pin_t", shown ? 1 : 0);
}

void settings_set_mesh_ble_address(const char* address)
{
  // Knowledge about the previous node's PIN does not apply to another node.
  if (strcmp(address ? address : "", s_mesh_address) != 0) { settings_set_mesh_pin_on_screen(false); }
  snprintf(s_mesh_address, sizeof(s_mesh_address), "%s", address ? address : "");
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_str(h, "mesh_end", s_mesh_address);
  nvs_commit(h);
  nvs_close(h);
}

bool settings_mesh_internet(void) { return s_mesh_internet; }

void settings_set_mesh_internet(bool enabled)
{
  if (enabled == s_mesh_internet) { return; }
  s_mesh_internet = enabled;
  write_u8("mesh_net", enabled ? 1 : 0);
}

bool settings_mesh_public_network(void) { return s_mesh_public_network; }

void settings_set_mesh_public_network(bool enabled)
{
  if (enabled == s_mesh_public_network) { return; }
  s_mesh_public_network = enabled;
  write_u8("mesh_rede", enabled ? 1 : 0);
}

uint8_t settings_mesh_position_interval_min(void) { return s_mesh_position_interval_min; }

void settings_set_mesh_position_interval_min(uint8_t minutes)
{
  if (minutes == s_mesh_position_interval_min) { return; }
  s_mesh_position_interval_min = minutes;
  write_u8("mesh_pos", minutes);
}

bool settings_bluetooth(void) { return s_bluetooth; }

void settings_set_bluetooth(bool enabled)
{
  s_bluetooth = enabled;
  write_u8("bluetooth", enabled ? 1 : 0);
}

bool settings_lock_screen(void) { return s_lock_screen; }

void settings_set_lock_screen(bool enabled)
{
  s_lock_screen = enabled;
  write_u8("bloqueio", enabled ? 1 : 0);
}

bool settings_flash_drive(void) { return s_flash_drive; }

void settings_set_flash_drive(bool enabled)
{
  s_flash_drive = enabled;
  write_u8("pendrive", enabled ? 1 : 0);
}

uint8_t settings_wallpaper(void) { return s_wallpaper; }

void settings_set_wallpaper(uint8_t option)
{
  if (option > SETTINGS_WALLPAPER_NONE) { option = SETTINGS_WALLPAPER_RANDOM; }
  s_wallpaper = option;
  write_u8("parede", option);
}

float settings_gyro_scale(int axis)
{
  if (axis < 0 || axis > 2) { return 1.0f; }
  return (float)s_gyro_scale_ppm[axis] / 1000000.0f;
}

void settings_set_gyro_scale(int axis, float scale)
{
  if (axis < 0 || axis > 2) { return; }
  int32_t ppm = (int32_t)lroundf(scale * 1000000.0f);
  if (ppm < GYRO_SCALE_MIN_PPM || ppm > GYRO_SCALE_MAX_PPM) { return; }
  s_gyro_scale_ppm[axis] = ppm;
  ESP_LOGI(TAG, "escala do giro, eixo %c: %.4f", "xyz"[axis], (double)scale);

  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_i32(h, GYRO_SCALE_KEY[axis], ppm);
  nvs_commit(h);
  nvs_close(h);
}

// ICON ORDER: see settings.h. Read from NVS on demand, only at boot and from
// the console, without a RAM copy. The shell owns the active order.
void settings_apps_order(char* out, size_t n)
{
  if (!out || n == 0) { return; }
  out[0] = 0;
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t size = n;
  if (nvs_get_str(h, "apps_ordem", out, &size) != ESP_OK) { out[0] = 0; }
  nvs_close(h);
}

void settings_set_apps_order(const char* names)
{
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (names && names[0]) { nvs_set_str(h, "apps_ordem", names); }
  else                   { nvs_erase_key(h, "apps_ordem"); }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "ordem dos apps: %s", (names && names[0]) ? names : "(padrao)");
}

// HIDDEN APPS: see settings.h. Read from NVS with the order; the shell itself
// and applies the active list.
void settings_apps_hidden(char* out, size_t n)
{
  if (!out || n == 0) { return; }
  out[0] = 0;
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t size = n;
  if (nvs_get_str(h, "apps_ocultos", out, &size) != ESP_OK) { out[0] = 0; }
  nvs_close(h);
}

void settings_set_apps_hidden(const char* names)
{
  if (!s_ready) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (names && names[0]) { nvs_set_str(h, "apps_ocultos", names); }
  else                   { nvs_erase_key(h, "apps_ocultos"); }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "apps ocultos: %s", (names && names[0]) ? names : "(nenhum)");
}
