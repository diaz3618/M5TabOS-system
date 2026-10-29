# The Tab5 Radio: ESP32-C6

The ESP32-P4 at the heart of the Tab5 has no built-in radio. Wi-Fi, Bluetooth
LE, and the 802.15.4 transceiver all live on the companion **ESP32-C6**, wired
to the P4 over SDIO and communicating through **esp_hosted** (version 3.0.9 on
both sides). This document explains how M5tabOS uses the C6, which chip controls
which settings, and the non-obvious pitfalls that cost us hours of hardware
debugging — complete with proof from the ESP-IDF and esp_hosted sources or
disassembled driver binaries.

Verified against: ESP-IDF 5.5.5, esp_hosted 3.0.9, C6 firmware `m5tabos-c6-3`
(protocol contract `C6_PROTO_VERSAO 3`).

## What runs where

| Feature | Where it runs | How the P4 talks to it |
|---|---|---|
| Wi-Fi STA (association, IP, sockets) | Wi-Fi driver on the C6 | esp_hosted RPC (`esp_wifi_*` calls on the P4 become remote procedure calls) |
| Bluetooth LE | Controller on the C6, NimBLE host stack on the P4 | HCI over VHCI |
| ESP-NOW, promiscuous sniffer, CSI, FTM, 802.15.4 | Custom C6 firmware (`c6/`) | esp_hosted custom `peer_data` channel; wire contract in `shared/tab5_c6.h` |

esp_hosted does not forward ESP-NOW, promiscuous mode, or Wi-Fi CSI over RPC:
the message IDs exist in `rpc_v2.proto`, but neither side implements the
handlers. Consequently, those features run **locally on the C6**, while the P4
simply enables them, disables them, and consumes the resulting data streams.

## Golden rule: the C6's Wi-Fi init config comes from the P4

`esp_wifi_init()` on the C6 is not called by our `app_main`; it is called by
the esp_hosted RPC handler (`req_wifi_init` in
`coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_wifi.c`),
using the `wifi_init_config_t` **sent by the P4**. The helper
`get_merged_init_config` states this policy explicitly in its own comment —
*"always use value from host"* — for nearly every field:

`static_rx_buf_num`, `dynamic_rx_buf_num`, `tx_buf_type`, `static_tx_buf_num`,
`dynamic_tx_buf_num`, `rx_mgmt_buf_*`, **`csi_enable`**, `ampdu_*`, `amsdu_tx`,
`nvs_enable`, `rx_ba_win`, `beacon_max_len`, `mgmt_sbuf_num`,
`sta_disconnected_pm`, `espnow_max_encrypt_num`...

What this means in practice: **the C6's own `sdkconfig` does not control these
settings.** Setting `CONFIG_ESP_WIFI_CSI_ENABLED=y` on the C6 only changes the
C6's local `WIFI_INIT_CONFIG_DEFAULT`, which the RPC handler promptly overwrites
with whatever arrived from the P4. The settings that actually take effect are
the `CONFIG_WIFI_RMT_*` options in the P4's `sdkconfig`, or the fields explicitly
populated in the struct before calling `esp_wifi_init` on the P4
(`main/hal/tab5_wifi.cpp`).

