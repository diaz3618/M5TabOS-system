// APP: Camera - live preview of SC2356 (MIPI-CSI) + capture to card.
//
// FIRST VERSION, deliberately small scope - see large warning in
// hal/tab5_camera.hpp for what has already been checked (sensor, pins, bus
// I2C shared with M5.In_I2C, the official example this code follows).
//
// The photo exits with the entire frame of sensor (1200x1600); the preview uses the
// same image at uniform scale, without stretching (see hal/tab5_camera.cpp).
//
// WHAT IS FOR LATER, of purpose:
// - Gallery (view photos taken without exiting to the Files app).
// - Zoom/focus/exposure adjustments.
// None of this is difficult later - and only more than could fit in a single answer
// without becoming a wall of code not tested on any hardware.

#include "app.h"
#include "tab5_camera.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"
#include "tab5_bmp.hpp"
#include "work.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>

static const char *TAG = "app.camera";

static uint16_t* s_foto   = nullptr;   // the photo being captured (TAB5_CAM_PHOTO_W x _A, PSRAM) - of app until it becomes work
static char      s_foto_name[32];
static volatile bool s_gravou_ok = false;   // written by work (static: exists even with the app closed)
static int       s_width, s_height;
static uint32_t  s_fotos = 0;

// CAPTURING: the camera is taking the photo for s_foto (one frame, ~33 ms).
// RECORDING: one WORK LOOSE records the photo on the card (seconds).
// The app DOES NOT stop navigation in either of them. When recording, the buffer
// BECOMES WORK (loose_work_system): it records and releases. By
// This closes the app in the middle of recording and instantly - before closing
// I waited for the entire recording and the interface froze.
typedef enum { BUTTON_IDLE, BUTTON_CAPTURING, BUTTON_SAVING, BUTTON_RESULT_FLASH } capture_button_state_t;
static capture_button_state_t s_estado = BUTTON_IDLE;
static bool     s_flash_sucesso = false;
static uint32_t s_flash_ate_ms = 0;   // flashes the edge of the button for one instant after the result

// CAPTURE button: fixed bottom range, independent of size of area (variables
// little between landscape/portrait, but calculation by fraction avoids magic number).
#define BOTAO_A 96

// BIGGEST "foto_NNNN.bmp" ALREADY ON THE CARD - defect observed: s_fotos
// it started from zero every time the app was opened, so every new session was overwritten
// "foto_0001.bmp" of previous session. The numbering has to come from what already exists
// on the card, in the SAME spirit that tab5_mesh_ Carregar_do_cartao() already uses to
// rebuild the list of conversations from the files - the card and the source
// Of course, it's not a variable in RAM that dies when the app closes.
static uint32_t largest_existing_photo_number(void)
{
  char pasta[128];
  if (!tab5_sd_folder("Camera", pasta, sizeof(pasta))) { return 0; }

  DIR* d = opendir(pasta);
  if (!d) { return 0; }   // folder does not exist yet - first photo of all

  uint32_t maior = 0;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    unsigned long n;
    if (sscanf(e->d_name, "foto_%04lu.bmp", &n) == 1 && (uint32_t)n > maior) {
      maior = (uint32_t)n;
    }
  }
  closedir(d);
  return maior;
}

// THE COUNT AND OF THE WORKER: the readdir of Camera folder ran
// no ao_abrir, in task of interface - with hundreds of photos, the camera
// it took a while to appear. The number is only accurate when CAPTURING; even him
// arrives (AVISO_BALHO), the button waits.
static volatile uint32_t s_fotos_lidas = 0;
static volatile bool     s_fotos_prontas = false;   // the work is finished (result in s_fotos_lidas)
static bool              s_fotos_ok = false;        // s_photos are worth it

static void work_count(void*)
{
  s_fotos_lidas = largest_existing_photo_number();
  s_fotos_prontas = true;
}

// The rectangle of preview, in coordinates of TELA.
static area_t rectangle_preview(void)
{
  const area_t a = system_area();
  return area_t{ a.x, a.y, s_width, s_height };
}

// Turn on the sensor and point the preview DIRECTly at the virtual screen: the PPA of
// camera writes the frame in place, and each frame requests one redraw - which is
// warns the composer (sistema_surface_changed). No copying by CPU.
static bool enable_camera(void)
{
  if (!tab5_camera_start(s_width, s_height)) { return false; }
  surface_t sup;
  if (!system_surface(rectangle_preview(), &sup)) {
    ESP_LOGE(TAG, "sem superficie de DMA para o preview");
    tab5_camera_stop();
    return false;
  }
  const tab5_cam_target_t d = { sup.buf, sup.bytes, sup.pic_l, sup.pic_a, sup.x, sup.y,
                                 system_request_redraw };
  tab5_camera_set_target(&d);
  return true;
}

