// Perfil de energia do M5Tab5.
//
// A ideia: TUDO nasce desligado, e cada projeto liga estritamente o que usa.
// O Tab5 sai do M5.begin() com camera fora do reset, IMU com giroscopio em
// performance maxima, radio alimentado e os dois trilhos de 5V (Grove e USB-A)
// ligados por pull-up nos expansores - nada disso e pedido, e some da conta de
// bateria sem aparecer no code.
//
// O profile e aplicado em DOIS momentos, e eles nao sao intercambiaveis:
//
//   fase 1  tab5_power_config()   ANTES do M5.begin()
//           Mexe nas flags do M5.config(). O periferico nunca chega a ser
//           inicializado - fica no sleep de fabrica, sem trafego I2C.
//
//   fase 2  tab5_power_apply()  DEPOIS do M5.begin()
//           Mexe no que o M5Unified liga incondicionalmente e nao expoe:
//           reset da camera, trilho do radio, brightness, gerenciamento de CPU.
//
// Uso tipico:
//
//   tab5_profile_t profile;          // tudo desligado
//   profile.radio  = true;          // este projeto imprime por WiFi
//   profile.brightness = 80;
//
//   auto cfg = M5.config();
//   tab5_power_config(cfg, profile);
//   M5.begin(cfg);
//   tab5_power_apply(profile);

#pragma once

#include "sdkconfig.h"

// CLOCK MAXIMO DO CPU - depende da revisao do P4 (sdkconfig.defaults.chip_*):
// 360 MHz no v1.x, 400 no v3.x. Os steps que o esp_pm aceita sao os do PLL
// (o maximo, a half, um quarto) e os do cristal de 40 MHz dividido.
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

  // BMI270 (acelerometro + giroscopio). ATENCAO: quando on, o M5Unified o
  // deixa com giro active e advanced power save DESLIGADO (PWR_CONF=0x00) - o
  // pior caso de consumo do chip. Se voce so precisa de orientacao, ligue este
  // campo e chame tab5_power_imu_accelerometer_only() depois do begin.
  bool imu           = false;

  // ES7210: os microfones. Ligado = codec ADC alimentado.
  bool microphone     = false;

  // ES8388: o codec de output + SPK_EN no expansor 0.
  bool speaker  = false;

  // RX8130. Custa quase nada em corrente (dominio proprio); turn_off aqui
  // economiza time de boot e trafego I2C, nao bateria.
  bool rtc           = true;

  // EXT5V_EN, expansor 0 bit 2. Alimenta a porta Grove.
  bool grove_5v      = false;

  // USB5V_EN, expansor 1 bit 3. Alimenta a USB-A embutida.
  bool usb_a_5v      = false;

  // ---- fase 2: so da para mexer com a placa de pe --------------------------

  // CAM_RST, expansor 0 bit 6. O M5Unified tira a camera do reset no boot.
  bool camera        = false;

  // WLAN_PWR_EN, expansor 1 bit 0. E o trilho do ESP32-C6 inteiro: WiFi E BLE
  // saem por ele. Cortar e a maior economia depois do backlight.
  // LEIA o comentario de tab5_power_radio() antes de religar em runtime.
  bool radio         = false;

  // 0..255. O backlight (PWM em GPIO22) e o maior consumidor da placa com a
  // tela acesa, e a resposta e quase linear: 80 gasta ~1/3 de 255.
  //
  // NO M5tabOS ESTE CAMPO NAO E DO APP. O sistema o sobrescreve com a
  // preferencia do usuario em sistema_impor_preferencias() - brightness e de who
  // segura o aparelho, nao do programa que esta na tela. Descritor de app que
  // define brightness engana who le: o value nunca chega ao hardware.
  //
  // O campo continua existindo porque o tab5_power e AUTOCONTIDO e copiavel
  // para projetos sem sistema de apps, onde set brightness aqui faz sentido.
  uint8_t brightness     = 64;

  // ---- fase 2: gerenciamento de energia da CPU ----------------------------

  // Liga o DFS do IDF: a CPU cai sozinha para cpu_min_mhz quando nenhuma
  // tarefa esta pronta. Exige CONFIG_PM_ENABLE=y no sdkconfig - sem isso o
  // esp_pm_configure() devolve ESP_ERR_NOT_SUPPORTED (era a trava do Arduino).
  bool pm_dinamico   = true;

  // O P4 do Tab5 e revisao < 3.0 (ver o bloco de REVISAO DO SILICIO no
  // sdkconfig.defaults). Nessa familia as frequencias validas sao
  // 360, 180, 90 e XTAL/N (40, 20, 10, 8, 5, 4, 2, 1) - a escada 400/200/100
  // e da revisao >= 3.0 e NAO vale aqui. Qualquer outro value faz o
  // esp_pm_configure return_card ESP_ERR_INVALID_ARG.
  //
  // TESTADO: ceiling baixado para 180 COM A TELA LIGADA (nao so no
  // sleep) deu erro na pratica - de volta a 360. Ou seja, mesmo com a
  // prioridade no AXI-ICM ja ativa (tab5_bus_prioritize_screen), o panel
  // DSI ainda precisa do CPU no ceiling pra sustentar o frame sem underrun. A
  // trava de PM que ele segura (dsi_dpi, CPU_FREQ_MAX) nao e conservadorismo
  // do driver - e uma dependencia real deste hardware. Nao ha degrau
  // intermediario para tentar (300 nao existe nesta escada - so 360/180/90
  // vem do PLL); o proximo candidato, se algum dia valer a pena reabrir isto,
  // e measure com um clock de panel MAIS BAIXO que 60MHz (menos banda
  // exigida) e ver se 180 passa a bastar - nao tentado ainda.
  int  cpu_max_mhz   = TAB5_CPU_MAX_MHZ;
  int  cpu_min_mhz   = 40;

  // Light sleep automatico no idle.
  //
  // HOJE ESTE CAMPO NAO FAZ NADA, e e importante saber disso antes de tentar:
  // CONFIG_FREERTOS_USE_TICKLESS_IDLE esta DESLIGADO no sdkconfig (conferido em
  // ). Sem tickless o idle acorda a cada tick e nunca chega a dormir,
  // entao passar true aqui nao da erro nenhum - simplesmente nao acontece nada,
  // que e o pior type de failure para depurar.
  //
  // Ligar tickless e uma decisao de medicao, nao de gosto: com FREERTOS_HZ=1000
  // o ganho depende de haver janelas ociosas de varios milissegundos, e o panel
  // DPI pode nem permitir o sono. Medir com tab5_power_locks() antes.
  bool light_sleep_when_idle = false;
};

