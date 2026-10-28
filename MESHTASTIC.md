# Meshtastic Integration

An overview of what the Meshtastic client protocol exposes to a companion
device (the role the Tab5 plays when connected to a node), and what M5tabOS
currently implements or can support. Verified against the generated firmware
headers (`src/mesh/generated/meshtastic/*.pb.h`) and `AdminModule.cpp`.

## Radio NodeDB limits

The maximum number of nodes a radio can store is a **compile-time firmware
constant** (`MAX_NUM_NODES` in `src/mesh/mesh-pb-constants.h`), not a user
setting:

| Firmware | ESP32-S3 (Heltec V3, T-Beam S3) | Generic ESP32, C3, **C6 (Unit C6L)** | nRF52840 |
|---|---|---|---|
| up to 2.7.x | 100 / 200 / 250 depending on flash size (4 / 8 / 16 MB) | **100** | **80** |
| 2.8.0+ | same | **120** + "warm tier" | **120** + "warm tier" |

A Heltec V3 with 8 MB of flash holds 200 nodes, while a Unit C6L holds 100:
hearing the exact same mesh traffic yields different node list sizes. The
"online/total nodes" count reported in `LocalStats` reflects the radio's own
view and caps out at its internal limit. Updating the C6L to 2.8 raises its cap
to 120 (and 80 to 120 on nRF52840). The "warm tier" preserves the identity and
public key of evicted nodes; remembering anything beyond that requires off-radio
storage.

**Off-radio storage is where the Tab5 shines.** The system service table (256
slots in PSRAM) learns nodes from live packets (`NODEINFO`, `POSITION`,
`TELEMETRY`, text messages) as well as from the initial connection dump. Backed
by its own on-SD NodeDB (`nodedb.bin`, storing full node records rather than
just names), the Tab5 remembers far more nodes than the attached radio can fit.
`MyNodeInfo.nodedb_count` (field 15) reports how many entries the radio itself
holds, allowing the UI to display both counts side by side.

## How the client talks to the node

- **FromRadio** (node to client): `1 id, 2 packet, 3 my_info, 4 node_info,
  5 config, 6 log_record, 7 config_complete_id, 8 rebooted, 9 moduleConfig,
  10 channel, 11 queueStatus, 13 metadata, 14 mqttClientProxyMessage,
  15 fileInfo, 16 clientNotification, 17 deviceuiConfig, 19 region_presets`.
- **ToRadio** (client to node): `1 packet, 3 want_config_id, 4 disconnect,
  7 heartbeat`.
- **Administration** uses a standard `MeshPacket`: destination = the local node,
  port `ADMIN_APP` (6), payload = `AdminMessage`. When `from` is omitted (`0`),
  the firmware treats the packet as LOCAL administration from a directly
  attached client and **does not require a session passkey** (`AdminModule.cpp`,
  `mp.from == 0` branch). A `session_passkey` is only required when
  administering a REMOTE node over the mesh.

## AdminMessage — supported operations

| Field | Operation | Notes |
|---|---|---|
| 1/2 | `get_channel_request` / response | one channel per request |
| 3/4 | `get_owner_request` / response (`User`) | |
| 5/6 | `get_config_request` (type) / response | already included in the initial config dump |
| 7/8 | `get_module_config_request` / response | same as above |
| 12/13 | `get_device_metadata` | firmware version, capabilities, hardware model |
| 32 | `set_owner` (`User`) | long/short name, `is_licensed`, `is_unmessagable` |
| 33 | `set_channel` (`Channel`) | name, PSK, role, MQTT uplink/downlink, position precision |
| 34 | `set_config` (`Config`) | **replaces the ENTIRE section** (see below) |
| 35 | `set_module_config` | same, per module |
| 38 | `remove_by_nodenum` | removes a node from the radio's NodeDB |
| 39/40 | `set/remove_favorite_node` | **in use** (star / favorite toggle) |
| 41/42 | `set/remove_fixed_position` | fixed position — can be populated from the Tab5 GPS |
| 43 | `set_time_only` | sets the node's real-time clock |
| 47/48 | `set/remove_ignored_node` | ignore/unignore a node |
| 49 | `toggle_muted_node` | mute/unmute a node |
| 64/65 | `begin/commit_edit_settings` | transaction wrapper: batch multiple changes into a single flash write |
| 97 | `reboot_seconds` | reboot the node |
| 98 | `shutdown_seconds` | power off the node |
| 99 / 94 | `factory_reset_config` / `factory_reset_device` | |
| 100 | `nodedb_reset` | wipe the radio's NodeDB |

