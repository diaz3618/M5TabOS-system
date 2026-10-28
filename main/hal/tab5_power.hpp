// Perfil of energia of M5Tab5.
//
// A ideia: TUDO nasce disabled, e cada projeto liga estritamente o que usa.
// O Tab5 exit of M5.begin() com camera fora of reset, IMU com giroscopio em
// performance maxima, radio alimentado e os dois trilhos of 5V (Grove e USB-A)
// ligados por pull-up nos expansores - nada disso e request, e some of conta of
// battery sem aparecer no code.
//
// O profile e aplicado em DOIS momentos, e eles nao sao intercambiaveis:
//
//   fase 1  tab5_power_config()   ANTES of M5.begin()
//           Mexe nas flags of M5.config(). O periferico nunca chega a ser
//           inicializado - stays no sleep of fabrica, sem trafego I2C.
//
//   fase 2  tab5_power_apply()  DEPOIS of M5.begin()
//           Mexe no que o M5Unified liga incondicionalmente e nao expoe:
//           reset of camera, trilho of radio, brightness, gerenciamento of CPU.
//
// Uso tipico:
//
//   tab5_profile_t profile;          // everything disabled
//   profile.radio  = true;          // este projeto imprime por WiFi
//   profile.brightness = 80;
//
//   auto cfg = M5.config();
//   tab5_power_config(cfg, profile);
//   M5.begin(cfg);
//   tab5_power_apply(profile);

#pragma once

#include "sdkconfig.h"

// CLOCK MAXIMO DO CPU - depende of revisao of P4 (sdkconfig.defaults.chip_*):
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

  // BMI270 (acelerometro + giroscopio). ATENCAO: when on, o M5Unified o
  // deixa com giro active e advanced power save DESLIGADO (PWR_CONF=0x00) - o
  // pior caso of consumo of chip. Se voce so precisa of orientation, ligue este
  // field e chame tab5_power_imu_accelerometer_only() depois of begin.
  bool imu           = false;

  // ES7210: os microfones. Ligado = codec ADC alimentado.
  bool microphone     = false;

  // ES8388: o codec of output + SPK_EN no expansor 0.
  bool speaker  = false;

  // RX8130. Custa quase nada em corrente (dominio proprio); turn_off aqui
  // economiza time of boot e trafego I2C, nao battery.
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
  // NO M5tabOS ESTE CAMPO NAO E DO APP. O sistema o sobrescreve com a
  // preferencia of user em sistema_impor_preferencias() - brightness e of who
  // segura o aparelho, nao of programa que esta in screen. Descritor of app que
  // define brightness engana who le: o value nunca chega on hardware.
  //
  // O field continua existindo porque o tab5_power e AUTOCONTIDO e copiavel
  // para projetos sem sistema of apps, onde set brightness aqui faz sentido.
  uint8_t brightness     = 64;

  // ---- fase 2: gerenciamento of energia of CPU ----------------------------

  // Liga o DFS of IDF: a CPU cai sozinha para cpu_min_mhz when none
  // tarefa esta pronta. Exige CONFIG_PM_ENABLE=y no sdkconfig - sem isso o
  // esp_pm_configure() devolve ESP_ERR_NOT_SUPPORTED (era a trava of Arduino).
  bool pm_dinamico   = true;

  // O P4 of Tab5 e revisao < 3.0 (ver o bloco of REVISAO DO SILICIO no
  // sdkconfig.defaults). Nessa familia as frequencias validas sao
  // 360, 180, 90 e XTAL/N (40, 20, 10, 8, 5, 4, 2, 1) - a escada 400/200/100
  // e of revisao >= 3.0 e NAO vale aqui. Qualquer outro value faz o
  // esp_pm_configure return_card ESP_ERR_INVALID_ARG.
  //
  // TESTADO: ceiling baixado para 180 COM A TELA LIGADA (nao so no
  // sleep) deu erro in pratica - of iteration a 360. Ou seja, mesmo com a
  // priority no AXI-ICM ja active (tab5_bus_prioritize_screen), o panel
  // DSI ainda precisa of CPU no ceiling pra sustentar o frame sem underrun. A
  // trava of PM que ele segura (dsi_dpi, CPU_FREQ_MAX) nao e conservadorismo
  // of driver - e uma dependencia real deste hardware. Nao ha degrau
  // intermediario para tentar (300 nao existe nesta escada - so 360/180/90
  // vem of PLL); o next candidato, se algum dia valer a pena reabrir isto,
  // e measure com one clock of panel MAIS BAIXO que 60MHz (menos banda
  // exigida) e ver se 180 passa a bastar - nao tentado ainda.
  int  cpu_max_mhz   = TAB5_CPU_MAX_MHZ;
  int  cpu_min_mhz   = 40;

  // Light sleep automatico no idle.
  //
  // HOJE ESTE CAMPO NAO FAZ NADA, e e importante saber disso antes of tentar:
  // CONFIG_FREERTOS_USE_TICKLESS_IDLE esta DESLIGADO no sdkconfig (conferido em
  // ). Sem tickless o idle acorda a cada tick e nunca chega a dormir,
  // entao passar true aqui nao of erro nenhum - simplesmente nao acontece nada,
  // que e o pior type of failure para depurar.
  //
  // Ligar tickless e uma decisao of medicao, nao of gosto: com FREERTOS_HZ=1000
  // o ganho depende of haver janelas ociosas of varios milissegundos, e o panel
  // DPI pode nem permitir o sono. Medir com tab5_power_locks() antes.
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
// ATENCAO: turn_off derruba WiFi e BLE juntos e reinicia o C6. Religar devolve
// a alimentacao, mas a stack of host (esp_hosted/esp_wifi) NAO se reconecta
// sozinha - o SDIO precisa ser renegociado. Enquanto a gente nao write essa
// sequencia, trate o desligamento como caminho of mao unica dentro of one boot:
// desligue when terminar of usar o radio e va dormir, ou reinicie para
// religar.
void tab5_power_radio(bool turn_on);


