/*
 * Firmware do coprocessador ESP32-C6 do M5Stack Tab5.
 *
 * ---------------------------------------------------------------------------
 * POR QUE ESTE ARQUIVO EXISTE
 * ---------------------------------------------------------------------------
 * O ESP32-P4 nao tem radio. Tudo que e WiFi passa pelo C6 por SDIO, atraves do
 * esp_hosted. E o esp_hosted NAO leva o ESP-NOW junto: esp_now_init, _send e
 * _register_recv_cb nem sequer linkam do lado do P4 (o esp_now.h de la e so
 * "injected", faz compilar e nao existe implementacao). esp_wifi_80211_tx e o
 * modo promiscuo tambem nao sao proxiados.
 *
 * Ou seja: para ter ESP-NOW no Tab5, ele tem que rodar AQUI, onde o radio esta.
 * O P4 conversa com este firmware pelo canal peer_data do esp_hosted - um cano
 * de bytes opaco que ja existe e dispensa mexer em protobuf.
 *
 * ---------------------------------------------------------------------------
 * ISTO SUBSTITUI O FIRMWARE DA M5STACK
 * ---------------------------------------------------------------------------
 * O C6 do Tab5 vem com o firmware da M5Stack (esp_hosted 2.12.6, backup em
 * c6/backup_fabrica_2.12.6.bin). O host do M5tabOS e 3.0.7, e este firmware e
 * da mesma versao - o boot confere ("host=3.0.7 coprocessor=3.0.7 (match)").
 *
 * Mas e o firmware que faz o WiFi funcionar. Por isso as DUAS redes de
 * seguranca abaixo.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "espnow.h"            /* SO os tipos do componente espressif/esp-now (ver DOIS DIALETOS) */
#include "esp_random.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include <stdlib.h>   /* free do relatorio FTM */
#if CONFIG_IEEE802154_ENABLED
#include "esp_ieee802154.h"
#include "esp_coexist.h"       /* esp_coex_wifi_i154_enable */
#include "esp_coex_i154.h"     /* esp_coex_ieee802154_status_disable */
#endif
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "eh_cp_feat_peer_data.h"
#include "tab5_c6.h"   /* o contrato, shared com o P4 */

static const char *TAG = "tab5c6";

/* ---------------------------------------------------------------------------
 * PROTOCOLO COM O P4
 * ---------------------------------------------------------------------------
 * Os msg_id sao um contrato entre os dois lados, e moram num arquivo so:
 * shared/tab5_c6.h, incluido pelos dois projetos.
 *
 * Formato do payload, sempre cru - nada de protobuf aqui. O canal ja entrega
 * (msg_id, bytes, tamanho), e inventar serializacao em cima disso seria
 * trabalho sem retorno: os dois lados sao nossos e compilam juntos.
 */
#define MSG_HOST_VIVO     C6_MSG_HOST_VIVO
#define MSG_HOST_ENVIAR   C6_MSG_HOST_ENVIAR
#define MSG_HOST_CANAL    C6_MSG_HOST_CANAL
#define MSG_HOST_ESPNOW   C6_MSG_HOST_ESPNOW
#define MSG_C6_PRONTO     C6_MSG_PRONTO
#define MSG_C6_RECEBIDO   C6_MSG_RECEBIDO
#define MSG_C6_ESTADO     C6_MSG_ESTADO

/* O que ESTE firmware sabe fazer: vai no PRONTO e no INFO. Os tres extras do
 * radio so acendem o bit se o modulo foi COMPILADO (depende do sdkconfig do
 * c6/) - assim o P4 esconde a funcao que esta gravação nao trouxe, sem reabrir. */
#if CONFIG_ESP_WIFI_CSI_ENABLED
#define CAP_CSI C6_CAP_CSI
#else
#define CAP_CSI 0
#endif
#if CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
#define CAP_FTM C6_CAP_FTM
#else
#define CAP_FTM 0
#endif
#if CONFIG_IEEE802154_ENABLED
#define CAP_154 C6_CAP_154
#else
#define CAP_154 0
#endif
#define CAPS (C6_CAP_ESPNOW | C6_CAP_FAREJO | C6_CAP_CRU | C6_CAP_INFO \
              | CAP_CSI | CAP_FTM | CAP_154)

/* O que ESTE chip faz, alem do que foi compilado. O FTM initiator nao existe nos
 * C6 de revisao v0.0/v0.1: o proprio esp_wifi_init (esp_wifi/src/wifi_init.c)
 * apaga o bit dele no feature_caps nessas revisoes - errata do silicio - e o
 * initiate_session responde ESP_ERR_NOT_SUPPORTED ("FTM Initiator mode not
 * supported!"). Anunciar o cap nesse chip seria prometer o que ele nao faz. */
static uint8_t caps_reais(void)
{
    uint8_t c = CAPS;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    if (ci.model == CHIP_ESP32C6 && ci.revision <= 1) { c &= (uint8_t)~C6_CAP_FTM; }
    return c;
}

/* Revisao do chip para o INFO: 0x80 marca "valida"; o resto e maior*16+menor
 * (esp_chip_info da maior*100+menor). */
static uint8_t chip_rev_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    return (uint8_t)(0x80 | (((ci.revision / 100) & 0x7) << 4) | ((ci.revision % 100) & 0xF));
}

