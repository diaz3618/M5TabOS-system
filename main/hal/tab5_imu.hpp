// IMU (BMI270) - leitura compartilhada, no MESMO espirito de tab5_power.hpp
// (measure() vs latest()) e tab5_sensors.hpp (cache em vez de queue).
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE - achado ao revisar "ready para mais de um
// consumidor", o mesmo exercicio que corrigiu tab5_gps.cpp.
// -------------------------------------------------------------------------
// app_imu.cpp sempre leu o BMI270 direto (M5.Imu.update()/getAccel/getGyro),
// e isso e SEGURO hoje - mas nao porque alguem pensou em concorrencia: e
// porque o unico chamador e o proprio desenhar() do app, que so roda na
// tarefa de interface, a UNICA que pode falar no barramento I2C interno
// (regra documentada em tab5_power.hpp: "o I2C_Class do M5Unified nao tem
// mutex e duas tarefas intercalando leem registrador trocado sem dar erro").
// Nenhum outro app tinha como perguntar "qual a inclinacao do aparelho
// agora" sem duplicar aquele code E sem correr o risco de acessar o
// barramento de uma tarefa errada.
//
// A SOLUCAO, EXATAMENTE COMO PARA ENERGIA E TEMPERATURA: uma funcao que MEDE
// de verdade (so da tarefa de interface) e outra que LE O CACHE (qualquer
// tarefa, protegido por mutex - ver o aviso sobre tab5_gps.cpp que motivou
// isto).
//
// -------------------------------------------------------------------------
// CADENCIAS DIFERENTES DE PROPOSITO - isto NAO e so "copiar o padrao do INA226"
// -------------------------------------------------------------------------
// tab5_power_measure()/tab5_sensors_measure() rodam 1x/segundo porque energia e
// temperatura mudam devagar - 1Hz serve pra qualquer consumidor, incluindo
// who "e owner" delas. A IMU e diferente: o proprio app_imu.cpp precisa de
// leitura A CADA FRAME para o indicador de nivel nao parecer travado -
// amostrar so 1x/segundo pioraria a UNICA tela que hoje usa isto de verdade.
//
// Por isso app_imu.cpp CONTINUA lendo direto (M5.Imu.update()/getAccel/
// getGyro), sem passar por aqui - ele ja e seguro (mesma tarefa) e ja e
// rapido. tab5_imu_measure() abaixo roda em paralelo, 1x/segundo, alimentando
// SO o cache que os OUTROS apps vao read - "o aparelho esta de pe ou
// deitado", nao "o nivel bolha precisa ser fluido". Um consumidor que
// precisar de mais frequencia que isso no futuro precisa da MESMA discussao
// que este file documenta, nao so aumentar um numero.
#pragma once

#include <stdbool.h>

typedef struct {
  bool  valid;   // false ate a primeira measurement (ou se a IMU nunca subiu)
  float ax, ay, az;   // g (aceleracao, com gravidade incluida)
  float gx, gy, gz;   // graus/s (giroscopio)
} tab5_imu_data_t;

// Le o BMI270 DE VERDADE e atualiza o cache. SO DA TAREFA DE INTERFACE -
// mesma regra do INA226/temperatura/keyboard, ver o aviso grande acima.
// No-op silencioso se M5.Imu.isEnabled() for false (profile.imu=false no
// boot deste app, ou nenhum app que precise de IMU rodou ainda).
void tab5_imu_measure(void);

// A ULTIMA leitura, sem tocar no I2C - o que qualquer tarefa deve usar.
// Devolve false (com 'valid'=false em '*output') se a IMU nunca mediu.
bool tab5_imu_latest(tab5_imu_data_t* output);
