idf_component_register(
    SRCS "sms.c" "pdu_parser.c"
    INCLUDE_DIRS "include"
    REQUIRES network_manager esp_modem esp_timer
)
