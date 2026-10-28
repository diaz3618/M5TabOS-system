// APP: Camera - preview on vivo of SC2356 (MIPI-CSI) + captura para o cartao.
//
// PRIMEIRA VERSAO, escopo deliberadamente pequeno - ver o aviso grande em
// hal/tab5_camera.hpp para o que ja foi verificado (sensor, pins, barramento
// I2C shared com M5.In_I2C, o exemplo oficial que este code segue).
//
// A foto exit com o frame inteiro of sensor (1200x1600); o preview usa a
// mesma imagem em escala uniforme, sem esticar (ver hal/tab5_camera.cpp).
//
// O QUE FICA PARA DEPOIS, of proposito:
//   - Galeria (ver as fotos tiradas sem exit para o app Arquivos).
//   - Zoom/foco/ajustes of exposicao.
// Nada disso e dificil depois - e so mais of que cabia numa unica resposta
// sem virar uma parede of code nao testada em hardware nenhum.

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

static uint16_t* s_foto   = nullptr;   // a foto sendo capturada (TAB5_CAM_PHOTO_W x _A, PSRAM) - of app ate virar work
static char      s_foto_name[32];
static volatile bool s_gravou_ok = false;   // escrito pelo work (estatica: existe mesmo com o app closed)
static int       s_width, s_height;
static uint32_t  s_fotos = 0;

// CAPTURANDO: a camera esta tirando a foto para s_foto (one frame, ~33 ms).
// GRAVANDO:   one TRABALHO SOLTO grava a foto no cartao (segundos).
// O app NAO trava a navigation em nenhum dos dois. Na hora of gravar, o buffer
// PASSA A SER DO TRABALHO (sistema_trabalho_solto): ele grava e libera. Por
// isso close o app no middle of gravacao e instantaneo - antes, o fechamento
// esperava a gravacao inteira e a interface congelava.
typedef enum { ESTADO_NORMAL, ESTADO_CAPTURANDO, ESTADO_GRAVANDO, ESTADO_FLASH } estado_botao_t;
static estado_botao_t s_estado = ESTADO_NORMAL;
static bool     s_flash_sucesso = false;
static uint32_t s_flash_ate_ms = 0;   // pisca a borda of botao por one instante apos o resultado

// Botao CAPTURAR: faixa bottom fixa, independente of tamanho of area (varia
// pouco entre paisagem/retrato, mas o calculo por fracao evita numero magico).
#define BOTAO_A 96

// MAIOR "foto_NNNN.bmp" JA NO CARTAO - defeito observado: s_fotos
// comecava of zero a cada ABERTURA of app, entao toda sessao nova sobrescrevia
// "foto_0001.bmp" of sessao anterior. A numeracao tem que vir of que ja existe
// no cartao, no MESMO espirito que tab5_mesh_carregar_do_cartao() ja usa para
// reconstruir a lista of conversas a partir dos arquivos - o cartao e a fonte
// of verdade, nao uma variavel in RAM que morre when o app fecha.
static uint32_t maior_foto_existente(void)
{
  char pasta[128];
  if (!tab5_sd_folder("Camera", pasta, sizeof(pasta))) { return 0; }

  DIR* d = opendir(pasta);
  if (!d) { return 0; }   // pasta ainda nao existe - primeira foto of todas

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

// A CONTAGEM E DO WORKER: o readdir of pasta Camera rodava
// no ao_abrir, in tarefa of interface - com centenas of fotos, a camera
// demorava a aparecer. O numero so e preciso in hora of CAPTURAR; ate ele
// chegar (AVISO_TRABALHO), o botao espera.
static volatile uint32_t s_fotos_lidas = 0;
static volatile bool     s_fotos_prontas = false;   // o work terminou (resultado em s_fotos_lidas)
static bool              s_fotos_ok = false;        // s_fotos ja vale

static void work_count(void*)
{
  s_fotos_lidas = maior_foto_existente();
  s_fotos_prontas = true;
}

// O rectangle of preview, em coordenadas of TELA.
static area_t rectangle_preview(void)
{
  const area_t a = system_area();
  return area_t{ a.x, a.y, s_width, s_height };
}

// Liga o sensor e aponta o preview DIRETO para a screen virtual: o PPA of
// camera escreve o frame no lugar, e cada frame pede one redraw - que so
// avisa o compositor (sistema_superficie_mudou). Nenhuma copia pela CPU.
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
  // Multiplo of 8: exigencia of surface of DMA (sistema_superficie).
  s_height  = (a.height - BOTAO_A - 12) & ~7;
  if (s_width <= 0 || s_height <= 0) { return false; }

  if (!enable_camera()) { return false; }

  s_fotos_ok = false;
  s_fotos_prontas = false;
  if (!system_work(work_count, nullptr)) { s_fotos = 0; s_fotos_ok = true; }
  s_estado = ESTADO_NORMAL;
  s_flash_ate_ms = 0;
  ESP_LOGI(TAG, "aberto (preview %dx%d)", s_width, s_height);
  return true;
}