static const uint8_t DIFUSAO[ESP_NOW_ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* Diagnostico dos extras do radio, devolvido no INFO: sem o log serial do C6
 * (que exige o Downloader), e o unico jeito de o P4 saber onde parou. */
static volatile int16_t  s_csi_err = 0, s_z154_err = 0;
static volatile uint32_t s_csi_cb = 0, s_csi_env = 0, s_z154_rx = 0;
static volatile int16_t  s_csi_err2 = 0;
static volatile bool     s_z154_lig = false;          /* 802.15.4 ouvindo now */
static bool              s_z154_habilitado __attribute__((unused)) = false;   /* esp_ieee802154_enable feito (uma vez so) */
static volatile uint8_t  s_csi_etapa = 0, s_csi_modo = 0;
/* Os extras do radio sao ligados pela tarefa do RPC e desligados pelo vigia
 * (esp_timer): esta trava impede os dois de mexerem no driver ao mesmo time. */
static SemaphoreHandle_t s_radio_mtx = NULL;

/* ---------------------------------------------------------------------------
 * REDE DE SEGURANCA 2 - no nivel do APLICATIVO
 * ---------------------------------------------------------------------------
 * A rede 1 e o CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE do sdkconfig.defaults. Ela
 * so vale com o NOSSO bootloader: a OTA nao substitui o bootloader, e o da
 * M5Stack pode nao ter rollback. A gravacao pelo cabo (idf.py flash) leva o
 * bootloader, a tabela de particoes e o otadata deste projeto juntos.
 *
 * Esta segunda rede nao depende dele. Se o P4 nao disser "estou te ouvindo"
 * dentro do prazo, NOS MESMOS apontamos o boot de iteration para a particao
 * anterior e reiniciamos. Cobre o caso provavel de uma OTA - subimos, mas o
 * enlace SDIO nao fecha e o Tab5 fica sem WiFi. So vale para imagem que chegou
 * por OTA (ver GUARDA DE IMAGEM NOVA); gravada pelo cabo nao ha para onde voltar.
 *
 * O que ela NAO cobre: falha antes deste codigo rodar (loop de panico no boot).
 * Para isso so o bootloader, ou acesso fisico ao C6.
 */
#define PRAZO_HOST_MS (C6_PRAZO_HOST_S * 1000)

/* GUARDA DE IMAGEM NOVA - por que nao basta o rollback do bootloader
 *
 * Uma OTA pelo enlace reinicia o C6 DUAS vezes seguidas: a 1a pelo proprio
 * esp_hosted (2 s depois do activate) e a 2a pelo P4, que ve o SDIO cair,
 * se reinicia e, no boot, reseta o C6 de novo (CP_RESET_STRATEGY_ALWAYS). O
 * bootloader entende "reiniciou ainda em PENDING_VERIFY" como imagem quebrada
 * e iteration para a anterior - ou seja, TODA OTA seria desfeita, boa ou ruim.
 *
 * Entao a decisao muda de lugar:
 *   - o bootloader cobre so o panico ANTES do app_main (a imagem nem chega
 *     aqui): no 1o boot da imagem nova, assim que chegamos, ela e marcada
 *     valida para ele;
 *   - daqui em diante quem decide e esta guarda, com o estado na NVS ("tab5c6"):
 *     "pend" = imagem nova ainda nao confirmada pelo P4, "tent" = boots dela
 *     sem confirmacao. Passou de GUARDA_MAX_BOOTS (laco de panico depois do
 *     app_main, enlace que nunca fecha), ou o prazo correu sem o VIVO do P4:
 *     iteration para o outro slot.
 *   - imagem gravada pelo cabo nunca fica pending: nada para onde voltar. */
#define GUARDA_MAX_BOOTS 4

static esp_timer_handle_t s_carencia = NULL;
static bool s_confirmado = false;
static nvs_handle_t s_guarda = 0;
static volatile bool s_pendente = false;

static void guarda_gravar(uint8_t pend, uint8_t tent)
{
    if (!s_guarda) { return; }
    nvs_set_u8(s_guarda, "pend", pend);
    nvs_set_u8(s_guarda, "tent", tent);
    nvs_commit(s_guarda);
}

static void desistir_e_voltar(void *arg)
{
    (void)arg;
    if (s_confirmado) { return; }

    /* "O OUTRO slot" (next_update_partition), onde ficou a imagem que gravou
     * esta. NAO esp_ota_get_last_invalid_partition(): ela so acha particao
     * INVALID/ABORTED, estado que nao existe antes de um rollback. A factory e
     * a rede de baixo, se um dia houver uma. */
    const esp_partition_t *cand[2] = {
        esp_ota_get_next_update_partition(NULL),
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL),
    };

    ESP_LOGE(TAG, "imagem nova sem confirmacao do host - revertendo");

    for (int i = 0; i < 2; ++i) {
        if (!cand[i]) { continue; }

        esp_app_desc_t d;
        if (esp_ota_get_partition_description(cand[i], &d) == ESP_OK) {
            ESP_LOGE(TAG, "tentando '%s' (%s %s)", cand[i]->label,
                     d.project_name, d.version);
        } else {
            ESP_LOGE(TAG, "'%s' nao tem imagem valida - pulando", cand[i]->label);
            continue;
        }

        if (esp_ota_set_boot_partition(cand[i]) == ESP_OK) {
            /* Limpa a guarda ANTES: a imagem de iteration le a mesma NVS, e com
             * "pend" de pe ela tambem se acharia nova e reverteria de novo. */
            guarda_gravar(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "'%s' recusada", cand[i]->label);
    }

    /* Sem iteration: fica de pe e para de count, senao reverteria a cada boot. */
    guarda_gravar(0, 0);
    s_pendente = false;
    ESP_LOGE(TAG, "NAO HA PARA ONDE VOLTAR. Seguindo de pe.");
}

/* No app_main, logo depois da NVS: decide se esta imagem esta em prova. */
static void guarda_iniciar(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *rodando = esp_ota_get_running_partition();
    const bool chegou_por_ota = rodando
        && esp_ota_get_state_partition(rodando, &st) == ESP_OK
        && st == ESP_OTA_IMG_PENDING_VERIFY;

    uint8_t pend = 0, tent = 0;
    if (nvs_open("tab5c6", NVS_READWRITE, &s_guarda) != ESP_OK) {
        /* Sem NVS nao ha guarda propria: fica o rollback do bootloader e a
         * carencia classica (o VIVO marca valida). */
        s_guarda = 0;
        pend = chegou_por_ota ? 1 : 0;
    } else {
        if (chegou_por_ota) {
            pend = 1; tent = 0;
        } else {
            nvs_get_u8(s_guarda, "pend", &pend);
            nvs_get_u8(s_guarda, "tent", &tent);
        }
        if (pend) { ++tent; }
        guarda_gravar(pend, tent);
        /* Com a guarda gravada, o bootloader ja fez a parte dele. */
        if (chegou_por_ota) { esp_ota_mark_app_valid_cancel_rollback(); }
    }

    s_pendente = pend != 0;
    if (!s_pendente) {
        ESP_LOGI(TAG, "imagem confirmada: sem carencia");
        return;
    }
    if (tent > GUARDA_MAX_BOOTS) {
        ESP_LOGE(TAG, "imagem nova: %u boots sem confirmacao", (unsigned)tent);
        desistir_e_voltar(NULL);
        return;
    }
    const esp_timer_create_args_t ta = { .callback = desistir_e_voltar, .name = "carencia" };
    if (esp_timer_create(&ta, &s_carencia) == ESP_OK) {
        esp_timer_start_once(s_carencia, (uint64_t)PRAZO_HOST_MS * 1000);
        ESP_LOGW(TAG, "imagem NOVA em prova (boot %u de %u): sem o VIVO do host em %d s, "
                      "volto para a anterior", (unsigned)tent, GUARDA_MAX_BOOTS + 1,
                 PRAZO_HOST_MS / 1000);
    }
}

static void host_falou(void)
{
    if (s_confirmado) { return; }
    s_confirmado = true;
    if (s_carencia) { esp_timer_stop(s_carencia); }
    if (s_pendente) {
        guarda_gravar(0, 0);
        s_pendente = false;
        ESP_LOGI(TAG, "host confirmou o enlace - imagem nova vira definitiva");
    }
    /* Caso sem NVS: e aqui que o rollback do bootloader e cancelado. */
    esp_ota_mark_app_valid_cancel_rollback();
}

/* ---------------------------------------------------------------------------
 * ESP-NOW
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * SAIDA PARA O HOST - e ela NAO e luxo
 * ---------------------------------------------------------------------------
 * Os callbacks do radio (ESP-NOW, CSI, promiscuo) rodam na tarefa de WiFi e
 * tem que devolver depressa - e a mesma regra do "nada de E/S bloqueante no
 * ao_tocar" do lado do P4. O eh_cp_feat_peer_data_send fala por RPC com o host
 * e pode BLOQUEAR; chamado de dentro do callback, trava a stack de WiFi
 * inteira, e nao ha sintoma pior: o radio simplesmente para.
 *
 * Entao o callback so escreve no RING BUFFER e sai; quem fala com o host e a
 * tarefa c6saida. Ring e nao fila de itens fixos: as mensagens vao de 7 B (um
 * ESTADO) a ~1 KB (um CSI HE). Com fila, cada item custaria o maior tamanho na
 * RAM e o produtor montaria uma copia desse tamanho na PROPRIA stack - a stack
 * da tarefa de WiFi, que nao e nossa. No ring, o produtor reserva o espaco
 * exato (SendAcquire) e escreve direto nele: zero copia intermediaria.
 *
 * Item = msg_id (4 B) + body. Cheio = descarta (o host nao esta consumindo, e
 * travar aqui so pioraria; perder um record e melhor que stop o radio). */
#define SAIDA_RING_BYTES  (12 * 1024)

static RingbufHandle_t s_saida = NULL;
static volatile uint32_t s_saida_perdas = 0;

/* Em DUAS partes (cabecalho + body), para quem monta a message nao precisar
 * juntar as duas num buffer seu. Qualquer tarefa; NUNCA de ISR. */
static bool enfileirar2(uint32_t msg_id, const uint8_t *cab, size_t ncab,
                        const uint8_t *body, size_t ncorpo)
{
    if (!s_saida) { return false; }
    uint8_t *it = NULL;
    if (xRingbufferSendAcquire(s_saida, (void **)&it, 4 + ncab + ncorpo, 0) != pdTRUE || !it) {
        s_saida_perdas++;
        return false;
    }
    memcpy(it, &msg_id, 4);
    memcpy(it + 4, cab, ncab);
    if (ncorpo) { memcpy(it + 4 + ncab, body, ncorpo); }
    xRingbufferSendComplete(s_saida, it);
    return true;
}

static void enfileirar(uint32_t msg_id, const uint8_t *data, size_t n)
{
    enfileirar2(msg_id, data, n, NULL, 0);
}

static void vTarefaSaida(void *arg)
{
    (void)arg;
    for (;;) {
        size_t n = 0;
        uint8_t *it = (uint8_t *)xRingbufferReceive(s_saida, &n, portMAX_DELAY);
        if (!it) { continue; }
        if (n > 4) {
            uint32_t msg_id;
            memcpy(&msg_id, it, 4);
            eh_cp_feat_peer_data_send(msg_id, it + 4, n - 4);
        }
        vRingbufferReturnItem(s_saida, it);
    }
}

/* ---------------------------------------------------------------------------
 * DOIS DIALETOS DE ESP-NOW
 * ---------------------------------------------------------------------------
 * O ESP-NOW so transporta bytes; o que vai dentro e decisao de cada firmware:
 *   - FORMATO ESPRESSIF: o componente espressif/esp-now poe um cabecalho
 *     (versao, tipo, tamanho, "magic" anti-repeticao, flags, enderecos) e
 *     retransmite. E o do chat do Cardputer oficial (M5Cardputer-UserDemo).
 *   - TEXTO PURO: esp_now_send() direto com o texto. Arduino, UIFlow, exemplos
 *     simples - o StickC e o Basic, provavelmente.
 * O Tab5 fala OS DOIS. Nao usamos a biblioteca para rodar (ela descarta tudo
 * que nao e dela - texto puro nunca chegaria - e o espnow_init dela aborta o
 * chip se o ESP-NOW falhar por baixo, o modo de falha de 09/2026). Ela so
 * empresta os TIPOS (espnow.h), para o cabecalho ser identico por construcao;
 * as regras de aceite abaixo sao as do espnow_recv_cb dela (versao 2.5.3,
 * formato no ar identico ao 2.5.1 do UserDemo).
 *
 * RECEBER: passou nas regras do formato Espressif -> tira o cabecalho; senao
 * -> texto puro. ENVIAR: nos dialetos ouvidos nos ultimos 10 min; sem ninguem
 * ouvido ainda, nos dois (para ser achado). */

typedef struct {                       /* = espnow_data_t de espnow.c */
    uint8_t type    : 4;
    uint8_t version : 2;
    uint8_t         : 2;
    uint8_t size;
    espnow_frame_head_t frame_head;
    uint8_t dest_addr[6];
    uint8_t src_addr[6];
    uint8_t payload[0];
} __attribute__((packed)) pacote_espressif_t;

#define ESPRESSIF_VERSAO   2           /* ESPNOW_VERSION do componente */
#define ESPRESSIF_REPETE   3           /* copias com o mesmo magic (quem recebe descarta as repetidas) */
#define DIALETO_VALIDADE_US (10LL * 60 * 1000000)

static uint8_t  s_meu_mac[ESP_NOW_ETH_ALEN];
static uint16_t s_magicos[32];         /* ultimos 'magic' vistos: descarta retransmissao */
static int      s_magico_prox = 0, s_magicos_n = 0;
/* Duas tarefas mexem no anel (a de WiFi recebendo, a c6envio registrando o
 * nosso): a trava e curta, 32 comparacoes. */
static portMUX_TYPE s_magico_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_ouviu_espressif_us = 0, s_ouviu_cru_us = 0;

static bool magico_visto(uint16_t m)
{
    bool visto = false;
    portENTER_CRITICAL(&s_magico_mux);
    /* So as entradas PREENCHIDAS: o anel nasce zerado, e um magic 0 legitimo
     * (a biblioteca sorteia de 0 a 65535) seria descartado como repetido. */
    for (int i = 0; i < s_magicos_n && !visto; ++i) { visto = (s_magicos[i] == m); }
    if (!visto) {
        s_magicos[s_magico_prox] = m;
        s_magico_prox = (s_magico_prox + 1) % 32;
        if (s_magicos_n < 32) { ++s_magicos_n; }
    }
    portEXIT_CRITICAL(&s_magico_mux);
    return visto;
}

/* Um par precisa estar CADASTRADO antes de qualquer envio, inclusive a difusao. */
static esp_err_t garantir_par(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (esp_now_is_peer_exist(mac)) { return ESP_OK; }
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.ifidx   = WIFI_IF_STA;
    p.channel = 0;
    p.encrypt = false;
    esp_err_t r = esp_now_add_peer(&p);
    if (r == ESP_ERR_ESPNOW_FULL) {
        /* TABELA CHEIA (ESP_NOW_MAX_TOTAL_PEER_NUM = 20). Cada destino unicast
         * novo ocupa uma vaga para sempre; no 21o, todo envio para MAC novo
         * falharia ate o C6 reiniciar. Solta os unicast (a difusao fica) e tenta
         * de novo. Coleta antes de erase: erase no meio do fetch desanda a
         * iteracao do driver. */
        uint8_t erase[ESP_NOW_MAX_TOTAL_PEER_NUM][ESP_NOW_ETH_ALEN];
        int n = 0;
        esp_now_peer_info_t q;
        for (bool start = true; n < ESP_NOW_MAX_TOTAL_PEER_NUM
                                 && esp_now_fetch_peer(start, &q) == ESP_OK; start = false) {
            if (memcmp(q.peer_addr, DIFUSAO, ESP_NOW_ETH_ALEN) != 0) {
                memcpy(erase[n++], q.peer_addr, ESP_NOW_ETH_ALEN);
            }
        }
        for (int i = 0; i < n; ++i) { esp_now_del_peer(erase[i]); }
        r = esp_now_add_peer(&p);
    }
    return r;
}

/* No context da tarefa de WiFi: decide o dialeto, so copia e sai. */
static volatile bool s_repassar_cru = false;   /* C6_AJ_CRU */

static void on_receber(const esp_now_recv_info_t *info, const uint8_t *data, int n)
{
    if (!info || !data || n <= 0) { return; }
    if (s_repassar_cru) {
        /* TUBO TRANSPARENTE: o frame inteiro, sem interpretar, com o RSSI.
         * Formato novo de outro aparelho passa a ser problema so do P4 - sem
         * regravar o C6. */
        uint8_t cab[9];
        memcpy(cab, info->src_addr, ESP_NOW_ETH_ALEN);
        cab[6] = info->rx_ctrl ? (uint8_t)info->rx_ctrl->rssi : 0;
        cab[7] = info->rx_ctrl ? (uint8_t)info->rx_ctrl->channel : 0;
        cab[8] = (memcmp(info->des_addr, DIFUSAO, ESP_NOW_ETH_ALEN) == 0) ? 1 : 0;
        /* Ate 1470 B: o ESP-NOW v2.0 (IDF 5.4+, C6/S3 novos) passa de 250. */
        enfileirar2(C6_MSG_RECEBIDO_CRU, cab, sizeof(cab), data,
                    (size_t)(n > ESP_NOW_MAX_DATA_LEN_V2 ? ESP_NOW_MAX_DATA_LEN_V2 : n));
    }
    const uint8_t *texto = data;
    int tam = n;
    const uint8_t *de = info->src_addr;
    uint8_t marca = (memcmp(info->des_addr, DIFUSAO, ESP_NOW_ETH_ALEN) == 0) ? 1 : 0;

    const pacote_espressif_t *p = (const pacote_espressif_t *)data;
    if (n >= (int)sizeof(pacote_espressif_t) && p->version == ESPRESSIF_VERSAO
            && p->type < ESPNOW_DATA_TYPE_MAX
            && n == (int)(sizeof(pacote_espressif_t) + p->size)) {
        /* FORMATO ESPRESSIF. So o tipo DATA e message; o resto (ACK, controle)
         * e protocolo deles. Criptografado nao da para read. Retransmissao do
         * mesmo 'magic' ja entregue e descartada. */
        if (p->type != ESPNOW_DATA_TYPE_DATA || p->frame_head.security) { return; }
        if (memcmp(p->src_addr, s_meu_mac, ESP_NOW_ETH_ALEN) == 0) { return; }
        if (magico_visto(p->frame_head.magic)) { return; }
        texto = p->payload;
        tam = p->size;
        de = p->src_addr;   /* o autor (pode ter vindo repassado por outro) */
        marca |= 2;         /* bit 1: veio no formato Espressif */
        s_ouviu_espressif_us = esp_timer_get_time();
    } else {
        s_ouviu_cru_us = esp_timer_get_time();
    }
    if (tam <= 0) { return; }

    uint8_t cab[7];
    memcpy(cab, de, ESP_NOW_ETH_ALEN);
    cab[6] = marca;
    const int c = tam > ESP_NOW_MAX_DATA_LEN ? ESP_NOW_MAX_DATA_LEN : tam;
    enfileirar2(MSG_C6_RECEBIDO, cab, sizeof(cab), texto, (size_t)c);
}

static void on_enviar(const esp_now_send_info_t *info, esp_now_send_status_t st)
{
    (void)info; (void)st;   /* o estado vai pelo retorno do esp_now_send (ver vTarefaEnvio) */
}

/* ENVIO EM TAREFA PROPRIA: as copias do formato Espressif tem espera entre
 * elas, e o pedido chega no callback do peer_data, que nao pode wait. */
typedef struct {
    uint8_t  mac[ESP_NOW_ETH_ALEN];
    uint16_t n;
    bool     cru;              /* C6_MSG_HOST_CRU: uma vez, sem cabecalho, sem ESTADO */
    /* Do tamanho do v2.0 (1470): o CRU pode mandar frame grande para quem fala
     * v2.0; o texto (dois dialetos) continua cortado em 250. */
    uint8_t  texto[ESP_NOW_MAX_DATA_LEN_V2];
} envio_t;
static QueueHandle_t s_envio = NULL;

static void vTarefaEnvio(void *arg)
{
    (void)arg;
    static envio_t e;
    static uint8_t frame[ESP_NOW_MAX_DATA_LEN];
    for (;;) {
        if (xQueueReceive(s_envio, &e, portMAX_DELAY) != pdTRUE) { continue; }
        bool ok = (garantir_par(e.mac) == ESP_OK);
        if (e.cru) {
            if (ok) { esp_now_send(e.mac, e.texto, e.n); }
            continue;
        }

        const int64_t now = esp_timer_get_time();
        bool espressif = s_ouviu_espressif_us && (now - s_ouviu_espressif_us) < DIALETO_VALIDADE_US;
        bool cru       = s_ouviu_cru_us       && (now - s_ouviu_cru_us)       < DIALETO_VALIDADE_US;
        if (!espressif && !cru) { espressif = cru = true; }   /* ninguem ouvido: os dois */

        bool algum = false;
        if (ok && cru) {
            algum |= (esp_now_send(e.mac, e.texto, e.n) == ESP_OK);
        }
        if (ok && espressif) {
            /* Cabecalho + texto no MESMO limit de 250 bytes (ESP-NOW v1.0, o
             * que os ESP32/S3 falam). */
            const size_t cab = sizeof(pacote_espressif_t);
            size_t t = e.n;
            if (cab + t > ESP_NOW_MAX_DATA_LEN) { t = ESP_NOW_MAX_DATA_LEN - cab; }
            memset(frame, 0, cab);
            pacote_espressif_t *p = (pacote_espressif_t *)frame;
            p->version = ESPRESSIF_VERSAO;
            p->type = ESPNOW_DATA_TYPE_DATA;
            p->size = (uint8_t)t;
            uint16_t m = (uint16_t)esp_random();
            if (m == 0) { m = 1; }
            p->frame_head.magic = m;
            uint8_t canal = 0; wifi_second_chan_t sec;
            esp_wifi_get_channel(&canal, &sec);
            p->frame_head.channel = canal;
            p->frame_head.broadcast = (memcmp(e.mac, DIFUSAO, ESP_NOW_ETH_ALEN) == 0);
            p->frame_head.retransmit_count = ESPRESSIF_REPETE;
            memcpy(p->dest_addr, e.mac, ESP_NOW_ETH_ALEN);
            memcpy(p->src_addr, s_meu_mac, ESP_NOW_ETH_ALEN);
            memcpy(p->payload, e.texto, t);
            magico_visto(m);   /* o nosso proprio eco repassado nao iteration como message */
            for (int k = 0; k < ESPRESSIF_REPETE; ++k) {
                algum |= (esp_now_send(e.mac, frame, cab + t) == ESP_OK);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        uint8_t st[ESP_NOW_ETH_ALEN + 1];
        memcpy(st, e.mac, ESP_NOW_ETH_ALEN);
        st[6] = algum ? 1 : 0;
        enfileirar(MSG_C6_ESTADO, st, sizeof(st));
    }
}

/* LIGAR O ESP-NOW E DECISAO DO HOST (ele manda when o WiFi ja subiu), e NADA
 * aqui aborta: falhou, desiste calado - o host pede de novo em 5 s. */
static bool s_espnow_no_ar = false;

static void ligar_espnow(void)
{
    /* O ESP-NOW sobrevive a esp_wifi_stop/start, mas NAO a um esp_wifi_deinit:
     * o handler do esp_hosted faz deinit+init when o host manda uma config de
     * init diferente, e o P4 pode disable o WiFi no nivel 2 (deinit). Sem esta
     * conferencia, s_espnow_no_ar ficaria true com o ESP-NOW morto por baixo, e
     * todo envio/recepcao sumiria calado ate o C6 reiniciar. */
    uint32_t ver;
    if (s_espnow_no_ar && esp_now_get_version(&ver) != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW caiu por baixo (WiFi reiniciado) - religando");
        s_espnow_no_ar = false;
    }
    if (!s_espnow_no_ar) {
        const esp_err_t r = esp_now_init();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_init: %s - o WiFi ja subiu mesmo?", esp_err_to_name(r));
            return;
        }
        esp_now_register_recv_cb(on_receber);
        esp_now_register_send_cb(on_enviar);
        garantir_par(DIFUSAO);
        esp_wifi_get_mac(WIFI_IF_STA, s_meu_mac);
        if (!s_envio) {
            s_envio = xQueueCreate(4, sizeof(envio_t));
            if (s_envio) { xTaskCreate(vTarefaEnvio, "c6envio", 4096, NULL, 4, NULL); }
        }
        s_espnow_no_ar = true;
    }
    /* PRONTO sempre (o host repete o pedido ate ver este aviso). */
    uint8_t canal = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&canal, &sec);
    uint8_t msg[ESP_NOW_ETH_ALEN + 3];
    memcpy(msg, s_meu_mac, ESP_NOW_ETH_ALEN);
    msg[6] = canal;
    msg[7] = C6_PROTO_VERSAO;
    msg[8] = caps_reais();
    enfileirar(MSG_C6_PRONTO, msg, sizeof(msg));
    ESP_LOGI(TAG, "ESP-NOW no ar (formato Espressif + texto puro): canal %u", (unsigned)canal);
}

/* ---------------------------------------------------------------------------
 * FAREJADOR - modo promiscuo, resumido no C6
 * ---------------------------------------------------------------------------
 * O contrato (formato, validade, canais) esta em tab5_c6.h. Aqui, o como:
 *
 * QUEM MEXE NO QUE. O callback do promiscuo roda na tarefa de WiFi, a cada
 * frame - mil ou mais por segundo num canal movimentado. Ele so acha o MAC na
 * tabela e soma; nada de alocar, logar ou falar com o host. Ligar, disable,
 * saltar de canal e mandar o lote e tudo da tarefa c6farejo, a UNICA que chama
 * esp_wifi_set_promiscuous*. O pedido do host (que chega na tarefa do RPC) so
 * deixa a configuracao nova e acorda a c6farejo.
 *
 * DUAS TABELAS. O callback escreve na active; ao close o lote a c6farejo troca
 * a active (uma linha dentro da trava) e le a outra sem trava none, com
 * calma, enquanto os quadros seguem caindo na nova.
 *
 * Tabela por hash do MAC com sondagem linear: 128 vagas para no maximum 96 MACs
 * por periodo (3/4 cheia, a busca para em poucas comparacoes). Passou de 96,
 * conta como perdido - o P4 ve no lote e pode encurtar o periodo.
 */
#define FJ_VAGAS   128
#define FJ_MAX      96

typedef struct {
    uint8_t mac[6];
    int8_t  ult, max;
    int16_t soma;       /* 255 x -128 cabe */
    uint8_t n, tipos, canal, usado;
} fj_ent_t;             /* 16 B */

typedef struct {
    fj_ent_t e[FJ_VAGAS];
    uint16_t usados, perdidos;
    uint32_t quadros;
} fj_tab_t;

static fj_tab_t s_fj[2];
static volatile uint8_t s_fj_ativa = 0;
static portMUX_TYPE s_fj_mux = portMUX_INITIALIZER_UNLOCKED;

/* Lidos pelo callback a cada frame: copias simples do s_cfg em vigor. */
static volatile int8_t s_fj_rssi_min = -128;
static volatile bool   s_fj_tem_alvo = false;
static uint8_t         s_fj_alvo[6];
static volatile uint32_t s_fj_quadros_total = 0;

/* Pedido do host -> c6farejo (sob s_fj_mux). */
static c6_farejo_cfg_t s_fj_pedido;
static bool            s_fj_tem_pedido = false;
static int64_t         s_fj_validade_us = 0;

static volatile bool s_farejando = false;
static TaskHandle_t  s_fj_tarefa = NULL;
/* Para onde o radio iteration when o farejo para: o canal de when ele ligou,
 * ou o que o host pediu DEPOIS (C6_MSG_HOST_CANAL no meio do farejo). Sem isso,
 * disable o farejo desfazia o "canal fixo" do ESP-NOW pedido no meio do caminho. */
static volatile uint8_t s_canal_volta = 0;

static void radio_guarda(int64_t now);   /* extras do radio, mais abaixo */

static void on_farejar(void *buf, wifi_promiscuous_pkt_type_t tipo)
{
    const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
    if (!pk || pk->rx_ctrl.sig_len < 16) { return; }   /* sem addr2 */
    const int8_t rssi = (int8_t)pk->rx_ctrl.rssi;
    if (rssi < s_fj_rssi_min) { return; }

    const uint8_t *q = pk->payload;
    const uint8_t sub = q[0] >> 4;
    uint8_t t;
    if (tipo == WIFI_PKT_MGMT) {
        t = (sub == 8 || sub == 5) ? C6_FT_BEACON
          : (sub == 4)             ? C6_FT_PROCURA
          :                          C6_FT_GERENCIA;
    } else if (tipo == WIFI_PKT_DATA) {
        const uint8_t ds = q[1] & 0x03;
        t = C6_FT_DADOS | (ds == 1 ? C6_FT_PARA_AP : ds == 2 ? C6_FT_DO_AP : 0);
    } else {
        return;
    }

    const uint8_t *ta = q + 10;   /* addr2 = transmissor, em gerencia e data */
    if (s_fj_tem_alvo && memcmp(ta, s_fj_alvo, 6) != 0) { return; }
    const uint8_t canal = (uint8_t)pk->rx_ctrl.channel;

    /* Os 3 ultimos bytes variam entre aparelhos; os 3 primeiros sao do fabricante. */
    unsigned h = ((unsigned)ta[5] * 31u + ta[4] * 7u + ta[3]) & (FJ_VAGAS - 1);

    portENTER_CRITICAL(&s_fj_mux);
    fj_tab_t *tb = &s_fj[s_fj_ativa];
    tb->frames++;
    for (int k = 0; k < FJ_VAGAS; ++k, h = (h + 1) & (FJ_VAGAS - 1)) {
        fj_ent_t *e = &tb->e[h];
        if (!e->usado) {
            if (tb->usados >= FJ_MAX) { tb->perdidos++; break; }
            memcpy(e->mac, ta, 6);
            e->usado = 1;
            e->ult = e->max = rssi;
            e->soma = rssi;
            e->n = 1;
            e->tipos = t;
            e->channel = canal;
            tb->usados++;
            break;
        }
        if (memcmp(e->mac, ta, 6) == 0) {
            e->ult = rssi;
            if (rssi > e->max) { e->max = rssi; }
            if (e->n < 255) { e->soma += rssi; e->n++; }
            e->tipos |= t;
            e->channel = canal;
            break;
        }
    }
    portEXIT_CRITICAL(&s_fj_mux);
    s_fj_quadros_total++;
}

/* Troca a tabela active e manda o que a velha juntou, em pedacos de LOTE_MAX. */
static void fechar_lote(void)
{
    static uint8_t msg[sizeof(c6_farejo_lote_t) + C6_FAREJO_LOTE_MAX * sizeof(c6_farejo_reg_t)];

    portENTER_CRITICAL(&s_fj_mux);
    fj_tab_t *tb = &s_fj[s_fj_ativa];
    s_fj_ativa ^= 1;
    portEXIT_CRITICAL(&s_fj_mux);

    if (tb->usados) {
        uint8_t canal = 0; wifi_second_chan_t sec;
        esp_wifi_get_channel(&canal, &sec);
        c6_farejo_lote_t *cab = (c6_farejo_lote_t *)msg;
        c6_farejo_reg_t *r = (c6_farejo_reg_t *)(msg + sizeof(*cab));
        int n = 0;
        for (int i = 0; i < FJ_VAGAS; ++i) {
            const fj_ent_t *e = &tb->e[i];
            if (!e->usado) { continue; }
            memcpy(r[n].mac, e->mac, 6);
            r[n].rssi_ult = e->ult;
            r[n].rssi_max = e->max;
            r[n].rssi_med = (int8_t)(e->soma / (int)e->n);
            r[n].n = e->n;
            r[n].tipos = e->tipos;
            r[n].channel = e->channel;
            if (++n == C6_FAREJO_LOTE_MAX) {
                cab->channel = canal; cab->n = (uint8_t)n;
                cab->perdidos = tb->perdidos;
                cab->frames = tb->frames > 0xFFFF ? 0xFFFF : (uint16_t)tb->frames;
                eh_cp_feat_peer_data_send(C6_MSG_FAREJO, msg, sizeof(*cab) + n * sizeof(*r));
                n = 0;
            }
        }
        if (n) {
            cab->channel = canal; cab->n = (uint8_t)n;
            cab->perdidos = tb->perdidos;
            cab->frames = tb->frames > 0xFFFF ? 0xFFFF : (uint16_t)tb->frames;
            eh_cp_feat_peer_data_send(C6_MSG_FAREJO, msg, sizeof(*cab) + n * sizeof(*r));
        }
    }
    /* Nada ouvido: nada enviado. Silencio no SDIO e o P4 dormindo. */
    memset(tb, 0, sizeof(*tb));
}

static bool associado(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static void vTarefaFarejo(void *arg)
{
    (void)arg;
    c6_farejo_cfg_t cfg = { 0 };   /* o pedido CRU em vigor: so para comparar */
    uint16_t lote_ms = 250, perm_ms = 200, mascara = 0;
    bool saltou = false;
    int64_t prox_lote = 0, prox_salto = 0;
    int canal_idx = 0;

    for (;;) {
        /* 1. Pedido novo do host? */
        c6_farejo_cfg_t novo;
        bool tem = false;
        int64_t validade;
        portENTER_CRITICAL(&s_fj_mux);
        if (s_fj_tem_pedido) { novo = s_fj_pedido; s_fj_tem_pedido = false; tem = true; }
        validade = s_fj_validade_us;
        portEXIT_CRITICAL(&s_fj_mux);

        const int64_t now = esp_timer_get_time();
        /* Tambem e esta tarefa que desliga CSI/802.15.4 abandonados pelo host:
         * o esp_timer do vigia so a acorda (ver radio_guarda_cb). */
        radio_guarda(now);
        bool stop = s_farejando && now > validade;
        if (stop) { ESP_LOGW(TAG, "farejo: o host nao renovou - desligando"); }

        if (tem && !novo.liga) { stop = s_farejando; tem = false; }

        if (stop) {
            esp_wifi_set_promiscuous(false);
            s_farejando = false;
            if (saltou && s_canal_volta && !associado()) {
                esp_wifi_set_channel(s_canal_volta, WIFI_SECOND_CHAN_NONE);
            }
            saltou = false;
            portENTER_CRITICAL(&s_fj_mux);
            memset(s_fj, 0, sizeof(s_fj));
            portEXIT_CRITICAL(&s_fj_mux);
            ESP_LOGI(TAG, "farejo desligado");
        }

        if (tem) {
            const bool changed = !s_farejando || memcmp(&novo, &cfg, sizeof(cfg)) != 0;
            if (changed) {
                cfg = novo;
                lote_ms = cfg.lote_ms ? cfg.lote_ms : 250;
                if (lote_ms < 50) { lote_ms = 50; }
                perm_ms = cfg.permanencia_ms ? cfg.permanencia_ms : 200;
                if (perm_ms < 50) { perm_ms = 50; }
                mascara = cfg.canais & 0x3FFE;   /* canais 1..13 */
                s_fj_rssi_min = cfg.rssi_min ? cfg.rssi_min : -128;
                static const uint8_t zero[6] = { 0 };
                memcpy(s_fj_alvo, cfg.alvo, 6);
                s_fj_tem_alvo = memcmp(cfg.alvo, zero, 6) != 0;

                const uint8_t f = cfg.filtro ? cfg.filtro : (C6_FJ_MGMT | C6_FJ_DADOS);
                wifi_promiscuous_filter_t filtro = { .filter_mask =
                    ((f & C6_FJ_MGMT)  ? WIFI_PROMIS_FILTER_MASK_MGMT : 0) |
                    ((f & C6_FJ_DADOS) ? WIFI_PROMIS_FILTER_MASK_DATA : 0) };
                esp_wifi_set_promiscuous_filter(&filtro);
            }
            if (!s_farejando) {
                wifi_second_chan_t sec;
                uint8_t canal_antes = 0;
                esp_wifi_get_channel(&canal_antes, &sec);
                s_canal_volta = canal_antes;
                esp_wifi_set_promiscuous_rx_cb(on_farejar);
                const esp_err_t r = esp_wifi_set_promiscuous(true);
                if (r != ESP_OK) {
                    /* WiFi parado, por exemplo. O host renova em segundos e
                     * a gente tenta de novo - nada de abortar. */
                    ESP_LOGW(TAG, "farejo: promiscuo recusado (%s)", esp_err_to_name(r));
                } else {
                    s_farejando = true;
                    prox_lote = now + (int64_t)lote_ms * 1000;
                    prox_salto = now;
                    canal_idx = 0;
                    ESP_LOGI(TAG, "farejo ligado: canal %u, mascara 0x%04x, lote %u ms",
                             (unsigned)s_canal_volta, (unsigned)mascara, (unsigned)lote_ms);
                }
            } else {
                /* Renovacao: o WiFi pode ter sido parado e religado no meio,
                 * e esp_wifi_stop desliga o promiscuo por baixo. */
                bool enabled = false;
                if (esp_wifi_get_promiscuous(&enabled) == ESP_OK && !enabled) {
                    esp_wifi_set_promiscuous_rx_cb(on_farejar);
                    esp_wifi_set_promiscuous(true);
                }
            }
        }

        if (!s_farejando) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* parado: zero CPU */
            continue;
        }

        /* 2. Salto de canal - so solto do roteador. */
        const bool um_so = mascara && !(mascara & (mascara - 1));
        if (mascara && now >= prox_salto) {
            if (!associado()) {
                int c = 0;
                for (int k = 0; k < 14 && !c; ++k) {
                    canal_idx = canal_idx % 13 + 1;
                    if (mascara & (1u << canal_idx)) { c = canal_idx; }
                }
                uint8_t current = 0; wifi_second_chan_t sec;
                esp_wifi_get_channel(&current, &sec);
                if (c && c != current) {
                    esp_wifi_set_channel((uint8_t)c, WIFI_SECOND_CHAN_NONE);
                    saltou = true;
                }
            }
            /* Um canal so: confere de segundo em segundo (alguem pode ter
             * mudado); varios: o ritmo e a permanencia. */
            prox_salto = now + (int64_t)(um_so ? 1000 : perm_ms) * 1000;
        }

        /* 3. Lote. */
        if (now >= prox_lote) {
            fechar_lote();
            prox_lote += (int64_t)lote_ms * 1000;
            if (prox_lote <= now) { prox_lote = now + (int64_t)lote_ms * 1000; }
        }

        /* 4. Dorme ate o proximo evento - ou ate um pedido do host. */
        int64_t prox = prox_lote;
        if (mascara && prox_salto < prox) { prox = prox_salto; }
        if (validade < prox) { prox = validade + 1000; }
        int64_t esp = (prox - esp_timer_get_time()) / 1000;
        if (esp < 1) { esp = 1; }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(esp));
    }
}

