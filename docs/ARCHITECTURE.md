# M5tabOS — Kernel and Shell Architecture

An architectural overview describing **what exists today**, **what is still
missing to call it a true kernel**, and an incremental roadmap. Every stage is
self-contained: we can stop after any stage and leave the system in a clean,
working state.

Written after measuring on real hardware. Every number cited here comes from the
source code, `sdkconfig`, or live profiling, with file and line references.
Wherever something hasn't been measured yet, this document says so explicitly.

---

## 1. Where we stand today

The system already has a kernel — it just wasn't gathered into one place at the
start.

**Already kernel-grade and in good shape:**

| Subsystem | File | Responsibility |
|---|---|---|
| App lifecycle | `app_task.cpp` (84 lines) | Creates and cleanly tears down per-app FreeRTOS tasks |
| Power management | `tab5_power.cpp` (1248 lines) | Hardware profiles, INA226 telemetry, sleep teardown, CPU clock ceiling |
| Notifications | `notification.cpp` | System-wide notification service for apps and services: idempotent registration by name, per-source state, 4 severity levels, top-priority highlight in the status bar — see [WRITING_APPS.md](WRITING_APPS.md) §8.2 |
| Settings | `settings.cpp` (183 lines) | NVS persistence (brightness, volume, Wi-Fi, Grove power, display, clock, home coordinates) |
| Background jobs | `kernel/background.cpp` | App work that outlives the window: dedicated task, status bar pill, merged power profile, clean stop handshake (first consumer: Trail app, `app_trilha/trilha_background.cpp`) |
| Persistent logging | `tab5_log.cpp` (183 lines) | `vprintf` hook, ring buffer, daily flush to SD card |
| Paint tracking | `paint.cpp` | Dirty-value tracking per frame |
| Lazy mutex | `tab5_mutex.hpp` | On-demand atomic mutex initialization — shared by 4 modules that previously duplicated this pattern |
| Hardware drivers | `tab5_screen`, `tab5_sd`, `tab5_grove`, `tab5_keyboard`, `tab5_camera` | Single-owner hardware abstraction modules |

**STAGE 1 COMPLETE.** `sistema.cpp` had grown to 2,457 lines doing eight
distinct jobs (frame scheduling, touch routing, home screen rendering, status
bar rendering, app lifecycle management, power policy enforcement, touch
calibration correction, and — added earlier that same day — cross-task HAL call
marshaling for the camera). It was split in two **without changing a single line
of logic** (the sole exception, documented in `kernel/internal.hpp`, was
replacing one internal call with its public equivalent):

| File | Lines | Responsibilities | Items from list above |
|---|---|---|---|
| `kernel/core.cpp` | 1571 | The engine — frame loop, app lifecycle, power policy, camera bridge | 1, 5, 6, 8 |
| `kernel/shell.cpp` | 960 | The look and feel — home screen, status bar, touch routing, calibration | 2, 3, 4, 7 |
| `kernel/internal.hpp` | 78 | Private bridge between core and shell — never included by apps | — |

`sistema.cpp` was temporarily kept as `sistema.cpp.old` until hardware testing
confirmed identical behavior across several sessions, after which it was
deleted. Roughly 24 comments in other files that still pointed to `sistema.cpp`
(for code now living in `core.cpp`, `shell.cpp`, or `internal.hpp`) were updated
the same day; the handful of remaining references to `sistema.cpp` are genuine
historical notes describing how the system worked *before* Stage 1.

Item 8 (the camera bridge) stayed inside `core.cpp` because it deals with
lifecycle and HAL ownership rather than UI chrome. The open question at the time
— *"if a background task ever needs to play a sound, will we need the same
bridge pattern?"* — now has a definitive answer: **no.** Audio turned out to be
our third shared-hardware case (discovered while investigating why the
`i2s_driver` PM lock was never released; see §2.2.1) and needed a completely
different ownership model — see §1.1 below. Only the two camera-specific bridge
functions remain; the takeaway wasn't "we should have generalized the bridge,"
but rather "not every shared resource wants to be shared the same way."

