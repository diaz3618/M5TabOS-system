#include "console.hpp"
#include "tab5_time.hpp"   // tab5_now_ms

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <strings.h>   // strcasecmp
#include <ctype.h>     // tolower
#include <dirent.h>
#include <sys/stat.h>

#include <M5Unified.h>
#include "driver/i2c_types.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "tab5_frame.hpp"
#include "tab5_bmp.hpp"
#include "tab5_camera.hpp"
#include "tab5_camera_ipa.h"
#include "hal/efuse_hal.h"
#include "esp_heap_caps.h"   // heap_caps_malloc - buffer temporario of "mesh msgs"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  #include "driver/usb_serial_jtag.h"
  #include "driver/usb_serial_jtag_vfs.h"
#else
  #include "driver/uart.h"
  #include "driver/uart_vfs.h"
#endif

#include "app.h"
#include "esp_wifi.h"
#include "tab5_espnow.hpp"      // sistema_diag_toque
#include "tab5_flashdrive.hpp"
#include "tab5_sniffer.hpp"
#include "tab5_csi.hpp"
#include "tab5_sd.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_wifi.hpp"
#include "tab5_grove.hpp"
#include "tab5_mesh.hpp"
#include "tab5_gps.hpp"
#include "tab5_imu.hpp"
#include "eh_host_sys.h"
#include "esp_hosted.h"
#include "tab5_c6.h"
#include "eh_host_feat_peer_data.h"
#include "eh_host_cp_ota.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "mbedtls/md5.h"
#include "ff.h"
#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "energy_log.h"
#include "tab5_log.hpp"
#include "settings.h"
#include "tab5_power.hpp"
#include "tab5_ble.hpp"
#include "tab5_mesh_ble.hpp"
#include "virtual_screen.hpp"
#include "compositor.hpp"
#include "tab5_glyphs.hpp"
#include "internal.hpp"   // sistema_trocar_relogio_painel
#include "esp_pm.h"
#include "tab5_bus.hpp"
#include "tab5_screen.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_extrato



// ---------------------------------------------------------------------------
// 'screen measure' / 'screen' - THE COST OF PAGES IN A WINDOW
// ---------------------------------------------------------------------------
// The composer only accumulates (comp_estat); the window and from here: 'screen measure' guard
// the photo of now, and 'screen' shows the difference until the moment of reading. THE
// monitor has his photo - one does not erase the window of another.
static comp_estat_t s_medir_base = {};
static int64_t      s_medir_since = 0;

static void screen_measure_reset(void)
{
  comp_estat(&s_medir_base);
  s_medir_since = esp_timer_get_time();
  printf("measurement reset - use the device (scroll, open apps), then run 'screen'\n");
}

static void screen_measure_show(void)
{
  if (s_medir_since == 0) { printf("\n  (run 'screen measure' to start a measurement window)\n"); return; }
  comp_estat_t a;
  comp_estat(&a);
  const comp_estat_t& b = s_medir_base;
  const double seg = (double)(esp_timer_get_time() - s_medir_since) / 1e6;
  const uint32_t q = a.frames - b.frames;
  // Each pixel copied by DMA is read (2 B) and written (2 B) in PSRAM. Put
  // isso x4. "drawn" e o que a CPU pintou direto in pagina (so escrita).
  const double mb_d  = (double)(a.px_frame    - b.px_frame)    * 2.0 / 1e6;
  const double mb_s  = (double)(a.px_sinc      - b.px_sinc)      * 4.0 / 1e6;
  const double mb_rd = (double)(a.px_rolar_dma - b.px_rolar_dma) * 4.0 / 1e6;
  auto ms_por = [&](uint64_t us) { return q ? (double)us / 1000.0 / q : 0.0; };
  printf("\n--- frames (last %.1f s) ---\n", seg);
  printf("  frames                    : %lu (%.1f fps)  full(>=90%%): %lu  queue overflows: %lu\n",
         (unsigned long)q, seg > 0 ? q / seg : 0.0,
         (unsigned long)(a.cheios - b.cheios), (unsigned long)(a.estouros - b.estouros));
  printf("  drawn directly on the page: %.1f MB\n", mb_d);
  printf("  DMA2D copies              : sync %.1f MB + scrolling %.1f MB = %.1f MB/s\n",
         mb_s, mb_rd, seg > 0 ? (mb_s + mb_rd) / seg : 0.0);
  printf("  scrolling (DMA or CPU)    : %.2f ms/frame\n", ms_por(a.us_rolar - b.us_rolar));
  printf("  per frame (average)       : drawing %.2f ms, copy %.2f ms, swap wait %.2f ms\n",
         ms_por(a.us_drawing - b.us_drawing), ms_por(a.us_copia - b.us_copia),
         ms_por(a.us_espera - b.us_espera));
  {
    // O drawing separado: pass COMPLETA (open app, home) e o resto
    // (rolagem, one valor changed). Problemas diferentes, remedios diferentes.
    const uint32_t nc = a.n_drawing_complete - b.n_drawing_complete;
    const uint32_t nt = a.n_drawing - b.n_drawing;
    const uint64_t uc = a.us_drawing_complete - b.us_drawing_complete;
    const uint64_t ut = a.us_drawing - b.us_drawing;
    const uint32_t np = nt - nc;
    printf("  desenho por passada       : completa %.2f ms (%lu)  parcial %.2f ms (%lu)\n",
           nc ? (double)uc / 1000.0 / nc : 0.0, (unsigned long)nc,
           np ? (double)(ut - uc) / 1000.0 / np : 0.0, (unsigned long)np);
  }
  {
    system_pass_t ps[24];
    const int n = system_passes_complete(ps, 24);
    if (n > 0) {
      printf("  complete pass per app (average ms since boot):\n");
      printf("    %-12s %5s %8s %8s %8s %8s %8s\n", "app", "times", "total", "worst", "bar", "clear", "content");
      for (int i = 0; i < n; ++i) {
        const system_pass_t& p = ps[i];
        if (!p.n) { continue; }
        auto m = [&](uint64_t us) { return (double)us / 1000.0 / p.n; };
        printf("    %-12.12s %5lu %8.1f %8.1f %8.1f %8.1f %8.1f\n", p.name ? p.name : "?",
               (unsigned long)p.n, m(p.us), (double)p.max_us / 1000.0,
               m(p.us_barra), m(p.us_limpar), m(p.us_content));
      }
    }
  }
  {
    glifos_estat_t g;
    tab5_glifos_estat(&g);
    const uint32_t tot = g.acertos + g.faltas + g.recusas;
    printf("  glyph cache (since boot): %lu%% cached; %lu hits, %lu misses, %lu used the original path\n",
           (unsigned long)(tot ? (uint64_t)g.acertos * 100 / tot : 0),
           (unsigned long)g.acertos, (unsigned long)g.faltas, (unsigned long)g.recusas);
    printf("                              %lu glyphs stored, %lu KB, cleared %lu time(s)\n",
           (unsigned long)g.entries, (unsigned long)(g.bytes / 1024), (unsigned long)g.limpezas);
    printf("                              original path by reason: space %lu, no background %lu, overflow %lu,\n"
           "                              destination %lu, scale %lu, other %lu\n",
           (unsigned long)g.rec_espaco, (unsigned long)g.rec_sem_fundo, (unsigned long)g.rec_invade,
           (unsigned long)g.rec_destino, (unsigned long)g.rec_escala, (unsigned long)g.rec_outro);
  }
  static const char* const RANGE_LABELS[COMP_FAIXAS] = { "1", "2", "3", "4+" };
  printf("  frame iteration (average) : %.2f ms   panel periods:", ms_por(a.us_iteration - b.us_iteration));
  for (int f = 0; f < COMP_FAIXAS; ++f) {
    printf(" %s:%lu", RANGE_LABELS[f], (unsigned long)(a.voltas[f] - b.voltas[f]));
  }
  printf("\n");
}

static const char *TAG = "console";

// Yield the CPU during long listings. The console has priority above IDLE, so
// printing a large list without blocking would hold a core. With about 200 Mesh
// nodes arriving over MQTT, 'mesh' held core 0 for more than five seconds and
// the watchdog complained about IDLE0. One tick every eight items is not visible
// on screen and lets IDLE (and anything else waiting) run.
static void yield(int i)
{
  if ((i & 7) == 7) { fflush(stdout); vTaskDelay(1); }
}

// ---------------------------------------------------------------------------
// Application commands
// ---------------------------------------------------------------------------
// An app shows information on screen while the console shows it as text. This
// is the same data through two paths, and the console is the only one that works
// on the bench without anyone watching the screen.