/* Chamado na tarefa do RPC: so anota e acorda a c6farejo. */
static void pedir_farejo(const uint8_t *data, size_t n)
{
    c6_farejo_cfg_t c = { 0 };
    memcpy(&c, data, n < sizeof(c) ? n : sizeof(c));
    portENTER_CRITICAL(&s_fj_mux);
    s_fj_pedido = c;
    s_fj_tem_pedido = true;
    s_fj_validade_us = esp_timer_get_time() + (int64_t)C6_FAREJO_VALIDADE_MS * 1000;
    portEXIT_CRITICAL(&s_fj_mux);
    if (s_fj_tarefa) { xTaskNotifyGive(s_fj_tarefa); }
}

static void responder_info(void)
{
    c6_info_t i = { 0 };
    uint8_t canal = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&canal, &sec);
    i.proto = C6_PROTO_VERSAO;
    i.caps = caps_reais();
    i.chip_rev = chip_rev_info();
    i.channel = canal;
    i.farejando = s_farejando ? 1 : 0;
    i.heap_livre = esp_get_free_heap_size();
    i.heap_min = esp_get_minimum_free_heap_size();
    i.no_ar_s = (uint32_t)(esp_timer_get_time() / 1000000);
    i.frames = s_fj_quadros_total;
    i.csi_err = s_csi_err;
    i.z154_err = s_z154_err;
    i.csi_cb = s_csi_cb;
    i.csi_env = s_csi_env;
    i.z154_rx = s_z154_rx;
    i.csi_err2 = s_csi_err2;
    i.csi_etapa = s_csi_etapa;
    i.csi_modo = s_csi_modo;
    i.saida_perdas = s_saida_perdas;
    i.flags = (esp_core_dump_image_check() == ESP_OK ? C6_INFO_F_COREDUMP : 0)
            | (s_pendente ? C6_INFO_F_EM_PROVA : 0);
