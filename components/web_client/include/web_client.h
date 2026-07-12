#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Performs a single HTTPS/HTTP request.
 *
 * - GET:  pass post_data = NULL
 * - POST: pass post_data as a JSON (or other) body string; Content-Type is
 *         set to application/json automatically
 *
 * TLS (for https:// URLs) is handled via the IDF certificate bundle - no
 * per-host cert setup needed. Requires network_manager_connect() (or
 * equivalent) to have been called first; this function does not manage
 * connectivity itself.
 *
 * response_buf / response_buf_size: caller-provided buffer for the response
 * body (always NUL-terminated on success, truncated if it doesn't fit).
 * Pass NULL / 0 if you don't care about the body.
 *
 * status_code_out: optional, receives the HTTP status code on ESP_OK.
 *
 * Returns ESP_OK if the request completed (check status_code_out for the
 * actual HTTP result), or an esp_err_t describing a transport-level failure
 * (DNS, TLS handshake, timeout, etc.)
 */
esp_err_t http_request(const char *url, const char *post_data,
                        char *response_buf, size_t response_buf_size,
                        int *status_code_out);

/*
 * Same as http_request(), but allocates and grows the response buffer on the
 * heap as data arrives instead of using a caller-supplied fixed buffer.
 * Use this for endpoints with unpredictable/large response sizes (e.g. GitHub
 * release JSON with several assets).
 *
 * - *response_out receives a heap-allocated, NUL-terminated buffer.
 *   Caller must free() it when done (even on error, if non-NULL).
 * - max_response_size: hard cap in bytes, 0 = no explicit cap (still bounded
 *   by available heap - a safety margin is always reserved so growth never
 *   starves the rest of the system).
 */
esp_err_t http_request_dynamic(const char *url, const char *post_data,
                                char **response_out, size_t *response_len_out,
                                int *status_code_out, size_t max_response_size);

                                
/* Starts SNTP (if not already synced) and blocks up to timeout_ms waiting
 * for sync. Safe to call repeatedly - a no-op once synced. Returns true on
 * success. */
bool web_client_time_sync_wait(uint32_t timeout_ms);

/* Resets internal "already synced" state - mainly for tests that want to
 * force a fresh sync attempt. */
void web_client_time_sync_reset(void);

#ifdef __cplusplus
}
#endif
