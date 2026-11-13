#include "base.h"
#include "esp_log.h"
#include "storage.h"
#include "mgnetwork.h"
#include "mgaudio.h"
#include "esp_task_wdt.h"
#include "ota_update.h"
#include "bw_settings.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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
static void send_status_report(int battery_mv);
esp_modem_dce_t *dce = NULL;
esp_netif_t * esp_netif = NULL;
esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
esp_err_t initialize_modem_drivers(void);

int sock = -1;
char *txbuffer;
char *rxbuffer;
int adc_val;


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
        
    ota_boot_check();

    /*  After ota_boot_check, which starts NVS: the settings last sent by the server  */
    settings_init();

    /* Global Core Subsystems Initialization (Run ONCE at boot) */
    esp32_init();

    /*  Restarts the unit if it installs new firmware from the SD card  */
    ota_install_from_sd();

    init_dte();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_status, NULL));
                                               
                                               
    //  upgrade_modem_baud_rate();
    
    vTaskDelay(pdMS_TO_TICKS(BW_INITIAL_START_WAIT));
    adc_val = read_adc_voltage_gpio8();
    ESP_LOGI(MAIN_TAG, "Battery: %d mV (pin %d mV)", adc_val * 2, adc_val);
     
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

            send_status_report(adc_val * 2);

            /*  Report to the server, take any new settings, and fetch any new firmware it
             *  offers. Sampling has not started, so a restart here cannot cut a clip short.  */
            if (ota_check_server(adc_val * 2, true) == OTA_CHECK_READY) {
                graceful_power_down();
                ESP_LOGI(MAIN_TAG, "Restarting to install the new firmware");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
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
    
    /*  A hung task must reboot the unit, as nobody is on site to restart it  */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = BW_WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wdt_config));

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

/*  Failed attempts per clip, kept across connections. A clip is deleted after
 *  BW_MAX_FILE_ATTEMPTS; if the table is full, the oldest entry is reused.  */
#define BW_MAX_FAILED_CLIPS 16
static struct {
    char name[BW_CLIP_NAME_MAX];
    int  attempts;
} s_clip_failures[BW_MAX_FAILED_CLIPS];
static int s_clip_failures_next = 0;

static int count_clip_failure(const char *name)
{
    for (int i = 0; i < BW_MAX_FAILED_CLIPS; i++) {
        if (strcmp(s_clip_failures[i].name, name) == 0) {
            return ++s_clip_failures[i].attempts;
        }
    }
    int slot = s_clip_failures_next;
    s_clip_failures_next = (s_clip_failures_next + 1) % BW_MAX_FAILED_CLIPS;
    strlcpy(s_clip_failures[slot].name, name, sizeof(s_clip_failures[slot].name));
    s_clip_failures[slot].attempts = 1;
    return 1;
}

static void forget_clip_failure(const char *name)
{
    for (int i = 0; i < BW_MAX_FAILED_CLIPS; i++) {
        if (strcmp(s_clip_failures[i].name, name) == 0) {
            s_clip_failures[i].name[0] = '\0';
            s_clip_failures[i].attempts = 0;
        }
    }
}

/*  Sends every pending clip it can on this connection. A clip the server rejects
 *  is passed over until the next connection, so it cannot hold up the others;
 *  only losing the server ends the batch early.  */
