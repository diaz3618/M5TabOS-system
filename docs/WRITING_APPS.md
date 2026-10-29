# Writing Applications for M5tabOS

This guide is a distillation of every bug we hit the hard way. Nearly every rule
here exists because something broke in a subtle way that took hours to track
down — and each section explains what broke, so you don't have to repeat the
experience.

> **For the complete API reference** (every function, what it does, and which
> task context may call it), see [API.md](API.md). The
> [`Exemplo`](main/app_example/app_example.cpp) app exercises every single API
> in its own interactive card: the fastest way to start a new app is to copy
> its folder.

---

## 1. The bare minimum to exist

An application is **a directory inside `main/` prefixed with `app_`** containing
one or more `.cpp` files (the prefix makes it immediately obvious which folders
are apps as opposed to `main/kernel/`, `main/hal/`, and `main/services/` — see
the full directory tree at the bottom of this section). `CMakeLists.txt` uses a
recursive glob: simply creating the directory is enough; there is no build list
to edit.

```
main/
  app_meuapp/
    app_meuapp.cpp
```

At the bottom of your main source file, export an `app_t` descriptor:

```c
extern const app_t APP_MEUAPP = {
  .nome   = "MeuApp",
  .cor    = TFT_ORANGE,
  .perfil = { },              // the hardware THIS app needs powered on
  .usa_sd = false,
  .orientacao = ORI_EM_PE,    // or ORI_DEITADO, or ORI_SEGUE_IMU (Section 9)
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,      // can be nullptr
  .ao_fechar = ao_fechar,     // can be nullptr
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,      // can be nullptr
  .periodo_ms = 0,
};
```

> **`extern` is mandatory.** In C++, a `const` at file scope has **internal**
> linkage by default. Without `extern`, the generated `apps.cpp` table cannot
> see your symbol and the build fails at link time with an `undefined reference`
> error far from the cause.

There is no central app registry to maintain: `main/CMakeLists.txt` generates
the app table directly from the `app_*` directories, deriving the symbol name
from the folder name (`app_meuapp` → `APP_MEUAPP`). To remove an app, delete its
folder.

**Full repository layout, to show where everything belongs:**

```
main/
  kernel/     core.cpp, shell.cpp, apps.cpp (generated), app_task.*, paint.*, app.h
              (the contract between apps and the OS; owns service lifecycle)
  hal/        tab5_screen, tab5_power, tab5_grove, tab5_keyboard, tab5_sd,
              tab5_sensors, tab5_bus, tab5_wifi, tab5_gps, nmea
              (pure hardware abstractions - none exist solely because an app is open)
  services/   tab5_mesh, notification, settings, energy_log
              (state that must stay alive when the app window is closed - see Section 8)
  app_*/      each application in its own app_-prefixed directory
```

Dependency rule, from bottom to top: HAL never depends on services or apps;
services never depend on apps; the kernel never depends on a specific app (only
the reverse — apps `#include "app.h"`, never the other way around). An app may
freely use kernel, HAL, and service headers, but must never include another
app's headers.

---

## 2. The four rules of drawing

This is the most important section in this guide. **Almost every visual glitch
in the history of the project came from violating one of these four rules.**

### 2.1. Never clear-then-redraw to update text

Even with the compositor, erasing a box with `fillRect` and drawing text on top
wastes bandwidth and forces unnecessary region invalidations.

Instead, **overwrite in place** using an opaque background color and a
**fixed-width format string**:

```c
// WRONG - erases then redraws
d.fillRect(x, y, 200, 30, TFT_BLACK);
d.drawString(texto, x, y);

// RIGHT - overwrites in a single pass
snprintf(buf, sizeof(buf), "%-20.20s", texto);   // fixed width!
d.setTextColor(TFT_WHITE, TFT_BLACK);            // opaque background color!
d.drawString(buf, x, y);
```

If dynamic content **shrank** (for example, a list with fewer rows than on the
previous frame), clear **only the leftover strip at the bottom**, never the
entire viewport.

### 2.2. Text style is GLOBAL across the display, not private to your app

`setTextDatum`, `setTextSize`, `setFont`, and `setTextColor` mutate global state
on the shared `M5.Display` instance. **Between two calls to your app's
`desenhar()`, the system status bar renders** and leaves its own datum behind
(`middle_right`, used for the battery percentage).

Therefore: **always set datum, font, and size before drawing text**, not just on
the first full repaint.

```c
// WRONG - on incremental frames, text anchors wherever the status bar left it
if (first_time) { d.setTextDatum(top_left); }
d.drawString(buf, x, y);

// RIGHT
d.setTextSize(2);
d.setTextDatum(top_left);
d.drawString(buf, x, y);
```

If you write a helper function (like a custom badge or box) that changes text
style, **save and restore it by value** — because `getTextStyle()` returns a
const reference to the internal member:

```c
const lgfx::TextStyle estilo = d.getTextStyle();   // copy by value, not reference!
...draw...
d.setTextStyle(estilo);
```

### 2.3. Paint differentially with `paint.hpp`

```c
#include "paint.hpp"

static pintura_t s_p;
static int  s_ult_valor;
static char s_ult_nome[24];

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);                    // checks whether this pass is a full repaint

  if (pintura_completa(&s_p)) {
    ...titles, frames, static labels that never change...
  }

  if (pintura_mudou_i(&s_p, &s_ult_valor, valor)) {
    ...redraw only the number...
  }
}
```

Three critical pitfalls to avoid:

**`pintura_iniciar()` automatically checks whether the OS just cleared your
viewport.** Without it (or if you roll your own `s_full_redraw` boolean that
forgets to query `sistema_area_foi_limpa()`), **the screen comes back blank
after waking from sleep** — your app thinks nothing changed, even though the
entire framebuffer was wiped.

**Never chain `pintura_mudou_*` calls with `||`.** Short-circuit evaluation
skips the right-hand call whenever the left-hand side returns `true`, leaving the
second cached value stale and causing the classic "only updates every other
frame" bug:

```c
// WRONG - short-circuits!
if (pintura_mudou_i(&s_p, &a, x) || pintura_mudou_i(&s_p, &b, y)) { ... }

// RIGHT
const bool m1 = pintura_mudou_i(&s_p, &a, x);
const bool m2 = pintura_mudou_i(&s_p, &b, y);
if (m1 || m2) { ... }
```

**Change comparators are for deciding what to DRAW, not when to do I/O.** By
design, every `pintura_mudou_*` comparator returns `true` on every full repaint
pass. If you use one to decide "should I re-read the SD card?", and reading the
SD card requests a full repaint, you create an **infinite loop**. For I/O
decisions, compare your state variables directly.

### 2.4. Clear large regions in STRIPS

When you genuinely **must** clear a large region — switching internal tabs or
changing layouts — do not call `d.fillRect()` over the whole viewport. Call:

```c
#include "tab5_screen.hpp"

tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TFT_BLACK);
```

The reason isn't cosmetic; it's **memory bus bandwidth**. The MIPI-DSI panel
continuously streams ~110 MB/s out of PSRAM just to keep the image on screen. A
full-screen CPU `fillRect` blasts ~1.8 MB of writes into that same PSRAM as fast
as the CPU can issue stores. When those two bursts collide, the display
controller's internal FIFO runs dry and the panel flashes **solid blue**. The
ESP-IDF driver documents this exact symptom in its own source code
(`esp_lcd_panel_dpi.c:113`):

```c
// when an underrun happens, the LCD display may already becomes blue
ESP_DRAM_LOGE(TAG, "can't fetch data from external memory fast enough,
                    underrun happens");
```

`tab5_tela_limpar()` cooperates with the compositor and hardware PPA fill engine
to clear large areas without starving the display DMA.

> Historically, this blue flash showed up **when exiting an app back to the home
> screen**, never when opening one. The home screen is mostly solid black, and a
> giant black rectangle is the fastest, tightest write burst the CPU can
> generate. Opening an app writes just as many pixels, but broken up across
> dozens of smaller draw calls — so a sustained bus-saturating burst never forms.

**For small rectangles — a button, a table cell, a progress bar — regular
`d.fillRect()` is completely fine.**

**Watch out for "clear the leftover tail" rectangles.** Clearing only the unused
space below a shrinking list sounds small, but when a full list suddenly drops
to one item, that leftover rectangle spans half the screen. We had the blue
flash come back for that exact reason: the kernel cleared in strips, but apps
still had nine large `fillRect` calls scattered around, any one of which could
starve the bus on its own. If a rectangle **can** grow large, use
`tab5_tela_limpar()`.

**If you ever see a blue flash, grep the serial monitor for `lcd.dsi.dpi`.** The
driver logs every FIFO underrun at `ERROR` level. If that log line isn't there,
something else drew blue pixels.

---

## 3. Touch handling

The tap callback signature is `void ao_tocar(int x, int y)`, where `x` and `y`
are **relative to your app's viewport**. Always fetch the viewport dimensions
via `sistema_area()` — **never reconstruct them from hardcoded constants**:

```c
// WRONG - status bar and nav bar heights are private to the system shell
const area_t a = { 0, 0, M5.Display.width(), M5.Display.height() - 64 - 96 };

// RIGHT
const area_t a = sistema_area();
```

**Never perform blocking I/O inside `ao_tocar`.** Reading the SD card, talking
to slow hardware, or waiting on a network socket freezes the entire UI task
**before any visual feedback can be drawn**. Instead, set a flag or dispatch work
to the system worker (`sistema_trabalho`), and request a redraw:

```c
static bool s_needs_read = false;

static void ao_tocar(int x, int y) {
  if (...) { s_page++; s_needs_read = true; sistema_pedir_redesenho_completo(); }
}

static void desenhar(...) {
  if (s_needs_read) { read_from_card(); s_needs_read = false; }
  ...
}
```

### Requesting a redraw

| Call | When to use |
|---|---|
| `sistema_pedir_redesenho()` | A **value** changed in place |
| `sistema_pedir_redesenho_completo()` | The **screen layout** changed (page switch, selection change, modal opened) |

---

## 4. Power profiles

Your app's `.perfil` declares **which hardware rails your app needs powered on**,
never what other subsystems are allowed to turn off. The kernel **unions** your
app's profile with the active system services and user preferences:

```c
.perfil = { .grove_5v = true },   // this app uses the Grove port
```

| Field | Who controls it |
|---|---|
| `brilho` | **The system** (user preference). Do not set this in an app — it is ignored. |
| `radio` | **The system**, whenever the Wi-Fi / ESP-NOW / BLE service needs it |
| `grove_5v` | **Union** of your app's `.perfil` and the user's Settings toggle |
| `imu`, `microfone`, `alto_falante`, `camera`, `usb_a_5v` | Powered on while any open app or active background job requests them |

> This rule exists because we broke it early on: every app defaulted to
> `.radio = false`, so **opening any app cut power to the C6 radio** and dropped
> the active Wi-Fi connection mid-transfer.

`piso_repouso_mhz` sets the minimum CPU clock frequency while the display is
asleep. `0` means "allow dropping all the way to the minimum sleep clock".

### If your app modifies a system-wide preference

A settings app (like the built-in Settings app) changes preferences that affect
the entire OS. For those changes to take effect **immediately**, never poke the
hardware HAL directly:

```c
// WRONG - bypasses the kernel's "union of active profiles" rule
tab5_power_grove(false);

// RIGHT - update the saved preference and ask the kernel to recompute and apply
ajustes_definir_grove(false);
sistema_reaplicar_perfil();
```

> This rule came out of a real bug: toggling Grove 5V off in Settings didn't
> actually turn the 5V rail off — the rail stayed live until the user exited to
> the home screen. The app was calling `tab5_power_grove(true)` manually on one
> code path without a matching `false` path, so the UI said "OFF" while the pin
> still measured 5 V.
>
> The fix wasn't adding `tab5_power_grove(false)` to the app. The fix was
> **stopping apps from poking power rails directly**: only the kernel knows the
> full set of active consumers to compute the true union state.

---

## 5. App tasks

If your app needs a continuous worker loop while its window is open, **always
use `tarefa_app`**, never raw `xTaskCreate`:

