#include "tab5_log.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM
#include "tab5_sd.hpp"
#include "tab5_time.hpp"   // tab5_hora_nome_do_dia

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

// 12KB - poucas dezenas de linhas de log num sistema com varios servicos
// falando ao mesmo tempo (GPS, Mesh, camera, interface) cabem tranquilas
// entre duas descargas de FLUSH_MS. Um unico bloco fixo, criado uma vez no
// boot e nunca mais realocado - nao fragmenta. Fica na PSRAM: 12KB fixos na
// SRAM interna saem das pilhas e dos buffers DMA de todo o resto. Pode, porque
// ninguem escreve aqui com o cache desligado - o hook nao roda em ISR, e o
// codigo de escrita na flash loga pela ROM (ESP_EARLY_LOG), nao por este
// vprintf.
#define LOG_BUF_TAM    (12 * 1024)

// De quanto em quanto tempo o log (na tarefa "cartao") esvazia o buffer no cartao,
// para linhas INFORMATIVAS. Aviso e erro nao esperam (ver log_na_carona). Foi 15s
// e as linhas "sd: montado/desmontado" de cada descarga
// realimentavam o buffer, montando o cartao a cada 15s para sempre.
#define LOG_FLUSH_MS   60000   // era 15000 - ver "EM LOTE" em log_na_carona

// Cada chamada ao vprintf do ESP_LOG e UMA linha inteira ja formatada (tag,
// nivel, mensagem, contador de milissegundos, quebra de linha - o proprio
// esp_log_writev monta essa string e so entao chama esta funcao). 200 bytes
// cobre qualquer linha normal deste sistema; uma linha mais longa e cortada,
// nao estoura - ver o vsnprintf abaixo.
#define LOG_LINHA_TAM  200

static StreamBufferHandle_t s_buf      = nullptr;
static volatile bool        s_urgente  = false;   // chegou aviso/erro: descarregar ja
static vprintf_like_t       s_original = nullptr;

// UM ESCRITOR POR VEZ. O stream buffer do FreeRTOS e feito para
// UM escritor e UM leitor - a documentacao manda serializar quando ha mais de
// um. E aqui ha muitos: toda tarefa que chama ESP_LOG escreve (interface,
// WiFi, ESP-NOW, Mesh, a recepcao do esp_hosted...), e o esp_log do IDF (versao
// 1, a nossa) NAO trava nada antes de chamar esta funcao. Duas linhas ao mesmo
// tempo podiam embaralhar o indice do buffer. Espera curta, nunca infinita: se
// a trava nao vier em 5 ms, a linha sai no console e so nao vai ao cartao
// (conta como perdida) - log nunca pode travar quem loga.
static SemaphoreHandle_t    s_escrita  = nullptr;

static uint32_t s_linhas_pendentes = 0;   // enfileiradas, ainda nao gravadas
static uint32_t s_linhas_gravadas  = 0;   // confirmadas no cartao
static uint32_t s_linhas_perdidas  = 0;   // buffer cheio, linha descartada
static char     s_arquivo_atual[24] = "";
static bool     s_boot_marcado     = false;

void tab5_log_estado(size_t* bytes_pendentes, uint32_t* linhas_gravadas,
                      uint32_t* linhas_perdidas, char* arquivo, size_t arquivo_tam)
{
  if (bytes_pendentes) { *bytes_pendentes = s_buf ? xStreamBufferBytesAvailable(s_buf) : 0; }
  if (linhas_gravadas) { *linhas_gravadas = s_linhas_gravadas; }
  if (linhas_perdidas) { *linhas_perdidas = s_linhas_perdidas; }
  if (arquivo)          { snprintf(arquivo, arquivo_tam, "%s", s_arquivo_atual); }
}

