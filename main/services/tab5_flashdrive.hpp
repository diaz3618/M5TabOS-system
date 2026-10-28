// USB-drive mode: the Tab5 SD card appears on the computer as a USB disk.
//
// -------------------------------------------------------------------------
// COMO, NO HARDWARE
// -------------------------------------------------------------------------
// The Tab5 USB-C port uses P4 full-speed PHY 0, normally owned by
// USB-Serial-JTAG (the console and flashing). P4 also has a second controller,
// the full-speed OTG controller ("USB_WRAP"), and a selector that assigns the
// PHY to either controller (LP_SYS.usb_ctrl.sw_usb_phy_sel). Enabling the
// USB drive releases PHY 0 from JTAG, assigns it to OTG, and makes TinyUSB
// advertise mass storage to the computer. Disabling it reverses that process
// and restores the console. This is the same approach used by M5Launcher
// (src/massStorage.cpp), which already runs on the Tab5.
//
// While USB-drive mode is active there is no console or cable flashing: the
// computer sees a disk instead of the serial port. Logging resumes on the card
// as soon as it is returned (tab5_sd_enqueue_append queues incoming records).
//
// -------------------------------------------------------------------------
// O CARTAO
// -------------------------------------------------------------------------
// There must never be two owners at once; see tab5_sd_emprestar(). The session
// borrows the card BEFORE enabling USB and returns it only AFTER USB is off.
//
// The computer receives SECTORS, not files: it reads and writes the card
// directly, with no cache on the Tab5 side (each write is already on the card
// when the computer receives "ok"). Only the COMPUTER'S cache can still be
// pending, so the disk should be ejected there before unplugging the cable.
// Settings > USB explains this and returns the card without an eject only if
// the user insists.
#pragma once

#include <stdint.h>
#include "sd_protocol_types.h"

// ---------------------------------------------------------------------------
// A SESSAO (em segundo plano - o Tab5 segue usavel durante o pendrive)
// ---------------------------------------------------------------------------
// Start: the card is borrowed (tab5_sd_emprestar writes the pending state,
// takes a snapshot, and redirects writes to the flash journal), then USB starts.
// The status bar shows "Card connected to computer" for the duration; tapping it
// opens Settings > USB.
//
// It ends in either of two ways: the computer ejects the disk (the card returns
// automatically because no computer-side cache remains), or the user requests
// a return in Settings > USB (the screen asks for confirmation while the disk is
// still mounted). USB stops first, then the card returns and the journal is
// applied; the result remains on screen and conflicts appear in the status bar.
//
// System rules, not app rules:
//   - do not start while an app that requires the card is open (app_t.exige_sd),
//     and do not open that app during the session (the kernel explains why);
//   - while the disk is mounted, keep the screen awake
//     (SISTEMA_ACORDADO_PENDRIVE);
//   - during USB-drive mode the cable is neither console nor flashing, and CPU
//     frequency stays at maximum while the computer is communicating;
//   - if the bus is idle for more than 2 s (unplugged without ejecting or the
//     computer sleeping), the session continues but the screen and CPU may
//     enter low-power mode and wake automatically when the computer returns;
//     with no computer connected, the CPU slows after 10 s.
typedef enum {
  PENDRIVE_PARADO = 0,
  PENDRIVE_PREPARANDO,   // borrowing the card and starting USB
  PENDRIVE_ATIVO,        // the card is connected to the computer
  PENDRIVE_DEVOLVENDO,   // stopping USB, returning the card, applying the journal
} tab5_pendrive_fase_t;

// Por que a ultima tentativa nao ligou (PENDRIVE_OK = ligou ou nunca tentou).
typedef enum {
  PENDRIVE_OK = 0,
  PENDRIVE_RECUSA_APP,         // one app que requires o cartao esta open
  PENDRIVE_RECUSA_EM_USO,      // one servico segurou o cartao alem of prazo
  PENDRIVE_RECUSA_SEM_CARTAO,  // sem cartao, ou ele nao respondeu
  PENDRIVE_RECUSA_SEM_DIARIO,  // falta a particao "reserva" (grave a tabela nova)
  PENDRIVE_RECUSA_USB,         // o USB nao subiu (o cartao ja voltou)
} tab5_pendrive_reason_t;

// Pede para enable. Nao bloqueia (a sessao tem tarefa propria). false = ja
// havia uma sessao, ou recusado in hora (o reason stays no estado).
bool tab5_pendrive_start(void);

