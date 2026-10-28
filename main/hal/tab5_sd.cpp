#include "tab5_sd.hpp"
#include "tab5_marks.hpp"
#include "tab5_journal.hpp"
#include <errno.h>

#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"   // ff_diskio_get_pdrv_card - o drive of FatFs, para o rotulo
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

static const char *TAG = "sd";

// ---- fiacao (M5Unified, _pin_table_sd, line of board_M5Tab5) --------------
static const int SD_CLK = 43;
static const int SD_CMD = 44;
static const int SD_D0  = 39;
static const int SD_D1  = 40;
static const int SD_D2  = 41;
static const int SD_D3  = 42;

// Canal of LDO interno que alimenta a IO of SDMMC.
//
// O P4 declara SOC_SDMMC_IO_POWER_EXTERNAL: a alimentacao of IO of cartao NAO
// vem of one trilho fixo, ela exit of one LDO of dentro of chip que precisa ser
// on antes. O channel 4 (LDO_VO4) e o que as placas of referencia of P4 usam;
// o M5GFX ja usa o channel 3 para o PHY of DSI, o que reforca que o 4 esta free_space
// para isto. Se a montagem falhar com erro of timeout, este numero e o first
// suspeito.
static const int SD_LDO_CANAL = 4;

// ---- state ----------------------------------------------------------------
static sdmmc_card_t*       s_card = nullptr;
static sd_pwr_ctrl_handle_t s_ldo = nullptr;
static int                 s_refs = 0;

// A contagem of referencia e mexida por tarefas diferentes: a interface (on
// open e close app) e o registro of energia (a cada descarga). Sem trava, dois
// mount() simultaneos poderiam mount duas vezes, ou one unmount() puxar o
// tapete of who esta gravando. Mutex criado sob demanda, in primeira chamada.
static SemaphoreHandle_t   s_mutex = nullptr;

// O mutex NAO pode ser criado sob demanda dentro of trava(): duas tarefas
// chegando juntas in primeira vez criariam dois mutexes diferentes e cada uma
// travaria o seu - protecao none, e ainda vaza one. Criado uma vez em
// tab5_sd_init(), antes of existir concorrencia.
static void trava(void)
{
  if (s_mutex) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
}
static void destrava(void)
{
  if (s_mutex) { xSemaphoreGive(s_mutex); }
}
// ---------------------------------------------------------------------------
// O ESTADO DO CARTAO SAO QUATRO COISAS, NAO DUAS
// ---------------------------------------------------------------------------
// A description dizia "sem cartao" toda vez que o cartao era DESMONTADO - que e
// o state normal when nenhum app o esta usando, ja que a montagem e sob
// demanda e contada por referencia. O comando `servicos` entao anunciava "sem
// cartao" com o cartao encaixado e funcionando.
//
// Misturar "nao esta mounted now" com "nao existe" e o mesmo erro of WiFi
// dizendo "failed" when o coprocessador estava mudo: duas causas diferentes,
// uma message so, e o user mandado investigar o lugar errado.
//
//   nao verificado      - ninguem tentou mount ainda neste boot
//   SD 14.5GB           - mounted now
//   SD 14.5GB (free_space)   - ja montou, esta desmontado, o cartao continua la
//   ausente ou ilegivel - a latest tentativa of mount failed
static char s_ident[32]      = "";      // identidade, when ja a conhecemos
static bool s_ja_montou      = false;   // alguma montagem deu certo neste boot
// O NOME DO CARTAO (rotulo of volume FAT), value_read a cada montagem of verdade:
// renomeado no computador durante o pendrive, a iteration remonta e le of novo.
// Fica valendo com o cartao desmontado. Vazio = sem name.
static char s_rotulo[36]     = "";
static void arrumar_sistema(void);
static bool s_anunciou = false;   // a primeira montagem of boot ja foi para o log

// DESMONTAGEM ADIADA. Quando o ultimo user devolve o cartao,
// ele stays mounted mais SD_OCIOSO_MS: o log, o monitor, o registro of energia
// e a track gravam em momentos proximos, e cada montagem liga o regulador e
// reinicializa o cartao. Com a folga, gravacoes vizinhas dividem UMA montagem;
// sem ninguem request of novo, desmonta e o regulador desliga. E o mesmo default
// of "timeout of ociosidade" of high-falante (ARCHITECTURE.md, 1.1).
static const uint64_t   SD_OCIOSO_US = 10ULL * 1000 * 1000;
static esp_timer_handle_t s_ocioso = nullptr;
static void desmontar_of_verdade(void);

// Quando o cartao montou/desmontou DE VERDADE pela latest vez (us of uptime).
// Diagnostico: o monitor cruza com o frame atrasado of panel (o cartao e
// alimentado por one LDO of proprio P4, vizinho of LDO of DSI).
static volatile int64_t s_montou_us = 0, s_desmontou_us = 0;
void tab5_sd_last_transitions(int64_t* montou_us, int64_t* desmontou_us)
{
  if (montou_us)    { *montou_us = s_montou_us; }
  if (desmontou_us) { *desmontou_us = s_desmontou_us; }
}
static void on_ocioso(void*);
static bool s_ultima_falhou  = false;   // a latest tentativa failed
// SEM CARTAO, ESPERA CRESCENTE. Cada tentativa num slot vazio leva ~1 s of
// timeout com a trava presa, e todo mundo que grava (log, WiFi, notificacoes)
// pede montagem: sem freio eram 5 lines of erro por segundo e one segundo of
// queue para cada one. Depois of uma failure, a next tentativa of verdade so
// exit depois of SD_ESPERA_MIN_MS, dobrando a cada nova failure ate SD_ESPERA_MAX_MS;
// entre uma e outra, mount devolve failure in hora. Cartao inserido com o
// aparelho on e achado em ate SD_ESPERA_MAX_MS.
#define SD_ESPERA_MIN_MS  2000
#define SD_ESPERA_MAX_MS 10000
static int64_t  s_next_us = 0;
static uint32_t s_espera_ms  = 0;
static char s_desc[64]       = "nao verificado";

static void gravador_start(void);

// EMPRESTADO AO COMPUTADOR (mode pendrive, ver tab5_sd_lend no .hpp).
// Enquanto valer, o cartao e of computador: mount() recusa in hora, como
// se nao houvesse cartao, e o gravador segura o que chegar. s_cru e o mesmo
// cartao, inicializado SEM sistema of arquivos - so setores.
static volatile bool s_emprestado = false;
static sdmmc_card_t* s_cru = nullptr;
static tab5_sd_summary_t s_summary = { };   // o diario deste lending (tab5_sd_summary)

