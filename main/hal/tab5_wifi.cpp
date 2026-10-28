#include "tab5_wifi.hpp"
#include "tab5_owners.hpp"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "eh_host_feat_peer_data.h"
#include "tab5_c6.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "esp_mac.h"
#include <unistd.h>
#include "esp_random.h"

#include "tab5_power.hpp"
#include "tab5_sd.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_marco

static const char *TAG = "wifi";

// Tempo entre devolver energia ao C6 e falar com ele. O C6 tem que executar o
// proprio bootloader e subir o firmware esp_hosted antes de responder no SDIO.
// Falar cedo demais devolve o mesmo 0x108 (ESP_ERR_TIMEOUT) que a gente viu
// quando o trilho estava desligado - e o erro NAO diz "espere mais", diz so
// timeout, entao e um chute caro de diagnosticar. 600ms tem folga; se um dia
// aparecer falha intermitente ao ligar, este numero e o primeiro suspeito.
#define ESPERA_BOOT_C6_MS   600

// Tentativas de associacao antes de desistir e mostrar FALHOU. Sem limite, um
// SSID errado faria o C6 varrer o espectro para sempre - o pior consumo
// possivel, justamente no aparelho onde autonomia e o problema.
#define MAX_TENTATIVAS      5

enum comando_t { CMD_LIGAR, CMD_DESLIGAR, CMD_VARRER, CMD_SO_RADIO, CMD_ROTEADOR, CMD_ECONOMIA, CMD_PILHA, CMD_PROCURAR, CMD_TROCAR, CMD_RECONECTAR };

static QueueHandle_t s_fila     = nullptr;
static esp_netif_t*  s_netif    = nullptr;
static volatile bool s_pilha_de_pe = false;   // escrito pela tarefa do Wi-Fi, lido por outras
static volatile bool s_c6_nosso    = false;   // o C6 respondeu ao peer_data: firmware nosso (ESP-NOW disponivel)   // esp_hosted + esp_wifi ja iniciados neste boot

// ENLACE COM O C6, que NAO e a mesma coisa que "WiFi de pe".
//
// A diferenca custou caro. Com firmware quebrado no C6, o
// esp_hosted_connect_to_slave() passava - transporte SDIO negociado, versoes
// trocadas - e so o esp_wifi_init() morria, porque ele depende de RPC e o RPC
// do coprocessador estava travado. Como s_pilha_de_pe so vira true no FIM de
// subir_pilha(), ele ficava false, e os comandos de recuperacao do C6 (que
// precisam SO do enlace, nao do WiFi) se recusavam a rodar.
//
// Ou seja: o unico caminho para consertar o coprocessador estava trancado
// justamente pelo defeito que ele deveria consertar.
static bool          s_c6_enlace   = false;
static volatile tab5_wifi_estado_t s_estado = TAB5_WIFI_DESLIGADO;
static volatile int8_t s_rssi   = TAB5_WIFI_RSSI_DESCONHECIDO;
static volatile int s_tentativas = 0;   // tarefa do Wi-Fi e tarefa de eventos
// O USUARIO QUER WIFI. Liga em tab5_wifi_ligar/conectar e desliga SO em
// tab5_wifi_desligar (Ajustes, Radar) - a desistencia longe do roteador para o
// radio mas NAO mexe aqui. E o que separa "desligado de proposito" de "desistiu
// e deve voltar sozinho quando a rede aparecer" sem o HAL perguntar aos
// ajustes (camada de cima).
static volatile bool s_querer = false;

// RETOMADA: depois de desistir (FALHOU), procura de novo de tempos em tempos e
// conecta na conhecida que aparecer. Comeca em RETOMAR_MIN_MS e dobra ate
// RETOMAR_MAX_MS: voltar para casa reconecta em ~30 s, e longe de casa uma
// procura (~2 s de radio) a cada 5 min e o custo.
#define RETOMAR_MIN_MS   30000u
#define RETOMAR_MAX_MS  300000u
static uint32_t s_retomar_ms    = 0;   // proxima procura (0 = nao agendada)
static uint32_t s_retomar_passo = 0;
static volatile bool s_descendo = false;   // distingue queda de verdade de stop() nosso (lido pela tarefa de eventos)
// MODO SO RADIO: rádio ligado, SEM roteador - para o ESP-NOW usar
// um canal que nao e o do roteador (o radio so fica num canal por vez). Nao
// reconecta, e nao desiste-e-para-o-radio como faria fora do alcance do roteador.
static volatile bool s_so_radio = false;
// O RADIO ESTA INICIADO (esp_wifi_start) - com ou sem roteador. E o que o
// ESP-NOW precisa, e NAO e o mesmo que o enlace: desligar o WiFi em Ajustes (ou
// a desistencia fora do alcance do roteador) para o radio com o C6 alimentado e o enlace de
// pe. Sem esta distincao o app ESP-NOW mostrava "no ar" com o radio parado.
static volatile bool s_radio_ativo = false;
static void (* volatile s_ao_subir)(void) = nullptr;   // ver tab5_wifi_ao_subir

// ECONOMIA DO RADIO - quem manda e o SISTEMA, por pedidos.
// Modem sleep (WIFI_PS_MIN_MODEM) e o padrao: o C6 desliga o receptor entre
// os beacons do roteador. Quem precisa do receptor ligado o tempo todo (o
// ESP-NOW conversando - frame chegando no cochilo se perde) PEDE, com o seu
// bit; o radio fica acordado enquanto houver ao menos um pedido. Ate aqui o
// servico ESP-NOW chamava esp_wifi_set_ps(WIFI_PS_NONE) direto, uma vez, e o
// radio nunca mais economizava - com ou sem alguem usando o ESP-NOW.
static volatile uint32_t s_acordado = 0;   // pedidos de "receptor sempre ligado"
static int s_ps_aplicado = -1;             // o ultimo WIFI_PS_* mandado ao C6 (-1 = nenhum)

// So com o esp_wifi iniciado (subir_pilha, ou o comando com a stack de pe).
static void aplicar_economia(void)
{
  const int ps = s_acordado ? (int)WIFI_PS_NONE : (int)WIFI_PS_MIN_MODEM;
  if (ps == s_ps_aplicado) { return; }
  if (esp_wifi_set_ps((wifi_ps_type_t)ps) == ESP_OK) {
    s_ps_aplicado = ps;
    ESP_LOGI(TAG, "radio: %s", s_acordado ? "receptor sempre ligado (pedido)" : "modem sleep (economia)");
  }
}
static bool s_varrendo   = false;   // suprime a reconexao automatica durante a varredura

static char s_ssid[33] = { 0 };
static char s_senha[65] = { 0 };
static char s_ip[16]   = "0.0.0.0";

// ---------------------------------------------------------------------------
// Credenciais
// ---------------------------------------------------------------------------
// Arquivo texto no cartao em vez de compilado no binario: assim voce troca de
// rede sem recompilar, e o binario nao carrega senha nenhuma.
// Formato deliberadamente burro - primeira linha util e o SSID, segunda e a
// senha. Linhas vazias e comecadas por # sao ignoradas.
//
//     # rede do escritorio
//     MinhaRede
//     minhasenha123
//
// Rede aberta: deixe a segunda linha vazia... nao da, linha vazia e ignorada.
// Para rede aberta escreva a senha como uma linha so com "-" (tratado abaixo).
//
// SENHA CIFRADA NO ARQUIVO. O SSID nao e segredo; a senha nao deve ficar em
// texto puro no cartao. O que isto resolve e SO isto: quem tiver so o CARTAO
// (num leitor de SD, por exemplo) nao le a senha. NAO protege contra quem tem
// o APARELHO: a chave sai de dados do proprio chip, e a flash pode ser lida
// por esptool. Secure boot e flash encryption ficam de fora de proposito -
// queimam eFuse, sao irreversiveis e travam a regravacao por USB.
//
// CHAVE POR APARELHO: SHA-256 de um rotulo fixo + o MAC de fabrica (eFuse),
// primeiros 16 bytes. Cada Tab5 tem a sua; o codigo publico nao carrega chave
// nenhuma, e um cartao levado para outro Tab5 nao decifra la (a senha precisa
// ser escrita de novo em texto - o sistema cifra sozinho no proximo boot).
//
// AES-128-CBC com IV aleatorio por gravacao (nunca reaproveitado - CBC com o
// mesmo IV+chave duas vezes vaza padrao entre as duas cifras). O IV nao e
// segredo: vai NA FRENTE do texto cifrado, os dois em hex numa linha so. PKCS7
// preenche o ultimo bloco - o ultimo byte decifrado diz quantos bytes de
// preenchimento cortar.
static bool chave_do_aparelho(uint8_t chave[16])
{
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) != ESP_OK) { return false; }
  static const char ROTULO[] = "M5tabOS/wifi.txt/v1";
  uint8_t dig[32];
  mbedtls_sha256_context h;
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts(&h, 0);
  mbedtls_sha256_update(&h, (const uint8_t*)ROTULO, sizeof(ROTULO) - 1);
  mbedtls_sha256_update(&h, mac, sizeof(mac));
  mbedtls_sha256_finish(&h, dig);
  mbedtls_sha256_free(&h);
  memcpy(chave, dig, 16);
  return true;
}
#define CIFRA_BLOCO   16
#define CIFRA_MAX_BRUTO 80   // sizeof(s_senha)-1 (64) arredondado para cima ao proximo bloco de 16
#define CIFRA_MAX_HEX ((CIFRA_BLOCO + CIFRA_MAX_BRUTO) * 2 + 1)   // IV + cifrado, em hex

