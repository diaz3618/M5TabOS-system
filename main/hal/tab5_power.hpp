// Perfil of energia of M5Tab5.
//
// The idea: EVERYTHING is born disabled, and each project strictly links what it uses.
// Tab5 exit of M5.begin() with camera out of reset, IMU with gyroscope in
// maximum performance, powered radio and both 5V rails (Grove and USB-A)
// ligados por pull-up nos expansores - nada disso e request, e some of conta of
// battery sem aparecer no code.
//
// The profile is applied in TWO moments, and they are not interchangeable:
//
//   fase 1  tab5_power_config()   ANTES of M5.begin()
//           Mexe nas flags of M5.config(). O periferico nunca chega a ser
// initialized - stays in sleep of factory, no I2C traffic.
//
//   fase 2  tab5_power_apply()  DEPOIS of M5.begin()
// Change what M5Unified connects unconditionally and does not expose:
// reset of camera, rail of radio, brightness, management of CPU.
//
// Uso tipico:
//
//   tab5_profile_t profile;          // everything disabled
// profile.radio = true;          // this project prints over WiFi
//   profile.brightness = 80;
//
//   auto cfg = M5.config();
//   tab5_power_config(cfg, profile);
//   M5.begin(cfg);
//   tab5_power_apply(profile);

#pragma once

#include "sdkconfig.h"

// MAXIMUM CPU CLOCK - depends on revision of P4 (sdkconfig.defaults.chip_*):
// 360 MHz no v1.x, 400 no v3.x. Os steps que o esp_pm aceita sao os of PLL
// (o maximum, a half, one quarto) e os of cristal of 40 MHz dividido.
#define TAB5_CPU_MAX_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define TAB5_CPU_STEPS "360 180 90 40 20 10 8 5 4 2 1"
#define TAB5_CPU_HALF  "180"
#else
#define TAB5_CPU_STEPS "400 200 100 40 20 10 8 5 4 2 1"
#define TAB5_CPU_HALF  "200"
#endif

#include <M5Unified.h>
#include <stdint.h>

struct tab5_profile_t
{
  // ---- fase 1: nem chega a ser inicializado -------------------------------

  // BMI270 (accelerometer + gyroscope). ATTENTION: when on, the M5Unified
  // deixa com giro active e advanced power save DESLIGADO (PWR_CONF=0x00) - o
  // worst case of chip consumption. If you just need orientation, turn this on
  // field e chame tab5_power_imu_accelerometer_only() depois of begin.
  bool imu           = false;

  // ES7210: os microfones. Ligado = codec ADC alimentado.
  bool microphone     = false;

  // ES8388: o codec of output + SPK_EN no expansor 0.
  bool speaker  = false;

  // RX8130. It costs almost nothing in current terms (own domain); turn_off here
  // saves boot time and I2C traffic, not battery.
  bool rtc           = true;

  // EXT5V_EN, expansor 0 bit 2. Alimenta a porta Grove.
  bool grove_5v      = false;

  // USB5V_EN, expansor 1 bit 3. Alimenta a USB-A embutida.
  bool usb_a_5v      = false;

  // ---- fase 2: so of para mexer com a placa of pe --------------------------

  // CAM_RST, expansor 0 bit 6. O M5Unified tira a camera of reset no boot.
  bool camera        = false;

  // WLAN_PWR_EN, expansor 1 bit 0. E o trilho of ESP32-C6 inteiro: WiFi E BLE
  // saem por ele. Cortar e a maior economia depois of backlight.
  // LEIA o comentario of tab5_power_radio() antes of religar em runtime.
  bool radio         = false;

  // 0..255. O backlight (PWM em GPIO22) e o maior consumidor of placa com a
  // screen acesa, e a resposta e quase linear: 80 gasta ~1/3 of 255.
  //
  // IN M5tabOS THIS FIELD IS NOT FROM THE APP. The system overwrites it with the
  // preferencia of user em sistema_impor_preferencias() - brightness e of who
  // holds the device, not the program that is on screen. App descriptor that
  // define brightness engana who le: o value nunca chega on hardware.
  //
  // O field continua existindo porque o tab5_power e AUTOCONTIDO e copiavel
  // for projects without a system of apps, where setting brightness here makes sense.
  uint8_t brightness     = 64;