// Trilho of 5V of Grove (expansor 0, PortA). Ligar em runtime e necessario para
// SONDAR o que esta plugado antes of saber qual app vai usar - o modulo precisa
// of energia para se apresentar. Ver tab5_grove.
void tab5_power_grove(bool turn_on);

// Trilho ja esta on AGORA? Existe para tab5_grove.cpp decidir se precisa
// wait o modulo bootar antes of sondar - pergunta of ESTADO DE HARDWARE,
// nao of preferencia of user (por isso mora aqui, e nao em settings.h: HAL
// pergunta a HAL, nunca a one service - ver WRITING_APPS.md secao 8).
bool tab5_power_grove_on(void);

// Quando o Tab5 mexeu no 5V of Grove / reaplicou o profile pela latest vez
// (esp_timer em ms, 0 = nunca). Prova ou descarta o Tab5 num reinicio of no.
uint32_t tab5_power_grove_touch_ms(void);
uint32_t tab5_power_apply_ms(void);

// CAM_RST. false = camera segurada em reset.
void tab5_power_camera(bool turn_on);

// Poe o BMI270 so com acelerometro e com advanced power save on.
// Devolve false se nao achou o chip (ou se profile.imu era false).
bool tab5_power_imu_accelerometer_only(void);

// O profile of boot pediu a IMU (cfg.internal_imu). Para who precisa saber se
// uma IMU ausente e escolha ou failure - ver tab5_imu_measure().
bool tab5_power_imu_requested(void);

// ---- history curto, para grafico ----------------------------------------
//
// Fica AQUI, e nao no app, porque o tab5_power_measure() roda uma vez por segundo
// pela tarefa of interface MESMO EM REPOUSO. Assim o history se enche com o
// app closed e o grafico ja nasce cheio on open - se morasse no app, ele
// comecaria vazio toda vez.
//
// 60 pontos a cada 30s = 30 minutos. Custa 60 * 12 = 720 bytes.
#define TAB5_HIST_N     60
#define TAB5_HIST_SEG   30

// A temperatura mora aqui e nao num anel proprio porque e o MESMO instante: os
// dois sao lidos no mesmo tique of 1s e desenhados no mesmo eixo of time. Dois
// aneis separados dariam o work of casa-los depois, e a chance of eles
// escorregarem one of outro. Vale -273 when nao houve leitura.
typedef struct { float volts; float mA; float temp; int16_t pct; } tab5_point_t;   // pct = a % of sistema naquele instante

