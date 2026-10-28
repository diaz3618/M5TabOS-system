// Perfil de energia do M5Tab5.
//
// A ideia: TUDO nasce desligado, e cada projeto liga estritamente o que usa.
// O Tab5 sai do M5.begin() com camera fora do reset, IMU com giroscopio em
// performance maxima, radio alimentado e os dois trilhos de 5V (Grove e USB-A)
// ligados por pull-up nos expansores - nada disso e pedido, e some da conta de
// bateria sem aparecer no codigo.
//
// O perfil e aplicado em DOIS momentos, e eles nao sao intercambiaveis:
//
//   fase 1  tab5_power_config()   ANTES do M5.begin()
//           Mexe nas flags do M5.config(). O periferico nunca chega a ser
//           inicializado - fica no repouso de fabrica, sem trafego I2C.
//
//   fase 2  tab5_power_aplicar()  DEPOIS do M5.begin()
//           Mexe no que o M5Unified liga incondicionalmente e nao expoe:
//           reset da camera, trilho do radio, brilho, gerenciamento de CPU.
//
// Uso tipico:
//
//   tab5_perfil_t perfil;          // tudo desligado
//   perfil.radio  = true;          // este projeto imprime por WiFi
//   perfil.brilho = 80;
//
//   auto cfg = M5.config();
//   tab5_power_config(cfg, perfil);
//   M5.begin(cfg);
//   tab5_power_aplicar(perfil);

#pragma once

#include "sdkconfig.h"

// CLOCK MAXIMO DO CPU - depende da revisao do P4 (sdkconfig.defaults.chip_*):
// 360 MHz no v1.x, 400 no v3.x. Os degraus que o esp_pm aceita sao os do PLL
// (o maximo, a metade, um quarto) e os do cristal de 40 MHz dividido.
#define TAB5_CPU_MAX_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define TAB5_CPU_DEGRAUS "360 180 90 40 20 10 8 5 4 2 1"
#define TAB5_CPU_METADE  "180"
#else
#define TAB5_CPU_DEGRAUS "400 200 100 40 20 10 8 5 4 2 1"
#define TAB5_CPU_METADE  "200"
#endif

#include <M5Unified.h>
#include <stdint.h>

struct tab5_perfil_t
{
  // ---- fase 1: nem chega a ser inicializado -------------------------------

  // BMI270 (acelerometro + giroscopio). ATENCAO: quando ligado, o M5Unified o
  // deixa com giro ativo e advanced power save DESLIGADO (PWR_CONF=0x00) - o
  // pior caso de consumo do chip. Se voce so precisa de orientacao, ligue este
  // campo e chame tab5_power_imu_so_acelerometro() depois do begin.
  bool imu           = false;

  // ES7210: os microfones. Ligado = codec ADC alimentado.
  bool microfone     = false;

  // ES8388: o codec de saida + SPK_EN no expansor 0.
  bool alto_falante  = false;

  // RX8130. Custa quase nada em corrente (dominio proprio); desligar aqui
  // economiza tempo de boot e trafego I2C, nao bateria.
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
  // preferencia do usuario em sistema_impor_preferencias() - brilho e de quem
  // segura o aparelho, nao do programa que esta na tela. Descritor de app que
  // define brilho engana quem le: o valor nunca chega ao hardware.
  //
  // O campo continua existindo porque o tab5_power e AUTOCONTIDO e copiavel
  // para projetos sem sistema de apps, onde definir brilho aqui faz sentido.
  uint8_t brilho     = 64;

  // ---- fase 2: gerenciamento de energia da CPU ----------------------------

  // Liga o DFS do IDF: a CPU cai sozinha para cpu_min_mhz quando nenhuma
  // tarefa esta pronta. Exige CONFIG_PM_ENABLE=y no sdkconfig - sem isso o
  // esp_pm_configure() devolve ESP_ERR_NOT_SUPPORTED (era a trava do Arduino).
  bool pm_dinamico   = true;

