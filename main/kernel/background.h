// PARTE DE FUNDO DE UM APP - trabalho que continua quando a janela fecha
// .
//
// -------------------------------------------------------------------------
// O PROBLEMA
// -------------------------------------------------------------------------
// Um app vive enquanto esta na tela: ao_fechar() libera tudo. Mas existe o app
// cujo TRABALHO nao e a tela - gravar a trilha do GPS, mandar a posicao pela
// internet, um calculo longo. A Trilha resolvia isso virando um servico feito
// a mao (services/tab5_trilha, com tarefa, notificacao e parada proprias). O
// segundo app assim teria que repetir tudo - e cada um repetiria diferente.
//
// -------------------------------------------------------------------------
// O MODELO
// -------------------------------------------------------------------------
// O app se divide em duas metades:
//   - a JANELA: o app de sempre (desenha, toca, nasce e morre com a tela);
//   - a PARTE DE FUNDO: um loop que o KERNEL roda numa tarefa DELE. O app so
//     descreve (fundo_def_t) e pede para ligar/desligar. Fechar a janela nao
//     para o fundo.
//
// O que o SISTEMA garante a toda parte de fundo, sem o app escrever nada:
//   - TAREFA propria, prioridade 1 (abaixo da interface, dos radios e do
//     worker) - o loop mais pesado nunca trava o toque;
//   - VISIVEL SEMPRE: enquanto roda, uma notificacao NOTIF_ESTADO com o nome
//     dela (pastilha cinza na barra). Nada roda escondido do usuario;
//   - ENERGIA somada: o perfil declarado entra no perfil efetivo do aparelho
//     (junto do app da tela, ou da home) enquanto ela rodar - WiFi pedido
//     aqui fica de pe com qualquer app na tela e em repouso;
//   - PARADA LIMPA: pedir para parar ACORDA o loop (fundo_esperar volta na
//     hora) e o sistema espera ele sair antes de chamar ao_parar();
//   - quem para: a janela, o console ("fundo parar <nome>"), e - quando
//     existir - a tela de Em execucao.
//
// -------------------------------------------------------------------------
// O CONTRATO DO LOOP (corpo)
// -------------------------------------------------------------------------
//   static void meu_laco(fundo_t* f) {
//       while (fundo_esperar(f, 5000)) {       // dorme 5 s; false = pare
//           ...trabalho com esperas LIMITADAS...
//           fundo_estado(f, n, "42 envios");    // o texto da pastilha
//       }
//   }                                           // retornou: o sistema limpa
//
//   - NUNCA espere para sempre (portMAX_DELAY): o pedido de parada precisa
//     conseguir chegar. Espera de verdade e fundo_esperar().
//   - Loop de CALCULO (sem esperar nada) chama fundo_ceder(f) a cada volta:
//     sem isso a tarefa ocupa o core inteiro, o IDLE dele nunca roda, o
//     watchdog dispara e o aparelho nao dorme. fundo_ceder so cede de fato
//     a cada ~20 ms, entao pode ser chamado em loop apertado.
//   - O loop NAO desenha. O que a janela precisa mostrar, o fundo guarda em
//     estado proprio (protegido) e a janela le por copia - igual servico.
//   - Memoria do fundo e do fundo: estatica ou alocada no ao_iniciar() e
//     liberada no ao_parar(). NUNCA um buffer da janela - ela pode fechar a
//     qualquer momento.
//   - Retornar do loop sozinho (terminou o trabalho) e permitido: o sistema
//     percebe e limpa como numa parada pedida.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_power.hpp"
#include "notification.h"

#define FUNDO_MAX 4   // partes de fundo rodando ao mesmo tempo

typedef struct fundo_s fundo_t;
typedef void (*fundo_corpo_t)(fundo_t* f);

typedef struct {
  const char*   nome;       // literal: nome da tarefa (<=15), da notificacao e nas listas
  fundo_corpo_t corpo;
  uint32_t      pilha;      // bytes; 0 = 4096
  tab5_perfil_t perfil;     // o que precisa LIGADO enquanto roda (soma no perfil efetivo)
  int           piso_repouso_mhz;   // 0 = o padrao do sistema
  // Opcionais. Rodam na tarefa de quem pediu (a interface, na pratica).
  bool (*ao_iniciar)(void); // prepara o estado; false = recusa ligar
  void (*ao_parar)(void);   // depois que o loop saiu: fecha e libera
  bool (*com_som)(void);    // preferencia de beep da notificacao; nullptr = com som
} fundo_def_t;

// ---- para a janela (e qualquer um) -----------------------------------------
bool sistema_fundo_iniciar(const fundo_def_t* d);   // false: ja rodando, sem vaga ou recusou
bool sistema_fundo_parar(const fundo_def_t* d);     // ESPERA o loop sair (ate 3 s)
bool sistema_fundo_ativo(const fundo_def_t* d);

typedef struct {
  const char* nome;
  uint32_t    ha_s;                  // rodando ha quantos segundos
  char        resumo[NOTIF_RESUMO];  // o ultimo fundo_estado()
} fundo_info_t;
int  sistema_fundo_listar(fundo_info_t* out, int max);
bool sistema_fundo_parar_nome(const char* nome);

// ---- para o loop ----------------------------------------------------------
bool fundo_esperar(fundo_t* f, uint32_t ms);   // dorme ate ms; false = pediram para parar
bool fundo_ceder(fundo_t* f);                  // loop de calculo; false = pediram para parar
bool fundo_rodando(const fundo_t* f);
void fundo_estado(fundo_t* f, int quantidade, const char* resumo);

// ---- para o kernel --------------------------------------------------------
void fundo_somar_perfil(tab5_perfil_t& p);   // chamado por sistema_impor_preferencias()
int  fundo_piso_repouso(void);               // maior piso pedido (0 = nenhum)
void fundo_manter(void);                     // no loop da interface: limpa quem terminou sozinho,
                                             // reaplica o perfil quando o conjunto mudou
