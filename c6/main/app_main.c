/*
 * ESP32-C6 coprocessor firmware for the M5Stack Tab5.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS
 * ---------------------------------------------------------------------------
 * The ESP32-P4 has no radio. All WiFi traffic goes through the C6 over SDIO
 * via esp_hosted. esp_hosted does NOT carry ESP-NOW: esp_now_init, _send, and
 * _register_recv_cb do not even link on the P4 side (its esp_now.h is merely
 * "injected" to make builds compile and has no implementation). Neither
 * esp_wifi_80211_tx nor promiscuous mode is proxied.
 *
 * Therefore ESP-NOW for the Tab5 must run HERE, where the radio is. The P4
 * communicates with this firmware through esp_hosted's peer_data channel—an
 * existing opaque byte pipe that needs no protobuf changes.
 *
 * ---------------------------------------------------------------------------
 * THIS REPLACES THE M5STACK FIRMWARE
 * ---------------------------------------------------------------------------
 * The Tab5 C6 ships with M5Stack firmware (esp_hosted 2.12.6, backed up at
 * c6/backup_fabrica_2.12.6.bin). The M5tabOS host is 3.0.7, and this firmware
 * uses the same version—the boot log confirms it ("host=3.0.7 coprocessor=3.0.7 (match)").
 *
 * But this is the firmware that makes WiFi work, which is why the TWO safety
 * nets below are needed.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "espnow.h" /* ONLY types from espressif/esp-now (see TWO DIALECTS) */
#include "esp_random.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include <stdlib.h> /* free the FTM report */
#if CONFIG_IEEE802154_ENABLED
#include "esp_ieee802154.h"
#include "esp_coexist.h"   /* esp_coex_wifi_i154_enable */
#include "esp_coex_i154.h" /* esp_coex_ieee802154_status_disable */
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
#include "tab5_c6.h" /* shared contract with the P4 */

static const char *TAG = "tab5c6";

/* ---------------------------------------------------------------------------
 * PROTOCOL WITH THE P4
 * ---------------------------------------------------------------------------
 * msg_ids are a contract between both sides and live in one file:
 * shared/tab5_c6.h, included by both projects.
 *
 * Payloads are always raw—no protobuf here. The channel already provides
 * (msg_id, bytes, length), so inventing serialization on top would be
 * pointless work: both sides are ours and are built together.
 */
#define MSG_HOST_VIVO C6_MSG_HOST_VIVO
#define MSG_HOST_ENVIAR C6_MSG_HOST_ENVIAR
#define MSG_HOST_CANAL C6_MSG_HOST_CANAL
#define MSG_HOST_ESPNOW C6_MSG_HOST_ESPNOW
#define MSG_C6_PRONTO C6_MSG_PRONTO
#define MSG_C6_RECEBIDO C6_MSG_RECEBIDO
#define MSG_C6_ESTADO C6_MSG_ESTADO

/* What THIS firmware can do is reported in PRONTO and INFO. The three radio
 * extras set their bits only when COMPILED in (depending on c6/'s sdkconfig),
 * so the P4 hides features missing from this build without reopening the device. */
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
#define CAPS (C6_CAP_ESPNOW | C6_CAP_FAREJO | C6_CAP_CRU | C6_CAP_INFO | CAP_CSI | CAP_FTM | CAP_154)

/* What THIS chip can do beyond what was compiled. The FTM initiator does not
 * exist on C6 revisions v0.0/v0.1: esp_wifi_init itself (esp_wifi/src/wifi_init.c)
 * clears its feature_caps bit on those revisions—a silicon erratum—and
 * initiate_session returns ESP_ERR_NOT_SUPPORTED ("FTM Initiator mode not
 * supported!"). Advertising that capability would promise something it cannot do. */
static uint8_t caps_reais(void)
{
    uint8_t c = CAPS;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    if (ci.model == CHIP_ESP32C6 && ci.revision <= 1)
    {
        c &= (uint8_t)~C6_CAP_FTM;
    }
    return c;
}

/* Chip revision for INFO: 0x80 marks it as "valid"; the remainder is
 * major*16+minor (esp_chip_info reports major*100+minor). */
static uint8_t chip_rev_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    return (uint8_t)(0x80 | (((ci.revision / 100) & 0x7) << 4) | ((ci.revision % 100) & 0xF));
}