#if CONFIG_IEEE802154_ENABLED
    i.z154_estado = s_z154_lig ? 1 : (s_z154_habilitado ? 2 : 0);
#endif
    enfileirar(C6_MSG_INFO, (const uint8_t *)&i, sizeof(i));
}

/* ===========================================================================
 * EXTRAS DO RADIO - CSI, FTM, 802.15.4
 * ===========================================================================
 * Cada um dorme ate o P4 pedir (C6_MSG_HOST_RADIO), e cada bit de CAPS so
 * acende se o modulo foi compilado - um firmware gravado sem a flag no
 * sdkconfig deixa o P4 esconder a funcao, sem reabrir o aparelho.
 *
 * Mesma disciplina do farejo: o repasse cru sai do callback (tarefa do radio)
 * so pela queue de saida, nunca bloqueia; CSI e 802.15.4 desligam sozinhos se o
 * P4 nao renovar em C6_RADIO_VALIDADE_MS; FTM e disparo unico.
 */

/* ---- CSI ---- */
#if CONFIG_ESP_WIFI_CSI_ENABLED
static volatile bool     s_csi_lig = false;
static volatile int64_t  s_csi_validade = 0, s_csi_prox_us = 0;
static volatile uint16_t s_csi_intervalo_ms = 100;
static volatile int8_t   s_csi_rssi_min = -128;
static volatile bool     s_csi_so_alvo = false;
static uint8_t           s_csi_alvo[6];

