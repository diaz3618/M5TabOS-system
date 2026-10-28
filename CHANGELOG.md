# Changelog

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/). The
firmware version is stored in `version.txt`. Rules for version increments and
cutting releases (`tools/release.sh`) are documented in `VERSIONING.md`.

## [Unreleased]

### Added
- Versioning: `tools/release.sh patch|minor|major` cuts a release (`version.txt`,
  CHANGELOG section, git commit, and tag) following the rules in
  `VERSIONING.md`. The version string embedded in the binary identifies the
  exact commit it was built from (`0.7.0+66.16a4d6d` = 66 commits past `0.7.0`),
  and the build fails if `version.txt` ever falls behind the latest git tag.
- Mesh > Node > Location: toggle location sharing on/off (controls position
  precision on the node's primary channel: disabled, exact, or approximate from
  ~90 m to ~6 km), smart position broadcast on movement, the node's own GPS mode
  (enabled/disabled, or "board has no GPS" when unsupported), and forwarding the
  Tab5's GPS fix to the node, alongside fixed position and broadcast interval.
- USB mass-storage (thumb-drive) mode: exposes the SD card to a connected
  computer as a USB drive over USB-C in the background while the Tab5 stays
  usable. Enable and reclaim the card in Settings > USB; while active, the
  status bar displays "Card on computer". Ejecting the drive on the computer
  returns the card to the Tab5 automatically; reclaiming it on the Tab5 before
  the computer ejects prompts for confirmation. While USB mass storage is
  active, the USB-C port is unavailable for serial console or flashing, the CPU
  runs at maximum clock, and the screen stays awake while the host has the
  volume mounted.
- Internal flash write journal (`reserva` partition, 2 MB): while the SD card is
  loaned to a computer over USB, all system writes (logs, chat history, energy
  telemetry, Mesh nodes, Wi-Fi credentials) are recorded to the flash journal
  and replayed onto the SD card in exact chronological order when it returns —
  even across a reboot. If the computer modified a file that the Tab5 also
  replaced in full, the computer's version wins and the Tab5's version is saved
  alongside it as `<name>.tab5`. The status bar warns if the journal exceeds 90%
  capacity, and the USB page reports any dropped records. **Requires a full
  reflashing via `./flash.sh`** (new partition table; NVS settings are
  preserved).
- Settings > USB: option to start USB mass-storage mode automatically when
  plugged into a computer (disabled by default; if an app is open or the screen
  is locked when plugged in, it shows a notification instead of interrupting).
- `tab5_sd_acrescentar()` and `tab5_sd_substituir()` — the system's journaled SD
  write APIs — plus `tab5_sd_emprestar()` / `tab5_sd_devolver()`
  (`hal/tab5_sd.hpp`), `app_t.exige_sd`, `sistema_manter_acordado()`,
  `sistema_bloqueado()`, and `sistema_app_exige_sd()` (`kernel/app.h`).
- Notification center: tapping the status bar opens a dropdown listing all apps
  with active notifications (name, summary, count, severity color); tapping a
  row launches the corresponding app. Dismiss by tapping outside, tapping the
  bar again, pressing Esc, or swiping up. The status bar now shows a colored dot
  for each additional notifying source alongside the top-priority highlight, and
  the lock screen lists up to 4 notifying apps instead of just one.
- `notif_listar()` and `notif_associar_app()` in `services/notification.h`.
- Mesh: parallel US mesh feed. A second MQTT client (`mqtt.meshtastic.org`,
  root topic `msh/US`, default `LongFast` channel) runs alongside the primary
  link — whether in internet-only mode, connected to a node over Grove/Bluetooth,
  or acting as an MQTT proxy. Incoming messages land in a dedicated "LongFast
  EUA" conversation beneath an "EUA" divider in the conversation list; nodes are
  tagged with their network origin, and the Nodes tab gains BRAZIL / USA filter
  buttons with an on/off toggle for the US feed (disabled by default). US nodes
  are capped at 96 entries in the table, never trigger notification beeps or
  status bar alerts, and do not offer direct messaging (channel-only, no PKI).
- Conversations: displays the timestamp beneath each message bubble, aligned to
  the sender's side (Mesh and ESP-NOW).

### Changed
- Moved the clock to the top-left corner of the status bar (except on the lock
  screen, which already displays a large clock); moved the ESP32-P4 die
  temperature down to the navigation bar alongside the running average since
  boot. The thermometer icon fills and changes color (blue, green, yellow,
  orange, red) with temperature.
- Replaced the word "HOME" on the bottom navigation bar button with a home
  circle icon.
- Mesh > Node settings organized into collapsible groups: the tab now opens to a
  compact category list (Name & Role, Location, LoRa Radio, Connections &
  Internet, Notifications & Telemetry, Maintenance), each showing a one-line
  summary; tapping a group opens its settings, and BACK returns to the group
  list. Pending edits apply across the entire Node tab — modifying settings in
  multiple groups still commits everything in a single transaction (rebooting
  the node only once).
- When a node is connected, the Tab5's GPS fix is sent **only to the local
  node** (matching the official mobile apps), letting the node broadcast its
  position at its own configured interval and precision. Previously, the Tab5
  also transmitted a 3-hop broadcast packet over the mesh, causing duplicate
  airtime when the node broadcast the same coordinates on its own schedule. When
  the node has a fixed position or its own internal GPS enabled, the Tab5 does
  not send GPS updates (which the node would ignore or overwrite). In
  internet-only mode (no node attached), the Tab5 honors the position precision
  learned from the node's primary channel (`0` = do not send).
- Removed the duplicate "Send GPS position to mesh" toggle from Settings >
  Meshtastic; it now lives exclusively under Mesh > Node > Location.
- Setting or clearing a fixed position now requests only the configuration dump
  back from the node (`want_config_id` 69420) rather than re-downloading the
  entire NodeDB; older firmware versions still send the full dump as before.
- Disabling the node's MQTT module now automatically disables the MQTT Client
  Proxy (Tab5 internet sharing) as well — mirroring the rule that enabling the
  proxy automatically enables MQTT.