// Copia o history em order cronologica (mais antigo first) para 'output',
// que precisa ter TAB5_HIST_N posicoes. Devolve quantos pontos existem.
int tab5_power_history(tab5_point_t* output);

// Porcentagem PELA CURVA DE TENSAO, pura (tensao corrigida pela corrente).
// NAO e o que o sistema mostra: a % of sistema vem of estimador em
// tab5_power_measure() (contagem of mAh + curva como correcao lenta). Exposta
// para diagnostics e para reprocessar arquivos antigos.
int tab5_power_pct(float volts, float mA);

// Estado of estimador, para o console: % com fracao, o que a curva diria
// now, of onde o estimador partiu, e minutos since a latest carga (-1 =
// none neste boot).
void tab5_power_debug_soc(float* soc, float* curva, const char** fonte, float* min_since_carga);

// ---- medicao (INA226 embutido, shunt of 5 mOhm no barramento of battery) ---

// RESISTENCIA INTERNA of conjunto (battery + fiacao + shunt), em ohms.
//
// Serve para estimar a tensao EM REPOUSO a partir of tensao measurement sob carga:
//     V_aberto = V_medido - I * R      (I com signal: + loading, - descarregando)
//
// A formula vale nos dois sentidos, e por isso ela e uma so: descarregando, a
// tensao afunda e a correcao SOMA; loading, a tensao esta inflada pelo
// carregador e a correcao SUBTRAI.
//
// MEDIDO, nao mais estimado., a partir of energia.csv real:
//
//   39 steps of corrente maiores que 150mA numa descarga of 10,3h
//   R = dV/dI  ->  mediana 0.268 ohm, faixa 0.179..0.312
//
// Era 0.15 por estimativa, quase half of value real - entao a correcao of
// I*R era menor que deveria e a porcentagem caia when a screen acendia, que e
// exatamente o sintoma que esta constante existe para eliminar.
//
// Degraus grandes of proposito: a tensao e lida com resolucao of 1,25mV, e a
// 20mA of diferenca o dV se perde no ruido. Sao as transicoes of sleep que
// dao o degrau clean.
#define TAB5_INTERNAL_R 0.268f

// Capacidade measurement of battery, em mAh - SUBSTITUIDA pela
// analise of descarga of 21,5h SEM EMENDA achada no energia.csv (a of
// 1448mAh, vinha of duas sessoes coladas + tensao com só 2 casas
// ver o comentario grande junto of CURVA[] em tab5_power.cpp para o metodo e
// para o porque a label of battery dizer "2400mAh" nao bate com isto).
// Usada pelo estimador of carga (tab5_power.cpp, estimar_pct)
// e exposta aqui, num lugar so, para o console poder mostrar "quantos % of
// capacity" sem guardar uma segunda copia of mesmo numero.
#define TAB5_CAPACITY_MAH 1306.8f

struct tab5_measurement_t
{
  float volts;   // barramento (2S Li-Po, ~7.4V nominal)
  float mA;      // (+) loading / (-) descarregando
  float mW;      // potencia no barramento of battery
  int   pct;     // carga estimada 0..100 (negativo = o modelo nao sabe dizer)
};

// ATENCAO - CONCORRENCIA: estas leituras vao on INA226 pelo I2C interno, e o
// I2C_Class of M5Unified NAO tem mutex nenhum (conferido no fonte). Se outra
// tarefa usar o mesmo barramento on mesmo time - e M5.update() usa, para read
// touch e botoes - as duas transacoes se intercalam e voce le REGISTRADOR
// ERRADO, sem erro nenhum, so numero sem sentido. Chame everything daqui a partir of
// MESMA tarefa que chama M5.update(), ou proteja o barramento com one mutex
// seu que envolva os dois.
tab5_measurement_t tab5_power_measure(void);

// A ULTIMA measurement feita, sem touch no I2C. E o que outras tarefas devem usar.
//
// Regra of projeto: so a tarefa of interface fala no barramento I2C, porque o
// I2C_Class of M5Unified nao tem mutex e duas tarefas intercalando leem
// registrador trocado sem dar erro. Quem nao for a interface le daqui.
// A interface atualiza este value a cada segundo, acordada ou em sleep.
tab5_measurement_t tab5_power_latest(void);

