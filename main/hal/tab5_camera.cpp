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
  SemaphoreHandle_t   ppa_fim;   // dado pela ISR do PPA ao fim de cada operacao (ver ppa_rodar)

  int       alvo_largura, alvo_altura;   // tamanho do preview
  // Preview com escala uniforme (ver o laco de captura): o pedaco central do
  // quadro bruto que vai para a tela e a escala, em passos de 1/16 (a
  // precisao do PPA). Calculados uma vez, quando o formato do sensor e lido.
  int       recorte_w, recorte_h;
  float     escala;
  tab5_cam_destino_t destino;          // onde o preview e escrito (buf nulo = nenhum)
  bool      tem_novo;
  SemaphoreHandle_t trava;

  tarefa_app_t tarefa;
  bool ligado;
  bool video_iniciado;   // esp_video_init() teve sucesso - precisa de esp_video_deinit() no parar()

  // PEDIDO DE FOTO - ver tab5_camera_pedir_foto(). Protegido por 'trava'.
  uint16_t* foto_destino;      // != nullptr = pedido pendente
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
// (sistema_camera_iniciar/parar) - HOJE, toda chamada a tab5_camera_iniciar/
// parar() acontece NA tarefa de interface, direto (app Camera) ou servida
// pela ponte (console), nunca as duas ao mesmo tempo de tarefas diferentes.
// A trava ficou, de proposito, como rede de seguranca BARATA contra alguem
// um dia chamar estas funcoes direto de uma terceira tarefa, violando o
// contrato - nao proteger custaria quase nada hoje, mas remover e assumir
// que ninguem nunca vai violar o contrato, e essa aposta ja perdeu uma vez
// neste mesmo arquivo.
static SemaphoreHandle_t s_ctrl = nullptr;
static int               s_refs = 0;

static inline void travar_ctrl(void)    { tab5_mutex_travar(&s_ctrl); }
static inline void destravar_ctrl(void) { tab5_mutex_destravar(&s_ctrl); }

bool tab5_camera_ativa(void) { return s_refs > 0; }

// Idem - iniciar_hardware() chama no caminho de erro (goto erro), antes de
// parar_hardware() estar definida na leitura do arquivo.
static void parar_hardware(void);

// O PPA EM MODO BLOQUEANTE NAO SOBREVIVE AO ENCERRAMENTO (lendo
// esp_driver_ppa/src/ppa_core.c). No modo BLOCKING o driver espera com
// xSemaphoreTake(trans_elm->sem, portMAX_DELAY) e IGNORA o retorno. Mas
// tarefa_app_encerrar() acorda a tarefa com xTaskAbortDelay() - e, se ela
// estiver nessa espera, o driver devolve "ESP_OK" com a operacao AINDA
// RODANDO no hardware. Dai em diante: QBUF de um buffer que o PPA ainda le,
// e o ppa_unregister_client() recusando (a transacao ainda conta) - era
// essa, e nao so a corrida da ISR, a origem das tentativas esgotadas
// vistas nos testes.
//
// A saida: modo NAO bloqueante, com um semaphore NOSSO dado pela ISR, e uma
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
// quem subtrai e o bloco BLC do ISP, que so existe na v3 do P4
// (ESP_VIDEO_ISP_DEVICE_BLC, esp_video_caps.h). Abaixo da v3 o pedestal passa
// direto: vira um deslocamento fixo somado a TODO pixel, depois multiplicado
// pelo balanco de branco e pela CCM. Com os ganhos de luz do dia isso da
// 16 x (2,9 ; 0,4 ; 2,0) em linear - o preto sai roxo-escuro e a imagem
// inteira ganha um veu magenta, mais forte nas sombras (a gama amplia o
// fundo da escala). A saida e pedir ao proprio sensor o preto no zero.
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

bool tab5_camera_reg(uint16_t reg, int valor, uint8_t* lido)
{
  if (!s_cam.ligado) { return false; }
  esp_cam_sensor_device_t* sensor = esp_video_get_csi_video_device_sensor();
  if (!sensor) { return false; }
  esp_cam_sensor_reg_val_t r = { .regaddr = reg, .value = 0 };
  if (valor >= 0) {
    r.value = (uint32_t)valor;
    if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r) != ESP_OK) { return false; }
  }
  if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_G_REG, &r) != ESP_OK) { return false; }
  if (lido) { *lido = (uint8_t)r.value; }
  return true;
}

