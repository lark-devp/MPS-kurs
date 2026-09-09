idf_component_register(SRCS "wifi_sniffer.c" "sniffer_core.c" "lcd_menu.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_event esp_netif driver)