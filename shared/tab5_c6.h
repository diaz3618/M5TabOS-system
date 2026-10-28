// CONTRACT between the P4 (M5tabOS) and C6 firmware (the c6/ directory).
//
// It lives in shared/ rather than main/ for a concrete reason: these are TWO
// IDF projects for TWO chips, compiled separately. If each side has its own
// copy of the numbers, they will eventually disagree and the compiler will
// not warn us—as already happened when the grace period was 90 on one side
// and 120 on the other. Both sides include this EXACT file.
//
// This file exists so the two sides do not drift apart. The numbers are agreed
// upon and travel raw over esp_hosted's peer_data channel; there is no
// negotiation. If one side changes by itself, the other sees only an unknown
// message and silence.
//
// Payloads are always raw—no protobuf. The channel already supplies
// (msg_id, bytes, length), and serializing on top of that would add pointless
// work when both endpoints are ours.

#pragma once

#include <stdint.h>

// P4 -> C6
#define C6_MSG_HOST_VIVO    1   // no body: "I am listening"
#define C6_MSG_HOST_ENVIAR  2   // 6B destination MAC + text
#define C6_MSG_HOST_CANAL   3   // 1B WiFi channel
#define C6_MSG_HOST_ESPNOW  4   // no body: "WiFi is up; ESP-NOW may be enabled"
#define C6_MSG_HOST_FAREJO  5   // c6_farejo_cfg_t: enable/renew/disable promiscuous mode
#define C6_MSG_HOST_CRU     6   // 6B destination MAC + bytes: one raw ESP-NOW packet
#define C6_MSG_HOST_AJUSTE  7   // 1B key (C6_AJ_*) + value
#define C6_MSG_HOST_RADIO   8   // 1B subcommand (C6_RX_*) + config; the 8th and FINAL handler

// C6 -> P4
#define C6_MSG_PRONTO      10   // 6B own MAC + 1B channel
#define C6_MSG_RECEBIDO    11   // 6B source MAC + 1B flags (bit0 broadcast, bit1 Espressif format) + text
#define C6_MSG_ESTADO      12   // 6B destination MAC + 1B success flag
#define C6_MSG_FAREJO      13   // c6_farejo_lote_t + n x c6_farejo_reg_t
#define C6_MSG_RECEBIDO_CRU 14  // 6B source MAC + 1B RSSI + 1B channel + 1B flags (bit0 broadcast) + raw bytes
#define C6_MSG_INFO        15   // c6_info_t
#define C6_MSG_CSI         16   // c6_csi_cab_t + raw CSI bytes
#define C6_MSG_FTM         17   // c6_ftm_rep_t
#define C6_MSG_154         18   // c6_154_cab_t + raw 802.15.4 frame bytes

// HOW MANY msg_ids EACH SIDE LISTENS FOR. The C6 peer_data has a FIXED table
// of 8 callbacks (EH_CP_FEAT_PEER_DATA_MAX_HANDLERS); registration number 9
// fails silently and that message is ignored. C6 currently listens for 8
// (1..8): the table is full. A new P4-to-C6 command must be a KEY within
// C6_MSG_HOST_AJUSTE, not a new msg_id. The C6-to-P4 direction has no such
// limit (the host grows its table).

// CONTRACT VERSION. It is carried in PRONTO (byte 7) and INFO. Older C6
// firmware sends PRONTO with only 7 bytes; readers treat "no byte 7" as v1.
//   1 - ESP-NOW (text, two dialects) and channel
//   2 - + promiscuous sniffer, raw ESP-NOW, INFO
//   3 - + CSI, FTM, 802.15.4 (each enabled on request from the P4)
#define C6_PROTO_VERSAO    3
#define C6_CAP_ESPNOW      0x01
#define C6_CAP_FAREJO      0x02
#define C6_CAP_CRU         0x04
#define C6_CAP_INFO        0x08
// These three bits are set only when the feature was COMPILED into the C6
// (depending on c6/'s sdkconfig). Firmware flashed without one leaves its bit
// clear, and the P4 hides that feature—allowing P4-side evolution without
// reopening the device.
#define C6_CAP_CSI         0x10
#define C6_CAP_FTM         0x20
#define C6_CAP_154         0x40
// PRONTO v2: 6B MAC + 1B channel + 1B C6_PROTO_VERSAO + 1B C6_CAP_*