static bool send_pending_files(void)
{
    static char skip[BW_MAX_FAILED_CLIPS][BW_CLIP_NAME_MAX];   /*  Failed on this connection  */
    int skip_count = 0;
    char filename[256];
    int result;
    bool all_sent = true;

    while ((result = get_next_dir_entry_excluding(filename, skip, skip_count)) > 0) {
        esp_task_wdt_reset();
        ESP_LOGI(MAIN_TAG, "Sending pending clip: %s", filename);
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(MAIN_TAG, "Unable to create socket: errno %d", errno);
            sock = -1;
            return false;
        }

        int sent = send_file(filename);
        if (sent == BW_SEND_NO_SERVER) {
            ESP_LOGE(MAIN_TAG, "Server unavailable; leaving %s pending", filename);
            return false;
        }
        if (sent == BW_SEND_FILE_FAILED) {
            int attempts = count_clip_failure(filename);
            if (attempts >= BW_MAX_FILE_ATTEMPTS) {
                ESP_LOGE(MAIN_TAG, "Deleting %s after %d failed attempts", filename, attempts);
                forget_clip_failure(filename);
                if (delete_clip(filename) != 0) {
                    return false;
                }
                continue;
            }
            all_sent = false;
            if (strlen(filename) >= BW_CLIP_NAME_MAX || skip_count >= BW_MAX_FAILED_CLIPS) {
                /*  Cannot be passed over, so stop rather than retry it in a loop  */
                ESP_LOGE(MAIN_TAG, "Transfer of %s failed (attempt %d/%d); stopping this batch",
                         filename, attempts, BW_MAX_FILE_ATTEMPTS);
                return false;
            }
            ESP_LOGE(MAIN_TAG, "Transfer of %s failed (attempt %d/%d); carrying on with the others",
                     filename, attempts, BW_MAX_FILE_ATTEMPTS);
            strlcpy(skip[skip_count++], filename, BW_CLIP_NAME_MAX);
            continue;
        }
        forget_clip_failure(filename);
        if (archive_clip(filename) != 0) {
            /*  The server already has this clip, so only the local copy is lost  */
            ESP_LOGE(MAIN_TAG, "Failed to archive %s; deleting it", filename);
            if (delete_clip(filename) != 0) {
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BW_INTERCLIP_INTERVAL));
    }

    if (result < 0) {
        ESP_LOGE(MAIN_TAG, "Failed to scan the recordings directory");
        return false;
    }
    if (skip_count > 0) {
        ESP_LOGW(MAIN_TAG, "All other clips sent; %d failed and will be retried next time", skip_count);
    } else {
        ESP_LOGI(MAIN_TAG, "No more pending clips");
    }
    return all_sent;
}


static bool sync_network_time(void)
{
    time_t network_time = get_network_time("time.nist.gov");
    if (network_time == 0) {
        ESP_LOGW(MAIN_TAG, "Network time synchronization failed");
        return false;
    }

    /*  The modem, PPP and DNS all worked, so new firmware has proved it can connect  */
    ota_mark_valid("network time received");

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

/*  Set by the reporting task when a downloaded update is ready. The sampler
 *  restarts the unit between samples, so a clip being saved is never cut short.  */
static volatile bool s_restart_for_update = false;

void reporting_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t next_report = xTaskGetTickCount();
    const TickType_t feed_interval = pdMS_TO_TICKS(BW_WDT_FEED_INTERVAL_MS);
    /*  The startup connection has just checked for firmware  */
    TickType_t last_fw_check = xTaskGetTickCount();

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        /*  Both intervals come from the server's settings, read each cycle so a change
         *  applies at the next wait. Worked in seconds, not with pdMS_TO_TICKS, which
         *  overflows 32 bits above about 11.9 hours (24 hours came out as 8 minutes).  */
        const TickType_t report_interval = (TickType_t) g_settings.reporting_interval_s * configTICK_RATE_HZ;
        const TickType_t fw_check_interval = (TickType_t) g_settings.fw_check_interval_h * 3600 * configTICK_RATE_HZ;

        /*  Count the interval from the end of the last cycle. A fixed timetable would
         *  start the next connection at once whenever a long upload overran the
         *  interval, which costs a whole reconnection for nothing.  */
        next_report = xTaskGetTickCount();

        /*  The report interval can exceed the watchdog timeout, so wait in slices  */
        for (TickType_t remaining = report_interval; remaining > 0; ) {
            TickType_t wait = (remaining < feed_interval) ? remaining : feed_interval;
            vTaskDelayUntil(&next_report, wait);
            esp_task_wdt_reset();
            remaining -= wait;
        }

        if (s_restart_for_update) {
            continue;   /*  Waiting for the sampler to restart the unit  */
        }

        char filename[256];
        int pending = get_next_dir_entry(filename);
        if (pending < 0) {
            ESP_LOGE(MAIN_TAG, "Unable to inspect recordings directory");
            continue;
        }
        bool fw_check_due = (xTaskGetTickCount() - last_fw_check) >= fw_check_interval;
        if (pending == 0 && !fw_check_due) {
            ESP_LOGI(MAIN_TAG, "Hourly check: no recordings to send");
            continue;
        }

        if (pending > 0) {
            ESP_LOGI(MAIN_TAG, "Hourly check: pending recordings found");
        } else {
            ESP_LOGI(MAIN_TAG, "Hourly check: no recordings, but a firmware check is due");
        }
        if (fw_check_due) {
            /*  Count the attempt, so a site with no signal does not retry every cycle  */
            last_fw_check = xTaskGetTickCount();
        }

        /*  Read the battery before the modem loads it  */
        int battery_mv = read_adc_voltage_gpio8() * 2;
        if (ensure_modem_on() != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Modem is not ready; will retry at the next hourly check");
            graceful_power_down();
            continue;
        }
        esp_task_wdt_reset();

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

        esp_task_wdt_reset();

        sync_network_time();
        send_status_report(battery_mv);
        esp_task_wdt_reset();
        /*  Settings before clips, so a long backlog never delays them; a firmware
         *  download waits until after the clips, so an update never delays those  */
        ota_check_t check = OTA_CHECK_WANTED;
        if (pending > 0) {
            check = ota_check_server(battery_mv, false);
            esp_task_wdt_reset();
            ESP_LOGI(MAIN_TAG, "Connected; sending pending recordings");
            if (!send_pending_files()) {
                ESP_LOGW(MAIN_TAG, "Some recordings remain pending until the next hourly check");
            }
        }
        esp_task_wdt_reset();

        /*  Back for the download only if one is worth having (or the first check failed)  */
        if (check != OTA_CHECK_NOTHING) {
            check = ota_check_server(battery_mv, true);
        }
        bool update_ready = (check == OTA_CHECK_READY);
        last_fw_check = xTaskGetTickCount();
        esp_task_wdt_reset();
        graceful_power_down();

        if (update_ready) {
            ESP_LOGI(MAIN_TAG, "New firmware is ready; restarting between samples");
            s_restart_for_update = true;
        }
    }
}

