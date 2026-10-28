// CONVERSA - o componente de chat do sistema.
//
// Baloes de mensagem, barra de escrita, teclado virtual (e o fisico do
// ExtPort1), rolagem e divisor de dia - tudo o que um app de mensagem
// precisa, num lugar so. Nasceu do app Mesh (mesmas medidas, cores e regras,
// medidas em hardware ao longo de setembro), para o ESP-NOW nao ser uma copia
// dele e o proximo app de mensagem nao ser uma terceira.
//
// O APP SO RESPONDE TRES PERGUNTAS, por funcao:
//   - quantas mensagens ha            (contar)
//   - como e a mensagem i (0 = a mais antiga)   (ler)
//   - envie este texto                (enviar)
// O resto e do componente. Ele guarda as medidas de cada balao (medir texto e
// caro) e so re-mede quando o app avisa que algo mudou (conversa_mudou).
//
// COMO USAR (ver app_espnow/app_espnow.cpp):
//   ao_abrir:        conversa_abrir(&s_c, &s_p, contar, ler, enviar, nullptr);
//   ao_fechar:       conversa_fechar(&s_c);
//   ao_aviso:        conversa_mudou(&s_c);        // chegou mensagem, mudou estado
//   desenhar:        conversa_desenhar(&s_c, d, retangulo, completo, pode_escrever, motivo);
//   ao_tocar:        conversa_tocar(&s_c, x, y);
//   ao_tocar_nivel:  conversa_tocar_nivel(&s_c, x, y, pressionado);
//   ao_teclar:       conversa_teclar(&s_c, texto, codigo, mod);
//
// O RETANGULO vai ate a BASE da area do app e ocupa a largura inteira dela: o
// teclado virtual abre por cima da parte de baixo, e a barra de escrita sobe
// junto. Em cima dele o app poe o que quiser (a barra de informacoes do
// ESP-NOW, por exemplo).
#pragma once

#include "app.h"
#include "list.hpp"
#include "paint.hpp"
#include "virtual_keyboard.hpp"

#define CONVERSA_TEXTO        256
// Bytes de UMA linha desenhada. Com acento (2 bytes) e emoji (4), 64 acabava
// antes da largura da tela; 128 cobre uma linha inteira mesmo so de emoji.
#define CONVERSA_LINHA_BYTES  128
#define CONVERSA_MAX_LINHAS   16

typedef enum {
  CONVERSA_ENVIO_NADA = 0,   // recebida (sem marca)
  CONVERSA_ENVIO_PENDENTE,
  CONVERSA_ENVIO_OK,
  CONVERSA_ENVIO_FALHOU,
  CONVERSA_ENVIO_SEM_RESPOSTA,   // o prazo acabou sem o radio dizer nada (Mesh)
  CONVERSA_ENVIO_OUVIDO,         // a rede ouviu, o destino nao confirmou (visto cinza)
} conversa_envio_t;

typedef struct {
  char     texto[CONVERSA_TEXTO];
  bool     minha;               // a direita, com a marca de envio
  uint32_t quando;              // epoch UTC; 0 = sem data (nao abre divisor de dia)
  char     name[16];            // rotulo do remetente ("" = sem rotulo)
  uint16_t cor_nome;
  char     meta[24];            // linha pequena embaixo ("" = sem)
  uint16_t cor_meta;
  uint8_t  envio;               // conversa_envio_t
} conversa_msg_t;

typedef int  (*conversa_contar_t)(void* ctx);
typedef bool (*conversa_ler_t)(int i, conversa_msg_t* out, void* ctx);
typedef bool (*conversa_enviar_t)(const char* texto, void* ctx);

struct conversa_balao_s;

