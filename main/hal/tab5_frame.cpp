#include "tab5_frame.hpp"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/ppa.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "soc/mipi_dsi_host_struct.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_async_memcpy.h"
// Privado do esp_lcd: o DMA2D puro que o proprio driver DPI usa. O header nao
// tem extern "C" (so e incluido por C dentro do IDF).
extern "C" {
#include "esp_async_fbcpy.h"
}

static const char *TAG = "quadro";

// Dimensoes fisicas do painel do Tab5 (Panel_ST7123: retrato, RGB565).
static const uint32_t FB_L = 720;
static const uint32_t FB_A = 1280;
static const uint32_t FB_BYTES = FB_L * FB_A * 2;

static esp_lcd_panel_handle_t s_painel = nullptr;
static SemaphoreHandle_t      s_troca  = nullptr;   // dado a cada fim de frame
// A COPIA ENTRE PAGINAS e do DMA2D PURO (esp_async_fbcpy), nao do PPA. Medido
// no aparelho com 'tela banco', painel varrendo, 720x1024 RGB565:
//   PPA (motor SRM)  32,1 ms   92 MB/s
//   GDMA AXI         14,3 ms  206 MB/s
//   DMA2D puro       10,2 ms  288 MB/s
//   CPU (memcpy)     25,0 ms  118 MB/s
// O SRM existe para escalar e girar: anda em macroblocos e le a PSRAM em
// rajadas curtas. O DMA2D le linha corrida. E o mesmo motor que o driver DPI
// usa quando pedem 'use_dma2d'. O PPA fica so de reserva, se o DMA2D nao subir.
static esp_async_fbcpy_handle_t s_dma2d = nullptr;
static SemaphoreHandle_t        s_dma2d_fim = nullptr;
static ppa_client_handle_t      s_ppa   = nullptr;
static volatile uint32_t      s_quadros = 0;

// ---------------------------------------------------------------------------
// CONTADOR DE UNDERRUN
// ---------------------------------------------------------------------------
// O driver nao expoe o evento: a ISR dele limpa o bit e so imprime, com
// esp_rom_printf, direto da interrupcao. Esse printf da ROM escreve nos DOIS
// canais de saida dela - o 1 (o console) e o 2, que o boot deixa vazio
// (cpu_start.c: esp_rom_install_channel_putc(2, NULL)). Pendurando um
// "leitor" no canal 2 vemos cada caractere impresso pela ROM sem encadear nem
// substituir o console. Ele procura a palavra "underrun" e conta.
//
// Roda dentro de ISR: IRAM, sem chamada nenhuma, so comparacao de caractere.
static const char     PALAVRA[] = "underrun";
static volatile uint8_t  s_casou = 0;
static volatile uint32_t s_underruns = 0;

static void IRAM_ATTR vigia_canal2(char c)
{
  if (c == PALAVRA[s_casou]) {
    s_casou = s_casou + 1;
    if (s_casou == sizeof(PALAVRA) - 1) { s_underruns = s_underruns + 1; s_casou = 0; }
  } else {
    s_casou = (c == PALAVRA[0]) ? 1 : 0;
  }
}

