// ENLACE PELA INTERNET (MQTT) - a malha Meshtastic sem o no de radio.
//
// O terceiro enlace do servico Mesh, ao lado da serial (Grove) e do BLE. Os
// nos com internet (gateways) publicam num servidor MQTT o que ouvem pelo
// radio, e descem para o radio o que chega por la. Falando o mesmo protocolo,
// o Tab5 conversa com a malha usando so o Wi-Fi: sem o C6L ligado, as
// mensagens continuam chegando, e numa emergencia com internet ainda da para
// escrever.
//
// PARA O SERVICO, ISTO E UM RADIO. Ele entrega FromRadio e recebe ToRadio,
// como o BLE; por dentro faz a traducao:
//   recebendo: ServiceEnvelope{packet, channel_id} -> decifra o payload com a
//              chave do canal -> FromRadio{packet: MeshPacket{decoded, ...}};
//   enviando:  ToRadio{packet} -> cifra o Data -> ServiceEnvelope -> publica
//              em <raiz>/2/e/<canal>/!<no>;
//   want_config: responde sozinho com o nosso numero, os canais conhecidos e
//              o fim da configuracao.
//
// A CONFIGURACAO VEM DO NO. Canais (nome e chave), regiao e servidor MQTT sao
// aprendidos do dump do C6L quando ele esta conectado (mesh_mqtt_aprender_*)
// e guardados na NVS: o modo internet usa exatamente o que o no usa. Sem
// nada aprendido: servidor publico oficial (raiz msh), canal LongFast com a chave padrao.
//
// LIMITES (ver tab5_mesh.hpp): so mensagens de CANAL - a mensagem direta
// cifrada com chave publica (PKI) nao e decifrada aqui; no servidor publico o
// que chega da internet nao e repetido pela malha (zero saltos), entao o
// alcance e quem esta no MQTT e os nos de radio perto de um gateway com
// downlink ligado.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Conecta ao servidor (o Wi-Fi ja tem que estar conectado). Assincrono.
bool mesh_mqtt_iniciar(void);
void mesh_mqtt_parar(void);
bool mesh_mqtt_ativo(void);       // iniciado (conectado ou tentando)
bool mesh_mqtt_conectado(void);   // sessao com o servidor de pe
bool mesh_mqtt_conectou_agora(void);   // true UMA vez por conexao nova

// Mesmo contrato do enlace BLE: corpo de ToRadio/FromRadio, sem frame.
bool   mesh_mqtt_enviar(const uint8_t* toradio, size_t n);
size_t mesh_mqtt_receber(uint8_t* buf, size_t max, uint32_t espera_ms);

// "internet: conectado a mqtt.meshtastic.org (msh/BR_902)" etc.
const char* mesh_mqtt_descricao(void);

// Quem somos na malha: o numero do nosso no (o do C6L, se ja conhecido - a
// mesma identidade pelos dois caminhos) e os nomes que vao no NodeInfo.
void mesh_mqtt_definir_no(uint32_t num, const char* curto, const char* longo);

// Aprendidos do dump do no (chamados pelo servico). So gravam quando mudam.
void mesh_mqtt_aprender_canal(int idx, int papel, const char* nome, const uint8_t* psk, size_t n_psk);
void mesh_mqtt_aprender_regiao(int regiao);
void mesh_mqtt_aprender_servidor(const char* endereco, const char* usuario,
                                 const char* senha, const char* raiz);

