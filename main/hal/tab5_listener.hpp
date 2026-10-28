// SINGLE EVENT LISTENER, with removal that WAITS for an in-flight call.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// Three modules deliver events to ONE registered listener: BLE scanning
// (tab5_ble), Wi-Fi sniffing, and CSI. All three used to keep only a volatile
// pointer: the caller copied and invoked it, while removal wrote nullptr. The
// copy avoids a null pointer, but DOES NOT say when an already-started call has
// finished. Code that removed a listener and then destroyed its data (Radar's
// queue) could not know when that was safe. Radar "solved" this by waiting
// 40 ms while closing: a guess on the UI task, and a use-after-free as soon as
// a listener took longer than the guess.
//
// Here removal (setting nullptr) RETURNS only after no call is in flight. Once
// it returns, the remover may safely destroy anything it wants.
//
// -------------------------------------------------------------------------
// HOW, WITHOUT A LOCK
// -------------------------------------------------------------------------
// A caller counts itself as in flight BEFORE reading the pointer; removal
// changes the pointer BEFORE reading the count. With sequentially consistent
// atomics there are only two cases: the call read the old pointer and was
// already counted when removal checked, so removal waits; or it read null and
// does not call. Listeners only enqueue and return, so in practice removal
// rarely makes even one trip around the loop.
//
// DO NOT remove a listener from inside itself: removal would wait forever for
// its own call.

#pragma once

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

template <typename F>
struct tab5_listener_t {
  std::atomic<F>   fn{nullptr};
  std::atomic<int> in_flight{0};
};

// Event side (any task).
template <typename F, typename... A>
inline void tab5_listener_call(tab5_listener_t<F>* o, A... args)
{
  o->in_flight.fetch_add(1);
  const F f = o->fn.load();
  if (f) { f(args...); }
  o->in_flight.fetch_sub(1);
}

// Replaces the listener (nullptr removes it) and returns only after any
// in-flight call has finished.
template <typename F>
inline void tab5_listener_set(tab5_listener_t<F>* o, F f)
{
  o->fn.store(f);
  while (o->in_flight.load() != 0) { vTaskDelay(1); }
}
