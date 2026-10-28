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

// O sensor do driver do esp_video. A funcao e global, mas o header que a
// declara e privado do componente (esp_video_device_internal.h).
extern "C" esp_cam_sensor_device_t* esp_video_get_csi_video_device_sensor(void);

#define TAB5_CAM_NBUF 2

// O SENSOR ESTA MONTADO DE LADO na placa - o mesmo BSP_CAMERA_ROTATION=270 do
// BSP oficial. Sem isto a imagem sairia deitada 90 graus.
#define TAB5_CAM_ROTACAO PPA_SRM_ROTATION_ANGLE_270

typedef struct {
  int      fd;
  uint8_t* bruto[TAB5_CAM_NBUF];   // buffers MMAP do driver (formato do pipeline, ver abaixo)
  size_t   bruto_tam;
  uint32_t bruto_largura, bruto_altura;

  ppa_client_handle_t ppa;
  SemaphoreHandle_t   ppa_fim;   // dado pela ISR do PPA ao end de cada operacao (ver ppa_rodar)

  int       alvo_largura, alvo_altura;   // size do preview
  // Preview com escala uniforme (ver o laco de captura): o pedaco central do
  // quadro bruto que vai para a tela e a escala, em passos de 1/16 (a
  // precisao do PPA). Calculados uma vez, quando o formato do sensor e value_read.
  int       recorte_w, recorte_h;
  float     escala;
  tab5_cam_target_t target;          // onde o preview e escrito (buf nulo = nenhum)
  bool      tem_novo;
  SemaphoreHandle_t trava;

  tarefa_app_t tarefa;
  bool on;
  bool video_iniciado;   // esp_video_init() teve sucesso - precisa de esp_video_deinit() no stop()

  // PEDIDO DE FOTO - ver tab5_camera_request_photo(). Protegido por 'trava'.
  uint16_t* foto_destino;      // != nullptr = pedido pending
  bool      foto_pronta;
  bool      foto_ok;
} tab5_camera_t;

static tab5_camera_t s_cam = { .fd = -1 };

static inline void travar(void)    { if (s_cam.trava) { xSemaphoreTake(s_cam.trava, portMAX_DELAY); } }
static inline void destravar(void) { if (s_cam.trava) { xSemaphoreGive(s_cam.trava); } }

// ---------------------------------------------------------------------------
// CONTAGEM DE REFERENCIA - ver o aviso grande em tab5_camera.hpp.
// ---------------------------------------------------------------------------
// TRAVA PROPRIA, separada de 's_cam.trava' (que so protege os CAMPOS DE
// FRAME - indice_pronto/tem_novo/pedido - lidos e escritos a cada volta do
// loop de captura). Esta aqui protege a TRANSICAO 0<->1 de s_refs.
//
// COMENTARIO ATUALIZADO - o motivo original ("o comando 'foto'
// do console chama estas funcoes da propria tarefa 'console'") NAO E MAIS
// VERDADE: aquilo causava colisao de I2C de verdade (ver o aviso grande em
// tab5_camera.hpp) e foi corrigido com a ponte kernel/core.cpp
// (sistema_camera_iniciar/stop) - HOJE, toda chamada a tab5_camera_start/
// stop() acontece NA tarefa de interface, direto (app Camera) ou servida
// pela ponte (console), nunca as duas ao mesmo time de tarefas diferentes.
// A trava ficou, de proposito, como network de seguranca BARATA contra alguem
// um dia chamar estas funcoes direto de uma terceira tarefa, violando o
// contrato - nao proteger custaria quase nada hoje, mas remover e assumir
// que ninguem nunca vai violar o contrato, e essa aposta ja perdeu uma vez
// neste mesmo file.
static SemaphoreHandle_t s_ctrl = nullptr;
static int               s_refs = 0;

static inline void travar_ctrl(void)    { tab5_mutex_travar(&s_ctrl); }
static inline void destravar_ctrl(void) { tab5_mutex_destravar(&s_ctrl); }

bool tab5_camera_active(void) { return s_refs > 0; }

// Idem - iniciar_hardware() chama no caminho de erro (goto erro), antes de
// parar_hardware() estar definida na leitura do file.
static void parar_hardware(void);

