// REDES DO WI-FI DO NO MESHTASTIC, guardadas no cartao.
//
// O no so tem UMA rede configurada por vez (Config.network), e trocar exigia
// digitar nome e senha de novo. Aqui ficam as redes que ja foram gravadas nele,
// para escolher numa lista. Arquivo: Meshtastic/wifi_no.txt, par de linhas por
// rede (nome, senha cifrada com a chave do aparelho - a mesma do wifi.txt do
// Tab5; "-" = rede aberta). O C6L so enxerga 2,4 GHz, como o Tab5.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_WIFI_REDES_MAX 8

typedef struct {
  char ssid[33];
  char senha[65];     // texto puro, so na RAM; vazio = rede aberta
  bool aberta;
} mesh_wifi_rede_t;

void     mesh_wifi_no_carregar(void);                    // cartao -> RAM, na subida do servico
int      mesh_wifi_no_n(void);
bool     mesh_wifi_no_rede(int i, mesh_wifi_rede_t* o);  // 0 = a mais recente
// Guarda (ou atualiza) e passa para o topo da lista. Senha vazia = rede aberta.
bool     mesh_wifi_no_salvar(const char* ssid, const char* senha);
bool     mesh_wifi_no_esquecer(int i);
// Copia para ca as redes guardadas no Tab5 que ainda nao estao na lista.
// Devolve quantas entraram, ou -1 se o Tab5 nao tem nenhuma carregada.
int      mesh_wifi_no_trazer_do_tab5(void);
uint32_t mesh_wifi_no_versao(void);
