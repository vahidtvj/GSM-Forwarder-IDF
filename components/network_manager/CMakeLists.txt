idf_component_register(
    SRCS
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash driver esp_modem
)