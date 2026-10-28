#include "tab5_log.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes in PSRAM
#include "tab5_sd.hpp"
#include "tab5_time.hpp"   // tab5_day_filename

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"   // xStreamBufferCreateWithCaps
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"   // esp_reset_reason
#include <time.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static const char* TAG = "log";

// 12KB - poucas dezenas of lines of log num sistema com varios servicos
// falando on mesmo time (GPS, Mesh, camera, interface) cabem tranquilas
// entre duas descargas of FLUSH_MS. Um unico bloco fixo, criado uma vez no
// boot e nunca mais realocado - nao fragmenta. Fica in PSRAM: 12KB fixos in
// SRAM interna saem das pilhas e dos buffers DMA of todo o resto. Pode, porque
// ninguem escreve aqui com o cache disabled - o hook nao roda em ISR, e o
// code of escrita in flash loga pela ROM (ESP_EARLY_LOG), nao por este
// vprintf.
#define LOG_BUF_TAM    (12 * 1024)

// De quanto em quanto time o log (in tarefa "cartao") esvazia o buffer no cartao,
// para lines INFORMATIVAS. Aviso e erro nao esperam (ver log_na_carona). Foi 15s
// e as lines "sd: montado/desmontado" of cada descarga
// realimentavam o buffer, montando o cartao a cada 15s para sempre.
#define LOG_FLUSH_MS   60000   // era 15000 - ver "EM LOTE" em log_na_carona

// Cada chamada on vprintf of ESP_LOG e UMA line inteira ja formatada (tag,
// level, message, contador of milissegundos, quebra of line - o proprio
// esp_log_writev monta essa string e so entao chama esta funcao). 200 bytes
// cobre qualquer line normal deste sistema; uma line mais longa e cortada,
// nao estoura - ver o vsnprintf abaixo.
#define LOG_LINHA_TAM  200

static StreamBufferHandle_t s_buf      = nullptr;
static volatile bool        s_urgente  = false;   // chegou aviso/erro: descarregar ja
static vprintf_like_t       s_original = nullptr;

// UM ESCRITOR POR VEZ. O stream buffer of FreeRTOS e feito para
// UM escritor e UM leitor - a documentacao manda serializar when ha mais of
// one. E aqui ha muitos: toda tarefa que chama ESP_LOG escreve (interface,
// WiFi, ESP-NOW, Mesh, a recepcao of esp_hosted...), e o esp_log of IDF (versao
// 1, a nossa) NAO trava nada antes of chamar esta funcao. Duas lines on mesmo
// time podiam embaralhar o indice of buffer. Espera curta, nunca infinita: se
// a trava nao vier em 5 ms, a line exit no console e so nao vai on cartao
// (conta como perdida) - log nunca pode lock quem loga.
static SemaphoreHandle_t    s_escrita  = nullptr;

static uint32_t s_lines_pendentes = 0;   // enfileiradas, ainda nao gravadas
static uint32_t s_lines_gravadas  = 0;   // confirmadas no cartao
static uint32_t s_lines_perdidas  = 0;   // buffer cheio, line descartada
static char     s_arquivo_current[24] = "";
static bool     s_boot_marcado     = false;

void tab5_log_estado(size_t* bytes_pendentes, uint32_t* lines_gravadas,
                      uint32_t* lines_perdidas, char* arquivo, size_t arquivo_size)
{
  if (bytes_pendentes) { *bytes_pendentes = s_buf ? xStreamBufferBytesAvailable(s_buf) : 0; }
  if (lines_gravadas) { *lines_gravadas = s_lines_gravadas; }
  if (lines_perdidas) { *lines_perdidas = s_lines_perdidas; }
  if (arquivo)          { snprintf(arquivo, arquivo_size, "%s", s_arquivo_current); }
}