// Frequencia of operacao of CPU NESTE INSTANTE, em MHz. Com o DFS on
// (pm_dinamico) ela muda em runtime, entao leia a cada atualizacao em vez of
// guardar o value uma vez no boot.
int tab5_power_cpu_mhz(void);

// Imprime uma line of log com a measurement current, marcada com a label dada.
// Use antes/depois of cada corte para saber quanto ele valeu.
void tab5_power_log(const char* label);


// Lista as TRAVAS DE PM ativas no console.
//
// Existe para stop por medicao, e nao por deducao, a pergunta que atrasa
// toda a economia of energia deste projeto: QUEM prende a CPU no maximum? Ate
// hoje a resposta era inferida (o panel DPI puxa a PSRAM sem stop, logo deve
// ser ele). O esp_pm_dump_locks() diz o name of cada trava e quantas vezes foi
// adquirida - se o panel aparecer com ESP_PM_CPU_FREQ_MAX, esta provado; se
// aparecer outro owner, a gente estava caçando o alvo errado.
//
// Chamar em pontos diferentes (boot, com app open, em sleep) e comparar as
// listas mostra o que released e o que nunca released.
void tab5_power_locks(const char* label);

// ---- mode of espera (screen apagada + clock bottom) ---------------------------
//
// O PORQUE: baixar o clock com o panel active NAO funciona - foi measured em
// e o DSI passou a dar underrun em todo frame. O panel DPI
// transmite o framebuffer of PSRAM continuamente, e a 180MHz o barramento nao
// alimenta ele a time. O M5.Display.sleep() NAO resolve isso: o setSleep() of
// M5GFX so manda o comando DCS 0x10 (SLPIN) para o panel; o controlador DPI
// continua puxando ~1.8MB por frame of PSRAM para one panel que ignora.
//
// A output e stop o proprio fluxo com esp_lcd_dpi_panel_set_pattern(): o fonte
// of IDF diz "stop the DSI bridge from generating the DPI stream" when o
// default e diferente of NONE. Com o fetch parado, nao ha mais underrun possivel
// e o clock pode cair.
//
// Repare que sao DOIS ganhos empilhados, e provavelmente o first vale mais:
// stop o streaming elimina trafego continuo of PSRAM que existe hoje mesmo com
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
// Confirma a deducao sobre o clock: who prende a CPU e o panel, nao o
// M5Unified nem a revisao of silicio. Mas acrescenta one fato NOVO e trouxe
// junto one limit que a gente nao conhecia:
//
// 1. A trava of clock NAO impede baixar o ceiling. ESP_PM_CPU_FREQ_MAX prende a
//    CPU no value of max_freq_mhz, e esse value e ours - por isso o sleep
//    com ajustar_teto(90) funciona. O que quebra a 90MHz e o underrun, nao a
//    trava. A estrategia acima continua valendo.
//
// 2. LIGHT SLEEP E IMPOSSIVEL enquanto o barramento DSI existir, e nenhum
//    set_pattern muda isso. Verificado no fonte of IDF: em
//    esp_lcd_panel_dpi.c a trava e adquirida in CRIACAO of panel (line 291)
//    e so liberada dentro of destrutor, junto of free() (lines 405-406); em
//    esp_lcd_mipi_dsi_bus.c e igual (72 -> 151). O set_pattern() nao tem uma
//    unica referencia a pm_lock.
//
// Consequencia pratica: turn_on CONFIG_FREERTOS_USE_TICKLESS_IDLE hoje daria
// ZERO - o sistema nunca chegaria a dormir. O unico caminho para light sleep e
// DESTRUIR panel e barramento on erase a screen e recria-los on wake, o que
// e obra grande e arriscada (o M5GFX e owner desses objetos). Nao tente pelo
// caminho barato: ele nao existe.

// Entra em sleep: apaga a screen, para o fluxo of video e low o ceiling of
// clock. Devolve false se algum step falhar (e nesse caso desfaz o que deu).
//
// cpu_mhz: steps valid_entries nesta revisao of silicio sao 360, 180, 90 e XTAL/N.
//
// ATENCAO: nao entre em sleep com o radio on sem testar. O driver SDIO of
// C6 tem exigencias proprias of banda, e como a trava of PM prende no MAXIMO,
// baixar o ceiling low o clock para ele tambem.
bool tab5_power_sleep(int cpu_mhz = 90);