// ---------------------------------------------------------------------------
// FIM DE FRAME
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// DETECTORES QUE NAO DEPENDEM DO LOG DO DRIVER
// ---------------------------------------------------------------------------
// Piscadas azuis foram vistas a olho nu SEM nenhum "underrun" no log, mesmo com
// a mascara da ponte zerada. Sobram tres lugares onde a imagem pode falhar sem
// que a interrupcao de underrun da ponte dispare:
//
// 1) RELOGIO DO FRAME. O DMA do painel NAO e circular: esta ISR (fim do
//    frame) e que o religa para o frame seguinte (mipi_dsi_dma_trans_done_cb).
//    Se ela atrasar - interrupcoes desligadas tempo demais nesse core - o
//    frame seguinte comeca sem dados. Medimos o intervalo entre fins de
//    frame: o menor visto e o periodo real; um intervalo 50% maior e um
//    frame que atrasou. Em ciclos da CPU (o painel aceso segura
//    CPU_FREQ_MAX, entao o relogio nao muda com ele no ar).
//
// 2) ERROS DO DSI HOST (o controlador Synopsys depois da ponte). INT_ST0 traz
//    erros da camada fisica e ACK com erro do painel; INT_ST1 traz os do
//    proprio host - inclusive dpi_buff_pld_under (bit 19), a falta de dados
//    no buffer DO HOST, que a ponte nao ve. Os dois registradores se limpam
//    na leitura e ninguem mais os le (o driver do IDF nao usa as
//    interrupcoes do host). Lidos AQUI, na ISR: so existe ISR com o DSI
//    ligado, entao nunca se le um periferico desligado, e nao ha corrida com
//    o repouso desmontando o painel.
//
// 3) A COR DO UNDERRUN - ver tab5_quadro_iniciar.
static volatile int64_t  s_ult_us      = 0;   // em us (esp_timer), NAO ciclos - ver a ISR
static volatile uint32_t s_periodo_min = UINT32_MAX;   // us
static volatile uint32_t s_atrasos     = 0;            // desde o boot
static volatile uint32_t s_pior_atraso = 0;            // us, desde a ultima leitura
static volatile uint32_t s_dsi_st0     = 0;            // OU acumulado desde a ultima leitura
static volatile uint32_t s_dsi_st1     = 0;
static volatile uint32_t s_dsi_erros   = 0;
static volatile bool     s_primeiro_quadro = true;
// QUEM ESTAVA RODANDO quando o frame atrasado finalmente foi atendido. A ISR
// interrompe justamente quem segurou o core (interrupcoes desligadas, secao
// critica longa) - o nome dele e a melhor pista. Os dois cores, porque uma
// espera cruzada (trava do kernel, operacao de flash) prende o outro tambem.
static char              s_atr_t[2][16];
static volatile int      s_atr_nucleo = -1;   // core em que a ISR roda (-1 = nenhum atraso ainda)
static volatile uint32_t s_atr_seq = 0;       // sobe a cada registro novo
static volatile int64_t  s_atr_us  = 0;       // uptime (us) em que o frame atrasado foi atendido            // frames com algum bit, desde o boot

static bool IRAM_ATTR ao_fim_do_quadro(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*)
{
  s_quadros = s_quadros + 1;   // escritor unico: esta ISR

  // EM MICROSSEGUNDOS, E NAO EM CICLOS. Ciclos contam no ritmo do
  // clock da CPU, e o clock muda (360 -> 40 no repouso): o monitor dividia
  // pelo clock ATUAL e um atraso de 30 ms saiu "273,9 ms (normal 138,8)" -
  // exatos 9x. O esp_timer nao depende do clock da CPU.
  const int64_t agora = esp_timer_get_time();
  if (s_ult_us != 0) {
    const uint32_t intervalo = (uint32_t)(agora - s_ult_us);
    // Abaixo de 3 ms nao e frame de verdade (nenhum painel aqui passa de
    // ~70 Hz): ignorar, para nao envenenar o periodo minimo.
    if (intervalo < 3000u) {
    } else if (intervalo < s_periodo_min) {
      s_periodo_min = intervalo;
    } else if (intervalo > s_periodo_min + s_periodo_min / 2) {
      s_atrasos = s_atrasos + 1;
      if (intervalo > s_pior_atraso) { s_pior_atraso = intervalo; }
      for (int c = 0; c < 2; ++c) {
        const char* nm = pcTaskGetName(xTaskGetCurrentTaskHandleForCore(c));
        int k = 0;
        for (; nm && nm[k] && k < 15; ++k) { s_atr_t[c][k] = nm[k]; }
        s_atr_t[c][k] = 0;
      }
      s_atr_nucleo = xPortGetCoreID();
      s_atr_us = agora;
      s_atr_seq = s_atr_seq + 1;
    }
  }
  s_ult_us = agora;

  const uint32_t st0 = MIPI_DSI_HOST.int_st0.val;
  const uint32_t st1 = MIPI_DSI_HOST.int_st1.val;
  // O PRIMEIRO FRAME de cada painel so limpa: os registradores guardam o que
  // aconteceu na sequencia de init (comandos em LP). No aparelho, todo painel
  // novo trazia st0=0x40 (bit 6 do ACK = "false control error" observado pelo
  // painel durante o init) - sempre um, sempre ali, nunca depois.
  if (s_primeiro_quadro) { s_primeiro_quadro = false; }
  else if (st0 | st1) { s_dsi_st0 |= st0; s_dsi_st1 |= st1; s_dsi_erros = s_dsi_erros + 1; }

  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR(s_troca, &acordou);
  return acordou == pdTRUE;
}