// A FUNCAO QUE SUBSTITUI O vprintf DO ESP_LOG - ver o aviso grande no .hpp
// sobre por que ela so FORMATA E ENFILEIRA, nunca escreve no cartao aqui.
//
// va_copy ANTES de tocar em 'args': a lista original e consumida pela
// PRIMEIRA formatacao que a usar (aqui, a chamada ao vprintf original) -
// reusa-la depois disso e comportamento indefinido em varias libc. Por
// isso tiramos uma copia logo de cara e formatamos NOSSA copia a partir
// dela, nunca do 'args' original depois que ele ja foi usado.
// A MATRIZ DE COR RECUSADA PELO ISP - ruido conhecido, filtrado.
//
// No P4 v1.0 cada coeficiente da CCM cabe em 13 bits com 2 de parte inteira
// (isp_ll.h: ISP_LL_CCM_MATRIX_INT_BITS) - maximo +-4, fixo no silicio. Numa
// virada brusca de luz o balanco de branco pede mais que isso, o driver
// recusa e o ISP MANTEM a ultima matriz boa - que e justamente o que deixa a
// cor certa (limitar a matriz para caber deixava a imagem amarelada, testado
// e desfeito). O efeito e so no log: cinco linhas vermelhas por recusa.
//
// A cadeia e reconhecida pelo TEXTO, nao pela etiqueta: 'isp_video',
// 'esp_video' e 'ISP' tambem carregam erros de verdade, que continuam
// aparecendo. So some o que vem logo depois (10 ms) de um "out of range" da
// CCM. Cada recusa vira um contador; a tarefa de log imprime um resumo, no
// maximo uma linha por minuto.
static volatile int64_t  s_ccm_cadeia_ate = 0;
static volatile uint32_t s_ccm_recusas    = 0;

static bool ruido_da_ccm(const char* linha)
{
  const int64_t agora = esp_timer_get_time();
  if (strstr(linha, "ISP_CCM") && strstr(linha, "out of range")) {
    s_ccm_cadeia_ate = agora + 10000;
    s_ccm_recusas = s_ccm_recusas + 1;
    return true;
  }
  if (agora > s_ccm_cadeia_ate) { return false; }
  return strstr(linha, "failed to configure CCM")   || strstr(linha, "failed to reconfigure CCM")
      || strstr(linha, "set_ext_ctrl=")             || strstr(linha, "ISP: failed to set CCM")
      || strstr(linha, "ISP: failed to set blue balance") || strstr(linha, "ISP: failed to set red balance");
}

static int log_vprintf(const char* fmt, va_list args)
{
  char linha[LOG_LINHA_TAM];
  va_list copia;
  va_copy(copia, args);
  int n = vsnprintf(linha, sizeof(linha), fmt, copia);
  va_end(copia);

  // Ruido conhecido da CCM: nem na serial, nem no cartao (ver acima).
  if (n > 0 && ruido_da_ccm(linha)) { return n; }

  // ORIGINAL com os argumentos ORIGINAIS - o console/serial continua
  // identico ao que sempre foi, ninguem que ja depende dele (o proprio
  // flash.sh, quem acompanha ao vivo) percebe qualquer diferenca.
  const int ret = s_original ? s_original(fmt, args) : vprintf(fmt, args);

  if (n > 0 && s_buf && s_escrita && !xPortInIsrContext()) {
    if ((size_t)n >= sizeof(linha)) { n = sizeof(linha) - 1; }   // truncada, nao estourada
    // AVISO E ERRO NAO ESPERAM O LOTE: a linha comeca com "W (" / "E (" -
    // depois do codigo de cor, se houver ("\033[0;33mW (").
    const char* c = linha;
    if (c[0] == '\033') { while (*c && *c != 'm') { ++c; } if (*c) { ++c; } }
    if ((c[0] == 'W' || c[0] == 'E') && c[1] == ' ' && c[2] == '(') { s_urgente = true; }
    // Com o escalonador parado nao se pode esperar (assert do FreeRTOS).
    const TickType_t espera = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? pdMS_TO_TICKS(5) : 0;
    if (xSemaphoreTake(s_escrita, espera) == pdTRUE) {
      const size_t enviado = xStreamBufferSend(s_buf, linha, (size_t)n, 0);   // 0 = nunca espera
      if (enviado < (size_t)n) { s_linhas_perdidas++; }
      else                     { s_linhas_pendentes++; }
      xSemaphoreGive(s_escrita);
    } else {
      s_linhas_perdidas++;
    }
  }
  return ret;
}

