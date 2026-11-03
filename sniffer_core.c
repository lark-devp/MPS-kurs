/*
 * sniffer_core.c
 *
 *  Created on: 1 сент. 2026 г.
 *      Author: Lev
 */

#include "sniffer_core.h"
#include "sd_db.h"
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "SNIFFER";

QueueHandle_t sd_log_queue = NULL;

typedef struct {
    uint8_t mac[6];
    char vendor[24];
    char ssid[33];
    int8_t rssi;
    device_type_t type;
    bool in_sd;          
    bool rssi_changed;   
    bool full_changed;   
} sram_device_t;

static sram_device_t sram_cache[MAX_DEVICES];
static int sram_count = 0;
static SemaphoreHandle_t cache_mutex = NULL;

typedef struct {
    uint8_t prefix[3];
    char vendor[24];
} oui_entry_t;

#define MAX_OUI_RECORDS 300
static oui_entry_t oui_table[MAX_OUI_RECORDS];
static int oui_count = 0;

void load_oui_database_from_sd(void) {
    sd_db_lock();
    FILE *f = fopen(OUI_FILE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "oui.txt not found on SD. Creating default template...");
        f = fopen(OUI_FILE_PATH, "w");
        if (f) {
            fprintf(f, "# Format: XX:XX:XX Vendor_Name\n");
            fprintf(f, "F0:18:98 Apple\n");
            fprintf(f, "AC:BC:32 Apple\n");
            fprintf(f, "A8:9C:ED Samsung\n");
            fprintf(f, "5C:E8:EB Samsung\n");
            fprintf(f, "64:09:80 Xiaomi\n");
            fprintf(f, "88:6C:60 Xiaomi\n");
            fprintf(f, "D8:45:67 Tecno\n");
            fprintf(f, "38:D5:7A Cloud NT\n");
            fclose(f);
        }
        f = fopen(OUI_FILE_PATH, "r");
    }

    if (f) {
        char line[64];
        oui_count = 0;
        while (fgets(line, sizeof(line), f) && oui_count < MAX_OUI_RECORDS) {
            if (line[0] == '#' || strlen(line) < 8) continue;
            unsigned int p[3];
            char vname[24] = {0};
            // Поддерживает и "AA:BB:CC Vendor", и "AABBCC Vendor"
            if (sscanf(line, "%02x:%02x:%02x %23[^\r\n]", &p[0], &p[1], &p[2], vname) >= 4 ||
                sscanf(line, "%02x%02x%02x %23[^\r\n]", &p[0], &p[1], &p[2], vname) >= 4) {
                oui_table[oui_count].prefix[0] = (uint8_t)p[0];
                oui_table[oui_count].prefix[1] = (uint8_t)p[1];
                oui_table[oui_count].prefix[2] = (uint8_t)p[2];
                strncpy(oui_table[oui_count].vendor, vname, sizeof(oui_table[oui_count].vendor) - 1);
                oui_count++;
            }
        }
        fclose(f);
        ESP_LOGI(TAG, "Loaded %d OUI records from SD", oui_count);
    }
    sd_db_unlock();
}

static void get_vendor(const uint8_t *mac, char *vendor_out) {
    if (mac[0] & 0x02) {
        strcpy(vendor_out, "Random");
        return;
    }
    for (int i = 0; i < oui_count; i++) {
        if (memcmp(mac, oui_table[i].prefix, 3) == 0) {
            strncpy(vendor_out, oui_table[i].vendor, 23);
            vendor_out[23] = '\0';
            return;
        }
    }
    strcpy(vendor_out, "Unknown");
}

static void update_or_add_device(const uint8_t *mac, device_type_t type, const char *ssid, int8_t rssi) {
    if (xSemaphoreTake(cache_mutex, 0) != pdTRUE) return;

    for (int i = 0; i < sram_count; i++) {
        if (memcmp(sram_cache[i].mac, mac, 6) == 0) {
            if (sram_cache[i].rssi != rssi) {
                sram_cache[i].rssi = rssi;
                sram_cache[i].rssi_changed = true;
            }

            if (type == DEVICE_ROUTER && sram_cache[i].type != DEVICE_ROUTER) {
                sram_cache[i].type = DEVICE_ROUTER;
                sram_cache[i].full_changed = true;
            }

            if (type == DEVICE_ROUTER && ssid && strlen(ssid) > 0 && strcmp(sram_cache[i].ssid, ssid) != 0) {
                strncpy(sram_cache[i].ssid, ssid, sizeof(sram_cache[i].ssid) - 1);
                sram_cache[i].ssid[sizeof(sram_cache[i].ssid) - 1] = '\0';
                sram_cache[i].full_changed = true;
            }

            xSemaphoreGive(cache_mutex);
            return;
        }
    }

    if (sram_count < MAX_DEVICES) {
        memcpy(sram_cache[sram_count].mac, mac, 6);
        sram_cache[sram_count].type = type;
        sram_cache[sram_count].rssi = rssi;
        get_vendor(mac, sram_cache[sram_count].vendor);

        if (type == DEVICE_ROUTER && ssid) {
            strncpy(sram_cache[sram_count].ssid, ssid, sizeof(sram_cache[sram_count].ssid) - 1);
            sram_cache[sram_count].ssid[sizeof(sram_cache[sram_count].ssid) - 1] = '\0';
        } else {
            sram_cache[sram_count].ssid[0] = '\0';
        }

        sram_cache[sram_count].in_sd = false;
        sram_cache[sram_count].full_changed = true;
        sram_cache[sram_count].rssi_changed = false;
        sram_count++;
    }
    xSemaphoreGive(cache_mutex);
}

