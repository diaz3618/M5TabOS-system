// CONTRATO entre o P4 (M5tabOS) e o firmware do C6 (pasta c6/).
//
// Mora em shared/ e nao em main/ por um reason concreto: sao DOIS
// projetos IDF, para DOIS chips, compilados separadamente. Se cada lado tiver a
// sua copia dos numeros, um dia eles discordam e o compilador nao avisa - ja
// aconteceu nesta mesma etapa, com o prazo da carencia escrito 90 num lado e
// 120 no outro. Aqui os dois incluem o MESMO arquivo.
//
// Este arquivo existe para os dois lados NAO se desencontrarem. Os numeros sao
// combinados, viajam crus pelo canal peer_data do esp_hosted, e nao ha
// negociacao none - se um lado mudar sozinho, o outro so vai ver message
// desconhecida e silencio.
//
// Formato dos corpos, sempre cru - nada de protobuf. O canal ja entrega
// (msg_id, bytes, tamanho), e serializar em cima disso seria trabalho sem
// retorno when os dois lados sao nossos.

#pragma once

#include <stdint.h>

// P4 -> C6
#define C6_MSG_HOST_VIVO    1   // sem body: "estou te ouvindo"
#define C6_MSG_HOST_ENVIAR  2   // 6B MAC destino + texto
#define C6_MSG_HOST_CANAL   3   // 1B canal WiFi
#define C6_MSG_HOST_ESPNOW  4   // sem body: "o WiFi ja subiu, pode enable o ESP-NOW"
#define C6_MSG_HOST_FAREJO  5   // c6_farejo_cfg_t: liga/renova/desliga o modo promiscuo
#define C6_MSG_HOST_CRU     6   // 6B MAC destino + bytes: ESP-NOW sem cabecalho nenhum, uma vez
#define C6_MSG_HOST_AJUSTE  7   // 1B chave (C6_AJ_*) + valor
#define C6_MSG_HOST_RADIO   8   // 1B sub (C6_RX_*) + config; o 8o e ULTIMO handler

// C6 -> P4
#define C6_MSG_PRONTO      10   // 6B MAC proprio + 1B canal
#define C6_MSG_RECEBIDO    11   // 6B MAC origem + 1B marca (bit0 difusao, bit1 formato Espressif) + texto
#define C6_MSG_ESTADO      12   // 6B MAC destino + 1B ok
#define C6_MSG_FAREJO      13   // c6_farejo_lote_t + n x c6_farejo_reg_t
#define C6_MSG_RECEBIDO_CRU 14  // 6B MAC origem + 1B rssi + 1B canal + 1B marca (bit0 difusao) + bytes crus
#define C6_MSG_INFO        15   // c6_info_t
#define C6_MSG_CSI         16   // c6_csi_cab_t + bytes crus de CSI
#define C6_MSG_FTM         17   // c6_ftm_rep_t
#define C6_MSG_154         18   // c6_154_cab_t + bytes crus do frame 802.15.4

// QUANTOS msg_id CADA LADO ESCUTA. O peer_data do C6 tem tabela FIXA de 8
// callbacks (EH_CP_FEAT_PEER_DATA_MAX_HANDLERS); o 9o registro falha calado e
// aquela message passa a ser ignorada. Hoje o C6 escuta 8 (1..8): tabela cheia. Comando novo
// do P4 para o C6 entra como CHAVE do C6_MSG_HOST_AJUSTE, nao como msg_id novo.
// O sentido C6 -> P4 nao tem esse teto (o host cresce a tabela).

// VERSAO DO CONTRATO. Vai no PRONTO (byte 7) e no INFO. Firmware antigo do C6
// manda o PRONTO com 7 bytes so: quem le trata "sem byte 7" como versao 1.
//   1 - ESP-NOW (texto, dois dialetos) e canal
//   2 - + farejador promiscuo, ESP-NOW cru, INFO
//   3 - + CSI, FTM, 802.15.4 (cada um sob pedido do P4)
#define C6_PROTO_VERSAO    3
#define C6_CAP_ESPNOW      0x01
#define C6_CAP_FAREJO      0x02
#define C6_CAP_CRU         0x04
#define C6_CAP_INFO        0x08
// Estes 3 so acendem se o modulo foi COMPILADO no C6 (depende do sdkconfig do
// c6/). Firmware gravado sem um deles deixa o bit zerado, e o P4 esconde a
// funcao - e como as evolucoes ficam do lado do P4 sem reabrir o aparelho.
#define C6_CAP_CSI         0x10
#define C6_CAP_FTM         0x20
#define C6_CAP_154         0x40
// PRONTO v2: 6B MAC + 1B canal + 1B C6_PROTO_VERSAO + 1B C6_CAP_*

