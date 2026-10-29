/*
 * init.c
 *
 *  Created on: 10 Apr 2026
 *      Author: gardn
 */

#include "base.h"
#include "esp_mac.h"
#include "esp_mac.h"
#include "esp_modem_config.h"
#include "storage.h"
#include "mgaudio.h"

//#include "esp_modem_config.h"

static const char *INIT_TAG = "MODEM_APP";
extern esp_modem_dce_t *dce;
extern esp_modem_dte_config_t dte_config;
extern char * txbuffer;
extern char * rxbuffer;
extern struct sockaddr_in dest_addr;
extern i2s_chan_handle_t rx_handle;
extern char szmac_addr[32];
static const char *MAIN_TAG = "init()";

// 
// 
// 
//void init() {
//    
//
//
//}
//

/*
 *  @brief Initialise all ESP32 hardware, driver and logging related properties
 *  required for use by this application
 * 
 *  This function is called at start-up
 *
 */
void esp32_init() {
    
  unsigned char mac[16];
  int i;

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);

  /*  Enable pull-ups for SPI lines for the SD card.  */
  gpio_set_pull_mode(CONFIG_MGEXAMPLE_SPI_MOSI_GPIO, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CONFIG_MGEXAMPLE_SPI_MISO_GPIO, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CONFIG_MGEXAMPLE_SPI_SCLK_GPIO, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(CONFIG_MGEXAMPLE_SPI_CS_GPIO, GPIO_PULLUP_ONLY);
  gpio_set_direction(MIC_EN, GPIO_MODE_OUTPUT);
  gpio_set_direction(MODEM_PWRKEY_PIN, GPIO_MODE_OUTPUT);
  gpio_set_direction(MODEM_RESET_PIN, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);
  gpio_set_direction(GSM_LVL_OE, GPIO_MODE_OUTPUT);
  
  gpio_set_level(MIC_EN, MIC_ON);
  gpio_set_level(MODEM_PWRKEY_PIN, 0);
  gpio_set_level(MODEM_RESET_PIN, 0);
  gpio_set_level(LED_RED, LED_OFF);
  gpio_set_level(LED_GREEN, LED_OFF);
  gpio_set_level(LED_BLUE, LED_OFF);
  gpio_set_level(GSM_LVL_OE, 1);
  
  /*  Start the I2S interface to enable the sound recordings  */
  i2s_start(rx_handle);
  /*  Set up the SD Card  */
  printf("MOUNT 97\r\n"); 
  mount_sdcard();
  /*  Wait a couple of seconds before starting any recordings to avoid hardware
   * contention and audible clicks */
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  /*  General info  */
  ESP_LOGI(MAIN_TAG, "[APP] Startup..");
  ESP_LOGI(MAIN_TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
  ESP_LOGI(MAIN_TAG, "[APP] IDF version: %s", esp_get_idf_version());

  /*  Set logging levels  */
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("mqtt_client", ESP_LOG_WARN);
  esp_log_level_set("mqtt_example", ESP_LOG_INFO);
  esp_log_level_set("transport_base", ESP_LOG_INFO);
  esp_log_level_set("esp-tls", ESP_LOG_INFO);
  esp_log_level_set("transport", ESP_LOG_INFO);
  esp_log_level_set("outbox", ESP_LOG_INFO);
  esp_log_level_set("wifi", ESP_LOG_NONE);

  //    TODO:  INVESTIGATE ESP_ERROR_CHECK(esp_flash_init());

  esp_efuse_mac_get_default(mac);
  memset(szmac_addr, 0, 32);
  for (i = 0; i < 6; i++) {
    sprintf(&szmac_addr[i * 2], "%02x", mac[i]);
  }
  ESP_LOGI(MAIN_TAG, "MAC Address: %s", szmac_addr);
  
  //handle_AP_config();
  
//    // Instead of simple malloc, use a capability-aware allocation
    txbuffer = heap_caps_malloc(4096, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (txbuffer == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to allocate 4KB txbuffer!");
        vTaskDelete(NULL);

    }
    
    rxbuffer = heap_caps_malloc(256, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (rxbuffer == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to allocate rxbuffer!");
        heap_caps_free(txbuffer);
        txbuffer = NULL;
        vTaskDelete(NULL);
    }
    
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);  

    /**
     * Configure GPIO 9 as an output for the Modem Power Key
     */

    gpio_config_t io_conf = {
        .pin_bit_mask = 0x0000a00000000200, // (1ULL << 9,  45, 47 */),  0000 0000 0000 0000 1010 0000 0000 0000 0000 0000 0000 0000 0010 0000 0000
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    gpio_config_t out_conf = {
        .pin_bit_mask = 0x0000000000205000, // (1ULL <<  12, 14, 21,  */),   0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0010 0000 0101 0000 0000 0000
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    
    // Ensure the pin starts in a neutral (High) state if the 
    // modem expects an active-low pulse to trigger via a logic inverter
    gpio_set_level(MODEM_PWRKEY_PIN, 0);
    gpio_set_level(GSM_LVL_OE, 1);

}


void setup_modem_power_hw(void) {
}

void init_dte() {
    
    ESP_LOGI(INIT_TAG, "Initialising DTE");
    dte_config.uart_config.port_num = MODEM_UART_PORT;
    dte_config.uart_config.tx_io_num = MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = MODEM_UART_RTS_PIN;  // Output on MCU -> level shifter ->  Input on MODEM 
    dte_config.uart_config.cts_io_num = MODEM_UART_CTS_PIN;  //  Input on MCU <-  level shifter <- Output on MODEM
    //dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_HW; UART_HW_FLOWCTRL_CTS_RTS
    dte_config.uart_config.flow_control = UART_HW_FLOWCTRL_CTS_RTS;
    dte_config.uart_config.event_queue_size = 30;
    dte_config.task_stack_size = 8192; 
    dte_config.uart_config.rx_buffer_size = 4096; 
    dte_config.uart_config.tx_buffer_size = 2048;
        
 }
 
/*
RTS (Request to Send – Output):
    Asserted (usually LOW) by a device to signal that it is ready to receive data into its internal buffer.
    Deasserted (usually HIGH) when its receive buffer is filling up and getting close to capacity, warning the peer to stop sending.
CTS (Clear to Send – Input):
    Read by the transmitting device from its own CTS pin (which is wired to the receiver's RTS pin).
    If CTS is Low, the sender is allowed to transmit data.
    If CTS is High, the sender must pause transmission until the receiver lowers its RTS line again.
*/