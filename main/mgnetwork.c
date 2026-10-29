/*
 * mgnetwork.c
 *
 *  Created on: 15 Jun 2026
 *      Author: gardn
 */
#include "mgnetwork.h"
#include "esp_err.h"
#include <fcntl.h>

static const char *MGNW_TAG = "MG_NW";
extern esp_modem_dce_t *dce;
extern SemaphoreHandle_t connection_sema; 
extern rep_state reporting_state;
extern esp_netif_t * esp_netif;

static esp_err_t log_modem_status(const char *command)
{
    char response[256] = {0};
    esp_err_t err = esp_modem_at(dce, command, response, 3000);
    if (err == ESP_OK) {
        ESP_LOGI(MGNW_TAG, "%s: %s", command, response);
    } else {
        ESP_LOGW(MGNW_TAG, "%s failed: %s", command, esp_err_to_name(err));
    }
    return err;
}


/*  Workflow and lifecycle for clip sampling and PPP network connections
 *  1. Initialise the system
 *  2. Initialise the uart and serial dte 
 *  3. Initialise the modem
 *  4. Enter the state-handler Loop:
 *      Start in idle mode
 *      Check time and if active time enter sampling mode
 *      If sample matches rules:
 *          Capture a 5 second clip      
 *          Power on the modem
 *          Create a TCP socket and request PPP mode
 *          Wait in the state handler for the PPP IP event
 *          Send and archive each pending clip
 *          On completeion of the send:
 *          Break out of PPP mode back to command mode 
 *          Power down the modem
 *          Enter the idle state re-start at line 20
 * 
 */ 



esp_err_t wait_for_cellular_ready(void)
{
    esp_err_t err = esp_modem_at(dce, "AT+CEREG=2\r", NULL, 3000);
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Failed to enable detailed registration reports: %s",
                 esp_err_to_name(err));
        return err;
    }

    log_modem_status("AT+CGDCONT?");

    const TickType_t registration_timeout = pdMS_TO_TICKS(120000);
    const TickType_t poll_interval = pdMS_TO_TICKS(5000);
    TickType_t start = xTaskGetTickCount();
    bool registered = false;

    while ((xTaskGetTickCount() - start) < registration_timeout) {
        char response[128] = {0};
        int reporting_mode;
        int registration_status;

        err = esp_modem_at(dce, "AT+CEREG?\r", response, 3000);
        char *registration = strstr(response, "+CEREG:");

        if (err == ESP_OK && registration != NULL &&
            sscanf(registration, "+CEREG: %d,%d",
                   &reporting_mode, &registration_status) == 2) {
            if (registration_status == 1 || registration_status == 5) {
                ESP_LOGI(MGNW_TAG, "Cellular registration successful (status %d)",
                         registration_status);
                registered = true;
                break;
            }

            ESP_LOGI(MGNW_TAG, "Waiting for cellular registration (status %d)",
                     registration_status);
        } else {
            ESP_LOGW(MGNW_TAG, "Could not parse AT+CEREG? response: %s", response);
        }

        vTaskDelay(poll_interval);
    }

    if (!registered) {
        ESP_LOGE(MGNW_TAG, "Timed out waiting for cellular registration");
        err = log_modem_status("AT+CPIN?");
        if (err == ESP_OK) {
          err = log_modem_status("AT+CFUN?");
        }
        if (err == ESP_OK) {
            log_modem_status("AT+COPS?");
        }
        if (err == ESP_OK) {
            log_modem_status("AT+CEREG?");
        }
        if (err == ESP_OK) {
            log_modem_status("AT+CGATT?");
        }
        if (err == ESP_OK) {
            log_modem_status("AT+QNWINFO");
        }
        return ESP_ERR_TIMEOUT;
    }

    const TickType_t attachment_timeout = pdMS_TO_TICKS(60000);
    start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < attachment_timeout) {
        char response[128] = {0};
        int attached = 0;

        err = esp_modem_at(dce, "AT+CGATT?\r", response, 3000);
        char *attachment = strstr(response, "+CGATT:");

        if (err == ESP_OK && attachment != NULL &&
            sscanf(attachment, "+CGATT: %d", &attached) == 1) {
            if (attached == 1) {
                ESP_LOGI(MGNW_TAG, "Packet service attached");
                return ESP_OK;
            }

            ESP_LOGI(MGNW_TAG, "Waiting for packet-service attachment");
        } else {
            ESP_LOGW(MGNW_TAG, "Could not parse AT+CGATT? response: %s", response);
        }

        vTaskDelay(poll_interval);
    }

    ESP_LOGE(MGNW_TAG, "Timed out waiting for packet-service attachment");
    log_modem_status("AT+CGATT?");
    log_modem_status("AT+QNWINFO");
    return ESP_ERR_TIMEOUT;
}