// ---------------------------------------------------------------------------
// C6 IDENTIFICATION
// ---------------------------------------------------------------------------
// READ-ONLY. This does not change the coprocessor; it only asks what it is.
//
// This exists because the following line has always appeared during boot:
//
//   esp-hosted fw versions: host=3.0.6 coprocessor=0.0.0
//
// A coprocessor that reports 0.0.0 either leaves the field empty or is not the
// reference esp_hosted firmware. This matters for two concrete reasons:
//
//   1. It is the likely cause of RSSI=0, which the Settings app already treats
//      as unavailable.
//   2. It determines whether ESP-NOW can run on the C6. ESP-NOW is not provided
//      by esp_hosted (esp_now_init does not link for this target), so the only
//      route would be custom C6 firmware using the peer_data channel. Before
//      considering replacing the firmware that currently makes WiFi work, we
//      need to know what is installed.
//
// The app descriptor contains the project name, version, IDF version, and build
// date. That is enough to tell whether this is an M5Stack build or factory
// esp_hosted firmware.
static void cmd_c6(void)
{
  if (!tab5_wifi_c6_link()) {
    printf("the C6 link is not up.\n"
           "Turn on WiFi in the Settings app and try again -\n"
           "without esp_hosted_init there is nobody to ask.\n");
    return;
  }

  printf("--- C6 coprocessor ---\n");

  eh_host_coprocessor_fwver_t v = { };
  if (eh_host_sys_get_cp_fw_version(&v) == ESP_OK) {
    printf("  firmware   : %d.%d.%d  (rev %d, build %d)\n",
           (int)v.major1, (int)v.minor1, (int)v.patch1,
           (int)v.revision, (int)v.build);
  } else {
    printf("  firmware   : no response\n");
  }

  // ONLY THE TRAVEL VERSION.
  //
  // O esp_hosted_app_desc_t tem project_name, date, time e idf_ver, e a
  // The first version of this command printed all four. It comes empty, and the
  // header itself says why:
  //
  //   /* App descriptor; wire carries version + idf_target only. */
  //
  // Os campos existem no struct porque ele espelha o esp_app_desc_t of IDF,
  // but the RPC does not transport the rest. Print field that never arrives and worse than
  // does not print: it seems like a coprocessor defect.
  esp_hosted_app_desc_t d = { };
  if (eh_host_sys_get_cp_app_desc(&d) == ESP_OK) {
    printf("  app version: %.*s\n", (int)sizeof(d.version), d.version);
  } else {
    printf("  app version: no response\n");
  }


  // The MAC is the ESP-NOW identity: there is no name there, only a six-byte
  // address. If ESP-NOW ever runs on the C6, this is the number another device
  // will register as its peer.
  uint8_t mac[6] = { };
  if (eh_host_sys_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    printf("  MAC (STA)  : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  if (eh_host_sys_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
    printf("  MAC (AP)   : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  // LAST notice, after the date. He was in the middle of reporting and
  // split the list in two - the MACs appeared after one block of alert,
  // como se fossem parte dele.
  //
  // The table of docs/migration.md of esp_hosted:
  //   3.x host + 3.x coprocessor Supported
  //   3.x host + 1.x backwards compatible coprocessor, update RECOMMENDED
  //   host < 3.x + 3.x coprocessor Not supported
  if (v.major1 != 0 && v.major1 < 3) {
    printf("\n  ATENCAO: host 3.x com coprocessador %d.x.\n"
           "  Combinacao retrocompativel, mas o esp_hosted RECOMENDA atualizar\n"
           "  o coprocessador. E o principal suspeito do RSSI=0 que o app Ajustes ja\n"
           "  trata como indisponivel.\n", (int)v.major1);
  }
}

// ---------------------------------------------------------------------------
// RECEIVE A FILE OVER SERIAL
// ---------------------------------------------------------------------------
//   receive <app> <file> <bytes>
//
// This was created to send 1.4 MB of C6 firmware to the card without removing
// the card from the device.
//
// Three non-obvious design decisions:
//
// 1. Read directly from the driver, not stdin. The console uses
//    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR), que traduz
//    end of line in entrada - inofensivo para text, DESTRUTIVO para binario:
//    todo 0x0D viraria outra coisa. O usb_serial_jtag_read_bytes() passa on
//    largo of VFS e entrega o byte como veio.
//
// 2. Store everything in PSRAM before writing to the card. Serial has no flow
//    controle of fluxo: se a gente stop para write no SD - e o FATFS para,
//    as vezes por centenas of milissegundos, when fecha one cluster - o buffer
//    of recepcao enche e os bytes se perdem em silencio. Com 32MB of PSRAM,
//    receiving integer and then recording decouples the two things completely.
//
// 3. Print nothing while receiving. The natural way to send the file is
//    "cat file > /dev/cu.usbmodem*", and in this arrangement NOBODY is reading the
//    saida. Se a gente write, o buffer of TX enche e a escrita bloqueia -
//    stopping precisely the loop that needs to consume the input. Silence until
//    end e o que mantem o fluxo vivo.
#define RECEIVE_MAX_BYTES (6u * 1024u * 1024u)

// Human-readable size in a private static buffer. It is static rather than
// because it appears twice in the SAME line of printf at some points - and
// two simultaneous uses of the same buffer would run over each other. That's why it alternates
// between two.
static const char* format_short_size(uint64_t bytes)
{
  static char buf[2][16];
  static int  alt = 0;
  char* b = buf[alt = (alt + 1) & 1];
  if      (bytes >= 1024 * 1024) { snprintf(b, 16, "%.1f MB", bytes / (1024.0 * 1024.0)); }
  else if (bytes >= 1024)        { snprintf(b, 16, "%.1f KB", bytes / 1024.0); }
  else                           { snprintf(b, 16, "%llu B", bytes); }
  return b;
}

// Card overview: space usage and one line per application folder.
//
// `list` shows ONE folder and is used to find a file. This answers the other
// first question: "what is here, and how much space is it using?"
static void cmd_card(void)
{
  {
    // O gravador of sistema (hal/tab5_sd.hpp): o que ainda espera, o que ja foi.
    uint32_t pend = 0, grav = 0, perd = 0;
    tab5_sd_append_status(&pend, &grav, &perd);
    printf("writer: %lu B pending, %lu line(s) written, %lu lost since boot\n",
           (unsigned long)pend, (unsigned long)grav, (unsigned long)perd);
  }
  if (!tab5_sd_mount()) {
    printf("card could not be mounted (%s)\n", tab5_sd_description());
    return;
  }

  printf("--- card ---\n  %s\n", tab5_sd_description());

  // Free space by FATFS. statvfs of VFS is not implemented for FAT in
  // IDF, entao vamos on driver: clusters livres x setores por cluster.
  FATFS* fs = nullptr;
  DWORD livres = 0;
  if (f_getfree("0:", &livres, &fs) == FR_OK && fs) {
    // REAL SECTOR, not FF_MAX_SS. The FF_MAX_SS and the MAXIMO sector that FATFS was
    // compiled to accept (4096 in IDF); the of card is 512. Multiply by
    // maximum gave 8x more - one card of 14.5GB appeared with 118GB.
    //
    // Quando FF_MAX_SS == FF_MIN_SS o field ssize nem existe (o FATFS o remove,
    // because there is only one size possible), hence the #if.
#if FF_MAX_SS != FF_MIN_SS
    const uint64_t setor = fs->ssize;
#else
    const uint64_t setor = FF_MAX_SS;
#endif
    const uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize * setor;
    const uint64_t liv = (uint64_t)livres * fs->csize * setor;
    printf("  %llu MB free of %llu MB (%llu%% used)\n",
           liv / (1024 * 1024), tot / (1024 * 1024),
           tot ? ((tot - liv) * 100 / tot) : 0);
  }

  DIR* d = opendir(TAB5_SD_APPS);
  if (!d) {
    printf("  (no %s yet)\n", TAB5_SD_APPS);
    tab5_sd_unmount();
    return;
  }

  printf("\n  %-16s %6s  %s\n", "folder", "files", "size");
  struct dirent* e;
  int k = 0;
  while ((e = readdir(d)) != nullptr) {
    yield(k++);
    if (e->d_name[0] == '.') { continue; }

    char sub[288];   // TAB5_SD_APPS + '/' + up to 255 of d_name + null
    snprintf(sub, sizeof(sub), "%s/%s", TAB5_SD_APPS, e->d_name);

    // Count and sum without recursion: of app folders are flat by convention
    // (ver tab5_sd.hpp), e lower em arvore aqui so criaria caminho para
    // blowing the stack on a card that someone filled with subfolders.
    DIR* sd = opendir(sub);
    if (!sd) { continue; }

    // Save the names to print AFTER the folder header - the total has
    // que aparecer em top, e so of para saber o total depois of percorrer.
    #define CARD_MAX_SHOWN 10
    char names[CARD_MAX_SHOWN][256];   // up to 255 of d_name + null
    uint64_t tams[CARD_MAX_SHOWN];
    int n = 0; uint64_t bytes = 0;

    struct dirent* f2;
    while ((f2 = readdir(sd)) != nullptr) {
      if (f2->d_name[0] == '.') { continue; }
      char cam[560];   // sub + '/' + up to 255 of d_name + null
      snprintf(cam, sizeof(cam), "%s/%s", sub, f2->d_name);
      struct stat st;
      if (stat(cam, &st) != 0 || !S_ISREG(st.st_mode)) { continue; }
      if (n < CARD_MAX_SHOWN) {
        snprintf(names[n], sizeof(names[0]), "%s", f2->d_name);
        tams[n] = (uint64_t)st.st_size;
      }
      ++n;
      bytes += (uint64_t)st.st_size;
    }
    closedir(sd);

    printf("  %-16s %6d  %s\n", e->d_name, n, format_short_size(bytes));

    // Os arquivos, indentados. Faltava exatamente isto: dava para ver que a
    // System folder had 3 files and 1.5MB, and it was NOT possible to see if the firmware
    // of C6 era one deles.
    const int shown = (n < CARD_MAX_SHOWN) ? n : CARD_MAX_SHOWN;
    for (int i = 0; i < shown; ++i) {
      printf("      %-28.28s %10s\n", names[i], format_short_size(tams[i]));
    }
    if (n > CARD_MAX_SHOWN) {
      printf("      ... and %d more\n", n - CARD_MAX_SHOWN);
    }
  }
  closedir(d);
  tab5_sd_unmount();
  printf("\n  'list %s/<folder>' shows the files in one folder\n", TAB5_SD_APPS);
}

static void cmd_receive(int argc, char** argv)
{
  if (argc < 4) {
    printf("usage: receive <app> <file> <bytes>\n"
           "  ex: receber Sistema tab5_c6.bin 1397904\n"
           "\n"
           "After entering the command, send the raw file through the same port.\n"
           "Do Mac, com o monitor FECHADO:\n"
           "  cat arquivo.bin > /dev/cu.usbmodem*\n"
           "\n"
           "Or do everything at once without the monitor:\n"
           "  { printf 'receber Sistema tab5_c6.bin 1397904\\r'; sleep 2; \\\n"
           "    cat arquivo.bin; } > /dev/cu.usbmodem*\n");
    return;
  }

  const long size = atol(argv[3]);
  if (size <= 0 || (unsigned long)size > RECEIVE_MAX_BYTES) {
    printf("invalid size (1 to %u bytes)\n", (unsigned)RECEIVE_MAX_BYTES);
    return;
  }

  uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
  if (!buf) { printf("not enough PSRAM for %ld bytes\n", size); return; }

  printf("send %ld bytes now (allow 10 s between blocks)\n", size);
  fflush(stdout);

  size_t lidos = 0;
  int vazios = 0;
  while (lidos < (size_t)size) {
    const int n = usb_serial_jtag_read_bytes(buf + lidos, (size_t)size - lidos,
                                             pdMS_TO_TICKS(1000));
    if (n > 0) { lidos += (size_t)n; vazios = 0; }
    else if (++vazios >= 10) { break; }   // gave up waiting
  }

  // RECORD WHAT ARRIVED, even incomplete. Throw 1.4MB away because of
  // last 2KB doesn't help anyone: with the file on the card and the md5 in screen,
  // of to see IF you missed and HOW MUCH you missed. Who decides if it serves is you - and the
  // `c6 gravar` confere o cabecalho antes of usar, of qualquer forma.
  const bool complete = (lidos == (size_t)size);
  if (lidos == 0) {
    printf("\nno bytes arrived.\n");
    heap_caps_free(buf);
    return;
  }
  if (!complete) {
    printf("\nINCOMPLETE: %u of %ld bytes (%ld missing). Saving anyway\n"
           "so you can inspect it - compare the MD5 below.\n",
           (unsigned)lidos, size, size - (long)lidos);
  }

  // MD5 to check against `md5 -q` of Mac. An equal size does not prove
  // nothing: byte swapped in the middle of serial of the same size.
  unsigned char md5[16];
  mbedtls_md5(buf, lidos, md5);

  bool ok = false;
  if (tab5_sd_mount()) {
    FILE* f = tab5_sd_open(argv[1], argv[2], "wb");
    if (f) {
      ok = (fwrite(buf, 1, lidos, f) == lidos);
      fclose(f);
    }
    tab5_sd_unmount();
  }
  heap_caps_free(buf);

  if (!ok) { printf("\nreceived everything, but writing to the card failed\n"); return; }

  printf("\ngravado: /sdcard/apps/%s/%s  (%u bytes)%s\nmd5: ",
         argv[1], argv[2], (unsigned)lidos, complete ? "" : "  <- INCOMPLETO");
  for (int i = 0; i < 16; ++i) { printf("%02x", md5[i]); }
  printf("\n");
}

// Read what the monitor already measured (tab5_monitor_foto) instead of taking
// two snapshots with uxTaskGetSystemState. Each snapshot requires a long
// critical section and internal allocations; the monitor snapshot covers the
// last second and includes ownership information.
static void cmd_tasks(void)
{
  mon_foto_t* f = (mon_foto_t*)heap_caps_malloc(sizeof(mon_foto_t), MALLOC_CAP_SPIRAM);
  if (!f) { printf("not enough memory for the snapshot\n"); return; }
  if (!tab5_monitor_foto(f)) { heap_caps_free(f); printf("the monitor has no first sample yet\n"); return; }

  // EM QUE ESTADO ISTO FOI MEDIDO. Sem esta line a measurement e ambigua: o mesmo
  // comando com a screen acesa e com ela apagada of numeros muito diferentes, e
  // Nothing in the portrait said which of the two you were looking at. Type in serial
  // Does NOT wake up the screen (only the touch wakes up), so to measure the sleep of
  // verdade - mas so se der para conferir que era sleep mesmo.
  printf("--- %d tasks   [%s]   CPU %.1f%%   snapshot %lu us (worst %lu) ---\n", f->n,
         tab5_power_is_sleeping() ? "SLEEPING - screen off" : "SCREEN ON",
         (double)f->cpu, (unsigned long)f->foto_us, (unsigned long)f->foto_pior_us);
  printf("  %-16s %3s %6s %6s %10s  %s\n", "name", "pri", "core", "CPU%", "free stack", "owner");

  for (int i = 0; i < f->n; ++i) {
    const mon_tarefa_t& t = f->t[i];
    // xCoreID is worth tskNO_AFFINITY (0x7FFFFFFF) when the task runs on both -
    // testar por negativo nunca acerta, e imprimir cru mostra one numero
    // gigante. Foi one bug real in coluna CORE of app.
    char nucleo[8];
    if (t.core == tskNO_AFFINITY) { snprintf(nucleo, sizeof(nucleo), "0e1"); }
    else                          { snprintf(nucleo, sizeof(nucleo), "%d", (int)t.core); }
    printf("  %-16s %3u %6s %6.1f %8u B  %s\n",
           t.name, (unsigned)t.priority, nucleo, (double)t.pct,
           (unsigned)t.stack_livre, t.dono);
  }
  // THE ACCOUNT IS FOR THE DEVICE, NOT PER CORE: the percentages add up to 100% (the
  // two cores together), the same Tasks app account. The two IDLE together
  // are the idleness of the device; The entire core occupied appears as 50%.
  printf("\n  Device CPU%% (both cores total 100%%): one full core = 50%%.\n");
  heap_caps_free(f);
}

static void cmd_memoria(void)
{
  // WHAT idf.py size DOESN'T COUNT.
  //
  // The compilation report only shows the STATIC: .text, .data, .bss. He doesn't
  // sabe nada of que e alocado depois - stack of tarefa, buffer of UART, queue
  // of WiFi, the two framebuffers of 1.8MB of panel. And that's exactly what
  // decide if one more app fits.
  printf("--- memory ---\n");

  const size_t i_liv = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t i_tot = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const size_t i_blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  const size_t p_liv = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t p_tot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  const size_t p_blk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  printf("  P4 SRAM     : %u KB usable (768 - L2 cache); heap %u KB, fixed (code+data) %u KB\n",
         (unsigned)((768u * 1024u - (unsigned)CONFIG_CACHE_L2_CACHE_SIZE) / 1024), (unsigned)(i_tot / 1024),
         (unsigned)(((768u * 1024u - (unsigned)CONFIG_CACHE_L2_CACHE_SIZE) - i_tot) / 1024));
  printf("  internal RAM: %u of %u KB used (%u%%), %u KB free\n",
         (unsigned)((i_tot - i_liv) / 1024), (unsigned)(i_tot / 1024),
         (unsigned)(i_tot ? ((i_tot - i_liv) * 100 / i_tot) : 0),
         (unsigned)(i_liv / 1024));
  // The "minimum already seen" is the number that matters to know if it is missing: the livre of
  // now it does not show the peak of usage that has already passed.
  printf("                minimum seen %u KB, largest block %u KB\n",
         (unsigned)(internal_min / 1024), (unsigned)(i_blk / 1024));

  printf("  PSRAM       : %u of %u KB used (%u%%), %u KB free\n",
         (unsigned)((p_tot - p_liv) / 1024), (unsigned)(p_tot / 1024),
         (unsigned)(p_tot ? ((p_tot - p_liv) * 100 / p_tot) : 0),
         (unsigned)(p_liv / 1024));
  printf("                minimum seen %u KB, largest block %u KB\n",
         (unsigned)(psram_min / 1024), (unsigned)(p_blk / 1024));

  const esp_partition_t* app = esp_ota_get_running_partition();
  if (app) {
    printf("  partition   : %u KB for the application\n", (unsigned)(app->size / 1024));
  }
}

// INTERNAL RAM MAP - where each byte is and how chopped up the free one is.
//
// "free 80 KB" does not say whether to allocate 16 KB: if free is 40
// pieces of 2 KB, not of. The heap of IDF (TLSF) is divided into REGIONS (each
// track of address left after .bss, TCM, RAM of LP, reserve
// DMA); a block never crosses regions. Here each block of each region and
// visited (heap_caps_walk) and counted by size range, used and free.
//
// COMO LER:
//   - blocos LIVRES pequenos e muitos = fragmentacao. O que importa e o
//     "greater" of each region, not the sum;
//   - blocos USADOS entre 4 e 16 KB sao malloc() comuns que o
//     SPIRAM_MALLOC_ALWAYSINTERNAL (16384) obriga a ficar in interna. Se forem
//     muitos, baixar esse limit os manda para a PSRAM.
#define MAPA_FAIXAS 6
static const uint32_t s_range_limits[MAPA_FAIXAS] = { 64, 256, 1024, 4096, 16384, UINT32_MAX };
static const char*    s_range_names[MAPA_FAIXAS] = { "<64", "<256", "<1K", "<4K", "<16K", ">=16K" };
#define MAPA_REGIOES 8

typedef struct {
  intptr_t ini, end;
  uint32_t usado, free_bytes, largest_free_block, n_livres, n_usados;
  uint32_t livres_n[MAPA_FAIXAS], usados_n[MAPA_FAIXAS], usados_b[MAPA_FAIXAS];
} heap_region_t;

typedef struct {
  heap_region_t r[MAPA_REGIOES];
  int n;
  bool estourou;
} mapa_t;

// It runs with the heap lock stuck: no printf or malloc in here.
static bool mapa_visitar(walker_heap_into_t h, walker_block_info_t b, void* u)
{
  mapa_t* m = (mapa_t*)u;
  heap_region_t* r = nullptr;
  for (int i = 0; i < m->n; ++i) { if (m->r[i].ini == h.start) { r = &m->r[i]; break; } }
  if (!r) {
    if (m->n >= MAPA_REGIOES) { m->estourou = true; return false; }
    r = &m->r[m->n++];
    memset(r, 0, sizeof(*r));
    r->ini = h.start; r->end = h.end;
  }
  int f = 0;
  while (f < MAPA_FAIXAS - 1 && b.size >= s_range_limits[f]) { ++f; }
  if (b.used) {
    r->usado += b.size; r->n_usados++;
    r->usados_n[f]++; r->usados_b[f] += b.size;
  } else {
    r->free_bytes += b.size; r->n_livres++;
    r->livres_n[f]++;
    if (b.size > r->largest_free_block) { r->largest_free_block = b.size; }
  }
  return true;
}

static void cmd_memory_mapa(void)
{
  // static: ~1 KB, outside of stack of console.
  static mapa_t m;
  memset(&m, 0, sizeof(m));
  heap_caps_walk(MALLOC_CAP_INTERNAL, mapa_visitar, &m);

  printf("--- internal RAM map by region ---\n");
  uint32_t u_n[MAPA_FAIXAS] = {0}, u_b[MAPA_FAIXAS] = {0}, l_n[MAPA_FAIXAS] = {0};
  for (int i = 0; i < m.n; ++i) {
    const heap_region_t& r = m.r[i];
    const uint32_t frag = r.free_bytes ? (uint32_t)(100 - (uint64_t)r.largest_free_block * 100 / r.free_bytes) : 0;
    printf("  %08x-%08x %4u KB: usado %4u KB em %4u blocos | livre %4u KB em %3u, maior %3u KB (frag %u%%)\n",
           (unsigned)r.ini, (unsigned)r.end, (unsigned)((r.end - r.ini) / 1024),
           (unsigned)(r.usado / 1024), (unsigned)r.n_usados,
           (unsigned)(r.free_bytes / 1024), (unsigned)r.n_livres, (unsigned)(r.largest_free_block / 1024),
           (unsigned)frag);
    for (int f = 0; f < MAPA_FAIXAS; ++f) {
      u_n[f] += r.usados_n[f]; u_b[f] += r.usados_b[f]; l_n[f] += r.livres_n[f];
    }
  }
  if (m.estourou) { printf("  (more than %d regions - list truncated)\n", MAPA_REGIOES); }

  printf("\n  range    used (count / KB)   free (count)\n");
  for (int f = 0; f < MAPA_FAIXAS; ++f) {
    printf("  %-6s   %5u / %5u       %5u\n", s_range_names[f],
           (unsigned)u_n[f], (unsigned)(u_b[f] / 1024), (unsigned)l_n[f]);
  }
}

// LEAK HUNT: 'memory brand' takes a photo of heap; 'memory delta' says what
// changed since ela. O uso e open e close o app (ou enable e disable o servico)
// sometimes between the two: what NAO iteration on value of brand and what became
// arrested. Compares bytes and QUANTITY of blocks used (a list that grows of
// 1 in 1 block escapes from count in bytes), by range of size in internal (the
// growing range tells the type of thing: stack, buffer, small object), and the
// living tasks (a task that doesn't die holds its stack).
typedef struct {
  bool     ok;
  mapa_t   mapa;                 // so a interna, por faixa
  uint32_t i_bytes, i_blocos;    // total usado, internal
  uint32_t p_bytes, p_blocos;    // total usado, PSRAM
  uint32_t task_count;
} memory_snapshot_t;
static memory_snapshot_t s_marked_memory;

static void capture_memory_snapshot(memory_snapshot_t* o)
{
  memset(o, 0, sizeof(*o));
  heap_caps_walk(MALLOC_CAP_INTERNAL, mapa_visitar, &o->mapa);
  multi_heap_info_t hi;
  heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
  o->i_bytes = (uint32_t)hi.total_allocated_bytes; o->i_blocos = (uint32_t)hi.allocated_blocks;
  heap_caps_get_info(&hi, MALLOC_CAP_SPIRAM);
  o->p_bytes = (uint32_t)hi.total_allocated_bytes; o->p_blocos = (uint32_t)hi.allocated_blocks;
  o->task_count = (uint32_t)uxTaskGetNumberOfTasks();
  o->ok = true;
}

static void cmd_memory_mark(void)
{
  capture_memory_snapshot(&s_marked_memory);
  printf("snapshot taken: internal %u KB in %u blocks, PSRAM %u KB in %u blocks, %u tasks.\n"
         "Open and close the app a few times, then run 'memory delta'.\n",
         (unsigned)(s_marked_memory.i_bytes / 1024), (unsigned)s_marked_memory.i_blocos,
         (unsigned)(s_marked_memory.p_bytes / 1024), (unsigned)s_marked_memory.p_blocos,
         (unsigned)s_marked_memory.task_count);
}

static void cmd_memory_delta(void)
{
  if (!s_marked_memory.ok) { printf("no snapshot - run 'memory mark' first\n"); return; }
  static memory_snapshot_t now;
  capture_memory_snapshot(&now);
  printf("--- changes since snapshot ---\n");
  printf("  internal : %+ld B in %+ld blocks\n", (long)now.i_bytes - (long)s_marked_memory.i_bytes,
         (long)now.i_blocos - (long)s_marked_memory.i_blocos);
  printf("  PSRAM   : %+ld B em %+ld blocos\n", (long)now.p_bytes - (long)s_marked_memory.p_bytes,
         (long)now.p_blocos - (long)s_marked_memory.p_blocos);
  printf("  tasks    : %+ld\n", (long)now.task_count - (long)s_marked_memory.task_count);
  uint32_t a_n[MAPA_FAIXAS] = {0}, a_b[MAPA_FAIXAS] = {0}, m_n[MAPA_FAIXAS] = {0}, m_b[MAPA_FAIXAS] = {0};
  for (int i = 0; i < now.mapa.n; ++i) {
    for (int f = 0; f < MAPA_FAIXAS; ++f) { a_n[f] += now.mapa.r[i].usados_n[f]; a_b[f] += now.mapa.r[i].usados_b[f]; }
  }
  for (int i = 0; i < s_marked_memory.mapa.n; ++i) {
    for (int f = 0; f < MAPA_FAIXAS; ++f) { m_n[f] += s_marked_memory.mapa.r[i].usados_n[f]; m_b[f] += s_marked_memory.mapa.r[i].usados_b[f]; }
  }
  printf("\n  internal used blocks by range (difference):\n");
  for (int f = 0; f < MAPA_FAIXAS; ++f) {
    printf("  %-6s   %+5ld blocos  %+7ld B\n", s_range_names[f],
           (long)a_n[f] - (long)m_n[f], (long)a_b[f] - (long)m_b[f]);
  }
  printf("\n  Near zero after opening/closing means no leak. If the same number\n"
         "  grows every cycle, there is a leak; the range identifies the block type.\n");
}

static void cmd_memory_extrato(void)
{
  static memory_marco_t e[MEM_MARCOS_N];
  const int n = tab5_memory_extrato(e, MEM_MARCOS_N);
  printf("--- extrato da RAM interna (boot e subida dos servicos) ---\n");
  printf("  %8s  %-26s %8s %8s %8s %8s\n", "ms", "etapa", "livre", "delta", "maior", "minimo");
  for (int i = 0; i < n; ++i) {
    printf("  %8lu  %-26s %8lu %+8ld %8lu %8lu\n", (unsigned long)e[i].ms, e[i].etapa,
           (unsigned long)e[i].livre, (long)e[i].delta, (unsigned long)e[i].maior_bloco,
           (unsigned long)e[i].minimum);
  }
  if (n == MEM_MARCOS_N) { printf("  (cheio: marcos seguintes descartados)\n"); }
}

// QUEM ESTA PROIBINDO O LIGHT SLEEP.
//
// All the energy savings of IDF pass through the PM locks. A driver that
// need for stable clock creates a lock on startup and secures it; while
// there is a latch alive, the system does not go down to the mode it prohibits - and it does not
// there is no warning, esp_pm simply does not sleep.
//
// This is how it was discovered that the DPI panel held two latches
// since a criacao e so as soltava no destrutor - o que levou on
// DESMONTAR_DSI_NO_REPOUSO em tab5_power.cpp. Sem este despejo, aquilo teria
// sido adivinhacao.
//
// HOW TO USE: the number that matters is not this one, with the screen on. And the of INSIDE
// of sleep - e por isso o comando aceita one atraso em segundos:
//
//     energy locks 30 (let the screen erase and only then dump)
//
// Tipos que importam: NO_LIGHT_SLEEP proibe dormir; CPU_FREQ_MAX prende o
// clock on the ceiling; APB_FREQ_MAX traps the bus - and this also prevents
// light sleep, porque dormir pararia o APB.
static void cmd_travas(int seconds)
{
#if CONFIG_PM_ENABLE
  // The time of screen comes from Settings and can be anything. Say which one and
  // evita a measurement inutil mais provavel: wait menos que ele e despejar as
  // locks with the screen still on, when the panel still holds both.
  const int screen = (int)settings_screen_timeout_s();
  if (seconds > 0 && seconds <= screen) {
    printf("WARNING: the screen turns off in %d s, but you requested %d. The measurement will be awake.\n"
           "         use 'power locks %d' or more.\n", screen, seconds, screen + 10);
  }

  if (seconds > 0) {
    printf("waiting %d s - do not touch the screen (you may use the serial keyboard)...\n",
           seconds);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)seconds * 1000));
  }

  const bool dormindo = tab5_power_is_sleeping();
  printf("--- travas de PM  [%s] ---\n",
         dormindo ? "SLEEPING - this is the valid measurement"
                  : "SCREEN ON - this measurement is not valid for light sleep");
  esp_pm_dump_locks(stdout);

  if (dormindo) {
    printf("\nempty, or only released locks = light sleep is available\n"
           "dsi_dpi / dsi_phy held      = the screen was not unmounted\n"
           "uart_driver held            = Grove; intentional design decision\n");
  } else {
    printf("\nthe screen did not turn off. Configured time: %d s.\n"
           "try 'power locks %d' and do not touch the screen while waiting.\n",
           screen, screen + 10);
  }
#else
  (void)seconds;
  printf("CONFIG_PM_ENABLE is disabled: there are no locks to show\n");
#endif
}

// FORCE SLEEP NOW, at the clock you choose - to find out if one
// piso MENOR que os 40MHz of hoje (ver o "int piso = ... : 40" em
// core.cpp) and accepted by the silicon and the panel, without rewriting each
// new number. Same idea of ​​'screen clock <mhz>' - compare running on
// vez of adivinhar.
//
// SAFE CALL FROM CONSOLE (a different task from interface): reread
// tab5_power_sleep()/resume() and neither of them touch I2C - they just change the
// panel DSI, no PM (esp_pm_configure) e no AXI-ICM. A regra of "so a
// interface talks to the internal bus" and about INA226/touch/keyboard/
// IMU, and it doesn't apply here.
//
// UM CUIDADO QUE FICA COM QUEM TESTA: se o sleep AUTOMATICO (o timer of
// screen idleness, in Settings) triggers at the SAME instant as this command,
// both calls run without any hangs due to top of the same state
// (s_em_repouso e companhia sao globais of modulo, sem mutex) - e o resultado
// it would be dismantling the panel twice. In practice, the window is very narrow
// (o guard "ja em sleep?" barra o caso comum), mas para testar com sossego
// increase the time of screen in Settings before, or test right after touch a
// screen (the idle clock has just reset).
//
// If the MHz request does not exist in this silicon revision (valids: 360, 180,
// 90 e XTAL/N -> 40, 20, 10, 8, 5, 4, 2, 1), tab5_power_sleep() ja desfaz
// alone and returns false - the screen iteration lighting up, without locking anything.
static void cmd_sleep_forcar(int mhz)
{
  if (mhz <= 0) {
    printf("usage: power sleep <mhz>   (example: power sleep 20)\n"
           "valid values on this board: " TAB5_CPU_STEPS "\n");
    return;
  }
  if (tab5_power_is_sleeping()) {
    printf("already sleeping - touch the screen to wake it, then try again\n"
           "(calling this again does nothing: tab5_power_sleep() is "
           "idempotent while sleeping)\n");
    return;
  }

  printf("entering sleep at %d MHz...\n", mhz);
  fflush(stdout);
  const bool ok = tab5_power_sleep(mhz);
  if (ok) {
    printf("sleep confirmed. The screen is off, but this console remains alive.\n"
           "Use 'power' now (and again in a few seconds, after the current settles)\n"
           "to measure. Touch the screen to wake it.\n");
  } else {
    printf("o repouso RECUSOU esse clock - o log de erro acima (ESP_LOGE de "
           "'power') explica o motivo. a tela deve ter voltado a acender sozinha.\n");
  }
}

// CPU ROOF WITH SCREEN WOKEN - see large warning in tab5_power.hpp.
// Unlike 'energia sleep', the screen DOES NOT go off: the panel remains enabled,
// desenhando, no clock of pixel que estiver configurado now ('screen
// clock <mhz>'). Existe para testar a combinacao que faltava: panel devagar
// And CPU slowly, both AWAKE at the same time.
static void cmd_teto_cpu(int mhz)
{
  if (mhz <= 0) {
    printf("uso: energia teto <mhz>   (ex: energia teto 180)\n"
           "combine com 'tela relogio <mhz>' primeiro, se for testar os dois juntos\n"
           "valores validos nesta placa: " TAB5_CPU_STEPS "\n");
    return;
  }
  const bool ok = tab5_power_cpu_ceiling(mhz);
  if (ok) {
    printf("teto aceito - a tela continua ligada, de olho em underrun/piscada azul\n"
           "agora. Se der problema, 'energia teto %d' desfaz na hora.\n"
           "\n"
           "ATENCAO se for deixar o aparelho parado: o repouso fotografa o clock\n"
           "ATUAL como \"normal\" e acorda de volta nele - se a tela apagar sozinha\n"
           "com o teto ainda em %d, ela acorda PRESA em %d, nao em %d. Volte para\n"
           "%d antes de largar o aparelho, ou abra/feche qualquer app (que\n"
           "reaplica o perfil e resolve sozinho).\n",
           TAB5_CPU_MAX_MHZ, mhz, mhz, TAB5_CPU_MAX_MHZ, TAB5_CPU_MAX_MHZ);
  } else {
    printf("REJECTED - the error log above ('power') explains why.\n");
  }
}

static void cmd_power(void)
{
  // CACHE READING, NOT INA226.
  //
  // The I2C_Class of M5Unified does not have a mutex: if this task is on INA226
  // while the interface reads the touch, the two transactions are interspersed and you read
  // registrador errado - sem erro nenhum, so numero sem sentido. A interface
  // measure once per second and leave the value here; and from here it is read.
  const tab5_measurement_t m = tab5_power_latest();

  printf("--- energia ---\n");
  printf("  agora    : %.2f V   %.0f mA   %.0f mW   %d%%   CPU %d MHz\n",
         m.volts, m.mA, m.mW, m.pct, tab5_power_cpu_mhz());
  printf("             %s\n",
         (m.mA > 20)  ? "carregando"
       : (m.mA < -20) ? "on battery" : "idle / plugged in, not charging");

  // O ESTIMADOR, on vivo - ver estimar_pct() em tab5_power.cpp. A diferenca
  // entre "estimador" e "curva" e o que a contagem of mAh esta corrigindo.
  {
    float soc, curva, since; const char* source;
    tab5_power_debug_soc(&soc, &curva, &source, &since);
    printf("  bateria  : estimador %.1f%%  | curva de tensao diria %.0f%%  | partiu de: %s\n",
           soc, curva, source);
    if (since >= 0) {
      printf("             ultima carga ha %.0f min%s\n", since,
             since < 60 ? " (a curva ainda nao corrige - tensao pos-carga mente)" : "");
    }
  }

  tab5_point_t h[TAB5_HIST_N];
  const int n = tab5_power_history(h);
  if (n <= 1) {
    printf("\n  historico ainda vazio (uma amostra a cada %d s)\n", TAB5_HIST_SEG);
    return;
  }

  // Scale of bar due to GREATER consumption of window, and not due to a fixed roof: with
  // fixed ceiling, an entire window in sleep would become a column of nothing.
  float pico = 1.0f;
  for (int i = 0; i < n; ++i) { const float v = fabsf(h[i].mA); if (v > pico) pico = v; }

  printf("\n  ultimos %d min (uma linha por minuto, barra = corrente ate %.0f mA)\n",
         (n * TAB5_HIST_SEG) / 60, pico);
  printf("  %6s %7s %11s\n", "quando", "tensao", "corrente");

  // Every two samples: with TAB5_HIST_SEG=30 this is one line per
  // minuto, e 30 lines cabem numa screen of terminal. Todas as 60 obrigariam a
  // rolar para ver o comeco.
  for (int i = 0; i < n; i += 2) {
    const int minutes_ago = ((n - 1 - i) * TAB5_HIST_SEG) / 60;
    char bar[33];
    int larg = (int)((fabsf(h[i].mA) / pico) * 32.0f);
    if (larg < 0)  { larg = 0; }
    if (larg > 32) { larg = 32; }
    memset(bar, '#', (size_t)larg);
    bar[larg] = 0;
    printf("  -%3d m %6.2fV %7.0f mA  %s\n", minutes_ago, h[i].volts, h[i].mA, bar);
  }
}



// Path on the card: absolute as it came; relative, from /sdcard. Save
// type the prefix in every command of file ('read apps/Sistema/x.txt').
static const char* sd_path(const char* c, char* buf, size_t n)
{
  if (c[0] == '/') { return c; }
  snprintf(buf, n, "/sdcard/%s", c);
  return buf;
}