void tab5_sd_init(void)
{
  if (!s_mutex) { s_mutex = xSemaphoreCreateMutex(); }
  // O diario ANTES of gravador: o que became of one pendrive interrompido por
  // one reinicio tem que estar visivel when a primeira gravacao chegar.
  tab5_journal_init();
  gravador_start();
  if (!s_ocioso) {
    const esp_timer_create_args_t a = { .callback = on_ocioso, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "sd_ocioso",
                                        .skip_unhandled_events = true };
    esp_timer_create(&a, &s_ocioso);
  }

  // CALADO PARA SEMPRE, e nao so numa window - o aviso inundava o console.
  // sd_pwr_ctrl_new_on_chip_ldo()
  // (chamada em montar_de_verdade() logo abaixo) sempre cria o controlador of
  // LDO com tensao 0 antes of programar a tensao real, e o driver of IDF avisa
  // isso em WARN toda vez - benigno (o cartao monta e le normal, a tensao real
  // e configurada logo em seguida), mas o cartao era mounted/desmontado a cada
  // ~15s PARA SEMPRE (era o proprio log se realimentando - corrigido em
  // ver montar_de_verdade e a desmontagem adiada) - e nao so numa window
  // rara como o "ledc" durante o wake (tab5_power.cpp), entao nao ha
  // "depois" para restaurar o level: e a mesma logica of "eh_sdio"/"sdmmc_io"
  // em tab5_wifi_init(), silenciados uma vez no boot.
  esp_log_level_set("ldo", ESP_LOG_ERROR);
  // As camadas of bottom repetem, a cada tentativa, o erro que a nossa ja of
  // (e so in mudanca of state - ver montar_de_verdade).
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
}

bool tab5_sd_mounted(void) { return s_card != nullptr; }
const char* tab5_sd_name(void) { return s_rotulo; }

// O NOME DO VOLUME. O FAT32 guarda o rotulo em DOIS lugares: uma input
// especial in folder root (a que o FatFs le, f_getlabel) e one field of setor of
// boot of volume (offset 0x47, 11 bytes). Quem renomeia nem sempre escreve os
// dois - entao, sem a input in root, vale o of setor of boot. "NO NAME" ali
// e o "sem name" of formatador. Os dois em maiusculas (e o que o FAT guarda).
static void read_rotulo(void)
{
  s_rotulo[0] = 0;
  const char drv[3] = { (char)('0' + ff_diskio_get_pdrv_card(s_card)), ':', 0 };
  char r[sizeof(s_rotulo)] = "";
  if (f_getlabel(drv, r, nullptr) == FR_OK && r[0]) {
    snprintf(s_rotulo, sizeof(s_rotulo), "%s", r);
    return;
  }
  // O setor of boot of volume: o 0 of cartao, ou o start of 1a particao
  // (cartao com tabela of particoes - o normal, formatado no computador).
  // Lido raw e nao pelo f_getfree, que num cartao grande pode scan a FAT.
  uint8_t* setor = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!setor) { return; }
  bool ok = sdmmc_read_sectors(s_card, setor, 0, 1) == ESP_OK && setor[510] == 0x55 && setor[511] == 0xAA;
  if (ok && memcmp(setor + 0x52, "FAT32", 5) != 0) {
    const uint32_t start = setor[0x1C6] | (setor[0x1C7] << 8) | (setor[0x1C8] << 16) | ((uint32_t)setor[0x1C9] << 24);
    ok = start && sdmmc_read_sectors(s_card, setor, start, 1) == ESP_OK && setor[510] == 0x55
         && setor[511] == 0xAA && memcmp(setor + 0x52, "FAT32", 5) == 0;
  }
  if (ok) {
    char b[12];
    memcpy(b, setor + 0x47, 11);
    b[11] = 0;
    for (int i = 10; i >= 0 && b[i] == ' '; --i) { b[i] = 0; }   // o field vem completado com espacos
    if (b[0] && strcmp(b, "NO NAME") != 0) { snprintf(s_rotulo, sizeof(s_rotulo), "%s", b); }
  }
  heap_caps_free(setor);
}

const char* tab5_sd_description(void)
{
  if (s_emprestado)         { snprintf(s_desc, sizeof(s_desc), "no computador (USB)"); }
  else if (s_card)          { snprintf(s_desc, sizeof(s_desc), "%s", s_ident); }
  else if (s_ultima_falhou) { snprintf(s_desc, sizeof(s_desc), "ausente ou ilegivel"); }
  else if (s_ja_montou)     { snprintf(s_desc, sizeof(s_desc), "%s (free_space)", s_ident); }
  else                      { snprintf(s_desc, sizeof(s_desc), "nao verificado"); }
  return s_desc;
}

