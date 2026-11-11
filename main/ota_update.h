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

/*  Call once at boot, before the other tasks start. Logs the running version, reports
 *  an earlier rollback, and starts the 1 hour timer if this firmware is on trial.     */
void ota_boot_check(void);

/*  Call once at boot, after the SD card is mounted. Installs /sdcard/firmware.bin
 *  and restarts if it holds a new version; otherwise returns.                        */
void ota_install_from_sd(void);

/*  Marks firmware on trial as valid, which cancels the rollback. Safe to call at any
 *  time, from any task; it does nothing if the firmware is already valid.            */
void ota_mark_valid(const char *reason);

#endif /* MAIN_OTA_UPDATE_H_ */
