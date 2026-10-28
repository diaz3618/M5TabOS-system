#include "console.hpp"
#include "tab5_hora.hpp"   // tab5_agora_ms

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
#include "tab5_quadro.hpp"
#include "tab5_bmp.hpp"
#include "tab5_camera.hpp"
#include "tab5_camera_ipa.h"
#include "hal/efuse_hal.h"
#include "esp_heap_caps.h"   // heap_caps_malloc - buffer temporario de "mesh msgs"
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
#include "tab5_pendrive.hpp"
#include "tab5_farejo.hpp"
#include "tab5_csi.hpp"
#include "tab5_sd.hpp"
#include "tab5_teclado.hpp"
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
#include "registro.h"
#include "tab5_log.hpp"
#include "ajustes.h"
#include "tab5_power.hpp"
#include "tab5_ble.hpp"
#include "tab5_mesh_ble.hpp"
#include "tela_virtual.hpp"
#include "compositor.hpp"
#include "tab5_glifos.hpp"
#include "internal.hpp"   // sistema_trocar_relogio_painel
#include "esp_pm.h"
#include "tab5_barramento.hpp"
#include "tab5_tela.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_extrato



// ---------------------------------------------------------------------------
// 'tela medir' / 'tela' - O CUSTO DOS QUADROS NUMA JANELA
// ---------------------------------------------------------------------------
// O compositor so acumula (comp_estat); a janela e daqui: 'tela medir' guarda
// a foto de agora, e 'tela' mostra a diferenca ate o instante da leitura. O
// monitor tem a foto dele - um nao apaga a janela do outro.
static comp_estat_t s_medir_base = {};
static int64_t      s_medir_desde = 0;

static void tela_medir_marcar(void)
{
  comp_estat(&s_medir_base);
  s_medir_desde = esp_timer_get_time();
  printf("medida zerada - use o aparelho (role, abra apps) e rode 'tela'\n");
}

static void tela_medir_mostrar(void)
{
  if (s_medir_desde == 0) { printf("\n  (rode 'tela medir' para abrir uma janela de medida)\n"); return; }
  comp_estat_t a;
  comp_estat(&a);
  const comp_estat_t& b = s_medir_base;
  const double seg = (double)(esp_timer_get_time() - s_medir_desde) / 1e6;
  const uint32_t q = a.quadros - b.quadros;
  // Cada pixel copiado por DMA e lido (2 B) e escrito (2 B) na PSRAM. Por
  // isso x4. "desenhado" e o que a CPU pintou direto na pagina (so escrita).
  const double mb_d  = (double)(a.px_quadro    - b.px_quadro)    * 2.0 / 1e6;
  const double mb_s  = (double)(a.px_sinc      - b.px_sinc)      * 4.0 / 1e6;
  const double mb_rd = (double)(a.px_rolar_dma - b.px_rolar_dma) * 4.0 / 1e6;
  auto ms_por = [&](uint64_t us) { return q ? (double)us / 1000.0 / q : 0.0; };
  printf("\n--- quadros (ultimos %.1f s) ---\n", seg);
  printf("  quadros                   : %lu (%.1f q/s)  cheios(>=90%%): %lu  lista estourou: %lu\n",
         (unsigned long)q, seg > 0 ? q / seg : 0.0,
         (unsigned long)(a.cheios - b.cheios), (unsigned long)(a.estouros - b.estouros));
  printf("  desenhado direto na pagina: %.1f MB\n", mb_d);
  printf("  copia por DMA2D           : sincronia %.1f MB + rolagem %.1f MB = %.1f MB/s\n",
         mb_s, mb_rd, seg > 0 ? (mb_s + mb_rd) / seg : 0.0);
  printf("  rolagem (DMA ou CPU)      : %.2f ms/quadro\n", ms_por(a.us_rolar - b.us_rolar));
  printf("  por quadro (media)        : desenho %.2f ms, copia %.2f ms, espera da troca %.2f ms\n",
         ms_por(a.us_desenho - b.us_desenho), ms_por(a.us_copia - b.us_copia),
         ms_por(a.us_espera - b.us_espera));
  {
    // O desenho separado: passada COMPLETA (abrir app, home) e o resto
    // (rolagem, um valor mudou). Problemas diferentes, remedios diferentes.
    const uint32_t nc = a.n_desenho_completo - b.n_desenho_completo;
    const uint32_t nt = a.n_desenho - b.n_desenho;
    const uint64_t uc = a.us_desenho_completo - b.us_desenho_completo;
    const uint64_t ut = a.us_desenho - b.us_desenho;
    const uint32_t np = nt - nc;
    printf("  desenho por passada       : completa %.2f ms (%lu)  parcial %.2f ms (%lu)\n",
           nc ? (double)uc / 1000.0 / nc : 0.0, (unsigned long)nc,
           np ? (double)(ut - uc) / 1000.0 / np : 0.0, (unsigned long)np);
  }
  {
    sistema_passada_t ps[24];
    const int n = sistema_passadas_completas(ps, 24);
    if (n > 0) {
      printf("  passada completa por app (desde o boot, ms medios):\n");
      printf("    %-12s %5s %8s %8s %8s %8s %8s\n", "app", "vezes", "total", "pior", "barra", "limpar", "conteudo");
      for (int i = 0; i < n; ++i) {
        const sistema_passada_t& p = ps[i];
        if (!p.n) { continue; }
        auto m = [&](uint64_t us) { return (double)us / 1000.0 / p.n; };
        printf("    %-12.12s %5lu %8.1f %8.1f %8.1f %8.1f %8.1f\n", p.nome ? p.nome : "?",
               (unsigned long)p.n, m(p.us), (double)p.max_us / 1000.0,
               m(p.us_barra), m(p.us_limpar), m(p.us_conteudo));
      }
    }
  }
  {
    glifos_estat_t g;
    tab5_glifos_estat(&g);
    const uint32_t tot = g.acertos + g.faltas + g.recusas;
    printf("  cache de glifos (desde o boot): %lu%% da imagem guardada; %lu acertos, %lu faltas, %lu pelo caminho original\n",
           (unsigned long)(tot ? (uint64_t)g.acertos * 100 / tot : 0),
           (unsigned long)g.acertos, (unsigned long)g.faltas, (unsigned long)g.recusas);
    printf("                              %lu letras guardadas, %lu KB, recomecou %lu vez(es)\n",
           (unsigned long)g.entradas, (unsigned long)(g.bytes / 1024), (unsigned long)g.limpezas);
    printf("                              caminho original por motivo: espaco %lu, sem fundo %lu, invade %lu,\n"
           "                              destino %lu, escala %lu, outro %lu\n",
           (unsigned long)g.rec_espaco, (unsigned long)g.rec_sem_fundo, (unsigned long)g.rec_invade,
           (unsigned long)g.rec_destino, (unsigned long)g.rec_escala, (unsigned long)g.rec_outro);
  }
  static const char* const FAIXA[COMP_FAIXAS] = { "1", "2", "3", "4+" };
  printf("  volta com quadro (media)  : %.2f ms   em periodos do painel:", ms_por(a.us_volta - b.us_volta));
  for (int f = 0; f < COMP_FAIXAS; ++f) {
    printf(" %s:%lu", FAIXA[f], (unsigned long)(a.voltas[f] - b.voltas[f]));
  }
  printf("\n");
}

static const char *TAG = "console";

// CEDE A CPU NAS LISTAGENS LONGAS. O console tem prioridade acima da IDLE, e
// imprimir uma lista grande sem nunca bloquear prende o nucleo: com ~200 nos
// do Mesh vindos pelo MQTT, o 'mesh' segurou o nucleo 0 por mais de 5 s e o
// watchdog reclamou da IDLE0. Um tick a cada 8 itens nao se nota na tela e
// deixa a IDLE (e quem mais estiver esperando) rodar.
static void ceder(int i)
{
  if ((i & 7) == 7) { fflush(stdout); vTaskDelay(1); }
}

// ---------------------------------------------------------------------------
// Comandos dos apps
// ---------------------------------------------------------------------------
// A ideia: o que o app mostra na tela, o console mostra em texto. Nao e
// duplicacao - e o mesmo dado por dois caminhos, e o do console e o unico que
// funciona com o aparelho na bancada, sem ninguem olhando para a tela.

