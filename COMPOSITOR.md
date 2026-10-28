# M5tabOS — The Display Compositor (LVGL-Style Rendering)

Decision made: rather than adopting LVGL, we built **our own** display
compositor using the exact same rendering principles that make LVGL smooth,
tailored to our stack (M5GFX as the rasterizer, FreeRTOS, per-app power
profiles, and the Tab5 hardware).

The guiding rule from here on out:

> **Display glitches are SYSTEM bugs.** If an app needs an extra `fillRect`, a
> delay, or a special case to avoid flickering or tearing, the display layer is
> broken, not the app. No per-app band-aids.

---

## 1. Why the original direct-to-front-buffer model couldn't be fixed with patches

Originally, M5GFX drew **directly into the framebuffer that the MIPI-DSI panel
was actively scanning out**. Nearly every drawing rule in the codebase existed
as a defense against that single architectural choice:

| Legacy workaround | What it was fighting |
|---|---|
| Differential painting, fixed-width strings, `pintura_*` | Erase-then-redraw was visible to the eye as flicker |
| `s_sujo_layout` vs. `s_sujo_dados`, `sistema_area_foi_limpa()` | Apps couldn't tell when the screen had been wiped behind their back |
| Saving/restoring `getTextStyle()` (Rule #2) | Global drawing state leaking across functions |
| Stripped `tab5_tela_limpar`, `respirar` delays, `tab5_tela_gastou` | CPU write bursts to PSRAM → DSI FIFO underrun → solid blue frame |
| `tab5_tela_rolar` + `rolagem_item_preservado` | Reusing on-screen pixels to make scrolling cheaper |
| `ROLAR_RESPIRO_US`, "drawing faster than the scanout beam" | Tearing during vertical scroll |
| `REPINTURA_MIN_MS = 80` | Excessive redraws during continuous touch input |
| Double buffering limited to full-screen repaints | Black flash when switching apps |

Every row in that table solved a real hardware bug in the only way possible
**given the foundation we had**. The foundation itself was the problem. Our
first experiment (double buffering only the message list viewport) exposed the
limit: hardware page flips swap the entire screen buffer at once, so without
tracking **everything** that changed during a frame (the compose bar, the
channel badge, the status bar), the other buffer holds stale pixels in those
regions and flickers on flip.

---

## 2. The six principles (how LVGL works, and how our compositor works)

1. **Never write to the buffer the panel is scanning out.** All drawing targets
   the back buffer. The front buffer is read exclusively by the DSI DMA engine.
2. **A single GLOBAL dirty-rectangle list per frame.** Every visual change (a
   label update, the battery icon, a scroll step) *invalidates* a rectangle.
   After a page flip, we copy **only those dirty rectangles** to the new back
   buffer — that is what was missing in our first attempt, and it is what makes
   true double buffering fast: the sync cost scales with what changed, not with
   the full screen size.
3. **Flip on the frame boundary.** The ESP-IDF DPI driver already does this in
   hardware (`cur_fb_index` is only latched when the DMA descriptor restarts at
   VSYNC, `esp_lcd_panel_dpi.c:75`). We wait for `on_frame_buf_complete` before
   touching the buffer that just rotated off-screen.
4. **One frame per UI cycle.** Touch events, incoming data, and timers only
   *invalidate* regions. A single frame step at the end of the UI loop merges
   all invalidations and renders once. Nothing draws synchronously inside an
   event handler. When nothing is dirty, no frame is produced — zero idle cost.
5. **Scrolling means redrawing with a clip rect, or DMA-blitting from the front
   buffer.** The list updates its scroll offset, invalidates its viewport, and
   renders cleanly into the back buffer before flipping.
6. **Pre-rendered anti-aliased fonts at exact pixel heights.** No fractional
   scaling of bitmap fonts (like the old `TAM_MSG 3.8`). Every font size used in
   the UI is generated at its native pixel height.

---

## 3. What the hardware provides out of the box (verified on ESP-IDF 5.5.5)

- **Two hardware framebuffers** already allocated (`num_fbs = 2` in M5GFX's
  `Panel_DSI`), 720×1280 RGB565 = 1.84 MB each, in PSRAM.
- **Zero-copy hardware page flip**: calling `esp_lcd_panel_draw_bitmap()` with a
  pointer targeting one of the panel's own framebuffers simply swaps the active
  buffer index (`esp_lcd_panel_dpi.c:506-524`).
- **Frame completion interrupt**: `esp_lcd_dpi_panel_register_event_callbacks()`
  provides `on_frame_buf_complete` (and `on_vsync`).
- **PPA / DMA2D** (`esp_driver_ppa` and `esp_async_fbcpy`): `ppa_do_fill`,
  `ppa_do_blend`, and `ppa_do_scale_rotate_mirror` (at scale 1.0 = strided
  rectangular blit). Copies and fills memory via hardware DMA without tying up
  the CPU.
- **AXI bus QoS priority** (`hal/axi_icm_ll.h`): per-master QoS priorities on
  the internal AXI interconnect, including `DW_GDMA`, which feeds the MIPI-DSI
  controller. The ESP-IDF driver itself points to this as the fix for display
  underruns (`esp_lcd_panel_dpi.c:113`). It addresses the **root cause** of the
  blue flash: instead of the CPU and display DMA competing as equals for PSRAM
  bandwidth, display scanout reads get priority.

---

## 4. Architecture

```
   events (touch, data, timers)
         │  only invalidate
         ▼
  ┌──────────────────────┐
  │  OBJECT TREE / APPS  │  screens, labels, buttons, lists, images, canvas
  │  (kernel/ui_*)       │  mutating state = invalidating the affected rect
  └─────────┬────────────┘
            │ dirty rectangles (global list, merged)
            ▼
  ┌──────────────────────┐
  │  COMPOSITOR          │  1 frame per cycle, on the UI task
  │  (kernel/compositor) │  tracks dirty rects on the back buffer
  └─────────┬────────────┘
            │ cache writeback + DMA2D/PPA sync
            ▼
  ┌──────────────────────┐
  │  FRAME HAL           │  back buffer ← rendered rects
  │  (hal/tab5_frame)    │  page flip on VSYNC boundary
  └─────────┬────────────┘  awaits on_frame_buf_complete
            │               lazily syncs dirty rects onto the new back buffer
            ▼
         MIPI-DSI panel (reads the front buffer, prioritized on AXI)
```

### 4.1 Tile rendering in SRAM vs. direct back-buffer rendering

Initially we explored rendering into small horizontal **tiles** in internal SRAM
(e.g., 720 × 32 lines × 2 B = 46 KB, double-buffered) and pushing each finished
tile to PSRAM via DMA, so overdraw (background + bubble + glyph) would happen in
fast internal SRAM rather than PSRAM.

As documented in the engineering log below (Section 7), once we switched buffer
copies from the PPA engine to raw **DMA2D** (`esp_async_fbcpy` at 288 MB/s),
added **PPA hardware fills** for large rectangles, and introduced a **glyph
cache**, drawing directly into the PSRAM back buffer ("direct mode" in LVGL
terminology) with lazy dirty-region synchronization proved both faster and
simpler — freeing 90 KB of precious internal SRAM and 1.84 MB of PSRAM.

### 4.2 Retained-mode UI widgets

Instead of every app manually tracking dirty rects and re-implementing scroll
physics, common patterns live in shared system components (`kernel/list`,
`kernel/conversation`, `kernel/virtual_keyboard`, `tab5_ui.hpp`), while apps
that do custom canvas drawing simply draw into `M5.Display` and let the
compositor's tracker panel record every touched rectangle automatically.

### 4.3 Input handling

Touch input is sampled centrally by the kernel, which routes gestures to the
active component or app. Lists handle drag thresholds, momentum, clamping, and
swallowing taps at the end of a scroll gesture automatically.

### 4.4 Power efficiency

When no region is dirty, no frame is presented: the UI task sleeps on the event
queue. The frame loop only runs at panel refresh rate while an animation is
active or a finger is dragging on screen; a clock that updates once a minute
costs one small rectangle copy per minute. Per-app hardware power profiles work
unchanged.

### 4.5 Incremental migration without a flag day

By subclassing M5GFX's `Panel_Sprite` as a **tracker panel** that intercepts
every drawing primitive and records its bounding box in physical coordinates,
every existing `desenhar()` function automatically benefits from hardware
double-buffering, tear-free page flips, and partial dirty-rectangle sync without
rewriting all apps at once.

---

## 5. Implementation stages

Every stage was compiled, flashed, and benchmarked on physical hardware before
moving to the next.

| # | Stage | Deliverable | Acceptance criteria |
|---|---|---|---|
| 0 | **Measure** | Underrun counter, frame timing, free internal SRAM | Baseline numbers for comparison |
| 1 | **Frame HAL** | `hal/tab5_frame`: AXI QoS, VSYNC page flip, `on_frame_buf_complete`, dirty-rect sync, PPA/DMA2D copy | Console test screen: animated rect + synthetic scroll with zero underruns and zero tearing |
| 2 | **Compositor** | Dirty-rect list with LVGL-style merging, SRAM tile prototype, frame loop | Same test screen running through the compositor |
| 3 | **Bridge + widgets** | Tracker panel (`Panel_Sprite` subclass), `kernel/list`, `kernel/conversation`, native anti-aliased fonts | Entire OS running through the compositor |
| 4 | **Mesh app** | Message history migrated to virtualized `kernel/list` with cached bubble layouts | Smooth scrolling with zero tearing |
| 5 | **Remaining apps** | Migrate lists in Files, Tasks, Azimuthal, Trail; optimize full-screen clears | Every app tested on hardware |
| 6 | **Delete legacy hacks** | Remove old double-buffer code, strip delays, `rolagem_item_preservado`, `REPINTURA_MIN_MS` | Clean display HAL |

---

## 6. Open questions (answered by measurement in Section 7)

- **Free internal SRAM** with Wi-Fi and `esp_hosted` active: determines whether
  SRAM tiles are worth their footprint (Stage 0).
- **`DW_GDMA` QoS level** that eliminates underruns without starving the CPU
  (Stage 1).
- **PPA vs. raw DMA2D (`esp_async_fbcpy`)** for rectangular copies and fills:
  benchmarked head-to-head on hardware (Stage 1 / Audit).
- **Fonts**: pre-rendered smooth VLW/bitmap fonts generated at exact target pixel
  heights by script, matching `generate_emoji.py` (Stage 3).

---

## 7. Engineering log

**Stages 0 and 1 implemented and benchmarked on hardware.**
- `hal/tab5_frame.{hpp,cpp}`: `DW-GDMA` AXI QoS toggle, frame-complete callback
  (`on_frame_buf_complete`), rectangular buffer copy via PPA, and a hardware
  underrun counter (hooking channel 2 of the ROM UART output, which boot leaves
  free — counts driver `ESP_DRAM_LOGE` underrun messages without hijacking the
  serial console).
- `hal/tab5_screen`: `tab5_tela_igualar_paginas()`, `tab5_tela_apresentar()`
  (flushes cache for dirty rects only → requests flip → awaits frame completion
  → copies dirty rects to the buffer that rotated off-screen),
  `tab5_tela_escrever_na_frente()`.
- `app_quadro` (temporary benchmark app): runs the exact same animation in
  LEGACY and NEW modes with QoS ON/OFF, measuring FPS, draw time, flip wait
  time, worst frame, and underrun count.

**Stage 1 hardware results.**
- Synchronized page flipping **works**: "missed flips: 0" across all runs in NEW
  mode.
- Drawing the full viewport directly in PSRAM takes **~34 ms** (identical in
  legacy and new modes), capping full-screen redraws at ~29 FPS.
- Initial NEW mode ran at 16–17 FPS because it waited for VSYNC immediately
  after requesting the flip and copied the entire viewport that the next frame
  was about to overwrite anyway. Fixed in Stage 2 by deferring the flip wait and
  buffer sync to the start of the *next* frame.
- **Maximum QoS (`15`) on `DW-GDMA` makes things WORSE**: with the panel at
  79 MHz, the screen flashed blue continuously. Turned OFF until investigated
  further. (A 35 Hz reading in one test run turned out to be the panel clock
  lowered in Settings, not a hardware fault.)

**Stage 2 in code (`kernel/compositor.{hpp,cpp}`).** Dirty-rectangle list with
LVGL-style bounding-box merge, dual SRAM tiles, M5GFX rendering in screen
coordinates (sprite buffer pointer offset), async PPA copy from tile to back
buffer, deferred sync that skips regions slated to be overwritten in the new
frame, and non-blocking flip requests. Tested via `COMP ROLAR` / `COMP QUADRADO`.

**Fixes after investigating blue flashes.**
- The test app's "QoS" toggle was duplicating `hal/tab5_bus` (which was already
  configured at boot with arbitration priority 8 + `ARQOS 8`). In the test app,
  "QoS OFF" was actually that balanced boot state, while "QoS ON" cranked
  `ARQOS` to `15`, which starved the system and caused continuous blue flashes.
  Removed the duplicate code; the test toggle now switches `hal/tab5_bus` cleanly.
