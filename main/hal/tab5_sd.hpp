// Cartao SD do Tab5, tratado como "disco do sistema".
//
// A ideia: cada aplicativo tem a sua folder em /sdcard/apps/<Nome>/ e nao
// precisa saber nada sobre montagem, pins ou FAT. Ele pede um file dentro
// da propria folder e escreve com fopen/fprintf/fwrite normais - o VFS do IDF
// faz o resto.
//
// FIACAO (do proprio M5Unified, tabela _pin_table_sd):
//   CLK=43  CMD=44  D0=39  D1=40  D2=41  D3=42
// Seis pins = SDMMC de 4 bits, nao SPI. Bem mais rapido, e o P4 permite
// remapear porque tem GPIO matrix para o SDMMC.
//
// MONTAGEM POR CONTAGEM DE REFERENCIA: mount e unmount leva ~100ms e desgasta
// nada, mas cartao mounted consome. Entao who precisa chama mount(), who
// termina chama unmount(), e a montagem real so acontece na primeira e a
// desmontagem na latest. O sistema faz isso sozinho para apps que declararam
// usa_sd - o app nao precisa chamar nenhuma das duas.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "sd_protocol_types.h"   // sdmmc_card_t (o cartao raw do pendrive)

// Onde o cartao aparece no sistema de arquivos.
#define TAB5_SD_ROOT   "/sdcard"
#define TAB5_SD_APPS   TAB5_SD_ROOT "/apps"

// Monta o cartao (ou apenas incrementa a contagem, se ja estiver mounted).
// Devolve false se nao ha cartao ou se a montagem failed - e nesse caso a
// contagem NAO e incrementada.
// Prepara o modulo. Chamar UMA vez, antes de qualquer tarefa existir.
void tab5_sd_init(void);

bool tab5_sd_mount(void);

// Decrementa a contagem; desmonta de verdade quando ela chega a zero.
void tab5_sd_unmount(void);

bool tab5_sd_mounted(void);

// Diagnostico: uptime (us) da latest montagem e desmontagem de verdade (0 = nunca).
void tab5_sd_last_transitions(int64_t* montou_us, int64_t* desmontou_us);

// Caminho da folder do app, CRIADA se nao existir.
//   tab5_sd_folder("Azimutal", buf, sizeof buf) -> "/sdcard/apps/Azimutal"
// Caracteres problematicos no name (space, barra, acento) viram '_'.
bool tab5_sd_folder(const char* app, char* out, size_t out_tam);

// Abre um file DENTRO da folder do app, criando a folder se preciso.
// mode e o do fopen ("w", "a", "r"...). Devolve NULL em qualquer failure.
//   FILE* f = tab5_sd_open("Azimutal", "satelites.csv", "a");
FILE* tab5_sd_open(const char* app, const char* file, const char* mode);

// "app" aceita subpasta: "Sistema/logs" -> /sdcard/apps/Sistema/logs (cada
// nivel e criado se faltar). As subpastas do sistema:
#define TAB5_SD_LOGS    "Sistema/logs"      // log_AAAA-MM-DD.txt
#define TAB5_SD_POWER "Sistema/energia"   // energia-AAAA-MM-DD.csv

// ---------------------------------------------------------------------------
// AS PORTAS DE GRAVACAO - todo dado que o sistema grava passa por uma delas
// ---------------------------------------------------------------------------
// Tres jeitos de write, e so tres:
//
//   tab5_sd_enqueue_append      append linha, SEM esperar (vai em lote)
//   tab5_sd_append append um bloco AGORA (who ja junta em lote: o log)
//   tab5_sd_replace  trocar o file inteiro, sem nunca deixar storage file
//
// Por que so por elas: o cartao pode estar EMPRESTADO ao computador (mode
// pendrive, ver mais abaixo). Ai o que for gravado por uma porta vai para o
// diario na flash interna (hal/tab5_journal) e chega ao cartao na volta, na
// ordem em que aconteceu. Quem grava com fopen() direto perde o dado nessa
// hora - e, se escrevesse no cartao com o computador usando, o corromperia.
//
// tab5_sd_open() fica para LER, e para file novo GRANDE (foto, firmware
// recebido pelo console): esses exigem o cartao de verdade, e o sistema nao
// deixa abrir who os produz com o cartao lent (app_t.exige_sd).
//
// A ORDEM e garantida: as tres portas, o lending e a aplicacao do diario
// passam pela mesma trava. Substituir grava antes o que estava para
// append. Um mesmo file nao deve receber pelas DUAS portas de
// append (o lote do enqueue_append pode sair depois).
//
// LEITURA SEMPRE VE A ULTIMA ESCRITA: tab5_sd_open() grava o pending e
// aplica o diario de um pendrive ANTES de abrir. Quem le com fopen() direto
// (o Arquivos) chama tab5_sd_flush_pending() antes.
//
// ---- enqueue_append (o gravador) -----------------------------------------------------
// Antes, SETE modulos gravavam no cartao cada um do seu jeito. Agora who quer
// append uma linha ENTREGA e segue: tab5_sd_enqueue_append copia para um buffer
// na PSRAM e volta na hora, sem tocar no cartao. Uma tarefa so ("cartao")
// grava em lote: monta UMA vez, abre cada file UMA vez, escreve tudo o que
// ele tem pending, na ordem em que chegou. Grava:
//   - a cada 5 s, se houver algo pending;
//   - na hora, para o que foi pedido com 'urgent' (mensagem de conversa);
//   - quando o buffer passa da half.
//
// 'header' (opcional): escrito uma vez, se o file estiver vazio ao abrir
// - a linha de titulo de um CSV.
//
// Falhou (sem cartao, buffer cheio): a linha se perde e conta em 'lost' -
// mesmo trato de antes, quando cada modulo simplesmente nao gravava.
bool tab5_sd_enqueue_append(const char* app, const char* file, const char* text,
                    const char* header = nullptr, bool urgent = false);

