#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sms.h"
#include "pdu_parser.h"
#include "network_manager.h"

#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "sms";

#define SMS_SWEEP_SENTINEL      0xFFFFu
#define SMS_QUEUE_LEN           8
#define SMS_MAX_PENDING_PARTS   8
#define SMS_MODEM_LOCK_TIMEOUT_MS 10000
#define SMS_AT_TIMEOUT_MS       5000

typedef struct {
    bool used;
    char ref[SMS_PDU_MAX_REF_LEN];
    uint8_t total_parts;
    char sender[SMS_PDU_MAX_SENDER_LEN];
    char date[SMS_PDU_MAX_DATE_LEN];
    char *parts[255]; /* indexed by part-1; NULL until that part arrives */
    int64_t first_seen_us;
} pending_group_t;

#define SMS_DEFAULT_MULTIPART_TIMEOUT_MS (60UL * 60UL * 1000UL) /* 1 hour */
static uint32_t s_multipart_timeout_ms = SMS_DEFAULT_MULTIPART_TIMEOUT_MS;

static sms_received_cb_t s_cb = NULL;
static void *s_ctx = NULL;

/* Replaces invalid/truncated UTF-8 sequences (which a malformed or
 * corrupted PDU can produce) with U+FFFD, so the result is always safe to
 * hand to a JSON encoder or any other UTF-8-expecting consumer. Returns a
 * new heap buffer; caller frees. Returns NULL if `in` is NULL or on
 * allocation failure. */
