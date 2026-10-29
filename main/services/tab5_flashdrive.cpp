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
static volatile bool      s_stop   = false;
static volatile bool      s_enabled  = false;
static volatile bool      s_ejetado = false;
static volatile bool      s_suspenso = false;
static esp_pm_lock_handle_t s_pm = nullptr;

// DIAGNOSTICO POR ETAPA - onde o computador parou (ver tab5_pendrive_usb_t):
// recognized the device? asked what he is? the size? read?
static volatile uint32_t s_n_config = 0;      // SET_CONFIGURATION (tud_mount_cb)
static volatile uint32_t s_n_inquiry = 0;     // INQUIRY
static volatile uint32_t s_n_capacidade = 0;  // READ CAPACITY / READ FORMAT CAPACITIES
static volatile uint32_t s_n_leituras = 0;    // pieces of READ10 read from card (up to 4 KB each)
static volatile uint8_t  s_op_recusado = 0;   // last SCSI command not supported (0 = none)
static volatile uint32_t s_leitura_max_us = 0;// card reading slower
static uint32_t          s_ops_logados[8];    // commands recusados (bit por code)
static volatile uint32_t s_n_tur = 0;         // TEST UNIT READY
static volatile uint32_t s_n_sense = 0;       // REQUEST SENSE (o computador quer saber of one erro)
static volatile uint32_t s_n_respostas = 0;   // commands (not read/write) with status delivered
static volatile uint8_t  s_op_respondido = 0; // the last of them
static volatile uint32_t s_n_soltou = 0;      // unconfigured (reset of bus, cable)
static uint32_t          s_ops_respondidos[8];// commands ja respondidos (bit por code)
static volatile uint32_t s_n_leituras_ok = 0; // commands READ10 com data e status entregues
// ONDE PAROU, when para: os tres estados que one comando of leitura atravessa.
static volatile uint32_t s_n_cmd_leitura = 0; // commands READ10 recebidos
static volatile bool     s_lendo = false;     // one READ10 started and the status is not yet exited
static volatile bool     s_no_cartao = false; // inside of sdmmc_read/write_sectors now
static volatile bool     s_gravando = false;  // ...and and a writing (for the log to say which one)
static volatile uint32_t s_escrita_max_us = 0;// slower card writing
static volatile uint32_t s_n_escritas_lentas = 0; // escritas of mais of 100 ms
static volatile uint32_t s_cmd_setor = 0;     // first setor of READ10 em progress
static volatile uint32_t s_ultimo_setor = 0;  // last piece request on card
static volatile int64_t  s_evento_us = 0;     // last sign of life of computer or card
static volatile uint32_t s_erro_setor = 0;    // the last card failure (to the log)
static volatile int      s_erro_cod = 0;
static volatile uint64_t  s_lidos = 0, s_gravados = 0;
static volatile uint32_t  s_erros = 0;
static volatile int64_t   s_ultimo_io_us = 0, s_ultima_escrita_us = 0;

// ---------------------------------------------------------------------------
// O PHY: of console para o OTG, e of iteration
// ---------------------------------------------------------------------------
// LP_SYS.usb_ctrl.sw_usb_phy_sel (hal/usb_wrap_ll.h):
//   falso - JTAG no PHY 0 (o USB-C), OTG no PHY 1 (o default of chip)
//   verdadeiro - OTG no PHY 0, JTAG no PHY 1
// JTAG is NOT disabled or restarted: the console driver continues
// instalado, com as interrupcoes como estavam, e so deixa of ver o
// computer (for him and one cable pulled - the log that does not exit is discarded,
// as with the device in battery). Hence the console iteration alone.
static void phy_para_o_otg(void)
{
  usb_serial_jtag_ll_phy_enable_pad(false);   // released os resistores of JTAG no PHY 0
  usb_wrap_ll_phy_select(&USB_WRAP, 0);       // OTG -> PHY 0 (USB-C), JTAG -> PHY 1
}

// O pad of OTG ja tem que estar disabled (antes of usb_del_phy, que corta
// the clock of USB_WRAP - clockless peripheral register is not written).
static void phy_para_o_console(void)
{
  usb_serial_jtag_ll_phy_select(0);           // JTAG -> PHY 0 of novo
  usb_serial_jtag_ll_phy_set_defaults();      // e com o pad (pull-up) enabled
  usb_serial_jtag_ll_phy_enable_pad(true);
}

// SENTINELA DO PAD. O ESP-IDF supoe que o PHY dos GPIO 26/27 e sempre of OTG
// (hal/esp32p4/.../gpio_ll.h): configurar qualquer one desses pins - o I2S of
// alto-falante usa os dois - escreve usb_pad_enable = 0 e tira o pull-up of
// D+ no registrador DO OTG, que now esta no USB-C. Para o computador, o cabo
// exited (sem reset nenhum), e o Tab5 seguia esperando o next comando. O
// alto-falante e calado antes of o USB raise (sistema_som_ceder_pinos); isto
// pega qualquer outro que ainda mexer nesses pins: religa e conta.
static volatile uint32_t s_n_pad_religado = 0;
static bool pad_conferir(void)
{
  usb_wrap_otg_conf_reg_t c;
  c.val = USB_WRAP.otg_conf.val;
  if (c.usb_pad_enable && !c.pad_pull_override) { return false; }
  c.usb_pad_enable = 1;
  c.pad_pull_override = 0;   // o pull-up iteration a ser of controlador
  USB_WRAP.otg_conf.val = c.val;
  s_n_pad_religado = s_n_pad_religado + 1;
  return true;
}

