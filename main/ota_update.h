/*
 * ota_update.h
 *
 *  Firmware update from the SD card, and the rollback check for new firmware.
 */

#ifndef MAIN_OTA_UPDATE_H_
#define MAIN_OTA_UPDATE_H_

#define BW_OTA_FILE           SD_MOUNT_POINT "/firmware.bin"   /*  Firmware waiting to be installed       */
#define BW_OTA_DONE_FILE      SD_MOUNT_POINT "/fw_inst.bin"    /*  Renamed to this once installed         */
#define BW_OTA_BAD_FILE       SD_MOUNT_POINT "/fw_bad.bin"     /*  Renamed to this if it was rolled back  */
#define BW_OTA_VALID_AFTER_MS (60 * 60 * 1000)                 /*  1 hour without a crash marks it valid  */
#define BW_OTA_MAX_POWER_ROLLBACKS 3                           /*  Trials lost to power problems before a release is blocked  */
#define BW_OTA_MIN_BATTERY_MV 3600                             /*  No download or install below this (battery, at rest)  */
#define BW_OTA_CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)         /*  Connect for a firmware check at least this often,
                                                                   even with no clips to send. Too big for
                                                                   pdMS_TO_TICKS, which overflows above ~11.9 h  */
#define BW_OTA_DL_FILE        SD_MOUNT_POINT "/fw_dl.bin"      /*  Download in progress                    */
#define BW_OTA_DL_INFO_FILE   SD_MOUNT_POINT "/fw_dl.inf"      /*  What the download in progress should be */

/*  Call once at boot, before the other tasks start. Logs the running version, reports
 *  an earlier rollback, and starts the 1 hour timer if this firmware is on trial.     */
void ota_boot_check(void);

/*  Call once at boot, after the SD card is mounted. Installs /sdcard/firmware.bin
 *  and restarts if it holds a new version; otherwise returns.                        */
void ota_install_from_sd(void);

/*  Marks firmware on trial as valid, which cancels the rollback. Safe to call at any
 *  time, from any task; it does nothing if the firmware is already valid.            */
void ota_mark_valid(const char *reason);

/*  True if this version failed a trial and must not be installed (or downloaded) again:
 *  it crashed, or lost BW_OTA_MAX_POWER_ROLLBACKS trials to power problems.          */
bool ota_version_blocked(const char *version);

/*  The last rolled-back version, or "" if none; reported to the server  */
void ota_rolled_back_version(char *out, size_t size);

/*  True while the running firmware is on trial  */
bool ota_on_trial(void);

typedef enum {
    OTA_CHECK_FAILED,       /*  Server not reached, or the conversation broke off   */
    OTA_CHECK_NOTHING,      /*  Nothing to download (none offered, or not taken)    */
    OTA_CHECK_WANTED,       /*  An offer worth downloading; download was false      */
    OTA_CHECK_READY,        /*  A complete, verified image is in /sdcard/firmware.bin  */
} ota_check_t;

/*  Call while PPP is up. Reports this unit to the server and applies any new settings
 *  it sends. With download true, also downloads new firmware it offers to
 *  /sdcard/firmware.bin, resuming an earlier partial download; with download false
 *  it only says whether there is an offer worth downloading, so the caller can send
 *  its clips first and then call again. On OTA_CHECK_READY the caller should shut
 *  the modem down and restart, so ota_install_from_sd() runs.                       */
ota_check_t ota_check_server(int battery_mv, bool download);

#endif /* MAIN_OTA_UPDATE_H_ */
