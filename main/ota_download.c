/*
 * ota_download.c
 *
 *  Phase 2 of the update plan: fetch new firmware from the server to the SD card.
 *  The download never touches flash. Once it is complete and its SHA-256 matches,
 *  it becomes /sdcard/firmware.bin, and the Phase 1 code installs it at the next
 *  boot, checking the signature. Protocol: docs/ota-phase2-design.md.
 */

#include "base.h"
#include "ota_update.h"
#include "storage.h"
#include "bw_settings.h"
#include "esp_app_desc.h"
#include "esp_task_wdt.h"
#include "mbedtls/sha256.h"
#include <sys/stat.h>
#include <errno.h>

#define FW_HELLO_LEN    72              /*  FWH2: FWH1 (64) + settings CRC, refused count, 3 reserved  */
#define FW_OFFER_LEN    72
#define FW_MAX_IMAGE    (3 * 1024 * 1024)       /*  One app partition  */
#define DL_CHUNK        4096
#define DL_SYNC_EVERY   (64 * 1024)             /*  Flush to the card this often, so a resume loses little  */

static const char *DL_TAG = "OTA_DL";
static const uint8_t fw_signature[8] = {0x19, 0x57, 0x55, 0xaa, 0xc0, 0x03, 0xf0, 0x1f};

extern struct sockaddr_in dest_addr;
extern char szmac_addr[32];

/*  What fw_dl.bin is meant to become; kept in fw_dl.inf so a resume can be checked  */
typedef struct {
    char     magic[4];          /*  "FWI1"  */
    char     version[32];
    uint32_t size;
    uint8_t  sha256[32];
} dl_info_t;

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = v >> 24; }
static uint32_t get_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }

/*  The reporting task is watched by the task watchdog; the startup code is not  */
static void feed_watchdog(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

static void discard_download(void)
{
    unlink(BW_OTA_DL_FILE);
    unlink(BW_OTA_DL_INFO_FILE);
}

static bool read_info(dl_info_t *info)
{
    FILE *f = fopen(BW_OTA_DL_INFO_FILE, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = fread(info, 1, sizeof(*info), f) == sizeof(*info) && memcmp(info->magic, "FWI1", 4) == 0;
    fclose(f);
    return ok;
}

static bool write_info(const dl_info_t *info)
{
    FILE *f = fopen(BW_OTA_DL_INFO_FILE, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(info, 1, sizeof(*info), f) == sizeof(*info);
    return (fclose(f) == 0) && ok;
}

static bool file_sha256(const char *path, uint8_t out[32])
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint8_t *buf = malloc(DL_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t n;
    while ((n = fread(buf, 1, DL_CHUNK, f)) > 0) {
        mbedtls_sha256_update(&ctx, buf, n);
        feed_watchdog();
    }
    bool ok = !ferror(f);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    free(buf);
    fclose(f);
    return ok;
}

/*  Turns a complete download into firmware.bin, if its hash matches the offer  */
static bool finish_download(const dl_info_t *info)
{
    uint8_t sha[32];
    if (!file_sha256(BW_OTA_DL_FILE, sha)) {
        ESP_LOGE(DL_TAG, "Unable to read %s back to check it", BW_OTA_DL_FILE);
        return false;
    }
    if (memcmp(sha, info->sha256, 32) != 0) {
        ESP_LOGE(DL_TAG, "Firmware %s failed its SHA-256 check; discarding it", info->version);
        discard_download();
        return false;
    }
    unlink(BW_OTA_FILE);
    if (rename(BW_OTA_DL_FILE, BW_OTA_FILE) != 0) {
        ESP_LOGE(DL_TAG, "Unable to rename the download to %s: errno %d", BW_OTA_FILE, errno);
        return false;
    }
    unlink(BW_OTA_DL_INFO_FILE);
    ESP_LOGI(DL_TAG, "Firmware %s downloaded and verified; it installs at the next boot", info->version);
    return true;
}

/*  Receives the image from the current offset to the end, appending to fw_dl.bin  */
static bool receive_image(int fd, const dl_info_t *info, uint32_t offset)
{
    FILE *f = fopen(BW_OTA_DL_FILE, offset > 0 ? "ab" : "wb");
    uint8_t *buf = malloc(DL_CHUNK);
    if (f == NULL || buf == NULL) {
        ESP_LOGE(DL_TAG, "Unable to open %s: errno %d", BW_OTA_DL_FILE, errno);
        if (f != NULL) fclose(f);
        free(buf);
        return false;
    }

    uint32_t pos = offset;
    uint32_t since_sync = 0;
    bool write_ok = true;
    while (pos < info->size) {
        size_t want = info->size - pos < DL_CHUNK ? info->size - pos : DL_CHUNK;
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) {
            break;      /*  Timeout or connection lost: keep what arrived  */
        }
        if (fwrite(buf, 1, (size_t) n, f) != (size_t) n) {
            write_ok = false;
            break;
        }
        pos += (uint32_t) n;
        since_sync += (uint32_t) n;
        if (since_sync >= DL_SYNC_EVERY) {
            fflush(f);
            fsync(fileno(f));
            since_sync = 0;
            ESP_LOGI(DL_TAG, "Downloaded %u of %u bytes", (unsigned) pos, (unsigned) info->size);
        }
        feed_watchdog();
    }
    free(buf);
    if (fclose(f) != 0) {
        write_ok = false;
    }

    if (!write_ok) {
        /*  A card error leaves the file in doubt, so start again next time  */
        ESP_LOGE(DL_TAG, "SD card write failed during the download; discarding it");
        discard_download();
        return false;
    }
    if (pos < info->size) {
        ESP_LOGW(DL_TAG, "Download stopped at %u of %u bytes; it resumes next time",
                 (unsigned) pos, (unsigned) info->size);
        return false;
    }
    return true;
}

bool ota_check_server(int battery_mv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    bool trial = ota_on_trial();

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(DL_TAG, "Unable to create socket: errno %d", errno);
        return false;
    }
    struct timeval io_timeout = {.tv_sec = 15, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

    bool ready = false;
    if (!connect_with_timeout(fd, (struct sockaddr *) &dest_addr, sizeof(dest_addr))) {
        ESP_LOGW(DL_TAG, "Unable to reach the server for the firmware check: errno %d", errno);
        goto done;
    }

    /*  HELLO (FWH2): id, version, battery, flags, last rolled-back version,
     *  then the CRC of the settings applied here and how many values were refused  */
    uint8_t hello[FW_HELLO_LEN] = {0};
    char rolled_back[32];
    ota_rolled_back_version(rolled_back, sizeof(rolled_back));
    memcpy(hello, "FWH2", 4);
    strncpy((char *) hello + 4, &szmac_addr[6], 8);
    strncpy((char *) hello + 12, app->version, 32);
    put_u16(hello + 44, battery_mv > 0 ? (uint16_t) battery_mv : 0);
    /*  Flags: bit 0 on trial; bits 8-15 why this boot started (esp_reset_reason_t)  */
    put_u16(hello + 46, (uint16_t) ((trial ? 1 : 0) | ((esp_reset_reason() & 0xff) << 8)));
    strncpy((char *) hello + 48, rolled_back, 16);
    put_u32(hello + 64, settings_crc());
    hello[68] = settings_rejected();

    if (!send_buffer(fd, fw_signature, sizeof(fw_signature)) ||
        !send_buffer(fd, hello, sizeof(hello))) {
        ESP_LOGW(DL_TAG, "Firmware check did not complete");
        goto done;
    }

    /*  Settings first: "CFG0" (ours are current, or the server has none), or "CFG1",
     *  a 2-byte length and the server's config text  */
    uint8_t tag[4];
    if (!receive_exact(fd, tag, sizeof(tag))) {
        ESP_LOGW(DL_TAG, "Firmware check did not complete");
        goto done;
    }
    if (memcmp(tag, "CFG1", 4) == 0) {
        static char cfg_text[BW_SETTINGS_MAX_TEXT];     /*  Static: the startup task's stack is small  */
        uint8_t len_bytes[2];
        if (!receive_exact(fd, len_bytes, 2)) {
            goto done;
        }
        size_t cfg_len = len_bytes[0] | (len_bytes[1] << 8);
        if (cfg_len > sizeof(cfg_text) || !receive_exact(fd, cfg_text, cfg_len)) {
            ESP_LOGE(DL_TAG, "Settings from the server were too long or incomplete; ignored");
            goto done;
        }
        ESP_LOGI(DL_TAG, "New settings from the server (%u bytes)", (unsigned) cfg_len);
        settings_apply_text(cfg_text, cfg_len);
    } else if (memcmp(tag, "CFG0", 4) != 0) {
        ESP_LOGW(DL_TAG, "Unexpected reply from the server; firmware check abandoned");
        goto done;
    }

    uint8_t offer[FW_OFFER_LEN];
    if (!receive_exact(fd, offer, sizeof(offer))) {
        ESP_LOGW(DL_TAG, "Firmware check did not complete");
        goto done;
    }
    if (memcmp(offer, "OFFR", 4) != 0) {
        ESP_LOGI(DL_TAG, "No new firmware on the server (running %s)", app->version);
        goto done;
    }

    dl_info_t want = {.magic = {'F', 'W', 'I', '1'}};
    memcpy(want.version, offer + 4, 31);
    want.version[31] = '\0';
    want.size = get_u32(offer + 36);
    memcpy(want.sha256, offer + 40, 32);
    ESP_LOGI(DL_TAG, "Server offers firmware %s (%u bytes)", want.version, (unsigned) want.size);

    /*  Reasons not to take it. Closing without a GET tells the server it was declined.  */
    if (strcmp(want.version, app->version) == 0) {
        goto done;
    }
    if (ota_version_blocked(want.version)) {
        ESP_LOGW(DL_TAG, "Firmware %s failed its trial here before; not downloading it", want.version);
        goto done;
    }
    if (trial) {
        ESP_LOGW(DL_TAG, "Running firmware is still on trial; download postponed");
        goto done;
    }
    int min_mv = (int) g_settings.ota_min_battery_mv;
    if (battery_mv > 0 && battery_mv < min_mv) {
        ESP_LOGW(DL_TAG, "Battery %d mV is below %d mV; download postponed", battery_mv, min_mv);
        goto done;
    }
    if (want.size == 0 || want.size > FW_MAX_IMAGE) {
        ESP_LOGE(DL_TAG, "Offered image size %u is not plausible", (unsigned) want.size);
        goto done;
    }

    /*  Resume only if the partial file belongs to this exact offer  */
    dl_info_t have;
    struct stat st;
    uint32_t offset = 0;
    if (read_info(&have) && memcmp(&have, &want, sizeof(want)) == 0 && stat(BW_OTA_DL_FILE, &st) == 0 &&
        st.st_size <= want.size) {
        offset = (uint32_t) st.st_size;
        ESP_LOGI(DL_TAG, "Resuming the download at %u bytes", (unsigned) offset);
    } else {
        discard_download();
        if (!write_info(&want)) {
            ESP_LOGE(DL_TAG, "Unable to write %s", BW_OTA_DL_INFO_FILE);
            goto done;
        }
    }

    if (offset < want.size) {
        uint8_t get[8];
        memcpy(get, "GET1", 4);
        put_u32(get + 4, offset);
        if (!send_buffer(fd, get, sizeof(get)) || !receive_image(fd, &want, offset)) {
            goto done;
        }
    }
    ready = finish_download(&want);

done:
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return ready;
}
