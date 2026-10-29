#include "tab5_camera.hpp"
#include "tab5_marks.hpp"
#include "app_task.h"
#include "tab5_power.hpp"
#include "tab5_mutex.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_cam_sensor.h"
#include "esp_video_caps.h"
#include "linux/videodev2.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdio>

static const char* TAG = "tab5_camera";

// O sensor of driver of esp_video. A funcao e global, mas o header que a
// declara e privado of componente (esp_video_device_internal.h).
extern "C" esp_cam_sensor_device_t* esp_video_get_csi_video_device_sensor(void);

#define TAB5_CAM_NBUF 2

// O SENSOR ESTA MONTADO DE LADO in placa - o mesmo BSP_CAMERA_ROTATION=270 of
// BSP oficial. Sem isto a imagem sairia deitada 90 graus.
#define TAB5_CAM_ROTACAO PPA_SRM_ROTATION_ANGLE_270

typedef struct {
  int      fd;
  uint8_t* bruto[TAB5_CAM_NBUF];   // buffers MMAP of driver (formato of pipeline, ver abaixo)
  size_t   bruto_size;
  uint32_t bruto_width, bruto_height;

  ppa_client_handle_t ppa;
  SemaphoreHandle_t   ppa_end;   // given by the ISR of PPA at the end of each operation (see ppa_rodar)

  int       alvo_width, alvo_height;   // size of preview
  // Preview com escala uniforme (ver o laco of captura): o pedaco central of
  // frame bruto que vai para a screen e a escala, em passos of 1/16 (a
  // precisao of PPA). Calculados uma vez, when o formato of sensor e value_read.
  int       recorte_w, recorte_h;
  float     escala;
  tab5_cam_target_t target;          // onde o preview e escrito (buf nulo = nenhum)
  bool      tem_novo;
  SemaphoreHandle_t trava;

  app_task_t tarefa;
  bool on;
  bool video_iniciado;   // esp_video_init() teve sucesso - precisa of esp_video_deinit() no stop()

  // PHOTO REQUEST - see tab5_camera_request_photo(). Protected by 'lock'.
  uint16_t* foto_destino;      // != nullptr = request pending
  bool      foto_pronta;
  bool      foto_ok;
} tab5_camera_t;

static tab5_camera_t s_cam = { .fd = -1 };

static inline void lock(void)    { if (s_cam.trava) { xSemaphoreTake(s_cam.trava, portMAX_DELAY); } }
static inline void destravar(void) { if (s_cam.trava) { xSemaphoreGive(s_cam.trava); } }

// ---------------------------------------------------------------------------
// CONTAGEM DE REFERENCIA - ver o aviso grande em tab5_camera.hpp.
// ---------------------------------------------------------------------------
// OWN LOCK, separate from 's_cam.trava' (which only protects the
// FRAME - index_ready/has_new/request - read and written at each iteration of
// loop of captura). Esta aqui protege a TRANSICAO 0<->1 of s_refs.
//
// COMENTARIO ATUALIZADO - o reason original ("o comando 'foto'
// of console calls these functions of its own task 'console'") NO LONGER
// VERDADE: aquilo causava colisao of I2C of verdade (ver o aviso grande em
// tab5_camera.hpp) e foi corrigido com a ponte kernel/core.cpp
// (sistema_camera_start/stop) - TODAY, every call to tab5_camera_start/
// stop() acontece NA tarefa of interface, direto (app Camera) ou servida
// pela ponte (console), nunca as duas on mesmo time of tarefas diferentes.
// The lock became, of purpose, a CHEAP security network against someone
// one dia chamar estas funcoes direto of uma terceira tarefa, violando o
// contract - not protecting would cost almost nothing today, but removing and assuming
// que ninguem nunca vai violar o contrato, e essa aposta ja perdeu uma vez
// neste mesmo file.
static SemaphoreHandle_t s_ctrl = nullptr;
static int               s_refs = 0;

static inline void lock_ctrl(void)    { tab5_mutex_lock(&s_ctrl); }
static inline void destravar_ctrl(void) { tab5_mutex_destravar(&s_ctrl); }

bool tab5_camera_active(void) { return s_refs > 0; }

// Idem - iniciar_hardware() chama no caminho of erro (goto erro), antes of
// parar_hardware() estar definida in leitura of file.
static void stop_hardware(void);

