/*
 * battery_guard.h
 *
 *  Low-battery sleep. When the battery falls below low_battery_mv, the unit stops
 *  recording and calling home and deep sleeps, waking every low_battery_sleep_s to
 *  measure the battery again. It carries on only once the battery is back above
 *  resume_battery_mv; the gap between the two stops it waking straight into another
 *  low reading. The charger keeps charging while the unit sleeps.
 *
 *  Each wake from deep sleep restarts the firmware, so recovery is a normal boot.
 *  The record of the last low-battery spell is kept in RTC memory, which survives
 *  deep sleep but not a power cut, and goes out in the next status report.
 *
 *  Firmware on trial is never put to sleep: a restart during the trial (an hour at
 *  most) would roll it back, so it runs on, as before this guard existed.
 */

#ifndef MAIN_BATTERY_GUARD_H_
#define MAIN_BATTERY_GUARD_H_

#include <stdbool.h>
#include <stddef.h>

/*  Defaults for the server-adjustable thresholds (see bw_settings.c for their ranges)  */
#define BW_LOW_BATTERY_MV       3400    /*  Stop and sleep below this  */
#define BW_RESUME_BATTERY_MV    3700    /*  ...and start again only above this  */
#define BW_LOW_BATTERY_SLEEP_S  3600    /*  Time between checks while asleep  */

/*  Below this a reading is taken to mean no battery is fitted (bench power on USB)  */
#define BW_BATTERY_PRESENT_MV   2500

/*  The battery voltage in mV (an average of a few readings), or -1 when there is no
 *  usable reading, including when no battery is fitted                              */
int battery_guard_read_mv(void);

/*  Call at boot, after settings_init and before anything powers the modem. Releases
 *  the pins held through the last sleep, then measures the battery. Goes back to
 *  sleep (and does not return) if it is too low to start the unit.                  */
void battery_guard_boot(void);

/*  True if this reading (from battery_guard_read_mv) means the unit should sleep  */
bool battery_guard_is_low(int battery_mv);

/*  Starts a low-battery spell and deep sleeps; does not return. Call only between
 *  clips, with the modem already powered down. Returns at once, doing nothing, while
 *  the firmware is on trial.                                                        */
void battery_guard_sleep(int battery_mv);

/*  Writes the low-battery record as "key=value" lines for the status report.
 *  Returns the length written.                                               */
size_t battery_guard_format(char *out, size_t size);

#endif /* MAIN_BATTERY_GUARD_H_ */