// ---------------------------------------------------------------------------
// Descritores
// ---------------------------------------------------------------------------
// VID of Espressif com o PID que o esp_tinyusb usa para "so MSC" (0x4000 com
// the bit of MSC) - and NOT the 0x1001 of JTAG: the computer saves driver for
// VID:PID, e o mesmo par para duas funcoes diferentes confunde o Windows.
static const tusb_desc_device_t DESC_APARELHO = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = 0x00,   // a classe vem in interface (o macOS prefere assim)
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

// O NUMERO DE SERIE segue a especificacao of armazenamento em massa (Bulk-Only
// Transport, 4.1.1): pelo menos 12 digitos hexadecimais. O macOS e o mais
// demanding about it. Leave MAC of your own device: unique and always the same,
// entao o computador reconhece o Tab5 of uma vez para a outra.
static char s_serie[13] = "000000000000";
static const char* const TEXTOS[] = { "", "M5Stack", "M5tabOS SD card", s_serie };

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
// MSC: the card in sectors
// ---------------------------------------------------------------------------
// Tudo aqui roda in tarefa "pendrive" (tud_task), uma chamada por vez - o
// raw card only has this user while it is on loan.
static uint32_t setor_size(void) { return s_cartao ? (uint32_t)s_cartao->csd.sector_size : 512; }

// O QUE O DISCO E, e o tamanho of resposta. O TinyUSB responde o INQUIRY com
// 36 bytes; se o computador pediu mais (ha sistemas que pedem 255), ela manda
// os 36 and LOCKS the data channel (STALL) - the computer needs to unlock it
// (CLEAR_FEATURE) para so entao read o status. E o first comando of disco a
// passar por esse caminho, e o log parava exatamente nele: o computador
// perguntava e nunca chegava on tamanho. A especificacao of transporte (BOT
// 6.7, case 5) leaves the other output: complete with zeros up to the request size.
// The true length goes in the header itself (additional_length), so
// os zeros sao so enchimento. Pedido of 36, resposta of 36 - nada muda.
#define RESPOSTA_CHEIA 255
static void inquiry_count(void) { s_n_inquiry = s_n_inquiry + 1; }

static void inquiry_textos(uint8_t vendor[8], uint8_t produto[16], uint8_t rev[4])
{
  memset(vendor, ' ', 8);   memcpy(vendor, "M5Stack", 7);
  memset(produto, ' ', 16); memcpy(produto, "Tab5 SD card", 12);
  memset(rev, ' ', 4);      memcpy(rev, "1.0", 3);
}

extern "C" uint32_t tud_msc_inquiry2_cb(uint8_t, scsi_inquiry_resp_t* r, uint32_t bufsize)
{
  inquiry_count();
  inquiry_textos(r->vendor_id, r->product_id, r->product_rev);
  const uint32_t n = bufsize < RESPOSTA_CHEIA ? bufsize : RESPOSTA_CHEIA;
  if (n <= sizeof(*r)) { return sizeof(*r); }
  memset((uint8_t*)r + sizeof(*r), 0, n - sizeof(*r));
  return n;   // o TinyUSB corta no que o computador pediu
}

// The old version: is only called if the of top does not exist in the library.
extern "C" void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t produto[16], uint8_t rev[4])
{
  inquiry_count();
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

// O STATUS DE UM COMANDO CHEGOU AO COMPUTADOR (o TinyUSB so chama isto when
// entire answer exited). And what separates "the computer didn't ask" from
// "asked and the answer didn't come".
//
// NO LOG IN HERE, nor in other TinyUSB returns: ESP_LOG goes down
// to the console (USB-Serial-JTAG, without computer during the pendrive - until
// 50 ms of wait), by the locks of stdout and by the stack of task of USB, in
// middle of uma transferencia. Os retornos so contam; quem escreve no log e a
// tarefa of sessao, que olha os contadores (ver registrar_eventos).
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
  // Ejetado: "sem midia" - o computador para of mandar commands on disco.
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
  *bloco    = (uint16_t)setor_size();
}

extern "C" bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool ejetar)
{
  // "Ejetar" no Finder/Explorer/Linux chega aqui como STOP + eject - o sinal
  // of the computer has finished and has nothing left cached to disk.
  if (ejetar) {
    s_ejetado = !start;
  }
  return true;
}

extern "C" bool tud_msc_prevent_allow_medium_removal_cb(uint8_t, uint8_t, uint8_t) { return true; }

extern "C" bool tud_msc_is_writable_cb(uint8_t) { return !s_ejetado; }

extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buf, uint32_t size)
{
  const uint32_t st = setor_size();
  if (!s_cartao || s_ejetado || offset % st || size < st) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return -1;
  }
  const uint32_t n = size / st;
  if (!s_lendo) {   // first pedaco of one comando novo
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
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // erro of leitura
    return -1;
  }
  s_lidos = s_lidos + (uint64_t)n * st;
  s_ultimo_io_us = esp_timer_get_time();
  return (int32_t)(n * st);
}

extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t size)
{
  const uint32_t st = setor_size();
  if (!s_cartao || s_ejetado || offset % st || size < st) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return -1;
  }
  // So setores inteiros: o que sobrar (nunca, com o buffer em multiplo of
  // 512) o TinyUSB entrega of novo in next chamada.
  const uint32_t n = size / st;
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
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);   // erro of escrita
    return -1;
  }
  s_gravados = s_gravados + (uint64_t)n * st;
  const int64_t now = esp_timer_get_time();
  s_ultimo_io_us = now;
  s_ultima_escrita_us = now;
  return (int32_t)(n * st);
}

extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t cmd[16], void* buf, uint16_t size)
{
  switch (cmd[0]) {
    case 0x35:   // SYNCHRONIZE CACHE (10): no cache of side of ca
      return 0;
    case 0x5A: { // MODE SENSE (10). O macOS pergunta por ele (o TinyUSB so traz
                 // o of 6 bytes) para saber se o disco aceita escrita. Cabecalho
                 // of 8 bytes, sem descritor of bloco: tamanho dos data (6), tipo
                 // of midia 0, bit of protecao contra escrita.
      const uint8_t r[8] = { 0, 6, 0, (uint8_t)(s_ejetado ? 0x80 : 0x00), 0, 0, 0, 0 };
      const uint16_t k = size < sizeof(r) ? size : (uint16_t)sizeof(r);
      memcpy(buf, r, k);
      return k;
    }
    default: {
      // Recusado: e registrado (uma vez por comando) - e o first suspeito
      // when one system does not mount the disk.
      const uint8_t op = cmd[0];
      s_op_recusado = op;
      s_ops_logados[op >> 5] |= 1u << (op & 31);
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      return -1;
    }
  }
}

// "Mount" on TinyUSB and the computer CONFIGURE the device (enumeration done) -
// is not yet the open disk: that comes later, with INQUIRY, size and readings.
extern "C" void tud_mount_cb(void)
{
  s_suspenso = false;
  s_n_config = s_n_config + 1;
  s_lendo = false;
  s_evento_us = esp_timer_get_time();
}

// NOT ejection. Unconfiguring also happens in a reset of recovery of
// computer in the middle of a copy - return the card then it would be removing the disk
// of bottom dele. Sem montagem, a screen so deixa of request confirmacao para exit.
extern "C" void tud_umount_cb(void)
{
  s_n_soltou = s_n_soltou + 1;
  s_lendo = false;   // reset of bus: the command in progress died with it
}
extern "C" void tud_suspend_cb(bool)  { s_suspenso = true; }
extern "C" void tud_resume_cb(void)   { s_suspenso = false; }

// ---------------------------------------------------------------------------
// A tarefa e o ciclo of vida
// ---------------------------------------------------------------------------
// TINYUSB IS BORN AND DIES WITHIN ITS OWN TASK, fixed in a nucleus. O
// tusb_init allocates the USB interrupt on the caller's core; to
// library protects the state of transfers with a lock that, in the same
// core, turns off interrupts - but the handling of interrupt (write
// and read the FIFO, finish transfers) does not take this lock. With the
// interruption in one nucleus and the task migrating to the other, both change the
// same endpoint on the same time, and the transfer hangs without any error -
// each time at a different point. Before tusb_init ran in task of
// session (no fixed core) and this task also had no core. And what does the
// esp_tinyusb of Espressif does (tinyusb_task.c): init in task, fixed task.
// Core 1, the same as esp_tinyusb uses by default (0 stays with WiFi).
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
    while (!s_stop) {
      tud_task_ext(20, false);   // wakes up every 20 ms to see the request of stop
    }
    tusb_deinit(0);              // in the same nucleus where the interruption was born
  }
  tab5_owner_forget(xTaskGetCurrentTaskHandle());
  xSemaphoreGive(s_parou);
  vTaskDelete(nullptr);
}

// O controlador tem DMA? (GHWCFG2.OtgArch = 2). Sem ele o TinyUSB cai no modo
// slave - just for the log to tell which of the two is in use.
static const char* usb_modo(void)
{
  const uint32_t ghwcfg2 = *(volatile const uint32_t*)(0x50040000UL + 0x48);
  return ((ghwcfg2 >> 3) & 3) == 2 ? "DMA" : "escravo";
}

