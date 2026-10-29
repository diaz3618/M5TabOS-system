#include "tab5_gps.hpp"
#include "tab5_mutex.hpp"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // cooperative stop of task (kernel)
#include "tab5_grove.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tab5_time.hpp"   // tab5_now_ms
#include <string.h>

static const char* TAG = "gps";

// ---- wiring of GPS in Grove (Port A of Tab5) ----------------------------
// MOVIDA of app_azimutal.cpp - the module is the same, so who calls
// a UART changed from "o app, on open" to "o service, when Grove has one
// GPS of truth".
static const uart_port_t GPS_UART   = UART_NUM_1;
static const int         GPS_RX_PIN = 54;   // GPIO54 = SCL of Port A, receives the TX of GPS
static const int         GPS_TX_PIN = 53;   // GPIO53 = SDA of Port A
static const int         GPS_BAUD   = 9600;

static bool     s_service_active = false;
static stop_t s_stop_state = { };   // request of stop and confirmation of output of task

static nmea_position_t s_pos   = { };
static nmea_time_t   s_time = { };
static uint32_t       s_ultima_sentenca_ms = 0;
static uint32_t       s_started_ms     = 0;   // see warning in tab5_gps_silence_ms()

static QueueHandle_t s_assinante = nullptr;   // who wants a raw sentence (Azimutal)

// LOCKS FOR s_pos/s_tempo - found on review "ready for
// more than one consumer at the same time": tab5_gps_position()/tab5_gps_time()
// are already documented (see the .hpp) as "any app can ask where
// I am", but they returned the entire struct (two double, two float, flags -
// does not fit in a single instruction) without any protection against the task of GPS
// write no storage of copy. With ONE reader at a time this almost never happens.
// note (position stays wrong for one frame only); the declared intention and
// several apps reading at the same time, and then the chance to get one value
// "torn" (new latitude with old longitude) rises. Lazy-create and never
// destroyed, SAME TEMPLATE of services/notification.cpp - first use and
// always before any dispute of truth (boot calls tab5_gps_start
// before any app can read), so create here is safe.
static SemaphoreHandle_t s_trava = nullptr;


static inline void lock(void)
{
  tab5_mutex_lock(&s_trava);   // created without race in first time
}
static inline void destravar(void) { if (s_trava) { xSemaphoreGive(s_trava); } }

nmea_position_t tab5_gps_position(void)
{
  lock();
  nmea_position_t copia = s_pos;
  destravar();
  return copia;
}

nmea_time_t tab5_gps_time(void)
{
  lock();
  nmea_time_t copia = s_time;
  destravar();
  return copia;
}

bool tab5_gps_service_active(void) { return s_service_active; }

bool tab5_gps_on_air(void)
{
  return s_ultima_sentenca_ms != 0 && (tab5_now_ms() - s_ultima_sentenca_ms) < 15000;
}

uint32_t tab5_gps_silence_ms(void)
{
  if (s_ultima_sentenca_ms == 0) {
    // NO rulings YET - but that could be because the service is over
    // start now, not because the module disappeared. Bug seen in hardware
    // return_card UINT32_MAX here unconditionally makes the lookout
    // of Grove (kernel/core.cpp, "GPS mute ha Xs") fire on FIRST tick
    // (up to 2s) after ANY restart - before the module has even
    // had the CHANCE to speak within the period of 5s that the code thought
    // was giving. Result: service goes up, and dropped 2s later by
    // "mute", rises to new, and drops to new - one loop without end, never of
    // real time for the GPS to respond.
    //
    // The correct account is from WHEN THE SERVICE RISES, not one value
    // fixed sentry - so the caller (the lookout) sees the silence growing
    // of truth and only distrust after the ENTIRE period has passed, just like
    // It already happens when he speaks and then disappears.
    return s_service_active ? (tab5_now_ms() - s_started_ms) : UINT32_MAX;
  }
  return tab5_now_ms() - s_ultima_sentenca_ms;
}