static const uint8_t DIFUSAO[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Diagnostics for radio extras, returned in INFO: without the C6 serial log
 * (which requires the Downloader), this is the only way for the P4 to know where it stopped. */
static volatile int16_t s_csi_err = 0, s_z154_err = 0;
static volatile uint32_t s_csi_cb = 0, s_csi_env = 0, s_z154_rx = 0;
static volatile int16_t s_csi_err2 = 0;
static volatile bool s_z154_lig = false;                       /* 802.15.4 listening now */
static bool s_z154_habilitado __attribute__((unused)) = false; /* esp_ieee802154_enable completed (once) */
static volatile uint8_t s_csi_etapa = 0, s_csi_modo = 0;
/* Radio extras are enabled by the RPC task and disabled by the watchdog
 * (esp_timer); this lock keeps both from touching the driver at the same time. */
static SemaphoreHandle_t s_radio_mtx = NULL;

/* ---------------------------------------------------------------------------
 * SAFETY NET 2 - at the APPLICATION level
 * ---------------------------------------------------------------------------
 * Safety net 1 is CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in sdkconfig.defaults.
 * It works only with OUR bootloader: OTA does not replace the bootloader, and
 * the M5Stack one may lack rollback. A cable flash (idf.py flash) installs
 * this project's bootloader, partition table, and otadata together.
 *
 * This second net does not depend on it. If the P4 does not say "I am
 * listening" in time, WE point the bootloader to the previous partition and
 * reboot. This covers the likely OTA failure where we boot but the SDIO link
 * never comes up and the Tab5 loses WiFi. It applies only to an OTA image
 * (see NEW IMAGE GUARD); a cable-flashed image has nowhere to roll back to.
 *
 * What it does NOT cover: failure before this code runs (a boot panic loop).
 * That requires the bootloader or physical access to the C6.
 */
#define PRAZO_HOST_MS (C6_PRAZO_HOST_S * 1000)

/* NEW IMAGE GUARD - why bootloader rollback alone is not enough
 *
 * An OTA over the link reboots the C6 TWICE in succession: first by esp_hosted
 * (2 s after activate), then by the P4, which sees SDIO drop, reboots, and
 * resets the C6 again at boot (CP_RESET_STRATEGY_ALWAYS). The bootloader treats
 * "rebooted while still PENDING_VERIFY" as a broken image and rolls back—so
 * EVERY OTA would be undone, good or bad.
 *
 * The decision therefore moves here:
 *   - the bootloader covers only panics BEFORE app_main (the image never gets
 *     here); on the first boot of a new image, as soon as we arrive, we mark it
 *     valid for the bootloader;
 *   - from then on this guard decides, using NVS state ("tab5c6"): "pend" is
 *     a new image not yet confirmed by the P4, and "tent" counts its boots
 *     without confirmation. After GUARDA_MAX_BOOTS (a panic loop after
 *     app_main, with the link never coming up), or when the deadline expires
 *     without the P4's VIVO, it rolls back to the other slot;
 *   - a cable-flashed image never becomes pending: there is nowhere to roll back to. */
#define GUARDA_MAX_BOOTS 4

static esp_timer_handle_t s_carencia = NULL;
static bool s_confirmado = false;
static nvs_handle_t s_guarda = 0;
static volatile bool s_pendente = false;

static void save_image_guard(uint8_t pend, uint8_t tent)
{
    if (!s_guarda)
    {
        return;
    }
    nvs_set_u8(s_guarda, "pend", pend);
    nvs_set_u8(s_guarda, "tent", tent);
    nvs_commit(s_guarda);
}

static void abandon_and_revert(void *arg)
{
    (void)arg;
    if (s_confirmado)
    {
        return;
    }

    /* "THE OTHER slot" (next_update_partition), where the image that wrote
     * this one resides. NOT esp_ota_get_last_invalid_partition(): it finds only
     * INVALID/ABORTED partitions, a state that does not exist before rollback.
     * The factory partition is the fallback if one ever exists. */
    const esp_partition_t *cand[2] = {
        esp_ota_get_next_update_partition(NULL),
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL),
    };

    ESP_LOGE(TAG, "new image unconfirmed by host - reverting");

    for (int i = 0; i < 2; ++i)
    {
        if (!cand[i])
        {
            continue;
        }

        esp_app_desc_t d;
        if (esp_ota_get_partition_description(cand[i], &d) == ESP_OK)
        {
            ESP_LOGE(TAG, "trying '%s' (%s %s)", cand[i]->label,
                     d.project_name, d.version);
        }
        else
        {
            ESP_LOGE(TAG, "'%s' has no valid image - skipping", cand[i]->label);
            continue;
        }

        if (esp_ota_set_boot_partition(cand[i]) == ESP_OK)
        {
            /* Clear the guard FIRST: the rollback image reads the same NVS, and
             * with "pend" set it would also consider itself new and roll back again. */
            save_image_guard(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "'%s' rejected", cand[i]->label);
    }

    /* No rollback target: stay up and stop counting, or we would roll back on every boot. */
    save_image_guard(0, 0);
    s_pendente = false;
    ESP_LOGE(TAG, "NO ROLLBACK TARGET. Continuing anyway.");
}

/* Called from app_main just after NVS: decide whether this image is on trial. */
static void start_image_guard(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *rodando = esp_ota_get_running_partition();
    const bool chegou_por_ota = rodando && esp_ota_get_state_partition(rodando, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;

    uint8_t pend = 0, tent = 0;
    if (nvs_open("tab5c6", NVS_READWRITE, &s_guarda) != ESP_OK)
    {
        /* Without NVS there is no application guard: use bootloader rollback
         * and the classic grace period (VIVO marks the image valid). */
        s_guarda = 0;
        pend = chegou_por_ota ? 1 : 0;
    }
    else
    {
        if (chegou_por_ota)
        {
            pend = 1;
            tent = 0;
        }
        else
        {
            nvs_get_u8(s_guarda, "pend", &pend);
            nvs_get_u8(s_guarda, "tent", &tent);
        }
        if (pend)
        {
            ++tent;
        }
        save_image_guard(pend, tent);
        /* With the guard stored, the bootloader has already done its part. */
        if (chegou_por_ota)
        {
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    s_pendente = pend != 0;
    if (!s_pendente)
    {
        ESP_LOGI(TAG, "image confirmed: no grace period");
        return;
    }
    if (tent > GUARDA_MAX_BOOTS)
    {
        ESP_LOGE(TAG, "new image: %u boots without confirmation", (unsigned)tent);
        abandon_and_revert(NULL);
        return;
    }
    const esp_timer_create_args_t ta = {.callback = abandon_and_revert, .name = "grace_period"};
    if (esp_timer_create(&ta, &s_carencia) == ESP_OK)
    {
        esp_timer_start_once(s_carencia, (uint64_t)PRAZO_HOST_MS * 1000);
        ESP_LOGW(TAG, "NEW image on trial (boot %u of %u): no host VIVO in %d s, "
                      "returning to the previous image",
                 (unsigned)tent, GUARDA_MAX_BOOTS + 1,
                 PRAZO_HOST_MS / 1000);
    }
}

static void host_confirmed(void)
{
    if (s_confirmado)
    {
        return;
    }
    s_confirmado = true;
    if (s_carencia)
    {
        esp_timer_stop(s_carencia);
    }
    if (s_pendente)
    {
        save_image_guard(0, 0);
        s_pendente = false;
        ESP_LOGI(TAG, "host confirmed the link - new image is permanent");
    }
    /* Without NVS, this is where bootloader rollback is cancelled. */
    esp_ota_mark_app_valid_cancel_rollback();
}

/* ---------------------------------------------------------------------------
 * ESP-NOW
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * OUTPUT TO THE HOST - this is NOT optional
 * ---------------------------------------------------------------------------
 * Radio callbacks (ESP-NOW, CSI, promiscuous mode) run in the WiFi task and
 * must return quickly—the same "no blocking I/O in touch handling" rule applies on
 * the P4. eh_cp_feat_peer_data_send talks to the host over RPC and can BLOCK;
 * calling it from a callback stalls the entire WiFi stack, with the worst
 * possible symptom: the radio simply stops.
 *
 * The callback therefore writes only to the RING BUFFER and returns; the
 * The output task talks to the host. This is not a fixed-item queue: messages
 * range from 7 B (an ESTADO) to about 1 KB (HE CSI). A queue would reserve the
 * largest size for every item in RAM, and the producer would build a copy of
 * that size on the WiFi task's stack, which is not ours. The ring reserves the
 * exact space (SendAcquire) and writes directly into it: no intermediate copy.
 *
 * Item = msg_id (4 B) + body. Full means drop it (the host is not consuming;
 * blocking here would only make things worse; losing a record is better than
 * stopping the radio). */
#define SAIDA_RING_BYTES (12 * 1024)

static RingbufHandle_t s_saida = NULL;
static volatile uint32_t s_saida_perdas = 0;

/* In TWO parts (header + body), so callers do not need to join both in their
 * own buffer. Any task; NEVER from an ISR. */
static bool queue_message_parts(uint32_t msg_id, const uint8_t *cab, size_t ncab,
                                const uint8_t *body, size_t ncorpo)
{
    if (!s_saida)
    {
        return false;
    }
    uint8_t *it = NULL;
    if (xRingbufferSendAcquire(s_saida, (void **)&it, 4 + ncab + ncorpo, 0) != pdTRUE || !it)
    {
        s_saida_perdas++;
        return false;
    }
    memcpy(it, &msg_id, 4);
    memcpy(it + 4, cab, ncab);
    if (ncorpo)
    {
        memcpy(it + 4 + ncab, body, ncorpo);
    }
    xRingbufferSendComplete(s_saida, it);
    return true;
}

static void queue_message(uint32_t msg_id, const uint8_t *data, size_t n)
{
    queue_message_parts(msg_id, data, n, NULL, 0);
}

static void output_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        size_t n = 0;
        uint8_t *it = (uint8_t *)xRingbufferReceive(s_saida, &n, portMAX_DELAY);
        if (!it)
        {
            continue;
        }
        if (n > 4)
        {
            uint32_t msg_id;
            memcpy(&msg_id, it, 4);
            eh_cp_feat_peer_data_send(msg_id, it + 4, n - 4);
        }
        vRingbufferReturnItem(s_saida, it);
    }
}

/* ---------------------------------------------------------------------------
 * TWO ESP-NOW DIALECTS
 * ---------------------------------------------------------------------------
 * ESP-NOW carries only bytes; each firmware decides what they contain:
 *   - ESPRESSIF FORMAT: the espressif/esp-now component adds a header
 *     (version, type, size, anti-duplicate "magic", flags, addresses) and
 *     retransmits. This is the official Cardputer chat (M5Cardputer-UserDemo).
 *   - RAW TEXT: esp_now_send() directly with the text. Arduino, UIFlow, and
 *     simple examples—probably StickC and Basic.
 * The Tab5 supports BOTH. We do not run the library (it drops anything that
 * is not its own—raw text would never arrive—and its espnow_init aborts the
 * chip if ESP-NOW fails underneath, the 09/2026 failure mode). We borrow only
 * its TYPES (espnow.h), keeping the wire header identical by construction;
 * the acceptance rules below match its espnow_recv_cb (v2.5.3, with the same
 * over-the-air format as UserDemo v2.5.1).
 *
 * RECEIVE: if it passes the Espressif format rules, remove the header;
 * otherwise treat it as raw text. SEND: use dialects heard in the last 10
 * minutes; if nothing has been heard yet, use both (to be discoverable). */

typedef struct
{ /* = espnow_data_t from espnow.c */
    uint8_t type : 4;
    uint8_t version : 2;
    uint8_t : 2;
    uint8_t size;
    espnow_frame_head_t frame_head;
    uint8_t dest_addr[6];
    uint8_t src_addr[6];
    uint8_t payload[0];
} __attribute__((packed)) pacote_espressif_t;

#define ESPRESSIF_VERSAO 2 /* component's ESPNOW_VERSION */
#define ESPRESSIF_REPETE 3 /* copies with the same magic (receivers discard duplicates) */
#define DIALETO_VALIDADE_US (10LL * 60 * 1000000)

static uint8_t s_meu_mac[ESP_NOW_ETH_ALEN];
static uint16_t s_magicos[32]; /* latest 'magic' values: discard retransmissions */
static int s_magico_prox = 0, s_magicos_n = 0;
/* Two tasks touch the ring (WiFi receive and c6envio recording our own): the
 * lock is brief, with 32 comparisons. */
static portMUX_TYPE s_magico_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_ouviu_espressif_us = 0, s_ouviu_cru_us = 0;

static bool magico_visto(uint16_t m)
{
    bool visto = false;
    portENTER_CRITICAL(&s_magico_mux);
    /* Only FILLED entries: the ring starts at zero, and a legitimate magic 0
     * (the library chooses 0 through 65535) would otherwise be discarded as a duplicate. */
    for (int i = 0; i < s_magicos_n && !visto; ++i)
    {
        visto = (s_magicos[i] == m);
    }
    if (!visto)
    {
        s_magicos[s_magico_prox] = m;
        s_magico_prox = (s_magico_prox + 1) % 32;
        if (s_magicos_n < 32)
        {
            ++s_magicos_n;
        }
    }
    portEXIT_CRITICAL(&s_magico_mux);
    return visto;
}

/* A peer must be REGISTERED before any send, including broadcast. */
static esp_err_t ensure_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (esp_now_is_peer_exist(mac))
    {
        return ESP_OK;
    }
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.ifidx = WIFI_IF_STA;
    p.channel = 0;
    p.encrypt = false;
    esp_err_t r = esp_now_add_peer(&p);
    if (r == ESP_ERR_ESPNOW_FULL)
    {
        /* TABLE FULL (ESP_NOW_MAX_TOTAL_PEER_NUM = 20). Each new unicast
         * destination occupies a slot forever; at number 21, every send to a
         * new MAC would fail until C6 reboots. Release unicasts (keep broadcast)
         * and try again. Collect before erasing: erasing during fetch breaks
         * the driver's iteration. */
        uint8_t erase[ESP_NOW_MAX_TOTAL_PEER_NUM][ESP_NOW_ETH_ALEN];
        int n = 0;
        esp_now_peer_info_t q;
        for (bool start = true; n < ESP_NOW_MAX_TOTAL_PEER_NUM && esp_now_fetch_peer(start, &q) == ESP_OK; start = false)
        {
            if (memcmp(q.peer_addr, DIFUSAO, ESP_NOW_ETH_ALEN) != 0)
            {
                memcpy(erase[n++], q.peer_addr, ESP_NOW_ETH_ALEN);
            }
        }
        for (int i = 0; i < n; ++i)
        {
            esp_now_del_peer(erase[i]);
        }
        r = esp_now_add_peer(&p);
    }
    return r;
}

/* In the WiFi task context: choose the dialect, copy, and return. */
static volatile bool s_repassar_cru = false; /* C6_AJ_CRU */

static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int n)
{
    if (!info || !data || n <= 0)
    {
        return;
    }
    if (s_repassar_cru)
    {
        /* TRANSPARENT PIPE: the entire frame, uninterpreted, with RSSI. A new
         * format from another device becomes the P4's problem, without reflashing C6. */
        uint8_t cab[9];
        memcpy(cab, info->src_addr, ESP_NOW_ETH_ALEN);
        cab[6] = info->rx_ctrl ? (uint8_t)info->rx_ctrl->rssi : 0;
        cab[7] = info->rx_ctrl ? (uint8_t)info->rx_ctrl->channel : 0;
        cab[8] = (memcmp(info->des_addr, DIFUSAO, ESP_NOW_ETH_ALEN) == 0) ? 1 : 0;
        /* Up to 1470 B: ESP-NOW v2.0 (IDF 5.4+, newer C6/S3) exceeds 250. */
        queue_message_parts(C6_MSG_RECEBIDO_CRU, cab, sizeof(cab), data,
                            (size_t)(n > ESP_NOW_MAX_DATA_LEN_V2 ? ESP_NOW_MAX_DATA_LEN_V2 : n));
    }
    const uint8_t *texto = data;
    int tam = n;
    const uint8_t *de = info->src_addr;
    uint8_t marca = (memcmp(info->des_addr, DIFUSAO, ESP_NOW_ETH_ALEN) == 0) ? 1 : 0;

    const pacote_espressif_t *p = (const pacote_espressif_t *)data;
    if (n >= (int)sizeof(pacote_espressif_t) && p->version == ESPRESSIF_VERSAO && p->type < ESPNOW_DATA_TYPE_MAX && n == (int)(sizeof(pacote_espressif_t) + p->size))
    {
        /* ESPRESSIF FORMAT. Only DATA is a message; the rest (ACK, control) is
         * their protocol. Encrypted packets cannot be read. A retransmission
         * with an already-delivered 'magic' is discarded. */
        if (p->type != ESPNOW_DATA_TYPE_DATA || p->frame_head.security)
        {
            return;
        }
        if (memcmp(p->src_addr, s_meu_mac, ESP_NOW_ETH_ALEN) == 0)
        {
            return;
        }
        if (magico_visto(p->frame_head.magic))
        {
            return;
        }
        texto = p->payload;
        tam = p->size;
        de = p->src_addr; /* author (may have been forwarded by another device) */
        marca |= 2;       /* bit 1: arrived in Espressif format */
        s_ouviu_espressif_us = esp_timer_get_time();
    }
    else
    {
        s_ouviu_cru_us = esp_timer_get_time();
    }
    if (tam <= 0)
    {
        return;
    }

    uint8_t cab[7];
    memcpy(cab, de, ESP_NOW_ETH_ALEN);
    cab[6] = marca;
    const int c = tam > ESP_NOW_MAX_DATA_LEN ? ESP_NOW_MAX_DATA_LEN : tam;
    queue_message_parts(MSG_C6_RECEBIDO, cab, sizeof(cab), texto, (size_t)c);
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t st)
{
    (void)info;
    (void)st; /* status comes from esp_now_send's return (see send_task) */
}

