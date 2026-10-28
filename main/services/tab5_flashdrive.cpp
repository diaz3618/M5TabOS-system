#include "tab5_flashdrive.hpp"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "driver/usb_serial_jtag.h"
#include "esp_private/usb_phy.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/usb_wrap_struct.h"
#include "tusb.h"
#include "tab5_owners.hpp"
#include "tab5_sd.hpp"
#include "tab5_power.hpp"
#include "app.h"            // sistema_app_exige_sd, sistema_manter_acordado, sistema_app_atual...
#include "settings.h"
#include "notification.h"
#include "tab5_log.hpp"

static const char* TAG = "pendrive";

static sdmmc_card_t*      s_cartao  = nullptr;
static usb_phy_handle_t   s_phy     = nullptr;
static TaskHandle_t       s_tarefa  = nullptr;
static SemaphoreHandle_t  s_parou   = nullptr;
static volatile bool      s_parar   = false;
static volatile bool      s_ligado  = false;
static volatile bool      s_ejetado = false;
static volatile bool      s_suspenso = false;
static esp_pm_lock_handle_t s_pm = nullptr;

// DIAGNOSTICO POR ETAPA - onde o computador parou (ver tab5_pendrive_usb_t):
// reconheceu o aparelho? perguntou o que ele e? o tamanho? leu?
static volatile uint32_t s_n_config = 0;      // SET_CONFIGURATION (tud_mount_cb)
static volatile uint32_t s_n_inquiry = 0;     // INQUIRY
static volatile uint32_t s_n_capacidade = 0;  // READ CAPACITY / READ FORMAT CAPACITIES
static volatile uint32_t s_n_leituras = 0;    // pedacos de READ10 lidos do cartao (ate 4 KB cada)
static volatile uint8_t  s_op_recusado = 0;   // ultimo comando SCSI nao suportado (0 = nenhum)
static volatile uint32_t s_leitura_max_us = 0;// leitura do cartao mais lenta
static uint32_t          s_ops_logados[8];    // comandos recusados (bit por codigo)
static volatile uint32_t s_n_tur = 0;         // TEST UNIT READY
static volatile uint32_t s_n_sense = 0;       // REQUEST SENSE (o computador quer saber de um erro)
static volatile uint32_t s_n_respostas = 0;   // comandos (fora leitura/escrita) com o status entregue
static volatile uint8_t  s_op_respondido = 0; // o ultimo deles
static volatile uint32_t s_n_soltou = 0;      // desconfigurou (reset do barramento, cabo)
static uint32_t          s_ops_respondidos[8];// comandos ja respondidos (bit por codigo)
static volatile uint32_t s_n_leituras_ok = 0; // comandos READ10 com dados e status entregues
// ONDE PAROU, quando para: os tres estados que um comando de leitura atravessa.
static volatile uint32_t s_n_cmd_leitura = 0; // comandos READ10 recebidos
static volatile bool     s_lendo = false;     // um READ10 comecou e o status ainda nao saiu
static volatile bool     s_no_cartao = false; // dentro do sdmmc_read/write_sectors agora
static volatile bool     s_gravando = false;  // ...e e uma escrita (para o log dizer qual)
static volatile uint32_t s_escrita_max_us = 0;// escrita do cartao mais lenta
static volatile uint32_t s_n_escritas_lentas = 0; // escritas de mais de 100 ms
static volatile uint32_t s_cmd_setor = 0;     // primeiro setor do READ10 em curso
static volatile uint32_t s_ultimo_setor = 0;  // ultimo pedaco pedido ao cartao
static volatile int64_t  s_evento_us = 0;     // ultimo sinal de vida do computador ou do cartao
static volatile uint32_t s_erro_setor = 0;    // a ultima falha do cartao (para o log)
static volatile int      s_erro_cod = 0;
static volatile uint64_t  s_lidos = 0, s_gravados = 0;
static volatile uint32_t  s_erros = 0;
static volatile int64_t   s_ultimo_io_us = 0, s_ultima_escrita_us = 0;

// ---------------------------------------------------------------------------
// O PHY: do console para o OTG, e de volta
// ---------------------------------------------------------------------------
// LP_SYS.usb_ctrl.sw_usb_phy_sel (hal/usb_wrap_ll.h):
//   falso - JTAG no PHY 0 (o USB-C), OTG no PHY 1 (o padrao do chip)
//   verdadeiro - OTG no PHY 0, JTAG no PHY 1
// O JTAG NAO e desligado nem reiniciado: o driver do console continua
// instalado, com as interrupcoes como estavam, e so deixa de ver o
// computador (para ele e um cabo puxado - o log que nao sai e descartado,
// como com o aparelho na bateria). Por isso o console volta sozinho.
static void phy_para_o_otg(void)
{
  usb_serial_jtag_ll_phy_enable_pad(false);   // solta os resistores do JTAG no PHY 0
  usb_wrap_ll_phy_select(&USB_WRAP, 0);       // OTG -> PHY 0 (USB-C), JTAG -> PHY 1
}

// O pad do OTG ja tem que estar desligado (antes do usb_del_phy, que corta
// o clock do USB_WRAP - registrador de periferico sem clock nao se escreve).
static void phy_para_o_console(void)
{
  usb_serial_jtag_ll_phy_select(0);           // JTAG -> PHY 0 de novo
  usb_serial_jtag_ll_phy_set_defaults();      // e com o pad (pull-up) ligado
  usb_serial_jtag_ll_phy_enable_pad(true);
}

// SENTINELA DO PAD. O ESP-IDF supoe que o PHY dos GPIO 26/27 e sempre do OTG
// (hal/esp32p4/.../gpio_ll.h): configurar qualquer um desses pinos - o I2S do
// alto-falante usa os dois - escreve usb_pad_enable = 0 e tira o pull-up do
// D+ no registrador DO OTG, que agora esta no USB-C. Para o computador, o cabo
// saiu (sem reset nenhum), e o Tab5 seguia esperando o proximo comando. O
// alto-falante e calado antes de o USB subir (sistema_som_ceder_pinos); isto
// pega qualquer outro que ainda mexer nesses pinos: religa e conta.
static volatile uint32_t s_n_pad_religado = 0;
static bool pad_conferir(void)
{
  usb_wrap_otg_conf_reg_t c;
  c.val = USB_WRAP.otg_conf.val;
  if (c.usb_pad_enable && !c.pad_pull_override) { return false; }
  c.usb_pad_enable = 1;
  c.pad_pull_override = 0;   // o pull-up volta a ser do controlador
  USB_WRAP.otg_conf.val = c.val;
  s_n_pad_religado = s_n_pad_religado + 1;
  return true;
}

