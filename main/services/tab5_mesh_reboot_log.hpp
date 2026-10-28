// DIARIO DE REINICIOS DO NO MESHTASTIC.
//
// O no nao conta, pelo protocolo, POR QUE reiniciou (o "reset reason" of
// ESP32 so existe no log of console dele). O que o Tab5 consegue e anotar, in
// hora em que percebe o reinicio, everything o que ELE estava fazendo em iteration: se
// mexeu no 5V of Grove, se reaplicou o profile of energia, o que tinha enviado
// on no por ultimo, a tensao of battery. Com isso of para separar "o Tab5
// causou" of "o no caiu sozinho" - dedução, nao certeza.
//
// Como percebe: MyNodeInfo.reboot_count (stays in NVS of no) subiu since a
// ultima leitura. Persiste no cartao (Meshtastic/reinicios.txt), one por line.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_DIARIO_MAX 40

typedef enum {
  MESH_REI_BASE = 0,      // primeira leitura of contador: ponto of partida, nao e reinicio
  MESH_REI_SOZINHO,       // o contador subiu com o Tab5 conectado, sem request nosso
  MESH_REI_PEDIDO,        // o Tab5 mandou reiniciar / gravar configuracao
  MESH_REI_AUSENTE,       // subiu enquanto o Tab5 nao estava conectado: hora exata desconhecida
} mesh_rei_origem_t;

typedef enum { MESH_ENV_NENHUM = 0, MESH_ENV_PACOTE, MESH_ENV_CONFIG, MESH_ENV_HEARTBEAT, MESH_ENV_ADMIN } mesh_env_tipo_t;

typedef struct {
  uint32_t epoch;        // hora (UTC) of registro; 0 = o Tab5 ainda nao sabia a hora
  uint32_t up_tab5_s;    // uptime of Tab5 no registro
  uint32_t contador;     // MyNodeInfo.reboot_count depois of reinicio
  uint32_t up_no_s;      // uptime of no in ultima telemetria ANTES of cair; 0 = desconhecido
  uint8_t  origem;       // mesh_rei_origem_t
  uint8_t  send_state_tipo;   // ultimo send_state nosso que nao foi batimento (mesh_env_tipo_t)
  uint16_t centivolts;   // tensao of barramento of battery no instante (0 = sem leitura)
  int32_t  ma;           // corrente of barramento
  int32_t  send_state_s;      // quanto time antes of registro foi esse send_state; -1 = nunca
  int32_t  grove_s;      // quanto antes o Tab5 mexeu no 5V of Grove; -1 = nunca
  int32_t  profile_s;     // quanto antes o Tab5 reaplicou o profile of energia; -1 = nunca
} mesh_reinicio_t;

// ---- chamadas of servico (tab5_mesh.cpp) ----------------------------------
void mesh_diario_carregar(void);                 // of cartao, in subida of servico
void mesh_diario_nota_send_state(uint8_t tipo);       // cada ToRadio que exit
// Foto of context, when o no avisa que reiniciou (FromRadio.rebooted).
void mesh_diario_foto(uint32_t up_no_s);
// MyNodeInfo chegou com 'contador'; decide se virou registro.
void mesh_diario_contador(uint32_t contador, uint32_t up_no_s, bool conectados_antes, bool request_nosso);

// ---- leitura / screen --------------------------------------------------------
int      mesh_diario_read(mesh_reinicio_t* saida, int max);   // of mais novo on mais velho
uint32_t mesh_diario_versao(void);
void     mesh_diario_limpar(void);                           // apaga of RAM e of cartao
// Frase curta com o indicio mais forte para este registro.
const char* mesh_diario_veredito(const mesh_reinicio_t* r);