static void on_csi(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    s_csi_cb++;
    if (!info || !info->buf || info->len <= 0) { return; }
    const int8_t rssi = (int8_t)info->rx_ctrl.rssi;
    if (rssi < s_csi_rssi_min) { return; }
    if (s_csi_so_alvo && memcmp(info->mac, s_csi_alvo, 6) != 0) { return; }
    const int64_t now = esp_timer_get_time();
    /* RITMO POR CADENCIA, nao "intervalo since o ultimo". Com a fonte no mesmo
     * ritmo do limit (ping de 10 Hz, limit de 100 ms), o atraso variavel do
     * ar fazia metade dos quadros chegar a 95 ms do anterior e ser descartada:
     * o teste deu 64 records de 110. A cadencia anda em passos fixos e aceita
     * o primeiro frame de cada passo; atrasou demais, recomeca de now. */
    const int64_t passo = (int64_t)s_csi_intervalo_ms * 1000;
    if (now < s_csi_prox_us) { return; }
    s_csi_prox_us = (now - s_csi_prox_us > passo) ? now + passo : s_csi_prox_us + passo;

    c6_csi_cab_t cab;
    memcpy(cab.mac, info->mac, 6);
    cab.rssi = rssi;
    cab.channel = (uint8_t)info->rx_ctrl.channel;
    cab.sec_canal = (uint8_t)info->rx_ctrl.second;
    int len = info->len;
    /* O que o P4 precisa para interpretar os bytes: o formato do frame (cada
     * um traz um conjunto diferente de subportadoras), se ha 40 MHz, e se a
     * primeira palavra presta - no C6 ela pode vir invalidate por limit do
     * hardware, e quem nao sabe disso le 2 subportadoras de lixo. */
    cab.flags = (uint8_t)((info->rx_ctrl.cur_bb_format & 0x0F) << 4)
              | (info->rx_ctrl.second ? C6_CSI_F_HT40 : 0)
              | (info->first_word_invalid ? C6_CSI_F_W0_RUIM : 0)
              | (len > C6_CSI_MAX ? C6_CSI_F_CORTADO : 0);
    if (len > C6_CSI_MAX) { len = C6_CSI_MAX; }
    cab.len = (uint16_t)len;
    cab.t_us = (uint32_t)now;
    if (enfileirar2(C6_MSG_CSI, (const uint8_t *)&cab, sizeof(cab),
                    (const uint8_t *)info->buf, (size_t)len)) {
        s_csi_env++;
    }
}

