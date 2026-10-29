// System WiFi service - NOT an application.
//
// Why service and not app: the status bar needs state for the entire team,
// and one future app (Meshtastic, network printing, NTP) will want the connection already
// of pe. If WiFi died along with the screen of app that turned it on, every app would have
// to reconnect - seconds of waiting with each screen change.
//
// The user will live in settings_wifi() (NVS). This module is the ARM that
// carries out this will; He doesn't decide anything alone.
//
// ------------------------------------------------------------------------
// TWO TASKS, AND THE DIVISION IS NOT ARBITRARY
// ------------------------------------------------------------------------
// The C6 (WLAN_PWR_EN) rail stays on an I2C expander, and in this system the I2C
// has a single owner: the task of interface (see the header of kernel/core.cpp - the
// I2C_Class of M5Unified has no mutex). Therefore:
//
// - tab5_wifi_turn_on()/turn_off() TOCAM I2C -> can only be called of
// within task of interface. They just move the track and let you know.
// - everything else (wait for C6 to boot, SDIO, esp_wifi, DHCP) runs in task
// "wifi" of this module, which does not support any I2C.
//
// Calling tab5_wifi_turn_on() of another task is not of error - of race on I2C, which
// and worse, because one day it appears as a wrong reading of battery.
//
// ------------------------------------------------------------------------
// ECONOMY LEVELS - today the 1st, on purpose
// ------------------------------------------------------------------------
// Combined: start conservatively and measure before tightening. The levels are:
//
// 1 (CURRENT) turn_off = esp_wifi_stop(). The radio of C6 turns off, the SDIO link
// and the stack remains of pe. Reconnecting is instantaneous and never fails.
// 2 + esp_wifi_deinit() + esp_hosted_deinit(). Frees up RAM, but
// reconnecting has to renegotiate the SDIO.
// 3 + cut WLAN_PWR_EN. Zero consumption on the C6. And the only one that turns off
// the BLE together, and the only one where reconnecting can simply fail.
//
// Measure each one with the Energy app before starting of level. The plan and compare
// numbers, don't guess - the same way it was done with frequency.

#pragma once

#include <stdint.h>
#include <stddef.h>

enum tab5_wifi_state_t
{
  TAB5_WIFI_OFF = 0,
  TAB5_WIFI_STARTING,      // turning on C6 and negotiating SDIO
  TAB5_WIFI_CONNECTING,   // searching/authenticating at point of access
  TAB5_WIFI_CONNECTED,    // has IP
  TAB5_WIFI_NO_CREDENTIALS,
  TAB5_WIFI_FAILED,

  // THE LINK WITH C6 EXISTS, BUT IT DOES NOT RESPOND TO RPC.
  //
  // Own status because it is a defect of ANOTHER nature: it is not a wrong password
  // no network is offline, and there's no point trying again. The coprocessor has
  // bad firmware and needs to be rewritten - physically, via the connector
  // reserved of plate.
  //
  // Deserves own name in screen: "failed" would tell the user to check the password
  // for the rest of your life.
  TAB5_WIFI_C6_UNRESPONSIVE
};

// Creates the of service task. DO NOT turn on the radio. Call once, at boot, then
// of settings_init() and of tab5_sd_init().
void tab5_wifi_init(void);

// ONLY of the interface task (changes the I2C expander). Do not block:
// time-consuming work happens in task of service.
void tab5_wifi_turn_on(void);
void tab5_wifi_turn_off(void);

// SO the link with the C6 (esp_hosted) goes up, without turn_on or WiFi or search
// router. For those who use the C6 for another reason - Bluetooth. Connect the trail
// of radio (I2C): call of interface task, such as tab5_wifi_turn_on().
// Asynchronous: tab5_wifi_stack_ready() says when became ready.
void tab5_wifi_ensure_stack(void);

tab5_wifi_state_t tab5_wifi_state(void);

// Has the stack of esp_hosted (and of esp_wifi) already gone up at this boot?
//
// Other than "connected": the SDIO link with the C6 may be offline with WiFi
// on network none. Anyone who wants to talk to the coprocessor - consult version,
// request MAC, send data via channel peer_data - you need this, not of state of
// connection.
bool tab5_wifi_stack_ready(void);
// Warns when a stack (esp_hosted + esp_wifi) has just started - for who
// depends on link with the C6 (the ESP-NOW) to sleep without asking every second.
// Just one function; call of task of Wi-Fi, cannot block.
void tab5_wifi_on_stack_ready(void (*fn)(void));

// The C6 runs OUR firmware (folder c6/) - he responded to the notice via the channel
// peer_data on this boot. Only then there is ESP-NOW (services/tab5_espnow):
// factory firmware does not have this channel.
bool tab5_wifi_c6_is_ours(void);

// SO RADIO MODE - the radio stays of pe WITHOUT a router, for the ESP-NOW to use one
// own channel (the radio only stays on one channel at a time, and is associated with it and of
// router). Without reconnection, and without the dropout that stops the radio away from
// home. false = iteration a connect on the router. Does not block (queue of service).
void tab5_wifi_radio_only_mode(bool turn_on);
bool tab5_wifi_radio_only(void);

// The radio is STARTED (with router, without router, or scanning). False after
// of turning_off the WiFi in Settings or of turning off out of range of router - which stops the
// radio but leave the C6 powered and the ENLACE of pe. And the question of ESP-NOW.
bool tab5_wifi_radio_active(void);

