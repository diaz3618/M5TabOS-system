#include "ajustes.h"

#include <M5Unified.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdio>
#include <cmath>

static const char *TAG = "ajustes";
static const char *NAMESPACE = "sistema";

static uint8_t s_brilho = 80;
static uint8_t s_volume = 128;   // meio da escala - nem mudo nem no talo na primeira vez
static bool    s_wifi   = false;
static bool    s_teclado = false;
static bool    s_grove   = false;
static uint8_t s_tela_s  = 30;   // padrao antigo do sistema, em segundos
static uint8_t s_relogio_painel = 60;  // padrao de fabrica do M5GFX
static bool    s_pronto = false;

// Graus x 1.000.000 (microgaus), inteiro: cabe num i32 da NVS sem depender
// de blob, e da ~11cm de precisao no equador - de sobra para marcar um
// endereco. 0,0 (Golfo da Guine) e o valor de fabrica e ninguem mora la -
// serve de sentinela de "nao configurado" sem precisar de uma flag a parte.
static int32_t s_casa_lat_ue6 = 0;
static int32_t s_casa_lon_ue6 = 0;

static bool s_beep_trilha = true;   // default TOCAR - ver o aviso grande em ajustes.h
static bool s_beep_mesh   = true;   // idem, par do Mesh
static uint8_t  s_espnow_canal = 0;
static uint8_t  s_mesh_via = AJUSTES_MESH_SERIAL;
static uint32_t s_mesh_pin = 123456;
static char     s_mesh_end[18] = "";
static bool     s_mesh_pin_tela = false;
static uint8_t  s_mesh_pos_min = 0;
static bool     s_mesh_internet = true;
static bool     s_mesh_rede_internet = false;
static bool     s_bluetooth = false;
static uint8_t  s_parede    = AJUSTES_PAREDE_SORTEIO;
static bool     s_bloqueio  = false;   // tela de bloqueio ao acordar (desligada de fabrica)
static bool     s_pendrive  = false;   // abrir o Pendrive ao ligar num computador (desligado de fabrica)
// Escala do giroscopio em partes por milhao (1000000 = 1,0), por eixo: i32 da
// NVS, como a casa, sem blob.
static int32_t  s_giro_esc_ppm[3] = { 1000000, 1000000, 1000000 };
static const char* const GIRO_ESC_CHAVE[3] = { "giro_esc_x", "giro_esc_y", "giro_esc_z" };
#define GIRO_ESC_MIN_PPM  900000
#define GIRO_ESC_MAX_PPM 1100000

static void gravar_u8(const char* chave, uint8_t valor)
{
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u8(h, chave, valor);
  nvs_commit(h);      // sem o commit, o valor some no proximo boot
  nvs_close(h);
}

// GRAVACAO ADIADA do que se ajusta ARRASTANDO (brilho, volume). A barra de
// Ajustes manda um valor novo a cada quadro do arraste, e cada gravacao era
// um nvs_commit na tarefa da interface: flash gravada com o cache desligado
// (os dois nucleos param alguns ms - o arraste engasgava) e dezenas de
// gravacoes de valores intermediarios que ninguem quer guardar. Agora o
// hardware muda NA HORA e a NVS recebe so o valor final, 1 s depois do
// ultimo movimento (na tarefa do esp_timer).
#define ADIAR_GRAVACAO_US 1000000
static esp_timer_handle_t s_adiar = nullptr;
static volatile bool s_brilho_pendente = false, s_volume_pendente = false;

static void gravar_adiados(void*)
{
  if (s_brilho_pendente) { s_brilho_pendente = false; gravar_u8("brilho", s_brilho); }
  if (s_volume_pendente) { s_volume_pendente = false; gravar_u8("volume", s_volume); }
}

static void adiar_gravacao(void)
{
  if (!s_adiar) {
    const esp_timer_create_args_t a = { .callback = gravar_adiados, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "ajustes_gravar",
                                        .skip_unhandled_events = true };
    if (esp_timer_create(&a, &s_adiar) != ESP_OK) { s_adiar = nullptr; gravar_adiados(nullptr); return; }
  }
  esp_timer_stop(s_adiar);   // cada valor novo empurra o prazo
  esp_timer_start_once(s_adiar, ADIAR_GRAVACAO_US);
}

