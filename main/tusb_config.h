// Configuracao do TinyUSB (componente espressif/tinyusb) para o modo pendrive
// - services/tab5_flashdrive.cpp. O main/CMakeLists.txt passa esta pasta para
// a biblioteca do TinyUSB, que procura "tusb_config.h" pelo nome.
//
// SO DISPOSITIVO, SO MSC, SO A PORTA FULL SPEED. O USB-C do Tab5 esta ligado
// no PHY full speed 0 do P4 - o mesmo do USB-Serial-JTAG (o console). A porta
// 0 do TinyUSB e o controlador OTG full speed (dwc2_esp32.h: "Port0 to
// OTG_FS, Port1 to OTG_HS"); o de alta velocidade tem PHY proprio, em outros
// pinos, e nao chega no conector.
#pragma once

#include "sdkconfig.h"
#include "esp_attr.h"   // DRAM_ATTR

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG              0

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// DMA DO CONTROLADOR, como o esp_tinyusb da Espressif usa por padrao (o modo
// mais testado no P4). Era o modo escravo (a CPU copiando da FIFO, um pacote
// de 64 bytes por interrupcao): 64 interrupcoes por bloco de 4 KB, e o
// caminho menos usado da biblioteca neste chip. O escravo fica ligado como
// reserva: o TinyUSB so usa o DMA se o controlador tiver (GHWCFG2).
#define CFG_TUD_DWC2_SLAVE_ENABLE   1
#define CFG_TUD_DWC2_DMA_ENABLE     1

// Com DMA o controlador le e escreve a RAM por fora do cache: os buffers do
// TinyUSB ficam na RAM interna (DRAM_ATTR - a biblioteca nao entra no
// linker.lf) e alinhados a linha de cache do P4 (64 bytes), e a biblioteca
// sincroniza o cache antes e depois de cada transferencia (no P4 isso ja vem
// ligado junto com o DMA: CFG_TUD_MEM_DCACHE_ENABLE, linha de 64). O buffer
// do MSC tambem vai direto para o DMA do SDMMC - desalinhado, o driver do
// cartao cairia num buffer intermediario, um setor por vez.
#define CFG_TUSB_MEM_SECTION        __attribute__((aligned(64))) DRAM_ATTR
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(64)))

#define CFG_TUD_ENDPOINT0_SIZE      64

// So o MSC.
#define CFG_TUD_MSC                 1
#define CFG_TUD_CDC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VIDEO               0
#define CFG_TUD_BTH                 0
#define CFG_TUD_DFU                 0
#define CFG_TUD_DFU_RUNTIME         0
#define CFG_TUD_ECM_RNDIS           0
#define CFG_TUD_NCM                 0
#define CFG_TUD_USBTMC              0

// Quanto o MSC pede ao cartao de uma vez: 8 setores. Multiplo de 512 de
// proposito - cada chamada de leitura/escrita cai em setores inteiros.
#define CFG_TUD_MSC_EP_BUFSIZE      4096

#ifdef __cplusplus
}
#endif
