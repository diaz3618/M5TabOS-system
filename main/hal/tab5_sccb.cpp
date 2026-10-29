// THE CAMERA SCCB BY THE SAME LOCK AS THE REST OF THE INTERNAL I2C.
//
// THE PROBLEM. The internal I2C bus (I2C_NUM_1: touch GT911, INA226, IMU,
// RTC, expanders, camera sensor) has TWO drivers on top of the same
// controller:
// - the of M5GFX/M5Unified, which directly affects the registers and protects each
// transaction with one mutex per port (common.cpp, i2c_context_t::lock);
// - the of IDF (i2c_master), used by the SCCB of sensor of camera via esp_video.
// M5GFX itself warns that the two sides "are not serialized to each other - the
// application should not run them at the same time". And esp_video runs: the control
// automatic of gain/exposure (IPA: AGC, AEN) writes to the sensor DURING
// captures, in his task, while the interface reads the touch. The "bridge of
// camera" (kernel/core.cpp) only protected init/stop.
//
// THE SOLUTION. esp_video creates the channel of sensor by calling sccb_new_i2c_io()
// (esp_video_init.c:255). The link diverts this call to ca
// (-Wl,--wrap=sccb_new_i2c_io, main/CMakeLists.txt) and we deliver one channel
// SCCB whose operations are transactions of M5GFX - which go through the lock of
// door. From then on, ALL internal I2C is serialized by ONE latch only, of
// any task, and no third-party components were edited.
//
// Every SCCB operation reduces to three forms (see sccb_i2c.c of component):
// write, write-and-read, read. The bytes of register/value are already assembled
// in buffer by who calls; Here you can only transport.

#include <stdlib.h>
#include <M5Unified.h>
#include "driver/i2c_master.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_io_interface.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "sccb";

static const int INTERNAL_I2C_PORT = 1;   // I2C_NUM_1 - see the warning of numbering in tab5-sistema-apps

typedef struct {
  esp_sccb_io_t base;
  uint16_t      address;
  uint32_t      freq;
} sccb_lgfx_t;

static inline sccb_lgfx_t* of(esp_sccb_io_t* io) { return __containerof(io, sccb_lgfx_t, base); }

static esp_err_t write(esp_sccb_io_t* io, const uint8_t* w, size_t n, int)
{
  if (n > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = of(io);
  return lgfx::i2c::transactionWrite(INTERNAL_I2C_PORT, s->address, w, (uint8_t)n, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_read(esp_sccb_io_t* io, const uint8_t* w, size_t wn, uint8_t* r, size_t rn, int)
{
  if (wn > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = of(io);
  return lgfx::i2c::transactionWriteRead(INTERNAL_I2C_PORT, s->address, w, (uint8_t)wn, r, rn, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t read(esp_sccb_io_t* io, uint8_t* r, size_t rn, int)
{
  if (rn > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = of(io);
  return lgfx::i2c::transactionRead(INTERNAL_I2C_PORT, s->address, r, (uint8_t)rn, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t erase(esp_sccb_io_t* io)
{
  heap_caps_free(of(io));
  return ESP_OK;
}

extern "C" esp_err_t __real_sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t* cfg,
                                            esp_sccb_io_handle_t* io);

extern "C" esp_err_t __wrap_sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t* cfg,
                                            esp_sccb_io_handle_t* io)
{
  if (!bus || !cfg || !io) { return ESP_ERR_INVALID_ARG; }

  // Only the INTERNAL bus is shared with the M5GFX. Anyone else follows
  // the normal path of component.
  i2c_master_bus_handle_t internal_bus = nullptr;
  if (i2c_master_get_bus_handle((i2c_port_num_t)INTERNAL_I2C_PORT, &internal_bus) != ESP_OK || bus != internal_bus
   || cfg->dev_addr_length != I2C_ADDR_BIT_LEN_7) {
    return __real_sccb_new_i2c_io(bus, cfg, io);
  }

  auto* s = (sccb_lgfx_t*)heap_caps_calloc(1, sizeof(sccb_lgfx_t), MALLOC_CAP_DEFAULT);
  if (!s) { return ESP_ERR_NO_MEM; }
  s->address = cfg->device_address;
  s->freq     = cfg->scl_speed_hz ? cfg->scl_speed_hz : 400000;

  s->base.transmit_reg_a8v8   = write;
  s->base.transmit_reg_a16v8  = write;
  s->base.transmit_reg_a8v16  = write;
  s->base.transmit_reg_a16v16 = write;
  s->base.transmit_reg_a16v32 = write;
  s->base.transmit_reg_a16    = write;
  s->base.transmit_receive_reg_a8v8   = write_read;
  s->base.transmit_receive_reg_a16v8  = write_read;
  s->base.transmit_receive_reg_a8v16  = write_read;
  s->base.transmit_receive_reg_a16v16 = write_read;
  s->base.transmit_receive_reg_a16v32 = write_read;
  s->base.transmit_receive_reg_a16    = write_read;
  s->base.transmit_v16 = write;
  s->base.receive_v16  = read;
  s->base.del          = erase;

  *io = &s->base;
  ESP_LOGI(TAG, "sensor 0x%02X no I2C interno pela trava do M5GFX (%lu Hz)",
           (unsigned)s->address, (unsigned long)s->freq);
  return ESP_OK;
}
