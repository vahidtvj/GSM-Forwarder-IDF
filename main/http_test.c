#include "http_test.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "http_test";

typedef struct {
    char *buffer;
    size_t len;
    size_t max_len;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        http_response_t *resp = evt->user_data;
        size_t to_copy = evt->data_len;
        if (resp->len + to_copy >= resp->max_len) {
            to_copy = resp->max_len - resp->len - 1;
        }
        if (to_copy > 0) {
            memcpy(resp->buffer + resp->len, evt->data, to_copy);
            resp->len += to_copy;
            resp->buffer[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t http_test_get(const char *url, esp_netif_t *bind_netif)
{
    struct ifreq ifr = { 0 };
    char response[128] = { 0 };
    http_response_t resp = {
        .buffer = response,
        .len = 0,
        .max_len = sizeof(response),
    };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .disable_auto_redirect = false,
    };

    if (bind_netif) {
        if (esp_netif_get_netif_impl_name(bind_netif, ifr.ifr_name) == ESP_OK) {
            config.if_name = &ifr;
            ESP_LOGI(TAG, "Binding request to interface '%s'", ifr.ifr_name);
        } else {
            ESP_LOGW(TAG, "Could not resolve interface name for binding — "
                           "request will go via whatever holds the default route");
        }
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GET %s ...", url);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int64_t len = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "GET %s -> status=%d, content_length=%lld", url, status, len);

        if (resp.len > 0) {
            ESP_LOGI(TAG, "Response: %s", response);
        } else {
            ESP_LOGW(TAG, "No response body captured by event handler");
        }

        if (status < 200 || status >= 400) {
            ESP_LOGW(TAG, "Non-success status code");
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}