// ---------------------------------------------------------------------------
// RECONHECIMENTO DO C6
// ---------------------------------------------------------------------------
// SOMENTE LEITURA. Nao muda nada no coprocessador - so pergunta quem ele e.
//
// Existe por causa de uma linha que aparece no boot desde sempre:
//
//   esp-hosted fw versions: host=3.0.6 coprocessor=0.0.0
//
// Um coprocessador que se declara 0.0.0 ou nao preenche o campo, ou nao e o
// firmware esp_hosted de referencia. Isso importa por dois motivos concretos:
//
//   1. E o suspeito do RSSI=0 que o app Ajustes ja trata como "indisponivel";
//   2. E decide se da para rodar ESP-NOW no C6. O ESP-NOW nao existe pelo
//      esp_hosted (esp_now_init nao linka neste alvo - conferido), entao a
//      unica via seria firmware proprio no C6 usando o canal peer_data. Antes
//      de cogitar substituir o firmware que HOJE faz o WiFi funcionar, e
//      preciso saber o que esta la.
//
// O app descriptor traz nome do projeto, versao, IDF usado e data da
// compilacao - e o suficiente para dizer se e um build da M5Stack ou o
// esp_hosted de fabrica.
static void cmd_c6(void)
{
  if (!tab5_wifi_c6_enlace()) {
    printf("o enlace com o C6 nao esta de pe.\n"
           "ligue o WiFi no app Ajustes (ou o ajuste) e tente de novo -\n"
           "sem esp_hosted_init nao ha a quem perguntar.\n");
    return;
  }

  printf("--- coprocessador C6 ---\n");

  eh_host_coprocessor_fwver_t v = { };
  if (eh_host_sys_get_cp_fw_version(&v) == ESP_OK) {
    printf("  firmware   : %d.%d.%d  (rev %d, build %d)\n",
           (int)v.major1, (int)v.minor1, (int)v.patch1,
           (int)v.revision, (int)v.build);
  } else {
    printf("  firmware   : nao respondeu\n");
  }

  // SO A VERSAO VIAJA.
  //
  // O esp_hosted_app_desc_t tem project_name, date, time e idf_ver, e a
  // primeira versao deste comando imprimia os quatro. Vem vazios, e o
  // proprio header diz por que:
  //
  //   /* App descriptor; wire carries version + idf_target only. */
  //
  // Os campos existem no struct porque ele espelha o esp_app_desc_t do IDF,
  // mas o RPC nao transporta o resto. Imprimir campo que nunca chega e pior que
  // nao imprimir: parece defeito do coprocessador.
  esp_hosted_app_desc_t d = { };
  if (eh_host_sys_get_cp_app_desc(&d) == ESP_OK) {
    printf("  versao app : %.*s\n", (int)sizeof(d.version), d.version);
  } else {
    printf("  versao app : nao respondeu\n");
  }


  // O MAC e a IDENTIDADE no ESP-NOW - la nao existe nome, so endereco de 6
  // bytes. Se um dia o ESP-NOW rodar no C6, e este numero que o outro aparelho
  // vai cadastrar como par.
  uint8_t mac[6] = { };
  if (eh_host_sys_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    printf("  MAC (STA)  : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  if (eh_host_sys_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
    printf("  MAC (AP)   : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  // Aviso por ULTIMO, depois dos dados. Ele estava no meio do relatorio e
  // partia a lista em duas - os MACs apareciam depois de um bloco de alerta,
  // como se fossem parte dele.
  //
  // A tabela do docs/migration.md do esp_hosted:
  //   host 3.x + coprocessador 3.x    Suportado
  //   host 3.x + coprocessador 1.x    Retrocompativel, atualizacao RECOMENDADA
  //   host < 3.x + coprocessador 3.x  Nao suportado
  if (v.major1 != 0 && v.major1 < 3) {
    printf("\n  ATENCAO: host 3.x com coprocessador %d.x.\n"
           "  Combinacao retrocompativel, mas o esp_hosted RECOMENDA atualizar\n"
           "  o coprocessador. E o principal suspeito do RSSI=0 que o app Ajustes ja\n"
           "  trata como indisponivel.\n", (int)v.major1);
  }
}

// ---------------------------------------------------------------------------
// RECEBER ARQUIVO PELA SERIAL
// ---------------------------------------------------------------------------
//   receber <app> <arquivo> <bytes>
//
// Nasceu de uma necessidade concreta: mandar 1,4MB de firmware do C6 para o
// cartao sem tirar o cartao do aparelho.
//
// TRES DECISOES QUE NAO SAO OBVIAS:
//
// 1. LE DIRETO DO DRIVER, nao do stdin. O console roda com
//    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR), que traduz
//    fim de linha na entrada - inofensivo para texto, DESTRUTIVO para binario:
//    todo 0x0D viraria outra coisa. O usb_serial_jtag_read_bytes() passa ao
//    largo do VFS e entrega o byte como veio.
//
// 2. GUARDA TUDO NA PSRAM ANTES DE ESCREVER NO CARTAO. A serial nao tem
//    controle de fluxo: se a gente parar para escrever no SD - e o FATFS para,
//    as vezes por centenas de milissegundos, quando fecha um cluster - o buffer
//    de recepcao enche e os bytes se perdem em silencio. Com 32MB de PSRAM,
//    receber inteiro e depois gravar desacopla as duas coisas por completo.
//
// 3. NAO IMPRIME NADA DURANTE A RECEPCAO. O jeito natural de mandar o arquivo e
//    "cat arquivo > /dev/cu.usbmodem*", e nesse arranjo NINGUEM esta lendo a
//    saida. Se a gente escrever, o buffer de TX enche e a escrita bloqueia -
//    parando justamente o loop que precisa consumir a entrada. Silencio ate o
//    fim e o que mantem o fluxo vivo.
#define RECEBER_MAX (6u * 1024u * 1024u)

// Tamanho legivel, num buffer estatico proprio. Estatico e nao do chamador
// porque ele aparece duas vezes na MESMA linha de printf em alguns pontos - e
// dois usos simultaneos do mesmo buffer se atropelariam. Por isso alterna
// entre dois.
static const char* tamanho_curto(uint64_t bytes)
{
  static char buf[2][16];
  static int  alt = 0;
  char* b = buf[alt = (alt + 1) & 1];
  if      (bytes >= 1024 * 1024) { snprintf(b, 16, "%.1f MB", bytes / (1024.0 * 1024.0)); }
  else if (bytes >= 1024)        { snprintf(b, 16, "%.1f KB", bytes / 1024.0); }
  else                           { snprintf(b, 16, "%llu B", bytes); }
  return b;
}

// Panorama do cartao: espaco e uma linha por pasta de aplicativo.
//
// O `lista` mostra UMA pasta e serve para procurar arquivo. Este responde outra
// pergunta, a que se faz primeiro: "o que existe aqui e quanto esta ocupado?".
static void cmd_cartao(void)
{
  {
    // O gravador do sistema (hal/tab5_sd.hpp): o que ainda espera, o que ja foi.
    uint32_t pend = 0, grav = 0, perd = 0;
    tab5_sd_anexos_estado(&pend, &grav, &perd);
    printf("gravador: %lu B esperando, %lu linha(s) gravadas, %lu perdida(s) desde o boot\n",
           (unsigned long)pend, (unsigned long)grav, (unsigned long)perd);
  }
  if (!tab5_sd_montar()) {
    printf("cartao nao montou (%s)\n", tab5_sd_descricao());
    return;
  }

  printf("--- cartao ---\n  %s\n", tab5_sd_descricao());

  // Espaco livre pelo FATFS. O statvfs do VFS nao e implementado para FAT no
  // IDF, entao vamos ao driver: clusters livres x setores por cluster.
  FATFS* fs = nullptr;
  DWORD livres = 0;
  if (f_getfree("0:", &livres, &fs) == FR_OK && fs) {
    // SETOR REAL, nao FF_MAX_SS. O FF_MAX_SS e o setor MAXIMO que o FATFS foi
    // compilado para aceitar (4096 no IDF); o do cartao e 512. Multiplicar pelo
    // maximo dava 8x a mais - um cartao de 14,5GB aparecia com 118GB.
    //
    // Quando FF_MAX_SS == FF_MIN_SS o campo ssize nem existe (o FATFS o remove,
    // porque so ha um tamanho possivel), dai o #if.
#if FF_MAX_SS != FF_MIN_SS
    const uint64_t setor = fs->ssize;
#else
    const uint64_t setor = FF_MAX_SS;
#endif
    const uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize * setor;
    const uint64_t liv = (uint64_t)livres * fs->csize * setor;
    printf("  %llu MB livres de %llu MB (%llu%% em uso)\n",
           liv / (1024 * 1024), tot / (1024 * 1024),
           tot ? ((tot - liv) * 100 / tot) : 0);
  }

  DIR* d = opendir(TAB5_SD_APPS);
  if (!d) {
    printf("  (nao ha %s ainda)\n", TAB5_SD_APPS);
    tab5_sd_desmontar();
    return;
  }

  printf("\n  %-16s %6s  %s\n", "pasta", "arqs", "tamanho");
  struct dirent* e;
  int k = 0;
  while ((e = readdir(d)) != nullptr) {
    ceder(k++);
    if (e->d_name[0] == '.') { continue; }

    char sub[288];   // TAB5_SD_APPS + '/' + ate 255 de d_name + nulo
    snprintf(sub, sizeof(sub), "%s/%s", TAB5_SD_APPS, e->d_name);

    // Conta e soma sem recursao: as pastas de app sao planas por convencao
    // (ver tab5_sd.hpp), e descer em arvore aqui so criaria caminho para
    // estourar stack num cartao que alguem encheu de subpastas.
    DIR* sd = opendir(sub);
    if (!sd) { continue; }

    // Guarda os nomes para imprimir DEPOIS do cabecalho da pasta - o total tem
    // que aparecer em cima, e so da para saber o total depois de percorrer.
    #define CARTAO_MOSTRA 10
    char nomes[CARTAO_MOSTRA][256];   // ate 255 de d_name + nulo
    uint64_t tams[CARTAO_MOSTRA];
    int n = 0; uint64_t bytes = 0;

    struct dirent* f2;
    while ((f2 = readdir(sd)) != nullptr) {
      if (f2->d_name[0] == '.') { continue; }
      char cam[560];   // sub + '/' + ate 255 de d_name + nulo
      snprintf(cam, sizeof(cam), "%s/%s", sub, f2->d_name);
      struct stat st;
      if (stat(cam, &st) != 0 || !S_ISREG(st.st_mode)) { continue; }
      if (n < CARTAO_MOSTRA) {
        snprintf(nomes[n], sizeof(nomes[0]), "%s", f2->d_name);
        tams[n] = (uint64_t)st.st_size;
      }
      ++n;
      bytes += (uint64_t)st.st_size;
    }
    closedir(sd);

    printf("  %-16s %6d  %s\n", e->d_name, n, tamanho_curto(bytes));

    // Os arquivos, indentados. Faltava exatamente isto: dava para ver que a
    // pasta Sistema tinha 3 arquivos e 1,5MB, e NAO dava para ver se o firmware
    // do C6 era um deles.
    const int mostrar = (n < CARTAO_MOSTRA) ? n : CARTAO_MOSTRA;
    for (int i = 0; i < mostrar; ++i) {
      printf("      %-28.28s %10s\n", nomes[i], tamanho_curto(tams[i]));
    }
    if (n > CARTAO_MOSTRA) {
      printf("      ... e mais %d\n", n - CARTAO_MOSTRA);
    }
  }
  closedir(d);
  tab5_sd_desmontar();
  printf("\n  'lista %s/<pasta>' mostra os arquivos de uma delas\n", TAB5_SD_APPS);
}

static void cmd_receber(int argc, char** argv)
{
  if (argc < 4) {
    printf("uso: receber <app> <arquivo> <bytes>\n"
           "  ex: receber Sistema tab5_c6.bin 1397904\n"
           "\n"
           "Depois de dar o comando, mande o arquivo CRU pela mesma porta.\n"
           "Do Mac, com o monitor FECHADO:\n"
           "  cat arquivo.bin > /dev/cu.usbmodem*\n"
           "\n"
           "Ou tudo de uma vez, sem precisar do monitor:\n"
           "  { printf 'receber Sistema tab5_c6.bin 1397904\\r'; sleep 2; \\\n"
           "    cat arquivo.bin; } > /dev/cu.usbmodem*\n");
    return;
  }

  const long tam = atol(argv[3]);
  if (tam <= 0 || (unsigned long)tam > RECEBER_MAX) {
    printf("tamanho invalido (1 a %u bytes)\n", (unsigned)RECEBER_MAX);
    return;
  }

  uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)tam, MALLOC_CAP_SPIRAM);
  if (!buf) { printf("sem PSRAM para %ld bytes\n", tam); return; }

  printf("mande %ld bytes agora (10 s de paciencia entre blocos)\n", tam);
  fflush(stdout);

  size_t lidos = 0;
  int vazios = 0;
  while (lidos < (size_t)tam) {
    const int n = usb_serial_jtag_read_bytes(buf + lidos, (size_t)tam - lidos,
                                             pdMS_TO_TICKS(1000));
    if (n > 0) { lidos += (size_t)n; vazios = 0; }
    else if (++vazios >= 10) { break; }   // desistiu de esperar
  }

  // GRAVA O QUE CHEGOU, mesmo incompleto. Jogar 1,4MB fora por causa dos
  // ultimos 2KB nao ajuda ninguem: com o arquivo no cartao e o md5 na tela,
  // da para ver SE faltou e QUANTO faltou. Quem decide se serve e voce - e o
  // `c6 gravar` confere o cabecalho antes de usar, de qualquer forma.
  const bool completo = (lidos == (size_t)tam);
  if (lidos == 0) {
    printf("\nnao chegou byte nenhum.\n");
    heap_caps_free(buf);
    return;
  }
  if (!completo) {
    printf("\nINCOMPLETO: %u de %ld bytes (faltaram %ld). Gravando assim mesmo\n"
           "para voce conferir - compare o md5 abaixo.\n",
           (unsigned)lidos, tam, tam - (long)lidos);
  }

  // MD5 para conferir contra o `md5 -q` do Mac. Um tamanho igual nao prova
  // nada: byte trocado no meio da serial da o mesmo tamanho.
  unsigned char md5[16];
  mbedtls_md5(buf, lidos, md5);

  bool ok = false;
  if (tab5_sd_montar()) {
    FILE* f = tab5_sd_abrir(argv[1], argv[2], "wb");
    if (f) {
      ok = (fwrite(buf, 1, lidos, f) == lidos);
      fclose(f);
    }
    tab5_sd_desmontar();
  }
  heap_caps_free(buf);

  if (!ok) { printf("\nrecebi tudo, mas a gravacao no cartao falhou\n"); return; }

  printf("\ngravado: /sdcard/apps/%s/%s  (%u bytes)%s\nmd5: ",
         argv[1], argv[2], (unsigned)lidos, completo ? "" : "  <- INCOMPLETO");
  for (int i = 0; i < 16; ++i) { printf("%02x", md5[i]); }
  printf("\n");
}

// LE O QUE O MONITOR JA MEDIU (tab5_monitor_foto), em vez de tirar duas fotos
// proprias de uxTaskGetSystemState: cada foto e uma secao critica longa (a que
// ja derrubou frame do painel) e o retrato pedia 2 x 40 TaskStatus_t de
// malloc interno. A foto do monitor e do ultimo segundo, com os donos.
static void cmd_tarefas(void)
{
  mon_foto_t* f = (mon_foto_t*)heap_caps_malloc(sizeof(mon_foto_t), MALLOC_CAP_SPIRAM);
  if (!f) { printf("sem memoria para o retrato\n"); return; }
  if (!tab5_monitor_foto(f)) { heap_caps_free(f); printf("monitor ainda sem a primeira amostra\n"); return; }

  // EM QUE ESTADO ISTO FOI MEDIDO. Sem esta linha a medida e ambigua: o mesmo
  // comando com a tela acesa e com ela apagada da numeros muito diferentes, e
  // nada no retrato dizia qual dos dois voce estava olhando. Digitar no serial
  // NAO acorda a tela (so o toque acorda), entao da para medir o repouso de
  // verdade - mas so se der para conferir que era repouso mesmo.
  printf("--- %d tarefas   [%s]   CPU %.1f%%   foto %lu us (pior %lu) ---\n", f->n,
         tab5_power_em_repouso() ? "EM REPOUSO - tela apagada" : "tela acesa",
         (double)f->cpu, (unsigned long)f->foto_us, (unsigned long)f->foto_pior_us);
  printf("  %-16s %3s %6s %6s %10s  %s\n", "nome", "pri", "nucleo", "CPU%", "pilha livre", "dono");

  for (int i = 0; i < f->n; ++i) {
    const mon_tarefa_t& t = f->t[i];
    // xCoreID vale tskNO_AFFINITY (0x7FFFFFFF) quando a tarefa roda nos dois -
    // testar por negativo nunca acerta, e imprimir cru mostra um numero
    // gigante. Foi um bug real na coluna CORE do app.
    char nucleo[8];
    if (t.core == tskNO_AFFINITY) { snprintf(nucleo, sizeof(nucleo), "0e1"); }
    else                          { snprintf(nucleo, sizeof(nucleo), "%d", (int)t.core); }
    printf("  %-16s %3u %6s %6.1f %8u B  %s\n",
           t.nome, (unsigned)t.prioridade, nucleo, (double)t.pct,
           (unsigned)t.pilha_livre, t.dono);
  }
  // A CONTA E DO APARELHO, NAO POR NUCLEO: as porcentagens somam 100% (os
  // dois nucleos juntos), a mesma conta do app Tarefas. Os dois IDLE juntos
  // sao a ociosidade do aparelho; um nucleo inteiro ocupado aparece como 50%.
  printf("\n  CPU%% do aparelho (os dois nucleos somam 100%%): um nucleo cheio = 50%%.\n");
  heap_caps_free(f);
}

static void cmd_memoria(void)
{
  // O QUE O idf.py size NAO CONTA.
  //
  // O relatorio da compilacao mostra so o ESTATICO: .text, .data, .bss. Ele nao
  // sabe nada do que e alocado depois - stack de tarefa, buffer de UART, queue
  // do WiFi, os dois framebuffers de 1,8MB do painel. E justamente isso que
  // decide se cabe mais um app.
  printf("--- memoria ---\n");

  const size_t i_liv = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t i_tot = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  const size_t i_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const size_t i_blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  const size_t p_liv = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t p_tot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t p_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  const size_t p_blk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  printf("  SRAM do P4  : %u KB uteis (768 - cache L2); heap %u KB, fixo (codigo+dados) %u KB\n",
         (unsigned)((768u * 1024u - (unsigned)CONFIG_CACHE_L2_CACHE_SIZE) / 1024), (unsigned)(i_tot / 1024),
         (unsigned)(((768u * 1024u - (unsigned)CONFIG_CACHE_L2_CACHE_SIZE) - i_tot) / 1024));
  printf("  RAM interna : %u de %u KB em uso (%u%%), livre %u KB\n",
         (unsigned)((i_tot - i_liv) / 1024), (unsigned)(i_tot / 1024),
         (unsigned)(i_tot ? ((i_tot - i_liv) * 100 / i_tot) : 0),
         (unsigned)(i_liv / 1024));
  // O "minimo ja visto" e o numero que importa para saber se falta: o livre de
  // agora nao mostra o pico de uso que ja passou.
  printf("                minimo ja visto %u KB, maior bloco %u KB\n",
         (unsigned)(i_min / 1024), (unsigned)(i_blk / 1024));

  printf("  PSRAM       : %u de %u KB em uso (%u%%), livre %u KB\n",
         (unsigned)((p_tot - p_liv) / 1024), (unsigned)(p_tot / 1024),
         (unsigned)(p_tot ? ((p_tot - p_liv) * 100 / p_tot) : 0),
         (unsigned)(p_liv / 1024));
  printf("                minimo ja visto %u KB, maior bloco %u KB\n",
         (unsigned)(p_min / 1024), (unsigned)(p_blk / 1024));

  const esp_partition_t* app = esp_ota_get_running_partition();
  if (app) {
    printf("  particao    : %u KB para o programa\n", (unsigned)(app->size / 1024));
  }
}

// MAPA DA RAM INTERNA - onde esta cada byte e quao picotado esta o livre.
//
// "livre 80 KB" nao diz se da para alocar 16 KB: se o livre estiver em 40
// pedacos de 2 KB, nao da. O heap do IDF (TLSF) e dividido em REGIOES (cada
// faixa de endereco que sobrou depois do .bss, a TCM, a RAM do LP, a reserva
// DMA); um bloco nunca atravessa regioes. Aqui cada bloco de cada regiao e
// visitado (heap_caps_walk) e contado por faixa de tamanho, usados e livres.
//
// COMO LER:
//   - blocos LIVRES pequenos e muitos = fragmentacao. O que importa e o
//     "maior" de cada regiao, nao a soma;
//   - blocos USADOS entre 4 e 16 KB sao malloc() comuns que o
//     SPIRAM_MALLOC_ALWAYSINTERNAL (16384) obriga a ficar na interna. Se forem
//     muitos, baixar esse limite os manda para a PSRAM.
#define MAPA_FAIXAS 6
static const uint32_t s_faixa_lim[MAPA_FAIXAS] = { 64, 256, 1024, 4096, 16384, UINT32_MAX };
static const char*    s_faixa_nome[MAPA_FAIXAS] = { "<64", "<256", "<1K", "<4K", "<16K", ">=16K" };
#define MAPA_REGIOES 8

typedef struct {
  intptr_t ini, fim;
  uint32_t usado, livre, maior, n_livres, n_usados;
  uint32_t livres_n[MAPA_FAIXAS], usados_n[MAPA_FAIXAS], usados_b[MAPA_FAIXAS];
} mapa_regiao_t;

typedef struct {
  mapa_regiao_t r[MAPA_REGIOES];
  int n;
  bool estourou;
} mapa_t;

// Roda com a trava do heap presa: nada de printf nem malloc aqui dentro.
static bool mapa_visitar(walker_heap_into_t h, walker_block_info_t b, void* u)
{
  mapa_t* m = (mapa_t*)u;
  mapa_regiao_t* r = nullptr;
  for (int i = 0; i < m->n; ++i) { if (m->r[i].ini == h.start) { r = &m->r[i]; break; } }
  if (!r) {
    if (m->n >= MAPA_REGIOES) { m->estourou = true; return false; }
    r = &m->r[m->n++];
    memset(r, 0, sizeof(*r));
    r->ini = h.start; r->fim = h.end;
  }
  int f = 0;
  while (f < MAPA_FAIXAS - 1 && b.size >= s_faixa_lim[f]) { ++f; }
  if (b.used) {
    r->usado += b.size; r->n_usados++;
    r->usados_n[f]++; r->usados_b[f] += b.size;
  } else {
    r->livre += b.size; r->n_livres++;
    r->livres_n[f]++;
    if (b.size > r->maior) { r->maior = b.size; }
  }
  return true;
}

static void cmd_mem_mapa(void)
{
  // static: ~1 KB, fora da pilha do console.
  static mapa_t m;
  memset(&m, 0, sizeof(m));
  heap_caps_walk(MALLOC_CAP_INTERNAL, mapa_visitar, &m);

  printf("--- mapa da RAM interna, por regiao ---\n");
  uint32_t u_n[MAPA_FAIXAS] = {0}, u_b[MAPA_FAIXAS] = {0}, l_n[MAPA_FAIXAS] = {0};
  for (int i = 0; i < m.n; ++i) {
    const mapa_regiao_t& r = m.r[i];
    const uint32_t frag = r.livre ? (uint32_t)(100 - (uint64_t)r.maior * 100 / r.livre) : 0;
    printf("  %08x-%08x %4u KB: usado %4u KB em %4u blocos | livre %4u KB em %3u, maior %3u KB (frag %u%%)\n",
           (unsigned)r.ini, (unsigned)r.fim, (unsigned)((r.fim - r.ini) / 1024),
           (unsigned)(r.usado / 1024), (unsigned)r.n_usados,
           (unsigned)(r.livre / 1024), (unsigned)r.n_livres, (unsigned)(r.maior / 1024),
           (unsigned)frag);
    for (int f = 0; f < MAPA_FAIXAS; ++f) {
      u_n[f] += r.usados_n[f]; u_b[f] += r.usados_b[f]; l_n[f] += r.livres_n[f];
    }
  }
  if (m.estourou) { printf("  (mais regioes que %d - lista cortada)\n", MAPA_REGIOES); }

  printf("\n  faixa    usados (qtd / KB)   livres (qtd)\n");
  for (int f = 0; f < MAPA_FAIXAS; ++f) {
    printf("  %-6s   %5u / %5u       %5u\n", s_faixa_nome[f],
           (unsigned)u_n[f], (unsigned)(u_b[f] / 1024), (unsigned)l_n[f]);
  }
}

// CACA AO VAZAMENTO: 'mem marca' tira uma foto do heap; 'mem delta' diz o que
// mudou desde ela. O uso e abrir e fechar o app (ou ligar e desligar o servico)
// algumas vezes entre os dois: o que NAO volta ao valor da marca e o que ficou
// preso. Compara bytes e QUANTIDADE de blocos usados (uma lista que cresce de
// 1 em 1 bloco escapa da conta em bytes), por faixa de tamanho na interna (a
// faixa que cresce diz o tipo de coisa: pilha, buffer, objeto pequeno), e as
// tarefas vivas (uma tarefa que nao morre segura a pilha dela).
typedef struct {
  bool     ok;
  mapa_t   mapa;                 // so a interna, por faixa
  uint32_t i_bytes, i_blocos;    // total usado, internal
  uint32_t p_bytes, p_blocos;    // total usado, PSRAM
  uint32_t tarefas;
} marca_mem_t;
static marca_mem_t s_marca_mem;

static void foto_mem(marca_mem_t* o)
{
  memset(o, 0, sizeof(*o));
  heap_caps_walk(MALLOC_CAP_INTERNAL, mapa_visitar, &o->mapa);
  multi_heap_info_t hi;
  heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
  o->i_bytes = (uint32_t)hi.total_allocated_bytes; o->i_blocos = (uint32_t)hi.allocated_blocks;
  heap_caps_get_info(&hi, MALLOC_CAP_SPIRAM);
  o->p_bytes = (uint32_t)hi.total_allocated_bytes; o->p_blocos = (uint32_t)hi.allocated_blocks;
  o->tarefas = (uint32_t)uxTaskGetNumberOfTasks();
  o->ok = true;
}

static void cmd_mem_marca(void)
{
  foto_mem(&s_marca_mem);
  printf("marca tirada: interna %u KB em %u blocos, PSRAM %u KB em %u blocos, %u tarefas.\n"
         "Abra e feche o app algumas vezes e rode 'mem delta'.\n",
         (unsigned)(s_marca_mem.i_bytes / 1024), (unsigned)s_marca_mem.i_blocos,
         (unsigned)(s_marca_mem.p_bytes / 1024), (unsigned)s_marca_mem.p_blocos,
         (unsigned)s_marca_mem.tarefas);
}

static void cmd_mem_delta(void)
{
  if (!s_marca_mem.ok) { printf("sem marca - rode 'mem marca' primeiro\n"); return; }
  static marca_mem_t agora;
  foto_mem(&agora);
  printf("--- o que mudou desde a marca ---\n");
  printf("  interna : %+ld B em %+ld blocos\n", (long)agora.i_bytes - (long)s_marca_mem.i_bytes,
         (long)agora.i_blocos - (long)s_marca_mem.i_blocos);
  printf("  PSRAM   : %+ld B em %+ld blocos\n", (long)agora.p_bytes - (long)s_marca_mem.p_bytes,
         (long)agora.p_blocos - (long)s_marca_mem.p_blocos);
  printf("  tarefas : %+ld\n", (long)agora.tarefas - (long)s_marca_mem.tarefas);
  uint32_t a_n[MAPA_FAIXAS] = {0}, a_b[MAPA_FAIXAS] = {0}, m_n[MAPA_FAIXAS] = {0}, m_b[MAPA_FAIXAS] = {0};
  for (int i = 0; i < agora.mapa.n; ++i) {
    for (int f = 0; f < MAPA_FAIXAS; ++f) { a_n[f] += agora.mapa.r[i].usados_n[f]; a_b[f] += agora.mapa.r[i].usados_b[f]; }
  }
  for (int i = 0; i < s_marca_mem.mapa.n; ++i) {
    for (int f = 0; f < MAPA_FAIXAS; ++f) { m_n[f] += s_marca_mem.mapa.r[i].usados_n[f]; m_b[f] += s_marca_mem.mapa.r[i].usados_b[f]; }
  }
  printf("\n  interna, blocos usados por faixa (variacao):\n");
  for (int f = 0; f < MAPA_FAIXAS; ++f) {
    printf("  %-6s   %+5ld blocos  %+7ld B\n", s_faixa_nome[f],
           (long)a_n[f] - (long)m_n[f], (long)a_b[f] - (long)m_b[f]);
  }
  printf("\n  Tudo perto de zero depois de abrir/fechar = sem vazamento. Se o mesmo\n"
         "  numero cresce a cada ciclo, e vazamento: a faixa mostra o tipo de bloco.\n");
}

static void cmd_mem_extrato(void)
{
  static mem_marco_t e[MEM_MARCOS_N];
  const int n = tab5_mem_extrato(e, MEM_MARCOS_N);
  printf("--- extrato da RAM interna (boot e subida dos servicos) ---\n");
  printf("  %8s  %-26s %8s %8s %8s %8s\n", "ms", "etapa", "livre", "delta", "maior", "minimo");
  for (int i = 0; i < n; ++i) {
    printf("  %8lu  %-26s %8lu %+8ld %8lu %8lu\n", (unsigned long)e[i].ms, e[i].etapa,
           (unsigned long)e[i].livre, (long)e[i].delta, (unsigned long)e[i].maior_bloco,
           (unsigned long)e[i].minimo);
  }
  if (n == MEM_MARCOS_N) { printf("  (cheio: marcos seguintes descartados)\n"); }
}

// QUEM ESTA PROIBINDO O LIGHT SLEEP.
//
// Toda a economia de energia do IDF passa pelas travas de PM. Um driver que
// precisa de clock estavel cria uma trava na inicializacao e a segura; enquanto
// houver UMA trava viva, o sistema nao desce para o modo que ela proibe - e nao
// ha aviso nenhum, o esp_pm simplesmente nao dorme.
//
// Foi assim que se descobriu que o painel DPI segurava duas travas
// desde a criacao e so as soltava no destrutor - o que levou ao
// DESMONTAR_DSI_NO_REPOUSO em tab5_power.cpp. Sem este despejo, aquilo teria
// sido adivinhacao.
//
// COMO USAR: o numero que interessa nao e este, com a tela acesa. E o de DENTRO
// do repouso - e por isso o comando aceita um atraso em segundos:
//
//     energia travas 30      (deixa a tela apagar e so entao despeja)
//
// Tipos que importam: NO_LIGHT_SLEEP proibe dormir; CPU_FREQ_MAX prende o
// clock no teto; APB_FREQ_MAX prende o barramento - e este tambem impede o
// light sleep, porque dormir pararia o APB.
static void cmd_travas(int segundos)
{
#if CONFIG_PM_ENABLE
  // O tempo de tela vem de Ajustes e pode ser qualquer coisa. Dizer qual e
  // evita a medida inutil mais provavel: esperar menos que ele e despejar as
  // travas com a tela ainda acesa, quando o painel ainda segura as duas.
  const int tela = (int)ajustes_tela_s();
  if (segundos > 0 && segundos <= tela) {
    printf("AVISO: a tela apaga em %d s e voce pediu %d. Vai medir acordado.\n"
           "       use 'energia travas %d' ou mais.\n", tela, segundos, tela + 10);
  }

  if (segundos > 0) {
    printf("esperando %d s - NAO toque na tela (o teclado do serial pode usar)...\n",
           segundos);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)segundos * 1000));
  }

  const bool dormindo = tab5_power_em_repouso();
  printf("--- travas de PM  [%s] ---\n",
         dormindo ? "EM REPOUSO - e esta a medida que vale"
                  : "TELA ACESA - esta medida NAO vale para o light sleep");
  esp_pm_dump_locks(stdout);

  if (dormindo) {
    printf("\nvazio, ou so travas soltas = caminho livre para o light sleep\n"
           "dsi_dpi / dsi_phy presos    = o desmonte da tela nao aconteceu\n"
           "uart_driver preso           = e o Grove; decisao de projeto antes\n");
  } else {
    printf("\na tela nao apagou. tempo configurado: %d s.\n"
           "tente 'energia travas %d' e nao toque na tela durante a espera.\n",
           tela, tela + 10);
  }
#else
  (void)segundos;
  printf("CONFIG_PM_ENABLE desligado: nao ha travas para mostrar\n");
#endif
}

// FORCA O REPOUSO AGORA, no clock que voce escolher - para descobrir se um
// piso MENOR que os 40MHz de hoje (ver o "int piso = ... : 40" em
// core.cpp) e aceito pelo silicio e pelo painel, sem regravar a cada
// numero novo. Mesma ideia do 'tela relogio <mhz>' - comparar em execucao em
// vez de adivinhar.
//
// SEGURO CHAMAR DO CONSOLE (uma tarefa diferente da interface): reli
// tab5_power_repouso()/retomar() e nenhuma das duas toca I2C - so mexem no
// painel DSI, no PM (esp_pm_configure) e no AXI-ICM. A regra de "so a
// interface fala com o barramento interno" e sobre o INA226/touch/teclado/
// IMU, e nao vale aqui.
//
// UM CUIDADO QUE FICA COM QUEM TESTA: se o repouso AUTOMATICO (o timer de
// ociosidade da tela, em Ajustes) disparar no MESMO instante que este comando,
// as duas chamadas correm sem trava nenhuma por cima do mesmo estado
// (s_em_repouso e companhia sao globais do modulo, sem mutex) - e o resultado
// seria desmontar o painel duas vezes. Na pratica a janela e estreitissima
// (o guard "ja em repouso?" barra o caso comum), mas para testar com sossego
// aumente o tempo de tela em Ajustes antes, ou teste logo depois de tocar a
// tela (o relogio de ociosidade acabou de reiniciar).
//
// Se o MHz pedido nao existir nesta revisao de silicio (validos: 360, 180,
// 90 e XTAL/N -> 40, 20, 10, 8, 5, 4, 2, 1), tab5_power_repouso() ja desfaz
// sozinho e devolve false - a tela volta a acender, sem travar nada.
static void cmd_repouso_forcar(int mhz)
{
  if (mhz <= 0) {
    printf("uso: energia repouso <mhz>   (ex: energia repouso 20)\n"
           "valores validos nesta placa: " TAB5_CPU_DEGRAUS "\n");
    return;
  }
  if (tab5_power_em_repouso()) {
    printf("ja esta em repouso - toque a tela para acordar e tente de novo\n"
           "(chamar de novo agora nao faz nada: tab5_power_repouso() e "
           "idempotente enquanto dormindo)\n");
    return;
  }

  printf("entrando em repouso a %d MHz...\n", mhz);
  fflush(stdout);
  const bool ok = tab5_power_repouso(mhz);
  if (ok) {
    printf("repouso confirmado. a tela esta apagada, mas este console continua\n"
           "vivo - use 'energia' agora (e de novo daqui uns segundos, para a\n"
           "corrente assentar) para medir. toque a tela para acordar.\n");
  } else {
    printf("o repouso RECUSOU esse clock - o log de erro acima (ESP_LOGE de "
           "'power') explica o motivo. a tela deve ter voltado a acender sozinha.\n");
  }
}

