# M5tabOS API Reference

A reference to everything the system exposes to an application: each function,
what it does, and **which task context it may be called from**. The
[`Exemplo`](main/app_example/app_example.cpp) app demonstrates each of these
areas in its own interactive card. Open it on the device and read its source
code side by side with this guide.

For *how to think about building an app* (the drawing rules, the rationale
behind each one, and the bugs that taught us the hard way), read
[WRITING_APPS.md](WRITING_APPS.md). That document explains *why*; this one covers
*what*.

---

## Why go through the system API

Nothing physically stops an app from talking directly to the hardware:
`M5.Imu`, `Wire`, `fopen`, `xTaskCreate`, or a full-screen `M5.Display.fillRect`.
It will seem to work on the bench and then fail in the field, far from the root
cause. Every API below exists because taking the direct route broke in exactly
this way:

| Direct call | What goes wrong | System API |
|---|---|---|
| Reading an internal I2C sensor from another task | M5Unified's `I2C_Class` has no mutex: two tasks interleave transactions and read the **wrong register, with no error** | `tab5_imu_ultima`, `tab5_power_ultima` (cached copy, zero I2C traffic) |
| Drawing from an app task | LovyanGFX is not thread-safe: corrupted pointers and CPU exceptions | Update your state, then call `sistema_pedir_redesenho()` |
| `fopen(..., "a")` to write to SD | If the card is mounted on a PC (USB mass-storage mode), this corrupts the FAT filesystem or loses data | `tab5_sd_anexar` / `_acrescentar` / `_substituir` |
| `xTaskCreate` + `vTaskDelete` | Killing a task while it holds a mutex or blocks inside a driver leaks that resource forever | `tarefa_app_criar` / `tarefa_app_encerrar` |
| `M5.Speaker.tone()` | The audio codec powers down when idle, so the first beep comes out **silent** | `sistema_beep` |
| Large `fillRect` | Saturates PSRAM bandwidth, starves the display FIFO, and makes the screen **flash blue** | `tab5_tela_limpar` |
| Turning hardware on in `ao_abrir` via `tab5_power_*` | Cuts power out from under another service or leaves hardware running after your app closes | `app_t.perfil` (the system merges profiles and cleans up automatically) |

**The three-task model**, which governs the "Task context" column in the tables
below:

- **UI task**: the only task allowed to draw and the only task that touches the
  internal I2C bus. It runs `ao_abrir`, `ao_fechar`, `desenhar`, `ao_tocar*`,
  `ao_teclar`, and `ao_aviso`. Nothing running here may block (no SD card I/O,
  network calls, or long waits).
- **App tasks** (`tarefa_app`), **system worker** (`sistema_trabalho`),
  **background jobs** (`background.h`), and **serial console**: perform slow or
  blocking work, update shared state (via atomics, queues, or mutexes), and
  request a redraw.
- **System services** (Mesh, ESP-NOW, Wi-Fi, GPS, etc.): run on their own tasks
  and expose data by value (`tab5_*_msg`, `tab5_*_posicao`) along with event
  flags (`AVISO_*`).

In the **Task context** column: **UI** = UI task only; **any** = safe from any
task; **non-UI** = any task *except* the UI task (because the call blocks).

---

## 1. The application descriptor: `kernel/app.h`

An app is a `main/app_<name>/` directory that defines
`extern const app_t APP_<NAME>`. The build system discovers apps automatically
(`main/CMakeLists.txt`) — there is no central registry to edit.

