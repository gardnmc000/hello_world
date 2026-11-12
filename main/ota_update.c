/*
 * ota_update.c
 *
 *  Phase 1 of the update plan: install firmware from the SD card at boot.
 *
 *  New firmware starts "on trial". If the unit reboots for any reason before
 *  ota_mark_valid() is called, the bootloader goes back to the previous firmware.
 *  It is marked valid when it first reaches the network, or after 1 hour of
 *  running without a crash, whichever comes first.
 *
 *  A rollback caused by a crash or watchdog blocks that release for good. One
 *  caused by a power problem (brownout, power cut, reset pin) only means the
 *  release never got a fair trial, so it is installed again, up to
 *  BW_OTA_MAX_POWER_ROLLBACKS times in all. The record of this is kept in NVS.
 */

#include "base.h"
#include "ota_update.h"
#include "storage.h"
#include "bw_settings.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <sys/stat.h>
#include <errno.h>

#define OTA_CHUNK_SIZE 4096

/*  NVS keys, in namespace "ota"  */
#define NVS_NS          "ota"
#define KEY_TRIAL_VER   "trial_ver"     /*  Version installed and not yet proved      */
#define KEY_RB_VER      "rb_ver"        /*  Version of the last rolled-back release    */
#define KEY_RB_POWER    "rb_power"      /*  Its rollbacks caused by power problems     */
#define KEY_RB_HARD     "rb_hard"       /*  1 if it was ever rolled back by a crash    */

static const char *OTA_TAG = "OTA";
static bool s_nvs_ok = false;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:                return "unknown";
    }
}

/*  True if the reset says nothing about the firmware itself  */
static bool reset_was_power_related(esp_reset_reason_t reason)
{
    return reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT ||
           reason == ESP_RST_PWR_GLITCH || reason == ESP_RST_EXT || reason == ESP_RST_USB;
}

static void nvs_get_string(nvs_handle_t h, const char *key, char *out, size_t size)
{
    out[0] = '\0';
    if (nvs_get_str(h, key, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
}

/*  Records that a version is now on trial, or clears the record when version is NULL  */
static void set_trial_version(const char *version)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(OTA_TAG, "Unable to open NVS; the rollback cause will not be recorded");
        return;
    }
    if (version != NULL) {
        nvs_set_str(h, KEY_TRIAL_VER, version);
    } else {
        nvs_erase_key(h, KEY_TRIAL_VER);
    }
    nvs_commit(h);
    nvs_close(h);
}

/*  First boot after a rollback: work out why the trial ended, and count it  */
static void record_rollback(const char *running_version)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    char trial[32];
    nvs_get_string(h, KEY_TRIAL_VER, trial, sizeof(trial));
    if (trial[0] == '\0' || strcmp(trial, running_version) == 0) {
        nvs_close(h);   /*  No trial, or the trial is still running  */
        return;
    }

    /*  Only a real rollback leaves the trial's partition marked invalid; otherwise
     *  something else replaced it (e.g. a USB flash), and there is nothing to count  */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    if (invalid == NULL || esp_ota_get_partition_description(invalid, &invalid_desc) != ESP_OK ||
        strcmp(invalid_desc.version, trial) != 0) {
        nvs_erase_key(h, KEY_TRIAL_VER);
        nvs_commit(h);
        nvs_close(h);
        return;
    }

    char rb_ver[32];
    uint8_t rb_power = 0, rb_hard = 0;
    nvs_get_string(h, KEY_RB_VER, rb_ver, sizeof(rb_ver));
    if (strcmp(rb_ver, trial) == 0) {
        nvs_get_u8(h, KEY_RB_POWER, &rb_power);
        nvs_get_u8(h, KEY_RB_HARD, &rb_hard);
    }

    /*  The bootloader does not reset again when it rolls back, so this is the reset that ended the trial  */
    esp_reset_reason_t reason = esp_reset_reason();
    if (reset_was_power_related(reason)) {
        rb_power++;
        if (rb_power < BW_OTA_MAX_POWER_ROLLBACKS) {
            ESP_LOGW(OTA_TAG, "Firmware %s was rolled back after a %s (%u of %d); it will be installed again",
                     trial, reset_reason_name(reason), rb_power, BW_OTA_MAX_POWER_ROLLBACKS);
        } else {
            ESP_LOGE(OTA_TAG, "Firmware %s was rolled back after a %s (%u of %d); it will not be installed again",
                     trial, reset_reason_name(reason), rb_power, BW_OTA_MAX_POWER_ROLLBACKS);
        }
    } else {
        rb_hard = 1;
        ESP_LOGE(OTA_TAG, "Firmware %s was rolled back after a %s; it will not be installed again",
                 trial, reset_reason_name(reason));
    }

    nvs_set_str(h, KEY_RB_VER, trial);
    nvs_set_u8(h, KEY_RB_POWER, rb_power);
    nvs_set_u8(h, KEY_RB_HARD, rb_hard);
    nvs_erase_key(h, KEY_TRIAL_VER);
    nvs_commit(h);
    nvs_close(h);
}