// A TAREFA DE CAPTURA. DQBUF bloqueia ate o sensor entregar o proximo frame
// (livre-rodante, ~30fps) - por isso mora em tarefa_app_t, e nao na tarefa de
// interface: se o sensor travar, quem fica preso e SO esta tarefa (ver o
// aviso grande em kernel/app_task.h sobre "abandonada de proposito" - o
// mesmo raciocinio que ja vale para Mesh/GPS vale aqui).
static void laco_captura(tarefa_app_t* t)
{
  const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int64_t t_conta = esp_timer_get_time();
  int quadros = 0;

  while (tarefa_app_ativa(t)) {
    struct v4l2_buffer buf = { };
    buf.type   = tipo;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
      // xTaskAbortDelay NAO acorda um ioctl bloqueado - se caiu aqui por um
      // erro de verdade (nao por termos pedido para sair), evita busy-loop.
      if (tarefa_app_ativa(t)) { vTaskDelay(pdMS_TO_TICKS(50)); }
      continue;
    }

    // A base de toda operacao: o frame bruto do sensor (1600x1200), girado 270
    // (o sensor esta montado de lado - BSP_CAMERA_ROTATION=270) e espelhado
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
    const tab5_cam_destino_t dst = s_cam.destino;
    uint16_t* foto = s_cam.foto_destino;
    destravar();

    // PREVIEW DIRETO NO DESTINO: o PPA escreve o frame ja
    // girado e escalado no retangulo que o app indicou - na pratica, dentro
    // da TELA VIRTUAL do sistema. Antes eram quatro viagens pela PSRAM por
    // frame (PPA -> buffer da camera -> memcpy do app -> pushImage na tela
    // virtual -> compositor); somadas ao painel, a banda nao fechava a 30 qps
    // e a imagem chegava atrasada. Agora sao duas (PPA -> tela virtual ->
    // compositor).
    //
    // ESCALA UNIFORME, PREENCHENDO: a mesma escala nos dois eixos (a imagem
    // nao estica) e a menor que ainda cobre o retangulo inteiro; o que sobra
    // do quadro sai por igual dos dois lados. No retrato do Tab5 isso corta
    // uns 4% da largura - o resto do quadro e o que a foto grava.
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
    // quem pediu.
    if (foto) {
      ppa_srm_oper_config_t fc = base;
      fc.out.buffer      = foto;
      fc.out.buffer_size = tab5_camera_foto_bytes();
      fc.out.pic_w       = TAB5_CAM_FOTO_L;
      fc.out.pic_h       = TAB5_CAM_FOTO_A;
      fc.scale_x = (float)TAB5_CAM_FOTO_A / (float)s_cam.bruto_largura;
      fc.scale_y = (float)TAB5_CAM_FOTO_L / (float)s_cam.bruto_altura;
      const esp_err_t ef = ppa_rodar(&fc);
      travar();
      s_cam.foto_destino = nullptr;
      s_cam.foto_ok      = (ef == ESP_OK);
      s_cam.foto_pronta  = true;
      destravar();
      if (ef != ESP_OK) { ESP_LOGW(TAG, "foto: PPA falhou (%s)", esp_err_to_name(ef)); }
    }

    // Devolve o buffer BRUTO ao driver JA - o sensor pode reusar o slot.
    ioctl(s_cam.fd, VIDIOC_QBUF, &buf);

    if (dst.buf && err == ESP_OK) {
      travar();
      s_cam.tem_novo = true;
      destravar();
      if (dst.avisar) { dst.avisar(); }
    }
    if (err != ESP_OK) { ESP_LOGW(TAG, "PPA falhou: %s", esp_err_to_name(err)); }

    // Frames por segundo de verdade, a cada 10 s - o numero que diz se o
    // sensor (ou a exposicao automatica, que alonga o frame no escuro) esta
    // entregando os 30 qps.
    ++quadros;
    const int64_t agora = esp_timer_get_time();
    if (agora - t_conta >= 10000000) {
      ESP_LOGI(TAG, "%.1f quadros/s", quadros * 1e6 / (double)(agora - t_conta));
      t_conta = agora;
      quadros = 0;
    }
  }
}

