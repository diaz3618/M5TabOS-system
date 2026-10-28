#include "tab5_sd.hpp"
#include "tab5_marks.hpp"
#include "tab5_journal.hpp"
#include <errno.h>

#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"   // ff_diskio_get_pdrv_card - o drive do FatFs, para o rotulo
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

// ---- fiacao (M5Unified, _pin_table_sd, linha do board_M5Tab5) --------------
static const int SD_CLK = 43;
static const int SD_CMD = 44;
static const int SD_D0  = 39;
static const int SD_D1  = 40;
static const int SD_D2  = 41;
static const int SD_D3  = 42;

// Canal do LDO interno que alimenta a IO do SDMMC.
//
// O P4 declara SOC_SDMMC_IO_POWER_EXTERNAL: a alimentacao de IO do cartao NAO
// vem de um trilho fixo, ela sai de um LDO de dentro do chip que precisa ser
// ligado antes. O canal 4 (LDO_VO4) e o que as placas de referencia do P4 usam;
// o M5GFX ja usa o canal 3 para o PHY do DSI, o que reforca que o 4 esta livre
// para isto. Se a montagem falhar com erro de timeout, este numero e o primeiro
// suspeito.
static const int SD_LDO_CANAL = 4;

// ---- estado ----------------------------------------------------------------
static sdmmc_card_t*       s_card = nullptr;
static sd_pwr_ctrl_handle_t s_ldo = nullptr;
static int                 s_refs = 0;

// A contagem de referencia e mexida por tarefas diferentes: a interface (ao
// abrir e fechar app) e o registro de energia (a cada descarga). Sem trava, dois
// montar() simultaneos poderiam montar duas vezes, ou um desmontar() puxar o
// tapete de quem esta gravando. Mutex criado sob demanda, na primeira chamada.
static SemaphoreHandle_t   s_mutex = nullptr;

// O mutex NAO pode ser criado sob demanda dentro de trava(): duas tarefas
// chegando juntas na primeira vez criariam dois mutexes diferentes e cada uma
// travaria o seu - protecao nenhuma, e ainda vaza um. Criado uma vez em
// tab5_sd_iniciar(), antes de existir concorrencia.
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
// A descricao dizia "sem cartao" toda vez que o cartao era DESMONTADO - que e
// o estado normal quando nenhum app o esta usando, ja que a montagem e sob
// demanda e contada por referencia. O comando `servicos` entao anunciava "sem
// cartao" com o cartao encaixado e funcionando.
//
// Misturar "nao esta montado agora" com "nao existe" e o mesmo erro do WiFi
// dizendo "falhou" quando o coprocessador estava mudo: duas causas diferentes,
// uma mensagem so, e o usuario mandado investigar o lugar errado.
//
//   nao verificado      - ninguem tentou montar ainda neste boot
//   SD 14.5GB           - montado agora
//   SD 14.5GB (livre)   - ja montou, esta desmontado, o cartao continua la
//   ausente ou ilegivel - a ultima tentativa de montar falhou
static char s_ident[32]      = "";      // identidade, quando ja a conhecemos
static bool s_ja_montou      = false;   // alguma montagem deu certo neste boot
// O NOME DO CARTAO (rotulo do volume FAT), lido a cada montagem de verdade:
// renomeado no computador durante o pendrive, a volta remonta e le de novo.
// Fica valendo com o cartao desmontado. Vazio = sem nome.
static char s_rotulo[36]     = "";
static void arrumar_sistema(void);
static bool s_anunciou = false;   // a primeira montagem do boot ja foi para o log

// DESMONTAGEM ADIADA. Quando o ultimo usuario devolve o cartao,
// ele fica montado mais SD_OCIOSO_MS: o log, o monitor, o registro de energia
// e a trilha gravam em momentos proximos, e cada montagem liga o regulador e
// reinicializa o cartao. Com a folga, gravacoes vizinhas dividem UMA montagem;
// sem ninguem pedir de novo, desmonta e o regulador desliga. E o mesmo padrao
// de "timeout de ociosidade" do alto-falante (ARCHITECTURE.md, 1.1).
static const uint64_t   SD_OCIOSO_US = 10ULL * 1000 * 1000;
static esp_timer_handle_t s_ocioso = nullptr;
static void desmontar_de_verdade(void);

// Quando o cartao montou/desmontou DE VERDADE pela ultima vez (us de uptime).
// Diagnostico: o monitor cruza com o frame atrasado do painel (o cartao e
// alimentado por um LDO do proprio P4, vizinho do LDO do DSI).
static volatile int64_t s_montou_us = 0, s_desmontou_us = 0;
void tab5_sd_ultimas_transicoes(int64_t* montou_us, int64_t* desmontou_us)
{
  if (montou_us)    { *montou_us = s_montou_us; }
  if (desmontou_us) { *desmontou_us = s_desmontou_us; }
}
static void ao_ocioso(void*);
static bool s_ultima_falhou  = false;   // a ultima tentativa falhou
// SEM CARTAO, ESPERA CRESCENTE. Cada tentativa num slot vazio leva ~1 s de
// timeout com a trava presa, e todo mundo que grava (log, WiFi, notificacoes)
// pede montagem: sem freio eram 5 linhas de erro por segundo e um segundo de
// fila para cada um. Depois de uma falha, a proxima tentativa de verdade so
// sai depois de SD_ESPERA_MIN_MS, dobrando a cada nova falha ate SD_ESPERA_MAX_MS;
// entre uma e outra, montar devolve falha na hora. Cartao inserido com o
// aparelho ligado e achado em ate SD_ESPERA_MAX_MS.
#define SD_ESPERA_MIN_MS  2000
#define SD_ESPERA_MAX_MS 10000
static int64_t  s_proxima_us = 0;
static uint32_t s_espera_ms  = 0;
static char s_desc[64]       = "nao verificado";

static void gravador_iniciar(void);

// EMPRESTADO AO COMPUTADOR (modo pendrive, ver tab5_sd_emprestar no .hpp).
// Enquanto valer, o cartao e do computador: montar() recusa na hora, como
// se nao houvesse cartao, e o gravador segura o que chegar. s_cru e o mesmo
// cartao, inicializado SEM sistema de arquivos - so setores.
static volatile bool s_emprestado = false;
static sdmmc_card_t* s_cru = nullptr;
static tab5_sd_resumo_t s_resumo = { };   // o diario deste emprestimo (tab5_sd_resumo)

