#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char sender[24];
    char date[20];
    char *message;   /* heap-allocated, full reassembled text. Free() it after use. */
} sms_message_t;

typedef void (*sms_received_cb_t)(const sms_message_t *sms, void *ctx);

/*
 * Call after network_manager_init(). Registers a URC handler with
 * network_manager (set network_manager_config_t.cellular.urc_handler =
 * sms_manager_urc_handler before calling network_manager_init - see below)
 * and a network event subscriber for post-PPP catch-up sweeps.
 */
esp_err_t sms_manager_init(sms_received_cb_t cb, void *ctx);

/* Manually trigger a sweep for any unread SMS (also runs automatically after
 * a cellular data session ends). */
esp_err_t sms_manager_check_now(void);

esp_err_t sms_manager_delete(uint16_t index);

/*
 * Pass this as network_manager_config_t.cellular.urc_handler before calling
 * network_manager_init(), e.g.:
 *   cfg.cellular.urc_handler = sms_manager_urc_handler;
 *   network_manager_init(&cfg);
 *   sms_manager_init(my_callback, NULL);
 */
esp_err_t sms_manager_urc_handler(uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