static bool on_open(void)
{
  const area_t a = system_area();
  s_width = a.width;
  // Multiple of 8: requirement of surface of DMA (system_surface).
  s_height  = (a.height - BOTAO_A - 12) & ~7;
  if (s_width <= 0 || s_height <= 0) { return false; }

  if (!enable_camera()) { return false; }

  s_fotos_ok = false;
  s_fotos_prontas = false;
  if (!system_work(work_count, nullptr)) { s_fotos = 0; s_fotos_ok = true; }
  s_estado = BUTTON_IDLE;
  s_flash_ate_ms = 0;
  ESP_LOGI(TAG, "aberto (preview %dx%d)", s_width, s_height);
  return true;
}

static void on_close(void)
{
  tab5_camera_stop();   // waits for camera task: no PPA writing (screen, s_foto) after here
  // It only exists if the capture has not become work; the one that became and of work.
  if (s_foto) { heap_caps_free(s_foto); s_foto = nullptr; }
  ESP_LOGI(TAG, "fechado");
}

// ao_repouso()/ao_retomar() - crash (camera+sleep), see the
// big warning in kernel/app.h. Solution: the SAME tab5_camera_stop()/
// start() that ao_close()/ao_open() already use - the capture released
// DW_GDMA of it completely (sensor, PPA, background task, CAM_RST) and iteration
// of zero afterwards, without inventing a third "paused" state just for this.
// s_foto is NOT touched here - the app continues to be "open" for the entire team, just
// captures for and iteration.
static void on_sleep(void)
{
  tab5_camera_stop();
}

static void on_resume(void)
{
  // A request for photo in flight died with capture (the driver returned from zero):
  // without this the button would remain on "CAPPING..." forever.
  if (s_estado == BUTTON_CAPTURING) { s_estado = BUTTON_IDLE; }
  if (!enable_camera()) {
    ESP_LOGE(TAG, "falha ao religar a camera apos o repouso");
  }
}

// THE RECORDING REQUEST takes everything you need: the work is released and can
// run with the app already closed (or reopened, with another s_foto_nome).
typedef struct {
  uint16_t* px;
  char      name[32];
} photo_write_t;

// In the system worker: records the photo (entire frame) and releases everything.
static void write_photo_work(void* arg)
{
  photo_write_t* g = (photo_write_t*)arg;
  s_gravou_ok = tab5_bmp_salvar("Camera", g->name, g->px, TAB5_CAM_PHOTO_W, TAB5_CAM_PHOTO_H);
  heap_caps_free(g->px);
  heap_caps_free(g);
}

// Deliver s_foto to one work released. false = did not work (s_foto continues from app).
static bool queue_photo_write(void)
{
  photo_write_t* g = (photo_write_t*)heap_caps_malloc(sizeof(photo_write_t), MALLOC_CAP_DEFAULT);
  if (!g) { return false; }
  g->px = s_foto;
  snprintf(g->name, sizeof(g->name), "%s", s_foto_name);
  if (!system_work_released(write_photo_work, g)) { heap_caps_free(g); return false; }
  s_foto = nullptr;
  return true;
}

static void on_notice(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_fotos_prontas && !s_fotos_ok) {
    s_fotos = s_fotos_lidas;
    s_fotos_ok = true;
    ESP_LOGI(TAG, "%u foto(s) ja no cartao", (unsigned)s_fotos);
  }
  if ((av & AVISO_TRABALHO) && s_estado == BUTTON_SAVING) {
    s_estado = BUTTON_RESULT_FLASH;
    s_flash_sucesso = s_gravou_ok;
    s_flash_ate_ms = pdTICKS_TO_MS(xTaskGetTickCount()) + 600;
  }
  system_request_redraw();
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  // The frame is already in virtual screen (the PPA of camera wrote it there); so
  // the composer needs to know that the area has changed.
  if (tab5_camera_frame_written()) {
    system_surface_changed(rectangle_preview());
  } else if (system_area_foi_limpa()) {
    // Still no frame (sensor just raised) - warns instead of
    // leave the area black looking stuck.
    tab5_text(d, 2);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("starting the camera...", a.x + s_width / 2, a.y + s_height / 2);
    d.setTextDatum(top_left);
  }

  // THE PHOTO HAS ARRIVED AT THE BUFFER: the recording goes to the system worker.
  if (s_estado == BUTTON_CAPTURING) {
    bool ok = false;
    if (tab5_camera_photo_ready(&ok)) {
      if (ok && queue_photo_write()) {
        s_estado = BUTTON_SAVING;
      } else {
        s_estado = BUTTON_RESULT_FLASH;
        s_flash_sucesso = false;
        s_flash_ate_ms = pdTICKS_TO_MS(xTaskGetTickCount()) + 600;
      }
    }
  } else if (s_estado == BUTTON_RESULT_FLASH && pdTICKS_TO_MS(xTaskGetTickCount()) >= s_flash_ate_ms) {
    s_estado = BUTTON_IDLE;
  }

  // ---- bottom bar: CAPTURE button + counter -----------------------
  const int by = a.y + s_height + 12;
  const char* text = "CAPTURAR";
  uint16_t    color    = TFT_ORANGE;
  if (s_estado == BUTTON_CAPTURING)                    { text = "CAPTURANDO...";      color = TFT_DARKCYAN; }
  else if (s_estado == BUTTON_SAVING)                 { text = "SALVANDO...";        color = TFT_DARKCYAN; }
  else if (s_estado == BUTTON_RESULT_FLASH && s_flash_sucesso) { text = "CAPTURADO!";         color = TFT_GREEN;    }
  else if (s_estado == BUTTON_RESULT_FLASH)                    { text = "FALHOU";             color = TFT_RED;      }

  tab5_botao(d, a.x + 16, by, a.width - 32, BOTAO_A - 12,
             text, color, TFT_BLACK, TAB5_BOTAO_TEXTO_GRANDE);

  // WHILE THERE IS AN ORDER PENDING, the app needs to keep checking -
  // period_ms alone already covers this (redraws every 100ms of any
  // way), there is no need for an extra request here.
}