static void on_close(void)
{
  tab5_camera_stop();   // espera a tarefa of camera: nenhum PPA escrevendo (screen, s_foto) depois daqui
  // So existe se a captura nao chegou a virar work; a que virou e of work.
  if (s_foto) { heap_caps_free(s_foto); s_foto = nullptr; }
  ESP_LOGI(TAG, "fechado");
}

// ao_repouso()/ao_retomar() - crash (camera+sleep), ver o
// aviso grande em kernel/app.h. Solucao: os MESMOS tab5_camera_stop()/
// start() que ao_fechar()/ao_abrir() ja usam - a captura released o
// DW_GDMA dela por complete (sensor, PPA, tarefa of fundo, CAM_RST) e iteration
// of zero depois, sem inventar one terceiro estado "pausado" so para isto.
// s_foto NAO e touched aqui - o app continua "open" o time todo, so a
// captura para e iteration.
static void on_sleep(void)
{
  tab5_camera_stop();
}

static void on_resume(void)
{
  // Um request of foto em voo morreu com a captura (o driver voltou of zero):
  // sem isto o botao ficaria em "CAPTURANDO..." para sempre.
  if (s_estado == ESTADO_CAPTURANDO) { s_estado = ESTADO_NORMAL; }
  if (!enable_camera()) {
    ESP_LOGE(TAG, "falha ao religar a camera apos o repouso");
  }
}

// O PEDIDO DE GRAVACAO leva everything o que precisa: o work e released e pode
// rodar com o app ja closed (ou reaberto, com outro s_foto_nome).
typedef struct {
  uint16_t* px;
  char      name[32];
} gravacao_t;

// No worker of sistema: grava a foto (frame inteiro) e libera everything.
static void work_gravar(void* arg)
{
  gravacao_t* g = (gravacao_t*)arg;
  s_gravou_ok = tab5_bmp_salvar("Camera", g->name, g->px, TAB5_CAM_PHOTO_W, TAB5_CAM_PHOTO_H);
  heap_caps_free(g->px);
  heap_caps_free(g);
}

// Entrega s_foto a one work released. false = nao deu (s_foto continua of app).
static bool gravar_foto(void)
{
  gravacao_t* g = (gravacao_t*)heap_caps_malloc(sizeof(gravacao_t), MALLOC_CAP_DEFAULT);
  if (!g) { return false; }
  g->px = s_foto;
  snprintf(g->name, sizeof(g->name), "%s", s_foto_name);
  if (!system_work_released(work_gravar, g)) { heap_caps_free(g); return false; }
  s_foto = nullptr;
  return true;
}

static void on_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_fotos_prontas && !s_fotos_ok) {
    s_fotos = s_fotos_lidas;
    s_fotos_ok = true;
    ESP_LOGI(TAG, "%u foto(s) ja no cartao", (unsigned)s_fotos);
  }
  if ((av & AVISO_TRABALHO) && s_estado == ESTADO_GRAVANDO) {
    s_estado = ESTADO_FLASH;
    s_flash_sucesso = s_gravou_ok;
    s_flash_ate_ms = pdTICKS_TO_MS(xTaskGetTickCount()) + 600;
  }
  system_request_redraw();
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  // O frame ja esta in screen virtual (o PPA of camera o escreveu la); so
  // falta o compositor saber que a area changed.
  if (tab5_camera_frame_written()) {
    system_surface_changed(rectangle_preview());
  } else if (system_area_foi_limpa()) {
    // Ainda sem nenhum frame (sensor acabou of raise) - avisa em vez of
    // deixar a area preta parecendo travado.
    tab5_text(d, 2);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("starting the camera...", a.x + s_width / 2, a.y + s_height / 2);
    d.setTextDatum(top_left);
  }

  // A FOTO CHEGOU AO BUFFER: a gravacao vai para o worker of sistema.
  if (s_estado == ESTADO_CAPTURANDO) {
    bool ok = false;
    if (tab5_camera_photo_ready(&ok)) {
      if (ok && gravar_foto()) {
        s_estado = ESTADO_GRAVANDO;
      } else {
        s_estado = ESTADO_FLASH;
        s_flash_sucesso = false;
        s_flash_ate_ms = pdTICKS_TO_MS(xTaskGetTickCount()) + 600;
      }
    }
  } else if (s_estado == ESTADO_FLASH && pdTICKS_TO_MS(xTaskGetTickCount()) >= s_flash_ate_ms) {
    s_estado = ESTADO_NORMAL;
  }

  // ---- barra bottom: botao CAPTURAR + contador -----------------------
  const int by = a.y + s_height + 12;
  const char* text = "CAPTURAR";
  uint16_t    color    = TFT_ORANGE;
  if (s_estado == ESTADO_CAPTURANDO)                    { text = "CAPTURANDO...";      color = TFT_DARKCYAN; }
  else if (s_estado == ESTADO_GRAVANDO)                 { text = "SALVANDO...";        color = TFT_DARKCYAN; }
  else if (s_estado == ESTADO_FLASH && s_flash_sucesso) { text = "CAPTURADO!";         color = TFT_GREEN;    }
  else if (s_estado == ESTADO_FLASH)                    { text = "FALHOU";             color = TFT_RED;      }

  tab5_botao(d, a.x + 16, by, a.width - 32, BOTAO_A - 12,
             text, color, TFT_BLACK, TAB5_BOTAO_TEXTO_GRANDE);

  // ENQUANTO HOUVER PEDIDO PENDENTE, o app precisa continuar conferindo -
  // periodo_ms sozinho ja cobre isto (redesenha a cada 100ms of qualquer
  // jeito), nao precisa of request extra aqui.
}

