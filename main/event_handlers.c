/*
 * event_handlers.c
 *
 *  Created on: 10 Apr 2026
 *      Author: gardn
 */
#include "base.h"
#include "esp_netif_ppp.h"

extern SemaphoreHandle_t connection_sema;
extern esp_modem_dce_t *dce;

static const char * EVENT_TAG = "EVENT_HANDLER";

static const char *ppp_error_name(int32_t event_id)
{
    switch (event_id) {
    case NETIF_PPP_ERRORNONE: return "none";
    case NETIF_PPP_ERRORPARAM: return "invalid parameter";
    case NETIF_PPP_ERROROPEN: return "unable to open PPP session";
    case NETIF_PPP_ERRORDEVICE: return "invalid I/O device";
    case NETIF_PPP_ERRORALLOC: return "allocation failure";
    case NETIF_PPP_ERRORUSER: return "user interrupt";
    case NETIF_PPP_ERRORCONNECT: return "connection lost";
    case NETIF_PPP_ERRORAUTHFAIL: return "authentication failure";
    case NETIF_PPP_ERRORPROTOCOL: return "protocol failure";
    case NETIF_PPP_ERRORPEERDEAD: return "peer timeout";
    case NETIF_PPP_ERRORIDLETIMEOUT: return "idle timeout";
    case NETIF_PPP_ERRORCONNECTTIME: return "maximum connect time reached";
    case NETIF_PPP_ERRORLOOPBACK: return "loopback detected";
    case NETIF_PPP_CONNECT_FAILED: return "PPP connect failed";
    default: return "unknown PPP error";
    }
}

static const char *ppp_phase_name(int32_t event_id)
{
    switch (event_id - NETIF_PP_PHASE_OFFSET) {
    case 0: return "dead";
    case 1: return "master";
    case 2: return "holdoff";
    case 3: return "initialize";
    case 4: return "serial connect";
    case 5: return "dormant";
    case 6: return "establish";
    case 7: return "authenticate";
    case 8: return "callback";
    case 9: return "network";
    case 10: return "running";
    case 11: return "terminate";
    case 12: return "disconnect";
    default: return "unknown PPP phase";
    }
}

void on_ppp_status(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id >= NETIF_PP_PHASE_OFFSET) {
        ESP_LOGI(EVENT_TAG, "PPP phase: %s (%" PRIi32 ")",
                 ppp_phase_name(event_id), event_id);
    } else if (event_id > 0) {
        ESP_LOGE(EVENT_TAG, "PPP error: %s (%" PRIi32 ")",
                 ppp_error_name(event_id), event_id);
    }
}

void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    /*  As soon as the network connection has been established and an IP address has been obtained, the connection_sema can be signalled.  
     *  This will unblock the socket_send() function  */
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(EVENT_TAG, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (connection_sema != NULL) {
            xSemaphoreGive(connection_sema);
        }
        else {
            ESP_LOGI(EVENT_TAG, "Connection semaphore not initialised");
        }
    }
    /*  NO ATTEMPT AT LOST IP RECOVERY  */ 
    else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(EVENT_TAG, "PPP Link Lost! (Not Attempting Recovery) ");
        
        // 1. Ensure the modem is physically powered and responding
        //    ensure_modem_on();

        // 2. Attempt to re-enter Data Mode
        //  esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
        //  if (err != ESP_OK) {
            //  ESP_LOGE(EVENT_TAG, "Recovery: Failed to re-enter PPP mode");
        //  }
    }
}
