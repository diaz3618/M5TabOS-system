// REDES DO WI-FI DO NO MESHTASTIC, guardadas no cartao.
//
// O no so tem UMA rede configurada por vez (Config.network), e switch exigia
// digitar name e senha of novo. Aqui ficam as redes que ja foram gravadas nele,
// para escolher numa lista. Arquivo: Meshtastic/wifi_no.txt, par of lines por
// rede (name, senha cifrada com a chave of aparelho - a mesma of wifi.txt of
// Tab5; "-" = rede open). O C6L so enxerga 2,4 GHz, como o Tab5.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_WIFI_REDES_MAX 8

typedef struct {
  char ssid[33];
  char senha[65];     // text puro, so in RAM; vazio = rede open
  bool open;
} mesh_wifi_rede_t;

void     mesh_wifi_no_carregar(void);                    // cartao -> RAM, in subida of servico
int      mesh_wifi_no_n(void);
bool     mesh_wifi_no_rede(int i, mesh_wifi_rede_t* o);  // 0 = a mais recente
// Guarda (ou atualiza) e passa para o topo of lista. Senha vazia = rede open.
bool     mesh_wifi_no_salvar(const char* ssid, const char* senha);
bool     mesh_wifi_no_esquecer(int i);
// Copia para ca as redes guardadas no Tab5 que ainda nao estao in lista.
// Devolve quantas entraram, ou -1 se o Tab5 nao tem none carregada.
int      mesh_wifi_no_trazer_of_tab5(void);
uint32_t mesh_wifi_no_versao(void);