- L2 cache line size bumped from 64 B → 128 B, significantly improving external
  memory throughput.
- Panel pixel clock clamped to 24..80 MHz in Settings and the serial console
  (clamping instead of rejecting out-of-range steps, so 5 MHz steps reach 80 MHz).

**Stages 1–2 validated on hardware; Stage 3a (system-wide bridge) landed.**
Benchmark results: `COMP ROLAR` is completely smooth with zero tearing or
artifacts; `COMP QUADRADO` hits 63.5 FPS (the panel's physical refresh rate) at
2.4 ms/frame with zero blue flashes at 80 MHz.

Stage 3a — **routing the entire OS through the compositor**:
- `kernel/virtual_screen`: a 720×1280 shadow canvas in PSRAM that the panel
  never scans out, paired with a **tracker panel** (a `Panel_Sprite` subclass)
  that records the bounding box of every M5GFX drawing primitive, automatically
  transformed into physical panel coordinates (using `Panel_Sprite`'s own
  rotation math, so landscape apps like Mesh work transparently).
- The kernel (home screen, bars, apps) draws into the virtual screen; at the end
  of each UI loop iteration, `comp_quadro_imagem()` blits only the dirty
  rectangles to the back buffer via PPA and requests a flip. On wake from sleep:
  renders the full screen and awaits flip completion before turning on the
  backlight.
- `tab5_tela_limpar` and `tab5_tela_rolar` now write straight to the target
  surface **with zero artificial delays**.
- Whenever the DSI panel is recreated (sleep/wake or pixel clock change), the
  compositor detects the new panel generation ID from `tab5_screen` and
  invalidates the entire screen.
- Azimuthal app fix: `pushSprite(px, py)` was pushing to the canvas's default
  parent (`M5.Display`), bypassing the compositor; changed to
  `pushSprite(&d, ...)`.

**Stage 3a validated; legacy cleanup; first standard UI widget.**
- `hal/tab5_screen` shrank from 811 → 275 lines: removed legacy double
  buffering, strip delays, `gastou` (kept as an inline no-op for compatibility),
  and striped cache flushes. Removed `REPINTURA_MIN_MS` and `rolagem_em_arraste`
  from the kernel, removed the 70 ms wake-up settle delay from `tab5_power`, and
  removed `comp_encerrar` from the compositor.
- **`kernel/list`**: standard scrollable list widget (cached per-item heights,
  exposed-strip partial repaint, scrollbar, and scroll-offset compensation when
  items are appended to a bottom-anchored list).
- **Mesh messages view** migrated to `kernel/list` with a **bubble layout cache**
  (`construir_layout`): each message is measured once when history loads rather
  than on every scroll step (previously, every frame ran word-wrapping, emoji
  scanning, 2× `gmtime`, and locked the service mutex for *every* message in
  history). Dragging no longer forces a full-window repaint.

**Fixes + structural audit.**
- Mesh: outgoing message timestamp was being recorded as `0` (rendering as
  `31/12/1969` in UTC−3); now uses system UTC when valid (`MESH_EPOCH_VALIDO`),
  and undated messages no longer insert a date divider. `AVISO` and `ENV`
  buttons switched to `TAB5_BOTAO_TEXTO_GRANDE`.
- Virtual keyboard: native 24 pt bold smooth font (replacing 12 pt bitmap scaled
  1.5×); inter-key gaps snap to the nearest key; key presses confirm on finger
  **release** (previously relied on M5Unified's tap detector, which dropped keys
  if the finger drifted slightly or held too long), keeping tap detection as a
  fallback for sub-sample taps.
- Structural audit findings: UI was still waking at 40–50 Hz when idle; Mesh was
  reading SD card history **on the UI task** on every incoming message; internal
  I2C lacked a shared lock across M5Unified and ESP-IDF's camera SCCB driver;
  and apps were polling via `periodo_ms` instead of reacting to events.

**The four structural fixes + Protobuf audit.**
1. **Event notifications** (`sistema_avisar`, `AVISO_*`,
   `app_t.avisos/ao_aviso/periodo_agora`): services signal what changed and the
   open app redraws immediately. Mesh message view now runs with `periodo_ms = 0`
   (only the Nodes and Mesh tabs, which display live age counters, use a 1 s
   timer).
2. **System worker** (`kernel/work`): offloads slow I/O from the UI task and
   delivers completion via `AVISO_TRABALHO`; closing an app waits for any
   in-flight job to finish and flushes pending jobs. Migrated Mesh (history and
   SD card reads, double-buffered) and Files (line counting and page loading).
3. **UI task sleeps when idle**: while a finger is down or work is pending, it
   polls at 20 ms with the touch GPIO interrupt disabled; when idle, it enables
   the touch interrupt and sleeps until the next real timer deadline. Calling
   `sistema_pedir_redesenho()` from another task wakes the UI task immediately.
4. **Unified internal I2C locking**: M5GFX locks its I2C bus per transaction,
   whereas ESP-IDF's camera SCCB driver did not. Using
   `-Wl,--wrap=sccb_new_i2c_io`, we routed `esp_video`'s SCCB transactions
   through M5GFX's locked I2C bus (`hal/tab5_sccb.cpp`), allowing us to delete
   the cross-task camera marshaling bridge. This also made background auto-exposure
   (IPA/AGC) register writes thread-safe during live capture.
- Keyboard: release debouncing (2 finger-up samples, 80 ms minimum repeat on the
  same key).
- **Protobuf wire format verified against `meshtastic/src/mesh/generated/.../mesh.pb.h`**:
  `MeshPacket.via_mqtt` is field 14 and `hop_start` is field 15 (our code was
  reading 13 and 14); `QueueStatus.mesh_packet_id` is a `varint` (was being read
  as `fixed32`). All other decoder and encoder fields matched.

**Tasks app: profiling promoted to a system service.** `services/tab5_monitor`
samples continuously at 1 Hz, records high/low watermarks with timestamps, keeps
a 5-minute rolling history, triggers diagnostic events, and logs per-minute
summaries and events to the SD card. The Tasks app is now just a viewer window.

**Power app overhauled.** Reads the kernel's cached sensor sample (eliminating a
duplicate I2C read), uses native 18 pt / 12 pt smooth fonts, plots a full 24-hour
rolling window loaded via the system worker (leaving gaps when powered off),
shows signed current with a zero reference line, power in watts, and integrated
daily energy (mAh discharged/charged, Wh). Logs to
`Sistema/energia/energia-YYYY-MM-DD.csv` (`ENE3` format with real timestamps) in
batches of 5. Added an `UNDERRUN` monitor trigger.