// THE PPA IN BLOCKING MODE DOES NOT SURVIVE CLOSURE (reading
// esp_driver_ppa/src/ppa_core.c). No mode BLOCKING o driver espera com
// xSemaphoreTake(trans_elm->sem, portMAX_DELAY) e IGNORA o return. Mas
// tarefa_app_encerrar() acorda a tarefa com xTaskAbortDelay() - e, se ela
// estiver nessa espera, o driver devolve "ESP_OK" com a operacao AINDA
// RODANDO no hardware. Dai em diante: QBUF of one buffer que o PPA ainda le,
// e o ppa_unregister_client() recusando (a transacao ainda conta) - era
// this, and not just the race of ISR, is the source of failed attempts
// vistas nos testes.
//
// The output: NON-blocking mode, with one semaphore OUR given by the ISR, and a
// espera que, se for abortada, simplesmente espera of novo. A tarefa so exit
// of PPA when o hardware of fato terminou.
static bool IRAM_ATTR on_end_ppa(ppa_client_handle_t, ppa_event_data_t*, void* sem)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &acordou);
  return acordou == pdTRUE;
}

static esp_err_t ppa_rodar(ppa_srm_oper_config_t* cfg)
{
  cfg->mode      = PPA_TRANS_MODE_NON_BLOCKING;
  cfg->user_data = s_cam.ppa_end;
  const esp_err_t err = ppa_do_scale_rotate_mirror(s_cam.ppa, cfg);
  if (err != ESP_OK) { return err; }
  // Uma operacao leva poucos ms; 1 s so estoura se o PPA travou of vez.
  for (int i = 0; i < 20; ++i) {
    if (xSemaphoreTake(s_cam.ppa_end, pdMS_TO_TICKS(50)) == pdTRUE) { return ESP_OK; }
  }
  return ESP_ERR_TIMEOUT;
}

// O PRETO DO SENSOR. O SC202CS entrega o preto num pedestal: o laco of level
// of his black targets the target of registers 0x3907/0x3908, which the table of
// inicializacao of driver deixa em 0x041 (65 em 10 bits, 16 no RAW8). A
// calibracao of componente conta com isso (bloco "blc" of 16 no JSON) - mas
// who subtrai e o bloco BLC of ISP, que so existe in v3 of P4
// (ESP_VIDEO_ISP_DEVICE_BLC, esp_video_caps.h). Abaixo of v3 o pedestal passa
// direto: vira one offset fixo somado a TODO pixel, depois multiplicado
// pelo balanco of branco e pela CCM. Com os ganhos of luz of dia isso of
// 16 x (2,9 ; 0,4 ; 2,0) em linear - o preto exit roxo-escuro e a imagem
// inteira ganha one veu magenta, mais forte nas sombras (a gama amplia o
// full scale). The output and request on the sensor itself is black at zero.
// After STREAMON: the format rewrites the initialization table.
#define TAB5_CAM_PRETO_REG   0x3908
#define TAB5_CAM_PRETO_ALVO  0x00   // of fabrica 0x41

static void reset_pedestal(void)
{
#if !ESP_VIDEO_ISP_DEVICE_BLC
  esp_cam_sensor_device_t* sensor = esp_video_get_csi_video_device_sensor();
  if (!sensor) { return; }
  esp_cam_sensor_reg_val_t r = { .regaddr = TAB5_CAM_PRETO_REG, .value = TAB5_CAM_PRETO_ALVO };
  const esp_err_t err = esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r);
  if (err != ESP_OK) { ESP_LOGW(TAG, "alvo do preto no sensor: %s", esp_err_to_name(err)); }
#endif
}

bool tab5_camera_reg(uint16_t reg, int value, uint8_t* value_read)
{
  if (!s_cam.on) { return false; }
  esp_cam_sensor_device_t* sensor = esp_video_get_csi_video_device_sensor();
  if (!sensor) { return false; }
  esp_cam_sensor_reg_val_t r = { .regaddr = reg, .value = 0 };
  if (value >= 0) {
    r.value = (uint32_t)value;
    if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r) != ESP_OK) { return false; }
  }
  if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_G_REG, &r) != ESP_OK) { return false; }
  if (value_read) { *value_read = (uint8_t)r.value; }
  return true;
}

