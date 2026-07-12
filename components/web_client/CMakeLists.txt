idf_component_register(
    SRCS "web_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls mbedtls lwip esp_netif
    PRIV_REQUIRES esp_http_client
)