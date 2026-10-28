// RTC (RX8130) - bateria propria, guarda a hora entre boots.
// ele so era LIGADO (perfil.rtc=true) e nunca lido nem ajustado por ninguem -
// M5.begin() ja chama M5.Rtc.setSystemTimeFromRtc() sozinho (ver
// M5Unified.cpp), entao QUALQUER CODIGO que precise "que dia e hoje" so
// precisa chamar time()/gmtime() da libc - nao ha leitura de I2C nenhuma
// nisso, e por isso nao existe um "tab5_rtc_ler()" aqui: nao faz falta.
//
// O QUE FALTAVA ERA ESCREVER. Sem alguem AJUSTAR o RX8130 pelo menos uma vez,
// ele guarda uma hora sem sentido (de fabrica) - a bateria preserva o que foi
// gravado, nao inventa a hora certa sozinha. Ver services/tab5_time.hpp para
// QUEM decide quando ajustar e de qual fonte.
#pragma once

#include <stdbool.h>
#include <time.h>

// Escreve o RTC E o relogio do sistema (settimeofday) NA MESMA CHAMADA - sem
// o segundo passo, so a PROXIMA vez que o aparelho ligar e que a sessao
// passaria a responder certo em time()/gmtime() (e so entao porque
// M5.begin() le o RTC sozinho); com os dois juntos, quem chamar time() JA
// AGORA, nesta mesma sessao, ve a hora certa.
//
// SO DA TAREFA DE INTERFACE - o RX8130 e I2C interno (RTC_Base usa In_I2C
// por padrao, mesmo barramento do toque/IMU/PMIC), mesma regra documentada
// em tab5_power.hpp. 'epoch_utc' em segundos desde 1970, UTC.
bool tab5_rtc_definir(time_t epoch_utc);

// O RX8130 responde no barramento? (nao diz se a hora gravada esta CERTA, so
// se o chip existe e esta acessivel).
bool tab5_rtc_disponivel(void);