// true so quando 'texto' pode mesmo ser a saida de cifrar_senha() - usado para
// distinguir uma senha CIFRADA (escrita por este firmware a partir de
// ) de uma senha em TEXTO PURO deixada por uma versao anterior. Sem
// isto, o primeiro boot depois desta mudanca leria a senha antiga como se
// fosse hex cifrado, falharia ao decifrar e o WiFi pararia de funcionar sem
// aviso nenhum ate o usuario regravar o wifi.txt. Hex de verdade tem no minimo
// um bloco (IV=16B) + um bloco de cifrado (16B) = 32 bytes = 64 caracteres,
// sempre par, sempre em [0-9a-f] - uma senha de verdade raramente bate as tres
// condicoes ao mesmo tempo (maiuscula, simbolo ou tamanho impar ja bastam para
// nunca confundir).
static bool parece_cifrado(const char* texto)
{
  const size_t n = strlen(texto);
  if (n < CIFRA_BLOCO * 2 * 2 || (n % 2) != 0) { return false; }
  for (size_t i = 0; i < n; ++i) {
    const char c = texto[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
  }
  return true;
}

static bool cifrar_senha(const char* claro, char* saida_hex, size_t tam_saida)
{
  const size_t n = strlen(claro);
  if (n > CIFRA_MAX_BRUTO - CIFRA_BLOCO) { return false; }   // nunca deveria acontecer - s_senha tem 65

  const size_t pad   = CIFRA_BLOCO - (n % CIFRA_BLOCO);      // PKCS7: 1..16, nunca 0
  const size_t bruto = n + pad;

  uint8_t buf[CIFRA_MAX_BRUTO];
  memcpy(buf, claro, n);
  memset(buf + n, (uint8_t)pad, pad);

  uint8_t iv[CIFRA_BLOCO];
  esp_fill_random(iv, sizeof(iv));
  uint8_t iv_trabalho[CIFRA_BLOCO];
  memcpy(iv_trabalho, iv, sizeof(iv));   // mbedtls consome/atualiza o IV que recebe - guarda uma copia

  uint8_t chave[16];
  if (!chave_do_aparelho(chave)) { return false; }
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, chave, 128);
  uint8_t cifrado[CIFRA_MAX_BRUTO];
  const int r = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, bruto, iv_trabalho, buf, cifrado);
  mbedtls_aes_free(&ctx);
  if (r != 0 || tam_saida < (size_t)(sizeof(iv) + bruto) * 2 + 1) { return false; }

  // IV na frente, cifrado atras - tudo em hex, numa linha so.
  size_t p = 0;
  for (size_t i = 0; i < sizeof(iv); ++i) { p += (size_t)snprintf(saida_hex + p, tam_saida - p, "%02x", iv[i]); }
  for (size_t i = 0; i < bruto;       ++i) { p += (size_t)snprintf(saida_hex + p, tam_saida - p, "%02x", cifrado[i]); }
  return true;
}

static bool decifrar_senha(const char* hex, char* saida, size_t tam_saida)
{
  const size_t hl = strlen(hex);
  const size_t bruto_bytes = hl / 2;
  if (hl < CIFRA_BLOCO * 2 * 2 || (hl % 2) != 0
   || bruto_bytes > (size_t)(CIFRA_BLOCO + CIFRA_MAX_BRUTO)) { return false; }

  uint8_t todos[CIFRA_BLOCO + CIFRA_MAX_BRUTO];
  for (size_t i = 0; i < bruto_bytes; ++i) {
    unsigned int b;
    if (sscanf(hex + i * 2, "%2x", &b) != 1) { return false; }
    todos[i] = (uint8_t)b;
  }

  uint8_t iv[CIFRA_BLOCO];
  memcpy(iv, todos, sizeof(iv));
  const uint8_t* cifrado    = todos + CIFRA_BLOCO;
  const size_t   n_cifrado  = bruto_bytes - CIFRA_BLOCO;
  if (n_cifrado == 0 || (n_cifrado % CIFRA_BLOCO) != 0) { return false; }

  uint8_t claro[CIFRA_MAX_BRUTO];
  uint8_t chave[16];
  if (!chave_do_aparelho(chave)) { return false; }
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, chave, 128);
  const int r = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, n_cifrado, iv, cifrado, claro);
  mbedtls_aes_free(&ctx);
  if (r != 0) { return false; }

  const uint8_t pad = claro[n_cifrado - 1];
  if (pad == 0 || pad > CIFRA_BLOCO || pad > n_cifrado) { return false; }   // PKCS7 invalido
  const size_t texto_n = n_cifrado - pad;
  if (texto_n >= tam_saida) { return false; }
  memcpy(saida, claro, texto_n);
  saida[texto_n] = 0;
  return true;
}
// ---------------------------------------------------------------------------
// REDES CONHECIDAS - varias, no mesmo wifi.txt
// ---------------------------------------------------------------------------
// O arquivo guarda PARES de linhas (SSID, senha), um par por rede. O arquivo
// antigo, de uma rede so, e so o caso de um par. A senha fica cifrada (ver
// acima); em texto puro ela e aceita e cifrada no proximo boot. Ao ligar, o
// sistema procura as redes em volta e conecta na conhecida de sinal mais forte.
static tab5_wifi_rede_t s_redes[TAB5_WIFI_MAX_REDES];   // .senha e texto puro: so RAM
static char             s_senhas[TAB5_WIFI_MAX_REDES][65];
static int              s_n_redes = 0;
static bool             s_lido = false;              // o cartao ja foi lido neste boot
// O ARQUIVO foi lido de verdade (ou, com o cartao presente, nao existia).
// So assim ele pode ser REESCRITO: reescrever a partir de uma lista que nao
// veio dele apagaria as outras redes guardadas (cartao ausente, ou emprestado
// ao computador no modo pendrive).
static bool             s_arquivo_lido = false;

// Redes vistas na ultima procura (para a tela de Ajustes).
static tab5_wifi_visto_t s_vistas[TAB5_WIFI_MAX_VISTAS];
static volatile int      s_n_vistas = 0;
static volatile bool     s_procurando = false;
static volatile uint32_t s_redes_ver = 1;
static char              s_alvo[33];                 // CMD_TROCAR: para qual rede

// A lista e mexida pela tarefa do Wi-Fi (ler ao ligar) e por quem grava rede
// nova (worker de Ajustes, console). Recursiva: gravar le antes.
static SemaphoreHandle_t s_trava_redes = nullptr;
struct trava_redes_t {
  trava_redes_t()  { if (s_trava_redes) { xSemaphoreTakeRecursive(s_trava_redes, portMAX_DELAY); } }
  ~trava_redes_t() { if (s_trava_redes) { xSemaphoreGiveRecursive(s_trava_redes); } }
};

static int achar_rede(const char* ssid)
{
  for (int i = 0; i < s_n_redes; ++i) { if (strcmp(s_redes[i].ssid, ssid) == 0) { return i; } }
  return -1;
}

// A rede em uso passa a ser a conhecida 'i'.
static void usar_rede(int i)
{
  snprintf(s_ssid, sizeof(s_ssid), "%s", s_redes[i].ssid);
  snprintf(s_senha, sizeof(s_senha), "%s", s_senhas[i]);
}