### Updating settings without modeling the entire protobuf schema

`set_config` overwrites the entire configuration section
(`config.lora = c.payload_variant.lora`): sending only the modified field
zeroes out every other field in that section. To keep the service's protobuf
parser small and self-contained, we use a raw-append trick:

1. Save the **raw protobuf bytes** of each section received during the initial
   dump (`FromRadio` fields 5 and 9).
2. To modify a setting, send the saved raw bytes **concatenated with** the newly
   encoded field at the end. Per the Protobuf wire format specification, when a
   scalar field appears multiple times, the last occurrence wins — and the
   firmware's nanopb decoder honors this. (Note: this does not apply to
   `repeated` fields like `ignore_incoming`, which would append instead of
   replace.)
3. Wrap the changes in `begin_edit_settings` … `commit_edit_settings`. The
   firmware writes to flash once and, if required by the modified section,
   **reboots automatically**; the subsequent `rebooted` notification
   (`FromRadio` 8) triggers the service to re-request the configuration dump.

### Configuration sections and key fields

- **device** (Config 1): `role` (`CLIENT`, `CLIENT_MUTE`, `ROUTER`, `REPEATER`,
  `TRACKER`, `SENSOR`, `CLIENT_HIDDEN`, `ROUTER_LATE`, `CLIENT_BASE`…),
  `rebroadcast_mode`, `node_info_broadcast_secs`, `tzdef`,
  `led_heartbeat_disabled`.
- **position** (2): `position_broadcast_secs`, `smart_enabled`,
  `fixed_position`, `gps_mode` (**must stay `NOT_PRESENT` on a Unit C6L plugged
  into the Grove port**, because its GPS configuration claims the same UART pins
  used by the serial link), `broadcast_smart_minimum_*`.
- **power** (3): `is_power_saving`, `ls_secs`, `min_wake_secs`.
- **network** (4): `wifi_enabled`, `wifi_ssid`, `wifi_psk`, `ntp_server` —
  controls the C6L's own Wi-Fi radio used by its internal MQTT client.
- **display** (5): on-node screen settings (`screen_on_secs`, `flip_screen`,
  `units`).
- **lora** (6): `region` (`BR_902` = 26), `modem_preset` (`LONG_FAST` = 0 …),
  `hop_limit`, `tx_enabled`, `tx_power`, `channel_num`,
  `sx126x_rx_boosted_gain`, `ignore_mqtt`, `config_ok_to_mqtt`.
- **bluetooth** (7): `enabled`, `mode` (random/fixed/no PIN), `fixed_pin`.
- **security** (8): keys, `admin_key` (up to 3), `serial_enabled`,
  `admin_channel_enabled`. **Do not touch** carelessly: misconfiguring this can
  lock you out of the node.

ModuleConfig sections: `1 mqtt, 2 serial, 3 external_notification,
4 store_forward, 5 range_test, 6 telemetry, 7 canned_message,
9 remote_hardware, 10 neighbor_info, 11 ambient_lighting, 12 detection_sensor,
13 paxcounter, 14 statusmessage, 15 traffic_management, 17 mesh_beacon`. The
most useful ones here are **mqtt** (enable, server, per-channel
uplink/downlink, map reporting), **telemetry** (intervals), **neighbor_info**,
**store_forward**, and **range_test**.

**Serial (ModuleConfig 2) is our lifeline to a wired node**: changing `mode`,
pins, or baud rate severs the Grove link, so it is intentionally excluded from
remote editing.

## Application portnums

Currently used: `1 TEXT, 3 POSITION, 4 NODEINFO, 5 ROUTING, 6 ADMIN,
67 TELEMETRY`. Good candidates for future integration: `8 WAYPOINT` (map pins),
`70 TRACEROUTE` (hop-by-hop route discovery with per-hop SNR),
`71 NEIGHBORINFO` (direct 1-hop neighbors per node — the mesh topology graph),
`65 STORE_FORWARD` (message history cached by a router), `66 RANGE_TEST`,
`73 MAP_REPORT`, and `12 KEY_VERIFICATION`.