// THE CAPTURE TASK. DQBUF blocks until the sensor delivers the next frame
// (free_space-rodante, ~30fps) - that's why it lives in task_app_t, and not in task of
// interface: se o sensor lock, who stays preso e SO esta tarefa (ver o
// aviso grande em kernel/app_task.h sobre "abandoned of proposito" - o
// mesmo raciocinio que ja vale para Mesh/GPS vale aqui).
static void laco_captura(app_task_t* t)
{
  const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int64_t t_conta = esp_timer_get_time();
  int frames = 0;

  while (app_task_active(t)) {
    struct v4l2_buffer buf = { };
    buf.type   = type;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
      // xTaskAbortDelay DOES NOT wake up one ioctl locked - if it fell here for one
      // true error (not because we have request for exit), avoids busy-loop.
      if (app_task_active(t)) { vTaskDelay(pdMS_TO_TICKS(50)); }
      continue;
    }

    // The basis of all operation: the raw frame of sensor (1600x1200), rotated 270
    // (o sensor esta mounted of lado - BSP_CAMERA_ROTATION=270) e espelhado
    // (mirror_x e no bloco of SAIDA, pos-rotacao - left/right vinham
    // trocadas).
    ppa_srm_oper_config_t base = { };
    base.in.buffer   = s_cam.bruto[buf.index];
    base.in.pic_w    = s_cam.bruto_width;
    base.in.pic_h    = s_cam.bruto_height;
    base.in.block_w  = s_cam.bruto_width;
    base.in.block_h  = s_cam.bruto_height;
    base.in.srm_cm   = PPA_SRM_COLOR_MODE_RGB565;
    base.out.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    base.rotation_angle = TAB5_CAM_ROTACAO;
    base.mirror_x    = true;

    lock();
    const tab5_cam_target_t dst = s_cam.target;
    uint16_t* foto = s_cam.foto_destino;
    destravar();

    // PREVIEW DIRETO NO DESTINO: o PPA escreve o frame ja
    // rotated and scaled in the rectangle that the app indicated - in practice, within
    // of TELA VIRTUAL of sistema. Antes eram quatro viagens pela PSRAM por
    // frame (PPA -> buffer of camera -> memcpy of app -> pushImage in screen
    // virtual -> composer); added on panel, the band did not close at 30 fps
    // e a imagem chegava atrasada. Agora sao duas (PPA -> screen virtual ->
    // compositor).
    //
    // ESCALA UNIFORME, PREENCHENDO: a mesma escala nos dois eixos (a imagem
    // does not stretch) is the smallest one that still covers the entire rectangle; what's left
    // of frame exit por igual dos dois lados. No retrato of Tab5 isso corta
    // uns 4% of width - o resto of frame e o que a foto grava.
    esp_err_t err = ESP_OK;
    if (dst.buf) {
      ppa_srm_oper_config_t cfg = base;
      cfg.out.buffer         = dst.buf;
      cfg.out.buffer_size    = dst.bytes;
      cfg.out.pic_w          = dst.pic_l;
      cfg.out.pic_h          = dst.pic_a;
      cfg.out.block_offset_x = dst.x;
      cfg.out.block_offset_y = dst.y;
      cfg.in.block_w         = s_cam.recorte_w;
      cfg.in.block_h         = s_cam.recorte_h;
      cfg.in.block_offset_x  = (s_cam.bruto_width - s_cam.recorte_w) / 2;
      cfg.in.block_offset_y  = (s_cam.bruto_height  - s_cam.recorte_h) / 2;
      cfg.scale_x = cfg.scale_y = s_cam.escala;
      err = ppa_rodar(&cfg);
    }

    // FOTO PEDIDA: o MESMO frame bruto, inteiro e sem escala - 1600x1200
    // girado of exatamente 1200x1600, o field of visao todo of sensor. Uma
    // operation of PPA (a few months): the preview does not stop, and the recording remains with
    // who pediu.
    if (foto) {
      ppa_srm_oper_config_t fc = base;
      fc.out.buffer      = foto;
      fc.out.buffer_size = tab5_camera_photo_bytes();
      fc.out.pic_w       = TAB5_CAM_PHOTO_W;
      fc.out.pic_h       = TAB5_CAM_PHOTO_H;
      fc.scale_x = (float)TAB5_CAM_PHOTO_H / (float)s_cam.bruto_width;
      fc.scale_y = (float)TAB5_CAM_PHOTO_W / (float)s_cam.bruto_height;
      const esp_err_t ef = ppa_rodar(&fc);
      lock();
      s_cam.foto_destino = nullptr;
      s_cam.foto_ok      = (ef == ESP_OK);
      s_cam.foto_pronta  = true;
      destravar();
      if (ef != ESP_OK) { ESP_LOGW(TAG, "foto: PPA failed (%s)", esp_err_to_name(ef)); }
    }

    // Devolve o buffer BRUTO on driver JA - o sensor pode reusar o slot.
    ioctl(s_cam.fd, VIDIOC_QBUF, &buf);

    if (dst.buf && err == ESP_OK) {
      lock();
      s_cam.tem_novo = true;
      destravar();
      if (dst.notify) { dst.notify(); }
    }
    if (err != ESP_OK) { ESP_LOGW(TAG, "PPA failed: %s", esp_err_to_name(err)); }

    // Frames per second of true, every 10 s - the number that tells whether the
    // sensor (ou a exposicao automatica, que alonga o frame no escuro) esta
    // entregando os 30 qps.
    ++frames;
    const int64_t now = esp_timer_get_time();
    if (now - t_conta >= 10000000) {
      ESP_LOGI(TAG, "%.1f frames/s", frames * 1e6 / (double)(now - t_conta));
      t_conta = now;
      frames = 0;
    }
  }
}

