// DIARIO DE REINICIOS DO NO MESHTASTIC.
//
// O no nao conta, pelo protocolo, POR QUE reiniciou (o "reset reason" do
// ESP32 so existe no log de console dele). O que o Tab5 consegue e anotar, na
// hora em que percebe o reinicio, tudo o que ELE estava fazendo em volta: se
// mexeu no 5V do Grove, se reaplicou o perfil de energia, o que tinha enviado
// ao no por ultimo, a tensao da bateria. Com isso da para separar "o Tab5
// causou" de "o no caiu sozinho" - dedução, nao certeza.
//
// Como percebe: MyNodeInfo.reboot_count (fica na NVS do no) subiu desde a
// ultima leitura. Persiste no cartao (Meshtastic/reinicios.txt), um por linha.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MESH_DIARIO_MAX 40

typedef enum {
  MESH_REI_BASE = 0,      // primeira leitura do contador: ponto de partida, nao e reinicio
  MESH_REI_SOZINHO,       // o contador subiu com o Tab5 conectado, sem pedido nosso
  MESH_REI_PEDIDO,        // o Tab5 mandou reiniciar / gravar configuracao
  MESH_REI_AUSENTE,       // subiu enquanto o Tab5 nao estava conectado: hora exata desconhecida
} mesh_rei_origem_t;

typedef enum { MESH_ENV_NENHUM = 0, MESH_ENV_PACOTE, MESH_ENV_CONFIG, MESH_ENV_HEARTBEAT, MESH_ENV_ADMIN } mesh_env_tipo_t;

typedef struct {
  uint32_t epoch;        // hora (UTC) do registro; 0 = o Tab5 ainda nao sabia a hora
  uint32_t up_tab5_s;    // uptime do Tab5 no registro
  uint32_t contador;     // MyNodeInfo.reboot_count depois do reinicio
  uint32_t up_no_s;      // uptime do no na ultima telemetria ANTES de cair; 0 = desconhecido
  uint8_t  origem;       // mesh_rei_origem_t
  uint8_t  envio_tipo;   // ultimo envio nosso que nao foi batimento (mesh_env_tipo_t)
  uint16_t centivolts;   // tensao do barramento da bateria no instante (0 = sem leitura)
  int32_t  ma;           // corrente do barramento
  int32_t  envio_s;      // quanto tempo antes do registro foi esse envio; -1 = nunca
  int32_t  grove_s;      // quanto antes o Tab5 mexeu no 5V do Grove; -1 = nunca
  int32_t  perfil_s;     // quanto antes o Tab5 reaplicou o perfil de energia; -1 = nunca
} mesh_reinicio_t;

// ---- chamadas do servico (tab5_mesh.cpp) ----------------------------------
void mesh_diario_carregar(void);                 // do cartao, na subida do servico
void mesh_diario_nota_envio(uint8_t tipo);       // cada ToRadio que sai
// Foto do contexto, quando o no avisa que reiniciou (FromRadio.rebooted).
void mesh_diario_foto(uint32_t up_no_s);
// MyNodeInfo chegou com 'contador'; decide se virou registro.
void mesh_diario_contador(uint32_t contador, uint32_t up_no_s, bool conectados_antes, bool pedido_nosso);

// ---- leitura / tela --------------------------------------------------------
int      mesh_diario_ler(mesh_reinicio_t* saida, int max);   // do mais novo ao mais velho
uint32_t mesh_diario_versao(void);
void     mesh_diario_limpar(void);                           // apaga da RAM e do cartao
// Frase curta com o indicio mais forte para este registro.
const char* mesh_diario_veredito(const mesh_reinicio_t* r);