void ajustes_iniciar(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Particao NVS gasta ou de versao antiga. Apagar aqui e seguro: so ha
    // ajustes nossos, nada que o usuario perca de verdade.
    ESP_LOGW(TAG, "NVS precisou ser reinicializada");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS indisponivel (%s) - ajustes so valem nesta sessao",
             esp_err_to_name(err));
    return;
  }
  s_pronto = true;

  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v;
    if (nvs_get_u8(h, "brilho", &v) == ESP_OK) { s_brilho = v; }
    if (nvs_get_u8(h, "volume", &v) == ESP_OK) { s_volume = v; }
    if (nvs_get_u8(h, "wifi",   &v) == ESP_OK) { s_wifi = (v != 0); }
    if (nvs_get_u8(h, "teclado",&v) == ESP_OK) { s_teclado = (v != 0); }
    if (nvs_get_u8(h, "grove",  &v) == ESP_OK) { s_grove = (v != 0); }
    if (nvs_get_u8(h, "tela_s", &v) == ESP_OK) {
      // Limita na LEITURA tambem, nao so na escrita: um valor gravado por uma
      // versao antiga (ou lixo) nao pode virar uma tela que nunca apaga.
      s_tela_s = (v < AJUSTES_TELA_MIN_S) ? AJUSTES_TELA_MIN_S
               : (v > AJUSTES_TELA_MAX_S) ? AJUSTES_TELA_MAX_S : v;
    }
    if (nvs_get_u8(h, "relogio_p", &v) == ESP_OK) {
      s_relogio_painel = (v < AJUSTES_RELOGIO_MIN) ? AJUSTES_RELOGIO_MIN
                       : (v > AJUSTES_RELOGIO_MAX) ? AJUSTES_RELOGIO_MAX : v;
    }
    int32_t vi;
    if (nvs_get_i32(h, "casa_lat", &vi) == ESP_OK) { s_casa_lat_ue6 = vi; }
    if (nvs_get_i32(h, "casa_lon", &vi) == ESP_OK) { s_casa_lon_ue6 = vi; }
    for (int e = 0; e < 3; ++e) {
      if (nvs_get_i32(h, GIRO_ESC_CHAVE[e], &vi) == ESP_OK
          && vi >= GIRO_ESC_MIN_PPM && vi <= GIRO_ESC_MAX_PPM) { s_giro_esc_ppm[e] = vi; }
    }
    if (nvs_get_u8(h, "beep_trilha", &v) == ESP_OK) { s_beep_trilha = (v != 0); }
    if (nvs_get_u8(h, "beep_mesh", &v) == ESP_OK) { s_beep_mesh = (v != 0); }
    if (nvs_get_u8(h, "espnow_c", &v) == ESP_OK && v <= 13) { s_espnow_canal = v; }
    if (nvs_get_u8(h, "mesh_via", &v) == ESP_OK && v <= AJUSTES_MESH_INTERNET) { s_mesh_via = v; }
    if (nvs_get_u8(h, "mesh_pos", &v) == ESP_OK) { s_mesh_pos_min = v; }
    if (nvs_get_u8(h, "mesh_net", &v) == ESP_OK) { s_mesh_internet = (v != 0); }
    if (nvs_get_u8(h, "mesh_rede", &v) == ESP_OK
     || nvs_get_u8(h, "mesh_eua", &v) == ESP_OK) {   // segunda chave: compatibilidade com gravacoes antigas
      s_mesh_rede_internet = (v != 0);
    }
    if (nvs_get_u8(h, "bluetooth", &v) == ESP_OK) { s_bluetooth = (v != 0); }
    if (nvs_get_u8(h, "bloqueio", &v) == ESP_OK) { s_bloqueio = (v != 0); }
    if (nvs_get_u8(h, "pendrive", &v) == ESP_OK) { s_pendrive = (v != 0); }
    if (nvs_get_u8(h, "parede", &v) == ESP_OK && v <= AJUSTES_PAREDE_NENHUM) { s_parede = v; }
    uint32_t vu;
    if (nvs_get_u32(h, "mesh_pin", &vu) == ESP_OK && vu <= 999999) { s_mesh_pin = vu; }
    if (nvs_get_u8(h, "mesh_pin_t", &v) == ESP_OK) { s_mesh_pin_tela = (v != 0); }
    size_t tam = sizeof(s_mesh_end);
    if (nvs_get_str(h, "mesh_end", s_mesh_end, &tam) != ESP_OK) { s_mesh_end[0] = 0; }
    nvs_close(h);
  }
  // Um valor absurdo gravado por engano nao pode deixar a tela inutilizavel.
  if (s_brilho < AJUSTES_BRILHO_MIN) { s_brilho = AJUSTES_BRILHO_MIN; }

  ESP_LOGI(TAG, "ajustes lidos: brilho %d, WiFi %s", (int)s_brilho, s_wifi ? "ligado" : "desligado");
}