/* SEND IN A DEDICATED TASK: Espressif-format copies need a delay between them,
 * and the request arrives in the peer_data callback, which cannot wait. */
typedef struct
{
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint16_t n;
    bool cru; /* C6_MSG_HOST_CRU: once, raw, no header, no ESTADO */
    /* Sized for v2.0 (1470): raw mode can send a large frame to v2.0 peers;
     * text (both dialects) remains capped at 250. */
    uint8_t texto[ESP_NOW_MAX_DATA_LEN_V2];
} envio_t;
static QueueHandle_t s_envio = NULL;

static void send_task(void *arg)
{
    (void)arg;
    static envio_t e;
    static uint8_t frame[ESP_NOW_MAX_DATA_LEN];
    for (;;)
    {
        if (xQueueReceive(s_envio, &e, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        bool ok = (ensure_peer(e.mac) == ESP_OK);
        if (e.cru)
        {
            if (ok)
            {
                esp_now_send(e.mac, e.texto, e.n);
            }
            continue;
        }

        const int64_t now = esp_timer_get_time();
        bool espressif = s_ouviu_espressif_us && (now - s_ouviu_espressif_us) < DIALETO_VALIDADE_US;
        bool cru = s_ouviu_cru_us && (now - s_ouviu_cru_us) < DIALETO_VALIDADE_US;
        if (!espressif && !cru)
        {
            espressif = cru = true;
        } /* nothing heard: use both */

        bool algum = false;
        if (ok && cru)
        {
            algum |= (esp_now_send(e.mac, e.texto, e.n) == ESP_OK);
        }
        if (ok && espressif)
        {
            /* Header + text within the SAME 250-byte limit (ESP-NOW v1.0,
             * used by ESP32/S3 devices). */
            const size_t cab = sizeof(pacote_espressif_t);
            size_t t = e.n;
            if (cab + t > ESP_NOW_MAX_DATA_LEN)
            {
                t = ESP_NOW_MAX_DATA_LEN - cab;
            }
            memset(frame, 0, cab);
            pacote_espressif_t *p = (pacote_espressif_t *)frame;
            p->version = ESPRESSIF_VERSAO;
            p->type = ESPNOW_DATA_TYPE_DATA;
            p->size = (uint8_t)t;
            uint16_t m = (uint16_t)esp_random();
            if (m == 0)
            {
                m = 1;
            }
            p->frame_head.magic = m;
            uint8_t canal = 0;
            wifi_second_chan_t sec;
            esp_wifi_get_channel(&canal, &sec);
            p->frame_head.channel = canal;
            p->frame_head.broadcast = (memcmp(e.mac, DIFUSAO, ESP_NOW_ETH_ALEN) == 0);
            p->frame_head.retransmit_count = ESPRESSIF_REPETE;
            memcpy(p->dest_addr, e.mac, ESP_NOW_ETH_ALEN);
            memcpy(p->src_addr, s_meu_mac, ESP_NOW_ETH_ALEN);
            memcpy(p->payload, e.texto, t);
            magico_visto(m); /* do not treat our own forwarded echo as a message */
            for (int k = 0; k < ESPRESSIF_REPETE; ++k)
            {
                algum |= (esp_now_send(e.mac, frame, cab + t) == ESP_OK);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        uint8_t st[ESP_NOW_ETH_ALEN + 1];
        memcpy(st, e.mac, ESP_NOW_ETH_ALEN);
        st[6] = algum ? 1 : 0;
        queue_message(MSG_C6_ESTADO, st, sizeof(st));
    }
}

/* ENABLE ESP-NOW AND HONOR THE HOST'S DECISION (it sends this when WiFi is up).
 * Nothing aborts here: on failure, give up quietly—the host retries in 5 s. */
static bool s_espnow_no_ar = false;

static void enable_espnow(void)
{
    /* ESP-NOW survives esp_wifi_stop/start, but NOT esp_wifi_deinit:
     * esp_hosted's handler does deinit+init when the host sends a different
     * init config, and the P4 may disable WiFi at level 2 (deinit). Without
     * this check, s_espnow_no_ar would remain true with ESP-NOW dead underneath,
     * and all traffic would silently disappear until C6 rebooted. */
    uint32_t ver;
    if (s_espnow_no_ar && esp_now_get_version(&ver) != ESP_OK)
    {
        ESP_LOGW(TAG, "ESP-NOW went down underneath (WiFi restarted) - re-enabling");
        s_espnow_no_ar = false;
    }
    if (!s_espnow_no_ar)
    {
        const esp_err_t r = esp_now_init();
        if (r != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_now_init: %s - is WiFi really up?", esp_err_to_name(r));
            return;
        }
        esp_now_register_recv_cb(on_receive);
        esp_now_register_send_cb(on_send);
        ensure_peer(DIFUSAO);
        esp_wifi_get_mac(WIFI_IF_STA, s_meu_mac);
        if (!s_envio)
        {
            s_envio = xQueueCreate(4, sizeof(envio_t));
            if (s_envio)
            {
                xTaskCreate(send_task, "c6send", 4096, NULL, 4, NULL);
            }
        }
        s_espnow_no_ar = true;
    }
    /* Always send PRONTO (the host repeats the request until it sees this). */
    uint8_t canal = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&canal, &sec);
    uint8_t msg[ESP_NOW_ETH_ALEN + 3];
    memcpy(msg, s_meu_mac, ESP_NOW_ETH_ALEN);
    msg[6] = canal;
    msg[7] = C6_PROTO_VERSAO;
    msg[8] = caps_reais();
    queue_message(MSG_C6_PRONTO, msg, sizeof(msg));
    ESP_LOGI(TAG, "ESP-NOW active (Espressif format + raw text): channel %u", (unsigned)canal);
}

/* ---------------------------------------------------------------------------
 * SNIFFER - promiscuous mode, summarized on the C6
 * ---------------------------------------------------------------------------
 * The contract (format, validity, channels) is in tab5_c6.h. This is the how:
 *
 * WHO TOUCHES WHAT. The promiscuous callback runs in the WiFi task for every
 * frame—one thousand or more per second on a busy channel. It only finds the
 * MAC in the table and updates it; no allocation, logging, or host traffic.
 * Enabling, disabling, channel hopping, and sending batches all happen in the
 * sniffer task, the ONLY caller of esp_wifi_set_promiscuous*. The host request
 * (arriving in the RPC task) only stores the new configuration and wakes the sniffer.
 *
 * TWO TABLES. The callback writes to the active table; when a batch closes,
 * The sniffer swaps the active table (one line under the lock) and reads the
 * other without a lock, calmly, while frames continue arriving in the new one.
 *
 * MAC hash table with linear probing: 128 slots for at most 96 MACs per period
 * (3/4 full, so searches stop after few comparisons). Beyond 96, count it as
 * lost—the P4 sees that in the batch and can shorten the period.
 */
#define FJ_VAGAS 128
#define FJ_MAX 96

typedef struct
{
    uint8_t mac[6];
    int8_t ult, max;
    int16_t soma; /* 255 x -128 fits */
    uint8_t n, tipos, canal, usado;
} fj_ent_t; /* 16 B */

typedef struct
{
    fj_ent_t e[FJ_VAGAS];
    uint16_t usados, perdidos;
    uint32_t quadros;
} fj_tab_t;

static fj_tab_t s_fj[2];
static volatile uint8_t s_fj_ativa = 0;
static portMUX_TYPE s_fj_mux = portMUX_INITIALIZER_UNLOCKED;

/* Read by the callback for each frame: simple copies of the active s_cfg. */
static volatile int8_t s_fj_rssi_min = -128;
static volatile bool s_fj_tem_alvo = false;
static uint8_t s_fj_alvo[6];
static volatile uint32_t s_fj_quadros_total = 0;

/* Host request -> sniffer (under s_fj_mux). */
static c6_farejo_cfg_t s_fj_pedido;
static bool s_fj_tem_pedido = false;
static int64_t s_fj_validade_us = 0;

static volatile bool s_farejando = false;
static TaskHandle_t s_fj_tarefa = NULL;
/* Where the radio returns when the sniffer stops: the channel used when it
 * started, or the one the host requested AFTERWARD (C6_MSG_HOST_CANAL during
 * sniffing). Without this, disabling the sniffer would undo ESP-NOW's requested
 * "fixed channel" midway through operation. */
static volatile uint8_t s_canal_volta = 0;

static void radio_watchdog(int64_t now); /* radio extras, below */

static void on_sniff(void *buf, wifi_promiscuous_pkt_type_t tipo)
{
    const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
    if (!pk || pk->rx_ctrl.sig_len < 16)
    {
        return;
    } /* no addr2 */
    const int8_t rssi = (int8_t)pk->rx_ctrl.rssi;
    if (rssi < s_fj_rssi_min)
    {
        return;
    }

    const uint8_t *q = pk->payload;
    const uint8_t sub = q[0] >> 4;
    uint8_t t;
    if (tipo == WIFI_PKT_MGMT)
    {
        t = (sub == 8 || sub == 5) ? C6_FT_BEACON
            : (sub == 4)           ? C6_FT_PROCURA
                                   : C6_FT_GERENCIA;
    }
    else if (tipo == WIFI_PKT_DATA)
    {
        const uint8_t ds = q[1] & 0x03;
        t = C6_FT_DADOS | (ds == 1 ? C6_FT_PARA_AP : ds == 2 ? C6_FT_DO_AP
                                                             : 0);
    }
    else
    {
        return;
    }

    const uint8_t *ta = q + 10; /* addr2 = transmitter, in management and data frames */
    if (s_fj_tem_alvo && memcmp(ta, s_fj_alvo, 6) != 0)
    {
        return;
    }
    const uint8_t canal = (uint8_t)pk->rx_ctrl.channel;

    /* The last 3 bytes vary between devices; the first 3 identify the manufacturer. */
    unsigned h = ((unsigned)ta[5] * 31u + ta[4] * 7u + ta[3]) & (FJ_VAGAS - 1);

    portENTER_CRITICAL(&s_fj_mux);
    fj_tab_t *tb = &s_fj[s_fj_ativa];
    tb->frames++;
    for (int k = 0; k < FJ_VAGAS; ++k, h = (h + 1) & (FJ_VAGAS - 1))
    {
        fj_ent_t *e = &tb->e[h];
        if (!e->usado)
        {
            if (tb->usados >= FJ_MAX)
            {
                tb->perdidos++;
                break;
            }
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
        if (memcmp(e->mac, ta, 6) == 0)
        {
            e->ult = rssi;
            if (rssi > e->max)
            {
                e->max = rssi;
            }
            if (e->n < 255)
            {
                e->soma += rssi;
                e->n++;
            }
            e->tipos |= t;
            e->channel = canal;
            break;
        }
    }
    portEXIT_CRITICAL(&s_fj_mux);
    s_fj_quadros_total++;
}

/* Swap the active table and send what the old one collected, in LOTE_MAX-sized chunks. */
static void close_batch(void)
{
    static uint8_t msg[sizeof(c6_farejo_lote_t) + C6_FAREJO_LOTE_MAX * sizeof(c6_farejo_reg_t)];

    portENTER_CRITICAL(&s_fj_mux);
    fj_tab_t *tb = &s_fj[s_fj_ativa];
    s_fj_ativa ^= 1;
    portEXIT_CRITICAL(&s_fj_mux);

    if (tb->usados)
    {
        uint8_t canal = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&canal, &sec);
        c6_farejo_lote_t *cab = (c6_farejo_lote_t *)msg;
        c6_farejo_reg_t *r = (c6_farejo_reg_t *)(msg + sizeof(*cab));
        int n = 0;
        for (int i = 0; i < FJ_VAGAS; ++i)
        {
            const fj_ent_t *e = &tb->e[i];
            if (!e->usado)
            {
                continue;
            }
            memcpy(r[n].mac, e->mac, 6);
            r[n].rssi_ult = e->ult;
            r[n].rssi_max = e->max;
            r[n].rssi_med = (int8_t)(e->soma / (int)e->n);
            r[n].n = e->n;
            r[n].tipos = e->tipos;
            r[n].channel = e->channel;
            if (++n == C6_FAREJO_LOTE_MAX)
            {
                cab->channel = canal;
                cab->n = (uint8_t)n;
                cab->perdidos = tb->perdidos;
                cab->frames = tb->frames > 0xFFFF ? 0xFFFF : (uint16_t)tb->frames;
                eh_cp_feat_peer_data_send(C6_MSG_FAREJO, msg, sizeof(*cab) + n * sizeof(*r));
                n = 0;
            }
        }
        if (n)
        {
            cab->channel = canal;
            cab->n = (uint8_t)n;
            cab->perdidos = tb->perdidos;
            cab->frames = tb->frames > 0xFFFF ? 0xFFFF : (uint16_t)tb->frames;
            eh_cp_feat_peer_data_send(C6_MSG_FAREJO, msg, sizeof(*cab) + n * sizeof(*r));
        }
    }
    /* Nothing heard: nothing sent. Silence on SDIO and the P4 can sleep. */
    memset(tb, 0, sizeof(*tb));
}

static bool is_associated(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static void sniffer_task(void *arg)
{
    (void)arg;
    c6_farejo_cfg_t cfg = {0}; /* current RAW request: only used for comparison */
    uint16_t lote_ms = 250, perm_ms = 200, mascara = 0;
    bool saltou = false;
    int64_t prox_lote = 0, prox_salto = 0;
    int canal_idx = 0;

    for (;;)
    {
        /* 1. New host request? */
        c6_farejo_cfg_t novo;
        bool tem = false;
        int64_t validade;
        portENTER_CRITICAL(&s_fj_mux);
        if (s_fj_tem_pedido)
        {
            novo = s_fj_pedido;
            s_fj_tem_pedido = false;
            tem = true;
        }
        validade = s_fj_validade_us;
        portEXIT_CRITICAL(&s_fj_mux);

        const int64_t now = esp_timer_get_time();
        /* This task also disables CSI/802.15.4 abandoned by the host; the
         * watchdog's esp_timer only wakes it (see radio_watchdog_cb). */
        radio_watchdog(now);
        bool stop = s_farejando && now > validade;
        if (stop)
        {
            ESP_LOGW(TAG, "sniffer: host did not renew - disabling");
        }

        if (tem && !novo.liga)
        {
            stop = s_farejando;
            tem = false;
        }

        if (stop)
        {
            esp_wifi_set_promiscuous(false);
            s_farejando = false;
            if (saltou && s_canal_volta && !is_associated())
            {
                esp_wifi_set_channel(s_canal_volta, WIFI_SECOND_CHAN_NONE);
            }
            saltou = false;
            portENTER_CRITICAL(&s_fj_mux);
            memset(s_fj, 0, sizeof(s_fj));
            portEXIT_CRITICAL(&s_fj_mux);
            ESP_LOGI(TAG, "sniffer disabled");
        }

        if (tem)
        {
            const bool changed = !s_farejando || memcmp(&novo, &cfg, sizeof(cfg)) != 0;
            if (changed)
            {
                cfg = novo;
                lote_ms = cfg.lote_ms ? cfg.lote_ms : 250;
                if (lote_ms < 50)
                {
                    lote_ms = 50;
                }
                perm_ms = cfg.permanencia_ms ? cfg.permanencia_ms : 200;
                if (perm_ms < 50)
                {
                    perm_ms = 50;
                }
                mascara = cfg.canais & 0x3FFE; /* channels 1..13 */
                s_fj_rssi_min = cfg.rssi_min ? cfg.rssi_min : -128;
                static const uint8_t zero[6] = {0};
                memcpy(s_fj_alvo, cfg.alvo, 6);
                s_fj_tem_alvo = memcmp(cfg.alvo, zero, 6) != 0;

                const uint8_t f = cfg.filtro ? cfg.filtro : (C6_FJ_MGMT | C6_FJ_DADOS);
                wifi_promiscuous_filter_t filtro = {.filter_mask =
                                                        ((f & C6_FJ_MGMT) ? WIFI_PROMIS_FILTER_MASK_MGMT : 0) |
                                                        ((f & C6_FJ_DADOS) ? WIFI_PROMIS_FILTER_MASK_DATA : 0)};
                esp_wifi_set_promiscuous_filter(&filtro);
            }
            if (!s_farejando)
            {
                wifi_second_chan_t sec;
                uint8_t canal_antes = 0;
                esp_wifi_get_channel(&canal_antes, &sec);
                s_canal_volta = canal_antes;
                esp_wifi_set_promiscuous_rx_cb(on_sniff);
                const esp_err_t r = esp_wifi_set_promiscuous(true);
                if (r != ESP_OK)
                {
                    /* WiFi may be stopped, for example. The host renews in
                     * seconds and we try again—never abort. */
                    ESP_LOGW(TAG, "sniffer: promiscuous mode rejected (%s)", esp_err_to_name(r));
                }
                else
                {
                    s_farejando = true;
                    prox_lote = now + (int64_t)lote_ms * 1000;
                    prox_salto = now;
                    canal_idx = 0;
                    ESP_LOGI(TAG, "sniffer enabled: channel %u, mask 0x%04x, batch %u ms",
                             (unsigned)s_canal_volta, (unsigned)mascara, (unsigned)lote_ms);
                }
            }
            else
            {
                /* Renewal: WiFi may have been stopped and restarted, and
                 * esp_wifi_stop disables promiscuous mode underneath. */
                bool enabled = false;
                if (esp_wifi_get_promiscuous(&enabled) == ESP_OK && !enabled)
                {
                    esp_wifi_set_promiscuous_rx_cb(on_sniff);
                    esp_wifi_set_promiscuous(true);
                }
            }
        }

        if (!s_farejando)
        {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* stopped: zero CPU */
            continue;
        }

        /* 2. Channel hopping—only when free of the router. */
        const bool um_so = mascara && !(mascara & (mascara - 1));
        if (mascara && now >= prox_salto)
        {
            if (!is_associated())
            {
                int c = 0;
                for (int k = 0; k < 14 && !c; ++k)
                {
                    canal_idx = canal_idx % 13 + 1;
                    if (mascara & (1u << canal_idx))
                    {
                        c = canal_idx;
                    }
                }
                uint8_t current = 0;
                wifi_second_chan_t sec;
                esp_wifi_get_channel(&current, &sec);
                if (c && c != current)
                {
                    esp_wifi_set_channel((uint8_t)c, WIFI_SECOND_CHAN_NONE);
                    saltou = true;
                }
            }
            /* One channel: check every second (someone may have changed it);
             * multiple channels: use the hop interval. */
            prox_salto = now + (int64_t)(um_so ? 1000 : perm_ms) * 1000;
        }

        /* 3. Batch. */
        if (now >= prox_lote)
        {
            close_batch();
            prox_lote += (int64_t)lote_ms * 1000;
            if (prox_lote <= now)
            {
                prox_lote = now + (int64_t)lote_ms * 1000;
            }
        }

        /* 4. Sleep until the next event—or a host request. */
        int64_t prox = prox_lote;
        if (mascara && prox_salto < prox)
        {
            prox = prox_salto;
        }
        if (validade < prox)
        {
            prox = validade + 1000;
        }
        int64_t esp = (prox - esp_timer_get_time()) / 1000;
        if (esp < 1)
        {
            esp = 1;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(esp));
    }
}

/* Called in the RPC task: only record the request and wake the sniffer. */
static void request_sniffer(const uint8_t *data, size_t n)
{
    c6_farejo_cfg_t c = {0};
    memcpy(&c, data, n < sizeof(c) ? n : sizeof(c));
    portENTER_CRITICAL(&s_fj_mux);
    s_fj_pedido = c;
    s_fj_tem_pedido = true;
    s_fj_validade_us = esp_timer_get_time() + (int64_t)C6_FAREJO_VALIDADE_MS * 1000;
    portEXIT_CRITICAL(&s_fj_mux);
    if (s_fj_tarefa)
    {
        xTaskNotifyGive(s_fj_tarefa);
    }
}

static void send_info(void)
{
    c6_info_t i = {0};
    uint8_t canal = 0;
    wifi_second_chan_t sec;
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
    i.flags = (esp_core_dump_image_check() == ESP_OK ? C6_INFO_F_COREDUMP : 0) | (s_pendente ? C6_INFO_F_EM_PROVA : 0);
#if CONFIG_IEEE802154_ENABLED
    i.z154_estado = s_z154_lig ? 1 : (s_z154_habilitado ? 2 : 0);
#endif
    queue_message(C6_MSG_INFO, (const uint8_t *)&i, sizeof(i));
}

/* ===========================================================================
 * RADIO EXTRAS - CSI, FTM, 802.15.4
 * ===========================================================================
 * Each sleeps until the P4 requests it (C6_MSG_HOST_RADIO), and each CAPS bit
 * is set only when compiled in—a firmware flashed without the sdkconfig flag
 * lets the P4 hide the feature without reopening the device.
 *
 * The same discipline as the sniffer: raw forwarding leaves the callback
 * (radio task) only through the output queue and never blocks; CSI and 802.15.4
 * disable themselves if the P4 does not renew within C6_RADIO_VALIDADE_MS;
 * FTM is one-shot.
 */

/* ---- CSI ---- */
#if CONFIG_ESP_WIFI_CSI_ENABLED
static volatile bool s_csi_lig = false;
static volatile int64_t s_csi_validade = 0, s_csi_prox_us = 0;
static volatile uint16_t s_csi_intervalo_ms = 100;
static volatile int8_t s_csi_rssi_min = -128;
static volatile bool s_csi_so_alvo = false;
static uint8_t s_csi_alvo[6];

static void on_csi(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    s_csi_cb++;
    if (!info || !info->buf || info->len <= 0)
    {
        return;
    }
    const int8_t rssi = (int8_t)info->rx_ctrl.rssi;
    if (rssi < s_csi_rssi_min)
    {
        return;
    }
    if (s_csi_so_alvo && memcmp(info->mac, s_csi_alvo, 6) != 0)
    {
        return;
    }
    const int64_t now = esp_timer_get_time();
    /* CADENCE RATE, not "interval since the last one". When the source and
     * limit have the same rate (10 Hz ping, 100 ms limit), variable air delay
     * made half the frames arrive 95 ms after the previous one and be dropped:
     * a test produced 64 records out of 110. Cadence advances in fixed steps
     * and accepts the first frame of each step; after a long delay, restart at now. */
    const int64_t passo = (int64_t)s_csi_intervalo_ms * 1000;
    if (now < s_csi_prox_us)
    {
        return;
    }
    s_csi_prox_us = (now - s_csi_prox_us > passo) ? now + passo : s_csi_prox_us + passo;

    c6_csi_cab_t cab;
    memcpy(cab.mac, info->mac, 6);
    cab.rssi = rssi;
    cab.channel = (uint8_t)info->rx_ctrl.channel;
    cab.sec_canal = (uint8_t)info->rx_ctrl.second;
    int len = info->len;
    /* What the P4 needs to interpret the bytes: frame format (each has a
     * different set of subcarriers), whether 40 MHz is used, and whether the
     * first word is valid. On C6 it may be invalid due to a hardware limit;
     * readers unaware of this would interpret two garbage subcarriers. */
    cab.flags = (uint8_t)((info->rx_ctrl.cur_bb_format & 0x0F) << 4) | (info->rx_ctrl.second ? C6_CSI_F_HT40 : 0) | (info->first_word_invalid ? C6_CSI_F_W0_RUIM : 0) | (len > C6_CSI_MAX ? C6_CSI_F_CORTADO : 0);
    if (len > C6_CSI_MAX)
    {
        len = C6_CSI_MAX;
    }
    cab.len = (uint16_t)len;
    cab.t_us = (uint32_t)now;
    if (queue_message_parts(C6_MSG_CSI, (const uint8_t *)&cab, sizeof(cab),
                            (const uint8_t *)info->buf, (size_t)len))
    {
        s_csi_env++;
    }
}

/* WHY CSI RETURNED ESP_FAIL (fixed on the P4 side)
 *
 * The driver's esp_wifi_set_csi* checks a field saved by esp_wifi_init:
 * wifi_init_config_t's csi_enable. When zero, it logs "CSI not enabled in
 * menuconfig!" and returns ESP_FAIL before doing anything else. With
 * esp_hosted, the RPC handler calls esp_wifi_init here using the fields sent
 * by the P4 (get_merged_init_config: "always use value from host"), and the
 * P4 was sending zero. CONFIG_ESP_WIFI_CSI_ENABLED in this sdkconfig only
 * changes WIFI_INIT_CONFIG_DEFAULT, which the handler overwrites.
 *
 * It was neither call order nor promiscuous mode: after the check, the driver
 * only writes the filter to hardware and no longer fails. One path, following
 * Espressif's example; INFO records the step where it stopped, if any. */
static esp_err_t csi_aplicar(uint8_t *etapa)
{
    /* Start with the driver's configuration; untouched fields remain as it expects. */
    wifi_csi_config_t cfg;
    if (esp_wifi_get_csi_config(&cfg) != ESP_OK)
    {
        memset(&cfg, 0, sizeof(cfg));
    }
    cfg.enable = 1;
    cfg.acquire_csi_legacy = 1; /* L-LTF: beacons and 11b/g frames—every device */
    cfg.acquire_csi_ht20 = 1;   /* HT-LTF: 802.11n */
    cfg.acquire_csi_ht40 = 1;
    cfg.acquire_csi_su = 1;      /* HE-LTF: 802.11ax SU, sent by a WiFi 6 router to C6 */
    cfg.acquire_csi_he_stbc = 2; /* as in Espressif's esp-csi for C6 */
    esp_err_t r;
    *etapa = 1;
    r = esp_wifi_set_csi_config(&cfg);
    if (r != ESP_OK)
    {
        return r;
    }
    *etapa = 2;
    r = esp_wifi_set_csi_rx_cb(on_csi, NULL);
    if (r != ESP_OK)
    {
        return r;
    }
    *etapa = 3;
    r = esp_wifi_set_csi(true);
    if (r != ESP_OK)
    {
        return r;
    }
    *etapa = 0;
    return ESP_OK;
}

static void enable_csi(const c6_csi_cfg_t *c)
{
    s_csi_intervalo_ms = c->min_intervalo_ms ? c->min_intervalo_ms : 100;
    s_csi_rssi_min = c->rssi_min ? c->rssi_min : -128;
    static const uint8_t zero[6] = {0};
    s_csi_so_alvo = c->so_alvo && memcmp(c->alvo, zero, 6) != 0;
    memcpy(s_csi_alvo, c->alvo, 6);
    s_csi_validade = esp_timer_get_time() + (int64_t)C6_RADIO_VALIDADE_MS * 1000;
    /* Reapplied on EVERY renewal, not only the first: the P4 may stop and
     * restart WiFi midway (Config), and esp_wifi_stop disables CSI underneath.
     * With "first time only," the P4 would renew a dead CSI forever—the same
     * trap handled by the sniffer during its renewal. */
    uint8_t etapa = 0;
    const esp_err_t r = csi_aplicar(&etapa);
    s_csi_err = s_csi_err2 = (int16_t)r; /* latest attempt; P4 reads this in INFO */
    s_csi_etapa = etapa;
    if (r == ESP_OK)
    {
        if (!s_csi_lig)
        {
            ESP_LOGI(TAG, "CSI enabled (interval %u ms)", (unsigned)s_csi_intervalo_ms);
        }
        s_csi_lig = true;
    }
    else
    {
        if (!s_csi_lig)
        {
            ESP_LOGW(TAG, "CSI rejected: %s (step %u)%s", esp_err_to_name(r), (unsigned)etapa,
                     r == ESP_FAIL ? " - host initialized WiFi with csi_enable 0" : "");
        }
        s_csi_lig = false;
    }
    s_csi_modo = s_csi_lig ? 1 : 0;
}
static void disable_csi(void)
{
    if (s_csi_lig)
    {
        esp_wifi_set_csi(false);
        s_csi_lig = false;
        ESP_LOGI(TAG, "CSI disabled");
    }
    s_csi_modo = 0;
}
static void csi_watchdog(int64_t now)
{
    if (s_csi_lig && now > s_csi_validade)
    {
        ESP_LOGW(TAG, "CSI: host did not renew");
        disable_csi();
    }
}
#else
static void enable_csi(const c6_csi_cfg_t *c) { (void)c; }
static void disable_csi(void) {}
static void csi_watchdog(int64_t a) { (void)a; }
#endif

/* ---- FTM (one-shot) ---- */
#if CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
static bool s_ftm_evt = false;

static void on_ftm_report(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != WIFI_EVENT_FTM_REPORT || !data)
    {
        return;
    }
    const wifi_event_ftm_report_t *r = (const wifi_event_ftm_report_t *)data;
    c6_ftm_rep_t rep;
    memcpy(rep.peer, r->peer_mac, 6);
    rep.status = (uint8_t)r->status;
    rep.num = r->ftm_report_num_entries;
    rep.rtt_est_ns = r->rtt_est;
    rep.dist_est_cm = r->dist_est;
    rep.rtt_raw_ns = r->rtt_raw;
    queue_message(C6_MSG_FTM, (const uint8_t *)&rep, sizeof(rep));
    /* The detailed report does not go to the P4 (only the summary); free it to avoid leaking. */
    if (r->ftm_report_data)
    {
        free(r->ftm_report_data);
    }
}

static void start_ftm(const c6_ftm_cfg_t *c)
{
    if (!s_ftm_evt)
    {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_ftm_report, NULL);
        s_ftm_evt = true;
    }
    wifi_ftm_initiator_cfg_t cfg = {
        .channel = c->channel,
        .frm_count = c->frm_count,
        .burst_period = c->burst_period,
    };
    memcpy(cfg.resp_mac, c->peer, 6);
    const esp_err_t r = esp_wifi_ftm_initiate_session(&cfg);
    if (r != ESP_OK)
    {
        ESP_LOGW(TAG, "FTM rejected (%s)", esp_err_to_name(r));
        c6_ftm_rep_t rep;
        memset(&rep, 0, sizeof(rep));
        memcpy(rep.peer, c->peer, 6);
        rep.status = C6_FTM_NAO_INICIOU;
        rep.rtt_raw_ns = (uint32_t)(int32_t)r; /* reason; without it P4 would only know "failed" */
        queue_message(C6_MSG_FTM, (const uint8_t *)&rep, sizeof(rep));
    }
}
#else
static void start_ftm(const c6_ftm_cfg_t *c) { (void)c; }
#endif

/* ---- 802.15.4 (the other radio: raw Thread/Zigbee) ---- */
#if CONFIG_IEEE802154_ENABLED
static volatile int64_t s_z154_validade = 0, s_z154_ult_us = 0;
static volatile uint16_t s_z154_intervalo_ms = 0;

/* WEAK driver symbol: ours wins and is called for every received frame.
 *
 * WARNING: the driver calls this DIRECTLY from the 802.15.4 radio ISR (there
 * is no task in between). Only FromISR variants are valid here: a normal
 * xQueueSend (queue_message_parts) triggers a FreeRTOS assertion on the first frame
 * and takes down C6. The buffer is static: the ISR does not re-enter, and its
 * stack is small. */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    /* msg_id + header + frame, assembled here and copied once into the ring
     * (the FromISR variant has no Acquire). Static: the ISR does not re-enter,
     * and its stack is small. */
    static uint8_t it[4 + sizeof(c6_154_cab_t) + C6_154_MAX];
    s_z154_rx++;
    if (frame && info && s_saida)
    {
        const int64_t now = esp_timer_get_time();
        if (!s_z154_intervalo_ms || now - s_z154_ult_us >= (int64_t)s_z154_intervalo_ms * 1000)
        {
            s_z154_ult_us = now;
            uint8_t len = frame[0]; /* frame[0] = length; frame follows (FCS replaced by RSSI/LQI) */
            if (len > C6_154_MAX)
            {
                len = C6_154_MAX;
            }
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
            if (xRingbufferSendFromISR(s_saida, it, 4 + sizeof(cab) + len, &acordou) != pdTRUE)
            {
                s_saida_perdas++;
            }
            if (acordou)
            {
                portYIELD_FROM_ISR();
            }
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

/* LISTEN ONLY, NEVER RESPOND. The driver starts with automatic ACK enabled;
 * in promiscuous mode (no address filter), it would ACK frames from ANY mesh
 * device requesting confirmation. The sender would believe the destination
 * received a frame when it may not have, and the neighboring Zigbee/Thread
 * network would lose messages because of us. No public API exists for this;
 * the two functions below are private driver APIs (esp_ieee802154_pib.h) and
 * take effect on the next esp_ieee802154_receive(), which reloads pending config. */
extern void ieee802154_pib_set_auto_ack_tx(bool enable);
extern void ieee802154_pib_set_enhance_ack_tx(bool enable);

/* ENABLE AND DISABLE - the two points where WiFi used to fail.
 *
 * 1. COEXISTENCE (on enable). WiFi, BLE, and 802.15.4 share ONE RF path on
 *    C6, and the coexistence arbiter allocates time. It accounts for
 *    802.15.4 only after esp_coex_wifi_i154_enable() (coex_enable +
 *    esp_coex_ieee802154_status_enable); the 802.15.4 driver never does this
 *    itself—the official ot_br and esp_zigbee_gateway examples, both "WiFi +
 *    802.15.4 on C6," call it from the application. While listening, 802.15.4
 *    requests the radio with the driver's low "txrx" priority; WiFi goes first
 *    when traffic exists and 802.15.4 listens in the gaps.
 *
 * 2. DISABLE WITH SLEEP, NEVER WITH DISABLE. Device measurements showed that
 *    with correct coexistence, WiFi carried data WITH 802.15.4 enabled and
 *    failed immediately AFTER disable. esp_ieee802154_disable() cuts the MAC
 *    (modem_clock_module_disable) and changes state without
 *    stop_current_operation(): with the receiver enabled (rx_when_idle), the
 *    MAC freezes mid-reception while its radio request is with the arbiter.
 *    esp_ieee802154_sleep() is the clean path—it stops the current operation
 *    (CMD_STOP), enters SLEEP, and releases RF (esp_phy_disable)—and OpenThread
 *    uses it continuously with WiFi enabled. The driver remains enabled and
 *    sleeping: an idle MAC costs only its clock, and re-enabling is receive().
 *
 * Different tasks call this (host RPC and the 1 s watchdog); s_radio_mtx serializes them. */

static void enable_z154(const c6_154_cfg_t *c)
{
    s_z154_intervalo_ms = c->min_intervalo_ms;
    s_z154_validade = esp_timer_get_time() + (int64_t)C6_RADIO_VALIDADE_MS * 1000;
    uint8_t canal = (c->channel >= 11 && c->channel <= 26) ? c->channel : 15;
    const bool novo = !s_z154_lig;
    if (!s_z154_habilitado)
    {
        esp_coex_wifi_i154_enable();
        const esp_err_t r = esp_ieee802154_enable();
        s_z154_err = (int16_t)r;
        if (r != ESP_OK)
        {
            ESP_LOGW(TAG, "802.15.4 enable rejected: %s", esp_err_to_name(r));
            esp_coex_ieee802154_status_disable();
            return;
        }
        s_z154_habilitado = true;
    }
    else if (novo)
    {
        esp_coex_ieee802154_status_enable();
    }
    s_z154_lig = true;
    /* Each receive() reloads the pending configuration (PIB). */
    ieee802154_pib_set_auto_ack_tx(false);
    ieee802154_pib_set_enhance_ack_tx(false);
    esp_ieee802154_set_channel(canal);
    esp_ieee802154_set_promiscuous(c->promiscuo != 0);
    esp_ieee802154_set_rx_when_idle(true);
    const esp_err_t rr = esp_ieee802154_receive();
    s_z154_err = (int16_t)rr;
    /* Renewal arrives every few seconds: log only changes. */
    if (novo)
    {
        ESP_LOGI(TAG, "802.15.4 listening: channel %u", (unsigned)canal);
    }
}
static void disable_z154(void)
{
    if (!s_z154_lig)
    {
        return;
    }
    esp_ieee802154_set_rx_when_idle(false); /* otherwise the driver would keep listening by itself */
    esp_ieee802154_sleep();
    esp_coex_ieee802154_status_disable(); /* outside the arbiter: WiFi gets the radio exclusively */
    s_z154_lig = false;
    ESP_LOGI(TAG, "802.15.4 sleeping");
}
static void z154_watchdog(int64_t now)
{
    if (s_z154_lig && now > s_z154_validade)
    {
        ESP_LOGW(TAG, "802.15.4: host did not renew");
        disable_z154();
    }
}
#else
static void enable_z154(const c6_154_cfg_t *c) { (void)c; }
static void disable_z154(void) {}
static void z154_watchdog(int64_t a) { (void)a; }
#endif

/* Single watchdog: disables CSI/802.15.4 that the host stopped renewing.
 *
 * The WORK DOES NOT RUN IN esp_timer. The esp_timer task is shared—the BLE
 * controller uses it (CONFIG_BT_LE_USE_ESP_TIMER), as does esp_hosted. Waiting
 * for a lock there (up to 50 ms) and calling the WiFi or 802.15.4 driver (which
 * wait for the radio task) delayed every other timer. The callback only wakes
 * the sniffer task, which is already sleeping and performs the check calmly. */
static void radio_watchdog(int64_t now)
{
    if (!s_radio_mtx)
    {
        return;
    }
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    csi_watchdog(now);
    z154_watchdog(now);
    xSemaphoreGive(s_radio_mtx);
}

static void radio_watchdog_cb(void *arg)
{
    (void)arg;
#if CONFIG_ESP_WIFI_CSI_ENABLED
    const bool csi = s_csi_lig;
#else
    const bool csi = false;
#endif
    /* Nothing enabled: wake nobody (an idle C6 stays idle). */
    if ((csi || s_z154_lig) && s_fj_tarefa)
    {
        xTaskNotifyGive(s_fj_tarefa);
    }
}

/* ---------------------------------------------------------------------------
 * FROM THE P4
 * --------------------------------------------------------------------------- */

static void do_host(uint32_t msg_id, const uint8_t *data, size_t n, void *ctx)
{
    (void)ctx;

    switch (msg_id)
    {
    case MSG_HOST_VIVO:
        host_confirmed();
        break;

    case MSG_HOST_ENVIAR:
    {
        if (n <= ESP_NOW_ETH_ALEN)
        {
            return;
        }
        /* STATIC: envio_t is 1.5 KB and this is esp_hosted's pserial_task
         * stack (4 KB, not ours). Only it calls do_host; xQueueSend copies
         * before returning. */
        static envio_t e;
        memcpy(e.mac, data, ESP_NOW_ETH_ALEN);
        size_t tam = n - ESP_NOW_ETH_ALEN;
        if (tam > ESP_NOW_MAX_DATA_LEN)
        {
            tam = ESP_NOW_MAX_DATA_LEN;
        }
        memcpy(e.texto, data + ESP_NOW_ETH_ALEN, tam);
        e.n = (uint16_t)tam;
        e.cru = false;
        /* ESP-NOW inactive or queue full: fail IMMEDIATELY, or the Tab5 screen
         * would wait for confirmation forever. */
        if (!s_espnow_no_ar || !s_envio || xQueueSend(s_envio, &e, 0) != pdTRUE)
        {
            uint8_t st[ESP_NOW_ETH_ALEN + 1];
            memcpy(st, e.mac, ESP_NOW_ETH_ALEN);
            st[6] = 0;
            queue_message(MSG_C6_ESTADO, st, sizeof(st));
        }
        break;
    }

    case MSG_HOST_ESPNOW:
        enable_espnow();
        break;

    case C6_MSG_HOST_FAREJO:
        request_sniffer(data, n);
        break;

    case C6_MSG_HOST_CRU:
    {
        if (n <= ESP_NOW_ETH_ALEN || !s_espnow_no_ar || !s_envio)
        {
            return;
        }
        static envio_t e; /* static: see MSG_HOST_ENVIAR */
        memcpy(e.mac, data, ESP_NOW_ETH_ALEN);
        size_t tam = n - ESP_NOW_ETH_ALEN;
        if (tam > ESP_NOW_MAX_DATA_LEN_V2)
        {
            tam = ESP_NOW_MAX_DATA_LEN_V2;
        }
        memcpy(e.texto, data + ESP_NOW_ETH_ALEN, tam);
        e.n = (uint16_t)tam;
        e.cru = true;
        xQueueSend(s_envio, &e, 0);
        break;
    }

    case C6_MSG_HOST_AJUSTE:
        if (n < 1)
        {
            return;
        }
        switch (data[0])
        {
        case C6_AJ_INFO:
            send_info();
            break;
        case C6_AJ_CRU:
            s_repassar_cru = (n >= 2 && data[1]);
            break;
        default:
            break; /* key added by a newer P4: ignore */
        }
        break;

    case C6_MSG_HOST_RADIO:
    {
        if (n < 1)
        {
            return;
        }
        const uint8_t sub = data[0];
        const uint8_t *p = data + 1;
        const size_t m = n - 1;
        if (!s_radio_mtx)
        {
            return;
        }
        xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
        switch (sub)
        {
        case C6_RX_CSI:
        {
            c6_csi_cfg_t c = {0};
            memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
            if (c.liga)
            {
                enable_csi(&c);
            }
            else
            {
                disable_csi();
            }
            break;
        }
        case C6_RX_FTM:
        {
            c6_ftm_cfg_t c = {0};
            memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
            start_ftm(&c);
            break;
        }
        case C6_RX_154:
        {
            c6_154_cfg_t c = {0};
            memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
            if (c.liga)
            {
                enable_z154(&c);
            }
            else
            {
                disable_z154();
            }
            break;
        }
        default:
            break; /* subcommand added by a newer P4: ignore */
        }
        xSemaphoreGive(s_radio_mtx);
        break;
    }

    case MSG_HOST_CANAL:
        /* EVERYONE MUST BE ON THE SAME CHANNEL; this is the number-one reason
         * for "it does not work." If C6 is associated with an AP, the AP owns
         * the channel—changing it here would drop the connection. Therefore we
         * change it only when the P4 explicitly asks. */
        if (n >= 1)
        {
            esp_err_t r = esp_wifi_set_channel(data[0], WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "channel -> %u: %s", (unsigned)data[0], esp_err_to_name(r));
            if (r == ESP_OK)
            {
                s_canal_volta = data[0];
            } /* sniffer returns to this */
        }
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Tab5 C6 firmware (esp_hosted + ESP-NOW + sniffer), contract v%d", C6_PROTO_VERSAO);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    /* No ESP_ERROR_CHECK, for the same reason as the registration below:
     * nothing here may take down the coprocessor. NVS failure is bad; aborting is MUCH worse. */
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(ret));
    }
    esp_event_loop_create_default();

    /* New image guard BEFORE anything that could fail. */
    start_image_guard();

    s_saida = xRingbufferCreate(SAIDA_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (s_saida)
    {
        xTaskCreate(output_task, "c6output", 4096, NULL, 5, NULL);
    }
    /* Lock before the sniffer: it runs the radio-extra watchdog. */
    s_radio_mtx = xSemaphoreCreateMutex();
    /* Sleeps in ulTaskNotifyTake until the host requests sniffing (or the
     * watchdog wakes it): stopped, it costs only stack. 4096 like the output task:
     * both call eh_cp_feat_peer_data_send, which encodes the RPC event on the caller's stack. */
    xTaskCreate(sniffer_task, "c6sniffer", 4096, NULL, 4, &s_fj_tarefa);

    /* Radio-extra watchdog (CSI/802.15.4): once per second, wake the sniffer if
     * either is enabled. */
    esp_timer_handle_t guarda = NULL;
    const esp_timer_create_args_t ga = {.callback = radio_watchdog_cb, .name = "radio_watchdog"};
    if (esp_timer_create(&ga, &guarda) == ESP_OK)
    {
        esp_timer_start_periodic(guarda, 1000000);
    }

    /* NO ESP_ERROR_CHECK HERE. That was the bug.
     *
     * Coprocessor features (including peer_data) start in a DEDICATED TASK—the
     * auto_feat_init_task in eh_cp_core—not before app_main. Until it runs,
     * register_callback returns ESP_ERR_INVALID_STATE ("Not initialized"),
     * and ESP_ERROR_CHECK on that result ABORTS C6.
     *
     * Aborting here is the worst possible outcome: C6 enters a reboot loop,
     * the SDIO link negotiates and dies on every iteration, EVERY host RPC
     * expires, and because the device never stays up long enough, new firmware
     * cannot even be flashed over OTA to fix it. That was exactly the symptom.
     *
     * Rule: NOTHING in a coprocessor's app_main may abort. It is its only
     * recovery path. */
    for (int tent = 0;; ++tent)
    {
        const esp_err_t r = eh_cp_feat_peer_data_register_callback(MSG_HOST_VIVO, do_host, NULL);
        if (r == ESP_OK)
        {
            break;
        }
        if (tent == 0)
        {
            ESP_LOGW(TAG, "peer_data is not up yet (%s) - waiting",
                     esp_err_to_name(r));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    eh_cp_feat_peer_data_register_callback(MSG_HOST_ENVIAR, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(MSG_HOST_CANAL, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(MSG_HOST_ESPNOW, do_host, NULL);
    /* peer_data table: 8 of 8 slots, including VIVO (see tab5_c6.h). */
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_FAREJO, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_CRU, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_AJUSTE, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_RADIO, do_host, NULL); /* 8th: table full */

    ESP_LOGI(TAG, "ready - waiting for host to enable ESP-NOW");
}
