/*
 * bw_settings.c
 *
 *  Server-adjustable settings: defaults, ranges, parsing and storage. See bw_settings.h.
 */

#include "base.h"
#include "bw_settings.h"
#include "ota_update.h"
#include "nvs.h"
#include <ctype.h>
#include <math.h>
#include <time.h>

static const char *SET_TAG = "SETTINGS";

#define NVS_NS     "cfg"
#define KEY_TEXT   "text"
#define KEY_CRC    "crc"
#define KEY_REJ    "rej"

volatile bw_settings_t g_settings;
static uint32_t s_crc = 0;
static uint8_t  s_rejected = 0;

typedef enum { SET_FLOAT, SET_U32 } set_type_t;

typedef struct {
    const char *key;
    set_type_t  type;
    size_t      offset;
    double      def, min, max;
} setting_def_t;

#define F(field) offsetof(bw_settings_t, field)

/*  The defaults come from base.h and ota_update.h, so a build without a config
 *  behaves exactly as before. Ranges stop a mistyped value disabling a unit.  */
static const setting_def_t s_defs[] = {
    {"min_bird_freq_hz",     SET_FLOAT, F(min_bird_freq_hz),     BW_MIN_BIRD_FREQ_HZ,          1000,    8000},
    {"bird_level_ratio",     SET_FLOAT, F(bird_level_ratio),     BW_BIRD_LEVEL_RATIO,           1.1,       5},
    {"min_noise_floor",      SET_FLOAT, F(min_noise_floor),      BW_MIN_NOISE_FLOOR,          10000, 2000000},
    {"sample_interval_ms",   SET_U32,   F(sample_interval_ms),   BW_SAMPLE_INTERVAL,            200,   60000},
    {"record_holdoff_s",     SET_U32,   F(record_holdoff_s),     BW_RECORD_INTERVAL / 1000,       0,     600},
    {"clip_length_s",        SET_U32,   F(clip_length_s),        BW_DEFAULT_CLIP_LENGTH,          1,      10},
    {"active_start_hour",    SET_U32,   F(active_start_hour),    BW_ACTIVE_START_HOUR,            0,      23},
    {"active_end_hour",      SET_U32,   F(active_end_hour),      BW_ACTIVE_END_HOUR,              1,      24},
    {"reporting_interval_s", SET_U32,   F(reporting_interval_s), BW_REPORTING_INTERVAL_MS / 1000, 60,  86400},
    {"fw_check_interval_h",  SET_U32,   F(fw_check_interval_h),  BW_OTA_CHECK_INTERVAL_MS / 3600000, 1,  168},
    {"ota_min_battery_mv",   SET_U32,   F(ota_min_battery_mv),   BW_OTA_MIN_BATTERY_MV,        3300,    4000},
};
#define NUM_DEFS (sizeof(s_defs) / sizeof(s_defs[0]))