  // O P4 do Tab5 e revisao < 3.0 (ver o bloco de REVISAO DO SILICIO no
  // sdkconfig.defaults). Nessa familia as frequencias validas sao
  // 360, 180, 90 e XTAL/N (40, 20, 10, 8, 5, 4, 2, 1) - a escada 400/200/100
  // e da revisao >= 3.0 e NAO vale aqui. Qualquer outro valor faz o
  // esp_pm_configure devolver ESP_ERR_INVALID_ARG.
  //
  // TESTADO: teto baixado para 180 COM A TELA LIGADA (nao so no
  // repouso) deu erro na pratica - de volta a 360. Ou seja, mesmo com a
  // prioridade no AXI-ICM ja ativa (tab5_barramento_priorizar_tela), o painel
  // DSI ainda precisa do CPU no teto pra sustentar o frame sem underrun. A
  // trava de PM que ele segura (dsi_dpi, CPU_FREQ_MAX) nao e conservadorismo
  // do driver - e uma dependencia real deste hardware. Nao ha degrau
  // intermediario para tentar (300 nao existe nesta escada - so 360/180/90
  // vem do PLL); o proximo candidato, se algum dia valer a pena reabrir isto,
  // e medir com um relogio de painel MAIS BAIXO que 60MHz (menos banda
  // exigida) e ver se 180 passa a bastar - nao tentado ainda.
  int  cpu_max_mhz   = TAB5_CPU_MAX_MHZ;
  int  cpu_min_mhz   = 40;

  // Light sleep automatico no idle.
  //
  // HOJE ESTE CAMPO NAO FAZ NADA, e e importante saber disso antes de tentar:
  // CONFIG_FREERTOS_USE_TICKLESS_IDLE esta DESLIGADO no sdkconfig (conferido em
  // ). Sem tickless o idle acorda a cada tick e nunca chega a dormir,
  // entao passar true aqui nao da erro nenhum - simplesmente nao acontece nada,
  // que e o pior tipo de falha para depurar.
  //
  // Ligar tickless e uma decisao de medicao, nao de gosto: com FREERTOS_HZ=1000
  // o ganho depende de haver janelas ociosas de varios milissegundos, e o painel
  // DPI pode nem permitir o sono. Medir com tab5_power_travas() antes.
  bool light_sleep_ocioso = false;
};

// ---- aplicacao do perfil --------------------------------------------------

// Fase 1. Chame com o cfg do M5.config(), antes do M5.begin().
void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_perfil_t& perfil);

// Fase 2. Chame logo depois do M5.begin().
void tab5_power_aplicar(const tab5_perfil_t& perfil);

// ---- controles de runtime -------------------------------------------------

// Corta ou devolve o trilho do ESP32-C6.
//
// ATENCAO: desligar derruba WiFi e BLE juntos e reinicia o C6. Religar devolve
// a alimentacao, mas a stack do host (esp_hosted/esp_wifi) NAO se reconecta
// sozinha - o SDIO precisa ser renegociado. Enquanto a gente nao escrever essa
// sequencia, trate o desligamento como caminho de mao unica dentro de um boot:
// desligue quando terminar de usar o radio e va dormir, ou reinicie para
// religar.
void tab5_power_radio(bool ligar);


// Trilho de 5V do Grove (expansor 0, PortA). Ligar em runtime e necessario para
// SONDAR o que esta plugado antes de saber qual app vai usar - o modulo precisa
// de energia para se apresentar. Ver tab5_grove.
void tab5_power_grove(bool ligar);

// Trilho ja esta ligado AGORA? Existe para tab5_grove.cpp decidir se precisa
// esperar o modulo bootar antes de sondar - pergunta de ESTADO DE HARDWARE,
// nao de preferencia do usuario (por isso mora aqui, e nao em settings.h: HAL
// pergunta a HAL, nunca a um servico - ver WRITING_APPS.md secao 8).
bool tab5_power_grove_ligado(void);

// Quando o Tab5 mexeu no 5V do Grove / reaplicou o perfil pela ultima vez
// (esp_timer em ms, 0 = nunca). Prova ou descarta o Tab5 num reinicio do no.
uint32_t tab5_power_grove_toque_ms(void);
uint32_t tab5_power_aplicar_ms(void);

// CAM_RST. false = camera segurada em reset.
void tab5_power_camera(bool ligar);

// Poe o BMI270 so com acelerometro e com advanced power save ligado.
// Devolve false se nao achou o chip (ou se perfil.imu era false).
bool tab5_power_imu_so_acelerometro(void);