// A INICIALIZACAO DE VERDADE - so roda na transicao 0->1 de s_refs (ver
// tab5_camera_iniciar() mais abaixo, a funcao PUBLICA que decide quando
// chamar esta aqui).
static bool iniciar_hardware(int largura, int altura)
{
  if (largura <= 0 || altura <= 0) { return false; }

  s_cam = { .fd = -1 };
  s_cam.alvo_largura = largura;
  s_cam.alvo_altura  = altura;
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
  // apos soltar o reset - mas SO quando 'reset_pin' e um GPIO que o proprio
  // driver controla. O nosso CAM_RST e por fora (expansor I2C, ver
  // tab5_power_camera()), entao passamos GPIO_NUM_NC para reset_pin/pwdn_pin
  // (ver csi_cfg abaixo) - o driver nunca sabe que acabamos de tirar o
  // sensor do reset, e pula o proprio delay dele. NOS temos que dar essa
  // folga, e antes disto ela sempre "acontecia" por acidente: o app Camera
  // ainda montava o cartao (usa_sd=true) e desenhava "abrindo..." antes de
  // ao_abrir() chegar aqui, dezenas de ms de sobra sem ninguem ter pedido.
  // O comando 'foto' chama esta funcao direto, sem nenhum desses passos no
  // meio - e foi so ai que a falta de delay EXPLICITO ficou visivel. 20ms:
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
    .reset_pin = GPIO_NUM_NC,   // CAM_RST e o expansor I2C, ja aplicado via perfil.camera - ver .hpp
    .pwdn_pin  = GPIO_NUM_NC,
  };
  esp_video_init_config_t video_cfg = { .csi = &csi_cfg };

  if (esp_video_init(&video_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init falhou - confira CONFIG_CAMERA_SC202CS no menuconfig");
    vSemaphoreDelete(s_cam.trava);
    s_cam.trava = nullptr;
    return false;
  }
  s_cam.video_iniciado = true;

  s_cam.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (s_cam.fd < 0) {
    ESP_LOGE(TAG, "abrir %s falhou", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    goto erro;
  }

  {
    // O FORMATO PADRAO DO SENSOR E RAW8 (Bayer cru) - conferido no sdkconfig
    // apos baixar o componente (CONFIG_CAMERA_SC202CS_MIPI_DEFAULT_FMT_RAW8_*).
    // Isto e o sensor de verdade falando: uma camera CMOS como esta NUNCA
    // entrega RGB direto, so o padrao de Bayer. Quem faz o demosaico e o ISP
    // do proprio chip (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER, ja
    // ligado) - mas so quando a gente PEDE RGB565 via VIDIOC_S_FMT neste MESMO
    // /dev/video0. Sem este passo, o PPA logo abaixo receberia Bayer cru e
    // tentaria le-lo como RGB565 - a imagem sairia um chuvisco sem sentido
    // nenhum, nao uma foto "esquisita": bytes crus do sensor, cor nenhuma.
    struct v4l2_format fmt = { };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
      ESP_LOGE(TAG, "VIDIOC_G_FMT falhou");
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
        ESP_LOGE(TAG, "VIDIOC_S_FMT(RGB565) falhou - o ISP nao aceitou a conversao");
        goto erro;
      }
      // Confirma o que realmente ficou valendo - o driver pode ajustar.
      memset(&fmt, 0, sizeof(fmt));
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT (apos S_FMT) falhou");
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

    // ESCALA DO PREVIEW. Girado 270, a largura da tela vem da ALTURA do
    // quadro bruto e a altura da tela, da LARGURA. A escala e a menor, em
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
    // entregava Bayer "cru", verde (dois pixels verdes em cada quatro, sem
    // correcao) e no ganho minimo. Ligado, a tarefa "isp_task" do esp_video
    // le as estatisticas do ISP a cada frame e ajusta ganho/exposicao do
    // sensor (pelo SCCB, que passa pela trava do I2C - hal/tab5_sccb.cpp) e
    // os ganhos de cor/CCM/gama do ISP, com a calibracao do SC202CS que vem
    // no componente. Um ganho fixo aqui so brigaria com ela.
    // A calibracao em uso e a PROPRIA, hal/sc202cs_tab5.json (exposicao
    // mais baixa contra o branco estourado) - o porque esta no
    // sdkconfig.defaults, secao "Camera". O branco magenta em si e resolvido
    // na matriz de cor, por um IPA proprio: hal/tab5_camera_ipa.c.

    struct v4l2_requestbuffers req = { };
    req.count  = TAB5_CAM_NBUF;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
      ESP_LOGE(TAG, "VIDIOC_REQBUFS falhou");
      goto erro;
    }

    for (int i = 0; i < TAB5_CAM_NBUF; ++i) {
      struct v4l2_buffer buf = { };
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%d) falhou", i);
        goto erro;
      }
      s_cam.bruto[i] = (uint8_t*)mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, s_cam.fd, buf.m.offset);
      if (!s_cam.bruto[i]) { ESP_LOGE(TAG, "mmap(%d) falhou", i); goto erro; }
      s_cam.bruto_tam = buf.length;
      if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QBUF(%d) inicial falhou", i);
        goto erro;
      }
    }
  }

  {
    const ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    if (ppa_register_client(&ppa_cfg, &s_cam.ppa) != ESP_OK) {
      ESP_LOGE(TAG, "ppa_register_client falhou");
      goto erro;
    }
    s_cam.ppa_fim = xSemaphoreCreateBinary();
    const ppa_event_callbacks_t cbs = { .on_trans_done = ao_fim_ppa };
    if (!s_cam.ppa_fim || ppa_client_register_event_callbacks(s_cam.ppa, &cbs) != ESP_OK) {
      ESP_LOGE(TAG, "callback do PPA falhou");
      goto erro;
    }
  }

  {
    const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &tipo) != 0) {
      ESP_LOGE(TAG, "VIDIOC_STREAMON falhou");
      goto erro;
    }
  }
  zerar_pedestal();

  // So DQBUF, PPA e QBUF - a gravacao da foto saiu daqui (ver
  // tab5_camera_pedir_foto), entao a stack volta aos 4 KB originais.
  if (!tarefa_app_criar(&s_cam.tarefa, laco_captura, "cam", 4096, 3)) {
    ESP_LOGE(TAG, "nao consegui subir a tarefa de captura");
    { const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(s_cam.fd, VIDIOC_STREAMOFF, &tipo); }
    goto erro;
  }

  s_cam.ligado = true;
  tab5_marco("camera: ligada", 0);
  ESP_LOGI(TAG, "camera no ar (%dx%d, rotacao %d)", largura, altura, (int)TAB5_CAM_ROTACAO);
  return true;