typedef struct {
  conversa_contar_t contar;
  conversa_ler_t    ler;
  conversa_enviar_t enviar;
  void*             ctx;
  pintura_t*        p;    // a pintura do APP: teclado abrindo/fechando pede passada completa
  // TETO DO RASCUNHO EM BYTES (0 = o buffer inteiro). O app define depois do
  // conversa_abrir quando o protocolo dele aceita menos que CONVERSA_TEXTO -
  // a barra para de aceitar letra, em vez de a mensagem sair cortada.
  int               limite;
  // COR DO BOTAO DE ENVIAR (0 = a padrao do sistema). O app pinta com a
  // identidade dele - o Mesh usa o verde da marca do Meshtastic.
  uint16_t          cor_enviar;

  // Estado interno - o app nao mexe.
  lista_t   lista;
  struct conversa_balao_s* bal;   // PSRAM, 'cap' entradas
  int       n, cap, larg;
  bool      medir;                // re-medir os baloes na proxima passada
  area_t    ret;                  // o retangulo do ultimo desenho (tela)
  char      rascunho[CONVERSA_TEXTO];
  tecvirt_t kb;                   // o teclado virtual do sistema (dedos, retorno, desenho)
  // BARRA DE ESCRITA. 'cursor' e o deslocamento em BYTES dentro do rascunho
  // (sempre na fronteira de um caractere UTF-8). A quebra em linhas guarda o
  // inicio de cada linha (bar_ini[n] = fim do texto) para o toque no campo
  // achar a posicao sem re-medir, e so e refeita quando o texto ou a largura
  // mudam - nao a cada passada.
  int       cursor;
  char      barra_txt[CONVERSA_TEXTO];   // o texto da ultima passada (para o "mudou")
  int       barra_base, barra_modo, barra_larg, barra_cursor, barra_topo;
  int       barra_env;                   // o botao de enviar como foi pintado (aceso/apagado)
  int16_t   bar_ini[CONVERSA_MAX_LINHAS + 2];
  int       bar_n, bar_topo, bar_nl;     // linhas do texto, primeira visivel, quantas visiveis
  int       bar_x0, bar_y0, bar_passo;   // onde a primeira linha visivel foi desenhada (tela)
  bool      bar_rascunho;                // o que esta na barra e o rascunho (nao a dica)
  lgfx::LovyanGFX* disp;                 // para medir texto ao tocar no campo (so a tarefa de interface)
  bool      pode_escrever;        // o do ultimo desenho (o toque obedece o que esta na tela)
  area_t    tec;                  // onde o teclado abre (tela); largura 0 = o proprio retangulo
} conversa_t;

void conversa_abrir(conversa_t* c, pintura_t* p, conversa_contar_t contar, conversa_ler_t ler,
                    conversa_enviar_t enviar, void* ctx);
void conversa_fechar(conversa_t* c);
void conversa_mudou(conversa_t* c);

// 'pode_escrever' false: a barra vira aviso ('motivo') e nao abre teclado.
void conversa_desenhar(conversa_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool completo, bool pode_escrever, const char* motivo);

// Coordenadas da AREA do app - as que ao_tocar/ao_tocar_nivel recebem.
void conversa_tocar(conversa_t* c, int x, int y);
void conversa_tocar_nivel(conversa_t* c, int x, int y, bool pressionado);
void conversa_teclar(conversa_t* c, const char* texto, uint8_t codigo, uint8_t mod);
bool conversa_teclado_aberto(const conversa_t* c);

// O TECLADO MAIS LARGO QUE A CONVERSA. No Mesh a conversa e so a
// coluna da direita, mas o teclado precisa da largura inteira da area (tecla
// estreita e erro de digitacao). 'ka' em coordenadas de TELA, com a MESMA base
// do retangulo da conversa. Chamar antes de conversa_desenhar.
void conversa_area_teclado(conversa_t* c, const area_t& ka);

// A conversa mostrada mudou (outro canal, outra pessoa): fecha o teclado, volta
// ao fim da lista e re-mede. O rascunho fica - e do usuario, nao da conversa.
void conversa_trocar(conversa_t* c);
void conversa_fechar_teclado(conversa_t* c);

// Quebra texto em linhas que cabem em 'largura_px' (por palavra; por caractere
// so quando uma palavra sozinha nao cabe). Publica para quem desenha texto
// corrido com a mesma regra dos baloes (o Mesh usa).
int conversa_quebrar_linhas(lgfx::LovyanGFX& d, const char* txt, int largura_px,
                            char linhas[][CONVERSA_LINHA_BYTES], int max_linhas);
