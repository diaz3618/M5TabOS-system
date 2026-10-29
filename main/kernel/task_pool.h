// FIXED TASK POOL - who needs a task per one time (service that
// turns on and off, part of one app, assembly of card, survey of Grove) picks up
// a vacancy that already exists, instead of creating and destroying a task.
//
// WHY. xTaskCreate allocates TCB and stack on the internal heap with each call, and
// vTaskDelete returns later - for the idle task, without a time stamp. Create and
// destroy blocks of 4 to 6 KB in SRAM, the entire team, and which fragments the heap
// internal (the "largest block" drops even with free RAM left) and what does the
// apps' memory depends on what the services did before.
//
// HOW. The stacks and TCBs of the vacancies are STATIC (.bss): the total size is
// known in compilation, never fragments the heap and does not exit apps
// see - the free RAM measurement at time of execution already discounts the vacancies. Each
// vacancy is a task that sleeps in a semaphore (zero CPU cost) and executes the
// function they give you; when the function RETURNS, the vacant iteration goes to sleep.
//
// RULES FOR USERS
// - The function must RETURN to finish. Never call vTaskDelete(NULL):
// that would kill the entire vacancy.
// - Do not save the TaskHandle_t after the function has finished: the vacancy is
// reused by another. Use pool_id_t and pool_acordar(), which checks
// if the vacancy is still the same.
// - Without a vacancy of request size (or stack larger than the largest vacancy), it falls into a
// common task and warns in the log. Nothing fails: you just lose your advantage.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_owners.hpp"

typedef void (*pool_body_t)(void* arg);

// Identity of an execution. 'idx' < 0: did not use vacancy (common task).
typedef struct {
  int8_t       idx;
  uint32_t     geracao;
  TaskHandle_t handle;   // is only valid while the function runs
} pool_id_t;

#define POOL_ID_EMPTY { -1, 0, nullptr }

// Increase vacancies. Call once, early at boot. Idempotent.
void pool_start(void);

// Executes 'fn(arg)' in a slot with a stack of at least 'stack' bytes, in
// priority 'prio'. 'owner' (optional: OWNER_SYSTEM + nullptr = none) and
// registered BEFORE the function starts. 'id' can be nullptr (fires and
// forget). Returns false only if neither the common task fits.
bool pool_create(pool_id_t* id, pool_body_t fn, void* arg, const char* name,
                uint32_t stack, UBaseType_t prio,
                owner_type_t tipo = OWNER_SYSTEM, const char* dono = nullptr);

// Removes the task from any delay with a deadline (xTaskAbortDelay), IF it is still
// same execution. Safe to call after she's finished.
void pool_wake(const pool_id_t* id);

// Diagnosis and budget.
typedef struct {
  int      vagas;        // how many are there
  int      ocupadas;     // how many are running now
  uint32_t bytes_fixos;  // batteries + reserved TCBs
  uint32_t dinamicas;    // times when there was no vacancy and fell into a common task
} pool_estado_t;
void pool_estado(pool_estado_t* out);

// Smallest free stack (bytes) seen by NAME of execution, including those already
// have finished - what the vacancies repaint at the end of each one. Return how many.
typedef struct { char name[16]; uint32_t livre_min; } pool_stack_t;
int pool_pilhas(pool_stack_t* out, int max);
void pool_pilhas_reset(void);

// BATTERY IN LP SRAM: NO. The 32 KB of LP SRAM of P4 seems like a good place to
// stack of cold task (they are empty, and the IDF accepts stack there even during
// written in flash), but LP SRAM is TOO slow for the main CPU - in
// home of hundreds of nodes per byte. The cost appears where you don’t expect it: the
// uxTaskGetSystemState (monitor snapshot, 1x/s) measures the free stack of EACH
// task reading byte by byte, WITHIN taskENTER_CRITICAL (interrupts
// off + kernel crash). With four batteries there, the photo was ~0.8 ms
// for ~9 ms and dropped one frame of panel (the blue blink). Any stack
// outside of L2MEM lengthens this window - PSRAM inclusive.