| Field | Purpose |
|---|---|
| `nome`, `cor`, `icone` | Home screen identity (`icone` is optional: custom icon drawing callback) |
| `perfil` | Hardware **this** app requires to be powered on (`imu`, `alto_falante`, `microfone`, `camera`, `grove_5v`, `usb_a_5v`). Everything else stays off unless needed by the system |
| `usa_sd` / `exige_sd` | Mounts the SD card before `ao_abrir` / refuses to open the app if the card is missing or mounted over USB |
| `orientacao` | `ORI_EM_PE` (portrait), `ORI_DEITADO` (landscape), `ORI_SEGUE_IMU` (auto-rotate) |
| `piso_repouso_mhz` | Minimum CPU clock while the screen is asleep (`0` = lowest possible) |
| `ao_abrir` / `ao_fechar` | Allocate / free **everything**. A closed app must have zero runtime cost |
| `ao_repouso` / `ao_retomar` | Release and re-acquire hardware that cannot coexist with a torn-down display panel (e.g., camera DMA) |
| `desenhar(tela, area)` | The only callback that draws to the screen |
| `ao_tocar(x, y)` | Tap handler (fired on finger release), coordinates relative to `area` |
| `ao_tocar_nivel(x, y, pressionado)` | Polled every UI cycle while a finger is down (games, drag gestures, lists) |
| `ao_teclar(texto, codigo, mod)` | Physical keyboard input, already decoded to UTF-8 |
| `periodo_ms` / `periodo_agora` | Periodic redraw interval (prefer `0` + event flags whenever possible) |
| `avisos` / `ao_aviso` | Bitmask of `AVISO_*` flags: when a service signals an event, the app redraws immediately |
| `altura_conteudo` | If larger than `area.altura`, the system scrolls the entire app viewport automatically |
| `ao_girar` | Fired when screen orientation changes (`ORI_SEGUE_IMU`) |
| `comandos` | Custom serial console commands provided by this app (see Section 12) |

---

## 2. Redrawing and geometry: `kernel/app.h`

| Function | Task context | What it does |
|---|---|---|
| `sistema_pedir_redesenho()` | any | A **value** changed: schedules an incremental redraw on the next frame |
| `sistema_pedir_redesenho_completo()` | UI | The **screen layout** changed (page switch, layout change): clears and repaints without flicker |
| `sistema_area_foi_limpa()` | UI, inside `desenhar` | `true` = the app viewport was just cleared (app opened or woke from sleep): repaint everything |
| `sistema_area()` | UI | Returns the app's drawable area; **never** hardcode status/navigation bar heights |
| `sistema_quadro_dt()` | UI, inside `desenhar` | Elapsed seconds represented by this frame (for frame-rate-independent animation) |
| `sistema_avisar(AVISO_*)` | any | Broadcasts an event flag from a service to any open app listening for it |
| `sistema_acordar_interface()` | any | Wakes the UI task out of its sleep wait immediately |
| `sistema_zerar_rolagem()` / `sistema_travar_rolagem(b)` | UI | Resets or locks whole-viewport scrolling (`altura_conteudo`) |
| `sistema_superficie(r, &s)` / `_mudou(r)` | UI | Provides a DMA-capable RGB565 surface for direct image writers (e.g., camera) |
| `sistema_app_atual()`, `sistema_bloqueado()` | any | Queries which app is currently open, or whether the lock screen is covering it |

Built-in event flags: `AVISO_MESH_MSG`, `AVISO_MESH_ENVIO`, `AVISO_MESH_NOS`,
`AVISO_GPS`, `AVISO_TRILHA`, `AVISO_TRABALHO`, `AVISO_ESPNOW`.

> **Example:** every card in `app_example` calls `sistema_pedir_redesenho()`
> after mutating state, and `.avisos` subscribes to
> `AVISO_TRABALHO | AVISO_ESPNOW | ...`.

---

## 3. Drawing

### `tab5_ui.hpp`: ready-made UI helpers