// A INICIALIZACAO DE VERDADE - so roda in transicao 0->1 of s_refs (ver
// tab5_camera_start() mais abaixo, a funcao PUBLICA que decide when
// chamar esta aqui).
static bool start_hardware(int width, int height)
{
  if (width <= 0 || height <= 0) { return false; }

  s_cam = { .fd = -1 };
  s_cam.alvo_width = width;
  s_cam.alvo_height  = height;
  s_cam.trava = xSemaphoreCreateMutex();
  if (!s_cam.trava) { return false; }

  // A CAMERA SE ALIMENTA SOZINHA - ver o aviso grande no .hpp ("A CAMERA SE
  // ALIMENTA SOZINHA AGORA"). ANTES of qualquer coisa, inclusive antes of
  // I2C handle: the SCCB of sensor (used just below, within
  // esp_video_init) only responds with CAM_RST outside of reset.
  tab5_power_camera(true);

  // FOLGA PARA O SENSOR ACORDAR - achado em hardware testando
  // 'foto' pelo CONSOLE: "Get sensor ID failed" no first SCCB, sempre.
  //
  // O driver of SC202CS (sc202cs.c, sc202cs_hw_reset) TEM one delay of 10ms
  // after release the reset - but SO when 'reset_pin' is one GPIO that itself
  // driver controls. Ours CAM_RST and out (I2C expander, see
  // tab5_power_camera()), entao passamos GPIO_NUM_NC para reset_pin/pwdn_pin
  // (ver csi_cfg abaixo) - o driver nunca sabe que acabamos of tirar o
  // sensor of reset, and skips its own delay. WE have to give this
  // folga, e antes disto ela sempre "acontecia" por acidente: o app Camera
  // still mounted the card (usa_sd=true) and drew "opening..." before
  // ao_abrir() chegar aqui, dezenas of ms of sobra sem ninguem ter request.
  // O comando 'foto' chama esta funcao direto, sem nenhum desses passos no
  // storage - e foi so ai que a falta of delay EXPLICITO became visivel. 20ms:
  // double what the driver himself uses for the case he knows, slack
  // confortavel sem atrasar percebivelmente a abertura of app Camera.
  vTaskDelay(pdMS_TO_TICKS(20));

  // THE BUS IS THE SAME AS THE REST OF THE SYSTEM (M5.In_I2C) - see warning
  // big in .hpp. Get the handle already installed, DO NOT create a new one.
  i2c_master_bus_handle_t i2c_handle = nullptr;
  if (i2c_master_get_bus_handle(I2C_NUM_1, &i2c_handle) != ESP_OK || !i2c_handle) {
    ESP_LOGE(TAG, "internal I2C bus (I2C_NUM_1) is not ready - has M5.begin() run?");
    vSemaphoreDelete(s_cam.trava);
    s_cam.trava = nullptr;
    return false;
  }

  const esp_video_init_csi_config_t csi_cfg = {
    .sccb_config = {
      .init_sccb = false,
      .i2c_handle = i2c_handle,
      .freq = 400000,
    },
    .reset_pin = GPIO_NUM_NC,   // CAM_RST e o expansor I2C, ja aplicado via profile.camera - ver .hpp
    .pwdn_pin  = GPIO_NUM_NC,
  };
  esp_video_init_config_t video_cfg = { .csi = &csi_cfg };

  if (esp_video_init(&video_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init failed - confira CONFIG_CAMERA_SC202CS no menuconfig");
    vSemaphoreDelete(s_cam.trava);
    s_cam.trava = nullptr;
    return false;
  }
  s_cam.video_iniciado = true;

  s_cam.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (s_cam.fd < 0) {
    ESP_LOGE(TAG, "abrir %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    goto erro;
  }

  {
    // O FORMATO PADRAO DO SENSOR E RAW8 (Bayer raw) - conferido no sdkconfig
    // apos baixar o componente (CONFIG_CAMERA_SC202CS_MIPI_DEFAULT_FMT_RAW8_*).
    // Isto e o sensor of verdade falando: uma camera CMOS como esta NUNCA
    // delivers direct RGB, only the default of Bayer. Who makes the demosaic and the ISP
    // of own chip (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER, already
    // on) - mas so when a gente PEDE RGB565 via VIDIOC_S_FMT neste MESMO
    // /dev/video0. Sem este step, o PPA logo abaixo receberia Bayer raw e
    // tentaria le-lo como RGB565 - a imagem sairia one chuvisco sem sentido
    // none, not a "weird" photo: raw bytes of sensor, color none.
    struct v4l2_format fmt = { };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
      ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
      goto erro;
    }
    ESP_LOGI(TAG, "sensor default format: %ux%u, fourcc 0x%08lx (RAW Bayer data, ISP not applied yet)",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned long)fmt.fmt.pix.pixelformat);

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
      struct v4l2_format request = { };
      request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      request.fmt.pix.width       = fmt.fmt.pix.width;
      request.fmt.pix.height      = fmt.fmt.pix.height;
      request.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
      if (ioctl(s_cam.fd, VIDIOC_S_FMT, &request) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(RGB565) failed - the ISP rejected the conversion");
        goto erro;
      }
      // Confirma o que realmente became valendo - o driver pode ajustar.
      memset(&fmt, 0, sizeof(fmt));
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT (apos S_FMT) failed");
        goto erro;
      }
    }

    s_cam.bruto_width = fmt.fmt.pix.width;
    s_cam.bruto_height  = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "capturando: %ux%u, fourcc 0x%08lx", (unsigned)s_cam.bruto_width,
             (unsigned)s_cam.bruto_height, (unsigned long)fmt.fmt.pix.pixelformat);
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
      ESP_LOGE(TAG, "driver selected fourcc 0x%08lx, not RGB565 - PPA would receive invalid data",
               (unsigned long)fmt.fmt.pix.pixelformat);
      goto erro;
    }

    // ESCALA DO PREVIEW. Girado 270, a width of screen vem of ALTURA of
    // frame bruto e a height of screen, of LARGURA. A escala e a menor, em
    // 1/16, which covers both axes; the cutout and what fits with it.
    {
      const int L = s_cam.alvo_width, A = s_cam.alvo_height;
      const int W = (int)s_cam.bruto_width, H = (int)s_cam.bruto_height;
      int s16 = (L * 16 + H - 1) / H;
      const int s16a = (A * 16 + W - 1) / W;
      if (s16a > s16) { s16 = s16a; }
      if (s16 < 1) { s16 = 1; }
      s_cam.escala    = (float)s16 / 16.0f;
      s_cam.recorte_h = (L * 16) / s16; if (s_cam.recorte_h > H) { s_cam.recorte_h = H; }
      s_cam.recorte_w = (A * 16) / s16; if (s_cam.recorte_w > W) { s_cam.recorte_w = W; }
      ESP_LOGI(TAG, "preview %dx%d: scale %d/16, using %dx%d from the %dx%d frame",
               L, A, s16, s_cam.recorte_w, s_cam.recorte_h, W, H);
    }

    // EXPOSICAO E COR SAO DO ISP. O ganho manual fixo que havia
    // aqui (indice 16 = 1,5x) era remendo para a imagem escura
    // a causa era o controlador of pipeline of ISP DESLIGADO no sdkconfig
    // (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER): sem ele nada rodava
    // exposicao automatica (AGC/AE) nem balanco of branco (AWB) - o sensor
    // delivered Bayer "raw", green (two green pixels out of four, without
    // correcao) e no ganho minimum. Ligado, a tarefa "isp_task" of esp_video
    // reads ISP statistics every frame and adjusts gain/exposure of
    // sensor (by SCCB, which passes through the I2C lock - hal/tab5_sccb.cpp) and
    // os ganhos of color/CCM/gama of ISP, com a calibracao of SC202CS que vem
    // no componente. Um ganho fixo aqui so brigaria com ela.
    // A calibracao em uso e a PROPRIA, hal/sc202cs_tab5.json (exposicao
    // mais low contra o branco estourado) - o porque esta no
    // sdkconfig.defaults, secao "Camera". O branco magenta em si e resolvido
    // in matrix of color, by one IPA itself: hal/tab5_camera_ipa.c.

    struct v4l2_requestbuffers req = { };
    req.count  = TAB5_CAM_NBUF;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
      ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
      goto erro;
    }

    for (int i = 0; i < TAB5_CAM_NBUF; ++i) {
      struct v4l2_buffer buf = { };
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%d) failed", i);
        goto erro;
      }
      s_cam.bruto[i] = (uint8_t*)mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, s_cam.fd, buf.m.offset);
      if (!s_cam.bruto[i]) { ESP_LOGE(TAG, "mmap(%d) failed", i); goto erro; }
      s_cam.bruto_size = buf.length;
      if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QBUF(%d) inicial failed", i);
        goto erro;
      }
    }
  }

  {
    const ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    if (ppa_register_client(&ppa_cfg, &s_cam.ppa) != ESP_OK) {
      ESP_LOGE(TAG, "ppa_register_client failed");
      goto erro;
    }
    s_cam.ppa_end = xSemaphoreCreateBinary();
    const ppa_event_callbacks_t cbs = { .on_trans_done = on_end_ppa };
    if (!s_cam.ppa_end || ppa_client_register_event_callbacks(s_cam.ppa, &cbs) != ESP_OK) {
      ESP_LOGE(TAG, "callback do PPA failed");
      goto erro;
    }
  }

  {
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
      ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
      goto erro;
    }
  }
  reset_pedestal();

  // Only DQBUF, PPA and QBUF - the recording of photo exited from here (see
  // tab5_camera_request_photo), entao a stack iteration aos 4 KB originais.
  if (!app_task_create(&s_cam.tarefa, laco_captura, "cam", 4096, 3)) {
    ESP_LOGE(TAG, "could not start the capture task");
    { const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type); }
    goto erro;
  }

  s_cam.on = true;
  tab5_mark("camera: ligada", 0);
  ESP_LOGI(TAG, "camera no ar (%dx%d, rotacao %d)", width, height, (int)TAB5_CAM_ROTACAO);
  return true;

