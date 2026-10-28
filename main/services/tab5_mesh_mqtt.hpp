// ENLACE PELA INTERNET (MQTT) - a malha Meshtastic sem o no of radio.
//
// O terceiro enlace of servico Mesh, on lado of serial (Grove) e of BLE. Os
// nos com internet (gateways) publicam num servidor MQTT o que ouvem pelo
// radio, e descem para o radio o que chega por la. Falando o mesmo protocolo,
// o Tab5 conversa com a malha usando so o Wi-Fi: sem o C6L enabled, as
// mensagens continuam chegando, e numa emergencia com internet ainda of para
// write.
//
// PARA O SERVICO, ISTO E UM RADIO. Ele entrega FromRadio e recebe ToRadio,
// como o BLE; por dentro faz a traducao:
//   recebendo: ServiceEnvelope{packet, channel_id} -> decifra o payload com a
//              chave of canal -> FromRadio{packet: MeshPacket{decoded, ...}};
//   enviando:  ToRadio{packet} -> cifra o Data -> ServiceEnvelope -> publica
//              em <raiz>/2/e/<canal>/!<no>;
//   want_config: responde sozinho com o nosso numero, os canais conhecidos e
//              o end of configuracao.
//
// A CONFIGURACAO VEM DO NO. Canais (name e chave), regiao e servidor MQTT sao
// aprendidos of dump of C6L when ele esta conectado (mesh_mqtt_aprender_*)
// e guardados in NVS: o modo internet usa exatamente o que o no usa. Sem
// nada aprendido: servidor publico oficial (raiz msh), canal LongFast com a chave default.
//
// LIMITES (ver tab5_mesh.hpp): so mensagens of CANAL - a message direta
// cifrada com chave publica (PKI) nao e decifrada aqui; no servidor publico o
// que chega of internet nao e repetido pela malha (zero saltos), entao o
// alcance e quem esta no MQTT e os nos of radio perto of one gateway com
// downlink enabled.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Conecta on servidor (o Wi-Fi ja tem que estar conectado). Assincrono.
bool mesh_mqtt_start(void);
void mesh_mqtt_stop(void);
bool mesh_mqtt_active(void);       // iniciado (conectado ou tentando)
bool mesh_mqtt_conectado(void);   // sessao com o servidor of pe
bool mesh_mqtt_conectou_now(void);   // true UMA vez por connection nova

// Mesmo contrato of enlace BLE: body of ToRadio/FromRadio, sem frame.
bool   mesh_mqtt_send(const uint8_t* toradio, size_t n);
size_t mesh_mqtt_receber(uint8_t* buf, size_t max, uint32_t espera_ms);

// "internet: conectado a mqtt.meshtastic.org (msh/BR_902)" etc.
const char* mesh_mqtt_descricao(void);

// Quem somos in malha: o numero of nosso no (o of C6L, se ja conhecido - a
// mesma identidade pelos dois caminhos) e os nomes que vao no NodeInfo.
void mesh_mqtt_set_no(uint32_t num, const char* curto, const char* longo);

// Aprendidos of dump of no (chamados pelo servico). So gravam when mudam.
void mesh_mqtt_aprender_canal(int idx, int papel, const char* name, const uint8_t* psk, size_t n_psk);
void mesh_mqtt_aprender_regiao(int regiao);
void mesh_mqtt_aprender_servidor(const char* endereco, const char* user,
                                 const char* senha, const char* raiz);

// ---------------------------------------------------------------------------
// MQTT CLIENT PROXY - o mesmo cliente, no outro papel
// ---------------------------------------------------------------------------
// Com ModuleConfig.mqtt.proxy_to_client_enabled enabled, o NO nao abre connection
// propria: entrega on cliente conectado (FromRadio 14, MqttClientProxyMessage
// {1 topic, 2 data, 3 text, 4 retained}) o que publicaria, e recebe of cliente
// (ToRadio 6, a mesma message) o que chegar nos topicos assinados. E o "MQTT
// pelo app" of celular. Aqui o Tab5 faz o papel of app: o no vira one gateway
// complete - o que ele ouve no radio sobe, o que vem of internet desce para a
// malha - sem enable o Wi-Fi dele (no C6L, Wi-Fi e Bluetooth nao convivem).
//
// DIFERENCA PARA O MODO INTERNET: aqui o Tab5 NAO cifra nem decifra nada. O
// no publica o envelope pronto; o Tab5 so carrega bytes. Por isso a message
// direta com PKI funciona (quem decifra e o no), e o servidor e o of NO
// (endereco vazio = mqtt.meshtastic.org, o default of firmware), nao o default
// of modo internet.
//
// Os dois modos dividem o cliente e sao exclusivos: proxy so com o no
// conectado; internet so sem ele (ou com a via Internet). Quem escolhe e o
// laco of servico (tab5_mesh.cpp). A chave e a of proprio no
// (proxy_to_client_enabled), mexida in pagina "No" of app Mesh: enabled e
// confirmada no dump, o Tab5 empresta a internet.

// Do dump of no: ModuleConfig.mqtt (1 enabled, 9 proxy_to_client_enabled,
// 7 tls_enabled) e ChannelSettings.downlink_enabled (6) of cada canal. So RAM:
// o proxy so existe com o no conectado, e o dump vem a cada connection.
void mesh_mqtt_aprender_no_mqtt(bool enabled, bool proxy, bool tls);
void mesh_mqtt_aprender_downlink(int idx, bool downlink);
bool mesh_mqtt_no_tem_proxy(void);   // o no veio com mqtt e proxy ligados

bool mesh_mqtt_start_proxy(void);  // o Wi-Fi ja tem que estar conectado
bool mesh_mqtt_em_proxy(void);       // o cliente of pe esta no papel of proxy

// SUBIDA: o que o no mandou publicar. Chamada of tarefa of servico.
bool   mesh_mqtt_proxy_publicar(const char* topico, const uint8_t* d, size_t n, bool retido);
// DESCIDA: o next ToRadio{ mqttClientProxyMessage } pronto para o no;
// 0 = nada in fila. Chamada of tarefa of servico, sem espera.
size_t mesh_mqtt_proxy_receber(uint8_t* buf, size_t max);
void   mesh_mqtt_proxy_contadores(uint32_t* subiu, uint32_t* desceu, uint32_t* perdeu);

// ---------------------------------------------------------------------------
// REDE PUBLICA PELA INTERNET - one segundo cliente, on mesmo time que o of top
// ---------------------------------------------------------------------------
// mqtt.meshtastic.org, raiz msh/US, so o LongFast of fabrica. Funciona no modo
// internet e tambem com o no conectado (no proxy inclusive): o no fala com a
// malha daqui, e este cliente e of Tab5 sozinho - ele decifra e cifra, como o
// modo internet. O servico marca everything o que exit desta fila como of rede of
// internet (nos separados, conversa "LongFast Internet"). Mesmos limites:
// so canal, sem PKI.
bool     mesh_mqtt_internet_start(void);   // o Wi-Fi ja tem que estar conectado
void     mesh_mqtt_internet_stop(void);
bool     mesh_mqtt_internet_active(void);
bool     mesh_mqtt_internet_conectado(void);
uint32_t mesh_mqtt_internet_recebidos(void); // pacotes entregues on servico since o boot
// FromRadio pronto (0 = nada), sem espera. Chamada of tarefa of servico.
size_t   mesh_mqtt_internet_receber(uint8_t* buf, size_t max);
// ToRadio{ packet } of canal -> publicado em msh/US/2/e/LongFast/!<no>.
bool     mesh_mqtt_internet_send(const uint8_t* toradio, size_t n);