// Dia LOCAL no nome, como energia e monitor (tab5_hora_nome_do_dia); as linhas
// e a marca de boot continuam em UTC.
static void nome_arquivo_hoje(char* out, size_t tam)
{
  tab5_hora_nome_do_dia((uint32_t)time(nullptr), "log_", ".txt", out, tam);
}

// Esvazia o que estiver no buffer para o arquivo de HOJE. Chamada so pela
// tarefa "cartao" (log_na_carona), nunca por quem esta logando.
//
// Pela porta tab5_sd_append (hal/tab5_sd.hpp): com o cartao emprestado
// ao computador, o lote vai para o diario na flash e chega ao arquivo na
// volta - o log de uma sessao de pendrive nao se perde.
static void descarregar(void)
{
  if (!s_buf || xStreamBufferIsEmpty(s_buf)) { return; }

  // HA PARA ONDE GRAVAR? O cartao (segurado ate o fim do lote, para nao
  // sumir no meio) ou, com ele emprestado, o diario. Sem nenhum dos dois NAO
  // DESCARTA: as linhas continuam no buffer, esperando o cartao voltar (ou
  // cedendo lugar as mais novas se ele enche antes, a mesma politica que
  // xStreamBufferSend ja aplica).
  const bool montou = tab5_sd_mount();
  if (!montou && !tab5_sd_lent()) { return; }

  char nome[24];
  nome_arquivo_hoje(nome, sizeof(nome));

  // MARCA DE BOOT, uma vez por sessao - o mesmo raciocinio do "BOOT" em
  // energy_log.cpp/energia.csv: como varios boots do mesmo dia caem no MESMO
  // arquivo (ver o aviso grande no .hpp sobre "por dia, nao por boot"), sem
  // isto ficaria impossivel saber onde um reinicio separa duas sessoes. O
  // "(N)" de cada linha de ESP_LOG e so milissegundos desde ESTE boot - a
  // marca amarra esse relogio local a uma data/hora de verdade.
  if (!s_boot_marcado) {
    const time_t agora = time(nullptr);
    struct tm t = { };
    gmtime_r(&agora, &t);
    char marca[80];
    const int n = snprintf(marca, sizeof(marca), "=== boot %04d-%02d-%02dT%02d:%02d:%02dZ, uptime %llums ===\n",
                           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                           (unsigned long long)(esp_timer_get_time() / 1000));
    if (tab5_sd_append(TAB5_SD_LOGS, nome, marca, (size_t)n)) { s_boot_marcado = true; }
  }

  // Drena em blocos, sem alocar por linha, num bloco estatico do tamanho do
  // buffer inteiro (12KB, na PSRAM - nunca na pilha de quem descarrega).
  EXT_RAM_BSS_ATTR static uint8_t bloco[LOG_BUF_TAM];
  size_t total = 0;
  size_t n;
  while ((n = xStreamBufferReceive(s_buf, bloco, sizeof(bloco), 0)) > 0) {
    if (!tab5_sd_append(TAB5_SD_LOGS, nome, bloco, n)) { s_linhas_perdidas++; break; }
    total += n;
  }
  if (montou) { tab5_sd_unmount(); }

  if (total > 0) {
    snprintf(s_arquivo_atual, sizeof(s_arquivo_atual), "%s", nome);
    s_linhas_gravadas  += s_linhas_pendentes;
    s_linhas_pendentes  = 0;
  }
}