  // ---- fase 2: gerenciamento of energia of CPU ----------------------------

  // Liga o DFS of IDF: a CPU cai sozinha para cpu_min_mhz when none
  // tarefa esta pronta. Exige CONFIG_PM_ENABLE=y no sdkconfig - sem isso o
  // esp_pm_configure() returns ESP_ERR_NOT_SUPPORTED (was the Arduino crash).
  bool pm_dinamico   = true;

  // O P4 of Tab5 e revisao < 3.0 (ver o bloco of REVISAO DO SILICIO no
  // sdkconfig.defaults). Nessa familia as frequencias validas sao
  // 360, 180, 90 e XTAL/N (40, 20, 10, 8, 5, 4, 2, 1) - a escada 400/200/100
  // is of revision >= 3.0 and is NOT valid here. Any other value does the
  // esp_pm_configure return_card ESP_ERR_INVALID_ARG.
  //
  // TESTED: ceiling lowered to 180 WITH THE SCREEN ON (not just in the
  // sleep) deu erro in pratica - of iteration a 360. Ou seja, mesmo com a
  // priority no AXI-ICM ja active (tab5_bus_prioritize_screen), o panel
  // DSI ainda precisa of CPU no ceiling pra sustentar o frame sem underrun. A
  // lock of PM that it holds (dsi_dpi, CPU_FREQ_MAX) is not conservatism
  // of driver - is a real dependency on this hardware. There is no step
  // intermediate to try (300 does not exist on this ladder - only 360/180/90
  // vem of PLL); o next candidato, se algum dia valer a pena reabrir isto,
  // e measure com one clock of panel MAIS BAIXO que 60MHz (menos banda
  // required) and see if 180 is enough - not tried yet.
  int  cpu_max_mhz   = TAB5_CPU_MAX_MHZ;
  int  cpu_min_mhz   = 40;

  // Light sleep automatico no idle.
  //
  // TODAY THIS FIELD DOES NOTHING, and it is important to know this before trying:
  // CONFIG_FREERTOS_USE_TICKLESS_IDLE esta DESLIGADO no sdkconfig (conferido em
  // ). Without tickless, idle wakes up every tick and never goes to sleep,
  // so passing true here doesn't cause any error - simply nothing happens,
  // que e o pior type of failure para depurar.
  //
  // Connecting tickless is a decision of measurement, not of taste: with FREERTOS_HZ=1000
  // the gain depends on there being idle windows of several milliseconds, and the panel
  // DPI may not even allow sleep. Measure with tab5_power_locks() first.
  bool light_sleep_when_idle = false;
};

// ---- aplicacao of profile --------------------------------------------------

// Fase 1. Chame com o cfg of M5.config(), antes of M5.begin().
void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_profile_t& profile);

// Fase 2. Chame logo depois of M5.begin().
void tab5_power_apply(const tab5_profile_t& profile);

// ---- controles of runtime -------------------------------------------------

// Corta ou devolve o trilho of ESP32-C6.
//
// ATTENTION: turn_off drops WiFi and BLE together and restarts the C6. Reconnecting returns
// power, but the host stack (esp_hosted/esp_wifi) DOES NOT reconnect
// alone - SDIO needs to be renegotiated. Until we write this
// sequence, treat shutdown as a one-way path within one boot:
// turn off when finished using the radio and go to sleep, or restart to
// religar.
void tab5_power_radio(bool turn_on);


// Trilho of 5V of Grove (expansor 0, PortA). Ligar em runtime e necessario para
// PROBE what is plugged in before knowing which app you are going to use - the module needs
// of energia para se apresentar. Ver tab5_grove.
void tab5_power_grove(bool turn_on);

// Trilho ja esta on AGORA? Existe para tab5_grove.cpp decidir se precisa
// wait o modulo bootar antes of sondar - pergunta of ESTADO DE HARDWARE,
// not of preference of user (that's why it lives here, and not in settings.h: HAL
// pergunta a HAL, nunca a one service - ver docs/WRITING_APPS.md secao 8).
bool tab5_power_grove_on(void);

