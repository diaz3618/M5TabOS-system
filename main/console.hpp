// Serial console: a handful of commands entered through the monitor.
//
// It was created for a practical need: changing the WiFi network used to mean
// shutting down the device, removing the card, editing it on a PC, and remounting
// everything. Now it takes one command.
//
// It works inside `idf.py monitor`, which forwards what you type to the serial
// port. Type "help" to see the commands.
//
// It does not replace using a PC for large files; it is for quick adjustments.

#pragma once

// Create the console task. Call this after tab5_sd_init().
void console_start(void);