// O PPA EM MODO BLOQUEANTE NAO SOBREVIVE AO ENCERRAMENTO (lendo
// esp_driver_ppa/src/ppa_core.c). No mode BLOCKING o driver espera com
// xSemaphoreTake(trans_elm->sem, portMAX_DELAY) e IGNORA o retorno. Mas
// tarefa_app_encerrar() acorda a tarefa com xTaskAbortDelay() - e, se ela
// estiver nessa espera, o driver devolve "ESP_OK" com a operacao AINDA
// RODANDO no hardware. Dai em diante: QBUF de um buffer que o PPA ainda le,
// e o ppa_unregister_client() recusando (a transacao ainda conta) - era
// essa, e nao so a corrida da ISR, a source das tentativas esgotadas
// vistas nos testes.
//
// A output: mode NAO bloqueante, com um semaphore NOSSO dado pela ISR, e uma
// espera que, se for abortada, simplesmente espera de novo. A tarefa so sai
// do PPA quando o hardware de fato terminou.
static bool IRAM_ATTR ao_fim_ppa(ppa_client_handle_t, ppa_event_data_t*, void* sem)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &acordou);
  return acordou == pdTRUE;
}

static esp_err_t ppa_rodar(ppa_srm_oper_config_t* cfg)
{
  cfg->mode      = PPA_TRANS_MODE_NON_BLOCKING;
  cfg->user_data = s_cam.ppa_fim;
  const esp_err_t err = ppa_do_scale_rotate_mirror(s_cam.ppa, cfg);
  if (err != ESP_OK) { return err; }
  // Uma operacao leva poucos ms; 1 s so estoura se o PPA travou de vez.
  for (int i = 0; i < 20; ++i) {
    if (xSemaphoreTake(s_cam.ppa_fim, pdMS_TO_TICKS(50)) == pdTRUE) { return ESP_OK; }
  }
  return ESP_ERR_TIMEOUT;
}

// O PRETO DO SENSOR. O SC202CS entrega o preto num pedestal: o laco de nivel
// de preto dele mira o alvo dos registradores 0x3907/0x3908, que a tabela de
// inicializacao do driver deixa em 0x041 (65 em 10 bits, 16 no RAW8). A
// calibracao do componente conta com isso (bloco "blc" de 16 no JSON) - mas
// who subtrai e o bloco BLC do ISP, que so existe na v3 do P4
// (ESP_VIDEO_ISP_DEVICE_BLC, esp_video_caps.h). Abaixo da v3 o pedestal passa
// direto: vira um deslocamento fixo somado a TODO pixel, depois multiplicado
// pelo balanco de branco e pela CCM. Com os ganhos de luz do dia isso da
// 16 x (2,9 ; 0,4 ; 2,0) em linear - o preto sai roxo-escuro e a imagem
// inteira ganha um veu magenta, mais forte nas sombras (a gama amplia o
// fundo da escala). A output e pedir ao proprio sensor o preto no zero.
// Depois do STREAMON: o formato reescreve a tabela de inicializacao.
#define TAB5_CAM_PRETO_REG   0x3908
#define TAB5_CAM_PRETO_ALVO  0x00   // de fabrica 0x41

static void zerar_pedestal(void)
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