- Files and Camera refuse to open while the SD card is mounted on a computer:
  the system displays an explanation screen in place of the app and opens the
  app automatically as soon as the card returns (`app_t.exige_sd`), and USB
  mass-storage mode will not start while either app is open.
- All persistent data writes now go through the system SD write APIs
  (`hal/tab5_sd.hpp`), leaving `tab5_sd_abrir()` for reads and large new binary
  files. Services that rewrite an entire file (Wi-Fi networks, node Wi-Fi
  networks, deleted conversations, NodeDB) only do so if they successfully read
  the existing file first — if the card was absent at boot, runtime changes live
  in RAM until reboot instead of wiping the saved file when the card returns.
- Atomic whole-file replacement (`tab5_sd_substituir`) now survives power loss
  mid-swap: the next reader automatically completes any interrupted `.tmp`
  rename.
- `sistema_manter_acordado()` now wakes the display if called while the screen
  is asleep (e.g., when a computer mounts the USB drive while the Tab5 is
  sleeping).
- Navigation lock (`sistema_travar_navegacao`) now also blocks opening the
  notification center from the status bar and rejects external app-switch
  requests (`sistema_simular_abrir`, serial console).
- Stored Twemoji bitmaps as 8-bit palettized images (1 byte per pixel + per-emoji
  color table) instead of raw RGB565, shrinking flash usage from ~1.49 MB to
  ~0.91 MB with pixel-identical rendering. Transparency now uses palette index 0
  instead of a magic magenta key color.
- Emoji rendered inline in text smaller than 24 px now scale down to match the
  line height instead of overflowing into adjacent lines.
- Mobile-style virtual keyboard layout and behavior. Tapping Shift arms uppercase
  for a SINGLE letter and releases automatically; double-tapping locks Caps Lock,
  with the Shift arrow icon indicating the state (outline / filled / filled with
  underline). In chat conversations, auto-capitalization arms automatically at
  the start of a message and after `". "`, `"! "`, and `"? "` (disabled for
  password, PIN, and name fields). Moved Shift and Backspace to the outer edges
  of the `"zxcv"` row like on Android, placing the `"?123"` key where Backspace
  used to be: two symbol pages featuring parentheses, braces, brackets,
  `< > \ | ~ ^`, and Portuguese accented characters (`á à â ã é ê í ó ô õ ú ç`,
  uppercase when shifted). Typing a space or accented letter on a symbol page
  returns to the letter layout automatically. Comma and period no longer turn
  into `?` and `!` when shifted, and the top number row stays numbers on all
  pages.
- Virtual keyboard: unified single-line field editing into `tecvirt_editar()`
  (used by Wi-Fi passwords, node names, and node Wi-Fi credentials) instead of
  three separate "append / pop last" implementations, and made Backspace delete
  a full UTF-8 codepoint rather than a single byte.
- Consolidated three separate Protobuf codecs (`tab5_mesh`, `tab5_mesh_admin`,
  `tab5_mesh_mqtt`) into a single bounds-checked header (`services/mesh_pb.hpp`).
  Encoders verify buffer capacity on every write and reject oversized payloads
  cleanly instead of writing past the end via raw pointers. Text, admin, and
  position packets share a single envelope builder, verified byte-for-byte on
  macOS via `testes/rodar_mesh_pb.sh` under ASan/UBSan.
