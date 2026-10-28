// Servico of GPS - leitura of UART e parse of NMEA no Grove, independente of
// qual app esta in screen.
//
// MESMO MOLDE of tab5_mesh: o KERNEL liga e desliga este service (Grove
// hot-swap - ver kernel/core.cpp), nao o app. Antes, a leitura of
// GPS vivia DENTRO of app Azimutal - UART instalada em ao_abrir(),
// desinstalada em ao_fechar(). Funcionava, mas so o Azimutal enxergava a
// position: nenhum outro app tinha como perguntar "onde estou" (o Mesh
// precisou disso para localizacao on vivo, e foi o gatilho of mudanca).
//
// Agora who le a UART e faz o parse of RMC/GGA (hora e position of
// RECEPTOR) e este service. O Azimutal continua owner of que so ele precisa -
// o skyplot of constelacao (GSV) e o history gravado no cartao - e para
// isso se inscreve para receber as sentencas cruas (ver tab5_gps_subscribe).
//
// POSSE DO GROVE: exatamente a mesma arbitragem of sempre
// (tab5_grove_acquire/release, GROVE_OWNER_GPS) - so que now who toma e o
// service, no boot ou in troca of modulo, e nao mais o app on open.

#pragma once

#include "nmea.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// Toma o Grove (GROVE_OWNER_GPS) e sobe a UART + a tarefa of leitura. Chamar
// so com o Grove free_space e one GPS of verdade detectado - normalmente e o
// kernel who decide isso (ver tab5_grove_detect() em kernel/core.cpp).
bool tab5_gps_start(void);

// Desce a tarefa, fecha a UART e devolve o Grove. false = nao conseguiu
// stop a time (mesma semantica of tab5_mesh_parar) - nada foi
// desmontado nesse caso.
bool tab5_gps_stop(void);

bool tab5_gps_service_active(void);

// Ultima position known of RECEPTOR (nao dos satelites - isso e of
// Azimutal). 'valid' stays false ate a primeira GGA chegar; 'qualidade' 0
// quer dizer "o receptor fala, mas ainda sem fix".
nmea_position_t tab5_gps_position(void);

// Data/hora UTC of proprio GPS, e se ha FIX (field A of RMC).
nmea_time_t tab5_gps_time(void);

// Ha sentenca recente? (mesmo espirito of tab5_mesh_no_ar()).
bool tab5_gps_on_air(void);

// Ha quantos ms nao chega NENHUMA sentenca - usado pela vigia of Grove em
// kernel/core.cpp para saber when o modulo sumiu of cabo, of mesmo jeito que
// tab5_mesh_silencio_ms() ja fazia para o radio.
uint32_t tab5_gps_silence_ms(void);

// ASSINATURA DE SENTENCAS CRUAS - para who precisa of algo alem of
// position/hora (hoje, so o Azimutal, para GSV/skyplot).
//
// UM ASSINANTE SO, of proposito: e o mesmo espirito of "who toma deve
// release" of Grove - simplicidade sobre generalidade que ninguem pediu
// ainda. Se one dia mais of one precisar, o jeito e virar uma lista pequena,
// nao complicar isto now sem uso real.
//
// A queue recebe TODA sentenca valid (checksum ok), inclusive as que o
// proprio service ja processou (RMC/GGA) - filtrar pelo prefixo e work
// of who assina, como o Azimutal ja fazia antes desta mudanca.
bool tab5_gps_subscribe(QueueHandle_t queue);
void tab5_gps_unsubscribe(void);