The sole exception is `feature_caps` (FTM, WPA3, GCMP, etc.): the C6 only
accepts the P4's value if it matches its own or if TX caching is enabled (which
requires PSRAM that the C6 lacks); otherwise the C6 uses **its own value**.
Verified in the flashed C6 binary (`req_wifi_init`): the C6's native
`feature_caps` is `301` (`0x12D` = WPA3-SAE, FTM initiator and responder, GMAC,
BSS max idle), and that is what gets passed to `esp_wifi_init`. Because driver
support for FTM lives in this bitmask (`ftm_is_initiator_supported` checks bit 2
of `feature_caps`), the C6's own build determines whether it can initiate an FTM
session — see [FTM](#ftm-distance-measurement--silicon-limitation) below.

Rebooting the P4 also resets the C6, so any change to this configuration takes
effect on the next boot. If the C6 already has Wi-Fi initialized when a
different config arrives, esp_hosted automatically stops, deinitializes, and
reinitializes Wi-Fi (`__wrap_esp_wifi_init`).

## Pitfall 1: CSI rejected with `ESP_FAIL`

**Symptom.** Running `radio csi` in the console: the C6 refuses to enable CSI,
returning `ESP_FAIL`, and the RX callback never fires. Our C6 setup matched the
official `esp-csi` example line for line; changing the call order or turning on
promiscuous mode beforehand made no difference.

**Root cause.** The P4 was initializing Wi-Fi with `csi_enable = 0`
(`CONFIG_WIFI_RMT_CSI_ENABLED` unset), and the C6 dutifully obeyed.

**Proof from the driver binary** (`components/esp_wifi/lib/esp32c6/libnet80211.a`,
disassembled with `riscv32-esp-elf-objdump`):

1. `wifi_menuconfig_init` (in `ieee80211_ioctl.o`) copies fields from
   `wifi_init_config_t` into the global struct `g_wifi_menuconfig`. The field at
   byte offset 80 of the input struct is copied to offset 24 of the global.
   Offset 80 is `csi_enable`: `osi_funcs` (4) + `wpa_crypto_funcs` (44) place
   `static_rx_buf_num` at offset 48, and eight 4-byte `int` fields bring us to
   offset 80.
2. `esp_wifi_set_csi` and `esp_wifi_set_csi_config` (in `ieee80211_api.o`)
   check, in exact order: whether Wi-Fi is initialized (`ESP_ERR_WIFI_NOT_INIT`),
   whether Wi-Fi is started (`ESP_ERR_WIFI_NOT_STARTED`), and
   **`g_wifi_menuconfig + 24`**. If that word is zero, they log *"CSI not
   enabled in menuconfig!"* via `wifi_log` and return **-1 (`ESP_FAIL`)**.
3. Past that check, there are no other error paths: `wifi_set_csi` →
   `ic_set_csi` → `hal_mac_set_csi`, and `wifi_csi_set_config` →
   `ic_csi_set_config` simply write hardware registers. Neither call order nor
   promiscuous mode affects the outcome.

**Fix.** On the P4, set `cfg.csi_enable = 1` before calling `esp_wifi_init`
(`main/hal/tab5_wifi.cpp`). This does not require reflashing the C6. Cost: the
C6 reserves roughly 1 KB per static RX buffer for CSI metadata (with 10 buffers,
~10 KB of C6 SRAM), even while CSI is disabled. Enabling it on the fly would
require tearing down and restarting Wi-Fi every time.

On the C6 side, CSI setup now follows a single clean path matching Espressif's
example: `esp_wifi_set_csi_config` → `esp_wifi_set_csi_rx_cb` →
`esp_wifi_set_csi(true)`, starting from the driver's default config. Enabled
frame types: L-LTF (legacy/beacons), HT20, HT40, and HE SU (802.11ax, what a
Wi-Fi 6 router sends to the C6), with `acquire_csi_he_stbc = 2`. On the C6,
`CONFIG_SOC_WIFI_MAC_VERSION_NUM` is 2, which selects the struct layout without
`acquire_csi_force_lltf`/`vht` (`esp_wifi_he_types.h`).

`farejo info` prints the CSI diagnostic state: last error code, which step
failed (1 = config, 2 = callback, 3 = enable), and whether CSI is currently
active. Seeing `ESP_FAIL` there always means Wi-Fi was initialized with
`csi_enable == 0`.

### Where CSI comes from (and why we only got 6 callbacks in 10 seconds)

Once CSI was enabled, our first 10-second test produced **6 callbacks** instead
of the expected ~100. The driver really only invoked the callback 6 times. Once
we added a background ping, we got 110 callbacks in 10 seconds, all HT frames.

CSI is estimated from the **OFDM** training symbols in the PHY preamble (L-LTF,
HT-LTF, HE-LTF). On 2.4 GHz, router beacons are almost always transmitted at
1 Mbps (802.11b DSSS), which contains no OFDM training fields and therefore
produces no CSI. Without promiscuous mode, the C6 only processes frames
addressed to its own MAC, and on an idle link there are almost none — those 6
records were incoming MQTT packets that happened to arrive during the test.

To get a steady CSI stream, something must **generate OFDM traffic addressed to
the C6**. The official `esp-csi` (`csi_recv_router`) example pings the router
continuously, and `radio csi` does the same: it pings the gateway at 10 Hz for
the duration of the capture window. Every echo reply is an HT/HE frame from the
router to the C6. Capturing CSI from **other** nearby devices requires enabling
promiscuous mode (sniffing).

### What each CSI record contains

`c6_csi_cab_t` (16 bytes) followed by the raw CSI payload bytes. The `flags`
byte tells the parser how to interpret the payload:

- bits 4..7: baseband frame format (`cur_bb_format` from the driver: `0` = 11b,
  `1` = 11g, `2` = HT, `4` = HE SU, etc.), since each format carries a different
  set of subcarriers;
- `C6_CSI_F_HT40`: secondary channel present;
- `C6_CSI_F_W0_RUIM`: the first 4 bytes are invalid. This reflects the driver's
  `first_word_invalid` flag, a known ESP32-C6 hardware quirk; ignoring it reads
  two subcarriers of garbage;
- `C6_CSI_F_CORTADO`: the driver delivered more than `C6_CSI_MAX` bytes and the
  record was truncated.

`C6_CSI_MAX` is 1024 bytes because `rx_channel_estimate_len` is a 10-bit field
(up to 1023). Our initial firmware capped records at 256 bytes, and every single
record in our first test came back at 256 bytes — silently dropping half the
subcarriers.

Practical details:
- During modem sleep, the C6 powers down the receiver between beacons;
  `radio csi` holds a Wi-Fi wake lock during capture
  (`tab5_wifi_manter_acordado`).
- `min_intervalo_ms` rate-limits CSI on the C6 so it doesn't flood the SDIO bus.
  Rate limiting uses fixed **cadence buckets** (accepting the first frame in
  each time slot) rather than "time elapsed since last accepted frame": when
  the ping source ran at the exact same rate as the filter, normal over-the-air
  jitter caused nearly half the replies to be dropped (only 64 of 110 kept).
  For the same reason, `radio csi`'s ping source runs at twice the target
  capture rate (20 Hz).

## Pitfall 2: 802.15.4 killed Wi-Fi traffic

**Symptom.** Pinging the router succeeded 4/4 before running `radio 154` and
dropped to 0/4 afterward. The C6 stayed associated to the AP, but no IP packets
got through, and the 802.15.4 receiver captured zero frames.

There were actually **two** separate bugs, uncovered one after the other: fixing
coexistence allowed Wi-Fi traffic to flow **while** 802.15.4 was active (4/4
pings mid-test), but Wi-Fi still died **after** turning 802.15.4 off. That is
why `radio 154` pings the router before, during, and after the capture window —
they test failure modes at different points in the lifecycle.

### Cause 1 (while active): missing coexistence setup

Wi-Fi, BLE, and 802.15.4 share a **single** RF path on the ESP32-C6, arbitrated
in time by the hardware coexistence module. The arbiter completely ignores
802.15.4 until `esp_coex_wifi_i154_enable()` is called — and our C6 firmware was
not calling it.

**Proof from ESP-IDF sources:**
- `components/esp_coex/src/coexist.c`: `esp_coex_wifi_i154_enable()` calls
  `coex_enable()` followed by `esp_coex_ieee802154_status_enable()`.
- The 802.15.4 driver (`components/ieee802154/`) **never** calls
  `esp_coex_ieee802154_status_enable` on its own; it only updates per-event
  priority (PTI) values. Enabling status reporting is the application's job.
- Both official ESP-IDF examples that combine Wi-Fi and 802.15.4 on a single C6
  call this function explicitly in app code:
  `examples/openthread/ot_br/main/esp_ot_br.c` and
  `examples/zigbee/esp_zigbee_gateway/main/esp_zigbee_gateway.c`.

**Fix:** call `esp_coex_wifi_i154_enable()` before the first
`esp_ieee802154_enable()`, and call `esp_coex_ieee802154_status_enable()` every
time the receiver wakes back up.

With coexistence enabled, `esp_ieee802154_receive()` places the radio in the
`RX` state, requesting RF access at the `txrx` priority defined in
`s_coex_config` (`driver/esp_ieee802154_util.c`; `IDLE` priority is only used
when the MAC is stopped). Active Wi-Fi traffic preempts 802.15.4, which listens
in the gaps. In testing, router ping latency rose from 5 ms to 48 ms while
802.15.4 was listening continuously, with 0% packet loss.

### Cause 2 (on shutdown): calling `disable` mid-receive

```c
esp_err_t esp_ieee802154_disable(void)   // components/ieee802154/esp_ieee802154.c
{
    esp_btbb_disable();
    ieee802154_rf_disable();   // esp_phy_disable(PHY_MODEM_IEEE802154)
    ieee802154_disable();      // modem_clock_module_disable + state = DISABLE
    return ieee802154_mac_deinit();
}
```

`ieee802154_disable()` gates the MAC clock and flips the driver state **without
calling `stop_current_operation()`**. Because our sniffer runs with
`rx_when_idle` enabled, the MAC is actively in `RX` holding an RF grant request
with the coexistence arbiter when its clock is abruptly cut — freezing that
grant in place. Shared modem clocks aren't the culprit (`modem_clock` is
reference-counted, and Wi-Fi/BLE hold their own references); the stuck arbiter
state is.

