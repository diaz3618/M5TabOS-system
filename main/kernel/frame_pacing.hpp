// RITMO DE QUADRO - o relogio de quem anima, preso a varredura do painel.
//
// O PROBLEMA. O app periodico era redesenhado a cada periodo_ms "de relogio",
// e o painel troca de pagina no ritmo DELE (15,7 a 20,6 ms, conforme o clock
// do painel - Ajustes > Tela). Os dois nao batem: um app de 40 ms ficava
// quase sempre 2 varreduras na tela e, de tempos em tempos, 3 - um tranco
// periodico numa animacao que no resto do tempo era lisa. Foi visto na tela
// de bloqueio, mas valia para todo app animado (Radar, Cobra, IMU...).
//
// A REGRA. O periodo do app vira um numero inteiro de varreduras (o mais
// perto do que ele pediu, nunca menos de uma), e o quadro comeca logo depois
// de uma varredura: tem quase um periodo inteiro para ficar pronto e vai ao
// ar sempre na mesma fase. Nenhum app muda uma linha para ganhar isso.
//
// E O MOVIMENTO. Quem anima por tempo pode pedir sistema_quadro_dt() (app.h):
// a distancia entre as varreduras em que este quadro e o anterior VAO AO AR -
// nao entre os instantes em que foram desenhados. Assim o que aparece andou
// exatamente o tempo que ficou na tela, mesmo se o desenho atrasou um pouco.
//
// So a tarefa de interface chama isto.

#pragma once

#include <stdint.h>

// Chamado no INICIO de toda passada de desenho (desenhar_conteudo), seja qual
// for o motivo. 'dono' identifica quem desenha (o app, a home, o bloqueio):
// trocou de dono, o relogio recomeca. 'periodo_ms' = o periodo de agora (0 =
// nao e periodico: nada e agendado).
void ritmo_quadro(const void* dono, uint32_t periodo_ms);

// Quanto falta, em ms, para o proximo quadro periodico (0 = ja passou da hora).
uint32_t ritmo_falta_ms(void);

// dt (s) do quadro em andamento - ver o topo. Sempre > 0; quem anima limita
// do jeito que lhe convem (um app que ficou parado 5 s recebe 5 s).
float ritmo_dt(void);
