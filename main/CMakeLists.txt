idf_component_register(SRCS "ota_update.c" "hello_world_main.c" "init.c" "mgaudio.c" "mgnetwork.c" "event_handlers.c" "storage.c" "ota_update.c"
                    REQUIRES driver esp_modem esp_netif fatfs sdmmc esp_adc spi_flash esp_psram app_update esp_app_format
                    INCLUDE_DIRS "" ) 