// ---- aplicacao do profile --------------------------------------------------

// Fase 1. Chame com o cfg do M5.config(), antes do M5.begin().
void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_profile_t& profile);

// Fase 2. Chame logo depois do M5.begin().
void tab5_power_apply(const tab5_profile_t& profile);

// ---- controles de runtime -------------------------------------------------

// Corta ou devolve o trilho do ESP32-C6.
//
// ATENCAO: turn_off derruba WiFi e BLE juntos e reinicia o C6. Religar devolve
// a alimentacao, mas a stack do host (esp_hosted/esp_wifi) NAO se reconecta
// sozinha - o SDIO precisa ser renegociado. Enquanto a gente nao escrever essa
// sequencia, trate o desligamento como caminho de mao unica dentro de um boot:
// desligue quando terminar de usar o radio e va dormir, ou reinicie para
// religar.
void tab5_power_radio(bool turn_on);


// Trilho de 5V do Grove (expansor 0, PortA). Ligar em runtime e necessario para
// SONDAR o que esta plugado antes de saber qual app vai usar - o modulo precisa
// de energia para se apresentar. Ver tab5_grove.
void tab5_power_grove(bool turn_on);

// Trilho ja esta on AGORA? Existe para tab5_grove.cpp decidir se precisa
// esperar o modulo bootar antes de sondar - pergunta de ESTADO DE HARDWARE,
// nao de preferencia do usuario (por isso mora aqui, e nao em settings.h: HAL
// pergunta a HAL, nunca a um service - ver WRITING_APPS.md secao 8).
bool tab5_power_grove_on(void);

// Quando o Tab5 mexeu no 5V do Grove / reaplicou o profile pela latest vez
// (esp_timer em ms, 0 = nunca). Prova ou descarta o Tab5 num reinicio do no.
uint32_t tab5_power_grove_touch_ms(void);
uint32_t tab5_power_apply_ms(void);