// A FUNCAO QUE SUBSTITUI O vprintf DO ESP_LOG - ver o aviso grande no .hpp
// sobre por que ela so FORMATA E ENFILEIRA, nunca escreve no cartao aqui.
//
// va_copy ANTES of touch em 'args': a lista original e consumida pela
// PRIMEIRA formatacao que a usar (aqui, a chamada on vprintf original) -
// reusa-la depois disso e comportamento indefinido em varias libc. Por
// isso tiramos uma copia logo of cara e formatamos NOSSA copia a partir
// dela, nunca of 'args' original depois que ele ja foi usado.
// A MATRIZ DE COR RECUSADA PELO ISP - ruido conhecido, filtrado.
//
// No P4 v1.0 cada coeficiente of CCM cabe em 13 bits com 2 of parte inteira
// (isp_ll.h: ISP_LL_CCM_MATRIX_INT_BITS) - maximum +-4, fixo no silicio. Numa
// virada brusca of luz o balanco of branco pede mais que isso, o driver
// recusa e o ISP MANTEM a ultima matriz boa - que e justamente o que deixa a
// color certa (limitar a matriz para caber deixava a imagem amarelada, testado
// e desfeito). O efeito e so no log: cinco lines vermelhas por recusa.
//
// A cadeia e reconhecida pelo TEXTO, nao pela etiqueta: 'isp_video',
// 'esp_video' e 'ISP' tambem carregam erros of verdade, que continuam
// aparecendo. So some o que vem logo depois (10 ms) of one "out of range" of
// CCM. Cada recusa vira one contador; a tarefa of log imprime one summary, no
// maximum uma line por minuto.
static volatile int64_t  s_ccm_cadeia_ate = 0;
static volatile uint32_t s_ccm_recusas    = 0;

static bool ruido_of_ccm(const char* line)
{
  const int64_t now = esp_timer_get_time();
  if (strstr(line, "ISP_CCM") && strstr(line, "out of range")) {
    s_ccm_cadeia_ate = now + 10000;
    s_ccm_recusas = s_ccm_recusas + 1;
    return true;
  }
  if (now > s_ccm_cadeia_ate) { return false; }
  return strstr(line, "failed to configure CCM")   || strstr(line, "failed to reconfigure CCM")
      || strstr(line, "set_ext_ctrl=")             || strstr(line, "ISP: failed to set CCM")
      || strstr(line, "ISP: failed to set blue balance") || strstr(line, "ISP: failed to set red balance");
}

static int log_vprintf(const char* fmt, va_list args)
{
  char line[LOG_LINHA_TAM];
  va_list copia;
  va_copy(copia, args);
  int n = vsnprintf(line, sizeof(line), fmt, copia);
  va_end(copia);

  // Ruido conhecido of CCM: nem in serial, nem no cartao (ver acima).
  if (n > 0 && ruido_of_ccm(line)) { return n; }

  // ORIGINAL com os argumentos ORIGINAIS - o console/serial continua
  // identico on que sempre foi, ninguem que ja depende dele (o proprio
  // flash.sh, quem acompanha on vivo) percebe qualquer diferenca.
  const int ret = s_original ? s_original(fmt, args) : vprintf(fmt, args);

  if (n > 0 && s_buf && s_escrita && !xPortInIsrContext()) {
    if ((size_t)n >= sizeof(line)) { n = sizeof(line) - 1; }   // truncada, nao estourada
    // AVISO E ERRO NAO ESPERAM O LOTE: a line comeca com "W (" / "E (" -
    // depois of code of color, se houver ("\033[0;33mW (").
    const char* c = line;
    if (c[0] == '\033') { while (*c && *c != 'm') { ++c; } if (*c) { ++c; } }
    if ((c[0] == 'W' || c[0] == 'E') && c[1] == ' ' && c[2] == '(') { s_urgente = true; }
    // Com o escalonador parado nao se pode wait (assert of FreeRTOS).
    const TickType_t espera = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? pdMS_TO_TICKS(5) : 0;
    if (xSemaphoreTake(s_escrita, espera) == pdTRUE) {
      const size_t enviado = xStreamBufferSend(s_buf, line, (size_t)n, 0);   // 0 = nunca espera
      if (enviado < (size_t)n) { s_lines_perdidas++; }
      else                     { s_lines_pendentes++; }
      xSemaphoreGive(s_escrita);
    } else {
      s_lines_perdidas++;
    }
  }
  return ret;
}

