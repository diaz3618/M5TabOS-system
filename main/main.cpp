// M5tabOS - esqueleto de sistema com aplicativos para o M5Stack Tab5.
//
// app_main = SETUP. Inicializa, aplica o perfil de energia de boot, cria a
// tarefa de interface e retorna. Quando ele retorna, o FreeRTOS destroi a
// tarefa "main" e devolve a stack dela ao heap - a interface continua rodando.

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
#include "tab5_monitor.hpp"   // tab5_mem_marco - o extrato da RAM interna
#include "tab5_flashdrive.hpp"

static const char *TAG = "main";


// extern "C" e OBRIGATORIO: o startup do IDF e C e procura o simbolo "app_main"
// cru. Sem isso o C++ decora para _Z8app_mainv e o link falha.
extern "C" void app_main(void)
{
    // O perfil de boot e a UNIAO do que os apps precisam que EXISTA (IMU, mic,
    // alto-falante, RTC) - decisoes que so podem ser tomadas antes do begin.
    // Tudo que e chaveavel depois fica desligado aqui e e ligado por app.
    // Os ajustes precisam vir ANTES do begin: o brilho inicial da tela ja sai
    // do que o usuario escolheu, sem piscar no valor padrao e corrigir depois.
    // Extrato da RAM interna (tab5_monitor.hpp): cada marco anota quanto a
    // etapa anterior consumiu. O primeiro e a linha de base do heap.
    tab5_mem_marco("boot: app_main");
    ajustes_iniciar();
    tab5_mem_marco("ajustes (NVS)");

    tab5_perfil_t boot = sistema_perfil_de_boot();

    // As preferencias do usuario valem JA NO BOOT, nao a partir do primeiro app
    // aberto. Antes so o brilho era imposto aqui, e o 5V do Grove nascia
    // desligado mesmo com a preferencia gravada como ligada.
    sistema_impor_preferencias(boot);

    auto cfg = M5.config();
    tab5_power_config(cfg, boot);     // fase 1: antes do begin
    M5.begin(cfg);                    // trilhos, painel MIPI-DSI, touch
    tab5_power_aplicar(boot);         // fase 2: depois do begin
    tab5_mem_marco("M5.begin (painel, toque)");
    tab5_painel();                    // qual das tres telas (tab5_power.hpp) - antes de mexer no relogio

    // VOLUME salvo, aplicado JA - antes que qualquer app toque o primeiro som
    // (M5.Speaker.tone()/playWav() sobe o ES8388 sozinho, na hora, ver o
    // aviso grande em settings.h). Sem isto, o primeiro som de uma sessao
    // sairia no volume de fabrica do objeto Speaker, so corrigindo depois se
    // o usuario passasse por Ajustes.
    M5.Speaker.setVolume(ajustes_volume());

    // EMOJI, UMA VEZ SO, PARA O SISTEMA INTEIRO. Precisa vir depois do begin
    // (o callback e um metodo do painel, que so existe daqui para frente) e
    // antes de qualquer app poder desenhar texto - dai o lugar, logo no boot.
    // Nenhum app precisa saber que isto existe: e o mesmo desenho do resto do
    // kernel, "quem desenha texto nao deveria se preocupar com isso".
    tab5_emoji_iniciar();
    tab5_mem_marco("emoji");

    // FONTE PADRAO DO SISTEMA INTEIRO. setFont() e ESTADO GLOBAL do display
    // (nao faz parte de TextStyle - ver o aviso identico em tab5_ui.hpp), entao
    // definir uma vez aqui, antes de qualquer app desenhar, vale para kernel e
    // apps sem que nenhum precise chamar setFont() sozinho.
    //
    // TAB5_FONTE_CORPO vive em tab5_ui.hpp (troca em andamento,: a
    // Font0 embutida virou Font4, que nao deu conforto de leitura e virou
    // DejaVu24 - so muda ali, nao aqui). Quem desenha um TITULO usa a
    // Orbitron_Light por cima e restaura o corpo depois (ver tab5_fonte_titulo
    // / tab5_fonte_corpo em tab5_ui.hpp); todo o resto do sistema fica nela.
    tab5_fontes_iniciar();   // as fontes suaves (tab5_ui.hpp) - antes do primeiro texto
    M5.Display.setFont(TAB5_FONTE_CORPO);
    tab5_mem_marco("fontes");

    ESP_LOGI(TAG, "display %dx%d", M5.Display.width(), M5.Display.height());
    tab5_power_log("boot");

    // Logo DEPOIS do M5.begin(), que e quando o painel MIPI-DSI e inicializado:
    // se e o painel que prende a CPU no maximo, a trava dele aparece aqui e nao
    // antes. E a medicao que substitui a nossa deducao sobre o clock travado.
    tab5_power_travas("apos M5.begin");

    // O SPLASH ("M5Tab OS" na tela) NAO desenha aqui - moveu para dentro de
    // vTaskInterface (kernel/core.cpp), depois de tab5_power_relogio_dpi(). Ver o
    // aviso la: desenhar antes daquele ponto significa desenhar antes do
    // painel assumir o relogio final, e a troca de relogio remonta o DSI e
    // apaga o que estiver na tela - foi exatamente a piscada observada em
    // .
    sistema_iniciar();

    // MODO PENDRIVE: o aviso e a vigia do cabo (liga o pendrive sozinho ao
    // conectar num computador, so com o ajuste ligado). Depois do sistema: ela
    // consulta o app aberto e a tela de bloqueio.
    tab5_pendrive_servico_iniciar();

    tab5_mem_marco("sistema_iniciar (fim)");
    ESP_LOGI(TAG, "M5tabOS %s iniciado - boot concluido", esp_app_get_description()->version);
}