// CAM_RST. false = camera segurada em reset.
void tab5_power_camera(bool turn_on);

// Poe o BMI270 so com acelerometro e com advanced power save on.
// Devolve false se nao achou o chip (ou se profile.imu era false).
bool tab5_power_imu_accelerometer_only(void);

// O profile de boot pediu a IMU (cfg.internal_imu). Para who precisa saber se
// uma IMU ausente e escolha ou failure - ver tab5_imu_measure().
bool tab5_power_imu_requested(void);

// ---- history curto, para grafico ----------------------------------------
//
// Fica AQUI, e nao no app, porque o tab5_power_measure() roda uma vez por segundo
// pela tarefa de interface MESMO EM REPOUSO. Assim o history se enche com o
// app fechado e o grafico ja nasce cheio ao abrir - se morasse no app, ele
// comecaria vazio toda vez.
//
// 60 pontos a cada 30s = 30 minutos. Custa 60 * 12 = 720 bytes.
#define TAB5_HIST_N     60
#define TAB5_HIST_SEG   30

// A temperatura mora aqui e nao num anel proprio porque e o MESMO instante: os
// dois sao lidos no mesmo tique de 1s e desenhados no mesmo eixo de time. Dois
// aneis separados dariam o trabalho de casa-los depois, e a chance de eles
// escorregarem um do outro. Vale -273 quando nao houve leitura.
typedef struct { float volts; float mA; float temp; int16_t pct; } tab5_point_t;   // pct = a % do sistema naquele instante

// Copia o history em ordem cronologica (mais antigo primeiro) para 'output',
// que precisa ter TAB5_HIST_N posicoes. Devolve quantos pontos existem.
int tab5_power_history(tab5_point_t* output);

// Porcentagem PELA CURVA DE TENSAO, pura (tensao corrigida pela corrente).
// NAO e o que o sistema mostra: a % do sistema vem do estimador em
// tab5_power_measure() (contagem de mAh + curva como correcao lenta). Exposta
// para diagnostico e para reprocessar arquivos antigos.
int tab5_power_pct(float volts, float mA);

// Estado do estimador, para o console: % com fracao, o que a curva diria
// agora, de onde o estimador partiu, e minutos desde a latest carga (-1 =
// nenhuma neste boot).
void tab5_power_debug_soc(float* soc, float* curva, const char** fonte, float* min_desde_carga);

// ---- medicao (INA226 embutido, shunt de 5 mOhm no barramento da bateria) ---

// RESISTENCIA INTERNA do conjunto (bateria + fiacao + shunt), em ohms.
//
// Serve para estimar a tensao EM REPOUSO a partir da tensao measurement sob carga:
//     V_aberto = V_medido - I * R      (I com signal: + carregando, - descarregando)
//
// A formula vale nos dois sentidos, e por isso ela e uma so: descarregando, a
// tensao afunda e a correcao SOMA; carregando, a tensao esta inflada pelo
// carregador e a correcao SUBTRAI.
//
// MEDIDO, nao mais estimado., a partir do energia.csv real:
//
//   39 steps de corrente maiores que 150mA numa descarga de 10,3h
//   R = dV/dI  ->  mediana 0.268 ohm, faixa 0.179..0.312
//
// Era 0.15 por estimativa, quase half do value real - entao a correcao de
// I*R era menor que deveria e a porcentagem caia quando a tela acendia, que e
// exatamente o sintoma que esta constante existe para eliminar.
//
// Degraus grandes de proposito: a tensao e lida com resolucao de 1,25mV, e a
// 20mA de diferenca o dV se perde no ruido. Sao as transicoes de sleep que
// dao o degrau clean.
#define TAB5_INTERNAL_R 0.268f

// Capacidade measurement da bateria, em mAh - SUBSTITUIDA pela
// analise da descarga de 21,5h SEM EMENDA achada no energia.csv (a de
// 1448mAh, vinha de duas sessoes coladas + tensao com só 2 casas
// ver o comentario grande junto de CURVA[] em tab5_power.cpp para o metodo e
// para o porque a label da bateria dizer "2400mAh" nao bate com isto).
// Usada pelo estimador de carga (tab5_power.cpp, estimar_pct)
// e exposta aqui, num lugar so, para o console poder mostrar "quantos % da
// capacity" sem guardar uma segunda copia do mesmo numero.
#define TAB5_CAPACITY_MAH 1306.8f

