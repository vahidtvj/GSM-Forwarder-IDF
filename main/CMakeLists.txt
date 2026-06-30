idf_component_register(SRCS "main.c"
                             "modem_power.c"
                             "wifi_conn.c"
                             "cellular_conn.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     esp_driver_gpio
                                     esp_modem)