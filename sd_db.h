/*
 * sd_db.h
 *
 *  Created on: 20 окт. 2026 г.
 *      Author: Lev
 */

#ifndef MAIN_SD_DB_H_
#define MAIN_SD_DB_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SD_MOUNT_POINT "/sd_card"
#define SD_DB_PATH     "/sd_card/devices.db"

typedef enum {
    LOG_OP_ADD_NEW,
    LOG_OP_UPDATE_RSSI
} log_op_type_t;

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    char vendor[24];
    char ssid[33];
    int8_t rssi;
    uint8_t type;        
    uint8_t is_valid;
    uint8_t reserved[14]; 
} sd_db_record_t;

typedef struct {
    log_op_type_t op;
    sd_db_record_t record;
} sd_log_queue_item_t;

esp_err_t sd_db_init(void);
void sd_db_lock(void);
void sd_db_unlock(void);

void sd_db_get_counts(int *routers, int *clients);
int sd_db_read_page(uint8_t type, int page, int items_per_page, sd_db_record_t *out_buffer);

void sd_logger_task(void *pvParameter);
#endif /* MAIN_SD_DB_H_ */