struct tab5_measurement_t
{
  float volts;   // barramento (2S Li-Po, ~7.4V nominal)
  float mA;      // (+) carregando / (-) descarregando
  float mW;      // potencia no barramento da bateria
  int   pct;     // carga estimada 0..100 (negativo = o modelo nao sabe dizer)
};

// ATENCAO - CONCORRENCIA: estas leituras vao ao INA226 pelo I2C interno, e o
// I2C_Class do M5Unified NAO tem mutex nenhum (conferido no fonte). Se outra
// tarefa usar o mesmo barramento ao mesmo time - e M5.update() usa, para read
// touch e botoes - as duas transacoes se intercalam e voce le REGISTRADOR
// ERRADO, sem erro nenhum, so numero sem sentido. Chame tudo daqui a partir da
// MESMA tarefa que chama M5.update(), ou proteja o barramento com um mutex
// seu que envolva os dois.
tab5_measurement_t tab5_power_measure(void);

// A ULTIMA measurement feita, sem tocar no I2C. E o que outras tarefas devem usar.
//
// Regra do projeto: so a tarefa de interface fala no barramento I2C, porque o
// I2C_Class do M5Unified nao tem mutex e duas tarefas intercalando leem
// registrador trocado sem dar erro. Quem nao for a interface le daqui.
// A interface atualiza este value a cada segundo, acordada ou em sleep.
tab5_measurement_t tab5_power_latest(void);

// Frequencia de operacao da CPU NESTE INSTANTE, em MHz. Com o DFS on
// (pm_dinamico) ela muda em runtime, entao leia a cada atualizacao em vez de
// guardar o value uma vez no boot.
int tab5_power_cpu_mhz(void);

// Imprime uma linha de log com a measurement atual, marcada com a label dada.
// Use antes/depois de cada corte para saber quanto ele valeu.
void tab5_power_log(const char* label);


// Lista as TRAVAS DE PM ativas no console.
//
// Existe para encerrar por medicao, e nao por deducao, a pergunta que atrasa
// toda a economia de energia deste projeto: QUEM prende a CPU no maximo? Ate
// hoje a resposta era inferida (o panel DPI puxa a PSRAM sem stop, logo deve
// ser ele). O esp_pm_dump_locks() diz o name de cada trava e quantas vezes foi
// adquirida - se o panel aparecer com ESP_PM_CPU_FREQ_MAX, esta provado; se
// aparecer outro owner, a gente estava caçando o alvo errado.
//
// Chamar em pontos diferentes (boot, com app aberto, em sleep) e comparar as
// listas mostra o que solta e o que nunca solta.
void tab5_power_locks(const char* label);

