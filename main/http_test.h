#pragma once
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Performs a blocking HTTP GET against `url` and logs the outcome
 * (status code + content length).
 *
 * If `bind_netif` is non-NULL, the underlying socket is bound to that
 * specific interface (via esp_http_client's if_name option) so the
 * request is forced out over it — important here since WiFi and
 * cellular can both hold an IP at the same time, and we want to test
 * each link individually rather than whichever one currently owns the
 * default route.
 *
 * @return ESP_OK if the request completed with a 2xx/3xx status.
 */
esp_err_t http_test_get(const char *url, esp_netif_t *bind_netif);

#ifdef __cplusplus
}
#endif