// O perfil de boot pediu a IMU (cfg.internal_imu). Para quem precisa saber se
// uma IMU ausente e escolha ou falha - ver tab5_imu_medir().
bool tab5_power_imu_pedida(void);

// ---- historico curto, para grafico ----------------------------------------
//
// Fica AQUI, e nao no app, porque o tab5_power_medir() roda uma vez por segundo
// pela tarefa de interface MESMO EM REPOUSO. Assim o historico se enche com o
// app fechado e o grafico ja nasce cheio ao abrir - se morasse no app, ele
// comecaria vazio toda vez.
//
// 60 pontos a cada 30s = 30 minutos. Custa 60 * 12 = 720 bytes.
#define TAB5_HIST_N     60
#define TAB5_HIST_SEG   30

// A temperatura mora aqui e nao num anel proprio porque e o MESMO instante: os
// dois sao lidos no mesmo tique de 1s e desenhados no mesmo eixo de tempo. Dois
// aneis separados dariam o trabalho de casa-los depois, e a chance de eles
// escorregarem um do outro. Vale -273 quando nao houve leitura.
typedef struct { float volts; float mA; float temp; int16_t pct; } tab5_ponto_t;   // pct = a % do sistema naquele instante

// Copia o historico em ordem cronologica (mais antigo primeiro) para 'saida',
// que precisa ter TAB5_HIST_N posicoes. Devolve quantos pontos existem.
int tab5_power_historico(tab5_ponto_t* saida);

// Porcentagem PELA CURVA DE TENSAO, pura (tensao corrigida pela corrente).
// NAO e o que o sistema mostra: a % do sistema vem do estimador em
// tab5_power_medir() (contagem de mAh + curva como correcao lenta). Exposta
// para diagnostico e para reprocessar arquivos antigos.
int tab5_power_pct(float volts, float mA);

// Estado do estimador, para o console: % com fracao, o que a curva diria
// agora, de onde o estimador partiu, e minutos desde a ultima carga (-1 =
// nenhuma neste boot).
void tab5_power_soc_debug(float* soc, float* curva, const char** fonte, float* min_desde_carga);

// ---- medicao (INA226 embutido, shunt de 5 mOhm no barramento da bateria) ---

// RESISTENCIA INTERNA do conjunto (bateria + fiacao + shunt), em ohms.
//
// Serve para estimar a tensao EM REPOUSO a partir da tensao medida sob carga:
//     V_aberto = V_medido - I * R      (I com sinal: + carregando, - descarregando)
//
// A formula vale nos dois sentidos, e por isso ela e uma so: descarregando, a
// tensao afunda e a correcao SOMA; carregando, a tensao esta inflada pelo
// carregador e a correcao SUBTRAI.
//
// MEDIDO, nao mais estimado., a partir do energia.csv real:
//
//   39 degraus de corrente maiores que 150mA numa descarga de 10,3h
//   R = dV/dI  ->  mediana 0.268 ohm, faixa 0.179..0.312
//
// Era 0.15 por estimativa, quase metade do valor real - entao a correcao de
// I*R era menor que deveria e a porcentagem caia quando a tela acendia, que e
// exatamente o sintoma que esta constante existe para eliminar.
//
// Degraus grandes de proposito: a tensao e lida com resolucao de 1,25mV, e a
// 20mA de diferenca o dV se perde no ruido. Sao as transicoes de repouso que
// dao o degrau limpo.
#define TAB5_R_INTERNA 0.268f

// Capacidade medida da bateria, em mAh - SUBSTITUIDA pela
// analise da descarga de 21,5h SEM EMENDA achada no energia.csv (a de
// 1448mAh, vinha de duas sessoes coladas + tensao com só 2 casas
// ver o comentario grande junto de CURVA[] em tab5_power.cpp para o metodo e
// para o porque a etiqueta da bateria dizer "2400mAh" nao bate com isto).
// Usada pelo estimador de carga (tab5_power.cpp, estimar_pct)
// e exposta aqui, num lugar so, para o console poder mostrar "quantos % da
// capacidade" sem guardar uma segunda copia do mesmo numero.
#define TAB5_CAPACIDADE_MAH 1306.8f