// ---- mode de espera (tela apagada + clock baixo) ---------------------------
//
// O PORQUE: baixar o clock com o panel active NAO funciona - foi medido em
// e o DSI passou a dar underrun em todo frame. O panel DPI
// transmite o framebuffer da PSRAM continuamente, e a 180MHz o barramento nao
// alimenta ele a time. O M5.Display.sleep() NAO resolve isso: o setSleep() do
// M5GFX so manda o comando DCS 0x10 (SLPIN) para o panel; o controlador DPI
// continua puxando ~1.8MB por frame da PSRAM para um panel que ignora.
//
// A output e stop o proprio fluxo com esp_lcd_dpi_panel_set_pattern(): o fonte
// do IDF diz "stop the DSI bridge from generating the DPI stream" quando o
// padrao e diferente de NONE. Com o fetch parado, nao ha mais underrun possivel
// e o clock pode cair.
//
// Repare que sao DOIS ganhos empilhados, e provavelmente o primeiro vale mais:
// stop o streaming elimina trafego continuo de PSRAM que existe hoje mesmo com
// a tela "dormindo"; baixar o clock e o bonus que so fica disponivel depois.
//
// -------------------------------------------------------------------------
// MEDIDO com tab5_power_locks() - leia antes de mexer aqui
// -------------------------------------------------------------------------
// O esp_pm_dump_locks() no boot devolve:
//
//     dsi_dpi   CPU_FREQ_MAX     active 1
//     dsi_phy   NO_LIGHT_SLEEP   active 1
//
// Confirma a deducao sobre o clock: who prende a CPU e o panel, nao o
// M5Unified nem a revisao do silicio. Mas acrescenta um fato NOVO e trouxe
// junto um limite que a gente nao conhecia:
//
// 1. A trava do clock NAO impede baixar o ceiling. ESP_PM_CPU_FREQ_MAX prende a
//    CPU no value de max_freq_mhz, e esse value e ours - por isso o sleep
//    com ajustar_teto(90) funciona. O que quebra a 90MHz e o underrun, nao a
//    trava. A estrategia acima continua valendo.
//
// 2. LIGHT SLEEP E IMPOSSIVEL enquanto o barramento DSI existir, e nenhum
//    set_pattern muda isso. Verificado no fonte do IDF: em
//    esp_lcd_panel_dpi.c a trava e adquirida na CRIACAO do panel (linha 291)
//    e so liberada dentro do destrutor, junto do free() (linhas 405-406); em
//    esp_lcd_mipi_dsi_bus.c e igual (72 -> 151). O set_pattern() nao tem uma
//    unica referencia a pm_lock.
//
// Consequencia pratica: turn_on CONFIG_FREERTOS_USE_TICKLESS_IDLE hoje daria
// ZERO - o sistema nunca chegaria a dormir. O unico caminho para light sleep e
// DESTRUIR panel e barramento ao erase a tela e recria-los ao acordar, o que
// e obra grande e arriscada (o M5GFX e owner desses objetos). Nao tente pelo
// caminho barato: ele nao existe.

// Entra em sleep: apaga a tela, para o fluxo de video e low o ceiling de
// clock. Devolve false se algum step falhar (e nesse caso desfaz o que deu).
//
// cpu_mhz: steps valid_entries nesta revisao de silicio sao 360, 180, 90 e XTAL/N.
//
// ATENCAO: nao entre em sleep com o radio on sem testar. O driver SDIO do
// C6 tem exigencias proprias de banda, e como a trava de PM prende no MAXIMO,
// baixar o ceiling low o clock para ele tambem.
bool tab5_power_sleep(int cpu_mhz = 90);

// Sai do sleep, NESTA ORDEM: sobe o ceiling de clock, CONFIRMA que a frequencia
// subiu de fato, so entao religa o fluxo de video e acorda a tela.
//
// A confirmacao e o intertravamento: religar o fluxo ainda em clock baixo
// traria de volta a tempestade de underrun. Se a frequencia nao start dentro do
// prazo, esta funcao devolve false e DEIXA A TELA APAGADA de proposito - falhar
// escuro e melhor que falhar com a imagem rasgando.
bool tab5_power_resume(void);

// Acende a tela em rampa, ate o brightness anterior.
//
// SEPARADA do resume() de proposito: o resume() devolve o panel ao ar mas
// DEIXA A LUZ APAGADA, para who chamou redesenhar a tela antes de qualquer
// pixel ficar visivel. Sem essa separacao, a luz sobe mostrando o framebuffer
// velho ou o lixo da ressincronizacao da ponte DSI - o "artefato" ao acordar.
//
//   tab5_power_resume();   // volta clock, fluxo e panel, tela ainda escura
//   ...redesenhe tudo...
//   tab5_power_wake_screen();   // agora sim
void tab5_power_wake_screen(void);

// Gancho chamado a cada degrau da rampa de brightness do wake (a rampa bloqueia
// a tarefa por ~300 ms; sem o gancho, qualquer coisa animada na tela fica
// congelada nesse time). Rodar na tarefa de interface. nullptr = nenhum.
void tab5_power_ramp_hook(void (*step)(void));