bool tab5_quadro_iniciar(esp_lcd_panel_handle_t painel)
{
  if (painel == nullptr) { return false; }
  if (painel == s_painel) { return true; }

  if (s_troca == nullptr) {
    // Binario e nao contador: so interessa "houve um fim de frame desde
    // que armei", nunca quantos.
    s_troca = xSemaphoreCreateBinary();
    esp_rom_install_channel_putc(2, vigia_canal2);
    // Autoteste: a mesma funcao que a ISR do driver usa. Se o vigia nao
    // contar isto, o zero de underrun nao quer dizer nada.
    esp_rom_printf("quadro: teste do detector de underrun (linha esperada, nao e erro)\n");
    if (s_underruns == 1) { s_underruns = 0; ESP_LOGI(TAG, "detector de piscada (underrun) testado e funcionando"); }
    else { ESP_LOGE(TAG, "detector de piscada (underrun) NAO funciona - o contador de underrun nao vale"); }
  }
  if (s_dma2d == nullptr && s_ppa == nullptr) {
    esp_async_fbcpy_config_t fc = { };
    s_dma2d_fim = xSemaphoreCreateBinary();
    if (s_dma2d_fim == nullptr || esp_async_fbcpy_install(&fc, &s_dma2d) != ESP_OK) {
      ESP_LOGW(TAG, "DMA2D indisponivel - copia entre paginas pelo PPA (~3x mais lenta)");
      s_dma2d = nullptr;
    }
  }
  if (s_dma2d == nullptr && s_ppa == nullptr) {
    ppa_client_config_t cfg = { };
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;   // toda copia e bloqueante
    if (ppa_register_client(&cfg, &s_ppa) != ESP_OK) {
      ESP_LOGE(TAG, "PPA indisponivel - copia entre paginas desligada");
      s_ppa = nullptr;
    }
  }

  esp_lcd_dpi_panel_event_callbacks_t cbs = { };
  cbs.on_frame_buf_complete = ao_fim_do_quadro;
  if (esp_lcd_dpi_panel_register_event_callbacks(painel, &cbs, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "nao consegui registrar o aviso de fim de quadro");
    return false;
  }
  // UNDERRUN SEM MASCARA. O driver do IDF programa a ponte DSI
  // para NAO interromper em underrun enquanto a contagem de linhas estiver
  // abaixo de h_size (esp_lcd_panel_dpi.c:343 - no Tab5, 720 de 1280 linhas).
  // Resultado: falta de dados na parte de CIMA da tela deixava a imagem azul
  // em SILENCIO - sem o log do driver e sem o nosso vigia. Foi assim que uma
  // piscada azul vista a olho nu no app Energia nao deixou rastro nenhum.
  // Zerando, todo underrun vira interrupcao, log e evento do monitor.
  mipi_dsi_brg_ll_set_underrun_discard_count(&MIPI_DSI_BRIDGE, 0);

  // A COR DO UNDERRUN. O AZUL da piscada tem dono: e o registrador
  // DPI_RSV_DPI_DATA da ponte, "o pixel enviado ao host quando o FIFO da ponte
  // esvazia", cujo padrao 0x3FFF (30 bits, B nos 10 de baixo) e azul puro.
  // Trocamos para MAGENTA, que nenhuma tela do sistema usa em area grande:
  //   - piscou MAGENTA  -> e a ponte sem dados, mesmo sem a interrupcao dizer;
  //   - piscou AZUL     -> NAO e a ponte: o azul vem de outro lugar (host,
  //                        painel), e a investigacao muda de camada.
  // E diagnostico: quando a causa for achada, esta cor volta a ser decidida.
  MIPI_DSI_BRIDGE.dpi_rsv_dpi_data.dpi_rsv_data = (0x3FFu << 20) | 0x3FFu;
  mipi_dsi_brg_ll_update_dpi_config(&MIPI_DSI_BRIDGE);

  // Relogio do frame recomeca: painel novo pode ter outro periodo.
  s_ult_us      = 0;
  s_periodo_min = UINT32_MAX;
  s_primeiro_quadro = true;

  s_painel = painel;
  ESP_LOGI(TAG, "contador de quadros do painel no ar");
  return true;
}

// O repouso destroi o painel e cria outro - e o novo pode nascer no MESMO
// endereco do antigo. Sem esquecer aqui, o iniciar() acharia que ja registrou
// e o aviso de fim de frame nunca mais chegaria (achado:
// "painel 0 Hz" depois do primeiro repouso).
void tab5_quadro_encerrar(void) { s_painel = nullptr; }