/* POR QUE O CSI DAVA ESP_FAIL (resolvido do lado do P4)
 *
 * O esp_wifi_set_csi* do driver confere um field guardado no esp_wifi_init: o
 * csi_enable do wifi_init_config_t. Zerado, loga "CSI not enabled in
 * menuconfig!" e devolve ESP_FAIL - antes de olhar qualquer outra coisa. Com o
 * esp_hosted, quem chama o esp_wifi_init aqui e o handler do RPC, com os campos
 * que o P4 MANDA (get_merged_init_config: "always use value from host"), e o P4
 * mandava 0. O CONFIG_ESP_WIFI_CSI_ENABLED deste sdkconfig so muda o
 * WIFI_INIT_CONFIG_DEFAULT, que o handler sobrescreve.
 *
 * Nao era a order das chamadas nem o modo promiscuo: depois da checagem, o
 * driver so grava o filtro no hardware e nao falha mais. Um caminho so, o do
 * exemplo da Espressif; o INFO conta em que etapa parou, se stop. */
static esp_err_t csi_aplicar(uint8_t *etapa)
{
    /* Parte do que o driver tem; os campos que a gente nao mexe ficam como ele quer. */
    wifi_csi_config_t cfg;
    if (esp_wifi_get_csi_config(&cfg) != ESP_OK) { memset(&cfg, 0, sizeof(cfg)); }
    cfg.enable = 1;
    cfg.acquire_csi_legacy = 1;    /* L-LTF: beacons e quadros 11b/g - todo aparelho */
    cfg.acquire_csi_ht20 = 1;      /* HT-LTF: 802.11n */
    cfg.acquire_csi_ht40 = 1;
    cfg.acquire_csi_su = 1;        /* HE-LTF: 802.11ax SU, o que um roteador WiFi 6 manda ao C6 */
    cfg.acquire_csi_he_stbc = 2;   /* como o esp-csi da Espressif para o C6 */
    esp_err_t r;
    *etapa = 1; r = esp_wifi_set_csi_config(&cfg);        if (r != ESP_OK) { return r; }
    *etapa = 2; r = esp_wifi_set_csi_rx_cb(on_csi, NULL); if (r != ESP_OK) { return r; }
    *etapa = 3; r = esp_wifi_set_csi(true);               if (r != ESP_OK) { return r; }
    *etapa = 0; return ESP_OK;
}

static void csi_ligar(const c6_csi_cfg_t *c)
{
    s_csi_intervalo_ms = c->min_intervalo_ms ? c->min_intervalo_ms : 100;
    s_csi_rssi_min = c->rssi_min ? c->rssi_min : -128;
    static const uint8_t zero[6] = { 0 };
    s_csi_so_alvo = c->so_alvo && memcmp(c->alvo, zero, 6) != 0;
    memcpy(s_csi_alvo, c->alvo, 6);
    s_csi_validade = esp_timer_get_time() + (int64_t)C6_RADIO_VALIDADE_MS * 1000;
    /* Reaplicado a CADA renovacao, nao so na primeira: o P4 pode stop e
     * religar o WiFi no meio (Config), e o esp_wifi_stop desfaz o CSI por baixo.
     * Com o "so na primeira", o P4 renovaria para sempre um CSI ja morto - a
     * mesma armadilha que o farejo trata na renovacao dele. */
    uint8_t etapa = 0;
    const esp_err_t r = csi_aplicar(&etapa);
    s_csi_err = s_csi_err2 = (int16_t)r;   /* a ultima vez; o P4 le no INFO */
    s_csi_etapa = etapa;
    if (r == ESP_OK) {
        if (!s_csi_lig) {
            ESP_LOGI(TAG, "CSI ligado (intervalo %u ms)", (unsigned)s_csi_intervalo_ms);
        }
        s_csi_lig = true;
    } else {
        if (!s_csi_lig) {
            ESP_LOGW(TAG, "CSI recusado: %s (etapa %u)%s", esp_err_to_name(r), (unsigned)etapa,
                     r == ESP_FAIL ? " - o host iniciou o WiFi com csi_enable 0" : "");
        }
        s_csi_lig = false;
    }
    s_csi_modo = s_csi_lig ? 1 : 0;
}
static void csi_desligar(void)
{
    if (s_csi_lig) { esp_wifi_set_csi(false); s_csi_lig = false; ESP_LOGI(TAG, "CSI desligado"); }
    s_csi_modo = 0;
}
static void csi_guarda(int64_t now)
{
    if (s_csi_lig && now > s_csi_validade) { ESP_LOGW(TAG, "CSI: host nao renovou"); csi_desligar(); }
}
#else
static void csi_ligar(const c6_csi_cfg_t *c) { (void)c; }
static void csi_desligar(void) {}
static void csi_guarda(int64_t a) { (void)a; }
#endif

/* ---- FTM (disparo unico) ---- */
#if CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
static bool s_ftm_evt = false;

