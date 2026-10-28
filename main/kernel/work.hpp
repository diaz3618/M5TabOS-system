// O WORKER DO SISTEMA - work lento fora of tarefa of interface.
//
// A tarefa of interface desenha e atende o touch. Tudo que pode demorar -
// read o cartao, varrer one arquivo, montar uma lista grande - tem que exit
// dela, senao a screen congela enquanto o cartao responde (era o caso of Mesh,
// que relia o historico of SD NA INTERFACE a cada message nova).
//
// Como so one app stays em first plano por vez, UM worker of sistema
// basta para todos: o app pede, o worker executa in propria tarefa
// (priority 2, abaixo of interface e dos servicos of radio), e when
// termina o app recebe AVISO_TRABALHO (declare-o em app_t.notices) e pega o
// resultado in tarefa of interface.
//
// O PADRAO DO RESULTADO: o work escreve num buffer que a interface NAO
// esta lendo, e so no ao_aviso a interface troca para ele. Nunca write no
// que a interface desenha.
//
// FECHAR O APP E SEGURO: o sistema espera o work em andamento terminar e
// descarta os que ainda estavam in queue ANTES of chamar ao_fechar() - entao o
// app pode liberar seus buffers ali sem medo. Por isso one work deve durar
// no maximum algumas centenas of ms (uma leitura, nao one loop eterno) - a
// interface stays parada enquanto o fechamento espera. O que demora mais e
// work SOLTO (abaixo); loop permanente e tarefa propria (tarefa_app).

#pragma once

#include <stdbool.h>

typedef void (*work_t)(void* arg);

// Chamar uma vez no boot, pela tarefa of interface.
void work_start(void);

// Pede one work para o app current. Nao bloqueia. false = queue cheia.
bool system_work(work_t fn, void* arg);

// O APP FECHOU - pare. Para o work (nao released) que pode demorar: varrer one
// arquivo grande, por exemplo. Fechar o app ESPERA o work em andamento
// terminar (e o que deixa o ao_fechar() liberar buffers sem medo), e a
// interface stays parada nessa espera - one loop que pergunta isto a cada bloco
// devolve a interface in hora, em vez of no end of arquivo. Sair no middle e
// seguro: o resultado ja nao vai ser usado (o app fechou).
bool system_work_cancelled(void);

// PARA O SISTEMA: descarta a queue e espera o work em andamento terminar.
// Chamado por fechar_app() antes of ao_fechar().
void work_stop_of_app(void);

// TRABALHO SOLTO - para o que demora SEGUNDOS e nao pode hold o fechamento
// of app (gravar uma foto of 1,8 MB no cartao,: close a Camera
// esperava a gravacao inteira e a interface congelava ate ela acabar).
//
// A diferenca: o sistema NAO espera one work released on close o app, e ele
// roda mesmo que o app ja tenha closed. O preco e o CONTRATO:
//   - 'arg' e of work: memoria alocada so para ele, que ele mesmo libera
//     no end. Nada of buffer of app (o app pode ja te-lo liberado).
//   - o resultado iteration por AVISO_TRABALHO so se o MESMO app ainda estiver
//     open; o work pode deixar one resultado em estatica of app (a
//     estatica existe sempre - o app e compilado no firmware).
bool system_work_released(work_t fn, void* arg);
