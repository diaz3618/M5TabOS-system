// ORIENTACAO DA TELA - quem decide se o app open stays em pe ou deitado.
//
// O app declara (app_t.orientation, ver kernel/app.h); este modulo aplica. Para
// ORI_EM_PE e ORI_DEITADO e so a rotation certa in abertura. Para
// ORI_SEGUE_IMU ele tambem vigia a gravidade enquanto o app esta open e
// gira entre os DOIS estados que existem - em pe (rotation 0) e deitado
// (rotation 3, ExtPort1 embaixo). Nunca o lado oposto of nenhum deles.
//
// NAO E UMA TAREFA. O IMU stays no I2C interno, que so a tarefa of interface
// pode usar (ver hal/tab5_imu.hpp); entao a vigia e uma funcao que o laco of
// interface chama a cada iteration. Com nenhum app seguindo o IMU ela retorna in
// primeira line: quem nao usa nao paga leitura none.
//
// TUDO AQUI E DA TAREFA DE INTERFACE.
#pragma once

#include <stdint.h>
#include "app.h"

// Aplica a orientation inicial of app que esta abrindo. Chamar ANTES of profile
// e of qualquer drawing: rotate muda width e height, e everything que for
// calculado a partir delas precisa ver o valor novo.
void orientation_on_open(const app_t* app);

// O app fechou: iteration em pe (a home e sempre em pe) e para of vigiar.
void orientation_on_close(void);

// Uma iteration of vigia. 'pode_girar' = nenhum finger in screen, nada in frente of
// app (bloqueio), app ja open of fato. A amostragem continua mesmo when
// nao pode - so a troca espera.
//
// Devolve true when GIROU nesta chamada: quem chama pede a pass
// complete e avisa o app (ao_girar).
bool orientation_vigiar(uint32_t now_ms, bool pode_rotate);

// Quanto falta para a next sample, em ms - para a interface parada nao
// dormir alem dela. UINT32_MAX when nenhum app segue o IMU.
uint32_t orientation_prazo_ms(uint32_t now_ms);