// A TAREFA DE CAPTURA. DQBUF bloqueia ate o sensor entregar o proximo frame
// (free_space-rodante, ~30fps) - por isso mora em tarefa_app_t, e nao na tarefa de
// interface: se o sensor travar, who fica preso e SO esta tarefa (ver o
// aviso grande em kernel/app_task.h sobre "abandonada de proposito" - o
// mesmo raciocinio que ja vale para Mesh/GPS vale aqui).
static void laco_captura(tarefa_app_t* t)
{
  const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int64_t t_conta = esp_timer_get_time();
  int frames = 0;

  while (tarefa_app_ativa(t)) {
    struct v4l2_buffer buf = { };
    buf.type   = type;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
      // xTaskAbortDelay NAO acorda um ioctl bloqueado - se caiu aqui por um
      // erro de verdade (nao por termos pedido para sair), evita busy-loop.
      if (tarefa_app_ativa(t)) { vTaskDelay(pdMS_TO_TICKS(50)); }
      continue;
    }

    // A base de toda operacao: o frame bruto do sensor (1600x1200), girado 270
    // (o sensor esta mounted de lado - BSP_CAMERA_ROTATION=270) e espelhado
    // (mirror_x e no bloco de SAIDA, pos-rotacao - esquerda/direita vinham
    // trocadas).
    ppa_srm_oper_config_t base = { };
    base.in.buffer   = s_cam.bruto[buf.index];
    base.in.pic_w    = s_cam.bruto_largura;
    base.in.pic_h    = s_cam.bruto_altura;
    base.in.block_w  = s_cam.bruto_largura;
    base.in.block_h  = s_cam.bruto_altura;
    base.in.srm_cm   = PPA_SRM_COLOR_MODE_RGB565;
    base.out.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    base.rotation_angle = TAB5_CAM_ROTACAO;
    base.mirror_x    = true;

    travar();
    const tab5_cam_target_t dst = s_cam.target;
    uint16_t* foto = s_cam.foto_destino;
    destravar();

    // PREVIEW DIRETO NO DESTINO: o PPA escreve o frame ja
    // girado e escalado no retangulo que o app indicou - na pratica, dentro
    // da TELA VIRTUAL do sistema. Antes eram quatro viagens pela PSRAM por
    // frame (PPA -> buffer da camera -> memcpy do app -> pushImage na tela
    // virtual -> compositor); somadas ao panel, a banda nao fechava a 30 qps
    // e a imagem chegava atrasada. Agora sao duas (PPA -> tela virtual ->
    // compositor).
    //
    // ESCALA UNIFORME, PREENCHENDO: a mesma escala nos dois eixos (a imagem
    // nao estica) e a menor que ainda cobre o retangulo inteiro; o que sobra
    // do quadro sai por igual dos dois lados. No retrato do Tab5 isso corta
    // uns 4% da width - o resto do quadro e o que a foto grava.
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
      cfg.in.block_offset_x  = (s_cam.bruto_largura - s_cam.recorte_w) / 2;
      cfg.in.block_offset_y  = (s_cam.bruto_altura  - s_cam.recorte_h) / 2;
      cfg.scale_x = cfg.scale_y = s_cam.escala;
      err = ppa_rodar(&cfg);
    }

    // FOTO PEDIDA: o MESMO frame bruto, inteiro e sem escala - 1600x1200
    // girado da exatamente 1200x1600, o campo de visao todo do sensor. Uma
    // operacao de PPA (poucos ms): o preview nao para, e a gravacao fica com
    // who pediu.
    if (foto) {
      ppa_srm_oper_config_t fc = base;
      fc.out.buffer      = foto;
      fc.out.buffer_size = tab5_camera_photo_bytes();
      fc.out.pic_w       = TAB5_CAM_PHOTO_W;
      fc.out.pic_h       = TAB5_CAM_PHOTO_H;
      fc.scale_x = (float)TAB5_CAM_PHOTO_H / (float)s_cam.bruto_largura;
      fc.scale_y = (float)TAB5_CAM_PHOTO_W / (float)s_cam.bruto_altura;
      const esp_err_t ef = ppa_rodar(&fc);
      travar();
      s_cam.foto_destino = nullptr;
      s_cam.foto_ok      = (ef == ESP_OK);
      s_cam.foto_pronta  = true;
      destravar();
      if (ef != ESP_OK) { ESP_LOGW(TAG, "foto: PPA failed (%s)", esp_err_to_name(ef)); }
    }

    // Devolve o buffer BRUTO ao driver JA - o sensor pode reusar o slot.
    ioctl(s_cam.fd, VIDIOC_QBUF, &buf);

    if (dst.buf && err == ESP_OK) {
      travar();
      s_cam.tem_novo = true;
      destravar();
      if (dst.avisar) { dst.avisar(); }
    }
    if (err != ESP_OK) { ESP_LOGW(TAG, "PPA failed: %s", esp_err_to_name(err)); }

    // Frames por segundo de verdade, a cada 10 s - o numero que diz se o
    // sensor (ou a exposicao automatica, que alonga o frame no escuro) esta
    // entregando os 30 qps.
    ++frames;
    const int64_t agora = esp_timer_get_time();
    if (agora - t_conta >= 10000000) {
      ESP_LOGI(TAG, "%.1f frames/s", frames * 1e6 / (double)(agora - t_conta));
      t_conta = agora;
      frames = 0;
    }
  }
}