The driver already has a clean shutdown path: `esp_ieee802154_sleep()` calls
`stop_current_operation()` (issuing `CMD_STOP` to the MAC), transitions to
`SLEEP`, and releases the PHY via `esp_phy_disable`. That is what OpenThread
uses continuously while running alongside Wi-Fi (`esp_openthread_radio.c`),
reserving `disable` strictly for full stack teardown.

**Fix:** shutting down 802.15.4 now runs `esp_ieee802154_set_rx_when_idle(false)`
(otherwise the driver immediately re-enters RX on its own) +
`esp_ieee802154_sleep()` + `esp_coex_ieee802154_status_disable()`.
**`esp_ieee802154_disable()` is never called.** Once initialized, the driver
stays enabled in sleep mode (costing only the idle MAC clock), and resuming
reception is a single `esp_ieee802154_receive()` call. `farejo info` reports
whether 802.15.4 is uninitialized, listening, or sleeping.

Because start and stop happen on different tasks (host RPC starts capture; a
1-second watchdog stops it if the host stops renewing the lease), a mutex
(`s_radio_mtx`) serializes all calls into the driver.

**What to expect** — ESP-IDF's official ESP32-C6 coexistence matrix
([api-guides/coexist](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/api-guides/coexist.html)):

| 802.15.4 mode | Wi-Fi STA scanning | Wi-Fi STA connecting | Wi-Fi STA connected |
|---|---|---|---|
| Scan / Router (RX always on) | C1 (unstable) | C1 (unstable) | **Y (stable)** |
| End Device | Y | Y | Y |