**Consolidating scrollable lists across the OS.**
- `kernel/list` became the **sole** scrollable list implementation across all
  apps: added touch handling (`lista_arrastar`, `lista_item_tocado` with
  `LISTA_ARRASTE`), single-item invalidation (`lista_item_mudou`), top padding,
  and `lista_esconder`. Migrated Mesh (messages, conversations, nodes), Files,
  Tasks, Azimuthal (days), and Trail (sessions — replacing manual PREV/NEXT
  pagination buttons). No app calls `rolagem_*` directly anymore;
  `kernel/scroll` is purely an internal engine for `kernel/list` and the kernel.
- Raised directory/item caps in PSRAM (since `readdir` returns entries in
  creation order, small caps hid newly created files): Files 24 → 512 (sorted,
  folders first, newest files first), Azimuthal 24 → 400 days, Trail 60 → 1000
  sessions.
- Compositor cleanup: removed the unused SRAM tile path (freeing 90 KB of
  internal SRAM held since boot) and removed redundant full-buffer page copies
  on panel creation (saving 1.8 MB of PSRAM traffic on boot and wake).
- Enabled `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` and marked large static
  buffers with `EXT_RAM_BSS_ATTR` (Mesh node table, log drain buffer, monitor
  event history), freeing ~45 KB of internal SRAM.