// Mount the card, run whatever you pass, dismount - always, including when
// the command fails. Written as a function because each command forgets the
// dismantling would be one leak of counting: the card would be assembled for
// always and the app that uses it would never be able to truly dismantle it.
template <typename F>
static int with_card(F&& body)
{
  if (!tab5_sd_mount())
  {
    printf("error: card could not be mounted (%s)\n", tab5_sd_description());
    return 1;
  }
  const int r = body();
  tab5_sd_unmount();
  return r;
}

// ---------------------------------------------------------------------------
// wifi gravar <ssid> [senha]
// ---------------------------------------------------------------------------
static int wifi_save_credentials(const char* ssid, const char* password_or_null)
{
  const char* password = password_or_null ? password_or_null : "-";   // "-" = rede open

  return with_card([&]() -> int {
    // tab5_wifi_save_credentials() cifra a senha antes of write (AES-128-CBC,
    // see tab5_wifi.cpp) and already update the copy in RAM too - reread the file
    // then, with the C6 in the air, that's exactly what can't happen.
    if (!tab5_wifi_save_credentials(ssid, password))
    {
      printf("error: could not write %s\n", TAB5_WIFI_FILE);
      return 1;
    }
    printf("saved to %s (password encrypted in the file)\n  SSID:  %s\n  password: %s\n",
           TAB5_WIFI_FILE, ssid,
           strcmp(password, "-") == 0 ? "(rede aberta)" : "definida");
    printf("para usar agora: wifi conectar \"%s\"\n", ssid);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// read <caminho> [lines]
// ---------------------------------------------------------------------------
static int cmd_read(int argc, char** argv)
{
  if (argc < 2) { printf("uso: ler <caminho> [linhas]\n"); return 1; }
  char buf[160];
  const char* path = sd_path(argv[1], buf, sizeof(buf));
  // Teto of lines: one satelites-*.csv of one dia inteiro tem milhares delas,
  // e despejar everything a 115200 baud prenderia o console por minutos. Quem quer
  // but asks for the number; anyone who wants the entire file uses 'dump'.
  int teto = (argc >= 3) ? atoi(argv[2]) : 200;
  if (teto < 1) { teto = 1; }

  return with_card([&]() -> int {
    FILE* f = fopen(path, "r");
    if (!f) { printf("erro: %s nao abriu\n", path); return 1; }

    printf("--- %s ---\n", path);
    char line[160];
    int n = 0;
    while (n < teto && fgets(line, sizeof(line), f)) {
      fputs(line, stdout);
      if ((++n % 64) == 0) { fflush(stdout); vTaskDelay(1); }
    }
    if (n >= teto && !feof(f)) {
      printf("... (cortado em %d linhas - 'ler %s <N>' mostra mais, 'despejar' mostra tudo)\n",
             teto, argv[1]);
    }
    printf("--- fim ---\n");
    fclose(f);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// dump <path> - the ENTIRE file, to be captured from another side
// ---------------------------------------------------------------------------
// Irmao of "read", com tres diferencas que sao a razao of ele existir separado:
//
//   1. NO ROOF ROOMS. The "read" cuts at 200 because it is for LOOKING - and
//      Looking more than that in the terminal screen is useless. This is for
//      COPY: one energy.csv of an entire download has about 500 lines, and one
//      file cut in the middle is not used to adjust curve none.
//
//   2. CALA O LOG ENQUANTO DESPEJA. Este console divide a serial com o
//      ESP_LOG of all tasks. Without remaining silent, a line of "power" or
//      "mesh" cai NO MEIO of CSV, e quem esta capturando of outro lado recebe
//      one contaminated file. Restored at the end, always - including when the
//      fopen fails.
//
//   3. MARCADORES QUE O SCRIPT RECONHECE. O ">>>INICIO" e o ">>>FIM" com a
//      count of lines tell the capturer where it starts, where it ends and if
//      chegou everything. Sem eles, o script of outro lado teria que adivinhar
//      when stop of escutar - e adivinhar sempre erra in hora errada.
static int cmd_despejar(int argc, char** argv)
{
  if (argc < 2) {
    printf("uso: despejar <caminho>\n"
           "  ex.: despejar /sdcard/apps/Sistema/energia/energia-AAAA-MM-DD.csv\n"
           "  no Mac, para salvar:  ./tools/download.py <caminho>\n");
    return 1;
  }
  char buf[160];
  const char* path = sd_path(argv[1], buf, sizeof(buf));

  return with_card([&]() -> int {
    FILE* f = fopen(path, "r");
    if (!f) { printf("erro: %s nao abriu\n", path); return 1; }

    // Size BEFORE, so the other side can check what they received.
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) { size = ftell(f); fseek(f, 0, SEEK_SET); }

    // A partir daqui, silencio - ver o reason 2 no cabecalho.
    esp_log_level_set("*", ESP_LOG_NONE);

    printf(">>>INICIO %s %ld\n", path, size);
    char line[256];
    long n = 0;
    while (fgets(line, sizeof(line), f)) {
      fputs(line, stdout);
      // A pause every 64 lines. It's not education: the dump of one file
      // grande e one loop closed nesta tarefa, e sem yield a CPU o watchdog of
      // tarefa ociosa reclama e a interface engasga.
      if ((++n % 64) == 0) { fflush(stdout); vTaskDelay(1); }
    }
    fflush(stdout);
    printf(">>>FIM %ld linhas\n", n);

    esp_log_level_set("*", ESP_LOG_INFO);
    fclose(f);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// lista [caminho]
// ---------------------------------------------------------------------------
static int cmd_list(int argc, char** argv)
{
  char buf[160];
  const char* path = (argc >= 2) ? sd_path(argv[1], buf, sizeof(buf)) : "/sdcard";

  return with_card([&]() -> int {
    DIR* d = opendir(path);
    if (!d) { printf("erro: %s nao abriu\n", path); return 1; }

    printf("--- %s ---\n", path);
    struct dirent* e;
    int i = 0;
    while ((e = readdir(d)) != nullptr)
    {
      yield(i++);
      char complete[300];
      snprintf(complete, sizeof(complete), "%s/%s", path, e->d_name);
      struct stat st;
      if (e->d_type == DT_DIR)            { printf("  [dir]  %s\n", e->d_name); }
      else if (stat(complete, &st) == 0)  { printf("  %6ld  %s\n", (long)st.st_size, e->d_name); }
      else                                { printf("     ?   %s\n", e->d_name); }
    }
    closedir(d);
    printf("--- fim ---\n");
    return 0;
  });
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// erase <path> - removes ONE file from card
// ---------------------------------------------------------------------------
//
// It exists to be able to start a recording from scratch without removing the card. Delete and
// irreversivel, entao:
//   - only accepts path within /sdcard/, so the wrong finger won't reach it
//     outra coisa;
//   - one file at a time, no wildcard: nada of "erase *".
static int cmd_erase(int argc, char** argv)
{
  if (argc < 2) { printf("uso: apagar apps/<App>/<arquivo>\n"); return 1; }
  char buf[160];
  const char* path = sd_path(argv[1], buf, sizeof(buf));

  if (strncmp(path, "/sdcard/", 8) != 0 || strstr(path, "..") != nullptr)
  {
    printf("recusado: so apago dentro de /sdcard/ (e sem '..')\n");
    return 1;
  }

  return with_card([&]() -> int {
    struct stat st;
    if (stat(path, &st) != 0) { printf("nao existe: %s\n", path); return 1; }

    if (remove(path) != 0) { printf("nao consegui apagar %s\n", path); return 1; }
    printf("apagado: %s (%ld bytes)\n", path, (long)st.st_size);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// i2c <sda> <scl> - find out who responds to a pair of pins
// ---------------------------------------------------------------------------
//
// There is no way to guess the protocol of new hardware. The side connector of
// Tab5 exposes G49, G50, G0, G1 and EXT 5V; If the keyboard speaks I2C, its address
// appears here and the integration becomes trivial. If nothing responds to none
// combination, it is a signal that is not I2C - and then the next hypothesis is UART.
//
// Use the pair in BOTH senses: there is no way of knowing outside who is SDA and who is
// SCL, and changed it doesn't respond at all.
//
//   i2c 49 50        i2c 50 49        i2c 0 1        i2c 1 0
//
// Run from the initial screen: the command affects the external bus, which is the
// mesmo que o Azimutal toma emprestado como UART of GPS.
static int cmd_i2c(int argc, char** argv)
{
  if (argc < 3) { printf("uso: i2c <sda> <scl>   (ex: i2c 49 50)\n"); return 1; }

  // THE WARNING HAS BECOME A LOCK. Until now, "run from initial screen" was just one
  // comment in the source - the command reconfigured the external bus by
  // bottom of quem quer que o estivesse usando, sem perguntar. O Azimutal usa
  // this same bus as the UART of GPS, and the keyboard of ExtPort1 also
  // is in it: running this with either one of the two open starts the
  // bus of task owner in the middle of a transaction.
  //
  // CONSISTENCY that was worth it: a bench rule documented in
  // comment is a rule that only applies as long as someone remembers to read it
  // comment. By checking the command itself, it is always valid.
  const char* app = system_app_current();
  if (app) {
    printf("nao da para varrer o I2C com '%s' aberto - ele pode estar usando\n"
           "o mesmo barramento externo. Volte para a tela inicial e tente de novo.\n", app);
    return 1;
  }

  const int sda = atoi(argv[1]);
  const int scl = atoi(argv[2]);

  // The EXTERNAL bus (I2C_NUM_1) is the only one that we can reuse. THE
  // interno (I2C_NUM_0) carrega o INA226 e os expansores, e e propriedade of
  // tarefa of interface - encostar nele daqui seria a corrida que o sistema
  // inteiro evita.
  M5.Ex_I2C.release();

  if (!M5.Ex_I2C.begin(I2C_NUM_1, sda, scl))
  {
    printf("nao consegui abrir I2C com SDA=%d SCL=%d\n", sda, scl);
    M5.Ex_I2C.begin();
    return 1;
  }

  printf("varrendo I2C em SDA=%d SCL=%d ...\n", sda, scl);
  bool achou[128] = { };
  M5.Ex_I2C.scanID(achou);

  int n = 0;
  for (int a = 0; a < 128; ++a)
  {
    if (achou[a]) { printf("  0x%02X responde\n", (unsigned)a); ++n; }
  }
  if (n == 0) { printf("  ninguem respondeu\n"); }
  else        { printf("  %d dispositivo(s)\n", n); }

  // Returns the external bus to the pins of Grove, as M5.begin() left it.
  M5.Ex_I2C.release();
  M5.Ex_I2C.begin();
  return 0;
}

// ---------------------------------------------------------------------------
// SECOND CONSUMER OF LIE (with one app just not able to test).
// ---------------------------------------------------------------------------
// The system only opens ONE app at a time, so there is no need to prove that
// tab5_gps_position()/tab5_gps_time()/tab5_imu_latest() can handle TWO
// readers on the same team just open two apps - there is no such thing as "two apps
// abertos" ainda. Esta tarefa e one segundo leitor of verdade, independente
// of qualquer app: martela as tres funcoes bem mais rapido que qualquer
// screen draws, while you open Azimutal or IMU normally. Proof
// two things: that the lock can withstand concurrent use without locking
// nem derrubar o sistema, e (comparando os numeros daqui com os of screen) que
// both readers always see the SAME value, never one "updated middle".
static bool s_conc_active = false;
static bool s_conc_stop = false;
static uint32_t s_conc_leituras = 0;

static void vTaskConcorrencia(void*)
{
  uint32_t last_log_ms = 0;
  while (!s_conc_stop) {
    nmea_position_t pos  = tab5_gps_position();
    nmea_time_t   gps_time = tab5_gps_time();
    tab5_imu_data_t imu;
    tab5_imu_latest(&imu);
    ++s_conc_leituras;
    (void)gps_time;

    const uint32_t now = tab5_now_ms();
    if (now - last_log_ms >= 1000) {
      last_log_ms = now;
      printf("[concorrencia] %lu leituras | gps valida=%d lat=%.5f lon=%.5f "
             "| imu valida=%d ax=%.2f ay=%.2f az=%.2f\n",
             (unsigned long)s_conc_leituras, (int)pos.valid, pos.lat, pos.lon,
             (int)imu.valid, imu.ax, imu.ay, imu.az);
    }
    // SEM vTaskDelay(0)/yield puro - 1 tick (o minimum real) ainda martela bem
    // mais rapido que qualquer app desenha (20ms of ciclo), sem virar one
    // busy-loop que rouba o core of quem esta desenhando of verdade.
    vTaskDelay(1);
  }
  s_conc_active = false;
  s_conc_stop = false;
  vTaskDelete(nullptr);
}

static void cmd_concorrencia(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "iniciar") == 0) {
    if (s_conc_active) { printf("ja esta rodando - %lu leituras ate agora\n", (unsigned long)s_conc_leituras); return; }
    s_conc_active = true;
    s_conc_stop = false;
    s_conc_leituras = 0;
    xTaskCreatePinnedToCore(vTaskConcorrencia, "conc_teste", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    printf("segundo leitor no ar - martelando GPS+IMU independente de qualquer app.\n"
           "abra o IMU (ou outro app que leia GPS/IMU) e compare os numeros com os daqui.\n"
           "'concorrencia parar' quando terminar.\n");
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "parar") == 0) {
    if (!s_conc_active) { printf("nao estava rodando\n"); return; }
    s_conc_stop = true;
    printf("pedido para parar - confirma na proxima volta (ate 1 tick)\n");
    return;
  }
  printf("rodando: %s | leituras ate agora: %lu\n"
         "uso: concorrencia iniciar | concorrencia parar\n",
         s_conc_active ? "sim" : "nao", (unsigned long)s_conc_leituras);
}

// ---------------------------------------------------------------------------
// THREE FAKE "APPS", EACH WITH ITS OWN TASK
// (more rigorous test: the 3 apps appear on the console asking for the service).
// ---------------------------------------------------------------------------
// A diferenca para "concorrencia" acima: aquele e UM leitor generico,
// martelando o mais rapido possivel. Isto aqui e tres CONSUMIDORES
// DIFFERENT, each with the default of access of one app of truth -
// own cadence, log with own name - running on the same team, in
// SAME interface task that any real app would use (no, just each one in
// YOUR task, independent of the other two and of any app open in
// screen). E o mais perto of "3 apps abertos on mesmo time" que of para
// simulate sem o sistema realmente suportar isso ainda.
//
// App-Map: just GPS, every 200ms - like a screen of position/map.
// App-Nivel  : le so IMU, o mais rapido possivel - como one nivel-bolha.
// App-Log: reads GPS AND IMU together, every 1s - like a periodic logger.
//              This is what matters most: it proves that a consumer reading
//              TWO services in sequence do not lock (the GPS and IMU locks
//              are independent, never one inside the other - without this
//              haveria risco of deadlock se one dia alguem invertesse a
//              order em outro consumidor).
static bool     s_sim_running = false;
static bool     s_sim_stop_requested = false;
static uint32_t s_sim_read_counts[3] = { 0, 0, 0 };

static void vTaskSimMapa(void*)
{
  uint32_t last_log_ms = 0;
  while (!s_sim_stop_requested) {
    nmea_position_t pos = tab5_gps_position();
    ++s_sim_read_counts[0];

    const uint32_t now = tab5_now_ms();
    if (now - last_log_ms >= 1000) {
      last_log_ms = now;
      printf("[app-mapa]  pedido #%-6lu -> gps valida=%d lat=%.5f lon=%.5f\n",
             (unsigned long)s_sim_read_counts[0], (int)pos.valid, pos.lat, pos.lon);
    }
    vTaskDelay(pdMS_TO_TICKS(200));   // cadence of a screen of position
  }
  vTaskDelete(nullptr);
}

static void vTaskSimNivel(void*)
{
  uint32_t last_log_ms = 0;
  while (!s_sim_stop_requested) {
    tab5_imu_data_t d;
    tab5_imu_latest(&d);
    ++s_sim_read_counts[1];

    const uint32_t now = tab5_now_ms();
    if (now - last_log_ms >= 1000) {
      last_log_ms = now;
      printf("[app-nivel] pedido #%-6lu -> imu valida=%d ax=%.2f ay=%.2f az=%.2f\n",
             (unsigned long)s_sim_read_counts[1], (int)d.valid, d.ax, d.ay, d.az);
    }
    vTaskDelay(1);   // o mais rapido possivel, como one nivel-bolha precisa
  }
  vTaskDelete(nullptr);
}

static void vTaskSimLog(void*)
{
  while (!s_sim_stop_requested) {
    nmea_position_t pos = tab5_gps_position();
    nmea_time_t   gps_time = tab5_gps_time();
    tab5_imu_data_t imu;
    tab5_imu_latest(&imu);
    ++s_sim_read_counts[2];

    printf("[app-log]   pedido #%-6lu -> gps(valida=%d) + hora(valida=%d) + imu(valida=%d, az=%.2f)\n",
           (unsigned long)s_sim_read_counts[2], (int)pos.valid, (int)gps_time.valid, (int)imu.valid, imu.az);
    vTaskDelay(pdMS_TO_TICKS(1000));   // cadencia of one logger periodico
  }
  vTaskDelete(nullptr);
}

static void cmd_simulacao(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "iniciar") == 0) {
    if (s_sim_running) { printf("ja esta rodando\n"); return; }
    s_sim_running = true;
    s_sim_stop_requested = false;
    s_sim_read_counts[0] = s_sim_read_counts[1] = s_sim_read_counts[2] = 0;
    xTaskCreatePinnedToCore(vTaskSimMapa,  "sim_mapa",  4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTaskSimNivel, "sim_nivel", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTaskSimLog,   "sim_log",   4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    printf("3 apps de mentira no ar: app-mapa (GPS/200ms), app-nivel (IMU/o mais rapido\n"
           "possivel), app-log (GPS+IMU/1s). Abra apps de verdade agora e compare.\n"
           "'simulacao parar' quando terminar.\n");
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "parar") == 0) {
    if (!s_sim_running) { printf("nao estava rodando\n"); return; }
    s_sim_stop_requested = true;
    s_sim_running = false;
    printf("pedido para parar os 3 - confirma nas proximas voltas de cada um\n");
    return;
  }
  printf("rodando: %s | leituras: mapa=%lu nivel=%lu log=%lu\n"
         "uso: simulacao iniciar | simulacao parar\n",
         s_sim_running ? "sim" : "nao",
         (unsigned long)s_sim_read_counts[0], (unsigned long)s_sim_read_counts[1], (unsigned long)s_sim_read_counts[2]);
}

// ---------------------------------------------------------------------------
// 'photo' - PROOF THAT THE CAMERA BECAME A SERVICE, IN THE SAME WAY
// que 'concorrencia'/'simulacao' acima provam GPS/IMU: liga a camera, tira
// ONE photo and turn it off again, entirely OUT of the Camera app - if this
// funcionar com o app Camera FECHADO, e funcionar of novo com ele ABERTO on
// mesmo time (ver a contagem of referencia em tab5_camera.hpp), a
// Independence between service and app is proven in hardware, not just in
// papel.
//
// THE PHOTO DOES NOT DEPEND ON THE SIZE REQUESTED HERE: it always exits with the frame
// integer of sensor (TAB5_CAM_PHOTO_W x _A). The size of tab5_camera_start
// e so o of preview, que este comando nem usa - vai o menor 3:4 razoavel.
static void cmd_photo(void)
{
  const int PREVIEW_WIDTH = 240, PREVIEW_HEIGHT = 320;

  // system_camera_start/stop, NOT direct tab5_camera_start/stop -
  // achado on vivo: esta funcao roda in tarefa "console", e a
  // HAL of camera toca SCCB (I2C interno) para configurar o sensor. Chamar
  // direct collided with the reading of 1s/1s that the INTERFACE task does
  // alone on this same bus (energy/IMU/sensors/time) - "Set
  // format regs fail" no log, sensor configurado pela metade. A ponte (ver
  // the big warning in kernel/app.h) sends the request to the right task and
  // wait. tab5_camera_request_captura()/captura_pronta() below DOES NOT play
  // I2C nenhum (so leem/escrevem one mutex of RAM) - seguros of chamar
  // daqui sem ponte.
  printf("ligando a camera (%dx%d)...\n", PREVIEW_WIDTH, PREVIEW_HEIGHT);
  if (!system_camera_start(PREVIEW_WIDTH, PREVIEW_HEIGHT)) {
    printf("falhou - confira o log (sensor ausente? app Camera aberto numa\n"
           "resolucao diferente, ja que so existe uma por vez?)\n");
    return;
  }

  // A camera so CAPTURA (one frame, por PPA, in tarefa dela); gravar e daqui,
  // of tarefa of console - ver services/tab5_bmp e tab5_camera_request_photo.
  uint16_t* photo = tab5_camera_alloc_photo();
  bool ok = false;
  if (!photo) {
    printf("sem PSRAM para a foto\n");
  } else if (!tab5_camera_request_photo(photo)) {
    printf("nao consegui pedir a foto (outra ainda pendente?)\n");
  } else {
    bool pronta = false;
    for (int i = 0; i < 50 && !pronta; ++i) {    // 5 s: o first frame pode demorar
      vTaskDelay(pdMS_TO_TICKS(100));
      pronta = tab5_camera_photo_ready(&ok);
    }
    if (!pronta)  { printf("a camera nao entregou quadro em 5 s (confira o log)\n"); }
    else if (ok)  { ok = tab5_bmp_salvar("Console", "console", photo, TAB5_CAM_PHOTO_W, TAB5_CAM_PHOTO_H);
                    printf(ok ? "foto salva: /sdcard/apps/Console/console.bmp\n" : "a gravacao falhou (confira o log)\n"); }
    else          { printf("a captura falhou (confira o log)\n"); }
  }

  // SEMPRE released o interesse que este comando pediu - mesmo em erro. Se mais
  // alguem (o app Camera, por exemplo) ainda estiver usando, a contagem of
  // referencia mantem o sensor of pe; se este comando era o unico
  // interessado, ele desliga of verdade now.
  system_camera_stop();   // espera a tarefa of camera - so depois o buffer pode exit
  if (photo) { heap_caps_free(photo); }
}

// 'camera' - O QUE O ISP ESTA FAZENDO COM A COR. Imprime o estado of IPA
// itself (hal/tab5_camera_ipa.c: AWB gains, the calibration matrix and the
// which was on ISP, the green that a saturated white would have with each one) and measures one
// frame inteiro: media of centro, media das sombras, fracao of pixel
// estourado e of pixel magenta. Com a lente tampada, a media of centro e o
// black that the sensor delivers - the direct test of pedestal (tab5_camera.cpp,
// zerar_pedestal). 'camera reg' fala com o sensor enquanto o app Camera esta
// open, para experimentar one registrador on vivo.
static void cmd_camera(int argc, char** argv)
{
  if (argc >= 3 && strcmp(argv[1], "reg") == 0) {
    const long reg   = strtol(argv[2], nullptr, 16);
    const int  value = (argc >= 4) ? (int)strtol(argv[3], nullptr, 16) : -1;
    uint8_t read_value = 0;
    if (!tab5_camera_reg((uint16_t)reg, value, &read_value)) {
      printf("camera desligada (abra o app Camera) ou o SCCB falhou\n");
      return;
    }
    printf("sensor 0x%04lx = 0x%02x\n", reg, read_value);
    return;
  }

  const unsigned rev = efuse_hal_chip_revision();
  printf("P4 revisao v%u.%u - %s\n", rev / 100, rev % 100,
         rev >= 300 ? "ISP com WBG e BLC (balanco e preto no hardware)"
                    : "ISP sem WBG nem BLC (balanco na CCM, preto pelo sensor)");

  // Se o app Camera ja esta com ela, so observa: request of novo com outro
  // size would be rejected (one resolution at a time).
  const bool ja_enabled = tab5_camera_active();
  if (!ja_enabled) {
    printf("ligando a camera e esperando o AE/AWB assentarem...\n");
    if (!system_camera_start(240, 320)) { printf("falhou - confira o log\n"); return; }
    vTaskDelay(pdMS_TO_TICKS(2500));
  }

  tab5_cam_ipa_state_t e;
  if (!tab5_camera_ipa_state(&e)) {
    printf("o IPA proprio nao rodou nenhum quadro (fora do pipeline?)\n");
  } else {
    printf("quadros do ISP   : %lu\n", (unsigned long)e.frames);
    printf("exposicao, ganho : %lu us, %.2fx\n", (unsigned long)e.exposure_us, (double)e.sensor_gain);
    printf("ganhos do AWB    : R %.2f  B %.2f\n", (double)e.red_gain, (double)e.blue_gain);
    printf("CCM calibracao   |  CCM no ISP\n");
    for (int i = 0; i < 3; ++i) {
      printf("  %6.2f %6.2f %6.2f  |  %6.2f %6.2f %6.2f\n",
             (double)e.ccm_awb[i][0], (double)e.ccm_awb[i][1], (double)e.ccm_awb[i][2],
             (double)e.ccm_isp[i][0], (double)e.ccm_isp[i][1], (double)e.ccm_isp[i][2]);
    }
    printf("verde do branco saturado: %.2f -> %.2f (1 = branco; abaixo de ~0,9 sai rosa)\n",
           (double)e.white_green_before, (double)e.white_green_after);
    printf("saturacao        : %lu (128 = 1,0)\n", (unsigned long)e.saturation);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) { total += e.hist[i]; }
    if (total) {
      printf("histograma do Y  :");
      for (int i = 0; i < 16; ++i) { printf(" %lu", (unsigned long)(e.hist[i] * 100 / total)); }
      printf(" (%% por faixa de 16)\n");
    }
  }
  uint8_t preto = 0;
  if (tab5_camera_reg(0x3908, -1, &preto)) { printf("alvo do preto    : 0x%02x (fabrica 0x41)\n", preto); }

  uint16_t* photo = tab5_camera_alloc_photo();
  bool ok = false, pronta = false;
  if (photo && tab5_camera_request_photo(photo)) {
    for (int i = 0; i < 50 && !pronta; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); pronta = tab5_camera_photo_ready(&ok); }
  }
  if (pronta && ok) {
    const int L = TAB5_CAM_PHOTO_W, A = TAB5_CAM_PHOTO_H;
    uint64_t c[3] = { }, sombra[3] = { };
    uint32_t n_c = 0, n_sombra = 0, n = 0, estouro = 0, magenta = 0;
    for (int y = 0; y < A; y += 2) {
      for (int x = 0; x < L; x += 2) {
        const uint16_t p = photo[y * L + x];
        const int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        ++n;
        if (r >= 248 && g >= 248 && b >= 248) { ++estouro; }
        const int min_red_blue = r < b ? r : b;
        if (min_red_blue >= 200 && g + 40 < min_red_blue) { ++magenta; }
        if (r + g + b < 96) { sombra[0] += r; sombra[1] += g; sombra[2] += b; ++n_sombra; }
        if (abs(x - L / 2) < 100 && abs(y - A / 2) < 100) { c[0] += r; c[1] += g; c[2] += b; ++n_c; }
      }
    }
    printf("centro (200x200) : R %llu G %llu B %llu\n", c[0] / n_c, c[1] / n_c, c[2] / n_c);
    if (n_sombra) {
      printf("sombras (%lu%%)    : R %llu G %llu B %llu (neutro = tres iguais)\n",
             (unsigned long)(n_sombra * 100 / n), sombra[0] / n_sombra, sombra[1] / n_sombra, sombra[2] / n_sombra);
    }
    printf("estourado        : %.2f%% do quadro\n", estouro * 100.0 / n);
    printf("magenta no branco: %.2f%% do quadro\n", magenta * 100.0 / n);
  } else {
    printf("nao veio quadro para medir\n");
  }

  if (!ja_enabled) { system_camera_stop(); }   // espera a tarefa: so depois o buffer exit
  if (photo) { heap_caps_free(photo); }
}

// ===========================================================================
// THE COMMANDS, IN TABLE
// ===========================================================================
// Each command is a line of COMMANDS table (more below): name, group, a
// phrase of summary, as formas of chamar e one paragrafo of para que serve. O
// 'help' is generated from it - and therefore it cannot be outdated: command
// that is not in the table does not run, and that which is shown in help.
//
// The previous version was one if/else of 40 branches and one text of help written to
// part. The two walked separately: command without line in help, line in group
// errado, subcomando que so existia no fonte.

static void show_usage(const char* name);   // after table

static bool equals_ignore_case(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// argv[of..] in a single sentence, with one space between the words.
static void juntar(char* out, size_t n, int argc, char** argv, int of)
{
  out[0] = 0;
  for (int k = of; k < argc; ++k) {
    strncat(out, argv[k], n - strlen(out) - 1);
    if (k + 1 < argc) { strncat(out, " ", n - strlen(out) - 1); }
  }
}

// ---------------------------------------------------------------------------
// SISTEMA
// ---------------------------------------------------------------------------

// One line per system service, in the same place. The first question when
// algo estranho acontece e "o que esta of pe now?" - e sem isto a resposta
// exigia cacar line of log of tres tarefas diferentes.
static void c_status(int, char**)
{
  const esp_app_desc_t* app = esp_app_get_description();
  const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
  const char* open = system_app_current();
  printf("--- M5tabOS %s ---\n", app->version);
  printf("  ligado ha: %luh%02lum%02lus   app aberto: %s%s\n",
         (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60),
         open ? open : "nenhum (tela inicial)",
         tab5_power_is_sleeping() ? "   (em repouso)" : "");
  printf("  memoria  : interna %u KB livre (minimo %u KB), PSRAM %u KB livre\n",
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

  printf("  cartao   : %s\n", tab5_sd_description());

  const char* est = tab5_wifi_description();
  const int r = (int)tab5_wifi_rssi();
  if (tab5_wifi_state() == TAB5_WIFI_CONNECTED) {
    char sinal[12];
    if (r == TAB5_WIFI_RSSI_UNKNOWN) { snprintf(sinal, sizeof(sinal), "sinal --"); }
    else { snprintf(sinal, sizeof(sinal), "%ddBm", r); }
    printf("  wifi     : %s  %s  %s\n", est, tab5_wifi_ip(), sinal);
  } else {
    printf("  wifi     : %s\n", est);
  }
  printf("  bluetooth: %s\n", tab5_ble_description());

  printf("  teclado  : %s%s\n",
         settings_keyboard() ? "" : "desativado em Ajustes; ",
         tab5_keyboard_present() ? "encaixado" : "ausente");

  printf("  grove 5V : %s   detectado: %s\n",
         settings_grove() ? "ligado" : "desligado", grove_name(tab5_grove_type()));

  if (tab5_mesh_meu_numero() != 0 || tab5_mesh_no_ar()) {
    uint32_t q, pk, tel, ls, tx, ni; int var;
    tab5_mesh_contadores(&q, &pk, &tel, &ls, &tx, &ni, &var);
    printf("  mesh     : %s, %d nos, %lu quadros, %lu pacotes\n",
           tab5_mesh_no_ar() ? "no ar" : "mudo",
           tab5_mesh_num_nos(), (unsigned long)q, (unsigned long)pk);
  } else {
    printf("  mesh     : nao iniciado\n");
  }

  int in_ram; uint32_t grav, failures;
  registro_estado(&in_ram, &grav, &failures);
  printf("  registro : %d na RAM, %lu gravadas, %lu falhas\n",
         in_ram, (unsigned long)grav, (unsigned long)failures);

  {
    size_t pend; uint32_t lgrav, lperd; char arq[24];
    tab5_log_estado(&pend, &lgrav, &lperd, arq, sizeof(arq));
    printf("  log      : %u byte(s) na RAM, %lu linha(s) gravadas, %lu perdidas (%s)\n",
           (unsigned)pend, (unsigned long)lgrav, (unsigned long)lperd,
           arq[0] ? arq : "ainda sem descarga");
  }

  tab5_measurement_t m = tab5_power_latest();
  printf("  energia  : %dMHz %.2fV %.1fmA %d%%\n",
         tab5_power_cpu_mhz(), m.volts, m.mA, m.pct);
}

static void c_tasks(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "memoria")) { cmd_memoria(); }   // forma antiga
  else                                      { cmd_tasks(); }
}

static void c_memory(int argc, char** argv)
{
  const char* o = (argc >= 2) ? argv[1] : "";
  if (!*o) {
    cmd_memoria();
    printf("\n  'mem extrato' e 'mem mapa' mostram para onde a RAM interna foi\n");
  }
  else if (equals_ignore_case(o, "extrato")) { cmd_memory_extrato(); }
  else if (equals_ignore_case(o, "mapa"))    { cmd_memory_mapa(); }
  else if (equals_ignore_case(o, "marca"))   { cmd_memory_mark(); }
  else if (equals_ignore_case(o, "delta"))   { cmd_memory_delta(); }
  else if (equals_ignore_case(o, "tudo"))    { cmd_memoria(); printf("\n"); cmd_memory_extrato(); printf("\n"); cmd_memory_mapa(); }
  else                       { show_usage("mem"); }
}

static void c_power(int argc, char** argv)
{
  if (argc < 2)                   { cmd_power(); }
  else if (equals_ignore_case(argv[1], "travas"))  { cmd_travas(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (equals_ignore_case(argv[1], "repouso")) { cmd_sleep_forcar(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (equals_ignore_case(argv[1], "teto"))    { cmd_teto_cpu(argc >= 3 ? atoi(argv[2]) : 0); }
  else                             { show_usage("energia"); }
}

// As partes of fundo dos apps (kernel/background.h). A parada requested daqui
// happens in interface (fundo_manter), like every stop.
static void c_fundo(int argc, char** argv)
{
  if (argc >= 3 && equals_ignore_case(argv[1], "parar")) {
    printf(system_fundo_stop_name(argv[2]) ? "pedido de parada enviado para '%s'\n"
                                             : "'%s' nao esta rodando\n", argv[2]);
    return;
  }
  if (argc >= 2) { show_usage("fundo"); return; }
  background_info_t l[FUNDO_MAX];
  const int n = system_fundo_listar(l, FUNDO_MAX);
  if (n == 0) { printf("nenhuma parte de fundo rodando\n"); }
  for (int i = 0; i < n; ++i) {
    printf("  %-12s ha %5lus  %s\n", l[i].name, (unsigned long)l[i].ha_s, l[i].summary);
  }
}

// Returns (or removes) the noise of a tag at execution time. The system
// silencia "eh_sdio" e "sdmmc_io" no boot - eles inundam o console when o
// enlace com o C6 morre. Silenciar sem deixar como reverter seria hide
// informacao; com isto, o cru esta a one comando of distancia.
static const char* const LOG_LEVEL_NAMES[6] = { "nada", "erro", "aviso", "info", "debug", "verboso" };

static int level_log(const char* s)
{
  for (int i = 0; i < 6; ++i) { if (equals_ignore_case(s, LOG_LEVEL_NAMES[i])) { return i; } }
  char* end = nullptr;
  const long n = strtol(s, &end, 10);
  return (*s && end && *end == 0 && n >= 0 && n <= 5) ? (int)n : -1;
}

static void c_log(int argc, char** argv)
{
  if (argc < 2) { show_usage("log"); return; }
  if (argc < 3) {
    const int n = (int)esp_log_level_get(argv[1]);
    printf("'%s' esta em %s (%d)\n", argv[1], (n >= 0 && n <= 5) ? LOG_LEVEL_NAMES[n] : "?", n);
    return;
  }
  const int n = level_log(argv[2]);
  if (n < 0) {
    printf("nivel '%s' nao existe - use nada|erro|aviso|info|debug|verboso (ou 0-5)\n", argv[2]);
    return;
  }
  esp_log_level_set(argv[1], (esp_log_level_t)n);
  printf("'%s' agora em %s (%d)\n", argv[1], LOG_LEVEL_NAMES[n], n);
}

// BANCADA DO PENDRIVE: enable e return sem touch in screen. Ligar tira o
// console of USB-C (the computer now sees the disk, not the serial port); he
// iteration when the computer ejects. What happened stays in the log of card.
static void c_pendrive(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "ligar")) {
    printf(tab5_pendrive_start() ? "ligando - o console sai do USB-C ate o computador ejetar\n"
                                   : "nao ligou (ja havia sessao, ou recusado - veja 'pendrive')\n");
    return;
  }
  if (argc >= 2 && equals_ignore_case(argv[1], "devolver")) { tab5_pendrive_return(); printf("devolvendo\n"); return; }
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  static const char* const FASE[] = { "parado", "preparando", "ativo", "devolvendo" };
  printf("fase %s, motivo %d, ultima por ejecao %s, sem ejetar %s, oferta %s, pc %s\n",
         FASE[e.fase & 3], (int)e.reason, e.por_ejecao ? "sim" : "nao", e.sem_ejetar ? "sim" : "nao",
         e.oferta ? "sim" : "nao", tab5_pendrive_pc_presente() ? "sim" : "nao");
}

// ---------------------------------------------------------------------------
// TELA
// ---------------------------------------------------------------------------

// DIAGNOSTICO DA PISCADA AZUL, e interruptores para comparar sem regravar.
//
// WORD ACKNOWLEDGED OR NOTHING DONE - and this rule cost a medication. One
// previous version was "if it's not 'on', turn it off": any second word that
// not married, one would become prioritize_tela(false) SILENT, and a measurement exited
// com a priority em "default" sem ninguem ter request. Comando of diagnostics
// cannot have a destructive path by default.
// BENCH: the screen as it is now, in a .bmp of card (apps/Console).
// readRect devolve o RGB565 in order of LovyanGFX (bytes trocados); o filtro
// undo in copy that goes to the file.
static void bmp_desinverter(uint16_t* l, int n) { for (int i = 0; i < n; ++i) { l[i] = (uint16_t)((l[i] << 8) | (l[i] >> 8)); } }
static void screen_photo(const char* name)
{
  const int w = M5.Display.width(), h = M5.Display.height();
  uint16_t* px = (uint16_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
  if (!px) { printf("sem memoria para %dx%d\n", w, h); return; }
  M5.Display.readRect(0, 0, w, h, px);
  const bool ok = tab5_bmp_salvar("Console", name, px, w, h, bmp_desinverter);
  heap_caps_free(px);
  printf(ok ? "gravada: /sdcard/apps/Console/%s.bmp (%dx%d)\n" : "falhou ao gravar %s.bmp\n", name, w, h);
}

// BANCADA: click e open app pela serial (ver sistema_simular_toque).
static void c_simulate_touch(int argc, char** argv)
{
  if (argc < 3) { show_usage("tocar"); return; }
  system_simulate_touch(atoi(argv[1]), atoi(argv[2]));
  printf("toque em %s,%s\n", argv[1], argv[2]);
}

static void c_open(int argc, char** argv)
{
  if (argc < 2) {
    for (int i = 0; i < SISTEMA_NUM_APPS; ++i) { printf("  %s\n", SISTEMA_APPS[i]->name); }
    return;
  }
  if (equals_ignore_case(argv[1], "home")) { system_simulate_open(nullptr); printf("voltando para a home\n"); return; }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->name, argv[1]) == 0) {
      system_simulate_open(SISTEMA_APPS[i]);
      printf("abrindo %s\n", SISTEMA_APPS[i]->name);
      return;
    }
  }
  printf("nao ha app '%s' - 'abrir' lista\n", argv[1]);
}

// ORDEM DOS ICONES NA HOME - a mesma que hold e drag muda in screen.
static const app_t* app_por_name(const char* name)
{
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->name, name) == 0) { return SISTEMA_APPS[i]; }
  }
  return nullptr;
}

static void c_order(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "padrao")) {
    system_home_order_default();
    printf("ordem de fabrica (apps.cpp) restaurada\n");
  } else if (argc >= 3) {
    const app_t* app = app_por_name(argv[1]);
    const int pos = atoi(argv[2]);
    if (!app) { printf("nao ha app '%s' - 'ordem' lista\n", argv[1]); return; }
    if (pos < 1 || pos > SISTEMA_NUM_APPS) { printf("posicao de 1 a %d\n", SISTEMA_NUM_APPS); return; }
    system_home_move(app, pos - 1);
    printf("'%s' agora na posicao %d\n", app->name, pos);
  } else if (argc == 2) {
    show_usage("ordem");
    return;
  }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const app_t* app = system_home_app(i);
    if (app) { printf("  %2d  %s\n", i + 1, app->name); }
  }
}

