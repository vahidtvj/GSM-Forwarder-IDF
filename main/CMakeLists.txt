idf_component_register(SRCS "http_client.c" "main.c"
                             "modem_power.c"
                       INCLUDE_DIRS "."
                       REQUIRES network_manager
                       PRIV_REQUIRES nvs_flash
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     esp_driver_gpio
                                     esp_modem
                                     esp_http_client
                                     lwip)