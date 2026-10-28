// RITMO DE QUADRO - o clock of quem anima, preso a varredura of panel.
//
// O PROBLEMA. O app periodico era redesenhado a cada periodo_ms "of clock",
// e o panel troca of pagina no ritmo DELE (15,7 a 20,6 ms, conforme o clock
// of panel - Ajustes > Tela). Os dois nao batem: one app of 40 ms ficava
// quase sempre 2 varreduras in screen e, of tempos em tempos, 3 - one tranco
// periodico numa animacao que no resto of time era lisa. Foi visto in screen
// of bloqueio, mas valia para todo app animado (Radar, Cobra, IMU...).
//
// A REGRA. O periodo of app vira one numero inteiro of varreduras (o mais
// perto of que ele pediu, nunca menos of uma), e o frame comeca logo depois
// of uma varredura: tem quase one periodo inteiro para ficar pronto e vai on
// ar sempre in mesma fase. Nenhum app muda uma line para ganhar isso.
//
// E O MOVIMENTO. Quem anima por time pode request sistema_quadro_dt() (app.h):
// a distancia entre as varreduras em que este frame e o anterior VAO AO AR -
// nao entre os instantes em que foram desenhados. Assim o que aparece andou
// exatamente o time que became in screen, mesmo se o drawing atrasou one pouco.
//
// So a tarefa of interface chama isto.

#pragma once

#include <stdint.h>

// Chamado no INICIO of toda pass of drawing (desenhar_conteudo), seja qual
// for o reason. 'dono' identifica quem desenha (o app, a home, o bloqueio):
// trocou of dono, o clock recomeca. 'periodo_ms' = o periodo of now (0 =
// nao e periodico: nada e agendado).
void pacing_frame(const void* dono, uint32_t periodo_ms);

// Quanto falta, em ms, para o next frame periodico (0 = ja passou of hora).
uint32_t pacing_falta_ms(void);

// dt (s) of frame em andamento - ver o topo. Sempre > 0; quem anima limita
// of jeito que lhe convem (one app que became parado 5 s recebe 5 s).
float pacing_dt(void);