uint8_t ajustes_brilho(void) { return s_brilho; }

void ajustes_definir_brilho(uint8_t brilho)
{
  if (brilho < AJUSTES_BRILHO_MIN) { brilho = AJUSTES_BRILHO_MIN; }
  s_brilho = brilho;

  // Aplica na hora: mexer numa barra e nao ver a tela reagir seria confuso.
  // Pela API normal (e nao pelo painel direto), porque este e o brilho
  // OFICIAL - e o valor que o tab5_power vai restaurar ao acordar do repouso.
  M5.Display.setBrightness(s_brilho);

  s_brilho_pendente = true;
  adiar_gravacao();   // a NVS recebe so o valor final (ver ADIAR_GRAVACAO_US)
}

uint8_t ajustes_volume(void) { return s_volume; }

void ajustes_definir_volume(uint8_t volume)
{
  if (volume < AJUSTES_VOLUME_MIN) { volume = AJUSTES_VOLUME_MIN; }
  if (volume > AJUSTES_VOLUME_MAX) { volume = AJUSTES_VOLUME_MAX; }
  s_volume = volume;

  // Sem custo de I2C aqui - setVolume() so grava o numero no objeto Speaker,
  // nao liga o ES8388 (ver o aviso grande em ajustes.h). Por isso e seguro
  // chamar de QUALQUER tarefa, ao contrario de tab5_camera_iniciar() e
  // companhia - a regra "so a tarefa de interface fala I2C" nao se aplica
  // aqui.
  M5.Speaker.setVolume(s_volume);

  s_volume_pendente = true;
  adiar_gravacao();   // idem: so o valor final vai para a NVS
}

bool ajustes_wifi(void) { return s_wifi; }

void ajustes_definir_wifi(bool ligado)
{
  s_wifi = ligado;
  gravar_u8("wifi", ligado ? 1 : 0);
}

bool ajustes_teclado(void) { return s_teclado; }

void ajustes_definir_teclado(bool ligado)
{
  s_teclado = ligado;
  gravar_u8("teclado", ligado ? 1 : 0);
  ESP_LOGI(TAG, "teclado: %s", ligado ? "ativado" : "desativado");
}

bool ajustes_grove(void) { return s_grove; }

uint8_t ajustes_tela_s(void) { return s_tela_s; }

void ajustes_definir_tela_s(uint8_t segundos)
{
  s_tela_s = (segundos < AJUSTES_TELA_MIN_S) ? AJUSTES_TELA_MIN_S
           : (segundos > AJUSTES_TELA_MAX_S) ? AJUSTES_TELA_MAX_S : segundos;
  gravar_u8("tela_s", s_tela_s);
}

void ajustes_definir_grove(bool ligado)
{
  s_grove = ligado;
  gravar_u8("grove", ligado ? 1 : 0);
  ESP_LOGI(TAG, "5V do Grove sempre ligado: %s", ligado ? "sim" : "nao");
}

uint8_t ajustes_relogio_painel(void) { return s_relogio_painel; }

void ajustes_definir_relogio_painel(uint8_t mhz)
{
  s_relogio_painel = (mhz < AJUSTES_RELOGIO_MIN) ? AJUSTES_RELOGIO_MIN
                   : (mhz > AJUSTES_RELOGIO_MAX) ? AJUSTES_RELOGIO_MAX : mhz;
  gravar_u8("relogio_p", s_relogio_painel);
  ESP_LOGI(TAG, "relogio do painel = %d MHz", (int)s_relogio_painel);
}