// --- C6_MSG_HOST_AJUSTE: chaves --------------------------------------------
#define C6_AJ_INFO         1    // no value: respond with C6_MSG_INFO
#define C6_AJ_CRU          2    // 1B: 1 = also forward every received ESP-NOW packet as C6_MSG_RECEBIDO_CRU

// --- C6_MSG_HOST_RADIO: sub-comandos ---------------------------------------
// The radio extras are each enabled or disabled here. They follow the SAME
// rule as the sniffer: the receiver stays enabled while the mode is active,
// and each one DISABLES ITSELF if the P4 does not resend its config within
// C6_RADIO_VALIDADE_MS (a closed app must not leave the radio consuming power).
// FTM is the exception: it is a one-shot operation with no renewal.
#define C6_RX_CSI          1    // c6_csi_cfg_t
#define C6_RX_FTM          2    // c6_ftm_cfg_t (disparo unico)
#define C6_RX_154          3    // c6_154_cfg_t
#define C6_RADIO_VALIDADE_MS 10000

// --- SNIFFER (promiscuous mode) ----------------------------------------------
// esp_hosted does NOT forward promiscuous mode (the IDs exist in rpc_v2.proto,
// but neither side implements them). It runs HERE on the C6, and the P4 gets
// only a SUMMARY: the C6 aggregates by MAC for `lote_ms` and sends a batch. A
// busy channel exceeds a thousand frames per second; sending every frame over
// SDIO would mean a thousand interrupts, allocations, and P4 events per second.
// In batches there are four.
//
// VALIDITY: the sniffer DISABLES ITSELF if the P4 does not resend its
// configuration within C6_FAREJO_VALIDADE_MS. Promiscuous mode keeps the
// receiver enabled continuously (no modem sleep); a closed or crashed app
// must not leave the radio consuming power. Resending the SAME configuration
// only renews it; it does not restart channel hopping.
//
// CHANNELS: `canais` is a mask (bit N = channel N). Zero means stay on the
// current channel. With multiple bits, the C6 hops every `permanencia_ms`, but
// ONLY when free of the router (tab5_wifi_radio_only_mode): while associated,
// changing channels would drop WiFi, so the C6 stays on the router's channel
// and ignores the mask. When disabled, it returns to the channel it was on.
#define C6_FAREJO_VALIDADE_MS 10000
#define C6_FAREJO_LOTE_MAX    48     // records per message (48 x 12 + 6 = 582 B)

#define C6_FJ_MGMT    0x01   // filter: management frames (beacon, probe...)
#define C6_FJ_DADOS   0x02   // filter: data frames

typedef struct __attribute__((packed)) {
  uint8_t  liga;             // 0 = disable immediately
  uint8_t  filtro;           // C6_FJ_*; 0 = both
  uint16_t canais;           // mask; 0 = current channel
  uint16_t permanencia_ms;   // per channel while hopping; 0 = 200
  uint16_t lote_ms;          // batch period; 0 = 250 (minimum 50)
  int8_t   rssi_min;         // discard weaker signals; 0 = accept all
  uint8_t  reserva;
  uint8_t  alvo[6];          // only this MAC; all zeroes = all
} c6_farejo_cfg_t;           // 16 B

// What each MAC did during the period (bits in c6_farejo_reg_t.tipos)
#define C6_FT_BEACON   0x01  // announced a network (beacon / probe response): an AP
#define C6_FT_PROCURA  0x02  // probe request: device searching for a network (phone!)
#define C6_FT_DADOS    0x04  // data frame
#define C6_FT_PARA_AP  0x08  // ToDS data: a STATION talking to the AP
#define C6_FT_DO_AP    0x10  // FromDS data: the AP talking
#define C6_FT_GERENCIA 0x20  // other management traffic (auth, assoc, deauth, action)

typedef struct __attribute__((packed)) {
  uint8_t  channel;            // radio channel when the batch closed
  uint8_t  n;                // number of following records
  uint16_t perdidos;         // MACs that did not fit in the period's table
  uint16_t frames;          // frames seen during the period (saturates at 65535)
} c6_farejo_lote_t;          // 6 B