// --- C6_MSG_HOST_AJUSTE: chaves --------------------------------------------
#define C6_AJ_INFO         1    // sem valor: responde C6_MSG_INFO
#define C6_AJ_CRU          2    // 1B: 1 = repassar todo ESP-NOW recebido tambem como C6_MSG_RECEBIDO_CRU

// --- C6_MSG_HOST_RADIO: sub-comandos ---------------------------------------
// Os extras do radio, cada um enabled/desligado por aqui. Todos valem a MESMA
// regra do farejo: o receptor fica enabled enquanto o modo esta no ar, e cada um
// DESLIGA SOZINHO se o P4 nao reenviar a config em C6_RADIO_VALIDADE_MS (um app
// que fechou nao pode deixar o radio gastando). FTM e a excecao: e um disparo
// unico, sem renovacao.
#define C6_RX_CSI          1    // c6_csi_cfg_t
#define C6_RX_FTM          2    // c6_ftm_cfg_t (disparo unico)
#define C6_RX_154          3    // c6_154_cfg_t
#define C6_RADIO_VALIDADE_MS 10000

// --- FAREJADOR (modo promiscuo) ----------------------------------------------
// O esp_hosted NAO repassa o modo promiscuo (os IDs existem no rpc_v2.proto,
// mas nao ha implementacao nos dois lados). Aqui ele roda NO C6 e o P4 recebe
// so o RESUMO: o C6 agrega por MAC durante `lote_ms` e manda um lote. Um canal
// movimentado passa de mil quadros por segundo; frame a frame pelo SDIO seria
// mil interrupcoes, mallocs e eventos por segundo no P4. Em lote sao 4.
//
// VALIDADE: o farejo DESLIGA SOZINHO se o P4 nao reenviar a configuracao em
// C6_FAREJO_VALIDADE_MS. O promiscuo deixa o receptor enabled o time todo (sem
// modem sleep); um app que fechou ou travou nao pode deixar o radio gastando.
// Reenviar a MESMA configuracao so renova, nao reinicia o salto de canal.
//
// CANAIS: `canais` e mascara (bit N = canal N). Zero = fica no canal current.
// Com mais de um bit o C6 salta de canal a cada `permanencia_ms` - mas SO solto
// do roteador (tab5_wifi_radio_only_mode): associado, switch de canal derruba o
// WiFi, entao o C6 fica no canal do roteador e ignora a mascara. Ao disable,
// iteration ao canal em que estava.
#define C6_FAREJO_VALIDADE_MS 10000
#define C6_FAREJO_LOTE_MAX    48     // registros por message (48 x 12 + 6 = 582 B)

#define C6_FJ_MGMT    0x01   // filtro: quadros de gerencia (beacon, probe...)
#define C6_FJ_DADOS   0x02   // filtro: quadros de data

typedef struct __attribute__((packed)) {
  uint8_t  liga;             // 0 = desliga ja
  uint8_t  filtro;           // C6_FJ_*; 0 = os dois
  uint16_t canais;           // mascara; 0 = canal current
  uint16_t permanencia_ms;   // por canal no salto; 0 = 200
  uint16_t lote_ms;          // periodo do lote; 0 = 250 (minimum 50)
  int8_t   rssi_min;         // descarta mais fraco que isto; 0 = aceita tudo
  uint8_t  reserva;
  uint8_t  alvo[6];          // so este MAC; zeros = todos
} c6_farejo_cfg_t;           // 16 B

// O que cada MAC fez no periodo (bits de c6_farejo_reg_t.tipos)
#define C6_FT_BEACON   0x01  // anunciou rede (beacon / probe response): e um AP
#define C6_FT_PROCURA  0x02  // probe request: aparelho procurando rede (celular!)
#define C6_FT_DADOS    0x04  // frame de data
#define C6_FT_PARA_AP  0x08  // data com ToDS: e uma ESTACAO falando com o AP
#define C6_FT_DO_AP    0x10  // data com FromDS: e o AP falando
#define C6_FT_GERENCIA 0x20  // outra gerencia (auth, assoc, deauth, acao)

typedef struct __attribute__((packed)) {
  uint8_t  channel;            // canal do radio when o lote fechou
  uint8_t  n;                // registros que seguem
  uint16_t perdidos;         // MACs que nao couberam na tabela do periodo
  uint16_t frames;          // quadros vistos no periodo (satura em 65535)
} c6_farejo_lote_t;          // 6 B

typedef struct __attribute__((packed)) {
  uint8_t  mac[6];           // transmissor (addr2)
  int8_t   rssi_ult;         // ultimo
  int8_t   rssi_max;
  int8_t   rssi_med;         // media do periodo
  uint8_t  n;                // quadros do periodo (satura em 255)
  uint8_t  tipos;            // C6_FT_*
  uint8_t  channel;            // canal onde foi ouvido
} c6_farejo_reg_t;           // 12 B