// GRAVA O ARQUIVO com todas as redes, senhas JA CIFRADAS (ou "-", rede
// aberta), pela porta do sistema (tab5_sd_substituir): nunca deixa o aparelho
// com meio arquivo, zera o antigo antes de apaga-lo (SIGILOSO - no FAT apagar
// so marca os setores como livres, e uma senha antiga continuaria legivel por
// recuperacao) e, com o cartao emprestado ao computador, vai para o diario.
static bool escrever_arquivo(void)
{
  if (!s_arquivo_lido) {
    ESP_LOGW(TAG, "%s nao foi lido (sem cartao?) - a mudanca vale so ate reiniciar", TAB5_WIFI_ARQUIVO);
    return false;
  }
  static const char CAB[] = "# redes Wi-Fi: SSID e senha, um par de linhas por rede.\n"
                            "# senhas cifradas (AES-128-CBC, chave do aparelho) - para trocar,\n"
                            "# escreva a senha nova em texto: ela e cifrada no proximo boot.\n";
  const size_t cap = sizeof(CAB) + (size_t)TAB5_WIFI_MAX_REDES * (sizeof(s_redes[0].ssid) + CIFRA_MAX_HEX + 2);
  char* txt = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!txt) { return false; }
  size_t k = (size_t)snprintf(txt, cap, "%s", CAB);
  bool ok = true;
  for (int i = 0; ok && i < s_n_redes && k < cap; ++i) {
    char linha[CIFRA_MAX_HEX];
    if (s_senhas[i][0] == 0) { snprintf(linha, sizeof(linha), "-"); }
    else if (!cifrar_senha(s_senhas[i], linha, sizeof(linha))) { ok = false; break; }
    k += (size_t)snprintf(txt + k, cap - k, "%s\n%s\n", s_redes[i].ssid, linha);
  }
  ok = ok && k < cap && tab5_sd_substituir("Sistema", "wifi.txt", txt, k, TAB5_SD_SIGILOSO);
  heap_caps_free(txt);
  if (!ok) { ESP_LOGE(TAG, "nao consegui gravar %s", TAB5_WIFI_ARQUIVO); }
  return ok;
}

static bool ler_credenciais(void)
{
  // JA LIDO NESTE BOOT? Entao NAO va ao cartao.
  //
  // Nao e otimizacao, e correcao. O controlador SDMMC do P4 e UM SO e
  // multiplexa entre os dois slots: o cartao no 0, o ESP32-C6 do WiFi no 1.
  // Montar ou desmontar o cartao mexe em estado shared - com o link do
  // C6 de pe, isso ja derrubou o WiFi e reiniciou a placa. Por isso o cartao
  // e lido UMA vez por boot, antes de existir qualquer link com o C6; quem
  // grava rede nova (Ajustes, console) atualiza a RAM junto.
  trava_redes_t trava;
  if (s_lido) { return s_n_redes > 0; }

  if (!tab5_sd_montar()) {
    ESP_LOGE(TAG, "sem cartao: nao da para ler %s", TAB5_WIFI_ARQUIVO);
    return false;
  }
  // Pela leitura do sistema: le a ultima versao (o diario de um pendrive entra
  // antes) e completa uma troca interrompida por queda de energia.
  FILE* f = tab5_sd_abrir("Sistema", "wifi.txt", "r");
  if (!f) {
    ESP_LOGE(TAG, "arquivo %s nao existe", TAB5_WIFI_ARQUIVO);
    tab5_sd_desmontar();
    s_lido = true;   // sem arquivo: rede nova entra pelo Ajustes (e cria o arquivo)
    s_arquivo_lido = true;
    return false;
  }

  char linha[CIFRA_MAX_HEX + 4];
  char ssid[33] = "";
  bool tem_ssid = false, tinha_texto_puro = false, alguma_ilegivel = false;
  s_n_redes = 0;
  while (fgets(linha, sizeof(linha), f) && s_n_redes < TAB5_WIFI_MAX_REDES) {
    // tira \r e \n do fim - arquivo salvo no Windows traz \r e ele entraria
    // dentro do SSID, que entao nunca casa com a rede real.
    size_t n = strlen(linha);
    while (n > 0 && (linha[n-1] == '\n' || linha[n-1] == '\r')) { linha[--n] = 0; }
    if (n == 0 || linha[0] == '#') { continue; }
    if (!tem_ssid) { snprintf(ssid, sizeof(ssid), "%s", linha); tem_ssid = true; continue; }
    tem_ssid = false;

    char senha[65] = "";
    if (strcmp(linha, "-") == 0) {
      // rede aberta
    } else if (parece_cifrado(linha)) {
      if (!decifrar_senha(linha, senha, sizeof(senha))) {
        // Cifrada por OUTRO aparelho (a chave e de cada Tab5) ou arquivo
        // corrompido: a rede fica de fora, e o log diz como resolver.
        ESP_LOGE(TAG, "%s: a senha de \"%s\" nao e deste aparelho - escreva-a de novo em "
                      "texto no arquivo, ou escolha a rede em Ajustes > Wi-Fi", TAB5_WIFI_ARQUIVO, ssid);
        alguma_ilegivel = true;
        continue;
      }
    } else {
      snprintf(senha, sizeof(senha), "%s", linha);   // texto puro: cifra logo abaixo
      tinha_texto_puro = true;
    }
    if (achar_rede(ssid) >= 0) { continue; }   // repetida no arquivo: vale a primeira
    snprintf(s_redes[s_n_redes].ssid, sizeof(s_redes[0].ssid), "%s", ssid);
    s_redes[s_n_redes].aberta = (senha[0] == 0);
    snprintf(s_senhas[s_n_redes], sizeof(s_senhas[0]), "%s", senha);
    ++s_n_redes;
  }
  fclose(f);
  s_arquivo_lido = true;

  // MIGRACAO AUTOMATICA: senha em texto puro vira cifrada aqui mesmo. E
  // SEGURO mexer no cartao neste ponto pelo mesmo motivo de toda esta funcao:
  // ela roda uma vez por boot, ANTES do enlace com o C6.
  if (tinha_texto_puro) {
    if (escrever_arquivo()) { ESP_LOGW(TAG, "%s: senha em texto puro - agora esta cifrada", TAB5_WIFI_ARQUIVO); }
    else                    { ESP_LOGE(TAG, "%s: senha em texto puro e NAO consegui cifrar o arquivo", TAB5_WIFI_ARQUIVO); }
  }
  (void)alguma_ilegivel;
  tab5_sd_desmontar();
  s_lido = true;

  if (s_n_redes > 0) { usar_rede(0); }
  ESP_LOGI(TAG, "%d rede(s) conhecida(s)", s_n_redes);
  return s_n_redes > 0;
}

// Procura as redes em volta (o radio precisa estar iniciado). Guarda em
// s_vistas, uma entrada por SSID (a de sinal mais forte), ordenadas por sinal.
static bool procurar_redes(void)
{
  wifi_scan_config_t sc = { };
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) { return false; }
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  wifi_ap_record_t* aps = n ? (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t)) : nullptr;
  if (n && !aps) { esp_wifi_clear_ap_list(); return false; }
  if (n) { esp_wifi_scan_get_ap_records(&n, aps); }
  int k = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const char* nome = (const char*)aps[i].ssid;
    if (!nome[0]) { continue; }
    int j = 0;
    while (j < k && strcmp(s_vistas[j].ssid, nome) != 0) { ++j; }
    if (j < k) { if (aps[i].rssi > s_vistas[j].rssi) { s_vistas[j].rssi = aps[i].rssi; } continue; }
    if (k >= TAB5_WIFI_MAX_VISTAS) { continue; }
    snprintf(s_vistas[k].ssid, sizeof(s_vistas[k].ssid), "%s", nome);
    s_vistas[k].rssi = aps[i].rssi;
    s_vistas[k].canal = aps[i].primary;
    s_vistas[k].aberta = (aps[i].authmode == WIFI_AUTH_OPEN);
    ++k;
  }
  free(aps);
  for (int i = 1; i < k; ++i) {   // mais forte primeiro
    tab5_wifi_visto_t t = s_vistas[i];
    int j = i - 1;
    while (j >= 0 && s_vistas[j].rssi < t.rssi) { s_vistas[j + 1] = s_vistas[j]; --j; }
    s_vistas[j + 1] = t;
  }
  for (int i = 0; i < k; ++i) { s_vistas[i].conhecida = achar_rede(s_vistas[i].ssid) >= 0; }
  s_n_vistas = k;
  s_redes_ver = s_redes_ver + 1;
  return true;
}