bool ota_version_blocked(const char *version)
{
    nvs_handle_t h;
    if (s_nvs_ok && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char rb_ver[32];
        uint8_t rb_power = 0, rb_hard = 0;
        nvs_get_string(h, KEY_RB_VER, rb_ver, sizeof(rb_ver));
        nvs_get_u8(h, KEY_RB_POWER, &rb_power);
        nvs_get_u8(h, KEY_RB_HARD, &rb_hard);
        nvs_close(h);
        if (strcmp(rb_ver, version) == 0) {
            return rb_hard || rb_power >= BW_OTA_MAX_POWER_ROLLBACKS;
        }
    }

    /*  No record (for example, rolled back by older firmware): the cause is unknown, so block it  */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    return invalid != NULL && esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK &&
           strcmp(version, invalid_desc.version) == 0;
}

/*  Renames a firmware file, replacing any earlier file of the same name  */
static void rename_firmware_file(const char *new_name)
{
    unlink(new_name);
    if (rename(BW_OTA_FILE, new_name) != 0) {
        ESP_LOGE(OTA_TAG, "Unable to rename %s to %s: errno %d", BW_OTA_FILE, new_name, errno);
    }
}

/*  Reads the version from the app description near the start of the image  */
static bool read_image_version(FILE *f, esp_app_desc_t *desc)
{
    long offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (fseek(f, offset, SEEK_SET) != 0 ||
        fread(desc, 1, sizeof(*desc), f) != sizeof(*desc) ||
        desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return false;
    }
    desc->version[sizeof(desc->version) - 1] = '\0';
    return fseek(f, 0, SEEK_SET) == 0;
}

void ota_rolled_back_version(char *out, size_t size)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (s_nvs_ok && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_string(h, KEY_RB_VER, out, size);
        nvs_close(h);
    }
    if (out[0] == '\0') {
        const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
        esp_app_desc_t invalid_desc;
        if (invalid != NULL && esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK) {
            strlcpy(out, invalid_desc.version, size);
        }
    }
}

bool ota_on_trial(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void validation_timer_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(BW_OTA_VALID_AFTER_MS));
    ota_mark_valid("1 hour without a crash");
    vTaskDelete(NULL);
}

void ota_boot_check(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(OTA_TAG, "Running firmware %s from partition %s", app->version, running->label);
    if (reason == ESP_RST_BROWNOUT) {
        ESP_LOGW(OTA_TAG, "Last reset: brownout (the supply voltage dropped; check the battery)");
    } else {
        ESP_LOGI(OTA_TAG, "Last reset: %s", reset_reason_name(reason));
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /*  Only the OTA record is kept in NVS, so starting afresh loses little  */
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_nvs_ok = (err == ESP_OK);
    if (!s_nvs_ok) {
        ESP_LOGE(OTA_TAG, "NVS unavailable (%s); rollback causes will not be recorded", esp_err_to_name(err));
    }
    record_rollback(app->version);

    /*  A rollback leaves the failed firmware's partition marked invalid  */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    if (invalid != NULL && esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK) {
        ESP_LOGW(OTA_TAG, "Firmware %s in partition %s was rolled back%s", invalid_desc.version, invalid->label,
                 ota_version_blocked(invalid_desc.version) ? "" : " (a retry is allowed)");
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(OTA_TAG, "Firmware is on trial; it is marked valid on network contact or after 1 hour");
        if (xTaskCreate(validation_timer_task, "ota_valid", 3072, NULL, 1, NULL) != pdPASS) {
            /*  Without the timer, only network contact can mark it valid  */
            ESP_LOGE(OTA_TAG, "Failed to create the validation timer task");
        }
    }
}

void ota_mark_valid(const char *reason)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(OTA_TAG, "Firmware marked valid (%s)", reason);
        set_trial_version(NULL);
    } else {
        ESP_LOGE(OTA_TAG, "Failed to mark firmware valid: %s", esp_err_to_name(err));
    }
}

/*  After a power-related rollback, the installed file (renamed to fw_inst.bin) is
 *  put back as firmware.bin, so the release gets another trial.  */
static void restore_for_retry(void)
{
    struct stat st;
    if (stat(BW_OTA_FILE, &st) == 0 || stat(BW_OTA_DONE_FILE, &st) != 0) {
        return;
    }
    FILE *f = fopen(BW_OTA_DONE_FILE, "rb");
    if (f == NULL) {
        return;
    }
    esp_app_desc_t desc;
    bool ok = read_image_version(f, &desc);
    fclose(f);

    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    if (ok && strcmp(desc.version, esp_app_get_description()->version) != 0 &&
        invalid != NULL && esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK &&
        strcmp(desc.version, invalid_desc.version) == 0 && !ota_version_blocked(desc.version)) {
        ESP_LOGI(OTA_TAG, "Retrying firmware %s after a power-related rollback", desc.version);
        if (rename(BW_OTA_DONE_FILE, BW_OTA_FILE) != 0) {
            ESP_LOGE(OTA_TAG, "Unable to restore %s for the retry: errno %d", BW_OTA_FILE, errno);
        }
    }
}

