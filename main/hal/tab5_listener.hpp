// OUVINTE UNICO DE UM EVENTO, com remocao que ESPERA a chamada em voo.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// Tres modulos entregam evento a UM ouvinte registrado: a varredura BLE
// (tab5_ble), o farejo de Wi-Fi e o CSI. Os tres guardavam so um ponteiro
// volatile: quem chama copia e chama, quem remove escreve nullptr. Copiar
// evita o ponteiro nulo - mas NAO diz quando a chamada que ja comecou acaba.
// Quem remove o ouvinte para em seguida destruir o que ele usa (a fila do
// Radar) ficava sem saber se podia. O Radar "resolvia" esperando 40 ms ao
// fechar - um palpite na tarefa de interface, e um uso de memoria liberada
// para o primeiro ouvinte mais lento que o palpite.
//
// Aqui a remocao (definir nullptr) so VOLTA depois que nenhuma chamada esta
// em voo. Daqui para baixo, quem removeu pode destruir o que quiser.
//
// -------------------------------------------------------------------------
// COMO, sem trava
// -------------------------------------------------------------------------
// Quem chama CONTA-SE em voo ANTES de ler o ponteiro; quem remove troca o
// ponteiro ANTES de olhar a contagem. Com atomicos (ordem sequencial), so ha
// dois casos: ou a chamada leu o ponteiro antigo - e entao ja estava contada
// quando a remocao olhou, e a remocao espera -, ou leu o nulo e nao chama.
// A espera e de uma chamada curta (os ouvintes so enfileiram e voltam): na
// pratica a remocao nunca da uma volta no laco.
//
// NAO remova o ouvinte de DENTRO dele mesmo: a remocao esperaria a propria
// chamada para sempre.

#pragma once

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

template <typename F>
struct tab5_ouvinte_t {
  std::atomic<F>   fn{nullptr};
  std::atomic<int> em_voo{0};
};

// Do lado do evento (qualquer tarefa).
template <typename F, typename... A>
inline void tab5_ouvinte_chamar(tab5_ouvinte_t<F>* o, A... args)
{
  o->em_voo.fetch_add(1);
  const F f = o->fn.load();
  if (f) { f(args...); }
  o->em_voo.fetch_sub(1);
}

// Troca o ouvinte (nullptr remove) e so volta quando a chamada em voo, se
// havia uma, terminou.
template <typename F>
inline void tab5_ouvinte_definir(tab5_ouvinte_t<F>* o, F f)
{
  o->fn.store(f);
  while (o->em_voo.load() != 0) { vTaskDelay(1); }
}