// Quando o Tab5 mexeu no 5V of Grove / reaplicou o profile pela latest vez
// (esp_timer in ms, 0 = never). Test or discard Tab5 on a restart of no.
uint32_t tab5_power_grove_touch_ms(void);
uint32_t tab5_power_apply_ms(void);

// CAM_RST. false = camera segurada em reset.
void tab5_power_camera(bool turn_on);

// Poe o BMI270 so com acelerometro e com advanced power save on.
// Returns false if the chip was not found (or if profile.imu was false).
bool tab5_power_imu_accelerometer_only(void);

// O profile of boot pediu a IMU (cfg.internal_imu). Para who precisa saber se
// uma IMU ausente e escolha ou failure - ver tab5_imu_measure().
bool tab5_power_imu_requested(void);

// ---- history curto, para grafico ----------------------------------------
//
// It's HERE, and not in the app, because tab5_power_measure() runs once per second
// for the task of interface EVEN AT REST. Thus history is filled with
// app closed e o grafico ja nasce cheio on open - se morasse no app, ele
// would start empty every time.
//
// 60 points every 30s = 30 minutes. It costs 60 * 12 = 720 bytes.
#define TAB5_HIST_N     60
#define TAB5_HIST_SEG   30

// The temperature lives here and not in its own ring because it is the SAME instant: the
// dois sao lidos no mesmo tique of 1s e desenhados no mesmo eixo of time. Dois
// separate rings would give the work of marrying them later, and the chance of them
// slip one from another. It is worth -273 when there was no reading.
typedef struct { float volts; float mA; float temp; int16_t pct; } tab5_point_t;   // pct = a % of sistema naquele instante

// Copia o history em order cronologica (mais antigo first) para 'output',
// which needs to have TAB5_HIST_N positions. Returns how many points there are.
int tab5_power_history(tab5_point_t* output);

// Percentage BY VOLTAGE CURVE, pure (voltage corrected by current).
// This is NOT what the system shows: the % of system comes from the estimator in
// tab5_power_measure() (contagem of mAh + curva como correcao lenta). Exposta
// para diagnostics e para reprocessar arquivos antigos.
int tab5_power_pct(float volts, float mA);

// Estado of estimador, para o console: % com fracao, o que a curva diria
// now, of onde o estimador partiu, e minutos since a latest carga (-1 =
// none neste boot).
void tab5_power_debug_soc(float* soc, float* curva, const char** fonte, float* min_since_carga);

// ---- measurement (built-in INA226, shunt of 5 mOhm on the battery bus) ---

// RESISTENCIA INTERNA of conjunto (battery + fiacao + shunt), em ohms.
//
// Used to estimate the voltage AT REST from the voltage measurement under load:
//     V_aberto = V_medido - I * R      (I com signal: + loading, - descarregando)
//
// A formula vale nos dois sentidos, e por isso ela e uma so: descarregando, a
// voltage sinks and the correction SUMMS; loading, the voltage is inflated by the
// carregador e a correcao SUBTRAI.
//
// MEASURED, no longer estimated, from real energy.csv:
//
// 39 steps of current greater than 150mA in a discharge of 10.3h
//   R = dV/dI  ->  mediana 0.268 ohm, faixa 0.179..0.312
//
// Era 0.15 por estimativa, quase half of value real - entao a correcao of
// I*R era menor que deveria e a porcentagem caia when a screen acendia, que e
// exatamente o sintoma que esta constante existe para eliminar.
//
// Large steps on purpose: the voltage is read with a resolution of 1.25mV, and the
// 20mA of diferenca o dV se perde no ruido. Sao as transicoes of sleep que
// dao o degrau clean.
#define TAB5_INTERNAL_R 0.268f

