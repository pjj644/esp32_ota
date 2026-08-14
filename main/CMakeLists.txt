idf_component_register(SRCS "ota.c" "http_client.c" "wifi.c" "led.c" "stm32_ota.c" "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client esp_https_ota app_update esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio esp_driver_uart spi_flash)
