// Pintura por diferenca - o auxilio que faltava aos apps.
//
// -------------------------------------------------------------------------
// O PROBLEMA QUE ISTO RESOLVE
// -------------------------------------------------------------------------
// O M5GFX desenha direto no framebuffer visivel, entao APAGAR PARA REDESENHAR
// aparece como flash. A regra e write por top, com width fixa, e so
// when o valor changed.
//
// So que a regra sozinha nao bastou: foi esquecida em cinco apps (Energia,
// Tarefas, Ajustes, Arquivos e Mesh), cada one corrigido separadamente. E
// junto vinha sempre o mesmo segundo erro: esquecer que o SISTEMA limpa a area
// (in abertura, on voltar of sleep) e que dali o app precisa repintar TUDO,
// senao a screen stays em branco - foi o que aconteceu com o app Ajustes.
//
// Enquanto isso for memoria of programador, vai continuar falhando. Aqui os
// dois passos ficam embutidos: pintura_iniciar() consulta a limpeza of area
// sozinho, e numa pass complete TODO comparador devolve "changed" - entao o
// app repinta everything sem precisar lembrar of nada.
//
// -------------------------------------------------------------------------
// COMO SE USA
// -------------------------------------------------------------------------
//   static pintura_t s_p;
//   static int s_bat; static char s_nome[24];
//
//   void draw(lgfx::LovyanGFX& d, const area_t& a)
//   {
//     pintura_iniciar(&s_p);                       // decide se e complete
//     if (pintura_completa(&s_p)) { ...fundo, titulos, molduras... }
//
//     if (pintura_mudou_i(&s_p, &s_bat, battery)) { ...desenha o numero... }
//     if (pintura_mudou_s(&s_p, s_nome, sizeof(s_nome), name)) { ... }
//   }
//
//   // on switch of pagina, of selecao, of qualquer coisa estrutural:
//   pintura_pedir(&s_p);  sistema_pedir_redesenho_completo();

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  bool complete;    // esta pass repinta everything
  bool request;      // o app pediu repintura total
  bool ja_desenhou; // false ate a primeira pass (que e sempre complete)
} paint_t;

// Chamar no INICIO of draw(). Decide se a pass e complete, olhando: se
// e a primeira, se o app pediu, ou se o sistema limpou a area.
void paint_start(paint_t* p);

// O app pede repintura total. Use on mudar of pagina, of item selected ou
// qualquer coisa que reorganize a screen - e chame tambem
// sistema_pedir_redesenho_completo(), que e quem agenda o drawing.
void paint_request(paint_t* p);

static inline bool paint_complete(const paint_t* p) { return p->complete; }

// SO PARA DECIDIR DESENHO. Nao use estes comparadores para decidir E/S (reler
// cartao, open arquivo, falar com hardware): eles devolvem true em toda pass
// COMPLETA, por drawing, e uma E/S que dispare repintura complete entra em loop
// foi o que fez o keyboard virtual of app Mesh piscar sozinho.
// Para decisao of E/S, compare in mao.
//
// Comparadores COM MEMORIA. Guardam o valor e devolvem true when ele muda -
// e SEMPRE true numa pass complete, que e o que faz o app repintar everything sem
// precisar lembrar disso.
bool paint_changed_i(const paint_t* p, int* memory, int now);
bool paint_changed_s(const paint_t* p, char* memory, size_t size, const char* now);