// ---------------------------------------------------------------------------
// Descritores
// ---------------------------------------------------------------------------
// VID da Espressif com o PID que o esp_tinyusb usa para "so MSC" (0x4000 com
// o bit do MSC) - e NAO o 0x1001 do JTAG: o computador guarda driver por
// VID:PID, e o mesmo par para duas funcoes diferentes confunde o Windows.
static const tusb_desc_device_t DESC_APARELHO = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = 0x00,   // a classe vem na interface (o macOS prefere assim)
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = 0x303A,
  .idProduct          = 0x4008,
  .bcdDevice          = 0x0100,
  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,
  .bNumConfigurations = 0x01,
};

#define EP_SAIDA   0x01
#define EP_ENTRADA 0x81
static const uint8_t DESC_CONFIG[] = {
  TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN,
                        TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
  TUD_MSC_DESCRIPTOR(0, 0, EP_SAIDA, EP_ENTRADA, 64),
};

// O NUMERO DE SERIE segue a especificacao de armazenamento em massa (Bulk-Only
// Transport, 4.1.1): pelo menos 12 digitos hexadecimais. O macOS e o mais
// exigente com isso. Sai do MAC do proprio aparelho: unico e sempre o mesmo,
// entao o computador reconhece o Tab5 de uma vez para a outra.
static char s_serie[13] = "000000000000";
static const char* const TEXTOS[] = { "", "M5Stack", "M5tabOS cartao SD", s_serie };

extern "C" const uint8_t* tud_descriptor_device_cb(void) { return (const uint8_t*)&DESC_APARELHO; }
extern "C" const uint8_t* tud_descriptor_configuration_cb(uint8_t) { return DESC_CONFIG; }

extern "C" const uint16_t* tud_descriptor_string_cb(uint8_t indice, uint16_t)
{
  static uint16_t d[33];
  uint8_t n;
  if (indice == 0) {
    d[1] = 0x0409;   // ingles (EUA): o idioma que todo sistema aceita
    n = 1;
  } else {
    if (indice >= sizeof(TEXTOS) / sizeof(TEXTOS[0])) { return nullptr; }
    const char* t = TEXTOS[indice];
    n = (uint8_t)strnlen(t, 32);
    for (uint8_t i = 0; i < n; ++i) { d[1 + i] = (uint8_t)t[i]; }
  }
  d[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return d;
}

// ---------------------------------------------------------------------------
// MSC: o cartao em setores
// ---------------------------------------------------------------------------
// Tudo aqui roda na tarefa "pendrive" (tud_task), uma chamada por vez - o
// cartao cru so tem esse usuario enquanto esta emprestado.
static uint32_t setor_tam(void) { return s_cartao ? (uint32_t)s_cartao->csd.sector_size : 512; }

// O QUE O DISCO E, e o tamanho da resposta. O TinyUSB responde o INQUIRY com
// 36 bytes; se o computador pediu mais (ha sistemas que pedem 255), ela manda
// os 36 e TRAVA o canal de dados (STALL) - o computador precisa destrava-lo
// (CLEAR_FEATURE) para so entao ler o status. E o primeiro comando do disco a
// passar por esse caminho, e o log parava exatamente nele: o computador
// perguntava e nunca chegava ao tamanho. A especificacao do transporte (BOT
// 6.7, caso 5) deixa a outra saida: completar com zeros ate o tamanho pedido.
// O comprimento verdadeiro vai no proprio cabecalho (additional_length), entao
// os zeros sao so enchimento. Pedido de 36, resposta de 36 - nada muda.
#define RESPOSTA_CHEIA 255
static void inquiry_contar(void) { s_n_inquiry = s_n_inquiry + 1; }

static void inquiry_textos(uint8_t vendor[8], uint8_t produto[16], uint8_t rev[4])
{
  memset(vendor, ' ', 8);   memcpy(vendor, "M5Stack", 7);
  memset(produto, ' ', 16); memcpy(produto, "Tab5 cartao SD", 14);
  memset(rev, ' ', 4);      memcpy(rev, "1.0", 3);
}

extern "C" uint32_t tud_msc_inquiry2_cb(uint8_t, scsi_inquiry_resp_t* r, uint32_t bufsize)
{
  inquiry_contar();
  inquiry_textos(r->vendor_id, r->product_id, r->product_rev);
  const uint32_t n = bufsize < RESPOSTA_CHEIA ? bufsize : RESPOSTA_CHEIA;
  if (n <= sizeof(*r)) { return sizeof(*r); }
  memset((uint8_t*)r + sizeof(*r), 0, n - sizeof(*r));
  return n;   // o TinyUSB corta no que o computador pediu
}

// A versao antiga: so e chamada se a de cima nao existir na biblioteca.
extern "C" void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t produto[16], uint8_t rev[4])
{
  inquiry_contar();
  inquiry_textos(vendor, produto, rev);
}

// REQUEST SENSE, o mesmo caso: 18 bytes, e o computador pede mais (o Linux
// pede 96). O TinyUSB ja montou os 18 em 'buf'; o resto e zero.
extern "C" int32_t tud_msc_request_sense_cb(uint8_t, void* buf, uint16_t bufsize)
{
  s_n_sense = s_n_sense + 1;
  const uint16_t n = bufsize < RESPOSTA_CHEIA ? bufsize : RESPOSTA_CHEIA;
  if (n <= sizeof(scsi_sense_fixed_resp_t)) { return sizeof(scsi_sense_fixed_resp_t); }
  memset((uint8_t*)buf + sizeof(scsi_sense_fixed_resp_t), 0, n - sizeof(scsi_sense_fixed_resp_t));
  return n;
}

// O STATUS DE UM COMANDO CHEGOU AO COMPUTADOR (o TinyUSB so chama isto quando
// a resposta inteira saiu). E o que separa "o computador nao perguntou" de
// "perguntou e a resposta nao chegou".
//
// NENHUM LOG AQUI DENTRO, nem nos outros retornos do TinyUSB: o ESP_LOG desce
// ate o console (USB-Serial-JTAG, sem computador durante o pendrive - ate
// 50 ms de espera), pelas travas do stdout e pela pilha da tarefa do USB, no
// meio de uma transferencia. Os retornos so contam; quem escreve no log e a
// tarefa da sessao, que olha os contadores (ver registrar_eventos).
extern "C" void tud_msc_scsi_complete_cb(uint8_t, const uint8_t cmd[16])
{
  const uint8_t op = cmd[0];
  s_n_respostas = s_n_respostas + 1;
  s_op_respondido = op;
  s_ops_respondidos[op >> 5] |= 1u << (op & 31);
  s_evento_us = esp_timer_get_time();
}

extern "C" void tud_msc_read10_complete_cb(uint8_t)
{
  s_n_leituras_ok = s_n_leituras_ok + 1;
  s_lendo = false;
  s_evento_us = esp_timer_get_time();
}

extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
  s_n_tur = s_n_tur + 1;
  s_evento_us = esp_timer_get_time();
  // Ejetado: "sem midia" - o computador para de mandar comandos ao disco.
  if (s_ejetado || !s_cartao) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
  }
  return true;
}

