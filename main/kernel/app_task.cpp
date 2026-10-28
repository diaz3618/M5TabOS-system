#include "app_task.h"
#include "tab5_owners.hpp"
#include "app.h"          // sistema_app_atual
#include "internal.hpp"   // sistema_na_interface
#include "esp_log.h"

static const char *TAG = "tarefa";

// Envolve o loop do app: quando ele retorna, avisa quem espera e so entao a
// tarefa se apaga. O app nunca chama vTaskDelete.
//
// 'fim' CAPTURADO AQUI, ANTES do corpo rodar - NAO lido de 't->fim' depois.
// Bug real, batido em hardware com a camera: uma foto de
// 720x1280 levou mais de 10s para gravar no cartao (bem mais que qualquer
// timeout de tarefa_app_encerrar() deste sistema), a tarefa foi "abandonada"
// - e o comentario de tarefa_app_encerrar() ja avisava, corretamente, que
// abandonar NAO apaga o semaphore (so o 't->fim' do CHAMADOR vira nullptr,
// para o chamador nao mexer mais nele). O problema e que este trampolim
// tambem enxerga o MESMO 't' (e o mesmo ponteiro passado para
// xTaskCreatePinnedToCore) - entao, ao finalmente terminar, ele lia
// 't->fim' DE NOVO e achava nullptr, e xSemaphoreGive(nullptr) derrubou o
// sistema inteiro (assert em xQueueGenericSend, "pxQueue" nulo).
//
// Guardando o handle AQUI, na stack desta tarefa, antes do corpo comecar,
// o trampolim sempre da baixa no semaphore de VERDADE (ele continua existindo
// - so 't->fim' foi zerado, nao o objeto), esteja 't' abandonado ou nao.
// Se ninguem mais espera por ele (o caso abandonado), o give so marca o
// semaphore como "dado" e ninguem nunca o lê de volta - um vazamento
// controlado, exatamente o trade-off que tarefa_app_encerrar() ja aceitava
// por escrito ("vazar e ruim; corromper e pior"), so que agora sem crash.
static void trampolim(void* arg)
{
  tarefa_app_t* t = (tarefa_app_t*)arg;
  SemaphoreHandle_t fim = t->fim;
  t->corpo(t);
  xSemaphoreGive(fim);
  // Retorna: a vaga do pool devolve o dono e volta a dormir. Nunca vTaskDelete.
}

bool tarefa_app_criar(tarefa_app_t* t, tarefa_app_corpo_t corpo,
                      const char* nome, uint32_t pilha, UBaseType_t prioridade)
{
  t->handle = nullptr;
  t->parar  = false;
  t->corpo  = corpo;

  // Binario, e nao contador: so um "acabei" nos interessa.
  t->fim = xSemaphoreCreateBinary();
  if (!t->fim) { return false; }

  // DONO AUTOMATICO (hal/tab5_owners.hpp): criada pela interface com um app na
  // frente = tarefa DESSE app (a Camera abre "cam", o Azimutal "NMEAParse").
  // Quem cria fora disso (um servico, no boot) registra o proprio dono - e
  // um registro depois deste troca o dono, entao o servico sempre vence.
  // O dono vai junto do pedido: a vaga o registra ANTES de a funcao comecar.
  const char* app = (sistema_app_atual() && sistema_na_interface()) ? sistema_app_atual() : nullptr;
  if (!pool_criar(&t->id, trampolim, t, nome, pilha, prioridade,
                  app ? DONO_APP : DONO_SISTEMA, app)) {
    vSemaphoreDelete(t->fim);
    t->fim = nullptr;
    return false;
  }
  t->handle = t->id.handle;
  return true;
}

void tarefa_app_encerrar(tarefa_app_t* t, uint32_t ms)
{
  if (!t->handle) { return; }

  t->parar = true;

  // Tira a tarefa do bloqueio AGORA, sem esperar o timeout dela. Devolve pdFAIL
  // se ela nao estava bloqueada - e nesse caso ela ja vai ver a bandeira na
  // proxima volta, entao nao ha o que fazer.
  pool_acordar(&t->id);

  if (xSemaphoreTake(t->fim, pdMS_TO_TICKS(ms)) == pdTRUE) {
    vSemaphoreDelete(t->fim);
    t->fim = nullptr;
    t->handle = nullptr;
    t->id = (pool_id_t)POOL_ID_VAZIO;
    return;
  }

  // Nao confirmou no prazo. NAO matamos a tarefa e NAO destruimos o semaphore:
  // ela pode estar viva e entregar depois, e apagar o semaphore agora daria
  // crash. Vazar uma tarefa e um semaphore e ruim; corromper um driver ou
  // escrever em memoria liberada e pior. O log existe para isso nao passar
  // despercebido.
  ESP_LOGE(TAG, "tarefa nao confirmou saida em %lums - abandonada de proposito",
           (unsigned long)ms);
  t->fim = nullptr;
  t->handle = nullptr;
  t->id = (pool_id_t)POOL_ID_VAZIO;
}