M5tabOS's passive sniffer mode (listen-only, RX always on) behaves like a
"Router" in this table: while Wi-Fi is associated it is stable and supported,
though 802.15.4 will miss frames during heavy Wi-Fi bursts. Espressif recommends
a dual-SoC design with separate antennas for production Thread/Zigbee border
routers; on the Tab5, 802.15.4 is meant for passive sniffing and diagnostics,
not acting as a mesh coordinator.

802.15.4 runs **strictly listen-only**: hardware auto-ACK is disabled
(`ieee802154_pib_set_auto_ack_tx(false)`, an internal driver API) so that
promiscuous capture never ACKs frames belonging to neighboring Zigbee/Thread
networks.

**Verification.** `radio 154 [channel] [seconds]` pings the router before,
mid-window (with 802.15.4 listening), and after. `DURANTE` at 4/4 confirms
coexistence, and `DEPOIS` at 4/4 confirms clean sleep teardown. Capturing zero
802.15.4 frames when no Zigbee/Thread devices are nearby is normal. Measured
with both fixes applied: 4/4 across all three phases, with ping latencies of
12 ms before, 19 ms during, and 7 ms after.

## FTM: distance measurement — silicon limitation

**Symptom.** `radio ftm` returned `status 255`. That status code does not come
from the Wi-Fi driver (whose FTM status codes range from 0 to 6); it is our C6
firmware's sentinel for "`esp_wifi_ftm_initiate_session` returned an error".
Once we plumbed the `esp_err_t` through to the P4, the error was
`ESP_ERR_NOT_SUPPORTED`.