typedef struct __attribute__((packed)) {
  uint8_t  proto;            // C6_PROTO_VERSAO
  uint8_t  caps;             // C6_CAP_*
  uint8_t  channel;
  uint8_t  farejando;        // 1 = promiscuo enabled now
  uint32_t heap_livre;
  uint32_t heap_min;
  uint32_t no_ar_s;          // since o boot do C6
  uint32_t frames;          // farejados since o boot
  // Diagnostico dos extras do radio (so firmware que os traz; o P4 aceita o
  // INFO curto de um C6 mais velho e deixa isto zerado).
  int16_t  csi_err;          // primeiro esp_err_t nao-OK ao enable o CSI (0 = tudo OK)
  int16_t  z154_err;         // idem para o 802.15.4
  uint32_t csi_cb;           // vezes que o driver chamou o callback do CSI
  uint32_t csi_env;          // records de CSI entregues a fila para o P4
  uint32_t z154_rx;          // quadros 802.15.4 recebidos pelo radio (antes do filtro de ritmo)
  int16_t  csi_err2;         // resultado da ULTIMA vez que o CSI foi enabled (0 = ligou)
  uint8_t  csi_etapa;        // onde parou: 1 config, 2 callback, 3 enable
  uint8_t  csi_modo;         // 1 = CSI enabled now, 0 = desligado
  // (ESP_FAIL no csi_err = o WiFi do C6 foi iniciado com csi_enable 0. Quem
  // manda esse field e o P4, no esp_wifi_init - ver main/hal/tab5_wifi.cpp.)
  uint32_t saida_perdas;     // mensagens descartadas por saida cheia (o P4 nao consumiu a time)
  uint8_t  z154_estado;      // 0 nunca enabled, 1 ouvindo, 2 dormindo (driver habilitado, RF solto)
  uint8_t  chip_rev;         // 0x80 | maior<<4 | menor (0 = firmware que nao informa)
  uint8_t  flags;            // C6_INFO_F_* (firmware c6-4 em diante; antes era reserva = 0)
  uint8_t  reserva_info;
} c6_info_t;                 // 48 B (firmware c6-3; o P4 aceita os mais curtos)
#define C6_INFO_V1_TAM 20
#define C6_INFO_F_COREDUMP 0x01   // ha retrato de travamento guardado na particao coredump do C6
#define C6_INFO_F_EM_PROVA 0x02   // imagem nova (OTA) ainda nao confirmada pelo VIVO    // tamanho do INFO de quem nao traz o diagnostics

// --- CSI (Channel State Information) ------------------------------------------
// O estado do canal por frame recebido: amplitude/fase das subportadoras. E o
// que da "precisao absurda" para presenca e movimento - o default muda when
// algo entre o emissor e o C6 se mexe, mesmo sem o aparelho na mao. O C6 entrega
// o CSI CRU; a conta (variancia, correlacao) fica no P4.
//
// FLOOD: um AP movimentado gera muito CSI. `min_intervalo_ms` descarta no C6 os
// quadros que vem rapido demais - um a cada intervalo basta para movimento. O
// filtro por MAC (so o alvo) corta o resto.
typedef struct __attribute__((packed)) {
  uint8_t  liga;
  uint8_t  so_alvo;          // 1 = so o MAC alvo
  uint8_t  alvo[6];
  uint16_t min_intervalo_ms; // minimum entre records enviados; 0 = 100
  int8_t   rssi_min;         // descarta mais fraco; 0 = tudo
  uint8_t  reserva;
} c6_csi_cfg_t;              // 12 B