- Deleted the temporary `app_quadro` benchmark app, `tab5_power_tela`,
  `notif_esta_silenciada`, `pintura_mudou_f`, and `rolagem_item_preservado`.

**Performance audit; Steps 0 and 1.**
At this point each pixel still crossed PSRAM up to 5 times (CPU draw to virtual
screen, PPA blit to back buffer, front-to-back sync on the next frame), and
scrolling still used a CPU `copyRect` on the virtual screen. Optimization plan:
0 measure → 1 frame pacing + smarter rect merging → 2 direct mode (draw straight
to the back buffer without a virtual screen) with DMA2D front-to-back scroll →
3 eliminate sprites in Radar and diff-render Tasks → 4 adaptive refresh rate.
- **Instrumentation** (`comp_estat`): tracks frame count, drawn vs. synced pixel
  counts, PPA/DMA time, flip wait time, draw time, and total loop duration
  (bucketed histogram: `<4`, `<8`, `<12`, `<16`, `<33`, `>=33` ms), plus CPU
  scroll time. Console commands: `tela medir` starts a capture window, `tela`
  prints results. Monitor logs `quadros-YYYY-MM-DD.csv` once per active minute.
- **Frame pacing:** in fast mode, a loop iteration that presents a frame waits
  for VSYNC completion and starts the next iteration immediately, instead of
  sleeping an extra 20 ms after presenting (which had capped frame rate at ~30
  FPS with beat-frequency judder against the ~63 Hz panel).