| Function | What it does |
|---|---|
| `tab5_fonte_corpo(d)`, `tab5_fonte_corpo_negrito(d)`, `tab5_titulo(d, px)` | Selects anti-aliased system fonts (Roboto with accented glyphs) |
| `tab5_texto(d, n)` | Selects logical text size `N` (`N×8` px tall) using the nearest native pre-rendered font height |
| `tab5_botao(d, x, y, l, a, txt, cor)` | Draws a standard button; saves and restores `TextStyle` automatically |
| `tab5_celula(d, cx, y, rotulo, valor, cor, sub)` | Draws a "small label on top, large value below" metric cell |
| `tab5_dentro(x, y, rx, ry, rl, ra)` | Point-in-rectangle hit test |
| `tab5_cortar_ao_caber` / `_com_reticencias` | Truncates a UTF-8 string cleanly at a character boundary to fit within `N` pixels |
| `tab5_linha_grossa(d, ...)` | Anti-aliased thick line that respects clipping (use instead of `drawWideLine`) |
| `tab5_sprite_novo` / `tab5_sprite_soltar` | Allocates a sprite guaranteed to live in PSRAM (returns `nullptr` on failure) |
| `SIS_FUNDO`, `SIS_LINHA`, `SIS_BORDA` | Standard system color palette (`app.h`) |

### Differential painting: `kernel/paint.hpp`

Call `pintura_iniciar(&p)` at the top of `desenhar`; check `pintura_completa(&p)`
to draw static chrome that never changes; use `pintura_mudou_i/_s(&p, &cached, current)`
for each dynamic value. Never chain `pintura_mudou_*` calls with `||` (see
[WRITING_APPS.md](WRITING_APPS.md), Section 2.3).

### Clearing large areas: `hal/tab5_screen.hpp`

`tab5_tela_limpar(x, y, l, a, cor)` (UI): clears a region in strips so PSRAM
bandwidth is never saturated and the screen never flashes blue. Use it for any
region that **can** exceed roughly 40 scanlines in height.

### Scrollable lists: `kernel/list.hpp`

The system's virtualized scrollable list: measures each item once, scrolls via
hardware-accelerated pixel copies, and repaints only the specific item that
changed (`lista_item_mudou`). The entire Example app is built as a single list.

```c
lista_definir(&l, ROLAGEM_INICIO, n, altura_de, desenhar_item, ctx);   // in ao_abrir
lista_desenhar(&l, d, area, TFT_BLACK, sistema_area_foi_limpa());       // in desenhar
if (lista_arrastar(&l, x, y, pressionado)) sistema_pedir_redesenho();   // in ao_tocar_nivel
int i = lista_item_tocado(&l, x, y, &dy);   // in ao_tocar; <0 = miss or end of drag
```

### Conversation view and virtual keyboard: `kernel/conversation.hpp`, `kernel/virtual_keyboard.hpp`

`conversa_t` is a complete messaging view (chat bubbles, history, compose bar,
and keyboard); the app only provides callbacks to count, read, and send
messages. `tecvirt_*` exposes the standalone on-screen keyboard for apps that
just need text input. Both the Mesh and ESP-NOW apps are built on top of these.

---

## 4. Touch and keyboard input: `kernel/app.h`, `hal/tab5_keyboard.hpp`

| Function | Task context | What it does |
|---|---|---|
| `sistema_toques(out, max)` | UI, inside `ao_tocar_nivel` | Returns all active touch points in the current sample (multitouch) |
| `sistema_consumir_toque()` | UI | Marks the current touch gesture as handled so releasing the finger won't fire `ao_tocar` |
| `sistema_travar_navegacao(b)` | UI | Hands the bottom navigation bar area over to the app (used by the virtual keyboard) |
| `tab5_teclado_presente()` | any | Returns `true` if the physical ExtPort1 keyboard is attached |
| `tab5_teclado_ascii(txt, out, n)` | any | Decomposes accented UTF-8 characters into plain ASCII for legacy protocols |

Physical keystrokes arrive via `ao_teclar`. Special key codes:
`TAB5_TECLA_ENTER`, `_APAGAR`, `_DEL`, `_ESC`, `_TAB`, `_ESQUERDA`, `_DIREITA`,
`_CIMA`, `_BAIXO`; modifier flags: `TAB5_TECLA_CTRL`, `SHIFT`, `ALT`.

---

## 5. Off-UI background work

