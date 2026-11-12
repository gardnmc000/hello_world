/*
 * bw_settings.h
 *
 *  Settings the server can change. Each has a default compiled into the firmware
 *  and a permitted range; the server sends a key=value text (its config.cfg), which
 *  the unit checks, stores in NVS and applies. At boot the stored text is applied
 *  again, so the settings survive restarts, firmware updates and SD-card formats.
 *
 *  A key missing from the text takes its compiled default, so the server's file
 *  only needs the settings that differ. A value outside its range is refused and
 *  the default kept; unknown keys are ignored, so new settings can be added freely.
 */

#ifndef MAIN_BW_SETTINGS_H_
#define MAIN_BW_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define BW_SETTINGS_MAX_TEXT 1024       /*  Largest config text the unit accepts  */

typedef struct {
    float    min_bird_freq_hz;          /*  A block must read at least this frequency to be a bird  */
    float    bird_level_ratio;          /*  ...and its slope RMS this multiple of the noise floor  */
    float    min_noise_floor;           /*  Lowest the tracked noise floor may fall  */
    uint32_t sample_interval_ms;        /*  Pause between bursts when nothing was heard  */
    uint32_t record_holdoff_s;          /*  Pause after recording a clip  */
    uint32_t clip_length_s;             /*  Length of each recorded clip  */
    uint32_t active_start_hour;         /*  Recording window, GMT: from this hour...  */
    uint32_t active_end_hour;           /*  ...up to (not including) this one; may wrap past midnight  */
    uint32_t reporting_interval_s;      /*  Time between reporting checks  */
    uint32_t fw_check_interval_h;       /*  Connect for a firmware check at least this often  */
    uint32_t ota_min_battery_mv;        /*  No firmware download or install below this  */
} bw_settings_t;

/*  The settings in force. Written only by settings_apply_text(); every user reads it
 *  directly, so a change takes effect at the next use.  */
extern volatile bw_settings_t g_settings;

/*  Call once at boot, after nvs_flash_init (ota_boot_check). Applies the stored text,
 *  or the compiled defaults if there is none, and logs the result.  */
void settings_init(void);

/*  Checks and applies a config text from the server and stores it in NVS.
 *  Returns the number of values refused (out of range, or not a number).  */
int settings_apply_text(const char *text, size_t length);

/*  CRC-32 of the stored text (0 if none) and how many values it had refused,
 *  both reported to the server so it can tell whether a unit is up to date  */
uint32_t settings_crc(void);
uint8_t  settings_rejected(void);

/*  Standard CRC-32 (as zlib), shared with the server  */
uint32_t settings_crc32(const void *data, size_t length);

/*  True inside the recording window. Always true while the clock is not set.  */
bool settings_recording_active(void);

#endif /* MAIN_BW_SETTINGS_H_ */
