// DONO DE CADA TAREFA - quem e responsavel por cada tarefa que
// esta rodando, no espirito do "usuario / sistema" dos gerenciadores de tarefa.
//
// O FreeRTOS so sabe o NOME de uma tarefa. Adivinhar o dono pelo nome seria
// uma tabela que envelhece a cada tarefa nova. Aqui o dono e DECLARADO por
// quem cria a tarefa, no momento em que cria:
//   - SERVICO: os servicos do sistema (WiFi, ESP-NOW, Mesh, GPS, log...) -
//     registram as proprias tarefas;
//   - APP:     tarefa criada por um app aberto (tarefa_app_criar, chamada da
//     interface com um app na frente) - o kernel registra sozinho;
//   - FUNDO:   a parte de fundo de um app (kernel/background) - o kernel registra;
//   - SISTEMA: todo o resto, e o padrao: kernel, drivers, o proprio IDF.
//
// Mora em hal/ e so depende do FreeRTOS: HAL, servicos e kernel registram sem
// ninguem passar a depender de camada de cima. O 'dono' e um texto que vive
// para sempre (literal, ou o nome do app/fundo, que tambem sao literais).
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
  DONO_SISTEMA = 0,
  DONO_SERVICO,
  DONO_APP,
  DONO_FUNDO,
} dono_tipo_t;

// Registra (ou troca) o dono. Chame logo depois de criar a tarefa.
void tab5_dono_registrar(TaskHandle_t h, dono_tipo_t tipo, const char* dono);

// A tarefa vai morrer: tira do registro (o handle pode ser reaproveitado por
// outra tarefa depois). Chame ANTES do vTaskDelete.
void tab5_dono_esquecer(TaskHandle_t h);

// O dono registrado; DONO_SISTEMA (e dono = NULL) se ninguem registrou.
dono_tipo_t tab5_dono_de(TaskHandle_t h, const char** dono);