| API | When to use | Task context |
|---|---|---|
| `tarefa_app_criar(&t, corpo, nome, pilha, prio)` / `tarefa_app_encerrar(&t, ms)` (`kernel/app_task.h`) | Continuous loop that runs while the app is open | UI (`ao_abrir` / `ao_fechar`) |
| `sistema_trabalho(fn, arg)` (`kernel/work.hpp`) | One-shot slow operation (reading a file, building a directory listing); completion arrives via `AVISO_TRABALHO` | any |
| `sistema_trabalho_cancelado()` | Checked inside a worker callback: returns `true` if the app closed and the job should abort early | worker |
| `sistema_trabalho_solto(fn, arg)` | Fire-and-forget job that takes a few seconds and shouldn't block app close (e.g., saving a photo); owns `arg` | any |
| `sistema_fundo_iniciar/parar/ativo(&def)` (`kernel/background.h`) | Work that **keeps running after the app closes** (GPS track recording, stopwatch) | UI |
| `fundo_esperar(f, ms)`, `fundo_ceder(f)`, `fundo_estado(f, n, txt)` | Helpers called from inside a background loop | background task |

The three golden rules for background loops: never wait with `portMAX_DELAY`
(stop requests must be able to wake the task); never draw to the screen; and
own your memory (never point into buffers owned by the UI window).

> **Example:** see the TAREFA (task + length-1 queue), TRABALHO (counts files
> on SD and signals `AVISO_TRABALHO`), and FUNDO (stopwatch that stays visible
> in the status bar after closing the app) cards.

---

## 6. SD card: `hal/tab5_sd.hpp`

Each app gets its own directory at `/sdcard/apps/<Name>/`, created automatically.

| Function | Task context | What it does |
|---|---|---|
| `tab5_sd_anexar(app, arq, linha, cabecalho)` | any | Appends a line **without blocking** (queued for batched write) |
| `tab5_sd_acrescentar(app, arq, dados, n)` | non-UI | Appends a binary/text block synchronously |
| `tab5_sd_substituir(app, arq, dados, n, flags)` | non-UI | Atomically replaces an entire file (never leaves a half-written file); `TAB5_SD_SIGILOSO` zeroes out the old file first |
| `tab5_sd_abrir(app, arq, "r")` | non-UI | Opens a file for **reading** (or writing a large new file when `exige_sd` is set) |
| `tab5_sd_pasta(app, buf, n)` | non-UI | Formats the full path to the app's SD folder |
| `tab5_sd_montado()`, `tab5_sd_espaco(&t, &l)` | any | Queries mount status and total/free space |

When the SD card is loaned to a computer over USB, all three write APIs
transparently journal writes to internal flash and replay them onto the card in
order when it returns. **Only overwrite a file if you successfully read it
first.**

> **Example:** see the CARD tile (`tab5_sd_anexar` with automatic CSV header
> creation).

---

## 7. Notifications: `services/notification.h`

Notifications represent **state, not a queue**: each source holds a single
current state, and publishing again overwrites the previous state.

```c
int id = notif_registrar("MeuApp");                 // idempotent by name
notif_associar_app(id, "MeuApp");                   // tapping the notification opens the app
notif_publicar(id, 3, NOTIF_ATENCAO, "3 alerts");   // safe from any task
notif_limpar(id);                                   // equivalent to publishing count = 0
```

Severity levels: `NOTIF_ESTADO` (ongoing background activity), `NOTIF_INFO`,
`NOTIF_ATENCAO` (directed at the user), `NOTIF_URGENTE` (requires immediate
action). The status bar highlights whichever active source has the highest
priority.

---

## 8. Power, sound, and display