void tab5_sd_iniciar(void)
{
  if (!s_mutex) { s_mutex = xSemaphoreCreateMutex(); }
  // O diario ANTES do gravador: o que ficou de um pendrive interrompido por
  // um reinicio tem que estar visivel quando a primeira gravacao chegar.
  tab5_reserva_iniciar();
  gravador_iniciar();
  if (!s_ocioso) {
    const esp_timer_create_args_t a = { .callback = ao_ocioso, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "sd_ocioso",
                                        .skip_unhandled_events = true };
    esp_timer_create(&a, &s_ocioso);
  }

  // CALADO PARA SEMPRE, e nao so numa janela - o aviso inundava o console.
  // sd_pwr_ctrl_new_on_chip_ldo()
  // (chamada em montar_de_verdade() logo abaixo) sempre cria o controlador do
  // LDO com tensao 0 antes de programar a tensao real, e o driver do IDF avisa
  // isso em WARN toda vez - benigno (o cartao monta e le normal, a tensao real
  // e configurada logo em seguida), mas o cartao era montado/desmontado a cada
  // ~15s PARA SEMPRE (era o proprio log se realimentando - corrigido em
  // ver montar_de_verdade e a desmontagem adiada) - e nao so numa janela
  // rara como o "ledc" durante o acordar (tab5_power.cpp), entao nao ha
  // "depois" para restaurar o nivel: e a mesma logica de "eh_sdio"/"sdmmc_io"
  // em tab5_wifi_iniciar(), silenciados uma vez no boot.
  esp_log_level_set("ldo", ESP_LOG_ERROR);
  // As camadas de baixo repetem, a cada tentativa, o erro que a nossa ja da
  // (e so na mudanca de estado - ver montar_de_verdade).
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
}

bool tab5_sd_montado(void) { return s_card != nullptr; }
const char* tab5_sd_nome(void) { return s_rotulo; }

// O NOME DO VOLUME. O FAT32 guarda o rotulo em DOIS lugares: uma entrada
// especial na pasta raiz (a que o FatFs le, f_getlabel) e um campo do setor de
// boot do volume (offset 0x47, 11 bytes). Quem renomeia nem sempre escreve os
// dois - entao, sem a entrada na raiz, vale o do setor de boot. "NO NAME" ali
// e o "sem nome" do formatador. Os dois em maiusculas (e o que o FAT guarda).
static void ler_rotulo(void)
{
  s_rotulo[0] = 0;
  const char drv[3] = { (char)('0' + ff_diskio_get_pdrv_card(s_card)), ':', 0 };
  char r[sizeof(s_rotulo)] = "";
  if (f_getlabel(drv, r, nullptr) == FR_OK && r[0]) {
    snprintf(s_rotulo, sizeof(s_rotulo), "%s", r);
    return;
  }
  // O setor de boot do volume: o 0 do cartao, ou o inicio da 1a particao
  // (cartao com tabela de particoes - o normal, formatado no computador).
  // Lido cru e nao pelo f_getfree, que num cartao grande pode varrer a FAT.
  uint8_t* setor = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!setor) { return; }
  bool ok = sdmmc_read_sectors(s_card, setor, 0, 1) == ESP_OK && setor[510] == 0x55 && setor[511] == 0xAA;
  if (ok && memcmp(setor + 0x52, "FAT32", 5) != 0) {
    const uint32_t inicio = setor[0x1C6] | (setor[0x1C7] << 8) | (setor[0x1C8] << 16) | ((uint32_t)setor[0x1C9] << 24);
    ok = inicio && sdmmc_read_sectors(s_card, setor, inicio, 1) == ESP_OK && setor[510] == 0x55
         && setor[511] == 0xAA && memcmp(setor + 0x52, "FAT32", 5) == 0;
  }
  if (ok) {
    char b[12];
    memcpy(b, setor + 0x47, 11);
    b[11] = 0;
    for (int i = 10; i >= 0 && b[i] == ' '; --i) { b[i] = 0; }   // o campo vem completado com espacos
    if (b[0] && strcmp(b, "NO NAME") != 0) { snprintf(s_rotulo, sizeof(s_rotulo), "%s", b); }
  }
  heap_caps_free(setor);
}

const char* tab5_sd_descricao(void)
{
  if (s_emprestado)         { snprintf(s_desc, sizeof(s_desc), "no computador (USB)"); }
  else if (s_card)          { snprintf(s_desc, sizeof(s_desc), "%s", s_ident); }
  else if (s_ultima_falhou) { snprintf(s_desc, sizeof(s_desc), "ausente ou ilegivel"); }
  else if (s_ja_montou)     { snprintf(s_desc, sizeof(s_desc), "%s (livre)", s_ident); }
  else                      { snprintf(s_desc, sizeof(s_desc), "nao verificado"); }
  return s_desc;
}