**Root cause — silicon errata, not configuration.** Disassembling the driver
shows `esp_wifi_ftm_initiate_session` only returns `ESP_ERR_NOT_SUPPORTED` in
two cases: Wi-Fi is in AP-only mode (*"FTM initiator needs Station mode!"*), or
`ftm_is_initiator_supported()` returns false (*"FTM Initiator mode not
supported!"*) by testing bit 2 of `feature_caps`. The C6 passes that bit set
(`301`, see above), but ESP-IDF's `esp_wifi_init` wrapper clears it before
handing the config to the closed-source blob:

```c
/* components/esp_wifi/src/wifi_init.c */
#ifdef CONFIG_ESP_WIFI_FTM_ENABLE
    esp_chip_info_t info = {0};
    esp_chip_info(&info);
    if (info.model == CHIP_ESP32C6 && info.revision <= 1) {
        ((wifi_init_config_t *)config)->feature_caps &= ~(CONFIG_FEATURE_FTM_INITIATOR_BIT);
    }
#endif
```

**ESP32-C6 silicon revisions v0.0 and v0.1 do not support FTM initiator mode**,
and Espressif explicitly disables it in ESP-IDF. The C6 inside our Tab5 is one
of those revisions. There is no legitimate workaround: forcing the bit back on
would yield bogus time-of-flight measurements. FTM responder mode (letting
another device measure its distance to the Tab5) is unaffected.

**In the firmware:** the C6 only advertises `C6_CAP_FTM` if its silicon revision
supports it (`caps_reais()`), the `INFO` response reports `chip_rev`, and
`radio ftm` prints the exact `esp_err_t` name if the driver rejects the call.
Other synchronous rejection codes exist in the binary — `ESP_ERR_INVALID_ARG`
(`frm_count` not in 0/8/16/24/32/64, `burst_period` equal to 1 or >255, channel
outside regulatory domain), `ESP_FAIL` (STA interface not created),
`ESP_ERR_NO_MEM` — and are reported by name. Target APs that don't support
802.11mc FTM do **not** cause a synchronous error: the session starts and
terminates asynchronously with `status = 1` in `WIFI_EVENT_FTM_REPORT`.

## Who consumes C6 radio streams on the P4

| Service | Message ID | Consumers |
|---|---|---|
| `tab5_sniffer` | `C6_MSG_FAREJO` (batched) | Radar app (Wi-Fi targets), `farejo` console command |
| `tab5_csi` | `C6_MSG_CSI` | Radar app (motion detection), `radio csi` console command |
| console | `C6_MSG_FTM`, `C6_MSG_154` | `radio ftm` / `radio 154` diagnostic commands |

**One listener per message ID.** The host `peer_data` dispatcher
(`eh_host_feat_peer_data_dispatch.c`) stores a single callback per `msg_id`, so
calling `eh_host_peer_data_register` for an already-registered `msg_id`
**overwrites** the previous listener. Therefore, each message ID has a single
owning service on the P4, and apps or console commands subscribe through that
service (`tab5_farejo_ouvinte`, `tab5_csi_ouvinte`).

**Motion detection via CSI** (`main/services/tab5_csi.cpp`). Uses the 52 valid
subcarriers of L-LTF (present in both 802.11g and HT frames). For each frame, it
computes per-subcarrier amplitude normalized by the frame's mean amplitude
(canceling out C6 AGC gain steps between frames) and measures the mean deviation
against a ~1-second exponential moving average of the channel. The local
"stillness baseline" is learned over the first ~3 seconds and adapts slowly;
motion level is derived from the ratio of current deviation to baseline
(2× = light motion, 4× = strong motion, with hysteresis). Because moving the
Tab5 itself perturbs the channel just as much as a person walking by, consumers
notify the service when the tablet is moving (`tab5_csi_aparelho_mexendo`),
which pauses detection and re-learns the baseline once the device settles.

**Wi-Fi targets in Radar.** Each entry in a 100 ms sniffer batch is fed into the
Radar pipeline stamped with the batch's midpoint timestamp and that MAC's mean
RSSI, where it is correlated against gyroscope heading just like a BLE
advertisement. The sniffer does not channel-hop: while associated to an AP,
changing channels would drop the Wi-Fi connection, and in radio-only mode it
stays on the configured ESP-NOW channel.

## P4 ↔ C6 wire contract

Defined in `shared/tab5_c6.h` and included by both builds.

- **Hard limit of 8 `peer_data` handlers** on the C6
  (`EH_CP_FEAT_PEER_DATA_MAX_HANDLERS`), and all 8 slots (`msg_id` 1..8) are
  taken. Registering a ninth handler would fail silently. Any new P4 → C6
  command must be added as a key under `C6_MSG_HOST_AJUSTE` or a subcommand of
  `C6_MSG_HOST_RADIO`, never as a new top-level `msg_id`. The C6 → P4 direction
  does not have this limit.
- **Capability bitmask.** Both `PRONTO` and `INFO` carry `caps`; each bit
  (`C6_CAP_CSI`, `C6_CAP_FTM`, `C6_CAP_154`, etc.) is set only if the
  corresponding module was compiled in and is supported by the silicon. The P4
  hides UI controls for capabilities absent from the flashed C6 firmware.
- **Everything sleeps until requested by the P4**, and CSI, promiscuous
  sniffing, and 802.15.4 automatically shut themselves off if the P4 stops
  renewing its lease (`C6_RADIO_VALIDADE_MS`, `C6_FAREJO_VALIDADE_MS`). FTM is
  one-shot.
- **Radio callbacks never call into esp_hosted directly.**
  `eh_cp_feat_peer_data_send` can block in the RPC layer; calling it from inside
  a Wi-Fi driver callback deadlocks the networking stack. Instead, radio
  callbacks push records into a 12 KB variable-item **ring buffer** (`s_saida`),
  and a dedicated `c6saida` task drains it over SDIO. We use a ring buffer
  rather than a fixed-element FreeRTOS queue because messages range from 7 bytes
  to ~1 KB: a fixed queue would waste 1 KB per slot and require assembling
  1 KB structs on the Wi-Fi task's stack. With `xRingbufferSendAcquire`, the
  producer reserves the exact payload size in-place. The 802.15.4 RX callback
  runs in ISR context and uses `xRingbufferSendFromISR`. If the ring fills up,
  new records are dropped and `farejo info` reports the drop counter.

## Flashing the C6

Because the ESP32-C6 has no dedicated USB port, it is flashed through the
board's programming header with the Tab5 **powered off** (the serial adapter
powers the C6). Step-by-step instructions, factory backup commands, and recovery
steps are in the [README](README.md#esp32-c6-firmware-optional).

```bash
c6/flash.sh   # auto-detects the serial port; use PORT=/dev/xxx c6/flash.sh to override
```

The C6 firmware includes two safety nets: bootloader rollback and a 120-second
application watchdog — if the P4 fails to complete the handshake within two
minutes of boot, the C6 marks the update invalid and rolls back to the previous
working image.

**Changes confined to the P4 side do not require opening the case or reflashing
the C6.** The CSI init fix above is a prime example. Only edits under `c6/` or
`shared/` require the external programmer.

### Modifying `c6/sdkconfig.defaults`

An existing `c6/sdkconfig` file takes precedence over `sdkconfig.defaults`:
editing a key in `sdkconfig.defaults` has no effect on an already-generated
`sdkconfig`. Always delete `c6/sdkconfig` before building and `grep` the
regenerated file to confirm your key took effect. Unknown Kconfig keys in
`sdkconfig.defaults` are silently discarded by ESP-IDF.

## Post-flash verification checklist

1. `farejo info` — shows `contrato v3` and capabilities `CSI s | FTM s | 802.15.4 s`.
2. `ping` — both the local router and external internet respond.
3. `radio csi 10` — captures roughly 100 records (1 every 100 ms), breaks them
   down by PHY format (HT/HE on a Wi-Fi 4/6 router), with zero truncated
   records; afterward, `farejo info` shows CSI `OK` and 0 dropped messages. On
   an 802.11n router, each HT20 record is 256 bytes (128 B L-LTF + 128 B HT-LTF).
4. `radio ftm` — either completes a session (driver status 0..6) or prints the
   exact `esp_err_t` name explaining why initiation was refused. On a C6
   v0.0/v0.1, `ESP_ERR_NOT_SUPPORTED` is expected (see
   [FTM](#ftm-distance-measurement--silicon-limitation)).
5. `radio 154 15 20` — `WiFi ANTES`, `DURANTE`, and `DEPOIS` all report 4/4;
   running `farejo info` afterward shows 802.15.4 as "sleeping".
6. `ping` again — confirms Wi-Fi remains healthy after exercising all radios.
7. `farejo on` / `farejo` / `farejo off` — nearby MAC addresses appear in
   batches.
