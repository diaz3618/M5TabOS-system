// O SCCB DA CAMERA PELA MESMA TRAVA DO RESTO DO I2C INTERNO.
//
// O PROBLEMA. O barramento I2C interno (I2C_NUM_1: toque GT911, INA226, IMU,
// RTC, expansores, sensor da camera) tem DOIS drivers em cima do mesmo
// controlador:
//   - o do M5GFX/M5Unified, que mexe direto nos registradores e protege cada
//     transacao com um mutex por porta (common.cpp, i2c_context_t::lock);
//   - o do IDF (i2c_master), usado pelo SCCB do sensor da camera via esp_video.
// O proprio M5GFX avisa que os dois lados "nao sao serializados entre si - a
// aplicacao nao deve roda-los ao mesmo tempo". E o esp_video roda: o controle
// automatico de ganho/exposicao (IPA: AGC, AEN) escreve no sensor DURANTE a
// captura, na tarefa dele, enquanto a interface le o toque. A "ponte da
// camera" (kernel/core.cpp) so protegia o iniciar/parar.
//
// A SOLUCAO. O esp_video cria o canal do sensor chamando sccb_new_i2c_io()
// (esp_video_init.c:255). O link desvia essa chamada para ca
// (-Wl,--wrap=sccb_new_i2c_io, main/CMakeLists.txt) e entregamos um canal
// SCCB cujas operacoes sao transacoes do M5GFX - que passam pela trava da
// porta. A partir dai TODO o I2C interno e serializado por UMA trava so, de
// qualquer tarefa, e nenhum componente de terceiros foi editado.
//
// Toda operacao SCCB se reduz a tres formas (ver sccb_i2c.c do componente):
// escrever, escrever-e-ler, ler. Os bytes de registrador/valor ja vem montados
// no buffer por quem chama; aqui so se transporta.

#include <stdlib.h>
#include <M5Unified.h>
#include "driver/i2c_master.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_io_interface.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "sccb";

static const int PORTA_INTERNA = 1;   // I2C_NUM_1 - ver o aviso da numeracao em tab5-sistema-apps

typedef struct {
  esp_sccb_io_t base;
  uint16_t      endereco;
  uint32_t      freq;
} sccb_lgfx_t;

static inline sccb_lgfx_t* de(esp_sccb_io_t* io) { return __containerof(io, sccb_lgfx_t, base); }

static esp_err_t escrever(esp_sccb_io_t* io, const uint8_t* w, size_t n, int)
{
  if (n > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = de(io);
  return lgfx::i2c::transactionWrite(PORTA_INTERNA, s->endereco, w, (uint8_t)n, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t escrever_ler(esp_sccb_io_t* io, const uint8_t* w, size_t wn, uint8_t* r, size_t rn, int)
{
  if (wn > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = de(io);
  return lgfx::i2c::transactionWriteRead(PORTA_INTERNA, s->endereco, w, (uint8_t)wn, r, rn, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t ler(esp_sccb_io_t* io, uint8_t* r, size_t rn, int)
{
  if (rn > 255) { return ESP_ERR_INVALID_SIZE; }
  auto* s = de(io);
  return lgfx::i2c::transactionRead(PORTA_INTERNA, s->endereco, r, (uint8_t)rn, s->freq).has_value()
         ? ESP_OK : ESP_FAIL;
}

static esp_err_t apagar(esp_sccb_io_t* io)
{
  heap_caps_free(de(io));
  return ESP_OK;
}

extern "C" esp_err_t __real_sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t* cfg,
                                            esp_sccb_io_handle_t* io);

extern "C" esp_err_t __wrap_sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t* cfg,
                                            esp_sccb_io_handle_t* io)
{
  if (!bus || !cfg || !io) { return ESP_ERR_INVALID_ARG; }

  // So o barramento INTERNO e shared com o M5GFX. Qualquer outro segue
  // o caminho normal do componente.
  i2c_master_bus_handle_t interno = nullptr;
  if (i2c_master_get_bus_handle((i2c_port_num_t)PORTA_INTERNA, &interno) != ESP_OK || bus != interno
   || cfg->dev_addr_length != I2C_ADDR_BIT_LEN_7) {
    return __real_sccb_new_i2c_io(bus, cfg, io);
  }

  auto* s = (sccb_lgfx_t*)heap_caps_calloc(1, sizeof(sccb_lgfx_t), MALLOC_CAP_DEFAULT);
  if (!s) { return ESP_ERR_NO_MEM; }
  s->endereco = cfg->device_address;
  s->freq     = cfg->scl_speed_hz ? cfg->scl_speed_hz : 400000;

  s->base.transmit_reg_a8v8   = escrever;
  s->base.transmit_reg_a16v8  = escrever;
  s->base.transmit_reg_a8v16  = escrever;
  s->base.transmit_reg_a16v16 = escrever;
  s->base.transmit_reg_a16v32 = escrever;
  s->base.transmit_reg_a16    = escrever;
  s->base.transmit_receive_reg_a8v8   = escrever_ler;
  s->base.transmit_receive_reg_a16v8  = escrever_ler;
  s->base.transmit_receive_reg_a8v16  = escrever_ler;
  s->base.transmit_receive_reg_a16v16 = escrever_ler;
  s->base.transmit_receive_reg_a16v32 = escrever_ler;
  s->base.transmit_receive_reg_a16    = escrever_ler;
  s->base.transmit_v16 = escrever;
  s->base.receive_v16  = ler;
  s->base.del          = apagar;

  *io = &s->base;
  ESP_LOGI(TAG, "sensor 0x%02X no I2C interno pela trava do M5GFX (%lu Hz)",
           (unsigned)s->endereco, (unsigned long)s->freq);
  return ESP_OK;
}