// Host SDMMC e slot do cartao, com o LDO (s_ldo) ja ligado amarrado no host.
// Usado pela montagem normal e pelo acesso cru do pendrive - os dois TEM que
// falar com o cartao do mesmo jeito (slot 0, ver o aviso abaixo).
static void configurar_host(sdmmc_host_t* host, sdmmc_slot_config_t* slot)
{
  *host = SDMMC_HOST_DEFAULT();

  // SLOT 0, E ISTO NAO E DETALHE.
  //
  // O SDMMC_HOST_DEFAULT() vem com .slot = SDMMC_HOST_SLOT_1
  // (sdmmc_default_configs.h:31), e o esp_hosted usa o SLOT 1 para falar com o
  // ESP32-C6 do WiFi (CONFIG_ESP_HOSTED_HOST_SDIO_SLOT=1). Com os dois no mesmo
  // slot, montar o cartao reprograma os pinos do slot e ARRANCA O C6 DO
  // BARRAMENTO. Foi exatamente o que aconteceu:
  //
  //   wifi: C6 no ar, firmware 1.4.1            <- link com o C6 de pe
  //   sdmmc_periph: SDMMC host already initialized, skipping initflow
  //   sdmmc_io: sdmmc_send_cmd returned 0x107   <- C6 morto, 1ms depois
  //   eh_sdio: Unrecoverable host sdio state    -> reinicio da placa
  //
  // O P4 tem dois slots (SOC_SDMMC_NUM_SLOTS=2) e roteia por GPIO matrix
  // (SOC_SDMMC_USE_GPIO_MATRIX=1), entao qualquer pino serve em qualquer slot.
  // Mudamos o NOSSO lado e nao o do esp_hosted, para nao mexer no preset de
  // placa que a Espressif mantem.
  host->slot = SDMMC_HOST_SLOT_0;
  host->pwr_ctrl_handle = s_ldo;

  // Slot em 4 bits, com os pinos do Tab5. O P4 tem GPIO matrix para o
  // SDMMC, entao da para remapear livremente.
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

static bool montar_de_verdade(void)
{
  // 1. Liga o LDO que alimenta a IO do cartao.
  sd_pwr_ctrl_ldo_config_t ldo_cfg = { };
  ldo_cfg.ldo_chan_id = SD_LDO_CANAL;
  esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_ldo);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LDO do cartao (canal %d) falhou: %s", SD_LDO_CANAL, esp_err_to_name(err));
    return false;
  }

  // 2. Host SDMMC e slot - a mesma configuracao do acesso cru (pendrive).
  sdmmc_host_t host;
  sdmmc_slot_config_t slot;
  configurar_host(&host, &slot);

  esp_vfs_fat_sdmmc_mount_config_t mnt = { };
  // NUNCA formatar sozinho. Um cartao que nao monta pode estar apenas com o
  // sistema de arquivos errado, e formatar destruiria o que o usuario tem la.
  // Se um dia precisar formatar, que seja um botao explicito num app.
  mnt.format_if_mount_failed = false;
  mnt.max_files              = 8;
  mnt.allocation_unit_size   = 16 * 1024;

  tab5_marco("sd: montando", 0);
  err = esp_vfs_fat_sdmmc_mount(TAB5_SD_RAIZ, &host, &slot, &mnt, &s_card);
  if (err != ESP_OK) {
    // So a PRIMEIRA falha de uma sequencia vai ao log; as repeticoes do
    // mesmo estado sao depuracao.
    if (!s_ultima_falhou) {
      if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "cartao presente mas sem sistema de arquivos FAT legivel");
      } else {
        ESP_LOGE(TAG, "montagem falhou: %s (cartao ausente ou mal encaixado?) - tento de novo em ate %d s",
                 esp_err_to_name(err), SD_ESPERA_MAX_MS / 1000);
      }
    } else {
      ESP_LOGD(TAG, "montagem falhou de novo: %s", esp_err_to_name(err));
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

  ler_rotulo();

  // Garante a raiz dos aplicativos. mkdir devolvendo EEXIST nao e erro.
  mkdir(TAB5_SD_APPS, 0777);

  // MONTAR NAO E NOTICIA. Estas linhas iam para o log, o log e
  // gravado NO CARTAO, e gravar exige montar: cada descarga do log gerava as
  // duas linhas que obrigavam a proxima descarga - o cartao montava e
  // desmontava a cada 15s para sempre, gravando que tinha gravado. So a
  // primeira montagem do boot fica visivel; o resto e depuracao.
  if (!s_anunciou) {
    ESP_LOGI(TAG, "montado em %s: %s, nome '%s'", TAB5_SD_RAIZ, s_ident, s_rotulo);
    s_anunciou = true;
    arrumar_sistema();
  }
  else             { ESP_LOGD(TAG, "montado"); }
  s_montou_us = esp_timer_get_time();
  return true;
}

// A PASTA SISTEMA EM SUBPASTAS. Os logs do dia e os arquivos de
// energia iam todos soltos em apps/Sistema e afogavam o resto (wifi.txt,
// firmware do C6). Agora cada um tem a sua: TAB5_SD_LOGS e TAB5_SD_ENERGIA.
// O que ja estava no cartao do jeito antigo e movido UMA vez por boot, na
// primeira montagem - com nada a mover, custa so listar a pasta.
static bool tem_prefixo(const char* s, const char* p) { return strncasecmp(s, p, strlen(p)) == 0; }

static void mover_para(const char* base, const char* nome, const char* sub, int* n)
{
  char dest_pasta[96];
  if (!tab5_sd_pasta(sub, dest_pasta, sizeof(dest_pasta))) { return; }
  char de[200], para[200];
  snprintf(de, sizeof(de), "%s/%s", base, nome);
  snprintf(para, sizeof(para), "%s/%s", dest_pasta, nome);
  struct stat st;
  if (stat(para, &st) == 0) {
    ESP_LOGW(TAG, "%s ja existe em %s - deixei o antigo onde estava", nome, sub);
    return;
  }
  if (rename(de, para) == 0) { ++*n; }
  else { ESP_LOGW(TAG, "nao consegui mover %s: %s", nome, strerror(errno)); }
}

static void arrumar_sistema(void)
{
  char base[96];
  snprintf(base, sizeof(base), "%s/Sistema", TAB5_SD_APPS);
  DIR* d = opendir(base);
  if (!d) { return; }
  // Junta os nomes antes de mover: renomear durante o readdir da mesma pasta
  // pode pular ou repetir entradas no FAT.
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
    mover_para(base, nomes[i], tem_prefixo(nomes[i], "log_") ? TAB5_SD_LOGS : TAB5_SD_ENERGIA, &n);
  }
  heap_caps_free(nomes);
  if (n) { ESP_LOGI(TAG, "Sistema arrumado: %d arquivo(s) movidos para logs/ e energia/", n); }
}

bool tab5_sd_montar(void)
{
  trava();
  bool ok;
  if (s_emprestado) {
    // O computador esta com o cartao: recusa calado, sem a espera crescente
    // de "sem cartao" - ele volta inteiro quando o pendrive acabar.
    ok = false;
  } else if (s_card) {
    // Ja montado - talvez so esperando o prazo de ociosidade: cancela o prazo
    // e conta. E aqui que a montagem e reaproveitada.
    if (s_refs == 0 && s_ocioso) { esp_timer_stop(s_ocioso); }
    s_refs++;
    ok = true;
  } else if (s_ultima_falhou && esp_timer_get_time() < s_proxima_us) {
    ok = false;                               // em espera depois de falhar: nem tenta
  } else if (montar_de_verdade()) {
    s_refs = 1;
    s_espera_ms = 0;
    ok = true;
  } else {
    ok = false;                               // falhou: NAO incrementa
    s_espera_ms = s_espera_ms ? s_espera_ms * 2 : SD_ESPERA_MIN_MS;
    if (s_espera_ms > SD_ESPERA_MAX_MS) { s_espera_ms = SD_ESPERA_MAX_MS; }
    s_proxima_us = esp_timer_get_time() + (int64_t)s_espera_ms * 1000;
  }
  destrava();
  return ok;
}

// Chamada com a trava presa.
static void desmontar_de_verdade(void)
{
  tab5_marco("sd: desmontando", 0);
  esp_vfs_fat_sdcard_unmount(TAB5_SD_RAIZ, s_card);
  s_card = nullptr;
  if (s_ldo) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; }
  // A identidade NAO e apagada: o cartao continua encaixado, so nao esta
  // montado. Apagar aqui era o que fazia o `servicos` anunciar "sem cartao"
  // com o cartao no lugar.
  ESP_LOGD(TAG, "desmontado");
  s_desmontou_us = esp_timer_get_time();
}

