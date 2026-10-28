// TRAVA PREGUICOSA - mutex criado no PRIMEIRO tab5_mutex_travar(), e nao num
// "_iniciar()" que toda tarefa precisaria lembrar de chamar antes de usar o
// modulo.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// A MESMA logica - "if (!mutex) mutex = xSemaphoreCreateMutex()" seguido de
// take/give - estava copiada em QUATRO modulos diferentes (notification.cpp,
// tab5_camera.cpp, app_trilha/trilha_background.cpp, e a ponte da camera em kernel/
// sistema.cpp), cada copia reescrevendo o mesmo par de funcoes. So UMA delas
// (notification.cpp) parava para explicar por que e seguro; as outras tres
// repetiam o codigo sem repetir o raciocinio. Juntar aqui nao muda
// comportamento nenhum - so garante que o risco abaixo seja lido uma vez,
// nao esquecido em tres.
//
// -------------------------------------------------------------------------
// O RISCO (corrigido - ver tab5_mutex_criar abaixo), por escrito
// -------------------------------------------------------------------------
// Se DUAS tarefas chamarem tab5_mutex_travar() PELA PRIMEIRA VEZ ao mesmo
// tempo, as duas podem ver o ponteiro nulo, cada uma cria O PROPRIO mutex, e
// a ultima escrita no ponteiro shared vence - as duas tarefas acham
// que se travaram, mas seguram objetos DIFERENTES, e a exclusao mutua
// simplesmente nao aconteceu (pior que nao ter mutex nenhum: parece seguro).
//
// tab5_sd.cpp ja bateu nesse risco de verdade e resolveu DIFERENTE: cria o
// mutex uma vez, em tab5_sd_init(), chamado antes de existir concorrencia
// - ver o comentario la. NAO troque tab5_sd.cpp para usar isto aqui: a
// escolha dele e deliberada e melhor para o caso dele (tem uma fase de boot
// clara e unica onde criar o mutex sem risco).
//
// Aqui a escolha e a OPOSTA de proposito, pelo motivo que notification.cpp
// registrou primeiro: nao impor ordem de inicializacao a quem usa o modulo.
// Isso so fica seguro enquanto a PRIMEIRA chamada de cada site continuar
// acontecendo ANTES de existir concorrencia real:
//   - notification.cpp: servicos se registram um de cada vez, no boot, a
//     partir da mesma tarefa;
//   - tab5_camera.cpp e a ponte de kernel/core.cpp: toda primeira chamada
//     acontece na (ou e servida pela) tarefa de interface - nunca duas
//     tarefas concorrentes de verdade;
//   - trilha_background.cpp: o primeiro travar_mapa() acontece dentro de
//     ao_iniciar(), ANTES de o kernel criar a tarefa "Trilha".
//
// SE UM DIA ISSO DEIXAR DE SER VERDADE - por exemplo, um SEGUNDO consumidor
// de camera fora da tarefa de interface, ou um servico que se registra de
// uma tarefa propria antes do boot terminar - troque aquele site para o
// padrao de tab5_sd.cpp (mutex criado num _iniciar() explicito, chamado cedo
// e sozinho) em vez de continuar confiando neste.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// SEM CORRIDA NA CRIACAO. O mutex e criado FORA da secao critica (alocar
// dentro dela nao pode) e publicado DENTRO dela: se duas tarefas chegarem
// juntas na primeira vez, as duas criam, so a primeira publica, e a outra
// apaga o que criou e usa o publicado. O risco descrito acima deixa de
// existir - a regra "primeira chamada antes de haver concorrencia" continua
// boa pratica, mas nao e mais o que segura a exclusao mutua.
inline void tab5_mutex_criar(SemaphoreHandle_t* m)
{
  if (*m) { return; }
  SemaphoreHandle_t novo = xSemaphoreCreateMutex();
  if (!novo) { return; }
  static portMUX_TYPE s_publicar = portMUX_INITIALIZER_UNLOCKED;
  taskENTER_CRITICAL(&s_publicar);
  if (!*m) { *m = novo; novo = nullptr; }
  taskEXIT_CRITICAL(&s_publicar);
  if (novo) { vSemaphoreDelete(novo); }   // outra tarefa publicou primeiro
}

inline void tab5_mutex_travar(SemaphoreHandle_t* m)
{
  tab5_mutex_criar(m);
  if (*m)  { xSemaphoreTake(*m, portMAX_DELAY); }
}

inline void tab5_mutex_destravar(SemaphoreHandle_t* m)
{
  if (*m) { xSemaphoreGive(*m); }
}