- **Rect merging:** 32 dirty-rect slots; when full, merges only the pair that
  introduces the smallest wasted area (previously collapsed everything into a
  single bounding box, often dirtying the entire screen).
- **First benchmark (scrolling, panel at 47.4 Hz):** mean frame loop was 120 ms,
  with 157 of 159 frames taking >33 ms. The culprit was the PPA copy at 88 ms
  per frame (~32 MB/s effective throughput) — our "hardware accelerator" was the
  bottleneck.
- **`tela banco` memory copy benchmark** (720×1024 while panel is scanning out):
  PPA achieved 92 MB/s, raw DMA2D (`esp_async_fbcpy`) hit **288 MB/s**, AXI GDMA
  hit 206 MB/s, and CPU `memcpy` hit 118 MB/s. Switched page-to-page blits from
  PPA to **DMA2D** (keeping PPA only as a fallback). Destination cache uses
  writeback+invalidate because invalidate-only requires 128-byte alignment on
  both edges.
- **Second benchmark (with DMA2D copy):** copy time dropped from 88 → **11.4 ms**
  per frame, and total frame loop dropped from 120 → **40.8 ms**. CPU drawing
  (26.9 ms, including CPU scroll copies) became the largest remaining cost.

**Step 2: Direct mode (LVGL-style dual-buffer direct rendering).**
- Deleted the virtual shadow screen (**−1.84 MB of PSRAM**). The tracker panel
  now draws straight into the back buffer; the first draw call of a UI iteration
  **opens** the frame (`comp_abrir`: awaits the previous flip and syncs dirty
  regions from front → back), and `comp_apresentar` flushes the CPU cache for
  drawn scanlines and requests the page flip. Page swapping updates only the
  sprite's raw buffer pointer (calling `setBuffer` would zero-clear the buffer).