// CLOCK AT MAXIMUM while the computer is talking: with the clock
// variable, each pause between packets drops the CPU to the floor and the next
// request of computer waits for her to raise. With the bus STOPPED (cable pulled
// without ejecting, computer sleeping) the exit lock - see the session loop: then the
// Tab5 is on battery and there is no request to respond quickly.
static bool s_clock_max = false;
static void clock_maximum(bool sim)
{
  if (sim == s_clock_max) { return; }
  if (!s_pm && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pendrive", &s_pm) != ESP_OK) { s_pm = nullptr; }
  if (!s_pm) { return; }
  if (sim) { esp_pm_lock_acquire(s_pm); } else { esp_pm_lock_release(s_pm); }
  s_clock_max = sim;
}

static bool usb_enable(sdmmc_card_t* cartao)
{
  if (s_enabled) { return true; }
  if (!cartao) { return false; }
  if (!s_parou) { s_parou = xSemaphoreCreateBinary(); }
  if (!s_iniciou) { s_iniciou = xSemaphoreCreateBinary(); }
  if (!s_parou || !s_iniciou) { return false; }

  s_cartao = cartao;
  s_ejetado = false; s_suspenso = false; s_stop = false;
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

  ESP_LOGI(TAG, "starting: console leaves USB-C until USB-drive mode ends");
  vTaskDelay(pdMS_TO_TICKS(50));   // deixa a line acima chegar on computador

  phy_para_o_otg();
  static const usb_phy_otg_io_conf_t io = USB_PHY_SELF_POWERED_DEVICE(-1);
  usb_phy_config_t cfg = { };
  cfg.controller  = USB_PHY_CTRL_OTG;
  cfg.target      = USB_PHY_TARGET_INT;      // o PHY full speed (now o 0)
  cfg.otg_mode    = USB_OTG_MODE_DEVICE;
  cfg.otg_speed   = USB_PHY_SPEED_FULL;      // sem isto o IDF troca para o PHY of alta velocidade
  cfg.otg_io_conf = &io;
  if (usb_new_phy(&cfg, &s_phy) != ESP_OK) {
    ESP_LOGE(TAG, "PHY do OTG recusado");
    s_phy = nullptr;
    phy_para_o_console();
    s_cartao = nullptr;
    return false;
  }

  // HIGH PRIORITY, above interface: on mount, the computer reads the table
  // FAT inteira of uma vez, e o macOS desiste (erro of E/S, o disco nem
  // appears) if she takes too long. An iteration of drawing of screen in front of each
  // request era atraso puro. A tarefa quase sempre dorme esperando o USB.
  // The stack is the TinyUSB itself plus the press_start until the card driver.
  xSemaphoreTake(s_iniciou, 0);   // sem sobra of uma vez anterior
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
    ESP_LOGE(TAG, "TinyUSB did not start");
    xSemaphoreTake(s_parou, portMAX_DELAY);   // a tarefa ja esta saindo
    s_tarefa = nullptr;
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
    usb_del_phy(s_phy); s_phy = nullptr;
    phy_para_o_console();
    s_cartao = nullptr;
    return false;
  }
  tab5_owner_register(s_tarefa, OWNER_SERVICE, "Pendrive");
  clock_maximum(true);
  tud_connect();
  s_enabled = true;
  ESP_LOGI(TAG, "USB no ar: modo %s, nucleo %d", usb_modo(), PENDRIVE_NUCLEO);
  return true;
}

static void usb_disable(void)
{
  if (!s_enabled) { return; }
  tud_disconnect();                       // o computador ve o disco exit
  vTaskDelay(pdMS_TO_TICKS(30));
  s_stop = true;
  // A tarefa termina o comando em progress (no maximum uma leitura/escrita of
  // 4 KB), turn off TinyUSB and exit; only after that the card will be
  // touched pelo USB.
  //
  // ESPERA SEM PRAZO. Seguir depois of one prazo seria release o PHY debaixo
  // of TinyUSB ainda running e, logo depois (tab5_sd_return), liberar o
  // raw card that the task may be using in the middle of one
  // sdmmc_read/write_sectors - memoria liberada em uso e o FAT of Tab5
  // assembling on top of a computer writing. Each command of card
  // has its own timeout in the driver: the task always iteration.
  while (xSemaphoreTake(s_parou, pdMS_TO_TICKS(2000)) != pdTRUE) {
  ESP_LOGW(TAG, "the USB task has not stopped yet (%s) - waiting",
             s_no_cartao ? (s_gravando ? "escrita no cartao" : "leitura no cartao") : "USB");
  }
  s_tarefa = nullptr;
  usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);   // ainda com o clock of USB_WRAP
  if (s_phy) { usb_del_phy(s_phy); s_phy = nullptr; }
  phy_para_o_console();
  s_cartao = nullptr;
  s_enabled = false;
  clock_maximum(false);
  vTaskDelay(pdMS_TO_TICKS(50));
  ESP_LOGI(TAG, "desligado: console de volta (%llu KB lidos, %llu KB gravados, %u erros)",
           (unsigned long long)(s_lidos / 1024), (unsigned long long)(s_gravados / 1024),
           (unsigned)s_erros);
}

void tab5_pendrive_usb(tab5_pendrive_usb_t* e)
{
  const int64_t now = esp_timer_get_time();
  e->enabled   = s_enabled;
  e->montado  = s_enabled && tud_mounted();
  e->suspenso = s_enabled && s_suspenso;
  e->ejetado  = s_ejetado;
  e->active    = s_ultimo_io_us && (now - s_ultimo_io_us) < 2000000;
  e->lidos    = s_lidos;
  e->gravados = s_gravados;
  e->erros    = s_erros;
  e->ms_sem_escrita = s_ultima_escrita_us ? (uint32_t)((now - s_ultima_escrita_us) / 1000) : UINT32_MAX;
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
  e->parado_ms = s_evento_us ? (uint32_t)((now - s_evento_us) / 1000) : 0;
  e->onde = !s_evento_us ? PENDRIVE_ESPERA_NADA
          : s_no_cartao  ? PENDRIVE_ESPERA_CARTAO
          : s_lendo      ? PENDRIVE_ESPERA_USB
          :                PENDRIVE_ESPERA_COMPUTADOR;
}

