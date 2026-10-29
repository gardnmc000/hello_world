
/*
 * mgaudio.h
 *
 *  Created on: 7 Apr 2025
 *      Author: Mike.Gardner
 */

#ifndef MAIN_MGAUDIO_H_
#define MAIN_MGAUDIO_H_

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "driver/i2s_types_legacy.h"
#include "hal/i2s_types.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
//#include "C:/Espressif/frameworks/esp-idf-v5.3.1/components/spi_flash/include/esp_flash.h"
#include "esp_system.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include "esp_check.h"
#include "esp_log.h"

//#include "mgstorage.h"

void build_clip16(i2s_chan_handle_t rx_handle, const char * filespec);
uint32_t get_max_slope(i2s_chan_handle_t rx_handle);
void i2s_start(i2s_chan_handle_t rx_handle);
void i2s_end (i2s_chan_handle_t rx_handle);

#endif /* MAIN_MGAUDIO_H_ */