static void c_screen(int argc, char** argv)
{
  if (argc >= 2) {
    const char* o = argv[1];
    if (equals_ignore_case(o, "foto")) { screen_photo(argc >= 3 ? argv[2] : "tela"); return; }
    if (equals_ignore_case(o, "medir")) { screen_measure_reset(); return; }
    if (equals_ignore_case(o, "banco")) { tab5_frame_benchmark(); return; }
    if (equals_ignore_case(o, "rolar")) {
      // Scrolling by the DMA (of front page) or by the CPU (inside of
      // tras). Rastro que some com 'cpu' esta no caminho of DMA.
      extern bool g_screen_rolar_cpu;
      if (argc >= 3 && equals_ignore_case(argv[2], "cpu"))      { g_screen_rolar_cpu = true; }
      else if (argc >= 3 && equals_ignore_case(argv[2], "dma")) { g_screen_rolar_cpu = false; }
      else { printf("uso: tela rolar cpu|dma\n"); }
      printf("rolagem pela %s\n", g_screen_rolar_cpu ? "CPU" : "DMA (da frente)");
      return;
    }
    if (equals_ignore_case(o, "relogio")) {
      // CHANGES THE RUNNING PIXEL CLOCK. And the only adjustment that reduces
      // demanda of panel em vez of disputar melhor - ver tab5_power.cpp.
      if (argc < 3) { printf("uso: tela relogio <mhz>   (ex.: tela relogio 60)\n"); return; }
      const int mhz = atoi(argv[2]);
      const bool ok = system_switch_clock_panel(mhz);   // in tarefa of interface
      // O panel remontado nasce com framebuffer em BRANCO (o
      // esp_lcd_new_panel_dpi aloca com calloc) - sem o redraw a screen
      // ficava preta com o backlight aceso. GRAVA SO DEPOIS of aceito: se
      // gravasse antes e o panel recusasse, o next boot tentaria of novo o
      // mesmo valor ruim.
      if (ok) {
        system_request_redraw_complete();
        settings_set_panel_clock_mhz((uint8_t)mhz);
      }
      printf(ok ? "painel remontado e gravado - vale no proximo boot tambem\n"
                : "nao subiu nesse relogio - voltei ao anterior\n");
    }
    else if (equals_ignore_case(o, "prioridade") && argc >= 3 && (equals_ignore_case(argv[2], "alta") || equals_ignore_case(argv[2], "on"))) {
      tab5_bus_prioritize_screen(true);
    }
    else if (equals_ignore_case(o, "prioridade") && argc >= 3 && (equals_ignore_case(argv[2], "padrao") || equals_ignore_case(argv[2], "off"))) {
      tab5_bus_prioritize_screen(false);
    }
    // Formas antigas, mantidas: 'screen on' / 'screen off'.
    else if (equals_ignore_case(o, "on")  || equals_ignore_case(o, "sim")) { tab5_bus_prioritize_screen(true); }
    else if (equals_ignore_case(o, "off") || equals_ignore_case(o, "nao")) { tab5_bus_prioritize_screen(false); }
    else {
      printf("nao conheco 'tela %s' - nada foi alterado.\n", o);
      show_usage("tela");
      return;
    }
  }
  printf("--- tela ---\n");
  printf("  prioridade da tela no AXI : %s\n",
         tab5_bus_is_prioritized() ? "ALTA" : "padrao (como vem de fabrica)");
  printf("  limpezas dispensadas      : %lu\n",
         (unsigned long)tab5_screen_avoided_clears());
  {
    // Os detectores of hal/tab5_frame. Sem 'pior': read zera, e quem
    // acompanha o pior e o monitor, que vira evento.
    uint32_t period_us = 0;
    const uint32_t atrasos = tab5_frame_delays(nullptr, &period_us);
    const uint32_t erros   = tab5_frame_dsi_errors(nullptr, nullptr);
    printf("  quadros varridos          : %lu (periodo %lu.%lu ms)\n",
           (unsigned long)tab5_frame_count(),
           (unsigned long)(period_us / 1000), (unsigned long)(period_us % 1000 / 100));
    printf("  underrun / atrasados / DSI: %lu / %lu / %lu\n",
           (unsigned long)tab5_frame_underruns(), (unsigned long)atrasos, (unsigned long)erros);
    int nucleo = -1; char t0[16], t1[16];
    if (tab5_frame_delay_source(&nucleo, t0, t1)) {
      printf("  ultimo atraso             : ISR no nucleo %d; rodando c0=%s c1=%s\n", nucleo, t0, t1);
    }
  }

  // THE ACCOUNT ON THE SCREEN, and not just in the comment of source. These are the numbers that
  // dizem se mexer no clock valeu: MB/s e o que o panel tira of PSRAM sem
  // stop, e e contra esse valor que todo o resto of sistema disputa.
  const int mhz = tab5_power_dpi_clock();
  printf("  tela                      : %s%s\n", tab5_panel_name(),
         tab5_panel_touch_gt911() ? "" : " (relogio fixo, repouso so de luz)");
  if (mhz > 0) {
    const double fps  = tab5_power_frames_per_second();
    printf("  relogio do painel         : %d MHz\n", mhz);
    printf("  taxa de quadros           : %.1f Hz\n", fps);
    printf("  leitura continua da PSRAM : %.1f MB/s\n", fps * 720.0 * 1280.0 * 2.0 / 1e6);
  }

  screen_measure_show();
  printf("\n  underrun > 0 = a piscada azul. 'help tela' lista as medidas e ajustes.\n");
}

// BACKLIGHT TEST, without recording: find the lowest level that still lights up and see
// se a frequencia of PWM muda o piso. Driver of LED com time minimum of enabled
// does not obey small duty at high frequency (at 44.1 kHz the period is
// 22,7 us) - abaixo of certo ponto o brilho para of cair.
static void c_brightness(int argc, char** argv)
{
  if (argc < 2) {
    printf("brilho: %d (gravado %d)\n", (int)M5.Display.getBrightness(), (int)settings_brightness());
    return;
  }
  int b = atoi(argv[1]);
  if (b < 1)   { b = 1; }
  if (b > 255) { b = 255; }
  if (argc >= 3) {
    const uint32_t hz = (uint32_t)atoi(argv[2]);
    // Canal 7 of M5GFX no Tab5 -> timer (7 >> 1) & 3 = 3, modo lento.
    const esp_err_t e = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3, hz);
    printf("PWM do backlight: %lu Hz (%s)\n", (unsigned long)hz, esp_err_to_name(e));
  }
  M5.Display.setBrightness((uint8_t)b);
  printf("brilho %d = duty %.2f%% (nao gravado; Ajustes ou reiniciar voltam ao gravado)\n",
         b, (float)(((b * 257 + 64) >> 7)) * 100.0f / 512.0f);
}