struct tab5_medida_t
{
  float volts;   // barramento (2S Li-Po, ~7.4V nominal)
  float mA;      // (+) carregando / (-) descarregando
  float mW;      // potencia no barramento da bateria
  int   pct;     // carga estimada 0..100 (negativo = o modelo nao sabe dizer)
};

// ATENCAO - CONCORRENCIA: estas leituras vao ao INA226 pelo I2C interno, e o
// I2C_Class do M5Unified NAO tem mutex nenhum (conferido no fonte). Se outra
// tarefa usar o mesmo barramento ao mesmo tempo - e M5.update() usa, para ler
// touch e botoes - as duas transacoes se intercalam e voce le REGISTRADOR
// ERRADO, sem erro nenhum, so numero sem sentido. Chame tudo daqui a partir da
// MESMA tarefa que chama M5.update(), ou proteja o barramento com um mutex
// seu que envolva os dois.
tab5_medida_t tab5_power_medir(void);

// A ULTIMA medida feita, sem tocar no I2C. E o que outras tarefas devem usar.
//
// Regra do projeto: so a tarefa de interface fala no barramento I2C, porque o
// I2C_Class do M5Unified nao tem mutex e duas tarefas intercalando leem
// registrador trocado sem dar erro. Quem nao for a interface le daqui.
// A interface atualiza este valor a cada segundo, acordada ou em repouso.
tab5_medida_t tab5_power_ultima(void);

// Frequencia de operacao da CPU NESTE INSTANTE, em MHz. Com o DFS ligado
// (pm_dinamico) ela muda em runtime, entao leia a cada atualizacao em vez de
// guardar o valor uma vez no boot.
int tab5_power_cpu_mhz(void);

// Imprime uma linha de log com a medida atual, marcada com a etiqueta dada.
// Use antes/depois de cada corte para saber quanto ele valeu.
void tab5_power_log(const char* etiqueta);


// Lista as TRAVAS DE PM ativas no console.
//
// Existe para encerrar por medicao, e nao por deducao, a pergunta que atrasa
// toda a economia de energia deste projeto: QUEM prende a CPU no maximo? Ate
// hoje a resposta era inferida (o painel DPI puxa a PSRAM sem parar, logo deve
// ser ele). O esp_pm_dump_locks() diz o nome de cada trava e quantas vezes foi
// adquirida - se o painel aparecer com ESP_PM_CPU_FREQ_MAX, esta provado; se
// aparecer outro dono, a gente estava caçando o alvo errado.
//
// Chamar em pontos diferentes (boot, com app aberto, em repouso) e comparar as
// listas mostra o que solta e o que nunca solta.
void tab5_power_travas(const char* etiqueta);

// ---- modo de espera (tela apagada + clock baixo) ---------------------------
//
// O PORQUE: baixar o clock com o painel ativo NAO funciona - foi medido em
// e o DSI passou a dar underrun em todo frame. O painel DPI
// transmite o framebuffer da PSRAM continuamente, e a 180MHz o barramento nao
// alimenta ele a tempo. O M5.Display.sleep() NAO resolve isso: o setSleep() do
// M5GFX so manda o comando DCS 0x10 (SLPIN) para o painel; o controlador DPI
// continua puxando ~1.8MB por frame da PSRAM para um painel que ignora.
//
// A saida e parar o proprio fluxo com esp_lcd_dpi_panel_set_pattern(): o fonte
// do IDF diz "stop the DSI bridge from generating the DPI stream" quando o
// padrao e diferente de NONE. Com o fetch parado, nao ha mais underrun possivel
// e o clock pode cair.
//
// Repare que sao DOIS ganhos empilhados, e provavelmente o primeiro vale mais:
// parar o streaming elimina trafego continuo de PSRAM que existe hoje mesmo com
// a tela "dormindo"; baixar o clock e o bonus que so fica disponivel depois.
//
// -------------------------------------------------------------------------
// MEDIDO com tab5_power_travas() - leia antes de mexer aqui
// -------------------------------------------------------------------------
// O esp_pm_dump_locks() no boot devolve:
//
//     dsi_dpi   CPU_FREQ_MAX     active 1
//     dsi_phy   NO_LIGHT_SLEEP   active 1
//
// Confirma a deducao sobre o clock: quem prende a CPU e o painel, nao o
// M5Unified nem a revisao do silicio. Mas acrescenta um fato NOVO e trouxe
// junto um limite que a gente nao conhecia:
//
// 1. A trava do clock NAO impede baixar o teto. ESP_PM_CPU_FREQ_MAX prende a
//    CPU no valor de max_freq_mhz, e esse valor e nosso - por isso o repouso
//    com ajustar_teto(90) funciona. O que quebra a 90MHz e o underrun, nao a
//    trava. A estrategia acima continua valendo.
//
// 2. LIGHT SLEEP E IMPOSSIVEL enquanto o barramento DSI existir, e nenhum
//    set_pattern muda isso. Verificado no fonte do IDF: em
//    esp_lcd_panel_dpi.c a trava e adquirida na CRIACAO do painel (linha 291)
//    e so liberada dentro do destrutor, junto do free() (linhas 405-406); em
//    esp_lcd_mipi_dsi_bus.c e igual (72 -> 151). O set_pattern() nao tem uma
//    unica referencia a pm_lock.
//
// Consequencia pratica: ligar CONFIG_FREERTOS_USE_TICKLESS_IDLE hoje daria
// ZERO - o sistema nunca chegaria a dormir. O unico caminho para light sleep e
// DESTRUIR painel e barramento ao apagar a tela e recria-los ao acordar, o que
// e obra grande e arriscada (o M5GFX e dono desses objetos). Nao tente pelo
// caminho barato: ele nao existe.