// ---------------------------------------------------------------------------
// MQTT CLIENT PROXY - o mesmo cliente, no outro papel
// ---------------------------------------------------------------------------
// Com ModuleConfig.mqtt.proxy_to_client_enabled ligado, o NO nao abre conexao
// propria: entrega ao cliente conectado (FromRadio 14, MqttClientProxyMessage
// {1 topic, 2 data, 3 text, 4 retained}) o que publicaria, e recebe do cliente
// (ToRadio 6, a mesma mensagem) o que chegar nos topicos assinados. E o "MQTT
// pelo app" do celular. Aqui o Tab5 faz o papel do app: o no vira um gateway
// completo - o que ele ouve no radio sobe, o que vem da internet desce para a
// malha - sem ligar o Wi-Fi dele (no C6L, Wi-Fi e Bluetooth nao convivem).
//
// DIFERENCA PARA O MODO INTERNET: aqui o Tab5 NAO cifra nem decifra nada. O
// no publica o envelope pronto; o Tab5 so carrega bytes. Por isso a mensagem
// direta com PKI funciona (quem decifra e o no), e o servidor e o do NO
// (endereco vazio = mqtt.meshtastic.org, o padrao do firmware), nao o padrao
// do modo internet.
//
// Os dois modos dividem o cliente e sao exclusivos: proxy so com o no
// conectado; internet so sem ele (ou com a via Internet). Quem escolhe e o
// laco do servico (tab5_mesh.cpp). A chave e a do proprio no
// (proxy_to_client_enabled), mexida na pagina "No" do app Mesh: ligada e
// confirmada no dump, o Tab5 empresta a internet.

// Do dump do no: ModuleConfig.mqtt (1 enabled, 9 proxy_to_client_enabled,
// 7 tls_enabled) e ChannelSettings.downlink_enabled (6) de cada canal. So RAM:
// o proxy so existe com o no conectado, e o dump vem a cada conexao.
void mesh_mqtt_aprender_no_mqtt(bool ligado, bool proxy, bool tls);
void mesh_mqtt_aprender_downlink(int idx, bool downlink);
bool mesh_mqtt_no_tem_proxy(void);   // o no veio com mqtt e proxy ligados

bool mesh_mqtt_iniciar_proxy(void);  // o Wi-Fi ja tem que estar conectado
bool mesh_mqtt_em_proxy(void);       // o cliente de pe esta no papel de proxy

// SUBIDA: o que o no mandou publicar. Chamada da tarefa do servico.
bool   mesh_mqtt_proxy_publicar(const char* topico, const uint8_t* d, size_t n, bool retido);
// DESCIDA: o proximo ToRadio{ mqttClientProxyMessage } pronto para o no;
// 0 = nada na fila. Chamada da tarefa do servico, sem espera.
size_t mesh_mqtt_proxy_receber(uint8_t* buf, size_t max);
void   mesh_mqtt_proxy_contadores(uint32_t* subiu, uint32_t* desceu, uint32_t* perdeu);

// ---------------------------------------------------------------------------
// REDE PUBLICA PELA INTERNET - um segundo cliente, ao mesmo tempo que o de cima
// ---------------------------------------------------------------------------
// mqtt.meshtastic.org, raiz msh/US, so o LongFast de fabrica. Funciona no modo
// internet e tambem com o no conectado (no proxy inclusive): o no fala com a
// malha daqui, e este cliente e do Tab5 sozinho - ele decifra e cifra, como o
// modo internet. O servico marca tudo o que sai desta fila como da rede de
// internet (nos separados, conversa "LongFast Internet"). Mesmos limites:
// so canal, sem PKI.
bool     mesh_mqtt_internet_iniciar(void);   // o Wi-Fi ja tem que estar conectado
void     mesh_mqtt_internet_parar(void);
bool     mesh_mqtt_internet_ativo(void);
bool     mesh_mqtt_internet_conectado(void);
uint32_t mesh_mqtt_internet_recebidos(void); // pacotes entregues ao servico desde o boot
// FromRadio pronto (0 = nada), sem espera. Chamada da tarefa do servico.
size_t   mesh_mqtt_internet_receber(uint8_t* buf, size_t max);
// ToRadio{ packet } de canal -> publicado em msh/US/2/e/LongFast/!<no>.
bool     mesh_mqtt_internet_enviar(const uint8_t* toradio, size_t n);
