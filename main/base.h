/*
 * base.h
 *  Created on: 7 Sept 2025
 *      Author: gardn
 */
 
#ifndef MAIN_BASE_H_
#define MAIN_BASE_H_

#include <stdio.h>
#include <inttypes.h>
#include "esp_modem_config.h"
#include "hal/uart_types.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_modem_api.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <time.h>
#include "driver/i2s_std.h"
#include "driver/i2s_types_legacy.h"

#define SERVER_IP   "31.125.133.237"
//  Was #define SERVER_PORT 3492
#define SERVER_PORT 3590
#define TOTAL_SIZE (500 * 1024)
#define CHUNK_SIZE  1024
#define BW_INITIAL_START_WAIT      (5 * 1000)          /*  5 seconds       */
#define BW_BATCH_REPEAT_INTERVAL   (10 * 60 * 1000)    /*  10 Minutes      */
#define BW_REPORTING_INTERVAL_MS   (60 * 60 * 1000)    /*  1 hour          */
#define BW_IP_EVENT_TIMEOUT        (2* 60 * 1000)      /*  2 Minutes       */
#define BW_INTERCLIP_INTERVAL      (500)               /*  500 millisec    */
#define BW_RESTART_INTERVAL        (1000)              /*  1 second        */

/*  Prototypes  */
void pulse_modem_power(void);
void init_dte();
//void init();
void esp32_init();
void on_ip_event(void *arg, esp_event_base_t event_base,int32_t event_id, void *event_data);
void on_ppp_status(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void handle_dev_state (void *pvParameters);
void reporting_task(void *pvParameters);
void blip_led();
bool measure_burst_sample(int count);
char * get_next_record(void);
int socket_send(char * filename);


struct BW_CONFIG {
    uint16_t clip_len;
    uint16_t clip_interval;
    uint16_t freq_slope;
    uint16_t vol_threshold;
    uint16_t reserved_1;
    uint16_t reserved_2;      
};

/*
 * Configure GPIO 9 as an output for the Modem Power Key
 */
//void setup_modem_power_hw(void);

/**
 * Toggles the EG915 Power state.
 * Pulse duration of 1 second is standard for most EG915 carrier boards.
 */
time_t get_network_time(const char *hostname);

typedef enum    REPORTING_STATE {
    
    reporting_state_notime,
    reporting_state_gettingtime,
    reporting_state_syncingtime,
    reporting_state_idle,
    reporting_state_sampling,
    reporting_state_pending,
    reporting_state_connecting,
    reporting_state_connected,
    reporting_state_sending,
    reporting_state_onesent,    
    reporting_state_allsent,
    reporting_state_disconnecting,
    reporting_state_disconnected,
    reporting_state_nomodem,
    
} rep_state;

/* Sampling/capture and periodic reporting run as independent tasks. The
 * reporting task connects once per hour, sends all completed clips, then
 * powers down the modem. The sampler never waits for network activity.
 */


/*  MIC pins  for ESP32  */
//#define CONFIG_I2S_LRCK_PIN 25
//#define CONFIG_I2S_DATA_PIN 27
//#define CONFIG_I2S_BCK_PIN 26

/*  MIC pins  for ESP32-S3  */
#define CONFIG_I2S_LRCK_PIN 38
#define CONFIG_I2S_DATA_PIN 3
#define CONFIG_I2S_BCK_PIN 15
#define CONFIG_BAT_MON_PIN 8
#define CONFIG_GNS_RXD 1
#define CONFIG_GNS_TXD 2


// Define the hardware pins for the EG915
//  CORRECT for wire mod:   #define MODEM_UART_TX_PIN  17
//  CORRECT for non mod:   #define MODEM_UART_TX_PIN  11

#define MODEM_UART_TX_PIN  17

#define MODEM_UART_RX_PIN  13
#define MODEM_UART_RTS_PIN 47
#define MODEM_UART_CTS_PIN 12
#define MODEM_UART_PORT    UART_NUM_1
#define TARGET_BAUD_RATE 230400 
//#define TARGET_BAUD_RATE 115200 


/*  SD Card pins for ESP32-S3 */
#define CONFIG_MGEXAMPLE_SPI_MOSI_GPIO 4
#define CONFIG_MGEXAMPLE_SPI_MISO_GPIO 5
#define CONFIG_MGEXAMPLE_SPI_SCLK_GPIO 6
#define CONFIG_MGEXAMPLE_SPI_CS_GPIO 7

#define MODEM_PWRKEY_PIN    9
#define MODEM_RESET_PIN 10
#define MIC_EN      46
#define LED_RED     48
#define LED_GREEN   16
#define LED_BLUE    18
#define GSM_LVL_OE  45

#define MIC_ON 1
#define MIC_OFF 0
#define LED_ON 0
#define LED_OFF 1 


/*  SD Card pins for ESP32 */
//#define CONFIG_MGEXAMPLE_SPI_MOSI_GPIO 23
//#define CONFIG_MGEXAMPLE_SPI_MISO_GPIO 19
//#define CONFIG_MGEXAMPLE_SPI_SCLK_GPIO 18
//#define CONFIG_MGEXAMPLE_SPI_CS_GPIO 5

#define BW_DELTA_THRESHOLD 800000      /*  Audio (32-bit) trigger threshold                    */
#define BW_DELAY_INTERVAL  1000        /*  1 Second general delay macro                        */
#define BW_RECORD_INTERVAL 100000       /*  100 seconds between recording each clip              */
#define BW_HIT_INTERVAL    300         /*  300ms pulse indicates recording took place          */
#define BW_SYNC_INTERVAL    100        /*  100ms pulse indicates time synced ok                */
#define BW_SAMPLE_INTERVAL 1000        /*  1 seconds between each test sample                  */
#define BW_DEFAULT_CLIP_LENGTH 5.0f    /*  Clip length is 5 seconds by defaul                  */

#define BW_REPORTING_START_HOUR     02  /*  The hour of day that the call-home starts (currently ignored) */       
#define BW_REPORTING_START_MINUTE   00  /*  The minute of the hour that the call-home starts    */       
#define BW_REPORTING_END_HOUR       22  /*  The hour of day that the call-home ends             */       
#define BW_REPORTING_END_MINUTE     05  /*  The minute at which the call home should expire     */

#define BW_ACTIVE_START_HOUR        02  /*  The start of all daily activity                    */       
#define BW_ACTIVE_END_HOUR          22  /*  The end of all daily activity                      */       

#define BW_REPORTING_DAY_HOUR       03  /*  The hour that dawn breaks daylight starts           */       
#define BW_REPORTING_NIGHT_HOUR     25  /*  The hour that dusk breaks daylight end              */       

//#define BW_SOCKSERVER_IP    "192.168.1.187"  31.125.133.237
#define BW_SOCKSERVER_IP    "31.125.133.237"

#define BW_TIME_PORT 37         // RFC 868 Time Protocol Port
#define BW_SECONDS_1900_TO_1970 2208988800ULL
#define BW_MAX_CONNECTION_TIMEOUT 30

#define SD_MOUNT_POINT      "/sdcard"
#define SAMPLE_BUFFER_SIZE  4096
#define HIST_BUFFER_SIZE  10
#define HIST_DEPTH        2

#define CONFIG_EXAMPLE_SAMPLE_RATE 44100
#define BYTE_RATE CONFIG_EXAMPLE_SAMPLE_RATE * 2

//#define EXAMPLE_ESP_WIFI_SSID      "SmartSentry-Guest"
//#define EXAMPLE_ESP_WIFI_PASS      "NUCLEOF411RE"
//  #define EXAMPLE_ESP_WIFI_SSID "Grasmere"
//  #define EXAMPLE_ESP_WIFI_PASS "Sh1pstonesAles"
//#define EXAMPLE_ESP_WIFI_SSID "moorhome"
//#define EXAMPLE_ESP_WIFI_SSID "NTGR_388F"
//#define EXAMPLE_ESP_WIFI_SSID "4G-MIFI-B2C9"
//#define EXAMPLE_ESP_WIFI_SSID "4G-MIFI-B03D"
//#define EXAMPLE_ESP_WIFI_SSID "4G-MIFI-B541" (D_1d14)
//#define EXAMPLE_ESP_WIFI_SSID "4G-MIFI-5C21" (D_4c30)
//#define EXAMPLE_ESP_WIFI_SSID "dlink_DWR-932_3A54"        ORIGINAL FACTORY SHIPPED
//#define EXAMPLE_ESP_WIFI_PASS "Birdwatch"  ** NOTE: Odd one may be "BirdWatch"
//#define EXAMPLE_ESP_WIFI_PASS "BirdWatch"
//#define EXAMPLE_ESP_WIFI_PASS "NEPnp39476"                ORIGINAL FACTORY SHIPPED
//#define EXAMPLE_ESP_WIFI_PASS "Paradigm01"
//#define EXAMPLE_ESP_WIFI_PASS "UA23292u"

//        Name: EE internet
//        APN: everywhere
//        Proxy: Not Required
//        Port: Not Required
//        Username: eesecure
//        Password: secure
//        Server: Not Required
//        MMSC: http://mms/
//        
//        MMS proxy: 149.254.201.135
//        MMS port: 8080
//        MCC: 234
//        MNC: 30
//        Authentication type: Not Required or use the default value shown.

/*  NOTE:       DEVICE IDENTITIES                           
 * 
 *  dev_4454        Grasmere Shed
 *  dev_8a0c        Jason's house
 *  dev_45a4        Mobile Unit (ESP32) at Nick's Farm
 *  D_4a0c          Mobile Unit (S3) at Jason's House
 *  D_c28c          Mobile Unit (S3) at Mike's House
 *  D_4c30          Mobile Unit (S3) For Jason 16 Jan
 *  D_f308          Mobile Unit (S3) TBD
 *  D_1d14          Mobile Unit (S3) For Jason 16 Jan
 *
 */

#endif /* MAIN_BASE_H_ */
