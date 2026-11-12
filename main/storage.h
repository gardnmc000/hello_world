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

/*  As get_next_dir_entry, but passes over the skip_count names in skip[]  */
#define BW_CLIP_NAME_MAX 64
#define BW_CLIP_NAME_LEN 22     /*  "Dxxxxxxyydddhhmmss.wav"; the server only accepts this length.
                                    Other .wav files are not clips and are left alone.  */
int get_next_dir_entry_excluding(char * filename, const char (*skip)[BW_CLIP_NAME_MAX], int skip_count);

/*  Scan the output directory, and identify the index number for the first file encountered with a '.bin' extensions    */
/*  Return the index number for use by the caller                                                                       */
int archive_clip(char * filename);
int delete_clip(const char * filename);

void close_data_socket(void);

/*  Socket helpers shared by the clip upload and the firmware download  */
bool send_buffer(int fd, const void *data, size_t length);
bool receive_exact(int fd, void *data, size_t length);
bool connect_with_timeout(int fd, const struct sockaddr *address, socklen_t address_length);   /*  15 s  */
int save_recording(const char *filespec, const void *pcm_data, size_t pcm_length);
int read_adc_voltage_gpio8(void);

/*  Lowest battery voltage seen while the modem is busy. A worn battery can read
 *  normally at rest and still sag far enough under modem load to cause brownouts.  */
void battery_load_reset(void);
void battery_load_sample(void);
int  battery_load_min_mv(void);     /*  0 if no sample has been taken  */
//void convert_wav16(const char * filespec);


int32_t read_pcm16_block(const char * filespec,  int offset);

#endif /* MAIN_STORAGE_H_ */
