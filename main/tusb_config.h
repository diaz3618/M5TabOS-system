// TinyUSB configuration (espressif/tinyusb component) for flash drive mode
// - services/tab5_flashdrive.cpp. The main/CMakeLists.txt passes this folder to
// the TinyUSB library, which looks for "tusb_config.h" by name.
//
// DEVICE ONLY, MSC ONLY, FULL SPEED PORT ONLY. The Tab5's USB-C is enabled
// on the P4's full-speed PHY 0 - the same one used by USB-Serial-JTAG (the console).
// TinyUSB port 0 is the full-speed OTG controller (dwc2_esp32.h: "Port0 to
// OTG_FS, Port1 to OTG_HS"); the high-speed controller has its own PHY, on different
// pins, and is not connected to the connector.
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

// CONTROLLER DMA, as Espressif's esp_tinyusb uses by default (the most
// tested mode on the P4). Previously, it was in slave mode (the CPU copying
// from the FIFO, one 64-byte packet per interrupt): 64 interrupts per 4 KB
// block, and the less commonly used library path on this chip. Slave mode
// remains enabled as a fallback: TinyUSB only uses DMA if the controller
// supports it (GHWCFG2).
#define CFG_TUD_DWC2_SLAVE_ENABLE   1
#define CFG_TUD_DWC2_DMA_ENABLE     1

// With DMA, the controller reads and writes RAM outside the cache: TinyUSB's
// buffers reside in internal RAM (DRAM_ATTR - the library is not included in
// linker.lf) and are aligned to the P4's cache line (64 bytes), and the library
// synchronizes the cache before and after each transfer (on the P4, this is
// already enabled along with DMA: CFG_TUD_MEM_DCACHE_ENABLE, 64-byte line).
// The MSC buffer also goes directly to SDMMC DMA - if misaligned, the SD card
// driver would fall back to an intermediate buffer, one sector at a time.
#define CFG_TUSB_MEM_SECTION        __attribute__((aligned(64))) DRAM_ATTR
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(64)))

#define CFG_TUD_ENDPOINT0_SIZE      64

// MSC only.
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

// How many sectors the MSC requests from the card at once: 8 sectors. Multiple of 512 on
// purpose - each read/write call falls on whole sectors.
#define CFG_TUD_MSC_EP_BUFSIZE      4096

#ifdef __cplusplus
}
#endif