```c
static tarefa_app_t s_t;

static void my_loop(tarefa_app_t* t) {
  while (tarefa_app_ativa(t)) {
    ...work...                          // with BOUNDED wait timeouts!
  }
  // clean up anything this task acquired: fclose, xSemaphoreGive, etc.
}

static bool ao_abrir(void) {
  return tarefa_app_criar(&s_t, my_loop, "meuapp", 4096, 3);
}

static void ao_fechar(void) {
  tarefa_app_encerrar(&s_t, 200);   // signals stop, waits up to 200 ms for exit, then returns
}
```

`tarefa_app_encerrar()` sets the stop flag, aborts any pending FreeRTOS delay
(`xTaskAbortDelay`), and **waits for the task to confirm exit** before returning
so you can safely destroy queues or drivers afterward. Killing a task abruptly
with `vTaskDelete` while it holds a mutex or sits inside `uart_read_bytes` leaks
that resource until reboot.

### Running multiple tasks in the same app

Yes, this is normal — just declare **one `tarefa_app_t` per task**, each with
its own loop function. For example, reading a UART GPS receiver and parsing
NMEA sentences:

```c
static tarefa_app_t s_t_reader;   // reads raw bytes from UART, nothing else
static tarefa_app_t s_t_parser;   // parses sentences published by the reader

static bool ao_abrir(void) {
  if (!tarefa_app_criar(&s_t_reader, reader_loop, "GPSRead",   4096, 3)) { return false; }
  if (!tarefa_app_criar(&s_t_parser, parser_loop, "NMEAParse", 4096, 2)) { return false; }
  return true;
}

static void ao_fechar(void) {
  tarefa_app_encerrar(&s_t_parser, 200);   // ORDER MATTERS: stop the consumer
  tarefa_app_encerrar(&s_t_reader, 200);   // first, so the producer doesn't push
}                                          // into a queue whose reader is gone
```

Why two tasks instead of one: **each task should do ONE job** at its own natural
cadence. Reading a hardware UART is blocking and latency-sensitive; parsing and
formatting can take longer without risking a UART FIFO overflow — and giving
them distinct priorities (`3` and `2` above) makes that contract explicit. Have
them communicate via a **FreeRTOS queue** (`xQueueCreate`), never via unlocked
globals: the exact same "UI task doesn't touch hardware directly" separation of
concerns applies between cooperating tasks.

Remember that stack budgets **add up** in internal SRAM: two 4096-byte tasks
consume 8 KB on top of the UI task (8 KB) and all running system services. That
isn't a reason to avoid a second task when you need one — it's a reason not to
spawn a third task "just because": if two queues solve the problem, two tasks
are enough.

---

## 6. SD card

At any time, the SD card might be **mounted on a computer over USB** (USB mass
storage / thumb-drive mode in Settings > USB). Because of that, reading and
writing files follow strict system rules — follow them and your app never has to
worry about whether the card is currently owned by the Tab5 or a PC.

**Writing data: always use the three system write APIs** (`hal/tab5_sd.hpp`).
They mount the card automatically and, if the card is currently loaned out over
USB, transparently record writes to the internal flash journal and replay them
onto the card in exact order as soon as it returns:

```c
// append a single line without blocking (batched; ideal for logs and history)
tab5_sd_anexar("MeuApp", "dados.csv", line, "date,value\n");   // header written if file is new/empty

// append a block synchronously (when you already batch data in memory)
tab5_sd_acrescentar("MeuApp", "dados.bin", block, n);

// atomically replace an entire file (never leaves a half-written file behind)
tab5_sd_substituir("MeuApp", "config.txt", text, n);
```

Because `acrescentar` and `substituir` block on flash/SD I/O, call them from the
system worker (`sistema_trabalho`) or an app task, never from the UI task.

**Only overwrite a file if you successfully read it first.** If your app stores
a list on the SD card and rewrites the whole file via `tab5_sd_substituir`, it
must have loaded that file first. If the card was unavailable at load time, do
not write your empty in-RAM list back out — doing so would wipe the user's saved
file when the card returns. (This is the exact pattern used by saved Wi-Fi
networks, the Mesh NodeDB, and deleted conversations.)

**Reading:** call `tab5_sd_abrir(app, filename, "r")` — it flushes any pending
batched writes for that file before opening, guaranteeing you always read your
latest writes. While the card is loaned to a computer over USB, it returns
`NULL` just as if no card were inserted; treat that as "no data available right
now". Internally it distinguishes "file does not exist yet" (normal on first
run) from real hardware errors.

**Declare your app's SD card requirement in `app_t`:**

- `.usa_sd = true` — the kernel mounts the card **before** `ao_abrir()` and
  unmounts it **after** `ao_fechar()`. The app still opens even if no card is
  inserted.
- `.exige_sd = true` — the app is pointless without direct SD access (e.g.,
  Files browser, Camera photo capture). While the card is mounted on a PC over
  USB, **the kernel intercepts app launch**, displays an explanation screen in
  its place, and launches the app automatically the moment the card is returned.
  Likewise, USB mass-storage mode refuses to start while an `exige_sd` app is
  open. Your app never has to handle the card disappearing underneath it.

Writing a **large new binary file** (like a captured BMP photo or downloaded
firmware image) is the sole exception where calling `tab5_sd_abrir(..., "wb")`
directly is appropriate — and any app that does so must declare
`.exige_sd = true`.

If you ever mount the SD card manually inside a background task, remember that
mounts are reference-counted: **every `tab5_sd_montar()` must be paired with a
`tab5_sd_desmontar()` on every return path**, including error paths:

```c
if (!tab5_sd_montar()) { return; }          // also returns false while loaned over USB
FILE* f = tab5_sd_abrir("MeuApp", "dados.csv", "r");
if (!f) { tab5_sd_desmontar(); return; }    // <- don't forget to unmount on error!
...
fclose(f);
tab5_sd_desmontar();
```

---

## 7. Exclusive hardware resources

Some hardware peripherals can only have **one owner at a time** and are guarded
by a system arbiter. Always request ownership, and **handle rejection
gracefully**:

```c
if (!tab5_grove_tomar(GROVE_DONO_GPS)) {
  // Another service owns the Grove port. Open anyway if useful, and EXPLAIN why on screen.
}
...
tab5_grove_soltar(GROVE_DONO_GPS);   // inside ao_fechar()
```

> Refusing to open with zero feedback is the worst possible UX: the user taps
> the icon and the tablet appears dead.

---

## 8. Application or system service?

This is the single most important architectural decision when adding a feature.

| | Application | System service |
|---|---|---|
| Lifetime | Only while visible on screen | As long as the OS is running (or the feature is enabled) |
| Examples | Files, Settings, Camera | Wi-Fi, Meshtastic, GPS, energy logger, keyboard |
| Use when | The work **is** the screen | State or I/O must continue when the window is closed |

If a feature needs to **notify** the user while its window is closed, or if
multiple apps need to query its data, it belongs in a **service** — and the app
becomes purely a **viewer window** for that service. That is how Meshtastic
alerts you to new messages while you're on the home screen, and it's the exact
reason GPS was promoted from an app-internal reader to a system service: the
Mesh app needed to ask "where am I right now?" to compute distance and bearing
to remote nodes, and one app can never reach into another app's private state.
Once GPS became a service, any app could query it — and the GPS viewer app
became just another window on top of it.

