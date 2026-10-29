// SYSTEM WORKER - slow work outside of interface task.
//
// The interface task draws and responds to touch. All that may take -
// read the card, scan a file, create a large list - you have to exit
// of it, otherwise the screen freezes while the card responds (this was the case of Mesh,
// which rereads the history of SD IN THE INTERFACE with each new message).
//
// Since only one app stays in the foreground at a time, ONE system worker
// enough for everyone: the app asks, the worker executes the task in its own right
// (priority 2, below the interface and services of radio), and when
// ends the app receives NOTICE_WORK (declare it in app_t.notices) and gets the
// result in task of interface.
//
// THE RESULT PATTERN: the work writes to a buffer that the interface DOES NOT
// is reading, and only at ao_aviso does the interface switch to it. Never write to
// that the interface draws.
//
// CLOSE THE APP AND SAFE: the system waits for the work in progress to finish and
// discards those that were still in queue BEFORE calling ao_close() - then the
// app can flush your buffers there without fear. That's why one work must last
// no maximum a few hundred ms (one read, not one eternal loop) - a
// interface stays stopped while the closure waits. What takes longer and
// work LOOSE (below); permanent loop and own task (task_app).

#pragma once

#include <stdbool.h>

typedef void (*work_t)(void* arg);

// Call once at boot, by the of interface task.
void work_start(void);

// Requests one work for the current app. Does not block. false = queue full.
bool system_work(work_t fn, void* arg);

// THE APP CLOSED - stop. For work (not released) that may take a while: sweep one
// large file, for example. Close the app WAIT for the work in progress
// finishes (which lets ao_close() release buffers without fear), and the
// interface stays stuck in this wait - one loop that asks this every block
// returns the interface in hour, instead of at the end of file. Exit in the middle and
// safe: the result will no longer be used (the app has closed).
bool system_work_cancelled(void);

// FOR THE SYSTEM: discards the queue and waits for the work in progress to finish.
// Called by closing_app() before of ao_closing().
void work_stop_of_app(void);

// LOOSE WORK - for what takes SECONDS and cannot hold the closing
// of app (write a photo of 1.8 MB to the card,: close the Camera
// waited for the entire recording and the interface froze until it ended).
//
// The difference: the system DOES NOT wait for one work released on close the app, and it
// runs even if the app has already closed. The price and the CONTRACT:
// - 'arg' and of work: memory allocated just for him, which he frees himself
// at address. Nothing buffering the app (the app may have already released it).
// - the result iteration by AVISO_WORK only if the SAME app is still
// open; the work can leave a result in static of app (a
// static always exists - the app is compiled into the firmware).
bool system_work_released(work_t fn, void* arg);
