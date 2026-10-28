// Cartao SD do Tab5, tratado como "disco do sistema".
//
// A ideia: cada aplicativo tem a sua pasta em /sdcard/apps/<Nome>/ e nao
// precisa saber nada sobre montagem, pinos ou FAT. Ele pede um arquivo dentro
// da propria pasta e escreve com fopen/fprintf/fwrite normais - o VFS do IDF
// faz o resto.
//
// FIACAO (do proprio M5Unified, tabela _pin_table_sd):
//   CLK=43  CMD=44  D0=39  D1=40  D2=41  D3=42
// Seis pinos = SDMMC de 4 bits, nao SPI. Bem mais rapido, e o P4 permite
// remapear porque tem GPIO matrix para o SDMMC.
//
// MONTAGEM POR CONTAGEM DE REFERENCIA: montar e desmontar leva ~100ms e desgasta
// nada, mas cartao montado consome. Entao quem precisa chama montar(), quem
// termina chama desmontar(), e a montagem real so acontece na primeira e a
// desmontagem na ultima. O sistema faz isso sozinho para apps que declararam
// usa_sd - o app nao precisa chamar nenhuma das duas.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "sd_protocol_types.h"   // sdmmc_card_t (o cartao cru do pendrive)

// Onde o cartao aparece no sistema de arquivos.
#define TAB5_SD_RAIZ   "/sdcard"
#define TAB5_SD_APPS   TAB5_SD_RAIZ "/apps"

// Monta o cartao (ou apenas incrementa a contagem, se ja estiver montado).
// Devolve false se nao ha cartao ou se a montagem falhou - e nesse caso a
// contagem NAO e incrementada.
// Prepara o modulo. Chamar UMA vez, antes de qualquer tarefa existir.
void tab5_sd_iniciar(void);

bool tab5_sd_montar(void);

// Decrementa a contagem; desmonta de verdade quando ela chega a zero.
void tab5_sd_desmontar(void);

bool tab5_sd_montado(void);

// Diagnostico: uptime (us) da ultima montagem e desmontagem de verdade (0 = nunca).
void tab5_sd_ultimas_transicoes(int64_t* montou_us, int64_t* desmontou_us);

// Caminho da pasta do app, CRIADA se nao existir.
//   tab5_sd_pasta("Azimutal", buf, sizeof buf) -> "/sdcard/apps/Azimutal"
// Caracteres problematicos no nome (espaco, barra, acento) viram '_'.
bool tab5_sd_pasta(const char* app, char* out, size_t out_tam);

// Abre um arquivo DENTRO da pasta do app, criando a pasta se preciso.
// modo e o do fopen ("w", "a", "r"...). Devolve NULL em qualquer falha.
//   FILE* f = tab5_sd_abrir("Azimutal", "satelites.csv", "a");
FILE* tab5_sd_abrir(const char* app, const char* arquivo, const char* modo);

// "app" aceita subpasta: "Sistema/logs" -> /sdcard/apps/Sistema/logs (cada
// nivel e criado se faltar). As subpastas do sistema:
#define TAB5_SD_LOGS    "Sistema/logs"      // log_AAAA-MM-DD.txt
#define TAB5_SD_ENERGIA "Sistema/energia"   // energia-AAAA-MM-DD.csv