erro:
  parar_hardware();
  return false;
}

// O DESLIGAMENTO DE VERDADE - so roda na transicao 1->0 de s_refs (ver
// tab5_camera_parar() mais abaixo). Tambem chamada direto por
// iniciar_hardware() no caminho de erro (goto erro acima) - por isso
// continua segura de chamar mesmo com a inicializacao pela metade, exatamente
// como o comentario original desta funcao ja avisava.
static void parar_hardware(void)
{
  tab5_marco("camera: desligando", 0);
  // QUANTO CUSTA DESLIGAR - roda na tarefa de interface (ao_fechar), entao
  // cada ms aqui e tela parada. O log diz qual etapa pesou.
  const int64_t t0 = esp_timer_get_time();
  int64_t t_tarefa = t0, t_ppa = t0, t_video = t0;

  if (s_cam.ligado) {
    // A tarefa so faz DQBUF (bloqueia ate o proximo frame, ~33 ms) e PPA -
    // nenhuma escrita no cartao. 500 ms e folga de sobra; se ela nao sair,
    // tarefa_app_encerrar() a abandona (ver app_task.h).
    tarefa_app_encerrar(&s_cam.tarefa, 500);
    const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &tipo);
  }
  s_cam.ligado = false;
  t_tarefa = esp_timer_get_time();

  // "client still has unprocessed trans" - CAUSA RAIZ CONFIRMADA lendo o
  // fonte do driver (esp_driver_ppa/src/ppa_core.c), nao so suposta:
  //
  //   ppa_transaction_done_cb() (a ISR de fim de transacao) faz, na ORDEM,
  //   dentro do MESMO critico:
  //     1. xSemaphoreGiveFromISR(trans_elm->sem, ...)   <- acorda quem
  //        chamou ppa_do_scale_rotate_mirror (nos, em modo bloqueante)
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
  // um bug nosso para "consertar direito"; tentativas curtas espacadas SAO o
  // conserto correto. O que dava para melhorar era o ruido: um vTaskDelay(1)
  // ANTES da primeira tentativa (a folga que a janela realmente precisa, so
  // que dada de proposito em vez de descoberta por tentativa e erro) faz a
  // 1a tentativa passar na quase totalidade das vezes; e como a mensagem de
  // erro sai de DENTRO do driver (ESP_LOGE, nao um log nosso), a unica forma
  // de nao assustar no log com uma condicao ja tratada e baixar o nivel
  // daquela TAG so durante a tentativa e devolver depois.
  //
  // ORCAMENTO AMPLIADO - visto em hardware: as 5 tentativas
  // originais (100ms de folga) as vezes NAO bastavam ("nao confirmou" no
  // log, mesmo com o sistema seguindo vivo depois). NAO E SO COSMETICO: fui
  // ler ppa_register_client()/ppa_unregister_client() em ppa_core.c, e
  // quando 'do_unregister' da falso a funcao devolve ANTES do free() do
  // client (heap_caps_calloc'd) e antes de soltar a referencia no motor SRM
  // shared - ou seja, esgotar as tentativas VAZA aquela estrutura de
  // verdade, nao so imprime um aviso feio. 20 tentativas de 25ms (max. meio
  // segundo, e SO quando a corrida realmente acontece) e um preco invisivel
  // no fechamento da camera para reduzir bastante a chance de esgotar - o
  // residual que ainda pode vazar se ate isso nao bastar fica registrado
  // aqui, nao escondido: se este aviso voltar a aparecer, o proximo passo e
  // investigar o que estaria atrasando a ISR do PPA por MEIO SEGUNDO
  // inteiro, nao alargar o numero de novo.
  // (a outra origem, maior, era o aborto da espera bloqueante
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
  // ligado) que sobrevivem ao fd fechado - sem desfazer isso, a PROXIMA
  // chamada a esp_video_init() (no proximo ao_abrir()) encontra "/dev/video20
  // ja registrado" e falha, e o app fica preso recusando abrir ate reiniciar
  // o aparelho. esp_video_deinit() e o par exato de esp_video_init().
  if (s_cam.video_iniciado) {
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) { ESP_LOGW(TAG, "esp_video_deinit falhou (%s)", esp_err_to_name(err)); }
    s_cam.video_iniciado = false;
  }
  t_video = esp_timer_get_time();

  if (s_cam.trava) { vSemaphoreDelete(s_cam.trava); s_cam.trava = nullptr; }
  s_cam.destino = { };
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
bool tab5_camera_iniciar(int largura, int altura)
{
  travar_ctrl();

  bool ok;
  if (s_refs > 0) {
    // JA LIGADA - so aceita mais um interessado se pedir a MESMA resolucao
    // (ver "UMA UNICA RESOLUCAO POR VEZ" no .hpp). Comparar com s_cam
    // diretamente e seguro aqui dentro da trava: nada mais escreve
    // alvo_largura/alvo_altura fora de iniciar_hardware().
    if (largura == s_cam.alvo_largura && altura == s_cam.alvo_altura) {
      ++s_refs;
      ok = true;
    } else {
      ESP_LOGW(TAG, "camera ja em uso a %dx%d - pedido de %dx%d recusado (uma resolucao por vez)",
               s_cam.alvo_largura, s_cam.alvo_altura, largura, altura);
      ok = false;
    }
  } else {
    ok = iniciar_hardware(largura, altura);
    if (ok) { s_refs = 1; }
  }

  destravar_ctrl();
  return ok;
}

