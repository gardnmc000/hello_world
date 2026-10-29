idf_component_register(SRCS "hello_world_main.c" "init.c" "mgaudio.c" "mgnetwork.c" "event_handlers.c" "storage.c"
                    REQUIRES driver esp_modem esp_netif fatfs sdmmc esp_adc spi_flash esp_psram
                    INCLUDE_DIRS "" ) 