// Entra em repouso: apaga a tela, para o fluxo de video e baixa o teto de
// clock. Devolve false se algum passo falhar (e nesse caso desfaz o que deu).
//
// cpu_mhz: degraus validos nesta revisao de silicio sao 360, 180, 90 e XTAL/N.
//
// ATENCAO: nao entre em repouso com o radio ligado sem testar. O driver SDIO do
// C6 tem exigencias proprias de banda, e como a trava de PM prende no MAXIMO,
// baixar o teto baixa o clock para ele tambem.
bool tab5_power_repouso(int cpu_mhz = 90);

// Sai do repouso, NESTA ORDEM: sobe o teto de clock, CONFIRMA que a frequencia
// subiu de fato, so entao religa o fluxo de video e acorda a tela.
//
// A confirmacao e o intertravamento: religar o fluxo ainda em clock baixo
// traria de volta a tempestade de underrun. Se a frequencia nao subir dentro do
// prazo, esta funcao devolve false e DEIXA A TELA APAGADA de proposito - falhar
// escuro e melhor que falhar com a imagem rasgando.
bool tab5_power_retomar(void);

// Acende a tela em rampa, ate o brilho anterior.
//
// SEPARADA do retomar() de proposito: o retomar() devolve o painel ao ar mas
// DEIXA A LUZ APAGADA, para quem chamou redesenhar a tela antes de qualquer
// pixel ficar visivel. Sem essa separacao, a luz sobe mostrando o framebuffer
// velho ou o lixo da ressincronizacao da ponte DSI - o "artefato" ao acordar.
//
//   tab5_power_retomar();   // volta clock, fluxo e painel, tela ainda escura
//   ...redesenhe tudo...
//   tab5_power_acender();   // agora sim
void tab5_power_acender(void);

// Gancho chamado a cada degrau da rampa de brilho do acender (a rampa bloqueia
// a tarefa por ~300 ms; sem o gancho, qualquer coisa animada na tela fica
// congelada nesse tempo). Rodar na tarefa de interface. nullptr = nenhum.
void tab5_power_gancho_rampa(void (*passo)(void));