// Opening an app does several things at once; each option makes ONLY ONE, writing down the
// time, to look at the screen and see which one blinks.
static void c_piscada(int argc, char** argv)
{
  const char* o = (argc >= 2) ? argv[1] : "";
  const unsigned long t = (unsigned long)(esp_timer_get_time() / 1000);
  if (equals_ignore_case(o, "brilho")) {
    printf("piscada: %lums - reescrevendo o MESMO brilho (%d)\n", t, (int)M5.Display.getBrightness());
    M5.Display.setBrightness(M5.Display.getBrightness());
  } else if (equals_ignore_case(o, "c6")) {
    printf("piscada: %lums - reescrevendo WLAN_PWR_EN=0 no expansor\n", t);
    tab5_power_radio(false);
  } else if (equals_ignore_case(o, "perfil")) {
    printf("piscada: %lums - reaplicando o perfil inteiro (trilhos + brilho + pm)\n", t);
    system_reapply_profile();
  } else if (equals_ignore_case(o, "pm")) {
    esp_pm_config_t c = { };
    esp_pm_get_configuration(&c);
    printf("piscada: %lums - esp_pm_configure(%d..%d MHz) de novo\n", t, c.min_freq_mhz, c.max_freq_mhz);
    esp_pm_configure(&c);
  } else if (equals_ignore_case(o, "grove")) {
    printf("piscada: %lums - 5V do Grove OFF, 1s, ON (o C6L reinicia)\n", t);
    M5.Power.setExtOutput(false, m5::ext_PA);
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("piscada: %lums - 5V do Grove ON agora\n", (unsigned long)(esp_timer_get_time() / 1000));
    M5.Power.setExtOutput(true, m5::ext_PA);
  } else if (equals_ignore_case(o, "quadro")) {
    printf("piscada: %lums - redesenho completo pelo compositor\n", t);
    virtual_screen_invalidate_everything();
    system_request_redraw_complete();
  } else {
    show_usage("piscada");
  }
}

// Turns diagnostics on/off. Don't read the panel here: I2C is a task
// interface, and two tasks on the same bus read the swapped register without
// give error. Here you only activate the flag; who prints and who measures.
static void c_touch(int argc, char** argv)
{
  const bool enable = !(argc >= 2 && equals_ignore_case(argv[1], "off"));
  system_diag_touch(enable);
  printf(enable ? "toque cada botao e veja onde o dedo cai.\n"
                 "compare o 'app (x,y)' com a posicao esperada do botao.\n"
                 "para desligar: toque off\n"
               : "diagnostico de toque desligado\n");
}

// Onde a interface esta AGORA - funciona com a screen congelada: o console e
// another task. Rotate twice: 'turns' stopped = loop stopped at this step.
static void c_ui(int, char**)
{
  system_diag_t d;
  system_diag_interface(&d);
  printf("interface: %s\n  voltas %lu, ultima ha %lu ms, %s%s\n  dedo lido ha %ld ms\n",
         d.etapa, (unsigned long)d.voltas, (unsigned long)d.ms_since_iteration,
         d.app ? d.app : "home", d.sleep ? " (repouso)" : "",
         d.ms_since_finger == UINT32_MAX ? -1L : (long)d.ms_since_finger);
  printf("toque: %lu pulsos, %lu falhas de I2C, %lu presos, %lu s surdos, status GT911 %02X\n",
         (unsigned long)d.touch_pulsos, (unsigned long)d.touch_falhas, (unsigned long)d.touch_presos,
         (unsigned long)d.touch_surdos, (unsigned)d.touch_status);
}

// ---------------------------------------------------------------------------
// REDE
// ---------------------------------------------------------------------------

static void c_wifi(int argc, char** argv)
{
  if (argc < 2) {
    printf("wifi     : %s\n", tab5_wifi_description());
    if (tab5_wifi_state() == TAB5_WIFI_CONNECTED) {
      const int r = (int)tab5_wifi_rssi();
      printf("rede     : %s   IP %s   ", tab5_wifi_ssid(), tab5_wifi_ip());
      if (r == TAB5_WIFI_RSSI_UNKNOWN) { printf("sinal --\n"); }
      else                                   { printf("sinal %d dBm\n", r); }
    }
    printf("guardadas: %d rede(s) no wifi.txt%s\n", tab5_wifi_known_count(),
           tab5_wifi_radio_only() ? "   (radio ligado so para ESP-NOW)" : "");
    printf("\n'help wifi' mostra como varrer, gravar, conectar e esquecer redes\n");
    return;
  }
  const char* o = argv[1];
  if (equals_ignore_case(o, "redes")) {
    // Requires the stack of pe: without esp_hosted on the air there is no radio to request.
    if (!tab5_wifi_stack_ready()) {
      printf("o WiFi ainda nao foi ligado neste boot - ligue em Ajustes e tente de novo\n");
      return;
    }
    tab5_wifi_scan_console();
    return;
  }
  if (equals_ignore_case(o, "conectar")) {
    if (argc < 3) { show_usage("wifi"); return; }
    if (!tab5_wifi_is_known(argv[2])) {
      printf("'%s' nao esta guardada - use: wifi gravar \"%s\" <senha>\n", argv[2], argv[2]);
      return;
    }
    tab5_wifi_connect(argv[2]);
    printf("trocando para '%s' - confira com 'wifi' em alguns segundos\n", argv[2]);
    return;
  }
  if (equals_ignore_case(o, "esquecer")) {
    if (argc < 3) { show_usage("wifi"); return; }
    const char* ssid = argv[2];
    with_card([&]() -> int {
      const bool ok = tab5_wifi_forget(ssid);
      printf(ok ? "'%s' esquecida\n" : "'%s' nao estava guardada\n", ssid);
      return ok ? 0 : 1;
    });
    return;
  }
  if (equals_ignore_case(o, "gravar")) {
    if (argc < 3) { show_usage("wifi"); return; }
    wifi_save_credentials(argv[2], argc >= 4 ? argv[3] : nullptr);
    return;
  }
  // Forma curta, a original: 'wifi <ssid> [senha]'.
  wifi_save_credentials(argv[1], argc >= 3 ? argv[2] : nullptr);
}

// Atalho antigo: 'redes' = 'wifi redes'.
static void c_redes(int, char**)
{
  char a0[] = "wifi", a1[] = "redes";
  char* a[] = { a0, a1 };
  c_wifi(2, a);
}

// BLUETOOTH: estado of stack, of enlace of Mesh e quem foi visto.
static void c_ble(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "varrer")) {
    printf(tab5_ble_scan(6000) ? "varrendo por 6 s... rode 'ble' depois\n"
                                 : "nao deu para varrer agora (%s)\n", tab5_ble_description());
    return;
  }
  if (argc >= 2 && equals_ignore_case(argv[1], "desligar")) {
    system_bluetooth(false);
    printf("desligando - confira com 'ble' em alguns segundos\n");
    return;
  }
  if (argc >= 2 && equals_ignore_case(argv[1], "ligar")) {
    // Ligar com o C6 disabled pede o trilho (I2C), que so a interface fala.
    if (!tab5_wifi_stack_ready()) { printf("o C6 ainda nao subiu neste boot - ligue em Ajustes\n"); return; }
    system_bluetooth(true);
    printf("ligando - confira com 'ble' em alguns segundos\n");
    return;
  }
  if (argc >= 2) { show_usage("ble"); return; }
  printf("bluetooth: %s\n", tab5_ble_description());
  printf("mesh ble:  %s%s\n", mesh_ble_descricao(), mesh_ble_esperando_pin() ? " (esperando PIN)" : "");
  const int n = tab5_ble_seen_count();
  if (n == 0) { printf("nenhum aparelho visto ainda - 'ble varrer' procura por 6 s\n"); }
  tab5_ble_seen_t v;
  for (int i = 0; i < n; ++i) {
    yield(i);
    if (!tab5_ble_get_seen(i, &v)) { continue; }
    char end[18];
    tab5_ble_address_text(v.addr, end);
    printf("  %s  %4d dBm  %-22s%s\n", end, (int)v.rssi, v.name[0] ? v.name : "(sem nome)",
           v.meshtastic ? "  MESHTASTIC" : "");
  }
}

// The real state of ESP-NOW and of radio - without depending on screen. The channel and
// asked C6 now (not what the service thinks), which is what decides whether
// two devices hear each other.
static void c_espnow(int argc, char** argv)
{
  if (argc >= 3 && equals_ignore_case(argv[1], "canal")) {
    const int c = atoi(argv[2]);
    printf(tab5_espnow_set_canal((uint8_t)c) ? "pedido: canal %d\n" : "recusado (canal %d)\n", c);
    return;
  }
  if (argc >= 3 && equals_ignore_case(argv[1], "enviar")) {
    // The entire sentence: before only the first word went, the rest disappeared silently.
    char msg[200];
    juntar(msg, sizeof(msg), argc, argv, 2);
    printf(tab5_espnow_send(msg) ? "na fila: '%s'\n" : "recusado: '%s'\n", msg);
    return;
  }
  if (argc >= 2) { show_usage("espnow"); return; }
  uint8_t mac[6]; tab5_espnow_meu_mac(mac);
  uint8_t c = 0; wifi_second_chan_t sec;
  const bool leu = tab5_wifi_stack_ready() && esp_wifi_get_channel(&c, &sec) == ESP_OK;
  printf("C6 firmware nosso: %s   enlace: %s   ESP-NOW no ar: %s\n",
         tab5_espnow_c6_compativel() ? "sim" : "NAO", tab5_wifi_c6_link() ? "sim" : "NAO",
         tab5_espnow_pronto() ? "sim" : "NAO");
  printf("WiFi: %s  (so radio: %s)\n", tab5_wifi_description(), tab5_wifi_radio_only() ? "sim" : "nao");
  printf("canal no C6 agora: %s%u   pedido: %s%u\n", leu ? "" : "? ", (unsigned)c,
         tab5_espnow_canal_fixo() ? "fixo " : "roteador ", (unsigned)tab5_espnow_canal_fixo());
  printf("meu MAC: %02X:%02X:%02X:%02X:%02X:%02X   msgs: %d   ouvidos: %d\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tab5_espnow_n(), tab5_espnow_vistos());
}

// RECORDING THE C6 THROUGH THE LINK IS OFF. OTA via SDIO itself does not have
// iteration when the new image doesn't speak - that's what left the C6 silent. With the
// M5Stack ESP32 Downloader, gravar e restaurar e FISICO e sempre tem iteration. As
// old functions (cmd_c6_ota/cmd_c6_gravar) are in the history of git.
// ---------------------------------------------------------------------------
// GRAVAR O C6 PELO ENLACE (OTA pelo SDIO)
// ---------------------------------------------------------------------------
// It only exists with the firmware of C6 of new table (bootloader with rollback, two
// slots of 1,875 MB, GUARDA DE IMAGEM NOVA no c6/main/app_main.c). O caminho:
//
//   1. le apps/Sistema/tab5_c6.bin INTEIRO para a PSRAM e confere antes of
//      touch no C6: e imagem of IDF, e para ESP32-C6, e e o projeto tab5_c6;
//   2. begin (deletes the FREE slot) -> write in pieces of 1536 -> end (the C6
//      valida a imagem inteira) -> activate (troca o boot; reinicia em 2 s).
//      Erro antes of activate: nada changed, o firmware current segue running;
//   3. o P4 se reinicia 4 s depois. No boot ele reseta o C6 (2o boot of
//      imagem nova - a guarda conta e tolera) e manda o VIVO, que a confirma.
//
// If the VIVO does not come (link does not close, image gets stuck), the C6 itself
// iteration para a imagem anterior: 120 s, ou no 5o boot sem confirmacao.
#define C6_FW_APP     "Sistema"
#define C6_FW_FILE "tab5_c6.bin"
#define C6_PEDACO     1536          // EH_RPC_OTA_CHUNK_MAX
#define C6_FW_MAX     0x1E0000      // slot of c6/partitions.csv

static void cmd_c6_flash(bool confirmado)
{
  if (!tab5_wifi_c6_link()) {
    printf("o enlace com o C6 nao esta de pe - ligue o WiFi primeiro.\n");
    return;
  }
  if (!confirmado) {
    printf("--- gravar o C6 pelo enlace ---\n"
           "1. mande o arquivo para o cartao (do Mac, monitor fechado):\n"
           "     receber %s %s <bytes>\n"
           "2. c6 gravar sim\n"
           "\n"
           "O C6 reinicia e o P4 reinicia logo depois, sozinho. Se a imagem\n"
           "nova nao for confirmada, o C6 volta para a anterior sem ajuda.\n"
           "NAO desligue o aparelho ate o P4 voltar.\n",
           C6_FW_APP, C6_FW_FILE);
    return;
  }

  // RPC alive? Without it, each OTA request would wait for the team to run out.
  eh_host_coprocessor_fwver_t v = { };
  if (eh_host_sys_get_cp_fw_version(&v) != ESP_OK) {
    printf("o C6 aceita o enlace mas nao responde a RPC - so pelo Downloader.\n");
    return;
  }

  if (!tab5_sd_mount()) { printf("cartao nao montou (%s)\n", tab5_sd_description()); return; }
  FILE* f = tab5_sd_open(C6_FW_APP, C6_FW_FILE, "rb");
  if (!f) { printf("nao achei apps/%s/%s\n", C6_FW_APP, C6_FW_FILE); tab5_sd_unmount(); return; }
  fseek(f, 0, SEEK_END);
  const long total = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* img = (total > 0 && total <= C6_FW_MAX)
                   ? (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM) : nullptr;
  const size_t lidos = img ? fread(img, 1, (size_t)total, f) : 0;
  fclose(f);
  tab5_sd_unmount();
  if (!img || lidos != (size_t)total) {
    printf("tamanho %ld invalido (max %u) ou leitura curta (%u)\n", total, (unsigned)C6_FW_MAX, (unsigned)lidos);
    heap_caps_free(img);
    return;
  }

  // CONFERENCIA ANTES DE TOCAR NO C6. Cabecalho of IDF: 0xE9 no byte 0,
  // chip_id no 12 (0x000D = ESP32-C6). Logo depois of cabecalho (24 B) e of
  // 1st segment (8 B) comes esp_app_desc_t: magic 0xABCD5432, version no +16,
  // name of projeto no +48.
  const uint32_t magic_desc = (uint32_t)img[32] | ((uint32_t)img[33] << 8) |
                              ((uint32_t)img[34] << 16) | ((uint32_t)img[35] << 24);
  char version[33] = { 0 }, projeto[33] = { 0 };
  if (total > 112) { memcpy(version, img + 48, 32); memcpy(projeto, img + 80, 32); }
  const unsigned chip = (unsigned)img[12] | ((unsigned)img[13] << 8);
  if (total <= 112 || img[0] != 0xE9 || chip != 0x000D || magic_desc != 0xABCD5432u
      || strcmp(projeto, "tab5_c6") != 0) {
    printf("%s nao e firmware do C6 deste projeto (magic 0x%02X, chip %u, projeto '%s')\n",
           C6_FW_FILE, img[0], chip, projeto);
    heap_caps_free(img);
    return;
  }
  printf("imagem %s, %ld bytes - gravando no slot livre do C6\n", version, total);

  esp_err_t r = eh_host_cp_ota_begin();
  if (r != ESP_OK) {
    printf("begin falhou: %s - nada mudou\n", esp_err_to_name(r));
    heap_caps_free(img);
    return;
  }
  int pct_ant = -1;
  for (long written = 0; written < total; ) {
    const uint32_t n = (uint32_t)((total - written) > C6_PEDACO ? C6_PEDACO : (total - written));
    r = eh_host_cp_ota_write(img + written, n);
    if (r != ESP_OK) {
      printf("\nwrite falhou em %ld/%ld: %s - ABORTADO, o firmware atual segue\n",
             written, total, esp_err_to_name(r));
      heap_caps_free(img);
      return;
    }
    written += n;
    const int pct = (int)(written * 100 / total);
    if (pct / 10 != pct_ant) { printf("  %d%%\n", pct); pct_ant = pct / 10; }
    yield((int)(written / C6_PEDACO));
  }
  heap_caps_free(img);

  r = eh_host_cp_ota_end();
  if (r != ESP_OK) {
    printf("end falhou: %s - o C6 recusou a imagem, nada mudou\n", esp_err_to_name(r));
    return;
  }
  r = eh_host_cp_ota_activate();
  if (r != ESP_OK) {
    printf("activate falhou: %s - a imagem esta no slot mas o boot nao mudou\n", esp_err_to_name(r));
    return;
  }
  printf("gravado e ativado. O C6 reinicia em 2 s; o P4 reinicia em 4 s para\n"
         "confirmar a imagem nova. Depois: 'farejo info'.\n");
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(4000));
  esp_restart();
}

static void c_c6(int argc, char** argv)
{
  if (argc >= 2 && (equals_ignore_case(argv[1], "ota") || equals_ignore_case(argv[1], "gravar"))) {
    cmd_c6_flash(argc >= 3 && equals_ignore_case(argv[2], "sim"));
    return;
  }
  cmd_c6();
}

// ---------------------------------------------------------------------------
// FAREJO (promiscuous mode on C6) - and the window for the radio capabilities that
// the recording of C6 brought. 'info' shows what this C6 can do (CSI, FTM,
// 802.15.4): o bit so acende se o modulo foi compilado no firmware gravado.
// ---------------------------------------------------------------------------
static void show_sniffer_status(void)
{
  printf("C6 sabe farejar: %s   farejo ativo: %s\n",
         tab5_farejo_c6_suporta() ? "sim" : "NAO (firmware v<3 ou sem a flag)",
         tab5_farejo_active() ? "sim" : "nao");
  const int n = tab5_farejo_n();
  printf("MACs ouvidos: %d   lotes: %lu   perdidos no C6: %lu\n",
         n, (unsigned long)tab5_farejo_lotes(), (unsigned long)tab5_farejo_perdidos());
  for (int i = 0; i < n && i < 20; ++i) {
    farejo_item_t it;
    if (!tab5_farejo_item(i, &it)) { continue; }
    yield(i);
    printf("  %02X:%02X:%02X:%02X:%02X:%02X  ch%-2u  rssi %4d (max %4d)  tipos 0x%02X  quadros %lu\n",
           it.mac[0], it.mac[1], it.mac[2], it.mac[3], it.mac[4], it.mac[5],
           it.channel, it.rssi_ult, it.rssi_max, it.tipos, (unsigned long)it.frames);
  }
  if (n > 20) { printf("  ... e mais %d\n", n - 20); }
}

static void show_sniffer_info(void)
{
  c6_info_t in;
  if (!tab5_farejo_info_c6(&in, 1500)) {
    printf("o C6 nao respondeu (sem enlace, firmware de fabrica, ou v<3).\n");
    return;
  }
  printf("C6 contrato v%u   canal %u   farejando: %s", in.proto, in.channel, in.farejando ? "sim" : "nao");
  if (in.chip_rev & 0x80) {
    printf("   chip ESP32-C6 v%u.%u", (unsigned)((in.chip_rev >> 4) & 0x7), (unsigned)(in.chip_rev & 0xF));
  }
  printf("\n");
  printf("capacidades: ESP-NOW %s | farejo %s | cru %s | CSI %s | FTM %s | 802.15.4 %s\n",
         (in.caps & C6_CAP_ESPNOW) ? "s" : "-", (in.caps & C6_CAP_FAREJO) ? "s" : "-",
         (in.caps & C6_CAP_CRU)    ? "s" : "-", (in.caps & C6_CAP_CSI)    ? "s" : "-",
         (in.caps & C6_CAP_FTM)    ? "s" : "-", (in.caps & C6_CAP_154)    ? "s" : "-");
  printf("heap livre %lu (min %lu)   no ar %lu s   quadros farejados %lu\n",
         (unsigned long)in.heap_livre, (unsigned long)in.heap_min,
         (unsigned long)in.no_ar_s, (unsigned long)in.frames);
  // Firmware of C6 m5tabos-c6-3 em diante: one caminho so; csi_modo 1 = enabled.
  // ESP_FAIL aqui = o WiFi of C6 subiu com csi_enable 0 (ver tab5_wifi.cpp).
  printf("CSI: %s (etapa %u: 1 config, 2 callback, 3 ligar)   ligado: %s\n",
         in.csi_err ? esp_err_to_name(in.csi_err) : "OK", (unsigned)in.csi_etapa,
         in.csi_modo ? "sim" : "nao");
  if (in.csi_err == ESP_FAIL) {
    printf("     ESP_FAIL = o WiFi do C6 nasceu com csi_enable 0 (P4 antigo?)\n");
  }
  static const char* const Z154[] = { "nunca ligado", "ouvindo", "dormindo" };
  printf("saida do C6: %lu mensagem(ns) descartada(s) por fila cheia\n", (unsigned long)in.saida_perdas);
  if (in.flags & C6_INFO_F_COREDUMP) { printf("ATENCAO: o C6 guardou um retrato de travamento (coredump)\n"); }
  if (in.flags & C6_INFO_F_EM_PROVA) { printf("imagem do C6 EM PROVA: ainda nao confirmada pelo P4\n"); }
  printf("     callbacks %lu   enviados ao P4 %lu\n",
         (unsigned long)in.csi_cb, (unsigned long)in.csi_env);
  printf("802.15.4: %s   erro %s   quadros recebidos pelo radio %lu\n",
         in.z154_estado < 3 ? Z154[in.z154_estado] : "?",
         in.z154_err ? esp_err_to_name(in.z154_err) : "nenhum", (unsigned long)in.z154_rx);
}


static void c_sniffer(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "info"))  { show_sniffer_info(); return; }
  if (argc >= 2 && equals_ignore_case(argv[1], "off"))   { tab5_farejo_disable(); printf("farejo desligado\n"); return; }
  if (argc >= 2 && equals_ignore_case(argv[1], "limpar")){ tab5_farejo_limpar(); printf("tabela zerada\n"); return; }
  if (argc >= 2 && equals_ignore_case(argv[1], "on")) {
    c6_farejo_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.filtro = 0;                              // the two types
    cfg.canais = 0;                              // canal current (associado on roteador)
    cfg.lote_ms = (argc >= 3) ? (uint16_t)atoi(argv[2]) : 250;
    // Mascara of canais em hex opcional: 'farejo on <lote_ms> <mask>'. Com
    // more than one channel, RELEASE the router first ('espnow channel 1' already does this),
    // senao o C6 stays preso no canal of AP.
    if (argc >= 4) { cfg.canais = (uint16_t)strtoul(argv[3], nullptr, 16); }
    if (!tab5_farejo_enable(&cfg)) {
      printf("nao liguei: o C6 tem o firmware nosso (v3)? WiFi ligado? ('farejo info')\n");
      return;
    }
    printf("farejo ligado (lote %u ms, canais 0x%04X=0 e o atual). Rode 'farejo' para ver.\n",
           (unsigned)cfg.lote_ms, (unsigned)cfg.canais);
    return;
  }
  if (argc >= 2) { show_usage("farejo"); return; }
  show_sniffer_status();
}

