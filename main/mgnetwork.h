/*
 * mgnetwork.h
 *
 *  Created on: 15 Jun 2026
 *      Author: gardn
 */

#ifndef MGNETWORK_H_
#define MGNETWORK_H_
#include "base.h"

#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif_ppp.h"


/* Declare this in mgnetwork.h, which hello_world_main.c already includes. */
esp_err_t initialize_modem_drivers(void);

void tcp_time_task(void *pvParameters);
void ensure_modem_off(void);
void graceful_power_down(void);
void check_modem_down(void);
void cleanup_modem_queues();
esp_err_t configure_quectel_eg915(esp_modem_dce_t *dce, esp_netif_t *esp_netif);

/**
 * Check modem power state and brings it up if necessary  
 */
esp_err_t ensure_modem_on(void);
esp_err_t upgrade_modem_baud_rate(void);
esp_err_t   display_modem_state();

esp_err_t wait_for_cellular_ready(void);



#endif /* MGNETWORK_H_ */