// O prazo de ociosidade venceu (tarefa do esp_timer). Se ninguem pegou o
// cartao de volta nesse meio tempo, agora desmonta.
static void ao_ocioso(void*)
{
  trava();
  if (s_refs == 0 && s_card) { desmontar_de_verdade(); }
  destrava();
}

void tab5_sd_desmontar(void)
{
  trava();
  if (s_refs <= 0 || --s_refs > 0) {          // ninguem montou, ou ainda ha quem use
    destrava();
    return;
  }
  // O ultimo devolveu: nao desmonta ja - arma o prazo (ou o reinicia).
  if (s_ocioso) {
    esp_timer_stop(s_ocioso);
    esp_timer_start_once(s_ocioso, SD_OCIOSO_US);
  } else {
    desmontar_de_verdade();
  }
  destrava();
}

// ===========================================================================
// EMPRESTIMO AO COMPUTADOR (modo pendrive) - ver o .hpp
// ===========================================================================
static void tarefa_cartao_acordar(void);
static void gravacao_travar(bool travar);
static void esvaziar_buffer(void);   // com a trava de gravacao presa
static void aplicar_diario(void);    // idem

// ---------------------------------------------------------------------------
// A FOTO DO CARTAO NO EMPRESTIMO
// ---------------------------------------------------------------------------
// Tamanho e data de cada arquivo de apps/ no instante em que o cartao sai. Um
// arquivo SUBSTITUIDO durante o pendrive leva a foto dele junto para o diario
// (a foto em si fica so na RAM: o registro e que precisa sobreviver a um
// reinicio). Na volta, arquivo diferente da foto = o computador mexeu nele.
typedef struct { uint32_t hash, tam, data; } foto_t;
#define FOTO_MAX 4096
static foto_t* s_foto = nullptr;
static int     s_foto_n = 0;

static uint32_t hash_texto(const char* s)
{
  uint32_t h = 2166136261u;   // FNV-1a
  while (*s) { h = (h ^ (uint8_t)*s++) * 16777619u; }
  return h;
}

static void fotografar_pasta(const char* pasta, int nivel)
{
  DIR* d = opendir(pasta);
  if (!d) { return; }
  while (struct dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') { continue; }
    char caminho[192];
    if (snprintf(caminho, sizeof(caminho), "%s/%s", pasta, e->d_name) >= (int)sizeof(caminho)) { continue; }
    if (e->d_type == DT_DIR) {
      if (nivel < 4) { fotografar_pasta(caminho, nivel + 1); }
      continue;
    }
    if (s_foto_n >= FOTO_MAX) { continue; }   // alem disso: sem foto, a volta trata como conflito
    struct stat st;
    if (stat(caminho, &st) != 0) { continue; }
    s_foto[s_foto_n++] = { hash_texto(caminho), (uint32_t)st.st_size, (uint32_t)st.st_mtime };
  }
  closedir(d);
}

// Com o cartao montado e a trava presa.
static void fotografar(void)
{
  if (!s_foto) { s_foto = (foto_t*)heap_caps_malloc(FOTO_MAX * sizeof(foto_t), MALLOC_CAP_SPIRAM); }
  s_foto_n = 0;
  if (!s_foto) { return; }
  fotografar_pasta(TAB5_SD_APPS, 0);
}

static const foto_t* foto_de(const char* caminho)
{
  const uint32_t h = hash_texto(caminho);
  for (int i = 0; i < s_foto_n; ++i) { if (s_foto[i].hash == h) { return &s_foto[i]; } }
  return nullptr;
}

// A FOTO SO SERVE ENQUANTO O CARTAO ESTA FORA: cada registro SUBSTITUIR do
// diario ja leva a sua base junto. Solta no fim do emprestimo (ou quando ele
// desiste) - 48 KB de PSRAM que ficavam presos ate o reinicio. Com a trava
// de gravacao presa: e com ela que o substituir le a foto.
static void foto_soltar(void)
{
  heap_caps_free(s_foto);
  s_foto = nullptr;
  s_foto_n = 0;
}

static void cru_soltar(void)
{
  if (!s_cru) { return; }
  // O mesmo desfazer do esp_vfs_fat_sdcard_unmount: so o SLOT 0 (o slot 1 e
  // o SDIO do C6, e um deinit do host inteiro o derrubaria).
  if (s_cru->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) { s_cru->host.deinit_p(s_cru->host.slot); }
  else                                                { s_cru->host.deinit(); }
  free(s_cru);
  s_cru = nullptr;
  if (s_ldo) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; }
}

// O cartao sem sistema de arquivos: LDO, host, slot e a inicializacao do
// cartao - o que o esp_vfs_fat_sdmmc_mount faz antes do f_mount. Com a trava.
static bool cru_preparar(void)
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
    cru_soltar();
    return false;
  }
  return true;
}