// ---------------------------------------------------------------------------
// RADIO - test of extras of C6 (CSI, FTM, 802.15.4)
// ---------------------------------------------------------------------------
// Turn on each extra for a few seconds and count what iteration. It serves to prove the
// recording of C6 (enable, receive and disable without dropping the C6 or the WiFi) - the
// which can only be fixed with the device open. Counters are written in
// task of events of esp_hosted and read here: volatile, no lock.
//
// O CSI passa pelo servico tab5_csi (o mesmo que o Radar usa): o peer_data
// guarda UM ouvinte por message, e registrar o CSI aqui roubaria o of servico.
static volatile uint32_t s_rx_csi = 0, s_rx_154 = 0, s_rx_ftm = 0;
static volatile int      s_rx_rssi = 0, s_rx_channel = 0, s_rx_len = 0;
static c6_ftm_rep_t      s_rx_ftm_rep;
static bool              s_radio_registrado = false;
// CSI por formato of frame (C6_CSI_FORMATO: 0 11b ... 7 HE TB) e os notices
// que mudam a leitura dos bytes.
static volatile uint32_t s_csi_fmt[16], s_csi_bad_w0 = 0, s_csi_cortado = 0, s_csi_len_max = 0;

static void radio_csi_rx(const c6_csi_cab_t* c, const uint8_t*, uint16_t)
{
  s_rx_rssi = c->rssi; s_rx_channel = c->channel; s_rx_len = c->len;
  s_csi_fmt[C6_CSI_FORMATO(c->flags)] = s_csi_fmt[C6_CSI_FORMATO(c->flags)] + 1;
  if (c->flags & C6_CSI_F_W0_RUIM)  { s_csi_bad_w0 = s_csi_bad_w0 + 1; }
  if (c->flags & C6_CSI_F_CORTADO)  { s_csi_cortado = s_csi_cortado + 1; }
  if (c->len > s_csi_len_max)       { s_csi_len_max = c->len; }
  s_rx_csi = s_rx_csi + 1;
}

static void radio_rx(uint32_t msg_id, const uint8_t* d, size_t n, void*)
{
  if (msg_id == C6_MSG_154 && n >= sizeof(c6_154_cab_t)) {
    c6_154_cab_t c; memcpy(&c, d, sizeof(c));
    s_rx_rssi = c.rssi; s_rx_channel = c.channel; s_rx_len = c.len;
    s_rx_154 = s_rx_154 + 1;
  } else if (msg_id == C6_MSG_FTM && n >= sizeof(c6_ftm_rep_t)) {
    memcpy(&s_rx_ftm_rep, d, sizeof(s_rx_ftm_rep));
    s_rx_ftm = s_rx_ftm + 1;
  }
}

static bool radio_prepare(void)
{
  if (!tab5_wifi_c6_is_ours() || !tab5_wifi_c6_link()) {
    printf("sem o C6 nosso no ar (WiFi ligado? 'farejo info')\n");
    return false;
  }
  if (!s_radio_registrado) {
    eh_host_peer_data_register(C6_MSG_FTM, radio_rx, nullptr);
    eh_host_peer_data_register(C6_MSG_154, radio_rx, nullptr);
    s_radio_registrado = true;
  }
  return true;
}

static void radio_mandar(uint8_t sub, const void* cfg, size_t n)
{
  uint8_t b[1 + 16];
  b[0] = sub;
  memcpy(b + 1, cfg, n);
  eh_host_peer_data_send(C6_MSG_HOST_RADIO, b, 1 + n);
}

// Turns on, renews every 3 s (the C6 turns off on its own in 10 s without renewing), shows
// counts every second and turns off at the end.
//
// ping_meio: pinga o roteador in metade of window, com o recurso LIGADO - e o
// test that separates "associated" from "passing date".
static void ping_roteador(const char* label);

static void radio_window(uint8_t sub, const void* cfg_on, const void* cfg_off, size_t n,
                         int seconds, volatile uint32_t* cont, bool ping_middle = false)
{
  const uint32_t before = *cont;
  for (int s = 0; s < seconds; ++s) {
    if (ping_middle && s > 0 && s == seconds / 2) { ping_roteador("  WiFi DURANTE"); }
    if (s % 3 == 0) { radio_mandar(sub, cfg_on, n); }
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("  %2d s: %lu recebidos   ultimo: canal %d  rssi %d  %d B\n", s + 1,
           (unsigned long)(*cont - before), s_rx_channel, s_rx_rssi, s_rx_len);
  }
  radio_mandar(sub, cfg_off, n);
  printf("desligado. O que o C6 diz (ve tambem se o 'no ar' nao zerou):\n");
  show_sniffer_info();
}

static void c_radio(int argc, char** argv)
{
  if (argc < 2) { show_usage("radio"); return; }
  if (!radio_prepare()) { return; }
  // csi [s] | 154 [canal] [s]
  const bool z154 = equals_ignore_case(argv[1], "154");
  const int  i_seg = z154 ? 3 : 2;
  int seg = (argc > i_seg) ? atoi(argv[i_seg]) : 10;
  if (seg < 1 || seg > 120) { seg = 10; }

  if (equals_ignore_case(argv[1], "csi")) {
    // Pelo servico tab5_csi: ele pede on C6 so o CSI of roteador, pinga o
    // router at 20 Hz as source (the 2.4 GHz beacon, at 1 Mbps, does not generate
    // CSI) e mantem o receptor of C6 acordado. Aqui so se conta o que chega e
    // se mostra a measurement of movimento - parado, sem ninguem andando perto, a
    // razao stays perto of 1.
    printf("CSI por %d s (1 record a cada 100 ms, so do roteador; farejo %s).\n"
           "Deixe o Tab5 apoiado: os 3 primeiros segundos aprendem o 'parado'.\n",
           seg, tab5_farejo_active() ? "LIGADO" : "desligado");
    memset((void*)s_csi_fmt, 0, sizeof(s_csi_fmt));
    s_csi_bad_w0 = 0; s_csi_cortado = 0; s_csi_len_max = 0;
    const uint32_t before = s_rx_csi;
    tab5_csi_ouvinte(radio_csi_rx);
    tab5_csi_enable(TAB5_CSI_CONSOLE);
    static const char* const MOTION_LEVEL_NAMES[3] = { "parado", "leve", "FORTE" };
    for (int i = 0; i < seg; ++i) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      tab5_csi_estado_t e;
      tab5_csi_estado(&e);
      printf("  %2d s: %lu recebidos  rssi %d  %d B   ", i + 1, (unsigned long)(s_rx_csi - before),
             s_rx_rssi, s_rx_len);
      switch (e.fase) {
        case TAB5_CSI_SEM_C6:          printf("C6 sem CSI\n"); break;
        case TAB5_CSI_SEM_ROTEADOR:    printf("sem roteador\n"); break;
        case TAB5_CSI_APRENDENDO:      printf("aprendendo o parado...\n"); break;
        case TAB5_CSI_APARELHO_MEXENDO:printf("Tab5 mexendo\n"); break;
        case TAB5_CSI_MEDINDO:         printf("movimento: %s (x%.1f)\n", MOTION_LEVEL_NAMES[e.level], (double)e.razao); break;
        default:                       printf("\n"); break;
      }
    }
    tab5_csi_disable(TAB5_CSI_CONSOLE);
    tab5_csi_ouvinte(nullptr);
    static const char* const FMT[8] = { "11b", "11g", "HT", "VHT", "HE-SU", "HE-MU", "HE-ERSU", "HE-TB" };
    printf("por formato:");
    for (int f = 0; f < 16; ++f) {
      if (s_csi_fmt[f]) { printf("  %s %lu", f < 8 ? FMT[f] : "?", (unsigned long)s_csi_fmt[f]); }
    }
    printf("\nmaior record %lu B   1a palavra invalida em %lu   cortados %lu\n",
           (unsigned long)s_csi_len_max, (unsigned long)s_csi_bad_w0, (unsigned long)s_csi_cortado);
    return;
  }
  if (equals_ignore_case(argv[1], "154")) {
    c6_154_cfg_t on, off;
    memset(&on, 0, sizeof(on)); memset(&off, 0, sizeof(off));
    on.liga = 1;
    on.channel = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 15;
    if (on.channel < 11 || on.channel > 26) { printf("canal 802.15.4 vai de 11 a 26\n"); return; }
    on.promiscuo = 1;
    printf("802.15.4 no canal %u por %d s (so ouvindo; sem Zigbee/Thread por perto, 0 e normal):\n",
           (unsigned)on.channel, seg);
    // 802.15.4 shares the antenna with WiFi: the test measures WiFi first, with
    // ele enabled e depois. Os tres em 4/4 = a coexistencia esta funcionando.
    ping_roteador("  WiFi ANTES");
    radio_window(C6_RX_154, &on, &off, sizeof(on), seg, &s_rx_154, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    ping_roteador("  WiFi DEPOIS");
    return;
  }
  if (equals_ignore_case(argv[1], "ftm")) {
    // Alvo: o roteador conectado (MAC e canal dele).
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { printf("sem roteador conectado\n"); return; }
    c6_ftm_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.peer, ap.bssid, 6);
    c.channel = ap.primary;
    c.frm_count = 16;
    printf("FTM contra %02X:%02X:%02X:%02X:%02X:%02X (canal %u, FTM %s no anuncio)...\n",
           c.peer[0], c.peer[1], c.peer[2], c.peer[3], c.peer[4], c.peer[5], (unsigned)c.channel,
           ap.ftm_responder ? "SIM" : "nao");
    const uint32_t before = s_rx_ftm;
    radio_mandar(C6_RX_FTM, &c, sizeof(c));
    for (int i = 0; i < 50 && s_rx_ftm == before; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
    if (s_rx_ftm == before) { printf("sem resposta do C6 em 5 s\n"); return; }
    const c6_ftm_rep_t& r = s_rx_ftm_rep;
    if (r.status == 0) {
      printf("ok: distancia %lu cm, rtt %lu ns (%u quadros)\n",
             (unsigned long)r.dist_est_cm, (unsigned long)r.rtt_est_ns, (unsigned)r.num);
    } else {
      if (r.status == C6_FTM_NAO_INICIOU) {
        // O C6 nem mandou a requisicao: o reason e o esp_err_t of driver dele.
        const esp_err_t e = (esp_err_t)(int32_t)r.rtt_raw_ns;
        printf("o C6 nao iniciou a sessao: %s\n", esp_err_to_name(e));
        if (e == ESP_ERR_NOT_SUPPORTED) {
          // esp_wifi/src/wifi_init.c apaga o bit of FTM initiator nos C6 v0.0
          // and v0.1 (errata of silicio). There is no honest way around: forcing the bit
          // daria distancias erradas.
          printf("  o ESP32-C6 das revisoes v0.0/v0.1 nao faz FTM initiator (errata do chip;\n"
                 "  o proprio ESP-IDF desliga). 'farejo info' mostra a revisao deste C6.\n");
        }
      } else {
        static const char* const ST[] = { "sucesso", "o outro lado nao fala FTM", "recusado",
                                          "sem resposta", "falhou", "sem medida valida", "encerrado" };
        printf("sessao feita, status %u (%s)%s\n", (unsigned)r.status,
               r.status < 7 ? ST[r.status] : "?",
               ap.ftm_responder ? "" : " - o roteador nao anuncia FTM responder");
      }
    }
    return;
  }
  show_usage("radio");
}

// ---------------------------------------------------------------------------
// PING - separates "WiFi does not pass data" from "internet/server does not respond"
// ---------------------------------------------------------------------------
static volatile int s_ping_ok = 0, s_ping_perdidos = 0;
static volatile uint32_t s_ping_ms_total = 0;

static void ping_ok(esp_ping_handle_t h, void*)
{
  uint32_t ms = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
  s_ping_ok = s_ping_ok + 1;
  s_ping_ms_total = s_ping_ms_total + ms;
}
static void ping_perdido(esp_ping_handle_t, void*) { s_ping_perdidos = s_ping_perdidos + 1; }

static void ping_one(const char* label, const ip_addr_t& target)
{
  s_ping_ok = 0; s_ping_perdidos = 0;
  s_ping_ms_total = 0;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = target;
  cfg.count = 4;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 1500;
  esp_ping_callbacks_t cb = { };
  cb.on_ping_success = ping_ok;
  cb.on_ping_timeout = ping_perdido;
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { printf("%s: nao deu para criar o ping\n", label); return; }
  esp_ping_start(h);
  for (int i = 0; i < 80 && s_ping_ok + s_ping_perdidos < 4; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
  esp_ping_stop(h);
  esp_ping_delete_session(h);
  if (s_ping_ok) {
    printf("%-22s %d/4 ok   media %lu ms\n", label, s_ping_ok,
           (unsigned long)(s_ping_ms_total / (uint32_t)s_ping_ok));
  } else {
    printf("%-22s 0/4 - SEM RESPOSTA\n", label);
  }
}

static void ping_roteador(const char* label)
{
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.gw.addr == 0) {
    printf("%-22s sem IP - WiFi desconectado\n", label);
    return;
  }
  ip_addr_t a = { };
  a.type = IPADDR_TYPE_V4;
  a.u_addr.ip4.addr = ip.gw.addr;
  ping_one(label, a);
}

static void c_ping(int argc, char** argv)
{
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
    printf("sem IP: o WiFi nao esta conectado\n");
    return;
  }
  ip_addr_t a = { };
  if (argc >= 2) {
    // Um alvo so: IP ou name.
    struct addrinfo* res = nullptr;
    if (getaddrinfo(argv[1], nullptr, nullptr, &res) != 0 || !res) {
      printf("%s: o DNS nao resolveu (sem internet ou nome errado)\n", argv[1]);
      return;
    }
    a.type = IPADDR_TYPE_V4;
    a.u_addr.ip4.addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    ping_one(argv[1], a);
    return;
  }
  // Sem alvo: o roteador (o WiFi passa data?) e a internet (passa of roteador?).
  a.type = IPADDR_TYPE_V4;
  a.u_addr.ip4.addr = ip.gw.addr;
  ping_one("roteador (gateway)", a);
  a.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
  ping_one("internet (8.8.8.8)", a);
}

// ---------------------------------------------------------------------------
// MESHTASTIC
// ---------------------------------------------------------------------------

// Lista compacta so com numero e name, para consultar antes of send.
static void mesh_nodes(void)
{
  const int n = tab5_mesh_num_nos();
  printf("%d no(s) - use o numero ou o nome curto no 'mesh enviar para'\n", n);
  for (int i = 0; i < n; ++i) {
    yield(i);
    mesh_no_t no;
    if (!tab5_mesh_no(i, &no)) { continue; }
    printf("  %08lX  %-6.6s  %s\n", (unsigned long)no.num, no.curto,
           no.longo[0] ? no.longo : "(sem nome)");
  }
}

// argv[0] = "send". Chamado por 'mesh send ...' e pelo atalho 'send ...'.
static void mesh_send(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "para")) {
    // "send to <number|name> <text>" - DIRECT message.
    //
    // Aceita o numero em hexadecimal (como o 'mesh nos' imprime) ou o name
    // short, because decorating F115C354 is not reasonable. The short and compared name
    // case insensitive: Meshtastic uses capital letters in some nodes and
    // not in others, and getting it wrong would result in "not found" without explanation.
    if (argc < 4) { show_usage("mesh"); return; }
    uint32_t target_node = 0;
    const char* quem = argv[2];

    char* end = nullptr;
    const unsigned long v = strtoul(quem, &end, 16);
    if (end && *end == 0 && strlen(quem) >= 6) { target_node = (uint32_t)v; }

    if (target_node == 0) {
      const int n = tab5_mesh_num_nos();
      for (int i = 0; i < n && target_node == 0; ++i) {
        mesh_no_t no;
        if (!tab5_mesh_no(i, &no)) { continue; }
        if (strcasecmp(no.curto, quem) == 0) { target_node = no.num; }
      }
    }
    if (target_node == 0) { printf("no '%s' nao encontrado - veja 'mesh nos'\n", quem); return; }

    char msg[160];
    juntar(msg, sizeof(msg), argc, argv, 3);
    printf(tab5_mesh_send(target_node, 0, msg)
           ? "enviado para %08lX\n" : "falhou (enlace mudo?) %08lX\n", (unsigned long)target_node);
    return;
  }
  if (argc < 2) { show_usage("mesh"); return; }
  // "send <text>" difunde no canal 0 (LongFast).
  char msg[160];
  juntar(msg, sizeof(msg), argc, argv, 1);
  printf(tab5_mesh_send(0xFFFFFFFFu, 0, msg) ? "enviado\n" : "falhou (enlace mudo?)\n");
}

// TRUNCATION DIAGNOSIS of Mesh messages. The text here exit CRU - without
// quebra of line, sem recorte of bubble, sem fonte proporcional no middle - e
// with the SIZE IN BYTES on the side, exactly to separate "the data that arrived
// esta cortado" (bug of captura) of "a screen que desenha esta cortando" (bug of
// drawing).
//
// GET ALL CONVERSATIONS TOGETHER (each one lives in its own file on the card - see
// tab5_mesh_historico()) because there is no "open conversation" here in the console, just
// in screen. Ordena por horario, mais recente first.
static void mesh_msgs(int count)
{
  if (count < 1)   { count = 1; }
  if (count > 200) { count = 200; }   // mesmo teto of tab5_mesh_historico()

  const int n_conv = tab5_mesh_num_conversas();
  const int CAP = 200;
  mesh_msg_t* todas = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * CAP, MALLOC_CAP_SPIRAM);
  mesh_msg_t* buf   = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * CAP, MALLOC_CAP_SPIRAM);
  if (!todas || !buf) {
    printf("sem memoria para juntar o historico\n");
  } else {
    int n_todas = 0;
    for (int c = 0; c < n_conv && n_todas < CAP; ++c) {
      const int n = tab5_mesh_historico(c, buf, CAP);
      for (int i = 0; i < n && n_todas < CAP; ++i) { todas[n_todas++] = buf[i]; }
    }

    // Insertion sort: comando manual, no maximum algumas centenas of items.
    for (int i = 1; i < n_todas; ++i) {
      mesh_msg_t key_message = todas[i];
      int j = i - 1;
      while (j >= 0 && todas[j].when < key_message.when) { todas[j + 1] = todas[j]; --j; }
      todas[j + 1] = key_message;
    }

    const int mostrar = (count < n_todas) ? count : n_todas;
    printf("%d mensagem(ns) em %d conversa(s), mostrando as %d mais recentes:\n",
           n_todas, n_conv, mostrar);
    for (int i = 0; i < mostrar; ++i) {
      yield(i);
      const mesh_msg_t& m = todas[i];
      printf("[%3u bytes] de %08lX (%lus): %s\n",
             (unsigned)strlen(m.text), (unsigned long)m.of,
             (unsigned long)m.when, m.text);
    }
  }
  if (todas) { heap_caps_free(todas); }
  if (buf)   { heap_caps_free(buf); }
}

// The complete form: link, health, each node and messages.
static void mesh_complete(void)
{
  printf("enlace: %s\n", tab5_mesh_no_ar() ? "no ar" : "SEM QUADRO recente");
  printf("meu no: %08lX   firmware do C6L: %s\n",
         (unsigned long)tab5_mesh_meu_numero(),
         tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : "(nao veio)");

  mesh_saude_t sa; tab5_mesh_saude(&sa);
  printf("canal %.1f%%  airtime %.2f%%  uptime %lus  nos %u/%u\n",
         sa.uso_canal, sa.airtime_tx, (unsigned long)sa.uptime_s,
         (unsigned)sa.nos_online, (unsigned)sa.nos_total);

  // COMPLETE FORM OF EACH NO, and not the summarized line. On the screen, space rules;
  // here there is no such pressure, and this is where the answer is "what device do I use?"
  // estou falando". Nome longo e curto sao etiquetas que o dono troca when
  // he wants; id, MAC and model are the device.
  const int n = tab5_mesh_num_nos();
  printf("\n%d no(s) (%lu despejo(s) da tabela desde o boot - ver achar_no):\n",
         n, (unsigned long)tab5_mesh_despejos());
  for (int i = 0; i < n; ++i) {
    yield(i);
    mesh_no_t no;
    if (!tab5_mesh_no(i, &no)) { continue; }

    const char* hw = mesh_hw_name(no.hw);
    char hwtxt[20];
    if (!hw) { snprintf(hwtxt, sizeof(hwtxt), "modelo %u", (unsigned)no.hw); hw = hwtxt; }

    printf("  %-20.20s [%s]\n", no.longo[0] ? no.longo : "(sem nome)", no.curto);
    printf("      num %08lX   id %-10s   %s\n",
           (unsigned long)no.num, no.id[0] ? no.id : "(nao veio)", hw);

    // MAC so when ele MANDOU. O field e marcado como obsoleto no protobuf e
    // firmware novo costuma omitir - imprimir 00:00:00:00:00:00 ali seria
    // present the absence as if it were an address.
    if (no.tem_mac) {
      printf("      mac %02X:%02X:%02X:%02X:%02X:%02X\n",
             no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      printf("      mac (nao enviado - campo obsoleto no protobuf; o id vem dele)\n");
    }

    // saltos == 255 e o sentinela of "desconhecido" (MESH_SALTOS_DESCONHECIDO)
    // - printing the raw number would look like real data instead of "I don't know yet".
    char saltos_txt[8];
    if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(saltos_txt, sizeof(saltos_txt), "?"); }
    else { snprintf(saltos_txt, sizeof(saltos_txt), "%u", (unsigned)no.saltos); }
    printf("      snr %.1f  bat %u%%  saltos %s%s",
           no.snr, (unsigned)no.battery, saltos_txt, no.via_mqtt ? "  (via MQTT)" : "");
    if (no.tem_pos) { printf("  pos %.5f,%.5f", no.lat_i / 1e7, no.lon_i / 1e7); }
    printf("\n");
  }

  uint32_t q, pk, tel, ls, tx, ni; int var;
  tab5_mesh_contadores(&q, &pk, &tel, &ls, &tx, &ni, &var);
  printf("\nrecebidos: %lu quadros, %lu pacotes, %lu nodeinfo, %lu telemetria "
         "(%lu localstats), %lu texto | ultima variante de Telemetry: %d\n",
         (unsigned long)q, (unsigned long)pk, (unsigned long)ni, (unsigned long)tel,
         (unsigned long)ls, (unsigned long)tx, var);
  // Route quality thermometer: see tab5_mesh_routing_extra(). High NO and
  // error - and "someone heard" coming easier than radio confirmation.
  printf("routing extra apos confirmado: %lu (nao muda o estado, so o mostra)\n",
         (unsigned long)tab5_mesh_routing_extra());

  const int m = tab5_mesh_num_msgs();
  printf("\n%d mensagem(ns), %d nao lida(s):\n", m, tab5_mesh_nao_lidas());
  for (int i = 0; i < m; ++i) {
    yield(i);
    mesh_msg_t ms;
    if (!tab5_mesh_msg(i, &ms)) { continue; }
    // Status of delivery only makes sense in OUR: message received does not have the
    // which confirms. The reason of failure comes from Routing.Error of radio.
    char status_mark[40] = "";
    switch (ms.send_state) {
      case MESH_ENVIANDO:     snprintf(status_mark, sizeof(status_mark), " [enviando]"); break;
      case MESH_ENTREGUE:     snprintf(status_mark, sizeof(status_mark), " [entregue]"); break;
      case MESH_SEM_RESPOSTA: snprintf(status_mark, sizeof(status_mark), " [sem resposta]"); break;
      case MESH_FALHOU:       snprintf(status_mark, sizeof(status_mark), " [FALHOU: %s]",
                                       mesh_erro_name(ms.erro)); break;
      default: break;
    }
    // RAW LINK OF THIS MESSAGE (via_mqtt/saltos of own MeshPacket, BEFORE
    // of fallback that the screen does for the link of no - see tab5_mesh_no_enlace).
    char enlace[24];
    if (ms.via_mqtt) { snprintf(enlace, sizeof(enlace), "mqtt"); }
    else if (ms.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(enlace, sizeof(enlace), "saltos ?"); }
    else { snprintf(enlace, sizeof(enlace), "%u salto(s)", (unsigned)ms.saltos); }
    printf("  de %08lX (snr %.1f, %s): %s%s\n",
           (unsigned long)ms.of, ms.snr, enlace, ms.text, status_mark);
  }
}