// TETO DO CPU COM A TELA ACORDADA - ver o aviso grande em tab5_power.hpp.
// Diferente de 'energia repouso', a tela NAO apaga: o painel continua ligado,
// desenhando, no relogio de pixel que estiver configurado agora ('tela
// relogio <mhz>'). Existe para testar a combinacao que faltava: painel devagar
// E CPU devagar, os dois ACORDADOS ao mesmo tempo.
static void cmd_teto_cpu(int mhz)
{
  if (mhz <= 0) {
    printf("uso: energia teto <mhz>   (ex: energia teto 180)\n"
           "combine com 'tela relogio <mhz>' primeiro, se for testar os dois juntos\n"
           "valores validos nesta placa: " TAB5_CPU_DEGRAUS "\n");
    return;
  }
  const bool ok = tab5_power_teto_cpu(mhz);
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
    printf("RECUSADO - o log de erro acima ('power') explica o motivo.\n");
  }
}

static void cmd_energia(void)
{
  // LEITURA DE CACHE, NAO DO INA226.
  //
  // O I2C_Class do M5Unified nao tem mutex: se esta tarefa for ao INA226
  // enquanto a interface le o touch, as duas transacoes se intercalam e voce le
  // registrador errado - sem erro nenhum, so numero sem sentido. A interface
  // mede uma vez por segundo e deixa o valor aqui; e daqui que se le.
  const tab5_medida_t m = tab5_power_ultima();

  printf("--- energia ---\n");
  printf("  agora    : %.2f V   %.0f mA   %.0f mW   %d%%   CPU %d MHz\n",
         m.volts, m.mA, m.mW, m.pct, tab5_power_cpu_mhz());
  printf("             %s\n",
         (m.mA > 20)  ? "carregando"
       : (m.mA < -20) ? "na bateria" : "parado / na tomada sem carregar");

  // O ESTIMADOR, ao vivo - ver estimar_pct() em tab5_power.cpp. A diferenca
  // entre "estimador" e "curva" e o que a contagem de mAh esta corrigindo.
  {
    float soc, curva, desde; const char* fonte;
    tab5_power_soc_debug(&soc, &curva, &fonte, &desde);
    printf("  bateria  : estimador %.1f%%  | curva de tensao diria %.0f%%  | partiu de: %s\n",
           soc, curva, fonte);
    if (desde >= 0) {
      printf("             ultima carga ha %.0f min%s\n", desde,
             desde < 60 ? " (a curva ainda nao corrige - tensao pos-carga mente)" : "");
    }
  }

  tab5_ponto_t h[TAB5_HIST_N];
  const int n = tab5_power_historico(h);
  if (n <= 1) {
    printf("\n  historico ainda vazio (uma amostra a cada %d s)\n", TAB5_HIST_SEG);
    return;
  }

  // Escala da barra pelo MAIOR consumo da janela, e nao por um teto fixo: com
  // teto fixo, uma janela inteira em repouso viraria uma coluna de nada.
  float pico = 1.0f;
  for (int i = 0; i < n; ++i) { const float v = fabsf(h[i].mA); if (v > pico) pico = v; }

  printf("\n  ultimos %d min (uma linha por minuto, barra = corrente ate %.0f mA)\n",
         (n * TAB5_HIST_SEG) / 60, pico);
  printf("  %6s %7s %11s\n", "quando", "tensao", "corrente");

  // De duas em duas amostras: com TAB5_HIST_SEG=30 isso da uma linha por
  // minuto, e 30 linhas cabem numa tela de terminal. Todas as 60 obrigariam a
  // rolar para ver o comeco.
  for (int i = 0; i < n; i += 2) {
    const int min_atras = ((n - 1 - i) * TAB5_HIST_SEG) / 60;
    char barra[33];
    int larg = (int)((fabsf(h[i].mA) / pico) * 32.0f);
    if (larg < 0)  { larg = 0; }
    if (larg > 32) { larg = 32; }
    memset(barra, '#', (size_t)larg);
    barra[larg] = 0;
    printf("  -%3d m %6.2fV %7.0f mA  %s\n", min_atras, h[i].volts, h[i].mA, barra);
  }
}



// Caminho no cartao: absoluto como veio; relativo, a partir de /sdcard. Poupa
// digitar o prefixo em todo comando de arquivo ('ler apps/Sistema/x.txt').
static const char* caminho_sd(const char* c, char* buf, size_t n)
{
  if (c[0] == '/') { return c; }
  snprintf(buf, n, "/sdcard/%s", c);
  return buf;
}

// Monta o cartao, roda o que voce passar, desmonta - sempre, inclusive quando
// o comando falha. Escrito como funcao porque cada comando esquecer o
// desmontar seria um vazamento de contagem: o cartao ficaria montado para
// sempre e o app que o usa nunca conseguiria desmontar de verdade.
template <typename F>
static int com_cartao(F&& corpo)
{
  if (!tab5_sd_montar())
  {
    printf("erro: cartao nao montou (%s)\n", tab5_sd_descricao());
    return 1;
  }
  const int r = corpo();
  tab5_sd_desmontar();
  return r;
}