static char *sanitize_utf8(const char *in)
{
    if (in == NULL) {
        return NULL;
    }
    size_t len = strlen(in);
    char *out = malloc(len * 3 + 1); /* worst case: every byte -> 3-byte replacement */
    if (out == NULL) {
        return NULL;
    }
    size_t oi = 0, i = 0;

    while (i < len) {
        unsigned char c = (unsigned char)in[i];
        int seq_len;
        uint32_t cp;

        if (c < 0x80)                 { seq_len = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0)  { seq_len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0)  { seq_len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0)  { seq_len = 4; cp = c & 0x07; }
        else                          { seq_len = 0; cp = 0; }

        bool valid = seq_len > 0 && (i + (size_t)seq_len) <= len;
        if (valid) {
            for (int k = 1; k < seq_len; k++) {
                unsigned char cc = (unsigned char)in[i + k];
                if ((cc & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        if (valid && ((seq_len == 2 && cp < 0x80) ||
                      (seq_len == 3 && cp < 0x800) ||
                      (seq_len == 4 && cp < 0x10000) ||
                      cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
            valid = false; /* overlong encoding, out of range, or surrogate */
        }

        if (valid) {
            memcpy(out + oi, in + i, (size_t)seq_len);
            oi += (size_t)seq_len;
            i += (size_t)seq_len;
        } else {
            out[oi++] = (char)0xEF;
            out[oi++] = (char)0xBF;
            out[oi++] = (char)0xBD; /* U+FFFD replacement character */
            i += 1;
        }
    }
    out[oi] = '\0';
    char *shrunk = realloc(out, oi + 1);
    return shrunk ? shrunk : out;
}

static QueueHandle_t s_index_queue = NULL;
static SemaphoreHandle_t s_pending_mutex = NULL;
static pending_group_t s_pending[SMS_MAX_PENDING_PARTS];

/* -------------------------------------------------------------------------
 * Multipart reassembly
 * ---------------------------------------------------------------------- */

static pending_group_t *find_or_create_group(const char *ref)
{
    pending_group_t *free_slot = NULL;
    for (int i = 0; i < SMS_MAX_PENDING_PARTS; i++) {
        if (s_pending[i].used && strcmp(s_pending[i].ref, ref) == 0) {
            return &s_pending[i];
        }
        if (!s_pending[i].used && free_slot == NULL) {
            free_slot = &s_pending[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->first_seen_us = esp_timer_get_time();
        strncpy(free_slot->ref, ref, sizeof(free_slot->ref) - 1);
    }
    return free_slot;
}

static void free_group(pending_group_t *g)
{
    for (int i = 0; i < 255; i++) {
        if (g->parts[i]) {
            free(g->parts[i]);
            g->parts[i] = NULL;
        }
    }
    g->used = false;
}

/* Returns a heap-allocated combined message via *out_message if the sms is
 * complete (single-part, or all parts of a multipart group now present).
 * Returns false if still waiting on more parts. */
static bool reassemble(sms_pdu_t *pdu, char sender_out[SMS_PDU_MAX_SENDER_LEN],
                        char date_out[SMS_PDU_MAX_DATE_LEN], char **out_message,
                        uint8_t *parts_received_out, uint8_t *total_parts_out)
{
    if (pdu->total_parts <= 1) {
        strncpy(sender_out, pdu->sender, SMS_PDU_MAX_SENDER_LEN - 1);
        strncpy(date_out, pdu->date, SMS_PDU_MAX_DATE_LEN - 1);
        *out_message = pdu->message; /* ownership transfers to caller */
        pdu->message = NULL;
        *parts_received_out = 1;
        *total_parts_out = 1;
        return true;
    }

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);

    pending_group_t *g = find_or_create_group(pdu->ref);
    if (g == NULL) {
        ESP_LOGW(TAG, "No free slot for multipart group (ref=%s), dropping part", pdu->ref);
        xSemaphoreGive(s_pending_mutex);
        return false;
    }

    g->total_parts = pdu->total_parts;
    strncpy(g->sender, pdu->sender, sizeof(g->sender) - 1);
    strncpy(g->date, pdu->date, sizeof(g->date) - 1);

    if (pdu->part >= 1 && pdu->part <= g->total_parts) {
        if (g->parts[pdu->part - 1] == NULL) {
            g->parts[pdu->part - 1] = pdu->message;
            pdu->message = NULL;
        }
    }

    bool complete = true;
    size_t total_len = 0;
    for (int i = 0; i < g->total_parts; i++) {
        if (g->parts[i] == NULL) {
            complete = false;
            break;
        }
        total_len += strlen(g->parts[i]);
    }

    bool result = false;
    if (complete) {
        char *combined = malloc(total_len + 1);
        if (combined) {
            combined[0] = '\0';
            for (int i = 0; i < g->total_parts; i++) {
                strcat(combined, g->parts[i]);
            }
            strncpy(sender_out, g->sender, SMS_PDU_MAX_SENDER_LEN - 1);
            strncpy(date_out, g->date, SMS_PDU_MAX_DATE_LEN - 1);
            *out_message = combined;
            *parts_received_out = g->total_parts;
            *total_parts_out = g->total_parts;
            result = true;
        }
        free_group(g);
    }

    xSemaphoreGive(s_pending_mutex);
    return result;
}

/* Builds combined text for a group that timed out with missing parts,
 * marking each gap inline in delivery order. */
static char *compose_with_gaps(pending_group_t *g)
{
    size_t total_len = 0;
    char marker[24];
    for (int i = 0; i < g->total_parts; i++) {
        if (g->parts[i]) {
            total_len += strlen(g->parts[i]);
        } else {
            total_len += (size_t)snprintf(marker, sizeof(marker), "[missing part %d]", i + 1);
        }
    }

    char *combined = malloc(total_len + 1);
    if (combined == NULL) {
        return NULL;
    }
    combined[0] = '\0';
    for (int i = 0; i < g->total_parts; i++) {
        if (g->parts[i]) {
            strcat(combined, g->parts[i]);
        } else {
            snprintf(marker, sizeof(marker), "[missing part %d]", i + 1);
            strcat(combined, marker);
        }
    }
    return combined;
}

/* Scans pending multipart groups and force-delivers any that have exceeded
 * the configured timeout, so a permanently lost part doesn't hold a
 * reassembly slot (and the sender's message) forever. */
static void scan_expired_groups(void)
{
    int64_t now = esp_timer_get_time();
    int64_t timeout_us = (int64_t)s_multipart_timeout_ms * 1000;

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < SMS_MAX_PENDING_PARTS; i++) {
        pending_group_t *g = &s_pending[i];
        if (!g->used || (now - g->first_seen_us) < timeout_us) {
            continue;
        }

        uint8_t received = 0;
        for (int p = 0; p < g->total_parts; p++) {
            if (g->parts[p]) {
                received++;
            }
        }

        ESP_LOGW(TAG, "Multipart SMS ref=%s timed out (%u/%u parts) - delivering with gaps marked",
                 g->ref, received, g->total_parts);

        char *combined = compose_with_gaps(g);
        char *clean = sanitize_utf8(combined);
        free(combined);
        combined = clean;

        if (combined && s_cb) {
            sms_message_t out = {
                .message = combined,
                .complete = false,
                .parts_received = received,
                .total_parts = g->total_parts,
            };
            strncpy(out.sender, g->sender, sizeof(out.sender) - 1);
            strncpy(out.date, g->date, sizeof(out.date) - 1);
            s_cb(&out, s_ctx);
        }
        free(combined);
        free_group(g);
    }
    xSemaphoreGive(s_pending_mutex);
}

/* -------------------------------------------------------------------------
 * Modem I/O
 * ---------------------------------------------------------------------- */

static void deliver(sms_pdu_t *pdu)
{
    char sender[SMS_PDU_MAX_SENDER_LEN];
    char date[SMS_PDU_MAX_DATE_LEN];
    char *message = NULL;
    uint8_t parts_received = 0, total_parts = 0;

    if (reassemble(pdu, sender, date, &message, &parts_received, &total_parts)) {
        char *clean = sanitize_utf8(message);
        free(message);
        message = clean;

        if (s_cb && message) {
            sms_message_t out = {
                .message = message,
                .complete = true,
                .parts_received = parts_received,
                .total_parts = total_parts,
            };
            strncpy(out.sender, sender, sizeof(out.sender) - 1);
            strncpy(out.date, date, sizeof(out.date) - 1);
            s_cb(&out, s_ctx);
        }
        free(message);
    }
    free(pdu->message);
    pdu->message = NULL;
}

static void process_single_index(uint16_t index, bool auto_delete)
{
    if (network_manager_modem_lock(pdMS_TO_TICKS(SMS_MODEM_LOCK_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Could not acquire modem to read SMS %u (busy with cellular session)", index);
        return;
    }

    esp_modem_dce_t *dce = network_manager_get_modem_dce();
    if (dce == NULL) {
        network_manager_modem_unlock();
        return;
    }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%u\r", index);
    char resp[700];
    esp_err_t err = esp_modem_at(dce, cmd, resp, SMS_AT_TIMEOUT_MS);

    if (auto_delete && err == ESP_OK) {
        char del_cmd[32];
        char del_resp[32];
        snprintf(del_cmd, sizeof(del_cmd), "AT+CMGD=%u\r", index);
        esp_modem_at(dce, del_cmd, del_resp, SMS_AT_TIMEOUT_MS);
    }

    network_manager_modem_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AT+CMGR=%u failed: %s", index, esp_err_to_name(err));
        return;
    }

    const char *body = strstr(resp, "+CMGR:");
    if (body == NULL) {
        ESP_LOGW(TAG, "Unexpected CMGR response for index %u", index);
        return;
    }
    body += strlen("+CMGR:");
    while (*body == ' ') {
        body++;
    }

    sms_pdu_t pdu;
    if (!sms_pdu_parse(body, &pdu)) {
        ESP_LOGW(TAG, "Failed to parse PDU for index %u (raw: %s)", index, body);
        return;
    }
    deliver(&pdu);
}

/* Sweep for any SMS that arrived while a cellular session had the UART
 * (no URCs could fire during that window). Deletes messages after reading,
 * matching the old readAll(..., remove=true) behavior. */
static void do_sweep(void)
{
    if (network_manager_modem_lock(pdMS_TO_TICKS(SMS_MODEM_LOCK_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Could not acquire modem for sweep");
        return;
    }
    esp_modem_dce_t *dce = network_manager_get_modem_dce();
    if (dce == NULL) {
        network_manager_modem_unlock();
        return;
    }

    char *resp = malloc(2048);
    if (resp == NULL) {
        network_manager_modem_unlock();
        return;
    }
    esp_err_t err = esp_modem_at(dce, "AT+CMGL=\"ALL\"\r", resp, 10000);
    network_manager_modem_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sweep AT+CMGL failed: %s", esp_err_to_name(err));
        free(resp);
        return;
    }

    const char *cursor = resp;
    while ((cursor = strstr(cursor, "+CMGL:")) != NULL) {
        cursor += strlen("+CMGL:");
        while (*cursor == ' ') {
            cursor++;
        }
        uint16_t index = (uint16_t)strtol(cursor, NULL, 10);

        sms_pdu_t pdu;
        if (sms_pdu_parse(cursor, &pdu)) {
            deliver(&pdu);
        } else {
            ESP_LOGW(TAG, "Failed to parse swept PDU at index %u", index);
        }

        if (network_manager_modem_lock(pdMS_TO_TICKS(SMS_MODEM_LOCK_TIMEOUT_MS)) == ESP_OK) {
            char del_cmd[32], del_resp[32];
            snprintf(del_cmd, sizeof(del_cmd), "AT+CMGD=%u\r", index);
            esp_modem_at(dce, del_cmd, del_resp, SMS_AT_TIMEOUT_MS);
            network_manager_modem_unlock();
        }

        cursor += 1; /* advance past this occurrence to find the next */
    }
    free(resp);
}

/* -------------------------------------------------------------------------
 * URC handling + task
 * ---------------------------------------------------------------------- */

esp_err_t sms_manager_urc_handler(uint8_t *data, size_t len)
{
    if (s_index_queue == NULL || data == NULL) {
        return ESP_OK;
    }

    char line[64];
    size_t copy_len = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, data, copy_len);
    line[copy_len] = '\0';

    const char *marker = strstr(line, "+CMTI:");
    if (marker == NULL) {
        return ESP_OK;
    }
    const char *comma = strchr(marker, ',');
    if (comma == NULL) {
        return ESP_OK;
    }
    uint16_t index = (uint16_t)strtol(comma + 1, NULL, 10);

    /* Non-blocking: this runs in esp_modem's internal context. */
    xQueueSend(s_index_queue, &index, 0);
    return ESP_OK;
}

static void net_event_cb(const network_event_data_t *data, void *ctx)
{
    (void)ctx;
    if (data->event == NET_EVENT_DISCONNECTED && data->iface == NET_IF_CELLULAR) {
        uint16_t sentinel = SMS_SWEEP_SENTINEL;
        xQueueSend(s_index_queue, &sentinel, 0);
    }
}

static void sms_task(void *arg)
{
    (void)arg;
    uint16_t index;
    while (1) {
        if (xQueueReceive(s_index_queue, &index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (index == SMS_SWEEP_SENTINEL) {
            do_sweep();
        } else {
            /* Always delete after a successful read: leaving messages on
             * the SIM fills its (often tiny) storage, which can cause the
             * network/carrier to silently drop subsequent incoming SMS -
             * including later parts of a multipart message. */
            process_single_index(index, true);
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t sms_manager_init(sms_received_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;

    s_index_queue = xQueueCreate(SMS_QUEUE_LEN, sizeof(uint16_t));
    s_pending_mutex = xSemaphoreCreateMutex();
    if (s_index_queue == NULL || s_pending_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = network_manager_register_event_cb(net_event_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not register network event callback: %s", esp_err_to_name(err));
    }

    if (xTaskCreate(sms_task, "sms_task", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sms_manager_check_now(void)
{
    if (s_index_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t sentinel = SMS_SWEEP_SENTINEL;
    return xQueueSend(s_index_queue, &sentinel, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t sms_manager_delete(uint16_t index)
{
    if (network_manager_modem_lock(pdMS_TO_TICKS(SMS_MODEM_LOCK_TIMEOUT_MS)) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    esp_modem_dce_t *dce = network_manager_get_modem_dce();
    esp_err_t err = ESP_FAIL;
    if (dce) {
        char cmd[32], resp[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%u\r", index);
        err = esp_modem_at(dce, cmd, resp, SMS_AT_TIMEOUT_MS);
    }
    network_manager_modem_unlock();
    return err;
}