void handle_dev_state(void *pvParameters)
{
    (void)pvParameters;
    int tick_count = 0;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    
    bool was_active = true;

    for (;;) {
        esp_task_wdt_reset();
        if (s_restart_for_update) {
            ESP_LOGI(MAIN_TAG, "Restarting to install the new firmware");
            vTaskDelay(pdMS_TO_TICKS(500));   /*  Let the log drain  */
            esp_restart();
        }

        /*  Outside the recording window nothing is sampled; reporting carries on  */
        bool active = settings_recording_active();
        if (active != was_active) {
            ESP_LOGI(MAIN_TAG, "%s the recording window (%02u:00 to %02u:00 GMT)",
                     active ? "Entering" : "Leaving",
                     (unsigned) g_settings.active_start_hour, (unsigned) g_settings.active_end_hour);
            was_active = active;
        }
        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(BW_WDT_FEED_INTERVAL_MS / 2));
            continue;
        }

        bool hit = measure_burst_sample(tick_count++);
        if (hit) {
            gpio_set_level(LED_GREEN, LED_ON);
            if (make_clip_filename()) {
                build_clip16(rx_handle, filespec);
            } else {
                ESP_LOGE(MAIN_TAG, "Unable to create timestamped filename; clip not recorded");
            }
            gpio_set_level(LED_GREEN, LED_OFF);
            /*  Up to 600 s, which is under the 10-minute watchdog, so one wait is safe  */
            vTaskDelay((TickType_t) g_settings.record_holdoff_s * configTICK_RATE_HZ);
        } else {
            vTaskDelay(pdMS_TO_TICKS(g_settings.sample_interval_ms));
        }
    }
}


/*  Read a burst of HIST_DEPTH samples from the i2s subsystem to check for consistent high frequency audio like bird sound  */
bool measure_burst_sample(int count) {
  (void) count;
  /*  One block passing the bird test is enough, because a call can end partway through a burst  */
  return get_max_slope(rx_handle) >= 1;
}

static const char *reset_reason_text(esp_reset_reason_t reason)
{
    static const char *names[] = {
        "unknown", "power-on", "reset pin", "software restart", "crash", "interrupt watchdog",
        "task watchdog", "watchdog", "deep sleep wake", "brownout", "SDIO", "USB", "JTAG",
        "eFuse error", "power glitch", "CPU lockup"
    };
    return (unsigned) reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "unknown";
}

/*  Writes a status report and uploads it like a clip, as "S<id><yydddhhmmss>.txt"
 *  (the server keeps these in ~/bin/reports), then deletes the local copy: the next
 *  connection makes a fresh one. Call while PPP is up, after the time is set.     */