static void c_mesh(int argc, char** argv)
{
  if (argc < 2) { mesh_complete(); return; }
  const char* o = argv[1];
  if (equals_ignore_case(o, "nos"))    { mesh_nodes(); return; }
  if (equals_ignore_case(o, "msgs"))   { mesh_msgs(argc >= 3 ? atoi(argv[2]) : 20); return; }
  if (equals_ignore_case(o, "enviar")) { mesh_send(argc - 1, argv + 1); return; }
  if (equals_ignore_case(o, "casa")) {
    // REFERENCE POSITION for the conversation to show distance/direction up to a
    // localizacao recebida - ver achar_coord() em app_mesh.cpp. Fica em
    // settings.cpp (NVS), not Mesh: and of user, not of service.
    if (argc < 4) {
      float lat, lon;
      if (settings_home(&lat, &lon)) { printf("casa: %.6f, %.6f\n", (double)lat, (double)lon); }
      else { printf("casa: nao configurada. uso: mesh casa <latitude> <longitude>\n"); }
      return;
    }
    const float lat = strtof(argv[2], nullptr);
    const float lon = strtof(argv[3], nullptr);
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
      printf("fora da faixa (latitude -90..90, longitude -180..180)\n");
    } else {
      settings_set_home(lat, lon);
      printf("casa gravada: %.6f, %.6f\n", (double)lat, (double)lon);
    }
    return;
  }
  if (equals_ignore_case(o, "pausar")) {
    // STOP TALKING TO THE RADIO WITHOUT TURNING OFF ANYTHING - the UART stops, the Grove and
    // devolvido, mas o trilho of 5V continua enabled e o modulo continua no ar.
    // Isolates a question: the instability comes from what M5tabOS sends through the
    // serial, ou of POSICAO of modulo? Com isto ele stays onde estava, e so o
    // traffic to.
    if (tab5_mesh_stop()) {
      printf("servico da malha parado - Grove liberado, mas o 5V continua ligado\n"
             "e o modulo continua no ar (o BLE dele nao e afetado por isto).\n"
             "use 'mesh retomar' para o M5tabOS voltar a conversar com ele.\n");
    } else {
      printf("nao consegui parar agora (a tarefa nao confirmou em 1s) - tente de novo\n");
    }
    return;
  }
  if (equals_ignore_case(o, "retomar")) {
    tab5_mesh_start();
    printf("pedido de retomada enviado - rode 'mesh' em alguns segundos para conferir.\n");
    return;
  }
  show_usage("mesh");
}

// Atalhos antigos: 'nos' e 'send ...' sem o 'mesh' in frente.
static void c_nodes(int, char**)             { mesh_nodes(); }
static void c_send(int argc, char** argv)  { mesh_send(argc, argv); }

// ---------------------------------------------------------------------------
// BANCADA
// ---------------------------------------------------------------------------

static void c_grove(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "trocar")) {
    // TROQUEI O MODULO NO CABO. Para o servico, devolve a porta, sonda of novo
    // and go up as much as possible. Done by the system, in task of interface - from here
    // It is not possible to safely touch the UART of another task.
    if (system_grove_redetect()) {
      printf("pedido feito - a interface sonda no proximo ciclo (uns 4s).\n"
             "rode 'grove' depois para ver o que achou.\n");
    } else {
      printf("um app esta usando o Grove agora - feche-o e tente de novo\n");
    }
    return;
  }
  // 'grove cru <baud>', ou a forma antiga 'grove <baud>': despeja os bytes
  // crus, para OLHAR em vez of deduzir.
  const char* baud = nullptr;
  if (argc >= 3 && equals_ignore_case(argv[1], "cru"))          { baud = argv[2]; }
  else if (argc >= 2 && atoi(argv[1]) > 0)      { baud = argv[1]; }
  if (baud) { tab5_grove_dump(atoi(baud), 2500); return; }
  if (argc >= 2) { show_usage("grove"); return; }

  // ONLY REPORT - not probe. Probing from here was a silent bug: the probe writes
  // and le in UART, and with the Meshtastic service of pe the port is DELE. Two
  // tarefas in mesma UART embaralham o fluxo e o enlace caia por causa of
  // comando que deveria so diagnosticar.
  const grove_owner_t owner = tab5_grove_owner();
  printf("Grove: %s\n", grove_name(tab5_grove_type()));
  printf("  energia : %s\n", settings_grove() ? "ligada" : "desligada");
  printf("  porta   : %s\n", (owner == GROVE_OWNER_NONE) ? "livre" : grove_owner_name(owner));
  if (system_grove_probing()) { printf("  sondagem em andamento...\n"); }
  printf("\ntrocou o modulo do cabo? use 'grove trocar'\n");
}

// ---------------------------------------------------------------------------
// HELP
// ---------------------------------------------------------------------------

typedef enum { G_SYSTEM, G_SCREEN, G_CARD, G_NETWORK, G_MESH, G_BANCADA, G_TESTES, G_APPS, G_N, G_OCULTO } command_group_t;

static const char* const GROUP_TITLES[G_N] = {
  "SYSTEM      what is running and what it costs",
  "SCREEN      panel, brightness, and touch",
  "CARD        files on the SD card",
  "NETWORK     WiFi, Bluetooth, ESP-NOW, and the C6 coprocessor",
  "MESHTASTIC  LoRa radio through Grove (C6L)",
  "BENCH       external hardware and camera",
  "TESTS       artificial load for validating services",
  "APPS        application commands: app <app> <command>",
};

typedef struct {
  const char* name;
  const char* apelidos;   // space-separated aliases; accepted but omitted from listings
  command_group_t group;
  const char* summary;     // one line in 'help'
  const char* usage;        // one form per line: "form\twhat it does"
  const char* detail;    // purpose and cautions - only in 'help <command>'
  void (*fn)(int argc, char** argv);
  const char* exemplos;   // ready-to-copy lines: "line\twhat it does"
} command_t;

static void c_ajuda(int argc, char** argv);
static void imprimir_colunas(const char* text, const char* prefix);

// ---------------------------------------------------------------------------
// APPLICATION COMMANDS - 'app <app> <command> ...' (app_comando_t, kernel/app.h)
// ---------------------------------------------------------------------------
// The app name is required: an app never shadows a system command or another
// app's command, and 'help' shows which app owns each command.

// "ESP-NOW", "esp-now", "espnow" sao o mesmo app.
static bool name_of_app(const char* name, const char* s)
{
  for (;;) {
    while (*name == ' ' || *name == '-') { ++name; }
    while (*s == ' ' || *s == '-')       { ++s; }
    if (!*name || !*s) { return !*name && !*s; }
    if (tolower((unsigned char)*name++) != tolower((unsigned char)*s++)) { return false; }
  }
}

static void ajuda_apps(const app_t* so_este)
{
  bool algum = false;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const app_t* a = SISTEMA_APPS[i];
    if (!a->commands || (so_este && a != so_este)) { continue; }
    for (const app_comando_t* c = a->commands; c->name; ++c) {
      char pre[48];
      snprintf(pre, sizeof(pre), "app %s %s ", a->name, c->name);
      imprimir_colunas(c->uso, pre);
      algum = true;
    }
  }
  if (!algum) { printf("  (nenhum app instalado oferece comandos)\n"); }
}

static void c_app(int argc, char** argv)
{
  const app_t* a = nullptr;
  for (int i = 0; argc >= 2 && i < SISTEMA_NUM_APPS; ++i) {
    if (name_of_app(SISTEMA_APPS[i]->name, argv[1])) { a = SISTEMA_APPS[i]; }
  }
  if (!a) {
    if (argc >= 2) { printf("no app '%s'\n", argv[1]); }
    printf("usage: app <app> <command> [arguments]\n");
    ajuda_apps(nullptr);
    return;
  }
  const app_comando_t* c = nullptr;
  for (const app_comando_t* k = a->commands; argc >= 3 && k && k->name; ++k) {
    if (equals_ignore_case(k->name, argv[2])) { c = k; }
  }
  if (!c) {
    if (argc >= 3) { printf("app %s has no command '%s'\n", a->name, argv[2]); }
    ajuda_apps(a);
    return;
  }
  const char* current = system_app_current();
  if (c->so_open && (!current || strcmp(current, a->name) != 0)) {
    printf("open app %s first ('open %s')\n", a->name, a->name);
    return;
  }
  c->fn(argc - 2, argv + 2);
}

static const command_t COMANDOS[] = {
  // --- SISTEMA ---
  { "estado", "servicos", G_SYSTEM,
    "panorama: versao, tempo ligado, RAM e uma linha por servico",
    "estado\tversao, app aberto, RAM, cartao, wifi, ble, mesh, energia",
    "A primeira pergunta quando algo esta estranho: o que esta de pe agora?\n"
    "Cada servico responde em uma linha; o detalhe de cada um esta no comando\n"
    "dele (wifi, mesh, energia...).",
    c_status,
    "estado\tprimeiro passo antes de investigar qualquer problema" },
  { "tarefas", "tarefa", G_SYSTEM,
    "quem esta rodando: CPU%, prioridade e folga de pilha",
    "tarefas\tuma linha por tarefa do FreeRTOS, com o app dono",
    "Le a ultima foto do monitor (1 s) sem parar o sistema. CPU% e do aparelho:\n"
    "os dois nucleos somam 100%, entao um nucleo cheio aparece como 50%.\n"
    "Pilha livre abaixo de ~500 B e risco de estouro. Digitar no serial nao\n"
    "acorda a tela, entao da para medir o repouso de verdade.",
    c_tasks,
    "tarefas\tquem esta gastando CPU agora" },
  { "mem", "memoria", G_SYSTEM,
    "RAM interna e PSRAM: uso, pico e fragmentacao",
    "mem\tresumo: em uso, minimo ja visto, maior bloco\n"
    "mem extrato\tquanto cada etapa do boot consumiu\n"
    "mem mapa\tblocos por regiao e tamanho (fragmentacao)\n"
    "mem marca\ttira uma foto do heap (ponto de partida)\n"
    "mem delta\to que cresceu desde a marca (cacar vazamento)\n"
    "mem tudo\tos tres juntos",
    "O 'idf.py size' so conta o estatico; aqui aparece o que foi alocado em\n"
    "execucao (pilhas, buffers, framebuffers). O numero que diz se cabe mais um\n"
    "app e o 'minimo ja visto', nao o livre de agora. Muito livre com 'maior\n"
    "bloco' pequeno = fragmentacao.",
    c_memory,
    "mem\tquanto de RAM sobra\n"
    "mem extrato\tqual servico consumiu RAM no boot" },
  { "energia", "", G_SYSTEM,
    "bateria, corrente, clock e os ultimos 30 min",
    "energia\ttensao, corrente, % e historico por minuto\n"
    "energia travas [seg]\tquem impede o light sleep (espera seg antes)\n"
    "energia repouso <mhz>\tforca o repouso agora nesse clock\n"
    "energia teto <mhz>\tlimita o CPU com a tela acesa (o maximo desfaz)",
    "Os valores vem do cache que a interface atualiza 1x/s - o console nunca\n"
    "le o INA226 direto. 'travas' so vale medido em repouso: use um tempo maior\n"
    "que o tempo de tela de Ajustes e nao toque na tela durante a espera.\n"
    "Clocks validos: " TAB5_CPU_STEPS ".",
    c_power,
    "energia\tquanto a bateria esta gastando\n"
    "energia travas 40\tespera a tela apagar e mostra quem impede o sono\n"
    "energia teto " TAB5_CPU_HALF "\tCPU no maximo a " TAB5_CPU_HALF " MHz" },
  { "fundo", "", G_SYSTEM,
    "partes de apps que seguem rodando com a janela fechada",
    "fundo\to que esta rodando e ha quanto tempo\n"
    "fundo parar <nome>\tpede para uma delas parar",
    "Parte de fundo e o pedaco de um app que continua com a janela fechada\n"
    "(ex.: o cronometro do Exemplo). A parada acontece na interface, no proximo ciclo.",
    c_fundo,
    "fundo parar Exemplo\tpara o cronometro do Exemplo" },
  { "log", "", G_SYSTEM,
    "ve ou muda o nivel de log de um modulo",
    "log <etiqueta>\tmostra o nivel atual\n"
    "log <etiqueta> <nivel>\tnada erro aviso info debug verboso (ou 0-5)\n"
    "log * <nivel>\ttodos os modulos de uma vez",
    "O sistema silencia alguns modulos barulhentos no boot (eh_sdio, sdmmc_io).\n"
    "Isto devolve o log cru deles sem regravar. Ex.: log eh_sdio debug.\n"
    "Reiniciar volta tudo ao normal.",
    c_log,
    "log eh_sdio debug\tver o log cru do enlace com o C6\n"
    "log mesh\tem que nivel esta o log do Mesh\n"
    "log * aviso\tso avisos e erros, de todos os modulos" },
  { "pendrive", "", G_SYSTEM,
    "liga o cartao como disco USB no computador, sem tocar na tela",
    "pendrive\testado da ultima sessao\n"
    "pendrive ligar\to cartao vai para o computador\n"
    "pendrive devolver\to cartao volta (so com o console de volta)",
    "Ligar tira o console do USB-C: a serial some e o computador ve o disco.\n"
    "O console volta quando o computador ejeta (ou devolvendo na tela).\n"
    "O caminho da sessao fica no log do cartao (tag 'pendrive').",
    c_pendrive,
    "pendrive ligar\tmontar o cartao no computador" },

  // --- TELA ---
  { "tela", "", G_SCREEN,
    "painel: quadros, piscada azul, relogio e prioridade",
    "tela\testado do painel e da janela de medida\n"
    "tela medir\tzera a janela; use o aparelho e rode 'tela'\n"
    "tela foto [nome]\tgrava a tela em apps/Console/<nome>.bmp\n"
    "tela banco\tcompara os motores de copia (PPA, DMA2D, CPU)\n"
    "tela relogio <mhz>\ttroca o relogio de pixel e grava (80 = fabrica)\n"
    "tela prioridade alta|padrao\tprioridade da tela no barramento AXI\n"
    "tela rolar cpu|dma\tquem faz a copia da rolagem",
    "Para investigar piscada e fluidez. 'underrun' acima de zero = o painel\n"
    "ficou sem dado a tempo, que e a piscada azul. Palavra nao reconhecida nao\n"
    "altera nada.",
    c_screen,
    "tela medir\tzera; use o aparelho; depois rode tela\n"
    "tela relogio 80\tvolta ao relogio de fabrica do painel\n"
    "tela prioridade padrao\tdesliga a prioridade para comparar" },
  { "brilho", "", G_SCREEN,
    "testa o backlight sem gravar",
    "brilho\tvalor atual e o gravado\n"
    "brilho <1-255> [hz]\taplica (e muda a frequencia do PWM)",
    "Para achar o menor brilho que ainda acende. Nao grava: Ajustes ou\n"
    "reiniciar voltam ao valor salvo. Se a tela sumir, 'brilho 80' a traz de volta.",
    c_brightness,
    "brilho 6\to menor brilho ainda acende?\n"
    "brilho 80\ttraz a tela de volta se ela sumir" },
  { "piscada", "", G_SCREEN,
    "dispara UMA causa possivel da piscada azul",
    "piscada brilho\treescreve o mesmo brilho\n"
    "piscada c6\treescreve o pino de energia do C6\n"
    "piscada perfil\treaplica o perfil de energia inteiro\n"
    "piscada pm\treconfigura o gerenciador de energia\n"
    "piscada grove\tcorta o 5V do Grove por 1 s\n"
    "piscada quadro\tredesenho completo da tela",
    "Abrir um app faz varias coisas ao mesmo tempo; cada opcao faz so uma e\n"
    "anota a hora. Olhe a tela e veja qual pisca.",
    c_piscada,
    "piscada perfil\treaplicar o perfil faz piscar?" },
  { "toque", "", G_SCREEN,
    "mostra onde cada toque cai na tela",
    "toque\tliga: posicao crua e convertida de cada toque\n"
    "toque off\tdesliga",
    "Para conferir se um botao recebe o dedo onde deveria. Quem le o painel e\n"
    "a interface; o console so liga a bandeira.",
    c_touch,
    "toque\tdepois toque nos botoes e veja a posicao no log" },
  { "tocar", "", G_SCREEN,
    "clique simulado (bancada)",
    "tocar <x> <y>\tclica em x,y de tela, como o dedo",
    "Para testar uma tela pela serial. Acorda a tela se estiver em repouso\n"
    "(e esse primeiro pedido nao vira clique, como o dedo).",
    c_simulate_touch,
    "tocar 360 640\tclica no meio da tela" },
  { "abrir", "", G_SCREEN,
    "abre um app pela serial (bancada)",
    "abrir\tlista os apps\n"
    "abrir <nome>\tabre (fecha o que estiver aberto)\n"
    "abrir home\tvolta para a tela inicial",
    "Junto de 'tocar' e 'tela foto', testa uma tela sem tocar no aparelho.",
    c_open,
    "abrir Arquivos\tdepois 'tela foto'" },
  { "ordem", "", G_SCREEN,
    "ordem dos icones na tela inicial",
    "ordem\tlista os apps na ordem da home\n"
    "ordem <nome> <posicao>\tmove o app para a posicao (1 = primeiro)\n"
    "ordem padrao\tvolta para a ordem de apps.cpp",
    "Gravada na NVS: vale depois de reiniciar, sem compilar. Na propria tela:\n"
    "segure um icone ate a moldura aparecer e arraste; para outra pagina,\n"
    "segure o icone na beirada lateral.",
    c_order,
    "ordem Exemplo 1\tExemplo vira o primeiro icone" },
  { "ui", "", G_SCREEN,
    "onde a interface esta agora (tela travada?)",
    "ui\tetapa atual, voltas do laco, ultimo toque, saude do GT911",
    "Funciona com a tela travada, porque o console e outra tarefa. Rode duas\n"
    "vezes: se 'voltas' nao mudou, o laco da interface parou nessa etapa.",
    c_ui,
    "ui\trode duas vezes com a tela travada e compare" },

  // --- CARD ---
  { "cartao", "", G_CARD,
    "espaco livre e o conteudo de cada pasta de app",
    "cartao\tespaco, gravador do sistema e ate 10 arquivos por pasta",
    "O panorama do SD. Para uma pasta especifica, use 'lista'.",
    [](int, char**) { cmd_card(); },
    "cartao\tquanto espaco sobra no SD" },
  { "lista", "", G_CARD,
    "lista uma pasta",
    "lista [caminho]\tpadrao /sdcard (ex.: lista apps/Sistema)",
    "Caminho relativo e lido a partir de /sdcard.",
    [](int c, char** v) { cmd_list(c, v); },
    "lista apps/Mesh\tarquivos do app Mesh" },
  { "ler", "", G_CARD,
    "mostra um arquivo de texto",
    "ler <caminho> [linhas]\tate 200 linhas, ou quantas pedir",
    "Para olhar. Para copiar o arquivo inteiro para o Mac, use 'despejar'.\n"
    "Caminho relativo e lido a partir de /sdcard.",
    [](int c, char** v) { cmd_read(c, v); },
    "ler apps/Sistema/logs/log_2026-10-01.txt 50\tas 50 primeiras linhas do log do dia" },
  { "despejar", "", G_CARD,
    "manda o arquivo inteiro, para salvar no Mac",
    "despejar <caminho>\tsem limite, entre marcadores >>>INICIO/>>>FIM",
    "Cala o log durante o envio para nenhuma linha cair no meio do arquivo.\n"
    "No Mac, com o monitor fechado: ./tools/download.py <caminho>",
    [](int c, char** v) { cmd_despejar(c, v); },
    "despejar apps/Sistema/energia/energia-2026-10-01.csv\tcopia para o Mac (use tools/download.py)" },
  { "apagar", "", G_CARD,
    "apaga um arquivo do cartao",
    "apagar <caminho>\tum arquivo por vez, sem curinga",
    "Irreversivel. So aceita caminhos dentro de /sdcard e sem '..'.",
    [](int c, char** v) { cmd_erase(c, v); },
    "apagar apps/Console/console.bmp\tapaga a foto de teste" },
  { "receber", "", G_CARD,
    "recebe um arquivo pela serial e grava no cartao",
    "receber <app> <arquivo> <bytes>\tgrava em /sdcard/apps/<app>/<arquivo>",
    "Depois do comando, mande o arquivo cru pela mesma porta, com o monitor\n"
    "fechado. Tudo de uma vez, do Mac:\n"
    "  { printf 'receber Sistema x.bin 1234\\r'; sleep 2; cat x.bin; } > /dev/cu.usbmodem*\n"
    "Confira o md5 impresso no fim com 'md5 -q x.bin'. Ate 6 MB.",
    [](int c, char** v) { cmd_receive(c, v); },
    "receber Sistema tab5_c6.bin 1397904\tdepois mande o arquivo pela porta" },

  // --- REDE ---
  { "wifi", "", G_NETWORK,
    "estado, redes em volta e redes guardadas",
    "wifi\testado: rede, IP, sinal\n"
    "wifi redes\tvarre e lista as redes visiveis\n"
    "wifi gravar <ssid> [senha]\tguarda a rede (sem senha = aberta)\n"
    "wifi conectar <ssid>\ttroca para uma rede ja guardada\n"
    "wifi esquecer <ssid>\ttira a rede do wifi.txt",
    "Use \"aspas\" se o nome ou a senha tiver espaco. A senha e gravada cifrada.\n"
    "'wifi <ssid> [senha]' (sem 'gravar') tambem grava. 'wifi redes' exige o\n"
    "WiFi ligado ao menos uma vez neste boot.",
    c_wifi,
    "wifi\testou conectado? em que rede?\n"
    "wifi redes\tquais redes estao em volta\n"
    "wifi gravar \"Casa 2G\" minhasenha\tguarda uma rede com espaco no nome\n"
    "wifi conectar \"Casa 2G\"\ttroca para ela agora" },
  { "ble", "", G_NETWORK,
    "Bluetooth: estado, enlace do Mesh e aparelhos vistos",
    "ble\testado e aparelhos vistos\n"
    "ble varrer\tprocura por 6 s (rode 'ble' depois)\n"
    "ble ligar\tliga o Bluetooth (como o botao de Ajustes)\n"
    "ble desligar\tdesliga o Bluetooth (como o botao de Ajustes)",
    "Aparelhos Meshtastic aparecem marcados - sao os que o Mesh pode usar\n"
    "pelo Bluetooth.",
    c_ble,
    "ble varrer\tprocura; depois rode ble para ver a lista" },
  { "espnow", "", G_NETWORK,
    "ESP-NOW: estado, canal e envio de teste",
    "espnow\tfirmware do C6, canal real, MAC e contadores\n"
    "espnow canal <0-13>\t0 = segue o roteador; 1-13 = fixo\n"
    "espnow enviar <texto>\tdifunde uma mensagem",
    "Dois aparelhos so se ouvem no mesmo canal. O canal mostrado e lido do C6\n"
    "agora, nao o que o servico acha. Exige o firmware proprio do C6.",
    c_espnow,
    "espnow canal 1\tfixa no canal 1 (o mesmo do outro aparelho)\n"
    "espnow enviar ola Cardputer\tmensagem de teste" },
  { "c6", "", G_NETWORK,
    "coprocessador C6: firmware, MAC e gravacao pelo enlace",
    "c6\tversao do firmware e MACs\n"
    "c6 gravar\tcomo gravar firmware novo no C6 pelo enlace (sem abrir)\n"
    "c6 gravar sim\tgrava apps/Sistema/tab5_c6.bin no C6",
    "Exige o WiFi ligado. A gravacao confere a imagem antes, e o C6 volta\n"
    "sozinho para a anterior se a nova nao for confirmada.",
    c_c6,
    "c6\tqual firmware esta no C6" },
  { "farejo", "", G_NETWORK,
    "modo promiscuo no C6: quem esta no ar em volta",
    "farejo\testado e os MACs ouvidos (RSSI, tipo, quadros)\n"
    "farejo on [lote_ms] [mask_hex]\tliga; mask = canais a saltar (solte o roteador)\n"
    "farejo off\tdesliga\n"
    "farejo info\tcapacidades de radio do C6 gravado (CSI/FTM/802.15.4)\n"
    "farejo limpar\tzera a tabela",
    "O C6 junta por MAC e manda em lotes - o P4 so recebe o resumo. Deixa o\n"
    "receptor ligado enquanto ativo; desliga sozinho se a tela que usa fechar.\n"
    "Saltar varios canais exige o WiFi solto do roteador ('espnow canal 1').",
    c_sniffer,
    "farejo info\teste C6 sabe CSI/FTM/802.15.4?\n"
    "farejo on\tcomeca a ouvir no canal atual\n"
    "farejo on 200 0x1452\tlote 200 ms, salta canais 1/6/11/13 (solte o roteador antes)" },

  { "ping", "", G_NETWORK,
    "o WiFi passa dados? a internet responde?",
    "ping\troteador e 8.8.8.8, 4 pacotes cada\n"
    "ping <ip|nome>\tum alvo so",
    "WiFi 'conectado' so diz que o C6 esta associado ao roteador. Se o roteador\n"
    "nao responde, o caminho de dados (C6/SDIO) parou; se so a internet nao\n"
    "responde, o problema esta depois do roteador.",
    c_ping,
    "ping\tos dois de uma vez\n"
    "ping mqtt.meshtastic.org\to servidor do MQTT do Mesh" },
  { "radio", "", G_NETWORK,
    "teste dos extras do C6: CSI, FTM, 802.15.4",
    "radio csi [s]\tliga o CSI por s segundos (10), so do roteador, pingando-o a 20 Hz como\n"
    "\tfonte; conta os records por formato (11g/HT/HE) e mostra a medida de movimento\n"
    "radio 154 [canal] [s]\touve o 802.15.4 no canal (11..26, padrao 15); pinga o roteador\n"
    "\tantes, durante e depois para provar que o WiFi segue passando dados\n"
    "radio ftm\tmede a distancia ate o roteador conectado por FTM",
    "Ainda nao ha app que use estes dados: o comando prova que a gravacao do\n"
    "C6 liga, recebe e desliga cada recurso sem derrubar o C6 nem o WiFi.\n"
    "Depois de cada teste, 'farejo info' deve mostrar o C6 de pe (no ar sem\n"
    "zerar). FTM so mede distancia se o roteador falar FTM (802.11mc).",
    c_radio,
    "radio csi\tCSI por 10 s: perto de 100 records (1 a cada 100 ms)\n"
    "radio 154 15 20\t802.15.4 no canal 15 por 20 s: os tres pings em 4/4\n"
    "radio ftm\to caminho do FTM ate o roteador" },

  // --- MESHTASTIC ---
  { "mesh", "", G_MESH,
    "Meshtastic: enlace, nos, mensagens e envio",
    "mesh\tficha completa: enlace, saude, nos e mensagens\n"
    "mesh nos\tlista curta: numero e nome de cada no\n"
    "mesh msgs [N]\tultimas N mensagens de todas as conversas\n"
    "mesh enviar <texto>\tdifunde no canal principal (LongFast)\n"
    "mesh enviar para <no> <texto>\tmensagem direta (numero hex ou nome curto)\n"
    "mesh casa [lat lon]\tmostra ou grava a posicao de referencia\n"
    "mesh pausar\tpara de falar com o radio (o 5V continua)\n"
    "mesh retomar\tvolta a falar com ele",
    "Atalhos: 'nos' e 'enviar ...' funcionam sem o 'mesh' na frente. 'msgs'\n"
    "mostra o tamanho em bytes de cada texto, para separar mensagem cortada na\n"
    "captura de cortada no desenho. 'pausar' isola se um problema vem do\n"
    "software ou da posicao do modulo. Mensagem direta para no que veio pela\n"
    "internet (via MQTT) e recusada: por la so vai mensagem de canal.",
    c_mesh,
    "mesh nos\tacha o nome curto antes de mandar direta\n"
    "mesh enviar bom dia a todos\tmensagem no canal principal\n"
    "mesh enviar para RP05 oi\tdireta ao no de nome curto RP05\n"
    "mesh enviar para ACA97F24 oi\tdireta, pelo numero do no\n"
    "mesh msgs 50\tas 50 mensagens mais recentes\n"
    "mesh casa 0.0 0.0\tgrava a posicao de casa" },

  // --- BANCADA ---
  { "grove", "", G_BANCADA,
    "porta Grove: o que esta plugado e quem a usa",
    "grove\tmodulo detectado, energia e dono da porta\n"
    "grove trocar\tsonda de novo depois de trocar o modulo\n"
    "grove cru <baud>\t2,5 s de bytes crus (ex.: grove cru 115200)",
    "'grove' so relata: sondar com o Mesh no ar embaralharia a UART. 'trocar'\n"
    "para o servico, sonda e sobe o que couber - feche antes o app que usa o Grove.",
    c_grove,
    "grove cru 9600\tum GPS esta falando no cabo?\n"
    "grove trocar\tdepois de plugar outro modulo" },
  { "i2c", "", G_BANCADA,
    "procura dispositivos I2C num par de pinos",
    "i2c <sda> <scl>\tvarre os enderecos (ex.: i2c 49 50)",
    "Para identificar hardware novo. Teste o par nos dois sentidos (49 50 e\n"
    "50 49). So roda na tela inicial: o barramento externo e o mesmo do GPS e\n"
    "do teclado.",
    [](int c, char** v) { cmd_i2c(c, v); },
    "i2c 49 50\tprocura no conector lateral\n"
    "i2c 50 49\to mesmo par, invertido" },
  { "camera", "", G_BANCADA,
    "diagnostico de cor da camera (AWB, CCM, preto)",
    "camera\tmede um quadro: centro, sombras, estourado, magenta\n"
    "camera reg <hex> [valor]\tle ou escreve um registrador do sensor",
    "Liga a camera se preciso e espera 2,5 s o AE/AWB assentar. Com a lente\n"
    "tampada, 'centro' mostra o preto do sensor. 'reg' exige o app Camera aberto.",
    cmd_camera,
    "camera reg 3908\tle o alvo do preto do sensor" },
  { "foto", "", G_BANCADA,
    "tira uma foto e grava no cartao",
    "foto\tquadro inteiro em /sdcard/apps/Console/console.bmp",
    "Funciona com o app Camera fechado ou aberto - prova que a camera e um\n"
    "servico, independente do app.",
    [](int, char**) { cmd_photo(); },
    "foto\tdepois: lista apps/Console" },

  // --- TESTES ---
  { "concorrencia", "", G_TESTES,
    "segundo leitor de GPS+IMU, fora de qualquer app",
    "concorrencia\testa rodando? quantas leituras\n"
    "concorrencia iniciar\tliga o leitor (uma linha por segundo)\n"
    "concorrencia parar\tdesliga",
    "Abra o IMU junto e compare os numeros: os dois leitores\n"
    "devem ver sempre o mesmo valor, nunca um meio atualizado.",
    cmd_concorrencia,
    "concorrencia iniciar\tdepois abra o IMU e compare" },
  { "simulacao", "", G_TESTES,
    "tres apps de mentira pedindo GPS e IMU juntos",
    "simulacao\testado e leituras de cada um\n"
    "simulacao iniciar\tmapa (GPS/200ms), nivel (IMU), log (GPS+IMU/1s)\n"
    "simulacao parar\tpara os tres",
    "Teste mais forte que 'concorrencia': tres cadencias diferentes, cada uma\n"
    "na sua tarefa, como apps reais abertos ao mesmo tempo.",
    cmd_simulacao,
    "simulacao iniciar\tdepois abra apps de verdade e compare" },

  // --- out of list: old help and shortcuts ---
  { "help",   "ajuda ?", G_OCULTO, "", "", "", c_ajuda },
  { "app",    "", G_OCULTO, "", "", "", c_app },
  { "redes",  "", G_OCULTO, "", "", "", c_redes },
  { "nos",    "", G_OCULTO, "", "", "", c_nodes },
  { "enviar", "", G_OCULTO, "", "", "", c_send },
};
static const int N_COMANDOS = (int)(sizeof(COMANDOS) / sizeof(COMANDOS[0]));