erro:
  stop_hardware();
  return false;
}

// O DESLIGAMENTO DE VERDADE - so roda in transicao 1->0 of s_refs (ver
// tab5_camera_stop() below). Also call directly
// iniciar_hardware() no caminho of erro (goto erro acima) - por isso
// continua segura of chamar mesmo com a inicializacao pela half, exatamente
// como o comentario original desta funcao ja avisava.
static void stop_hardware(void)
{
  tab5_mark("camera: desligando", 0);
  // QUANTO CUSTA DESLIGAR - roda in tarefa of interface (ao_fechar), entao
  // every month here and screen stops. The log tells you which step weighed.
  const int64_t t0 = esp_timer_get_time();
  int64_t t_tarefa = t0, t_ppa = t0, t_video = t0;

  if (s_cam.on) {
    // The task only does DQBUF (blocks until the next frame, ~33 ms) and PPA -
    // none written on the card. 500 ms and slack of remainder; if it doesn't exit,
    // tarefa_app_encerrar() a abandona (ver app_task.h).
    app_task_stop(&s_cam.tarefa, 500);
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
  }
  s_cam.on = false;
  t_tarefa = esp_timer_get_time();

  // "client still has unprocessed trans" - CAUSA RAIZ CONFIRMADA lendo o
  // source of driver (esp_driver_ppa/src/ppa_core.c), not only supposed:
  //
  // ppa_transaction_done_cb() (the ISR of end of transaction) does, in ORDER,
  // within SAME critic:
  //     1. xSemaphoreGiveFromISR(trans_elm->sem, ...)   <- acorda who
  //        chamou ppa_do_scale_rotate_mirror (nos, em mode bloqueante)
  //     2. client->trans_cnt--                          <- SO DEPOIS
  //
  //   ppa_unregister_client() recusa se trans_cnt != 0. Ou seja: existe uma
  //   window real, of poucas instrucoes, entre "nossa chamada bloqueante
  // returns control" and "the driver itself finishes of count that
  //   transacao como concluida". Em SMP (P4 e dual-core, e a tarefa "cam"
  // does not have fixed affinity - task_app_create uses tskNO_AFFINITY) this
  //   window pode realmente ser cruzada.
  //
  // IT IS NOT OPTIONAL TO AVOID - it is a race documented in the driver itself, not
  // one bug ours para "consertar direito"; tentativas curtas espacadas SAO o
  // conserto correto. O que dava para melhorar era o ruido: one vTaskDelay(1)
  // ANTES of primeira tentativa (a folga que a window realmente precisa, so
  // that given of purpose rather than discovery by trial and error) makes the
  // 1a tentativa passar in quase totalidade das vezes; e como a message of
  // error exit of DENTRO of driver (ESP_LOGE, not one log ours), the only way
  // of not getting scared in the log with a condition already treated and lowering the level
  // daquela TAG so durante a tentativa e return_card depois.
  //
  // ORCAMENTO AMPLIADO - seen em hardware: as 5 tentativas
  // originals (100ms of slack) were sometimes NOT enough ("not confirmed" in the
  // log, even with the system still alive afterwards). IT'S NOT JUST COSMETIC: I was
  // read ppa_register_client()/ppa_unregister_client() em ppa_core.c, e
  // when 'do_unregister' of falso a funcao devolve ANTES of free() of
  // client (heap_caps_calloc'd) e antes of release a referencia no motor SRM
  // shared - ou seja, esgotar as tentativas VAZA aquela estrutura of
  // True, it doesn't just print an ugly warning. 20 attempts of 25ms (max. storage
  // segundo, e SO when a corrida realmente acontece) e one preco invisivel
  // no fechamento of camera para reduzir bastante a chance of esgotar - o
  // residual that may still leak if even that is not enough stays registered
  // here, not hidden: if this warning appears again, the next step is
  // investigar o que estaria atrasando a ISR of PPA por MEIO SEGUNDO
  // integer, do not increase the number of new.
  // (a outra source, maior, era o aborto of espera bloqueante
  // resolvida em ppa_rodar(). As tentativas ficam para a window of ISR.)
  if (s_cam.ppa) {
    vTaskDelay(pdMS_TO_TICKS(1));   // a folga que a window documentada acima precisa

    esp_log_level_set("ppa_core", ESP_LOG_NONE);
    esp_err_t err = ESP_FAIL;
    for (int tentativa = 0; tentativa < 20; ++tentativa) {
      err = ppa_unregister_client(s_cam.ppa);
      if (err == ESP_OK) { break; }
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    esp_log_level_set("ppa_core", ESP_LOG_INFO);

    if (err != ESP_OK) { ESP_LOGW(TAG, "ppa_unregister_client did not confirm (%s) - continuing anyway (PPA client leaked)", esp_err_to_name(err)); }
    s_cam.ppa = nullptr;
  }
  if (s_cam.ppa_end) { vSemaphoreDelete(s_cam.ppa_end); s_cam.ppa_end = nullptr; }
  t_ppa = esp_timer_get_time();

  for (int i = 0; i < TAB5_CAM_NBUF; ++i) {
    if (s_cam.bruto[i]) { munmap(s_cam.bruto[i], s_cam.bruto_size); s_cam.bruto[i] = nullptr; }
  }
  if (s_cam.fd >= 0) { close(s_cam.fd); s_cam.fd = -1; }

  // O PASSO QUE FALTAVA (defeito observado): esp_video_init() registra
  // dispositivos GLOBAIS (/dev/video0, e /dev/video20 "ISP" com o pipeline
  // on) que sobrevivem on fd closed - sem desfazer isso, a PROXIMA
  // chamada a esp_video_init() (no next ao_abrir()) encontra "/dev/video20
  // already registered" and failure, and the app stays stuck refusing to open until restarted
  // the device. esp_video_deinit() and the exact pair of esp_video_init().
  if (s_cam.video_iniciado) {
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) { ESP_LOGW(TAG, "esp_video_deinit failed (%s)", esp_err_to_name(err)); }
    s_cam.video_iniciado = false;
  }
  t_video = esp_timer_get_time();

  if (s_cam.trava) { vSemaphoreDelete(s_cam.trava); s_cam.trava = nullptr; }
  s_cam.target = { };
  s_cam.tem_novo = false;

  // PAR EXATO of tab5_power_camera(true) no comeco of iniciar_hardware() -
  // see the big warning in the .hpp. FINALLY: everything else has already been dismantled
  // above, so there is no risk of leaving the sensor without power with any
  // outra parte of desligamento ainda tentando falar SCCB com ele.
  tab5_power_camera(false);

  const int64_t t_end = esp_timer_get_time();
  ESP_LOGI(TAG, "desligada em %lld ms (tarefa %lld, ppa %lld, esp_video %lld, energia %lld)",
           (t_end - t0) / 1000, (t_tarefa - t0) / 1000, (t_ppa - t_tarefa) / 1000,
           (t_video - t_ppa) / 1000, (t_end - t_video) / 1000);
}