bool ajustes_casa(float* lat, float* lon)
{
  if (s_casa_lat_ue6 == 0 && s_casa_lon_ue6 == 0) { return false; }
  if (lat) { *lat = (float)s_casa_lat_ue6 / 1000000.0f; }
  if (lon) { *lon = (float)s_casa_lon_ue6 / 1000000.0f; }
  return true;
}

void ajustes_definir_casa(float lat, float lon)
{
  s_casa_lat_ue6 = (int32_t)(lat * 1000000.0f);
  s_casa_lon_ue6 = (int32_t)(lon * 1000000.0f);
  ESP_LOGI(TAG, "casa gravada: %.6f, %.6f", (double)lat, (double)lon);

  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_i32(h, "casa_lat", s_casa_lat_ue6);
  nvs_set_i32(h, "casa_lon", s_casa_lon_ue6);
  nvs_commit(h);
  nvs_close(h);
}

bool ajustes_beep_trilha(void) { return s_beep_trilha; }

void ajustes_definir_beep_trilha(bool tocar)
{
  s_beep_trilha = tocar;
  gravar_u8("beep_trilha", tocar ? 1 : 0);
  ESP_LOGI(TAG, "beep da Trilha: %s", tocar ? "ligado" : "desligado");
}

bool ajustes_beep_mesh(void) { return s_beep_mesh; }

void ajustes_definir_beep_mesh(bool tocar)
{
  s_beep_mesh = tocar;
  gravar_u8("beep_mesh", tocar ? 1 : 0);
  ESP_LOGI(TAG, "beep do Mesh: %s", tocar ? "ligado" : "desligado");
}

uint8_t ajustes_espnow_canal(void) { return s_espnow_canal; }

void ajustes_definir_espnow_canal(uint8_t canal)
{
  if (canal > 13) { canal = 0; }
  if (canal == s_espnow_canal) { return; }   // o servico reaplica no boot: sem regravar a flash a toa
  s_espnow_canal = canal;
  gravar_u8("espnow_c", canal);
}

uint8_t ajustes_mesh_via(void) { return s_mesh_via; }

void ajustes_definir_mesh_via(uint8_t via)
{
  if (via > AJUSTES_MESH_INTERNET) { via = AJUSTES_MESH_SERIAL; }
  if (via == s_mesh_via) { return; }   // a NVS tem ciclos de escrita finitos
  s_mesh_via = via;
  gravar_u8("mesh_via", via);
  ESP_LOGI(TAG, "Mesh vai pelo %s", via == AJUSTES_MESH_BLUETOOTH ? "Bluetooth"
                                  : via == AJUSTES_MESH_INTERNET  ? "internet" : "cabo Grove");
}

uint32_t ajustes_mesh_pin(void) { return s_mesh_pin; }

void ajustes_definir_mesh_pin(uint32_t pin)
{
  if (pin > 999999) { return; }
  s_mesh_pin = pin;
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u32(h, "mesh_pin", pin);
  nvs_commit(h);
  nvs_close(h);
}

void ajustes_mesh_ble_endereco(char out[18]) { memcpy(out, s_mesh_end, sizeof(s_mesh_end)); }

bool ajustes_mesh_pin_na_tela(void) { return s_mesh_pin_tela; }

void ajustes_definir_mesh_pin_na_tela(bool sim)
{
  if (sim == s_mesh_pin_tela) { return; }
  s_mesh_pin_tela = sim;
  gravar_u8("mesh_pin_t", sim ? 1 : 0);
}

void ajustes_definir_mesh_ble_endereco(const char* end)
{
  // Outro no: o que se sabia sobre o PIN do anterior nao vale para ele.
  if (strcmp(end ? end : "", s_mesh_end) != 0) { ajustes_definir_mesh_pin_na_tela(false); }
  snprintf(s_mesh_end, sizeof(s_mesh_end), "%s", end ? end : "");
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_str(h, "mesh_end", s_mesh_end);
  nvs_commit(h);
  nvs_close(h);
}

bool ajustes_mesh_internet(void) { return s_mesh_internet; }

void ajustes_definir_mesh_internet(bool ligado)
{
  if (ligado == s_mesh_internet) { return; }
  s_mesh_internet = ligado;
  gravar_u8("mesh_net", ligado ? 1 : 0);
}

