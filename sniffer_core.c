/*
 * sniffer_core.c
 *
 *  Created on: 1 сент. 2026 г.
 *      Author: Lev
 */
#include "sniffer_core.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

device_info_t device_table[MAX_DEVICES];
int device_count = 0;
int deauth_count = 0;
static SemaphoreHandle_t table_mutex = NULL;

typedef struct {
    uint8_t prefix[3];
    const char *vendor_name;
} oui_entry_t;

static const oui_entry_t OUI_DATABASE[] = {
    {{0xF0, 0x18, 0x98}, "Apple"},
    {{0xAC, 0xBC, 0x32}, "Apple"},
    {{0xA8, 0x9C, 0xED}, "Samsung"},
    {{0x5C, 0xE8, 0xEB}, "Samsung"},
    {{0x64, 0x09, 0x80}, "Xiaomi"},
    {{0x88, 0x6C, 0x60}, "Xiaomi"},
    {{0xD8, 0x45, 0x67}, "Tecno"},
    {{0x38, 0xD5, 0x7A}, "Cloud NT"}
};
#define OUI_DB_SIZE (sizeof(OUI_DATABASE) / sizeof(oui_entry_t))

static void get_vendor(const uint8_t *mac, char *vendor_out) {
    if (mac[0] & 0x02) {
        strcpy(vendor_out, "Random");
        return;
    }
    for (size_t i = 0; i < OUI_DB_SIZE; i++) {
        if (mac[0] == OUI_DATABASE[i].prefix[0] &&
            mac[1] == OUI_DATABASE[i].prefix[1] &&
            mac[2] == OUI_DATABASE[i].prefix[2]) {
            strcpy(vendor_out, OUI_DATABASE[i].vendor_name);
            return;
        }
    }
    strcpy(vendor_out, "Unknown");
}

static void update_or_add_device(const uint8_t *mac, device_type_t type, const char *ssid, int8_t rssi) {
    if (xSemaphoreTake(table_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    for (int i = 0; i < device_count; i++) {
        if (memcmp(device_table[i].mac, mac, 6) == 0) {
            device_table[i].rssi = rssi;
            if (type == DEVICE_ROUTER && ssid && strlen(ssid) > 0) {
                strncpy(device_table[i].ssid, ssid, sizeof(device_table[i].ssid) - 1);
            }
            xSemaphoreGive(table_mutex);
            return;
        }
    }

    if (device_count < MAX_DEVICES) {
        memcpy(device_table[device_count].mac, mac, 6);
        device_table[device_count].type = type;
        device_table[device_count].rssi = rssi;
        get_vendor(mac, device_table[device_count].vendor);

        if (type == DEVICE_ROUTER && ssid != NULL) {
            strncpy(device_table[device_count].ssid, ssid, sizeof(device_table[device_count].ssid) - 1);
        } else {
            device_table[device_count].ssid[0] = '\0';
        }
        device_count++;
    }
    xSemaphoreGive(table_mutex);
}

static void parse_beacon_ssid(uint8_t *payload, uint16_t length, char *ssid_out) {
    uint16_t offset = 36;
    if (length <= offset + 2) {
        strcpy(ssid_out, "<Hidden>");
        return;
    }
    uint8_t tag_num = payload[offset];
    uint8_t tag_len = payload[offset + 1];
    if (tag_num == 0 && tag_len <= 32 && (offset + 2 + tag_len) <= length) {
        if (tag_len == 0) strcpy(ssid_out, "<Hidden>");
        else {
            memcpy(ssid_out, &payload[offset + 2], tag_len);
            ssid_out[tag_len] = '\0';
        }
    } else {
        strcpy(ssid_out, "<Unknown>");
    }
}

static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t *payload = pkt->payload;
    uint16_t length = pkt->rx_ctrl.sig_len;
    uint8_t frame_type = payload[0];

    uint8_t *src_mac = &payload[10];

    if (frame_type == 0x80) { // Beacon
        char ssid[33] = {0};
        parse_beacon_ssid(payload, length, ssid);
        update_or_add_device(src_mac, DEVICE_ROUTER, ssid, pkt->rx_ctrl.rssi);
    } else if (frame_type == 0x40) { // Probe Request
        update_or_add_device(src_mac, DEVICE_CLIENT, NULL, pkt->rx_ctrl.rssi);
    }
}
void get_stats(int *routers, int *clients, int *deauth) {
    if (xSemaphoreTake(table_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *routers = 0;
        *clients = 0;
        for (int i = 0; i < device_count; i++) {
            if (device_table[i].type == DEVICE_ROUTER) (*routers)++;
            else (*clients)++;
        }
        *deauth = deauth_count;
        xSemaphoreGive(table_mutex);
    }
}

void channel_hop_task(void *pvParameter) {
    uint8_t chan = 1;
    while (1) {
        esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
        chan++;
        if (chan > 13) chan = 1;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void init_sniffer_system(void) {
    table_mutex = xSemaphoreCreateMutex();

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

    xTaskCreate(&channel_hop_task, "hop_task", 2048, NULL, 5, NULL);
}