// ---------------------------------------------------------------------------
// AS PORTAS DE GRAVACAO - todo dado que o sistema grava passa por uma delas
// ---------------------------------------------------------------------------
// Tres jeitos de gravar, e so tres:
//
//   tab5_sd_anexar      acrescentar linha, SEM esperar (vai em lote)
//   tab5_sd_acrescentar acrescentar um bloco AGORA (quem ja junta em lote: o log)
//   tab5_sd_substituir  trocar o arquivo inteiro, sem nunca deixar meio arquivo
//
// Por que so por elas: o cartao pode estar EMPRESTADO ao computador (modo
// pendrive, ver mais abaixo). Ai o que for gravado por uma porta vai para o
// diario na flash interna (hal/tab5_journal) e chega ao cartao na volta, na
// ordem em que aconteceu. Quem grava com fopen() direto perde o dado nessa
// hora - e, se escrevesse no cartao com o computador usando, o corromperia.
//
// tab5_sd_abrir() fica para LER, e para arquivo novo GRANDE (foto, firmware
// recebido pelo console): esses exigem o cartao de verdade, e o sistema nao
// deixa abrir quem os produz com o cartao emprestado (app_t.exige_sd).
//
// A ORDEM e garantida: as tres portas, o emprestimo e a aplicacao do diario
// passam pela mesma trava. Substituir grava antes o que estava para
// acrescentar. Um mesmo arquivo nao deve receber pelas DUAS portas de
// acrescentar (o lote do anexar pode sair depois).
//
// LEITURA SEMPRE VE A ULTIMA ESCRITA: tab5_sd_abrir() grava o pendente e
// aplica o diario de um pendrive ANTES de abrir. Quem le com fopen() direto
// (o Arquivos) chama tab5_sd_gravar_pendentes() antes.
//
// ---- anexar (o gravador) -----------------------------------------------------
// Antes, SETE modulos gravavam no cartao cada um do seu jeito. Agora quem quer
// acrescentar uma linha ENTREGA e segue: tab5_sd_anexar copia para um buffer
// na PSRAM e volta na hora, sem tocar no cartao. Uma tarefa so ("cartao")
// grava em lote: monta UMA vez, abre cada arquivo UMA vez, escreve tudo o que
// ele tem pendente, na ordem em que chegou. Grava:
//   - a cada 5 s, se houver algo pendente;
//   - na hora, para o que foi pedido com 'urgente' (mensagem de conversa);
//   - quando o buffer passa da metade.
//
// 'cabecalho' (opcional): escrito uma vez, se o arquivo estiver vazio ao abrir
// - a linha de titulo de um CSV.
//
// Falhou (sem cartao, buffer cheio): a linha se perde e conta em 'perdidas' -
// mesmo trato de antes, quando cada modulo simplesmente nao gravava.
bool tab5_sd_anexar(const char* app, const char* arquivo, const char* texto,
                    const char* cabecalho = nullptr, bool urgente = false);

// Grava AGORA tudo o que estiver pendente (e o diario de um pendrive, se o
// cartao ja voltou) e espera terminar. Para quem vai ler por fora do
// tab5_sd_abrir.
void tab5_sd_gravar_pendentes(void);

// ---- acrescentar e substituir -----------------------------------------------
// Bloqueiam (cartao ou flash): nunca da tarefa de interface - use o worker
// (sistema_trabalho) ou a carona da tarefa "cartao". false = nao gravou (sem
// cartao e sem emprestimo, ou o diario cheio).
bool tab5_sd_acrescentar(const char* app, const char* arquivo, const void* dados, size_t n);

// SIGILOSO: o conteudo antigo e zerado antes de apagado (senhas) - no FAT,
// apagar so marca os setores como livres.
#define TAB5_SD_SIGILOSO  0x01
bool tab5_sd_substituir(const char* app, const char* arquivo, const void* dados, size_t n,
                        uint8_t flags = 0);

// Diagnostico: bytes esperando, linhas gravadas e perdidas desde o boot.
void tab5_sd_anexos_estado(uint32_t* pendentes, uint32_t* gravadas, uint32_t* perdidas);

// CARONA NA TAREFA "cartao". Um servico que so precisa, de tempos em tempos,
// esvaziar algo seu no cartao (o log) nao ganha tarefa propria: registra uma
// funcao, e a tarefa "cartao" a chama a cada volta - no maximo 1 s entre
// voltas, as vezes menos (quando um anexo urgente a acorda). Quem pega carona
// mede o proprio tempo (esp_timer), sem contar voltas.
// A funcao roda na pilha do "cartao" (6 KB): pode montar e abrir arquivo, nao
// pode esperar muito (as gravacoes de todos esperam junto). Ate 8 (CARONAS_MAX).
typedef void (*tab5_sd_carona_t)(void);
bool tab5_sd_carona(tab5_sd_carona_t fn);