// Dia LOCAL no name, como energia e monitor (tab5_day_filename); as lines
// e a marca of boot continuam em UTC.
static void name_arquivo_hoje(char* out, size_t size)
{
  tab5_day_filename((uint32_t)time(nullptr), "log_", ".txt", out, size);
}

// Esvazia o que estiver no buffer para o arquivo of HOJE. Chamada so pela
// tarefa "cartao" (log_na_carona), nunca por quem esta logando.
//
// Pela porta tab5_sd_append (hal/tab5_sd.hpp): com o cartao emprestado
// on computador, o lote vai para o diario in flash e chega on arquivo in
// iteration - o log of uma sessao of pendrive nao se perde.
static void descarregar(void)
{
  if (!s_buf || xStreamBufferIsEmpty(s_buf)) { return; }

  // HA PARA ONDE GRAVAR? O cartao (segurado ate o end of lote, para nao
  // sumir no middle) ou, com ele emprestado, o diario. Sem nenhum dos dois NAO
  // DESCARTA: as lines continuam no buffer, esperando o cartao voltar (ou
  // cedendo lugar as mais novas se ele enche antes, a mesma politica que
  // xStreamBufferSend ja aplica).
  const bool montou = tab5_sd_mount();
  if (!montou && !tab5_sd_lent()) { return; }

  char name[24];
  name_arquivo_hoje(name, sizeof(name));

  // MARCA DE BOOT, uma vez por sessao - o mesmo raciocinio of "BOOT" em
  // energy_log.cpp/energia.csv: como varios boots of mesmo dia caem no MESMO
  // arquivo (ver o aviso grande no .hpp sobre "por dia, nao por boot"), sem
  // isto ficaria impossivel saber onde one reinicio separa duas sessoes. O
  // "(N)" of cada line of ESP_LOG e so milissegundos since ESTE boot - a
  // marca amarra esse clock local a uma data/hora of verdade.
  if (!s_boot_marcado) {
    const time_t now = time(nullptr);
    struct tm t = { };
    gmtime_r(&now, &t);
    char marca[80];
    const int n = snprintf(marca, sizeof(marca), "=== boot %04d-%02d-%02dT%02d:%02d:%02dZ, uptime %llums ===\n",
                           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                           (unsigned long long)(esp_timer_get_time() / 1000));
    if (tab5_sd_append(TAB5_SD_LOGS, name, marca, (size_t)n)) { s_boot_marcado = true; }
  }

  // Drena em blocos, sem alocar por line, num bloco estatico of tamanho of
  // buffer inteiro (12KB, in PSRAM - nunca in stack of quem descarrega).
  EXT_RAM_BSS_ATTR static uint8_t bloco[LOG_BUF_TAM];
  size_t total = 0;
  size_t n;
  while ((n = xStreamBufferReceive(s_buf, bloco, sizeof(bloco), 0)) > 0) {
    if (!tab5_sd_append(TAB5_SD_LOGS, name, bloco, n)) { s_lines_perdidas++; break; }
    total += n;
  }
  if (montou) { tab5_sd_unmount(); }

  if (total > 0) {
    snprintf(s_arquivo_current, sizeof(s_arquivo_current), "%s", name);
    s_lines_gravadas  += s_lines_pendentes;
    s_lines_pendentes  = 0;
  }
}