// ---------------------------------------------------------------------------
// API PUBLICA - so aqui mora a CONTAGEM DE REFERENCIA (ver o aviso grande no
// .hpp). start_hardware()/stop_hardware() above are never called outside
// destas duas funcoes.
// ---------------------------------------------------------------------------
bool tab5_camera_start(int width, int height)
{
  lock_ctrl();

  bool ok;
  if (s_refs > 0) {
    // JA LIGADA - so aceita mais one interessado se request a MESMA resolucao
    // (ver "UMA UNICA RESOLUCAO POR VEZ" no .hpp). Comparar com s_cam
    // directly and safely inside the lock: nothing else writes
    // target_width/target_height out of start_hardware().
    if (width == s_cam.alvo_width && height == s_cam.alvo_height) {
      ++s_refs;
      ok = true;
    } else {
      ESP_LOGW(TAG, "camera ja em uso a %dx%d - pedido de %dx%d recusado (uma resolucao por vez)",
               s_cam.alvo_width, s_cam.alvo_height, width, height);
      ok = false;
    }
  } else {
    ok = start_hardware(width, height);
    if (ok) { s_refs = 1; }
  }

  destravar_ctrl();
  return ok;
}

void tab5_camera_stop(void)
{
  lock_ctrl();

  if (s_refs > 0) {
    --s_refs;
    if (s_refs == 0) { stop_hardware(); }
  }

  destravar_ctrl();
}