static void on_touch(int x, int y)
{
  const int by = s_height + 12;
  if (y < by || y >= by + BOTAO_A - 12) { return; }
  // ALSO BLOCKS DURING FLASH ("CAPPED!"/"FAILED", 600ms) - not only
  // during CAPTURING_STATE. Found on review "and press
  // repeatedly": one touch during flash went through
  // old guard (who only looked at CAPTURING), a SECOND capture began
  // and the START of new was stuck - seen as "the button is not iteration on
  // normal", when in fact it had come back and been stopped again by one
  // touch that no one was sure had happened.
  if (s_estado != BUTTON_IDLE) { return; }
  if (!s_fotos_ok) { return; }   // the numbering is still being read from the card (instantly)

  // One buffer per photo: the previous one was with the recording work.
  if (!s_foto) { s_foto = tab5_camera_alloc_photo(); }
  if (!s_foto) { ESP_LOGE(TAG, "sem PSRAM para a foto"); return; }
  if (!tab5_camera_request_photo(s_foto)) { return; }
  snprintf(s_foto_name, sizeof(s_foto_name), "foto_%04lu", (unsigned long)(s_fotos + 1));
  ++s_fotos;
  s_estado = BUTTON_CAPTURING;

  // SHUTTER "CLICK" - in time of touch, like on a camera
  // of truth: confirms "I captured THIS instant". 600 Hz ("I found it sharp").
  system_beep(600.0f, 40);
  system_request_redraw();
}

// -------------------------------------------------------------------------------
// HOME SCREEN ICON - body of camera + lens, primitives.
// -------------------------------------------------------------------------------
static void icon_camera(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(45, 40, 20);
  const uint16_t body = TFT_ORANGE;

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  const int cx = x + lado / 2, cy = y + lado / 2 + (int)(0.06f * lado);
  const int body_l = (int)(0.62f * lado), body_a = (int)(0.40f * lado);
  d.fillRoundRect(cx - body_l / 2, cy - body_a / 2, body_l, body_a, lado / 14, body);

  // "Visor" protruding on top, left side of body.
  const int vis_l = (int)(0.18f * lado), vis_a = (int)(0.10f * lado);
  d.fillRoundRect(cx - body_l / 2 + (int)(0.06f * lado), cy - body_a / 2 - vis_a + 2,
                   vis_l, vis_a, lado / 20, body);

  // Lens: two concentric circles.
  const int r_ext = (int)(0.16f * lado), r_int = (int)(0.10f * lado);
  d.fillCircle(cx, cy, r_ext, fundo);
  d.drawCircle(cx, cy, r_ext, body);
  d.fillCircle(cx, cy, r_int, body);
}

extern const app_t APP_CAMERA = {
  .name  = "Camera",
  .color   = TFT_ORANGE,
  // WITHOUT .camera=true HERE - the camera became a service (count
  // of reference, see the big warning in hal/tab5_camera.hpp) and turn on the
  // CAM_RST itself alone inside of tab5_camera_start(), in the same
  // spirit that Grove already does for GPS/Mesh. Declaring here would also be
  // are redundant (the kernel would still apply before of ao_abrir(), but who
  // guarantees the energy of truth now and the service itself).
  //
  // .speaker=true (along with click of shutter in
  // ao_tocar()): on the contrary of camera, the speaker is NOT able to
  // auto-declare after boot (reserving I2S pins of ES8388 and
  // decision of PHASE 1, before of M5.begin() - see the big warning in
  // settings.h). This app uses sound of truth now, so it declares - even
  // that the Adjustments app already declares the same today, and it is a union (OR), not competition.
  .profile = { .speaker = true },
  .uses_sd = true,
  .requires_sd = true,         // without a card there is nothing to do (see app.h)
  .sleep_clock_floor_mhz = 0,
  .on_open   = on_open,
  .on_close  = on_close,
  .on_sleep = on_sleep,
  .on_resume = on_resume,
  .draw  = draw,
  .on_touch  = on_touch,
  // THE PREVIEW DOES NOT DEPEND ON THE CLOCK: each frame written by
  // camera asks for one redraw in time (tab5_cam_target_t.notify). The period
  // only presses the button (CAPTURING/SAVING/flash) when there is no frame.
  .redraw_period_ms = 100,
  .notices    = AVISO_TRABALHO,   // photo recording has finished
  .on_notice  = on_notice,
  .icon = icon_camera,
};
