#include "sniffer_core.h"
#include "lcd_menu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi Sniffer Core...");
    init_sniffer_system();

    ESP_LOGI(TAG, "Initializing LCD2004 & Buttons...");
    init_lcd_and_buttons();

    // Запуск задачи управления экраном и кнопками
    xTaskCreate(&lcd_menu_task, "lcd_menu_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System is fully operational!");
}