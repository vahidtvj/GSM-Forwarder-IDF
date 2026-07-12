#include <string.h>
#include <stdlib.h>

#include "web_client.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <time.h>

static const char *TAG = "web_client";

#define HTTP_DYN_INITIAL_CAPACITY 512
#define HTTP_DYN_HEAP_SAFETY_MARGIN (32 * 1024) /* always keep this much free */

typedef struct {
    char *buf;
    size_t buf_size;
    size_t len;
} recv_ctx_t;

static esp_err_t event_handler(esp_http_client_event_t *evt)
{
    recv_ctx_t *ctx = (recv_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && ctx->buf) {
        size_t space = (ctx->buf_size > ctx->len + 1) ? (ctx->buf_size - ctx->len - 1) : 0;
        size_t to_copy = evt->data_len < space ? evt->data_len : space;
        if (to_copy > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, to_copy);
            ctx->len += to_copy;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t http_request(const char *url, const char *post_data,
                        char *response_buf, size_t response_buf_size,
                        int *status_code_out)
{
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    recv_ctx_t ctx = { .buf = response_buf, .buf_size = response_buf_size, .len = 0 };
    if (response_buf && response_buf_size > 0) {
        response_buf[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init http client for %s", url);
        return ESP_FAIL;
    }

    if (post_data != NULL) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status_code_out) {
            *status_code_out = status;
        }
        ESP_LOGI(TAG, "%s %s -> %d", post_data ? "POST" : "GET", url, status);
    } else {
        ESP_LOGW(TAG, "%s %s failed: %s", post_data ? "POST" : "GET", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

typedef struct {
    char *buf;
    size_t capacity;
    size_t len;
    size_t max_size;   /* 0 = unlimited (still heap-guarded) */
    bool truncated;
} dyn_recv_ctx_t;

static esp_err_t dyn_event_handler(esp_http_client_event_t *evt)
{
    dyn_recv_ctx_t *ctx = (dyn_recv_ctx_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || ctx == NULL || ctx->truncated || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t needed = ctx->len + (size_t)evt->data_len + 1; /* +1 for NUL */

    if (ctx->max_size && needed > ctx->max_size) {
        ESP_LOGW(TAG, "Response exceeds max_size (%u), truncating rest", (unsigned)ctx->max_size);
        ctx->truncated = true;
        return ESP_OK;
    }

    if (needed > ctx->capacity) {
        size_t new_capacity = ctx->capacity ? ctx->capacity * 2 : HTTP_DYN_INITIAL_CAPACITY;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }

        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (free_heap < new_capacity + HTTP_DYN_HEAP_SAFETY_MARGIN) {
            ESP_LOGE(TAG, "Refusing to grow response buffer to %u bytes (%u free, %u margin reserved)",
                     (unsigned)new_capacity, (unsigned)free_heap, HTTP_DYN_HEAP_SAFETY_MARGIN);
            ctx->truncated = true;
            return ESP_OK;
        }

        char *new_buf = realloc(ctx->buf, new_capacity);
        if (new_buf == NULL) {
            ESP_LOGE(TAG, "realloc failed for %u bytes", (unsigned)new_capacity);
            ctx->truncated = true;
            return ESP_OK;
        }
        ctx->buf = new_buf;
        ctx->capacity = new_capacity;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

esp_err_t http_request_dynamic(const char *url, const char *post_data,
                                char **response_out, size_t *response_len_out,
                                int *status_code_out, size_t max_response_size)
{
    if (url == NULL || response_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *response_out = NULL;
    if (response_len_out) {
        *response_len_out = 0;
    }

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < HTTP_DYN_INITIAL_CAPACITY + HTTP_DYN_HEAP_SAFETY_MARGIN) {
        ESP_LOGE(TAG, "Not enough free heap to start request (%u free)", (unsigned)free_heap);
        return ESP_ERR_NO_MEM;
    }

    dyn_recv_ctx_t ctx = {
        .buf = NULL, .capacity = 0, .len = 0,
        .max_size = max_response_size, .truncated = false,
    };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = dyn_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(ctx.buf);
        return ESP_FAIL;
    }

    if (post_data != NULL) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status_code_out) {
            *status_code_out = status;
        }
        ESP_LOGI(TAG, "%s %s -> %d (%u bytes%s)", post_data ? "POST" : "GET", url,
                 status, (unsigned)ctx.len, ctx.truncated ? ", truncated" : "");
    } else {
        ESP_LOGW(TAG, "%s %s failed: %s", post_data ? "POST" : "GET", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    *response_out = ctx.buf; /* caller must free(), even if NULL */
    if (response_len_out) {
        *response_len_out = ctx.len;
    }
    return err;
}

static bool s_time_synced = false;

bool web_client_time_sync_wait(uint32_t timeout_ms)
{
    if (s_time_synced) {
        return true;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK) {
        time_t now = time(NULL);
        ESP_LOGI(TAG, "Time synced: %s", ctime(&now));
        s_time_synced = true;
    } else {
        ESP_LOGW(TAG, "Time sync failed/timed out");
    }
    return s_time_synced;
}

void web_client_time_sync_reset(void)
{
    s_time_synced = false;
}