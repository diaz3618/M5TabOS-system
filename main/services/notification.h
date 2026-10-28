// NOTIFICACAO - servico do sistema, nao remendo de aplicativo.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// A barra de status chamava tab5_mesh_nao_lidas() diretamente. Funcionava, e
// era uma divida: o SISTEMA conhecia um SERVICO pelo nome. O segundo app que
// precisasse avisar alguma coisa - um alarme de bateria, um download pronto,
// um GPS que perdeu o fix - teria que ser costurado no mesmo lugar, com o mesmo
// if. Ao quinto, a barra de status seria uma lista de casos especiais e mexer
// nela viraria mexer em cinco modulos.
//
// Aqui a relacao inverte: quem tem novidade ANUNCIA, e o sistema so pergunta
// "ha algo?" sem saber de quem. O tab5_mesh some do sistema.cpp.
//
// -------------------------------------------------------------------------
// O MODELO: UMA FONTE, UM ESTADO
// -------------------------------------------------------------------------
// Cada servico (ou app) tem UMA fonte registrada e reescreve o estado dela
// quando quer. Guia de uso para quem publica: WRITING_APPS.md, secao 8.2.
// Nao e queue de eventos, e ESTADO ATUAL - "tenho 3 mensagens, uma delas
// direta". Isso e de proposito:
//
//   - queue cresce sem limite quando ninguem consome; estado nao;
//   - avisar "3 mensagens" e mais util que tres avisos de "1 mensagem";
//   - quem gera sabe agregar melhor que quem exibe.
//
// -------------------------------------------------------------------------
// CONCORRENCIA
// -------------------------------------------------------------------------
// notif_publicar() e chamavel de QUALQUER tarefa - e o ponto, ja que quem tem
// novidade e o servico e nao a interface. A leitura acontece na tarefa de
// interface. O modulo se protege sozinho.
//
// O que ele NAO faz: desenhar. Notificacao nao sabe o que e uma tela.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define NOTIF_MAX_FONTES  8
#define NOTIF_RESUMO      32

// Gravidade. A barra pinta pela PIOR fonte ativa - vermelho ganha de azul,
// porque uma coisa endereçada a voce nao e a mesma coisa que trafego passando.
typedef enum {
  NOTIF_ESTADO = 0,   // atividade em curso ("gravando trilha") - so aparece se nada mais tiver o que dizer
  NOTIF_INFO,         // acontecendo algo, nada exige voce
  NOTIF_ATENCAO,      // algo endereçado a voce, ou que vai atrapalhar em breve
  NOTIF_URGENTE,      // exige acao: bateria acabando, cartao cheio
} notif_nivel_t;

// Registra uma fonte e devolve o identificador, ou -1 se nao houver vaga.
// IDEMPOTENTE PELO NOME: registrar de novo um nome que ja existe
// devolve a MESMA vaga, COM O ESTADO QUE ELA TINHA. (Antes a vaga voltava
// zerada - e o Mesh, que se registra de novo a cada vez que o servico sobe
// por outra via, apagava o proprio aviso sem ninguem ter lido nada. Quem
// quer apagar publica 0; o registro nao decide isso por ele.) O Mesh se registra a cada vez que o servico sobe - e
// ele sobe de novo a cada sondagem do Grove. Antes, cada subida tomava uma
// vaga nova; na 8a nao havia mais vaga, s_notif virava -1 e toda mensagem
// dali em diante chegava sem aviso nenhum, ate reiniciar o aparelho.
//
// 'nome' precisa sobreviver ao registro (literal ou estatico): guardamos o
// ponteiro, nao uma copia. E o mesmo contrato do app_t.nome.
int notif_registrar(const char* nome);

// Publica o estado ATUAL da fonte. quantidade 0 apaga o aviso.
// 'resumo' pode ser nullptr quando nao houver texto util.
void notif_publicar(int id, int quantidade, notif_nivel_t nivel, const char* resumo);

// QUAL APP ABRE quando o aviso e tocado na central de avisos (kernel/shell).
// Sem associar, vale o app com o MESMO NOME da fonte - o caso das partes de
// fundo (background.h), cujo nome ja e o do app. 'app_nome' segue o contrato do
// 'nome': literal ou estatico. nullptr = o aviso nao abre nada.
void notif_associar_app(int id, const char* app_nome);

// Atalho para "nao tenho mais nada a dizer".
static inline void notif_limpar(int id) { notif_publicar(id, 0, NOTIF_INFO, nullptr); }

// Liga/desliga o AVISO SONORO desta fonte, e SO desta
// . Nao afeta
// quantidade/nivel/resumo: a fonte continua publicando e aparecendo na barra
// e na lista de ativas exatamente igual, so para de contar para o beep (ver
// notif_houve_novidade_sonora() abaixo). Quem grava a preferencia entre
// reinicios e o CHAMADOR (ver app_trilha/trilha_background.cpp e settings.h) - este modulo so
// guarda o estado EM RAM, do mesmo jeito que guarda quantidade/nivel.
void notif_silenciar(int id, bool silenciar);

// ---- leitura, para quem desenha -------------------------------------------

// Muda a cada publicacao que altere alguma coisa. A barra compara com o que ela
// desenhou por ultimo - e o que evita repintar a toa e, junto, o que garante
// que a novidade apareca na hora e nao no proximo 1% de bateria.
uint32_t notif_versao(void);

typedef struct {
  const char*   nome;
  const char*   app;          // nome do app que o aviso abre (nullptr = nenhum)
  int           quantidade;
  notif_nivel_t nivel;
  char          resumo[NOTIF_RESUMO];
} notif_item_t;

// A FONTE EM DESTAQUE - a que a barra mostra. E a de maior
// nivel entre as ATIVAS (quantidade > 0); no empate, a que mudou por ultimo.
// 'outras' recebe quantas fontes ativas ficaram de fora. false = nada ativo.
//
// Substitui o antigo "N avisos" com varias fontes ativas: ele SOMAVA coisas
// que nao se somam (pontos da Trilha + mensagens do Mesh) - com uma trilha
// gravando, uma mensagem nova virava "348 avisos" em vez de "1 em LongFast",
// e sumia no numero. Copia em vez de devolver ponteiro: a tabela muda a
// qualquer momento, noutra tarefa.
bool notif_destaque(notif_item_t* saida, int* outras);

// TODAS AS ATIVAS, na ordem em que a barra as mostra: o destaque primeiro
// (mais grave; no empate, a mais recente) e o resto na mesma regra. Devolve
// quantas copiou (ate 'max'). E o que a central de avisos e a tela de
// bloqueio listam - com mais de um app avisando, um so destaque e um "+N"
// escondiam justamente o que o usuario queria saber: DE QUEM.
int notif_listar(notif_item_t* saida, int max);

// ---- leitura, para quem toca som -------------------------------------------

// Verdadeiro se alguma fonte NAO SILENCIADA subiu de quantidade desde a
// ULTIMA chamada a esta funcao especifica - e a UNICA pergunta que quem toca
// o beep (kernel/core.cpp) precisa fazer; ele nao sabe, e nao precisa saber,
// quantas fontes existem ou os nomes delas. Chamar sempre no MESMO passo de
// tempo (hoje, a cada BARRA_MS) - e o que define "desde a ultima vez".
bool notif_houve_novidade_sonora(void);
