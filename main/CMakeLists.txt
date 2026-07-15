idf_component_register(SRCS  "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES network_manager modem_power web_client sms esp_http_client esp-tls mbedtls
                       PRIV_REQUIRES nvs_flash
                       )