Services live in `main/services/` (or `main/hal/` when they wrap a pure hardware
stream like GPS — the dividing line is "business logic & state" vs. "parsed
hardware data"). Their tasks are started and stopped by the **kernel**
(`main/kernel/core.cpp`), never by an app, and they expose state exclusively
**by value copy**, never by returning raw pointers to internal structs that a
background task is mutating.

**Built-in building blocks for services — do not reinvent these:**

| When you need to... | Use | Instead of... |
|---|---|---|
| Stop a service task cleanly | `kernel/service_stop.hpp` (`parada_preparar` / `parada_pedida` / `parada_confirmar` / `parada_pedir`) | Two ad-hoc booleans + a `for (...) vTaskDelay(10)` polling loop |
| Deliver asynchronous events to a single listener | `hal/tab5_listener.hpp` — unregistering blocks until any in-flight callback finishes | A `volatile` function pointer + `vTaskDelay(40)` "just to be safe" before freeing state |
| Encode/decode Meshtastic Protobuf wire format | `services/mesh_pb.hpp` — bounds-checked reader and writer | Raw `*p++ = 0x22` pointer arithmetic into a "probably big enough" buffer |
| Check whether the system clock has been set | `tab5_hora_epoch()` / `tab5_hora_valida()` (`services/tab5_time.hpp`) | Hardcoding `time(nullptr) > 1600000000` |

The unifying principle behind all four: **wait for a synchronization event,
never for a magic timeout.** When tearing something down, wait on a semaphore or
completion signal; `vTaskDelay` is for hardware datasheet timing requirements and
yielding the CPU, never for "it's probably done by now".

### 8.1. Apps with a BACKGROUND JOB: `kernel/background.h`

Between a pure "foreground app" and a permanent "system service" sits a third
pattern: an app whose work needs to keep running after the user closes its
window — recording a GPS trail, uploading telemetry over Wi-Fi, or running a
long calculation. **Do not** write a custom always-on service for this: declare a
`fundo_def_t` background job and let the kernel manage its lifecycle.

```c
static void my_bg_loop(fundo_t* f) {
    while (fundo_esperar(f, 5000)) {          // sleeps; returns false when asked to stop
        ...work with BOUNDED timeouts...
        fundo_estado(f, n, "42 sent");        // updates the gray status-bar pill text
    }
}

const fundo_def_t MEU_FUNDO = {
    .nome   = "GPS online",                    // shown in task list, notifications, and console
    .corpo  = my_bg_loop,
    .pilha  = 4096,
    .perfil = { .radio = true },               // MERGED into the active power profile while running
    .ao_iniciar = prepare,                     // optional, runs on UI task before spawning
    .ao_parar   = cleanup,                     // optional, runs after the loop exits
};

// from the app window:
sistema_fundo_iniciar(&MEU_FUNDO);
sistema_fundo_parar(&MEU_FUNDO);               // signals stop and waits for exit (up to 3 s)
bool running = sistema_fundo_ativo(&MEU_FUNDO);
```

What the kernel guarantees automatically: a dedicated priority-1 task (never
starves touch or UI), a **gray status pill** with the job's name while active
(nothing runs invisibly in the background), its **power profile merged** into
the system state, instant wakeup on stop requests, and automatic cleanup if the
loop finishes on its own. From the serial console: `fundo` lists active jobs, and
`fundo parar <name>` stops one.

Rules for background loops:

- **Never block forever.** Use `fundo_esperar()` for your main sleep interval and
  keep all I/O timeouts bounded so stop requests are answered promptly.
- **CPU-bound loops must call `fundo_ceder(f)` on every iteration** — otherwise
  the task starves `IDLE`, trips the task watchdog, and prevents the CPU from
  entering sleep.
- **Never draw from the background loop.** Update protected state and let the UI
  window read a copy when open.
- **Background memory belongs to the background job** — either static or
  allocated in `ao_iniciar()` and freed in `ao_parar()`. Never point at buffers
  owned by the foreground window, which can close at any time.

For a working reference, check the FUNDO card in the Example app: a background
stopwatch (`laco_fundo`, `EXEMPLO_FUNDO`) that keeps ticking in the status bar
after you close the Example app.

### 8.2. Notifying the user: `services/notification.h`

Notifications are a **system service**: any service or app publishes its current
state, and the status bar renders it without knowing anything about the
publisher. Nobody draws directly into the status bar.

```c
#include "notification.h"

static int s_notif = -1;

// Register: safe to call on every start/open — idempotent by NAME
// (the same name returns the same slot, preserving its existing state).
// The name string must be a string literal or static buffer.
s_notif = notif_registrar("My service");
notif_silenciar(s_notif, !my_sound_preference);   // optional per-source mute
notif_associar_app(s_notif, "MeuApp");            // optional: app launched when tapped

// Publish CURRENT STATE (not an incremental event). Safe from any task.
notif_publicar(s_notif, 3, NOTIF_INFO, "3 new");
notif_limpar(s_notif);                            // nothing left to report
```

The rules — and the bugs that motivated them:

- **Publish state, not events.** Publish "I currently have 3 unread items",
  never "1 more item arrived". Republishing the exact same state is free (the
  status bar only repaints when something actually changes).
- **Derive the count from your single source of truth**, never from a separate
  running counter. Early on, Mesh maintained a separate unread counter alongside
  its conversation array; the two drifted out of sync and new messages vanished
  from the status bar. Recompute the total directly from your data structures
  when publishing.
- **Pick the severity level that matches what it means to the user:**

  | Level | Meaning | Status bar color |
  |---|---|---|
  | `NOTIF_ESTADO` | Ongoing background activity ("recording trail") | Gray; only shown in the bar when nothing higher-priority is active |
  | `NOTIF_INFO` | New activity that doesn't demand immediate action (channel chatter) | Blue |
  | `NOTIF_ATENCAO` | Directed specifically at the user (direct message) | Orange |
  | `NOTIF_URGENTE` | Requires action now (low battery, SD journal full) | Red |

- **The status bar highlights ONE top source** (`notif_destaque`): the active
  source with the highest severity (breaking ties by most recently updated),
  showing its **summary text** — so write summaries that make sense on their own
  (`"1 in LongFast"`, not `"1"`). Other active sources appear as **colored dots**
  next to it (or `"+N"` if there isn't enough room).
- **Notification center:** tapping the status bar opens a dropdown list of ALL
  active notification sources (`notif_listar`), one row per source with name,
  summary, and count. Tapping a row opens the associated app — by default, the
  app whose `.nome` matches the notification source name; if they differ (e.g.,
  source `"Meshtastic"` → app `"Mesh"`), bind them with `notif_associar_app()`.
  The lock screen displays the same list (up to 4 rows).
- **Do not wipe notifications across service restarts.** Re-registering an
  existing source name preserves its unread state, and services that restart
  automatically (transport switch, Wi-Fi reconnect) must not call
  `notif_limpar()` if unread items remain — otherwise notifications vanish
  before the user ever sees them. Only clear when the user has **seen** the
  content.
- **"Seen" means the app is actually visible.** While the lock screen or
  notification center is covering the screen, the underlying app is *obscured*
  (`sistema_app_encoberto()`): the kernel suppresses `ao_aviso`, touch, and
  keyboard delivery until the app returns to the foreground. That makes it safe
  to mark messages as read inside `ao_aviso` or `desenhar`.
- **The notification beep** sounds whenever an unmuted source's count
  **increases**. Publishing a rapidly ticking progress counter (points, bytes)
  at a level above `NOTIF_ESTADO` will beep on every tick — use `NOTIF_ESTADO`
  or mute the source.
- **App vs. service:** a pure foreground app only runs while open, so any
  notification it publishes should be cleared in `ao_fechar()` via
  `notif_limpar()`. Notifications that must persist after closing the window
  belong in a service or background job.

---

## 9. Screen orientation: portrait, landscape, or IMU auto-rotate

`.orientacao` selects between the two screen orientations supported by the OS:

| Value | Resolution | When to use |
|---|---|---|
| `ORI_EM_PE` (default) | 720×1280, rotation 0 | Most apps, matching the home screen |
| `ORI_DEITADO` | 1280×720, rotation 3 | Fixed landscape, with ExtPort1 (physical keyboard) at the bottom |
| `ORI_SEGUE_IMU` | Automatically switches between the two based on gravity | Messaging apps (Mesh, ESP-NOW) |

Rotations 1 and 2 (upside-down portrait and inverted landscape) are never used.
The kernel rotates the display before opening the app and restores portrait mode
when closing it. `sistema_area()` always returns the dimensions for the active
orientation.

**`ORI_SEGUE_IMU`** (`kernel/orientation`): while the app is open, the kernel
samples the IMU at 5 Hz and rotates the screen when gravity aligns clearly with
one of the display axes for ~0.6 s. Laying the tablet flat on a desk or holding
it diagonally does nothing (dead zone), and after rotating there is a 1.5 s
cooldown. When the physical ExtPort1 keyboard is attached, orientation locks to
landscape. Rotation happens cleanly between frames — never while a finger is
touching the screen — and is followed immediately by a full repaint pass. An app
using `ORI_SEGUE_IMU` must:

- declare `.perfil = { .imu = true }` (the IMU is only powered when requested);
- compute layout positions dynamically from the `area_t` passed to `desenhar()`
  — because viewport width and height swap live while the app is open. Any cached
  line-wrap or layout geometry must be recomputed on rotation (`kernel/list` and
  `kernel/conversation` handle this automatically);
- optionally implement `ao_girar()` if it switches between distinct portrait and
  landscape layouts; it is called after the new rotation takes effect and right
  before the full repaint pass.

Both `desenhar()` and `ao_tocar()` must share the **exact same** geometry helper
functions (`area` → button rect) so visual positions and hit targets stay in
sync across both orientations — see `det_btn_l(a)`, `cfg_btn_l(a)`, and the
`COLUNA`/`COLUNA_PE` node table grids in the Mesh app.

If your app places interactive controls right against the bottom edge of its
viewport, lock the navigation bar while those controls are active — otherwise an
accidental low tap hits the Home button right below. When locked, the navigation
bar strip is handed over to the app's viewport:

```c
sistema_travar_navegacao(true);    // when opening the bottom control / keyboard
sistema_travar_navegacao(false);   // when closing it
```

The system virtual keyboard does this automatically (`tecvirt_abrir` /
`tecvirt_fechar`), and the kernel forcibly unlocks navigation whenever any app
closes as a safety net.

While navigation is locked, the kernel also blocks indirect app switches:
tapping the status bar won't open the notification center (since tapping a row
there would switch apps), and external `sistema_simular_abrir` requests are
rejected.

For long operations where the user watches progress without touching the glass
(such as a large file transfer), your app can hold a display wake lock so idle
sleep doesn't kick in:

```c
sistema_manter_acordado(SISTEMA_ACORDADO_APP, true);    // while active
sistema_manter_acordado(SISTEMA_ACORDADO_APP, false);   // also released automatically on app close
```

### Virtual keyboard

Whenever your app needs text input (passwords, PINs, chat messages), use the
system's built-in virtual keyboard (`kernel/virtual_keyboard.hpp`) — never draw
your own keys or write custom touch hit-testing:

```c
static tecvirt_t s_kb;
tecvirt_abrir(&s_kb);                        // when starting text entry
tecvirt_posicionar(&s_kb, a);                // in desenhar: positions keyboard at the bottom of area `a`
tecvirt_pintar(&s_kb, d, completo);          // call EVERY frame; no-op when nothing on the keyboard changed
n = tecvirt_nivel(&s_kb, acoes, 8);          // in ao_tocar_nivel: returns confirmed keystrokes
ac = tecvirt_clique(&s_kb, xs, ys);          // in ao_tocar, if tecvirt_contem(&s_kb, xs, ys)
tecvirt_fechar(&s_kb);                       // when done editing, and in ao_fechar
```

Returned actions are either a positive Unicode **codepoint** (already shifted if
Shift/Caps Lock was active — the second symbol page includes accented Latin
characters), `TECVIRT_APAGAR` (backspace), `TECVIRT_ENVIAR` (submit/enter), or
`TECVIRT_FECHAR` (dismiss). Shift and symbol-page toggles are handled internally
by the keyboard and never surface to the app. For simple single-line fields
without a movable cursor (Wi-Fi password, node name), you don't even need to
manipulate UTF-8 bytes yourself:

```c
if (ac == TECVIRT_ENVIAR) { ... }
else if (ac == TECVIRT_FECHAR) { ... }
else { tecvirt_editar(buf, sizeof(buf), ac, ascii_only); }   // appends or deletes last UTF-8 char
```

Prose fields (chat messages) can enable automatic sentence capitalization via
`tecvirt_frase(&s_kb, true)` and feed the current buffer and cursor offset back
to the keyboard whenever either changes via `tecvirt_contexto(&s_kb, texto, cursor)`.
The system `conversa_t` component does all of this automatically. When a key
is typed, repaint only the text field — never trigger a full-screen repaint per
keystroke. For a standalone example, see the Wi-Fi password prompt in
`app_config/app_config.cpp`.

Keyboard height **depends on screen orientation** (keys are taller in portrait
mode where vertical space is plentiful and horizontal width is tight). To reserve
space above the keyboard, query `tecvirt_altura(a)` passing the same `area_t`
given to `tecvirt_posicionar` — never hardcode a pixel height.

---

## 10. Whole-viewport touch scrolling

If your app's static layout is taller than the screen, **do not shrink your
fonts**. Simply declare your total content height in `app_t` and let the kernel
scroll the viewport:

```c
extern const app_t APP_X = {
  ...
  .altura_conteudo = 1240,   // taller than the viewport: enables vertical scrolling
};
```

`0` (the default) means "does not scroll", and the app sees the fixed screen
viewport as usual.

When `.altura_conteudo` is greater than the viewport height, the kernel:

- tracks finger drag gestures and scrolls the viewport, clamped at both ends;
- **clips** all drawing to the app viewport — so your app never accidentally
  paints over the status bar or navigation bar;
- **adds the current scroll offset to the `y` coordinate passed to `ao_tocar`**
  in one central place;
- **swallows tap events** whenever the finger dragged more than 12 px, so lifting
  your finger at the end of a scroll doesn't click whatever button happened to
  land underneath;
- draws the scroll indicator bar along the right edge.

The third point is the key ergonomics win: **your `ao_tocar` handler tests tap
coordinates against the exact same `y` constants used in `desenhar()`.** Your
entire app lives in a single virtual coordinate space from `0` to
`altura_conteudo`, never manually adding or subtracting scroll offsets. The
alternative — requiring every app to add `scroll_y` to every `y` coordinate in
both drawing and hit-testing — guarantees that sooner or later someone forgets
one button and breaks tap targets after scrolling.

> The Settings app originally had no grid: controls were positioned by eye, and
> the screen timeout label (`x` 264..528) overlapped its own `-`/`+` buttons
> (`x` 400..490 and 500..590). The fix wasn't tweaking magic coordinates — it
> was giving each section a well-defined row height and laying everything out
> top-to-bottom by relative offset. Once it grew taller than the screen, setting
> `.altura_conteudo` made it scrollable in one line.

---

## 11. Pre-PR checklist

- [ ] Directory `main/app_x/` exporting `extern const app_t APP_X` (symbol matches folder name)
- [ ] No erase-then-redraw `fillRect` calls on periodic redraw paths
- [ ] Large regions cleared with `tab5_tela_limpar()`, not `fillRect` — including regions that *can* grow large dynamically
- [ ] `setTextDatum` / `setTextSize` / font set explicitly before drawing text
- [ ] Uses `paint.hpp` without chaining `pintura_mudou_*` calls inside `||`
- [ ] `ao_tocar` queries `sistema_area()` instead of hardcoding bar heights
- [ ] Zero blocking I/O inside `ao_tocar`
- [ ] App tasks created with `tarefa_app_criar` and stopped in `ao_fechar`
- [ ] Every `tab5_sd_montar()` paired with `tab5_sd_desmontar()` on all return paths
- [ ] `.perfil` does not set `brilho`
- [ ] System preferences applied via `sistema_reaplicar_perfil()`, never by calling hardware power HALs directly
- [ ] Exclusive peripherals claimed with `tomar()` and released with `soltar()`
- [ ] Standard buttons and metric cells use `tab5_ui.hpp` instead of local copy-paste helpers
- [ ] Tested on hardware: open, close, let the display enter sleep, and **wake back up**

That last item catches more bugs than all the others combined.

## Ready-made UI helpers: don't reimplement buttons

`tab5_ui.hpp` exists because **four separate apps** had copy-pasted the exact
same button helper — byte for byte, comment included — before anyone noticed.
And the comment inside each copy told the same cautionary tale: if a button
helper sets `middle_center` datum without saving and restoring `TextStyle`, that
datum **leaks** into the very next `drawString` call made by whoever draws
afterward, shoving text off-screen. It was the exact same bug that had bitten
`tab5_power_mostrar` earlier — and it bit us four more times because each app
rediscovered the bug on its own instead of sharing the fix.

```c
#include "tab5_ui.hpp"

tab5_botao(d, x, y, width, height, "OK", TFT_SKYBLUE);
tab5_botao(d, x, y, width, height, "OK", TFT_SKYBLUE, TFT_WHITE, 3);  // custom text color and size
```

For a "small label on top, large value below, optional subtitle" metric card —
matching the home screen's `GROVE` / `CARD` tiles — use `tab5_celula()`
instead of hand-coding three `drawString` calls:

```c
tab5_celula(d, center_x, y0, "FIX", "GPS", TFT_WHITE, "HDOP 0.9 - 8 sats");
```

If you notice another UI pattern duplicated across two apps, extract it into
`tab5_ui.hpp` rather than creating a third copy.

## Clearing the screen: call `tab5_tela_limpar()` unconditionally

Call `tab5_tela_limpar()` whenever you want a region cleared. **Do not** check
`sistema_area_foi_limpa()` first to decide whether to skip the clear — that
optimization belongs to the display layer, not the app.

Before calling your `desenhar()` on a full repaint, the kernel clears the app
viewport and records that fact in the display layer. If your `tab5_tela_limpar()`
call covers an area that was already just cleared, the HAL turns it into a
no-op automatically. You express your intent; the kernel avoids redundant work.

This matters because clearing the full viewport twice in one frame blasts ~3.2 MB
of writes into PSRAM while the DSI panel is pulling 119 MB/s to refresh the
glass — starving the display FIFO and flashing the screen **blue**. Previously,
three out of eight apps remembered to check `sistema_area_foi_limpa()` and five
didn't, which is the textbook symptom of a rule enforced at the wrong layer.

Running `tela` in the serial console reports how many redundant clears were
skipped.

Never use `d.fillRect()` for large regions: use `tab5_tela_limpar()`.

**What counts as "large", in concrete numbers:** a single strip is 96 KB, or
roughly 49,000 pixels. Any rectangle larger than that (~40 full-width scanlines
in portrait, or ~38 in landscape) should use `tab5_tela_limpar()`.

When in doubt, call `tab5_tela_limpar()`. It is never slower or worse than
`d.fillRect()`.

## Loops that draw lots of primitives

No special throttling calls are needed. Since the compositor landed
([COMPOSITOR.md](COMPOSITOR.md)), apps draw into the back buffer and the system
transfers dirty regions via DMA2D and flips on VSYNC: CPU drawing no longer
races the display scanout beam. The legacy `tab5_tela_gastou()` function is now
an empty inline no-op kept only for source compatibility.

---

## Animation: `periodo_ms` and `sistema_quadro_dt()`

`periodo_ms` in `app_t` is your **requested** frame interval. The kernel rounds
it to the nearest integer multiple of the physical panel refresh period (never
less than 1 VSYNC) and starts each frame immediately after VSYNC completion —
ensuring frames are presented at a rock-steady phase without beat-frequency
stutter (`kernel/frame_pacing.hpp`). Your app gets this automatically.

To **move** objects smoothly, don't measure wall-clock time yourself inside
`desenhar()`: call `sistema_quadro_dt()`. It returns the exact time delta
between the VSYNC scanout of the previous frame and the VSYNC scanout of the
current frame — so on-screen motion matches display presentation times rather
than CPU draw timestamps. Clamp it as appropriate for your physics step (after a
pause or page switch, `dt` can be large):

```c
float dt = sistema_quadro_dt();
if (dt > 0.1f) { dt = 0.1f; }
ball.x += ball.vx * dt;
```

Feel free to draw many small bounding boxes per frame: coalescing overlapping
dirty rectangles is the compositor's job, and it handles dozens of dirty rects
per frame without copying wasted pixels.

---

## The blue flash: what the OS handles vs. what is up to you

Understanding the hardware mechanism explains why the rules above are shaped the
way they are.

The Tab5 display uses MIPI-DSI in DPI video mode. The panel has **no internal
framebuffer RAM**: a GDMA channel streams pixels out of PSRAM scanline by
scanline, non-stop, at the pixel clock rate. If that DMA stream falls behind for
even a few microseconds, the panel's internal FIFO underflows and the entire
frame turns **solid blue** — as documented in ESP-IDF's `esp_lcd_panel_dpi.c:113`.

Two system-level contentions can delay that DMA stream, and **both are handled
by the OS**:

| Contention source | How the OS solves it |
|---|---|
| CPU and DMA2D competing with display scanout for PSRAM bandwidth | Elevated AXI-ICM QoS priority for `DW_GDMA` (`tab5_bus`), 128-byte L2 cache lines, hardware PPA fills, and a 60 MHz default pixel clock on ILI9881C (`tab5_power_relogio_dpi`) |
| SPI flash writes disabling non-IRAM interrupts and delaying the VSYNC DMA restart ISR | `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y` |

All that remains on the app side is avoiding unnecessary multi-megabyte PSRAM
churn:

- clear large areas with `tab5_tela_limpar()` unconditionally;
- request incremental redraws whenever the layout hasn't changed.

### Request the RIGHT kind of redraw

This is the easiest performance mistake to make:

```c
sistema_pedir_redesenho();            // a VALUE changed   -> cheap (only touched rects)
sistema_pedir_redesenho_completo();   // the LAYOUT changed -> ~4 MB of PSRAM traffic
```

A full redraw clears the entire app viewport (1.6 MB), redraws every widget from
scratch, and syncs the full buffer across pages. **Calling
`sistema_pedir_redesenho_completo()` just to echo a single typed character in a
text box is the fastest way to burn bandwidth** — and that is what the Mesh app
originally did on every keystroke.

Rule of thumb: if what changed stays **in the same position and the same bounding
box**, call `sistema_pedir_redesenho()`. Reserve `sistema_pedir_redesenho_completo()`
for page switches, layout changes, or elements appearing/disappearing.

### Physical keyboard

Input from the ExtPort1 physical keyboard arrives via the `ao_teclar` callback
in `app_t`, already decoded by the driver (`hal/tab5_keyboard`, running in HID
mode with the official 70-key map). Printable characters arrive as a UTF-8
string in `texto`, while special keys arrive in `codigo`:

| Key code | Key |
|---|---|
| `TAB5_TECLA_ENTER`, `_ESC`, `_TAB` | Enter, Esc, Tab |
| `TAB5_TECLA_APAGAR` / `_DEL` | Backspace (before cursor) / Delete (after cursor) |
| `TAB5_TECLA_ESQUERDA`, `_DIREITA`, `_CIMA`, `_BAIXO` | Arrow keys |

Ignore unknown key codes. When `Ctrl` or `Alt` is set in `mod`, treat the event
as a shortcut rather than printable text. Accented characters are composed via
dead keys just like the US-International layout: `'` + vowel = `á`, `'` + `c` =
`ç`, `~` + `a` = `ã`, `^` + `e` = `ê`, `` ` `` + `a` = `à`; `'` + Space = `'`.
Fields restricted to plain ASCII (passwords, node names) can pass `texto`
through `tab5_teclado_ascii()`, which decomposes accented characters back into
ASCII (`á` → `'a`).

```c
static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  if (codigo == TAB5_TECLA_ENTER) { send(); return; }
  if (codigo == TAB5_TECLA_APAGAR) { delete_last(); return; }
  append(texto);                 // empty string when a special key was pressed
}
```

`tab5_teclado_presente()` returns `true` when the physical keyboard is attached.
If your app uses the on-screen virtual keyboard, check this to avoid wasting
half the screen — **but make sure you implement `ao_teclar`**, or the user won't
be able to type at all when the hardware keyboard is plugged in!

---

## Scrollable lists: use `kernel/list`

Never hand-roll scroll offsets or "which row did the user tap?" math. Every list
in the OS (Mesh messages, conversations, and nodes; Files; Tasks; Azimuthal;
Trail) uses `kernel/list.hpp`. It follows LVGL's virtualized list model: **each
item's height is measured once when data changes, and the list caches the
layout.**

```c
static lista_t s_l;
static int  item_height(int i, void* ctx);                // reads from YOUR layout cache
static void draw_item(lgfx::LovyanGFX& d, int i, int x, int y, int width, void* ctx);

// in ao_abrir
lista_definir(&s_l, ROLAGEM_INICIO /* or ROLAGEM_FIM */, n, item_height, draw_item, nullptr);
s_l.topo = 12;                          // optional: top padding before item 0
// when items are added, removed, or resized
lista_itens_mudaram(&s_l, n);
// when ONE item's text/value changed without changing its height (age counter, badge)
lista_item_mudou(&s_l, i);
// in ao_tocar_nivel
if (lista_arrastar(&s_l, x, y, pressionado)) { sistema_pedir_redesenho(); }
// in ao_tocar
int dy;
const int i = lista_item_tocado(&s_l, x, y, &dy);   // -1 = miss; LISTA_ARRASTE = end of drag
// in desenhar
lista_desenhar(&s_l, d, window_rect, bg_color, pintura_completa(&s_p));
// when switching away from the page/tab containing this list
lista_esconder(&s_l);
```

Rules for `lista_t`:

- **The list owns its `window_rect`.** It clears backgrounds, sets clip
  rectangles, blits scrolled pixels, draws the 8 px scrollbar on the right edge
  (leave those 8 px clear in `draw_item`), and repaints only newly exposed strips
  or explicitly invalidated items. Do not call `tab5_tela_limpar` inside the
  list's window.
- **Each item must draw strictly within `[y, y + height)`.** Any decoration that
  pokes above `y` will be clipped or left behind as an orphan artifact when
  scrolling, because the list only repaints items whose vertical span intersects
  the dirty strip.
- **Hit-testing (`lista_item_tocado`) is only valid where the list last drew.**
  It automatically translates coordinates and ignores areas covered by overlays
  (like the virtual keyboard or compose bar) as long as `window_rect` excludes
  them. Whenever you switch tabs or open a modal over a list, call
  `lista_esconder(&s_l)` so stale coordinates don't intercept taps.
- **Static headers and borders belong outside `window_rect`.** Draw them on
  `pintura_completa(&s_p)`; the list never touches pixels outside its window.
- **Never measure text or word-wrap inside `draw_item`**: compute heights once
  when building your item cache (see `construir_layout` in the Mesh app).

`kernel/scroll` is the low-level scroll physics engine used internally by
`kernel/list` and whole-viewport scrolling (`altura_conteudo`). Apps should
never call `rolagem_*` functions directly.

## Messaging apps: use `kernel/conversation`

Message bubbles, the compose bar, virtual and physical keyboard integration,
scrolling, and date dividers are packaged as a single reusable system component
(`kernel/conversation.hpp`) — do not copy-paste the Mesh app. Your app simply
provides three callbacks (message count, read message `i`, send text) and
forwards draw and input callbacks:

```c
static conversa_t s_c;  static pintura_t s_p;
ao_abrir:        conversa_abrir(&s_c, &s_p, count_fn, read_fn, send_fn, nullptr);
ao_fechar:       conversa_fechar(&s_c);
ao_aviso:        conversa_mudou(&s_c);
desenhar:        pintura_iniciar(&s_p);
                 conversa_desenhar(&s_c, d, rect, pintura_completa(&s_p), can_write, reason);
ao_tocar / ao_tocar_nivel / ao_teclar: conversa_tocar / _tocar_nivel / _teclar
```

`rect` extends all the way to the bottom of the app viewport (when the virtual
keyboard opens, it overlays the bottom portion automatically). For a complete,
concise reference implementation, read `app_espnow/app_espnow.cpp`. Standalone
virtual keyboard support lives in `kernel/virtual_keyboard` (see Section 9), and
word-wrapping is exposed via `conversa_quebrar_linhas` — the Mesh app uses both.