// A INICIALIZACAO DE VERDADE - so roda na transicao 0->1 de s_refs (ver
// tab5_camera_start() mais abaixo, a funcao PUBLICA que decide quando
// chamar esta aqui).
static bool iniciar_hardware(int width, int height)
{
  if (width <= 0 || height <= 0) { return false; }

  s_cam = { .fd = -1 };
  s_cam.alvo_largura = width;
  s_cam.alvo_altura  = height;
  s_cam.trava = xSemaphoreCreateMutex();
  if (!s_cam.trava) { return false; }

  // A CAMERA SE ALIMENTA SOZINHA - ver o aviso grande no .hpp ("A CAMERA SE
  // ALIMENTA SOZINHA AGORA"). ANTES de qualquer coisa, inclusive antes do
  // handle I2C: o SCCB do sensor (usado logo abaixo, dentro de
  // esp_video_init) so responde com CAM_RST fora de reset.
  tab5_power_camera(true);

  // FOLGA PARA O SENSOR ACORDAR - achado em hardware testando
  // 'foto' pelo CONSOLE: "Get sensor ID failed" no primeiro SCCB, sempre.
  //
  // O driver do SC202CS (sc202cs.c, sc202cs_hw_reset) TEM um delay de 10ms
  // apos release o reset - mas SO quando 'reset_pin' e um GPIO que o proprio
  // driver controla. O ours CAM_RST e por fora (expansor I2C, ver
  // tab5_power_camera()), entao passamos GPIO_NUM_NC para reset_pin/pwdn_pin
  // (ver csi_cfg abaixo) - o driver nunca sabe que acabamos de tirar o
  // sensor do reset, e pula o proprio delay dele. NOS temos que dar essa
  // folga, e antes disto ela sempre "acontecia" por acidente: o app Camera
  // ainda montava o cartao (usa_sd=true) e desenhava "abrindo..." antes de
  // ao_abrir() chegar aqui, dezenas de ms de sobra sem ninguem ter pedido.
  // O comando 'foto' chama esta funcao direto, sem nenhum desses passos no
  // storage - e foi so ai que a falta de delay EXPLICITO ficou visivel. 20ms:
  // o dobro do que o proprio driver usa para o caso que ele conhece, folga
  // confortavel sem atrasar percebivelmente a abertura do app Camera.
  vTaskDelay(pdMS_TO_TICKS(20));

  // O BARRAMENTO E O MESMO DO RESTO DO SISTEMA (M5.In_I2C) - ver o aviso
  // grande no .hpp. Pega o handle ja instalado, NAO cria um novo.
  i2c_master_bus_handle_t i2c_handle = nullptr;
  if (i2c_master_get_bus_handle(I2C_NUM_1, &i2c_handle) != ESP_OK || !i2c_handle) {
    ESP_LOGE(TAG, "barramento I2C interno (I2C_NUM_1) nao esta de pe - M5.begin() ja rodou?");
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
    // Isto e o sensor de verdade falando: uma camera CMOS como esta NUNCA
    // entrega RGB direto, so o padrao de Bayer. Quem faz o demosaico e o ISP
    // do proprio chip (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER, ja
    // on) - mas so quando a gente PEDE RGB565 via VIDIOC_S_FMT neste MESMO
    // /dev/video0. Sem este step, o PPA logo abaixo receberia Bayer raw e
    // tentaria le-lo como RGB565 - a imagem sairia um chuvisco sem sentido
    // nenhum, nao uma foto "esquisita": bytes crus do sensor, color nenhuma.
    struct v4l2_format fmt = { };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
      ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
      goto erro;
    }
    ESP_LOGI(TAG, "formato padrao do sensor: %ux%u, fourcc 0x%08lx (RAW = Bayer, sem ISP ainda)",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned long)fmt.fmt.pix.pixelformat);

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
      struct v4l2_format pedido = { };
      pedido.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      pedido.fmt.pix.width       = fmt.fmt.pix.width;
      pedido.fmt.pix.height      = fmt.fmt.pix.height;
      pedido.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
      if (ioctl(s_cam.fd, VIDIOC_S_FMT, &pedido) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(RGB565) failed - o ISP nao aceitou a conversao");
        goto erro;
      }
      // Confirma o que realmente ficou valendo - o driver pode ajustar.
      memset(&fmt, 0, sizeof(fmt));
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT (apos S_FMT) failed");
        goto erro;
      }
    }

    s_cam.bruto_largura = fmt.fmt.pix.width;
    s_cam.bruto_altura  = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "capturando: %ux%u, fourcc 0x%08lx", (unsigned)s_cam.bruto_largura,
             (unsigned)s_cam.bruto_altura, (unsigned long)fmt.fmt.pix.pixelformat);
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
      ESP_LOGE(TAG, "driver ficou em fourcc 0x%08lx, nao RGB565 - PPA vai receber lixo",
               (unsigned long)fmt.fmt.pix.pixelformat);
      goto erro;
    }

    // ESCALA DO PREVIEW. Girado 270, a width da tela vem da ALTURA do
    // quadro bruto e a height da tela, da LARGURA. A escala e a menor, em
    // 1/16, que cobre os dois eixos; o recorte e o que cabe com ela.
    {
      const int L = s_cam.alvo_largura, A = s_cam.alvo_altura;
      const int W = (int)s_cam.bruto_largura, H = (int)s_cam.bruto_altura;
      int s16 = (L * 16 + H - 1) / H;
      const int s16a = (A * 16 + W - 1) / W;
      if (s16a > s16) { s16 = s16a; }
      if (s16 < 1) { s16 = 1; }
      s_cam.escala    = (float)s16 / 16.0f;
      s_cam.recorte_h = (L * 16) / s16; if (s_cam.recorte_h > H) { s_cam.recorte_h = H; }
      s_cam.recorte_w = (A * 16) / s16; if (s_cam.recorte_w > W) { s_cam.recorte_w = W; }
      ESP_LOGI(TAG, "preview %dx%d: escala %d/16, usa %dx%d do quadro de %dx%d",
               L, A, s16, s_cam.recorte_w, s_cam.recorte_h, W, H);
    }

    // EXPOSICAO E COR SAO DO ISP. O ganho manual fixo que havia
    // aqui (indice 16 = 1,5x) era remendo para a imagem escura
    // a causa era o controlador do pipeline do ISP DESLIGADO no sdkconfig
    // (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER): sem ele nada rodava
    // exposicao automatica (AGC/AE) nem balanco de branco (AWB) - o sensor
    // entregava Bayer "raw", verde (dois pixels verdes em cada quatro, sem
    // correcao) e no ganho minimo. Ligado, a tarefa "isp_task" do esp_video
    // le as estatisticas do ISP a cada frame e ajusta ganho/exposicao do
    // sensor (pelo SCCB, que passa pela trava do I2C - hal/tab5_sccb.cpp) e
    // os ganhos de color/CCM/gama do ISP, com a calibracao do SC202CS que vem
    // no componente. Um ganho fixo aqui so brigaria com ela.
    // A calibracao em uso e a PROPRIA, hal/sc202cs_tab5.json (exposicao
    // mais low contra o branco estourado) - o porque esta no
    // sdkconfig.defaults, secao "Camera". O branco magenta em si e resolvido
    // na matriz de color, por um IPA proprio: hal/tab5_camera_ipa.c.

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
      s_cam.bruto_tam = buf.length;
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
    s_cam.ppa_fim = xSemaphoreCreateBinary();
    const ppa_event_callbacks_t cbs = { .on_trans_done = ao_fim_ppa };
    if (!s_cam.ppa_fim || ppa_client_register_event_callbacks(s_cam.ppa, &cbs) != ESP_OK) {
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
  zerar_pedestal();

  // So DQBUF, PPA e QBUF - a gravacao da foto saiu daqui (ver
  // tab5_camera_request_photo), entao a stack volta aos 4 KB originais.
  if (!tarefa_app_criar(&s_cam.tarefa, laco_captura, "cam", 4096, 3)) {
    ESP_LOGE(TAG, "nao consegui start a tarefa de captura");
    { const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type); }
    goto erro;
  }

  s_cam.on = true;
  tab5_mark("camera: ligada", 0);
  ESP_LOGI(TAG, "camera no ar (%dx%d, rotacao %d)", width, height, (int)TAB5_CAM_ROTACAO);
  return true;