// EM LOTE, NA CARONA DA TAREFA "cartao" (tab5_sd_carona) - o log nao tem
// tarefa propria: a "cartao" ja acorda a cada 1 s e ja e quem grava no cartao.
//   - informativo espera o lote de LOG_FLUSH_MS (60s);
//   - AVISO ou ERRO vai ao cartao em ate 1s (e o que se quer ler depois de um
//     travamento);
//   - buffer passando de 3/4 descarrega na hora, para nao perder linha.
// Somado a desmontagem adiada do tab5_sd, gravacoes proximas (log, monitor,
// energia) dividem uma montagem so.
static void log_na_carona(void)
{
  static int64_t  desde_us      = esp_timer_get_time();
  static int64_t  ccm_resumo_us = desde_us;
  static uint32_t ccm_contadas  = 0;
  const int64_t agora = esp_timer_get_time();

  // O resumo das recusas da CCM filtradas (ver ruido_da_ccm): uma linha, no
  // maximo por minuto, so quando houve recusa nova.
  const uint32_t recusas = s_ccm_recusas;
  if (recusas != ccm_contadas && agora - ccm_resumo_us >= 60000000LL) {
    ESP_LOGI(TAG, "camera: %lu ajuste(s) de cor fora do limite do chip foram ignorados (esperado, nao e erro)",
             (unsigned long)(recusas - ccm_contadas));
    ccm_contadas = recusas;
    ccm_resumo_us = agora;
  }
  const bool cheio = s_buf && xStreamBufferBytesAvailable(s_buf) > (LOG_BUF_TAM * 3) / 4;
  if (s_urgente || cheio || agora - desde_us >= (int64_t)LOG_FLUSH_MS * 1000) {
    s_urgente = false;
    desde_us = agora;
    descarregar();
  }
}

void tab5_log_descarregar_logo(void) { s_urgente = true; }

void tab5_log_iniciar(void)
{
  // 1 byte de "trigger" - nao usamos xStreamBufferReceive bloqueante
  s_buf = xStreamBufferCreateWithCaps(LOG_BUF_TAM, 1, MALLOC_CAP_SPIRAM);
  s_escrita = xSemaphoreCreateMutex();              // ANTES do hook: ele so escreve com a trava
  if (!s_buf || !s_escrita) {
    ESP_LOGE(TAG, "sem RAM para o buffer de log - captura em cartao desativada");
    return;
  }

  // Guarda a funcao ATUAL antes de trocar - normalmente a vprintf() padrao
  // do IDF, mas nao presumimos: o que estiver instalado continua recebendo
  // TODO log, so que atraves da nossa funcao agora.
  s_original = esp_log_set_vprintf(log_vprintf);

  // Sem tarefa propria: descarregar() roda na tarefa "cartao" (ver
  // log_na_carona). O bloco de dreno de 12KB e 'static' na PSRAM, fora da pilha.
  tab5_sd_piggyback(log_na_carona);

  ESP_LOGI(TAG, "captura no ar: buffer %dKB, descarga a cada %lus, /sdcard/apps/Sistema/logs/log_AAAA-MM-DD.txt",
           LOG_BUF_TAM / 1024, (unsigned long)(LOG_FLUSH_MS / 1000));

  // POR QUE O APARELHO REINICIOU. Um travamento perde as linhas que ainda
  // estavam no buffer (ate 60 s de log informativo) - sem esta linha, um
  // reinicio no meio de algo parece so "o log acabou ali".
  const esp_reset_reason_t r = esp_reset_reason();
  const char* motivo = "outro";
  switch (r) {
    case ESP_RST_POWERON:   motivo = "ligado agora"; break;
    case ESP_RST_SW:        motivo = "reinicio pedido (esp_restart)"; break;
    case ESP_RST_PANIC:     motivo = "TRAVOU (panic/excecao)"; break;
    case ESP_RST_INT_WDT:   motivo = "TRAVOU (watchdog de interrupcao)"; break;
    case ESP_RST_TASK_WDT:  motivo = "TRAVOU (watchdog de tarefa)"; break;
    case ESP_RST_WDT:       motivo = "TRAVOU (outro watchdog)"; break;
    case ESP_RST_BROWNOUT:  motivo = "queda de tensao"; break;
    case ESP_RST_DEEPSLEEP: motivo = "acordou do sono profundo"; break;
    default: break;
  }
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
    ESP_LOGW(TAG, "boot: o ultimo reinicio foi %s (codigo %d)", motivo, (int)r);
  } else {
    ESP_LOGI(TAG, "boot: o ultimo reinicio foi %s (codigo %d)", motivo, (int)r);
  }
}