// Host SDMMC e slot of cartao, com o LDO (s_ldo) ja on amarrado no host.
// Usado pela montagem normal e pelo acesso raw of pendrive - os dois TEM que
// falar com o cartao of mesmo jeito (slot 0, ver o aviso abaixo).
static void configurar_host(sdmmc_host_t* host, sdmmc_slot_config_t* slot)
{
  *host = SDMMC_HOST_DEFAULT();

  // SLOT 0, E ISTO NAO E DETALHE.
  //
  // O SDMMC_HOST_DEFAULT() vem com .slot = SDMMC_HOST_SLOT_1
  // (sdmmc_default_configs.h:31), e o esp_hosted usa o SLOT 1 para falar com o
  // ESP32-C6 of WiFi (CONFIG_ESP_HOSTED_HOST_SDIO_SLOT=1). Com os dois no mesmo
  // slot, mount o cartao reprograma os pins of slot e ARRANCA O C6 DO
  // BARRAMENTO. Foi exatamente o que aconteceu:
  //
  //   wifi: C6 no ar, firmware 1.4.1            <- link com o C6 of pe
  //   sdmmc_periph: SDMMC host already initialized, skipping initflow
  //   sdmmc_io: sdmmc_send_cmd returned 0x107   <- C6 morto, 1ms depois
  //   eh_sdio: Unrecoverable host sdio state    -> reinicio of placa
  //
  // O P4 tem dois slots (SOC_SDMMC_NUM_SLOTS=2) e roteia por GPIO matrix
  // (SOC_SDMMC_USE_GPIO_MATRIX=1), entao qualquer pino serve em qualquer slot.
  // Mudamos o NOSSO lado e nao o of esp_hosted, para nao mexer no preset of
  // placa que a Espressif mantem.
  host->slot = SDMMC_HOST_SLOT_0;
  host->pwr_ctrl_handle = s_ldo;

  // Slot em 4 bits, com os pins of Tab5. O P4 tem GPIO matrix para o
  // SDMMC, entao of para remapear livremente.
  *slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot->width = 4;
  slot->clk = (gpio_num_t)SD_CLK;
  slot->cmd = (gpio_num_t)SD_CMD;
  slot->d0  = (gpio_num_t)SD_D0;
  slot->d1  = (gpio_num_t)SD_D1;
  slot->d2  = (gpio_num_t)SD_D2;
  slot->d3  = (gpio_num_t)SD_D3;
  slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static bool montar_of_verdade(void)
{
  // 1. Liga o LDO que alimenta a IO of cartao.
  sd_pwr_ctrl_ldo_config_t ldo_cfg = { };
  ldo_cfg.ldo_chan_id = SD_LDO_CANAL;
  esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_ldo);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LDO do cartao (channel %d) failed: %s", SD_LDO_CANAL, esp_err_to_name(err));
    return false;
  }

  // 2. Host SDMMC e slot - a mesma configuracao of acesso raw (pendrive).
  sdmmc_host_t host;
  sdmmc_slot_config_t slot;
  configurar_host(&host, &slot);

  esp_vfs_fat_sdmmc_mount_config_t mnt = { };
  // NUNCA formatar sozinho. Um cartao que nao monta pode estar apenas com o
  // sistema of arquivos errado, e formatar destruiria o que o user tem la.
  // Se one dia precisar formatar, que seja one botao explicito num app.
  mnt.format_if_mount_failed = false;
  mnt.max_files              = 8;
  mnt.allocation_unit_size   = 16 * 1024;

  tab5_mark("sd: montando", 0);
  err = esp_vfs_fat_sdmmc_mount(TAB5_SD_ROOT, &host, &slot, &mnt, &s_card);
  if (err != ESP_OK) {
    // So a PRIMEIRA failure of uma sequencia vai on log; as repeticoes of
    // mesmo state sao depuracao.
    if (!s_ultima_falhou) {
      if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "cartao present mas sem sistema de arquivos FAT legivel");
      } else {
        ESP_LOGE(TAG, "montagem failed: %s (cartao ausente ou mal encaixado?) - tento de novo em ate %d s",
                 esp_err_to_name(err), SD_ESPERA_MAX_MS / 1000);
      }
    } else {
      ESP_LOGD(TAG, "montagem failed de novo: %s", esp_err_to_name(err));
    }
    sd_pwr_ctrl_del_on_chip_ldo(s_ldo);
    s_ldo  = nullptr;
    s_card = nullptr;
    s_ultima_falhou = true;
    return false;
  }

  if (s_ultima_falhou) { ESP_LOGI(TAG, "cartao achado"); }
  s_ultima_falhou = false;
  s_ja_montou     = true;
  snprintf(s_ident, sizeof(s_ident), "%s %.1fGB",
           s_card->is_sdio ? "SDIO" : (s_card->is_mmc ? "MMC" : "SD"),
           ((double)s_card->csd.capacity * s_card->csd.sector_size) / (1024.0 * 1024.0 * 1024.0));

  read_rotulo();

  // Garante a root dos aplicativos. mkdir devolvendo EEXIST nao e erro.
  mkdir(TAB5_SD_APPS, 0777);

  // MONTAR NAO E NOTICIA. Estas lines iam para o log, o log e
  // gravado NO CARTAO, e write requires mount: cada descarga of log gerava as
  // duas lines que obrigavam a next descarga - o cartao montava e
  // desmontava a cada 15s para sempre, gravando que tinha gravado. So a
  // primeira montagem of boot stays visivel; o resto e depuracao.
  if (!s_anunciou) {
    ESP_LOGI(TAG, "mounted em %s: %s, name '%s'", TAB5_SD_ROOT, s_ident, s_rotulo);
    s_anunciou = true;
    arrumar_sistema();
  }
  else             { ESP_LOGD(TAG, "mounted"); }
  s_montou_us = esp_timer_get_time();
  return true;
}

// A PASTA SISTEMA EM SUBPASTAS. Os logs of dia e os arquivos of
// energia iam todos soltos em apps/Sistema e afogavam o resto (wifi.txt,
// firmware of C6). Agora cada one tem a sua: TAB5_SD_LOGS e TAB5_SD_POWER.
// O que ja estava no cartao of jeito antigo e movido UMA vez por boot, in
// primeira montagem - com nada a move, custa so listar a folder.
static bool tem_prefixo(const char* s, const char* p) { return strncasecmp(s, p, strlen(p)) == 0; }

static void move_para(const char* base, const char* name, const char* sub, int* n)
{
  char dest_pasta[96];
  if (!tab5_sd_folder(sub, dest_pasta, sizeof(dest_pasta))) { return; }
  char of[200], para[200];
  snprintf(of, sizeof(of), "%s/%s", base, name);
  snprintf(para, sizeof(para), "%s/%s", dest_pasta, name);
  struct stat st;
  if (stat(para, &st) == 0) {
    ESP_LOGW(TAG, "%s ja existe em %s - deixei o antigo onde estava", name, sub);
    return;
  }
  if (rename(of, para) == 0) { ++*n; }
  else { ESP_LOGW(TAG, "nao consegui mover %s: %s", name, strerror(errno)); }
}

static void arrumar_sistema(void)
{
  char base[96];
  snprintf(base, sizeof(base), "%s/Sistema", TAB5_SD_APPS);
  DIR* d = opendir(base);
  if (!d) { return; }
  // Junta os nomes antes of move: renomear durante o readdir of mesma folder
  // pode pular ou repetir entries no FAT.
  // Na PSRAM e so durante a arrumacao: roda uma vez por boot.
  char (*nomes)[64] = (char (*)[64])heap_caps_malloc(48 * 64, MALLOC_CAP_SPIRAM);
  if (!nomes) { closedir(d); return; }
  int total = 0;
  while (struct dirent* e = readdir(d)) {
    if (e->d_type == DT_DIR || total >= 48) { continue; }
    const char* nm = e->d_name;
    const bool eh_log = tem_prefixo(nm, "log_");
    const bool eh_ene = tem_prefixo(nm, "energia");
    if ((eh_log || eh_ene) && strlen(nm) < 64) {
      snprintf(nomes[total++], 64, "%s", nm);
    }
  }
  closedir(d);

  int n = 0;
  for (int i = 0; i < total; ++i) {
    move_para(base, nomes[i], tem_prefixo(nomes[i], "log_") ? TAB5_SD_LOGS : TAB5_SD_POWER, &n);
  }
  heap_caps_free(nomes);
  if (n) { ESP_LOGI(TAG, "Sistema arrumado: %d file(s) movidos para logs/ e energia/", n); }
}

bool tab5_sd_mount(void)
{
  trava();
  bool ok;
  if (s_emprestado) {
    // O computador esta com o cartao: recusa calado, sem a espera crescente
    // of "sem cartao" - ele iteration inteiro when o pendrive acabar.
    ok = false;
  } else if (s_card) {
    // Ja mounted - talvez so esperando o prazo of ociosidade: cancela o prazo
    // e conta. E aqui que a montagem e reaproveitada.
    if (s_refs == 0 && s_ocioso) { esp_timer_stop(s_ocioso); }
    s_refs++;
    ok = true;
  } else if (s_ultima_falhou && esp_timer_get_time() < s_next_us) {
    ok = false;                               // em espera depois of falhar: nem tenta
  } else if (montar_of_verdade()) {
    s_refs = 1;
    s_espera_ms = 0;
    ok = true;
  } else {
    ok = false;                               // failed: NAO incrementa
    s_espera_ms = s_espera_ms ? s_espera_ms * 2 : SD_ESPERA_MIN_MS;
    if (s_espera_ms > SD_ESPERA_MAX_MS) { s_espera_ms = SD_ESPERA_MAX_MS; }
    s_next_us = esp_timer_get_time() + (int64_t)s_espera_ms * 1000;
  }
  destrava();
  return ok;
}