void tab5_camera_parar(void)
{
  travar_ctrl();

  if (s_refs > 0) {
    --s_refs;
    if (s_refs == 0) { parar_hardware(); }
  }

  destravar_ctrl();
}

void tab5_camera_definir_destino(const tab5_cam_destino_t* d)
{
  if (!s_cam.trava) { return; }
  travar();
  s_cam.destino = d ? *d : tab5_cam_destino_t{ };
  s_cam.tem_novo = false;
  destravar();
}

bool tab5_camera_quadro_escrito(void)
{
  if (!s_cam.ligado) { return false; }
  travar();
  const bool novo = s_cam.tem_novo;
  s_cam.tem_novo = false;
  destravar();
  return novo;
}

size_t tab5_camera_foto_bytes(void)
{
  size_t alinhamento = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alinhamento);
  return ((size_t)TAB5_CAM_FOTO_L * TAB5_CAM_FOTO_A * 2 + alinhamento - 1) & ~(alinhamento - 1);
}

uint16_t* tab5_camera_foto_alocar(void)
{
  size_t alinhamento = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alinhamento);
  return (uint16_t*)heap_caps_aligned_calloc(alinhamento, 1, tab5_camera_foto_bytes(), MALLOC_CAP_SPIRAM);
}

bool tab5_camera_pedir_foto(uint16_t* destino)
{
  if (!s_cam.ligado || !destino) { return false; }
  bool aceito = false;
  travar();
  if (!s_cam.foto_destino) {
    s_cam.foto_destino = destino;
    s_cam.foto_pronta  = false;
    aceito = true;
  }
  destravar();
  return aceito;
}

bool tab5_camera_foto_pronta(bool* ok)
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
