/*
 * sd_db.c
 *
 *  Created on: 20 окт. 2026 г.
 *      Author: Lev
 */
 
#include "sd_db.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "esp_efuse.h"

static const char *TAG = "SD_SPI";

static uint8_t AES_KEY[16];

typedef struct __attribute__((packed)) {
    uint8_t iv[16];
    uint8_t encrypted_data[sizeof(sd_db_record_t)];
} sd_disk_record_t;

#define PIN_NUM_MISO GPIO_NUM_4
#define PIN_NUM_MOSI GPIO_NUM_23
#define PIN_NUM_CLK  GPIO_NUM_14
#define PIN_NUM_CS   GPIO_NUM_5

extern QueueHandle_t sd_log_queue;
static SemaphoreHandle_t sd_mutex = NULL;
static sdmmc_card_t *card = NULL;

void sd_db_lock(void) {
    if (sd_mutex) {
        xSemaphoreTake(sd_mutex, portMAX_DELAY);
    }
}

void sd_db_unlock(void) {
    if (sd_mutex) {
        xSemaphoreGive(sd_mutex);
    }
}

static void encrypt_record(const sd_db_record_t *src, sd_disk_record_t *dst) {
    esp_fill_random(dst->iv, sizeof(dst->iv));
    uint8_t iv_copy[16];
    memcpy(iv_copy, dst->iv, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, AES_KEY, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, sizeof(sd_db_record_t),
                          iv_copy, (const unsigned char *)src, dst->encrypted_data);
    mbedtls_aes_free(&ctx);
}

static void decrypt_record(mbedtls_aes_context *ctx, const sd_disk_record_t *src, sd_db_record_t *dst) {
    uint8_t iv_copy[16];
    memcpy(iv_copy, src->iv, 16);
    mbedtls_aes_crypt_cbc(ctx, MBEDTLS_AES_DECRYPT, sizeof(sd_db_record_t),
                          iv_copy, src->encrypted_data, (unsigned char *)dst);
}

esp_err_t sd_db_init(void) {
	esp_err_t err = esp_efuse_read_block(EFUSE_BLK3, AES_KEY, 0, 128);
    if (!sd_mutex) {
        sd_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Initializing SPI bus for SD card...");

    gpio_set_pull_mode(PIN_NUM_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK,  GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS,   GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 4000; 

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus (0x%x)", ret);
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting SD filesystem via SPI (4 MHz)...");
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card via SPI (0x%x: %s)", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SUCCESS! SD Card mounted: %s (Encrypted AES-128-CBC)", card->cid.name);
    sdmmc_card_print_info(stdout, card);

    sd_db_lock();
    FILE *f = fopen(SD_DB_PATH, "ab+");
    if (f) fclose(f);
    sd_db_unlock();

    return ESP_OK;
}

static long find_record_offset(FILE *f, const uint8_t *mac, sd_db_record_t *out_rec) {
    fseek(f, 0, SEEK_SET);
    sd_disk_record_t disk_rec;
    long offset = 0;

    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_dec(&aes_ctx, AES_KEY, 128);

    while (fread(&disk_rec, sizeof(sd_disk_record_t), 1, f) == 1) {
        sd_db_record_t rec;
        decrypt_record(&aes_ctx, &disk_rec, &rec);
        if (rec.is_valid && memcmp(rec.mac, mac, 6) == 0) {
            if (out_rec) {
                *out_rec = rec;
            }
            mbedtls_aes_free(&aes_ctx);
            return offset;
        }
        offset += sizeof(sd_disk_record_t);
    }
    mbedtls_aes_free(&aes_ctx);
    return -1;
}

void sd_db_get_counts(int *routers, int *clients) {
    *routers = 0;
    *clients = 0;

    sd_db_lock();
    FILE *f = fopen(SD_DB_PATH, "rb");
    if (!f) {
        sd_db_unlock();
        return;
    }

    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_dec(&aes_ctx, AES_KEY, 128);

    sd_disk_record_t disk_rec;
    sd_db_record_t rec;
    while (fread(&disk_rec, sizeof(sd_disk_record_t), 1, f) == 1) {
        decrypt_record(&aes_ctx, &disk_rec, &rec);
        if (rec.is_valid) {
            if (rec.type == 0) (*routers)++;
            else (*clients)++;
        }
    }
    mbedtls_aes_free(&aes_ctx);
    fclose(f);
    sd_db_unlock();
}

int sd_db_read_page(uint8_t type, int page, int items_per_page, sd_db_record_t *out_buffer) {
    int read_count = 0;
    int skip = page * items_per_page;

    sd_db_lock();
    FILE *f = fopen(SD_DB_PATH, "rb");
    if (!f) {
        sd_db_unlock();
        return 0;
    }

    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_dec(&aes_ctx, AES_KEY, 128);

    sd_disk_record_t disk_rec;
    sd_db_record_t rec;
    while (fread(&disk_rec, sizeof(sd_disk_record_t), 1, f) == 1) {
        decrypt_record(&aes_ctx, &disk_rec, &rec);
        if (rec.is_valid && rec.type == type) {
            if (skip > 0) {
                skip--;
                continue;
            }
            out_buffer[read_count++] = rec;
            if (read_count >= items_per_page) break;
        }
    }
    mbedtls_aes_free(&aes_ctx);
    fclose(f);
    sd_db_unlock();
    return read_count;
}

void sd_logger_task(void *pvParameter) {
    sd_log_queue_item_t item;
    ESP_LOGI(TAG, "Encrypted SD Logger task running on Core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(sd_log_queue, &item, portMAX_DELAY) == pdTRUE) {
            sd_db_lock();
            FILE *f = fopen(SD_DB_PATH, "r+b");
            if (!f) {
                f = fopen(SD_DB_PATH, "w+b");
            }

            if (f) {
                sd_db_record_t existing_rec;
                long offset = find_record_offset(f, item.record.mac, &existing_rec);
                sd_disk_record_t disk_rec;

                if (item.op == LOG_OP_UPDATE_RSSI && offset >= 0) {
                    existing_rec.rssi = item.record.rssi;
                    encrypt_record(&existing_rec, &disk_rec);
                    fseek(f, offset, SEEK_SET);
                    fwrite(&disk_rec, sizeof(sd_disk_record_t), 1, f);
                } else if (item.op == LOG_OP_ADD_NEW) {
                    encrypt_record(&item.record, &disk_rec);
                    if (offset >= 0) {
                        fseek(f, offset, SEEK_SET);
                    } else {
                        fseek(f, 0, SEEK_END);
                    }
                    fwrite(&disk_rec, sizeof(sd_disk_record_t), 1, f);
                }
                fflush(f);
                fclose(f);
            }
            sd_db_unlock();
        }
    }
}