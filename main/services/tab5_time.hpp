// SINCRONIA DE HORA - acerta o RTC (e o relogio do sistema junto, ver
// tab5_rtc.hpp) a partir da MELHOR fonte disponivel no momento.
//
// -------------------------------------------------------------------------
// AS FONTES, da mais confiavel para a menos
// -------------------------------------------------------------------------
// NTP (internet, com o WiFi conectado) - servidor de hora de verdade, erro
// de milissegundos. Sobe sozinho na primeira vez que o WiFi pega IP.
//
// GPS (tab5_gps_time()) - o receptor tem relogio proprio, sincronizado
// pelos satelites. So vale COM FIX: antes disso muitos receptores mandam a
// hora do relogio interno deles, que pode estar qualquer coisa.
//
// Mesh (tab5_mesh_hora_viva()) - o relogio do no Meshtastic, lido no rx_time
// de pacote que chegou AGORA. Nao conta o last_heard da lista de nos nem o
// que estava parado na queue do no durante a conexao: sao horas do PASSADO,
// e usadas como "agora" atrasam o relogio em horas ou dias - uma mensagem
// enviada nesse intervalo e carimbada com a data errada.
//
// -------------------------------------------------------------------------
// CONSISTENCIA
// -------------------------------------------------------------------------
// - Fonte melhor que a atual acerta na hora; a mesma fonte, a cada
//   RESSINCRONIZAR_MS; uma fonte PIOR so depois de a melhor sumir por
//   VALIDADE_MS. Assim o Mesh nunca desfaz o que o NTP ou o GPS acertaram.
// - SALTO: quando o acerto muda o relogio em mais de SALTO_S, quem carimbou
//   coisas com a hora velha e avisado (tab5_time_on_jump) e corrige os
//   proprios carimbos desta sessao - a mensagem enviada antes do acerto sai
//   do dia errado junto com o relogio.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_timer.h"
struct tm;   // forward-declarado so para o ponteiro de tab5_local_time() -
             // quem usa struct tm de verdade inclui <ctime> por conta propria.

// Confere as fontes e ajusta o RTC quando a regra de consistencia acima
// deixa. Chamar 1x/s, DA TAREFA DE INTERFACE - e quem pode escrever no RTC
// (ver tab5_rtc.hpp).
void tab5_time_sync(void);

// De onde veio o ultimo acerto desta sessao: "internet", "GPS", "Mesh", ou
// "relogio interno" (nenhuma fonte ainda - vale o que o RTC guardou).
const char* tab5_time_source(void);

// Aviso de SALTO: 'delta_s' e quanto o relogio andou (positivo = foi para a
// frente). Chamado da tarefa de interface, logo depois do acerto; quem
// assina so corrige os proprios carimbos e volta (sem cartao, sem espera
// longa). Ate 4 assinantes.
typedef void (*tab5_time_jump_cb)(int32_t delta_s);
void tab5_time_on_jump(tab5_time_jump_cb cb);

// -------------------------------------------------------------------------
// TIMEZONE.
// -------------------------------------------------------------------------
// User-configurable whole-hour offset from UTC (Settings > About), no DST
// logic - if that's ever needed, this is where it goes, not scattered
// across apps.
//
// DISPLAY ONLY. The RTC, sync, and logs stay in pure UTC on purpose - it's
// the only way to compare timestamps between mesh nodes, log files, and GPS
// without ambiguity. Only code that's about to SHOW the time calls this -
// gmtime_r() directly is still correct for any other use.
int32_t tab5_utc_offset_s(void);

// O RELOGIO JA FOI ACERTADO? Abaixo disto (2020-09-13) uma hora nao e de
// verdade: o RTC sem acerto (sem GPS, rede ou malha desde o boot) ou um
// carimbo antigo gravado como 0. Era o mesmo numero escrito em doze lugares,
// com quatro nomes diferentes - um so, aqui.
#define TAB5_EPOCH_VALIDO 1600000000u
static inline bool tab5_time_is_valid(uint32_t epoch_utc) { return epoch_utc > TAB5_EPOCH_VALIDO; }

// AGORA, em epoch UTC - ou 0 enquanto o relogio nao foi acertado. O 0 e o
// "sem data" que telas, arquivos e o Meshtastic ja sabem tratar.
static inline uint32_t tab5_time_epoch(void)
{
  const time_t t = time(nullptr);
  return (t > (time_t)TAB5_EPOCH_VALIDO) ? (uint32_t)t : 0;
}

// Converts UTC epoch to struct tm in the LOCAL TIMEZONE (tab5_utc_offset_s())
// - same format as gmtime_r() (which is what it does underneath, just on the
// already-offset epoch).
void tab5_local_time(uint32_t epoch_utc, struct tm* out);

// NOME DE ARQUIVO DO DIA: "<prefixo>AAAA-MM-DD<sufixo>", no dia LOCAL - o dia
// de quem usa. Todo arquivo diario do sistema (log, energia, monitor) usa
// este, para os do mesmo dia virarem juntos a meia-noite (com cada um fazendo
// a conta do seu jeito, o log em UTC virava o dia 3 horas antes dos outros).
// So o NOME e local: o conteudo continua carimbado em UTC.
// Relogio ainda sem acerto (epoch antes de 2020): "<prefixo>sem-data<sufixo>".
//   tab5_day_filename(time(nullptr), "log_", ".txt", nome, sizeof(nome));
void tab5_day_filename(uint32_t epoch_utc, const char* prefixo, const char* sufixo,
                           char* out, size_t tam);

// RELOGIO MONOTONO, em ms desde o boot - para medir intervalos, nunca para
// mostrar hora. Um so para o sistema inteiro (eram oito copias locais, metade
// pelo tick do FreeRTOS e metade pelo esp_timer). Da a volta em ~49 dias:
// compare sempre por diferenca (agora - antes), nunca por maior/menor.
static inline uint32_t tab5_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
