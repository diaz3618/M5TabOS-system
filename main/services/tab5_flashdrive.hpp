// USB-drive mode: the Tab5 SD card appears on the computer as a USB disk.
//
// ------------------------------------------------------------------------
// HOW TO, ON HARDWARE
// ------------------------------------------------------------------------
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
// computer sees a disk instead of the serial port. Logging summaries on the card
// as soon as it is returned (tab5_sd_enqueue_append queues incoming records).
//
// ------------------------------------------------------------------------
// THE CARD
// ------------------------------------------------------------------------
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

// -------------------------------------------------------------------------------
// THE SESSION (in the background - the Tab5 remains usable while on the pendrive)
// -------------------------------------------------------------------------------
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
// - do not start while an app that requires the card is open (app_t.exige_sd),
// and do not open that app during the session (the kernel explains why);
// - while the disk is mounted, keep the screen awake
// (SYSTEM_WAKE_PENDRIVE);
// - during USB-drive mode the cable is neither console nor flashing, and CPU
// frequency stays at maximum while the computer is communicating;
// - if the bus is idle for more than 2 s (unplugged without ejecting or the
// computer sleeping), the session continues but the screen and CPU may
// enter low-power mode and wake automatically when the computer returns;
// with no computer connected, the CPU slows down after 10 s.
typedef enum {
  PENDRIVE_PARADO = 0,
  PENDRIVE_PREPARANDO,   // borrowing the card and starting USB
  PENDRIVE_ATIVO,        // the card is connected to the computer
  PENDRIVE_DEVOLVENDO,   // stopping USB, returning the card, applying the journal
} tab5_pendrive_fase_t;

// Why didn't the last attempt turn on (PENDRIVE_OK = turned on or never tried).
typedef enum {
  PENDRIVE_OK = 0,
  PENDRIVE_RECUSA_APP,         // one app that requires the card is open
  PENDRIVE_RECUSA_EM_USO,      // one service held the card beyond the deadline
  PENDRIVE_RECUSA_SEM_CARTAO,  // no card, or he didn't respond
  PENDRIVE_RECUSA_SEM_DIARIO,  // "reserve" partition is missing (write the new table)
  PENDRIVE_RECUSA_USB,         // the USB did not come up (the card has already come back)
} tab5_pendrive_reason_t;

// Asks to enable. Does not block (the session has its own task). false = ja
// there was a session, or refused in hora (the reason stays in the state).
bool tab5_pendrive_start(void);

// Asks to return the card on Tab5. CONFIRMATION is of the caller (a screen):
// if the computer still has the disk mounted and has not ejected it, ask
// before. Does not block.
void tab5_pendrive_return(void);

// Where the pendrive is waiting now (to say where it stopped, when it stops).
typedef enum {
  PENDRIVE_ESPERA_NADA = 0,     // the computer hasn't done anything yet
  PENDRIVE_ESPERA_COMPUTADOR,   // everything answered: and the turn of computer request
  PENDRIVE_ESPERA_USB,          // a reading has started and the data/status has not yet come out
  PENDRIVE_ESPERA_CARTAO,       // within a reading/writing of card
} tab5_pendrive_espera_t;

// The USB inside, for the screen.
typedef struct {
  bool     enabled;        // the USB is like a pendrive
  bool     montado;       // the computer enumerated the disk
  bool     suspenso;      // bus stopped: cable pulled or computer asleep
  bool     ejetado;       // the computer ejected (START STOP UNIT with "eject")
  bool     active;         // there was reading/writing in the last 2 s
  uint64_t lidos;         // bytes read by computer since enable
  uint64_t gravados;      // bytes written by the computer since enable
  uint32_t erros;         // readings/writes that the card refused
  uint32_t ms_sem_escrita;// since last write (UINT32_MAX = none)
  // DIAGNOSTIC - how far the computer has reached, in order in which it does:
  uint32_t n_config;      // recognized the device (configured USB)
  uint32_t n_inquiry;     // asked what he and
  uint32_t n_capacidade;  // asked the size
  uint32_t n_leituras;    // read (FAT table comes first)
  uint8_t  op_recusado;   // last SCSI command refused (0 = none)
  uint32_t leitura_max_ms;// card reading slower
  uint32_t n_tur;         // asked if the disk is ready (TEST UNIT READY)
  uint32_t n_sense;       // asked what the error was (REQUEST SENSE)
  uint32_t n_respostas;   // commands with the response delivered (not read/write)
  uint8_t  op_respondido; // the last of them
  uint32_t n_soltou;      // unconfigured: reset of bus or cable
  uint32_t n_leituras_ok; // commands of reading with date and status delivered
  uint32_t n_cmd_leitura; // read commands received
  uint32_t ultimo_setor;  // last piece request on card
  uint32_t parado_ms;     // since the last sign of life (computer or card)
  tab5_pendrive_espera_t onde;
} tab5_pendrive_usb_t;
void tab5_pendrive_usb(tab5_pendrive_usb_t* u);

typedef struct {
  tab5_pendrive_fase_t   fase;
  tab5_pendrive_reason_t reason;
  bool                   por_ejecao;   // the last session ended by ejection on the computer
  bool                   sem_ejetar;   // ...or with the disk mounted, without ejecting: the computer
                                       // will check the card next time (it takes time)
  bool                   oferta;       // computer enabled on the cable, waiting for the person to decide
  tab5_pendrive_usb_t    usb;
} tab5_pendrive_estado_t;
void tab5_pendrive_estado(tab5_pendrive_estado_t* e);

// Something for the person to see in Settings > USB (session in progress, computer
// offered, conflict of last iteration) - Adjustments opens directly to this page.
bool tab5_pendrive_pede_atencao(void);
// The person saw the result: the warning of barra exit.
void tab5_pendrive_resultado_visto(void);

// Is a computer on the other side of the cable NOW? With the pendrive disabled
// who sees it is the USB-Serial-JTAG (the SOF frames that the computer sends every
// 1ms); Wall charger doesn't send SOF - it doesn't count.
bool tab5_pendrive_pc_presente(void);

// Boot, after system: logs the warning and turns on the cable monitor (which turns on
// the pendrive alone when the setting prompts - settings_flash_drive()).
void tab5_pendrive_servico_start(void);