// Sai of sleep, NESTA ORDEM: sobe o ceiling of clock, CONFIRMA que a frequencia
// subiu of fato, so entao religa o fluxo of video e acorda a screen.
//
// A confirmacao e o intertravamento: religar o fluxo ainda em clock bottom
// traria of iteration a tempestade of underrun. Se a frequencia nao start dentro of
// prazo, esta funcao devolve false e DEIXA A TELA APAGADA of proposito - falhar
// escuro e melhor que falhar com a imagem rasgando.
bool tab5_power_resume(void);

// Acende a screen em rampa, ate o brightness anterior.
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

// Gancho chamado a cada degrau of rampa of brightness of wake (a rampa bloqueia
// a tarefa por ~300 ms; sem o gancho, qualquer coisa animada in screen stays
// congelada nesse time). Rodar in tarefa of interface. nullptr = nenhum.
void tab5_power_ramp_hook(void (*step)(void));

// ---------------------------------------------------------------------------
// QUAL TELA ESTE TAB5 TEM
// ---------------------------------------------------------------------------
// O Tab5 exited com tres telas (todas 720x1280 MIPI-DSI, mesma placa):
//   ILI9881C + GT911 - o lancamento (2025): panel e touch em chips separados;
//   ST7123           - since out/2025: panel e touch no MESMO chip (TDDI);
//   ST7121           - since abr/2026: outro TDDI of mesma familia.
// O M5GFX descobre sozinho no M5.begin() e monta o driver certo; o drawing e o
// touch comuns (M5.Display, M5.Touch) funcionam igual nas tres. O que NAO
// funciona igual e o que este sistema faz por bottom of M5GFX, e cada point
// desses pergunta aqui antes:
//   - o vigia of touch le registradores of GT911 direto (kernel/core.cpp);
//   - o sleep fundo (SLPIN + unmount o DSI) - num TDDI o touch e varrido
//     junto com a imagem, entao com o panel dormindo o touch provavelmente
//     morre, e o touch e o que acorda a screen. Nos ST o sleep so apaga a luz;
//   - o clock of pixel (tab5_power_dpi_clock): os porticos que o M5GFX
//     usa nos ST deixam a varredura of touch in folga vertical, e mexer no
//     clock muda essa folga. Nos ST ele stays no of fabrica.
// Os caminhos dos ST foram escritos sem aparelho para testar: sao os
// CONSERVADORES (o que o M5GFX ja faz sozinho), nunca one palpite novo.
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
// true so no ILI9881C: o touch e one GT911 separado (vigia direto, pulso no
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
// Devolve false (e iteration sozinho on value anterior) se o panel nao start.
bool tab5_power_dpi_clock(int mhz);

// Relogio current, em MHz; 0 se nao ha panel.
int  tab5_power_dpi_clock(void);

// Quadros por segundo no clock current, pelos porticos que o M5GFX programou
// NESTA screen (cada uma tem os seus). 0 se nao ha panel.
double tab5_power_frames_per_second(void);

// ---------------------------------------------------------------------------
// TETO DO CPU COM A TELA ACORDADA - diagnostics, criado
// ---------------------------------------------------------------------------
// Muda SO o ceiling of clock of CPU, em execucao, SEM entrar em sleep e SEM
// unmount o panel - diferente of tab5_power_sleep(), que apaga a screen
// por complete. Existe para testar uma pergunta especifica: one clock of
// panel mais bottom (ver tab5_power_dpi_clock acima) abre space para one
// ceiling of CPU menor, com a screen LIGADA e desenhando normalmente, sem underrun?
//
// Ninguem tinha testado essa combinacao ainda - so "CPU bottom com o panel no
// clock of sempre" (deu underrun) e "CPU bottom com o panel
// desmontado" (o sleep, que sempre funcionou porque nao ha mais panel para
// dar underrun). Esta funcao permite o terceiro caso: os dois LIGADOS e os
// dois devagar, on mesmo time.
//
// Valores valid_entries nesta revisao of silicio: 360, 180, 90 (PLL) ou XTAL/N -
// 40, 20, 10, 8, 5, 4, 2, 1. Devolve false e nao muda nada se o IDF recusar.
bool tab5_power_cpu_ceiling(int mhz);

// true enquanto estiver em sleep.
bool tab5_power_is_sleeping(void);