static void ftm_evento(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != WIFI_EVENT_FTM_REPORT || !data) { return; }
    const wifi_event_ftm_report_t *r = (const wifi_event_ftm_report_t *)data;
    c6_ftm_rep_t rep;
    memcpy(rep.peer, r->peer_mac, 6);
    rep.status = (uint8_t)r->status;
    rep.num = r->ftm_report_num_entries;
    rep.rtt_est_ns = r->rtt_est;
    rep.dist_est_cm = r->dist_est;
    rep.rtt_raw_ns = r->rtt_raw;
    enfileirar(C6_MSG_FTM, (const uint8_t *)&rep, sizeof(rep));
    /* O relatorio detalhado nao vai ao P4 (so o summary); libera para nao vazar. */
    if (r->ftm_report_data) { free(r->ftm_report_data); }
}

static void ftm_disparar(const c6_ftm_cfg_t *c)
{
    if (!s_ftm_evt) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftm_evento, NULL);
        s_ftm_evt = true;
    }
    wifi_ftm_initiator_cfg_t cfg = {
        .channel = c->channel, .frm_count = c->frm_count, .burst_period = c->burst_period,
    };
    memcpy(cfg.resp_mac, c->peer, 6);
    const esp_err_t r = esp_wifi_ftm_initiate_session(&cfg);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "FTM recusado (%s)", esp_err_to_name(r));
        c6_ftm_rep_t rep;
        memset(&rep, 0, sizeof(rep));
        memcpy(rep.peer, c->peer, 6);
        rep.status = C6_FTM_NAO_INICIOU;
        rep.rtt_raw_ns = (uint32_t)(int32_t)r;   /* o reason: sem ele o P4 so saberia "falhou" */
        enfileirar(C6_MSG_FTM, (const uint8_t *)&rep, sizeof(rep));
    }
}
#else
static void ftm_disparar(const c6_ftm_cfg_t *c) { (void)c; }
#endif

/* ---- 802.15.4 (o outro radio: Thread/Zigbee cru) ---- */
#if CONFIG_IEEE802154_ENABLED
static volatile int64_t  s_z154_validade = 0, s_z154_ult_us = 0;
static volatile uint16_t s_z154_intervalo_ms = 0;

/* Simbolo FRACO do driver: o nosso vence e e chamado a cada frame recebido.
 *
 * ATENCAO: o driver chama isto DIRETO da ISR do radio 802.15.4 (nao ha tarefa no
 * meio). Por isso aqui so valem as variantes FromISR: um xQueueSend comum
 * (enfileirar2) estoura o assert do FreeRTOS no primeiro frame e derruba o C6.
 * O buffer e estatico: a ISR nao reentra, e a stack de ISR e curta. */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    /* msg_id + cabecalho + frame, montado aqui e copiado de uma vez para o
     * ring (a variante FromISR nao tem o Acquire). Estatico: a ISR nao reentra,
     * e a stack de ISR e curta. */
    static uint8_t it[4 + sizeof(c6_154_cab_t) + C6_154_MAX];
    s_z154_rx++;
    if (frame && info && s_saida) {
        const int64_t now = esp_timer_get_time();
        if (!s_z154_intervalo_ms || now - s_z154_ult_us >= (int64_t)s_z154_intervalo_ms * 1000) {
            s_z154_ult_us = now;
            uint8_t len = frame[0];            /* frame[0] = tamanho; segue o frame (FCS trocado por RSSI/LQI) */
            if (len > C6_154_MAX) { len = C6_154_MAX; }
            const uint32_t id = C6_MSG_154;
            c6_154_cab_t cab;
            cab.channel = info->channel;
            cab.rssi = info->rssi;
            cab.lqi = info->lqi;
            cab.len = len;
            cab.t_us = (uint32_t)now;
            memcpy(it, &id, 4);
            memcpy(it + 4, &cab, sizeof(cab));
            memcpy(it + 4 + sizeof(cab), frame + 1, len);
            BaseType_t acordou = pdFALSE;
            if (xRingbufferSendFromISR(s_saida, it, 4 + sizeof(cab) + len, &acordou) != pdTRUE) {
                s_saida_perdas++;
            }
            if (acordou) { portYIELD_FROM_ISR(); }
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

/* SO OUVIR, NUNCA RESPONDER. O driver nasce com ACK automatico enabled, e no
 * modo promiscuo (sem filtro de endereco) ele responderia ACK a frame de
 * QUALQUER aparelho da malha que pedisse confirmacao: o emissor acharia que o
 * destino recebeu when talvez nao tenha, e a rede Zigbee/Thread vizinha
 * passaria a perder message por nossa causa. Nao ha API publica para isto; as
 * duas abaixo sao do driver (esp_ieee802154_pib.h, privado) e valem na proxima
 * esp_ieee802154_receive(), que relê a configuracao pending. */
extern void ieee802154_pib_set_auto_ack_tx(bool enable);
extern void ieee802154_pib_set_enhance_ack_tx(bool enable);

/* LIGAR E DESLIGAR - os dois pontos onde o WiFi caia.
 *
 * 1. COEXISTENCIA (ao enable). WiFi, BLE e 802.15.4 dividem UM caminho de RF no
 *    C6, e quem reparte o time e o arbitro de coexistencia. Ele so leva o
 *    802.15.4 em conta depois de esp_coex_wifi_i154_enable() (coex_enable +
 *    esp_coex_ieee802154_status_enable); o driver do 802.15.4 nunca faz isso
 *    sozinho - os exemplos oficiais ot_br e esp_zigbee_gateway, os dois "WiFi +
 *    802.15.4 num C6", chamam a funcao na aplicacao. Com ela, ouvindo, o
 *    802.15.4 pede o radio com a priority "txrx" do driver (baixa); o WiFi
 *    passa na frente when tem trafego e o 802.15.4 ouve nas sobras.
 *
 * 2. DESLIGAR PELO SLEEP, NUNCA PELO DISABLE. Medido no aparelho: com a
 *    coexistencia certa, o WiFi passava data COM o 802.15.4 enabled e morria
 *    logo DEPOIS de disable. esp_ieee802154_disable() corta o clock do MAC
 *    (modem_clock_module_disable) e muda o estado sem stop_current_operation():
 *    com o receptor enabled (rx_when_idle), o MAC e congelado no meio de uma
 *    recepcao, com o pedido de radio feito ao arbitro. esp_ieee802154_sleep()
 *    e o caminho limpo - para a operacao em curso (CMD_STOP), entra em SLEEP e
 *    released o RF (esp_phy_disable) - e e o que o OpenThread usa o time todo com
 *    o WiFi enabled. O driver fica habilitado e dormindo: o MAC parado custa
 *    so o clock dele, e religar e um receive().
 *
 * Tarefas diferentes chamam isto (RPC do host e o vigia de 1 s): a trava
 * s_radio_mtx serializa. */

static void z154_ligar(const c6_154_cfg_t *c)
{
    s_z154_intervalo_ms = c->min_intervalo_ms;
    s_z154_validade = esp_timer_get_time() + (int64_t)C6_RADIO_VALIDADE_MS * 1000;
    uint8_t canal = (c->channel >= 11 && c->channel <= 26) ? c->channel : 15;
    const bool novo = !s_z154_lig;
    if (!s_z154_habilitado) {
        esp_coex_wifi_i154_enable();
        const esp_err_t r = esp_ieee802154_enable();
        s_z154_err = (int16_t)r;
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "802.15.4 enable recusado: %s", esp_err_to_name(r));
            esp_coex_ieee802154_status_disable();
            return;
        }
        s_z154_habilitado = true;
    } else if (novo) {
        esp_coex_ieee802154_status_enable();
    }
    s_z154_lig = true;
    /* A cada receive() o driver relê a configuracao pending (pib). */
    ieee802154_pib_set_auto_ack_tx(false);
    ieee802154_pib_set_enhance_ack_tx(false);
    esp_ieee802154_set_channel(canal);
    esp_ieee802154_set_promiscuous(c->promiscuo != 0);
    esp_ieee802154_set_rx_when_idle(true);
    const esp_err_t rr = esp_ieee802154_receive();
    s_z154_err = (int16_t)rr;
    /* Renovacao chega a cada poucos segundos: so loga o que muda. */
    if (novo) { ESP_LOGI(TAG, "802.15.4 ouvindo: canal %u", (unsigned)canal); }
}
static void z154_desligar(void)
{
    if (!s_z154_lig) { return; }
    esp_ieee802154_set_rx_when_idle(false);   /* senao o driver iteration a ouvir sozinho */
    esp_ieee802154_sleep();
    esp_coex_ieee802154_status_disable();     /* fora do arbitro: o WiFi tem o radio so para ele */
    s_z154_lig = false;
    ESP_LOGI(TAG, "802.15.4 dormindo");
}
static void z154_guarda(int64_t now)
{
    if (s_z154_lig && now > s_z154_validade) { ESP_LOGW(TAG, "802.15.4: host nao renovou"); z154_desligar(); }
}
#else
static void z154_ligar(const c6_154_cfg_t *c) { (void)c; }
static void z154_desligar(void) {}
static void z154_guarda(int64_t a) { (void)a; }
#endif