// Capacidade measurement of battery, em mAh - SUBSTITUIDA pela
// analise of descarga of 21,5h SEM EMENDA achada no energia.csv (a of
// 1448mAh, comes from two sections glued together + voltage with only 2 spaces
// ver o comentario grande junto of CURVA[] em tab5_power.cpp para o metodo e
// for why the battery label saying "2400mAh" doesn't match this).
// Usada pelo estimador of carga (tab5_power.cpp, estimar_pct)
// is displayed here, in one place, so the console can show "how many % of
// capacity" sem guardar uma segunda copia of mesmo numero.
#define TAB5_CAPACITY_MAH 1306.8f

struct tab5_measurement_t
{
  float volts;   // bus (2S Li-Po, ~7.4V nominal)
  float mA;      // (+) loading / (-) descarregando
  float mW;      // power on the battery bus
  int   pct;     // estimated load 0..100 (negative = the model cannot say)
};

// ATTENTION - COMPETITION: these readings go to INA226 by the internal I2C, and the
// I2C_Class of M5Unified does NOT have any mutex (checked in the source). If another
// task use the same bus at the same time - and M5.update() uses it, to read
// touch and buttons - the two transactions are interspersed and you read REGISTRATION
// ERRADO, sem erro nenhum, so numero sem sentido. Chame everything daqui a partir of
// SAME task that calls M5.update(), or protect the bus with one mutex
// seu que envolva os dois.
tab5_measurement_t tab5_power_measure(void);

// The LAST measurement made, without touch on I2C. And what other tasks should use.
//
// Design rule: only the interface task speaks on the I2C bus, because the
// I2C_Class of M5Unified does not have a mutex and two interleaving tasks read
// register changed without error. If you're not familiar with the interface, read it from here.
// The interface updates this value every second, awake or asleep.
tab5_measurement_t tab5_power_latest(void);

// Frequencia of operacao of CPU NESTE INSTANTE, em MHz. Com o DFS on
// (pm_dinamico) it changes at runtime, so read it with each update instead of
// guardar o value uma vez no boot.
int tab5_power_cpu_mhz(void);

// Imprime uma line of log com a measurement current, marcada com a label dada.
// Use before/after of each cut to find out how much it was worth.
void tab5_power_log(const char* label);


// Lists active PM LOCKS on the console.
//
// There is for stop by measurement, and not by deduction, the question that delays
// all the energy savings of this project: WHO holds the CPU at maximum? Until
// hoje a resposta era inferida (o panel DPI puxa a PSRAM sem stop, logo deve
// be him). esp_pm_dump_locks() tells the name of each lock and how many times it was
// adquirida - se o panel aparecer com ESP_PM_CPU_FREQ_MAX, esta provado; se
// aparecer outro owner, a gente estava caçando o alvo errado.
//
// Chamar em pontos diferentes (boot, com app open, em sleep) e comparar as
// listas mostra o que released e o que nunca released.
void tab5_power_locks(const char* label);

// ---- mode of espera (screen apagada + clock bottom) ---------------------------
//
// WHY: lowering the clock with panel active DOES NOT work - it was measured in
// e o DSI passou a dar underrun em todo frame. O panel DPI
// transmits the framebuffer of PSRAM continuously, and at 180MHz the bus does not
// feeds him to the team. M5.Display.sleep() DOES NOT resolve this: setSleep() of
// M5GFX so manda o comando DCS 0x10 (SLPIN) para o panel; o controlador DPI
// continua puxando ~1.8MB por frame of PSRAM para one panel que ignora.
//
// Output and stop the flow itself with esp_lcd_dpi_panel_set_pattern(): the source
// of IDF diz "stop the DSI bridge from generating the DPI stream" when o
// default and not equal to NONE. With the fetch stopped, there is no further underrun possible
// e o clock pode cair.
//
// Repare que sao DOIS ganhos empilhados, e provavelmente o first vale mais:
// stop streaming eliminates continuous PSRAM traffic that exists today even with
// a screen "dormindo"; baixar o clock e o bonus que so stays disponivel depois.
//
// -------------------------------------------------------------------------
// MEDIDO com tab5_power_locks() - leia antes of mexer aqui
// -------------------------------------------------------------------------
// O esp_pm_dump_locks() no boot devolve:
//
//     dsi_dpi   CPU_FREQ_MAX     active 1
//     dsi_phy   NO_LIGHT_SLEEP   active 1
//
// Confirms the deduction about the clock: who holds the CPU and the panel, not the
// M5Unified nem a revisao of silicio. Mas acrescenta one fato NOVO e trouxe
// along with a limit that we didn't know:
//
// 1. The clock lock DOES NOT prevent lowering the ceiling. ESP_PM_CPU_FREQ_MAX holds the
//    CPU no value of max_freq_mhz, e esse value e ours - por isso o sleep
// with adjust_ceiling(90) it works. What breaks at 90MHz is the underrun, not the
// hangs. The above strategy is still valid.
//
// 2. LIGHT SLEEP IS IMPOSSIBLE while the DSI bus exists, and no
//    set_pattern muda isso. Verificado no fonte of IDF: em
// esp_lcd_panel_dpi.c the lock is acquired in CRIACAO of panel (line 291)
// and are freed within the destructor, along with free() (lines 405-406); in
// esp_lcd_mipi_dsi_bus.c is equal (72 -> 151). set_pattern() does not have a
//    unica referencia a pm_lock.
//
// Consequencia pratica: turn_on CONFIG_FREERTOS_USE_TICKLESS_IDLE hoje daria
// ZERO - o sistema nunca chegaria a dormir. O unico caminho para light sleep e
// DESTROY panel and bus on erase a screen and recreate them on wake, which
// and large and risky work (M5GFX is the owner of these objects). Don't try for
// cheap way: it doesn't exist.