// POR QUE ARMAR DEPOIS DE PEDIR A TROCA, E NAO ANTES.
//
// O pedido de troca so muda um indice; quem o le e a ISR do DMA, no fim do
// frame. Se eu limpasse o semaphore ANTES do pedido, um fim de frame que
// caisse entre a limpeza e o pedido daria o semaphore com a pagina VELHA ainda
// na varredura - e eu escreveria nela achando que estava livre. Limpando
// DEPOIS, qualquer aviso que eu venha a pegar e de um reinicio que ja leu o
// indice novo. O pior caso custa um frame a mais de espera; nunca um rasgo.
void tab5_quadro_armar_espera(void)
{
  if (s_troca) { xSemaphoreTake(s_troca, 0); }
}

bool tab5_quadro_esperar_troca(uint32_t prazo_ms)
{
  if (s_troca == nullptr) { return false; }
  return xSemaphoreTake(s_troca, pdMS_TO_TICKS(prazo_ms)) == pdTRUE;
}

// Framebuffer para framebuffer (ou tela virtual para framebuffer - os tres
// tem 720x1280 RGB565): o retangulo vai para o MESMO lugar no destino.
// Bloqueante: o compositor so pede a troca de pagina com tudo copiado.
static bool IRAM_ATTR dma2d_terminou(esp_async_fbcpy_handle_t, esp_async_fbcpy_event_data_t*, void* arg)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &acordou);
  return acordou == pdTRUE;
}