// ---------------------------------------------------------------------------
// QUAL TELA ESTE TAB5 TEM
// ---------------------------------------------------------------------------
// O Tab5 saiu com tres telas (todas 720x1280 MIPI-DSI, mesma placa):
//   ILI9881C + GT911 - o lancamento (2025): panel e toque em chips separados;
//   ST7123           - desde out/2025: panel e toque no MESMO chip (TDDI);
//   ST7121           - desde abr/2026: outro TDDI da mesma familia.
// O M5GFX descobre sozinho no M5.begin() e monta o driver certo; o desenho e o
// toque comuns (M5.Display, M5.Touch) funcionam igual nas tres. O que NAO
// funciona igual e o que este sistema faz por baixo do M5GFX, e cada point
// desses pergunta aqui antes:
//   - o vigia do toque le registradores do GT911 direto (kernel/core.cpp);
//   - o sleep fundo (SLPIN + unmount o DSI) - num TDDI o toque e varrido
//     junto com a imagem, entao com o panel dormindo o toque provavelmente
//     morre, e o toque e o que acorda a tela. Nos ST o sleep so apaga a luz;
//   - o clock de pixel (tab5_power_dpi_clock): os porticos que o M5GFX
//     usa nos ST deixam a varredura do toque na folga vertical, e mexer no
//     clock muda essa folga. Nos ST ele fica no de fabrica.
// Os caminhos dos ST foram escritos sem aparelho para testar: sao os
// CONSERVADORES (o que o M5GFX ja faz sozinho), nunca um palpite novo.
//
// Detectado uma vez, na primeira chamada - que tem que ser DEPOIS do
// M5.begin() e ANTES de qualquer troca de clock (main.cpp chama logo apos
// o begin). Qualquer tarefa depois disso.
typedef enum {
  TAB5_PANEL_UNKNOWN = 0,
  TAB5_PANEL_ILI9881C,
  TAB5_PANEL_ST7123,
  TAB5_PANEL_ST7121,
} tab5_panel_t;
tab5_panel_t tab5_panel(void);
const char*   tab5_panel_name(void);
// true so no ILI9881C: o toque e um GT911 separado (vigia direto, pulso no
// GPIO 23 confirmado, sleep fundo e clock ajustavel validados nele).
bool          tab5_panel_touch_gt911(void);

// ---------------------------------------------------------------------------
// RELOGIO DE PIXEL DO PAINEL
// ---------------------------------------------------------------------------
// O panel DPI le o framebuffer da PSRAM sem stop, no ritmo deste clock. A
// 80MHz (o que o M5GFX programa para o ILI9881C) sao 64 frames por segundo e
// 118,5 MB/s de leitura CONTINUA. A 60MHz sao 48 frames e 88,9 MB/s.
//
// Esses 25% a menos sao a unica coisa que reduz a demanda do panel de forma
// permanente - todo o resto do sistema (fatiar limpeza, evitar clear duas
// vezes, espacar rajadas) mexe no OUTRO lado da disputa. Ver a explicacao
// completa em tab5_power.cpp, junto da implementacao.
//
// Trocar o clock DESMONTA E REMONTA o panel - o mesmo caminho do despertar.
// Devolve false (e volta sozinho ao value anterior) se o panel nao start.
bool tab5_power_dpi_clock(int mhz);

// Relogio atual, em MHz; 0 se nao ha panel.
int  tab5_power_dpi_clock(void);

// Quadros por segundo no clock atual, pelos porticos que o M5GFX programou
// NESTA tela (cada uma tem os seus). 0 se nao ha panel.
double tab5_power_frames_per_second(void);

// ---------------------------------------------------------------------------
// TETO DO CPU COM A TELA ACORDADA - diagnostico, criado
// ---------------------------------------------------------------------------
// Muda SO o ceiling de clock do CPU, em execucao, SEM entrar em sleep e SEM
// unmount o panel - diferente de tab5_power_sleep(), que apaga a tela
// por complete. Existe para testar uma pergunta especifica: um clock de
// panel mais baixo (ver tab5_power_dpi_clock acima) abre space para um
// ceiling de CPU menor, com a tela LIGADA e desenhando normalmente, sem underrun?
//
// Ninguem tinha testado essa combinacao ainda - so "CPU baixo com o panel no
// clock de sempre" (deu underrun) e "CPU baixo com o panel
// desmontado" (o sleep, que sempre funcionou porque nao ha mais panel para
// dar underrun). Esta funcao permite o terceiro caso: os dois LIGADOS e os
// dois devagar, ao mesmo time.
//
// Valores valid_entries nesta revisao de silicio: 360, 180, 90 (PLL) ou XTAL/N -
// 40, 20, 10, 8, 5, 4, 2, 1. Devolve false e nao muda nada se o IDF recusar.
bool tab5_power_cpu_ceiling(int mhz);

// true enquanto estiver em sleep.
bool tab5_power_is_sleeping(void);