erro:
  parar_hardware();
  return false;
}

// O DESLIGAMENTO DE VERDADE - so roda na transicao 1->0 de s_refs (ver
// tab5_camera_stop() mais abaixo). Tambem chamada direto por
// iniciar_hardware() no caminho de erro (goto erro acima) - por isso
// continua segura de chamar mesmo com a inicializacao pela half, exatamente
// como o comentario original desta funcao ja avisava.
static void parar_hardware(void)
{
  tab5_mark("camera: desligando", 0);
  // QUANTO CUSTA DESLIGAR - roda na tarefa de interface (ao_fechar), entao
  // cada ms aqui e tela parada. O log diz qual etapa pesou.
  const int64_t t0 = esp_timer_get_time();
  int64_t t_tarefa = t0, t_ppa = t0, t_video = t0;

  if (s_cam.on) {
    // A tarefa so faz DQBUF (bloqueia ate o proximo frame, ~33 ms) e PPA -
    // nenhuma escrita no cartao. 500 ms e folga de sobra; se ela nao sair,
    // tarefa_app_encerrar() a abandona (ver app_task.h).
    tarefa_app_encerrar(&s_cam.tarefa, 500);
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
  }
  s_cam.on = false;
  t_tarefa = esp_timer_get_time();

  // "client still has unprocessed trans" - CAUSA RAIZ CONFIRMADA lendo o
  // fonte do driver (esp_driver_ppa/src/ppa_core.c), nao so suposta:
  //
  //   ppa_transaction_done_cb() (a ISR de end de transacao) faz, na ORDEM,
  //   dentro do MESMO critico:
  //     1. xSemaphoreGiveFromISR(trans_elm->sem, ...)   <- acorda who
  //        chamou ppa_do_scale_rotate_mirror (nos, em mode bloqueante)
  //     2. client->trans_cnt--                          <- SO DEPOIS
  //
  //   ppa_unregister_client() recusa se trans_cnt != 0. Ou seja: existe uma
  //   janela real, de poucas instrucoes, entre "nossa chamada bloqueante
  //   devolve o controle" e "o proprio driver termina de contar aquela
  //   transacao como concluida". Em SMP (P4 e dual-core, e a tarefa "cam"
  //   nao tem afinidade fixa - tarefa_app_criar usa tskNO_AFFINITY) essa
  //   janela pode realmente ser cruzada.
  //
  // NAO E OPCIONAL EVITAR - e uma corrida documentada no proprio driver, nao
  // um bug ours para "consertar direito"; tentativas curtas espacadas SAO o
  // conserto correto. O que dava para melhorar era o ruido: um vTaskDelay(1)
  // ANTES da primeira tentativa (a folga que a janela realmente precisa, so
  // que dada de proposito em vez de descoberta por tentativa e erro) faz a
  // 1a tentativa passar na quase totalidade das vezes; e como a mensagem de
  // erro sai de DENTRO do driver (ESP_LOGE, nao um log ours), a unica forma
  // de nao assustar no log com uma condicao ja tratada e baixar o nivel
  // daquela TAG so durante a tentativa e return_card depois.
  //
  // ORCAMENTO AMPLIADO - seen em hardware: as 5 tentativas
  // originais (100ms de folga) as vezes NAO bastavam ("nao confirmou" no
  // log, mesmo com o sistema seguindo vivo depois). NAO E SO COSMETICO: fui
  // read ppa_register_client()/ppa_unregister_client() em ppa_core.c, e
  // quando 'do_unregister' da falso a funcao devolve ANTES do free() do
  // client (heap_caps_calloc'd) e antes de release a referencia no motor SRM
  // shared - ou seja, esgotar as tentativas VAZA aquela estrutura de
  // verdade, nao so imprime um aviso feio. 20 tentativas de 25ms (max. storage
  // segundo, e SO quando a corrida realmente acontece) e um preco invisivel
  // no fechamento da camera para reduzir bastante a chance de esgotar - o
  // residual que ainda pode vazar se ate isso nao bastar fica registrado
  // aqui, nao escondido: se este aviso voltar a aparecer, o proximo step e
  // investigar o que estaria atrasando a ISR do PPA por MEIO SEGUNDO
  // inteiro, nao alargar o numero de novo.
  // (a outra source, maior, era o aborto da espera bloqueante
  // resolvida em ppa_rodar(). As tentativas ficam para a janela da ISR.)
  if (s_cam.ppa) {
    vTaskDelay(pdMS_TO_TICKS(1));   // a folga que a janela documentada acima precisa

    esp_log_level_set("ppa_core", ESP_LOG_NONE);
    esp_err_t err = ESP_FAIL;
    for (int tentativa = 0; tentativa < 20; ++tentativa) {
      err = ppa_unregister_client(s_cam.ppa);
      if (err == ESP_OK) { break; }
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    esp_log_level_set("ppa_core", ESP_LOG_INFO);

    if (err != ESP_OK) { ESP_LOGW(TAG, "ppa_unregister_client nao confirmou (%s) - seguindo assim mesmo (cliente PPA vazado)", esp_err_to_name(err)); }
    s_cam.ppa = nullptr;
  }
  if (s_cam.ppa_fim) { vSemaphoreDelete(s_cam.ppa_fim); s_cam.ppa_fim = nullptr; }
  t_ppa = esp_timer_get_time();

  for (int i = 0; i < TAB5_CAM_NBUF; ++i) {
    if (s_cam.bruto[i]) { munmap(s_cam.bruto[i], s_cam.bruto_tam); s_cam.bruto[i] = nullptr; }
  }
  if (s_cam.fd >= 0) { close(s_cam.fd); s_cam.fd = -1; }

  // O PASSO QUE FALTAVA (defeito observado): esp_video_init() registra
  // dispositivos GLOBAIS (/dev/video0, e /dev/video20 "ISP" com o pipeline
  // on) que sobrevivem ao fd fechado - sem desfazer isso, a PROXIMA
  // chamada a esp_video_init() (no proximo ao_abrir()) encontra "/dev/video20
  // ja registrado" e failure, e o app fica preso recusando abrir ate reiniciar
  // o aparelho. esp_video_deinit() e o par exato de esp_video_init().
  if (s_cam.video_iniciado) {
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) { ESP_LOGW(TAG, "esp_video_deinit failed (%s)", esp_err_to_name(err)); }
    s_cam.video_iniciado = false;
  }
  t_video = esp_timer_get_time();

  if (s_cam.trava) { vSemaphoreDelete(s_cam.trava); s_cam.trava = nullptr; }
  s_cam.target = { };
  s_cam.tem_novo = false;

  // PAR EXATO do tab5_power_camera(true) no comeco de iniciar_hardware() -
  // ver o aviso grande no .hpp. POR ULTIMO: todo o resto ja foi desmontado
  // acima, entao nao ha risco de deixar o sensor sem energia com alguma
  // outra parte do desligamento ainda tentando falar SCCB com ele.
  tab5_power_camera(false);

  const int64_t t_fim = esp_timer_get_time();
  ESP_LOGI(TAG, "desligada em %lld ms (tarefa %lld, ppa %lld, esp_video %lld, energia %lld)",
           (t_fim - t0) / 1000, (t_tarefa - t0) / 1000, (t_ppa - t_tarefa) / 1000,
           (t_video - t_ppa) / 1000, (t_fim - t_video) / 1000);
}

