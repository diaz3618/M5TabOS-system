// PONTE PRIVADA entre core.cpp e shell.cpp - NUNCA incluido por um app.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE (Etapa 1 do ARCHITECTURE.md)
// -------------------------------------------------------------------------
// sistema.cpp tinha crescido de 1129 para 2457 linhas fazendo oito coisas
// diferentes, compartilhando estaticas sem fronteira nenhuma - "o arquivo
// onde o proximo bug vai se esconder", nas palavras do proprio documento.
// Ele foi partido em dois, SEM MUDAR UMA LINHA DE LOGICA:
//
//   core.cpp   o MOTOR - agenda de desenho, ciclo de vida dos apps
//              (abrir/fechar), politica de energia. Decide O QUE roda e
//              QUANDO.
//   shell.cpp  a APARENCIA - tela inicial, barra de status, roteamento de
//              toque para o app certo, calibracao do toque. E "shell" no
//              sentido de SO (Windows Shell, GNOME Shell): a camada visual
//              em volta dos apps. NAO E o mesmo "shell" de console.cpp (a
//              interface de linha de comando pela serial) - dois conceitos
//              diferentes, dois arquivos diferentes, sem relacao.
//
// A UNICA excecao ao "zero mudanca de logica": tratar_toque() (agora em
// shell.cpp) chamava a funcao PRIVADA area_util() para pegar a geometria da
// area util; ela virou a chamada a sistema_area() (a mesma coisa, publica -
// sistema_area() e literalmente "return area_util();"). Trocar por isso
// evita expor uma funcao inteira so para uma chamada, sem mudar nenhum
// resultado.
//
// app.h continua sendo o UNICO contrato que um app enxerga. Este header e
// deliberadamente mais cru que ele - variaveis extern em vez de getters -
// porque o objetivo aqui nao e API bonita, e nao duplicar estado entre os
// dois arquivos.
#pragma once

#include "app.h"
#include "scroll.hpp"

// ---------------------------------------------------------------------------
// GEOMETRIA COMPARTILHADA - usada por core.cpp (area_util(), calculos de
// toque) e shell.cpp (desenho da barra, da home, da navegacao).
// ---------------------------------------------------------------------------
static const int ALTURA_BARRA = 64;
static const int ALTURA_NAV   = 72;   // era 96 - alta demais com a fonte suave

// CORRECAO VERTICAL DO TOQUE (o dedo encosta abaixo de onde o olho mira) - o
// porque esta em shell.cpp, junto de tratar_toque(). Mora aqui porque os DOIS
// caminhos de toque precisam dela: o clique (shell.cpp) e o toque por nivel
// (core.cpp). Antes so o clique corrigia, e o teclado virtual - que decide a
// tecla pelo nivel - acertava 10 px abaixo do que o mesmo dedo acertava num botao.
static const int TOQUE_DY = 10;

// ---------------------------------------------------------------------------
// SPLASH DE BOOT - liga/desliga nos dois arquivos ao mesmo tempo (a
// DEFINICAO mora em shell.cpp, a CHAMADA em core.cpp). Ver o aviso original
// junto de desenhar_splash() em shell.cpp para o porque de existir.
// ---------------------------------------------------------------------------
#define TAB5_MOSTRAR_SPLASH

// ---------------------------------------------------------------------------
// ESTADO COMPARTILHADO - definido em core.cpp, lido (nunca escrito) por
// shell.cpp.
// ---------------------------------------------------------------------------
extern const app_t* s_app;             // NULL = tela inicial
extern const app_t* s_app_carregando;  // app com montagem de SD pendente
extern const app_t* s_app_recusado;    // app que o sistema nao abriu (tela de motivo no lugar)
extern rolagem_t    s_rol;             // rolagem do app aberto - mecanismo em kernel/scroll.hpp
extern bool         s_sujo_layout;     // a TELA mudou - pede repintura completa
extern bool         s_sujo_dados;      // so um VALOR mudou - pede repintura incremental
extern bool         s_nav_travada;     // navegacao travada (teclado/captura na tela)
extern bool         s_bloqueado;       // tela de bloqueio na frente de tudo (home ou app ficam por baixo)
extern bool         s_painel_avisos;   // central de avisos aberta por cima do app/home

// O APP ESTA ESCONDIDO atras de algo do sistema (bloqueio ou central de
// avisos). Enquanto for verdade ele nao recebe toque, tecla nem aviso - nao
// esta visivel, e um app que "le" o que mostra (conversa) marcaria como lido
// o que ninguem viu. UMA regra so, para nao esquecer um dos dois em algum
// dos caminhos de entrada.
static inline bool sistema_app_encoberto(void) { return s_bloqueado || s_painel_avisos; }

