idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "." "../../shared"
                       REQUIRES esp_hosted
                       PRIV_REQUIRES nvs_flash esp_wifi esp_coex esp_ringbuf app_update esp_timer espressif__esp-now ieee802154 espcoredump)