- Unified service task shutdown across the OS (`kernel/service_stop.hpp`): GPS,
  Mesh, and the Mesh BLE transport now await task exit on a semaphore instead of
  polling every 10 ms (and in GPS, the stop flags weren't even `volatile`).
- Added a thread-safe single-listener wrapper (`hal/tab5_listener.hpp`) for BLE
  scanning, promiscuous Wi-Fi sniffing, and CSI: unregistering a listener waits
  for any in-flight callback to finish, allowing the Radar app to drop its
  40 ms "just in case" sleep on close.
- Changing the display pixel clock from the serial console now waits on a
  completion semaphore instead of polling every 10 ms.
- Consolidated "is the system clock valid?" checks into `tab5_hora_epoch()`,
  replacing twelve copies of the same magic timestamp scattered under four
  different constant names.
- MQTT bridge: incoming frames destined for the Mesh service are now assembled
  directly into the static queue item buffer (fitting comfortably within the US
  MQTT task's 3.5 KB stack), and outgoing `ToRadio` packets are parsed by a
  single shared helper across both the Brazil and US clients.
- Internal SRAM optimization: moved the stacks of all 6 `esp_hosted` tasks
  (`sdio_*`, `eh_rpc_*`) into PSRAM (`CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM`).
  They had been reserving 24 KB of internal SRAM for ~8 KB of measured peak
  usage. With Wi-Fi + MQTT active, free internal SRAM rose from 202 → 227 KB,
  minimum free from 157 → 182 KB, and largest contiguous block from 77 → 102 KB.
  With the screen on, monitor snapshot time and display underruns were unchanged
  (~2.3 ms worst case, zero flashes). During sleep with an active BLE scan,
  worst-case snapshot time rose from ~25 → ~50 ms while the display panel is
  powered off.
- `mem extrato` gained a "minimum" column (showing the transient low-water mark
  inside each boot stage that net deltas hide), and `tarefas` now reports the
  duration of the monitor snapshot itself (last and worst).
- Mesh memory footprint: the Tab5's 31 KB PSRAM shadow copy of the NodeDB is now
  allocated only while actively reading or writing `nodedb.bin`; US node names
  are no longer appended to `nos.txt`; and the US MQTT client uses compact
  768/512-byte buffers with a 3.5 KB internal SRAM stack.
- Mesh: share the Tab5's internet connection with a connected node (Meshtastic
  MQTT Client Proxy) via the "Tab5 internet (proxy)" row on the Mesh app's Node
  tab, right next to "Node MQTT" and following the same transactional editing
  flow: tapping stages the change, tapping SAVE TO NODE commits both proxy and
  MQTT in a single transaction, and the row displays the node's confirmed state
  alongside live bridge status. The node publishes and subscribes over its link
  with the Tab5 (Bluetooth or Grove serial) while the Tab5 forwards traffic to
  the MQTT broker over Wi-Fi — turning the node into a full gateway without
  enabling the node's own Wi-Fi radio and keeping the Grove port free for GPS.
  Because the Tab5 merely shuttles encrypted envelopes, end-to-end PKI direct
  messages pass through intact using whatever broker is configured on the node.
- Radar now sees Wi-Fi devices: promiscuous sniffing on the C6 feeds a second
  target stream alongside BLE (access points, phones probing for networks,
  associated stations) into the same cosine-fit directional solver and hot/cold
  proximity tracker, with a dedicated filter button (`ALL` / `TAGS ONLY` /
  `Wi-Fi ONLY`). Sniffing stays on the radio's current channel.
- Radar now detects room motion via Wi-Fi CSI: the `tab5_csi` service measures
  channel variation between the Wi-Fi router and the Tab5 (still / light /
  strong), learns each location's stillness baseline automatically, and pauses
  detection whenever the IMU senses the Tab5 itself moving.
- Added the `tab5_csi` service as the sole owner of `C6_MSG_CSI` (since
  `peer_data` supports one callback per message ID): requests router-only CSI
  from the C6, pings the gateway at 20 Hz as an OFDM illumination source, and
  has zero overhead when idle. The console `radio csi` command now routes
  through `tab5_csi` and displays live motion readings.
- C6 firmware (protocol contract v3): a single C6 flash now bundles every
  radio-dependent feature, each staying dormant until requested by the P4 —
  ESP-NOW (text in two dialects) and channel control, batched promiscuous
  sniffer, raw ESP-NOW pipe, `INFO`, plus Wi-Fi CSI, 802.11mc FTM, and 802.15.4.
  Because reflashing the C6 requires an external hardware programmer, packing
  all radio primitives into contract v3 lets future features evolve exclusively
  on the P4 side without reopening the tablet. Each capability sets a bit in
  `caps` (only when compiled in and supported by the silicon), and the P4 hides
  UI controls for capabilities absent from the flashed C6 build.
- Added the `tab5_sniffer` service and `farejo` console command: the C6 listens
  in promiscuous mode, aggregates frames by source MAC, and sends 100 ms batches;
  the P4 allocates the MAC table in PSRAM (from the shared pool) only while
  active, and the C6 shuts sniffing off automatically if the P4 stops renewing
  its lease. `farejo info` displays the flashed C6's radio capabilities.
- Reorganized the serial console: all commands now live in a single declarative
  table that generates `help` across seven categories (System, Display, SD Card,
  Network, Meshtastic, Bench, Tests) with full usage syntax and examples.
  `help <command>` prints detailed help for a specific command; `help tudo`
  prints every command at once; mistyped commands suggest the closest matches.
- Console enhancements: `estado` (replacing `servicos`, now including firmware
  version, uptime, and free RAM); `wifi` alone prints status, joined by
  `wifi redes`, `wifi gravar`, `wifi conectar`, and `wifi esquecer`; `log`
  accepts log-level names (`log eh_sdio debug`) and prints the current level;
  `ler <file> [lines]`; SD card paths default to `/sdcard`; `mesh nos` and
  `mesh enviar`; `grove cru <baud>`; `tela prioridade alta|padrao`; `mem tudo`.
  Legacy command aliases (`servicos`, `redes`, `nos`, `enviar`, `grove <baud>`,
  `tela on|off`) remain supported.
- Mesh: choose among three transports in Settings > Meshtastic (Grove Cable,
  Bluetooth, Internet). Grove has automatic priority: whenever the background
  probe detects a Unit C6L plugged into the Grove port, Mesh switches to it
  automatically from any transport; manual selection remains in effect until the
  next cable plug event.
- Mesh, Nodes tab: displays all active channels from the node as buttons along
  the top (channels hidden from the conversation sidebar show a `"+"` and can be
  restored with a tap); tapping any node in the table opens its detail card —
  node ID, MAC address (when reported), hardware model, SNR, hop count, battery
  level and voltage, last-seen age, and GPS coordinates with distance and
  cardinal bearing from the Tab5 — plus `CHAT` and `BACK` buttons.
- Mesh: added the "Node" configuration tab (gear icon in the sidebar) to
  configure an attached Meshtastic node directly from the Tab5 without a PC or
  phone: long/short names, device role, hop limit, TX power, broadcast intervals
  (position, NodeInfo, telemetry), MQTT, and Bluetooth settings. Edits are
  staged in yellow and committed in a single `begin/commit_edit_settings`
  transaction when tapping `SAVE TO NODE` — the node writes to flash, reboots
  once, and the UI reflects the newly confirmed values from the post-reboot
  dump. Immediate actions: set fixed position from Tab5 GPS, sync node clock,
  and reboot node (with confirmation). Also displays firmware version, board
  model, LoRa region/preset, stored node counts (radio · Tab5), reboot counter,
  and the latest firmware `ClientNotification` warning. Moved the notification
  sound toggle from the Mesh status tab to this tab.
- Mesh: `services/tab5_mesh_admin` — `set_config` and `set_module_config`
  re-send the raw protobuf bytes captured from the node's config dump with the
  modified scalar fields appended at the end (per Protobuf rules, the last
  occurrence wins), avoiding the need to model the entire Meshtastic schema.
  Sensitive sections (`network`, `power`, `security`) and the `serial` module
  are never re-sent raw.
- Mesh: persistent Tab5 NodeDB on the SD card (`nodedb.bin`, 256 nodes in
  PSRAM), loaded when the service starts and flushed every 10 minutes and on
  service shutdown — allowing the Tab5 to remember more nodes than the attached
  radio's internal limit (100 on the Unit C6L up to firmware 2.7).
- `tools/test_mesh_admin.py`: validates the raw protobuf packets assembled by
  M5tabOS (admin, text, channels) against the official Meshtastic Python
  protobuf definitions.
- Mesh: favorite nodes (star). Tapping `FAVORITE` on a node's detail card stars
  it; the node table displays the star and pins favorites directly below our own
  node in the order they were starred, keeping them permanently in the table
  without eviction. Favorites are synced with native Meshtastic (`AdminMessage`
  `set_favorite_node` / `remove_favorite_node` over local admin) so they appear
  in the web client, and incoming `is_favorite` flags from the node's dump sync
  back to the Tab5. In internet-only mode (no radio attached), changes are
  staged in NVS and pushed to the node on the next connection dump.
- Mesh: added a gray "heard by mesh" checkmark on outgoing direct messages —
  indicating a neighbor rebroadcast the packet, while the green checkmark
  remains reserved for an explicit ACK from the destination node itself.
- Paginated home screen: when more apps are installed than fit on one screen
  (12 currently), swiping horizontally switches pages, with page indicator dots
  at the bottom. `TAB5_HOME_LINHAS_MAX` (`kernel/internal.hpp`) sets the maximum
  rows per page.
- Lock screen (Settings > Display, disabled by default): waking from sleep
  shows a large clock, date, and top notifications; swiping up unlocks and
  returns to whatever was open. While locked, touch, key, and event delivery to
  the underlying app are suppressed.
- Serverless Mesh over the internet (MQTT) when no radio node is connected: with
  Wi-Fi connected and no Unit C6L attached, channel messages are sent and
  received directly through an MQTT broker. If no broker has been learned from a
  node yet, it defaults to Mesh Brasil (`platform.meshbrasil.com`, root topic
  `meshdev`, `LongFast` channel with the default PSK), omitting the radio region
  from the topic path. Also subscribes to sub-roots
  (`meshdev/<country>/<region>/...`) and `MAP_REPORT` packets to populate node
  names and positions. Once a node is connected, its channels, PSKs, and broker
  settings are learned and cached automatically.
- Settings > Meshtastic: added an Internet ON/OFF toggle and live status line
  (active broker and root topic). A wired Grove node always takes priority:
  while plugged in, the standalone internet client stays off even when Wi-Fi is
  connected.
- `tools/teste_mqtt_vivo.py`: desktop test script that connects to the live MQTT
  broker and decrypts traffic using the exact same algorithm to verify topics
  and crypto outside the tablet.
- Settings > Wi-Fi: scans nearby networks, prompts for passwords via the
  on-screen keyboard, stores multiple networks (encrypted on SD), and
  auto-connects to the strongest known network.
- Settings > Bluetooth: tapping a discovered Meshtastic node connects the Mesh
  service to it and displays live connection status; nodes are also recognized
  by name pattern when service UUIDs aren't advertised.
- Automatic NTP time sync over Wi-Fi. Time source priority hierarchy: NTP
  (internet), GPS (with valid fix), Mesh (live traffic only); a lower-priority
  source never overwrites a clock already disciplined by a higher-priority one.
- Settings > Bluetooth now identifies consumer trackers (Apple Find My / AirTag,
  Tile, Samsung SmartTag, Google Find My Device) and beacons (iBeacon,
  Eddystone) and estimates distance from RSSI. Settings > Wi-Fi also shows
  estimated distance for each access point.
- Logging: clock adjustments now log in local time with the time zone and state
  the drift in plain language (e.g., "clock was 1 s behind") instead of raw UTC
  deltas.
- Logging: rewrote boot and settings log messages to be readable without
  consulting the source code — using "on/off" instead of `=0/=1`, listing only
  active rails in power profile logs, and replacing raw pointer addresses and
  internal symbol names (`peer_data`, `AXI`, `fb0`, `app_main`) with clear
  descriptions.
- Promoted the virtual keyboard to a shared system widget (`tecvirt_t`): chat
  conversations, Wi-Fi password entry, and BLE PIN prompts all share the same
  implementation with visual key-press feedback and multitouch tracking. Extended
  it down to the bottom edge of the screen (locking navigation so bottom-row taps
  never hit Home), reduced key latency by one touch sample, and optimized
  Settings so typing a character repaints only the input box rather than the
  entire page and keyboard.
- Applied the same 10 px vertical touch offset correction to level-polled touch
  (`ao_tocar_nivel`) as tap events.
- Internal SRAM savings: moved ~80 KB of static arrays in apps and services
  (monitor, Trail, Radar, MQTT, Mesh, ESP-NOW, font tables) plus lwIP's `.bss`
  segment from internal SRAM to PSRAM via `main/linker.lf`, along with the 12 KB
  log ring buffer, the secondary display line vector, and the monitor task
  snapshot array.
- Right-sized task stacks based on measured high-water marks in `pilhas-*.csv`:
  console 12 → 8 KB, Wi-Fi and ESP-NOW 8 → 6 KB, MQTT 6 → 4 KB; increased
  `sys_evt` from 2.3 → 3 KB (had dipped to 600 bytes free).
- Configured mbedTLS to allocate from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`),
  so opening a TLS session no longer consumes ~40 KB of internal SRAM at once.
- Console: `mem` now prints an internal SRAM ledger (showing how much internal
  memory each boot stage and radio subsystem consumed) alongside a heap
  fragmentation map per memory region.
- Settings > Meshtastic: replaced the fixed-length repaint state array with a
  hash of the entire page state, fixing a bug where status strings at the bottom
  of the page (internet, proxy) fell past the comparison limit and failed to
  trigger a redraw.
- Redesigned the Power app around modern smartphone battery screens. The top
  header shows the large battery percentage, charging state (on battery /
  charging / plugged in), and a runtime/charge estimate based on the 30-minute
  rolling average ("lasts ~6 h 30 min at this rate", "full in ~1 h 20 min").
  Below it sits a six-card metric grid (battery %, current, power, voltage, die
  temperature, and 24-hour energy usage); tapping any card switches the chart
  below it. Removed the redundant title, SD log status line (now shown only on
  write failure), CPU MHz readout, and sign legend.
- Power app, level chart: added round-number Y-axis labels on every grid line
  (replacing the old min/max corner labels), filled area under the curve, green
  background shading during charging hours, and one vertex per pixel column
  (averaging samples within each column to smooth current noise). On the current
  chart, the fill extends to the zero line (green above zero, orange below).
  Tapping the chart places an inspection cursor showing the exact timestamp and
  value. Changed the 24 H view from a midnight-to-midnight "TODAY" window (which
  was empty right after midnight) to a rolling 24-hour window spanning yesterday's
  and today's CSV files.
- Power app, new hourly energy usage chart: plots hourly mAh bars over the last
  24 hours broken down into screen-on vs. sleep consumption, with green bands on
  charging hours and the same rolling time axis. Tapping a bar inspects that
  hour's breakdown; with no bar selected, it displays the 24-hour totals.

### Fixed
- SD card: "piggyback slots exhausted" on boot. Five services queued initial SD
  writes during boot while the table only had four slots, causing the fifth
  service (Mesh node Wi-Fi credentials) to silently drop its write.
- Mesh: sending a direct message to an offline node while a Unit C6L had MQTT
  enabled falsely displayed the gray "heard by mesh" checkmark and never timed
  out with an `X`. The service was mistaking the node's local MQTT broker
  publish ACK for an over-the-air radio relay confirmation.
- Wi-Fi failing to auto-reconnect when returning home: after giving up out of
  range of known routers, stopping the radio overwrote the `FAILED` state with
  `OFF`, preventing the background auto-reconnect timer (which only checked for
  `FAILED`) from ever scanning again until the user manually opened Settings >
  Wi-Fi. Stopping the radio after a failed attempt now preserves the failure
  reason, background reconnect runs whenever Wi-Fi is enabled in Settings and
  disconnected, and waking the screen triggers an immediate scan. A connection
  attempt that gets no response from the C6 within 60 s now also times out into
  the retry loop, and running `varrer` in the console restores the connection
  when finished.
- Wi-Fi CSI and Sniffer: rapidly re-enabling either service right after
  disabling it could spawn duplicate tasks (CSI) or leave the sniffer disabled
  with no MAC table after returning success. Both now synchronize task teardown
  via `kernel/service_stop.hpp`.
- Mesh: channel configs and the NodeDB are no longer written to the SD card on
  the radio receive loop (where a slow SD card caused dropped serial frames from
  the node); they are now handed off to the asynchronous SD writer task.
- Background jobs (Trail, Azimuthal): stopping a job now awaits task exit on a
  semaphore instead of spinning in a 10 ms UI loop, sharing the same stop
  primitive as system services.
- Power & performance: the UI task now wakes via FreeRTOS task notifications
  (instead of a queue); the physical keyboard is only polled over I2C when its
  interrupt pin asserts (once verified working); the GPS reader blocks on the
  UART driver event queue; ESP-NOW sleeps for 30 s when the C6 link is absent;
  Settings pages only schedule periodic redraws at the rate required by the open
  sub-page; and the home screen wallpaper buffer stays resident while on the
  home screen.
- Memory: removed the permanent Bluetooth task (−4 KB of internal SRAM);
  enabling/disabling BLE now borrows a slot from the shared worker pool.
- Replaced duplicate point-in-rect and millisecond tick helpers across apps with
  shared `tab5_dentro()` and `tab5_agora_ms()`.
- `sdkconfig.defaults`: set `CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2`
  (builds with an older `sdkconfig` continue to work; run `idf.py menuconfig` or
  delete `sdkconfig` to pick up the dedicated notification slot).
- Mesh: stopping the service destroyed its main mutex while the home status bar
  or Mesh app could still be holding or waiting on it (during transport switches
  or Grove re-probes). The mutex is now allocated once and kept for the lifetime
  of the OS.
- Wi-Fi: reconnecting after a disconnect was sleeping for 1 s *inside* the
  system event task (`sys_evt`), freezing all ESP-IDF event delivery (IP,
  `esp_hosted`). Moved the delay onto the dedicated Wi-Fi task.
- Wi-Fi: ESP-NOW, CSI, and Sniffer were updating the shared "keep receiver
  awake" bitmask non-atomically and could clobber one another's bits.
- UTF-8 string truncation now splits strictly on codepoint boundaries across the
  entire OS via a single helper (`tab5_cortar_ao_caber`), replacing a dozen ad-hoc
  implementations — three of which sliced mid-codepoint and drew garbage glyphs
  on accented words (status bar, home dock, Files browser).
- On-demand mutex initialization (`tab5_mutex_travar`) is now race-free on first
  call; migrated GPS, IMU, MQTT, and Azimuthal to use it.
- Serial console commands that open or close apps now wait for an opening app to
  finish `ao_abrir` before issuing `ao_fechar` (preventing `ao_fechar` from
  running on an app that never finished opening).
- Power savings during sleep: the IMU is no longer polled over I2C every second
  while the screen is off, the system monitor samples every 10 s instead of 1 s
  and skips frame stats while the panel is torn down, the PM lock table is
  dumped only on the first sleep entry, and battery power is computed as `V × I`
  instead of performing a third INA226 register read.
- Settings: dragging the brightness or volume slider now writes to NVS flash
  only once after the drag settles (1 s debounce) instead of on every single
  frame of the drag gesture.
- Memory: the 1.8 MB PSRAM scratch buffer allocated if a draw occurs while the
  panel is torn down is now freed as soon as the panel wakes back up; moved
  monitor history and home icon order buffers out of internal SRAM.
- USB mass storage when the cable is unplugged without ejecting: the session
  stays open (in case the host OS still has cached writes), but once the USB bus
  has been idle for more than 2 s, the screen is allowed to sleep and the CPU
  drops back from maximum clock — previously it stayed pinned awake at max clock
  forever on battery. Similarly, if enabled before plugging in a cable, the max
  clock boost times out after 10 s and resumes once a host configures the USB
  device.
- USB mass storage on reclaim: if the TinyUSB task took longer than 2 s to stop,
  the SD card was previously handed back to the Tab5 while USB could still be in
  the middle of a sector read/write (use-after-free). Reclaiming now waits for
  the USB task to confirm exit.
- USB mass storage: requesting "reclaim card" while the USB session was still
  initializing was silently ignored; the request is now latched and honoured as
  soon as initialization completes.
- Freed the 48 KB PSRAM card snapshot when the SD card returns from USB loan or
  when loan setup aborts (previously leaked until reboot).
- Journal replay: applying a journal entry no longer places 512 bytes of path
  buffers on the caller's stack (paths now live inside the PSRAM journal block).
- USB cable watchdog: when "auto-start USB drive on plug" is disabled (the
  default), polls every 1 s instead of every 250 ms.
- Wi-Fi now automatically reconnects to known networks after extended outages.
  Previously, after 5 failed retries (walking out of range or a router rebooting)
  the radio gave up permanently, and even opening Settings > Wi-Fi saw the known
  network in scan results and turned the radio right back off. Now, as long as
  Wi-Fi is enabled in Settings, it scans periodically with exponential backoff
  (30 s, 1, 2, 4, and then every 5 min) and connects to the strongest known
  network found; scanning from the UI also reconnects.
- Mesh: if spawning the service task failed, the UART driver and Grove ownership
  remained locked until reboot; both are now released on error.
- MQTT bridge: the Brazil and US MQTT clients were decrypting and assembling
  packets into the **exact same** `static` buffers concurrently from two
  different tasks, corrupting each other's packets when messages arrived
  simultaneously. Each client instance now owns its own buffers.
- MQTT bridge: an outgoing frame that exceeded the buffer size could be sent
  truncated on some paths because the payload length was computed even after the
  bounded writer failed.
- Notifications disappearing without opening the app: when waking into the lock
  screen (or while locked), the kernel was still delivering `ao_aviso` events to
  the obscured app behind the lock screen, causing ESP-NOW to mark incoming
  messages as read before the user unlocked the tablet. Obscured apps no longer
  receive events, touch, or keystrokes.
- Mesh: restarting the service (transport switch, Wi-Fi drop in internet mode,
  BLE disconnect, Unit C6L plugged into Grove) zeroed out unread counts and
  wiped the status bar notification. Per-conversation unread counts now persist
  across restarts until read, and re-registering a notification source preserves
  its state.
- Messages bridged from Telegram (or sent from modern smartphones) rendered as
  tofu rectangles whenever they contained characters outside Roboto's Latin-1
  glyph set. All drawn text now passes through `tab5_unicode` normalization
  first — converting mathematical bold/script letters (`𝗻𝗲𝗴𝗿𝗶𝘁𝗼`, `𝓈𝒸𝓇𝒾𝓅𝓉`) back
  to ASCII, regional indicator flag pairs to country codes (`BR`), stripping
  skin-tone modifiers and zero-width joiners, composing combining accents,
  transliterating Cyrillic and Greek, and replacing remaining unknown codepoints
  with `?`. Tested on macOS via `testes/rodar_unicode.sh`.
- Multi-line incoming messages were truncated when reloaded from the SD card's
  line-oriented history file; embedded newlines are now normalized to spaces on
  receipt.
- Mesh: sending two messages back-to-back froze the UI for up to 2.1 s because
  the 2-second firmware rate-limit delay ran synchronously on the UI task.
  Outgoing text messages are now queued to the service task, which paces
  transmissions in the background while immediate validation errors (BLE
  disconnected, DM in internet-only mode, US feed offline) still report
  synchronously.
- Mesh over Grove serial: UART frame reads waited the full 200 ms timeout even
  after a complete frame had already arrived (because it requested 256 bytes at
  once). Now wakes on the first byte and reads the exact frame length.
- Mesh over Bluetooth: draining the node's `FromRadio` mailbox stopped after 64
  messages and waited up to 5 s (or the next BLE notify) before continuing —
  making initial dumps on large meshes stutter. The service now continues
  draining on the very next loop iteration and processes all queued notifications
  per pass instead of just one.
- Mesh over internet (MQTT): drains up to 16 incoming packets per loop pass (up
  from 1), drains up to 8 proxy downlink messages per pass (up from 4), drops
  duplicate packets replicated by multiple MQTT gateways before running AES
  decryption, and rate-limits "queue full" warnings to once every 10 s while
  naming which queue overflowed.
- Mesh: the node table was being re-sorted on every drawn row whenever live
  traffic arrived; sorting is now throttled to at most once every 250 ms, and
  `AVISO_MESH_NOS` UI redraw events are capped at 4 Hz (1 Hz for US traffic).
- Mesh: expanded the duplicate-packet ring buffer from 64 to 256 packet IDs so
  delayed MQTT copies aren't accepted as new messages when high-volume US
  traffic is active alongside the local mesh.
- Mesh, Node tab: changing the node's MQTT or proxy settings over Bluetooth
  failed to save or reboot the node. Upstream Meshtastic firmware shuts down
  Bluetooth the instant it receives a modified `mqtt` config section — even in
  the middle of an open `begin_edit_settings` transaction — so the subsequent
  `commit_edit_settings` packet never arrived, leaving the transaction dangling
  and Bluetooth off until a manual power cycle. Over Bluetooth, `mqtt` section
  changes are now sent standalone outside a transaction so the node validates,
  saves to flash, and reboots on its own; if other edits are staged at the same
  time, the UI prompts to save them first. Grove serial behavior is unchanged.
  The Node tab also displays the rejection reason whenever a save fails.
- Mesh and ESP-NOW sometimes opened in landscape while holding the tablet
  upright. On launch, a stale or zeroed IMU reading (the kernel caches readings
  for 1 s, and the BMI270 takes a moment to produce its first sample after
  waking from low-power mode) fell into the dead zone, whose fallback defaulted
  to landscape. Launch now polls the IMU until it gets a plausible gravity
  vector, picks the dominant axis directly (without the hysteresis dead zone
  meant for live rotation), and falls back to keeping the current screen
  orientation if flat on a table. While open, the tilt threshold for rotating
  was lowered from 30° to ~24° so holding the tablet reclined still triggers
  rotation reliably.
- Wi-Fi CSI rejected by the C6 with `ESP_FAIL`: `esp_hosted` initializes the C6's
  Wi-Fi driver using the `wifi_init_config_t` sent by the P4, and the P4 was
  sending `csi_enable = 0`, causing the C6 driver to fail its menuconfig check
  (*"CSI not enabled in menuconfig!"*). The P4 now sets `csi_enable = 1` on
  Wi-Fi init (requiring no C6 reflash), and the C6 enables CSI through a single
  clean path including 802.11ax HE SU frames. Full root-cause analysis and proof
  in [RADIO_C6.md](RADIO_C6.md).
- 802.15.4 left Wi-Fi associated but unable to pass traffic due to two bugs.
  While active: missing `esp_coex_wifi_i154_enable()` coexistence registration.
  On shutdown: calling `esp_ieee802154_disable()` gated the MAC clock mid-RX
  while holding an RF coexistence grant; switched shutdown to
  `esp_ieee802154_sleep()` (matching OpenThread). `radio 154` now pings the
  router before, during, and after capture to verify both fixes.
- Wi-Fi CSI arrived rarely and truncated: 2.4 GHz router beacons (802.11b DSSS)
  contain no OFDM training symbols and produce no CSI, so `radio csi` now pings
  the gateway at 10 Hz during capture (matching `esp-csi`); raised the maximum
  record payload from 256 B to 1024 B and exposed the baseband frame format and
  the C6's `first_word_invalid` hardware flag.
- Wi-Fi FTM: `status 255` was our C6 wrapper reporting that
  `esp_wifi_ftm_initiate_session` returned `ESP_ERR_NOT_SUPPORTED` because
  ESP-IDF disables FTM initiator mode on ESP32-C6 silicon revisions v0.0 and
  v0.1 (hardware errata). The C6 now only advertises `C6_CAP_FTM` on silicon
  revisions that support it, and `farejo info` reports the C6 chip revision.
- Wi-Fi CSI rate limiting now uses fixed cadence buckets instead of elapsed time
  since the last accepted frame, preventing over-the-air jitter from dropping
  nearly half the replies.
- C6 firmware: replaced the fixed-item output queue with a variable-item ring
  buffer (handling 7 B to ~1 KB records without copying 1 KB structs onto the
  Wi-Fi task stack), and added dropped-message counters and 802.15.4 state to
  `farejo info`.
- Console: `espnow enviar` was only transmitting the first word of the message.
- Console: long listings (`mesh` with hundreds of MQTT nodes, `ble`, `lista`,
  `cartao`) starved core 0 for over 5 s and tripped the `IDLE0` task watchdog;
  they now yield every 8 items.
- Mesh, "Node" and Nodes tabs: fixed visual ghosting at the top and bottom of
  the list while scrolling. Newly exposed rows had an empty cached signature and
  were marked dirty on every drag sample; because dirty row indices span a
  contiguous range, every drag sample repainted all visible rows over the
  freshly scrolled pixels. Now only previously drawn rows are diff-compared, and
  every row on the "Node" tab fills its own background.
- Added console command `tela rolar cpu|dma` to benchmark front-to-back DMA2D
  scroll blits against CPU `copyRect` live without reflashing.
- Mesh: sending two text messages less than 2 s apart caused the node firmware
  to silently drop the second one (PhoneAPI `TEXT_MESSAGE_APP` rate limit; on
  2.7 it returns no error, leaving the message stuck at "no response"). Outgoing
  sends now wait out the remaining cooldown automatically.
- Mesh: serial frame headers and payloads were written in two separate
  `uart_write_bytes` calls while both the Mesh service task (heartbeats) and UI
  task (text, favorites, admin) wrote to the UART, allowing one frame to splice
  into the middle of another and corrupt both. All UART writes are now serialized
  by a TX mutex.
- Mesh: channel 0 (`LongFast`, the primary channel) config was ignored and
  disabled channels appeared active. In Protobuf v3, `Channel.index = 0` and
  `role = DISABLED (0)` are omitted on the wire, whereas our decoder initialized
  them to `-1` and `PRIMARY`. A deleted `LongFast` conversation could never be
  reopened, and disabled channels stayed in the sidebar.
- Mesh: channels disabled on the node are now hidden from the conversation
  sidebar (preserving their history on the SD card), and learned channel configs
  are persisted in NVS so they remain available when operating without the node.
- Mesh: live `NODEINFO_APP` and `POSITION_APP` packets from the mesh now update
  the node table immediately. Previously, node names, models, MACs, and
  positions were only populated from the initial config dump, leaving newly
  heard nodes displayed as raw hex IDs.
- Mesh: node display names now fall back cleanly from `short_name` → `long_name`
  → hex ID, showing the hex ID only when no human-readable name is known, and
  the conversation sidebar displays the node's long name on a second line.
- Mesh: gave the conversation sidebar a deterministic sort order (channels
  first, then direct messages ordered by most recent activity) and tracked the
  open conversation by its unique key rather than its array index — fixing a bug
  where a new message in an earlier slot shifted indices and caused the open
  window to display and send to the wrong conversation.
- Mesh: long text messages failed to send. Payloads over 233 bytes were silently
  dropped by the node, and payloads between ~220 and 233 bytes failed with
  `TOO_LARGE` (total LoRa packet limit is 255 bytes including headers and +12 B
  of PKI overhead on DMs). Capped compose length at 200 bytes (matching official
  clients), stopping input at the limit and truncating cleanly on UTF-8
  character boundaries.
- Mesh: fixed a race where a newly arrived message only appeared on screen after
  the *next* message arrived. The SD writer task swaps its double buffer before
  flushing to disk, so a reader calling `tab5_sd_abrir` during that window saw
  "no pending lines in buffer" and read the file before the flush completed.
  `tab5_sd_gravar_pendentes` now waits for any in-flight flush to finish, and
  the message counter only increments after the line is queued to the writer.
- Mesh: our own sent messages echoed back by the node during a config dump were
  being reset to "no response" and overwriting their true delivery status on the
  SD card.
- Mesh: the seen-packet ID ring buffer is no longer cleared when switching
  transports — preventing the same message heard over LoRa and then over the
  MQTT bridge from being appended twice.
- Mesh, Mesh status tab: channel utilization, airtime, and uptime now display
  `"-"` when no node telemetry has been received yet (instead of `"0"`), online
  node count is computed locally when `LocalStats` is absent, the active
  transport is shown on the link line, and our own node is shown by name.
- Mesh: reorganized the Nodes table with our own node pinned at the top (white),
  followed by directly heard RF nodes with SNR (yellow) **pinned in discovery
  order** so they don't jump around as packets arrive, followed by remaining
  nodes (MQTT in blue, no-SNR in gray) sorted from most recently heard to
  oldest. When the table fills up, non-pinned MQTT nodes are evicted first.
- Mesh: RF nodes were disappearing from the table within seconds when MQTT
  downlink was enabled on the Unit C6L. The old table had only 32 slots that
  MQTT traffic churned through immediately, and dump nodes with `last_heard == 0`
  all collided into a single eviction slot. Expanded the table to 256 slots in
  PSRAM with a 1024-entry name cache, full-age eviction prioritizing MQTT nodes
  for removal, and RF pinning applied immediately on packet receipt even when
  the Mesh window is closed.
- Mesh: styled channel filter buttons in the same smooth font as the node table,
  prefixed with channel index (`"ch0 LongFast"`) and a `"channels"` label.
- Mesh over internet (MQTT): direct messages were being encrypted with the
  shared channel PSK and broadcast readable to everyone on `LongFast`; DMs are
  now rejected in serverless MQTT mode because PKI public keys are unavailable.
- Mesh over internet (MQTT): `via_mqtt` and `hop_start` were being encoded as
  Protobuf fields 13 and 14 of `MeshPacket` instead of 14 and 15.
- Virtual keyboard: tracks each finger independently and emits the character on
  that specific finger's release — previously, typing rapidly with two thumbs
  dropped the first letter whenever the second thumb touched down before the
  first lifted, and an 80 ms global cooldown ate double letters like `"ss"`.
- Compose bar: now displays the last two wrapped lines of the draft message
  (previously anything past the second line was invisible) with a trailing
  cursor, wrapping by actual pixel width instead of a fixed 64-byte slice.
- Lowered minimum display brightness from 4% to 0.9% PWM duty cycle and mapped
  the brightness slider to a perceptual curve so the lower half of the bar has
  fine-grained steps. Added `brilho <1-255> [hz]` console command to test PWM
  frequencies without saving.
- Outgoing messages were being stamped with the wrong date because the system
  clock was accepting historical `last_heard` timestamps from node dumps as the
  current time. Only live mesh packets now discipline the clock, and messages
  recorded before a large clock jump are retroactively adjusted.
- `nos.txt` grew with duplicate node entries on every boot where the SD card
  wasn't mounted in time; it is now only rewritten after being read and is
  compacted automatically when duplicates accumulate.
- Camera "zoomed-in" field of view: the sensor's 1280×720 mode was cropping a
  central window (80% × 60% of the array) instead of binning. Switched the
  sensor to full 1600×1200 resolution; saved photos now capture the full sensor
  frame (1200×1600 portrait) and the live viewfinder no longer stretches the
  aspect ratio.
- Bluetooth was powering on at every boot whenever Meshtastic had BLE selected as
  its transport, even if Bluetooth was turned off in Settings. Now only the
  Bluetooth toggle powers on the BLE controller.
- If the BMI270 IMU failed to initialize at boot, it stayed dead until reboot;
  the kernel now retries initialization automatically.
- Azimuthal GPS app: during a cold GPS start, the skyplot stayed completely
  blank for minutes because satellites tracked without elevation/azimuth were
  ignored. Now displays the count of tracked satellites and a signal-strength
  bar chart per satellite right away.
- Azimuthal no longer forces the Grove 5V rail on by itself (which powered an
  undetected module behind the kernel's back); Grove 5V is controlled via
  Settings > Grove 5V, and the app displays a notice when the rail is off.
- Tapping a Bluetooth device in Settings gave no visual feedback.
- Color emoji failed to render when drawn through the compositor (the emoji hook
  was only attached to the physical display object) and had byte-swapped RGB565
  colors. Emoji now render at integer scale through the compositor with accurate
  colors.
- Virtual keyboard: auto-scales label fonts to fit narrow keys in portrait mode
  (where `ENVIAR` previously overflowed) and uses graphical arrow icons instead
  of ASCII `"^"` and `"<-"`.
- Meshtastic BLE PIN: a dynamic PIN entered via the on-screen keyboard is no
  longer overwritten as the permanent default PIN; if a saved PIN is rejected by
  the node, the system falls back to `123456` and prompts the user on screen.

## [0.7.0] - 2026-09-25

Initial public release.

### System
- Kernel with UI event loop, double-buffered display compositor, per-app hardware
  power profiles, display sleep, and background jobs.
- UI components: scrollable list, conversation view (chat bubbles, virtual
  keyboard), buttons, and smooth anti-aliased Roboto fonts with full accent
  support.
- Single batched SD card writer shared across all apps, daily system logs, and a
  background system worker for off-UI file reads.
- Paginated Settings app.

### Radio
- Wi-Fi and Bluetooth LE via the companion ESP32-C6 (`esp_hosted`; NimBLE host
  stack on the P4).
- ESP-NOW running on custom C6 firmware with persistent channel selection.
- Meshtastic over Grove serial cable or Bluetooth LE, with optional GPS position
  forwarding.

### Security
- Wi-Fi passwords encrypted on the SD card using a per-device hardware-derived
  key.

### Applications
- Mesh, ESP-NOW, Trail, Azimuthal, Camera, Files, Power, Tasks, IMU, Settings,
  CreazySnake, and Example.