uint32_t settings_crc32(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (length--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    return ~crc;
}

static void set_value(bw_settings_t *s, const setting_def_t *d, double v)
{
    void *field = (uint8_t *) s + d->offset;
    if (d->type == SET_FLOAT) {
        *(float *) field = (float) v;
    } else {
        *(uint32_t *) field = (uint32_t) v;
    }
}

static double get_value(const bw_settings_t *s, const setting_def_t *d)
{
    const void *field = (const uint8_t *) s + d->offset;
    return d->type == SET_FLOAT ? *(const float *) field : *(const uint32_t *) field;
}

static void load_defaults(bw_settings_t *s)
{
    for (size_t i = 0; i < NUM_DEFS; i++) {
        set_value(s, &s_defs[i], s_defs[i].def);
    }
}

/*  Parses the text over the defaults into *s. Returns the number of values refused.  */
static int parse_text(const char *text, size_t length, bw_settings_t *s, bool log)
{
    load_defaults(s);
    int rejected = 0;
    char line[128];
    size_t pos = 0;

    while (pos < length) {
        size_t n = 0;
        while (pos < length && text[pos] != '\n') {
            if (n < sizeof(line) - 1) {
                line[n++] = text[pos];
            }
            pos++;
        }
        pos++;                              /*  Past the newline  */
        line[n] = '\0';

        char *hash = strchr(line, '#');     /*  Comments  */
        if (hash != NULL) *hash = '\0';
        char *key = line;
        while (isspace((unsigned char) *key)) key++;
        if (*key == '\0') continue;

        char *eq = strchr(key, '=');
        if (eq == NULL) {
            if (log) ESP_LOGW(SET_TAG, "Ignoring line without '=': %s", key);
            continue;
        }
        *eq = '\0';
        char *value = eq + 1;
        for (char *e = eq - 1; e >= key && isspace((unsigned char) *e); e--) *e = '\0';
        while (isspace((unsigned char) *value)) value++;
        for (char *e = value + strlen(value) - 1; e >= value && isspace((unsigned char) *e); e--) *e = '\0';

        const setting_def_t *d = NULL;
        for (size_t i = 0; i < NUM_DEFS && d == NULL; i++) {
            if (strcmp(s_defs[i].key, key) == 0) d = &s_defs[i];
        }
        if (d == NULL) {
            if (log) ESP_LOGW(SET_TAG, "Ignoring unknown setting '%s'", key);
            continue;
        }

        char *end;
        double v = strtod(value, &end);
        bool whole = (d->type == SET_FLOAT) || (v == floor(v));
        if (end == value || *end != '\0' || !isfinite(v) || !whole || v < d->min || v > d->max) {
            if (log) ESP_LOGE(SET_TAG, "Refused %s=%s (allowed %g to %g%s); keeping the default",
                              key, value, d->min, d->max, d->type == SET_U32 ? ", whole numbers" : "");
            rejected++;
            continue;
        }
        set_value(s, d, v);
    }

    /*  An empty window would stop recording altogether  */
    if (s->active_start_hour == s->active_end_hour) {
        if (log) ESP_LOGE(SET_TAG, "Refused active hours %u to %u (an empty window); recording all day",
                          (unsigned) s->active_start_hour, (unsigned) s->active_end_hour);
        s->active_start_hour = BW_ACTIVE_START_HOUR;
        s->active_end_hour = BW_ACTIVE_END_HOUR;
        rejected++;
    }
    return rejected;
}

static void log_settings(const char *why)
{
    bw_settings_t s = g_settings;
    ESP_LOGI(SET_TAG, "Settings %s (config CRC %08lx, %u refused):", why, (unsigned long) s_crc, s_rejected);
    for (size_t i = 0; i < NUM_DEFS; i++) {
        double v = get_value(&s, &s_defs[i]);
        bool is_default = fabs(v - s_defs[i].def) < 1e-6;
        ESP_LOGI(SET_TAG, "  %-22s %g%s", s_defs[i].key, v, is_default ? "" : "  (from server)");
    }
}

void settings_init(void)
{
    bw_settings_t s;
    load_defaults(&s);
    s_crc = 0;
    s_rejected = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char text[BW_SETTINGS_MAX_TEXT];
        size_t length = sizeof(text);
        if (nvs_get_blob(h, KEY_TEXT, text, &length) == ESP_OK) {
            s_rejected = (uint8_t) parse_text(text, length, &s, false);
            s_crc = settings_crc32(text, length);
        }
        nvs_close(h);
    }
    g_settings = s;
    log_settings(s_crc ? "at boot" : "at boot (defaults; no config received yet)");
}

int settings_apply_text(const char *text, size_t length)
{
    if (length > BW_SETTINGS_MAX_TEXT) {
        ESP_LOGE(SET_TAG, "Config text of %u bytes is too long; ignored", (unsigned) length);
        return 0;
    }
    bw_settings_t s;
    int rejected = parse_text(text, length, &s, true);
    g_settings = s;
    s_crc = settings_crc32(text, length);
    s_rejected = (uint8_t) (rejected > 255 ? 255 : rejected);

    /*  Stored even if some values were refused, so the server's copy is not sent again
     *  on every connection; the refusals are reported instead  */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, KEY_TEXT, text, length);
        nvs_set_u32(h, KEY_CRC, s_crc);
        nvs_set_u8(h, KEY_REJ, s_rejected);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(SET_TAG, "Unable to store the config; it applies until the next restart");
    }
    log_settings("received from the server");
    return rejected;
}

uint32_t settings_crc(void)      { return s_crc; }
uint8_t  settings_rejected(void) { return s_rejected; }

bool settings_recording_active(void)
{
    time_t now = time(NULL);
    struct tm t;
    if (now < 1700000000 || gmtime_r(&now, &t) == NULL) {
        return true;        /*  Clock not set yet: record rather than miss birds  */
    }
    uint32_t start = g_settings.active_start_hour, end = g_settings.active_end_hour;
    uint32_t hour = (uint32_t) t.tm_hour;
    if (start < end) {
        return hour >= start && hour < end;
    }
    return hour >= start || hour < end;     /*  The window wraps past midnight  */
}