### 1.1 Self-idling resources — three patterns, not one

We cataloged these patterns while deciding how to manage the speaker codec. This
isn't a new C++ abstraction to build — it's the taxonomy of what **already
exists** across the codebase, and the right question to ask before writing any
new background service: which of these three ownership patterns (if any) does
this hardware resource need?

| Pattern | Current users | Usage profile |
|---|---|---|
| **Idle timeout** | Speaker (`kernel/core.cpp`, `s_speaker_ocioso_desde`) | Short bursts where no caller holds an open session — powers on automatically on first use, powers down after a few seconds of silence |
| **Reference-counted loan** | SD card (`tab5_sd_montar()` / `tab5_sd_desmontar()`) | Explicit sessions where multiple callers may overlap; hardware stays powered until the last caller releases it |
| **Single exclusive owner** | Grove port (`tab5_grove_dono()`), camera bridge | Only one consumer makes sense at a time; ownership is explicitly claimed and handed off |

There is also a fourth, equally valid answer: **do nothing.** The IMU
(`tab5_imu_medir()`, polled at 1 Hz whether awake or asleep in `kernel/core.cpp`)
deliberately uses none of the three: one I²C read per second is cheap enough
that tracking "who needs the IMU right now" would add complexity for zero
measurable savings. Prematurely generalizing resource management would have
forced that complexity onto a peripheral that doesn't need it.

As Stage 2 introduces more true background services, choosing among these four
patterns becomes an explicit part of each service's design — rather than
inventing a one-size-fits-all framework before having enough real-world cases to
justify it.

---

## 2. What is still missing to call it a true kernel

### 2.1 No event queue — and how polling hurts battery life

Originally, **everything was polled**. Services raised dirty flags
(`s_sujo_dados`, `s_msgs_sujo`, counters) and the UI task woke up on a fixed
timer to check them:

```
PERIODO_UI     = 20 ms   sistema.cpp:48    active use
PERIODO_OCIOSO = 50 ms   sistema.cpp:58    3 s without touch
PERIODO_ZZZ    = 80 ms   sistema.cpp:75    screen asleep
```

Look closely at that last line. **With the screen completely off, the UI task
still woke up 12.5 times per second** just to ask if anything had happened. The
vast majority of the time, nothing had.

A task waking up 12.5 times a second prevents the CPU from ever entering deep
idle states. It doesn't matter how fast the check is — the energy cost comes
from *waking up*.

The right design flips this around: the UI task **blocks on a queue**, and
producers wake it only when there is actual work to do.

> I verified one crucial detail before writing this, because it would change the
> diagnosis: the GT911 touch controller has an interrupt pin on **GPIO 23**, and
> M5GFX already checks its logic level to skip the I²C read when no finger is
> present (`Touch_GT911.cpp:174`). That means idle touch polling was cheaper
> than it looked on paper — the waste wasn't I²C bus traffic, it was the FreeRTOS
> task wakeup itself. Better yet, it meant the GPIO pin was already wired and
> configured, giving Stage 2 a ready-made hardware interrupt source.

### 2.2 Power management hardware support exists — and **is turned off**

This was the single most important finding of the audit:

```
sdkconfig:2951        # CONFIG_FREERTOS_USE_TICKLESS_IDLE is not set
sdkconfig.defaults:88 # CONFIG_FREERTOS_USE_TICKLESS_IDLE=y     ← commented out
```

Without *tickless idle*, the 1000 Hz FreeRTOS tick timer (`CONFIG_FREERTOS_HZ=1000`)
interrupts both CPU cores **one thousand times per second, forever**. As a
direct consequence:

- `esp_pm_configure()` is passed `light_sleep_enable` (`tab5_power.cpp:84`),
  which **has zero effect** because the OS never sees an idle window long enough
  to enter light sleep.