// Chamada com a trava presa.
static void desmontar_of_verdade(void)
{
  tab5_mark("sd: desmontando", 0);
  esp_vfs_fat_sdcard_unmount(TAB5_SD_ROOT, s_card);
  s_card = nullptr;
  if (s_ldo) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; }
  // A identidade NAO e apagada: o cartao continua encaixado, so nao esta
  // mounted. Apagar aqui era o que fazia o `servicos` anunciar "sem cartao"
  // com o cartao no lugar.
  ESP_LOGD(TAG, "desmontado");
  s_desmontou_us = esp_timer_get_time();
}

// O prazo of ociosidade venceu (tarefa of esp_timer). Se ninguem pegou o
// cartao of iteration nesse storage time, now desmonta.
static void on_ocioso(void*)
{
  trava();
  if (s_refs == 0 && s_card) { desmontar_of_verdade(); }
  destrava();
}

void tab5_sd_unmount(void)
{
  trava();
  if (s_refs <= 0 || --s_refs > 0) {          // ninguem montou, ou ainda ha who use
    destrava();
    return;
  }
  // O ultimo devolveu: nao desmonta ja - arma o prazo (ou o reinicia).
  if (s_ocioso) {
    esp_timer_stop(s_ocioso);
    esp_timer_start_once(s_ocioso, SD_OCIOSO_US);
  } else {
    desmontar_of_verdade();
  }
  destrava();
}

// ===========================================================================
// EMPRESTIMO AO COMPUTADOR (mode pendrive) - ver o .hpp
// ===========================================================================
static void tarefa_cartao_wake(void);
static void gravacao_lock(bool lock);
static void esvaziar_buffer(void);   // com a trava of gravacao presa
static void aplicar_diario(void);    // idem

// ---------------------------------------------------------------------------
// A FOTO DO CARTAO NO EMPRESTIMO
// ---------------------------------------------------------------------------
// Tamanho e data of cada file of apps/ no instante em que o cartao exit. Um
// file SUBSTITUIDO durante o pendrive leva a foto dele junto para o diario
// (a foto em si stays so in RAM: o registro e que precisa sobreviver a one
// reinicio). Na iteration, file diferente of foto = o computador mexeu nele.
typedef struct { uint32_t hash, size, data; } foto_t;
#define FOTO_MAX 4096
static foto_t* s_foto = nullptr;
static int     s_foto_n = 0;

static uint32_t hash_text(const char* s)
{
  uint32_t h = 2166136261u;   // FNV-1a
  while (*s) { h = (h ^ (uint8_t)*s++) * 16777619u; }
  return h;
}

static void fotografar_pasta(const char* folder, int level)
{
  DIR* d = opendir(folder);
  if (!d) { return; }
  while (struct dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') { continue; }
    char caminho[192];
    if (snprintf(caminho, sizeof(caminho), "%s/%s", folder, e->d_name) >= (int)sizeof(caminho)) { continue; }
    if (e->d_type == DT_DIR) {
      if (level < 4) { fotografar_pasta(caminho, level + 1); }
      continue;
    }
    if (s_foto_n >= FOTO_MAX) { continue; }   // alem disso: sem foto, a iteration trata como conflito
    struct stat st;
    if (stat(caminho, &st) != 0) { continue; }
    s_foto[s_foto_n++] = { hash_text(caminho), (uint32_t)st.st_size, (uint32_t)st.st_mtime };
  }
  closedir(d);
}

// Com o cartao mounted e a trava presa.
static void fotografar(void)
{
  if (!s_foto) { s_foto = (foto_t*)heap_caps_malloc(FOTO_MAX * sizeof(foto_t), MALLOC_CAP_SPIRAM); }
  s_foto_n = 0;
  if (!s_foto) { return; }
  fotografar_pasta(TAB5_SD_APPS, 0);
}

static const foto_t* foto_of(const char* caminho)
{
  const uint32_t h = hash_text(caminho);
  for (int i = 0; i < s_foto_n; ++i) { if (s_foto[i].hash == h) { return &s_foto[i]; } }
  return nullptr;
}

// A FOTO SO SERVE ENQUANTO O CARTAO ESTA FORA: cada registro SUBSTITUIR of
// diario ja leva a sua base junto. Solta no end of lending (ou when ele
// desiste) - 48 KB of PSRAM que ficavam presos ate o reinicio. Com a trava
// of gravacao presa: e com ela que o replace le a foto.
static void foto_release(void)
{
  heap_caps_free(s_foto);
  s_foto = nullptr;
  s_foto_n = 0;
}

static void cru_release(void)
{
  if (!s_cru) { return; }
  // O mesmo desfazer of esp_vfs_fat_sdcard_unmount: so o SLOT 0 (o slot 1 e
  // o SDIO of C6, e one deinit of host inteiro o derrubaria).
  if (s_cru->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) { s_cru->host.deinit_p(s_cru->host.slot); }
  else                                                { s_cru->host.deinit(); }
  free(s_cru);
  s_cru = nullptr;
  if (s_ldo) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; }
}

// O cartao sem sistema of arquivos: LDO, host, slot e a inicializacao of
// cartao - o que o esp_vfs_fat_sdmmc_mount faz antes of f_mount. Com a trava.
static bool cru_prepare(void)
{
  sd_pwr_ctrl_ldo_config_t ldo_cfg = { };
  ldo_cfg.ldo_chan_id = SD_LDO_CANAL;
  if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_ldo) != ESP_OK) { s_ldo = nullptr; return false; }

  sdmmc_host_t host;
  sdmmc_slot_config_t slot;
  configurar_host(&host, &slot);

  s_cru = (sdmmc_card_t*)calloc(1, sizeof(sdmmc_card_t));
  if (!s_cru) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; return false; }
  esp_err_t err = host.init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cartao para o pendrive: host %s", esp_err_to_name(err));
    free(s_cru); s_cru = nullptr;
    sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr;
    return false;
  }
  err = sdmmc_host_init_slot(host.slot, &slot);
  if (err == ESP_OK) { err = sdmmc_card_init(&host, s_cru); }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cartao para o pendrive: %s", esp_err_to_name(err));
    s_cru->host = host;   // o host foi iniciado: o cru_soltar o desfaz
    cru_release();
    return false;
  }
  return true;
}

