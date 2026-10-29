#include "base.h"
#include "esp_log.h"
#include "storage.h"
#include "mgnetwork.h"
#include "mgaudio.h"
#include <time.h>

SemaphoreHandle_t connection_sema = NULL; 

/*  Operations  */
i2s_chan_handle_t rx_handle;
char szmac_addr[32];
static char filespec[64];
rep_state reporting_state;
EventBits_t evt_flags;

static const char *MAIN_TAG = "MODEM_APP";
static bool sync_network_time(void);
static bool make_clip_filename(void);
esp_modem_dce_t *dce = NULL;
esp_netif_t * esp_netif = NULL;
esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
esp_err_t initialize_modem_drivers(void);

int sock = -1;
char *txbuffer;
char *rxbuffer;
int adc_val;
struct BW_CONFIG bw_config;


//extern EventGroupHandle_t s_wifi_event_group;
extern int32_t * p_i2s_buff;

/*  Setup the parameters  */
struct sockaddr_in dest_addr;

void app_main(void)
{
    connection_sema = xSemaphoreCreateBinary();
    if (connection_sema == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to create modem connection semaphore");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    
    /* Global Core Subsystems Initialization (Run ONCE at boot) */
    esp32_init();
    init_dte();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_status, NULL));
                                               
    adc_val = read_adc_voltage_gpio8();
    ESP_LOGI(MAIN_TAG, "ADC Value is:  %04x", adc_val);
                                               
    //  upgrade_modem_baud_rate();
    
    vTaskDelay(pdMS_TO_TICKS(BW_INITIAL_START_WAIT));

    reporting_state = reporting_state_notime;
    ESP_LOGI(MAIN_TAG, "Synchronizing system time before sampling starts");
    if (ensure_modem_on() == ESP_OK) {
        while (xSemaphoreTake(connection_sema, 0) == pdTRUE) {
            /* Discard an IP notification left over from an earlier connection. */
        }

        esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
        if (err == ESP_OK &&
            xSemaphoreTake(connection_sema, pdMS_TO_TICKS(BW_IP_EVENT_TIMEOUT)) == pdTRUE) {
            if (!sync_network_time()) {
                ESP_LOGW(MAIN_TAG, "Startup network time synchronization failed");
            }
        } else if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start PPP for startup time synchronization: %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(MAIN_TAG, "Timed out waiting for PPP IP address during startup");
        }
        graceful_power_down();
    } else {
        ESP_LOGW(MAIN_TAG, "Unable to start modem for startup time synchronization");
        graceful_power_down();
    }

    if (xTaskCreate(handle_dev_state, "sampler", 8192, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(MAIN_TAG, "Failed to create sampling task");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    if (xTaskCreate(reporting_task, "reporter", 12288, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(MAIN_TAG, "Failed to create reporting task");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    vTaskDelete(NULL);
}

static bool send_pending_files(void)
{
    char filename[256];
    int result;

    while ((result = get_next_dir_entry(filename)) > 0) {
        ESP_LOGI(MAIN_TAG, "Sending pending clip: %s", filename);
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(MAIN_TAG, "Unable to create socket: errno %d", errno);
            sock = -1;
            return false;
        }

        if (socket_send(filename) != 0) {
            ESP_LOGE(MAIN_TAG, "Transfer failed; leaving %s pending", filename);
            return false;
        }
        if (archive_clip(filename) != 0) {
            ESP_LOGE(MAIN_TAG, "Failed to archive transferred file %s", filename);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(BW_INTERCLIP_INTERVAL));
    }

    if (result < 0) {
        ESP_LOGE(MAIN_TAG, "Failed to scan the recordings directory");
        return false;
    }
    ESP_LOGI(MAIN_TAG, "No more pending clips");
    return true;
}

static bool sync_network_time(void)
{
    time_t network_time = get_network_time("time.nist.gov");
    if (network_time == 0) {
        ESP_LOGW(MAIN_TAG, "Network time synchronization failed");
        return false;
    }

    struct timeval tv = {.tv_sec = network_time, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(MAIN_TAG, "Failed to set system time from network");
        return false;
    }
    
    char       buf[80]; 
    struct tm ts;
    if (localtime_r(&network_time, &ts) == NULL) {
        /*  The clock is already set; only the log message is lost  */
        ESP_LOGW(MAIN_TAG, "System time synchronized, but could not format it");
        return true;
    }
    strftime(buf, sizeof(buf), "%a %Y-%m-%d %H:%M:%S %Z", &ts);
    ESP_LOGI(MAIN_TAG, "System time synchronized from network:  %s", buf);
    return true;
}

/*  Test file D_0f44_26297141832 captured during file transfer  */

void reporting_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t next_report = xTaskGetTickCount();
    const TickType_t report_interval = pdMS_TO_TICKS(BW_REPORTING_INTERVAL_MS);

    for (;;) {
        vTaskDelayUntil(&next_report, report_interval);

        char filename[256];
        int pending = get_next_dir_entry(filename);
        if (pending < 0) {
            ESP_LOGE(MAIN_TAG, "Unable to inspect recordings directory");
            continue;
        }
        if (pending == 0) {
            ESP_LOGI(MAIN_TAG, "Hourly check: no recordings to send");
            continue;
        }

        ESP_LOGI(MAIN_TAG, "Hourly check: pending recordings found");
        if (ensure_modem_on() != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Modem is not ready; will retry at the next hourly check");
            graceful_power_down();
            continue;
        }

        /*  Wait here until connection is signalled  */
        while (connection_sema != NULL &&
               xSemaphoreTake(connection_sema, 0) == pdTRUE) {
        }

        esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
        if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start PPP: %s", esp_err_to_name(err));
            graceful_power_down();
            continue;
        }

        if (connection_sema == NULL ||
            xSemaphoreTake(connection_sema, pdMS_TO_TICKS(BW_IP_EVENT_TIMEOUT)) != pdTRUE) {
            ESP_LOGE(MAIN_TAG, "Timed out waiting for PPP IP address");
            graceful_power_down();
            continue;
        }

        ESP_LOGI(MAIN_TAG, "Connected; sending pending recordings");
        sync_network_time();
        if (!send_pending_files()) {
            ESP_LOGW(MAIN_TAG, "Some recordings remain pending until the next hourly check");
        }
        graceful_power_down();
    }
}