| Function | Task context | What it does |
|---|---|---|
| `app_t.perfil` | (declarative) | Declares which hardware rails the app needs; merged with user settings by the kernel |
| `sistema_reaplicar_perfil()` | UI | Re-evaluates and applies power profiles immediately after changing a system setting |
| `sistema_manter_acordado(SISTEMA_ACORDADO_APP, b)` | any | Prevents idle screen sleep (automatically released when the app closes) |
| `tab5_power_ultima()` | any | Latest battery reading: voltage, mA, mW, % (cached copy, no I2C transaction) |
| `sistema_beep(hz, ms)` | UI | Plays a tone (requires `perfil.alto_falante = true`) |
| `tab5_painel()`, `tab5_painel_nome()` | any | Identifies which of the three Tab5 display panels is installed (ILI9881C, ST7123, ST7121) |
| `tab5_power_cpu_mhz()` | any | Returns the current CPU clock in MHz (360 MHz max on P4 v1.x, 400 MHz on v3.x) |

> **Example:** see the ENERGIA, SOM, and ACORDADO cards.

---

## 9. Sensors and time

| Function | Task context | What it does |
|---|---|---|
| `tab5_imu_ultima(&d)` (`hal/tab5_imu.hpp`) | any | Latest accelerometer (g) and gyroscope (°/s) reading; requires `perfil.imu = true` |
| `tab5_gps_no_ar()`, `tab5_gps_posicao()`, `tab5_gps_tempo()` (`hal/tab5_gps.hpp`) | any | Grove GPS state and fix; lat/lon are zero when there is no fix |
| `tab5_gps_assinar(fila)` | any | Subscribes a FreeRTOS queue to receive every parsed GPS sentence |
| `tab5_sensores_temp(&c)` (`hal/tab5_sensors.hpp`) | any | Internal SoC die temperature in °C |
| `tab5_hora_epoch()` (`services/tab5_time.hpp`) | any | Current UTC Unix timestamp, or `0` if the clock has not been set yet |
| `tab5_hora_local(epoch, &tm)` | any | Converts UTC epoch to local time (applies time zone) for **display**; always store timestamps in UTC |
| `tab5_hora_fonte()` | any | Reports which time source last disciplined the clock (GPS, NTP, mesh, RTC) |
| `tab5_agora_ms()` | any | Monotonic millisecond tick counter for measuring elapsed intervals (compare by subtraction) |

> **Example:** see the HORA, IMU, and GPS cards.

---

## 10. Networking

### Wi-Fi: `hal/tab5_wifi.hpp`

| Function | Task context | What it does |
|---|---|---|
| `tab5_wifi_estado()`, `tab5_wifi_descricao()` | any | Connection state enum and human-readable status string |
| `tab5_wifi_ssid()`, `tab5_wifi_ip()`, `tab5_wifi_rssi()` | any | Details of the active connection |
| `tab5_wifi_ligar()` / `_desligar()` | UI | Powers Wi-Fi on/off (normally controlled by the user in Settings, not by individual apps) |
| `tab5_wifi_procurar()`, `tab5_wifi_vista(i, &v)` | any | Triggers a scan and reads discovered access points |
| `tab5_wifi_gravar_credenciais(ssid, senha)` | non-UI | Saves network credentials to SD (password is encrypted) |

Once Wi-Fi is connected, standard ESP-IDF networking APIs (`esp_http_client`,
BSD sockets, `esp-mqtt`) work normally.

### ESP-NOW: `services/tab5_espnow.hpp`

Requires the custom C6 firmware (`c6/`, see [RADIO_C6.md](RADIO_C6.md)).

| Function | Task context | What it does |
|---|---|---|
| `tab5_espnow_pronto()`, `tab5_espnow_canal()` | any | Checks whether ESP-NOW is active and returns its current Wi-Fi channel |
| `tab5_espnow_enviar(texto)` | any | Broadcasts a text frame without blocking |
| `tab5_espnow_n()`, `tab5_espnow_msg(i, &m)` | any | Reads in-RAM message history by value |
| `tab5_espnow_janela(b)` | UI | Notifies the service that its UI window opened/closed so the receiver stays awake |

Event flag: `AVISO_ESPNOW`.

### Mesh (Meshtastic): `services/tab5_mesh.hpp`

See [MESHTASTIC.md](MESHTASTIC.md). Runs as a persistent system service: radio
links, node table, and message history live in the service, while the Mesh app
is purely a front-end window.