- No power profile ever enables it anyway: `light_sleep_ocioso` defaults to
  `false` (`tab5_power.hpp:109`) and **nothing in the codebase ever sets it to
  `true`**.

In short: the light-sleep code in `tab5_power` is **dead code today**. We wrote
the mechanism and never flipped the switch.

### 2.2.1 PM locks — and why the news is better than it looks

Next, I checked what else besides the RTOS tick prevents the SoC from sleeping.
In ESP-IDF, dynamic power management is governed by **PM locks**: any driver
that requires a stable clock or peripheral bus acquires a lock on init and holds
it. As long as a single lock is held, the system silently refuses to enter any
sleep state forbidden by that lock.

The MIPI-DSI display driver holds **two** locks, acquired when the panel is
created and released only when the panel is destroyed (`esp_lcd_panel_dpi.c`
291→405, `esp_lcd_mipi_dsi_bus.c` 72→151):

```
dsi_dpi   CPU_FREQ_MAX     pins the CPU at 360 MHz
dsi_phy   NO_LIGHT_SLEEP   blocks light sleep
```

Normally this would be a showstopper — as long as the display driver is alive,
neither tickless idle nor `esp_pm` can do anything. **Fortunately, we already
solved this**: `DESMONTAR_DSI_NO_REPOUSO = true` (`tab5_power.cpp`) completely
destroys the DSI panel when entering sleep, dropping both locks. We verified
this with `esp_pm_dump_locks()` after two lighter-weight alternatives failed on
hardware (`set_pattern` and lowering the DPI pixel clock while keeping the video
stream running — both documented in the source comments).

In other words: **the hardest blocker for Stage 3 was already solved months
ago.** Stage 3 is much closer to working than §2.2 makes it sound.

**The remaining culprit** is the UART driver. ESP-IDF's UART driver holds an
`ESP_PM_APB_FREQ_MAX` lock for as long as the driver is installed
(`uart.c:1922`), and `APB_FREQ_MAX` **also** prevents light sleep because
sleeping would halt the APB clock. When Meshtastic is configured for the Grove
port, that UART driver stays installed permanently.

That exposes the real tension in Stage 3, which is a product decision rather
than a coding bug: **you cannot sleep the APB bus and listen to a continuous
UART at the same time.** Either Meshtastic drops incoming serial packets while
the tablet sleeps, or the tablet stays awake. Hardware offers a middle ground
(UART start-bit wakeup from light sleep, at the cost of losing the first byte),
which remains an open design question.

**Update — one new PM lock found and fixed; another explained.**

Running `energia travas 40` with the screen genuinely asleep (confirmed in the
log: `repouso apos 30 s`, `DSI desmontado`, clock `360 -> 40 MHz`) uncovered a
lock we hadn't anticipated: `i2s_driver` (`APB_FREQ_MAX`), held **forever**
after the very first `tone()` or `playWav()` call of a boot session, whether
audio was playing or not. Root cause: on the ESP32-P4 (which uses an external
ES8388 codec over standard I2S rather than the classic ESP32 internal DAC),
M5Unified only calls `i2s_stop()` in `spk_task`'s idle branch inside an
`#if M5UNIFIED_I2S_ADC_DAC` block that is never compiled for this chip
(`managed_components/m5stack__m5unified/src/utility/Speaker_Class.cpp:615`).
Patching a vendored component isn't worth the maintenance burden across upgrades,
so we fixed it on our side via the public API: an idle watchdog in
`kernel/core.cpp` calls `M5.Speaker.end()` after a few seconds of silence (our
first production use of the "idle timeout" pattern from §1.1). Verified again
with `energia travas 40`: the lock disappears cleanly once the timeout expires.