bool ajustes_mesh_rede_internet(void) { return s_mesh_rede_internet; }

void ajustes_definir_mesh_rede_internet(bool ligado)
{
  if (ligado == s_mesh_rede_internet) { return; }
  s_mesh_rede_internet = ligado;
  gravar_u8("mesh_rede", ligado ? 1 : 0);
}

uint8_t ajustes_mesh_posicao_min(void) { return s_mesh_pos_min; }

void ajustes_definir_mesh_posicao_min(uint8_t minutos)
{
  if (minutos == s_mesh_pos_min) { return; }
  s_mesh_pos_min = minutos;
  gravar_u8("mesh_pos", minutos);
}

bool ajustes_bluetooth(void) { return s_bluetooth; }

void ajustes_definir_bluetooth(bool ligado)
{
  s_bluetooth = ligado;
  gravar_u8("bluetooth", ligado ? 1 : 0);
}

bool ajustes_bloqueio(void) { return s_bloqueio; }

void ajustes_definir_bloqueio(bool ligado)
{
  s_bloqueio = ligado;
  gravar_u8("bloqueio", ligado ? 1 : 0);
}

bool ajustes_pendrive(void) { return s_pendrive; }

void ajustes_definir_pendrive(bool ligado)
{
  s_pendrive = ligado;
  gravar_u8("pendrive", ligado ? 1 : 0);
}

uint8_t ajustes_parede(void) { return s_parede; }

void ajustes_definir_parede(uint8_t opcao)
{
  if (opcao > AJUSTES_PAREDE_NENHUM) { opcao = AJUSTES_PAREDE_SORTEIO; }
  s_parede = opcao;
  gravar_u8("parede", opcao);
}

float ajustes_giro_escala(int eixo)
{
  if (eixo < 0 || eixo > 2) { return 1.0f; }
  return (float)s_giro_esc_ppm[eixo] / 1000000.0f;
}

void ajustes_definir_giro_escala(int eixo, float escala)
{
  if (eixo < 0 || eixo > 2) { return; }
  int32_t ppm = (int32_t)lroundf(escala * 1000000.0f);
  if (ppm < GIRO_ESC_MIN_PPM || ppm > GIRO_ESC_MAX_PPM) { return; }
  s_giro_esc_ppm[eixo] = ppm;
  ESP_LOGI(TAG, "escala do giro, eixo %c: %.4f", "xyz"[eixo], (double)escala);

  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_i32(h, GIRO_ESC_CHAVE[eixo], ppm);
  nvs_commit(h);
  nvs_close(h);
}

// ORDEM DOS ICONES - ver ajustes.h. Lida da NVS na hora (so no boot e no
// console), sem copia em RAM: quem guarda a ordem em uso e o shell.
void ajustes_ordem_apps(char* out, size_t n)
{
  if (!out || n == 0) { return; }
  out[0] = 0;
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t tam = n;
  if (nvs_get_str(h, "apps_ordem", out, &tam) != ESP_OK) { out[0] = 0; }
  nvs_close(h);
}

void ajustes_definir_ordem_apps(const char* nomes)
{
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (nomes && nomes[0]) { nvs_set_str(h, "apps_ordem", nomes); }
  else                   { nvs_erase_key(h, "apps_ordem"); }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "ordem dos apps: %s", (nomes && nomes[0]) ? nomes : "(padrao)");
}

// APLICATIVOS ESCONDIDOS - ver ajustes.h. Lidos da NVS junto da ordem; quem
// guarda e aplica a lista em uso e o shell.
void ajustes_apps_ocultos(char* out, size_t n)
{
  if (!out || n == 0) { return; }
  out[0] = 0;
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t tam = n;
  if (nvs_get_str(h, "apps_ocultos", out, &tam) != ESP_OK) { out[0] = 0; }
  nvs_close(h);
}

void ajustes_definir_apps_ocultos(const char* nomes)
{
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (nomes && nomes[0]) { nvs_set_str(h, "apps_ocultos", nomes); }
  else                   { nvs_erase_key(h, "apps_ocultos"); }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "apps ocultos: %s", (nomes && nomes[0]) ? nomes : "(nenhum)");
}