- **Hardware-accelerated scrolling:** `copyRect` now performs a **DMA2D blit from
  the FRONT buffer** (at the old offset) directly into the back buffer (at the
  new offset) whenever the source region hasn't been drawn to yet in the current
  frame; before opening the frame, the scrolled destination is marked covered
  (`comp_cobrir`) so front-to-back sync doesn't waste time copying pixels that
  the scroll blit is about to overwrite.
- Fresh panel (boot, clock change, wake from sleep) = uninitialized buffers:
  `comp_painel_novo()` triggers a full redraw. Any stray draw call while the
  panel is torn down during sleep is redirected to a lazily allocated scratch
  buffer and logged.
- Camera: uses its own DMA surface allocated on first use and freed when the
  Camera app closes, blitted to the back buffer via DMA2D inside `desenhar`.
- Changing the panel clock from the serial console now executes on the UI task
  (`sistema_trocar_relogio_painel`) so another task can never destroy the
  framebuffers mid-draw.
- Loop histogram buckets changed from fixed milliseconds to multiples of the
  measured panel refresh period (`1`, `2`, `3`, `4+` VSYNCs), logged to
  `quadros2-YYYY-MM-DD.csv`.
- **Third benchmark (direct mode):** frame loop dropped from 40.8 → **23.0 ms**,
  with 75% of frames completing within a single panel refresh; draw time dropped
  from 26.9 → **14.0 ms**; scroll blits dropped from 6.9 → **2.2 ms** (DMA2D).
  However, front-to-back sync still accounted for 2/3 of all DMA traffic
  (336 MB vs. 154 MB for scrolling) because apps drew their header *before*
  scrolling their list — opening the frame early and syncing the entire list
  viewport right before the scroll blit overwrote it.