One lock still shows up as held in that dump — `rtos0` (`CPU_FREQ_MAX` on core
0) — and that one is expected, not a bug. It is `esp_pm`'s own internal RTOS
lock (`s_rtos_lock_handle` in `pm_impl.c`), which is released only while a
core's `IDLE` task is running and re-acquired the instant any interrupt fires.
It is effectively a live gauge of how busy core 0 is. With `log` consuming 10.3%
of core 0 (measured via `tarefas`) plus `energy_log.cpp`'s `esp_timer` firing
every ~15 s, core 0 never stays idle for long uninterrupted stretches. That is
the exact same polling issue described in §2.1 seen from the PM lock side,
reinforcing why Stage 2 comes before Stage 3.

### 2.3 Task priorities and core affinities grew organically

```
ui          prio 4   stack 8192    core 1   sistema.cpp:2455   fixed (only pinned task)
gps         prio 3   stack 4096    any      tab5_gps.cpp:265
mesh        prio 3   stack 4096    any      tab5_mesh.cpp:2645
wifi        prio 3   stack 4096    any      tab5_wifi.cpp:550
cam         prio 3   stack 8192    any      tab5_camera.cpp:406   only while Camera app is open
trilha      prio 2   stack 4096    any      tab5_trilha.cpp:197   service, outlives app window
NMEAParse   prio 2   stack 4096    any      app_azimutal.cpp:367  only while Azimutal app is open
console     prio 1   stack 12288   any      console.cpp:2196
registro    prio 1   stack 4096    any      energy_log.cpp:271
log         prio 1   stack 6144    any      tab5_log.cpp:179
tarefas     prio 1   stack 4096    any      app_tasks.cpp:240     only while Tasks app is open
sd_monta/
desmonta    prio 1   stack 4096    any      sistema.cpp:1361/1427/1505   short-lived, once per app open
grove_sonda prio 1   stack 4096    any      sistema.cpp:1672      short-lived, single probe
```

Updated table — the original version of this list had 5 rows; today it has 12.
Every new service (trail, log, camera) slotted cleanly into the existing priority
tiers (priority 3 for real-time radio/sensor I/O, 2 for unhurried background
processing, 1 for kernel plumbing) without needing new tiers.

The priority hierarchy itself is sound: the UI task (4) preempts all services
(3), so background I/O never stalls a frame mid-render.

What was **not** deliberately chosen is core affinity: every service uses
`tskNO_AFFINITY` and floats freely between cores 0 and 1. Pinning all background
services to core 0 would leave core 1 dedicated exclusively to the UI — helpful
if we ever want to power down an entire core during sleep.

**We are not changing affinities right now.** The benefit is plausible but
unmeasured, and optimizing without measuring first is the exact mistake that has
cost us time before.

---

## 3. The measurement tools are already built in

Before changing anything, we should measure — and the instrumentation is already
compiled in:

```
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y
```

The `tarefas` console command (and the Tasks app) already report **true per-task
CPU%** computed from `ulRunTimeCounter` deltas — not an approximation.

There are **two** key measurements, and they answer different questions:

**1. How much headroom exists** — let the screen go to sleep and run `tarefas`
from the serial console. Check the **IDLE** row: that percentage is the
theoretical ceiling of what light sleep can save. If IDLE sits at 97%, there is
a lot of battery life on the table. If it sits at 60%, something is burning CPU
in the dark and the fix is finding the culprit first.

**2. Whether the path to sleep is clear** — run `energia travas 30`. This waits
30 seconds for the display to enter sleep and then dumps the active PM locks via
`esp_pm_dump_locks()`. Do not touch the screen while waiting.

How to read the dump:

- **Empty, or all locks released (`0`)** → the path is clear; Stage 3 is ready
  to try;
- **`dsi_dpi` or `dsi_phy` still held** → display teardown didn't run, which
  must be investigated before anything else;
- **`uart_driver` held** → that's the Meshtastic Grove UART, bringing us back to
  the product trade-off in §2.2.1.

The second measurement is the decisive one: the first tells us how much we
*could* save, while the second tells us whether the OS will *allow* it.