// Entra em sleep: apaga a screen, para o fluxo of video e low o ceiling of
// clock. Devolve false se algum step falhar (e nesse caso desfaz o que deu).
//
// cpu_mhz: steps valid_entries nesta revisao of silicio sao 360, 180, 90 e XTAL/N.
//
// ATTENTION: do not go to sleep with the radio on without testing. The SDIO driver
// C6 has its own bandwidth requirements, and as the PM lock holds at MAXIMUM,
// lower the ceiling low the clock for it too.
bool tab5_power_sleep(int cpu_mhz = 90);

// Sai of sleep, NESTA ORDEM: sobe o ceiling of clock, CONFIRMA que a frequencia
// subiu of fato, so entao religa o fluxo of video e acorda a screen.
//
// A confirmacao e o intertravamento: religar o fluxo ainda em clock bottom
// would bring of iteration the storm of underrun. If the frequency does not start within
// prazo, esta funcao devolve false e DEIXA A TELA APAGADA of proposito - falhar
// escuro e melhor que falhar com a imagem rasgando.
bool tab5_power_resume(void);

// Lights up the screen in a ramp, up to the previous brightness.
//
// SEPARADA of resume() of proposito: o resume() devolve o panel on ar mas
// DEIXA A LUZ APAGADA, para who chamou redesenhar a screen antes of qualquer
// pixel ficar visivel. Sem essa separacao, a luz sobe mostrando o framebuffer
// velho ou o lixo of ressincronizacao of ponte DSI - o "artefato" on wake.
//
//   tab5_power_resume();   // iteration clock, fluxo e panel, screen ainda escura
//   ...redesenhe everything...
//   tab5_power_wake_screen();   // now sim
void tab5_power_wake_screen(void);

// Hook called at each step of ramp of brightness of wake (the ramp blocks
// a tarefa por ~300 ms; sem o gancho, qualquer coisa animada in screen stays
// congelada nesse time). Rodar in tarefa of interface. nullptr = nenhum.
void tab5_power_ramp_hook(void (*step)(void));

