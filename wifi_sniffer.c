/*
 * wifi_sniffer.c
 *
 *  Created on: 20 окт. 2026 г.
 *      Author: Lev
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sniffer_core.h"
#include "lcd_menu.h"
#include "sd_db.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "Initializing SDIO Card & DB...");
    ESP_ERROR_CHECK(sd_db_init());
	load_oui_database_from_sd();
    ESP_LOGI(TAG, "Initializing Wi-Fi Sniffer Core...");
    init_sniffer_system();

    ESP_LOGI(TAG, "Initializing LCD2004 & Buttons...");
    init_lcd_and_buttons();

    xTaskCreatePinnedToCore(
        &lcd_menu_task,
        "lcd_menu_task",
        4096,
        NULL,
        5,
        NULL,
        0   
    );


    xTaskCreatePinnedToCore(
        &flush_to_queue_task,
        "flush_task",
        3072,
        NULL,
        4,
        NULL,
        1   
    );

    xTaskCreatePinnedToCore(
        &sd_logger_task,
        "sd_logger_task",
        4096,
        NULL,
        4,
        NULL,
        1   
    );

    ESP_LOGI(TAG, "All tasks pinned and running!");
}