---

## 4. The staged plan

### Stage 1 — Clean module boundaries with zero behavior change ✅ DONE

Split `sistema.cpp` in two **without altering a single line of logic**:

```
kernel/core.cpp    frame scheduler, app lifecycle, power policy
kernel/shell.cpp   home screen, status bar, navigation bar, touch routing, calibration
```

(Using standard OS terminology: `core.cpp` is the kernel proper, while `shell.cpp`
is the visual shell wrapping applications — see the header comment in
`kernel/internal.hpp` distinguishing this UI shell from the interactive serial
CLI in `console.cpp`.)

Performance gain: zero. Maintainability gain: huge — verified on build, the
resulting binary size differed by only ~96 bytes (cross-translation-unit call
indirection). A 2,457-line file mixing eight responsibilities and shared file-scope
statics was a breeding ground for subtle bugs. The seam between the two files is
`kernel/internal.hpp`, exposing only a handful of `extern` variables and
internal helpers that are never visible to apps.

### Stage 2 — The event queue (where the real power savings live)

**SLICE 1 COMPLETE.** A single event queue created in `sistema_iniciar()` and
awaited at the bottom of `vTaskInterface()`:

```c
typedef enum {
  EV_TOQUE,     // from GPIO 23 ISR - just "wake up and poll M5.Touch again"
  EV_SERVICO,   // sistema_acordar_interface() - a service published new state
} evento_t;
```

The bottom of the UI loop changed from `vTaskDelay(periodo)` to
`xQueueReceive(fila, &ev, periodo)`. In this first slice, the dequeued event
value itself is intentionally ignored: the very next iteration of the loop
already checks `M5.Touch` and all dirty flags just as it always did. **No
existing decision logic changed** — only how the task waits between iterations.

What changed in practice, measured on hardware:

- **Active and idle with the screen on:** behavior is **identical** to before —
  the exact same `PERIODO_UI` / `PERIODO_OCIOSO` timeouts, simply waiting on
  `xQueueReceive` instead of `vTaskDelay`. The GPIO touch ISR stays **disabled**
  in both of these states (see Risk 2 below), so there is zero observable
  difference while the screen is on.
- **Screen asleep (sleep mode):** the sleep timeout is no longer a fixed
  `PERIODO_ZZZ = 80 ms`; instead, it computes the exact time remaining until the
  next scheduled periodic job the loop already ran — the minimum of sensor
  sampling (1000 ms), status bar / beep updates (`BARRA_MS`), and the Grove
  probe watchdog (`VIGIA_MS`, only when active). In practice, the sleeping UI
  loop now wakes up on its own roughly **once per second instead of 12.5 times
  per second**, while a real hardware GPIO ISR on the touch interrupt pin
  (GPIO 23, which previously had no ISR attached — only synchronous level reads
  in `Touch_GT911.cpp:174`) wakes the queue immediately the instant a finger
  touches the glass.
- **A service publishing while the tablet is asleep:** `notification.cpp` became
  the first active event producer, calling `sistema_acordar_interface()` inside
  `notif_publicar()` whenever notification state actually changes (guarded by
  the same check that increments `notif_versao()`). Mesh and Trail get instant
  wakeups for free without knowing the queue exists: an incoming message with
  the screen off no longer waits up to ~1 s for the next poll tick before
  sounding the notification beep.

**Intentionally left out of Slice 1:**

- **Mesh, GPS, and Trail do not push to the queue directly.** They don't need
  to — all three already notify the user via `notif_publicar()`, so they inherit
  instant sleep wakeups without touching a single line of their own code.