// O CACHE, que o DMA nao enxerga. Linhas inteiras do retangulo:
//  - ORIGEM: devolver a memoria o que a CPU escreveu e ainda esta so no cache;
//  - DESTINO: devolver E invalidar, para a CPU nao ler depois uma copia velha
//    que tenha ficado no cache. "So invalidar" (M2C) exige bordas na linha de
//    cache de 128 B, e um retangulo qualquer nao cai nelas - o IDF recusa
//    (foi o erro do primeiro 'tela banco'). Devolver antes de invalidar e
//    seguro com qualquer borda: nada sujo e perdido.
static void cache_das_linhas(const void* base, const tab5_ret_t& r, int flags)
{
  uint8_t* p = (uint8_t*)base + (size_t)r.y * FB_L * 2;
  esp_cache_msync(p, (size_t)r.a * FB_L * 2, flags | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool copiar_dma2d(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy)
{
  esp_async_fbcpy_trans_desc_t t = { };
  t.src_buffer = origem;
  t.dst_buffer = destino;
  t.src_buffer_size_x = FB_L; t.src_buffer_size_y = FB_A;
  t.dst_buffer_size_x = FB_L; t.dst_buffer_size_y = FB_A;
  t.src_offset_x = ox;  t.src_offset_y = oy;
  t.dst_offset_x = r.x; t.dst_offset_y = r.y;
  t.copy_size_x = r.l;  t.copy_size_y = r.a;
  t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565);

  const tab5_ret_t ro = { (int16_t)ox, (int16_t)oy, r.l, r.a };
  cache_das_linhas(origem,  ro, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  cache_das_linhas(destino, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  xSemaphoreTake(s_dma2d_fim, 0);   // nenhum aviso velho conta como este
  if (esp_async_fbcpy(s_dma2d, &t, dma2d_terminou, s_dma2d_fim) != ESP_OK) {
    ESP_LOGE(TAG, "copia %dx%d em (%d,%d) recusada pelo DMA2D", r.l, r.a, r.x, r.y);
    return false;
  }
  // Tela inteira leva ~13 ms; 100 ms e so a rede contra um DMA travado.
  if (xSemaphoreTake(s_dma2d_fim, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGE(TAG, "copia %dx%d em (%d,%d) nao terminou em 100 ms", r.l, r.a, r.x, r.y);
    return false;
  }
  return true;
}

bool tab5_quadro_copiar_de(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy)
{
  if (destino == nullptr || origem == nullptr || destino == origem) { return false; }
  if (r.l <= 0 || r.a <= 0) { return true; }
  if (ox < 0 || oy < 0 || ox + r.l > (int)FB_L || oy + r.a > (int)FB_A) { return false; }
  if (s_dma2d) { return copiar_dma2d(destino, r, origem, ox, oy); }
  // O PPA de reserva so faz o caso sem deslocamento.
  if (ox != r.x || oy != r.y) { return false; }
  return tab5_quadro_copiar(destino, origem, r);
}

bool tab5_quadro_copiar(void* destino, const void* origem, const tab5_ret_t& r)
{
  if (destino == nullptr || origem == nullptr) { return false; }
  if (r.l <= 0 || r.a <= 0) { return true; }
  if (s_dma2d) { return copiar_dma2d(destino, r, origem, r.x, r.y); }
  if (s_ppa == nullptr) { return false; }

  ppa_srm_oper_config_t op = { };
  op.in.buffer         = origem;
  op.in.pic_w          = FB_L;
  op.in.pic_h          = FB_A;
  op.in.block_w        = r.l;
  op.in.block_h        = r.a;
  op.in.block_offset_x = r.x;
  op.in.block_offset_y = r.y;
  op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

  op.out.buffer         = destino;
  op.out.buffer_size    = FB_BYTES;
  op.out.pic_w          = FB_L;
  op.out.pic_h          = FB_A;
  op.out.block_offset_x = r.x;
  op.out.block_offset_y = r.y;
  op.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  op.scale_x = 1.0f;
  op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;

  const esp_err_t e = ppa_do_scale_rotate_mirror(s_ppa, &op);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "copia %dx%d em (%d,%d) falhou: %s", r.l, r.a, r.x, r.y, esp_err_to_name(e));
    return false;
  }
  return true;
}

uint32_t tab5_quadro_underruns(void) { return s_underruns; }

uint32_t tab5_quadro_atrasos(uint32_t* pior_us, uint32_t* periodo_us)
{
  if (pior_us)    { *pior_us = s_pior_atraso; s_pior_atraso = 0; }
  if (periodo_us) { *periodo_us = (s_periodo_min != UINT32_MAX) ? s_periodo_min : 0; }
  return s_atrasos;
}

int64_t tab5_quadro_atraso_quando(void) { return s_atr_us; }

int64_t tab5_quadro_ultimo_fim_us(void) { return s_ult_us; }

uint32_t tab5_quadro_atraso_quem(int* nucleo_isr, char* t0, char* t1)
{
  if (nucleo_isr) { *nucleo_isr = s_atr_nucleo; }
  if (t0) { memcpy(t0, s_atr_t[0], 16); t0[15] = 0; }
  if (t1) { memcpy(t1, s_atr_t[1], 16); t1[15] = 0; }
  return s_atr_seq;
}

uint32_t tab5_quadro_erros_dsi(uint32_t* st0, uint32_t* st1)
{
  if (st0) { *st0 = s_dsi_st0; s_dsi_st0 = 0; }
  if (st1) { *st1 = s_dsi_st1; s_dsi_st1 = 0; }
  return s_dsi_erros;
}
uint32_t tab5_quadro_quadros(void)   { return s_quadros; }

// Espia o contador da ISR a cada tique (1 ms): percebe o fim de frame no
// maximo 1 ms depois - sobra folga de sobra ate a proxima (~15 ms).
bool tab5_quadro_esperar_fim(uint32_t prazo_ms)
{
  const uint32_t q0 = s_quadros;
  const int64_t limite = esp_timer_get_time() + (int64_t)prazo_ms * 1000;
  while (s_quadros == q0) {
    if (esp_timer_get_time() >= limite) { return false; }
    vTaskDelay(1);
  }
  return true;
}

// ---------------------------------------------------------------------------
// PREENCHIMENTO POR HARDWARE
// ---------------------------------------------------------------------------
// A medida por fase da passada completa mostrou "limpar" custando 20-25 ms em
// TODO app: a CPU escrevendo 1,6 MB de preto. O PPA tem um motor de
// preenchimento so de escrita - nao le nada, so manda a cor pela DMA2D.
//
// O cache e do driver: antes de escrever, ele devolve E invalida as linhas do
// destino (ppa_fill.c, C2M|INVALIDATE) - o que a CPU desenhou do lado, nas
// mesmas linhas, vai para a memoria antes e nada se perde.
static ppa_client_handle_t s_ppa_fill = nullptr;
static int8_t              s_fill_ok  = -1;   // -1 nao conferido, 0 desligado, 1 conferido

bool tab5_quadro_preencher(void* destino, const tab5_ret_t& r, uint16_t cor)
{
  if (s_fill_ok == 0 || destino == nullptr || r.l <= 0 || r.a <= 0) { return false; }
  if (s_ppa_fill == nullptr) {
    ppa_client_config_t cfg = { };
    cfg.oper_type = PPA_OPERATION_FILL;
    cfg.max_pending_trans_num = 1;
    if (ppa_register_client(&cfg, &s_ppa_fill) != ESP_OK) {
      ESP_LOGW(TAG, "PPA de preenchimento indisponivel - limpeza pela CPU");
      s_ppa_fill = nullptr;
      s_fill_ok = 0;
      return false;
    }
  }

  ppa_fill_oper_config_t op = { };
  op.out.buffer         = destino;
  op.out.buffer_size    = FB_BYTES;
  op.out.pic_w          = FB_L;
  op.out.pic_h          = FB_A;
  op.out.block_offset_x = r.x;
  op.out.block_offset_y = r.y;
  op.out.fill_cm        = PPA_FILL_COLOR_MODE_RGB565;
  op.fill_block_w       = r.l;
  op.fill_block_h       = r.a;
  // RGB565 -> ARGB8888 repetindo os bits altos nos baixos: o PPA corta de
  // volta para 565 e chega exatamente a mesma cor.
  const uint8_t r5 = (cor >> 11) & 0x1F, g6 = (cor >> 5) & 0x3F, b5 = cor & 0x1F;
  op.fill_argb_color.a = 0xFF;
  op.fill_argb_color.r = (uint8_t)((r5 << 3) | (r5 >> 2));
  op.fill_argb_color.g = (uint8_t)((g6 << 2) | (g6 >> 4));
  op.fill_argb_color.b = (uint8_t)((b5 << 3) | (b5 >> 2));
  op.mode = PPA_TRANS_MODE_BLOCKING;

  if (ppa_do_fill(s_ppa_fill, &op) != ESP_OK) { return false; }

  // So uma cor com os dois bytes DIFERENTES denuncia ordem de bytes trocada -
  // o preto (0x0000), que e quase toda limpeza, passaria em qualquer caso.
  if (s_fill_ok < 0 && (cor >> 8) != (cor & 0xFF)) {
    // CONFERENCIA UNICA. O driver invalidou o cache dessas linhas antes do
    // DMA e a CPU nao tocou nelas desde entao: esta leitura vem da memoria.
    const uint16_t lido = ((const uint16_t*)destino)[(size_t)r.y * FB_L + r.x];
    if (lido != cor) {
      ESP_LOGE(TAG, "preenchimento por PPA escreveu %04x em vez de %04x - desligado, limpeza pela CPU",
               (unsigned)lido, (unsigned)cor);
      s_fill_ok = 0;
      return false;   // quem chamou pinta pela CPU por cima
    }
    s_fill_ok = 1;
    ESP_LOGI(TAG, "limpeza de tela por hardware (PPA) testada e funcionando");
  }
  return true;
}

// ---------------------------------------------------------------------------
// BANCO DE PROVA DA COPIA
// ---------------------------------------------------------------------------
// A medida do compositor mostrou o PPA levando ~88 ms por quadro (~32 MB/s
// somando leitura e escrita). O motor SRM do PPA e feito para ESCALAR e GIRAR:
// trabalha em macroblocos de 16/32 pixels, e numa PSRAM isso vira rajadas
// curtas de 32-64 bytes saltando de linha em linha. Este banco poe o PPA lado
// a lado com motores que copiam linha corrida, no mesmo retangulo, com o
// painel varrendo - nada de numero de folha de dados.
namespace {

struct banco_t {
  uint16_t* src = nullptr;
  uint16_t* dst = nullptr;
  SemaphoreHandle_t fim = nullptr;
};

double mbps(uint32_t px, int64_t us) { return us > 0 ? (double)px * 4.0 / (double)us : 0.0; }

bool IRAM_ATTR fbcpy_fim(esp_async_fbcpy_handle_t, esp_async_fbcpy_event_data_t*, void* arg)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &acordou);
  return acordou == pdTRUE;
}

bool IRAM_ATTR gdma_fim(async_memcpy_handle_t, async_memcpy_event_t*, void* arg)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &acordou);
  return acordou == pdTRUE;
}