// ---------------------------------------------------------------------------
// QUAL TELA ESTE TAB5 TEM
// ---------------------------------------------------------------------------
// O Tab5 saiu com tres telas (todas 720x1280 MIPI-DSI, mesma placa):
//   ILI9881C + GT911 - o lancamento (2025): painel e toque em chips separados;
//   ST7123           - desde out/2025: painel e toque no MESMO chip (TDDI);
//   ST7121           - desde abr/2026: outro TDDI da mesma familia.
// O M5GFX descobre sozinho no M5.begin() e monta o driver certo; o desenho e o
// toque comuns (M5.Display, M5.Touch) funcionam igual nas tres. O que NAO
// funciona igual e o que este sistema faz por baixo do M5GFX, e cada ponto
// desses pergunta aqui antes:
//   - o vigia do toque le registradores do GT911 direto (kernel/core.cpp);
//   - o repouso fundo (SLPIN + desmontar o DSI) - num TDDI o toque e varrido
//     junto com a imagem, entao com o painel dormindo o toque provavelmente
//     morre, e o toque e o que acorda a tela. Nos ST o repouso so apaga a luz;
//   - o relogio de pixel (tab5_power_relogio_dpi): os porticos que o M5GFX
//     usa nos ST deixam a varredura do toque na folga vertical, e mexer no
//     relogio muda essa folga. Nos ST ele fica no de fabrica.
// Os caminhos dos ST foram escritos sem aparelho para testar: sao os
// CONSERVADORES (o que o M5GFX ja faz sozinho), nunca um palpite novo.
//
// Detectado uma vez, na primeira chamada - que tem que ser DEPOIS do
// M5.begin() e ANTES de qualquer troca de relogio (main.cpp chama logo apos
// o begin). Qualquer tarefa depois disso.
typedef enum {
  TAB5_PAINEL_DESCONHECIDO = 0,
  TAB5_PAINEL_ILI9881C,
  TAB5_PAINEL_ST7123,
  TAB5_PAINEL_ST7121,
} tab5_painel_t;
tab5_painel_t tab5_painel(void);
const char*   tab5_painel_nome(void);
// true so no ILI9881C: o toque e um GT911 separado (vigia direto, pulso no
// GPIO 23 confirmado, repouso fundo e relogio ajustavel validados nele).
bool          tab5_painel_toque_gt911(void);

// ---------------------------------------------------------------------------
// RELOGIO DE PIXEL DO PAINEL
// ---------------------------------------------------------------------------
// O painel DPI le o framebuffer da PSRAM sem parar, no ritmo deste relogio. A
// 80MHz (o que o M5GFX programa para o ILI9881C) sao 64 frames por segundo e
// 118,5 MB/s de leitura CONTINUA. A 60MHz sao 48 frames e 88,9 MB/s.
//
// Esses 25% a menos sao a unica coisa que reduz a demanda do painel de forma
// permanente - todo o resto do sistema (fatiar limpeza, evitar limpar duas
// vezes, espacar rajadas) mexe no OUTRO lado da disputa. Ver a explicacao
// completa em tab5_power.cpp, junto da implementacao.
//
// Trocar o relogio DESMONTA E REMONTA o painel - o mesmo caminho do despertar.
// Devolve false (e volta sozinho ao valor anterior) se o painel nao subir.
bool tab5_power_relogio_dpi(int mhz);

// Relogio atual, em MHz; 0 se nao ha painel.
int  tab5_power_relogio_dpi(void);

// Quadros por segundo no relogio atual, pelos porticos que o M5GFX programou
// NESTA tela (cada uma tem os seus). 0 se nao ha painel.
double tab5_power_quadros_por_segundo(void);

// ---------------------------------------------------------------------------
// TETO DO CPU COM A TELA ACORDADA - diagnostico, criado
// ---------------------------------------------------------------------------
// Muda SO o teto de clock do CPU, em execucao, SEM entrar em repouso e SEM
// desmontar o painel - diferente de tab5_power_repouso(), que apaga a tela
// por completo. Existe para testar uma pergunta especifica: um relogio de
// painel mais baixo (ver tab5_power_relogio_dpi acima) abre espaco para um
// teto de CPU menor, com a tela LIGADA e desenhando normalmente, sem underrun?
//
// Ninguem tinha testado essa combinacao ainda - so "CPU baixo com o painel no
// relogio de sempre" (deu underrun) e "CPU baixo com o painel
// desmontado" (o repouso, que sempre funcionou porque nao ha mais painel para
// dar underrun). Esta funcao permite o terceiro caso: os dois LIGADOS e os
// dois devagar, ao mesmo tempo.
//
// Valores validos nesta revisao de silicio: 360, 180, 90 (PLL) ou XTAL/N -
// 40, 20, 10, 8, 5, 4, 2, 1. Devolve false e nao muda nada se o IDF recusar.
bool tab5_power_teto_cpu(int mhz);

// true enquanto estiver em repouso.
bool tab5_power_em_repouso(void);
