# M5tabOS

An application-centric operating system for the **M5Stack Tab5** (ESP32-P4 + ESP32-C6),
written in C++ on top of **ESP-IDF 5.5** and **FreeRTOS**, featuring a custom
M5GFX user interface.

> *An app-based operating system for the M5Stack Tab5, built on ESP-IDF 5.5 and
> FreeRTOS.*

Current version: **0.7.0** (see [CHANGELOG.md](CHANGELOG.md)).

## Features

**System**
- Home screen with icons, a status bar (Wi-Fi, Bluetooth, ESP-NOW,
  battery, notifications), and a navigation bar with a clock.
- Double-buffered screen compositor with zero tearing and zero flicker.
- Standard UI components: scrollable list, conversation view (message bubbles,
  virtual keyboard, scrolling), buttons, and anti-aliased fonts (Roboto at
  native heights, with full accent support).
- Custom touch recognition, touch watchdog, and serial console diagnostics.
- Power management: display sleep, per-app power profiles (each app declares
  the hardware it needs), and battery monitoring via the built-in INA226.
- SD card: a single batched system writer shared across all apps, plus daily
  on-card logging.
- Multitasking: background app tasks (which keep running after closing the
  window), a system worker for slow reads, and per-task owner tracking.
- Serial console with built-in diagnostics (`help`).

**Radio**
- **Wi-Fi** via the ESP32-C6 (esp_hosted over SDIO).
- **Bluetooth LE**: the NimBLE host stack runs on the P4 using the C6 as the
  controller.
- **ESP-NOW**: runs on the C6 with custom firmware (`c6/` directory).
- **Meshtastic**: talks to a node (such as the Unit C6L) **over a Grove cable
  or Bluetooth**, and can forward GPS coordinates to the mesh.

**Built-in apps**

| App | Purpose |
|---|---|
| Mesh | Meshtastic messaging: conversations, nodes, and mesh status |
| ESP-NOW | Direct device-to-device chat between ESP32 units |
| Camera | Live viewfinder and BMP photo capture to SD card |
| Files | SD card file browser: plain text, CSV, and images |
| Power | Battery level, current draw, and daily usage charts |
| Tasks | FreeRTOS tasks, CPU usage, and memory, grouped by owner |
| IMU | Accelerometer and gyroscope readout |
| Settings | Brightness, volume, Wi-Fi, Bluetooth, ESP-NOW, Meshtastic, display, and more |
| Example | One card per system API — the template for writing a new app |

Any `main/app_<name>/` directory automatically becomes an icon on the home
screen without registering it anywhere: your app lives right next to these.

## Hardware

- **M5Stack Tab5**, with any of the three display panels it has shipped with
  (ILI9881C + GT911, ST7123, ST7121). M5GFX auto-detects the panel and the
  system adjusts accordingly; the ST71xx panels have not yet been tested on
  physical hardware (see *Known limitations*).
- **ESP32-P4 revision**: v1.x (all Tab5 units shipped so far) and v3.x require
  different binaries. `./flash.sh` queries the connected device's revision and
  builds the right one; to build manually, run `idf.py build` (v1.x) or
  `idf.py -B build_v3 -DM5_CHIP=v3 build` (v3.x). Per-revision settings live in
  `sdkconfig.defaults.chip_v1` and `_v3`.
- Optional peripherals:
  - FAT32 microSD card;
  - ExtPort1 physical keyboard;
  - Grove GPS module or Meshtastic node.

## Building and flashing

1. Install **ESP-IDF 5.5** ([official guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/)).
2. Set up your shell environment and build:

   ```bash
   source environment.sh
   idf.py build
   ```

   `environment.sh` sources `$IDF_PATH/export.sh` (defaults to `~/esp/esp-idf`).
   For a non-standard setup, create `environment.local.sh` (git-ignored).

3. Connect the Tab5 via USB-C and flash:

   ```bash
   ./flash.sh
   ```

   The script auto-detects the serial port (`/dev/cu.usbmodem*` on macOS,
   `/dev/ttyACM*` on Linux) and opens the serial monitor. To specify a port
   manually: `PORT=/dev/xxx ./flash.sh`.

### ESP32-C6 firmware (optional)

**The system works out of the box with the factory radio firmware.** Wi-Fi,
internet access, Meshtastic over Grove or MQTT, and all standard apps work
immediately. Updating the C6 firmware only unlocks three additional features:

| Feature | Factory radio | Updated radio |
|---|---|---|
| Wi-Fi and internet | yes | yes |
| Meshtastic over Grove or internet | yes | yes |
| ESP-NOW | no | yes |
| Bluetooth (and Meshtastic over BLE) | no | yes |
| Promiscuous sniffing and Wi-Fi CSI | no | yes |

Without the update, the UI screens for those features simply note "factory C6
radio: optional update", while everything else works normally.

