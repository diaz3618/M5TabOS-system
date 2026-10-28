// NOTIFICACAO - servico of sistema, nao remendo of aplicativo.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// A barra of status chamava tab5_mesh_nao_lidas() diretamente. Funcionava, e
// era uma divida: o SISTEMA conhecia one SERVICO pelo name. O segundo app que
// precisasse notify alguma coisa - one alarme of battery, one download pronto,
// one GPS que perdeu o fix - teria que ser costurado no mesmo lugar, com o mesmo
// if. Ao quinto, a barra of status seria uma lista of casos especiais e mexer
// nela viraria mexer em cinco modulos.
//
// Aqui a relacao inverte: quem tem novidade ANUNCIA, e o sistema so pergunta
// "ha algo?" sem saber of quem. O tab5_mesh some of sistema.cpp.
//
// -------------------------------------------------------------------------
// O MODELO: UMA FONTE, UM ESTADO
// -------------------------------------------------------------------------
// Cada servico (ou app) tem UMA fonte registrada e reescreve o estado dela
// when quer. Guia of uso para quem publica: WRITING_APPS.md, secao 8.2.
// Nao e queue of eventos, e ESTADO ATUAL - "tenho 3 mensagens, uma delas
// direta". Isso e of proposito:
//
//   - queue cresce sem limit when ninguem consome; estado nao;
//   - notify "3 mensagens" e mais util que tres notices of "1 message";
//   - quem gera sabe agregar melhor que quem exibe.
//
// -------------------------------------------------------------------------
// CONCORRENCIA
// -------------------------------------------------------------------------
// notif_publicar() e chamavel of QUALQUER tarefa - e o ponto, ja que quem tem
// novidade e o servico e nao a interface. A leitura acontece in tarefa of
// interface. O modulo se protege sozinho.
//
// O que ele NAO faz: draw. Notificacao nao sabe o que e uma screen.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define NOTIF_MAX_FONTES  8
#define NOTIF_RESUMO      32

// Gravidade. A barra pinta pela PIOR fonte active - vermelho ganha of azul,
// porque uma coisa endereçada a voce nao e a mesma coisa que trafego passando.
typedef enum {
  NOTIF_ESTADO = 0,   // atividade em progress ("gravando track") - so aparece se nada mais tiver o que dizer
  NOTIF_INFO,         // acontecendo algo, nada requires voce
  NOTIF_ATENCAO,      // algo endereçado a voce, ou que vai atrapalhar em breve
  NOTIF_URGENTE,      // requires acao: battery acabando, cartao cheio
} notification_level_t;

// Registra uma fonte e devolve o identificador, ou -1 se nao houver vaga.
// IDEMPOTENTE PELO NOME: registrar of novo one name que ja existe
// devolve a MESMA vaga, COM O ESTADO QUE ELA TINHA. (Antes a vaga voltava
// zerada - e o Mesh, que se registra of novo a cada vez que o servico sobe
// por outra via, apagava o proprio aviso sem ninguem ter lido nada. Quem
// quer erase publica 0; o registro nao decide isso por ele.) O Mesh se registra a cada vez que o servico sobe - e
// ele sobe of novo a cada sondagem of Grove. Antes, cada subida tomava uma
// vaga nova; in 8a nao havia mais vaga, s_notif virava -1 e toda message
// dali em diante chegava sem aviso nenhum, ate reiniciar o aparelho.
//
// 'name' precisa sobreviver on registro (literal ou estatico): guardamos o
// ponteiro, nao uma copia. E o mesmo contrato of app_t.name.
int notification_registrar(const char* name);

// Publica o estado ATUAL of fonte. count 0 apaga o aviso.
// 'summary' pode ser nullptr when nao houver text util.
void notification_publicar(int id, int count, notification_level_t level, const char* summary);

// QUAL APP ABRE when o aviso e touched in central of notices (kernel/shell).
// Sem associar, vale o app com o MESMO NOME of fonte - o caso das partes of
// fundo (background.h), cujo name ja e o of app. 'app_nome' segue o contrato of
// 'name': literal ou estatico. nullptr = o aviso nao abre nada.
void notification_associar_app(int id, const char* app_name);

// Atalho para "nao tenho mais nada a dizer".
static inline void notification_limpar(int id) { notification_publicar(id, 0, NOTIF_INFO, nullptr); }

// Liga/desliga o AVISO SONORO desta fonte, e SO desta
// . Nao afeta
// count/level/summary: a fonte continua publicando e aparecendo in barra
// e in lista of ativas exatamente igual, so para of count para o beep (ver
// notif_houve_novidade_sonora() abaixo). Quem grava a preferencia entre
// reinicios e o CHAMADOR (ver app_trilha/trilha_background.cpp e settings.h) - este modulo so
// guarda o estado EM RAM, of mesmo jeito que guarda count/level.
void notification_silenciar(int id, bool silenciar);

// ---- leitura, para quem desenha -------------------------------------------

// Muda a cada publicacao que altere alguma coisa. A barra compara com o que ela
// desenhou por ultimo - e o que evita repintar a toa e, junto, o que garante
// que a novidade apareca in hora e nao no next 1% of battery.
uint32_t notification_versao(void);

typedef struct {
  const char*   name;
  const char*   app;          // name of app que o aviso abre (nullptr = nenhum)
  int           count;
  notification_level_t level;
  char          summary[NOTIF_RESUMO];
} notification_item_t;

// A FONTE EM DESTAQUE - a que a barra mostra. E a of maior
// level entre as ATIVAS (count > 0); no empate, a que changed por ultimo.
// 'outras' recebe quantas fontes ativas ficaram of fora. false = nada active.
//
// Substitui o antigo "N notices" com varias fontes ativas: ele SOMAVA coisas
// que nao se somam (pontos of Trilha + mensagens of Mesh) - com uma track
// gravando, uma message nova virava "348 notices" em vez of "1 em LongFast",
// e sumia no numero. Copia em vez of return ponteiro: a tabela muda a
// qualquer momento, noutra tarefa.
bool notification_destaque(notification_item_t* saida, int* outras);

// TODAS AS ATIVAS, in order em que a barra as mostra: o destaque first
// (mais grave; no empate, a mais recente) e o resto in mesma regra. Devolve
// quantas copiou (ate 'max'). E o que a central of notices e a screen of
// bloqueio listam - com mais of one app avisando, one so destaque e one "+N"
// escondiam justamente o que o user queria saber: DE QUEM.
int notification_listar(notification_item_t* saida, int max);

// ---- leitura, para quem toca sound -------------------------------------------

// Verdadeiro se alguma fonte NAO SILENCIADA subiu of count since a
// ULTIMA chamada a esta funcao especifica - e a UNICA pergunta que quem toca
// o beep (kernel/core.cpp) precisa fazer; ele nao sabe, e nao precisa saber,
// quantas fontes existem ou os nomes delas. Chamar sempre no MESMO passo of
// time (hoje, a cada BARRA_MS) - e o que define "since a ultima vez".
bool notification_houve_novidade_sonora(void);