// A conhecida de sinal mais forte entre as vistas; -1 se nenhuma aparece.
static int melhor_conhecida(void)
{
  for (int i = 0; i < s_n_vistas; ++i) {   // s_vistas ja vem do mais forte
    const int r = achar_rede(s_vistas[i].ssid);
    if (r >= 0) { return r; }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Eventos
// ---------------------------------------------------------------------------
static void ao_evento(void* arg, esp_event_base_t base, int32_t id, void* dados)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
  {
    // Durante a varredura o start NAO pode disparar conexao: o esp_wifi nao faz
    // as duas coisas ao mesmo tempo, e a associacao abortaria o scan.
    if (s_varrendo) { return; }
    // Radio de pe, sem roteador (ver s_so_radio). O estado tem que sair do
    // SUBINDO: ligar o WiFi em Ajustes com o canal fixo ativo parava aqui, e a
    // barra dizia "subindo" para sempre.
    if (s_so_radio) { s_estado = TAB5_WIFI_DESLIGADO; return; }
    s_estado = TAB5_WIFI_CONECTANDO;
    esp_wifi_connect();
  }
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
  {
    s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
    s_ip[0] = 0;
    strcpy(s_ip, "0.0.0.0");

    // Queda provocada pelo nosso proprio esp_wifi_stop(): nao e falha, e nao
    // se reconecta. Sem esta guarda, desligar o WiFi dispararia uma rajada de
    // tentativas de reconexao - exatamente o oposto do pedido.
    //
    // E NAO APAGA O MOTIVO. Este evento chega DEPOIS de quem o provocou: na
    // desistencia, o FALHOU ja esta posto e o CMD_DESLIGAR para o radio - e a
    // queda que esse stop gera chegava aqui e trocava FALHOU por DESLIGADO. A
    // retomada (que so olhava FALHOU) nunca mais procurava: voltar para casa
    // nao reconectava, e so a procura de Ajustes > Wi-Fi destravava.
    if (s_descendo) {
      if (s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO) { s_estado = TAB5_WIFI_DESLIGADO; }
      return;
    }
    if (s_so_radio) { s_estado = TAB5_WIFI_DESLIGADO; return; }   // saida pedida: sem reconexao

    s_tentativas = s_tentativas + 1;   // volatile: sem ++ (o C++20 o deprecia)
    if (s_tentativas > MAX_TENTATIVAS)
    {
      ESP_LOGW(TAG, "desisti depois de %d tentativas - parando o radio",
               MAX_TENTATIVAS);
      s_estado = TAB5_WIFI_FALHOU;
      // Desistir sem parar o radio era o pior dos mundos: o C6 ficava ligado e
      // associado a nada, gastando bateria em silencio ate o proximo boot.
      // Enfileira em vez de chamar esp_wifi_stop() aqui: estamos DENTRO do
      // tratador de evento do WiFi, e parar a stack de dentro dela mesma e
      // pedido de travamento.
      comando_t c = CMD_DESLIGAR;
      xQueueSend(s_fila, &c, 0);
      return;
    }
    ESP_LOGI(TAG, "caiu, tentativa %d/%d", s_tentativas, MAX_TENTATIVAS);
    s_estado = TAB5_WIFI_CONECTANDO;
    // NAO DORME AQUI. Isto roda na tarefa de eventos PADRAO do ESP-IDF - a
    // mesma de todos os eventos do sistema (IP, Wi-Fi, esp_hosted). O
    // vTaskDelay(1000) que morava aqui congelava todos eles por 1 s a cada
    // tentativa. A espera e a reconexao vao para a tarefa do proprio Wi-Fi.
    comando_t c = CMD_RECONECTAR;
    if (xQueueSend(s_fila, &c, 0) != pdTRUE) { esp_wifi_connect(); }   // fila cheia: na hora, sem a pausa
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t* e = (ip_event_got_ip_t*) dados;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_tentativas = 0;
    s_estado = TAB5_WIFI_CONECTADO;
    ESP_LOGI(TAG, "conectado, IP %s", s_ip);
    tab5_mem_marco("WiFi: IP recebido");
  }
}

// ---------------------------------------------------------------------------
// Subida e descida (tarefa do servico)
// ---------------------------------------------------------------------------
static bool subir_pilha(void)
{
  // O trilho ja foi ligado pela tarefa de interface antes de enfileirar o
  // comando; aqui so esperamos o C6 terminar de bootar.
  vTaskDelay(pdMS_TO_TICKS(ESPERA_BOOT_C6_MS));

  // Nas duas falhas abaixo, o log CRU do SDIO esta silenciado (ver
  // tab5_wifi_iniciar). Entao a mensagem aqui tem que ser autossuficiente:
  // dizer o que aconteceu, o que provavelmente causou, e como ver o cru.
  tab5_mem_marco("C6: antes do esp_hosted");
  int rc = esp_hosted_init();
  tab5_mem_marco("esp_hosted_init (SDIO)");
  if (rc != 0) {
    ESP_LOGE(TAG, "esp_hosted_init falhou: %d - o C6 nao respondeu no SDIO. "
                  "Para ver o log cru: 'log eh_sdio 4' e 'log sdmmc_io 4'", rc);
    return false;
  }

  rc = esp_hosted_connect_to_slave();
  if (rc == 0) {
    // Marcado AQUI, e nao no fim: daqui para baixo tudo depende de RPC, e e
    // exatamente o que pode faltar num C6 com firmware ruim. O enlace ja
    // existe, e e ele que a gravacao de firmware usa.
    s_c6_enlace = true;
    tab5_mem_marco("C6: enlace + RPC");
  }
  if (rc != 0) {
    ESP_LOGE(TAG, "conexao com o C6 falhou: %d.", rc);
    ESP_LOGE(TAG, "Se voce acabou de gravar firmware no C6, ELE SE RECUPERA "
                  "SOZINHO: sem confirmacao em %d s ele volta para a imagem "
                  "anterior. Deixe LIGADO ate la - cada reinicio rearma a "
                  "contagem do zero.", C6_PRAZO_HOST_S);
    return false;
  }

  // ESTA CHAMADA VIROU PORTAO, e nao mais uma curiosidade de diagnostico.
  //
  // Ela e o RPC mais barato que existe - so devolve tres numeros. Se ELE nao
  // volta, nenhum outro vai voltar, e insistir custa caro: cada RPC pendente
  // deixa a tarefa sdio_write do esp_hosted girando, e ela chega a estourar o
  // watchdog de tarefa do P4 (log, "CPU 0: sdio_write").
  //
  // Antes daqui a gente seguia em frente e batia noutro tempo esgotado no
  // esp_wifi_init - dez segundos de tempestade em vez de cinco, e ainda criava
  // um netif que so ia atrapalhar a proxima tentativa.
  //
  // Parar aqui e mais rapido, mais silencioso, e diz a verdade na tela.
  esp_hosted_coprocessor_fwver_t ver = { };
  if (esp_hosted_get_coprocessor_fwversion(&ver) != 0)
  {
    ESP_LOGE(TAG, "o C6 aceitou o enlace mas NAO RESPONDE A RPC.");
    ESP_LOGE(TAG, "O firmware dele precisa ser regravado pelo conector "
                  "reservado da placa (docs.m5stack.com, restore_factory). "
                  "O resto do sistema continua funcionando - so o WiFi cai.");
    s_estado = TAB5_WIFI_C6_MUDO;
    return false;
  }
  ESP_LOGI(TAG, "C6 no ar, firmware %d.%d.%d",
           (int)ver.major1, (int)ver.minor1, (int)ver.patch1);

  // esp_netif_init() e o loop de eventos podem ja existir (outro servico).
  // ESP_ERR_INVALID_STATE aqui significa "ja estava pronto", nao erro.
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { return false; }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { return false; }

  // REAPROVEITA O NETIF SE ELE JA EXISTE. Isto e correcao de uma TELA AZUL.
  //
  // subir_pilha() nao era idempotente, e ela PODE rodar mais de uma vez: quando
  // falha, s_pilha_de_pe continua false e a proxima tentativa de ligar o WiFi
  // entra aqui de novo. So que o netif criado na passada anterior continuou
  // vivo - o caminho de erro nao desfazia nada.
  //
  // O esp_netif_create_default_wifi_sta() nao devolve erro nesse caso: ele
  // ASSERTA e derruba o aparelho.
  //
  //   E esp_netif_lwip: Failed to configure netif (duplicate key)
  //   assert failed: esp_netif_create_default_wifi_sta wifi_default.c:422
  //
  // Aconteceu com o C6 mudo: a primeira subida criou o netif e
  // morreu no esp_wifi_init; religar o WiFi por Ajustes derrubou o P4 inteiro.
  // Um coprocessador que nao responde NAO PODE derrubar o host.
  //
  // Mesma solucao que o proprio esp_hosted usa do lado do coprocessador
  // (eh_cp_idem_wraps.c): procurar antes de criar.
  if (!s_netif) { s_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }
  if (!s_netif) { s_netif = esp_netif_create_default_wifi_sta(); }

  // ESP_ERR_INVALID_STATE aqui e "ja iniciado", nao erro - mesma logica do
  // netif acima, pelo mesmo motivo: esta funcao pode rodar duas vezes.
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  // CSI LIGADO NO INIT, e quem decide e ESTE lado, nao o C6.
  //
  // O esp_hosted do C6 (req_wifi_init + get_merged_init_config) chama o
  // esp_wifi_init de la com os campos que chegam daqui, e o csi_enable e um
  // deles ("always use value from host"). O driver do C6 guarda esse valor
  // (wifi_menuconfig_init) e o esp_wifi_set_csi* confere: zerado, loga
  // "CSI not enabled in menuconfig!" e devolve ESP_FAIL. O C6 compilado com
  // CONFIG_ESP_WIFI_CSI_ENABLED nao basta: o default dele nunca e usado.
  //
  // Por codigo e nao por CONFIG_WIFI_RMT_CSI_ENABLED: o CSI e do firmware do
  // C6, o P4 nao chama API de CSI nenhuma. Custo: o C6 reserva perto de 1 KB
  // por buffer RX estatico (16 -> ~16 KB da RAM dele), pago mesmo com o CSI
  // parado; ligar so quando pedido exigiria reiniciar o WiFi.
  cfg.csi_enable = 1;
  const esp_err_t erri = esp_wifi_init(&cfg);
  if (erri != ESP_OK && erri != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_wifi_init: %s - o C6 respondeu ao enlace mas nao ao RPC. "
                  "Se voce acabou de gravar firmware nele, DEIXE PARADO: sem "
                  "resetar o C6, ele volta sozinho para a imagem anterior em "
                  "%d s. Reiniciar o P4 RESETA o C6 e rearma essa contagem.",
             esp_err_to_name(erri), C6_PRAZO_HOST_S);
    return false;
  }

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ao_evento, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ao_evento, nullptr, nullptr);

  // RAM e nao NVS: quem guarda a preferencia aqui e o modulo ajustes, e o
  // arquivo no cartao e a fonte da verdade das credenciais. Deixar o esp_wifi
  // gravar copia propria na NVS criaria uma segunda verdade, que um dia
  // discorda da primeira.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  tab5_mem_marco("esp_wifi_init + netif");

  // Modem sleep: o C6 desliga o radio entre beacons. E economia grande e sem
  // contrapartida para o nosso uso (nao somos servidor, ninguem nos procura) -
  // A MENOS que alguem tenha pedido o receptor ligado (ver ECONOMIA DO RADIO).
  s_ps_aplicado = -1;   // stack nova: o C6 nao sabe de nada ainda
  aplicar_economia();

  // CONFIRMA O ENLACE COM O C6 - e o que cancela a reversao automatica.
  //
  // Se o C6 estiver rodando firmware NOSSO recem-gravado, ele subiu com uma
  // carencia armada: sem este aviso dentro do prazo, ele mesmo aponta o boot de
  // volta para a imagem anterior e reinicia. Este send e a prova de que o enlace
  // SDIO fechou e o host esta falando.
  //
  // POR ULTIMO, e por causa do relogio. O RPC sincrono do esp_hosted espera ate
  // 5 s por resposta (DEFAULT_RPC_RSP_TIMEOUT), e o firmware de fabrica 1.4.1
  // nao conhece esta mensagem - entao com ele a chamada gasta os 5 s e falha.
  // No comeco de subir_pilha() isso atrasaria o radio inteiro em 5 s a cada
  // boot; aqui no fim, o WiFi ja esta configurado e a espera nao segura nada.
  // Com o nosso firmware no C6 a resposta e imediata e o custo some.
  eh_host_feat_peer_data_init();
  if (eh_host_peer_data_send(C6_MSG_HOST_VIVO, nullptr, 0) == ESP_OK) {
    s_c6_nosso = true;
    ESP_LOGI(TAG, "C6 com o firmware do M5tabOS (ESP-NOW disponivel)");
  } else {
    ESP_LOGI(TAG, "C6 com o firmware de fabrica (WiFi normal; sem ESP-NOW)");
  }

  return true;
}