/**
 * @brief Configures APN, IPv4 protocol, PAP authentication, and roaming on a Quectel EG915 modem.
 */
esp_err_t configure_quectel_eg915(esp_modem_dce_t *dce, esp_netif_t *esp_netif)
{
    if (dce == NULL || esp_netif == NULL) {
        ESP_LOGE(MGNW_TAG, "Invalid modem or netif handle");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    // 1 & 2. Set APN to "eapn1.net" and IP Protocol to IPv4 ("IP") using CamelCase struct
    esp_modem_PdpContext_t pdp_ctx = {
        .apn = "eapn1.net",
        .protocol_type = "IP",  
        .context_id = 1
    };
    
    // Pass by reference using '&'
    err = esp_modem_set_pdp_context(dce, &pdp_ctx); 
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Failed to set APN and IP Protocol: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(MGNW_TAG, "APN set to '%s' and protocol set to '%s' context ID = %d", pdp_ctx.apn, pdp_ctx.protocol_type, pdp_ctx.context_id);

    // 3. Set Authentication type to PAP on the PPP network interface
    err = esp_netif_ppp_set_auth(esp_netif, NETIF_PPP_AUTHTYPE_PAP, "", "");
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Failed to set PAP authentication: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(MGNW_TAG, "Authentication type configured to PAP");

    // 4. Enable Roaming via Quectel-specific AT command configuration
    // Corrected to exactly 4 parameters: dce, command string, response buffer, timeout
    // Using NULL because we only care if the modem returns ESP_OK or a timeout/error
    err = esp_modem_at(dce, "AT+QCFG=\"roamservice\",2\r", NULL, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Failed to enable roaming via AT command: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(MGNW_TAG, "Roaming successfully enabled");

    return ESP_OK;
}





/**
 * Toggles the EG915 Power state.
 * Pulse duration of 1 second is standard for most EG915 carrier boards.
 */
void pulse_modem_power(void) {
    ESP_LOGI(MGNW_TAG, "Pulsing Modem PWRKEY on GPIO %d...", MODEM_PWRKEY_PIN);
    
    // Pull Low (via inverter)
    gpio_set_level(MODEM_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000)); // 1 second pulse
    
    // Release High (via inverter)
    gpio_set_level(MODEM_PWRKEY_PIN, 0);
    
    // Wait for the modem to actually boot or shut down
    ESP_LOGI(MGNW_TAG, "Waiting for modem state transition...");
    vTaskDelay(pdMS_TO_TICKS(5000)); 
}

esp_err_t ensure_modem_on(void) {
    const int max_power_retries = 5;
    esp_err_t err;

    /* The driver/netif must exist before esp_modem can inspect or change mode. */
    if (dce == NULL || esp_netif == NULL) {
        err = initialize_modem_drivers();
        if (err != ESP_OK) {
            ESP_LOGE(MGNW_TAG, "Failed to initialize modem drivers: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(MGNW_TAG, "Checking modem power and operating mode...");
    esp_err_t modem_error = esp_modem_sync(dce);

//    /* A live modem in PPP/data mode may not answer AT sync. Return it to command mode
//       first; do not pulse PWRKEY unless this recovery and a second sync both fail. */
//    if (modem_error != ESP_OK) {
//        ESP_LOGW(MGNW_TAG, "AT sync failed; requesting COMMAND mode before power cycling");
//        esp_err_t mode_error = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
//        if (mode_error == ESP_OK) {
//            vTaskDelay(pdMS_TO_TICKS(500));
//            modem_error = esp_modem_sync(dce);
//        } else {
//            ESP_LOGW(MGNW_TAG, "Could not switch to COMMAND mode: %s",
//                     esp_err_to_name(mode_error));
//        }
//    }

    err = log_modem_status("AT+CSQ");
    if (err == ESP_OK) {
        log_modem_status("AT+CEREG?");
    }
    if (err == ESP_OK) {
        log_modem_status("AT+CGATT?");
    }
    if (err == ESP_OK) {
        log_modem_status("AT+CGDCONT?");
    }
    if (err == ESP_OK) {
        log_modem_status("AT+QNWINFO");
    }
    if (err == ESP_OK) {
        log_modem_status("AT+CNUM");
    }

    for (int attempt = 0;
         modem_error != ESP_OK && attempt < max_power_retries;
         ++attempt) {
        ESP_LOGW(MGNW_TAG, "Modem not responding; PWRKEY attempt %d/%d",
                 attempt + 1, max_power_retries);
        pulse_modem_power();
        vTaskDelay(pdMS_TO_TICKS(8000));
        modem_error = esp_modem_sync(dce);
    }

    if (modem_error != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Modem did not respond after mode recovery and %d power attempts: %s",
                 max_power_retries, esp_err_to_name(modem_error));
        return modem_error;
    }

    ESP_LOGI(MGNW_TAG, "Modem is online and in command mode.");
   
//    err = upgrade_modem_baud_rate();
//    if (err != ESP_OK) {
//        ESP_LOGE(MGNW_TAG, "Failed to upgrade modem baud rate: %s",
//                 esp_err_to_name(err));
//        return err;
//    }
//
//    
    
    err = configure_quectel_eg915(dce, esp_netif);
    if (err != ESP_OK) {
        return err;
    }
    
    return wait_for_cellular_ready();    
    //return configure_quectel_eg915(dce, esp_netif);
}


void ensure_modem_off(void) {
    int err = -1;
    esp_err_t modem_error = esp_modem_sync(dce);
    
    if (modem_error == ESP_OK) {
        ESP_LOGI(MGNW_TAG, "Modem is SYNCED and responding to AT.");
    }
    else {
       
        ESP_LOGI(MGNW_TAG, "Modem not SYNCED.  Requesting COMMAND mode");
        err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        if (err != ESP_OK) {
            ESP_LOGE(MGNW_TAG, "Failed to switch to COMMAND mode");
        }
        /*  TODO:  This assumes the modem is still operational, there may be a condition where it's not  */
        ESP_LOGI(MGNW_TAG, "Shutting Down MODEM");
        pulse_modem_power();
    }
}

void graceful_power_down(void) {
    ESP_LOGI(MGNW_TAG, "Stopping modem and PPP network interface...");
    
    if (dce != NULL) {
        if (esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));

            ESP_LOGI(MGNW_TAG, "Sending AT+QPOWD...");
            esp_err_t err = esp_modem_at(dce, "AT+QPOWD=1", NULL, 5000);
            if (err == ESP_OK) {
                ESP_LOGI(MGNW_TAG, "Modem safely powered down via software.");
            } else {
                ESP_LOGW(MGNW_TAG, "No response to QPOWD, forcing hardware fallback.");
                pulse_modem_power();
            }
        } else {
            ESP_LOGE(MGNW_TAG, "Could not exit PPP mode. Forcing hardware shutdown.");
            pulse_modem_power();
        }

        uart_flush_input(MODEM_UART_PORT);
        ESP_LOGI(MGNW_TAG, "Destroying modem driver objects...");
        esp_modem_destroy(dce);
        dce = NULL;
    }

    if (esp_netif != NULL) {
        esp_netif_destroy(esp_netif);
        esp_netif = NULL;
    }

    ESP_LOGI(MGNW_TAG, "Power down and driver teardown complete.");
}


void check_modem_down(void) {
    ESP_LOGI(MGNW_TAG, "(check_powered_down)  Checking Modem OFF...");
    if (dce == NULL) {
        ESP_LOGI(MGNW_TAG, "Modem driver is not initialized");
        return;
    }
    esp_err_t modem_error = esp_modem_sync(dce);
   
    // 1. Drop out of PPP mode back to AT command mode
    if (modem_error == ESP_OK) {
        ESP_LOGI(MGNW_TAG, "Modem is SYNCED and responding to AT.");
        vTaskDelay(pdMS_TO_TICKS(500)); // Allow AT interface to settle
        ESP_LOGI(MGNW_TAG, "Sending AT+QPOWD...");
        esp_err_t err = esp_modem_at(dce, "AT+QPOWD=1", NULL, 5000);
        
        if (err == ESP_OK) {
            ESP_LOGI(MGNW_TAG, "Modem safely powered down.");
        } else {
            ESP_LOGW(MGNW_TAG, "No response to QPOWD, forcing hardware fallback.");
            pulse_modem_power(); // Fallback to PWRKEY if software fails
            
            
            // 5. Clear the driver buffer cleanly so your next AT parser read starts fresh
            int length = 0;
            uart_get_buffered_data_len(UART_NUM_1, (size_t*)&length);
            if (length > 0) {
                uint8_t* junk = (uint8_t*) malloc(length);
                uart_read_bytes(UART_NUM_1, junk, length, 0);
                free(junk);
            }
        }
    } else {
        ESP_LOGI(MGNW_TAG, "MODEM is already OFF");
    }
}

esp_err_t upgrade_modem_baud_rate(void)
{
    if (dce == NULL) {
        ESP_LOGE(MGNW_TAG, "Cannot change baud rate: modem is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(MGNW_TAG, "Requesting modem baud rate %d", TARGET_BAUD_RATE);

    /* Sends AT+IPR and reports whether the modem accepted the command. */
    esp_err_t err = esp_modem_set_baud(dce, TARGET_BAUD_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Modem rejected or did not confirm baud change: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* The modem accepted the change; now match the ESP32 UART to it. */
    err = uart_set_baudrate(MODEM_UART_PORT, TARGET_BAUD_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Failed to set ESP32 UART baud rate: %s",
                 esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    /* Querying the setting at the new rate also checks that communication works. */
    char response[128] = {0};
    err = esp_modem_at(dce, "AT+IPR?\r", response, 2000);
    if (err != ESP_OK) {
        ESP_LOGE(MGNW_TAG, "Modem did not respond at baud rate %d: %s",
                 TARGET_BAUD_RATE, esp_err_to_name(err));
        return err;
    }

    int reported_baud = 0;
    char *ipr_response = strstr(response, "+IPR:");
    if (ipr_response == NULL ||
        sscanf(ipr_response, "+IPR: %d", &reported_baud) != 1) {
        ESP_LOGE(MGNW_TAG, "Could not parse AT+IPR? response: %s", response);
        return ESP_FAIL;
    }

    if (reported_baud != TARGET_BAUD_RATE) {
        ESP_LOGE(MGNW_TAG, "Modem reports baud rate %d; expected %d",
                 reported_baud, TARGET_BAUD_RATE);
        return ESP_FAIL;
    }

    ESP_LOGI(MGNW_TAG, "Baud rate verified at %d", reported_baud);
    return ESP_OK;
}


void cleanup_modem_queues(void) {
    ESP_LOGI(MGNW_TAG, "Cleaning up ESP32 UART and modem instances (IDF 5.3)...");

    // 1. Destroy the esp_modem object
    // This safely detaches the modem from the netif layer and frees internal tasks
    if (dce != NULL) {
        esp_modem_destroy(dce);
        dce = NULL; 
    }

    // 2. Completely destroy the network interface 
    // This removes all active PPP/IP layer handlers automatically
    if (esp_netif != NULL) {
        esp_netif_destroy(esp_netif);
        esp_netif = NULL;
    }

//    // 3. Purge remaining remnants from the hardware UART FIFO
//    // Use the specific UART port instance connected to your EG915
//    uart_flush_input(UART_NUM_1);
//    uart_flush(UART_NUM_1); 
//
    ESP_LOGI(MGNW_TAG, "Cleanup finished. Ready for safe reboot sequence.");
}

static bool connect_time_socket(int fd, const struct sockaddr *address, socklen_t address_length) {
    int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        return false;
    }

    int connect_result = connect(fd, address, address_length);
    if (connect_result != 0 && errno != EINPROGRESS) {
        fcntl(fd, F_SETFL, original_flags);
        return false;
    }

    if (connect_result != 0) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
        int select_result = select(fd + 1, NULL, &write_fds, NULL, &timeout);
        if (select_result <= 0) {
            if (select_result == 0) {
                errno = ETIMEDOUT;
            }
            fcntl(fd, F_SETFL, original_flags);
            return false;
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            if (socket_error != 0) {
                errno = socket_error;
            }
            fcntl(fd, F_SETFL, original_flags);
            return false;
        }
    }

    return fcntl(fd, F_SETFL, original_flags) == 0;
}

time_t get_network_time(const char *hostname) {
    int time_sock;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    uint32_t network_time = 0;
    time_t current_time = 0;

    // Set up address criteria
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     // Works with IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets

    // Resolve the server hostname
    if ((rv = getaddrinfo(hostname, "37", &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", strerror(rv));
        return 0;
    }

    // Loop through results and connect to the first available
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((time_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            /*  No socket established , try next in for loop  */
            continue;
        }
        struct timeval receive_timeout = {.tv_sec = 15, .tv_usec = 0};
        if (setsockopt(time_sock, SOL_SOCKET, SO_RCVTIMEO,
                       &receive_timeout, sizeof(receive_timeout)) != 0 ||
            !connect_time_socket(time_sock, p->ai_addr, p->ai_addrlen)) {
            /*  Couldn't connect, close the socket and try next in for loop  */
            close(time_sock);
            continue;
        }
        /*  Reached here, successful connection and valid socket, break out of for loop  */ 
        break; 
    }
    if (p == NULL) {
        fprintf(stderr, "Failed to connect to %s\n", hostname);
        freeaddrinfo(servinfo);
        return 0;
    }

    freeaddrinfo(servinfo); // Free the linked list

    // Read the 4-byte time value from the server
    ssize_t numbytes = recv(time_sock, &network_time, sizeof(network_time), 0);
    close(time_sock);

    if (numbytes < (ssize_t)sizeof(network_time)) {
        fprintf(stderr, "Failed to receive full 4-byte time payload\n");
        return 0;
    }

    // Convert from network byte order (Big Endian) to host byte order
    network_time = ntohl(network_time);

    // Convert RFC 868 time (since 1900) to Unix time (since 1970)
    if (network_time >= BW_SECONDS_1900_TO_1970) {
        current_time = (time_t)(network_time - BW_SECONDS_1900_TO_1970);
    } else {
        fprintf(stderr, "Received invalid or wrapped timestamp\n");
        return 0;
    }
    return current_time;
}


esp_err_t   display_modem_state() {
esp_err_t err = ESP_OK;

    /*
    The key responses are:

        AT+CSQ: 99,99 — signal strength is unknown or unavailable.
        AT+CEREG?: 0,0 — the modem is not registered.
        AT+CGATT?: 0 — it is not attached to packet service.
        AT+QNWINFO: NO SERVICE — the modem reports no cellular network.
        The PPP “user interrupt” follows this failed connection attempt; it doesn’t explain why the modem has no service. Changing the PPP timeout or server settings won’t resolve the underlying issue.


        Check the SIM is active and unlocked, the antenna and RF connections are correct, and the modem has adequate power and coverage.
        For a useful next diagnostic, query AT+CPIN?, AT+CFUN?, and AT+COPS?; then enable detailed registration reporting with AT+CEREG=2 and query AT+CEREG? again. 
        Once registered, verify AT+CGATT? reports 1.

        One additional item to verify after registration: your log’s AT+CGDCONT? output shows context 9 using i-smsr.net, but not the expected context 1 using eapn1.net. 
        Check that context 1 is actually configured with the carrier’s required data APN.

    */



//    if (dce != NULL) {
//        esp_modem_dce_state_t state;
//        esp_err_t err = esp_modem_get_state(dce, &state);
//        if (err == ESP_OK) {
//            ESP_LOGI(MGNW_TAG, "Modem State: %d", state);
//        } else {
//            ESP_LOGE(MGNW_TAG, "Failed to get modem state: %s", esp_err_to_name(err));
//        }
//    } else {
//        ESP_LOGW(MGNW_TAG, "DCE handle is NULL; cannot display modem state.");
//    }

    return err;
}

