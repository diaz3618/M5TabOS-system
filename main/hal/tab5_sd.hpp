// Cartao SD of Tab5, tratado como "disco of sistema".
//
// A ideia: cada aplicativo tem a sua folder em /sdcard/apps/<Nome>/ e nao
// precisa saber nada sobre montagem, pins ou FAT. Ele pede one file dentro
// of propria folder e escreve com fopen/fprintf/fwrite normais - o VFS of IDF
// faz o resto.
//
// FIACAO (of proprio M5Unified, tabela _pin_table_sd):
//   CLK=43  CMD=44  D0=39  D1=40  D2=41  D3=42
// Seis pins = SDMMC of 4 bits, nao SPI. Bem mais rapido, e o P4 permite
// remapear porque tem GPIO matrix para o SDMMC.
//
// MONTAGEM POR CONTAGEM DE REFERENCIA: mount e unmount leva ~100ms e desgasta
// nada, mas cartao mounted consome. Entao who precisa chama mount(), who
// termina chama unmount(), e a montagem real so acontece in primeira e a
// desmontagem in latest. O sistema faz isso sozinho para apps que declararam
// usa_sd - o app nao precisa chamar none das duas.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "sd_protocol_types.h"   // sdmmc_card_t (o cartao raw of pendrive)

// Onde o cartao aparece no sistema of arquivos.
#define TAB5_SD_ROOT   "/sdcard"
#define TAB5_SD_APPS   TAB5_SD_ROOT "/apps"

// Monta o cartao (ou apenas incrementa a contagem, se ja estiver mounted).
// Devolve false se nao ha cartao ou se a montagem failed - e nesse caso a
// contagem NAO e incrementada.
// Prepara o modulo. Chamar UMA vez, antes of qualquer tarefa existir.
void tab5_sd_init(void);

bool tab5_sd_mount(void);

// Decrementa a contagem; desmonta of verdade when ela chega a zero.
void tab5_sd_unmount(void);

bool tab5_sd_mounted(void);

// Diagnostico: uptime (us) of latest montagem e desmontagem of verdade (0 = nunca).
void tab5_sd_last_transitions(int64_t* montou_us, int64_t* desmontou_us);

// Caminho of folder of app, CRIADA se nao existir.
//   tab5_sd_folder("Azimutal", buf, sizeof buf) -> "/sdcard/apps/Azimutal"
// Caracteres problematicos no name (space, barra, acento) viram '_'.
bool tab5_sd_folder(const char* app, char* out, size_t out_size);

// Abre one file DENTRO of folder of app, criando a folder se preciso.
// mode e o of fopen ("w", "a", "r"...). Devolve NULL em qualquer failure.
//   FILE* f = tab5_sd_open("Azimutal", "satelites.csv", "a");
FILE* tab5_sd_open(const char* app, const char* file, const char* mode);

// "app" aceita subpasta: "Sistema/logs" -> /sdcard/apps/Sistema/logs (cada
// level e criado se faltar). As subpastas of sistema:
#define TAB5_SD_LOGS    "Sistema/logs"      // log_AAAA-MM-DD.txt
#define TAB5_SD_POWER "Sistema/energia"   // energia-AAAA-MM-DD.csv

// ---------------------------------------------------------------------------
// AS PORTAS DE GRAVACAO - todo dado que o sistema grava passa por uma delas
// ---------------------------------------------------------------------------
// Tres jeitos of write, e so tres:
//
//   tab5_sd_enqueue_append      append line, SEM wait (vai em lote)
//   tab5_sd_append append one bloco AGORA (who ja junta em lote: o log)
//   tab5_sd_replace  switch o file inteiro, sem nunca deixar storage file
//
// Por que so por elas: o cartao pode estar EMPRESTADO on computador (mode
// pendrive, ver mais abaixo). Ai o que for gravado por uma porta vai para o
// diario in flash interna (hal/tab5_journal) e chega on cartao in iteration, in
// order em que aconteceu. Quem grava com fopen() direto perde o dado nessa
// hora - e, se escrevesse no cartao com o computador usando, o corromperia.
//
// tab5_sd_open() stays para LER, e para file novo GRANDE (foto, firmware
// recebido pelo console): esses exigem o cartao of verdade, e o sistema nao
// deixa open who os produz com o cartao lent (app_t.exige_sd).
//
// A ORDEM e garantida: as tres portas, o lending e a aplicacao of diario
// passam pela mesma trava. Substituir grava antes o que estava para
// append. Um mesmo file nao deve receber pelas DUAS portas of
// append (o lote of enqueue_append pode exit depois).
//
// LEITURA SEMPRE VE A ULTIMA ESCRITA: tab5_sd_open() grava o pending e
// aplica o diario of one pendrive ANTES of open. Quem le com fopen() direto
// (o Arquivos) chama tab5_sd_flush_pending() antes.
//
// ---- enqueue_append (o gravador) -----------------------------------------------------
// Antes, SETE modulos gravavam no cartao cada one of seu jeito. Agora who quer
// append uma line ENTREGA e segue: tab5_sd_enqueue_append copia para one buffer
// in PSRAM e iteration in hora, sem touch no cartao. Uma tarefa so ("cartao")
// grava em lote: monta UMA vez, abre cada file UMA vez, escreve everything o que
// ele tem pending, in order em que chegou. Grava:
//   - a cada 5 s, se houver algo pending;
//   - in hora, para o que foi request com 'urgent' (message of conversa);
//   - when o buffer passa of half.
//
// 'header' (opcional): escrito uma vez, se o file estiver vazio on open
// - a line of titulo of one CSV.
//
// Falhou (sem cartao, buffer cheio): a line se perde e conta em 'lost' -
// mesmo trato of antes, when cada modulo simplesmente nao gravava.
bool tab5_sd_enqueue_append(const char* app, const char* file, const char* text,
                    const char* header = nullptr, bool urgent = false);