// Manda s_ssid/s_senha ao C6 (a rede escolhida por usar_rede).
static void aplicar_config(void)
{
  wifi_config_t wc = { };
  strncpy((char*)wc.sta.ssid,     s_ssid,  sizeof(wc.sta.ssid)     - 1);
  strncpy((char*)wc.sta.password, s_senha, sizeof(wc.sta.password) - 1);
  esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void tratar_ligar(void)
{
  if (s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO) { return; }

  s_estado = TAB5_WIFI_SUBINDO;

  // Credenciais PRIMEIRO, antes de subir a stack. Duas razoes, e a segunda foi
  // aprendida na marra:
  //
  // 1. Se nao ha wifi.txt, nem vale acordar o C6 - poupa energia e o usuario ve
  //    o motivo na hora, em vez de esperar a negociacao inteira para falhar.
  // 2. Ler as credenciais MONTA O CARTAO, e montar o cartao mexe no SDMMC. Com
  //    o cartao no slot 0 e o C6 no slot 1 isso deixou de derrubar o link
  //    (ver o comentario grande em tab5_sd.cpp), mas fazer o acesso ao cartao
  //    ANTES de o link existir remove a sobreposicao por completo. Defesa em
  //    profundidade: se um dia alguem mexer nos slots, aqui ainda nao quebra.
  if (!ler_credenciais()) { s_estado = TAB5_WIFI_SEM_CREDENCIAL; return; }

  if (!s_pilha_de_pe)
  {
    if (!subir_pilha()) {
      // Preserva C6_MUDO: ele diz O QUE fazer (regravar o coprocessador), e
      // "falhou" mandaria o usuario conferir a senha para sempre.
      if (s_estado != TAB5_WIFI_C6_MUDO) { s_estado = TAB5_WIFI_FALHOU; }
      return;
    }
    s_pilha_de_pe = true;
    if (s_ao_subir) { s_ao_subir(); }   // quem espera o enlace acorda agora
  }

  s_tentativas = 0;
  s_descendo   = false;

  // A REDE MAIS FORTE ENTRE AS CONHECIDAS. Com mais de uma rede guardada,
  // procura antes de conectar: o radio sobe SEM conectar (s_varrendo segura o
  // STA_START), procura, e so entao escolhe. Com uma so, ou se a procura
  // falhar, vai direto na primeira (pode ser uma rede oculta).
  s_varrendo = true;
  esp_err_t err = esp_wifi_start();
  s_radio_ativo = (err == ESP_OK);
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    s_varrendo = false;
    ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    s_estado = TAB5_WIFI_FALHOU;
    return;
  }
  if (procurar_redes()) {
    const int r = melhor_conhecida();
    if (r >= 0) { usar_rede(r); ESP_LOGI(TAG, "rede conhecida mais forte: \"%s\"", s_ssid); }
    else        { ESP_LOGW(TAG, "nenhuma rede conhecida a vista - tentando \"%s\"", s_ssid); }
  }
  s_varrendo = false;

  aplicar_config();
  if (s_so_radio) { s_estado = TAB5_WIFI_DESLIGADO; return; }   // ESP-NOW em canal fixo: sem roteador
  s_estado = TAB5_WIFI_CONECTANDO;
  esp_wifi_connect();
}

static bool ligado_ou_ligando(void)
{
  return s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO || s_estado == TAB5_WIFI_SUBINDO;
}

// A PROCURA ACHOU UMA CONHECIDA E O USUARIO QUER WIFI, mas nao ha conexao (a
// desistencia parou o radio, ou ele estava fora do alcance): conecta nela. O
// radio precisa estar iniciado. true = conectando.
static bool conectar_conhecida_vista(void)
{
  if (!s_querer || s_so_radio || ligado_ou_ligando()) { return false; }
  const int r = melhor_conhecida();
  if (r < 0) { return false; }
  usar_rede(r);
  aplicar_config();
  s_tentativas = 0;
  s_descendo = false;
  s_radio_ativo = true;
  s_estado = TAB5_WIFI_CONECTANDO;
  ESP_LOGI(TAG, "rede conhecida a vista: conectando em \"%s\"", s_ssid);
  esp_wifi_connect();
  return true;
}