extern "C" void tud_msc_capacity_cb(uint8_t, uint32_t* n_blocos, uint16_t* bloco)
{
  s_n_capacidade = s_n_capacidade + 1;
  *n_blocos = s_cartao ? (uint32_t)s_cartao->csd.capacity : 0;
  *bloco    = (uint16_t)setor_tam();
}

extern "C" bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool ejetar)
{
  // "Ejetar" no Finder/Explorer/Linux chega aqui como STOP + eject - o sinal
  // de que o computador terminou e nao tem mais nada em cache para o disco.
  if (ejetar) {
    s_ejetado = !start;
  }
  return true;
}

extern "C" bool tud_msc_prevent_allow_medium_removal_cb(uint8_t, uint8_t, uint8_t) { return true; }

extern "C" bool tud_msc_is_writable_cb(uint8_t) { return !s_ejetado; }

extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buf, uint32_t tam)
{
  const uint32_t st = setor_tam();
  if (!s_cartao || s_ejetado || offset % st || tam < st) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return -1;
  }
  const uint32_t n = tam / st;
  if (!s_lendo) {   // primeiro pedaco de um comando novo
    s_lendo = true;
    s_n_cmd_leitura = s_n_cmd_leitura + 1;
    s_cmd_setor = lba;
  }
  s_n_leituras = s_n_leituras + 1;
  s_ultimo_setor = lba + offset / st;
  const int64_t t0 = esp_timer_get_time();
  s_evento_us = t0;
  s_no_cartao = true;
  const esp_err_t lido = sdmmc_read_sectors(s_cartao, buf, lba + offset / st, n);
  s_no_cartao = false;
  const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  s_evento_us = esp_timer_get_time();
  if (dt > s_leitura_max_us) { s_leitura_max_us = dt; }
  if (lido != ESP_OK) {
    s_erro_setor = lba + offset / st;
    s_erro_cod = lido;
    s_erros = s_erros + 1;
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // erro de leitura
    return -1;
  }
  s_lidos = s_lidos + (uint64_t)n * st;
  s_ultimo_io_us = esp_timer_get_time();
  return (int32_t)(n * st);
}

extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t tam)
{
  const uint32_t st = setor_tam();
  if (!s_cartao || s_ejetado || offset % st || tam < st) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return -1;
  }
  // So setores inteiros: o que sobrar (nunca, com o buffer em multiplo de
  // 512) o TinyUSB entrega de novo na proxima chamada.
  const uint32_t n = tam / st;
  const int64_t t0 = esp_timer_get_time();
  s_evento_us = t0;
  s_ultimo_setor = lba + offset / st;
  s_gravando = true;
  s_no_cartao = true;
  const esp_err_t gravado = sdmmc_write_sectors(s_cartao, buf, lba + offset / st, n);
  s_no_cartao = false;
  s_gravando = false;
  s_evento_us = esp_timer_get_time();
  const uint32_t dt = (uint32_t)(s_evento_us - t0);
  if (dt > s_escrita_max_us) { s_escrita_max_us = dt; }
  if (dt > 100000) { s_n_escritas_lentas = s_n_escritas_lentas + 1; }
  if (gravado != ESP_OK) {
    s_erro_setor = lba + offset / st;
    s_erro_cod = gravado;
    s_erros = s_erros + 1;
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);   // erro de escrita
    return -1;
  }
  s_gravados = s_gravados + (uint64_t)n * st;
  const int64_t agora = esp_timer_get_time();
  s_ultimo_io_us = agora;
  s_ultima_escrita_us = agora;
  return (int32_t)(n * st);
}

extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t cmd[16], void* buf, uint16_t tam)
{
  switch (cmd[0]) {
    case 0x35:   // SYNCHRONIZE CACHE (10): nao ha cache do lado de ca
      return 0;
    case 0x5A: { // MODE SENSE (10). O macOS pergunta por ele (o TinyUSB so traz
                 // o de 6 bytes) para saber se o disco aceita escrita. Cabecalho
                 // de 8 bytes, sem descritor de bloco: tamanho dos dados (6), tipo
                 // de midia 0, bit de protecao contra escrita.
      const uint8_t r[8] = { 0, 6, 0, (uint8_t)(s_ejetado ? 0x80 : 0x00), 0, 0, 0, 0 };
      const uint16_t k = tam < sizeof(r) ? tam : (uint16_t)sizeof(r);
      memcpy(buf, r, k);
      return k;
    }
    default: {
      // Recusado: e registrado (uma vez por comando) - e o primeiro suspeito
      // quando um sistema nao monta o disco.
      const uint8_t op = cmd[0];
      s_op_recusado = op;
      s_ops_logados[op >> 5] |= 1u << (op & 31);
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      return -1;
    }
  }
}

// "Mount" no TinyUSB e o computador CONFIGURAR o aparelho (enumeracao feita) -
// ainda nao e o disco aberto: isso vem depois, com INQUIRY, tamanho e leituras.
extern "C" void tud_mount_cb(void)
{
  s_suspenso = false;
  s_n_config = s_n_config + 1;
  s_lendo = false;
  s_evento_us = esp_timer_get_time();
}

// NAO e ejecao. Desconfigurar tambem acontece num reset de recuperacao do
// computador no meio de uma copia - devolver o cartao ai seria tirar o disco
// de baixo dele. Sem montagem, a tela so deixa de pedir confirmacao para sair.
extern "C" void tud_umount_cb(void)
{
  s_n_soltou = s_n_soltou + 1;
  s_lendo = false;   // reset do barramento: o comando em curso morreu com ele
}
extern "C" void tud_suspend_cb(bool)  { s_suspenso = true; }
extern "C" void tud_resume_cb(void)   { s_suspenso = false; }

// ---------------------------------------------------------------------------
// A tarefa e o ciclo de vida
// ---------------------------------------------------------------------------
// O TINYUSB NASCE E MORRE DENTRO DA PROPRIA TAREFA, fixa num nucleo. O
// tusb_init aloca a interrupcao do USB no nucleo de quem o chama; a
// biblioteca protege o estado das transferencias com uma trava que, no mesmo
// nucleo, desliga as interrupcoes - mas o tratamento da interrupcao (escrever
// e ler a FIFO, terminar transferencias) nao pega essa trava. Com a
// interrupcao num nucleo e a tarefa migrando para o outro, os dois mexem no
// mesmo endpoint ao mesmo tempo, e a transferencia trava sem erro nenhum -
// a cada vez num ponto diferente. Antes o tusb_init rodava na tarefa da
// sessao (sem nucleo fixo) e esta tarefa tambem nao tinha nucleo. E o que o
// esp_tinyusb da Espressif faz (tinyusb_task.c): init na tarefa, tarefa fixa.
// Nucleo 1, o mesmo que o esp_tinyusb usa por padrao (o 0 fica com o WiFi).
#define PENDRIVE_NUCLEO 1
static SemaphoreHandle_t s_iniciou = nullptr;
static volatile bool     s_init_ok = false;