// Linha de cache L2 = 128 B: o DMA precisa das bordas do destino nela.
void sincronizar_linhas(void* base, const tab5_ret_t& r, int dir)
{
  uint8_t* p = (uint8_t*)base + (size_t)r.y * FB_L * 2;
  esp_cache_msync(p, (size_t)r.a * FB_L * 2, dir | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

int64_t por_ppa(banco_t& b, ppa_client_handle_t cli, const tab5_ret_t& r)
{
  ppa_srm_oper_config_t op = { };
  op.in.buffer = b.src; op.in.pic_w = FB_L; op.in.pic_h = FB_A;
  op.in.block_w = r.l; op.in.block_h = r.a; op.in.block_offset_x = r.x; op.in.block_offset_y = r.y;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = b.dst; op.out.buffer_size = FB_BYTES; op.out.pic_w = FB_L; op.out.pic_h = FB_A;
  op.out.block_offset_x = r.x; op.out.block_offset_y = r.y; op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0; op.scale_x = 1.0f; op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  const int64_t t0 = esp_timer_get_time();
  if (ppa_do_scale_rotate_mirror(cli, &op) != ESP_OK) { return -1; }
  return esp_timer_get_time() - t0;
}

int64_t por_dma2d(banco_t& b, esp_async_fbcpy_handle_t h, const tab5_ret_t& r)
{
  esp_async_fbcpy_trans_desc_t t = { };
  t.src_buffer = b.src; t.dst_buffer = b.dst;
  t.src_buffer_size_x = FB_L; t.src_buffer_size_y = FB_A;
  t.dst_buffer_size_x = FB_L; t.dst_buffer_size_y = FB_A;
  t.src_offset_x = r.x; t.src_offset_y = r.y; t.dst_offset_x = r.x; t.dst_offset_y = r.y;
  t.copy_size_x = r.l; t.copy_size_y = r.a;
  t.pixel_format_unique_id.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565);
  const int64_t t0 = esp_timer_get_time();
  sincronizar_linhas(b.src, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  sincronizar_linhas(b.dst, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (esp_async_fbcpy(h, &t, fbcpy_fim, b.fim) != ESP_OK) { return -1; }
  if (xSemaphoreTake(b.fim, pdMS_TO_TICKS(500)) != pdTRUE) { return -2; }
  return esp_timer_get_time() - t0;
}

int64_t por_gdma(banco_t& b, async_memcpy_handle_t h, const tab5_ret_t& r)
{
  // So retangulo de largura inteira: ai ele e um trecho CONTIGUO da memoria.
  uint8_t* s = (uint8_t*)b.src + (size_t)r.y * FB_L * 2;
  uint8_t* d = (uint8_t*)b.dst + (size_t)r.y * FB_L * 2;
  const size_t n = (size_t)r.a * FB_L * 2;
  const int64_t t0 = esp_timer_get_time();
  if (esp_async_memcpy(h, d, s, n, gdma_fim, b.fim) != ESP_OK) { return -1; }
  if (xSemaphoreTake(b.fim, pdMS_TO_TICKS(500)) != pdTRUE) { return -2; }
  return esp_timer_get_time() - t0;
}

int64_t por_cpu(banco_t& b, const tab5_ret_t& r)
{
  const int64_t t0 = esp_timer_get_time();
  const size_t lb = (size_t)r.l * 2;
  for (int y = r.y; y < r.y + r.a; ++y) {
    memcpy(b.dst + (size_t)y * FB_L + r.x, b.src + (size_t)y * FB_L + r.x, lb);
  }
  // O painel (DMA) le da memoria, nao do cache: escrita da CPU so vale
  // depois de devolvida. Entra na conta.
  sincronizar_linhas(b.dst, r, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  return esp_timer_get_time() - t0;
}

void mostrar(const char* motor, const char* qual, uint32_t px, int64_t us)
{
  if (us < 0) { printf("  %-8s %-14s : falhou (%lld)\n", motor, qual, (long long)us); return; }
  printf("  %-8s %-14s : %7.2f ms  %6.1f MB/s\n", motor, qual, (double)us / 1000.0, mbps(px, us));
}

} // namespace

void tab5_quadro_banco(void)
{
  banco_t b;
  b.src = (uint16_t*)heap_caps_aligned_calloc(128, 1, FB_BYTES, MALLOC_CAP_SPIRAM);
  b.dst = (uint16_t*)heap_caps_aligned_calloc(128, 1, FB_BYTES, MALLOC_CAP_SPIRAM);
  b.fim = xSemaphoreCreateBinary();
  if (!b.src || !b.dst || !b.fim) {
    printf("banco: sem memoria\n");
  } else {
    for (uint32_t i = 0; i < FB_L * FB_A; ++i) { b.src[i] = (uint16_t)(i * 2654435761u >> 16); }
    esp_cache_msync(b.src, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // Cliente PROPRIO: o do compositor continua livre para a interface.
    ppa_client_handle_t cli = nullptr;
    ppa_client_config_t pc = { };
    pc.oper_type = PPA_OPERATION_SRM;
    pc.max_pending_trans_num = 1;
    if (ppa_register_client(&pc, &cli) != ESP_OK) { cli = nullptr; }

    esp_async_fbcpy_handle_t fb = nullptr;
    esp_async_fbcpy_config_t fc = { };
    if (esp_async_fbcpy_install(&fc, &fb) != ESP_OK) { fb = nullptr; }

    async_memcpy_handle_t gd = nullptr;
    async_memcpy_config_t gc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    gc.backlog = 4;
    gc.dma_burst_size = 64;
    if (esp_async_memcpy_install_gdma_axi(&gc, &gd) != ESP_OK) { gd = nullptr; }

    // Largura inteira (o caso da rolagem e da tela cheia) e meia largura (o
    // caso de uma area suja comum, com salto de linha).
    const tab5_ret_t cheio = { 0, 128, (int16_t)FB_L, 1024 };
    const tab5_ret_t meio  = { 0, 128, (int16_t)(FB_L / 2), 1024 };
    const uint32_t px_cheio = (uint32_t)cheio.l * cheio.a;
    const uint32_t px_meio  = (uint32_t)meio.l * meio.a;
    const int REP = 3;

    printf("--- banco da copia (PSRAM -> PSRAM, painel varrendo) ---\n");
    auto rodar = [&](const char* motor, const char* qual, uint32_t px, auto f) {
      int64_t soma = 0;
      for (int i = 0; i < REP; ++i) {
        const int64_t us = f();
        if (us < 0) { mostrar(motor, qual, px, us); return; }
        soma += us;
      }
      mostrar(motor, qual, px, soma / REP);
    };
    if (cli) {
      rodar("PPA", "720x1024", px_cheio, [&] { return por_ppa(b, cli, cheio); });
      rodar("PPA", "360x1024", px_meio,  [&] { return por_ppa(b, cli, meio); });
    }
    if (fb) {
      rodar("DMA2D", "720x1024", px_cheio, [&] { return por_dma2d(b, fb, cheio); });
      rodar("DMA2D", "360x1024", px_meio,  [&] { return por_dma2d(b, fb, meio); });
    }
    if (gd) {
      rodar("GDMA", "720x1024", px_cheio, [&] { return por_gdma(b, gd, cheio); });
    }
    // PREENCHIMENTO: so escrita (2 B por pixel - o MB/s abaixo conta x4 como
    // as copias, entao compare os ms).
    rodar("PPA-fill", "720x1024", px_cheio, [&] {
      const int64_t t0 = esp_timer_get_time();
      if (!tab5_quadro_preencher(b.dst, cheio, 0x0000)) { return (int64_t)-1; }
      return esp_timer_get_time() - t0;
    });
    rodar("CPU-fill", "720x1024", px_cheio, [&] {
      const int64_t t0 = esp_timer_get_time();
      for (int y = cheio.y; y < cheio.y + cheio.a; ++y) { memset(b.dst + (size_t)y * FB_L, 0, FB_L * 2); }
      sincronizar_linhas(b.dst, cheio, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      return esp_timer_get_time() - t0;
    });
    rodar("CPU", "720x1024", px_cheio, [&] { return por_cpu(b, cheio); });
    rodar("CPU", "360x1024", px_meio,  [&] { return por_cpu(b, meio); });

    // A copia saiu certa? Uma conferencia barata: o ultimo motor foi a CPU,
    // e a CPU nao erra - confere o DMA2D separadamente.
    if (fb) {
      memset(b.dst, 0, FB_BYTES);
      esp_cache_msync(b.dst, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      por_dma2d(b, fb, cheio);
      const bool ok = memcmp(b.dst + (size_t)cheio.y * FB_L, b.src + (size_t)cheio.y * FB_L,
                             (size_t)cheio.a * FB_L * 2) == 0;
      printf("  DMA2D copiou certo        : %s\n", ok ? "sim" : "NAO");
    }

    if (gd)  { esp_async_memcpy_uninstall(gd); }
    if (fb)  { esp_async_fbcpy_uninstall(fb); }
    if (cli) { ppa_unregister_client(cli); }
  }
  if (b.fim) { vSemaphoreDelete(b.fim); }
  heap_caps_free(b.src);
  heap_caps_free(b.dst);
}