// ---------------------------------------------------------------------------
// API PUBLICA - so aqui mora a CONTAGEM DE REFERENCIA (ver o aviso grande no
// .hpp). iniciar_hardware()/parar_hardware() acima nunca sao chamadas fora
// destas duas funcoes.
// ---------------------------------------------------------------------------
bool tab5_camera_start(int width, int height)
{
  travar_ctrl();

  bool ok;
  if (s_refs > 0) {
    // JA LIGADA - so aceita mais um interessado se pedir a MESMA resolucao
    // (ver "UMA UNICA RESOLUCAO POR VEZ" no .hpp). Comparar com s_cam
    // diretamente e seguro aqui dentro da trava: nada mais escreve
    // alvo_largura/alvo_altura fora de iniciar_hardware().
    if (width == s_cam.alvo_largura && height == s_cam.alvo_altura) {
      ++s_refs;
      ok = true;
    } else {
      ESP_LOGW(TAG, "camera ja em uso a %dx%d - pedido de %dx%d recusado (uma resolucao por vez)",
               s_cam.alvo_largura, s_cam.alvo_altura, width, height);
      ok = false;
    }
  } else {
    ok = iniciar_hardware(width, height);
    if (ok) { s_refs = 1; }
  }

  destravar_ctrl();
  return ok;
}

void tab5_camera_stop(void)
{
  travar_ctrl();

  if (s_refs > 0) {
    --s_refs;
    if (s_refs == 0) { parar_hardware(); }
  }

  destravar_ctrl();
}

void tab5_camera_set_target(const tab5_cam_target_t* d)
{
  if (!s_cam.trava) { return; }
  travar();
  s_cam.target = d ? *d : tab5_cam_target_t{ };
  s_cam.tem_novo = false;
  destravar();
}

bool tab5_camera_frame_written(void)
{
  if (!s_cam.on) { return false; }
  travar();
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
  travar();
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
  travar();
  if (s_cam.foto_pronta) {
    if (ok) { *ok = s_cam.foto_ok; }
    s_cam.foto_pronta = false;
    pronta = true;
  }
  destravar();
  return pronta;
}