// PROCURA (pedida pela tela, ou a retomada). Conectado, procura sem soltar a
// rede. Sem conexao: se aparece uma conhecida e o WiFi esta ligado em
// Ajustes, conecta nela; senao, o radio que foi ligado so para procurar e
// desligado de novo.
static void tratar_procurar(void)
{
  s_procurando = true;
  s_redes_ver = s_redes_ver + 1;
  if (s_pilha_de_pe) {
    const bool estava = s_radio_ativo;
    if (!estava) { s_varrendo = true; esp_wifi_start(); }
    if (!procurar_redes()) { ESP_LOGW(TAG, "procura de redes recusada (conectando?)"); }
    s_varrendo = false;
    const bool conectou = conectar_conhecida_vista();   // conectando: o radio fica
    if (!estava && !conectou) {
      if (s_so_radio || ligado_ou_ligando()) { s_radio_ativo = true; }
      else                                   { esp_wifi_stop(); }
    }
  }
  s_procurando = false;
  s_redes_ver = s_redes_ver + 1;
}

// Chamada a cada volta da tarefa. Age quando o WiFi esta LIGADO EM AJUSTES e,
// mesmo assim, sem rede nem tentativa em andamento: depois de uma desistencia
// (FALHOU) ou de qualquer caminho que tenha deixado o estado em DESLIGADO sem
// o usuario pedir (s_querer continua de pe). Ligado, conectando ou sem rede
// guardada cancela a agenda (a proxima vez recomeca do passo curto).
//
// ACORDAR A TELA PROCURA NA HORA: quem acende o Tab5 quer a rede agora, e
// nao no fim de um passo de ate 5 min que comecou longe de casa.
static void retomar_se_preciso(void)
{
  static bool dormia = false;
  const bool dorme = tab5_power_em_repouso();
  const bool acordou = dormia && !dorme;
  dormia = dorme;
  if (!s_querer || !s_pilha_de_pe || s_so_radio
   || (s_estado != TAB5_WIFI_FALHOU && s_estado != TAB5_WIFI_DESLIGADO)) {
    s_retomar_ms = 0;
    return;
  }
  const uint32_t agora = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
  if (acordou) {
    s_retomar_passo = RETOMAR_MIN_MS / 2;   // dobra para o minimo logo abaixo
    s_retomar_ms = agora;
  } else if (!s_retomar_ms) {
    s_retomar_passo = RETOMAR_MIN_MS;
    s_retomar_ms = agora + s_retomar_passo;
    return;
  }
  if ((int32_t)(agora - s_retomar_ms) < 0) { return; }
  ESP_LOGI(TAG, "retomada: procurando redes conhecidas");
  tratar_procurar();
  s_retomar_passo = s_retomar_passo * 2 > RETOMAR_MAX_MS ? RETOMAR_MAX_MS : s_retomar_passo * 2;
  s_retomar_ms = agora + s_retomar_passo;
}

// TROCA PARA A REDE s_alvo (ja conhecida - quem pede gravou antes).
static void tratar_trocar(void)
{
  const int r = achar_rede(s_alvo);
  if (r < 0) { return; }
  usar_rede(r);
  if (!s_pilha_de_pe) { tratar_ligar(); return; }
  s_tentativas = 0;
  s_descendo = false;
  s_so_radio = false;
  if (!s_radio_ativo) {
    aplicar_config();
    s_radio_ativo = (esp_wifi_start() == ESP_OK);   // STA_START conecta
  } else {
    s_descendo = true;          // a queda da rede antiga nao e falha
    esp_wifi_disconnect();
    s_descendo = false;
    aplicar_config();
    esp_wifi_connect();
  }
  s_estado = TAB5_WIFI_CONECTANDO;
  ESP_LOGI(TAG, "trocando para \"%s\"", s_ssid);
}

// SO RADIO: solta o roteador e deixa o radio livre para outro canal. O
// esp_wifi_start() cobre o caso de o radio ter sido parado (desistencia longe
// do roteador): o ESP-NOW precisa dele INICIADO, so nao associado.
// SO O ENLACE COM O C6, SEM ROTEADOR. Quem precisa do C6 mas nao
// do WiFi - o Bluetooth - sobe a stack por aqui. Mesmo caminho do LIGAR
// (subir_pilha, na tarefa do WiFi, que e a dona do esp_hosted), sem ler
// credencial e sem esp_wifi_start: o estado do WiFi continua o que era.
static void tratar_pilha(void)
{
  if (s_pilha_de_pe) { return; }
  if (subir_pilha()) {
    s_pilha_de_pe = true;
    if (s_ao_subir) { s_ao_subir(); }   // quem espera o enlace acorda agora
  } else if (s_estado != TAB5_WIFI_C6_MUDO) {
    ESP_LOGE(TAG, "pilha do C6 nao subiu - quem pediu (Bluetooth) fica sem radio");
  }
}

static void tratar_so_radio(void)
{
  if (!s_pilha_de_pe) { return; }
  s_so_radio = true;
  s_tentativas = 0;
  s_descendo = false;
  esp_wifi_start();       // ja iniciado: erro benigno
  s_radio_ativo = true;
  esp_wifi_disconnect();
  s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
  strcpy(s_ip, "0.0.0.0");
  s_estado = TAB5_WIFI_DESLIGADO;
  ESP_LOGI(TAG, "so radio: roteador solto, radio de pe (ESP-NOW em canal proprio)");
}

static void tratar_roteador(void)
{
  if (!s_pilha_de_pe) { return; }
  s_so_radio = false;
  s_tentativas = 0;
  s_descendo = false;
  esp_wifi_start();
  s_radio_ativo = true;
  s_estado = TAB5_WIFI_CONECTANDO;
  esp_wifi_connect();
  ESP_LOGI(TAG, "voltando ao roteador");
}

static void tratar_desligar(void)
{
  if (!s_pilha_de_pe) { s_estado = TAB5_WIFI_DESLIGADO; return; }

  s_descendo = true;
  esp_wifi_disconnect();
  esp_wifi_stop();       // nivel 1: stack e link SDIO seguem de pe
  s_radio_ativo = false;

  s_rssi   = TAB5_WIFI_RSSI_DESCONHECIDO;
  strcpy(s_ip, "0.0.0.0");
  // Preserva FALHOU/SEM_CREDENCIAL: quando a parada veio de uma desistencia, o
  // motivo tem que continuar na tela. Sobrescrever por DESLIGADO faria parecer
  // que o usuario desligou - e ele ficaria sem saber que a senha esta errada.
  if (s_estado != TAB5_WIFI_FALHOU && s_estado != TAB5_WIFI_SEM_CREDENCIAL
   && s_estado != TAB5_WIFI_C6_MUDO) {
    s_estado = TAB5_WIFI_DESLIGADO;
  }
  ESP_LOGI(TAG, "WiFi desligado (o C6 segue alimentado, religa rapido)");
}

static void varrer_e_listar(void);

// VARREDURA DO CONSOLE. Ela solta o roteador para varrer; no fim, devolve:
// sem isto o s_descendo ficava de pe (toda queda seguinte era tratada como
// "desligado de proposito") e o Tab5 ficava sem rede ate religar o WiFi.
static void tratar_varrer(void)
{
  const bool tinha_rede = (s_estado == TAB5_WIFI_CONECTADO || s_estado == TAB5_WIFI_CONECTANDO);
  varrer_e_listar();
  s_varrendo = false;
  s_descendo = false;
  if (s_pilha_de_pe && tinha_rede && s_querer && !s_so_radio) {
    s_tentativas = 0;
    s_estado = TAB5_WIFI_CONECTANDO;
    esp_wifi_connect();
  }
}

static void varrer_e_listar(void)
{
  if (!s_pilha_de_pe)
  {
    printf("WiFi nunca foi ligado neste boot - ligue no app Ajustes primeiro.\n");
    return;
  }

  s_varrendo = true;
  s_descendo = true;          // uma queda aqui nao e falha, e consequencia

  esp_wifi_disconnect();
  esp_wifi_start();           // se ja estiver iniciado, devolve erro benigno
  s_radio_ativo = true;

  wifi_scan_config_t sc = { };
  sc.show_hidden = true;      // rede oculta aparece com SSID vazio

  printf("varrendo...\n");
  esp_err_t err = esp_wifi_scan_start(&sc, true);   // bloqueante
  if (err != ESP_OK)
  {
    printf("varredura falhou: %s\n", esp_err_to_name(err));
    s_varrendo = false;
    return;
  }

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n == 0)
  {
    printf("NENHUMA rede encontrada.\n"
           "  Se ha redes 2.4GHz por perto, o problema e o radio do C6, nao a sua rede.\n");
    s_varrendo = false;
    return;
  }

  wifi_ap_record_t* aps = (wifi_ap_record_t*) calloc(n, sizeof(wifi_ap_record_t));
  if (!aps) { printf("sem memoria para %u redes\n", (unsigned)n); s_varrendo = false; return; }

  esp_wifi_scan_get_ap_records(&n, aps);
  printf("%u rede(s):\n", (unsigned)n);
  printf("  %-32s %5s %3s %s\n", "SSID", "dBm", "ch", "seguranca");
  for (uint16_t i = 0; i < n; ++i)
  {
    printf("  %-32s %5d %3d %s\n",
           aps[i].ssid[0] ? (const char*)aps[i].ssid : "(oculta)",
           (int)aps[i].rssi, (int)aps[i].primary,
           aps[i].authmode == WIFI_AUTH_OPEN ? "aberta" : "com senha");
  }
  free(aps);

  printf("\nO Tab5 so enxerga 2.4GHz (o radio e um ESP32-C6).\n"
         "Se a sua rede nao esta na lista, ela e 5GHz ou o nome esta diferente -\n"
         "compare LETRA POR LETRA, maiuscula conta.\n");
  s_varrendo = false;
}