static void send_status_report(int battery_mv)
{
    static char report[3072];       /*  Static: the startup task's stack is small  */
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const bw_network_info_t *net = network_info();
    char rolled_back[32];
    ota_rolled_back_version(rolled_back, sizeof(rolled_back));

    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    char name[80], when[32];        /*  name: room for any int, checked for 22 bytes below  */
    snprintf(name, sizeof(name), "S%s%02d%03d%02d%02d%02d.txt", &szmac_addr[6], tm_now.tm_year - 100,
             tm_now.tm_yday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S GMT", &tm_now);
    if (strlen(name) != BW_CLIP_NAME_LEN) {
        ESP_LOGE(MAIN_TAG, "Status report name %s is not %d bytes; not sent", name, BW_CLIP_NAME_LEN);
        return;
    }

    int len = snprintf(report, sizeof(report),
        "# Bird Watch status report\n"
        "time=%s\n"
        "unit=%s\n"
        "version=%s\n"
        "partition=%s\n"
        "on_trial=%d\n"
        "rolled_back=%s\n"
        "last_reset=%s\n"
        "uptime_s=%lld\n"
        "battery_mv=%d\n"
        "battery_under_load_mv=%d\n"
        "free_heap=%lu\n"
        "\n# Network at this connection\n"
        "network=%s\n"
        "network_code=%s\n"
        "radio=%s\n"
        "signal_csq=%d\n"
        "serving_cell=%s\n",
        when, &szmac_addr[6], app->version, running ? running->label : "?", ota_on_trial() ? 1 : 0,
        rolled_back, reset_reason_text(esp_reset_reason()), (long long) (xTaskGetTickCount() / configTICK_RATE_HZ),
        battery_mv, battery_load_min_mv(), (unsigned long) esp_get_free_heap_size(),
        net->operator_name, net->network_code, net->radio, net->csq, net->serving_cell);

    /*  The serving cell, decoded. cell_id and tac in hex as the modem gives them, and
     *  in decimal as cell databases (e.g. OpenCellID) list them.  */
    const bw_cell_t *c = &net->cell;
    if (len > 0 && len < (int) sizeof(report) && c->valid) {
        len += snprintf(report + len, sizeof(report) - (size_t) len,
            "cell_mcc=%d\n"
            "cell_mnc=%d\n"
            "cell_id=%lX (%lu; mast %lu, sector %lu)\n"
            "cell_tac=%lX (%lu)\n"
            "cell_pci=%d\n"
            "cell_earfcn=%d\n"
            "cell_band=%d\n"
            "cell_rsrp_dbm=%d\n"
            "cell_rsrq_db=%d\n"
            "cell_rssi_dbm=%d\n"
            "cell_sinr_db=%d\n",
            c->mcc, c->mnc, (unsigned long) c->cell_id, (unsigned long) c->cell_id,
            (unsigned long) (c->cell_id >> 8), (unsigned long) (c->cell_id & 0xff),
            (unsigned long) c->tac, (unsigned long) c->tac, c->pci, c->earfcn, c->band,
            c->rsrp, c->rsrq, c->rssi, c->sinr);
    }

    /*  Neighbour cells, raw (on trial), one line each  */
    if (len > 0 && len < (int) sizeof(report)) {
        len += snprintf(report + len, sizeof(report) - (size_t) len, "neighbour_count=%d\n", net->neighbour_count);
    }
    const char *line = net->neighbours;
    while (*line && len > 0 && len < (int) sizeof(report)) {
        const char *end = strchr(line, '\n');
        int line_len = end ? (int) (end - line) : (int) strlen(line);
        len += snprintf(report + len, sizeof(report) - (size_t) len, "neighbour=%.*s\n", line_len, line);
        line = end ? end + 1 : line + line_len;
    }

    if (len > 0 && len < (int) sizeof(report)) {
        len += snprintf(report + len, sizeof(report) - (size_t) len, "\n# Settings (config CRC %08lx, %u refused)\n",
                        (unsigned long) settings_crc(), (unsigned) settings_rejected());
    }
    if (len < 0 || len >= (int) sizeof(report)) {
        ESP_LOGE(MAIN_TAG, "Status report too long; not sent");
        return;
    }
    len += (int) settings_format(report + len, sizeof(report) - (size_t) len);

    if (write_text_file(name, report, (size_t) len) != 0) {
        return;
    }
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(MAIN_TAG, "Unable to create socket: errno %d", errno);
        sock = -1;
    } else if (send_file(name) == BW_SEND_OK) {
        ESP_LOGI(MAIN_TAG, "Status report %s sent (%d bytes)", name, len);
    } else {
        ESP_LOGW(MAIN_TAG, "Status report %s not sent; the next connection sends a new one", name);
    }
    delete_clip(name);
}

static bool make_clip_filename(void)
{
    time_t capture_time = time(NULL);
    struct tm capture_timeinfo;

    if (localtime_r(&capture_time, &capture_timeinfo) == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to convert clip capture time");
        return false;
    }

    int length = snprintf(filespec, sizeof(filespec), "D%s%02d%03d%02d%02d%02d",
                          &szmac_addr[6], capture_timeinfo.tm_year - 100,
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