- **`PERIODO_OCIOSO` (screen on, no touch for 3 s) stays fixed at 25 ms** rather
  than becoming deadline-driven. Making it deadline-driven would require
  enabling and disabling the touch GPIO interrupt around active drag gestures —
  because the GT911 pulses its INT pin every 5–20 ms while a finger is held
  down, leaving the ISR enabled while awake would flood the queue and defeat the
  frame-pacing throttle that `PERIODO_UI` / `PERIODO_OCIOSO` enforce. The
  potential savings while awake (40 Hz → ~16 Hz while the display backlight is
  drawing hundreds of milliwatts anyway) are tiny compared to sleep mode
  (12.5 Hz → ~1 Hz) and weren't worth the extra state-machine complexity.

**Two real bugs caught during design review before writing a single line of
code:**

1. *Unbounded busy-spin loop.* The Grove probe timer only advances when
   `ajustes_grove() && !s_grove_resondar`. Without checking that condition when
   computing the next deadline, having Grove 5V turned off (the default) or a
   re-probe in progress would leave that deadline stuck at 0 ms forever —
   causing `xQueueReceive` to return immediately on every iteration and spinning
   core 1 at 100% CPU during sleep. Fix: the Grove deadline is only included in
   the `min()` calculation when both conditions hold.
2. *The touch ISR must not feed the queue while the screen is awake.* As noted
   above under `PERIODO_OCIOSO`, the GPIO interrupt starts disabled and is only
   enabled (`gpio_intr_enable`) once `tab5_power_repouso()` succeeds, then
   disabled again (`gpio_intr_disable`) the moment `tab5_power_retomar()` is
   called.

**What remains before calling Stage 2 complete** (future work, not yet needed):
having Mesh/GPS/Trail wake the UI directly if they ever need to trigger an
update that doesn't go through `notif_publicar()`, and deciding whether adaptive
deadlines while the screen is awake are worth the finer-grained ISR toggling.

### Stage 3 — Tickless idle and automatic light sleep

Enable `CONFIG_FREERTOS_USE_TICKLESS_IDLE` and set `light_sleep_ocioso = true`
in the sleep power profile.

**This only makes sense after Stage 2.** With Slice 1 in place, sleep mode now
wakes up ~1×/s instead of 12.5×/s — creating idle windows long enough for
`CONFIG_FREERTOS_USE_TICKLESS_IDLE` to actually do its job. We should still
re-run `tarefas` and `energia travas` on hardware before flipping the Kconfig
switch, and the hardware blockers below remain unchanged:

**Honest blockers**, ordered by severity:

1. **The Grove UART** (§2.2.1). A product trade-off before it is a code change:
   entering light sleep risks dropping incoming bytes on a wired Meshtastic
   link. Run `energia travas 30` to see when `uart_driver` holds a lock.
2. **esp_hosted SDIO.** Combining an active SDIO slave link with host light
   sleep is delicate. Upstream `esp_hosted` does include a reference example for
   this (`examples/power_save/host+cp/…`), suggesting it is a supported path,
   but it requires careful testing to ensure the C6 link doesn't lock up on
   resume.
3. **The MIPI-DSI panel.** Already solved (§2.2.1); listed here only for
   completeness.

---

## 5. Three-line summary

1. The kernel already exists across well-defined modules; what was missing was
   an **event-driven wakeup queue** so the UI task stops polling during sleep.
2. `sistema.cpp` mixed kernel core and UI shell across 2,457 lines — Stage 1
   split them cleanly into `kernel/core.cpp` and `kernel/shell.cpp`, making the
   Stage 2 queue integration straightforward.
3. Automatic light sleep is **implemented in code but disabled in `sdkconfig`**
   (`sdkconfig:2951`) — yet the hardest blocker (MIPI-DSI PM locks) was already
   eliminated by tearing down the panel on sleep. Before enabling it, use
   `tarefas` to measure idle headroom and `energia travas 30` to verify no locks
   remain held.

**Recommended order:** measure → ~~Stage 2~~ → ~~Stage 1~~ (done) → Stage 3.
Stage 1 landed before Stage 2 out of convenience, without changing the core
thesis: the real battery savings come from Stage 2 and Stage 3.
