// SYSTEM LOG ON THE CARD, ONE FILE PER DAY.
//
// -------------------------------------------------------------------------
// WHY
// -------------------------------------------------------------------------
// Until now, the ONLY way to see the log was the serial monitor connected to a
// computer. Many bugs in this system (GPS, Mesh, and Grove in general) appear
// away from the desk, near a window where the signal is better but the USB
// cable cannot reach. Without a log, each incident is unproven and the cause
// must be guessed from the symptom.
//
// -------------------------------------------------------------------------
// BY DAY, NOT BY BOOT, with the reason included
// -------------------------------------------------------------------------
// "Per boot is complicated because we boot constantly"—the system restarts
// throughout development (each flash.sh creates a new boot). One file per boot
// would create an unreadable directory of short files. By DAY, using the RTC
// date (see tab5_rtc.hpp, with its own battery that almost never discharges),
// several boots on one day share the SAME file, one after another, separated
// by an "=== boot ... ===" marker.
//
// -------------------------------------------------------------------------
// HOW CAPTURE WORKS: replaces ESP_LOG's vprintf, without duplicating calls
// -------------------------------------------------------------------------
// tab5_log_iniciar() replaces the function ESP_LOG uses for output
// (esp_log_set_vprintf) with one that does TWO things: calls the ORIGINAL
// first (the serial console remains byte-for-byte identical) and sends a copy
// of the SAME line to a RAM buffer. Every ESP_LOGI/W/E/D from ANY system task
// (interface, GPS, Mesh, camera, and so on) is captured automatically; no task
// needs to know this file exists.
//
// -------------------------------------------------------------------------
// WHY A RAM BUFFER BEFORE THE CARD
// -------------------------------------------------------------------------
// The vprintf replacement runs on THE CALLER'S STACK—it may be the interface,
// GPS, camera, or any other task. Writing to the card there (mount, open,
// write, close, unmount—the ~100 ms documented by tab5_sd.hpp) would hold up
// ANY system task for every log call. This is exactly the kind of bug that
// forced Grove probing out of the interface task (see kernel/core.cpp).
//
// Instead, the vprintf replacement only FORMATS the line and puts it in a RAM
// queue (xStreamBuffer accepts writers from multiple tasks without a manual
// lock). It takes a few microseconds and never blocks. The "card" task, the
// low-priority system writer, drains the queue periodically through
// tab5_sd_carona and ONLY THEN mounts and writes the card. If the card is
// absent, lines wait in the buffer until it returns (or are dropped when the
// buffer fills—we prefer losing old logs to locking the system forever).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Install capture and use the "card" task for flushing. Call ONCE at boot,
// after tab5_sd_init() (it uses tab5_sd_mount/open); see kernel/core.cpp.
void tab5_log_start(void);

// Flush to the card within 1 second, as an urgent notice would, without
// waiting for the 60-second batch. Use at the end of an operation whose log
// must be read immediately (for example, a pendrive operation whose console
// iteration opens the serial port and reboots Tab5). Safe from any task.
void tab5_log_descarregar_logo(void);

// State for the 'servicos' command and for displays that want to show the log
// is alive: bytes waiting in RAM, lines actually written, lines lost because
// the buffer filled, and today's filename ("" if capture has not tried to
// write even once).
void tab5_log_estado(size_t* bytes_pendentes, uint32_t* lines_gravadas,
                      uint32_t* lines_perdidas, char* arquivo, size_t arquivo_size);