tab5_sd_emprestimo_t tab5_sd_emprestar(uint32_t espera_ms)
{
  if (s_emprestado) { return s_cru ? SD_EMPRESTADO : SD_EMPRESTIMO_SEM_CARTAO; }
  // Sem o diario, o que o sistema gravasse durante o pendrive se perderia.
  uint32_t total = 0;
  tab5_reserva_uso(nullptr, &total, nullptr);
  if (total == 0) { return SD_EMPRESTIMO_SEM_DIARIO; }
  s_resumo = { };   // o resumo e deste emprestimo

  // 1. Com a trava de gravacao presa do comeco ao fim deste passo, nada e
  //    gravado no meio dele:
  //    - o que ja foi entregue (e um diario antigo) vai para o cartao AGORA,
  //      enquanto ele ainda e do Tab5;
  //    - a FOTO do cartao (tamanho e data de cada arquivo) e tirada - e com
  //      ela que a volta reconhece o que o computador mudou;
  //    - montar() passa a recusar, e as portas passam a gravar no diario.
  gravacao_travar(true);
  esvaziar_buffer();
  aplicar_diario();
  trava();
  if (!s_card) { montar_de_verdade(); }   // so para a foto (sem referencia)
  if (s_card) { fotografar(); } else { s_foto_n = 0; }
  s_emprestado = true;
  destrava();
  gravacao_travar(false);

  // 2. Espera quem JA estava com o cartao devolver. Os servicos seguram por
  //    milissegundos (abrir, escrever, fechar); quem passar do prazo esta
  //    usando de verdade, e emprestar agora seria tirar o cartao de baixo dele.
  const int64_t limite = esp_timer_get_time() + (int64_t)espera_ms * 1000;
  for (;;) {
    trava();
    const int refs = s_refs;
    destrava();
    if (refs == 0) { break; }
    if (esp_timer_get_time() > limite) {
      ESP_LOGW(TAG, "pendrive: cartao em uso (%d), nao emprestado", refs);
      gravacao_travar(true);
      trava();
      s_emprestado = false;
      foto_soltar();
      // A montagem da foto nao tem dono: volta ao prazo de ociosidade.
      if (s_refs == 0 && s_card && s_ocioso) { esp_timer_start_once(s_ocioso, SD_OCIOSO_US); }
      destrava();
      gravacao_travar(false);
      tarefa_cartao_acordar();   // o que caiu no diario na espera vai para o cartao
      return SD_EMPRESTIMO_EM_USO;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // 3. Desmonta de verdade - a montagem ociosa tambem: o FAT do Tab5 nao
  //    pode ter NADA em cache quando o computador comecar a escrever. Entao
  //    o cartao cru.
  gravacao_travar(true);
  trava();
  if (s_ocioso) { esp_timer_stop(s_ocioso); }
  if (s_card) { desmontar_de_verdade(); }
  const bool ok = cru_preparar();
  if (!ok) { s_emprestado = false; foto_soltar(); }
  destrava();
  gravacao_travar(false);
  if (!ok) { tarefa_cartao_acordar(); return SD_EMPRESTIMO_SEM_CARTAO; }
  ESP_LOGI(TAG, "cartao emprestado ao computador: %s (%d arquivo(s) fotografado(s))",
           s_ident[0] ? s_ident : "SD", s_foto_n);
  return SD_EMPRESTADO;
}

sdmmc_card_t* tab5_sd_cru(void) { return s_emprestado ? s_cru : nullptr; }

void tab5_sd_devolver(void)
{
  // A trava de gravacao ANTES da do cartao (a ordem de todas as portas): o
  // substituir le a foto com ela presa, e a foto sai daqui junto com o
  // emprestimo.
  gravacao_travar(true);
  trava();
  if (!s_emprestado) { destrava(); gravacao_travar(false); return; }
  cru_soltar();
  s_emprestado = false;
  // A proxima montagem e normal: nada de espera de "sem cartao" herdada.
  s_ultima_falhou = false;
  s_espera_ms = 0;
  foto_soltar();
  destrava();
  gravacao_travar(false);
  ESP_LOGI(TAG, "cartao devolvido ao Tab5");
  tarefa_cartao_acordar();   // aplica o diario (vTaskCartao)
}

bool tab5_sd_emprestado(void) { return s_emprestado; }

// Nome de app -> nome de pasta seguro em FAT. Troca o que pode dar problema por
// '_' em vez de recusar: e melhor gravar em "Mapa_Azimutal" do que nao gravar.
static void nome_seguro(const char* origem, char* destino, size_t tam)
{
  size_t j = 0;
  for (size_t i = 0; origem[i] && j + 1 < tam; ++i) {
    const char c = origem[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9') || c == '-' || c == '_';
    destino[j++] = ok ? c : '_';
  }
  destino[j] = '\0';
  if (j == 0) { snprintf(destino, tam, "app"); }
}

bool tab5_sd_pasta(const char* app, char* out, size_t out_tam)
{
  if (!s_card || !app || !out) { return false; }

  // SUBPASTAS: "Sistema/logs" vira apps/Sistema/logs. Cada trecho
  // entre barras passa pelo nome_seguro e cada nivel e criado se faltar.
  size_t n = (size_t)snprintf(out, out_tam, "%s", TAB5_SD_APPS);
  const char* p = app;
  while (*p) {
    const char* fim = strchr(p, '/');
    const size_t tam = fim ? (size_t)(fim - p) : strlen(p);
    if (tam > 0) {
      char trecho[32], seguro[32];
      snprintf(trecho, sizeof(trecho), "%.*s", (int)tam, p);
      nome_seguro(trecho, seguro, sizeof(seguro));
      n += (size_t)snprintf(out + n, n < out_tam ? out_tam - n : 0, "/%s", seguro);
      if (n >= out_tam) { return false; }

      struct stat st;
      if (stat(out, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) { return false; }
      } else if (mkdir(out, 0777) != 0) {
        ESP_LOGE(TAG, "nao consegui criar %s", out);
        return false;
      }
    }
    if (!fim) { break; }
    p = fim + 1;
  }
  return true;
}

static FILE* abrir_cru(const char* app, const char* arquivo, const char* modo);

FILE* tab5_sd_abrir(const char* app, const char* arquivo, const char* modo)
{
  // Ler (ou escrever por fora das portas) tem que ver tudo o que ja foi
  // entregue: o pendente do gravador e o diario de um pendrive.
  tab5_sd_gravar_pendentes();
  return abrir_cru(app, arquivo, modo);
}

// TROCA INTERROMPIDA. Substituir um arquivo no FAT e: grava "<nome>.tmp"
// inteiro (e fsync), apaga o antigo, renomeia o .tmp - o FAT nao troca por
// cima. Uma queda de energia entre apagar e renomear deixa o arquivo SUMIDO e
// o conteudo novo, inteiro, no .tmp. Quem for ler encontra isso e completa a
// troca. (Um .tmp com o arquivo ainda la e uma troca que nem comecou a apagar:
// fica, e a proxima troca o sobrescreve.)
static void completar_troca(const char* caminho)
{
  struct stat st;
  if (stat(caminho, &st) == 0) { return; }
  char tmp[200];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", caminho) >= (int)sizeof(tmp)) { return; }
  if (stat(tmp, &st) != 0) { return; }
  if (rename(tmp, caminho) == 0) { ESP_LOGW(TAG, "%s: troca interrompida por queda de energia - completada", caminho); }
}

static FILE* abrir_cru(const char* app, const char* arquivo, const char* modo)
{
  char pasta[96];
  if (!tab5_sd_pasta(app, pasta, sizeof(pasta))) { return nullptr; }

  char caminho[160];
  snprintf(caminho, sizeof(caminho), "%s/%s", pasta, arquivo);

  completar_troca(caminho);
  FILE* f = fopen(caminho, modo);
  if (!f)
  {
    // NEM TODA FALHA E ERRO, e o servico e quem tem que saber a diferenca.
    //
    // Abrir para LEITURA um arquivo que ainda nao existe e resultado legitimo e
    // previsivel - acontece toda vez que um app le algo pela primeira vez (o
    // historico de uma conversa nova do Meshtastic, por exemplo). Registrar
    // isso como ERRO enche o log de alarme falso, e log que grita a toa ensina
    // a ignorar log.
    //
    // Ja falha ao ABRIR PARA ESCRITA, ou qualquer outro errno na leitura
    // (permissao, cartao corrompido, caminho invalido), e problema de verdade e
    // continua em ERRO.
    const bool so_leitura = (modo[0] == 'r');
    if (so_leitura && errno == ENOENT) {
      ESP_LOGD(TAG, "'%s' ainda nao existe", caminho);
    } else {
      ESP_LOGE(TAG, "fopen('%s','%s') falhou: %s", caminho, modo, strerror(errno));
    }
  }
  return f;
}


bool tab5_sd_espaco(uint64_t* total, uint64_t* livre)
{
  if (!s_card) { return false; }
  // O newlib do ESP nao tem statvfs; o IDF expoe isto pelo proprio VFS do FAT.
  return esp_vfs_fat_info(TAB5_SD_RAIZ, total, livre) == ESP_OK;
}

// ===========================================================================
// O GRAVADOR (ver o .hpp)
// ===========================================================================
#include "freertos/task.h"
#include "tab5_owners.hpp"

#define ANEXO_BUF      (16 * 1024)   // cada um dos dois buffers, na PSRAM
#define ANEXO_PRAZO_MS 5000          // o que nao e urgente espera no maximo isto
#define ANEXO_APP      32
#define ANEXO_ARQ      40

typedef struct {
  uint16_t tam;          // registro inteiro, cabecalho incluso
  uint16_t n_cab, n_txt;
  char     app[ANEXO_APP];
  char     arq[ANEXO_ARQ];
  // seguem n_cab bytes de cabecalho e n_txt de texto
} anexo_t;

// DOIS buffers: quem entrega escreve no ativo; a gravacao troca e grava o
// outro SEM a trava - entregar nunca espera o cartao.
static uint8_t*          s_buf[2]   = { nullptr, nullptr };
static int               s_ativo    = 0;
static size_t            s_usado    = 0;
static int64_t           s_desde_us = 0;      // quando chegou o mais antigo pendente
static SemaphoreHandle_t s_anexo    = nullptr; // o buffer ativo
static SemaphoreHandle_t s_gravando = nullptr; // uma gravacao por vez
static TaskHandle_t      s_tarefa   = nullptr;
static volatile uint32_t s_gravadas = 0, s_perdidas = 0;

bool tab5_sd_anexar(const char* app, const char* arquivo, const char* texto,
                    const char* cabecalho, bool urgente)
{
  if (!s_anexo || !app || !arquivo || !texto) { return false; }
  const size_t nc = cabecalho ? strlen(cabecalho) : 0;
  const size_t nt = strlen(texto);
  const size_t tam = sizeof(anexo_t) + nc + nt;
  if (tam > ANEXO_BUF / 2) { s_perdidas = s_perdidas + 1; return false; }

  xSemaphoreTake(s_anexo, portMAX_DELAY);   // so memcpy aqui dentro
  const bool cabe = (s_usado + tam <= ANEXO_BUF);
  if (cabe) {
    anexo_t* a = (anexo_t*)(s_buf[s_ativo] + s_usado);
    a->tam = (uint16_t)tam; a->n_cab = (uint16_t)nc; a->n_txt = (uint16_t)nt;
    snprintf(a->app, sizeof(a->app), "%s", app);
    snprintf(a->arq, sizeof(a->arq), "%s", arquivo);
    uint8_t* p = (uint8_t*)(a + 1);
    if (nc) { memcpy(p, cabecalho, nc); }
    memcpy(p + nc, texto, nt);
    if (s_usado == 0) { s_desde_us = esp_timer_get_time(); }
    s_usado += tam;
  }
  const bool metade = (s_usado > ANEXO_BUF / 2);
  xSemaphoreGive(s_anexo);

  if (!cabe) { s_perdidas = s_perdidas + 1; }
  if ((urgente || metade) && s_tarefa) { xTaskNotifyGive(s_tarefa); }
  return cabe;
}

// ---------------------------------------------------------------------------
// O DESTINO DE CADA GRAVACAO
// ---------------------------------------------------------------------------
// Cartao, ou o diario na flash (hal/tab5_journal) - decidido SEMPRE com a
// trava de gravacao presa, a mesma do emprestimo e da aplicacao do diario.
// Enquanto o diario tiver algo pendente, TUDO vai para ele, mesmo com o
// cartao ja de volta: gravar direto no cartao passaria na frente do que
// aconteceu antes e esta esperando.
static bool para_o_diario(void) { return s_emprestado || tab5_reserva_pendente(); }

static volatile uint32_t s_diario_perdidas = 0;   // nao couberam no diario

static bool diario_acrescentar(const char* app, const char* arq,
                               const void* cab, size_t n_cab, const void* dados, size_t n)
{
  reserva_reg_t r = { };
  r.tipo = RESERVA_ACRESCENTAR;
  r.app = app; r.arquivo = arq;
  r.cabecalho = cab; r.n_cabecalho = (uint16_t)n_cab;
  r.dados = dados; r.n_dados = (uint32_t)n;
  if (tab5_reserva_gravar(&r)) { return true; }
  s_diario_perdidas = s_diario_perdidas + 1;
  return false;
}

// O caminho no cartao de app/arquivo, SEM criar pasta (para a foto e o hash).
static void caminho_de(const char* app, const char* arq, char* out, size_t tam)
{
  size_t n = (size_t)snprintf(out, tam, "%s", TAB5_SD_APPS);
  const char* p = app;
  while (*p && n < tam) {
    const char* fim = strchr(p, '/');
    const size_t k = fim ? (size_t)(fim - p) : strlen(p);
    if (k > 0) {
      char trecho[32], seguro[32];
      snprintf(trecho, sizeof(trecho), "%.*s", (int)k, p);
      nome_seguro(trecho, seguro, sizeof(seguro));
      n += (size_t)snprintf(out + n, n < tam ? tam - n : 0, "/%s", seguro);
    }
    if (!fim) { break; }
    p = fim + 1;
  }
  if (n < tam) { snprintf(out + n, tam - n, "/%s", arq); }
}

// Grava um buffer ja trocado: cada arquivo aberto UMA vez, as linhas dele na
// ordem de chegada. Chamado com s_gravando presa.
static void gravar_buffer(uint8_t* b, size_t n)
{
  if (n == 0) { return; }
  if (para_o_diario()) {
    for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->tam) {
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
  if (!tab5_sd_montar()) {
    for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->tam) { s_perdidas = s_perdidas + 1; }
    return;
  }
  // Marca os ja gravados zerando o 'app' (o registro continua no lugar, so
  // deixa de valer) - varre de novo para o proximo arquivo distinto.
  for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->tam) {
    anexo_t* a = (anexo_t*)(b + o);
    if (!a->app[0]) { continue; }
    char app[ANEXO_APP], arq[ANEXO_ARQ];
    memcpy(app, a->app, sizeof(app));
    memcpy(arq, a->arq, sizeof(arq));
    FILE* f = abrir_cru(app, arq, "a");
    bool cabecalho_feito = false;
    for (size_t k = o; k < n; k += ((anexo_t*)(b + k))->tam) {
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
  tab5_sd_desmontar();
}

// Troca o buffer e grava o cheio. Com s_gravando presa.
static void esvaziar_buffer(void)
{
  if (!s_anexo) { return; }
  xSemaphoreTake(s_anexo, portMAX_DELAY);
  uint8_t* cheio = s_buf[s_ativo];
  const size_t n = s_usado;
  s_ativo = 1 - s_ativo;
  s_usado = 0;
  xSemaphoreGive(s_anexo);
  gravar_buffer(cheio, n);
}

static void gravar_tudo(void)
{
  if (!s_gravando) { return; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  xSemaphoreGive(s_gravando);
}

// ---------------------------------------------------------------------------
// APLICAR O DIARIO NO CARTAO
// ---------------------------------------------------------------------------
// Arquivos que ESTA aplicacao ja escreveu: mudaram por causa dela, nao do
// computador - um segundo SUBSTITUIR do mesmo arquivo nao e conflito.
#define TOCADOS_MAX 64
static uint32_t s_tocados[TOCADOS_MAX];
static int      s_n_tocados = 0;
static bool tocado(uint32_t h)
{
  for (int i = 0; i < s_n_tocados; ++i) { if (s_tocados[i] == h) { return true; } }
  return false;
}
static void tocar(uint32_t h) { if (!tocado(h) && s_n_tocados < TOCADOS_MAX) { s_tocados[s_n_tocados++] = h; } }

// Substitui o arquivo inteiro no cartao, sem nunca deixar meio arquivo: grava
// "<nome>.tmp", e so entao troca pelo antigo. SIGILOSO zera o antigo antes de
// apagar - no FAT apagar so marca os setores como livres, e o conteudo antigo
// ficaria legivel por qualquer programa de recuperacao. Cartao montado.
static bool substituir_no_cartao(const char* caminho, const void* dados, size_t n, uint8_t flags)
{
  char tmp[200];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", caminho) >= (int)sizeof(tmp)) { return false; }
  FILE* f = fopen(tmp, "wb");
  if (!f) { ESP_LOGE(TAG, "fopen('%s') falhou: %s", tmp, strerror(errno)); return false; }
  bool ok = (n == 0) || fwrite(dados, 1, n, f) == n;
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
  unlink(caminho);                          // o FAT nao troca por cima: tira o antigo...
  if (rename(tmp, caminho) != 0) {          // ...e poe o novo no lugar
    ESP_LOGE(TAG, "nao consegui trocar %s pelo novo", caminho);
    return false;
  }
  return true;
}

// Um registro do diario no cartao (montado). true = ja esta la de forma
// duravel (o diario so marca como aplicado depois disso).
static bool aplicar_um(const reserva_reg_t* r, void*)
{
  char pasta[96];
  if (!tab5_sd_pasta(r->app, pasta, sizeof(pasta))) { return false; }
  char caminho[160];
  if (snprintf(caminho, sizeof(caminho), "%s/%s", pasta, r->arquivo) >= (int)sizeof(caminho)) {
    s_resumo.descartadas++;   // nome que nao cabe nunca vai caber: nao trava o resto
    return true;
  }
  const uint32_t h = hash_texto(caminho);

  if (r->tipo == RESERVA_ACRESCENTAR) {
    FILE* f = fopen(caminho, "a");
    if (!f) { return false; }
    // Nao aplicado = nada escrito: se falhar no meio (cartao cheio), o que
    // entrou e cortado de volta - senao a nova tentativa repetiria o trecho.
    const long antes = ftell(f);
    bool ok = antes >= 0;
    if (ok && r->cabecalho && r->n_cabecalho && antes == 0) { ok = fwrite(r->cabecalho, 1, r->n_cabecalho, f) == r->n_cabecalho; }
    ok = ok && fwrite(r->dados, 1, r->n_dados, f) == r->n_dados;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (!ok && antes >= 0) { fflush(f); ftruncate(fileno(f), antes); }
    fclose(f);
    if (!ok) { return false; }
    tocar(h);
    s_resumo.aplicadas++;
    return true;
  }

  // SUBSTITUIR. O computador mexeu no arquivo enquanto o cartao estava com
  // ele (nao bate com a foto)? Entao o dele fica, e o do Tab5 vai ao lado
  // como "<nome>.tab5" - nada se perde em silencio, e quem decide e a pessoa.
  completar_troca(caminho);
  struct stat st;
  const bool existe = stat(caminho, &st) == 0;
  const bool igual = tocado(h)
                  || (existe == r->base_existia
                      && (!existe || ((uint32_t)st.st_size == r->base_tamanho
                                      && (uint32_t)st.st_mtime == r->base_data)));
  if (igual) {
    if (!substituir_no_cartao(caminho, r->dados, r->n_dados, r->flags)) { return false; }
    tocar(h);
    s_resumo.aplicadas++;
    return true;
  }
  char lado[176];
  snprintf(lado, sizeof(lado), "%s.tab5", caminho);
  if (!substituir_no_cartao(lado, r->dados, r->n_dados, r->flags)) { return false; }
  ESP_LOGW(TAG, "%s mudou no computador: o do Tab5 ficou em %s.tab5", caminho, r->arquivo);
  s_resumo.conflitos++;
  return true;
}

// Com s_gravando presa e o cartao do Tab5.
static void aplicar_diario(void)
{
  if (s_emprestado || !tab5_reserva_pendente()) { return; }
  if (!tab5_sd_montar()) { return; }   // sem cartao: fica para a proxima volta
  s_n_tocados = 0;
  const int n = tab5_reserva_aplicar(aplicar_um, nullptr);
  tab5_sd_desmontar();
  if (n < 0) { ESP_LOGE(TAG, "diario ilegivel"); return; }
  if (!tab5_reserva_pendente()) {
    s_resumo.completo = true;
    ESP_LOGI(TAG, "diario aplicado: %u gravacao(oes), %u conflito(s)",
             (unsigned)s_resumo.aplicadas, (unsigned)s_resumo.conflitos);
  }
}

void tab5_sd_gravar_pendentes(void)
{
  if (!s_anexo || !s_gravando) { return; }
  // BARREIRA: espera a gravacao QUE JA ESTA EM ANDAMENTO (a tarefa "cartao"
  // troca o buffer ANTES de escrever no arquivo - sem esperar, quem chamava
  // aqui nesse intervalo lia sem a linha que estava sendo escrita), grava o
  // pendente e aplica o diario de um pendrive, nessa ordem: quem le em
  // seguida ve tudo o que ja foi entregue.
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  aplicar_diario();
  xSemaphoreGive(s_gravando);
}

bool tab5_sd_acrescentar(const char* app, const char* arquivo, const void* dados, size_t n)
{
  if (!s_gravando || !app || !arquivo || (!dados && n)) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  bool ok;
  if (para_o_diario()) {
    ok = diario_acrescentar(app, arquivo, nullptr, 0, dados, n);
  } else if (tab5_sd_montar()) {
    FILE* f = abrir_cru(app, arquivo, "a");
    ok = f && (n == 0 || fwrite(dados, 1, n, f) == n);
    if (f) { fclose(f); }
    tab5_sd_desmontar();
  } else {
    ok = false;
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

bool tab5_sd_substituir(const char* app, const char* arquivo, const void* dados, size_t n, uint8_t flags)
{
  if (!s_gravando || !app || !arquivo || (!dados && n)) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  // O que foi entregue para ACRESCENTAR antes desta troca vai antes dela.
  esvaziar_buffer();
  bool ok;
  if (para_o_diario()) {
    char caminho[160];
    caminho_de(app, arquivo, caminho, sizeof(caminho));
    const foto_t* f = s_emprestado ? foto_de(caminho) : nullptr;
    reserva_reg_t r = { };
    r.tipo = RESERVA_SUBSTITUIR; r.flags = flags;
    r.app = app; r.arquivo = arquivo;
    r.dados = dados; r.n_dados = (uint32_t)n;
    r.base_existia = f != nullptr;
    r.base_tamanho = f ? f->tam : 0;
    r.base_data    = f ? f->data : 0;
    ok = tab5_reserva_gravar(&r);
    if (!ok) { s_diario_perdidas = s_diario_perdidas + 1; }
  } else if (tab5_sd_montar()) {
    char pasta[96], caminho[160];
    ok = tab5_sd_pasta(app, pasta, sizeof(pasta))
      && snprintf(caminho, sizeof(caminho), "%s/%s", pasta, arquivo) < (int)sizeof(caminho)
      && substituir_no_cartao(caminho, dados, n, flags);
    tab5_sd_desmontar();
  } else {
    ok = false;
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

void tab5_sd_resumo(tab5_sd_resumo_t* r)
{
  *r = s_resumo;
  r->no_diario  = 0;
  tab5_reserva_uso(&r->diario_usado, &r->diario_total, &r->no_diario);
  r->perdidas = s_diario_perdidas;
}

void tab5_sd_anexos_estado(uint32_t* pendentes, uint32_t* gravadas, uint32_t* perdidas)
{
  if (pendentes) { *pendentes = (uint32_t)s_usado; }
  if (gravadas)  { *gravadas  = s_gravadas; }
  if (perdidas)  { *perdidas  = s_perdidas; }
}

// 8, e nao 4: com o diario e as redes do no do Mesh, ja sao CINCO quem pega
// carona (log, registro, mesh, mesh_diario, mesh_wifi_no). Com 4, o quinto a
// se registrar ficava sem - "caronas esgotadas" no boot, e o que ele so faz
// na carona (gravar a lista de redes do no, por exemplo) nunca acontecia.
#define CARONAS_MAX 8
static tab5_sd_carona_t s_caronas[CARONAS_MAX] = { };

bool tab5_sd_carona(tab5_sd_carona_t fn)
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
    const bool pedido = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0;
    xSemaphoreTake(s_anexo, portMAX_DELAY);
    const bool ha = (s_usado > 0);
    const bool venceu = ha && (esp_timer_get_time() - s_desde_us) >= (int64_t)ANEXO_PRAZO_MS * 1000;
    xSemaphoreGive(s_anexo);
    if (ha && (pedido || venceu)) { gravar_tudo(); }
    // O diario de um pendrive vai para o cartao assim que ele estiver de
    // volta (ou depois de um reinicio, quando o cartao aparecer).
    // Falhou sem avancar nada (cartao cheio, sem cartao): espera antes da
    // proxima tentativa, em vez de bater no cartao a cada segundo.
    static int64_t proxima_us = 0;
    if (tab5_reserva_pendente() && !s_emprestado && (pedido || esp_timer_get_time() >= proxima_us)) {
      xSemaphoreTake(s_gravando, portMAX_DELAY);
      esvaziar_buffer();
      const uint32_t antes = s_resumo.aplicadas + s_resumo.conflitos;
      aplicar_diario();
      const bool andou = s_resumo.aplicadas + s_resumo.conflitos != antes;
      xSemaphoreGive(s_gravando);
      proxima_us = (andou || !tab5_reserva_pendente()) ? 0 : esp_timer_get_time() + 30LL * 1000 * 1000;
    }
    // As caronas gravam pelas portas (acrescentar/substituir): seguem
    // valendo com o cartao emprestado - o destino e decidido la dentro.
    for (int i = 0; i < CARONAS_MAX && s_caronas[i]; ++i) { s_caronas[i](); }
  }
}

static void gravacao_travar(bool travar)
{
  if (!s_gravando) { return; }
  if (travar) { xSemaphoreTake(s_gravando, portMAX_DELAY); }
  else        { xSemaphoreGive(s_gravando); }
}

static void tarefa_cartao_acordar(void)
{
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}

// Chamado pelo tab5_sd_iniciar (mesmo momento: antes de haver concorrencia).
static void gravador_iniciar(void)
{
  if (s_anexo) { return; }
  s_buf[0] = (uint8_t*)heap_caps_malloc(ANEXO_BUF, MALLOC_CAP_SPIRAM);
  s_buf[1] = (uint8_t*)heap_caps_malloc(ANEXO_BUF, MALLOC_CAP_SPIRAM);
  s_gravando = xSemaphoreCreateMutex();
  SemaphoreHandle_t anexo = xSemaphoreCreateMutex();
  if (!s_buf[0] || !s_buf[1] || !s_gravando || !anexo) {
    ESP_LOGE(TAG, "gravador sem memoria - anexos desligados");
    return;
  }
  // 6 KB: a tarefa MONTA o cartao, e o f_mount desce fundo na stack.
  xTaskCreatePinnedToCore(vTaskCartao, "cartao", 6144, nullptr, 2, &s_tarefa, tskNO_AFFINITY);
  tab5_dono_registrar(s_tarefa, DONO_SERVICO, "Cartao");
  s_anexo = anexo;   // por ultimo: tab5_sd_anexar so aceita com tudo pronto
}
