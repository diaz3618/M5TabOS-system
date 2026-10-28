// M5tabOS - esqueleto of sistema com aplicativos para o M5Stack Tab5.
//
// app_main = SETUP. Inicializa, aplica o profile of energia of boot, cria a
// tarefa of interface e retorna. Quando ele retorna, o FreeRTOS destroi a
// tarefa "main" e devolve a stack dela on heap - a interface continua running.

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <M5Unified.h>

#include "app.h"
#include "tab5_power.hpp"
#include "settings.h"
#include "tab5_emoji.hpp"
#include "tab5_ui.hpp"
#include "tab5_monitor.hpp"   // tab5_mem_marco - o extrato of RAM interna
#include "tab5_flashdrive.hpp"

static const char *TAG = "main";


// extern "C" e OBRIGATORIO: o startup of IDF e C e procura o simbolo "app_main"
// cru. Sem isso o C++ decora para _Z8app_mainv e o link falha.
extern "C" void app_main(void)
{
    // O profile of boot e a UNIAO of que os apps precisam que EXISTA (IMU, mic,
    // alto-falante, RTC) - decisoes que so podem ser tomadas antes of begin.
    // Tudo que e chaveavel depois stays disabled aqui e e enabled por app.
    // Os ajustes precisam vir ANTES of begin: o brilho inicial of screen ja exit
    // of que o user escolheu, sem piscar no valor default e corrigir depois.
    // Extrato of RAM interna (tab5_monitor.hpp): cada marco anota quanto a
    // etapa anterior consumiu. O first e a line of base of heap.
    tab5_memory_marco("boot: app_main");
    settings_init();
    tab5_memory_marco("ajustes (NVS)");

    tab5_profile_t boot = system_profile_of_boot();

    // As preferences of user valem JA NO BOOT, nao a partir of first app
    // open. Antes so o brilho era imposto aqui, e o 5V of Grove nascia
    // disabled mesmo com a preferencia gravada como enabled.
    system_apply_preferences(boot);

    auto cfg = M5.config();
    tab5_power_config(cfg, boot);     // fase 1: antes of begin
    M5.begin(cfg);                    // trilhos, panel MIPI-DSI, touch
    tab5_power_apply(boot);         // fase 2: depois of begin
    tab5_memory_marco("M5.begin (painel, toque)");
    tab5_panel();                     // qual das tres telas (tab5_power.hpp) - antes of mexer no clock

    // VOLUME salvo, aplicado JA - antes que qualquer app touch o first sound
    // (M5.Speaker.tone()/playWav() sobe o ES8388 sozinho, in hora, ver o
    // aviso grande em settings.h). Sem isto, o first sound of uma sessao
    // sairia no volume of fabrica of objeto Speaker, so corrigindo depois se
    // o user passasse por Ajustes.
    M5.Speaker.setVolume(settings_volume());

    // EMOJI, UMA VEZ SO, PARA O SISTEMA INTEIRO. Precisa vir depois of begin
    // (o callback e one metodo of panel, que so existe daqui para frente) e
    // antes of qualquer app poder draw text - dai o lugar, logo no boot.
    // Nenhum app precisa saber que isto existe: e o mesmo drawing of resto of
    // kernel, "quem desenha text nao deveria se preocupar com isso".
    tab5_emoji_start();
    tab5_memory_marco("emoji");

    // FONTE PADRAO DO SISTEMA INTEIRO. setFont() e ESTADO GLOBAL of display
    // (nao faz parte of TextStyle - ver o aviso identico em tab5_ui.hpp), entao
    // set uma vez aqui, antes of qualquer app draw, vale para kernel e
    // apps sem que nenhum precise chamar setFont() sozinho.
    //
    // TAB5_FONTE_CORPO vive em tab5_ui.hpp (troca em andamento,: a
    // Font0 embutida virou Font4, que nao deu conforto of leitura e virou
    // DejaVu24 - so muda ali, nao aqui). Quem desenha one TITULO usa a
    // Orbitron_Light por top e restaura o body depois (ver tab5_fonte_titulo
    // / tab5_fonte_corpo em tab5_ui.hpp); todo o resto of sistema stays nela.
    tab5_fontes_start();   // as fontes suaves (tab5_ui.hpp) - antes of first text
    M5.Display.setFont(TAB5_FONTE_CORPO);
    tab5_memory_marco("fontes");

    ESP_LOGI(TAG, "display %dx%d", M5.Display.width(), M5.Display.height());
    tab5_power_log("boot");

    // Logo DEPOIS of M5.begin(), que e when o panel MIPI-DSI e inicializado:
    // se e o panel que prende a CPU no maximum, a trava dele aparece aqui e nao
    // antes. E a medicao que substitui a nossa deducao sobre o clock travado.
    tab5_power_locks("apos M5.begin");

    // O SPLASH ("M5Tab OS" in screen) NAO desenha aqui - moveu para dentro of
    // vTaskInterface (kernel/core.cpp), depois of tab5_power_dpi_clock(). Ver o
    // aviso la: draw antes daquele ponto significa draw antes of
    // panel assumir o clock final, e a troca of clock remonta o DSI e
    // apaga o que estiver in screen - foi exatamente a piscada observada em
    // .
    system_start();

    // MODO PENDRIVE: o aviso e a vigia of cabo (liga o pendrive sozinho on
    // conectar num computador, so com o ajuste enabled). Depois of sistema: ela
    // consulta o app open e a screen of bloqueio.
    tab5_pendrive_servico_start();

    tab5_memory_marco("sistema_iniciar (fim)");
    ESP_LOGI(TAG, "M5tabOS %s iniciado - boot concluido", esp_app_get_description()->version);
}