static void on_touch(int x, int y)
{
  const int by = s_height + 12;
  if (y < by || y >= by + BOTAO_A - 12) { return; }
  // BLOQUEIA TAMBEM DURANTE O FLASH ("CAPTURADO!"/"FALHOU", 600ms) - nao so
  // durante ESTADO_CAPTURANDO. Achado on revisar "e se apertar
  // repetidamente": one touch durante o flash passava pelo
  // guarda antigo (que so olhava CAPTURANDO), comecava uma SEGUNDA captura
  // e travava o INICIO of novo - visto of fora como "o botao nao iteration on
  // normal", when in verdade tinha voltado e sido travado outra vez por one
  // touch que ninguem tinha certeza se tinha acontecido.
  if (s_estado != ESTADO_NORMAL) { return; }
  if (!s_fotos_ok) { return; }   // a numeracao ainda esta sendo lida of cartao (instantes)

  // Um buffer por foto: o anterior foi com o work of gravacao.
  if (!s_foto) { s_foto = tab5_camera_alloc_photo(); }
  if (!s_foto) { ESP_LOGE(TAG, "sem PSRAM para a foto"); return; }
  if (!tab5_camera_request_photo(s_foto)) { return; }
  snprintf(s_foto_name, sizeof(s_foto_name), "foto_%04lu", (unsigned long)(s_fotos + 1));
  ++s_fotos;
  s_estado = ESTADO_CAPTURANDO;

  // "CLIQUE" DO OBTURADOR - in hora of touch, como numa camera
  // of verdade: confirma "capturei ESTE instante". 600 Hz ("achei agudo").
  system_beep(600.0f, 40);
  system_request_redraw();
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL - body of maquina fotografica + lente, primitivas.
// ---------------------------------------------------------------------------
static void icon_camera(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(45, 40, 20);
  const uint16_t body = TFT_ORANGE;

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  const int cx = x + lado / 2, cy = y + lado / 2 + (int)(0.06f * lado);
  const int body_l = (int)(0.62f * lado), body_a = (int)(0.40f * lado);
  d.fillRoundRect(cx - body_l / 2, cy - body_a / 2, body_l, body_a, lado / 14, body);

  // "Visor" saliente em top, of lado esquerdo of body.
  const int vis_l = (int)(0.18f * lado), vis_a = (int)(0.10f * lado);
  d.fillRoundRect(cx - body_l / 2 + (int)(0.06f * lado), cy - body_a / 2 - vis_a + 2,
                   vis_l, vis_a, lado / 20, body);

  // Lente: dois circulos concentricos.
  const int r_ext = (int)(0.16f * lado), r_int = (int)(0.10f * lado);
  d.fillCircle(cx, cy, r_ext, fundo);
  d.drawCircle(cx, cy, r_ext, body);
  d.fillCircle(cx, cy, r_int, body);
}

extern const app_t APP_CAMERA = {
  .name  = "Camera",
  .color   = TFT_ORANGE,
  // SEM .camera=true AQUI - a camera virou servico (contagem
  // of referencia, ver o aviso grande em hal/tab5_camera.hpp) e liga o
  // proprio CAM_RST sozinha dentro of tab5_camera_start(), no mesmo
  // espirito que o Grove ja faz para GPS/Mesh. Declarar aqui tambem seria
  // so redundante (o kernel ainda aplicaria antes of ao_abrir(), mas quem
  // garante a energia of verdade now e o proprio servico).
  //
  // .speaker=true (junto of click of obturador em
  // ao_tocar()): on contrario of camera, o alto-falante NAO tem como se
  // auto-declarar depois of boot (a reserva dos pins I2S of ES8388 e
  // decisao of FASE 1, antes of M5.begin() - ver o aviso grande em
  // settings.h). Este app usa sound of verdade now, entao declara - mesmo
  // que o app Ajustes ja declare o mesmo hoje, e uma uniao (OR), nao concorrencia.
  .profile = { .speaker = true },
  .uses_sd = true,
  .requires_sd = true,         // sem cartao nao ha o que fazer (ver app.h)
  .piso_sleep_mhz = 0,
  .on_open   = on_open,
  .on_close  = on_close,
  .on_sleep = on_sleep,
  .on_resume = on_resume,
  .draw  = draw,
  .on_touch  = on_touch,
  // O PREVIEW NAO DEPENDE DO RELOGIO: cada frame escrito pela
  // camera pede one redraw in hora (tab5_cam_target_t.notify). O periodo
  // so conduz o botao (CAPTURANDO/SALVANDO/flash) when nao ha frame.
  .periodo_ms = 100,
  .notices    = AVISO_TRABALHO,   // a gravacao of foto terminou
  .on_aviso  = on_aviso,
  .icon = icon_camera,
};