bool tab5_pendrive_pc_presente(void)
{
  // With the pendrive enabled, JTAG doesn't see anything - OTG is the one who responds.
  if (s_enabled) { return tud_connected() && !s_suspenso; }
  return usb_serial_jtag_is_connected();
}

// ===========================================================================
// A SESSAO - emprestar, USB, return (ver o .hpp)
// ===========================================================================
static volatile tab5_pendrive_fase_t   s_fase = PENDRIVE_PARADO;
static volatile tab5_pendrive_reason_t s_reason = PENDRIVE_OK;
static volatile bool s_request_return = false;
static volatile bool s_devolvido_por_ejecao = false;
// A ultima sessao terminou com o disco MONTADO no computador, sem ejecao: o
// FAT of card became marked as "not disassembled". Next time macOS
// (and Windows) checks the entire card before showing the disk - over USB
// full speed, on a large card, this takes time, and nothing appears until it's over.
static volatile bool s_exited_sem_ejetar = false;
static volatile bool s_oferta = false;     // computador enabled, esperando a pessoa decidir
static volatile bool s_resultado_aviso = false;   // conflict/pending not yet seen in Settings > USB
static int           s_notif = -1;
static portMUX_TYPE  s_mux_sessao = portMUX_INITIALIZER_UNLOCKED;

static void notify(int count, notification_level_t level, const char* txt)
{
  if (s_notif >= 0) { notification_publicar(s_notif, count, level, txt); }
}

// Ends the session with the given reason (the pendrive did not reach enable).
static void recusou(tab5_pendrive_reason_t m)
{
  s_reason = m;
  s_fase = PENDRIVE_PARADO;
  notify(0, NOTIF_INFO, nullptr);
}

// THE SESSION LOG: each computer step once (acknowledged, asked the
// which is, the size, read, each command responded or refused, error of card,
// ejection) and one summary when something changes - every 3 s until the first reading
// delivered, every 10 s thereafter. "leit 12/11" = 12 commands of reading
// recebidos, 11 com os data e o status entregues. E, when everything para por
// mais of 3 s, ONDE parou (ver tab5_pendrive_espera_t).
typedef struct {
  uint32_t config, inquiry, capacidade, leituras, erros, soltou, brand;
  uint32_t ops_ok[8], ops_recusa[8];
  bool     ejetado;
  bool     suspenso;
  int64_t  summary_us;
  bool     stop_contada;
} passos_t;

