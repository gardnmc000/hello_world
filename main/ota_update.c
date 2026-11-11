/*
 * ota_update.c
 *
 *  Phase 1 of the update plan: install firmware from the SD card at boot.
 *
 *  New firmware starts "on trial". If the unit reboots for any reason before
 *  ota_mark_valid() is called, the bootloader goes back to the previous firmware.
 *  It is marked valid when it first reaches the network, or after 1 hour of
 *  running without a crash, whichever comes first.
 */

#include "base.h"
#include "ota_update.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include <sys/stat.h>
#include <errno.h>

#define OTA_CHUNK_SIZE 4096

static const char *OTA_TAG = "OTA";

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
    ESP_LOGI(OTA_TAG, "Running firmware %s from partition %s", app->version, running->label);

    /*  A rollback leaves the failed firmware's partition marked invalid  */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    if (invalid != NULL && esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK) {
        ESP_LOGW(OTA_TAG, "Firmware %s in partition %s was rolled back", invalid_desc.version, invalid->label);
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
    } else {
        ESP_LOGE(OTA_TAG, "Failed to mark firmware valid: %s", esp_err_to_name(err));
    }
}

void ota_install_from_sd(void)
{
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

    /*  The same firmware was installed before and rolled back  */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_desc;
    if (invalid != NULL && esp_ota_get_partition_description(invalid, &invalid_desc) == ESP_OK &&
        strcmp(new_desc.version, invalid_desc.version) == 0) {
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

    rename_firmware_file(BW_OTA_DONE_FILE);
    ESP_LOGI(OTA_TAG, "Firmware %s installed (%u bytes); restarting", new_desc.version, (unsigned)total);
    vTaskDelay(pdMS_TO_TICKS(500));   /*  Let the log drain  */
    esp_restart();
}