- **Lazy per-region front-to-back sync:** opening a frame now marks the previous
  frame's dirty rectangles as *pending*; a pending region is only copied from the
  front buffer when something is about to draw or read inside it
  (`comp_preparar`, called by the tracker panel including on `readRect`), and
  any untouched pending regions are synced right before flip. Any opaque DMA
  blit that completely covers a pending region (such as a list scroll or camera
  frame) discards that pending region without copying a single byte.
- **Clipped pending regions:** touching part of a pending rectangle now copies
  only the intersecting sub-rectangle from the front buffer and splits the
  remainder into up to 4 smaller pending rectangles. Opaque `fillRect` calls
  likewise clip and discard the covered portion of any pending region instead of
  syncing it first (so clearing the app viewport when opening an app no longer
  copies 1.6 MB from the front buffer just to paint black over it).
- Added a slow-frame log (`desenho completo de <app>: N ms` whenever a full
  redraw exceeds 40 ms) and a per-app breakdown table in `tela`: clearing took
  20–25 ms in every app (CPU writing 1.6 MB of black), while content rendering
  scaled with text volume (Camera 6 ms, Files 40 ms, home 57 ms, Mesh 55 ms,
  Settings 77 ms, Tasks 116 ms).
- **Hardware PPA fills:** any `fillRect` of 16K pixels or larger is routed to
  the PPA hardware `FILL` engine (after flushing/invalidating cache lines). A
  self-test on the first non-uniform color verifies byte order and falls back to
  CPU if anything mismatches. `tela banco` benchmarks PPA-fill vs. CPU-fill.
- **Benchmark with PPA-fill + async image loading:** viewport clear dropped from
  20–25 ms → **~8 ms** across all apps (4.2 ms vs. 14.1 ms for 720×1024 with the
  screen awake — 3.4× faster than CPU). Fixed a 2.3 s worst-case stall in Files
  where 1.4 MB BMP photos were being read from the SD card line-by-line *inside*
  `desenhar()` on every full repaint: the system worker now loads the image once
  into a PSRAM buffer in screen byte order, and `desenhar()` simply blits it
  (dropping Files' worst-case redraw from 2,271 ms → **92 ms**). Mean full-screen
  redraw across the OS dropped to 61 ms, with the remainder dominated by text
  rendering.
- **Why smooth text was expensive:** M5GFX's `VLWfont::drawChar` issues an
  individual 1×1 `writeFillRect` call for every anti-aliased edge pixel plus one
  per background span — roughly 200 virtual calls per 30 px character, each
  passing through the tracker panel.
- **Glyph cache** (`tab5_glyphs`, `FonteRapida` subclassing `VLWfont`): each
  glyph is rasterized once by M5GFX into a tiny RGB565 scratch buffer and cached
  by `(font, codepoint, fg_color, bg_color)` in a 1 MB PSRAM arena; subsequent
  draws emit a single `pushImage` per character. Applies to opaque-background
  text at scale 1 on 16-bit targets for glyphs that don't overhang the previous
  character; everything else transparently falls back to `VLWfont::drawChar`.
  Hit rate and stats are reported in `tela`.
- **Framebuffer epoch counter** (`g_tab5_tela_epoca`): every draw call checks
  whether the panel buffers still exist via a single integer comparison instead
  of querying the panel driver on every pixel.
- **Benchmark with 1 MB glyph cache:** 79% overall hit rate (**97% excluding
  spaces**, which are already cheap single-rect fills), zero arena resets. Mean
  full redraw dropped from 61 → **40 ms**, partial redraw to **4.5 ms**, and
  mean frame loop to **17.9 ms**.
- **Per-field diff rendering in Tasks (Step C):** previously, the Tasks app
  cleared its entire viewport and redrew every string at 1 Hz. Now each text
  line caches what is currently on screen and only repaints when its string
  changes; the task table diffs per row (`lista_item_mudou`); the chart updates
  only its inner plot area once per sample; and static headers, borders, and
  buttons draw only on a full repaint.
