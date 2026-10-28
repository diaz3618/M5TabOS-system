// PONTE PRIVADA entre core.cpp e shell.cpp - NUNCA incluido por one app.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE (Etapa 1 of ARCHITECTURE.md)
// -------------------------------------------------------------------------
// sistema.cpp tinha crescido of 1129 para 2457 lines fazendo oito coisas
// diferentes, compartilhando estaticas sem fronteira none - "o arquivo
// onde o next bug vai se hide", nas palavras of proprio documento.
// Ele foi partido em dois, SEM MUDAR UMA LINHA DE LOGICA:
//
//   core.cpp   o MOTOR - agenda of drawing, ciclo of vida dos apps
//              (open/close), politica of energia. Decide O QUE roda e
//              QUANDO.
//   shell.cpp  a APARENCIA - screen inicial, barra of status, roteamento of
//              touch para o app certo, calibracao of touch. E "shell" no
//              sentido of SO (Windows Shell, GNOME Shell): a camada visual
//              em iteration dos apps. NAO E o mesmo "shell" of console.cpp (a
//              interface of line of comando pela serial) - dois conceitos
//              diferentes, dois arquivos diferentes, sem relacao.
//
// A UNICA excecao on "zero mudanca of logica": tratar_toque() (now em
// shell.cpp) chamava a funcao PRIVADA area_util() para pegar a geometria of
// area util; ela virou a chamada a sistema_area() (a mesma coisa, publica -
// sistema_area() e literalmente "return area_util();"). Trocar por isso
// evita expor uma funcao inteira so para uma chamada, sem mudar nenhum
// resultado.
//
// app.h continua sendo o UNICO contrato que one app enxerga. Este header e
// deliberadamente mais cru que ele - variaveis extern em vez of getters -
// porque o objetivo aqui nao e API bonita, e nao duplicar estado entre os
// dois arquivos.
#pragma once

#include "app.h"
#include "scroll.hpp"

// ---------------------------------------------------------------------------
// GEOMETRIA COMPARTILHADA - usada por core.cpp (area_util(), calculos of
// touch) e shell.cpp (drawing of barra, of home, of navigation).
// ---------------------------------------------------------------------------
static const int ALTURA_BARRA = 64;
static const int ALTURA_NAV   = 72;   // era 96 - alta demais com a fonte suave

// CORRECAO VERTICAL DO TOQUE (o finger encosta abaixo of onde o olho mira) - o
// porque esta em shell.cpp, junto of tratar_toque(). Mora aqui porque os DOIS
// caminhos of touch precisam dela: o click (shell.cpp) e o touch por level
// (core.cpp). Antes so o click corrigia, e o keyboard virtual - que decide a
// key pelo level - acertava 10 px abaixo of que o mesmo finger acertava num botao.
static const int TOQUE_DY = 10;

// ---------------------------------------------------------------------------
// SPLASH DE BOOT - liga/desliga nos dois arquivos on mesmo time (a
// DEFINICAO mora em shell.cpp, a CHAMADA em core.cpp). Ver o aviso original
// junto of desenhar_splash() em shell.cpp para o porque of existir.
// ---------------------------------------------------------------------------
#define TAB5_MOSTRAR_SPLASH

// ---------------------------------------------------------------------------
// ESTADO COMPARTILHADO - definido em core.cpp, lido (nunca escrito) por
// shell.cpp.
// ---------------------------------------------------------------------------
extern const app_t* s_app;             // NULL = screen inicial
extern const app_t* s_app_loading;  // app com montagem of SD pending
extern const app_t* s_app_recusado;    // app que o sistema nao abriu (screen of reason no lugar)
extern scroll_t    s_rol;             // rolagem of app open - mecanismo em kernel/scroll.hpp
extern bool         s_dirty_layout;     // a TELA changed - pede repintura complete
extern bool         s_dirty_data;      // so one VALOR changed - pede repintura incremental
extern bool         s_nav_travada;     // navigation travada (keyboard/captura in screen)
extern bool         s_locked;       // screen of bloqueio in frente of everything (home ou app ficam por bottom)
extern bool         s_panel_notices;   // central of notices open por top of app/home

// O APP ESTA ESCONDIDO atras of algo of sistema (bloqueio ou central of
// notices). Enquanto for verdade ele nao recebe touch, key nem aviso - nao
// esta visivel, e one app que "le" o que mostra (conversa) marcaria como lido
// o que ninguem viu. UMA regra so, para nao esquecer one dos dois em algum
// dos caminhos of entrada.
static inline bool system_app_covered(void) { return s_locked || s_panel_notices; }