static void vTaskPendrive(void*)
{
  tusb_rhport_init_t ini = { };
  ini.role  = TUSB_ROLE_DEVICE;
  ini.speed = TUSB_SPEED_FULL;
  s_init_ok = tusb_init(0, &ini);
  xSemaphoreGive(s_iniciou);
  if (s_init_ok) {
    while (!s_parar) {
      tud_task_ext(20, false);   // acorda a cada 20 ms para ver o pedido de parar
    }
    tusb_deinit(0);              // no mesmo nucleo em que a interrupcao nasceu
  }
  tab5_owner_forget(xTaskGetCurrentTaskHandle());
  xSemaphoreGive(s_parou);
  vTaskDelete(nullptr);
}

// O controlador tem DMA? (GHWCFG2.OtgArch = 2). Sem ele o TinyUSB cai no modo
// escravo - so para o log dizer qual dos dois esta em uso.
static const char* usb_modo(void)
{
  const uint32_t ghwcfg2 = *(volatile const uint32_t*)(0x50040000UL + 0x48);
  return ((ghwcfg2 >> 3) & 3) == 2 ? "DMA" : "escravo";
}

// CLOCK NO MAXIMO enquanto o computador estiver falando: com o clock
// variavel, cada pausa entre pacotes derruba a CPU para o piso e o proximo
// pedido do computador espera ela subir. Com o barramento PARADO (cabo puxado
// sem ejetar, computador dormindo) a trava sai - ver o laco da sessao: ai o
// Tab5 esta na bateria e nao ha pedido nenhum para atender depressa.
static bool s_clock_max = false;
static void clock_maximo(bool sim)
{
  if (sim == s_clock_max) { return; }
  if (!s_pm && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pendrive", &s_pm) != ESP_OK) { s_pm = nullptr; }
  if (!s_pm) { return; }
  if (sim) { esp_pm_lock_acquire(s_pm); } else { esp_pm_lock_release(s_pm); }
  s_clock_max = sim;
}

static bool usb_ligar(sdmmc_card_t* cartao)
{
  if (s_ligado) { return true; }
  if (!cartao) { return false; }
  if (!s_parou) { s_parou = xSemaphoreCreateBinary(); }
  if (!s_iniciou) { s_iniciou = xSemaphoreCreateBinary(); }
  if (!s_parou || !s_iniciou) { return false; }

  s_cartao = cartao;
  s_ejetado = false; s_suspenso = false; s_parar = false;
  s_lidos = 0; s_gravados = 0; s_erros = 0;
  s_n_config = 0; s_n_inquiry = 0; s_n_capacidade = 0; s_n_leituras = 0; s_n_leituras_ok = 0;
  s_op_recusado = 0; s_leitura_max_us = 0;
  memset(s_ops_logados, 0, sizeof(s_ops_logados));
  s_n_tur = 0; s_n_sense = 0; s_n_respostas = 0; s_op_respondido = 0; s_n_soltou = 0;
  memset(s_ops_respondidos, 0, sizeof(s_ops_respondidos));
  s_erro_setor = 0; s_erro_cod = 0;
  s_n_cmd_leitura = 0; s_lendo = false; s_no_cartao = false;
  s_cmd_setor = 0; s_ultimo_setor = 0; s_evento_us = 0;
  s_n_pad_religado = 0;
  s_gravando = false; s_escrita_max_us = 0; s_n_escritas_lentas = 0;
  uint8_t mac[6] = { };
  esp_read_mac(mac, ESP_MAC_BASE);
  snprintf(s_serie, sizeof(s_serie), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  s_ultimo_io_us = 0; s_ultima_escrita_us = 0;

  ESP_LOGI(TAG, "ligando: o console sai do USB-C ate o pendrive acabar");
  vTaskDelay(pdMS_TO_TICKS(50));   // deixa a linha acima chegar ao computador

  phy_para_o_otg();
  static const usb_phy_otg_io_conf_t io = USB_PHY_SELF_POWERED_DEVICE(-1);
  usb_phy_config_t cfg = { };
  cfg.controller  = USB_PHY_CTRL_OTG;
  cfg.target      = USB_PHY_TARGET_INT;      // o PHY full speed (agora o 0)
  cfg.otg_mode    = USB_OTG_MODE_DEVICE;
  cfg.otg_speed   = USB_PHY_SPEED_FULL;      // sem isto o IDF troca para o PHY de alta velocidade
  cfg.otg_io_conf = &io;
  if (usb_new_phy(&cfg, &s_phy) != ESP_OK) {
    ESP_LOGE(TAG, "PHY do OTG recusado");
    s_phy = nullptr;
    phy_para_o_console();
    s_cartao = nullptr;
    return false;
  }

  // PRIORIDADE ALTA, acima da interface: ao montar, o computador le a tabela
  // FAT inteira de uma vez, e o macOS desiste (erro de E/S, o disco nem
  // aparece) se ela demorar. Uma volta de desenho da tela na frente de cada
  // pedido era atraso puro. A tarefa quase sempre dorme esperando o USB.
  // A pilha e a do proprio TinyUSB mais a descida ate o driver do cartao.
  xSemaphoreTake(s_iniciou, 0);   // sem sobra de uma vez anterior
  xTaskCreatePinnedToCore(vTaskPendrive, "pendrive", 4096, nullptr, configMAX_PRIORITIES - 2, &s_tarefa, PENDRIVE_NUCLEO);
  if (!s_tarefa) {
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
    usb_del_phy(s_phy); s_phy = nullptr;
    phy_para_o_console();
    s_cartao = nullptr;
    return false;
  }
  xSemaphoreTake(s_iniciou, portMAX_DELAY);
  if (!s_init_ok) {
    ESP_LOGE(TAG, "TinyUSB nao iniciou");
    xSemaphoreTake(s_parou, portMAX_DELAY);   // a tarefa ja esta saindo
    s_tarefa = nullptr;
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
    usb_del_phy(s_phy); s_phy = nullptr;
    phy_para_o_console();
    s_cartao = nullptr;
    return false;
  }
  tab5_owner_register(s_tarefa, OWNER_SERVICE, "Pendrive");
  clock_maximo(true);
  tud_connect();
  s_ligado = true;
  ESP_LOGI(TAG, "USB no ar: modo %s, nucleo %d", usb_modo(), PENDRIVE_NUCLEO);
  return true;
}

static void usb_desligar(void)
{
  if (!s_ligado) { return; }
  tud_disconnect();                       // o computador ve o disco sair
  vTaskDelay(pdMS_TO_TICKS(30));
  s_parar = true;
  // A tarefa termina o comando em curso (no maximo uma leitura/escrita de
  // 4 KB), desliga o TinyUSB e sai; so depois disso o cartao para de ser
  // tocado pelo USB.
  //
  // ESPERA SEM PRAZO. Seguir depois de um prazo seria soltar o PHY debaixo
  // do TinyUSB ainda rodando e, logo depois (tab5_sd_return), liberar o
  // cartao cru que a tarefa pode estar usando no meio de um
  // sdmmc_read/write_sectors - memoria liberada em uso e o FAT do Tab5
  // montando por cima de uma escrita do computador. Cada comando do cartao
  // tem o proprio timeout no driver: a tarefa sempre volta.
  while (xSemaphoreTake(s_parou, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGW(TAG, "a tarefa do USB ainda nao parou (%s) - esperando",
             s_no_cartao ? (s_gravando ? "escrita no cartao" : "leitura no cartao") : "USB");
  }
  s_tarefa = nullptr;
  usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);   // ainda com o clock do USB_WRAP
  if (s_phy) { usb_del_phy(s_phy); s_phy = nullptr; }
  phy_para_o_console();
  s_cartao = nullptr;
  s_ligado = false;
  clock_maximo(false);
  vTaskDelay(pdMS_TO_TICKS(50));
  ESP_LOGI(TAG, "desligado: console de volta (%llu KB lidos, %llu KB gravados, %u erros)",
           (unsigned long long)(s_lidos / 1024), (unsigned long long)(s_gravados / 1024),
           (unsigned)s_erros);
}

void tab5_pendrive_usb(tab5_pendrive_usb_t* e)
{
  const int64_t agora = esp_timer_get_time();
  e->ligado   = s_ligado;
  e->montado  = s_ligado && tud_mounted();
  e->suspenso = s_ligado && s_suspenso;
  e->ejetado  = s_ejetado;
  e->ativo    = s_ultimo_io_us && (agora - s_ultimo_io_us) < 2000000;
  e->lidos    = s_lidos;
  e->gravados = s_gravados;
  e->erros    = s_erros;
  e->ms_sem_escrita = s_ultima_escrita_us ? (uint32_t)((agora - s_ultima_escrita_us) / 1000) : UINT32_MAX;
  e->n_config     = s_n_config;
  e->n_inquiry    = s_n_inquiry;
  e->n_capacidade = s_n_capacidade;
  e->n_leituras   = s_n_leituras;
  e->op_recusado  = s_op_recusado;
  e->leitura_max_ms = s_leitura_max_us / 1000;
  e->n_tur        = s_n_tur;
  e->n_sense      = s_n_sense;
  e->n_respostas  = s_n_respostas;
  e->op_respondido = s_op_respondido;
  e->n_soltou     = s_n_soltou;
  e->n_leituras_ok = s_n_leituras_ok;
  e->n_cmd_leitura = s_n_cmd_leitura;
  e->ultimo_setor  = s_ultimo_setor;
  e->parado_ms = s_evento_us ? (uint32_t)((agora - s_evento_us) / 1000) : 0;
  e->onde = !s_evento_us ? PENDRIVE_ESPERA_NADA
          : s_no_cartao  ? PENDRIVE_ESPERA_CARTAO
          : s_lendo      ? PENDRIVE_ESPERA_USB
          :                PENDRIVE_ESPERA_COMPUTADOR;
}

bool tab5_pendrive_pc_presente(void)
{
  // Com o pendrive ligado o JTAG nao ve nada - quem responde e o OTG.
  if (s_ligado) { return tud_connected() && !s_suspenso; }
  return usb_serial_jtag_is_connected();
}

// ===========================================================================
// A SESSAO - emprestar, USB, devolver (ver o .hpp)
// ===========================================================================
static volatile tab5_pendrive_fase_t   s_fase = PENDRIVE_PARADO;
static volatile tab5_pendrive_motivo_t s_motivo = PENDRIVE_OK;
static volatile bool s_pedido_devolver = false;
static volatile bool s_devolvido_por_ejecao = false;
// A ultima sessao terminou com o disco MONTADO no computador, sem ejecao: o
// FAT do cartao ficou marcado como "nao desmontado". Na proxima vez o macOS
// (e o Windows) verifica o cartao inteiro antes de mostrar o disco - pelo USB
// full speed, num cartao grande, isso demora, e nada aparece ate acabar.
static volatile bool s_saiu_sem_ejetar = false;
static volatile bool s_oferta = false;     // computador ligado, esperando a pessoa decidir
static volatile bool s_resultado_aviso = false;   // conflito/pendencia ainda nao visto em Ajustes > USB
static int           s_notif = -1;
static portMUX_TYPE  s_mux_sessao = portMUX_INITIALIZER_UNLOCKED;

static void avisar(int quantidade, notif_nivel_t nivel, const char* txt)
{
  if (s_notif >= 0) { notif_publicar(s_notif, quantidade, nivel, txt); }
}

// Termina a sessao com o motivo dado (o pendrive nao chegou a ligar).
static void recusou(tab5_pendrive_motivo_t m)
{
  s_motivo = m;
  s_fase = PENDRIVE_PARADO;
  avisar(0, NOTIF_INFO, nullptr);
}

// O LOG DA SESSAO: cada passo do computador uma vez (reconheceu, perguntou o
// que e, o tamanho, leu, cada comando respondido ou recusado, erro do cartao,
// ejecao) e um resumo quando algo muda - a cada 3 s ate a primeira leitura
// entregue, a cada 10 s depois. "leit 12/11" = 12 comandos de leitura
// recebidos, 11 com os dados e o status entregues. E, quando tudo para por
// mais de 3 s, ONDE parou (ver tab5_pendrive_espera_t).
typedef struct {
  uint32_t config, inquiry, capacidade, leituras, erros, soltou, brand;
  uint32_t ops_ok[8], ops_recusa[8];
  bool     ejetado;
  bool     suspenso;
  int64_t  resumo_us;
  bool     parada_contada;
} passos_t;

static void registrar_eventos(passos_t* d, const tab5_pendrive_usb_t* u)
{
  if (u->n_config > d->config) {
    ESP_LOGI(TAG, "computador reconheceu o aparelho USB%s", d->config ? " (de novo)" : "");
    d->config = u->n_config;
  }
  if (u->n_inquiry && !d->inquiry) { ESP_LOGI(TAG, "computador perguntou o que e o disco (INQUIRY)"); }
  d->inquiry = u->n_inquiry;
  if (u->n_capacidade && !d->capacidade) {
    ESP_LOGI(TAG, "computador pediu o tamanho: %lu setores de %u",
             s_cartao ? (unsigned long)s_cartao->csd.capacity : 0UL, (unsigned)setor_tam());
  }
  d->capacidade = u->n_capacidade;
  if (u->n_leituras && !d->leituras) { ESP_LOGI(TAG, "computador comecou a ler o disco"); }
  d->leituras = u->n_leituras;
  for (int i = 0; i < 8; ++i) {
    const uint32_t novos_ok = s_ops_respondidos[i] & ~d->ops_ok[i];
    const uint32_t novas_recusas = s_ops_logados[i] & ~d->ops_recusa[i];
    for (int b = 0; b < 32; ++b) {
      if (novos_ok & (1u << b))      { ESP_LOGI(TAG, "resposta ao comando SCSI 0x%02X entregue", i * 32 + b); }
      if (novas_recusas & (1u << b)) { ESP_LOGW(TAG, "comando SCSI 0x%02X nao suportado (recusado)", i * 32 + b); }
    }
    d->ops_ok[i] |= novos_ok;
    d->ops_recusa[i] |= novas_recusas;
  }
  if (u->erros > d->erros) {
    ESP_LOGE(TAG, "cartao falhou %lu vez(es), a ultima no setor %lu: %s", (unsigned long)(u->erros - d->erros),
             (unsigned long)s_erro_setor, esp_err_to_name(s_erro_cod));
    d->erros = u->erros;
  }
  if (u->n_soltou > d->soltou) { ESP_LOGI(TAG, "computador soltou o disco"); d->soltou = u->n_soltou; }
  if (u->suspenso != d->suspenso) {
    // Sem SOF do computador por 3 ms: cabo fora, computador dormindo - ou o
    // computador deixou de ver o Tab5 (ver pad_conferir).
    ESP_LOGI(TAG, "barramento %s", u->suspenso ? "parado (suspenso)" : "de volta");
    d->suspenso = u->suspenso;
  }
  if (u->ejetado != d->ejetado) {
    ESP_LOGI(TAG, "%s pelo computador", u->ejetado ? "ejetado" : "carregado");
    d->ejetado = u->ejetado;
  }

  const uint32_t marca = u->n_config + u->n_inquiry + u->n_tur + u->n_sense + u->n_capacidade
                       + u->n_respostas + u->n_soltou + u->op_recusado + u->n_leituras
                       + u->n_leituras_ok + u->n_cmd_leitura + u->erros + (uint32_t)(u->gravados >> 9);
  const int64_t intervalo = u->n_leituras_ok ? 10000000 : 3000000;
  const int64_t agora = esp_timer_get_time();
  if (marca != d->brand && agora - d->resumo_us > intervalo) {
    d->brand = marca;
    d->resumo_us = agora;
    // Curta de proposito: o log corta a linha em 200 caracteres.
    ESP_LOGI(TAG, "USB: cfg %lu inq %lu pronto %lu sense %lu tam %lu resp %lu (0x%02X) solt %lu"
             " | leit %lu/%lu = %lu KB, grav %lu KB, erros %lu, max %lu/%lu ms (%lu lentas)",
             (unsigned long)u->n_config, (unsigned long)u->n_inquiry, (unsigned long)u->n_tur,
             (unsigned long)u->n_sense, (unsigned long)u->n_capacidade, (unsigned long)u->n_respostas,
             (unsigned)u->op_respondido, (unsigned long)u->n_soltou,
             (unsigned long)u->n_cmd_leitura, (unsigned long)u->n_leituras_ok, (unsigned long)(u->lidos / 1024),
             (unsigned long)(u->gravados / 1024), (unsigned long)u->erros, (unsigned long)u->leitura_max_ms,
             (unsigned long)(s_escrita_max_us / 1000), (unsigned long)s_n_escritas_lentas);
  }

  // PAROU: nada do computador nem do cartao ha mais de 3 s, depois de o
  // computador ter reconhecido o aparelho. Uma linha por parada.
  if (u->n_config && u->parado_ms >= 3000) {
    if (!d->parada_contada) {
      d->parada_contada = true;
      switch (u->onde) {
        case PENDRIVE_ESPERA_CARTAO:
          ESP_LOGW(TAG, "PARADO ha %lu s: preso no CARTAO (%s do setor %lu nao voltou)",
                   (unsigned long)(u->parado_ms / 1000), s_gravando ? "escrita" : "leitura",
                   (unsigned long)u->ultimo_setor);
          break;
        case PENDRIVE_ESPERA_USB:
          ESP_LOGW(TAG, "PARADO ha %lu s: preso no USB - leitura a partir do setor %lu comecou, o cartao "
                   "entregou ate o %lu e o status nao saiu (%lu/%lu comandos)",
                   (unsigned long)(u->parado_ms / 1000), (unsigned long)s_cmd_setor,
                   (unsigned long)u->ultimo_setor, (unsigned long)u->n_leituras_ok, (unsigned long)u->n_cmd_leitura);
          break;
        default: {
          // O Tab5 estava OUVINDO? O endpoint de saida (por onde chega o
          // proximo comando) tem que estar habilitado (EPENA) e sem STALL.
          // Desarmado ou travado = o computador mandou e o Tab5 nao recebeu.
          const uint32_t doepctl1 = *(volatile const uint32_t*)(0x50040000UL + 0xB20);
          const uint32_t diepctl1 = *(volatile const uint32_t*)(0x50040000UL + 0x920);
          ESP_LOGW(TAG, "PARADO ha %lu s: nada novo do COMPUTADOR - tudo entregue (%lu/%lu leituras, "
                   "ultima 0x%02X, %lu KB)",
                   (unsigned long)(u->parado_ms / 1000), (unsigned long)u->n_leituras_ok,
                   (unsigned long)u->n_cmd_leitura, (unsigned)u->op_respondido, (unsigned long)(u->lidos / 1024));
          ESP_LOGW(TAG, "  ouvindo o proximo comando? %s%s (saida %08lX, entrada %08lX%s)",
                   (doepctl1 & (1u << 31)) ? "sim" : "NAO (desarmado)",
                   (doepctl1 & (1u << 21)) ? ", saida TRAVADA (STALL)" : "",
                   (unsigned long)doepctl1, (unsigned long)diepctl1,
                   (diepctl1 & (1u << 21)) ? ", entrada TRAVADA" : "");
          break;
        }
      }
    }
  } else if (u->parado_ms < 3000 && d->parada_contada) {
    d->parada_contada = false;
    ESP_LOGI(TAG, "voltou a andar");
  }
}

// Suspenso por mais que isto = ninguem do outro lado (ver o laco abaixo). O
// barramento suspende sozinho depois de 3 ms sem SOF; um computador que so
// pisca (troca de energia da porta) volta bem antes disto.
#define BARRAMENTO_PARADO_US 2000000
// Ligado e nenhum computador configurou o aparelho ate aqui: nao ha ninguem.
#define ESPERA_PC_US         10000000

static void vTaskSessao(void*)
{
  // 1. O cartao. Quem EXIGE o cartao (Arquivos, Camera) nao pode estar aberto:
  //    o sistema nao tira o cartao de baixo dele (app_t.exige_sd).
  if (sistema_app_exige_sd()) { recusou(PENDRIVE_RECUSA_APP); vTaskDelete(nullptr); return; }
  switch (tab5_sd_lend(3000)) {
    case SD_LENT:             break;
    case SD_LENDING_IN_USE:      recusou(PENDRIVE_RECUSA_EM_USO);     vTaskDelete(nullptr); return;
    case SD_LENDING_NO_JOURNAL:  recusou(PENDRIVE_RECUSA_SEM_DIARIO); vTaskDelete(nullptr); return;
    default:                        recusou(PENDRIVE_RECUSA_SEM_CARTAO); vTaskDelete(nullptr); return;
  }
  // 2. O USB. Antes, o alto-falante sai dos pinos que o ESP-IDF confunde com
  //    os do OTG (ver pad_conferir) - desligado agora, ANTES do USB subir, e
  //    mudo ate o fim. Falhou: o cartao volta na hora, nada mudou para ninguem.
  if (!sistema_som_ceder_pinos(3000)) {
    ESP_LOGW(TAG, "a interface nao calou o alto-falante em 3 s - seguindo (a sentinela do pad cobre)");
  }
  if (!usb_ligar(tab5_sd_raw())) {
    sistema_som_devolver_pinos();
    tab5_sd_return();
    recusou(PENDRIVE_RECUSA_USB);
    vTaskDelete(nullptr);
    return;
  }
  s_fase = PENDRIVE_ATIVO;
  avisar(1, NOTIF_INFO, "Cartão no computador");

  // 3. Enquanto o computador estiver com o cartao. Sai quando ele ejeta (nada
  //    mais em cache do lado de la) ou quando a pessoa manda devolver (a tela
  //    pede confirmacao antes, se ele ainda estiver montado).
  tab5_sd_summary_t r0;
  tab5_sd_summary(&r0);
  bool avisou_cheio = false;
  // O CAMINHO ATE O DISCO APARECER, no log (que vai ao diario e chega ao
  // cartao na volta) - escrito AQUI, nunca nos retornos do USB.
  passos_t d = { };
  int64_t suspenso_desde_us = 0;
  bool    economia = false;
  const int64_t inicio_us = esp_timer_get_time();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    if (pad_conferir()) {
      ESP_LOGE(TAG, "o pad USB do OTG foi desligado por fora (pinos 26/27) - religado (%lu vez(es))",
               (unsigned long)s_n_pad_religado);
    }
    tab5_pendrive_usb_t u;
    tab5_pendrive_usb(&u);
    registrar_eventos(&d, &u);
    // O DIARIO ENCHENDO: passou de 90%, ou ja perdeu gravacao - a pessoa
    // precisa saber para devolver o cartao antes de perder mais.
    tab5_sd_summary_t rr;
    tab5_sd_summary(&rr);
    const bool cheio = rr.lost > r0.lost
                    || (rr.journal_total && (uint64_t)rr.journal_used * 10 >= (uint64_t)rr.journal_total * 9);
    if (cheio != avisou_cheio) {
      avisou_cheio = cheio;
      if (cheio) { avisar(1, NOTIF_ATENCAO, "Memória do pendrive cheia"); }
      else       { avisar(1, NOTIF_INFO, "Cartão no computador"); }
    }
    // BARRAMENTO PARADO ha mais de BARRAMENTO_PARADO_US: cabo puxado sem
    // ejetar, ou computador dormindo. O TinyUSB continua "montado" (suspender
    // nao desconfigura, e sem sensor de VBUS o cabo fora e so isso) - sem
    // esta regra a tela ficava acesa e a CPU no clock maximo PARA SEMPRE, na
    // bateria, esperando um computador que nao esta mais la. A sessao segue
    // (o cache do computador pode ter algo do cartao: so a ejecao ou a pessoa
    // o devolvem); so o que gasta energia sai, e volta quando o barramento
    // voltar.
    // O mesmo para o pendrive ligado SEM computador (pela tela, sem cabo):
    // depois de ESPERA_PC_US sem o computador configurar o aparelho, o clock
    // sai. A enumeracao nao precisa dele; quando o computador configura, o
    // clock sobe nesta volta (200 ms), antes da leitura grossa da FAT.
    const int64_t agora = esp_timer_get_time();
    if (!u.suspenso)             { suspenso_desde_us = 0; }
    else if (!suspenso_desde_us) { suspenso_desde_us = agora; }
    const bool parado = suspenso_desde_us && agora - suspenso_desde_us >= BARRAMENTO_PARADO_US;
    const bool sem_pc = !u.montado && agora - inicio_us >= ESPERA_PC_US;
    const bool economia_agora = parado || sem_pc;
    if (economia_agora != economia) {
      economia = economia_agora;
      ESP_LOGI(TAG, "%s", !economia ? "computador falando: CPU no maximo"
                        : parado    ? "barramento parado: tela e CPU liberadas ate o computador voltar"
                        :             "sem computador configurando o disco: CPU liberada ate ele chegar");
    }
    clock_maximo(!economia);
    // Disco montado: tela acesa - o repouso baixaria o clock no meio da copia,
    // e a pessoa precisa ver o aviso de que o cartao esta fora.
    sistema_manter_acordado(SISTEMA_ACORDADO_PENDRIVE, u.montado && !u.ejetado && !parado);
    if (u.ejetado || s_pedido_devolver) {
      s_devolvido_por_ejecao = u.ejetado;
      s_saiu_sem_ejetar = !u.ejetado && u.montado;
      break;
    }
  }

  // 4. Devolver: o USB sai PRIMEIRO (o computador para de tocar no cartao) e
  //    so entao o cartao volta ao Tab5. O diario e aplicado aqui mesmo (a
  //    barreira de gravacao espera terminar) para o resultado ser o de agora.
  s_fase = PENDRIVE_DEVOLVENDO;
  avisar(1, NOTIF_INFO, "Devolvendo o cartão...");
  usb_desligar();
  sistema_som_devolver_pinos();
  sistema_manter_acordado(SISTEMA_ACORDADO_PENDRIVE, false);
  tab5_sd_return();
  tab5_sd_flush_pending();   // aplica o diario agora (e espera)

  tab5_sd_summary_t r;
  tab5_sd_summary(&r);
  if (r.conflicts) {
    char t[NOTIF_RESUMO];
    snprintf(t, sizeof(t), "%u arquivo(s) em .tab5", (unsigned)r.conflicts);
    avisar((int)r.conflicts, NOTIF_ATENCAO, t);   // fica ate a pessoa ver em Ajustes > USB
    s_resultado_aviso = true;
  } else if (r.in_journal) {
    avisar((int)r.in_journal, NOTIF_ATENCAO, "Gravações esperando cartão");   // cabe em NOTIF_RESUMO
    s_resultado_aviso = true;
  } else {
    avisar(0, NOTIF_INFO, nullptr);
  }
  s_motivo = PENDRIVE_OK;
  s_pedido_devolver = false;
  s_fase = PENDRIVE_PARADO;
  ESP_LOGI(TAG, "pendrive encerrado (%s): %u gravacao(oes) do diario aplicadas, %u conflito(s)",
           s_devolvido_por_ejecao ? "ejetado no computador" : "devolvido no Tab5",
           (unsigned)r.applied, (unsigned)r.conflicts);
  tab5_log_descarregar_logo();
  vTaskDelete(nullptr);
}

bool tab5_pendrive_iniciar(void)
{
  taskENTER_CRITICAL(&s_mux_sessao);
  const bool livre = (s_fase == PENDRIVE_PARADO);
  if (livre) { s_fase = PENDRIVE_PREPARANDO; }
  taskEXIT_CRITICAL(&s_mux_sessao);
  if (!livre) { return false; }

  s_motivo = PENDRIVE_OK;
  s_pedido_devolver = false;
  s_devolvido_por_ejecao = false;
  s_oferta = false;
  s_resultado_aviso = false;
  if (sistema_app_exige_sd()) { recusou(PENDRIVE_RECUSA_APP); return false; }
  avisar(1, NOTIF_INFO, "Preparando o pendrive...");
  // Pilha da tarefa "cartao": emprestar fotografa o cartao (varre as pastas)
  // e o f_mount desce fundo. So existe enquanto durar a sessao.
  if (xTaskCreatePinnedToCore(vTaskSessao, "pendrive_sessao", 6144, nullptr, 3, nullptr, tskNO_AFFINITY) != pdPASS) {
    recusou(PENDRIVE_RECUSA_USB);
    return false;
  }
  return true;
}

// Vale tambem PREPARANDO: o pedido fica guardado e a sessao devolve assim que
// o USB subir (ou nem chega a usar, se recusar antes) - descartar calado
// deixava o cartao no computador depois de a pessoa ter pedido de volta.
void tab5_pendrive_devolver(void)
{
  const tab5_pendrive_fase_t f = s_fase;
  if (f == PENDRIVE_ATIVO || f == PENDRIVE_PREPARANDO) { s_pedido_devolver = true; }
}

void tab5_pendrive_estado(tab5_pendrive_estado_t* e)
{
  tab5_pendrive_usb(&e->usb);
  e->fase = s_fase;
  e->motivo = s_motivo;
  e->por_ejecao = s_devolvido_por_ejecao;
  e->sem_ejetar = s_saiu_sem_ejetar;
  e->oferta = s_oferta;
}

bool tab5_pendrive_pede_atencao(void) { return s_fase != PENDRIVE_PARADO || s_oferta || s_resultado_aviso; }

void tab5_pendrive_resultado_visto(void)
{
  if (!s_resultado_aviso || s_fase != PENDRIVE_PARADO) { return; }
  s_resultado_aviso = false;
  avisar(0, NOTIF_INFO, nullptr);
}

// ---------------------------------------------------------------------------
// A VIGIA: computador ligado no cabo
// ---------------------------------------------------------------------------
// A cada 250 ms (esp_timer). Na BORDA "sem computador -> com computador", com
// o ajuste ligado: na tela inicial, desbloqueado, o pendrive liga sozinho; com
// um app aberto ou a tela bloqueada, so avisa (o aviso abre Ajustes > USB).
// Uma CARENCIA, em que a borda nao conta:
//   - no boot (5 s): o computador ja ligado e quem acabou de gravar o
//     firmware - o console dele reaparece logo depois do reinicio, e roubar o
//     cabo tiraria o monitor serial;
//   - depois de um pendrive (4 s): o console volta ao USB-C e o computador
//     "reaparece" sem o cabo ter saido.
// Antes a regra era "so depois de 3 s com o cabo fora" (olhando uma vez por
// segundo): tirar e recolocar o cabo mais rapido que isso nao fazia nada - e
// o jeito natural de montar de novo depois de ejetar.
// Com o pendrive ligado o JTAG nao ve nada: a vigia nao olha.
#define VIGIA_US          250000
#define VIGIA_LENTA_US    1000000   // ajuste desligado (ver o fim de vigiar)
#define CARENCIA_BOOT_US  5000000
#define CARENCIA_FIM_US   4000000
static esp_timer_handle_t s_vigia = nullptr;
static uint64_t s_vigia_periodo = VIGIA_US;
static bool    s_viu = false, s_antes = false;
static int64_t s_carencia_ate_us = 0;
static int64_t s_ausente_desde_us = 0;

static void vigiar(void*)
{
  const int64_t agora = esp_timer_get_time();
  if (s_fase != PENDRIVE_PARADO || tab5_sd_lent()) {
    s_antes = true;
    s_carencia_ate_us = agora + CARENCIA_FIM_US;   // conta a partir do fim da sessao
    return;
  }

  const bool pc = tab5_pendrive_pc_presente();
  if (!s_viu) { s_viu = true; s_antes = pc; s_carencia_ate_us = agora + CARENCIA_BOOT_US; return; }

  if (s_oferta && !settings_flash_drive()) {   // o ajuste saiu: a oferta tambem
    s_oferta = false;
    avisar(0, NOTIF_INFO, nullptr);
  }
  if (pc) {
    s_ausente_desde_us = 0;
    if (!s_antes && agora >= s_carencia_ate_us && settings_flash_drive()) {
      const bool trancado = sistema_bloqueado() || (tab5_power_is_sleeping() && settings_lock_screen());
      if (!trancado && !sistema_app_atual()) {
        ESP_LOGI(TAG, "computador no USB-C: ligando o pendrive");
        tab5_pendrive_iniciar();
      } else {
        s_oferta = true;
        avisar(1, NOTIF_INFO, "Computador no USB: toque");
      }
    }
  } else {
    if (!s_ausente_desde_us) { s_ausente_desde_us = agora; }
    // A oferta so cai com o cabo fora de verdade, nao num soluco do barramento.
    if (s_oferta && agora - s_ausente_desde_us >= 1000000) { s_oferta = false; avisar(0, NOTIF_INFO, nullptr); }
  }
  s_antes = pc;

  // O RITMO SEGUE O AJUSTE. Com "ligar sozinho" desligado (o de fabrica) a
  // borda nao liga nada - a vigia so acompanha o cabo para nao confundir um
  // computador que JA estava ligado com um que acabou de chegar quando o
  // ajuste for ligado. Para isso 1 s basta; 250 ms so quando a borda vale.
  const uint64_t periodo = (settings_flash_drive() || s_oferta) ? VIGIA_US : VIGIA_LENTA_US;
  if (periodo != s_vigia_periodo) {
    s_vigia_periodo = periodo;
    esp_timer_restart(s_vigia, periodo);
  }
}

void tab5_pendrive_servico_iniciar(void)
{
  if (s_vigia) { return; }
  s_notif = notif_registrar("Pendrive");
  notif_associar_app(s_notif, "Settings");   // o aviso abre Ajustes (na pagina USB)
  const esp_timer_create_args_t a = { .callback = vigiar, .arg = nullptr,
                                      .dispatch_method = ESP_TIMER_TASK, .name = "pendrive_vigia",
                                      .skip_unhandled_events = true };
  if (esp_timer_create(&a, &s_vigia) == ESP_OK) { esp_timer_start_periodic(s_vigia, VIGIA_US); }
}
