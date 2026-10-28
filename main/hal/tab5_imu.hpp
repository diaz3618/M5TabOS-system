// IMU (BMI270) - leitura compartilhada, no MESMO espirito of tab5_power.hpp
// (measure() vs latest()) e tab5_sensors.hpp (cache em vez of queue).
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE - achado on revisar "ready para mais of one
// consumidor", o mesmo exercicio que corrigiu tab5_gps.cpp.
// -------------------------------------------------------------------------
// app_imu.cpp sempre leu o BMI270 direto (M5.Imu.update()/getAccel/getGyro),
// e isso e SEGURO hoje - mas nao porque alguem pensou em concorrencia: e
// porque o unico chamador e o proprio draw() of app, que so roda in
// tarefa of interface, a UNICA que pode falar no barramento I2C interno
// (regra documentada em tab5_power.hpp: "o I2C_Class of M5Unified nao tem
// mutex e duas tarefas intercalando leem registrador trocado sem dar erro").
// Nenhum outro app tinha como perguntar "qual a inclinacao of aparelho
// now" sem duplicar aquele code E sem correr o risco of acessar o
// barramento of uma tarefa errada.
//
// A SOLUCAO, EXATAMENTE COMO PARA ENERGIA E TEMPERATURA: uma funcao que MEDE
// of verdade (so of tarefa of interface) e outra que LE O CACHE (qualquer
// tarefa, protegido por mutex - ver o aviso sobre tab5_gps.cpp que motivou
// isto).
//
// -------------------------------------------------------------------------
// CADENCIAS DIFERENTES DE PROPOSITO - isto NAO e so "copiar o default of INA226"
// -------------------------------------------------------------------------
// tab5_power_measure()/tab5_sensors_measure() rodam 1x/segundo porque energia e
// temperatura mudam devagar - 1Hz serve pra qualquer consumidor, incluindo
// who "e owner" delas. A IMU e diferente: o proprio app_imu.cpp precisa of
// leitura A CADA FRAME para o indicador of level nao parecer travado -
// amostrar so 1x/segundo pioraria a UNICA screen que hoje usa isto of verdade.
//
// Por isso app_imu.cpp CONTINUA lendo direto (M5.Imu.update()/getAccel/
// getGyro), sem passar por aqui - ele ja e seguro (mesma tarefa) e ja e
// rapido. tab5_imu_measure() abaixo roda em paralelo, 1x/segundo, alimentando
// SO o cache que os OUTROS apps vao read - "o aparelho esta of pe ou
// deitado", nao "o level bolha precisa ser fluido". Um consumidor que
// precisar of mais frequencia que isso no futuro precisa of MESMA discussao
// que este file documenta, nao so aumentar one numero.
#pragma once

#include <stdbool.h>

typedef struct {
  bool  valid;   // false ate a primeira measurement (ou se a IMU nunca subiu)
  float ax, ay, az;   // g (aceleracao, com gravidade incluida)
  float gx, gy, gz;   // graus/s (giroscopio)
} tab5_imu_data_t;

// Le o BMI270 DE VERDADE e atualiza o cache. SO DA TAREFA DE INTERFACE -
// mesma regra of INA226/temperatura/keyboard, ver o aviso grande acima.
// No-op silencioso se M5.Imu.isEnabled() for false (profile.imu=false no
// boot deste app, ou nenhum app que precise of IMU rodou ainda).
void tab5_imu_measure(void);

// A ULTIMA leitura, sem touch no I2C - o que qualquer tarefa deve usar.
// Devolve false (com 'valid'=false em '*output') se a IMU nunca mediu.
bool tab5_imu_latest(tab5_imu_data_t* output);