// ---------------------------------------------------------------------------
// WHICH SCREEN DOES THIS TAB5 HAVE
// ---------------------------------------------------------------------------
// O Tab5 exited com tres telas (todas 720x1280 MIPI-DSI, mesma placa):
// ILI9881C + GT911 - launch (2025): panel and touch on separate chips;
//   ST7123           - since out/2025: panel e touch no MESMO chip (TDDI);
//   ST7121           - since abr/2026: outro TDDI of mesma familia.
// M5GFX discovers itself in M5.begin() and assembles the right driver; the drawing and the
// common touch (M5.Display, M5.Touch) work the same in all three. What NOT
// works the same and what this system does at the bottom of M5GFX, and each point
// desses pergunta aqui antes:
// - watchman of touch le registers of GT911 direct (kernel/core.cpp);
//   - o sleep fundo (SLPIN + unmount o DSI) - num TDDI o touch e varrido
//     junto com a imagem, entao com o panel dormindo o touch provavelmente
//     morre, e o touch e o que acorda a screen. Nos ST o sleep so apaga a luz;
//   - o clock of pixel (tab5_power_dpi_clock): os porticos que o M5GFX
//     usa nos ST deixam a varredura of touch in folga vertical, e mexer no
//     clock muda essa folga. Nos ST ele stays no of fabrica.
// The ST paths were written without testing equipment: they are the
// CONSERVATIVES (which the M5GFX already does on its own), never make a new guess.
//
// Detectado uma vez, in primeira chamada - que tem que ser DEPOIS of
// M5.begin() e ANTES of qualquer troca of clock (main.cpp chama logo apos
// o begin). Qualquer tarefa depois disso.
typedef enum {
  TAB5_PANEL_UNKNOWN = 0,
  TAB5_PANEL_ILI9881C,
  TAB5_PANEL_ST7123,
  TAB5_PANEL_ST7121,
} tab5_panel_t;
tab5_panel_t tab5_panel(void);
const char*   tab5_panel_name(void);
// true only on ILI9881C: the touch and one separate GT911 (direct watch, pulse on
// GPIO 23 confirmado, sleep fundo e clock ajustavel validados nele).
bool          tab5_panel_touch_gt911(void);

// ---------------------------------------------------------------------------
// RELOGIO DE PIXEL DO PAINEL
// ---------------------------------------------------------------------------
// O panel DPI le o framebuffer of PSRAM sem stop, no ritmo deste clock. A
// 80MHz (o que o M5GFX programa para o ILI9881C) sao 64 frames por segundo e
// 118,5 MB/s of leitura CONTINUA. A 60MHz sao 48 frames e 88,9 MB/s.
//
// Esses 25% a menos sao a unica coisa que reduz a demanda of panel of forma
// permanente - todo o resto of sistema (fatiar limpeza, evitar clear duas
// vezes, espacar rajadas) mexe no OUTRO lado of disputa. Ver a explicacao
// complete em tab5_power.cpp, junto of implementacao.
//
// Trocar o clock DESMONTA E REMONTA o panel - o mesmo caminho of despertar.
// Returns false (and iteration alone on previous value) if the panel does not start.
bool tab5_power_dpi_clock(int mhz);

// Clock current, in MHz; 0 if there is no panel.
int  tab5_power_dpi_clock(void);

// Frames per second in clock current, through the gates that the M5GFX programmed
// ON THIS screen (each has its own). 0 if there is no panel.
double tab5_power_frames_per_second(void);

// ---------------------------------------------------------------------------
// CPU ROOF WITH SCREEN AWAKE - diagnostics, created
// ---------------------------------------------------------------------------
// Changes OS the ceiling of clock of CPU, while running, WITHOUT going to sleep and WITHOUT
// unmount o panel - diferente of tab5_power_sleep(), que apaga a screen
// por complete. Existe para testar uma pergunta especifica: one clock of
// panel mais bottom (ver tab5_power_dpi_clock acima) abre space para one
// ceiling of CPU menor, com a screen LIGADA e desenhando normalmente, sem underrun?
//
// Ninguem tinha testado essa combinacao ainda - so "CPU bottom com o panel no
// clock of sempre" (deu underrun) e "CPU bottom com o panel
// disassembled" (sleep, which always worked because there is no longer a panel to
// dar underrun). Esta funcao permite o terceiro caso: os dois LIGADOS e os
// dois devagar, on mesmo time.
//
// Valores valid_entries nesta revisao of silicio: 360, 180, 90 (PLL) ou XTAL/N -
// 40, 20, 10, 8, 5, 4, 2, 1. Returns false and does not change anything if the IDF refuses.
bool tab5_power_cpu_ceiling(int mhz);

// true enquanto estiver em sleep.
bool tab5_power_is_sleeping(void);