typedef struct __attribute__((packed)) {
  uint8_t  mac[6];           // transmissor (addr2)
  int8_t   rssi_ult;         // latest
  int8_t   rssi_max;
  int8_t   rssi_med;         // period average
  uint8_t  n;                // frames during the period (saturates at 255)
  uint8_t  tipos;            // C6_FT_*
  uint8_t  channel;            // channel where it was heard
} c6_farejo_reg_t;           // 12 B

typedef struct __attribute__((packed)) {
  uint8_t  proto;            // C6_PROTO_VERSAO
  uint8_t  caps;             // C6_CAP_*
  uint8_t  channel;
  uint8_t  farejando;        // 1 = promiscuous mode enabled now
  uint32_t heap_livre;
  uint32_t heap_min;
  uint32_t no_ar_s;          // since C6 boot
  uint32_t frames;          // frames sniffed since boot
  // Diagnostics for radio extras (only firmware that provides them; the P4
  // accepts a short INFO from older C6 firmware and leaves these zeroed).
  int16_t  csi_err;          // first non-OK esp_err_t when enabling CSI (0 = all good)
  int16_t  z154_err;         // same for 802.15.4
  uint32_t csi_cb;           // times the driver called the CSI callback
  uint32_t csi_env;          // CSI records delivered to the P4 queue
  uint32_t z154_rx;          // 802.15.4 frames received by the radio (before rate limiting)
  int16_t  csi_err2;         // result of the LAST CSI enable attempt (0 = enabled)
  uint8_t  csi_etapa;        // where it stopped: 1 config, 2 callback, 3 enable
  uint8_t  csi_modo;         // 1 = CSI enabled now, 0 = disabled
  // (ESP_FAIL in csi_err means C6 WiFi started with csi_enable set to 0. The
  // P4 supplies this field in esp_wifi_init; see main/hal/tab5_wifi.cpp.)
  uint32_t saida_perdas;     // messages dropped because output was full (the P4 did not consume them in time)
  uint8_t  z154_estado;      // 0 never enabled, 1 listening, 2 sleeping (driver enabled, RF released)
  uint8_t  chip_rev;         // 0x80 | major<<4 | minor (0 = firmware does not report it)
  uint8_t  flags;            // C6_INFO_F_* (c6-4 firmware onward; previously reserved = 0)
  uint8_t  reserva_info;
} c6_info_t;                 // 48 B (c6-3 firmware; P4 accepts shorter versions)
#define C6_INFO_V1_TAM 20
#define C6_INFO_F_COREDUMP 0x01   // crash dump is stored in the C6 coredump partition
#define C6_INFO_F_EM_PROVA 0x02   // new OTA image not yet confirmed by VIVO    // INFO size for firmware without diagnostics

// --- CSI (Channel State Information) ------------------------------------------
// The channel state for each received frame: subcarrier amplitude/phase. This
// provides "absurd precision" for presence and movement—the signal changes
// when something between the transmitter and C6 moves, even without holding
// the device. The C6 delivers raw CSI; the P4 performs the calculations
// (variance, correlation).
//
// FLOOD: a busy AP generates a lot of CSI. `min_intervalo_ms` drops frames that
// arrive too quickly on the C6—one per interval is enough for movement. The
// MAC filter (target only) removes the rest.
typedef struct __attribute__((packed)) {
  uint8_t  liga;
  uint8_t  so_alvo;          // 1 = target MAC only
  uint8_t  alvo[6];
  uint16_t min_intervalo_ms; // minimum between sent records; 0 = 100
  int8_t   rssi_min;         // discard weaker signals; 0 = all
  uint8_t  reserva;
} c6_csi_cfg_t;              // 12 B