void handle_dev_state(void *pvParameters)
{
    (void)pvParameters;
    int tick_count = 0;

    for (;;) {
        bool hit = measure_burst_sample(tick_count++);
        if (hit) {
            gpio_set_level(LED_GREEN, LED_ON);
            if (make_clip_filename()) {
                build_clip16(rx_handle, filespec);
            } else {
                ESP_LOGE(MAIN_TAG, "Unable to create timestamped filename; clip not recorded");
            }
            gpio_set_level(LED_GREEN, LED_OFF);
            //  ESP_LOGI(MAIN_TAG, "Clip captured; sampling resumes in %3.1f seconds", (float)BW_RECORD_INTERVAL / 1000.0f);
            vTaskDelay(pdMS_TO_TICKS(BW_RECORD_INTERVAL));
        } else {
            time_t now = time(NULL);
            struct tm timeinfo;
            if (localtime_r(&now, &timeinfo) == NULL) {
                ESP_LOGE(MAIN_TAG, "Failed to convert current time for sampler log");
                vTaskDelay(pdMS_TO_TICKS(BW_SAMPLE_INTERVAL));
                continue;
            }
            //  ESP_LOGI(MAIN_TAG, "(%02d:%02d) No hit; sampling again in %3.1f seconds", timeinfo.tm_hour, timeinfo.tm_min, (float)BW_SAMPLE_INTERVAL / 1000.0f);
            vTaskDelay(pdMS_TO_TICKS(BW_SAMPLE_INTERVAL));
        }
    }
}


/*  Read a burst of HIST_DEPTH samples from the i2s subsystem to check for consistent high frequency audio like bird sound  */
bool measure_burst_sample(int count) {
  bool hit = false;
  uint32_t accum_delta = 0;

  /*  Determine the largest jump during this audio packet  */
  accum_delta = get_max_slope(rx_handle);
  
  /*  accum_delta is the summed max slope of HIST_DEPTH buffers in units of BW_DELTA_THRESHOLD,
      so record a clip when the average max slope per buffer reaches the threshold  */
  if (accum_delta >= HIST_DEPTH) {
    hit = true;
  }
  //ESP_LOGI(MAIN_TAG, "Sample:  %d.  Acc Delta:  %d,  Hit: %d", count, (int)accum_delta, hit);

  return (hit);
}

static bool make_clip_filename(void)
{
    time_t capture_time = time(NULL);
    struct tm capture_timeinfo;

    if (localtime_r(&capture_time, &capture_timeinfo) == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to convert clip capture time");
        return false;
    }

    int length = snprintf(filespec, sizeof(filespec), "D_%s_%02d%03d%02d%02d%02d",
                          &szmac_addr[8], capture_timeinfo.tm_year - 100,
                          capture_timeinfo.tm_yday, capture_timeinfo.tm_hour,
                          capture_timeinfo.tm_min, capture_timeinfo.tm_sec);
    if (length < 0 || (size_t)length >= sizeof(filespec)) {
        ESP_LOGE(MAIN_TAG, "Timestamped clip filename exceeds buffer capacity");
        return false;
    }
    return true;
}


esp_err_t initialize_modem_drivers(void) {
    
    ESP_LOGI(MAIN_TAG, "Initializing modem software stack...");

    // 1. Guard against double-initialization leaks
    if (esp_netif != NULL || dce != NULL) {
        ESP_LOGW(MAIN_TAG, "Drivers already initialized. Destroying old handles first.");
        graceful_power_down();
    }

    // 2. Configure & Create the PPP Netif
    ESP_LOGI(MAIN_TAG, "Creating the PPP Net interface");
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif = esp_netif_new(&netif_ppp_config);
    if (esp_netif == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to create esp_netif instance");
        return ESP_FAIL;
    }

    // 3. Setup DCE configurations (Using your target APN)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("eapn1.net");

    // 4. Create the modern C++ DCE driver handle
    ESP_LOGI(MAIN_TAG, "Creating the dce");
    dce = esp_modem_new_dev(ESP_MODEM_DCE_BG96, &dte_config, &dce_config, esp_netif);
    if (dce == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to create esp_modem device handle");
        esp_netif_destroy(esp_netif);
        esp_netif = NULL;
        return ESP_FAIL;
    }

    /* Configuration sends AT commands, so defer it until ensure_modem_on has
       recovered command mode and verified the modem is responding. */
    ESP_LOGI(MAIN_TAG, "Modem software stack initialized.");
    return ESP_OK;
}
