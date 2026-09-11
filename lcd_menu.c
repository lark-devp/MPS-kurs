#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_rom_sys.h"

#include "lcd_menu.h"
#include "sniffer_core.h"

#define PIN_BTN_NEXT 18   // ВПРАВО
#define PIN_BTN_PREV 19   // ВЛЕВО

#define LCD_I2C_ADDR 0x27

#define BIT_RS  0x01
#define BIT_RW  0x02
#define BIT_EN  0x04
#define BIT_BL  0x08

static uint8_t backlight_state = BIT_BL;
static int current_screen = 0;

static void lcd_write_nibble(uint8_t nibble, uint8_t rs_bit) {
    uint8_t data = (nibble & 0xF0) | rs_bit | backlight_state;
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    
    i2c_master_write_byte(cmd, data | BIT_EN, true);
    i2c_master_write_byte(cmd, data & ~BIT_EN, true);
    
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    esp_rom_delay_us(300);
}

static void lcd_send_byte(uint8_t val, uint8_t rs_bit) {
    lcd_write_nibble(val & 0xF0, rs_bit);
    lcd_write_nibble((val << 4) & 0xF0, rs_bit);
}

void lcd_command(uint8_t cmd) { lcd_send_byte(cmd, 0); }
void lcd_data(uint8_t data) { lcd_send_byte(data, BIT_RS); }

void lcd_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_21,
        .scl_io_num = GPIO_NUM_22,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 50000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    vTaskDelay(pdMS_TO_TICKS(100));

    lcd_write_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_write_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_write_nibble(0x30, 0);
    lcd_write_nibble(0x20, 0);

    lcd_command(0x28);
    lcd_command(0x0C);
    lcd_command(0x06);
    lcd_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t offsets[] = {0x00, 0x40, 0x14, 0x54};
    lcd_command(0x80 | (col + offsets[row]));
}

void lcd_print(const char *str) {
    while (*str) lcd_data(*str++);
}

void lcd_clear(void) {
    lcd_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void init_lcd_and_buttons(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_BTN_NEXT) | (1ULL << PIN_BTN_PREV),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    lcd_init();
}

void lcd_menu_task(void *pvParameter) {
    int btn_next_last = 1;
    int btn_prev_last = 1;
    char buf[64]; // Буфер 64 байта гарантированно исключает format-truncation
    uint32_t redraw_timer = 0;
    bool force_redraw = true;

    while (1) {
        int r = 0, c = 0, d = 0;
        get_stats(&r, &c, &d);

        int r_pages = (r > 0) ? ((r + 2) / 3) : 1;
        int c_pages = (c > 0) ? ((c + 2) / 3) : 1;
        int total_screens = 1 + r_pages + c_pages;

        int btn_next = gpio_get_level(PIN_BTN_NEXT);
        int btn_prev = gpio_get_level(PIN_BTN_PREV);

        // Кнопка ВПРАВО
        if (btn_next == 0 && btn_next_last == 1) {
            current_screen = (current_screen + 1) % total_screens;
            lcd_clear();
            force_redraw = true;
        }

        // Кнопка ВЛЕВО
        if (btn_prev == 0 && btn_prev_last == 1) {
            current_screen = (current_screen - 1 + total_screens) % total_screens;
            lcd_clear();
            force_redraw = true;
        }

        btn_next_last = btn_next;
        btn_prev_last = btn_prev;

        if (current_screen >= total_screens) {
            current_screen = 0;
            force_redraw = true;
        }

        redraw_timer += 20;
        if (redraw_timer >= 500 || force_redraw) {
            redraw_timer = 0;
            force_redraw = false;

            // Экран 0: Статистика
		if (current_screen == 0) {
    		lcd_set_cursor(0, 0); lcd_print("    ESP32 SNIFFER    "); 
    		lcd_set_cursor(0, 1); lcd_print("                    "); 
    		snprintf(buf, sizeof(buf), "Routers: %-11d", r);
    		lcd_set_cursor(0, 2); lcd_print(buf);
    		snprintf(buf, sizeof(buf), "Clients: %-11d", c);
    		lcd_set_cursor(0, 3); lcd_print(buf);
		}
            // Страницы роутеров
            else if (current_screen <= r_pages) {
                int r_page = current_screen - 1;
                snprintf(buf, sizeof(buf), "   ROUTERS (%d/%d)   ", r_page + 1, r_pages);
                lcd_set_cursor(0, 0); lcd_print(buf);

                int skip = r_page * 3;
                int row = 1;

                for (int i = 0; i < device_count && row < 4; i++) {
                    if (device_table[i].type == DEVICE_ROUTER) {
                        if (skip > 0) {
                            skip--;
                            continue;
                        }
                        const char *name = (device_table[i].ssid[0] != '\0') ? device_table[i].ssid : "<Hidden>";
                        snprintf(buf, sizeof(buf), "%-16.16s%4d", name, device_table[i].rssi);
                        lcd_set_cursor(0, row++); lcd_print(buf);
                    }
                }
                while (row < 4) {
                    lcd_set_cursor(0, row++);
                    lcd_print("                    ");
                }
            }
            // Страницы клиентов
            else {
                int c_page = current_screen - 1 - r_pages;
                snprintf(buf, sizeof(buf), "   CLIENTS (%d/%d)   ", c_page + 1, c_pages);
                lcd_set_cursor(0, 0); lcd_print(buf);

                int skip = c_page * 3;
                int row = 1;

                for (int i = 0; i < device_count && row < 4; i++) {
                    if (device_table[i].type == DEVICE_CLIENT) {
                        if (skip > 0) {
                            skip--;
                            continue;
                        }
                        snprintf(buf, sizeof(buf), "%02X:%02X:%02X %-8.8s%3d", 
                                 device_table[i].mac[3], device_table[i].mac[4], device_table[i].mac[5],
                                 device_table[i].vendor, device_table[i].rssi);
                        lcd_set_cursor(0, row++); lcd_print(buf);
                    }
                }
                while (row < 4) {
                    lcd_set_cursor(0, row++);
                    lcd_print("                    ");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