// CONECTANDO SEM FIM. Cada tentativa termina num evento (IP ou queda), e e a
// queda que conta as tentativas ate a desistencia. Se o esp_wifi_connect for
// recusado ou o evento se perder no caminho do C6, nenhum dos dois vem: o
// estado ficava em CONECTANDO para sempre, sem desistir e sem retomada.
// 60 s cobrem com folga as 6 tentativas (~4 s cada, com a pausa).
#define CONECTANDO_PRAZO_MS 60000u
static void vigiar_conectando(void)
{
  static uint32_t desde = 0;
  if (s_estado != TAB5_WIFI_CONECTANDO || s_descendo) { desde = 0; return; }
  const uint32_t agora = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
  if (!desde) { desde = agora | 1u; return; }
  if (agora - desde < CONECTANDO_PRAZO_MS) { return; }
  desde = 0;
  ESP_LOGW(TAG, "%u s conectando sem resposta - desisto; a retomada procura de novo",
           (unsigned)(CONECTANDO_PRAZO_MS / 1000));
  s_estado = TAB5_WIFI_FALHOU;
  tratar_desligar();
}

static void vTaskWifi(void*)
{
  comando_t c;
  for (;;)
  {
    // Espera com prazo em vez de portMAX_DELAY: o prazo e o batimento que
    // atualiza o RSSI. Ir ao C6 pelo SDIO e caro, entao 5s - a barra de status
    // le o cache e nunca vai ao radio.
    if (xQueueReceive(s_fila, &c, pdMS_TO_TICKS(5000)) == pdTRUE)
    {
      if      (c == CMD_LIGAR)    { tratar_ligar(); }
      else if (c == CMD_VARRER)   { tratar_varrer(); }
      else if (c == CMD_SO_RADIO) { tratar_so_radio(); }
      else if (c == CMD_ROTEADOR) { tratar_roteador(); }
      else if (c == CMD_ECONOMIA) { if (s_pilha_de_pe) { aplicar_economia(); } }
      else if (c == CMD_PILHA)    { tratar_pilha(); }
      else if (c == CMD_PROCURAR) { tratar_procurar(); }
      else if (c == CMD_TROCAR)   { tratar_trocar(); }
      else if (c == CMD_RECONECTAR) {
        // A pausa de 1 s entre tentativas (o roteador recusou, o sinal
        // piscou), agora fora da tarefa de eventos. Se nesse meio tempo
        // pediram para descer ou para soltar o roteador, nao reconecta.
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_descendo && !s_so_radio && s_estado == TAB5_WIFI_CONECTANDO) { esp_wifi_connect(); }
      }
      else                        { tratar_desligar(); }
      vigiar_conectando();
      retomar_se_preciso();
      continue;
    }
    vigiar_conectando();
    retomar_se_preciso();

    // Em repouso ninguem ve a barra: medir o sinal seria uma ida ao C6 pelo
    // SDIO a cada 5 s para nada. Ao acordar, a proxima volta mede.
    if (s_estado == TAB5_WIFI_CONECTADO && !tab5_power_em_repouso())
    {
      wifi_ap_record_t ap = { };
      const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);

      if (err != ESP_OK)
      {
        s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
      }
      else if (ap.rssi == 0)
      {
        // ZERO NAO E UM SINAL FORTE, E SIM UM CAMPO NAO PREENCHIDO.
        //
        // 0 dBm seriam 1mW chegando na antena - so encostando no roteador. Na
        // pratica um WiFi util vive entre -40 e -85 dBm. Quando o C6 devolve
        // zero e porque a resposta do RPC veio vazia, e mostrar "0 dBm" na tela
        // seria pior que nao mostrar nada: parece sinal maximo.
        //
        // Suspeito do descasamento de firmware que aparece no boot
        // ("esp-hosted fw versions: host=3.0.6 coprocessor=0.0.0"). Este log,
        // uma vez a cada troca de estado, e o que vai confirmar ou desmentir.
        if (s_rssi != TAB5_WIFI_RSSI_DESCONHECIDO) {
          ESP_LOGW(TAG, "o C6 devolveu RSSI=0 (campo nao preenchido) - "
                        "mostrando como indisponivel");
        }
        s_rssi = TAB5_WIFI_RSSI_DESCONHECIDO;
      }
      else
      {
        s_rssi = ap.rssi;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void tab5_wifi_iniciar(void)
{
  // ---------------------------------------------------------------------
  // CALA O DILUVIO DO SDIO. Nasceu.
  // ---------------------------------------------------------------------
  // Quando o enlace com o C6 morre, a tarefa interna do esp_hosted continua
  // tentando para sempre e cada tentativa loga DUAS linhas em nivel ERROR:
  //
  //   E (23294) eh_sdio: sdio_get_tx_buffer_num: err: 263
  //   E (23295) sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107
  //
  // Sao milhares por segundo. O console fica inutilizavel - nao da para ler o
  // que aconteceu ANTES nem digitar um comando para diagnosticar, justamente
  // quando voce mais precisa. Foi o que aconteceu na primeira tentativa de
  // gravar firmware no C6: o log que explicaria a falha foi soterrado pela
  // consequencia dela.
  //
  // A primeira linha ja diz tudo; da milesima em diante e ruido que ATRAPALHA.
  // Silenciamos as duas etiquetas e assumimos a responsabilidade de contar a
  // historia por conta propria - ver o aviso unico em subir_pilha() e o
  // comando "log", que devolve o barulho quando voce quiser ver o cru.
  //
  // Isto NAO esconde defeito: 0x107 e ESP_ERR_TIMEOUT, e "o C6 nao responde" e
  // uma informacao so, nao dez mil.
  esp_log_level_set("eh_sdio",  ESP_LOG_NONE);
  esp_log_level_set("sdmmc_io", ESP_LOG_NONE);

  // "SDMMC host already initialized, skipping init flow": sai a
  // cada montagem do cartao (1x/min: log e registro de energia). O controlador
  // SDMMC e UM so para os dois slots - cartao no 0, C6 no 1 -, o esp_hosted o
  // liga no boot, e a montagem do cartao ve isso e PULA a reinicializacao. E o
  // comportamento CERTO (reinicializar derrubaria o WiFi), e
  // a linha so anuncia isso. Aviso e erro dessa etiqueta continuam saindo.
  esp_log_level_set("sdmmc_periph", ESP_LOG_WARN);

  s_fila = xQueueCreate(6, sizeof(comando_t));
  s_trava_redes = xSemaphoreCreateRecursiveMutex();

  // Prioridade 3: abaixo da interface (4), para que subir o WiFi nunca engasgue
  // o toque nem o desenho. Acima do registro (1), porque uma associacao que
  // demora a responder atrasa o indicador na barra.
  //
  // STACK DE 6 KB. Com o C6 de volta, a subida da stack
  // (esp_hosted: transporte SDIO, RPC, auto-init de 'peer_data' e 'wifi')
  // roda INTEIRA nesta tarefa, e o monitor viu 4096 chegar a 228 bytes
  // livres na primeira conexao. Com 8 KB, dois dias de pilhas-*.csv (varias
  // subidas, conexoes e reconexoes) deram pico de ~4,3 KB. 6 KB e o pico
  // mais ~40%; estouro aqui reinicia o aparelho no meio de uma reconexao,
  // entao nao desca disto sem nova medicao.
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskWifi, "wifi", 6144, nullptr, 3, &h, tskNO_AFFINITY);
    tab5_dono_registrar(h, DONO_SERVICO, "WiFi");   // ver hal/tab5_owners.hpp
  }

  // Anuncia-se no boot. Sem esta linha o servico sobe invisivel, e um log de
  // boot em que o WiFi simplesmente nao aparece nao distingue "esta desligado
  // porque o usuario quer" de "a tarefa nem foi criada". A VONTADE do usuario
  // quem loga e o kernel, ao decidir ligar ou nao (ver sistema_iniciar) - o
  // HAL nao pergunta a um servico so para uma linha de log (auditoria de
  // camadas).
  ESP_LOGI(TAG, "servico no ar (radio desligado ate alguem pedir)");
}

