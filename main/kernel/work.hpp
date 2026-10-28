// O WORKER DO SISTEMA - trabalho lento fora da tarefa de interface.
//
// A tarefa de interface desenha e atende o toque. Tudo que pode demorar -
// ler o cartao, varrer um arquivo, montar uma lista grande - tem que sair
// dela, senao a tela congela enquanto o cartao responde (era o caso do Mesh,
// que relia o historico do SD NA INTERFACE a cada mensagem nova).
//
// Como so um app fica em primeiro plano por vez, UM worker de sistema
// basta para todos: o app pede, o worker executa na propria tarefa
// (prioridade 2, abaixo da interface e dos servicos de radio), e quando
// termina o app recebe AVISO_TRABALHO (declare-o em app_t.avisos) e pega o
// resultado na tarefa de interface.
//
// O PADRAO DO RESULTADO: o trabalho escreve num buffer que a interface NAO
// esta lendo, e so no ao_aviso a interface troca para ele. Nunca escrever no
// que a interface desenha.
//
// FECHAR O APP E SEGURO: o sistema espera o trabalho em andamento terminar e
// descarta os que ainda estavam na queue ANTES de chamar ao_fechar() - entao o
// app pode liberar seus buffers ali sem medo. Por isso um trabalho deve durar
// no maximo algumas centenas de ms (uma leitura, nao um loop eterno) - a
// interface fica parada enquanto o fechamento espera. O que demora mais e
// trabalho SOLTO (abaixo); loop permanente e tarefa propria (tarefa_app).

#pragma once

#include <stdbool.h>

typedef void (*trabalho_t)(void* arg);

// Chamar uma vez no boot, pela tarefa de interface.
void trabalho_iniciar(void);

// Pede um trabalho para o app atual. Nao bloqueia. false = queue cheia.
bool sistema_trabalho(trabalho_t fn, void* arg);

// O APP FECHOU - pare. Para o trabalho (nao solto) que pode demorar: varrer um
// arquivo grande, por exemplo. Fechar o app ESPERA o trabalho em andamento
// terminar (e o que deixa o ao_fechar() liberar buffers sem medo), e a
// interface fica parada nessa espera - um loop que pergunta isto a cada bloco
// devolve a interface na hora, em vez de no fim do arquivo. Sair no meio e
// seguro: o resultado ja nao vai ser usado (o app fechou).
bool sistema_trabalho_cancelado(void);

// PARA O SISTEMA: descarta a queue e espera o trabalho em andamento terminar.
// Chamado por fechar_app() antes do ao_fechar().
void trabalho_encerrar_do_app(void);

// TRABALHO SOLTO - para o que demora SEGUNDOS e nao pode segurar o fechamento
// do app (gravar uma foto de 1,8 MB no cartao,: fechar a Camera
// esperava a gravacao inteira e a interface congelava ate ela acabar).
//
// A diferenca: o sistema NAO espera um trabalho solto ao fechar o app, e ele
// roda mesmo que o app ja tenha fechado. O preco e o CONTRATO:
//   - 'arg' e do trabalho: memoria alocada so para ele, que ele mesmo libera
//     no fim. Nada de buffer do app (o app pode ja te-lo liberado).
//   - o resultado volta por AVISO_TRABALHO so se o MESMO app ainda estiver
//     aberto; o trabalho pode deixar um resultado em estatica do app (a
//     estatica existe sempre - o app e compilado no firmware).
bool sistema_trabalho_solto(trabalho_t fn, void* arg);