// HA UM APP ABERTO DE VERDADE: ao_abrir() ja rodou. Nem "abrindo..." (SD
// montando) nem recusado (tela de motivo do sistema) - nesses dois o app nao
// tem estado nenhum, e nenhuma funcao dele pode ser chamada. UMA regra so,
// para todos os caminhos que chamam o app.
static inline bool app_vivo(void) { return s_app && s_app != s_app_carregando && s_app != s_app_recusado; }

// ---------------------------------------------------------------------------
// FUNCOES DE core.cpp, chamadas por shell.cpp (ciclo de vida dos apps).
// ---------------------------------------------------------------------------
// Quem chamou esta rodando na tarefa de interface?
bool sistema_na_interface(void);

// Troca o relogio do painel (destroi e recria as paginas) NA TAREFA DE
// INTERFACE - de outra tarefa, deixa o pedido e espera ate 5 s.
bool sistema_trocar_relogio_painel(int mhz);

// PASSADA COMPLETA POR APP (abrir app, voltar a home), acumulado desde o
// boot, para o 'tela' do console. As tres fases somam o total aproximado:
// barra de status, limpeza da area (+ barra de navegacao) e o conteudo
// (o desenhar() do app, ou a home).
typedef struct {
  const char* nome;
  uint32_t n;
  uint32_t max_us;
  uint64_t us, us_barra, us_limpar, us_conteudo;
} sistema_passada_t;
int sistema_passadas_completas(sistema_passada_t* out, int max);

bool abrir_app(const app_t* app);
void fechar_app(void);

// ---------------------------------------------------------------------------
// FUNCOES DE shell.cpp, chamadas por core.cpp (desenho e o loop principal).
// ---------------------------------------------------------------------------
#ifdef TAB5_MOSTRAR_SPLASH
void desenhar_splash(void);
#endif
void tratar_toque(int tx, int ty);
void desenhar_barra_status(lgfx::LovyanGFX& d, bool completo);
bool barra_mudou(void);
void desenhar_barra_nav(lgfx::LovyanGFX& d);
void desenhar_home(lgfx::LovyanGFX& d, const area_t& a);
// PAGINAS DA HOME. Quantas linhas de icones cabem por pagina; com mais apps
// que isso a home ganha bolinhas embaixo e o arraste lateral troca de pagina.
// 4 = tudo o que a area comporta hoje (12 apps). Baixe para 3 para testar a
// segunda pagina com os apps que ja existem.
#define TAB5_HOME_LINHAS_MAX 4

// GESTOS: chamado na soltura do dedo com o deslocamento total (dx, dy) desde a
// descida, ja descartados o dedo que acordou a tela e o clique comum. Na home
// troca de pagina; com o bloqueio ligado, arrastar para cima desbloqueia.
void tratar_gesto(int dx, int dy);

// MOVER ICONE NA HOME. home_segurar() e chamado a cada volta com o dedo na
// tela (home aberta, sem bloqueio): depois de HOME_SEGURAR_MS parado sobre um
// icone ele "levanta" e passa a seguir o dedo; devolve true enquanto ha icone
// levantado, e entao o toque nao vira clique nem gesto. home_soltar() na
// soltura grava a ordem nova. home_ordem_carregar() uma vez, antes do laco.
bool home_segurar(int tx, int ty, uint32_t agora, uint32_t segurando_ms, int desvio);
bool home_soltar(void);
void home_ordem_carregar(void);

// BLOQUEIO. desenhar_bloqueio() pinta a area util inteira (completo) ou so o
// que mudou desde a ultima vez (relogio, aviso); a barra de status de cima
// continua a de sempre. bloquear_tela()/desbloquear_tela() so mexem no estado
// e pedem repintura completa.
void desenhar_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool completo);
void bloquear_tela(void);
// A TELA ACABOU DE LIGAR (boot ou despertar): a UNICA regra de quando a
// tela de bloqueio aparece. Ajuste ligado e ainda nao bloqueada: bloqueia;
// ja bloqueada: sorteia outra animacao. Quem liga a tela chama isto, e so.
void bloqueio_ao_ligar_tela(void);
void desbloquear_tela(void);

// CENTRAL DE AVISOS (shell.cpp). Tocar a barra de status abre uma lista de
// TODAS as fontes ativas (notif_listar), uma linha por app, por cima da area
// util; tocar uma linha abre o app dela, tocar fora (ou a barra de novo)
// fecha. painel_avisos_mudou() diz ao loop que a lista aberta envelheceu.
void painel_avisos_abrir(bool abrir);
bool painel_avisos_mudou(void);
void desenhar_painel_avisos(lgfx::LovyanGFX& d, const area_t& a, bool completo);

void atualizar_linha_servicos(void);   // testa e redesenha a faixa GROVE/CARTAO da home, se mudou

// Diagnostico de toque ligado pelo console ('toque') - kernel/shell.cpp.
bool diag_toque_ligado(void);