void tab5_wifi_ligar(void)
{
  s_querer = true;
  tab5_power_radio(true);        // I2C - por isso so da tarefa de interface
  comando_t c = CMD_LIGAR;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_garantir_pilha(void)
{
  if (s_pilha_de_pe) { return; }
  tab5_power_radio(true);        // I2C - mesma regra do ligar
  comando_t c = CMD_PILHA;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_desligar(void)
{
  // O trilho continua ligado de proposito (nivel 1). Quando formos ao nivel 3,
  // o corte entra AQUI, e nao la na tarefa - ela nao pode tocar I2C.
  s_querer = false;
  comando_t c = CMD_DESLIGAR;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_definir_credenciais(const char* ssid, const char* senha)
{
  // So RAM: entra (ou atualiza) na lista de conhecidas e vira a rede em uso.
  if (!ssid || !ssid[0]) { return; }
  trava_redes_t trava;
  const bool aberta = (!senha || senha[0] == 0 || strcmp(senha, "-") == 0);
  int i = achar_rede(ssid);
  if (i < 0) {
    if (s_n_redes >= TAB5_WIFI_MAX_REDES) { i = TAB5_WIFI_MAX_REDES - 1; }   // cheia: sai a ultima
    else                                  { i = s_n_redes++; }
  }
  snprintf(s_redes[i].ssid, sizeof(s_redes[0].ssid), "%s", ssid);
  s_redes[i].aberta = aberta;
  snprintf(s_senhas[i], sizeof(s_senhas[0]), "%s", aberta ? "" : senha);
  usar_rede(i);
  s_lido = true;
  s_redes_ver = s_redes_ver + 1;
  ESP_LOGI(TAG, "rede \"%s\" guardada (%d conhecida(s))", ssid, s_n_redes);
}

bool tab5_wifi_gravar_credenciais(const char* ssid, const char* senha)
{
  if (!ssid || ssid[0] == 0) { return false; }
  trava_redes_t trava;
  // Garante a lista do cartao em RAM antes de reescrever o arquivo - senao a
  // rede nova apagaria as outras. Sem cartao, a rede vale nesta sessao e o
  // arquivo fica como esta (escrever_arquivo recusa).
  if (!s_arquivo_lido) { s_lido = false; ler_credenciais(); }
  tab5_wifi_definir_credenciais(ssid, senha);
  // RAM em texto puro; so o ARQUIVO e cifrado.
  return escrever_arquivo();
}

bool tab5_wifi_esquecer(const char* ssid)
{
  trava_redes_t trava;
  // Esquecer so o que da para tirar do ARQUIVO tambem - senao a rede voltaria
  // no proximo boot.
  if (!s_arquivo_lido) { s_lido = false; ler_credenciais(); }
  if (!s_arquivo_lido) { return false; }
  const int i = achar_rede(ssid);
  if (i < 0) { return false; }
  for (int k = i; k + 1 < s_n_redes; ++k) {
    s_redes[k] = s_redes[k + 1];
    memcpy(s_senhas[k], s_senhas[k + 1], sizeof(s_senhas[0]));
  }
  --s_n_redes;
  memset(s_senhas[s_n_redes], 0, sizeof(s_senhas[0]));
  for (int k = 0; k < s_n_vistas; ++k) { s_vistas[k].conhecida = achar_rede(s_vistas[k].ssid) >= 0; }
  s_redes_ver = s_redes_ver + 1;
  return escrever_arquivo();
}

void tab5_wifi_procurar(void)
{
  if (!s_fila) { return; }
  tab5_wifi_garantir_pilha();   // I2C do trilho: da tarefa de interface
  comando_t c = CMD_PROCURAR;
  s_procurando = true;
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_conectar(const char* ssid)
{
  if (!s_fila || !ssid) { return; }
  snprintf(s_alvo, sizeof(s_alvo), "%s", ssid);
  s_querer = true;   // escolher uma rede e pedir WiFi
  comando_t c = CMD_TROCAR;
  xQueueSend(s_fila, &c, 0);
}

bool     tab5_wifi_procurando(void)  { return s_procurando; }
int      tab5_wifi_n_vistas(void)    { return s_n_vistas; }
bool     tab5_wifi_vista(int i, tab5_wifi_visto_t* out)
{
  if (!out || i < 0 || i >= s_n_vistas) { return false; }
  *out = s_vistas[i];
  return true;
}
int      tab5_wifi_n_conhecidas(void) { return s_n_redes; }
bool     tab5_wifi_conhecida(const char* ssid) { return achar_rede(ssid) >= 0; }
uint32_t tab5_wifi_redes_versao(void) { return s_redes_ver; }

bool tab5_wifi_rede_guardada(int i, char* ssid, size_t tam_ssid, char* senha, size_t tam_senha)
{
  trava_redes_t trava;
  if (i < 0 || i >= s_n_redes) { return false; }
  snprintf(ssid, tam_ssid, "%s", s_redes[i].ssid);
  snprintf(senha, tam_senha, "%s", s_redes[i].aberta ? "" : s_senhas[i]);
  return true;
}

bool tab5_wifi_cifrar_texto(const char* claro, char* hex, size_t tam_hex) { return cifrar_senha(claro, hex, tam_hex); }
bool tab5_wifi_decifrar_texto(const char* hex, char* claro, size_t tam_claro) { return decifrar_senha(hex, claro, tam_claro); }

void tab5_wifi_varrer(void)
{
  comando_t c = CMD_VARRER;
  xQueueSend(s_fila, &c, 0);
}

bool tab5_wifi_pilha_de_pe(void) { return s_pilha_de_pe; }
void tab5_wifi_ao_subir(void (*fn)(void)) { s_ao_subir = fn; }
bool tab5_wifi_c6_enlace(void)    { return s_c6_enlace; }
bool tab5_wifi_c6_nosso(void)     { return s_c6_nosso; }
bool tab5_wifi_so_radio(void)     { return s_so_radio; }
bool tab5_wifi_radio_ativo(void)  { return s_radio_ativo; }

void tab5_wifi_manter_acordado(uint32_t quem, bool sim)
{
  // ATOMICO: ESP-NOW (a cada 1 s), CSI e Farejo chamam isto de tarefas
  // diferentes. Com ler-modificar-escrever comum, um apagava o bit que o
  // outro tinha acabado de ligar (o CSI perdia o receptor ligado).
  const uint32_t antes = sim ? __atomic_fetch_or(&s_acordado, quem, __ATOMIC_ACQ_REL)
                             : __atomic_fetch_and(&s_acordado, ~quem, __ATOMIC_ACQ_REL);
  const uint32_t depois = sim ? (antes | quem) : (antes & ~quem);
  if (depois == antes || !s_fila) { return; }
  comando_t c = CMD_ECONOMIA;   // o RPC ao C6 e da tarefa do WiFi
  xQueueSend(s_fila, &c, 0);
}

void tab5_wifi_modo_so_radio(bool ligar)
{
  comando_t c = ligar ? CMD_SO_RADIO : CMD_ROTEADOR;
  xQueueSend(s_fila, &c, 0);
}


tab5_wifi_estado_t tab5_wifi_estado(void) { return s_estado; }
int8_t             tab5_wifi_rssi(void)   { return s_rssi; }
const char*        tab5_wifi_ssid(void)   { return s_ssid; }
const char*        tab5_wifi_ip(void)     { return s_ip; }

const char* tab5_wifi_descricao(void)
{
  switch (s_estado)
  {
    case TAB5_WIFI_DESLIGADO:      return "WiFi";
    case TAB5_WIFI_SUBINDO:        return "ligando";
    case TAB5_WIFI_CONECTANDO:     return "conectando";
    case TAB5_WIFI_CONECTADO:      return s_ssid;
    case TAB5_WIFI_SEM_CREDENCIAL: return "sem rede";
    case TAB5_WIFI_FALHOU:         return "falhou";
    case TAB5_WIFI_C6_MUDO:        return "C6 nao responde";
  }
  return "WiFi";
}