// Grava AGORA everything o que estiver pending (e o diario of one pendrive, se o
// cartao ja voltou) e espera terminar. Para who vai read por fora of
// tab5_sd_open.
void tab5_sd_flush_pending(void);

// ---- append e replace -----------------------------------------------
// Bloqueiam (cartao ou flash): nunca of tarefa of interface - use o worker
// (sistema_trabalho) ou a piggyback of tarefa "cartao". false = nao gravou (sem
// cartao e sem lending, ou o diario cheio).
bool tab5_sd_append(const char* app, const char* file, const void* data, size_t n);

// SIGILOSO: o content antigo e zerado antes of apagado (senhas) - no FAT,
// erase so brand os setores como livres.
#define TAB5_SD_SIGILOSO  0x01
bool tab5_sd_replace(const char* app, const char* file, const void* data, size_t n,
                        uint8_t flags = 0);

// Diagnostico: bytes esperando, lines written e lost since o boot.
void tab5_sd_append_status(uint32_t* pending, uint32_t* written, uint32_t* lost);

// CARONA NA TAREFA "cartao". Um service que so precisa, of tempos em tempos,
// esvaziar algo seu no cartao (o log) nao ganha tarefa propria: registra uma
// funcao, e a tarefa "cartao" a chama a cada iteration - no maximum 1 s entre
// voltas, as vezes menos (when one anexo urgent a acorda). Quem pega piggyback
// mede o proprio time (esp_timer), sem count voltas.
// A funcao roda in stack of "cartao" (6 KB): pode mount e open file, nao
// pode wait muito (as gravacoes of todos esperam junto). Ate 8 (CARONAS_MAX).
typedef void (*tab5_sd_piggyback_t)(void);
bool tab5_sd_piggyback(tab5_sd_piggyback_t fn);

// Espaco of cartao, em bytes. Qualquer ponteiro pode ser NULL.
bool tab5_sd_space(uint64_t* total, uint64_t* free_space);

// Descricao curta of cartao para mostrar in screen ("SDHC 29.7GB"), ou
// "sem cartao". Nunca devolve NULL.
const char* tab5_sd_description(void);

// O name of cartao (rotulo of volume, o mesmo que o computador mostra), ou ""
// se ele nao tem name ou ainda nao montou neste boot. Nunca NULL.
const char* tab5_sd_name(void);

// ---------------------------------------------------------------------------
// EMPRESTIMO AO COMPUTADOR - o mode pendrive (services/tab5_flashdrive)
// ---------------------------------------------------------------------------
// O cartao nao pode ter DOIS donos of sistema of arquivos. O FAT of Tab5
// guarda tabela e diretorio em cache; o of computador tambem. Se os dois
// escrevem, cada one sobrescreve o que o outro acha que esta la - e isso e
// cartao corrompido, nao file perdido. Por isso lend e exclusivo:
//
//   1. o que estava para write (tab5_sd_enqueue_append) vai para o cartao antes;
//   2. mount() passa a recusar, in hora e calado - para o resto of sistema
//      e como se nao houvesse cartao (todos ja tratam isso);
//   3. espera who ja estava com ele return_card, ate 'espera_ms'; passou disso,
//      desiste (SD_LENDING_IN_USE) e everything iteration como estava;
//   4. desmonta of verdade (sem cache of FAT of Tab5) e inicializa o cartao
//      CRU, so setores - e isso que o computador recebe.
//
// Enquanto lent, as portas of gravacao (acima) vao para o DIARIO in
// flash interna. Na devolucao a tarefa "cartao" o aplica, em order: append
// vai para o end of file; replace troca o file SE o computador nao
// mexeu nele (comparado com a foto of size e data tirada no lending) -
// se mexeu, o of computador stays e o of Tab5 vai para "<name>.tab5". Um
// reinicio no storage nao perde nada: o diario esta in flash e e aplicado when
// o cartao aparecer.
//
// Bloqueia (ate espera_ms + a inicializacao of cartao): nunca of tarefa of
// interface.
typedef enum {
  SD_LENT = 0,
  SD_LENDING_IN_USE,      // alguem segurou o cartao alem of prazo
  SD_LENDING_NO_CARD,  // nao ha cartao, ou ele nao respondeu
  SD_LENDING_NO_JOURNAL,  // a particao "reserva" nao existe (tabela antiga)
} tab5_sd_lending_t;
tab5_sd_lending_t tab5_sd_lend(uint32_t espera_ms);

// O cartao raw (sdmmc_read_sectors/sdmmc_write_sectors), ou NULL se nao
// estiver lent. So para who pediu o lending.
sdmmc_card_t* tab5_sd_raw(void);

// Devolve: released o cartao raw, mount() iteration a funcionar e o que became
// esperando e gravado. Chamar SO depois of o computador ter parado of usar o
// cartao (USB disabled).
void tab5_sd_return(void);

bool tab5_sd_lent(void);

// O que aconteceu com o diario deste lending (para a screen).
typedef struct {
  uint32_t applied;     // gravacoes que ja foram para o cartao
  uint32_t conflicts;     // arquivos que o computador changed: o of Tab5 became em .tab5
  uint32_t discarded;   // registros sem como aplicar (name invalido)
  uint32_t lost;      // nao couberam no diario (since o boot)
  uint32_t in_journal;     // ainda esperando o cartao
  uint32_t journal_used, journal_total;   // bytes
  bool     complete;      // o diario inteiro ja foi aplicado
} tab5_sd_summary_t;
void tab5_sd_summary(tab5_sd_summary_t* r);
