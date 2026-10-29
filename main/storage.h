/*
 * storage.h
 *
 *  Created on: 7 Jun 2026
 *      Author: gardn
 */

#ifndef MAIN_STORAGE_H_
#define MAIN_STORAGE_H_

#include "base.h"
#include "dirent.h"
#include <stddef.h>


void mount_sdcard(void);

//void restore_command_mode();

/*  Scan the output directory, and identify the index number for the first file encountered with a '.wav' extensions    */
/*  Return 1 when found, 0 when empty, or -1 when the directory cannot be scanned. Caller provides 256 bytes.           */
int get_next_dir_entry(char * filename);

/*  Scan the output directory, and identify the index number for the first file encountered with a '.bin' extensions    */
/*  Return the index number for use by the caller                                                                       */
int archive_clip(char * filename);
void close_data_socket(void);
int save_recording(const char *filespec, const void *pcm_data, size_t pcm_length);
int read_adc_voltage_gpio8(void);
//void convert_wav16(const char * filespec);

int store_config(struct BW_CONFIG cfg);

int32_t read_pcm16_block(const char * filespec,  int offset);

#endif /* MAIN_STORAGE_H_ */