bool tab5_gps_subscribe(QueueHandle_t queue)
{
  if (s_assinante != nullptr) { return false; }   // one subscriber only, see the header
  s_assinante = queue;
  return true;
}

void tab5_gps_unsubscribe(void) { s_assinante = nullptr; }

// -------------------------------------------------------------------------------
// THE TASK - read bytes of UART, assemble sentences, parse RMC/GGA and forward the
// crude sentence for who signed. And the SAME state machine for
// character that app_azimutal used (laco_gps + the dispatch of laco_parser),
// so now it's one loop so: there is no longer a queue between "read" and "parsear"
// because it is the SAME task that does both.
// THE TASK SLEEP IN THE UART EVENT QUEUE, not in a short period of time. Was
// uart_read_bytes with 50 ms: 20 wakeups per second for one module that
// sends one burst per second at 9600 baud. The driver warns (UART_DATA) when
// the FIFO fills up or the line stays quiet - few notices per burst - and the task
// read everything that has already arrived at once. For stop, tab5_gps_stop nudges the
// queue (gps_poke) and the task sees the request in time.
static QueueHandle_t s_uart_ev = nullptr;   // created by UART driver

static void gps_cutucar(void)
{
  if (!s_uart_ev) { return; }
  uart_event_t e = { };
  e.type = UART_EVENT_MAX;   // is not an event of driver: it only wakes up the task
  xQueueSend(s_uart_ev, &e, 0);
}

static void vTaskGPS_body(void*)
{
  char buffer[NMEA_MAX_LENGTH];
  int  idx = 0;
  bool gravando = false;

  static uint8_t input[256];   // block reading - see the comment that
                                  // existed in laco_gps about the cost per byte

  while (!stop_requested(&s_stop_state)) {
    uart_event_t ev;
    if (xQueueReceive(s_uart_ev, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) { continue; }
    if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
      // Nobody read the time: what is in the buffer is a cut sentence.
      uart_flush_input(GPS_UART);
      xQueueReset(s_uart_ev);
      gravando = false; idx = 0;
      continue;
    }
    if (ev.type != UART_DATA) { continue; }   // the nudge of stop, or other warning
    int lidos;
    while ((lidos = uart_read_bytes(GPS_UART, input, sizeof(input), 0)) > 0) {   // everything that has already arrived
      for (int k = 0; k < lidos; ++k) {
        const char c = (char)input[k];

        if (c == '$') {
          gravando = true;
          idx = 0;
          buffer[idx++] = c;
          continue;
        }
        if (!gravando) { continue; }
        if (idx >= NMEA_MAX_LENGTH - 1) { gravando = false; idx = 0; continue; }

        buffer[idx++] = c;

        if (c == '\n') {
          buffer[idx] = '\0';
          gravando = false;
          idx = 0;

          if (!nmea_checksum_ok(buffer)) {
            ESP_LOGD(TAG, "checksum invalido: %s", buffer);
            continue;
          }

          s_ultima_sentenca_ms = tab5_now_ms();

          // The first 2 characters after '$' change by constellation
          // (GP=GPS, GL=GLONASS...); That's why we compare from 3rd.
          if (strncmp(buffer + 3, "RMC", 3) == 0) {
            nmea_time_t t;
            if (nmea_parse_rmc(buffer, &t)) { lock(); s_time = t; destravar(); }
          } else if (strncmp(buffer + 3, "GGA", 3) == 0) {
            nmea_position_t p;
            if (nmea_parse_gga(buffer, &p)) { lock(); s_pos = p; destravar(); }
          }

          // ALL valid sentences go to whoever signed, including RMC/GGA who
          // we just finished processing - Azimutal filters GSV alone, as always
          // did; It's not worth this service knowing what each subscriber wants.
          if (s_assinante) {
            NMEARawSentence_t crua;
            strncpy(crua.sentence, buffer, NMEA_MAX_LENGTH - 1);
            crua.sentence[NMEA_MAX_LENGTH - 1] = '\0';
            xQueueSend(s_assinante, &crua, 0);   // don't wait: miss 1 sentence
                                                  // is better than lock reading
          }
        }
      }
    }
  }

  stop_confirm(&s_stop_state);
  // Returns: the vacancy of pool returns the owner and iteration to sleep (kernel/task_pool.h).
}