// RADIO ECONOMY. The default is modem sleep: the C6 turns off the receiver between
// beacons of router (great savings, and for regular WiFi it costs nothing).
// Who needs a receiver on ALL THE TIME - the ESP-NOW, which loses the frame
// that arrives during the nap - asks for it with its own bit, and undoes it when not
// need more. The radio only saves without any requests. Does not block.
#define TAB5_AWAKE_ESPNOW  (1u << 0)
// A bit FOR THOSE WHO ASK: the ESP-NOW service reaffirms its service from time to time,
// and with one bit shared it would undo the request of sniffing.
#define TAB5_AWAKE_SNIFFING  (1u << 1)
#define TAB5_AWAKE_TEST   (1u << 2)   // radio of console command
#define TAB5_AWAKE_CSI     (1u << 3)   // service CSI (tab5_csi)
void tab5_wifi_keep_awake(uint32_t who, bool sim);

// Is the SDIO LINK with the C6 working? Weaker than the question above, and of
// purpose: here it is enough for the transport to have negotiated. A C6 with firmware
// can have a broken link WITHOUT having RPC working - and in this state the
// recording of new firmware needs to be able to happen.
bool tab5_wifi_c6_link(void);

// REMOVED (audit of layers): made "s_pilha_de_pe ||
// settings_wifi()" in here - HAL asking one service (settings.h),
// wrong sense of dependency (see docs/WRITING_APPS.md section 8). The only one
// caller wassistema_impor_preferencias() in the kernel, which already has
// settings_wifi() legitimately available - account turned over
// "tab5_wifi_stack_ready() || settings_wifi()" right there, without missing anything.

// Scans and prints the networks visible on the console. Diagnosis: separates "the radio
// does not work" (empty list) of "a network does not appear" (list with other networks,
// without yours). Requires that WiFi has already been on once at this boot - a
// stack needs to be of pe.
void tab5_wifi_scan_console(void);

// Change the network in use without rereading the card. Call inside
// tab5_wifi_save_credentials() right after writing the file - read the file of
// iteration with the link of C6 of pe drops the WiFi (see the comment in
// read_credentials). Also exposed for those who just want to upgrade the RAM without
// touch on card (no callers do this today, but write/update are
// operations distinct enough to not always force the two together).
void tab5_wifi_set_credentials(const char* ssid, const char* password);

// Adds (or updates) the network in wifi.txt, encrypting the password, and updates the
// RAM (tab5_wifi_set_credentials). The other saved networks remain.
// Writes via the system port (tab5_sd_replace): mounts the card alone,
// and with it slow on the computer goes to the diary. Blocks - of worker,
// never of task of interface. password "-" or empty writes network open.
bool tab5_wifi_save_credentials(const char* ssid, const char* password);

// -------------------------------------------------------------------------------
// NETWORKS: several known, search and exchange
// -------------------------------------------------------------------------------
// wifi.txt stores several networks (SSID/password pairs, encrypted passwords). At turn_on,
// the system searches in iteration and connects in known of strongest signal.
#define TAB5_WIFI_MAX_NETWORKS   8
#define TAB5_WIFI_MAX_SEEN  24

typedef struct { char ssid[33]; bool open; } tab5_wifi_network_t;

typedef struct {
  char   ssid[33];
  int8_t rssi;
  uint8_t channel;
  bool   open;
  bool   known;   // is in wifi.txt
} tab5_wifi_seen_t;

// Looks for networks in iteration (asynchronous; connect C6 if necessary - I2C: call
// of task of interface). The exit result in tab5_wifi_get_seen().
void     tab5_wifi_search(void);
bool     tab5_wifi_searching(void);
int      tab5_wifi_seen_count(void);
bool     tab5_wifi_get_seen(int i, tab5_wifi_seen_t* out);   // of stronger signal on weaker
int      tab5_wifi_known_count(void);
bool     tab5_wifi_is_known(const char* ssid);
// The network is stored as number 'i' (0..n_known-1), with the password in text (only RAM;
// empty = network open). Serves those who want to COPY the networks (Mesh brings the of
// Tab5 for Wi-Fi of no). false = index outside of list.
bool     tab5_wifi_saved_network(int i, char* ssid, size_t size_ssid, char* password, size_t size_senha);
// The same cipher of wifi.txt (AES with the switch_number of device), for others
// files of credentials save password without plain text on the card.
// 'hex' needs TAB5_WIFI_CIPHER_HEX bytes.
#define TAB5_WIFI_CIPHER_HEX 200
bool     tab5_wifi_encrypt_text(const char* plaintext, char* hex, size_t hex_size);
bool     tab5_wifi_decrypt_text(const char* hex, char* plaintext, size_t plaintext_size);
uint32_t tab5_wifi_networks_version(void);   // changes with each search or saved/forgotten network

// Connects to an ALREADY KNOWN network (recorded with tab5_wifi_save_credentials).
void     tab5_wifi_connect(const char* ssid);
// Remove the network of wifi.txt (same port and same rules of write as above).
bool     tab5_wifi_forget(const char* ssid);

// Short text for the status bar ("WiFi", "connecting", the SSID...).
const char* tab5_wifi_description(void);

const char* tab5_wifi_ssid(void);
const char* tab5_wifi_ip(void);

// Unavailable signal: no connection, or the coprocessor did not fill the field.
// DO NOT use 0 as "no signal" - zero and one value of RSSI valid in theory and
// would mean signal maximum, the opposite of intended.
#define TAB5_WIFI_RSSI_UNKNOWN  (-127)

// TAB5_WIFI_RSSI_UNKNOWN when there is no connection. Le of cache powered by the of service task:
// esp_wifi_sta_get_ap_info() and a trip to C6 via SDIO, too expensive to
// call every redraw of bar.
int8_t tab5_wifi_rssi(void);

// Path of file of credentials, for the Settings app to show when missing.
#define TAB5_WIFI_FILE "/sdcard/apps/Sistema/wifi.txt"