// QUE QUADROS GERAM CSI. O CSI e medido nos campos de treino OFDM do preambulo
// (L-LTF, HT-LTF, HE-LTF). Beacon de 2,4 GHz costuma sair a 1 Mbps (11b, DSSS),
// que nao tem nenhum deles: nao gera CSI. Sem modo promiscuo, o C6 so ve os
// quadros endereçados a ele - com o WiFi parado, quase nada. Para um fluxo
// constante, quem le precisa provocar trafego (o 'radio csi' pinga o roteador,
// como o esp-csi da Espressif); para CSI de outros aparelhos, farejo enabled.
//
// O driver do C6 entrega ate 1023 B (rx_channel_estimate_len tem 10 bits):
// L-LTF 128 B, HT20 ate 256, HT40 e HE20 mais. O teto abaixo cobre o maximum, e
// o bit CORTADO avisa se um dia nao cover.
#define C6_CSI_MAX 1024
typedef struct __attribute__((packed)) {
  uint8_t  mac[6];           // de quem veio
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  sec_canal;
  uint8_t  flags;            // C6_CSI_F_* e o formato do frame (C6_CSI_FORMATO)
  uint16_t len;              // bytes de CSI que seguem (<= C6_CSI_MAX)
  uint32_t t_us;             // carimbo de recepcao do C6
} c6_csi_cab_t;             // 16 B
#define C6_CSI_F_HT40      0x01   // ha canal secundario (40 MHz)
#define C6_CSI_F_W0_RUIM   0x02   // os 4 primeiros bytes sao invalidos (limit do hardware: first_word_invalid)
#define C6_CSI_F_CORTADO   0x04   // o driver entregou mais que C6_CSI_MAX
// Formato do frame, bits 4..7 (cur_bb_format do driver): 0 11b, 1 11g,
// 2 HT, 3 VHT, 4 HE SU, 5 HE MU, 6 HE ER SU, 7 HE TB.
#define C6_CSI_FORMATO(f)  ((uint8_t)((f) >> 4))

// --- FTM (Fine Timing Measurement, 802.11mc) ---------------------------------
// Distancia por time de voo, contra um AP que fale FTM (responder). Disparo
// unico: o P4 pede, o C6 roda uma sessao e responde com o relatorio.
typedef struct __attribute__((packed)) {
  uint8_t  peer[6];          // AP responder
  uint8_t  channel;
  uint8_t  frm_count;        // quadros na rajada: 0(auto),8,16,24,32,64
  uint16_t burst_period;     // unidades de 100 ms; 0 = auto
} c6_ftm_cfg_t;             // 10 B

typedef struct __attribute__((packed)) {
  uint8_t  peer[6];
  uint8_t  status;           // 0 = sucesso
  uint8_t  num;              // quadros usados
  uint32_t rtt_est_ns;       // time de voo ida-e-volta estimado
  uint32_t dist_est_cm;      // distancia estimada pelo controlador
  uint32_t rtt_raw_ns;       // com status 0xFF: o esp_err_t do initiate_session
} c6_ftm_rep_t;             // 20 B
// status: 0..6 = wifi_ftm_status_t do driver (0 sucesso, 1 sem suporte no
// outro lado, 2 recusado, 3 sem resposta, 4 falha, 5 sem medida valida, 6
// encerrado). 0xFF = o C6 nem conseguiu start - o reason vai em rtt_raw_ns.
#define C6_FTM_NAO_INICIOU 0xFF

// --- 802.15.4 (o outro radio do C6: Thread / Zigbee / cru) -------------------
// O C6 tem um radio IEEE 802.15.4 alem do WiFi/BT. Em recepcao crua ele ouve o
// trafego de malhas Thread e Zigbee no canal pedido (11..26). Coexiste com o
// WiFi pelo mesmo arbitro do BLE (o C6 liga esp_coex_wifi_i154_enable). O C6 so repassa os quadros crus; decodificar
// e do P4.
typedef struct __attribute__((packed)) {
  uint8_t  liga;
  uint8_t  channel;            // 11..26
  uint8_t  promiscuo;        // 1 = sem filtro de endereco (ouve a malha toda); 0 = so
                             // quadros para o PAN/endereco do C6, que nao tem nenhum: quase nada
  uint8_t  reserva;
  uint16_t min_intervalo_ms; // afunila o repasse; 0 = sem limit
  uint16_t reserva2;
} c6_154_cfg_t;             // 8 B

#define C6_154_MAX 127       // tamanho maximum de um frame 802.15.4
typedef struct __attribute__((packed)) {
  uint8_t  channel;
  int8_t   rssi;
  uint8_t  lqi;              // qualidade do enlace (0..255)
  uint8_t  len;              // bytes do frame que seguem (<= C6_154_MAX)
  uint32_t t_us;
} c6_154_cab_t;             // 8 B

// Limite do ESP-NOW v1.0, que e o que os ESP32 e ESP32-S3 falam (Cardputer,
// Basic, StickC, M5Paper). O v2.0 vai a 1470, mas SO se os dois lados
// suportarem - e nao suportam.
#define C6_TEXTO_MAX      250

// Quanto o C6 espera pelo "estou te ouvindo" do P4 depois de subir com firmware
// novo. Sem esse aviso no prazo, ele mesmo aponta o boot de iteration para a imagem
// anterior e reinicia.
//
// A conta: o C6 reinicia 5 s depois do `end`, o P4 se reinicia em seguida e
// leva uns 15-25 s ate a stack do esp_hosted subir. 120 s da folga para um boot
// lento sem deixar o user olhando para um aparelho mudo por muito time.
#define C6_PRAZO_HOST_S   120