bool tab5_gps_stop(void)
{
  if (!s_service_active) { return true; }

  ESP_LOGI(TAG, "parando o service...");
  // The task reads the UART in slices of 50 ms: the output costs this in the worst case.
  if (!stop_request(&s_stop_state, 1000, gps_cutucar)) {
    ESP_LOGE(TAG, "task did not confirm within 1 s - stopping anyway; nothing was torn down");
    return false;
  }

  uart_driver_delete(GPS_UART);
  s_uart_ev = nullptr;   // the queue of events goes with the driver
  tab5_grove_release(GROVE_OWNER_GPS);

  s_ultima_sentenca_ms = 0;
  lock();
  memset(&s_pos,   0, sizeof(s_pos));
  memset(&s_time, 0, sizeof(s_time));
  destravar();

  s_service_active = false;
  ESP_LOGI(TAG, "service parado, Grove free_space");
  return true;
}

bool tab5_gps_start(void)
{
  if (s_service_active) {
    if (stop_em_progress(&s_stop_state)) {
      ESP_LOGW(TAG, "previous service is still stopping - will not start another one");
    } else {
      ESP_LOGI(TAG, "service is already running - nothing to do");
    }
    return true;
  }

  if (!tab5_grove_acquire(GROVE_OWNER_GPS)) {
    ESP_LOGE(TAG, "Grove is busy - service will not start");
    return false;
  }

  uart_config_t cfg = { };
  cfg.baud_rate  = GPS_BAUD;
  cfg.data_bits  = UART_DATA_8_BITS;
  cfg.parity     = UART_PARITY_DISABLE;
  cfg.stop_bits  = UART_STOP_BITS_1;
  cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  // DO NOT touch M5.Ex_I2C here - see the same warning that existed in
  // app_azimutal.cpp: the keyboard of ExtPort1 and who uses this bus today,
  // and releasing/restarting Ex_I2C would crash it.
  if (uart_driver_install(GPS_UART, 2048, 0, 16, &s_uart_ev, 0) != ESP_OK
   || uart_param_config(GPS_UART, &cfg) != ESP_OK
   || uart_set_pin(GPS_UART, GPS_TX_PIN, GPS_RX_PIN,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    ESP_LOGE(TAG, "could not open the Grove UART");
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // the queue of events goes with the driver
    tab5_grove_release(GROVE_OWNER_GPS);
    return false;
  }

  lock();
  memset(&s_pos,   0, sizeof(s_pos));
  memset(&s_time, 0, sizeof(s_time));
  destravar();
  s_ultima_sentenca_ms = 0;

  if (!stop_prepare(&s_stop_state)) {
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // the queue of events goes with the driver
    tab5_grove_release(GROVE_OWNER_GPS);
    ESP_LOGE(TAG, "out of memory for the GPS task");
    return false;
  }
  s_started_ms = tab5_now_ms();   // see warning in tab5_gps_silence_ms()
  s_service_active  = true;

  // Priority 3, same as service Mesh: below interface (4), above
  // register (1) - losing byte of serial is irreversible.
  if (!pool_create(nullptr, vTaskGPS_body, nullptr, "gps", 4096, 3, OWNER_SERVICE, "GPS")) {
    stop_abandon(&s_stop_state);
    s_service_active = false;
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // the queue of events goes with the driver
    tab5_grove_release(GROVE_OWNER_GPS);
    ESP_LOGE(TAG, "sem memoria para a tarefa do GPS");
    return false;
  }
  ESP_LOGI(TAG, "service no ar (Grove %d baud, RX=%d TX=%d)",
           GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);
  return true;
}