void ota_install_from_sd(void)
{
    restore_for_retry();

    struct stat st;
    if (stat(BW_OTA_FILE, &st) != 0) {
        return;   /*  No update waiting, or no SD card  */
    }

    FILE *f = fopen(BW_OTA_FILE, "rb");
    if (f == NULL) {
        ESP_LOGE(OTA_TAG, "Unable to open %s: errno %d", BW_OTA_FILE, errno);
        return;
    }

    esp_app_desc_t new_desc;
    if (!read_image_version(f, &new_desc)) {
        ESP_LOGE(OTA_TAG, "%s is not a valid firmware image", BW_OTA_FILE);
        fclose(f);
        rename_firmware_file(BW_OTA_BAD_FILE);
        return;
    }

    const esp_app_desc_t *running_desc = esp_app_get_description();
    ESP_LOGI(OTA_TAG, "Found firmware %s on the SD card (running %s)", new_desc.version, running_desc->version);

    /*  Already installed: the rename after the last install did not happen  */
    if (strcmp(new_desc.version, running_desc->version) == 0) {
        ESP_LOGI(OTA_TAG, "Firmware %s is already running", new_desc.version);
        fclose(f);
        rename_firmware_file(BW_OTA_DONE_FILE);
        return;
    }

    /*  The same firmware failed its trial before (a crash, or too many power failures)  */
    if (ota_version_blocked(new_desc.version)) {
        ESP_LOGW(OTA_TAG, "Firmware %s was rolled back before; not installing it again", new_desc.version);
        fclose(f);
        rename_firmware_file(BW_OTA_BAD_FILE);
        return;
    }

    /*  Do not replace firmware that has not proved itself yet  */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(OTA_TAG, "Running firmware is still on trial; install postponed");
        fclose(f);
        return;
    }

    /*  A power failure part-way through would be survived, but a flat battery
     *  would only lose the trial; wait for a charge. An ADC error does not block it.  */
    int battery_mv = read_adc_voltage_gpio8() * 2;
    int min_mv = (int) g_settings.ota_min_battery_mv;
    if (battery_mv > 0 && battery_mv < min_mv) {
        ESP_LOGW(OTA_TAG, "Battery %d mV is below %d mV; install postponed", battery_mv, min_mv);
        fclose(f);
        return;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || st.st_size <= 0 || (size_t)st.st_size > target->size) {
        ESP_LOGE(OTA_TAG, "No partition fits the %ld byte image", (long)st.st_size);
        fclose(f);
        rename_firmware_file(BW_OTA_BAD_FILE);
        return;
    }

    uint8_t *chunk = malloc(OTA_CHUNK_SIZE);
    if (chunk == NULL) {
        ESP_LOGE(OTA_TAG, "Out of memory for the install buffer");
        fclose(f);
        return;
    }

    ESP_LOGI(OTA_TAG, "Installing firmware %s to partition %s", new_desc.version, target->label);
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(chunk);
        fclose(f);
        return;
    }

    size_t total = 0;
    size_t n;
    while ((n = fread(chunk, 1, OTA_CHUNK_SIZE, f)) > 0) {
        err = esp_ota_write(handle, chunk, n);
        if (err != ESP_OK) {
            break;
        }
        total += n;
        vTaskDelay(1);   /*  Let the idle tasks run, so the default watchdog stays fed  */
    }
    bool read_error = ferror(f);
    free(chunk);
    fclose(f);

    if (err != ESP_OK || read_error || total != (size_t)st.st_size) {
        ESP_LOGE(OTA_TAG, "Install failed after %u bytes: %s", (unsigned)total,
                 read_error ? "SD card read error" : esp_err_to_name(err));
        esp_ota_abort(handle);
        return;   /*  The file is kept, so the next boot tries again  */
    }

    /*  esp_ota_end checks the image's checksum and SHA-256, so a damaged copy is rejected  */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "Firmware image failed verification: %s", esp_err_to_name(err));
        rename_firmware_file(BW_OTA_BAD_FILE);
        return;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return;
    }

    /*  Lets the next boot tell whether the trial ended in a rollback, and why  */
    set_trial_version(new_desc.version);
    rename_firmware_file(BW_OTA_DONE_FILE);
    ESP_LOGI(OTA_TAG, "Firmware %s installed (%u bytes); restarting", new_desc.version, (unsigned)total);
    vTaskDelay(pdMS_TO_TICKS(500));   /*  Let the log drain  */
    esp_restart();
}