// Pede para return o cartao on Tab5. A CONFIRMACAO e of quem chama (a screen):
// se o computador ainda estiver com o disco montado e nao ejetou, perguntar
// antes. Nao bloqueia.
void tab5_pendrive_return(void);

// Onde o pendrive esta esperando now (para dizer onde parou, when para).
typedef enum {
  PENDRIVE_ESPERA_NADA = 0,     // o computador ainda nao fez nada
  PENDRIVE_ESPERA_COMPUTADOR,   // everything respondido: e a vez of computador request
  PENDRIVE_ESPERA_USB,          // uma leitura comecou e os data/status ainda nao sairam
  PENDRIVE_ESPERA_CARTAO,       // dentro of uma leitura/escrita of cartao
} tab5_pendrive_espera_t;

// O USB por dentro, para a screen.
typedef struct {
  bool     enabled;        // o USB esta como pendrive
  bool     montado;       // o computador enumerou o disco
  bool     suspenso;      // o barramento parou: cabo puxado ou computador dormindo
  bool     ejetado;       // o computador ejetou (START STOP UNIT com "eject")
  bool     active;         // houve leitura/escrita nos ultimos 2 s
  uint64_t lidos;         // bytes lidos pelo computador since enable
  uint64_t gravados;      // bytes gravados pelo computador since enable
  uint32_t erros;         // leituras/escritas que o cartao recusou
  uint32_t ms_sem_escrita;// since a ultima escrita (UINT32_MAX = none)
  // DIAGNOSTICO - ate onde o computador chegou, in order em que ele faz:
  uint32_t n_config;      // reconheceu o aparelho (configurou o USB)
  uint32_t n_inquiry;     // perguntou o que ele e
  uint32_t n_capacidade;  // perguntou o tamanho
  uint32_t n_leituras;    // leu (a tabela FAT vem first)
  uint8_t  op_recusado;   // ultimo comando SCSI recusado (0 = nenhum)
  uint32_t leitura_max_ms;// leitura of cartao mais lenta
  uint32_t n_tur;         // perguntou se o disco esta pronto (TEST UNIT READY)
  uint32_t n_sense;       // perguntou qual foi o erro (REQUEST SENSE)
  uint32_t n_respostas;   // commands com a resposta entregue (fora leitura/escrita)
  uint8_t  op_respondido; // o ultimo deles
  uint32_t n_soltou;      // desconfigurou: reset of barramento ou cabo
  uint32_t n_leituras_ok; // commands of leitura com data e status entregues
  uint32_t n_cmd_leitura; // commands of leitura recebidos
  uint32_t ultimo_setor;  // ultimo pedaco request on cartao
  uint32_t parado_ms;     // since o ultimo sinal of vida (computador ou cartao)
  tab5_pendrive_espera_t onde;
} tab5_pendrive_usb_t;
void tab5_pendrive_usb(tab5_pendrive_usb_t* u);

typedef struct {
  tab5_pendrive_fase_t   fase;
  tab5_pendrive_reason_t reason;
  bool                   por_ejecao;   // a ultima sessao terminou pela ejecao no computador
  bool                   sem_ejetar;   // ...ou com o disco montado, sem ejecao: o computador
                                       // vai verificar o cartao in next vez (demora)
  bool                   oferta;       // computador enabled no cabo, esperando a pessoa decidir
  tab5_pendrive_usb_t    usb;
} tab5_pendrive_estado_t;
void tab5_pendrive_estado(tab5_pendrive_estado_t* e);

// Algo para a pessoa ver em Ajustes > USB (sessao em andamento, computador
// oferecido, conflito of ultima iteration) - Ajustes abre direto nessa pagina.
bool tab5_pendrive_pede_atencao(void);
// A pessoa viu o resultado: o aviso of barra exit.
void tab5_pendrive_resultado_visto(void);

// Um computador esta of outro lado of cabo AGORA? Com o pendrive disabled
// quem ve e o USB-Serial-JTAG (os quadros SOF que o computador manda a cada
// 1 ms); carregador of parede nao manda SOF - nao conta.
bool tab5_pendrive_pc_presente(void);

// Boot, depois of sistema: registra o aviso e liga a vigia of cabo (que liga
// o pendrive sozinho when o ajuste pede - settings_flash_drive()).
void tab5_pendrive_servico_start(void);
