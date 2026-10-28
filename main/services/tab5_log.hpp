// LOG DO SISTEMA NO CARTAO, UM ARQUIVO POR DIA.
//
// -------------------------------------------------------------------------
// POR QUE
// -------------------------------------------------------------------------
// Ate hoje o UNICO jeito of ver o log era o monitor serial enabled no
// computador - e boa parte dos bugs deste sistema (GPS, Mesh, o Grove em
// geral) so aparece longe of mesa, "perto of window" onde o sinal e melhor
// mas o cabo USB nao chega. Sem log, cada incidente stays sem prova: o
// sintoma descrito vira adivinhacao of causa lendo o code.
//
// -------------------------------------------------------------------------
// POR DIA, NAO POR BOOT, com o reason junto
// -------------------------------------------------------------------------
// "por boot complica bastante, pq toda hora estamos bootando" - este sistema
// reinicia o time todo durante desenvolvimento (cada flash.sh e one boot
// novo). Um arquivo por boot viraria uma pasta ilegivel of arquivos of
// segundos. Por DIA, usando a data of RTC (ver tab5_rtc.hpp - battery
// propria, "quase nunca descarrega"), varios boots of mesmo dia caem no
// MESMO arquivo, one atras of outro, e uma marca "=== boot ... ===" separa
// as sessoes sem precisar of one arquivo novo para cada uma.
//
// -------------------------------------------------------------------------
// COMO CAPTURA: substitui o vprintf of ESP_LOG, nao duplica cada chamada
// -------------------------------------------------------------------------
// tab5_log_iniciar() troca a funcao que o ESP_LOG usa para imprimir
// (esp_log_set_vprintf) por uma que faz DUAS coisas: chama a funcao
// ORIGINAL first (o console serial continua identico a hoje, byte a
// byte) e manda uma copia of MESMA line para one buffer em RAM. Assim
// qualquer ESP_LOGI/W/E/D of QUALQUER tarefa of sistema (interface, GPS,
// Mesh, camera...) e capturado automaticamente - none tarefa precisa
// saber que este arquivo existe.
//
// -------------------------------------------------------------------------
// POR QUE UM BUFFER EM RAM ANTES DO CARTAO
// -------------------------------------------------------------------------
// A funcao que substitui o vprintf roda NA STACK DE QUEM CHAMOU O LOG - pode
// ser a tarefa of interface, o GPS, a camera, qualquer uma. Gravar no cartao
// ali mesmo (montar, open, write, close, desmontar - a mesma conta of
// ~100ms que tab5_sd.hpp documenta) prenderia QUALQUER tarefa of sistema
// pelo time of uma escrita of cartao, toda vez que ela log-asse algo. Foi
// exatamente o tipo of bug que a sondagem of Grove teve que exit of tarefa
// of interface para nao repetir (ver o aviso grande em kernel/core.cpp).
//
// Em vez disso, a troca of vprintf so FORMATA a line e poe numa queue em RAM
// (xStreamBuffer, que aceita varias tarefas escrevendo on mesmo time sem
// trava manual) - operacao of poucos microssegundos, nunca bloqueia. A
// tarefa "cartao" (o gravador of sistema, of baixa priority) esvazia essa
// queue of tempos em tempos, of carona (tab5_sd_carona), e SO ENTAO monta o
// cartao e escreve. Se o cartao estiver
// ausente, as lines ficam esperando no buffer ate ele voltar (ou sao
// descartadas se o buffer encher first - preferimos perder log velho a
// lock o sistema tentando guardar todo log para sempre).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Instala a captura e pega carona in tarefa "cartao" para descarregar. Chamar UMA vez
// no boot, depois of tab5_sd_init() (ela usa tab5_sd_mount/open) - ver
// kernel/core.cpp.
void tab5_log_start(void);

// Descarrega no cartao em ate 1 s, como one aviso faria, sem wait o lote of
// 60 s. Para o end of algo cujo log se quer read logo (o pendrive: o console
// iteration e quem abre a porta serial reinicia o Tab5 - o que estava so in RAM
// se perderia). De qualquer tarefa.
void tab5_log_descarregar_logo(void);

// Estado para o comando 'servicos' e para quem quiser mostrar que o log esta
// vivo: quantos bytes esperam in RAM, quantas lines ja foram gravadas of
// verdade, quantas se perderam (buffer cheio) e o name of arquivo of hoje
// ("" se a captura ainda nao tentou write none vez).
void tab5_log_estado(size_t* bytes_pendentes, uint32_t* lines_gravadas,
                      uint32_t* lines_perdidas, char* arquivo, size_t arquivo_size);