static void registrar_eventos(passos_t* d, const tab5_pendrive_usb_t* u)
{
  if (u->n_config > d->config) {
  ESP_LOGI(TAG, "computer recognized the USB device%s", d->config ? " (again)" : "");
    d->config = u->n_config;
  }
  if (u->n_inquiry && !d->inquiry) { ESP_LOGI(TAG, "computer asked what the disk is (INQUIRY)"); }
  d->inquiry = u->n_inquiry;
  if (u->n_capacidade && !d->capacidade) {
    ESP_LOGI(TAG, "computer requested the size: %lu sectors of %u",
             s_cartao ? (unsigned long)s_cartao->csd.capacity : 0UL, (unsigned)setor_size());
  }
  d->capacidade = u->n_capacidade;
  if (u->n_leituras && !d->leituras) { ESP_LOGI(TAG, "computer started reading the disk"); }
  d->leituras = u->n_leituras;
  for (int i = 0; i < 8; ++i) {
    const uint32_t novos_ok = s_ops_respondidos[i] & ~d->ops_ok[i];
    const uint32_t novas_recusas = s_ops_logados[i] & ~d->ops_recusa[i];
    for (int b = 0; b < 32; ++b) {
      if (novos_ok & (1u << b))      { ESP_LOGI(TAG, "resposta ao comando SCSI 0x%02X entregue", i * 32 + b); }
      if (novas_recusas & (1u << b)) { ESP_LOGW(TAG, "SCSI command 0x%02X unsupported (rejected)", i * 32 + b); }
    }
    d->ops_ok[i] |= novos_ok;
    d->ops_recusa[i] |= novas_recusas;
  }
  if (u->erros > d->erros) {
    ESP_LOGE(TAG, "cartao falhou %lu vez(es), a ultima no setor %lu: %s", (unsigned long)(u->erros - d->erros),
             (unsigned long)s_erro_setor, esp_err_to_name(s_erro_cod));
    d->erros = u->erros;
  }
  if (u->n_soltou > d->soltou) { ESP_LOGI(TAG, "computer released the disk"); d->soltou = u->n_soltou; }
  if (u->suspenso != d->suspenso) {
    // No SOF of computer for 3 ms: cable out, computer sleeping - or the
    // computador deixou of ver o Tab5 (ver pad_conferir).
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
  const int64_t now = esp_timer_get_time();
  if (marca != d->brand && now - d->summary_us > intervalo) {
    d->brand = marca;
    d->summary_us = now;
    // Curta of proposito: o log corta a line em 200 caracteres.
    ESP_LOGI(TAG, "USB: cfg %lu inq %lu pronto %lu sense %lu tam %lu resp %lu (0x%02X) solt %lu"
             " | leit %lu/%lu = %lu KB, grav %lu KB, erros %lu, max %lu/%lu ms (%lu lentas)",
             (unsigned long)u->n_config, (unsigned long)u->n_inquiry, (unsigned long)u->n_tur,
             (unsigned long)u->n_sense, (unsigned long)u->n_capacidade, (unsigned long)u->n_respostas,
             (unsigned)u->op_respondido, (unsigned long)u->n_soltou,
             (unsigned long)u->n_cmd_leitura, (unsigned long)u->n_leituras_ok, (unsigned long)(u->lidos / 1024),
             (unsigned long)(u->gravados / 1024), (unsigned long)u->erros, (unsigned long)u->leitura_max_ms,
             (unsigned long)(s_escrita_max_us / 1000), (unsigned long)s_n_escritas_lentas);
  }

  // STOPPED: nothing on the computer or card for more than 3 s, after the
  // computer has recognized the device. One line per stop.
  if (u->n_config && u->parado_ms >= 3000) {
    if (!d->stop_contada) {
      d->stop_contada = true;
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
          // O Tab5 estava OUVINDO? O endpoint of saida (por onde chega o
          // next comando) tem que estar habilitado (EPENA) e sem STALL.
          // Disarmed or locked = the computer sent it and the Tab5 did not receive it.
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
  } else if (u->parado_ms < 3000 && d->stop_contada) {
    d->stop_contada = false;
    ESP_LOGI(TAG, "voltou a andar");
  }
}

// Suspenso por mais que isto = ninguem of outro lado (ver o laco abaixo). O
// bus suspends alone after 3 ms without SOF; one computer that only
// pisca (troca of energia of porta) iteration bem antes disto.
#define BARRAMENTO_PARADO_US 2000000
// Connected and no computer has configured the device until now: no one.
#define ESPERA_PC_US         10000000

static void vTaskSessao(void*)
{
  // 1. The card. Anyone who REQUIRES the card (Files, Camera) cannot be open:
  // the system does not remove his bottom card (app_t.exige_sd).
  if (system_app_requires_sd()) { recusou(PENDRIVE_RECUSA_APP); vTaskDelete(nullptr); return; }
  switch (tab5_sd_lend(3000)) {
    case SD_LENT:             break;
    case SD_LENDING_IN_USE:      recusou(PENDRIVE_RECUSA_EM_USO);     vTaskDelete(nullptr); return;
    case SD_LENDING_NO_JOURNAL:  recusou(PENDRIVE_RECUSA_SEM_DIARIO); vTaskDelete(nullptr); return;
    default:                        recusou(PENDRIVE_RECUSA_SEM_CARTAO); vTaskDelete(nullptr); return;
  }
  // 2. O USB. Antes, o alto-falante exit dos pins que o ESP-IDF confunde com
  //    os of OTG (ver pad_conferir) - disabled now, ANTES of USB raise, e
  // change to the end. Failed: the card iteration in time, nothing changed for anyone.
  if (!system_sound_yield_pins(3000)) {
    ESP_LOGW(TAG, "a interface nao calou o alto-falante em 3 s - seguindo (a sentinela do pad cobre)");
  }
  if (!usb_enable(tab5_sd_raw())) {
    system_sound_return_pins();
    tab5_sd_return();
    recusou(PENDRIVE_RECUSA_USB);
    vTaskDelete(nullptr);
    return;
  }
  s_fase = PENDRIVE_ATIVO;
  notify(1, NOTIF_INFO, "Card connected to computer");

  // 3. While the computer has the card. It comes out when it ejects (nothing
  //    mais em cache of lado of la) ou when a pessoa manda return (a screen
  //    pede confirmacao antes, se ele ainda estiver montado).
  tab5_sd_summary_t r0;
  tab5_sd_summary(&r0);
  bool avisou_cheio = false;
  // THE PATH UNTIL THE DISK APPEARS, in the log (which goes to the diary and arrives at
  // card in iteration) - written HERE, never returns of USB.
  passos_t d = { };
  int64_t suspenso_since_us = 0;
  bool    economia = false;
  const int64_t start_us = esp_timer_get_time();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    if (pad_conferir()) {
      ESP_LOGE(TAG, "o pad USB do OTG foi desligado por fora (pinos 26/27) - religado (%lu vez(es))",
               (unsigned long)s_n_pad_religado);
    }
    tab5_pendrive_usb_t u;
    tab5_pendrive_usb(&u);
    registrar_eventos(&d, &u);
    // THE DIARY IS FILLING UP: it's over 90%, or you've already lost the recording - the person
    // needs to know how to return the card before losing more.
    tab5_sd_summary_t rr;
    tab5_sd_summary(&rr);
    const bool cheio = rr.lost > r0.lost
                    || (rr.journal_total && (uint64_t)rr.journal_used * 10 >= (uint64_t)rr.journal_total * 9);
    if (cheio != avisou_cheio) {
      avisou_cheio = cheio;
      if (cheio) { notify(1, NOTIF_ATENCAO, "USB-drive storage full"); }
      else       { notify(1, NOTIF_INFO, "Card connected to computer"); }
    }
    // BUS STOPPED there is more than BARRRAMENTO_PARADO_US: cable pulled without
    // ejetar, ou computador dormindo. O TinyUSB continua "montado" (suspender
    // does not disconfigure, and without VBUS sensor the cable is out and that's it) - without
    // esta regra a screen ficava acesa e a CPU no clock maximum PARA SEMPRE, in
    // battery, waiting for a computer that is no longer there. The session continues
    // (the computer's cache may have something of card: just the ejection or the person
    // return it); only what uses energy exit, and iteration when the bus
    // voltar.
    // O mesmo para o pendrive enabled SEM computador (pela screen, sem cabo):
    // after ESPERA_PC_US without the computer configuring the device, the clock
    // exit. The enumeration does not need it; when the computer configures, the
    // clock sobe nesta iteration (200 ms), antes of leitura grossa of FAT.
    const int64_t now = esp_timer_get_time();
    if (!u.suspenso)             { suspenso_since_us = 0; }
    else if (!suspenso_since_us) { suspenso_since_us = now; }
    const bool parado = suspenso_since_us && now - suspenso_since_us >= BARRAMENTO_PARADO_US;
    const bool sem_pc = !u.montado && now - start_us >= ESPERA_PC_US;
    const bool economia_now = parado || sem_pc;
    if (economia_now != economia) {
      economia = economia_now;
      ESP_LOGI(TAG, "%s", !economia ? "computador falando: CPU no maximo"
                        : parado    ? "barramento parado: tela e CPU liberadas ate o computador voltar"
                        :             "sem computador configurando o disco: CPU liberada ate ele chegar");
    }
    clock_maximum(!economia);
    // Disco montado: screen acesa - o sleep baixaria o clock no middle of copia,
    // and the person needs to see the warning that the card is out.
    system_manter_acordado(SISTEMA_ACORDADO_PENDRIVE, u.montado && !u.ejetado && !parado);
    if (u.ejetado || s_request_return) {
      s_devolvido_por_ejecao = u.ejetado;
      s_exited_sem_ejetar = !u.ejetado && u.montado;
      break;
    }
  }

  // 4. Return: the USB exit FIRST (the computer stops touching the card) and
  // only then the card iteration on Tab5. The diary is applied right here (the
  // barrier of recording waits to finish) for the result to be of now.
  s_fase = PENDRIVE_DEVOLVENDO;
  notify(1, NOTIF_INFO, "Returning the card...");
  usb_disable();
  system_sound_return_pins();
  system_manter_acordado(SISTEMA_ACORDADO_PENDRIVE, false);
  tab5_sd_return();
  tab5_sd_flush_pending();   // aplica o diario now (e espera)

  tab5_sd_summary_t r;
  tab5_sd_summary(&r);
  if (r.conflicts) {
    char t[NOTIF_RESUMO];
    snprintf(t, sizeof(t), "%u arquivo(s) em .tab5", (unsigned)r.conflicts);
    notify((int)r.conflicts, NOTIF_ATENCAO, t);   // stays until the person sees it in Settings > USB
    s_resultado_aviso = true;
  } else if (r.in_journal) {
    notify((int)r.in_journal, NOTIF_ATENCAO, "Writes waiting for card");   // fits in NOTIF_RESUMO
    s_resultado_aviso = true;
  } else {
    notify(0, NOTIF_INFO, nullptr);
  }
  s_reason = PENDRIVE_OK;
  s_request_return = false;
  s_fase = PENDRIVE_PARADO;
  ESP_LOGI(TAG, "pendrive encerrado (%s): %u gravacao(oes) do diario aplicadas, %u conflito(s)",
           s_devolvido_por_ejecao ? "ejetado no computador" : "devolvido no Tab5",
           (unsigned)r.applied, (unsigned)r.conflicts);
  tab5_log_descarregar_logo();
  vTaskDelete(nullptr);
}

bool tab5_pendrive_start(void)
{
  taskENTER_CRITICAL(&s_mux_sessao);
  const bool livre = (s_fase == PENDRIVE_PARADO);
  if (livre) { s_fase = PENDRIVE_PREPARANDO; }
  taskEXIT_CRITICAL(&s_mux_sessao);
  if (!livre) { return false; }

  s_reason = PENDRIVE_OK;
  s_request_return = false;
  s_devolvido_por_ejecao = false;
  s_oferta = false;
  s_resultado_aviso = false;
  if (system_app_requires_sd()) { recusou(PENDRIVE_RECUSA_APP); return false; }
  notify(1, NOTIF_INFO, "Preparing the USB drive...");
  // Stack of task "card": borrow photos of card (scans folders)
  // e o f_mount desce fundo. So existe enquanto durar a sessao.
  if (xTaskCreatePinnedToCore(vTaskSessao, "pendrive_sessao", 6144, nullptr, 3, nullptr, tskNO_AFFINITY) != pdPASS) {
    recusou(PENDRIVE_RECUSA_USB);
    return false;
  }
  return true;
}

// It is also worth PREPARING: the request stays saved and the session returns as soon as
// o USB raise (ou nem chega a usar, se recusar antes) - descartar calado
// left the card on the computer after the person had requested iteration.
void tab5_pendrive_return(void)
{
  const tab5_pendrive_fase_t f = s_fase;
  if (f == PENDRIVE_ATIVO || f == PENDRIVE_PREPARANDO) { s_request_return = true; }
}

void tab5_pendrive_estado(tab5_pendrive_estado_t* e)
{
  tab5_pendrive_usb(&e->usb);
  e->fase = s_fase;
  e->reason = s_reason;
  e->por_ejecao = s_devolvido_por_ejecao;
  e->sem_ejetar = s_exited_sem_ejetar;
  e->oferta = s_oferta;
}

bool tab5_pendrive_pede_atencao(void) { return s_fase != PENDRIVE_PARADO || s_oferta || s_resultado_aviso; }

void tab5_pendrive_resultado_visto(void)
{
  if (!s_resultado_aviso || s_fase != PENDRIVE_PARADO) { return; }
  s_resultado_aviso = false;
  notify(0, NOTIF_INFO, nullptr);
}

// ---------------------------------------------------------------------------
// THE WATCH: computer enabled on cable
// ---------------------------------------------------------------------------
// Every 250 ms (esp_timer). On the EDGE "without computer -> with computer", with
// the setting enabled: in initial screen, unlocked, the pendrive turns on by itself; with
// one app open ou a screen bloqueada, so avisa (o aviso abre Ajustes > USB).
// A LACK, in which the edge does not count:
//   - no boot (5 s): o computador ja enabled e quem acabou of gravar o
//     firmware - o console dele reaparece logo depois of reinicio, e roubar o
//     cabo tiraria o monitor serial;
//   - depois of one pendrive (4 s): o console iteration on USB-C e o computador
//     "reaparece" sem o cabo ter saido.
// Before the rule was "only after 3 s with the cable out" (looking once a
// second): removing and replacing the cable faster than that did nothing - and
// the natural way to mount again after ejecting.
// With the pendrive enabled, JTAG doesn't see anything: the watchman doesn't look.
#define VIGIA_US          250000
#define VIGIA_LENTA_US    1000000   // ajuste disabled (ver o end of vigiar)
#define CARENCIA_BOOT_US  5000000
#define CARENCIA_FIM_US   4000000
static esp_timer_handle_t s_vigia = nullptr;
static uint64_t s_vigia_periodo = VIGIA_US;
static bool    s_viu = false, s_antes = false;
static int64_t s_carencia_ate_us = 0;
static int64_t s_ausente_since_us = 0;

static void vigiar(void*)
{
  const int64_t now = esp_timer_get_time();
  if (s_fase != PENDRIVE_PARADO || tab5_sd_lent()) {
    s_antes = true;
    s_carencia_ate_us = now + CARENCIA_FIM_US;   // conta a partir of end of sessao
    return;
  }

  const bool pc = tab5_pendrive_pc_presente();
  if (!s_viu) { s_viu = true; s_antes = pc; s_carencia_ate_us = now + CARENCIA_BOOT_US; return; }

  if (s_oferta && !settings_flash_drive()) {   // the setting exited: the offer also
    s_oferta = false;
    notify(0, NOTIF_INFO, nullptr);
  }
  if (pc) {
    s_ausente_since_us = 0;
    if (!s_antes && now >= s_carencia_ate_us && settings_flash_drive()) {
      const bool trancado = system_locked() || (tab5_power_is_sleeping() && settings_lock_screen());
      if (!trancado && !system_app_current()) {
        ESP_LOGI(TAG, "computador no USB-C: ligando o pendrive");
        tab5_pendrive_start();
      } else {
        s_oferta = true;
        notify(1, NOTIF_INFO, "Computador no USB: toque");
      }
    }
  } else {
    if (!s_ausente_since_us) { s_ausente_since_us = now; }
    // The offer only falls with the cable out of truth, not in a bus solution.
    if (s_oferta && now - s_ausente_since_us >= 1000000) { s_oferta = false; notify(0, NOTIF_INFO, nullptr); }
  }
  s_antes = pc;

  // THE RHYTHM FOLLOWS THE SETTING. With "enable alone" disabled (of factory)
  // edge doesn't turn on anything - the port only follows the cable so as not to confuse one
  // computador que JA estava enabled com one que acabou of chegar when o
  // ajuste for enabled. Para isso 1 s basta; 250 ms so when a borda vale.
  const uint64_t periodo = (settings_flash_drive() || s_oferta) ? VIGIA_US : VIGIA_LENTA_US;
  if (periodo != s_vigia_periodo) {
    s_vigia_periodo = periodo;
    esp_timer_restart(s_vigia, periodo);
  }
}

void tab5_pendrive_servico_start(void)
{
  if (s_vigia) { return; }
  s_notif = notification_registrar("Pendrive");
  notification_associar_app(s_notif, "Settings");   // o aviso abre Ajustes (in pagina USB)
  const esp_timer_create_args_t a = { .callback = vigiar, .arg = nullptr,
                                      .dispatch_method = ESP_TIMER_TASK, .name = "pendrive_vigia",
                                      .skip_unhandled_events = true };
  if (esp_timer_create(&a, &s_vigia) == ESP_OK) { esp_timer_start_periodic(s_vigia, VIGIA_US); }
}