// WHICH FRAMES GENERATE CSI. CSI is measured in the OFDM training fields of
// the preamble (L-LTF, HT-LTF, HE-LTF). A 2.4 GHz beacon usually uses 1 Mbps
// (11b, DSSS), which has none of these fields and therefore produces no CSI.
// Without promiscuous mode, C6 sees only frames addressed to it—almost
// nothing while WiFi is idle. For a constant stream, the reader must generate
// traffic (the 'CSI radio' pings the router, like Espressif's esp-csi); to get
// CSI from other devices, enable the sniffer.
//
// The C6 driver delivers up to 1023 B (rx_channel_estimate_len is 10 bits):
// L-LTF 128 B, HT20 up to 256, HT40 and HE20 more. The limit below covers the
// maximum, and the TRUNCATED bit reports if it is ever exceeded.
#define C6_CSI_MAX 1024
typedef struct __attribute__((packed)) {
  uint8_t  mac[6];           // source MAC
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  sec_canal;
  uint8_t  flags;            // C6_CSI_F_* and frame format (C6_CSI_FORMATO)
  uint16_t len;              // following CSI bytes (<= C6_CSI_MAX)
  uint32_t t_us;             // C6 reception timestamp
} c6_csi_cab_t;             // 16 B
#define C6_CSI_F_HT40      0x01   // secondary channel present (40 MHz)
#define C6_CSI_F_W0_RUIM   0x02   // first 4 bytes are invalid (hardware limit: first_word_invalid)
#define C6_CSI_F_CORTADO   0x04   // driver delivered more than C6_CSI_MAX
// Frame format, bits 4..7 (driver's cur_bb_format): 0 11b, 1 11g,
// 2 HT, 3 VHT, 4 HE SU, 5 HE MU, 6 HE ER SU, 7 HE TB.
#define C6_CSI_FORMATO(f)  ((uint8_t)((f) >> 4))

// --- FTM (Fine Timing Measurement, 802.11mc) ---------------------------------
// Distance by time of flight to an AP that supports FTM (responder). One-shot:
// the P4 requests it, the C6 runs a session, and replies with the report.
typedef struct __attribute__((packed)) {
  uint8_t  peer[6];          // responding AP
  uint8_t  channel;
  uint8_t  frm_count;        // frames in burst: 0(auto),8,16,24,32,64
  uint16_t burst_period;     // 100 ms units; 0 = auto
} c6_ftm_cfg_t;             // 10 B

typedef struct __attribute__((packed)) {
  uint8_t  peer[6];
  uint8_t  status;           // 0 = success
  uint8_t  num;              // frames used
  uint32_t rtt_est_ns;       // estimated round-trip time of flight
  uint32_t dist_est_cm;      // controller-estimated distance
  uint32_t rtt_raw_ns;       // with status 0xFF: esp_err_t from initiate_session
} c6_ftm_rep_t;             // 20 B
// status: 0..6 = driver's wifi_ftm_status_t (0 success, 1 unsupported by the
// other side, 2 rejected, 3 no response, 4 failure, 5 invalid measurement, 6
// ended). 0xFF = C6 could not even start; the reason is in rtt_raw_ns.
#define C6_FTM_NAO_INICIOU 0xFF

// --- 802.15.4 (the C6's other radio: Thread / Zigbee / raw) -------------------
// C6 has an IEEE 802.15.4 radio in addition to WiFi/BT. In raw receive mode it
// listens to Thread and Zigbee mesh traffic on the requested channel (11..26).
// It shares arbitration with WiFi through the BLE coexistence mechanism (C6
// enables esp_coex_wifi_i154_enable). C6 only forwards raw frames; decoding is
// the P4's job.
typedef struct __attribute__((packed)) {
  uint8_t  liga;
  uint8_t  channel;            // 11..26
  uint8_t  promiscuo;        // 1 = no address filter (hear the whole mesh); 0 = only
                             // frames for the C6's PAN/address, which it has none of: almost nothing
  uint8_t  reserva;
  uint16_t min_intervalo_ms; // throttle forwarding; 0 = unlimited
  uint16_t reserva2;
} c6_154_cfg_t;             // 8 B

#define C6_154_MAX 127       // maximum 802.15.4 frame size
typedef struct __attribute__((packed)) {
  uint8_t  channel;
  int8_t   rssi;
  uint8_t  lqi;              // link quality (0..255)
  uint8_t  len;              // following frame bytes (<= C6_154_MAX)
  uint32_t t_us;
} c6_154_cab_t;             // 8 B

// ESP-NOW v1.0 limit, used by ESP32 and ESP32-S3 devices (Cardputer, Basic,
// StickC, M5Paper). v2.0 goes to 1470, but only when both sides support it;
// these devices do not.
#define C6_TEXTO_MAX      250

// How long C6 waits for the P4's "I am listening" after booting new firmware.
// Without that acknowledgment in time, it points the bootloader back to the
// previous image and reboots.
//
// The timing: C6 reboots 5 s after `end`; the P4 reboots next and takes about
// 15–25 s for the esp_hosted stack to come up. 120 s allows for a slow boot
// without leaving the user staring at a silent device for too long.
#define C6_PRAZO_HOST_S   120