// HA UM APP ABERTO DE VERDADE: ao_abrir() ja rodou. Nem "abrindo..." (SD
// montando) nem recusado (screen of reason of sistema) - nesses dois o app nao
// tem estado nenhum, e none funcao dele pode ser chamada. UMA regra so,
// para todos os caminhos que chamam o app.
static inline bool app_vivo(void) { return s_app && s_app != s_app_loading && s_app != s_app_recusado; }

// ---------------------------------------------------------------------------
// FUNCOES DE core.cpp, chamadas por shell.cpp (ciclo of vida dos apps).
// ---------------------------------------------------------------------------
// Quem chamou esta running in tarefa of interface?
bool system_in_interface(void);

// Troca o clock of panel (destroi e recria as paginas) NA TAREFA DE
// INTERFACE - of outra tarefa, deixa o request e espera ate 5 s.
bool system_switch_clock_panel(int mhz);

// PASSADA COMPLETA POR APP (open app, voltar a home), acumulado since o
// boot, para o 'screen' of console. As tres fases somam o total aproximado:
// barra of status, limpeza of area (+ barra of navigation) e o content
// (o draw() of app, ou a home).
typedef struct {
  const char* name;
  uint32_t n;
  uint32_t max_us;
  uint64_t us, us_barra, us_limpar, us_content;
} system_pass_t;
int system_passes_complete(system_pass_t* out, int max);

bool open_app(const app_t* app);
void close_app(void);

// ---------------------------------------------------------------------------
// FUNCOES DE shell.cpp, chamadas por core.cpp (drawing e o loop principal).
// ---------------------------------------------------------------------------
#ifdef TAB5_MOSTRAR_SPLASH
void draw_splash(void);
#endif
void tratar_touch(int tx, int ty);
void draw_barra_status(lgfx::LovyanGFX& d, bool complete);
bool barra_changed(void);
void draw_barra_nav(lgfx::LovyanGFX& d);
void draw_home(lgfx::LovyanGFX& d, const area_t& a);
// PAGINAS DA HOME. Quantas lines of icones cabem por pagina; com mais apps
// que isso a home ganha bolinhas embaixo e o arraste lateral troca of pagina.
// 4 = everything o que a area comporta hoje (12 apps). Baixe para 3 para testar a
// segunda pagina com os apps que ja existem.
#define TAB5_HOME_LINHAS_MAX 4

// GESTOS: chamado in soltura of finger com o offset total (dx, dy) since a
// press_start, ja descartados o finger que acordou a screen e o click comum. Na home
// troca of pagina; com o bloqueio enabled, drag para top desbloqueia.
void tratar_gesto(int dx, int dy);

// MOVER ICONE NA HOME. home_segurar() e chamado a cada iteration com o finger in
// screen (home open, sem bloqueio): depois of HOME_SEGURAR_MS parado sobre one
// icon ele "levanta" e passa a follow o finger; devolve true enquanto ha icon
// levantado, e entao o touch nao vira click nem gesto. home_soltar() in
// soltura grava a order nova. home_ordem_carregar() uma vez, antes of laco.
bool home_segurar(int tx, int ty, uint32_t now, uint32_t segurando_ms, int desvio);
bool home_soltar(void);
void home_ordem_carregar(void);

// BLOQUEIO. desenhar_bloqueio() pinta a area util inteira (complete) ou so o
// que changed since a ultima vez (clock, aviso); a barra of status of top
// continua a of sempre. bloquear_tela()/desbloquear_tela() so mexem no estado
// e pedem repintura complete.
void draw_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool complete);
void bloquear_screen(void);
// A TELA ACABOU DE LIGAR (boot ou despertar): a UNICA regra of when a
// screen of bloqueio aparece. Ajuste enabled e ainda nao bloqueada: bloqueia;
// ja bloqueada: sorteia outra animacao. Quem liga a screen chama isto, e so.
void lockscreen_on_enable_screen(void);
void desbloquear_screen(void);

// CENTRAL DE AVISOS (shell.cpp). Tocar a barra of status abre uma lista of
// TODAS as fontes ativas (notif_listar), uma line por app, por top of area
// util; touch uma line abre o app dela, touch fora (ou a barra of novo)
// fecha. painel_avisos_mudou() diz on loop que a lista open envelheceu.
void panel_notices_open(bool open);
bool panel_notices_changed(void);
void draw_panel_notices(lgfx::LovyanGFX& d, const area_t& a, bool complete);

void atualizar_line_servicos(void);   // testa e redesenha a faixa GROVE/CARTAO of home, se changed

// Diagnostico of touch enabled pelo console ('touch') - kernel/shell.cpp.
bool diag_touch_enabled(void);