tab5_sd_lending_t tab5_sd_lend(uint32_t espera_ms)
{
  if (s_emprestado) { return s_cru ? SD_LENT : SD_LENDING_NO_CARD; }
  // Sem o diario, o que o sistema gravasse durante o pendrive se perderia.
  uint32_t total = 0;
  tab5_journal_usage(nullptr, &total, nullptr);
  if (total == 0) { return SD_LENDING_NO_JOURNAL; }
  s_summary = { };   // o summary e deste lending

  // 1. Com a trava of gravacao presa of comeco on end deste step, nada e
  //    gravado no storage dele:
  //    - o que ja foi entregue (e one diario antigo) vai para o cartao AGORA,
  //      enquanto ele ainda e of Tab5;
  //    - a FOTO of cartao (size e data of cada file) e tirada - e com
  //      ela que a iteration reconhece o que o computador changed;
  //    - mount() passa a recusar, e as portas passam a write no diario.
  gravacao_lock(true);
  esvaziar_buffer();
  aplicar_diario();
  trava();
  if (!s_card) { montar_of_verdade(); }   // so para a foto (sem referencia)
  if (s_card) { fotografar(); } else { s_foto_n = 0; }
  s_emprestado = true;
  destrava();
  gravacao_lock(false);

  // 2. Espera who JA estava com o cartao return_card. Os servicos seguram por
  //    milissegundos (open, write, close); who passar of prazo esta
  //    usando of verdade, e lend now seria tirar o cartao of bottom dele.
  const int64_t limit = esp_timer_get_time() + (int64_t)espera_ms * 1000;
  for (;;) {
    trava();
    const int refs = s_refs;
    destrava();
    if (refs == 0) { break; }
    if (esp_timer_get_time() > limit) {
      ESP_LOGW(TAG, "pendrive: cartao em uso (%d), nao lent", refs);
      gravacao_lock(true);
      trava();
      s_emprestado = false;
      foto_release();
      // A montagem of foto nao tem owner: iteration on prazo of ociosidade.
      if (s_refs == 0 && s_card && s_ocioso) { esp_timer_start_once(s_ocioso, SD_OCIOSO_US); }
      destrava();
      gravacao_lock(false);
      tarefa_cartao_wake();   // o que caiu no diario in espera vai para o cartao
      return SD_LENDING_IN_USE;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // 3. Desmonta of verdade - a montagem ociosa tambem: o FAT of Tab5 nao
  //    pode ter NADA em cache when o computador comecar a write. Entao
  //    o cartao raw.
  gravacao_lock(true);
  trava();
  if (s_ocioso) { esp_timer_stop(s_ocioso); }
  if (s_card) { desmontar_of_verdade(); }
  const bool ok = cru_prepare();
  if (!ok) { s_emprestado = false; foto_release(); }
  destrava();
  gravacao_lock(false);
  if (!ok) { tarefa_cartao_wake(); return SD_LENDING_NO_CARD; }
  ESP_LOGI(TAG, "cartao lent ao computador: %s (%d file(s) fotografado(s))",
           s_ident[0] ? s_ident : "SD", s_foto_n);
  return SD_LENT;
}

sdmmc_card_t* tab5_sd_raw(void) { return s_emprestado ? s_cru : nullptr; }

void tab5_sd_return(void)
{
  // A trava of gravacao ANTES of of cartao (a order of todas as portas): o
  // replace le a foto com ela presa, e a foto exit daqui junto com o
  // lending.
  gravacao_lock(true);
  trava();
  if (!s_emprestado) { destrava(); gravacao_lock(false); return; }
  cru_release();
  s_emprestado = false;
  // A next montagem e normal: nada of espera of "sem cartao" herdada.
  s_ultima_falhou = false;
  s_espera_ms = 0;
  foto_release();
  destrava();
  gravacao_lock(false);
  ESP_LOGI(TAG, "cartao devolvido ao Tab5");
  tarefa_cartao_wake();   // aplica o diario (vTaskCartao)
}

bool tab5_sd_lent(void) { return s_emprestado; }

// Nome of app -> name of folder seguro em FAT. Troca o que pode dar problema por
// '_' em vez of recusar: e melhor write em "Mapa_Azimutal" of que nao write.
static void name_seguro(const char* source, char* target, size_t size)
{
  size_t j = 0;
  for (size_t i = 0; source[i] && j + 1 < size; ++i) {
    const char c = source[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9') || c == '-' || c == '_';
    target[j++] = ok ? c : '_';
  }
  target[j] = '\0';
  if (j == 0) { snprintf(target, size, "app"); }
}

bool tab5_sd_folder(const char* app, char* out, size_t out_size)
{
  if (!s_card || !app || !out) { return false; }

  // SUBPASTAS: "Sistema/logs" vira apps/Sistema/logs. Cada trecho
  // entre barras passa pelo nome_seguro e cada level e criado se faltar.
  size_t n = (size_t)snprintf(out, out_size, "%s", TAB5_SD_APPS);
  const char* p = app;
  while (*p) {
    const char* end = strchr(p, '/');
    const size_t size = end ? (size_t)(end - p) : strlen(p);
    if (size > 0) {
      char trecho[32], seguro[32];
      snprintf(trecho, sizeof(trecho), "%.*s", (int)size, p);
      name_seguro(trecho, seguro, sizeof(seguro));
      n += (size_t)snprintf(out + n, n < out_size ? out_size - n : 0, "/%s", seguro);
      if (n >= out_size) { return false; }

      struct stat st;
      if (stat(out, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) { return false; }
      } else if (mkdir(out, 0777) != 0) {
        ESP_LOGE(TAG, "nao consegui criar %s", out);
        return false;
      }
    }
    if (!end) { break; }
    p = end + 1;
  }
  return true;
}

static FILE* open_cru(const char* app, const char* file, const char* mode);

FILE* tab5_sd_open(const char* app, const char* file, const char* mode)
{
  // Ler (ou write por fora das portas) tem que ver everything o que ja foi
  // entregue: o pending of gravador e o diario of one pendrive.
  tab5_sd_flush_pending();
  return open_cru(app, file, mode);
}

// TROCA INTERROMPIDA. Substituir one file no FAT e: grava "<name>.tmp"
// inteiro (e fsync), apaga o antigo, renomeia o .tmp - o FAT nao troca por
// top. Uma queda of energia entre erase e renomear deixa o file SUMIDO e
// o content novo, inteiro, no .tmp. Quem for read encontra isso e complete a
// troca. (Um .tmp com o file ainda la e uma troca que nem comecou a erase:
// stays, e a next troca o sobrescreve.)
static void completar_troca(const char* caminho)
{
  struct stat st;
  if (stat(caminho, &st) == 0) { return; }
  char tmp[200];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", caminho) >= (int)sizeof(tmp)) { return; }
  if (stat(tmp, &st) != 0) { return; }
  if (rename(tmp, caminho) == 0) { ESP_LOGW(TAG, "%s: troca interrompida por queda de energia - completada", caminho); }
}

static FILE* open_cru(const char* app, const char* file, const char* mode)
{
  char folder[96];
  if (!tab5_sd_folder(app, folder, sizeof(folder))) { return nullptr; }

  char caminho[160];
  snprintf(caminho, sizeof(caminho), "%s/%s", folder, file);

  completar_troca(caminho);
  FILE* f = fopen(caminho, mode);
  if (!f)
  {
    // NEM TODA FALHA E ERRO, e o service e who tem que saber a diferenca.
    //
    // Abrir para LEITURA one file que ainda nao existe e resultado legitimo e
    // previsivel - acontece toda vez que one app le algo pela primeira vez (o
    // history of uma conversa nova of Meshtastic, por exemplo). Registrar
    // isso como ERRO enche o log of alarme falso, e log que grita a toa ensina
    // a ignorar log.
    //
    // Ja failure on ABRIR PARA ESCRITA, ou qualquer outro errno in leitura
    // (permissao, cartao corrompido, caminho invalido), e problema of verdade e
    // continua em ERRO.
    const bool so_leitura = (mode[0] == 'r');
    if (so_leitura && errno == ENOENT) {
      ESP_LOGD(TAG, "'%s' ainda nao existe", caminho);
    } else {
      ESP_LOGE(TAG, "fopen('%s','%s') failed: %s", caminho, mode, strerror(errno));
    }
  }
  return f;
}


bool tab5_sd_space(uint64_t* total, uint64_t* free_space)
{
  if (!s_card) { return false; }
  // O newlib of ESP nao tem statvfs; o IDF expoe isto pelo proprio VFS of FAT.
  return esp_vfs_fat_info(TAB5_SD_ROOT, total, free_space) == ESP_OK;
}

// ===========================================================================
// O GRAVADOR (ver o .hpp)
// ===========================================================================
#include "freertos/task.h"
#include "tab5_owners.hpp"

#define ANEXO_BUF      (16 * 1024)   // cada one dos dois buffers, in PSRAM
#define ANEXO_PRAZO_MS 5000          // o que nao e urgent espera no maximum isto
#define ANEXO_APP      32
#define ANEXO_ARQ      40

typedef struct {
  uint16_t size;          // registro inteiro, header incluso
  uint16_t n_cab, n_txt;
  char     app[ANEXO_APP];
  char     arq[ANEXO_ARQ];
  // seguem n_cab bytes of header e n_txt of text
} anexo_t;

// DOIS buffers: who entrega escreve no active; a gravacao troca e grava o
// outro SEM a trava - entregar nunca espera o cartao.
static uint8_t*          s_buf[2]   = { nullptr, nullptr };
static int               s_active    = 0;
static size_t            s_usado    = 0;
static int64_t           s_since_us = 0;      // when chegou o mais antigo pending
static SemaphoreHandle_t s_anexo    = nullptr; // o buffer active
static SemaphoreHandle_t s_gravando = nullptr; // uma gravacao por vez
static TaskHandle_t      s_tarefa   = nullptr;
static volatile uint32_t s_gravadas = 0, s_perdidas = 0;

bool tab5_sd_enqueue_append(const char* app, const char* file, const char* text,
                    const char* header, bool urgent)
{
  if (!s_anexo || !app || !file || !text) { return false; }
  const size_t nc = header ? strlen(header) : 0;
  const size_t nt = strlen(text);
  const size_t size = sizeof(anexo_t) + nc + nt;
  if (size > ANEXO_BUF / 2) { s_perdidas = s_perdidas + 1; return false; }

  xSemaphoreTake(s_anexo, portMAX_DELAY);   // so memcpy aqui dentro
  const bool cabe = (s_usado + size <= ANEXO_BUF);
  if (cabe) {
    anexo_t* a = (anexo_t*)(s_buf[s_active] + s_usado);
    a->size = (uint16_t)size; a->n_cab = (uint16_t)nc; a->n_txt = (uint16_t)nt;
    snprintf(a->app, sizeof(a->app), "%s", app);
    snprintf(a->arq, sizeof(a->arq), "%s", file);
    uint8_t* p = (uint8_t*)(a + 1);
    if (nc) { memcpy(p, header, nc); }
    memcpy(p + nc, text, nt);
    if (s_usado == 0) { s_since_us = esp_timer_get_time(); }
    s_usado += size;
  }
  const bool half = (s_usado > ANEXO_BUF / 2);
  xSemaphoreGive(s_anexo);

  if (!cabe) { s_perdidas = s_perdidas + 1; }
  if ((urgent || half) && s_tarefa) { xTaskNotifyGive(s_tarefa); }
  return cabe;
}

// ---------------------------------------------------------------------------
// O DESTINO DE CADA GRAVACAO
// ---------------------------------------------------------------------------
// Cartao, ou o diario in flash (hal/tab5_journal) - decidido SEMPRE com a
// trava of gravacao presa, a mesma of lending e of aplicacao of diario.
// Enquanto o diario tiver algo pending, TUDO vai para ele, mesmo com o
// cartao ja of iteration: write direto no cartao passaria in frente of que
// aconteceu antes e esta esperando.
static bool para_o_diario(void) { return s_emprestado || tab5_journal_pending(); }

static volatile uint32_t s_diario_perdidas = 0;   // nao couberam no diario

static bool diario_acrescentar(const char* app, const char* arq,
                               const void* cab, size_t n_cab, const void* data, size_t n)
{
  journal_entry_t r = { };
  r.type = JOURNAL_APPEND;
  r.app = app; r.file = arq;
  r.header = cab; r.header_size = (uint16_t)n_cab;
  r.data = data; r.data_size = (uint32_t)n;
  if (tab5_journal_write(&r)) { return true; }
  s_diario_perdidas = s_diario_perdidas + 1;
  return false;
}

// O caminho no cartao of app/file, SEM create folder (para a foto e o hash).
static void caminho_of(const char* app, const char* arq, char* out, size_t size)
{
  size_t n = (size_t)snprintf(out, size, "%s", TAB5_SD_APPS);
  const char* p = app;
  while (*p && n < size) {
    const char* end = strchr(p, '/');
    const size_t k = end ? (size_t)(end - p) : strlen(p);
    if (k > 0) {
      char trecho[32], seguro[32];
      snprintf(trecho, sizeof(trecho), "%.*s", (int)k, p);
      name_seguro(trecho, seguro, sizeof(seguro));
      n += (size_t)snprintf(out + n, n < size ? size - n : 0, "/%s", seguro);
    }
    if (!end) { break; }
    p = end + 1;
  }
  if (n < size) { snprintf(out + n, size - n, "/%s", arq); }
}

// Grava one buffer ja trocado: cada file open UMA vez, as lines dele in
// order of chegada. Chamado com s_gravando presa.
static void gravar_buffer(uint8_t* b, size_t n)
{
  if (n == 0) { return; }
  if (para_o_diario()) {
    for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->size) {
      anexo_t* a = (anexo_t*)(b + o);
      const uint8_t* p = (const uint8_t*)(a + 1);
      if (diario_acrescentar(a->app, a->arq, a->n_cab ? p : nullptr, a->n_cab, p + a->n_cab, a->n_txt)) {
        s_gravadas = s_gravadas + 1;
      } else {
        s_perdidas = s_perdidas + 1;
      }
    }
    return;
  }
  if (!tab5_sd_mount()) {
    for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->size) { s_perdidas = s_perdidas + 1; }
    return;
  }
  // Marca os ja gravados zerando o 'app' (o registro continua no lugar, so
  // deixa of valer) - varre of novo para o next file distinto.
  for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->size) {
    anexo_t* a = (anexo_t*)(b + o);
    if (!a->app[0]) { continue; }
    char app[ANEXO_APP], arq[ANEXO_ARQ];
    memcpy(app, a->app, sizeof(app));
    memcpy(arq, a->arq, sizeof(arq));
    FILE* f = open_cru(app, arq, "a");
    bool cabecalho_feito = false;
    for (size_t k = o; k < n; k += ((anexo_t*)(b + k))->size) {
      anexo_t* r = (anexo_t*)(b + k);
      if (!r->app[0] || strcmp(r->app, app) != 0 || strcmp(r->arq, arq) != 0) { continue; }
      const uint8_t* p = (const uint8_t*)(r + 1);
      if (f) {
        if (!cabecalho_feito) {
          cabecalho_feito = true;
          if (r->n_cab && ftell(f) == 0) { fwrite(p, 1, r->n_cab, f); }
        }
        fwrite(p + r->n_cab, 1, r->n_txt, f);
        s_gravadas = s_gravadas + 1;
      } else {
        s_perdidas = s_perdidas + 1;
      }
      r->app[0] = 0;
    }
    if (f) { fclose(f); }
  }
  tab5_sd_unmount();
}

