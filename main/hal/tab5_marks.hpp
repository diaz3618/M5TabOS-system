// MARCOS DO SISTEMA - "o que aconteceu, e quando", para cruzar com o frame
// atrasado.
//
// O frame atrasado tem uma assinatura fixa: um frame inteiro perdido
// (~30 ms com periodo de 15), com os DOIS cores ociosos - ou seja, nao e
// software atrasando a ISR, e o DMA do panel ficando sem data. O que falta
// e saber o que o HARDWARE estava fazendo naquele instante. Cada suspeito
// deixa um mark aqui, e o monitor, ao ver um atraso, lista os marks que
// cairam logo antes dele.
//
// O suspeito numero 1 se registra sozinho: TODA troca de clock da CPU feita
// pelo gerenciador de energia (locks de PM de UART, I2C, PPA; esp_pm_configure)
// passa por rtc_clk_cpu_freq_set_config_fast() - e ANTES dela o IDF reajusta
// o mode do barramento da PSRAM/flash (esp_clk_utils_mspi_speed_mode_sync_*).
// O link desvia essa chamada para ca (-Wl,--wrap, main/CMakeLists.txt).
//
// Anel em RAM INTERNA (o desvio roda com a cache em qualquer state, dentro de
// secao critica): 32 marks, text sempre LITERAL (so o ponteiro e guardado).
#pragma once

#include <stdint.h>

// Deixa um mark. 'what' tem que ser literal. Chamavel de tarefa e de ISR.
void tab5_mark(const char* what, int32_t value);

typedef struct {
  int64_t     us;      // esp_timer_get_time()
  const char* what;
  int32_t     value;
} tab5_mark_t;

// Os marks entre [from_us, to_us], do mais antigo ao mais novo. Devolve quantos.
int tab5_marks_between(int64_t from_us, int64_t to_us, tab5_mark_t* out, int max);
