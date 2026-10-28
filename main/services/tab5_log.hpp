// LOG DO SISTEMA NO CARTAO, UM ARQUIVO POR DIA.
//
// -------------------------------------------------------------------------
// POR QUE
// -------------------------------------------------------------------------
// Ate hoje o UNICO jeito de ver o log era o monitor serial ligado no
// computador - e boa parte dos bugs deste sistema (GPS, Mesh, o Grove em
// geral) so aparece longe da mesa, "perto da janela" onde o sinal e melhor
// mas o cabo USB nao chega. Sem log, cada incidente fica sem prova: o
// sintoma descrito vira adivinhacao da causa lendo o codigo.
//
// -------------------------------------------------------------------------
// POR DIA, NAO POR BOOT, com o motivo junto
// -------------------------------------------------------------------------
// "por boot complica bastante, pq toda hora estamos bootando" - este sistema
// reinicia o tempo todo durante desenvolvimento (cada flash.sh e um boot
// novo). Um arquivo por boot viraria uma pasta ilegivel de arquivos de
// segundos. Por DIA, usando a data do RTC (ver tab5_rtc.hpp - bateria
// propria, "quase nunca descarrega"), varios boots do mesmo dia caem no
// MESMO arquivo, um atras do outro, e uma marca "=== boot ... ===" separa
// as sessoes sem precisar de um arquivo novo para cada uma.
//
// -------------------------------------------------------------------------
// COMO CAPTURA: substitui o vprintf do ESP_LOG, nao duplica cada chamada
// -------------------------------------------------------------------------
// tab5_log_iniciar() troca a funcao que o ESP_LOG usa para imprimir
// (esp_log_set_vprintf) por uma que faz DUAS coisas: chama a funcao
// ORIGINAL primeiro (o console serial continua identico a hoje, byte a
// byte) e manda uma copia da MESMA linha para um buffer em RAM. Assim
// qualquer ESP_LOGI/W/E/D de QUALQUER tarefa do sistema (interface, GPS,
// Mesh, camera...) e capturado automaticamente - nenhuma tarefa precisa
// saber que este arquivo existe.
//
// -------------------------------------------------------------------------
// POR QUE UM BUFFER EM RAM ANTES DO CARTAO
// -------------------------------------------------------------------------
// A funcao que substitui o vprintf roda NA STACK DE QUEM CHAMOU O LOG - pode
// ser a tarefa de interface, o GPS, a camera, qualquer uma. Gravar no cartao
// ali mesmo (montar, abrir, escrever, fechar, desmontar - a mesma conta de
// ~100ms que tab5_sd.hpp documenta) prenderia QUALQUER tarefa do sistema
// pelo tempo de uma escrita de cartao, toda vez que ela log-asse algo. Foi
// exatamente o tipo de bug que a sondagem do Grove teve que sair da tarefa
// de interface para nao repetir (ver o aviso grande em kernel/core.cpp).
//
// Em vez disso, a troca de vprintf so FORMATA a linha e poe numa queue em RAM
// (xStreamBuffer, que aceita varias tarefas escrevendo ao mesmo tempo sem
// trava manual) - operacao de poucos microssegundos, nunca bloqueia. A
// tarefa "cartao" (o gravador do sistema, de baixa prioridade) esvazia essa
// queue de tempos em tempos, de carona (tab5_sd_carona), e SO ENTAO monta o
// cartao e escreve. Se o cartao estiver
// ausente, as linhas ficam esperando no buffer ate ele voltar (ou sao
// descartadas se o buffer encher primeiro - preferimos perder log velho a
// travar o sistema tentando guardar todo log para sempre).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Instala a captura e pega carona na tarefa "cartao" para descarregar. Chamar UMA vez
// no boot, depois de tab5_sd_iniciar() (ela usa tab5_sd_montar/abrir) - ver
// kernel/core.cpp.
void tab5_log_iniciar(void);

// Descarrega no cartao em ate 1 s, como um aviso faria, sem esperar o lote de
// 60 s. Para o fim de algo cujo log se quer ler logo (o pendrive: o console
// volta e quem abre a porta serial reinicia o Tab5 - o que estava so na RAM
// se perderia). De qualquer tarefa.
void tab5_log_descarregar_logo(void);

// Estado para o comando 'servicos' e para quem quiser mostrar que o log esta
// vivo: quantos bytes esperam na RAM, quantas linhas ja foram gravadas de
// verdade, quantas se perderam (buffer cheio) e o nome do arquivo de hoje
// ("" se a captura ainda nao tentou escrever nenhuma vez).
void tab5_log_estado(size_t* bytes_pendentes, uint32_t* linhas_gravadas,
                      uint32_t* linhas_perdidas, char* arquivo, size_t arquivo_tam);