// Troca o buffer e grava o cheio. Com s_gravando presa.
static void esvaziar_buffer(void)
{
  if (!s_anexo) { return; }
  xSemaphoreTake(s_anexo, portMAX_DELAY);
  uint8_t* cheio = s_buf[s_active];
  const size_t n = s_usado;
  s_active = 1 - s_active;
  s_usado = 0;
  xSemaphoreGive(s_anexo);
  gravar_buffer(cheio, n);
}

static void gravar_everything(void)
{
  if (!s_gravando) { return; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  xSemaphoreGive(s_gravando);
}

// ---------------------------------------------------------------------------
// APLICAR O DIARIO NO CARTAO
// ---------------------------------------------------------------------------
// Arquivos que ESTA aplicacao ja escreveu: changed por causa dela, nao of
// computador - one segundo SUBSTITUIR of mesmo file nao e conflito.
#define TOCADOS_MAX 64
static uint32_t s_tocados[TOCADOS_MAX];
static int      s_n_tocados = 0;
static bool touched(uint32_t h)
{
  for (int i = 0; i < s_n_tocados; ++i) { if (s_tocados[i] == h) { return true; } }
  return false;
}
static void touch(uint32_t h) { if (!touched(h) && s_n_tocados < TOCADOS_MAX) { s_tocados[s_n_tocados++] = h; } }

// Substitui o file inteiro no cartao, sem nunca deixar storage file: grava
// "<name>.tmp", e so entao troca pelo antigo. SIGILOSO zera o antigo antes of
// erase - no FAT erase so brand os setores como livres, e o content antigo
// ficaria legivel por qualquer programa of recuperacao. Cartao mounted.
static bool substituir_no_cartao(const char* caminho, const void* data, size_t n, uint8_t flags)
{
  char tmp[200];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", caminho) >= (int)sizeof(tmp)) { return false; }
  FILE* f = fopen(tmp, "wb");
  if (!f) { ESP_LOGE(TAG, "fopen('%s') failed: %s", tmp, strerror(errno)); return false; }
  bool ok = (n == 0) || fwrite(data, 1, n, f) == n;
  ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
  if (fclose(f) != 0 || !ok) { unlink(tmp); return false; }
  if (flags & TAB5_SD_SIGILOSO) {
    FILE* velho = fopen(caminho, "r+");
    if (velho) {
      fseek(velho, 0, SEEK_END);
      long falta = ftell(velho);
      fseek(velho, 0, SEEK_SET);
      static const char zeros[64] = { };
      while (falta > 0) {
        const size_t k = falta > (long)sizeof(zeros) ? sizeof(zeros) : (size_t)falta;
        if (fwrite(zeros, 1, k, velho) != k) { break; }
        falta -= (long)k;
      }
      fflush(velho);
      fsync(fileno(velho));
      fclose(velho);
    }
  }
  unlink(caminho);                          // o FAT nao troca por top: tira o antigo...
  if (rename(tmp, caminho) != 0) {          // ...e poe o novo no lugar
    ESP_LOGE(TAG, "nao consegui trocar %s pelo novo", caminho);
    return false;
  }
  return true;
}

// Um registro of diario no cartao (mounted). true = ja esta la of forma
// duravel (o diario so brand como aplicado depois disso).
static bool aplicar_one(const journal_entry_t* r, void*)
{
  char folder[96];
  if (!tab5_sd_folder(r->app, folder, sizeof(folder))) { return false; }
  char caminho[160];
  if (snprintf(caminho, sizeof(caminho), "%s/%s", folder, r->file) >= (int)sizeof(caminho)) {
    s_summary.discarded++;   // name que nao cabe nunca vai caber: nao trava o resto
    return true;
  }
  const uint32_t h = hash_text(caminho);

  if (r->type == JOURNAL_APPEND) {
    FILE* f = fopen(caminho, "a");
    if (!f) { return false; }
    // Nao aplicado = nada escrito: se falhar no storage (cartao cheio), o que
    // entrou e cortado of iteration - senao a nova tentativa repetiria o trecho.
    const long antes = ftell(f);
    bool ok = antes >= 0;
    if (ok && r->header && r->header_size && antes == 0) { ok = fwrite(r->header, 1, r->header_size, f) == r->header_size; }
    ok = ok && fwrite(r->data, 1, r->data_size, f) == r->data_size;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (!ok && antes >= 0) { fflush(f); ftruncate(fileno(f), antes); }
    fclose(f);
    if (!ok) { return false; }
    touch(h);
    s_summary.applied++;
    return true;
  }

  // SUBSTITUIR. O computador mexeu no file enquanto o cartao estava com
  // ele (nao bate com a foto)? Entao o dele stays, e o of Tab5 vai on lado
  // como "<name>.tab5" - nada se perde em silencio, e who decide e a pessoa.
  completar_troca(caminho);
  struct stat st;
  const bool existe = stat(caminho, &st) == 0;
  const bool igual = touched(h)
                  || (existe == r->base_existia
                      && (!existe || ((uint32_t)st.st_size == r->base_tamanho
                                      && (uint32_t)st.st_mtime == r->base_data)));
  if (igual) {
    if (!substituir_no_cartao(caminho, r->data, r->data_size, r->flags)) { return false; }
    touch(h);
    s_summary.applied++;
    return true;
  }
  char lado[176];
  snprintf(lado, sizeof(lado), "%s.tab5", caminho);
  if (!substituir_no_cartao(lado, r->data, r->data_size, r->flags)) { return false; }
  ESP_LOGW(TAG, "%s mudou no computador: o do Tab5 ficou em %s.tab5", caminho, r->file);
  s_summary.conflicts++;
  return true;
}

// Com s_gravando presa e o cartao of Tab5.
static void aplicar_diario(void)
{
  if (s_emprestado || !tab5_journal_pending()) { return; }
  if (!tab5_sd_mount()) { return; }   // sem cartao: stays para a next iteration
  s_n_tocados = 0;
  const int n = tab5_journal_apply(aplicar_one, nullptr);
  tab5_sd_unmount();
  if (n < 0) { ESP_LOGE(TAG, "diario ilegivel"); return; }
  if (!tab5_journal_pending()) {
    s_summary.complete = true;
    ESP_LOGI(TAG, "diario aplicado: %u gravacao(oes), %u conflito(s)",
             (unsigned)s_summary.applied, (unsigned)s_summary.conflicts);
  }
}

void tab5_sd_flush_pending(void)
{
  if (!s_anexo || !s_gravando) { return; }
  // BARREIRA: espera a gravacao QUE JA ESTA EM ANDAMENTO (a tarefa "cartao"
  // troca o buffer ANTES of write no file - sem wait, who chamava
  // aqui nesse intervalo lia sem a line que estava sendo escrita), grava o
  // pending e aplica o diario of one pendrive, nessa order: who le em
  // seguida ve everything o que ja foi entregue.
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  aplicar_diario();
  xSemaphoreGive(s_gravando);
}

bool tab5_sd_append(const char* app, const char* file, const void* data, size_t n)
{
  if (!s_gravando || !app || !file || (!data && n)) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  bool ok;
  if (para_o_diario()) {
    ok = diario_acrescentar(app, file, nullptr, 0, data, n);
  } else if (tab5_sd_mount()) {
    FILE* f = open_cru(app, file, "a");
    ok = f && (n == 0 || fwrite(data, 1, n, f) == n);
    if (f) { fclose(f); }
    tab5_sd_unmount();
  } else {
    ok = false;
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

bool tab5_sd_replace(const char* app, const char* file, const void* data, size_t n, uint8_t flags)
{
  if (!s_gravando || !app || !file || (!data && n)) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  // O que foi entregue para ACRESCENTAR antes desta troca vai antes dela.
  esvaziar_buffer();
  bool ok;
  if (para_o_diario()) {
    char caminho[160];
    caminho_of(app, file, caminho, sizeof(caminho));
    const foto_t* f = s_emprestado ? foto_of(caminho) : nullptr;
    journal_entry_t r = { };
    r.type = JOURNAL_REPLACE; r.flags = flags;
    r.app = app; r.file = file;
    r.data = data; r.data_size = (uint32_t)n;
    r.base_existia = f != nullptr;
    r.base_tamanho = f ? f->size : 0;
    r.base_data    = f ? f->data : 0;
    ok = tab5_journal_write(&r);
    if (!ok) { s_diario_perdidas = s_diario_perdidas + 1; }
  } else if (tab5_sd_mount()) {
    char folder[96], caminho[160];
    ok = tab5_sd_folder(app, folder, sizeof(folder))
      && snprintf(caminho, sizeof(caminho), "%s/%s", folder, file) < (int)sizeof(caminho)
      && substituir_no_cartao(caminho, data, n, flags);
    tab5_sd_unmount();
  } else {
    ok = false;
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

void tab5_sd_summary(tab5_sd_summary_t* r)
{
  *r = s_summary;
  r->in_journal  = 0;
  tab5_journal_usage(&r->journal_used, &r->journal_total, &r->in_journal);
  r->lost = s_diario_perdidas;
}

void tab5_sd_append_status(uint32_t* pending, uint32_t* written, uint32_t* lost)
{
  if (pending) { *pending = (uint32_t)s_usado; }
  if (written)  { *written  = s_gravadas; }
  if (lost)  { *lost  = s_perdidas; }
}

// 8, e nao 4: com o diario e as networks of no of Mesh, ja sao CINCO who pega
// piggyback (log, registro, mesh, mesh_diario, mesh_wifi_no). Com 4, o quinto a
// se registrar ficava sem - "caronas esgotadas" no boot, e o que ele so faz
// in piggyback (write a lista of networks of no, por exemplo) nunca acontecia.
#define CARONAS_MAX 8
static tab5_sd_piggyback_t s_caronas[CARONAS_MAX] = { };

bool tab5_sd_piggyback(tab5_sd_piggyback_t fn)
{
  for (int i = 0; i < CARONAS_MAX; ++i) {
    if (s_caronas[i] == fn) { return true; }
    if (!s_caronas[i]) { s_caronas[i] = fn; return true; }
  }
  ESP_LOGE(TAG, "caronas esgotadas (%d) - aumente CARONAS_MAX", CARONAS_MAX);
  return false;
}

static void vTaskCartao(void*)
{
  for (;;) {
    const bool request = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0;
    xSemaphoreTake(s_anexo, portMAX_DELAY);
    const bool ha = (s_usado > 0);
    const bool venceu = ha && (esp_timer_get_time() - s_since_us) >= (int64_t)ANEXO_PRAZO_MS * 1000;
    xSemaphoreGive(s_anexo);
    if (ha && (request || venceu)) { gravar_everything(); }
    // O diario of one pendrive vai para o cartao assim que ele estiver of
    // iteration (ou depois of one reinicio, when o cartao aparecer).
    // Falhou sem avancar nada (cartao cheio, sem cartao): espera antes of
    // next tentativa, em vez of bater no cartao a cada segundo.
    static int64_t next_us = 0;
    if (tab5_journal_pending() && !s_emprestado && (request || esp_timer_get_time() >= next_us)) {
      xSemaphoreTake(s_gravando, portMAX_DELAY);
      esvaziar_buffer();
      const uint32_t antes = s_summary.applied + s_summary.conflicts;
      aplicar_diario();
      const bool andou = s_summary.applied + s_summary.conflicts != antes;
      xSemaphoreGive(s_gravando);
      next_us = (andou || !tab5_journal_pending()) ? 0 : esp_timer_get_time() + 30LL * 1000 * 1000;
    }
    // As caronas gravam pelas portas (append/replace): seguem
    // valendo com o cartao lent - o target e decidido la dentro.
    for (int i = 0; i < CARONAS_MAX && s_caronas[i]; ++i) { s_caronas[i](); }
  }
}

static void gravacao_lock(bool lock)
{
  if (!s_gravando) { return; }
  if (lock) { xSemaphoreTake(s_gravando, portMAX_DELAY); }
  else        { xSemaphoreGive(s_gravando); }
}

static void tarefa_cartao_wake(void)
{
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}

// Chamado pelo tab5_sd_init (mesmo momento: antes of haver concorrencia).
static void gravador_start(void)
{
  if (s_anexo) { return; }
  s_buf[0] = (uint8_t*)heap_caps_malloc(ANEXO_BUF, MALLOC_CAP_SPIRAM);
  s_buf[1] = (uint8_t*)heap_caps_malloc(ANEXO_BUF, MALLOC_CAP_SPIRAM);
  s_gravando = xSemaphoreCreateMutex();
  SemaphoreHandle_t anexo = xSemaphoreCreateMutex();
  if (!s_buf[0] || !s_buf[1] || !s_gravando || !anexo) {
    ESP_LOGE(TAG, "gravador sem memoria - appends desligados");
    return;
  }
  // 6 KB: a tarefa MONTA o cartao, e o f_mount desce fundo in stack.
  xTaskCreatePinnedToCore(vTaskCartao, "cartao", 6144, nullptr, 2, &s_tarefa, tskNO_AFFINITY);
  tab5_owner_register(s_tarefa, OWNER_SERVICE, "Cartao");
  s_anexo = anexo;   // por ultimo: tab5_sd_enqueue_append so aceita com everything ready
}