| Function | Task context | What it does |
|---|---|---|
| `tab5_mesh_servico_ativo()`, `tab5_mesh_no_ar()` | any | Checks service and link state |
| `tab5_mesh_num_nos()`, `tab5_mesh_no(i, &no)` | any | Reads known mesh nodes by value |
| `tab5_mesh_num_msgs()`, `tab5_mesh_msg(i, &m)` | any | Reads message history (`0` = newest) |
| `tab5_mesh_nao_lidas()` | any | Total unread message count |
| `tab5_mesh_enviar(para, canal, texto)` | any | Sends a message (`para = 0xFFFFFFFF` broadcasts on `canal`; any other node ID sends a DM) |
| `tab5_mesh_estado_envio(id, &erro)` | any | Queries delivery status (ACK, mesh heard, or error code) |

Event flags: `AVISO_MESH_MSG`, `AVISO_MESH_ENVIO`, `AVISO_MESH_NOS`.

### Bluetooth: `hal/tab5_ble.hpp`

`tab5_ble_pronto()`, `tab5_ble_varrer(ms)`, `tab5_ble_visto(i, &v)`. Turning
Bluetooth on and off is managed by the user (`sistema_bluetooth`).

> **Example:** see the WIFI, ESP-NOW (with broadcast button), and MESH
> (read-only so the example app doesn't spam a public channel) cards.

---

## 11. Camera: `kernel/app.h`, `hal/tab5_camera.hpp`

| Function | Task context | What it does |
|---|---|---|
| `sistema_camera_iniciar(l, a)` / `sistema_camera_parar()` | any | Starts/stops the camera pipeline (reference-counted so multiple consumers can coexist) |
| `tab5_camera_definir_destino(&d)` | UI | Sets the destination buffer for live frames (use `sistema_superficie`) |
| `tab5_camera_pedir_foto(buf)`, `tab5_camera_foto_pronta(&ok)` | any | Captures a full-resolution still frame |

The camera and the MIPI-DSI display share DMA infrastructure: always release
capture in `ao_repouso` and restart it in `ao_retomar`. See the Camera app for
a complete reference implementation.

---

## 12. App console commands: `app_t.comandos`

The serial console (`idf.py monitor`, type `help`) belongs to the system, but
any app can register its own subcommands. They appear under the **APPS** section
of `help` and are always invoked with `app <app_name>` in front to prevent name
collisions:

```
app exemplo contar 5
```

```c
static void cmd_contar(int argc, char** argv) { ... }   // argv[0] = "contar"

static const app_comando_t COMANDOS[] = {
  { "contar", "[n]\tadds n to the counter", cmd_contar, false },
  { "zerar",  "\tresets counter (only when app is open)", cmd_zerar, true },
  { nullptr },
};
// in app_t:  .comandos = COMANDOS,
```

Console handlers run on the **console task**, so all the rules of non-UI tasks
apply: never draw directly, synchronize shared state via atomics or mutexes, and
call `sistema_pedir_redesenho()`. Setting `so_aberto = true` tells the system
to reject the command unless the app is currently open — always set this for
commands that touch resources allocated in `ao_abrir`.

---

## 13. Hardware differences across Tab5 revisions

A single firmware binary runs across all **three display panels** shipped on
the Tab5 (auto-detected by M5GFX). The system automatically adapts touch
watchdog behavior, sleep teardown, and panel pixel clock (see `tab5_painel()`
in `hal/tab5_power.hpp`). An app that draws through `M5.Display` and handles
input via `ao_tocar` does not need to care which panel is installed.

The **SoC revision** is different: ESP32-P4 v1.x and v3.x require separate
binaries (`sdkconfig.defaults.chip_v1` / `_v3`). `./flash.sh` detects the
connected chip revision and builds the matching binary automatically. To build
for v3.x manually:

```bash
idf.py -B build_v3 -DM5_CHIP=v3 build
```

Application code should never depend on the chip revision. If you ever truly
need to check it at compile time, test `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`
(defined on v1.x) from `sdkconfig.h`.
