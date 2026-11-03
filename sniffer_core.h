/*
 * sniffer_core.h
 *
 *  Created on: 1 сент. 2026 г.
 *      Author: Lev
 */


#ifndef MAIN_SNIFFER_CORE_H_
#define MAIN_SNIFFER_CORE_H_

#include <stdint.h>
#include <stdbool.h>

#define MAX_DEVICES 128
#define OUI_FILE_PATH "/sd_card/oui.txt"

typedef enum {
    DEVICE_ROUTER = 0,
    DEVICE_CLIENT = 1
} device_type_t;

void init_sniffer_system(void);
void flush_to_queue_task(void *pvParameter);
void load_oui_database_from_sd(void);

#endif /* MAIN_SNIFFER_CORE_H_ */