void flush_to_queue_task(void *pvParameter) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); 

        if (xSemaphoreTake(cache_mutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < sram_count; i++) {
                if (!sram_cache[i].in_sd || sram_cache[i].full_changed) {
                    sd_log_queue_item_t item = {
                        .op = LOG_OP_ADD_NEW,
                        .record = {
                            .rssi = sram_cache[i].rssi,
                            .type = (uint8_t)sram_cache[i].type,
                            .is_valid = 1
                        }
                    };
                    memcpy(item.record.mac, sram_cache[i].mac, 6);
                    strncpy(item.record.vendor, sram_cache[i].vendor, sizeof(item.record.vendor) - 1);
                    strncpy(item.record.ssid, sram_cache[i].ssid, sizeof(item.record.ssid) - 1);

                    if (xQueueSend(sd_log_queue, &item, 0) == pdTRUE) {
                        sram_cache[i].in_sd = true;
                        sram_cache[i].full_changed = false;
                        sram_cache[i].rssi_changed = false;
                    }
                } 
                else if (sram_cache[i].rssi_changed) {
                    sd_log_queue_item_t item = {
                        .op = LOG_OP_UPDATE_RSSI,
                        .record = {
                            .rssi = sram_cache[i].rssi
                        }
                    };
                    memcpy(item.record.mac, sram_cache[i].mac, 6);

                    if (xQueueSend(sd_log_queue, &item, 0) == pdTRUE) {
                        sram_cache[i].rssi_changed = false;
                    }
                }
            }
            xSemaphoreGive(cache_mutex);
        }
    }
}

static void parse_beacon_ssid(const uint8_t *payload, uint16_t length, char *ssid_out) {
    ssid_out[0] = '\0';
    uint16_t offset = 36; 

    while (offset + 2 <= length) {
        uint8_t tag_num = payload[offset];
        uint8_t tag_len = payload[offset + 1];

        if (offset + 2 + tag_len > length) break;

        if (tag_num == 0) { 
            if (tag_len == 0) {
                strcpy(ssid_out, "<Hidden>");
                return;
            }
            if (tag_len > 32) tag_len = 32;

            for (int i = 0; i < tag_len; i++) {
                uint8_t c = payload[offset + 2 + i];
                ssid_out[i] = (c >= 32 && c <= 126) ? (char)c : '.';
            }
            ssid_out[tag_len] = '\0';
            return;
        }
        offset += 2 + tag_len;
    }
    strcpy(ssid_out, "<Hidden>");
}

static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t length = pkt->rx_ctrl.sig_len;

    if (length < 24) return;

    uint8_t frame_type = payload[0];
    const uint8_t *src_mac = &payload[10];

    // Игнорируем мультикаст/широковещательные MAC
    if (src_mac[0] & 0x01) return;

    if (frame_type == 0x80) { // Beacon -> РОУТЕР
        char ssid[33] = {0};
        parse_beacon_ssid(payload, length, ssid);
        update_or_add_device(src_mac, DEVICE_ROUTER, ssid, pkt->rx_ctrl.rssi);
    } 
    else if (frame_type == 0x40) { // Probe Request -> КЛИЕНТ
        update_or_add_device(src_mac, DEVICE_CLIENT, NULL, pkt->rx_ctrl.rssi);
    }
}

static void channel_hop_task(void *pvParameter) {
    uint8_t chan = 1;
    while (1) {
        esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
        chan = (chan >= 13) ? 1 : chan + 1;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void init_sniffer_system(void) {
    cache_mutex = xSemaphoreCreateMutex();
    sd_log_queue = xQueueCreate(64, sizeof(sd_log_queue_item_t));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_promiscuous_filter_t filter = { 
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT 
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    xTaskCreatePinnedToCore(&channel_hop_task, "hop_task", 2048, NULL, 3, NULL, 1);
}