// Espaco do cartao, em bytes. Qualquer ponteiro pode ser NULL.
bool tab5_sd_espaco(uint64_t* total, uint64_t* livre);

// Descricao curta do cartao para mostrar na tela ("SDHC 29.7GB"), ou
// "sem cartao". Nunca devolve NULL.
const char* tab5_sd_descricao(void);

// O nome do cartao (rotulo do volume, o mesmo que o computador mostra), ou ""
// se ele nao tem nome ou ainda nao montou neste boot. Nunca NULL.
const char* tab5_sd_nome(void);

// ---------------------------------------------------------------------------
// EMPRESTIMO AO COMPUTADOR - o modo pendrive (services/tab5_flashdrive)
// ---------------------------------------------------------------------------
// O cartao nao pode ter DOIS donos do sistema de arquivos. O FAT do Tab5
// guarda tabela e diretorio em cache; o do computador tambem. Se os dois
// escrevem, cada um sobrescreve o que o outro acha que esta la - e isso e
// cartao corrompido, nao arquivo perdido. Por isso emprestar e exclusivo:
//
//   1. o que estava para gravar (tab5_sd_anexar) vai para o cartao antes;
//   2. montar() passa a recusar, na hora e calado - para o resto do sistema
//      e como se nao houvesse cartao (todos ja tratam isso);
//   3. espera quem ja estava com ele devolver, ate 'espera_ms'; passou disso,
//      desiste (SD_EMPRESTIMO_EM_USO) e tudo volta como estava;
//   4. desmonta de verdade (sem cache do FAT do Tab5) e inicializa o cartao
//      CRU, so setores - e isso que o computador recebe.
//
// Enquanto emprestado, as portas de gravacao (acima) vao para o DIARIO na
// flash interna. Na devolucao a tarefa "cartao" o aplica, em ordem: acrescentar
// vai para o fim do arquivo; substituir troca o arquivo SE o computador nao
// mexeu nele (comparado com a foto de tamanho e data tirada no emprestimo) -
// se mexeu, o do computador fica e o do Tab5 vai para "<nome>.tab5". Um
// reinicio no meio nao perde nada: o diario esta na flash e e aplicado quando
// o cartao aparecer.
//
// Bloqueia (ate espera_ms + a inicializacao do cartao): nunca da tarefa de
// interface.
typedef enum {
  SD_EMPRESTADO = 0,
  SD_EMPRESTIMO_EM_USO,      // alguem segurou o cartao alem do prazo
  SD_EMPRESTIMO_SEM_CARTAO,  // nao ha cartao, ou ele nao respondeu
  SD_EMPRESTIMO_SEM_DIARIO,  // a particao "reserva" nao existe (tabela antiga)
} tab5_sd_emprestimo_t;
tab5_sd_emprestimo_t tab5_sd_emprestar(uint32_t espera_ms);

// O cartao cru (sdmmc_read_sectors/sdmmc_write_sectors), ou NULL se nao
// estiver emprestado. So para quem pediu o emprestimo.
sdmmc_card_t* tab5_sd_cru(void);

// Devolve: solta o cartao cru, montar() volta a funcionar e o que ficou
// esperando e gravado. Chamar SO depois de o computador ter parado de usar o
// cartao (USB desligado).
void tab5_sd_devolver(void);

bool tab5_sd_emprestado(void);

// O que aconteceu com o diario deste emprestimo (para a tela).
typedef struct {
  uint32_t aplicadas;     // gravacoes que ja foram para o cartao
  uint32_t conflitos;     // arquivos que o computador mudou: o do Tab5 ficou em .tab5
  uint32_t descartadas;   // registros sem como aplicar (nome invalido)
  uint32_t perdidas;      // nao couberam no diario (desde o boot)
  uint32_t no_diario;     // ainda esperando o cartao
  uint32_t diario_usado, diario_total;   // bytes
  bool     completo;      // o diario inteiro ja foi aplicado
} tab5_sd_resumo_t;
void tab5_sd_resumo(tab5_sd_resumo_t* r);
