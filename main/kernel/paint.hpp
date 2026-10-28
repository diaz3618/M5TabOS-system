// Pintura por diferenca - o auxilio que faltava aos apps.
//
// -------------------------------------------------------------------------
// O PROBLEMA QUE ISTO RESOLVE
// -------------------------------------------------------------------------
// O M5GFX desenha direto no framebuffer visivel, entao APAGAR PARA REDESENHAR
// aparece como flash. A regra e escrever por cima, com largura fixa, e so
// quando o valor mudou.
//
// So que a regra sozinha nao bastou: foi esquecida em cinco apps (Energia,
// Tarefas, Ajustes, Arquivos e Mesh), cada um corrigido separadamente. E
// junto vinha sempre o mesmo segundo erro: esquecer que o SISTEMA limpa a area
// (na abertura, ao voltar do repouso) e que dali o app precisa repintar TUDO,
// senao a tela fica em branco - foi o que aconteceu com o app Ajustes.
//
// Enquanto isso for memoria do programador, vai continuar falhando. Aqui os
// dois passos ficam embutidos: pintura_iniciar() consulta a limpeza da area
// sozinho, e numa passada completa TODO comparador devolve "mudou" - entao o
// app repinta tudo sem precisar lembrar de nada.
//
// -------------------------------------------------------------------------
// COMO SE USA
// -------------------------------------------------------------------------
//   static pintura_t s_p;
//   static int s_bat; static char s_nome[24];
//
//   void desenhar(lgfx::LovyanGFX& d, const area_t& a)
//   {
//     pintura_iniciar(&s_p);                       // decide se e completa
//     if (pintura_completa(&s_p)) { ...fundo, titulos, molduras... }
//
//     if (pintura_mudou_i(&s_p, &s_bat, bateria)) { ...desenha o numero... }
//     if (pintura_mudou_s(&s_p, s_nome, sizeof(s_nome), nome)) { ... }
//   }
//
//   // ao trocar de pagina, de selecao, de qualquer coisa estrutural:
//   pintura_pedir(&s_p);  sistema_pedir_redesenho_completo();

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  bool completo;    // esta passada repinta tudo
  bool pedido;      // o app pediu repintura total
  bool ja_desenhou; // false ate a primeira passada (que e sempre completa)
} pintura_t;

// Chamar no INICIO do desenhar(). Decide se a passada e completa, olhando: se
// e a primeira, se o app pediu, ou se o sistema limpou a area.
void pintura_iniciar(pintura_t* p);

// O app pede repintura total. Use ao mudar de pagina, de item selecionado ou
// qualquer coisa que reorganize a tela - e chame tambem
// sistema_pedir_redesenho_completo(), que e quem agenda o desenho.
void pintura_pedir(pintura_t* p);

static inline bool pintura_completa(const pintura_t* p) { return p->completo; }

// SO PARA DECIDIR DESENHO. Nao use estes comparadores para decidir E/S (reler
// cartao, abrir arquivo, falar com hardware): eles devolvem true em toda passada
// COMPLETA, por desenho, e uma E/S que dispare repintura completa entra em loop
// foi o que fez o teclado virtual do app Mesh piscar sozinho.
// Para decisao de E/S, compare na mao.
//
// Comparadores COM MEMORIA. Guardam o valor e devolvem true quando ele muda -
// e SEMPRE true numa passada completa, que e o que faz o app repintar tudo sem
// precisar lembrar disso.
bool pintura_mudou_i(const pintura_t* p, int* mem, int agora);
bool pintura_mudou_s(const pintura_t* p, char* mem, size_t tam, const char* agora);
