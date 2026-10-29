// INTERNET LINK (MQTT) - the Meshtastic mesh without the no of radio.
//
// The third link of the Mesh service, on the side of serial (Grove) and BLE. The
// those with internet (gateways) publish what they hear via the MQTT server
// radio, and what arrives there goes down to the radio. Speaking the same protocol,
// the Tab5 talks to the mesh using only Wi-Fi: without C6L enabled, the
// messages continue to arrive, and in an emergency with the internet there is still no way to
// write.
//
// FOR SERVICE, THIS IS A RADIO. It delivers FromRadio and receives ToRadio,
// like BLE; inside translates:
// receiving: ServiceEnvelope{packet, channel_id} -> decrypts the payload with the
// key of channel -> FromRadio{packet: MeshPacket{decoded, ...}};
// sending: ToRadio{packet} -> encrypt Data -> ServiceEnvelope -> publish
// in <root>/2/e/<channel>/!<no>;
// want_config: responds alone with our number, known channels and
// the end of configuration.
//
// THE CONFIGURATION COMES FROM NO. Channels (name and key), region and MQTT server are
// learned from dump of C6L when it is connected (mesh_mqtt_aprender_*)
// and saved in NVS: internet mode uses exactly what it doesn't. Without
// nothing learned: official public server (msh root), LongFast channel with default key.
//
// LIMITS (see tab5_mesh.hpp): only messages of CANAL - a direct message
// encrypted with a public key (PKI) is not decrypted here; on the public server
// that arrives from the internet is not repeated by the mesh (zero hops), so the
// range and who is on MQTT and the radio nodes close to one gateway with
// downlink enabled.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Connects to the server (Wi-Fi must already be connected). Asynchronous.
bool mesh_mqtt_start(void);
void mesh_mqtt_stop(void);
bool mesh_mqtt_active(void);       // started (connected or trying)
bool mesh_mqtt_conectado(void);   // session with the of pe server
bool mesh_mqtt_conectou_now(void);   // true ONCE per new connection

// Same BLE link contract: body of ToRadio/FromRadio, without frame.
bool   mesh_mqtt_send(const uint8_t* toradio, size_t n);
size_t mesh_mqtt_receber(uint8_t* buf, size_t max, uint32_t espera_ms);

// "internet: connected to mqtt.meshtastic.org (msh/BR_902)" etc.
const char* mesh_mqtt_descricao(void);

// Who we are in mesh: the number of our no (the of C6L, if already known - the
// same identity for both paths) and the names that go in NodeInfo.
void mesh_mqtt_set_no(uint32_t num, const char* curto, const char* longo);

// Learned from dump of no (service calls). They only record when they change.
void mesh_mqtt_aprender_canal(int idx, int papel, const char* name, const uint8_t* psk, size_t n_psk);
void mesh_mqtt_aprender_regiao(int regiao);
void mesh_mqtt_aprender_servidor(const char* endereco, const char* user,
                                 const char* senha, const char* raiz);

// -------------------------------------------------------------------------------
// MQTT CLIENT PROXY - the same client, in the other role
// -------------------------------------------------------------------------------
// With ModuleConfig.mqtt.proxy_to_client_enabled enabled, NO does not open connection
// own: delivery on connected client (FromRadio 14, MqttClientProxyMessage
// {1 topic, 2 data, 3 text, 4 retained}) what you would publish, and receive from the client
// (ToRadio 6, the same message) whatever arrives in the subscribed topics. And the "MQTT
// via the cell phone app. Here the Tab5 plays the role of app: it doesn't become a gateway
// complete - what he hears on the radio goes up, what comes from the internet goes down to the
// mesh - without enabling its Wi-Fi (on the C6L, Wi-Fi and Bluetooth do not coexist).
//
// DIFFERENCE FOR anything INTERNET MODE: here the Tab5 DOES NOT encrypt or decrypt. O
// does not publish the finished envelope; Tab5 only carries bytes. That's why the message
// direct with PKI works (who decrypts and the no), and the server and the of NO
// (empty address = mqtt.meshtastic.org, the default of firmware), not the default
// of internet mode.
//
// The two modes divide the client and are exclusive: proxy only with the node
// connected; internet only without it (or with the Internet). Who chooses and
// loop of service (tab5_mesh.cpp). The key is the one of itself in
// (proxy_to_client_enabled), changed in page "No" of app Mesh: enabled and
// confirmed in the dump, Tab5 lends the internet.

// From dump of no: ModuleConfig.mqtt (1 enabled, 9 proxy_to_client_enabled,
// 7 tls_enabled) and ChannelSettings.downlink_enabled (6) of each channel. Only RAM:
// the proxy only exists with the node connected, and the dump comes with each connection.
void mesh_mqtt_aprender_no_mqtt(bool enabled, bool proxy, bool tls);
void mesh_mqtt_aprender_downlink(int idx, bool downlink);
bool mesh_mqtt_no_tem_proxy(void);   // did not come with mqtt and proxy connected

bool mesh_mqtt_start_proxy(void);  // Wi-Fi must already be connected
bool mesh_mqtt_em_proxy(void);       // the client of pe is in the role of proxy

// ASCENT: what he did not order to be published. Call of task of service.
bool   mesh_mqtt_proxy_publicar(const char* topico, const uint8_t* d, size_t n, bool retido);
// DESCENT: next ToRadio{ mqttClientProxyMessage } ready for no;
// 0 = nothing in queue. Call of task of service, no waiting.
size_t mesh_mqtt_proxy_receber(uint8_t* buf, size_t max);
void   mesh_mqtt_proxy_contadores(uint32_t* subiu, uint32_t* desceu, uint32_t* perdeu);

// -------------------------------------------------------------------------------
// PUBLIC NETWORK VIA THE INTERNET - one second client, on the same team as the top one
// -------------------------------------------------------------------------------
// mqtt.meshtastic.org, root msh/US, only LongFast of manufacture. Works in mode
// internet and also with the unconnected (including the proxy): the unconnected
// mesh from here, and this client is of Tab5 alone - it decrypts and encrypts, like the
// internet mode. The service marks everything that exits this queue as of network of
// internet (separately, "LongFast Internet" conversation). Same limits:
// channel only, without PKI.
bool     mesh_mqtt_internet_start(void);   // Wi-Fi must already be connected
void     mesh_mqtt_internet_stop(void);
bool     mesh_mqtt_internet_active(void);
bool     mesh_mqtt_internet_conectado(void);
uint32_t mesh_mqtt_internet_recebidos(void); // packages delivered on service since boot
// FromRadio ready (0 = nothing), no waiting. Call of task of service.
size_t   mesh_mqtt_internet_receber(uint8_t* buf, size_t max);
// ToRadio{ packet } of canal -> published in msh/US/2/e/LongFast/!<no>.
bool     mesh_mqtt_internet_send(const uint8_t* toradio, size_t n);