/* Vigia unico: desliga CSI/802.15.4 que o host deixou de renovar.
 *
 * O TRABALHO NAO RODA NO esp_timer. A tarefa do esp_timer e compartilhada - o
 * controlador BLE usa ela (CONFIG_BT_LE_USE_ESP_TIMER), assim como o proprio
 * esp_hosted. Esperar uma trava ali (ate 50 ms) e chamar o driver de WiFi ou do
 * 802.15.4 (que esperam a tarefa do radio) atrasava todos os outros timers. O
 * callback so acorda a c6farejo, que ja dorme parada e faz a conta com calma. */
static void radio_guarda(int64_t now)
{
    if (!s_radio_mtx) { return; }
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    csi_guarda(now);
    z154_guarda(now);
    xSemaphoreGive(s_radio_mtx);
}

static void radio_guarda_cb(void *arg)
{
    (void)arg;
#if CONFIG_ESP_WIFI_CSI_ENABLED
    const bool csi = s_csi_lig;
#else
    const bool csi = false;
#endif
    /* Nada enabled: nem acorda ninguem (o C6 ocioso fica ocioso). */
    if ((csi || s_z154_lig) && s_fj_tarefa) { xTaskNotifyGive(s_fj_tarefa); }
}

/* ---------------------------------------------------------------------------
 * Vindo do P4
 * --------------------------------------------------------------------------- */

static void do_host(uint32_t msg_id, const uint8_t *data, size_t n, void *ctx)
{
    (void)ctx;

    switch (msg_id) {
    case MSG_HOST_VIVO:
        host_falou();
        break;

    case MSG_HOST_ENVIAR: {
        if (n <= ESP_NOW_ETH_ALEN) { return; }
        /* ESTATICO: o envio_t tem 1,5 KB e esta e a stack da pserial_task do
         * esp_hosted (4 KB, nao e nossa). So ela chama o do_host; o xQueueSend
         * copia antes de voltar. */
        static envio_t e;
        memcpy(e.mac, data, ESP_NOW_ETH_ALEN);
        size_t tam = n - ESP_NOW_ETH_ALEN;
        if (tam > ESP_NOW_MAX_DATA_LEN) { tam = ESP_NOW_MAX_DATA_LEN; }
        memcpy(e.texto, data + ESP_NOW_ETH_ALEN, tam);
        e.n = (uint16_t)tam;
        e.cru = false;
        /* Sem ESP-NOW no ar, ou queue cheia: falha IMEDIATA - senao a screen do
         * Tab5 esperaria a confirmacao para sempre. */
        if (!s_espnow_no_ar || !s_envio || xQueueSend(s_envio, &e, 0) != pdTRUE) {
            uint8_t st[ESP_NOW_ETH_ALEN + 1];
            memcpy(st, e.mac, ESP_NOW_ETH_ALEN);
            st[6] = 0;
            enfileirar(MSG_C6_ESTADO, st, sizeof(st));
        }
        break;
    }

    case MSG_HOST_ESPNOW:
        ligar_espnow();
        break;

    case C6_MSG_HOST_FAREJO:
        pedir_farejo(data, n);
        break;

    case C6_MSG_HOST_CRU: {
        if (n <= ESP_NOW_ETH_ALEN || !s_espnow_no_ar || !s_envio) { return; }
        static envio_t e;   /* estatico: ver MSG_HOST_ENVIAR */
        memcpy(e.mac, data, ESP_NOW_ETH_ALEN);
        size_t tam = n - ESP_NOW_ETH_ALEN;
        if (tam > ESP_NOW_MAX_DATA_LEN_V2) { tam = ESP_NOW_MAX_DATA_LEN_V2; }
        memcpy(e.texto, data + ESP_NOW_ETH_ALEN, tam);
        e.n = (uint16_t)tam;
        e.cru = true;
        xQueueSend(s_envio, &e, 0);
        break;
    }

    case C6_MSG_HOST_AJUSTE:
        if (n < 1) { return; }
        switch (data[0]) {
        case C6_AJ_INFO: responder_info(); break;
        case C6_AJ_CRU:  s_repassar_cru = (n >= 2 && data[1]); break;
        default: break;   /* chave nova de um P4 mais novo: ignora */
        }
        break;

    case C6_MSG_HOST_RADIO: {
        if (n < 1) { return; }
        const uint8_t  sub = data[0];
        const uint8_t *p = data + 1;
        const size_t   m = n - 1;
        if (!s_radio_mtx) { return; }
        xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
        switch (sub) {
        case C6_RX_CSI: { c6_csi_cfg_t c = { 0 }; memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
                          if (c.liga) { csi_ligar(&c); } else { csi_desligar(); } break; }
        case C6_RX_FTM: { c6_ftm_cfg_t c = { 0 }; memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
                          ftm_disparar(&c); break; }
        case C6_RX_154: { c6_154_cfg_t c = { 0 }; memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
                          if (c.liga) { z154_ligar(&c); } else { z154_desligar(); } break; }
        default: break;   /* sub-comando novo de um P4 mais novo: ignora */
        }
        xSemaphoreGive(s_radio_mtx);
        break;
    }

    case MSG_HOST_CANAL:
        /* TODO MUNDO TEM QUE ESTAR NO MESMO CANAL, e este e o reason numero um
         * de "nao funciona". Se o C6 estiver associado a um AP, quem manda no
         * canal e o AP - mudar aqui derrubaria a connection. Por isso so mexemos
         * when o P4 pede explicitamente. */
        if (n >= 1) {
            esp_err_t r = esp_wifi_set_channel(data[0], WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "canal -> %u: %s", (unsigned)data[0], esp_err_to_name(r));
            if (r == ESP_OK) { s_canal_volta = data[0]; }   /* o farejo iteration para este */
        }
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "firmware do C6 do Tab5 (esp_hosted + ESP-NOW + farejo), contrato v%d", C6_PROTO_VERSAO);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    /* Sem ESP_ERROR_CHECK, pelo mesmo reason do registro abaixo: nada aqui pode
     * derrubar o coprocessador. Falhar a NVS e ruim; abortar e MUITO pior. */
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(ret)); }
    esp_event_loop_create_default();

    /* Guarda de imagem nova ANTES de qualquer coisa que possa dar errado. */
    guarda_iniciar();

    s_saida = xRingbufferCreate(SAIDA_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (s_saida) {
        xTaskCreate(vTarefaSaida, "c6saida", 4096, NULL, 5, NULL);
    }
    /* A trava antes da c6farejo: e ela que roda o vigia dos extras do radio. */
    s_radio_mtx = xSemaphoreCreateMutex();
    /* Dorme num ulTaskNotifyTake ate o host pedir farejo (ou o vigia acordar):
     * parada, custa so a stack. 4096 como a c6saida: as duas chamam
     * eh_cp_feat_peer_data_send, que codifica o evento RPC na stack de quem chama. */
    xTaskCreate(vTarefaFarejo, "c6farejo", 4096, NULL, 4, &s_fj_tarefa);

    /* Vigia dos extras do radio (CSI/802.15.4): 1x/s acorda a c6farejo se algum
     * deles estiver enabled. */
    esp_timer_handle_t guarda = NULL;
    const esp_timer_create_args_t ga = { .callback = radio_guarda_cb, .name = "radio_guarda" };
    if (esp_timer_create(&ga, &guarda) == ESP_OK) {
        esp_timer_start_periodic(guarda, 1000000);
    }

    /* NADA DE ESP_ERROR_CHECK AQUI. Foi o defeito.
     *
     * As features do coprocessador (peer_data inclusive) sobem numa TAREFA
     * PROPRIA - o auto_feat_init_task do eh_cp_core -, nao antes do app_main.
     * Enquanto ela nao rodou, o register_callback devolve ESP_ERR_INVALID_STATE
     * ("Not initialized"), e ESP_ERROR_CHECK sobre isso ABORTA o C6.
     *
     * Abortar aqui e o pior desfecho possivel: o C6 entra em loop de reinicio,
     * o enlace SDIO negocia e morre a cada iteration, TODO RPC do host expira - e
     * como o aparelho nunca fica de pe time suficiente, nao da nem para gravar
     * firmware novo por OTA para consertar. O sintoma foi exatamente esse.
     *
     * Regra: NADA no app_main de um coprocessador pode abortar. Ele e a unica
     * via de recuperacao dele mesmo. */
    for (int tent = 0; ; ++tent) {
        const esp_err_t r = eh_cp_feat_peer_data_register_callback(MSG_HOST_VIVO, do_host, NULL);
        if (r == ESP_OK) { break; }
        if (tent == 0) {
            ESP_LOGW(TAG, "peer_data ainda nao subiu (%s) - esperando",
                     esp_err_to_name(r));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    eh_cp_feat_peer_data_register_callback(MSG_HOST_ENVIAR, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(MSG_HOST_CANAL,  do_host, NULL);
    eh_cp_feat_peer_data_register_callback(MSG_HOST_ESPNOW, do_host, NULL);
    /* Tabela do peer_data: 8 de 8 vagas, ja contado o VIVO (ver tab5_c6.h). */
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_FAREJO, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_CRU,    do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_AJUSTE, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_RADIO,  do_host, NULL);  /* 8o: tabela cheia */

    ESP_LOGI(TAG, "pronto - esperando o host mandar ligar o ESP-NOW");
}