If you want those three features, the custom firmware lives in `c6/` (built
against the same esp_hosted 3.0.9 version as the host) and requires an external
serial programmer because the C6 has no dedicated USB port.

The C6 is flashed through the board's programming header using a USB-to-UART
adapter (such as the M5Stack ESP32 Downloader):

1. **Power off the Tab5**; the programmer powers the C6 directly.
2. Plug the programmer into your computer via **USB-A**. A USB-C-to-USB-C cable
   often leaves the programmer unpowered.
3. First, **back up the factory firmware**:

   ```bash
   esptool.py -p /dev/xxx read_flash 0 0x400000 backup_c6.bin
   ```

4. Flash the custom firmware:

   ```bash
   c6/flash.sh
   ```

   The script auto-detects the programmer port (`/dev/cu.usbserial-*` on macOS,
   `/dev/ttyUSB*` on Linux). To override: `PORT=/dev/xxx c6/flash.sh`.

5. Power on the Tab5. The boot log should show
   `host=3.0.9 coprocessor=3.0.9 (match)`.

To restore the factory firmware, flash the backup back with
`esptool.py write_flash 0 backup_c6.bin`, or use M5Stack's official flashing tool.

For details on what runs on the C6 (promiscuous sniffing, CSI, FTM, 802.15.4),
how its Wi-Fi init configuration is controlled by the P4, and the post-flash
verification checklist, see [RADIO_C6.md](RADIO_C6.md).

## First-time setup

- **Wi-Fi:** in Settings > Wi-Fi, tap SCAN NETWORKS and pick your network. You
  will be prompted for the password on screen, which is saved **encrypted** on
  the SD card (`apps/Sistema/wifi.txt`) using a key derived from the device's
  unique hardware ID. Multiple networks can be saved; on boot, the system
  connects to whichever known network has the strongest signal. You can also
  edit the file manually (pairs of lines: SSID and password, or `-` for an open
  network; plain-text passwords are encrypted on the next boot) or configure it
  from the serial console: `wifi <ssid> <password>`.
- **Settings:** enable Wi-Fi, Bluetooth, and Grove 5V, and choose how to
  connect to your Meshtastic node.
- **Meshtastic over Bluetooth:** in Settings > Meshtastic, select BLUETOOTH.
  - On the first connection, the node will request a **PIN**. The default tested
    PIN is `123456`, standard for headless nodes.
  - If the node rejects it, the on-screen keyboard opens automatically so you
    can enter the PIN displayed on the node.

## Writing apps

The full API reference (display, touch, tasks, SD card, notifications, power,
sensors, Wi-Fi, ESP-NOW, Meshtastic, camera, console) is in [API.md](API.md).
For architectural guidance and hard-won lessons from bugs we hit along the way,
read [WRITING_APPS.md](WRITING_APPS.md). To create a new app:

1. copy `main/app_example/` to `main/app_<name>/`;
2. rename `APP_EXAMPLE` to `APP_<NAME>` (the symbol name must match the folder);
3. declare its power profile and delete any example cards you don't need.

An app can also register custom serial console commands
(`app <app> <command>`) by populating `.comandos` in its `app_t` descriptor.

Kernel architecture is documented in [ARCHITECTURE.md](ARCHITECTURE.md), and the
display compositor in [COMPOSITOR.md](COMPOSITOR.md).

## Directory layout

```
main/kernel/      core: UI loop, compositor, UI widgets (list, conversation, keyboard)
main/hal/         hardware: display, touch, power, SD card, Wi-Fi, Bluetooth, GPS, camera, IMU
main/services/    services: settings, notifications, logging, Meshtastic, ESP-NOW, energy log
main/app_*/       applications
c6/               ESP32-C6 coprocessor firmware (ESP-NOW + esp_hosted)
shared/           message contract shared between P4 and C6
tools/            generators (fonts, emoji, hardware table) and utilities
```

## Known limitations

- Support for ST7123 and ST7121 displays and ESP32-P4 v3.x compiles and uses a
  conservative fallback path (sleep only turns off the backlight; panel clock
  stays at factory defaults), but has not been tested on physical hardware yet.
  Feedback is welcome.
- The on-screen UI is in Portuguese, and the time zone is hardcoded
  (`TAB5_FUSO_S`, UTC−3) in `services/tab5_time.hpp`.
- The battery discharge curve (`hal/tab5_power.hpp`) was calibrated on a single
  unit (1306.8 mAh measured capacity). Other batteries may need recalibration.
- Bluetooth is BLE-only; the ESP32-C6 does not support Classic Bluetooth.
- Wi-Fi and ESP-NOW share a single-channel radio. While connected to an access
  point, ESP-NOW is locked to the router's channel.

## License

[MIT](LICENSE). Third-party components are covered by their respective licenses:
see [THIRD_PARTY.md](THIRD_PARTY.md).