## Other node messages currently ignored

- `metadata` (13): we currently read only the firmware version, but it also
  carries `hasWifi`, `hasBluetooth`, `role`, `hw_model`, `hasPKC`, and
  `excluded_modules`.
- `my_info` (3): besides the node number, it includes `reboot_count`,
  `nodedb_count`, `pio_env`, and `firmware_edition`.
- `clientNotification` (16): human-readable warnings from the firmware ("favorites
  full", duplicate key, weak key) that could be surfaced in the system status bar.
- `log_record` (6): diagnostic log stream from the node.
- `NodeInfo` fields 10–13: `is_favorite` (in use), `is_ignored`,
  `is_key_manually_verified`, `is_muted`.

## What is already implemented

- Phases 1, 2, and 5 from the roadmap below: `services/tab5_mesh_admin.{hpp,cpp}`
  and the "Node" tab in the Mesh app, plus the Tab5's extended NodeDB in
  `nodedb.bin`.
- Verified against firmware versions 2.5, 2.7, and latest: protobuf field
  numbers, passkey-free local admin, and automatic reboot on
  `commit_edit_settings`.
- `tools/test_mesh_admin.py` validates our raw packet assembly against the
  official Meshtastic Python protobuf definitions.

## Meshtastic features not implemented here

Knowing a port number or caching raw config bytes is not the same as supporting
a feature end-to-end. The protocol features below exist in upstream Meshtastic
and official clients, but are not implemented in our parser, UI, or the five-phase
roadmap below.

### Security and messaging

- **PKI direct messages**: instead of using the shared channel PSK, a direct
  message can be encrypted with the recipient's `User.public_key` (8). Incoming
  packets identify the sender's key in `MeshPacket.public_key` (16) and set
  `pki_encrypted` (17). `Config.security` (8) holds the node's `SecurityConfig`:
  `public_key` (1), `private_key` (2), up to three `admin_key` entries (3),
  `is_managed` (4), `debug_log_api_enabled` (6), and `admin_channel_enabled`
  (8). This is an additional encryption layer, not a replacement for channel PSKs.
- **Interactive key verification (SAS)**: uses port `KEY_VERIFICATION_APP` (12)
  and `AdminMessage.key_verification` (67), whose `KeyVerificationAdmin` carries
  `message_type` (1), `remote_nodenum` (2), `nonce` (3), and
  `security_number` (4). The firmware drives the UI handshake via
  `ClientNotification.key_verification_number_inform` (11),
  `key_verification_number_request` (12), and `key_verification_final` (13);
  once confirmed, the NodeDB marks `NodeInfo.is_key_manually_verified` (12).
- **Replies and emoji reactions**: `Data.reply_id` (7) links a message to a
  prior message ID; `Data.emoji` (8) marks the payload as a reaction. Currently
  both fields are ignored and conversations render as a flat chronological list.
- **Compressed text and alerts**: `TEXT_MESSAGE_COMPRESSED_APP` (7) compresses
  payloads with Unishox2 (converted transparently by official firmware to/from
  `TEXT_MESSAGE_APP`), while `ALERT_APP` (11) carries high-priority alert text.

### History, mapping, and modules

- **Store-and-forward**: port `STORE_FORWARD_APP` (65), configured via
  `ModuleConfig.store_forward` (4). A client requests missed messages using
  `StoreAndForward.rr = CLIENT_HISTORY` (65) and `history` (3), where
  `History.window` (2) bounds the time window; the router replays messages as
  `ROUTER_TEXT_DIRECT` (8) or `ROUTER_TEXT_BROADCAST` (9). Request encoding,
  pagination, and merging replayed messages into conversation history are not
  implemented.
- **MapReport**: `MAP_REPORT_APP` (73) publishes unencrypted node summaries over
  MQTT. Opt-in is controlled by `ModuleConfig.mqtt.map_reporting_enabled` (10)
  with parameters in `map_report_settings` (11); the Tab5 does not configure
  this setting on the node (though its MQTT client does parse incoming map
  reports).
- **Detection Sensor**: `DETECTION_SENSOR_APP` (10) and
  `ModuleConfig.detection_sensor` (12), which turn GPIO state changes into mesh
  alert broadcasts.
- **Canned Message**: `ModuleConfig.canned_message` (7); preset message strings
  are read and written via dedicated `AdminMessage` operations
  `get_canned_message_module_messages_request/response` (10/11) and
  `set_canned_message_module_messages` (36) rather than `set_module_config`.
- **Paxcounter**: `PAXCOUNTER_APP` (34) and `ModuleConfig.paxcounter` (13), for
  estimating nearby crowd size by counting Wi-Fi/BLE MAC addresses.
- **Remote Hardware**: `REMOTE_HARDWARE_APP` (2) and
  `ModuleConfig.remote_hardware` (9), for reading, watching, and toggling GPIO
  pins on a remote node.
- **Ambient Lighting**: `ModuleConfig.ambient_lighting` (11), for driving onboard
  RGB LEDs on supported hardware.
- **Traffic Management** (2.8+): `ModuleConfig.traffic_management` (15), which
  has no dedicated portnum and performs mesh traffic inspection and rate limiting.
- **Mesh Beacon** (2.8+): `MESH_BEACON_APP` (37) and
  `ModuleConfig.mesh_beacon` (17), for broadcasting discovery beacons and
  channel/preset offers to nearby clients.

### Maintenance, UI, and contact sharing

- **OTA/DFU updates**: `AdminMessage.ota_request` (102) takes an `OTAEvent`
  specifying `reboot_ota_mode` (1) for BLE/Wi-Fi OTA and `ota_hash` (2) for
  image verification. `enter_dfu_mode_request` (21) triggers UF2 DFU mode
  (currently supported on nRF52 targets). The legacy `reboot_ota_seconds` (95)
  field is deprecated in favor of `reboot_ota_mode`.
- **On-device UI configuration**: beyond the spontaneous dump in
  `FromRadio.deviceuiConfig` (17), the protocol offers `get_ui_config_request`
  (44), response (45), and `store_ui_config` (46). M5tabOS does not fetch, edit,
  or persist this block.
- **QR/URL contact and channel import**: `AdminMessage.add_contact` (66) accepts
  a `SharedContact` containing `node_num` (1), `user` (2), `should_ignore` (3),
  and `manually_verified` (4). Official clients share channel sets via
  `https://meshtastic.org/e/#...` URLs and contacts via
  `https://meshtastic.org/v/#...`; decoding, previewing, and importing these
  links into the radio's NodeDB are not implemented.
- **Ringtones and external notifications**: `get_ringtone_request/response`
  (14/15) and `set_ringtone_message` (37) read and write the RTTTL ringtone
  string, while physical output pins, vibration motors, and buzzers are
  configured in `ModuleConfig.external_notification` (3).

## Proposed roadmap

1. **Inspect the node** (read-only, zero risk): cache `Config`, `ModuleConfig`,
   `metadata`, and `my_info` from the initial dump; display a "Node" tab with
   firmware version, hardware model, region, modem preset, role, hop limit, TX
   power, MQTT, Bluetooth, and position settings; show the radio's node count
   (`nodedb_count`) next to the Tab5's; surface `clientNotification` messages as
   system notifications.
2. **Safe settings** (one tap to commit and reboot): long/short name, role,
   `hop_limit`, `tx_power`, position/NodeInfo/telemetry broadcast intervals,
   MQTT (enable, uplink/downlink), Bluetooth (enable, fixed PIN), fixed position
   from the Tab5 GPS, and syncing the node's clock.
3. **High-impact settings** (with confirmation dialog): LoRa region, modem
   preset, channels (create, rename, PSK, role, delete), reboot, shutdown,
   NodeDB wipe, and removing/ignoring/muting individual nodes.
4. **Mesh diagnostics**: initiate traceroute from a node's detail card, view
   1-hop neighbors (`NeighborInfo`), waypoints, and range tests.
5. **Extended NodeDB on SD card**: persist every field of every seen node on the
   Tab5 SD card, scaling past the radio's internal NodeDB limit.