// EM LOTE, NA CARONA DA TAREFA "cartao" (tab5_sd_carona) - o log nao tem
// tarefa propria: a "cartao" ja acorda a cada 1 s e ja e quem grava no cartao.
//   - informativo espera o lote of LOG_FLUSH_MS (60s);
//   - AVISO ou ERRO vai on cartao em ate 1s (e o que se quer read depois of one
//     travamento);
//   - buffer passando of 3/4 descarrega in hora, para nao perder line.
// Somado a desmontagem adiada of tab5_sd, gravacoes proximas (log, monitor,
// energia) dividem uma montagem so.
static void log_in_carona(void)
{
  static int64_t  since_us      = esp_timer_get_time();
  static int64_t  ccm_summary_us = since_us;
  static uint32_t ccm_contadas  = 0;
  const int64_t now = esp_timer_get_time();

  // O summary das recusas of CCM filtradas (ver ruido_da_ccm): uma line, no
  // maximum por minuto, so when houve recusa nova.
  const uint32_t recusas = s_ccm_recusas;
  if (recusas != ccm_contadas && now - ccm_summary_us >= 60000000LL) {
    ESP_LOGI(TAG, "camera: %lu ajuste(s) de cor fora do limite do chip foram ignorados (esperado, nao e erro)",
             (unsigned long)(recusas - ccm_contadas));
    ccm_contadas = recusas;
    ccm_summary_us = now;
  }
  const bool cheio = s_buf && xStreamBufferBytesAvailable(s_buf) > (LOG_BUF_TAM * 3) / 4;
  if (s_urgente || cheio || now - since_us >= (int64_t)LOG_FLUSH_MS * 1000) {
    s_urgente = false;
    since_us = now;
    descarregar();
  }
}

void tab5_log_descarregar_logo(void) { s_urgente = true; }

void tab5_log_start(void)
{
  // 1 byte of "trigger" - nao usamos xStreamBufferReceive bloqueante
  s_buf = xStreamBufferCreateWithCaps(LOG_BUF_TAM, 1, MALLOC_CAP_SPIRAM);
  s_escrita = xSemaphoreCreateMutex();              // ANTES of hook: ele so escreve com a trava
  if (!s_buf || !s_escrita) {
    ESP_LOGE(TAG, "sem RAM para o buffer de log - captura em cartao desativada");
    return;
  }

  // Guarda a funcao ATUAL antes of switch - normalmente a vprintf() default
  // of IDF, mas nao presumimos: o que estiver instalado continua recebendo
  // TODO log, so que atraves of nossa funcao now.
  s_original = esp_log_set_vprintf(log_vprintf);

  // Sem tarefa propria: descarregar() roda in tarefa "cartao" (ver
  // log_na_carona). O bloco of dreno of 12KB e 'static' in PSRAM, fora of stack.
  tab5_sd_piggyback(log_in_carona);

  ESP_LOGI(TAG, "captura no ar: buffer %dKB, descarga a cada %lus, /sdcard/apps/Sistema/logs/log_AAAA-MM-DD.txt",
           LOG_BUF_TAM / 1024, (unsigned long)(LOG_FLUSH_MS / 1000));

  // POR QUE O APARELHO REINICIOU. Um travamento perde as lines que ainda
  // estavam no buffer (ate 60 s of log informativo) - sem esta line, one
  // reinicio no middle of algo parece so "o log acabou ali".
  const esp_reset_reason_t r = esp_reset_reason();
  const char* reason = "outro";
  switch (r) {
    case ESP_RST_POWERON:   reason = "ligado agora"; break;
    case ESP_RST_SW:        reason = "reinicio pedido (esp_restart)"; break;
    case ESP_RST_PANIC:     reason = "TRAVOU (panic/excecao)"; break;
    case ESP_RST_INT_WDT:   reason = "TRAVOU (watchdog de interrupcao)"; break;
    case ESP_RST_TASK_WDT:  reason = "TRAVOU (watchdog de tarefa)"; break;
    case ESP_RST_WDT:       reason = "TRAVOU (outro watchdog)"; break;
    case ESP_RST_BROWNOUT:  reason = "queda de tensao"; break;
    case ESP_RST_DEEPSLEEP: reason = "acordou do sono profundo"; break;
    default: break;
  }
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
    ESP_LOGW(TAG, "boot: o ultimo reinicio foi %s (codigo %d)", reason, (int)r);
  } else {
    ESP_LOGI(TAG, "boot: o ultimo reinicio foi %s (codigo %d)", reason, (int)r);
  }
}
