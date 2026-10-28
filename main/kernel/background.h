// PARTE DE FUNDO DE UM APP - work que continua when a window fecha
// .
//
// -------------------------------------------------------------------------
// O PROBLEMA
// -------------------------------------------------------------------------
// Um app vive enquanto esta in screen: ao_fechar() libera everything. Mas existe o app
// cujo TRABALHO nao e a screen - gravar a track of GPS, mandar a position pela
// internet, one calculo longo. A Trilha resolvia isso virando one servico feito
// a mao (services/tab5_trilha, com tarefa, notification e parada proprias). O
// segundo app assim teria que repetir everything - e cada one repetiria diferente.
//
// -------------------------------------------------------------------------
// O MODELO
// -------------------------------------------------------------------------
// O app se divide em duas metades:
//   - a JANELA: o app of sempre (desenha, toca, nasce e morre com a screen);
//   - a PARTE DE FUNDO: one loop que o KERNEL roda numa tarefa DELE. O app so
//     descreve (fundo_def_t) e pede para enable/disable. Fechar a window nao
//     para o fundo.
//
// O que o SISTEMA garante a toda parte of fundo, sem o app write nada:
//   - TAREFA propria, priority 1 (abaixo of interface, dos radios e of
//     worker) - o loop mais pesado nunca trava o touch;
//   - VISIVEL SEMPRE: enquanto roda, uma notification NOTIF_ESTADO com o name
//     dela (pastilha cinza in barra). Nada roda escondido of user;
//   - ENERGIA somada: o profile declarado entra no profile efetivo of aparelho
//     (junto of app of screen, ou of home) enquanto ela rodar - WiFi request
//     aqui stays of pe com qualquer app in screen e em sleep;
//   - PARADA LIMPA: request para stop ACORDA o loop (fundo_esperar iteration in
//     hora) e o sistema espera ele exit antes of chamar ao_parar();
//   - quem para: a window, o console ("fundo stop <name>"), e - when
//     existir - a screen of Em execucao.
//
// -------------------------------------------------------------------------
// O CONTRATO DO LOOP (body)
// -------------------------------------------------------------------------
//   static void meu_laco(fundo_t* f) {
//       while (fundo_esperar(f, 5000)) {       // dorme 5 s; false = pare
//           ...work com esperas LIMITADAS...
//           fundo_estado(f, n, "42 envios");    // o text of pastilha
//       }
//   }                                           // retornou: o sistema limpa
//
//   - NUNCA espere para sempre (portMAX_DELAY): o request of parada precisa
//     conseguir chegar. Espera of verdade e fundo_esperar().
//   - Loop of CALCULO (sem wait nada) chama fundo_ceder(f) a cada iteration:
//     sem isso a tarefa ocupa o core inteiro, o IDLE dele nunca roda, o
//     watchdog dispara e o aparelho nao dorme. fundo_ceder so cede of fato
//     a cada ~20 ms, entao pode ser chamado em loop apertado.
//   - O loop NAO desenha. O que a window precisa mostrar, o fundo guarda em
//     estado proprio (protegido) e a window le por copia - igual servico.
//   - Memoria of fundo e of fundo: estatica ou alocada no ao_iniciar() e
//     liberada no ao_parar(). NUNCA one buffer of window - ela pode close a
//     qualquer momento.
//   - Retornar of loop sozinho (terminou o work) e permitido: o sistema
//     percebe e limpa como numa parada requested.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_power.hpp"
#include "notification.h"

#define FUNDO_MAX 4   // partes of fundo running on mesmo time

typedef struct background_s background_t;
typedef void (*background_body_t)(background_t* f);

typedef struct {
  const char*   name;       // literal: name of tarefa (<=15), of notification e nas listas
  background_body_t body;
  uint32_t      stack;      // bytes; 0 = 4096
  tab5_profile_t profile;     // o que precisa LIGADO enquanto roda (soma no profile efetivo)
  int           piso_sleep_mhz;   // 0 = o default of sistema
  // Opcionais. Rodam in tarefa of quem pediu (a interface, in pratica).
  bool (*on_start)(void); // prepara o estado; false = recusa enable
  void (*on_stop)(void);   // depois que o loop exited: fecha e libera
  bool (*com_sound)(void);    // preferencia of beep of notification; nullptr = com sound
} background_def_t;

// ---- para a window (e qualquer one) -----------------------------------------
bool system_fundo_start(const background_def_t* d);   // false: ja running, sem vaga ou recusou
bool system_fundo_stop(const background_def_t* d);     // ESPERA o loop exit (ate 3 s)
bool system_fundo_active(const background_def_t* d);

typedef struct {
  const char* name;
  uint32_t    ha_s;                  // running ha quantos segundos
  char        summary[NOTIF_RESUMO];  // o ultimo fundo_estado()
} background_info_t;
int  system_fundo_listar(background_info_t* out, int max);
bool system_fundo_stop_name(const char* name);

// ---- para o loop ----------------------------------------------------------
bool background_wait(background_t* f, uint32_t ms);   // dorme ate ms; false = pediram para stop
bool background_yield(background_t* f);                  // loop of calculo; false = pediram para stop
bool background_running(const background_t* f);
void background_estado(background_t* f, int count, const char* summary);

// ---- para o kernel --------------------------------------------------------
void background_somar_profile(tab5_profile_t& p);   // chamado por sistema_impor_preferencias()
int  background_piso_sleep(void);               // maior piso request (0 = nenhum)
void background_manter(void);                     // no loop of interface: limpa quem terminou sozinho,
                                             // reaplica o profile when o conjunto changed