static bool matches_command_name(const command_t& c, const char* s)
{
  if (equals_ignore_case(c.name, s)) { return true; }
  // Nicknames: words separated by space.
  const size_t n = strlen(s);
  for (const char* a = c.apelidos; *a; ) {
    while (*a == ' ') { ++a; }
    const char* end = a;
    while (*end && *end != ' ') { ++end; }
    if ((size_t)(end - a) == n && n && strncasecmp(a, s, n) == 0) { return true; }
    a = end;
  }
  return false;
}

static const command_t* find_command(const char* s)
{
  for (int i = 0; i < N_COMANDOS; ++i) { if (matches_command_name(COMANDOS[i], s)) { return &COMANDOS[i]; } }
  return nullptr;
}

// Text of several lines, each with indentation.
static void imprimir_recuado(const char* t, const char* recuo)
{
  while (*t) {
    const char* end = strchr(t, '\n');
    const int n = end ? (int)(end - t) : (int)strlen(t);
    printf("%s%.*s\n", recuo, n, t);
    t += n + (end ? 1 : 0);
  }
}

// "left\tright" lines in two columns. The left that doesn't fit in
// coluna stays sozinha, e a explicacao desce para a line of bottom, alinhada -
// cutting one path of file in middle would ruin the example for copying.
#define AJUDA_COLUNA 32
static void imprimir_colunas(const char* text, const char* prefix)
{
  const int larg = AJUDA_COLUNA - (int)strlen(prefix);
  for (const char* t = text; t && *t; ) {
    const char* end = strchr(t, '\n');
    const int n = end ? (int)(end - t) : (int)strlen(t);
    const char* tab = (const char*)memchr(t, '\t', (size_t)n);
    if (!tab) {
      printf("  %s%.*s\n", prefix, n, t);
    } else {
      const int a = (int)(tab - t);
      if (a <= larg) { printf("  %s%-*.*s %.*s\n", prefix, larg, a, t, n - a - 1, tab + 1); }
      else           { printf("  %s%.*s\n  %*s %.*s\n", prefix, a, t, AJUDA_COLUNA, "", n - a - 1, tab + 1); }
    }
    t += n + (end ? 1 : 0);
  }
}

// As formas of chamar e, logo abaixo, exemplos prontos para copiar.
static void print_usage(const command_t& c)
{
  imprimir_colunas(c.usage, "");
  imprimir_colunas(c.exemplos, "ex: ");
}

static void show_usage(const char* name)
{
  const command_t* c = find_command(name);
  if (!c) { return; }
  printf("uso:\n");
  print_usage(*c);
}

static void ajuda_comando(const command_t& c)
{
  printf("%s - %s\n\n", c.name, c.summary);
  print_usage(c);
  printf("\n");
  imprimir_recuado(c.detail, "  ");
  if (c.apelidos[0]) { printf("  (tambem aceito como: %s)\n", c.apelidos); }
}

// Comando desconhecido: sugere os que comecam igual ou contains o que foi
// typed. Getting a letter wrong cannot result in "read the entire list".
static void sugerir(const char* s)
{
  const size_t n = strlen(s);
  int achados = 0;
  for (int i = 0; i < N_COMANDOS; ++i) {
    const command_t& c = COMANDOS[i];
    if (c.group == G_OCULTO) { continue; }
    const bool parecido = (n >= 2 && strncasecmp(c.name, s, 2) == 0) ||
                          (n >= 3 && strcasestr(c.name, s) != nullptr);
    if (parecido) { printf(achados++ ? ", %s" : "  talvez: %s", c.name); }
  }
  if (achados) { printf("\n"); }
  printf("  'help' lista todos os comandos\n");
}

static void c_ajuda(int argc, char** argv)
{
  if (argc >= 2 && equals_ignore_case(argv[1], "app")) {
    const app_t* a = nullptr;
    for (int i = 0; argc >= 3 && i < SISTEMA_NUM_APPS; ++i) {
      if (name_of_app(SISTEMA_APPS[i]->name, argv[2])) { a = SISTEMA_APPS[i]; }
    }
    ajuda_apps(a);
    return;
  }
  if (argc >= 2 && !equals_ignore_case(argv[1], "tudo")) {
    const command_t* c = find_command(argv[1]);
    if (!c || c->group == G_OCULTO) {
      printf("nao ha comando '%s'\n", argv[1]);
      sugerir(argv[1]);
      return;
    }
    ajuda_comando(*c);
    return;
  }
  const bool everything = (argc >= 2);

  // Grouped by SUBJECT, with ALL forms of each command in view. One
  // line so por comando escondia justamente o que se procura ("como mando
  // message?" ficava atras of 'help mesh').
  printf("M5tabOS %s - console serial\n", esp_app_get_description()->version);
  for (int g = 0; g < G_N; ++g) {
    printf("\n%s\n", GROUP_TITLES[g]);
    if (g == G_APPS) { ajuda_apps(nullptr); continue; }
    bool first = true;
    for (int i = 0; i < N_COMANDOS; ++i) {
      const command_t& c = COMANDOS[i];
      if (c.group != g) { continue; }
      if (!first) { printf("\n"); }   // one respiro entre commands of grupo
      first = false;
      if (everything) { ajuda_comando(c); }
      else      { print_usage(c); }
      yield(i);
    }
  }
  printf("\n'help <comando>' explica para que serve cada um; 'help tudo' explica todos.\n"
         "Caminhos do cartao podem ser relativos a /sdcard. Maiusculas tanto faz.\n");
}

// ---------------------------------------------------------------------------
// Own offline player, WITHOUT linenoise
// ---------------------------------------------------------------------------
//
// The first version used esp_console_new_repl_uart(), and you couldn't type:
// nothing appeared on screen. The reason is in linenoise itself, in
// components/console/linenoise/linenoise.c:1191 - no modo "terminal burro" ele
// it only prints the prompt and DOES NOT echo what you type; the comment is explicit
// ("Only erase symbol echoed from stdin"), ou seja, ele conta com o eco local
// ofterminal. The idf.py monitor does not echo locally, it forwards raw keys.
// Result: you type blindly.
//
// Furthermore, whether or not you fall into dumb mode depends on a terminal probe that
// may give different results on each monitor. I preferred to eliminate the variable:
// este loop le byte a byte e ECOA ELE MESMO, entao funciona igual em qualquer
// terminal - idf.py monitor, screen, minicom, o que for.

static void executar(char* line)
{
  // Break into words, respecting quotation marks for passwords with spaces.
  //
  // 24 AND NOT 8: the "mesh send to <no> <text>" already uses four positions, and
  // com 8 o text era truncado em silencio in quinta palavra. 24 cobre a line
  // inteira: com 192 caracteres of entrada e o limit of 160 of MESH_TEXTO, o
  // text acaba antes das palavras. Custa 64 bytes of stack.
  char* argv[24];
  int argc = 0;
  char* p = line;
  while (*p && argc < 24)
  {
    while (*p == ' ') { ++p; }
    if (*p == 0) { break; }
    if (*p == '"')
    {
      argv[argc++] = ++p;
      while (*p && *p != '"') { ++p; }
    }
    else
    {
      argv[argc++] = p;
      while (*p && *p != ' ') { ++p; }
    }
    if (*p) { *p++ = 0; }
  }
  if (argc == 0) { return; }

  const command_t* c = find_command(argv[0]);
  if (!c) {
    printf("comando desconhecido: '%s'\n", argv[0]);
    sugerir(argv[0]);
    return;
  }
  c->fn(argc, argv);
}


static void vTaskConsole(void*)
{
  char line[192];
  size_t n = 0;

  printf("\nconsole pronto - digite help\nm5tab> ");
  fflush(stdout);

  for (;;)
  {
    // BLOQUEIA DE VERDADE, e isso e economia of energia.
    //
    // Era assim:
    //
    //     int c = fgetc(stdin);
    //     if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    //
    // fgetc on VFS returns EOF when there are no bytes, so the task
    // acordava 50 VEZES POR SEGUNDO, para sempre, mesmo em sleep e mesmo com
    // no one connected in serial. Each iteration is an exchange of context and a
    // query on driver to discover that there is nothing.
    //
    // O usb_serial_jtag_read_bytes com portMAX_DELAY dorme no semaphore of
    // driver: zero awake until key arrives. And the same path as the
    // comando `receber` usa, e ele passa on largo of traducao of end of line
    // of VFS - which is not necessary here, because the loop below already handles CR and LF.
    uint8_t byte = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) { continue; }
#else
    if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) { continue; }
#endif
    const int c = (int)byte;

    // Diagnosis while the console is not reliable: shows the code of
    // EVERY byte that arrives. It serves to distinguish two very
    // different - "the monitor does not forward the keyboard" (nothing appears here) of
    // "chega, mas o filtro descarta" (aparece o code e nada mais).
    ESP_LOGD(TAG, "rx: 0x%02x", (unsigned)c);

    if (c == '\r' || c == '\n')
    {
      printf("\n");
      line[n] = 0;
      if (n > 0) { executar(line); }
      n = 0;
      printf("m5tab> ");
      fflush(stdout);
    }
    else if (c == 8 || c == 127)          // backspace
    {
      if (n > 0) { --n; printf("\b \b"); fflush(stdout); }
    }
    else if (c >= 32 && c < 127 && n < sizeof(line) - 1)
    {
      line[n++] = (char)c;
      fputc(c, stdout);                   // O ECO QUE FALTAVA
      fflush(stdout);
    }
  }
}

void console_start(void)
{
  // O driver TEM que ser o of mesmo periferico que o console primario, senao o
  // stdin stays preso num canal onde ninguem digita - foi exatamente o erro que
  // deixou o console mudo (ver o comentario no sdkconfig.defaults).
  // Therefore, the choice is made by the console key itself, and is not fixed.
  //
  // In both cases: without a driver installed, reading of stdin DOES NOT block (the
  // fgetc devolveria EOF para sempre e o loop viraria espera ocupada), e e o
  // driver que permite read key a key em vez of line inteira.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  // LARGE RX RING, and this is NOT preventive clearance - it is correction.
  //
  // O default of IDF e 256 bytes (USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT), e a
  // ISR of driver does it like this (usb_serial_jtag.c:150):
  //
  //     xRingbufferSendFromISR(rx_ring_buf, buf, rx_fifo_len, &xTaskWoken);
  //
  // O RETORNO E IGNORADO. Anel cheio = bytes descartados em silencio, sem erro,
  // sem log, sem nada. Para digitar comando ninguem percebe; mandando 1,4MB of
  // firmware pelo `receber`, sumiram 2432 bytes no middle e a transferencia
  // morreu a 99,8%.
  //
  // 8KB dao ~30ms of folga a velocidade of USB - time of sobra para o loop of
  // leitura voltar mesmo que o escalonador atrase.
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = 8192;
  cfg.tx_buffer_size = 2048;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK)
  {
    ESP_LOGE(TAG, "nao consegui instalar o driver do USB-Serial-JTAG");
    return;
  }
  usb_serial_jtag_vfs_use_driver();
  // Enter chega como CR; sem a conversao a line nunca terminaria. A saida vai
  // like CRLF to not exit on stairs.
  usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#else
  if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK)
  {
    ESP_LOGE(TAG, "nao consegui instalar o driver da UART0");
    return;
  }
  uart_vfs_dev_use_driver(UART_NUM_0);
  uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
  uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
#endif

  // stdin sem buffer. O newlib bufferiza por line por default, e ai o fgetc
  // pode ficar esperando encher o buffer em vez of return a key in hora -
  // the echo would only appear after Enter, or it would not appear at all. Every example of
  // console of IDF does this.
  setvbuf(stdin, NULL, _IONBF, 0);

  // 12KB, not 4096. 4096 BROKE - "send" crashed the device with
  // Stack protection fault, e o retrato of tarefas ja avisava: 372 B livres.
  //
  // Esta tarefa tem o pior caso of sistema inteiro, porque of one comando ela
  // alcanca QUALQUER subsistema. So no caminho of "send":
  //
  //   executar()          ~1,3KB of buffers of comando (sub, cam, complete...)
  //   tab5_mesh_enviar() 656B - data[176]+pkt[224]+frame[256], all alive
  //   guardar_msg()        -> FATFS, que e faminto of stack
  //   printf with float -> vfprintf of newlib, more than 1KB alone
  //
  // The "card" and the "c6 ota" go down equally deep. Size by case
  // Typical here is to scale for the day when the hard command is not used.
  //
  // MEASURED: two days of batteries-*.csv with normal use of console, peak of
  // ~4.0KB (minimum free 8320 of 12288). 8KB and double the pico - clearance for
  // the deep commands that don't appear every day - and return 4KB to RAM
  // interna. Se one comando novo lower mais, o pilhas-*.csv mostra.
  xTaskCreatePinnedToCore(vTaskConsole, "console", 8192, nullptr, 1, nullptr, tskNO_AFFINITY);
  ESP_LOGI(TAG, "console no ar - digite 'help' no monitor");
}