void tab5_camera_set_target(const tab5_cam_target_t* d)
{
  if (!s_cam.trava) { return; }
  lock();
  s_cam.target = d ? *d : tab5_cam_target_t{ };
  s_cam.tem_novo = false;
  destravar();
}

bool tab5_camera_frame_written(void)
{
  if (!s_cam.on) { return false; }
  lock();
  const bool novo = s_cam.tem_novo;
  s_cam.tem_novo = false;
  destravar();
  return novo;
}

size_t tab5_camera_photo_bytes(void)
{
  size_t alinhamento = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alinhamento);
  return ((size_t)TAB5_CAM_PHOTO_W * TAB5_CAM_PHOTO_H * 2 + alinhamento - 1) & ~(alinhamento - 1);
}

uint16_t* tab5_camera_alloc_photo(void)
{
  size_t alinhamento = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alinhamento);
  return (uint16_t*)heap_caps_aligned_calloc(alinhamento, 1, tab5_camera_photo_bytes(), MALLOC_CAP_SPIRAM);
}

bool tab5_camera_request_photo(uint16_t* target)
{
  if (!s_cam.on || !target) { return false; }
  bool aceito = false;
  lock();
  if (!s_cam.foto_destino) {
    s_cam.foto_destino = target;
    s_cam.foto_pronta  = false;
    aceito = true;
  }
  destravar();
  return aceito;
}

bool tab5_camera_photo_ready(bool* ok)
{
  bool pronta = false;
  lock();
  if (s_cam.foto_pronta) {
    if (ok) { *ok = s_cam.foto_ok; }
    s_cam.foto_pronta = false;
    pronta = true;
  }
  destravar();
  return pronta;
}