// ---------------------------------------------------------------------------
// wifi gravar <ssid> [senha]
// ---------------------------------------------------------------------------
static int wifi_gravar(const char* ssid, const char* senha_ou_null)
{
  const char* senha = senha_ou_null ? senha_ou_null : "-";   // "-" = rede aberta

  return com_cartao([&]() -> int {
    // tab5_wifi_gravar_credenciais() cifra a senha antes de escrever (AES-128-CBC,
    // ver tab5_wifi.cpp) e ja atualiza a copia em RAM tambem - reler o arquivo
    // depois, com o C6 no ar, e justamente o que nao pode acontecer.
    if (!tab5_wifi_gravar_credenciais(ssid, senha))
    {
      printf("erro: nao consegui gravar %s\n", TAB5_WIFI_ARQUIVO);
      return 1;
    }
    printf("gravado em %s (senha cifrada no arquivo)\n  SSID:  %s\n  senha: %s\n",
           TAB5_WIFI_ARQUIVO, ssid,
           strcmp(senha, "-") == 0 ? "(rede aberta)" : "definida");
    printf("para usar agora: wifi conectar \"%s\"\n", ssid);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// ler <caminho> [linhas]
// ---------------------------------------------------------------------------
static int cmd_ler(int argc, char** argv)
{
  if (argc < 2) { printf("uso: ler <caminho> [linhas]\n"); return 1; }
  char buf[160];
  const char* caminho = caminho_sd(argv[1], buf, sizeof(buf));
  // Teto de linhas: um satelites-*.csv de um dia inteiro tem milhares delas,
  // e despejar tudo a 115200 baud prenderia o console por minutos. Quem quer
  // mais pede o numero; quem quer o arquivo inteiro usa 'despejar'.
  int teto = (argc >= 3) ? atoi(argv[2]) : 200;
  if (teto < 1) { teto = 1; }

  return com_cartao([&]() -> int {
    FILE* f = fopen(caminho, "r");
    if (!f) { printf("erro: %s nao abriu\n", caminho); return 1; }

    printf("--- %s ---\n", caminho);
    char linha[160];
    int n = 0;
    while (n < teto && fgets(linha, sizeof(linha), f)) {
      fputs(linha, stdout);
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
// despejar <caminho>  - o arquivo INTEIRO, para ser capturado do outro lado
// ---------------------------------------------------------------------------
// Irmao do "ler", com tres diferencas que sao a razao de ele existir separado:
//
//   1. SEM TETO DE LINHAS. O "ler" corta em 200 porque ele e para OLHAR - e
//      olhar mais que isso na tela do terminal nao serve para nada. Este e para
//      COPIAR: um energia.csv de uma descarga inteira tem umas 500 linhas, e um
//      arquivo cortado no meio nao serve para ajustar curva nenhuma.
//
//   2. CALA O LOG ENQUANTO DESPEJA. Este console divide a serial com o
//      ESP_LOG de todas as tarefas. Sem calar, uma linha de "power" ou
//      "mesh" cai NO MEIO do CSV, e quem esta capturando do outro lado recebe
//      um arquivo contaminado. Restaurado no fim, sempre - inclusive quando o
//      fopen falha.
//
//   3. MARCADORES QUE O SCRIPT RECONHECE. O ">>>INICIO" e o ">>>FIM" com a
//      contagem de linhas dizem ao capturador onde comeca, onde termina e se
//      chegou tudo. Sem eles, o script do outro lado teria que adivinhar
//      quando parar de escutar - e adivinhar sempre erra na hora errada.
static int cmd_despejar(int argc, char** argv)
{
  if (argc < 2) {
    printf("uso: despejar <caminho>\n"
           "  ex.: despejar /sdcard/apps/Sistema/energia/energia-AAAA-MM-DD.csv\n"
           "  no Mac, para salvar:  ./ferramentas/baixar.py <caminho>\n");
    return 1;
  }
  char buf[160];
  const char* caminho = caminho_sd(argv[1], buf, sizeof(buf));

  return com_cartao([&]() -> int {
    FILE* f = fopen(caminho, "r");
    if (!f) { printf("erro: %s nao abriu\n", caminho); return 1; }

    // Tamanho ANTES, para o outro lado poder conferir o que recebeu.
    long tam = 0;
    if (fseek(f, 0, SEEK_END) == 0) { tam = ftell(f); fseek(f, 0, SEEK_SET); }

    // A partir daqui, silencio - ver o motivo 2 no cabecalho.
    esp_log_level_set("*", ESP_LOG_NONE);

    printf(">>>INICIO %s %ld\n", caminho, tam);
    char linha[256];
    long n = 0;
    while (fgets(linha, sizeof(linha), f)) {
      fputs(linha, stdout);
      // Uma pausa a cada 64 linhas. Nao e educacao: o despejo de um arquivo
      // grande e um loop fechado nesta tarefa, e sem ceder a CPU o watchdog de
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
static int cmd_lista(int argc, char** argv)
{
  char buf[160];
  const char* caminho = (argc >= 2) ? caminho_sd(argv[1], buf, sizeof(buf)) : "/sdcard";

  return com_cartao([&]() -> int {
    DIR* d = opendir(caminho);
    if (!d) { printf("erro: %s nao abriu\n", caminho); return 1; }

    printf("--- %s ---\n", caminho);
    struct dirent* e;
    int i = 0;
    while ((e = readdir(d)) != nullptr)
    {
      ceder(i++);
      char completo[300];
      snprintf(completo, sizeof(completo), "%s/%s", caminho, e->d_name);
      struct stat st;
      if (e->d_type == DT_DIR)            { printf("  [dir]  %s\n", e->d_name); }
      else if (stat(completo, &st) == 0)  { printf("  %6ld  %s\n", (long)st.st_size, e->d_name); }
      else                                { printf("     ?   %s\n", e->d_name); }
    }
    closedir(d);
    printf("--- fim ---\n");
    return 0;
  });
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// apagar <caminho>  - remove UM arquivo do cartao
// ---------------------------------------------------------------------------
//
// Existe para poder comecar uma gravacao do zero sem tirar o cartao. Apagar e
// irreversivel, entao:
//   - so aceita caminho dentro de /sdcard/, para um dedo errado nao alcancar
//     outra coisa;
//   - um arquivo por vez, sem curinga: nada de "apagar *".
static int cmd_apagar(int argc, char** argv)
{
  if (argc < 2) { printf("uso: apagar apps/<App>/<arquivo>\n"); return 1; }
  char buf[160];
  const char* caminho = caminho_sd(argv[1], buf, sizeof(buf));

  if (strncmp(caminho, "/sdcard/", 8) != 0 || strstr(caminho, "..") != nullptr)
  {
    printf("recusado: so apago dentro de /sdcard/ (e sem '..')\n");
    return 1;
  }

  return com_cartao([&]() -> int {
    struct stat st;
    if (stat(caminho, &st) != 0) { printf("nao existe: %s\n", caminho); return 1; }

    if (remove(caminho) != 0) { printf("nao consegui apagar %s\n", caminho); return 1; }
    printf("apagado: %s (%ld bytes)\n", caminho, (long)st.st_size);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// i2c <sda> <scl>  - descobre quem responde num par de pinos
// ---------------------------------------------------------------------------
//
// Existe para NAO adivinhar protocolo de hardware novo. O conector lateral do
// Tab5 expoe G49, G50, G0, G1 e EXT 5V; se o teclado falar I2C, o endereco dele
// aparece aqui e a integracao vira trivial. Se nada responder em nenhuma
// combinacao, e sinal de que nao e I2C - e ai a hipotese seguinte e UART.
//
// Use o par nos DOIS sentidos: nao ha como saber de fora quem e SDA e quem e
// SCL, e trocado nao responde nada.
//
//   i2c 49 50        i2c 50 49        i2c 0 1        i2c 1 0
//
// Rode a partir da tela inicial: o comando mexe no barramento externo, que e o
// mesmo que o Azimutal toma emprestado como UART do GPS.
static int cmd_i2c(int argc, char** argv)
{
  if (argc < 3) { printf("uso: i2c <sda> <scl>   (ex: i2c 49 50)\n"); return 1; }

  // O AVISO VIROU TRAVA. Ate aqui, "rode a partir da tela inicial" era so um
  // comentario no fonte - o comando reconfigurava o barramento externo por
  // baixo de quem quer que o estivesse usando, sem perguntar. O Azimutal usa
  // esse mesmo barramento como UART do GPS, e o teclado do ExtPort1 tambem
  // esta nele: rodar isto com qualquer um dos dois aberto arranca o
  // barramento da tarefa dona no meio de uma transacao.
  //
  // A CONSISTENCIA que valia a pena: uma regra de bancada documentada em
  // comentario e uma regra que so vale enquanto alguem lembra de ler o
  // comentario. Fazendo o proprio comando checar, ela vale sempre.
  const char* app = sistema_app_atual();
  if (app) {
    printf("nao da para varrer o I2C com '%s' aberto - ele pode estar usando\n"
           "o mesmo barramento externo. Volte para a tela inicial e tente de novo.\n", app);
    return 1;
  }

  const int sda = atoi(argv[1]);
  const int scl = atoi(argv[2]);

  // O barramento EXTERNO (I2C_NUM_1) e o unico que podemos reaproveitar. O
  // interno (I2C_NUM_0) carrega o INA226 e os expansores, e e propriedade da
  // tarefa de interface - encostar nele daqui seria a corrida que o sistema
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

  // Devolve o barramento externo aos pinos do Grove, como o M5.begin() deixou.
  M5.Ex_I2C.release();
  M5.Ex_I2C.begin();
  return 0;
}

// ---------------------------------------------------------------------------
// SEGUNDO CONSUMIDOR DE MENTIRA (com um app so nao da para testar).
// ---------------------------------------------------------------------------
// O sistema so abre UM app por vez, entao nao da para provar que
// tab5_gps_posicao()/tab5_gps_tempo()/tab5_imu_ultima() aguentam DOIS
// leitores ao mesmo tempo so abrindo dois apps - nao existe "dois apps
// abertos" ainda. Esta tarefa e um segundo leitor de verdade, independente
// de qualquer app: martela as tres funcoes bem mais rapido que qualquer
// tela desenha, enquanto voce abre o Azimutal ou o IMU normalmente. Prova
// duas coisas: que a trava aguenta uso concorrente de verdade sem travar
// nem derrubar o sistema, e (comparando os numeros daqui com os da tela) que
// os dois leitores sempre veem o MESMO valor, nunca um "meio atualizado".
static bool s_conc_ativa = false;
static bool s_conc_parar = false;
static uint32_t s_conc_leituras = 0;

static void vTaskConcorrencia(void*)
{
  uint32_t ultimo_log = 0;
  while (!s_conc_parar) {
    nmea_posicao_t pos  = tab5_gps_posicao();
    nmea_tempo_t   hora = tab5_gps_tempo();
    tab5_imu_dados_t imu;
    tab5_imu_ultima(&imu);
    ++s_conc_leituras;
    (void)hora;

    const uint32_t agora = tab5_agora_ms();
    if (agora - ultimo_log >= 1000) {
      ultimo_log = agora;
      printf("[concorrencia] %lu leituras | gps valida=%d lat=%.5f lon=%.5f "
             "| imu valida=%d ax=%.2f ay=%.2f az=%.2f\n",
             (unsigned long)s_conc_leituras, (int)pos.valida, pos.lat, pos.lon,
             (int)imu.valida, imu.ax, imu.ay, imu.az);
    }
    // SEM vTaskDelay(0)/yield puro - 1 tick (o minimo real) ainda martela bem
    // mais rapido que qualquer app desenha (20ms de ciclo), sem virar um
    // busy-loop que rouba o core de quem esta desenhando de verdade.
    vTaskDelay(1);
  }
  s_conc_ativa = false;
  s_conc_parar = false;
  vTaskDelete(nullptr);
}

static void cmd_concorrencia(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "iniciar") == 0) {
    if (s_conc_ativa) { printf("ja esta rodando - %lu leituras ate agora\n", (unsigned long)s_conc_leituras); return; }
    s_conc_ativa = true;
    s_conc_parar = false;
    s_conc_leituras = 0;
    xTaskCreatePinnedToCore(vTaskConcorrencia, "conc_teste", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    printf("segundo leitor no ar - martelando GPS+IMU independente de qualquer app.\n"
           "abra o IMU (ou outro app que leia GPS/IMU) e compare os numeros com os daqui.\n"
           "'concorrencia parar' quando terminar.\n");
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "parar") == 0) {
    if (!s_conc_ativa) { printf("nao estava rodando\n"); return; }
    s_conc_parar = true;
    printf("pedido para parar - confirma na proxima volta (ate 1 tick)\n");
    return;
  }
  printf("rodando: %s | leituras ate agora: %lu\n"
         "uso: concorrencia iniciar | concorrencia parar\n",
         s_conc_ativa ? "sim" : "nao", (unsigned long)s_conc_leituras);
}

// ---------------------------------------------------------------------------
// TRES "APPS" DE MENTIRA, CADA UM NA SUA TAREFA
// (teste mais rigoroso: os 3 apps aparecem no console pedindo o servico).
// ---------------------------------------------------------------------------
// A diferenca para "concorrencia" acima: aquele e UM leitor generico,
// martelando o mais rapido possivel. Isto aqui e tres CONSUMIDORES
// DIFERENTES, cada um com o padrao de acesso de um app de verdade -
// cadencia propria, log com o proprio nome - rodando ao mesmo tempo, na
// MESMA tarefa de interface que qualquer app real usaria (nao, so cada um na
// SUA tarefa, independente das outras duas e de qualquer app aberto na
// tela). E o mais perto de "3 apps abertos ao mesmo tempo" que da para
// simular sem o sistema realmente suportar isso ainda.
//
// App-Mapa   : le so GPS, a cada 200ms - como uma tela de posicao/mapa.
// App-Nivel  : le so IMU, o mais rapido possivel - como um nivel-bolha.
// App-Log    : le GPS E IMU juntos, a cada 1s - como um logger periodico.
//              Este e o que mais interessa: prova que um consumidor lendo
//              DOIS servicos em sequencia nao trava (as travas de GPS e IMU
//              sao independentes, nunca uma dentro da outra - sem isso
//              haveria risco de deadlock se um dia alguem invertesse a
//              ordem em outro consumidor).
static bool     s_sim_ativa = false;
static bool     s_sim_parar = false;
static uint32_t s_sim_leituras[3] = { 0, 0, 0 };

static void vTaskSimMapa(void*)
{
  uint32_t ultimo_log = 0;
  while (!s_sim_parar) {
    nmea_posicao_t pos = tab5_gps_posicao();
    ++s_sim_leituras[0];

    const uint32_t agora = tab5_agora_ms();
    if (agora - ultimo_log >= 1000) {
      ultimo_log = agora;
      printf("[app-mapa]  pedido #%-6lu -> gps valida=%d lat=%.5f lon=%.5f\n",
             (unsigned long)s_sim_leituras[0], (int)pos.valida, pos.lat, pos.lon);
    }
    vTaskDelay(pdMS_TO_TICKS(200));   // cadencia de uma tela de posicao
  }
  vTaskDelete(nullptr);
}

static void vTaskSimNivel(void*)
{
  uint32_t ultimo_log = 0;
  while (!s_sim_parar) {
    tab5_imu_dados_t d;
    tab5_imu_ultima(&d);
    ++s_sim_leituras[1];

    const uint32_t agora = tab5_agora_ms();
    if (agora - ultimo_log >= 1000) {
      ultimo_log = agora;
      printf("[app-nivel] pedido #%-6lu -> imu valida=%d ax=%.2f ay=%.2f az=%.2f\n",
             (unsigned long)s_sim_leituras[1], (int)d.valida, d.ax, d.ay, d.az);
    }
    vTaskDelay(1);   // o mais rapido possivel, como um nivel-bolha precisa
  }
  vTaskDelete(nullptr);
}

static void vTaskSimLog(void*)
{
  while (!s_sim_parar) {
    nmea_posicao_t pos = tab5_gps_posicao();
    nmea_tempo_t   hora = tab5_gps_tempo();
    tab5_imu_dados_t imu;
    tab5_imu_ultima(&imu);
    ++s_sim_leituras[2];

    printf("[app-log]   pedido #%-6lu -> gps(valida=%d) + hora(valida=%d) + imu(valida=%d, az=%.2f)\n",
           (unsigned long)s_sim_leituras[2], (int)pos.valida, (int)hora.valida, (int)imu.valida, imu.az);
    vTaskDelay(pdMS_TO_TICKS(1000));   // cadencia de um logger periodico
  }
  vTaskDelete(nullptr);
}

static void cmd_simulacao(int argc, char** argv)
{
  if (argc >= 2 && strcmp(argv[1], "iniciar") == 0) {
    if (s_sim_ativa) { printf("ja esta rodando\n"); return; }
    s_sim_ativa = true;
    s_sim_parar = false;
    s_sim_leituras[0] = s_sim_leituras[1] = s_sim_leituras[2] = 0;
    xTaskCreatePinnedToCore(vTaskSimMapa,  "sim_mapa",  4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTaskSimNivel, "sim_nivel", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTaskSimLog,   "sim_log",   4096, nullptr, 2, nullptr, tskNO_AFFINITY);
    printf("3 apps de mentira no ar: app-mapa (GPS/200ms), app-nivel (IMU/o mais rapido\n"
           "possivel), app-log (GPS+IMU/1s). Abra apps de verdade agora e compare.\n"
           "'simulacao parar' quando terminar.\n");
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "parar") == 0) {
    if (!s_sim_ativa) { printf("nao estava rodando\n"); return; }
    s_sim_parar = true;
    s_sim_ativa = false;
    printf("pedido para parar os 3 - confirma nas proximas voltas de cada um\n");
    return;
  }
  printf("rodando: %s | leituras: mapa=%lu nivel=%lu log=%lu\n"
         "uso: simulacao iniciar | simulacao parar\n",
         s_sim_ativa ? "sim" : "nao",
         (unsigned long)s_sim_leituras[0], (unsigned long)s_sim_leituras[1], (unsigned long)s_sim_leituras[2]);
}

// ---------------------------------------------------------------------------
// 'foto' - PROVA DE QUE A CAMERA VIROU SERVICO, do MESMO jeito
// que 'concorrencia'/'simulacao' acima provam GPS/IMU: liga a camera, tira
// UMA foto e desliga de novo, inteiramente de FORA do app Camera - se isto
// funcionar com o app Camera FECHADO, e funcionar de novo com ele ABERTO ao
// mesmo tempo (ver a contagem de referencia em tab5_camera.hpp), a
// independencia entre servico e app esta provada em hardware, nao so no
// papel.
//
// A FOTO NAO DEPENDE DO TAMANHO PEDIDO AQUI: ela sai sempre com o quadro
// inteiro do sensor (TAB5_CAM_FOTO_L x _A). O tamanho de tab5_camera_iniciar
// e so o do preview, que este comando nem usa - vai o menor 3:4 razoavel.
static void cmd_foto(void)
{
  const int LARGURA = 240, ALTURA = 320;

  // sistema_camera_iniciar/parar, NAO tab5_camera_iniciar/parar direto -
  // achado ao vivo: esta funcao roda na tarefa "console", e a
  // HAL da camera toca SCCB (I2C interno) para configurar o sensor. Chamar
  // direto colidia com a leitura de 1s/1s que a tarefa de INTERFACE faz
  // sozinha nesse mesmo barramento (energia/IMU/sensores/hora) - "Set
  // format regs fail" no log, sensor configurado pela metade. A ponte (ver
  // o aviso grande em kernel/app.h) manda o pedido para a tarefa certa e
  // espera. tab5_camera_pedir_captura()/captura_pronta() abaixo NAO tocam
  // I2C nenhum (so leem/escrevem um mutex de RAM) - seguros de chamar
  // daqui sem ponte.
  printf("ligando a camera (%dx%d)...\n", LARGURA, ALTURA);
  if (!sistema_camera_iniciar(LARGURA, ALTURA)) {
    printf("falhou - confira o log (sensor ausente? app Camera aberto numa\n"
           "resolucao diferente, ja que so existe uma por vez?)\n");
    return;
  }

  // A camera so CAPTURA (um frame, por PPA, na tarefa dela); gravar e daqui,
  // da tarefa do console - ver services/tab5_bmp e tab5_camera_pedir_foto.
  uint16_t* foto = tab5_camera_foto_alocar();
  bool ok = false;
  if (!foto) {
    printf("sem PSRAM para a foto\n");
  } else if (!tab5_camera_pedir_foto(foto)) {
    printf("nao consegui pedir a foto (outra ainda pendente?)\n");
  } else {
    bool pronta = false;
    for (int i = 0; i < 50 && !pronta; ++i) {    // 5 s: o primeiro frame pode demorar
      vTaskDelay(pdMS_TO_TICKS(100));
      pronta = tab5_camera_foto_pronta(&ok);
    }
    if (!pronta)  { printf("a camera nao entregou quadro em 5 s (confira o log)\n"); }
    else if (ok)  { ok = tab5_bmp_salvar("Console", "console", foto, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A);
                    printf(ok ? "foto salva: /sdcard/apps/Console/console.bmp\n" : "a gravacao falhou (confira o log)\n"); }
    else          { printf("a captura falhou (confira o log)\n"); }
  }

  // SEMPRE solta o interesse que este comando pediu - mesmo em erro. Se mais
  // alguem (o app Camera, por exemplo) ainda estiver usando, a contagem de
  // referencia mantem o sensor de pe; se este comando era o unico
  // interessado, ele desliga de verdade agora.
  sistema_camera_parar();   // espera a tarefa da camera - so depois o buffer pode sair
  if (foto) { heap_caps_free(foto); }
}

// 'camera' - O QUE O ISP ESTA FAZENDO COM A COR. Imprime o estado do IPA
// proprio (hal/tab5_camera_ipa.c: ganhos do AWB, a matriz da calibracao e a
// que foi ao ISP, o verde que um branco saturado teria com cada uma) e mede um
// quadro inteiro: media do centro, media das sombras, fracao de pixel
// estourado e de pixel magenta. Com a lente tampada, a media do centro e o
// preto que o sensor entrega - o teste direto do pedestal (tab5_camera.cpp,
// zerar_pedestal). 'camera reg' fala com o sensor enquanto o app Camera esta
// aberto, para experimentar um registrador ao vivo.
static void cmd_camera(int argc, char** argv)
{
  if (argc >= 3 && strcmp(argv[1], "reg") == 0) {
    const long reg   = strtol(argv[2], nullptr, 16);
    const int  valor = (argc >= 4) ? (int)strtol(argv[3], nullptr, 16) : -1;
    uint8_t lido = 0;
    if (!tab5_camera_reg((uint16_t)reg, valor, &lido)) {
      printf("camera desligada (abra o app Camera) ou o SCCB falhou\n");
      return;
    }
    printf("sensor 0x%04lx = 0x%02x\n", reg, lido);
    return;
  }

  const unsigned rev = efuse_hal_chip_revision();
  printf("P4 revisao v%u.%u - %s\n", rev / 100, rev % 100,
         rev >= 300 ? "ISP com WBG e BLC (balanco e preto no hardware)"
                    : "ISP sem WBG nem BLC (balanco na CCM, preto pelo sensor)");

  // Se o app Camera ja esta com ela, so observa: pedir de novo com outro
  // tamanho seria recusado (uma resolucao por vez).
  const bool ja_ligada = tab5_camera_ativa();
  if (!ja_ligada) {
    printf("ligando a camera e esperando o AE/AWB assentarem...\n");
    if (!sistema_camera_iniciar(240, 320)) { printf("falhou - confira o log\n"); return; }
    vTaskDelay(pdMS_TO_TICKS(2500));
  }

  tab5_cam_ipa_estado_t e;
  if (!tab5_camera_ipa_estado(&e)) {
    printf("o IPA proprio nao rodou nenhum quadro (fora do pipeline?)\n");
  } else {
    printf("quadros do ISP   : %lu\n", (unsigned long)e.quadros);
    printf("exposicao, ganho : %lu us, %.2fx\n", (unsigned long)e.exposicao_us, (double)e.ganho_sensor);
    printf("ganhos do AWB    : R %.2f  B %.2f\n", (double)e.ganho_r, (double)e.ganho_b);
    printf("CCM calibracao   |  CCM no ISP\n");
    for (int i = 0; i < 3; ++i) {
      printf("  %6.2f %6.2f %6.2f  |  %6.2f %6.2f %6.2f\n",
             (double)e.ccm_awb[i][0], (double)e.ccm_awb[i][1], (double)e.ccm_awb[i][2],
             (double)e.ccm_isp[i][0], (double)e.ccm_isp[i][1], (double)e.ccm_isp[i][2]);
    }
    printf("verde do branco saturado: %.2f -> %.2f (1 = branco; abaixo de ~0,9 sai rosa)\n",
           (double)e.g_branco_antes, (double)e.g_branco_depois);
    printf("saturacao        : %lu (128 = 1,0)\n", (unsigned long)e.saturacao);
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

  uint16_t* foto = tab5_camera_foto_alocar();
  bool ok = false, pronta = false;
  if (foto && tab5_camera_pedir_foto(foto)) {
    for (int i = 0; i < 50 && !pronta; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); pronta = tab5_camera_foto_pronta(&ok); }
  }
  if (pronta && ok) {
    const int L = TAB5_CAM_FOTO_L, A = TAB5_CAM_FOTO_A;
    uint64_t c[3] = { }, sombra[3] = { };
    uint32_t n_c = 0, n_sombra = 0, n = 0, estouro = 0, magenta = 0;
    for (int y = 0; y < A; y += 2) {
      for (int x = 0; x < L; x += 2) {
        const uint16_t p = foto[y * L + x];
        const int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        ++n;
        if (r >= 248 && g >= 248 && b >= 248) { ++estouro; }
        const int menor_rb = r < b ? r : b;
        if (menor_rb >= 200 && g + 40 < menor_rb) { ++magenta; }
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

  if (!ja_ligada) { sistema_camera_parar(); }   // espera a tarefa: so depois o buffer sai
  if (foto) { heap_caps_free(foto); }
}

// ===========================================================================
// OS COMANDOS, EM TABELA
// ===========================================================================
// Cada comando e uma linha da tabela COMANDOS (mais abaixo): nome, grupo, uma
// frase de resumo, as formas de chamar e um paragrafo de para que serve. O
// 'help' e gerado dela - e por isso nao tem como ficar desatualizado: comando
// que nao esta na tabela nao roda, e o que esta aparece na ajuda.
//
// A versao anterior era um if/else de 40 ramos e um texto de ajuda escrito a
// parte. Os dois andavam separados: comando sem linha na ajuda, linha no grupo
// errado, subcomando que so existia no fonte.

static void mostrar_uso(const char* nome);   // depois da tabela

static bool eh(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// argv[de..] numa frase so, com um espaco entre as palavras.
static void juntar(char* out, size_t n, int argc, char** argv, int de)
{
  out[0] = 0;
  for (int k = de; k < argc; ++k) {
    strncat(out, argv[k], n - strlen(out) - 1);
    if (k + 1 < argc) { strncat(out, " ", n - strlen(out) - 1); }
  }
}

// ---------------------------------------------------------------------------
// SISTEMA
// ---------------------------------------------------------------------------

// Uma linha por servico do sistema, no mesmo lugar. A primeira pergunta quando
// algo estranho acontece e "o que esta de pe agora?" - e sem isto a resposta
// exigia cacar linha de log de tres tarefas diferentes.
static void c_estado(int, char**)
{
  const esp_app_desc_t* app = esp_app_get_description();
  const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
  const char* aberto = sistema_app_atual();
  printf("--- M5tabOS %s ---\n", app->version);
  printf("  ligado ha: %luh%02lum%02lus   app aberto: %s%s\n",
         (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60),
         aberto ? aberto : "nenhum (tela inicial)",
         tab5_power_em_repouso() ? "   (em repouso)" : "");
  printf("  memoria  : interna %u KB livre (minimo %u KB), PSRAM %u KB livre\n",
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

  printf("  cartao   : %s\n", tab5_sd_descricao());

  const char* est = tab5_wifi_descricao();
  const int r = (int)tab5_wifi_rssi();
  if (tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
    char sinal[12];
    if (r == TAB5_WIFI_RSSI_DESCONHECIDO) { snprintf(sinal, sizeof(sinal), "sinal --"); }
    else { snprintf(sinal, sizeof(sinal), "%ddBm", r); }
    printf("  wifi     : %s  %s  %s\n", est, tab5_wifi_ip(), sinal);
  } else {
    printf("  wifi     : %s\n", est);
  }
  printf("  bluetooth: %s\n", tab5_ble_descricao());

  printf("  teclado  : %s%s\n",
         ajustes_teclado() ? "" : "desativado em Ajustes; ",
         tab5_teclado_presente() ? "encaixado" : "ausente");

  printf("  grove 5V : %s   detectado: %s\n",
         ajustes_grove() ? "ligado" : "desligado", grove_nome(tab5_grove_tipo()));

  if (tab5_mesh_meu_numero() != 0 || tab5_mesh_no_ar()) {
    uint32_t q, pk, tel, ls, tx, ni; int var;
    tab5_mesh_contadores(&q, &pk, &tel, &ls, &tx, &ni, &var);
    printf("  mesh     : %s, %d nos, %lu quadros, %lu pacotes\n",
           tab5_mesh_no_ar() ? "no ar" : "mudo",
           tab5_mesh_num_nos(), (unsigned long)q, (unsigned long)pk);
  } else {
    printf("  mesh     : nao iniciado\n");
  }

  int na_ram; uint32_t grav, falhas;
  registro_estado(&na_ram, &grav, &falhas);
  printf("  registro : %d na RAM, %lu gravadas, %lu falhas\n",
         na_ram, (unsigned long)grav, (unsigned long)falhas);

  {
    size_t pend; uint32_t lgrav, lperd; char arq[24];
    tab5_log_estado(&pend, &lgrav, &lperd, arq, sizeof(arq));
    printf("  log      : %u byte(s) na RAM, %lu linha(s) gravadas, %lu perdidas (%s)\n",
           (unsigned)pend, (unsigned long)lgrav, (unsigned long)lperd,
           arq[0] ? arq : "ainda sem descarga");
  }

  tab5_medida_t m = tab5_power_ultima();
  printf("  energia  : %dMHz %.2fV %.1fmA %d%%\n",
         tab5_power_cpu_mhz(), m.volts, m.mA, m.pct);
}

static void c_tarefas(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "memoria")) { cmd_memoria(); }   // forma antiga
  else                                      { cmd_tarefas(); }
}

static void c_mem(int argc, char** argv)
{
  const char* o = (argc >= 2) ? argv[1] : "";
  if (!*o) {
    cmd_memoria();
    printf("\n  'mem extrato' e 'mem mapa' mostram para onde a RAM interna foi\n");
  }
  else if (eh(o, "extrato")) { cmd_mem_extrato(); }
  else if (eh(o, "mapa"))    { cmd_mem_mapa(); }
  else if (eh(o, "marca"))   { cmd_mem_marca(); }
  else if (eh(o, "delta"))   { cmd_mem_delta(); }
  else if (eh(o, "tudo"))    { cmd_memoria(); printf("\n"); cmd_mem_extrato(); printf("\n"); cmd_mem_mapa(); }
  else                       { mostrar_uso("mem"); }
}

static void c_energia(int argc, char** argv)
{
  if (argc < 2)                   { cmd_energia(); }
  else if (eh(argv[1], "travas"))  { cmd_travas(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (eh(argv[1], "repouso")) { cmd_repouso_forcar(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (eh(argv[1], "teto"))    { cmd_teto_cpu(argc >= 3 ? atoi(argv[2]) : 0); }
  else                             { mostrar_uso("energia"); }
}

// As partes de fundo dos apps (kernel/fundo.h). A parada pedida daqui
// acontece na interface (fundo_manter), como toda parada.
static void c_fundo(int argc, char** argv)
{
  if (argc >= 3 && eh(argv[1], "parar")) {
    printf(sistema_fundo_parar_nome(argv[2]) ? "pedido de parada enviado para '%s'\n"
                                             : "'%s' nao esta rodando\n", argv[2]);
    return;
  }
  if (argc >= 2) { mostrar_uso("fundo"); return; }
  fundo_info_t l[FUNDO_MAX];
  const int n = sistema_fundo_listar(l, FUNDO_MAX);
  if (n == 0) { printf("nenhuma parte de fundo rodando\n"); }
  for (int i = 0; i < n; ++i) {
    printf("  %-12s ha %5lus  %s\n", l[i].nome, (unsigned long)l[i].ha_s, l[i].resumo);
  }
}

// Devolve (ou tira) o barulho de uma etiqueta em tempo de execucao. O sistema
// silencia "eh_sdio" e "sdmmc_io" no boot - eles inundam o console quando o
// enlace com o C6 morre. Silenciar sem deixar como reverter seria esconder
// informacao; com isto, o cru esta a um comando de distancia.
static const char* const NIVEL_NOME[6] = { "nada", "erro", "aviso", "info", "debug", "verboso" };

static int nivel_log(const char* s)
{
  for (int i = 0; i < 6; ++i) { if (eh(s, NIVEL_NOME[i])) { return i; } }
  char* fim = nullptr;
  const long n = strtol(s, &fim, 10);
  return (*s && fim && *fim == 0 && n >= 0 && n <= 5) ? (int)n : -1;
}

static void c_log(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("log"); return; }
  if (argc < 3) {
    const int n = (int)esp_log_level_get(argv[1]);
    printf("'%s' esta em %s (%d)\n", argv[1], (n >= 0 && n <= 5) ? NIVEL_NOME[n] : "?", n);
    return;
  }
  const int n = nivel_log(argv[2]);
  if (n < 0) {
    printf("nivel '%s' nao existe - use nada|erro|aviso|info|debug|verboso (ou 0-5)\n", argv[2]);
    return;
  }
  esp_log_level_set(argv[1], (esp_log_level_t)n);
  printf("'%s' agora em %s (%d)\n", argv[1], NIVEL_NOME[n], n);
}

// BANCADA DO PENDRIVE: ligar e devolver sem tocar na tela. Ligar tira o
// console do USB-C (o computador passa a ver o disco, nao a porta serial); ele
// volta quando o computador ejeta. O que aconteceu fica no log do cartao.
static void c_pendrive(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "ligar")) {
    printf(tab5_pendrive_iniciar() ? "ligando - o console sai do USB-C ate o computador ejetar\n"
                                   : "nao ligou (ja havia sessao, ou recusado - veja 'pendrive')\n");
    return;
  }
  if (argc >= 2 && eh(argv[1], "devolver")) { tab5_pendrive_devolver(); printf("devolvendo\n"); return; }
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  static const char* const FASE[] = { "parado", "preparando", "ativo", "devolvendo" };
  printf("fase %s, motivo %d, ultima por ejecao %s, sem ejetar %s, oferta %s, pc %s\n",
         FASE[e.fase & 3], (int)e.motivo, e.por_ejecao ? "sim" : "nao", e.sem_ejetar ? "sim" : "nao",
         e.oferta ? "sim" : "nao", tab5_pendrive_pc_presente() ? "sim" : "nao");
}

// ---------------------------------------------------------------------------
// TELA
// ---------------------------------------------------------------------------

// DIAGNOSTICO DA PISCADA AZUL, e interruptores para comparar sem regravar.
//
// PALAVRA RECONHECIDA OU NADA FEITO - e esta regra custou uma medicao. Uma
// versao anterior era "se nao for 'on', desligue": qualquer segunda palavra que
// nao casasse virava um priorizar_tela(false) SILENCIOSO, e uma medicao saiu
// com a prioridade em "padrao" sem ninguem ter pedido. Comando de diagnostico
// nao pode ter caminho destrutivo por omissao.
// BANCADA: a tela como esta agora, num .bmp do cartao (apps/Console).
// readRect devolve o RGB565 na ordem do LovyanGFX (bytes trocados); o filtro
// desfaz na copia que vai para o arquivo.
static void bmp_desinverter(uint16_t* l, int n) { for (int i = 0; i < n; ++i) { l[i] = (uint16_t)((l[i] << 8) | (l[i] >> 8)); } }
static void tela_foto(const char* nome)
{
  const int w = M5.Display.width(), h = M5.Display.height();
  uint16_t* px = (uint16_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
  if (!px) { printf("sem memoria para %dx%d\n", w, h); return; }
  M5.Display.readRect(0, 0, w, h, px);
  const bool ok = tab5_bmp_salvar("Console", nome, px, w, h, bmp_desinverter);
  heap_caps_free(px);
  printf(ok ? "gravada: /sdcard/apps/Console/%s.bmp (%dx%d)\n" : "falhou ao gravar %s.bmp\n", nome, w, h);
}

// BANCADA: clique e abrir app pela serial (ver sistema_simular_toque).
static void c_tocar(int argc, char** argv)
{
  if (argc < 3) { mostrar_uso("tocar"); return; }
  sistema_simular_toque(atoi(argv[1]), atoi(argv[2]));
  printf("toque em %s,%s\n", argv[1], argv[2]);
}

static void c_abrir(int argc, char** argv)
{
  if (argc < 2) {
    for (int i = 0; i < SISTEMA_NUM_APPS; ++i) { printf("  %s\n", SISTEMA_APPS[i]->nome); }
    return;
  }
  if (eh(argv[1], "home")) { sistema_simular_abrir(nullptr); printf("voltando para a home\n"); return; }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->nome, argv[1]) == 0) {
      sistema_simular_abrir(SISTEMA_APPS[i]);
      printf("abrindo %s\n", SISTEMA_APPS[i]->nome);
      return;
    }
  }
  printf("nao ha app '%s' - 'abrir' lista\n", argv[1]);
}

// ORDEM DOS ICONES NA HOME - a mesma que segurar e arrastar muda na tela.
static const app_t* app_por_nome(const char* nome)
{
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->nome, nome) == 0) { return SISTEMA_APPS[i]; }
  }
  return nullptr;
}

static void c_ordem(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "padrao")) {
    sistema_home_ordem_padrao();
    printf("ordem de fabrica (apps.cpp) restaurada\n");
  } else if (argc >= 3) {
    const app_t* app = app_por_nome(argv[1]);
    const int pos = atoi(argv[2]);
    if (!app) { printf("nao ha app '%s' - 'ordem' lista\n", argv[1]); return; }
    if (pos < 1 || pos > SISTEMA_NUM_APPS) { printf("posicao de 1 a %d\n", SISTEMA_NUM_APPS); return; }
    sistema_home_mover(app, pos - 1);
    printf("'%s' agora na posicao %d\n", app->nome, pos);
  } else if (argc == 2) {
    mostrar_uso("ordem");
    return;
  }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const app_t* app = sistema_home_app(i);
    if (app) { printf("  %2d  %s\n", i + 1, app->nome); }
  }
}

static void c_tela(int argc, char** argv)
{
  if (argc >= 2) {
    const char* o = argv[1];
    if (eh(o, "foto")) { tela_foto(argc >= 3 ? argv[2] : "tela"); return; }
    if (eh(o, "medir")) { tela_medir_marcar(); return; }
    if (eh(o, "banco")) { tab5_quadro_banco(); return; }
    if (eh(o, "rolar")) {
      // Rolagem pelo DMA (da pagina da frente) ou pela CPU (dentro da de
      // tras). Rastro que some com 'cpu' esta no caminho do DMA.
      extern bool g_tela_rolar_cpu;
      if (argc >= 3 && eh(argv[2], "cpu"))      { g_tela_rolar_cpu = true; }
      else if (argc >= 3 && eh(argv[2], "dma")) { g_tela_rolar_cpu = false; }
      else { printf("uso: tela rolar cpu|dma\n"); }
      printf("rolagem pela %s\n", g_tela_rolar_cpu ? "CPU" : "DMA (da frente)");
      return;
    }
    if (eh(o, "relogio")) {
      // MUDA O RELOGIO DE PIXEL EM EXECUCAO. E o unico ajuste que reduz a
      // demanda do painel em vez de disputar melhor - ver tab5_power.cpp.
      if (argc < 3) { printf("uso: tela relogio <mhz>   (ex.: tela relogio 60)\n"); return; }
      const int mhz = atoi(argv[2]);
      const bool ok = sistema_trocar_relogio_painel(mhz);   // na tarefa de interface
      // O painel remontado nasce com framebuffer em BRANCO (o
      // esp_lcd_new_panel_dpi aloca com calloc) - sem o redesenho a tela
      // ficava preta com o backlight aceso. GRAVA SO DEPOIS de aceito: se
      // gravasse antes e o painel recusasse, o proximo boot tentaria de novo o
      // mesmo valor ruim.
      if (ok) {
        sistema_pedir_redesenho_completo();
        ajustes_definir_relogio_painel((uint8_t)mhz);
      }
      printf(ok ? "painel remontado e gravado - vale no proximo boot tambem\n"
                : "nao subiu nesse relogio - voltei ao anterior\n");
    }
    else if (eh(o, "prioridade") && argc >= 3 && (eh(argv[2], "alta") || eh(argv[2], "on"))) {
      tab5_barramento_priorizar_tela(true);
    }
    else if (eh(o, "prioridade") && argc >= 3 && (eh(argv[2], "padrao") || eh(argv[2], "off"))) {
      tab5_barramento_priorizar_tela(false);
    }
    // Formas antigas, mantidas: 'tela on' / 'tela off'.
    else if (eh(o, "on")  || eh(o, "sim")) { tab5_barramento_priorizar_tela(true); }
    else if (eh(o, "off") || eh(o, "nao")) { tab5_barramento_priorizar_tela(false); }
    else {
      printf("nao conheco 'tela %s' - nada foi alterado.\n", o);
      mostrar_uso("tela");
      return;
    }
  }
  printf("--- tela ---\n");
  printf("  prioridade da tela no AXI : %s\n",
         tab5_barramento_priorizado() ? "ALTA" : "padrao (como vem de fabrica)");
  printf("  limpezas dispensadas      : %lu\n",
         (unsigned long)tab5_tela_limpezas_evitadas());
  {
    // Os detectores de hal/tab5_quadro. Sem 'pior': ler zera, e quem
    // acompanha o pior e o monitor, que vira evento.
    uint32_t periodo_us = 0;
    const uint32_t atrasos = tab5_quadro_atrasos(nullptr, &periodo_us);
    const uint32_t erros   = tab5_quadro_erros_dsi(nullptr, nullptr);
    printf("  quadros varridos          : %lu (periodo %lu.%lu ms)\n",
           (unsigned long)tab5_quadro_quadros(),
           (unsigned long)(periodo_us / 1000), (unsigned long)(periodo_us % 1000 / 100));
    printf("  underrun / atrasados / DSI: %lu / %lu / %lu\n",
           (unsigned long)tab5_quadro_underruns(), (unsigned long)atrasos, (unsigned long)erros);
    int nucleo = -1; char t0[16], t1[16];
    if (tab5_quadro_atraso_quem(&nucleo, t0, t1)) {
      printf("  ultimo atraso             : ISR no nucleo %d; rodando c0=%s c1=%s\n", nucleo, t0, t1);
    }
  }

  // A CONTA NA TELA, e nao so no comentario do fonte. Sao estes numeros que
  // dizem se mexer no relogio valeu: MB/s e o que o painel tira da PSRAM sem
  // parar, e e contra esse valor que todo o resto do sistema disputa.
  const int mhz = tab5_power_relogio_dpi();
  printf("  tela                      : %s%s\n", tab5_painel_nome(),
         tab5_painel_toque_gt911() ? "" : " (relogio fixo, repouso so de luz)");
  if (mhz > 0) {
    const double fps  = tab5_power_quadros_por_segundo();
    printf("  relogio do painel         : %d MHz\n", mhz);
    printf("  taxa de quadros           : %.1f Hz\n", fps);
    printf("  leitura continua da PSRAM : %.1f MB/s\n", fps * 720.0 * 1280.0 * 2.0 / 1e6);
  }

  tela_medir_mostrar();
  printf("\n  underrun > 0 = a piscada azul. 'help tela' lista as medidas e ajustes.\n");
}

// TESTE DO BACKLIGHT, sem gravar: achar o menor nivel que ainda acende e ver
// se a frequencia do PWM muda o piso. Driver de LED com tempo minimo de ligado
// nao obedece duty pequeno em frequencia alta (a 44,1 kHz o periodo e
// 22,7 us) - abaixo de certo ponto o brilho para de cair.
static void c_brilho(int argc, char** argv)
{
  if (argc < 2) {
    printf("brilho: %d (gravado %d)\n", (int)M5.Display.getBrightness(), (int)ajustes_brilho());
    return;
  }
  int b = atoi(argv[1]);
  if (b < 1)   { b = 1; }
  if (b > 255) { b = 255; }
  if (argc >= 3) {
    const uint32_t hz = (uint32_t)atoi(argv[2]);
    // Canal 7 do M5GFX no Tab5 -> timer (7 >> 1) & 3 = 3, modo lento.
    const esp_err_t e = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3, hz);
    printf("PWM do backlight: %lu Hz (%s)\n", (unsigned long)hz, esp_err_to_name(e));
  }
  M5.Display.setBrightness((uint8_t)b);
  printf("brilho %d = duty %.2f%% (nao gravado; Ajustes ou reiniciar voltam ao gravado)\n",
         b, (float)(((b * 257 + 64) >> 7)) * 100.0f / 512.0f);
}

// Abrir um app faz varias coisas de uma vez; cada opcao faz SO UMA, anotando a
// hora, para olhar a tela e ver qual pisca.
static void c_piscada(int argc, char** argv)
{
  const char* o = (argc >= 2) ? argv[1] : "";
  const unsigned long t = (unsigned long)(esp_timer_get_time() / 1000);
  if (eh(o, "brilho")) {
    printf("piscada: %lums - reescrevendo o MESMO brilho (%d)\n", t, (int)M5.Display.getBrightness());
    M5.Display.setBrightness(M5.Display.getBrightness());
  } else if (eh(o, "c6")) {
    printf("piscada: %lums - reescrevendo WLAN_PWR_EN=0 no expansor\n", t);
    tab5_power_radio(false);
  } else if (eh(o, "perfil")) {
    printf("piscada: %lums - reaplicando o perfil inteiro (trilhos + brilho + pm)\n", t);
    sistema_reaplicar_perfil();
  } else if (eh(o, "pm")) {
    esp_pm_config_t c = { };
    esp_pm_get_configuration(&c);
    printf("piscada: %lums - esp_pm_configure(%d..%d MHz) de novo\n", t, c.min_freq_mhz, c.max_freq_mhz);
    esp_pm_configure(&c);
  } else if (eh(o, "grove")) {
    printf("piscada: %lums - 5V do Grove OFF, 1s, ON (o C6L reinicia)\n", t);
    M5.Power.setExtOutput(false, m5::ext_PA);
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("piscada: %lums - 5V do Grove ON agora\n", (unsigned long)(esp_timer_get_time() / 1000));
    M5.Power.setExtOutput(true, m5::ext_PA);
  } else if (eh(o, "quadro")) {
    printf("piscada: %lums - redesenho completo pelo compositor\n", t);
    tela_virtual_invalidar_tudo();
    sistema_pedir_redesenho_completo();
  } else {
    mostrar_uso("piscada");
  }
}

// Liga/desliga o diagnostico. Nao le o painel daqui: o I2C e da tarefa de
// interface, e duas tarefas no mesmo barramento leem registrador trocado sem
// dar erro. Aqui so se aciona a bandeira; quem imprime e quem mede.
static void c_toque(int argc, char** argv)
{
  const bool ligar = !(argc >= 2 && eh(argv[1], "off"));
  sistema_diag_toque(ligar);
  printf(ligar ? "toque cada botao e veja onde o dedo cai.\n"
                 "compare o 'app (x,y)' com a posicao esperada do botao.\n"
                 "para desligar: toque off\n"
               : "diagnostico de toque desligado\n");
}

// Onde a interface esta AGORA - funciona com a tela congelada: o console e
// outra tarefa. Rode duas vezes: 'voltas' parado = laco parado nesse passo.
static void c_ui(int, char**)
{
  sistema_diag_t d;
  sistema_diag_interface(&d);
  printf("interface: %s\n  voltas %lu, ultima ha %lu ms, %s%s\n  dedo lido ha %ld ms\n",
         d.etapa, (unsigned long)d.voltas, (unsigned long)d.ms_desde_volta,
         d.app ? d.app : "home", d.repouso ? " (repouso)" : "",
         d.ms_desde_dedo == UINT32_MAX ? -1L : (long)d.ms_desde_dedo);
  printf("toque: %lu pulsos, %lu falhas de I2C, %lu presos, %lu s surdos, status GT911 %02X\n",
         (unsigned long)d.toque_pulsos, (unsigned long)d.toque_falhas, (unsigned long)d.toque_presos,
         (unsigned long)d.toque_surdos, (unsigned)d.toque_status);
}

// ---------------------------------------------------------------------------
// REDE
// ---------------------------------------------------------------------------

static void c_wifi(int argc, char** argv)
{
  if (argc < 2) {
    printf("wifi     : %s\n", tab5_wifi_descricao());
    if (tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
      const int r = (int)tab5_wifi_rssi();
      printf("rede     : %s   IP %s   ", tab5_wifi_ssid(), tab5_wifi_ip());
      if (r == TAB5_WIFI_RSSI_DESCONHECIDO) { printf("sinal --\n"); }
      else                                   { printf("sinal %d dBm\n", r); }
    }
    printf("guardadas: %d rede(s) no wifi.txt%s\n", tab5_wifi_n_conhecidas(),
           tab5_wifi_so_radio() ? "   (radio ligado so para ESP-NOW)" : "");
    printf("\n'help wifi' mostra como varrer, gravar, conectar e esquecer redes\n");
    return;
  }
  const char* o = argv[1];
  if (eh(o, "redes")) {
    // Exige a pilha de pe: sem esp_hosted no ar nao ha radio a quem pedir.
    if (!tab5_wifi_pilha_de_pe()) {
      printf("o WiFi ainda nao foi ligado neste boot - ligue em Ajustes e tente de novo\n");
      return;
    }
    tab5_wifi_varrer();
    return;
  }
  if (eh(o, "conectar")) {
    if (argc < 3) { mostrar_uso("wifi"); return; }
    if (!tab5_wifi_conhecida(argv[2])) {
      printf("'%s' nao esta guardada - use: wifi gravar \"%s\" <senha>\n", argv[2], argv[2]);
      return;
    }
    tab5_wifi_conectar(argv[2]);
    printf("trocando para '%s' - confira com 'wifi' em alguns segundos\n", argv[2]);
    return;
  }
  if (eh(o, "esquecer")) {
    if (argc < 3) { mostrar_uso("wifi"); return; }
    const char* ssid = argv[2];
    com_cartao([&]() -> int {
      const bool ok = tab5_wifi_esquecer(ssid);
      printf(ok ? "'%s' esquecida\n" : "'%s' nao estava guardada\n", ssid);
      return ok ? 0 : 1;
    });
    return;
  }
  if (eh(o, "gravar")) {
    if (argc < 3) { mostrar_uso("wifi"); return; }
    wifi_gravar(argv[2], argc >= 4 ? argv[3] : nullptr);
    return;
  }
  // Forma curta, a original: 'wifi <ssid> [senha]'.
  wifi_gravar(argv[1], argc >= 3 ? argv[2] : nullptr);
}

// Atalho antigo: 'redes' = 'wifi redes'.
static void c_redes(int, char**)
{
  char a0[] = "wifi", a1[] = "redes";
  char* a[] = { a0, a1 };
  c_wifi(2, a);
}

// BLUETOOTH: estado da stack, do enlace do Mesh e quem foi visto.
static void c_ble(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "varrer")) {
    printf(tab5_ble_varrer(6000) ? "varrendo por 6 s... rode 'ble' depois\n"
                                 : "nao deu para varrer agora (%s)\n", tab5_ble_descricao());
    return;
  }
  if (argc >= 2 && eh(argv[1], "desligar")) {
    sistema_bluetooth(false);
    printf("desligando - confira com 'ble' em alguns segundos\n");
    return;
  }
  if (argc >= 2 && eh(argv[1], "ligar")) {
    // Ligar com o C6 desligado pede o trilho (I2C), que so a interface fala.
    if (!tab5_wifi_pilha_de_pe()) { printf("o C6 ainda nao subiu neste boot - ligue em Ajustes\n"); return; }
    sistema_bluetooth(true);
    printf("ligando - confira com 'ble' em alguns segundos\n");
    return;
  }
  if (argc >= 2) { mostrar_uso("ble"); return; }
  printf("bluetooth: %s\n", tab5_ble_descricao());
  printf("mesh ble:  %s%s\n", mesh_ble_descricao(), mesh_ble_esperando_pin() ? " (esperando PIN)" : "");
  const int n = tab5_ble_n_vistos();
  if (n == 0) { printf("nenhum aparelho visto ainda - 'ble varrer' procura por 6 s\n"); }
  tab5_ble_visto_t v;
  for (int i = 0; i < n; ++i) {
    ceder(i);
    if (!tab5_ble_visto(i, &v)) { continue; }
    char end[18];
    tab5_ble_endereco_txt(v.addr, end);
    printf("  %s  %4d dBm  %-22s%s\n", end, (int)v.rssi, v.nome[0] ? v.nome : "(sem nome)",
           v.meshtastic ? "  MESHTASTIC" : "");
  }
}

// O estado real do ESP-NOW e do radio - sem depender da tela. O canal e
// perguntado AO C6 agora (nao o que o servico acha), que e o que decide se
// dois aparelhos se ouvem.
static void c_espnow(int argc, char** argv)
{
  if (argc >= 3 && eh(argv[1], "canal")) {
    const int c = atoi(argv[2]);
    printf(tab5_espnow_definir_canal((uint8_t)c) ? "pedido: canal %d\n" : "recusado (canal %d)\n", c);
    return;
  }
  if (argc >= 3 && eh(argv[1], "enviar")) {
    // A frase inteira: antes so a primeira palavra ia, o resto sumia calado.
    char msg[200];
    juntar(msg, sizeof(msg), argc, argv, 2);
    printf(tab5_espnow_enviar(msg) ? "na fila: '%s'\n" : "recusado: '%s'\n", msg);
    return;
  }
  if (argc >= 2) { mostrar_uso("espnow"); return; }
  uint8_t mac[6]; tab5_espnow_meu_mac(mac);
  uint8_t c = 0; wifi_second_chan_t sec;
  const bool leu = tab5_wifi_pilha_de_pe() && esp_wifi_get_channel(&c, &sec) == ESP_OK;
  printf("C6 firmware nosso: %s   enlace: %s   ESP-NOW no ar: %s\n",
         tab5_espnow_c6_compativel() ? "sim" : "NAO", tab5_wifi_c6_enlace() ? "sim" : "NAO",
         tab5_espnow_pronto() ? "sim" : "NAO");
  printf("WiFi: %s  (so radio: %s)\n", tab5_wifi_descricao(), tab5_wifi_so_radio() ? "sim" : "nao");
  printf("canal no C6 agora: %s%u   pedido: %s%u\n", leu ? "" : "? ", (unsigned)c,
         tab5_espnow_canal_fixo() ? "fixo " : "roteador ", (unsigned)tab5_espnow_canal_fixo());
  printf("meu MAC: %02X:%02X:%02X:%02X:%02X:%02X   msgs: %d   ouvidos: %d\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tab5_espnow_n(), tab5_espnow_vistos());
}

// GRAVAR O C6 PELO ENLACE ESTA DESLIGADO. A OTA pelo proprio SDIO nao tem
// volta quando a imagem nova nao fala - foi o que ja deixou o C6 mudo. Com o
// M5Stack ESP32 Downloader, gravar e restaurar e FISICO e sempre tem volta. As
// funcoes antigas (cmd_c6_ota/cmd_c6_gravar) estao no historico do git.
// ---------------------------------------------------------------------------
// GRAVAR O C6 PELO ENLACE (OTA pelo SDIO)
// ---------------------------------------------------------------------------
// So existe com o firmware do C6 da tabela nova (bootloader com rollback, dois
// slots de 1,875 MB, GUARDA DE IMAGEM NOVA no c6/main/app_main.c). O caminho:
//
//   1. le apps/Sistema/tab5_c6.bin INTEIRO para a PSRAM e confere antes de
//      tocar no C6: e imagem do IDF, e para ESP32-C6, e e o projeto tab5_c6;
//   2. begin (apaga o slot LIVRE) -> write em pedacos de 1536 -> end (o C6
//      valida a imagem inteira) -> activate (troca o boot; reinicia em 2 s).
//      Erro antes do activate: nada mudou, o firmware atual segue rodando;
//   3. o P4 se reinicia 4 s depois. No boot ele reseta o C6 (2o boot da
//      imagem nova - a guarda conta e tolera) e manda o VIVO, que a confirma.
//
// Se o VIVO nao vier (enlace nao fecha, imagem trava em laco), o proprio C6
// volta para a imagem anterior: 120 s, ou no 5o boot sem confirmacao.
#define C6_FW_APP     "Sistema"
#define C6_FW_ARQUIVO "tab5_c6.bin"
#define C6_PEDACO     1536          // EH_RPC_OTA_CHUNK_MAX
#define C6_FW_MAX     0x1E0000      // slot do c6/partitions.csv

static void cmd_c6_gravar(bool confirmado)
{
  if (!tab5_wifi_c6_enlace()) {
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
           C6_FW_APP, C6_FW_ARQUIVO);
    return;
  }

  // RPC vivo? Sem ele, cada pedido da OTA esperaria o tempo esgotado.
  eh_host_coprocessor_fwver_t v = { };
  if (eh_host_sys_get_cp_fw_version(&v) != ESP_OK) {
    printf("o C6 aceita o enlace mas nao responde a RPC - so pelo Downloader.\n");
    return;
  }

  if (!tab5_sd_montar()) { printf("cartao nao montou (%s)\n", tab5_sd_descricao()); return; }
  FILE* f = tab5_sd_abrir(C6_FW_APP, C6_FW_ARQUIVO, "rb");
  if (!f) { printf("nao achei apps/%s/%s\n", C6_FW_APP, C6_FW_ARQUIVO); tab5_sd_desmontar(); return; }
  fseek(f, 0, SEEK_END);
  const long total = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* img = (total > 0 && total <= C6_FW_MAX)
                   ? (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM) : nullptr;
  const size_t lidos = img ? fread(img, 1, (size_t)total, f) : 0;
  fclose(f);
  tab5_sd_desmontar();
  if (!img || lidos != (size_t)total) {
    printf("tamanho %ld invalido (max %u) ou leitura curta (%u)\n", total, (unsigned)C6_FW_MAX, (unsigned)lidos);
    heap_caps_free(img);
    return;
  }

  // CONFERENCIA ANTES DE TOCAR NO C6. Cabecalho do IDF: 0xE9 no byte 0,
  // chip_id no 12 (0x000D = ESP32-C6). Logo depois do cabecalho (24 B) e do
  // 1o segmento (8 B) vem o esp_app_desc_t: magic 0xABCD5432, versao no +16,
  // nome do projeto no +48.
  const uint32_t magic_desc = (uint32_t)img[32] | ((uint32_t)img[33] << 8) |
                              ((uint32_t)img[34] << 16) | ((uint32_t)img[35] << 24);
  char versao[33] = { 0 }, projeto[33] = { 0 };
  if (total > 112) { memcpy(versao, img + 48, 32); memcpy(projeto, img + 80, 32); }
  const unsigned chip = (unsigned)img[12] | ((unsigned)img[13] << 8);
  if (total <= 112 || img[0] != 0xE9 || chip != 0x000D || magic_desc != 0xABCD5432u
      || strcmp(projeto, "tab5_c6") != 0) {
    printf("%s nao e firmware do C6 deste projeto (magic 0x%02X, chip %u, projeto '%s')\n",
           C6_FW_ARQUIVO, img[0], chip, projeto);
    heap_caps_free(img);
    return;
  }
  printf("imagem %s, %ld bytes - gravando no slot livre do C6\n", versao, total);

  esp_err_t r = eh_host_cp_ota_begin();
  if (r != ESP_OK) {
    printf("begin falhou: %s - nada mudou\n", esp_err_to_name(r));
    heap_caps_free(img);
    return;
  }
  int pct_ant = -1;
  for (long feito = 0; feito < total; ) {
    const uint32_t n = (uint32_t)((total - feito) > C6_PEDACO ? C6_PEDACO : (total - feito));
    r = eh_host_cp_ota_write(img + feito, n);
    if (r != ESP_OK) {
      printf("\nwrite falhou em %ld/%ld: %s - ABORTADO, o firmware atual segue\n",
             feito, total, esp_err_to_name(r));
      heap_caps_free(img);
      return;
    }
    feito += n;
    const int pct = (int)(feito * 100 / total);
    if (pct / 10 != pct_ant) { printf("  %d%%\n", pct); pct_ant = pct / 10; }
    ceder((int)(feito / C6_PEDACO));
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
  if (argc >= 2 && (eh(argv[1], "ota") || eh(argv[1], "gravar"))) {
    cmd_c6_gravar(argc >= 3 && eh(argv[2], "sim"));
    return;
  }
  cmd_c6();
}

// ---------------------------------------------------------------------------
// FAREJO (modo promiscuo no C6) - e a janela para as capacidades de radio que
// a gravacao do C6 trouxe. 'info' mostra o que este C6 sabe fazer (CSI, FTM,
// 802.15.4): o bit so acende se o modulo foi compilado no firmware gravado.
// ---------------------------------------------------------------------------
static void farejo_estado(void)
{
  printf("C6 sabe farejar: %s   farejo ativo: %s\n",
         tab5_farejo_c6_suporta() ? "sim" : "NAO (firmware v<3 ou sem a flag)",
         tab5_farejo_ativo() ? "sim" : "nao");
  const int n = tab5_farejo_n();
  printf("MACs ouvidos: %d   lotes: %lu   perdidos no C6: %lu\n",
         n, (unsigned long)tab5_farejo_lotes(), (unsigned long)tab5_farejo_perdidos());
  for (int i = 0; i < n && i < 20; ++i) {
    farejo_item_t it;
    if (!tab5_farejo_item(i, &it)) { continue; }
    ceder(i);
    printf("  %02X:%02X:%02X:%02X:%02X:%02X  ch%-2u  rssi %4d (max %4d)  tipos 0x%02X  quadros %lu\n",
           it.mac[0], it.mac[1], it.mac[2], it.mac[3], it.mac[4], it.mac[5],
           it.canal, it.rssi_ult, it.rssi_max, it.tipos, (unsigned long)it.quadros);
  }
  if (n > 20) { printf("  ... e mais %d\n", n - 20); }
}

static void farejo_info(void)
{
  c6_info_t in;
  if (!tab5_farejo_info_c6(&in, 1500)) {
    printf("o C6 nao respondeu (sem enlace, firmware de fabrica, ou v<3).\n");
    return;
  }
  printf("C6 contrato v%u   canal %u   farejando: %s", in.proto, in.canal, in.farejando ? "sim" : "nao");
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
         (unsigned long)in.no_ar_s, (unsigned long)in.quadros);
  // Firmware do C6 m5tabos-c6-3 em diante: um caminho so; csi_modo 1 = ligado.
  // ESP_FAIL aqui = o WiFi do C6 subiu com csi_enable 0 (ver tab5_wifi.cpp).
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


static void c_farejo(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "info"))  { farejo_info();     return; }
  if (argc >= 2 && eh(argv[1], "off"))   { tab5_farejo_desligar(); printf("farejo desligado\n"); return; }
  if (argc >= 2 && eh(argv[1], "limpar")){ tab5_farejo_limpar(); printf("tabela zerada\n"); return; }
  if (argc >= 2 && eh(argv[1], "on")) {
    c6_farejo_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.filtro = 0;                              // os dois tipos
    cfg.canais = 0;                              // canal atual (associado ao roteador)
    cfg.lote_ms = (argc >= 3) ? (uint16_t)atoi(argv[2]) : 250;
    // Mascara de canais em hex opcional: 'farejo on <lote_ms> <mask>'. Com
    // mais de um canal, SOLTE o roteador antes ('espnow canal 1' ja faz isso),
    // senao o C6 fica preso no canal do AP.
    if (argc >= 4) { cfg.canais = (uint16_t)strtoul(argv[3], nullptr, 16); }
    if (!tab5_farejo_ligar(&cfg)) {
      printf("nao liguei: o C6 tem o firmware nosso (v3)? WiFi ligado? ('farejo info')\n");
      return;
    }
    printf("farejo ligado (lote %u ms, canais 0x%04X=0 e o atual). Rode 'farejo' para ver.\n",
           (unsigned)cfg.lote_ms, (unsigned)cfg.canais);
    return;
  }
  if (argc >= 2) { mostrar_uso("farejo"); return; }
  farejo_estado();
}

// ---------------------------------------------------------------------------
// RADIO - teste dos extras do C6 (CSI, FTM, 802.15.4)
// ---------------------------------------------------------------------------
// Liga cada extra por alguns segundos e conta o que volta. Serve para provar a
// gravacao do C6 (ligar, receber e desligar sem derrubar o C6 nem o WiFi) - o
// que so se conserta com o aparelho aberto. Os contadores sao escritos na
// tarefa de eventos do esp_hosted e lidos aqui: volateis, sem trava.
//
// O CSI passa pelo servico tab5_csi (o mesmo que o Radar usa): o peer_data
// guarda UM ouvinte por mensagem, e registrar o CSI aqui roubaria o do servico.
static volatile uint32_t s_rx_csi = 0, s_rx_154 = 0, s_rx_ftm = 0;
static volatile int      s_rx_rssi = 0, s_rx_canal = 0, s_rx_len = 0;
static c6_ftm_rep_t      s_rx_ftm_rep;
static bool              s_radio_registrado = false;
// CSI por formato de quadro (C6_CSI_FORMATO: 0 11b ... 7 HE TB) e os avisos
// que mudam a leitura dos bytes.
static volatile uint32_t s_csi_fmt[16], s_csi_w0_ruim = 0, s_csi_cortado = 0, s_csi_len_max = 0;

static void radio_csi_rx(const c6_csi_cab_t* c, const uint8_t*, uint16_t)
{
  s_rx_rssi = c->rssi; s_rx_canal = c->canal; s_rx_len = c->len;
  s_csi_fmt[C6_CSI_FORMATO(c->flags)] = s_csi_fmt[C6_CSI_FORMATO(c->flags)] + 1;
  if (c->flags & C6_CSI_F_W0_RUIM)  { s_csi_w0_ruim = s_csi_w0_ruim + 1; }
  if (c->flags & C6_CSI_F_CORTADO)  { s_csi_cortado = s_csi_cortado + 1; }
  if (c->len > s_csi_len_max)       { s_csi_len_max = c->len; }
  s_rx_csi = s_rx_csi + 1;
}

static void radio_rx(uint32_t msg_id, const uint8_t* d, size_t n, void*)
{
  if (msg_id == C6_MSG_154 && n >= sizeof(c6_154_cab_t)) {
    c6_154_cab_t c; memcpy(&c, d, sizeof(c));
    s_rx_rssi = c.rssi; s_rx_canal = c.canal; s_rx_len = c.len;
    s_rx_154 = s_rx_154 + 1;
  } else if (msg_id == C6_MSG_FTM && n >= sizeof(c6_ftm_rep_t)) {
    memcpy(&s_rx_ftm_rep, d, sizeof(s_rx_ftm_rep));
    s_rx_ftm = s_rx_ftm + 1;
  }
}

static bool radio_preparar(void)
{
  if (!tab5_wifi_c6_nosso() || !tab5_wifi_c6_enlace()) {
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

// Liga, renova a cada 3 s (o C6 desliga sozinho em 10 s sem renovar), mostra
// a contagem a cada segundo e desliga no fim.
//
// ping_meio: pinga o roteador na metade da janela, com o recurso LIGADO - e o
// teste que separa "associado" de "passando dados".
static void ping_roteador(const char* rotulo);

static void radio_janela(uint8_t sub, const void* cfg_on, const void* cfg_off, size_t n,
                         int segundos, volatile uint32_t* cont, bool ping_meio = false)
{
  const uint32_t antes = *cont;
  for (int s = 0; s < segundos; ++s) {
    if (ping_meio && s > 0 && s == segundos / 2) { ping_roteador("  WiFi DURANTE"); }
    if (s % 3 == 0) { radio_mandar(sub, cfg_on, n); }
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("  %2d s: %lu recebidos   ultimo: canal %d  rssi %d  %d B\n", s + 1,
           (unsigned long)(*cont - antes), s_rx_canal, s_rx_rssi, s_rx_len);
  }
  radio_mandar(sub, cfg_off, n);
  printf("desligado. O que o C6 diz (ve tambem se o 'no ar' nao zerou):\n");
  farejo_info();
}

static void c_radio(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("radio"); return; }
  if (!radio_preparar()) { return; }
  // csi [s] | 154 [canal] [s]
  const bool z154 = eh(argv[1], "154");
  const int  i_seg = z154 ? 3 : 2;
  int seg = (argc > i_seg) ? atoi(argv[i_seg]) : 10;
  if (seg < 1 || seg > 120) { seg = 10; }

  if (eh(argv[1], "csi")) {
    // Pelo servico tab5_csi: ele pede ao C6 so o CSI do roteador, pinga o
    // roteador a 20 Hz como fonte (o beacon de 2,4 GHz, a 1 Mbps, nao gera
    // CSI) e mantem o receptor do C6 acordado. Aqui so se conta o que chega e
    // se mostra a medida de movimento - parado, sem ninguem andando perto, a
    // razao fica perto de 1.
    printf("CSI por %d s (1 record a cada 100 ms, so do roteador; farejo %s).\n"
           "Deixe o Tab5 apoiado: os 3 primeiros segundos aprendem o 'parado'.\n",
           seg, tab5_farejo_ativo() ? "LIGADO" : "desligado");
    memset((void*)s_csi_fmt, 0, sizeof(s_csi_fmt));
    s_csi_w0_ruim = 0; s_csi_cortado = 0; s_csi_len_max = 0;
    const uint32_t antes = s_rx_csi;
    tab5_csi_ouvinte(radio_csi_rx);
    tab5_csi_ligar(TAB5_CSI_CONSOLE);
    static const char* const NIVEL[3] = { "parado", "leve", "FORTE" };
    for (int i = 0; i < seg; ++i) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      tab5_csi_estado_t e;
      tab5_csi_estado(&e);
      printf("  %2d s: %lu recebidos  rssi %d  %d B   ", i + 1, (unsigned long)(s_rx_csi - antes),
             s_rx_rssi, s_rx_len);
      switch (e.fase) {
        case TAB5_CSI_SEM_C6:          printf("C6 sem CSI\n"); break;
        case TAB5_CSI_SEM_ROTEADOR:    printf("sem roteador\n"); break;
        case TAB5_CSI_APRENDENDO:      printf("aprendendo o parado...\n"); break;
        case TAB5_CSI_APARELHO_MEXENDO:printf("Tab5 mexendo\n"); break;
        case TAB5_CSI_MEDINDO:         printf("movimento: %s (x%.1f)\n", NIVEL[e.nivel], (double)e.razao); break;
        default:                       printf("\n"); break;
      }
    }
    tab5_csi_desligar(TAB5_CSI_CONSOLE);
    tab5_csi_ouvinte(nullptr);
    static const char* const FMT[8] = { "11b", "11g", "HT", "VHT", "HE-SU", "HE-MU", "HE-ERSU", "HE-TB" };
    printf("por formato:");
    for (int f = 0; f < 16; ++f) {
      if (s_csi_fmt[f]) { printf("  %s %lu", f < 8 ? FMT[f] : "?", (unsigned long)s_csi_fmt[f]); }
    }
    printf("\nmaior record %lu B   1a palavra invalida em %lu   cortados %lu\n",
           (unsigned long)s_csi_len_max, (unsigned long)s_csi_w0_ruim, (unsigned long)s_csi_cortado);
    return;
  }
  if (eh(argv[1], "154")) {
    c6_154_cfg_t on, off;
    memset(&on, 0, sizeof(on)); memset(&off, 0, sizeof(off));
    on.liga = 1;
    on.canal = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 15;
    if (on.canal < 11 || on.canal > 26) { printf("canal 802.15.4 vai de 11 a 26\n"); return; }
    on.promiscuo = 1;
    printf("802.15.4 no canal %u por %d s (so ouvindo; sem Zigbee/Thread por perto, 0 e normal):\n",
           (unsigned)on.canal, seg);
    // O 802.15.4 divide a antena com o WiFi: o teste mede o WiFi antes, com
    // ele ligado e depois. Os tres em 4/4 = a coexistencia esta funcionando.
    ping_roteador("  WiFi ANTES");
    radio_janela(C6_RX_154, &on, &off, sizeof(on), seg, &s_rx_154, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    ping_roteador("  WiFi DEPOIS");
    return;
  }
  if (eh(argv[1], "ftm")) {
    // Alvo: o roteador conectado (MAC e canal dele).
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { printf("sem roteador conectado\n"); return; }
    c6_ftm_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.peer, ap.bssid, 6);
    c.canal = ap.primary;
    c.frm_count = 16;
    printf("FTM contra %02X:%02X:%02X:%02X:%02X:%02X (canal %u, FTM %s no anuncio)...\n",
           c.peer[0], c.peer[1], c.peer[2], c.peer[3], c.peer[4], c.peer[5], (unsigned)c.canal,
           ap.ftm_responder ? "SIM" : "nao");
    const uint32_t antes = s_rx_ftm;
    radio_mandar(C6_RX_FTM, &c, sizeof(c));
    for (int i = 0; i < 50 && s_rx_ftm == antes; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
    if (s_rx_ftm == antes) { printf("sem resposta do C6 em 5 s\n"); return; }
    const c6_ftm_rep_t& r = s_rx_ftm_rep;
    if (r.status == 0) {
      printf("ok: distancia %lu cm, rtt %lu ns (%u quadros)\n",
             (unsigned long)r.dist_est_cm, (unsigned long)r.rtt_est_ns, (unsigned)r.num);
    } else {
      if (r.status == C6_FTM_NAO_INICIOU) {
        // O C6 nem mandou a requisicao: o motivo e o esp_err_t do driver dele.
        const esp_err_t e = (esp_err_t)(int32_t)r.rtt_raw_ns;
        printf("o C6 nao iniciou a sessao: %s\n", esp_err_to_name(e));
        if (e == ESP_ERR_NOT_SUPPORTED) {
          // esp_wifi/src/wifi_init.c apaga o bit do FTM initiator nos C6 v0.0
          // e v0.1 (errata do silicio). Nao ha contorno honesto: forcar o bit
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
  mostrar_uso("radio");
}

// ---------------------------------------------------------------------------
// PING - separa "o WiFi nao passa dados" de "a internet/servidor nao responde"
// ---------------------------------------------------------------------------
static volatile int s_ping_ok = 0, s_ping_perdidos = 0;
static volatile uint32_t s_ping_ms_soma = 0;

static void ping_ok(esp_ping_handle_t h, void*)
{
  uint32_t ms = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
  s_ping_ok = s_ping_ok + 1;
  s_ping_ms_soma = s_ping_ms_soma + ms;
}
static void ping_perdido(esp_ping_handle_t, void*) { s_ping_perdidos = s_ping_perdidos + 1; }

static void ping_um(const char* rotulo, const ip_addr_t& alvo)
{
  s_ping_ok = 0; s_ping_perdidos = 0;
  s_ping_ms_soma = 0;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = alvo;
  cfg.count = 4;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 1500;
  esp_ping_callbacks_t cb = { };
  cb.on_ping_success = ping_ok;
  cb.on_ping_timeout = ping_perdido;
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { printf("%s: nao deu para criar o ping\n", rotulo); return; }
  esp_ping_start(h);
  for (int i = 0; i < 80 && s_ping_ok + s_ping_perdidos < 4; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
  esp_ping_stop(h);
  esp_ping_delete_session(h);
  if (s_ping_ok) {
    printf("%-22s %d/4 ok   media %lu ms\n", rotulo, s_ping_ok,
           (unsigned long)(s_ping_ms_soma / (uint32_t)s_ping_ok));
  } else {
    printf("%-22s 0/4 - SEM RESPOSTA\n", rotulo);
  }
}

static void ping_roteador(const char* rotulo)
{
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.gw.addr == 0) {
    printf("%-22s sem IP - WiFi desconectado\n", rotulo);
    return;
  }
  ip_addr_t a = { };
  a.type = IPADDR_TYPE_V4;
  a.u_addr.ip4.addr = ip.gw.addr;
  ping_um(rotulo, a);
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
    // Um alvo so: IP ou nome.
    struct addrinfo* res = nullptr;
    if (getaddrinfo(argv[1], nullptr, nullptr, &res) != 0 || !res) {
      printf("%s: o DNS nao resolveu (sem internet ou nome errado)\n", argv[1]);
      return;
    }
    a.type = IPADDR_TYPE_V4;
    a.u_addr.ip4.addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    ping_um(argv[1], a);
    return;
  }
  // Sem alvo: o roteador (o WiFi passa dados?) e a internet (passa do roteador?).
  a.type = IPADDR_TYPE_V4;
  a.u_addr.ip4.addr = ip.gw.addr;
  ping_um("roteador (gateway)", a);
  a.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
  ping_um("internet (8.8.8.8)", a);
}

// ---------------------------------------------------------------------------
// MESHTASTIC
// ---------------------------------------------------------------------------

// Lista compacta so com numero e nome, para consultar antes de enviar.
static void mesh_nos(void)
{
  const int n = tab5_mesh_num_nos();
  printf("%d no(s) - use o numero ou o nome curto no 'mesh enviar para'\n", n);
  for (int i = 0; i < n; ++i) {
    ceder(i);
    mesh_no_t no;
    if (!tab5_mesh_no(i, &no)) { continue; }
    printf("  %08lX  %-6.6s  %s\n", (unsigned long)no.num, no.curto,
           no.longo[0] ? no.longo : "(sem nome)");
  }
}

// argv[0] = "enviar". Chamado por 'mesh enviar ...' e pelo atalho 'enviar ...'.
static void mesh_enviar(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "para")) {
    // "enviar para <numero|nome> <texto>" - mensagem DIRETA.
    //
    // Aceita o numero em hexadecimal (como o 'mesh nos' imprime) ou o nome
    // curto, porque decorar F115C354 nao e razoavel. O nome curto e comparado
    // sem diferenciar maiuscula: o Meshtastic usa maiusculas em alguns nos e
    // nao em outros, e errar isso daria "no nao encontrado" sem explicacao.
    if (argc < 4) { mostrar_uso("mesh"); return; }
    uint32_t alvo = 0;
    const char* quem = argv[2];

    char* fim = nullptr;
    const unsigned long v = strtoul(quem, &fim, 16);
    if (fim && *fim == 0 && strlen(quem) >= 6) { alvo = (uint32_t)v; }

    if (alvo == 0) {
      const int n = tab5_mesh_num_nos();
      for (int i = 0; i < n && alvo == 0; ++i) {
        mesh_no_t no;
        if (!tab5_mesh_no(i, &no)) { continue; }
        if (strcasecmp(no.curto, quem) == 0) { alvo = no.num; }
      }
    }
    if (alvo == 0) { printf("no '%s' nao encontrado - veja 'mesh nos'\n", quem); return; }

    char msg[160];
    juntar(msg, sizeof(msg), argc, argv, 3);
    printf(tab5_mesh_enviar(alvo, 0, msg)
           ? "enviado para %08lX\n" : "falhou (enlace mudo?) %08lX\n", (unsigned long)alvo);
    return;
  }
  if (argc < 2) { mostrar_uso("mesh"); return; }
  // "enviar <texto>" difunde no canal 0 (LongFast).
  char msg[160];
  juntar(msg, sizeof(msg), argc, argv, 1);
  printf(tab5_mesh_enviar(0xFFFFFFFFu, 0, msg) ? "enviado\n" : "falhou (enlace mudo?)\n");
}

// DIAGNOSTICO DE TRUNCAMENTO de mensagens do Mesh. O texto aqui sai CRU - sem
// quebra de linha, sem recorte de balao, sem fonte proporcional no meio - e
// com o TAMANHO EM BYTES ao lado, exatamente para separar "o dado que chegou
// esta cortado" (bug de captura) de "a tela que desenha esta cortando" (bug de
// desenho).
//
// JUNTA TODAS AS CONVERSAS (cada uma mora num arquivo proprio no cartao - ver
// tab5_mesh_historico()) porque nao ha "a conversa aberta" aqui no console, so
// na tela. Ordena por horario, mais recente primeiro.
static void mesh_msgs(int quantidade)
{
  if (quantidade < 1)   { quantidade = 1; }
  if (quantidade > 200) { quantidade = 200; }   // mesmo teto de tab5_mesh_historico()

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

    // Insertion sort: comando manual, no maximo algumas centenas de itens.
    for (int i = 1; i < n_todas; ++i) {
      mesh_msg_t chave = todas[i];
      int j = i - 1;
      while (j >= 0 && todas[j].quando < chave.quando) { todas[j + 1] = todas[j]; --j; }
      todas[j + 1] = chave;
    }

    const int mostrar = (quantidade < n_todas) ? quantidade : n_todas;
    printf("%d mensagem(ns) em %d conversa(s), mostrando as %d mais recentes:\n",
           n_todas, n_conv, mostrar);
    for (int i = 0; i < mostrar; ++i) {
      ceder(i);
      const mesh_msg_t& m = todas[i];
      printf("[%3u bytes] de %08lX (%lus): %s\n",
             (unsigned)strlen(m.texto), (unsigned long)m.de,
             (unsigned long)m.quando, m.texto);
    }
  }
  if (todas) { heap_caps_free(todas); }
  if (buf)   { heap_caps_free(buf); }
}

// A ficha completa: enlace, saude, cada no e as mensagens.
static void mesh_completo(void)
{
  printf("enlace: %s\n", tab5_mesh_no_ar() ? "no ar" : "SEM QUADRO recente");
  printf("meu no: %08lX   firmware do C6L: %s\n",
         (unsigned long)tab5_mesh_meu_numero(),
         tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : "(nao veio)");

  mesh_saude_t sa; tab5_mesh_saude(&sa);
  printf("canal %.1f%%  airtime %.2f%%  uptime %lus  nos %u/%u\n",
         sa.uso_canal, sa.airtime_tx, (unsigned long)sa.uptime_s,
         (unsigned)sa.nos_online, (unsigned)sa.nos_total);

  // FICHA COMPLETA DE CADA NO, e nao a linha resumida. Na tela o espaco manda;
  // aqui nao ha essa pressao, e e aqui que se responde "com que aparelho eu
  // estou falando". Nome longo e curto sao etiquetas que o dono troca quando
  // quer; id, MAC e modelo sao o aparelho.
  const int n = tab5_mesh_num_nos();
  printf("\n%d no(s) (%lu despejo(s) da tabela desde o boot - ver achar_no):\n",
         n, (unsigned long)tab5_mesh_despejos());
  for (int i = 0; i < n; ++i) {
    ceder(i);
    mesh_no_t no;
    if (!tab5_mesh_no(i, &no)) { continue; }

    const char* hw = mesh_hw_nome(no.hw);
    char hwtxt[20];
    if (!hw) { snprintf(hwtxt, sizeof(hwtxt), "modelo %u", (unsigned)no.hw); hw = hwtxt; }

    printf("  %-20.20s [%s]\n", no.longo[0] ? no.longo : "(sem nome)", no.curto);
    printf("      num %08lX   id %-10s   %s\n",
           (unsigned long)no.num, no.id[0] ? no.id : "(nao veio)", hw);

    // MAC so quando ele MANDOU. O campo e marcado como obsoleto no protobuf e
    // firmware novo costuma omitir - imprimir 00:00:00:00:00:00 ali seria
    // apresentar a ausencia como se fosse um endereco.
    if (no.tem_mac) {
      printf("      mac %02X:%02X:%02X:%02X:%02X:%02X\n",
             no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      printf("      mac (nao enviado - campo obsoleto no protobuf; o id vem dele)\n");
    }

    // saltos == 255 e o sentinela de "desconhecido" (MESH_SALTOS_DESCONHECIDO)
    // - imprimir o numero cru pareceria um dado real em vez de "ainda nao sei".
    char saltos_txt[8];
    if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(saltos_txt, sizeof(saltos_txt), "?"); }
    else { snprintf(saltos_txt, sizeof(saltos_txt), "%u", (unsigned)no.saltos); }
    printf("      snr %.1f  bat %u%%  saltos %s%s",
           no.snr, (unsigned)no.bateria, saltos_txt, no.via_mqtt ? "  (via MQTT)" : "");
    if (no.tem_pos) { printf("  pos %.5f,%.5f", no.lat_i / 1e7, no.lon_i / 1e7); }
    printf("\n");
  }

  uint32_t q, pk, tel, ls, tx, ni; int var;
  tab5_mesh_contadores(&q, &pk, &tel, &ls, &tx, &ni, &var);
  printf("\nrecebidos: %lu quadros, %lu pacotes, %lu nodeinfo, %lu telemetria "
         "(%lu localstats), %lu texto | ultima variante de Telemetry: %d\n",
         (unsigned long)q, (unsigned long)pk, (unsigned long)ni, (unsigned long)tel,
         (unsigned long)ls, (unsigned long)tx, var);
  // Termometro de qualidade de rota: ver tab5_mesh_routing_extra(). Alto NAO e
  // erro - e "alguem ouviu" chegando mais facil que a confirmacao do radio.
  printf("routing extra apos confirmado: %lu (nao muda o estado, so o mostra)\n",
         (unsigned long)tab5_mesh_routing_extra());

  const int m = tab5_mesh_num_msgs();
  printf("\n%d mensagem(ns), %d nao lida(s):\n", m, tab5_mesh_nao_lidas());
  for (int i = 0; i < m; ++i) {
    ceder(i);
    mesh_msg_t ms;
    if (!tab5_mesh_msg(i, &ms)) { continue; }
    // Estado de entrega so faz sentido nas NOSSAS: mensagem recebida nao tem o
    // que confirmar. O motivo da falha vem do Routing.Error do radio.
    char marca[40] = "";
    switch (ms.envio) {
      case MESH_ENVIANDO:     snprintf(marca, sizeof(marca), " [enviando]"); break;
      case MESH_ENTREGUE:     snprintf(marca, sizeof(marca), " [entregue]"); break;
      case MESH_SEM_RESPOSTA: snprintf(marca, sizeof(marca), " [sem resposta]"); break;
      case MESH_FALHOU:       snprintf(marca, sizeof(marca), " [FALHOU: %s]",
                                       mesh_erro_nome(ms.erro)); break;
      default: break;
    }
    // ENLACE CRU DESTA MENSAGEM (via_mqtt/saltos do proprio MeshPacket, ANTES
    // do fallback que a tela faz para o enlace do no - ver tab5_mesh_no_enlace).
    char enlace[24];
    if (ms.via_mqtt) { snprintf(enlace, sizeof(enlace), "mqtt"); }
    else if (ms.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(enlace, sizeof(enlace), "saltos ?"); }
    else { snprintf(enlace, sizeof(enlace), "%u salto(s)", (unsigned)ms.saltos); }
    printf("  de %08lX (snr %.1f, %s): %s%s\n",
           (unsigned long)ms.de, ms.snr, enlace, ms.texto, marca);
  }
}

static void c_mesh(int argc, char** argv)
{
  if (argc < 2) { mesh_completo(); return; }
  const char* o = argv[1];
  if (eh(o, "nos"))    { mesh_nos(); return; }
  if (eh(o, "msgs"))   { mesh_msgs(argc >= 3 ? atoi(argv[2]) : 20); return; }
  if (eh(o, "enviar")) { mesh_enviar(argc - 1, argv + 1); return; }
  if (eh(o, "casa")) {
    // POSICAO DE REFERENCIA para a conversa mostrar distancia/direcao ate uma
    // localizacao recebida - ver achar_coord() em app_mesh.cpp. Fica em
    // ajustes.cpp (NVS), nao no Mesh: e do usuario, nao do servico.
    if (argc < 4) {
      float lat, lon;
      if (ajustes_casa(&lat, &lon)) { printf("casa: %.6f, %.6f\n", (double)lat, (double)lon); }
      else { printf("casa: nao configurada. uso: mesh casa <latitude> <longitude>\n"); }
      return;
    }
    const float lat = strtof(argv[2], nullptr);
    const float lon = strtof(argv[3], nullptr);
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
      printf("fora da faixa (latitude -90..90, longitude -180..180)\n");
    } else {
      ajustes_definir_casa(lat, lon);
      printf("casa gravada: %.6f, %.6f\n", (double)lat, (double)lon);
    }
    return;
  }
  if (eh(o, "pausar")) {
    // PARA DE FALAR COM O RADIO SEM DESLIGAR NADA - a UART para, o Grove e
    // devolvido, mas o trilho de 5V continua ligado e o modulo continua no ar.
    // Isola uma pergunta: a instabilidade vem do que o M5tabOS manda pela
    // serial, ou da POSICAO do modulo? Com isto ele fica onde estava, e so o
    // trafego para.
    if (tab5_mesh_parar()) {
      printf("servico da malha parado - Grove liberado, mas o 5V continua ligado\n"
             "e o modulo continua no ar (o BLE dele nao e afetado por isto).\n"
             "use 'mesh retomar' para o M5tabOS voltar a conversar com ele.\n");
    } else {
      printf("nao consegui parar agora (a tarefa nao confirmou em 1s) - tente de novo\n");
    }
    return;
  }
  if (eh(o, "retomar")) {
    tab5_mesh_iniciar();
    printf("pedido de retomada enviado - rode 'mesh' em alguns segundos para conferir.\n");
    return;
  }
  mostrar_uso("mesh");
}

// Atalhos antigos: 'nos' e 'enviar ...' sem o 'mesh' na frente.
static void c_nos(int, char**)               { mesh_nos(); }
static void c_enviar(int argc, char** argv)  { mesh_enviar(argc, argv); }

// ---------------------------------------------------------------------------
// BANCADA
// ---------------------------------------------------------------------------

static void c_grove(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "trocar")) {
    // TROQUEI O MODULO NO CABO. Para o servico, devolve a porta, sonda de novo
    // e sobe o que couber. Feito pelo sistema, na tarefa de interface - daqui
    // nao da para mexer na UART de outra tarefa com seguranca.
    if (sistema_grove_redetectar()) {
      printf("pedido feito - a interface sonda no proximo ciclo (uns 4s).\n"
             "rode 'grove' depois para ver o que achou.\n");
    } else {
      printf("um app esta usando o Grove agora - feche-o e tente de novo\n");
    }
    return;
  }
  // 'grove cru <baud>', ou a forma antiga 'grove <baud>': despeja os bytes
  // crus, para OLHAR em vez de deduzir.
  const char* baud = nullptr;
  if (argc >= 3 && eh(argv[1], "cru"))          { baud = argv[2]; }
  else if (argc >= 2 && atoi(argv[1]) > 0)      { baud = argv[1]; }
  if (baud) { tab5_grove_despejar(atoi(baud), 2500); return; }
  if (argc >= 2) { mostrar_uso("grove"); return; }

  // SO RELATA - nao sonda. Sondar daqui era um bug silencioso: a sonda escreve
  // e le na UART, e com o servico Meshtastic de pe a porta e DELE. Duas
  // tarefas na mesma UART embaralham o fluxo e o enlace caia por causa do
  // comando que deveria so diagnosticar.
  const grove_dono_t dono = tab5_grove_dono();
  printf("Grove: %s\n", grove_nome(tab5_grove_tipo()));
  printf("  energia : %s\n", ajustes_grove() ? "ligada" : "desligada");
  printf("  porta   : %s\n", (dono == GROVE_DONO_NINGUEM) ? "livre" : grove_dono_nome(dono));
  if (sistema_grove_sondando()) { printf("  sondagem em andamento...\n"); }
  printf("\ntrocou o modulo do cabo? use 'grove trocar'\n");
}

// ---------------------------------------------------------------------------
// AJUDA
// ---------------------------------------------------------------------------

typedef enum { G_SISTEMA, G_TELA, G_CARTAO, G_REDE, G_MESH, G_BANCADA, G_TESTES, G_APPS, G_N, G_OCULTO } grupo_t;

static const char* const GRUPO_TITULO[G_N] = {
  "SISTEMA     o que esta rodando e quanto custa",
  "TELA        painel, brilho e toque",
  "CARTAO      arquivos no SD",
  "REDE        WiFi, Bluetooth, ESP-NOW e o coprocessador C6",
  "MESHTASTIC  radio LoRa pelo Grove (C6L)",
  "BANCADA     hardware externo e camera",
  "TESTES      carga artificial para provar os servicos",
  "APPS        comandos dos aplicativos: app <app> <comando>",
};

typedef struct {
  const char* nome;
  const char* apelidos;   // separados por espaco; aceitos, mas fora da lista
  grupo_t     grupo;
  const char* resumo;     // a linha do 'help'
  const char* uso;        // uma forma por linha: "forma\to que faz"
  const char* detalhe;    // para que serve e cuidados - so no 'help <comando>'
  void (*fn)(int argc, char** argv);
  const char* exemplos;   // linhas prontas para copiar: "linha\to que faz"
} comando_t;

static void c_ajuda(int argc, char** argv);
static void imprimir_colunas(const char* texto, const char* prefixo);

// ---------------------------------------------------------------------------
// COMANDOS DOS APPS - 'app <app> <comando> ...' (app_comando_t, kernel/app.h)
// ---------------------------------------------------------------------------
// O nome do app na frente e obrigatorio: um app nunca sombreia um comando do
// sistema nem o de outro app, e o 'help' mostra de quem e cada um.

// "ESP-NOW", "esp-now", "espnow" sao o mesmo app.
static bool nome_de_app(const char* nome, const char* s)
{
  for (;;) {
    while (*nome == ' ' || *nome == '-') { ++nome; }
    while (*s == ' ' || *s == '-')       { ++s; }
    if (!*nome || !*s) { return !*nome && !*s; }
    if (tolower((unsigned char)*nome++) != tolower((unsigned char)*s++)) { return false; }
  }
}

static void ajuda_apps(const app_t* so_este)
{
  bool algum = false;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const app_t* a = SISTEMA_APPS[i];
    if (!a->comandos || (so_este && a != so_este)) { continue; }
    for (const app_comando_t* c = a->comandos; c->nome; ++c) {
      char pre[48];
      snprintf(pre, sizeof(pre), "app %s %s ", a->nome, c->nome);
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
    if (nome_de_app(SISTEMA_APPS[i]->nome, argv[1])) { a = SISTEMA_APPS[i]; }
  }
  if (!a) {
    if (argc >= 2) { printf("nao ha app '%s'\n", argv[1]); }
    printf("uso: app <app> <comando> [argumentos]\n");
    ajuda_apps(nullptr);
    return;
  }
  const app_comando_t* c = nullptr;
  for (const app_comando_t* k = a->comandos; argc >= 3 && k && k->nome; ++k) {
    if (eh(k->nome, argv[2])) { c = k; }
  }
  if (!c) {
    if (argc >= 3) { printf("o app %s nao tem o comando '%s'\n", a->nome, argv[2]); }
    ajuda_apps(a);
    return;
  }
  const char* atual = sistema_app_atual();
  if (c->so_aberto && (!atual || strcmp(atual, a->nome) != 0)) {
    printf("abra o app %s primeiro ('abrir %s')\n", a->nome, a->nome);
    return;
  }
  c->fn(argc - 2, argv + 2);
}

static const comando_t COMANDOS[] = {
  // --- SISTEMA ---
  { "estado", "servicos", G_SISTEMA,
    "panorama: versao, tempo ligado, RAM e uma linha por servico",
    "estado\tversao, app aberto, RAM, cartao, wifi, ble, mesh, energia",
    "A primeira pergunta quando algo esta estranho: o que esta de pe agora?\n"
    "Cada servico responde em uma linha; o detalhe de cada um esta no comando\n"
    "dele (wifi, mesh, energia...).",
    c_estado,
    "estado\tprimeiro passo antes de investigar qualquer problema" },
  { "tarefas", "tarefa", G_SISTEMA,
    "quem esta rodando: CPU%, prioridade e folga de pilha",
    "tarefas\tuma linha por tarefa do FreeRTOS, com o app dono",
    "Le a ultima foto do monitor (1 s) sem parar o sistema. CPU% e do aparelho:\n"
    "os dois nucleos somam 100%, entao um nucleo cheio aparece como 50%.\n"
    "Pilha livre abaixo de ~500 B e risco de estouro. Digitar no serial nao\n"
    "acorda a tela, entao da para medir o repouso de verdade.",
    c_tarefas,
    "tarefas\tquem esta gastando CPU agora" },
  { "mem", "memoria", G_SISTEMA,
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
    c_mem,
    "mem\tquanto de RAM sobra\n"
    "mem extrato\tqual servico consumiu RAM no boot" },
  { "energia", "", G_SISTEMA,
    "bateria, corrente, clock e os ultimos 30 min",
    "energia\ttensao, corrente, % e historico por minuto\n"
    "energia travas [seg]\tquem impede o light sleep (espera seg antes)\n"
    "energia repouso <mhz>\tforca o repouso agora nesse clock\n"
    "energia teto <mhz>\tlimita o CPU com a tela acesa (o maximo desfaz)",
    "Os valores vem do cache que a interface atualiza 1x/s - o console nunca\n"
    "le o INA226 direto. 'travas' so vale medido em repouso: use um tempo maior\n"
    "que o tempo de tela de Ajustes e nao toque na tela durante a espera.\n"
    "Clocks validos: " TAB5_CPU_DEGRAUS ".",
    c_energia,
    "energia\tquanto a bateria esta gastando\n"
    "energia travas 40\tespera a tela apagar e mostra quem impede o sono\n"
    "energia teto " TAB5_CPU_METADE "\tCPU no maximo a " TAB5_CPU_METADE " MHz" },
  { "fundo", "", G_SISTEMA,
    "partes de apps que seguem rodando com a janela fechada",
    "fundo\to que esta rodando e ha quanto tempo\n"
    "fundo parar <nome>\tpede para uma delas parar",
    "Parte de fundo e o pedaco de um app que continua com a janela fechada\n"
    "(ex.: o cronometro do Exemplo). A parada acontece na interface, no proximo ciclo.",
    c_fundo,
    "fundo parar Exemplo\tpara o cronometro do Exemplo" },
  { "log", "", G_SISTEMA,
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
  { "pendrive", "", G_SISTEMA,
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
  { "tela", "", G_TELA,
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
    c_tela,
    "tela medir\tzera; use o aparelho; depois rode tela\n"
    "tela relogio 80\tvolta ao relogio de fabrica do painel\n"
    "tela prioridade padrao\tdesliga a prioridade para comparar" },
  { "brilho", "", G_TELA,
    "testa o backlight sem gravar",
    "brilho\tvalor atual e o gravado\n"
    "brilho <1-255> [hz]\taplica (e muda a frequencia do PWM)",
    "Para achar o menor brilho que ainda acende. Nao grava: Ajustes ou\n"
    "reiniciar voltam ao valor salvo. Se a tela sumir, 'brilho 80' a traz de volta.",
    c_brilho,
    "brilho 6\to menor brilho ainda acende?\n"
    "brilho 80\ttraz a tela de volta se ela sumir" },
  { "piscada", "", G_TELA,
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
  { "toque", "", G_TELA,
    "mostra onde cada toque cai na tela",
    "toque\tliga: posicao crua e convertida de cada toque\n"
    "toque off\tdesliga",
    "Para conferir se um botao recebe o dedo onde deveria. Quem le o painel e\n"
    "a interface; o console so liga a bandeira.",
    c_toque,
    "toque\tdepois toque nos botoes e veja a posicao no log" },
  { "tocar", "", G_TELA,
    "clique simulado (bancada)",
    "tocar <x> <y>\tclica em x,y de tela, como o dedo",
    "Para testar uma tela pela serial. Acorda a tela se estiver em repouso\n"
    "(e esse primeiro pedido nao vira clique, como o dedo).",
    c_tocar,
    "tocar 360 640\tclica no meio da tela" },
  { "abrir", "", G_TELA,
    "abre um app pela serial (bancada)",
    "abrir\tlista os apps\n"
    "abrir <nome>\tabre (fecha o que estiver aberto)\n"
    "abrir home\tvolta para a tela inicial",
    "Junto de 'tocar' e 'tela foto', testa uma tela sem tocar no aparelho.",
    c_abrir,
    "abrir Arquivos\tdepois 'tela foto'" },
  { "ordem", "", G_TELA,
    "ordem dos icones na tela inicial",
    "ordem\tlista os apps na ordem da home\n"
    "ordem <nome> <posicao>\tmove o app para a posicao (1 = primeiro)\n"
    "ordem padrao\tvolta para a ordem de apps.cpp",
    "Gravada na NVS: vale depois de reiniciar, sem compilar. Na propria tela:\n"
    "segure um icone ate a moldura aparecer e arraste; para outra pagina,\n"
    "segure o icone na beirada lateral.",
    c_ordem,
    "ordem Exemplo 1\tExemplo vira o primeiro icone" },
  { "ui", "", G_TELA,
    "onde a interface esta agora (tela travada?)",
    "ui\tetapa atual, voltas do laco, ultimo toque, saude do GT911",
    "Funciona com a tela travada, porque o console e outra tarefa. Rode duas\n"
    "vezes: se 'voltas' nao mudou, o laco da interface parou nessa etapa.",
    c_ui,
    "ui\trode duas vezes com a tela travada e compare" },

  // --- CARTAO ---
  { "cartao", "", G_CARTAO,
    "espaco livre e o conteudo de cada pasta de app",
    "cartao\tespaco, gravador do sistema e ate 10 arquivos por pasta",
    "O panorama do SD. Para uma pasta especifica, use 'lista'.",
    [](int, char**) { cmd_cartao(); },
    "cartao\tquanto espaco sobra no SD" },
  { "lista", "", G_CARTAO,
    "lista uma pasta",
    "lista [caminho]\tpadrao /sdcard (ex.: lista apps/Sistema)",
    "Caminho relativo e lido a partir de /sdcard.",
    [](int c, char** v) { cmd_lista(c, v); },
    "lista apps/Mesh\tarquivos do app Mesh" },
  { "ler", "", G_CARTAO,
    "mostra um arquivo de texto",
    "ler <caminho> [linhas]\tate 200 linhas, ou quantas pedir",
    "Para olhar. Para copiar o arquivo inteiro para o Mac, use 'despejar'.\n"
    "Caminho relativo e lido a partir de /sdcard.",
    [](int c, char** v) { cmd_ler(c, v); },
    "ler apps/Sistema/logs/log_2026-10-01.txt 50\tas 50 primeiras linhas do log do dia" },
  { "despejar", "", G_CARTAO,
    "manda o arquivo inteiro, para salvar no Mac",
    "despejar <caminho>\tsem limite, entre marcadores >>>INICIO/>>>FIM",
    "Cala o log durante o envio para nenhuma linha cair no meio do arquivo.\n"
    "No Mac, com o monitor fechado: ./ferramentas/baixar.py <caminho>",
    [](int c, char** v) { cmd_despejar(c, v); },
    "despejar apps/Sistema/energia/energia-2026-10-01.csv\tcopia para o Mac (use ferramentas/baixar.py)" },
  { "apagar", "", G_CARTAO,
    "apaga um arquivo do cartao",
    "apagar <caminho>\tum arquivo por vez, sem curinga",
    "Irreversivel. So aceita caminhos dentro de /sdcard e sem '..'.",
    [](int c, char** v) { cmd_apagar(c, v); },
    "apagar apps/Console/console.bmp\tapaga a foto de teste" },
  { "receber", "", G_CARTAO,
    "recebe um arquivo pela serial e grava no cartao",
    "receber <app> <arquivo> <bytes>\tgrava em /sdcard/apps/<app>/<arquivo>",
    "Depois do comando, mande o arquivo cru pela mesma porta, com o monitor\n"
    "fechado. Tudo de uma vez, do Mac:\n"
    "  { printf 'receber Sistema x.bin 1234\\r'; sleep 2; cat x.bin; } > /dev/cu.usbmodem*\n"
    "Confira o md5 impresso no fim com 'md5 -q x.bin'. Ate 6 MB.",
    [](int c, char** v) { cmd_receber(c, v); },
    "receber Sistema tab5_c6.bin 1397904\tdepois mande o arquivo pela porta" },

  // --- REDE ---
  { "wifi", "", G_REDE,
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
  { "ble", "", G_REDE,
    "Bluetooth: estado, enlace do Mesh e aparelhos vistos",
    "ble\testado e aparelhos vistos\n"
    "ble varrer\tprocura por 6 s (rode 'ble' depois)\n"
    "ble ligar\tliga o Bluetooth (como o botao de Ajustes)\n"
    "ble desligar\tdesliga o Bluetooth (como o botao de Ajustes)",
    "Aparelhos Meshtastic aparecem marcados - sao os que o Mesh pode usar\n"
    "pelo Bluetooth.",
    c_ble,
    "ble varrer\tprocura; depois rode ble para ver a lista" },
  { "espnow", "", G_REDE,
    "ESP-NOW: estado, canal e envio de teste",
    "espnow\tfirmware do C6, canal real, MAC e contadores\n"
    "espnow canal <0-13>\t0 = segue o roteador; 1-13 = fixo\n"
    "espnow enviar <texto>\tdifunde uma mensagem",
    "Dois aparelhos so se ouvem no mesmo canal. O canal mostrado e lido do C6\n"
    "agora, nao o que o servico acha. Exige o firmware proprio do C6.",
    c_espnow,
    "espnow canal 1\tfixa no canal 1 (o mesmo do outro aparelho)\n"
    "espnow enviar ola Cardputer\tmensagem de teste" },
  { "c6", "", G_REDE,
    "coprocessador C6: firmware, MAC e gravacao pelo enlace",
    "c6\tversao do firmware e MACs\n"
    "c6 gravar\tcomo gravar firmware novo no C6 pelo enlace (sem abrir)\n"
    "c6 gravar sim\tgrava apps/Sistema/tab5_c6.bin no C6",
    "Exige o WiFi ligado. A gravacao confere a imagem antes, e o C6 volta\n"
    "sozinho para a anterior se a nova nao for confirmada.",
    c_c6,
    "c6\tqual firmware esta no C6" },
  { "farejo", "", G_REDE,
    "modo promiscuo no C6: quem esta no ar em volta",
    "farejo\testado e os MACs ouvidos (RSSI, tipo, quadros)\n"
    "farejo on [lote_ms] [mask_hex]\tliga; mask = canais a saltar (solte o roteador)\n"
    "farejo off\tdesliga\n"
    "farejo info\tcapacidades de radio do C6 gravado (CSI/FTM/802.15.4)\n"
    "farejo limpar\tzera a tabela",
    "O C6 junta por MAC e manda em lotes - o P4 so recebe o resumo. Deixa o\n"
    "receptor ligado enquanto ativo; desliga sozinho se a tela que usa fechar.\n"
    "Saltar varios canais exige o WiFi solto do roteador ('espnow canal 1').",
    c_farejo,
    "farejo info\teste C6 sabe CSI/FTM/802.15.4?\n"
    "farejo on\tcomeca a ouvir no canal atual\n"
    "farejo on 200 0x1452\tlote 200 ms, salta canais 1/6/11/13 (solte o roteador antes)" },

  { "ping", "", G_REDE,
    "o WiFi passa dados? a internet responde?",
    "ping\troteador e 8.8.8.8, 4 pacotes cada\n"
    "ping <ip|nome>\tum alvo so",
    "WiFi 'conectado' so diz que o C6 esta associado ao roteador. Se o roteador\n"
    "nao responde, o caminho de dados (C6/SDIO) parou; se so a internet nao\n"
    "responde, o problema esta depois do roteador.",
    c_ping,
    "ping\tos dois de uma vez\n"
    "ping mqtt.meshtastic.org\to servidor do MQTT do Mesh" },
  { "radio", "", G_REDE,
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
    "mesh casa -22.88 -42.79\tgrava a posicao de casa" },

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
    [](int, char**) { cmd_foto(); },
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

  // --- fora da lista: ajuda e atalhos antigos ---
  { "help",   "ajuda ?", G_OCULTO, "", "", "", c_ajuda },
  { "app",    "", G_OCULTO, "", "", "", c_app },
  { "redes",  "", G_OCULTO, "", "", "", c_redes },
  { "nos",    "", G_OCULTO, "", "", "", c_nos },
  { "enviar", "", G_OCULTO, "", "", "", c_enviar },
};
static const int N_COMANDOS = (int)(sizeof(COMANDOS) / sizeof(COMANDOS[0]));

static bool casa_nome(const comando_t& c, const char* s)
{
  if (eh(c.nome, s)) { return true; }
  // Apelidos: palavras separadas por espaco.
  const size_t n = strlen(s);
  for (const char* a = c.apelidos; *a; ) {
    while (*a == ' ') { ++a; }
    const char* fim = a;
    while (*fim && *fim != ' ') { ++fim; }
    if ((size_t)(fim - a) == n && n && strncasecmp(a, s, n) == 0) { return true; }
    a = fim;
  }
  return false;
}

static const comando_t* achar(const char* s)
{
  for (int i = 0; i < N_COMANDOS; ++i) { if (casa_nome(COMANDOS[i], s)) { return &COMANDOS[i]; } }
  return nullptr;
}

// Texto de varias linhas, cada uma com recuo.
static void imprimir_recuado(const char* t, const char* recuo)
{
  while (*t) {
    const char* fim = strchr(t, '\n');
    const int n = fim ? (int)(fim - t) : (int)strlen(t);
    printf("%s%.*s\n", recuo, n, t);
    t += n + (fim ? 1 : 0);
  }
}

// Linhas "esquerda\tdireita" em duas colunas. A esquerda que nao cabe na
// coluna fica sozinha, e a explicacao desce para a linha de baixo, alinhada -
// cortar um caminho de arquivo no meio estragaria o exemplo para copiar.
#define AJUDA_COLUNA 32
static void imprimir_colunas(const char* texto, const char* prefixo)
{
  const int larg = AJUDA_COLUNA - (int)strlen(prefixo);
  for (const char* t = texto; t && *t; ) {
    const char* fim = strchr(t, '\n');
    const int n = fim ? (int)(fim - t) : (int)strlen(t);
    const char* tab = (const char*)memchr(t, '\t', (size_t)n);
    if (!tab) {
      printf("  %s%.*s\n", prefixo, n, t);
    } else {
      const int a = (int)(tab - t);
      if (a <= larg) { printf("  %s%-*.*s %.*s\n", prefixo, larg, a, t, n - a - 1, tab + 1); }
      else           { printf("  %s%.*s\n  %*s %.*s\n", prefixo, a, t, AJUDA_COLUNA, "", n - a - 1, tab + 1); }
    }
    t += n + (fim ? 1 : 0);
  }
}

// As formas de chamar e, logo abaixo, exemplos prontos para copiar.
static void imprimir_uso(const comando_t& c)
{
  imprimir_colunas(c.uso, "");
  imprimir_colunas(c.exemplos, "ex: ");
}

static void mostrar_uso(const char* nome)
{
  const comando_t* c = achar(nome);
  if (!c) { return; }
  printf("uso:\n");
  imprimir_uso(*c);
}

static void ajuda_comando(const comando_t& c)
{
  printf("%s - %s\n\n", c.nome, c.resumo);
  imprimir_uso(c);
  printf("\n");
  imprimir_recuado(c.detalhe, "  ");
  if (c.apelidos[0]) { printf("  (tambem aceito como: %s)\n", c.apelidos); }
}

// Comando desconhecido: sugere os que comecam igual ou contem o que foi
// digitado. Errar uma letra nao pode virar "leia a lista inteira".
static void sugerir(const char* s)
{
  const size_t n = strlen(s);
  int achados = 0;
  for (int i = 0; i < N_COMANDOS; ++i) {
    const comando_t& c = COMANDOS[i];
    if (c.grupo == G_OCULTO) { continue; }
    const bool parecido = (n >= 2 && strncasecmp(c.nome, s, 2) == 0) ||
                          (n >= 3 && strcasestr(c.nome, s) != nullptr);
    if (parecido) { printf(achados++ ? ", %s" : "  talvez: %s", c.nome); }
  }
  if (achados) { printf("\n"); }
  printf("  'help' lista todos os comandos\n");
}

static void c_ajuda(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "app")) {
    const app_t* a = nullptr;
    for (int i = 0; argc >= 3 && i < SISTEMA_NUM_APPS; ++i) {
      if (nome_de_app(SISTEMA_APPS[i]->nome, argv[2])) { a = SISTEMA_APPS[i]; }
    }
    ajuda_apps(a);
    return;
  }
  if (argc >= 2 && !eh(argv[1], "tudo")) {
    const comando_t* c = achar(argv[1]);
    if (!c || c->grupo == G_OCULTO) {
      printf("nao ha comando '%s'\n", argv[1]);
      sugerir(argv[1]);
      return;
    }
    ajuda_comando(*c);
    return;
  }
  const bool tudo = (argc >= 2);

  // Agrupado por ASSUNTO, com TODAS as formas de cada comando a vista. Uma
  // linha so por comando escondia justamente o que se procura ("como mando
  // mensagem?" ficava atras de 'help mesh').
  printf("M5tabOS %s - console serial\n", esp_app_get_description()->version);
  for (int g = 0; g < G_N; ++g) {
    printf("\n%s\n", GRUPO_TITULO[g]);
    if (g == G_APPS) { ajuda_apps(nullptr); continue; }
    bool primeiro = true;
    for (int i = 0; i < N_COMANDOS; ++i) {
      const comando_t& c = COMANDOS[i];
      if (c.grupo != g) { continue; }
      if (!primeiro) { printf("\n"); }   // um respiro entre comandos do grupo
      primeiro = false;
      if (tudo) { ajuda_comando(c); }
      else      { imprimir_uso(c); }
      ceder(i);
    }
  }
  printf("\n'help <comando>' explica para que serve cada um; 'help tudo' explica todos.\n"
         "Caminhos do cartao podem ser relativos a /sdcard. Maiusculas tanto faz.\n");
}

// ---------------------------------------------------------------------------
// Leitor de linha proprio, SEM linenoise
// ---------------------------------------------------------------------------
//
// A primeira versao usava esp_console_new_repl_uart(), e nao dava para digitar:
// nada aparecia na tela. O motivo esta no proprio linenoise, em
// components/console/linenoise/linenoise.c:1191 - no modo "terminal burro" ele
// so imprime o prompt e NAO ecoa o que voce digita; o comentario la e explicito
// ("Only erase symbol echoed from stdin"), ou seja, ele conta com o eco local
// do terminal. O idf.py monitor nao faz eco local, encaminha as teclas cruas.
// Resultado: voce digita as cegas.
//
// Alem disso, cair ou nao no modo burro depende de uma sondagem do terminal que
// pode dar resultados diferentes em cada monitor. Preferi eliminar a variavel:
// este loop le byte a byte e ECOA ELE MESMO, entao funciona igual em qualquer
// terminal - idf.py monitor, screen, minicom, o que for.

static void executar(char* linha)
{
  // Quebra em palavras, respeitando aspas para senha com espaco.
  //
  // 24 E NAO 8: o "mesh enviar para <no> <texto>" ja gasta quatro posicoes, e
  // com 8 o texto era truncado em silencio na quinta palavra. 24 cobre a linha
  // inteira: com 192 caracteres de entrada e o limite de 160 do MESH_TEXTO, o
  // texto acaba antes das palavras. Custa 64 bytes de stack.
  char* argv[24];
  int argc = 0;
  char* p = linha;
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

  const comando_t* c = achar(argv[0]);
  if (!c) {
    printf("comando desconhecido: '%s'\n", argv[0]);
    sugerir(argv[0]);
    return;
  }
  c->fn(argc, argv);
}


static void vTaskConsole(void*)
{
  char linha[192];
  size_t n = 0;

  printf("\nconsole pronto - digite help\nm5tab> ");
  fflush(stdout);

  for (;;)
  {
    // BLOQUEIA DE VERDADE, e isso e economia de energia.
    //
    // Era assim:
    //
    //     int c = fgetc(stdin);
    //     if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    //
    // O fgetc sobre o VFS devolve EOF quando nao ha byte, entao a tarefa
    // acordava 50 VEZES POR SEGUNDO, para sempre, mesmo em repouso e mesmo com
    // ninguem conectado na serial. Cada volta e uma troca de contexto e uma
    // consulta ao driver para descobrir que nao ha nada.
    //
    // O usb_serial_jtag_read_bytes com portMAX_DELAY dorme no semaphore do
    // driver: zero acordadas enquanto nao chega tecla. E o mesmo caminho que o
    // comando `receber` usa, e ele passa ao largo da traducao de fim de linha
    // do VFS - que aqui nao faz falta, porque o loop abaixo ja trata CR e LF.
    uint8_t byte = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) { continue; }
#else
    if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) { continue; }
#endif
    const int c = (int)byte;

    // Diagnostico enquanto o console nao estiver confiavel: mostra o codigo de
    // TODO byte que chega. Serve para distinguir dois problemas muito
    // diferentes - "o monitor nao encaminha o teclado" (nada aparece aqui) de
    // "chega, mas o filtro descarta" (aparece o codigo e nada mais).
    ESP_LOGD(TAG, "rx: 0x%02x", (unsigned)c);

    if (c == '\r' || c == '\n')
    {
      printf("\n");
      linha[n] = 0;
      if (n > 0) { executar(linha); }
      n = 0;
      printf("m5tab> ");
      fflush(stdout);
    }
    else if (c == 8 || c == 127)          // backspace
    {
      if (n > 0) { --n; printf("\b \b"); fflush(stdout); }
    }
    else if (c >= 32 && c < 127 && n < sizeof(linha) - 1)
    {
      linha[n++] = (char)c;
      fputc(c, stdout);                   // O ECO QUE FALTAVA
      fflush(stdout);
    }
  }
}

void console_iniciar(void)
{
  // O driver TEM que ser o do mesmo periferico que o console primario, senao o
  // stdin fica preso num canal onde ninguem digita - foi exatamente o erro que
  // deixou o console mudo (ver o comentario no sdkconfig.defaults).
  // Por isso a escolha e feita pela propria chave do console, e nao fixada.
  //
  // Em ambos os casos: sem driver instalado a leitura de stdin NAO bloqueia (o
  // fgetc devolveria EOF para sempre e o loop viraria espera ocupada), e e o
  // driver que permite ler tecla a tecla em vez de linha inteira.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  // ANEL DE RX GRANDE, e isto NAO e folga preventiva - e correcao.
  //
  // O padrao do IDF e 256 bytes (USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT), e a
  // ISR do driver faz assim (usb_serial_jtag.c:150):
  //
  //     xRingbufferSendFromISR(rx_ring_buf, buf, rx_fifo_len, &xTaskWoken);
  //
  // O RETORNO E IGNORADO. Anel cheio = bytes descartados em silencio, sem erro,
  // sem log, sem nada. Para digitar comando ninguem percebe; mandando 1,4MB de
  // firmware pelo `receber`, sumiram 2432 bytes no meio e a transferencia
  // morreu a 99,8%.
  //
  // 8KB dao ~30ms de folga a velocidade de USB - tempo de sobra para o loop de
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
  // Enter chega como CR; sem a conversao a linha nunca terminaria. A saida vai
  // como CRLF para nao sair em escadinha.
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

  // stdin sem buffer. O newlib bufferiza por linha por padrao, e ai o fgetc
  // pode ficar esperando encher o buffer em vez de devolver a tecla na hora -
  // o eco so apareceria depois do Enter, ou nao apareceria. Todo exemplo de
  // console do IDF faz isto.
  setvbuf(stdin, NULL, _IONBF, 0);

  // 12KB, e nao 4096. O 4096 ESTOUROU - "enviar" derrubou o aparelho com
  // Stack protection fault, e o retrato de tarefas ja avisava: 372 B livres.
  //
  // Esta tarefa tem o pior caso do sistema inteiro, porque de um comando ela
  // alcanca QUALQUER subsistema. So no caminho do "enviar":
  //
  //   executar()          ~1,3KB de buffers de comando (sub, cam, completo...)
  //   tab5_mesh_enviar()   656B - data[176]+pkt[224]+frame[256], todos vivos
  //   guardar_msg()        -> FATFS, que e faminto de stack
  //   printf com float     -> vfprintf do newlib, mais de 1KB sozinho
  //
  // O "cartao" e o "c6 ota" descem igualmente fundo. Dimensionar pelo caso
  // tipico aqui e dimensionar para o dia em que nao se usa o comando dificil.
  //
  // MEDIDO: dois dias de pilhas-*.csv com uso normal do console, pico de
  // ~4,0KB (minimo livre 8320 de 12288). 8KB e o dobro do pico - folga para
  // os comandos fundos que nao aparecem todo dia - e devolve 4KB a RAM
  // interna. Se um comando novo descer mais, o pilhas-*.csv mostra.
  xTaskCreatePinnedToCore(vTaskConsole, "console", 8192, nullptr, 1, nullptr, tskNO_AFFINITY);
  ESP_LOGI(TAG, "console no ar - digite 'help' no monitor");
}
