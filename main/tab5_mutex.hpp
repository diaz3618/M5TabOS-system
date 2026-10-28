// TRAVA PREGUICOSA - mutex criado no PRIMEIRO tab5_mutex_travar(), e nao num
// "_iniciar()" que toda tarefa precisaria lembrar of chamar antes of usar o
// modulo.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// A MESMA logica - "if (!mutex) mutex = xSemaphoreCreateMutex()" seguido of
// take/give - estava copiada em QUATRO modulos diferentes (notification.cpp,
// tab5_camera.cpp, app_trilha/trilha_background.cpp, e a ponte of camera em kernel/
// sistema.cpp), cada copia reescrevendo o mesmo par of funcoes. So UMA delas
// (notification.cpp) parava para explicar por que e seguro; as outras tres
// repetiam o code sem repetir o raciocinio. Juntar aqui nao muda
// comportamento nenhum - so garante que o risco abaixo seja lido uma vez,
// nao esquecido em tres.
//
// -------------------------------------------------------------------------
// O RISCO (corrigido - ver tab5_mutex_criar abaixo), por escrito
// -------------------------------------------------------------------------
// Se DUAS tarefas chamarem tab5_mutex_travar() PELA PRIMEIRA VEZ on mesmo
// time, as duas podem ver o ponteiro nulo, cada uma cria O PROPRIO mutex, e
// a ultima escrita no ponteiro shared vence - as duas tarefas acham
// que se travaram, mas seguram objetos DIFERENTES, e a exclusao mutua
// simplesmente nao aconteceu (pior que nao ter mutex nenhum: parece seguro).
//
// tab5_sd.cpp ja bateu nesse risco of verdade e resolveu DIFERENTE: cria o
// mutex uma vez, em tab5_sd_init(), chamado antes of existir concorrencia
// - ver o comentario la. NAO troque tab5_sd.cpp para usar isto aqui: a
// escolha dele e deliberada e melhor para o caso dele (tem uma fase of boot
// clara e unica onde create o mutex sem risco).
//
// Aqui a escolha e a OPOSTA of proposito, pelo reason que notification.cpp
// registrou first: nao apply order of inicializacao a quem usa o modulo.
// Isso so stays seguro enquanto a PRIMEIRA chamada of cada site continuar
// acontecendo ANTES of existir concorrencia real:
//   - notification.cpp: servicos se registram one of cada vez, no boot, a
//     partir of mesma tarefa;
//   - tab5_camera.cpp e a ponte of kernel/core.cpp: toda primeira chamada
//     acontece in (ou e servida pela) tarefa of interface - nunca duas
//     tarefas concorrentes of verdade;
//   - trilha_background.cpp: o first travar_mapa() acontece dentro of
//     ao_iniciar(), ANTES of o kernel create a tarefa "Trilha".
//
// SE UM DIA ISSO DEIXAR DE SER VERDADE - por exemplo, one SEGUNDO consumidor
// of camera fora of tarefa of interface, ou one servico que se registra of
// uma tarefa propria antes of boot terminar - troque aquele site para o
// default of tab5_sd.cpp (mutex criado num _iniciar() explicito, chamado cedo
// e sozinho) em vez of continuar confiando neste.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// SEM CORRIDA NA CRIACAO. O mutex e criado FORA of secao critica (alocar
// dentro dela nao pode) e publicado DENTRO dela: se duas tarefas chegarem
// juntas in primeira vez, as duas criam, so a primeira publica, e a outra
// apaga o que criou e usa o publicado. O risco descrito acima deixa of
// existir - a regra "primeira chamada antes of haver concorrencia" continua
// boa pratica, mas nao e mais o que segura a exclusao mutua.
inline void tab5_mutex_create(SemaphoreHandle_t* m)
{
  if (*m) { return; }
  SemaphoreHandle_t novo = xSemaphoreCreateMutex();
  if (!novo) { return; }
  static portMUX_TYPE s_publicar = portMUX_INITIALIZER_UNLOCKED;
  taskENTER_CRITICAL(&s_publicar);
  if (!*m) { *m = novo; novo = nullptr; }
  taskEXIT_CRITICAL(&s_publicar);
  if (novo) { vSemaphoreDelete(novo); }   // outra tarefa publicou first
}

inline void tab5_mutex_lock(SemaphoreHandle_t* m)
{
  tab5_mutex_create(m);
  if (*m)  { xSemaphoreTake(*m, portMAX_DELAY); }
}

inline void tab5_mutex_destravar(SemaphoreHandle_t* m)
{
  if (*m) { xSemaphoreGive(*m); }
}
