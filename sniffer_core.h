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

#define MAX_DEVICES 64

typedef enum {
    DEVICE_ROUTER,
    DEVICE_CLIENT
} device_type_t;

typedef struct {
    uint8_t mac[6];
    char vendor[24];
    char ssid[33];
    int8_t rssi;
    device_type_t type;
} device_info_t;

extern device_info_t device_table[MAX_DEVICES];
extern int device_count;
extern int deauth_count;

void init_sniffer_system(void);
void get_stats(int *routers, int *clients, int *deauth);




#endif /* MAIN_SNIFFER_CORE_H_ */