// Grava AGORA tudo o que estiver pending (e o diario de um pendrive, se o
// cartao ja voltou) e espera terminar. Para who vai read por fora do
// tab5_sd_open.
void tab5_sd_flush_pending(void);

// ---- append e replace -----------------------------------------------
// Bloqueiam (cartao ou flash): nunca da tarefa de interface - use o worker
// (sistema_trabalho) ou a piggyback da tarefa "cartao". false = nao gravou (sem
// cartao e sem lending, ou o diario cheio).
bool tab5_sd_append(const char* app, const char* file, const void* data, size_t n);

// SIGILOSO: o conteudo antigo e zerado antes de apagado (senhas) - no FAT,
// erase so brand os setores como livres.
#define TAB5_SD_SIGILOSO  0x01
bool tab5_sd_replace(const char* app, const char* file, const void* data, size_t n,
                        uint8_t flags = 0);

// Diagnostico: bytes esperando, linhas written e lost desde o boot.
void tab5_sd_append_status(uint32_t* pending, uint32_t* written, uint32_t* lost);

// CARONA NA TAREFA "cartao". Um service que so precisa, de tempos em tempos,
// esvaziar algo seu no cartao (o log) nao ganha tarefa propria: registra uma
// funcao, e a tarefa "cartao" a chama a cada volta - no maximo 1 s entre
// voltas, as vezes menos (quando um anexo urgent a acorda). Quem pega piggyback
// mede o proprio time (esp_timer), sem contar voltas.
// A funcao roda na stack do "cartao" (6 KB): pode mount e abrir file, nao
// pode esperar muito (as gravacoes de todos esperam junto). Ate 8 (CARONAS_MAX).
typedef void (*tab5_sd_piggyback_t)(void);
bool tab5_sd_piggyback(tab5_sd_piggyback_t fn);

// Espaco do cartao, em bytes. Qualquer ponteiro pode ser NULL.
bool tab5_sd_space(uint64_t* total, uint64_t* free_space);

// Descricao curta do cartao para mostrar na tela ("SDHC 29.7GB"), ou
// "sem cartao". Nunca devolve NULL.
const char* tab5_sd_description(void);

// O name do cartao (rotulo do volume, o mesmo que o computador mostra), ou ""
// se ele nao tem name ou ainda nao montou neste boot. Nunca NULL.
const char* tab5_sd_name(void);

// ---------------------------------------------------------------------------
// EMPRESTIMO AO COMPUTADOR - o mode pendrive (services/tab5_flashdrive)
// ---------------------------------------------------------------------------
// O cartao nao pode ter DOIS donos do sistema de arquivos. O FAT do Tab5
// guarda tabela e diretorio em cache; o do computador tambem. Se os dois
// escrevem, cada um sobrescreve o que o outro acha que esta la - e isso e
// cartao corrompido, nao file perdido. Por isso lend e exclusivo:
//
//   1. o que estava para write (tab5_sd_enqueue_append) vai para o cartao antes;
//   2. mount() passa a recusar, na hora e calado - para o resto do sistema
//      e como se nao houvesse cartao (todos ja tratam isso);
//   3. espera who ja estava com ele return_card, ate 'espera_ms'; passou disso,
//      desiste (SD_LENDING_IN_USE) e tudo volta como estava;
//   4. desmonta de verdade (sem cache do FAT do Tab5) e inicializa o cartao
//      CRU, so setores - e isso que o computador recebe.
//
// Enquanto lent, as portas de gravacao (acima) vao para o DIARIO na
// flash interna. Na devolucao a tarefa "cartao" o aplica, em ordem: append
// vai para o end do file; replace troca o file SE o computador nao
// mexeu nele (comparado com a foto de size e data tirada no lending) -
// se mexeu, o do computador fica e o do Tab5 vai para "<name>.tab5". Um
// reinicio no storage nao perde nada: o diario esta na flash e e aplicado quando
// o cartao aparecer.
//
// Bloqueia (ate espera_ms + a inicializacao do cartao): nunca da tarefa de
// interface.
typedef enum {
  SD_LENT = 0,
  SD_LENDING_IN_USE,      // alguem segurou o cartao alem do prazo
  SD_LENDING_NO_CARD,  // nao ha cartao, ou ele nao respondeu
  SD_LENDING_NO_JOURNAL,  // a particao "reserva" nao existe (tabela antiga)
} tab5_sd_lending_t;
tab5_sd_lending_t tab5_sd_lend(uint32_t espera_ms);

// O cartao raw (sdmmc_read_sectors/sdmmc_write_sectors), ou NULL se nao
// estiver lent. So para who pediu o lending.
sdmmc_card_t* tab5_sd_raw(void);

// Devolve: solta o cartao raw, mount() volta a funcionar e o que ficou
// esperando e gravado. Chamar SO depois de o computador ter parado de usar o
// cartao (USB desligado).
void tab5_sd_return(void);

bool tab5_sd_lent(void);

// O que aconteceu com o diario deste lending (para a tela).
typedef struct {
  uint32_t applied;     // gravacoes que ja foram para o cartao
  uint32_t conflicts;     // arquivos que o computador mudou: o do Tab5 ficou em .tab5
  uint32_t discarded;   // registros sem como aplicar (name invalido)
  uint32_t lost;      // nao couberam no diario (desde o boot)
  uint32_t in_journal;     // ainda esperando o cartao
  uint32_t journal_used, journal_total;   // bytes
  bool     complete;      // o diario inteiro ja foi aplicado
} tab5_sd_summary_t;
void tab5_sd_summary(tab5_sd_summary_t* r);
