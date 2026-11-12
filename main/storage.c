/*
 * storage.c
 *
 *  Created on: 11 Apr 2026
 *      Author: gardn
 */

#include "base.h"
#include "esp_modem_c_api_types.h"
#include "lwip/udp.h"
#include "soc/uart_struct.h"
#include "storage.h"
#include "format_wav.h"
#include "mgnetwork.h"
#include "ota_update.h"
#include <fcntl.h>
#include "freertos/semphr.h"
#include "esp_task_wdt.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"


static const char * STORAGE_TAG = "STORAGE";
static SemaphoreHandle_t storage_mutex;
extern int sock;
extern esp_modem_dce_t *dce;
//extern SemaphoreHandle_t transfer_sema; 
extern char*  txbuffer;
extern char*  rxbuffer;
extern struct sockaddr_in dest_addr;
static bool lock_storage(void)
{
    if (storage_mutex == NULL ||
        xSemaphoreTake(storage_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(STORAGE_TAG, "Unable to lock SD-card storage");
        return false;
    }
    return true;
}

static void unlock_storage(void)
{
    xSemaphoreGive(storage_mutex);
}

// When testing SD and SPI modes, keep in mind that once the card has been
// initialized in SPI mode, it can not be reinitialized in SD mode without
// toggling power to the card.
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
/*  Template only: save_recording() fills in both size fields from the actual PCM length  */
static const wav_header_t wav_header_template =
{
    .descriptor_chunk = {.chunk_id = {'R', 'I', 'F', 'F'},
                         .chunk_size = 0,
                         .chunk_format = {'W', 'A', 'V', 'E'}
                        },
    .fmt_chunk = {.subchunk_id = {'f', 'm', 't', ' '},
                  .subchunk_size = 16,
                  .audio_format = 1,
                  .num_of_channels = (1),
                  .sample_rate = (44100),
                  .byte_rate = (16) * (44100) * (1) / 8,
                  .block_align = (16) * (1) / 8,
                  .bits_per_sample = (16)},
    .data_chunk = {.subchunk_id = {'d', 'a', 't', 'a'},
                   .subchunk_size = 0
                  }
};
     

bool send_buffer(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(fd, bytes + sent, length - sent, 0);
        if (result <= 0) {
            ESP_LOGE(STORAGE_TAG, "Socket send failed: errno %d", errno);
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

bool receive_exact(int fd, void *data, size_t length) {
    uint8_t *bytes = data;
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(fd, bytes + received, length - received, 0);
        if (result <= 0) {
            ESP_LOGE(STORAGE_TAG, "Socket receive failed: errno %d", errno);
            return false;
        }
        received += (size_t)result;
    }
    return true;
}

bool connect_with_timeout(int fd, const struct sockaddr *address, socklen_t address_length) {
    int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        return false;
    }

    int connect_result = connect(fd, address, address_length);
    if (connect_result != 0 && errno != EINPROGRESS) {
        fcntl(fd, F_SETFL, original_flags);
        return false;
    }

    if (connect_result != 0) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
        int select_result = select(fd + 1, NULL, &write_fds, NULL, &timeout);
        if (select_result <= 0) {
            if (select_result == 0) {
                errno = ETIMEDOUT;
            }
            fcntl(fd, F_SETFL, original_flags);
            return false;
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            if (socket_error != 0) {
                errno = socket_error;
            }
            fcntl(fd, F_SETFL, original_flags);
            return false;
        }
    }

    if (fcntl(fd, F_SETFL, original_flags) < 0) {
        return false;
    }
    return true;
}

void close_data_socket(void) {
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
    }
}

int send_file(char *filename) {
    static const uint8_t signature[8] = {0x19, 0x57, 0x55, 0xaa, 0xc0, 0x03, 0xf0, 0x0f};
    struct timeval io_timeout = {.tv_sec = 15, .tv_usec = 0};
    int result = BW_SEND_NO_SERVER;
    int packet_number = 0;
    int offset = 0;

    if (sock < 0 || filename == NULL || txbuffer == NULL || rxbuffer == NULL) {
        ESP_LOGE(STORAGE_TAG, "Invalid socket or transfer buffer");
        goto cleanup;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) != 0) {
        ESP_LOGE(STORAGE_TAG, "Failed to configure socket timeouts: errno %d", errno);
        goto cleanup;
    }

    if (!connect_with_timeout(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr))) {
        ESP_LOGE(STORAGE_TAG, "Socket unable to connect: errno %d", errno);
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(STORAGE_TAG, "Connected to server using socket %d", sock);

    if (!send_buffer(sock, signature, sizeof(signature)) || 
        !receive_exact(sock, rxbuffer, 5) ||
        memcmp(rxbuffer, "READY", 5) != 0) 
    {
        ESP_LOGE(STORAGE_TAG, "Server did not provide READY");
        goto cleanup;
    }
    result = BW_SEND_FILE_FAILED;
    ota_mark_valid("server sent READY");

    size_t filename_length = strlen(filename);
    if (filename_length >= 64 ||
        !send_buffer(sock, filename, filename_length) ||
        !receive_exact(sock, rxbuffer, 2) ||
        memcmp(rxbuffer, "GO", 2) != 0) {
        ESP_LOGE(STORAGE_TAG, "Server rejected filename or did not provide GO");
        goto cleanup;
    }

    ESP_LOGI(STORAGE_TAG, "Transferring file: %s", filename);
    while (true) {
        int32_t bytes_read = read_pcm16_block(filename, offset);
        if (bytes_read < 0) {
            ESP_LOGE(STORAGE_TAG, "Failed reading file %s at offset %d", filename, offset);
            goto cleanup;
        }
        if (bytes_read == 0) {
            if (packet_number == 0) {
                ESP_LOGE(STORAGE_TAG, "File %s contains no transferable data", filename);
                goto cleanup;
            }
            break;
        }
        /*  The server acknowledges with "OK" and 3 digits, so 999 packets of 1024 bytes
         *  is the most it can take: a clip of about 11.6 s (clip_length_s allows up to 10)  */
        if (packet_number >= 999 || !send_buffer(sock, txbuffer, (size_t)bytes_read)) {
            ESP_LOGE(STORAGE_TAG, "Transfer failed or exceeded packet limit");
            goto cleanup;
        }

        packet_number++;
        memset(rxbuffer, 0, 6);
        if (!receive_exact(sock, rxbuffer, 5) ||
            rxbuffer[0] != 'O' || rxbuffer[1] != 'K') {
            ESP_LOGE(STORAGE_TAG, "Invalid acknowledgement for packet %d", packet_number);
            goto cleanup;
        }

        char acknowledged_packet[4] = {0};
        memcpy(acknowledged_packet, rxbuffer + 2, 3);
        if (atoi(acknowledged_packet) != packet_number) {
            ESP_LOGE(STORAGE_TAG, "Packet acknowledgement mismatch: expected %d, got %s",
                     packet_number, acknowledged_packet);
            goto cleanup;
        }

        offset += bytes_read;
        /*  A slow link can take longer than the watchdog timeout to send one clip  */
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    result = BW_SEND_OK;

cleanup:
    close_data_socket();
    return result;
}

//
//void restore_command_mode() {
//    esp_err_t err;
//    
//    ESP_LOGI(STORAGE_TAG, "Requesting COMMAND mode");
//    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
//    if (err != ESP_OK) {
//        ESP_LOGE(STORAGE_TAG, "Failed to switch to COMMAND mode");
//    }
//    /*  TODO:  This assumes the modem is still operational, there may be a condition where it's not  */
//    ESP_LOGI(STORAGE_TAG, "Shutting Down MODEM");
//    pulse_modem_power();
//}

///*  Requires the TCP connection to be established and an IP address granted  */
//int send_clip(char * filename) {
//    esp_err_t err;
//    
//    /*  TODO:  Handle the case where the system is still in PPP mode  */  
//
//    
//        socket_send(filename);
//        /*  transfer_file() frees the tx & rx buffer and closes The TCP socket */
//   
//        vTaskDelay(pdMS_TO_TICKS(BW_INTERCLIP_INTERVAL)); 
//
//    
//
//   
//    return (0);
//}




void mount_sdcard(void) {
  esp_err_t ret;
  storage_mutex = xSemaphoreCreateMutex();
  if (storage_mutex == NULL) {
    ESP_LOGE(STORAGE_TAG, "Failed to create SD-card mutex");
    return;
  }

  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = true,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  ESP_LOGI(STORAGE_TAG, "Initializing SD card");

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = CONFIG_MGEXAMPLE_SPI_MOSI_GPIO,
      .miso_io_num = CONFIG_MGEXAMPLE_SPI_MISO_GPIO,
      .sclk_io_num = CONFIG_MGEXAMPLE_SPI_SCLK_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4096,
  };

  host.max_freq_khz = 16000; // 16 MHz

  ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(STORAGE_TAG, "Failed to initialize bus.");
    return;
  }

  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
  // has these signals.
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = CONFIG_MGEXAMPLE_SPI_CS_GPIO;
  slot_config.host_id = host.slot;

  ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(STORAGE_TAG, "Failed to mount filesystem.");
    } else {
      ESP_LOGE(STORAGE_TAG,
               "Failed to initialize the SD card (%s). "
               "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
    }
    return;
  }

  // Card has been initialized, print its properties
  sdmmc_card_print_info(stdout, card);
}


/*  Scan the output directory, and identify the index number for the first file encountered with a '.wav' extensions    */
/*  Return the index number for use by the caller                                                                       */
int get_next_dir_entry(char * filename) {
  return get_next_dir_entry_excluding(filename, NULL, 0);
}

/*  As get_next_dir_entry, but passes over the names in skip[]  */
int get_next_dir_entry_excluding(char * filename, const char (*skip)[BW_CLIP_NAME_MAX], int skip_count) {

  ESP_LOGI(STORAGE_TAG, "list_directory()");

  if (filename == NULL || !lock_storage()) {
    return -1;
  }

  DIR *d = opendir(SD_MOUNT_POINT);
  if (d == NULL) {
    ESP_LOGE(STORAGE_TAG, "Unable to open recordings directory: errno %d", errno);
    unlock_storage();
    return -1;
  }

  int result = 0;
  struct dirent *dir;
  while ((dir = readdir(d)) != NULL) {
    const char *extension = strrchr(dir->d_name, '.');
    if (dir->d_type == DT_REG && extension != NULL &&
        strcmp(extension, ".wav") == 0 && strlen(dir->d_name) == BW_CLIP_NAME_LEN) {
      bool skipped = false;
      for (int i = 0; i < skip_count && !skipped; i++) {
        skipped = strcmp(dir->d_name, skip[i]) == 0;
      }
      if (skipped) {
        continue;
      }
      int length = snprintf(filename, 256, "%s", dir->d_name);
      if (length < 0 || length >= 256) {
        ESP_LOGE(STORAGE_TAG, "Recording filename is too long");
        result = -1;
      } else {
        ESP_LOGI(STORAGE_TAG, "Found pending recording: %s", filename);
        result = 1;
      }
      break;
    }
  }

  if (closedir(d) != 0) {
    ESP_LOGE(STORAGE_TAG, "Failed to close recordings directory: errno %d", errno);
    result = -1;
  }
  unlock_storage();
  return result;
}

/*  Scan the output directory, and identify the index number for the first file encountered with a '.bin' extensions    */
/*  Return the index number for use by the caller                                                                       */
int archive_clip(char * filename) {
  ESP_LOGI(STORAGE_TAG, "Archiving file: %s\r\n", filename);

  if (filename == NULL || strchr(filename, '/') != NULL || !lock_storage()) {
    ESP_LOGE(STORAGE_TAG, "Invalid filename or unable to lock storage");
    return -1;
  }

  int result;
  char oldname[320];
  char newname[320];
  struct stat st = {0};

  if (stat(SD_MOUNT_POINT "/archive", &st) != 0 &&
      (errno != ENOENT || mkdir(SD_MOUNT_POINT "/archive", 0777) != 0)) {
    ESP_LOGE(STORAGE_TAG, "Unable to create archive directory: errno %d", errno);
    unlock_storage();
    return -1;
  }

  int old_length = snprintf(oldname, sizeof(oldname), "%s/%s",
                            SD_MOUNT_POINT, filename);
  int new_length = snprintf(newname, sizeof(newname), "%s/archive/%s",
                            SD_MOUNT_POINT, filename);
  if (old_length < 0 || old_length >= (int)sizeof(oldname) ||
      new_length < 0 || new_length >= (int)sizeof(newname)) {
    ESP_LOGE(STORAGE_TAG, "Archive path is too long");
    unlock_storage();
    return -1;
  }

  result = rename(oldname, newname);
  if (result != 0) {
    ESP_LOGE(STORAGE_TAG, "Rename error %d: errno %d", result, errno);
  }
  unlock_storage();
  return result;
}


int delete_clip(const char * filename) {
  if (filename == NULL || strchr(filename, '/') != NULL || !lock_storage()) {
    ESP_LOGE(STORAGE_TAG, "Invalid filename or unable to lock storage");
    return -1;
  }

  char path[320];
  int result = -1;
  int length = snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, filename);
  if (length < 0 || length >= (int)sizeof(path)) {
    ESP_LOGE(STORAGE_TAG, "Recording path is too long");
  } else {
    result = unlink(path);
    if (result != 0) {
      ESP_LOGE(STORAGE_TAG, "Failed to delete %s: errno %d", path, errno);
    }
  }
  unlock_storage();
  return result;
}

/*  Read 1024 bytes from specified file at 'offset' into data array  */
int32_t read_pcm16_block(const char* fnam, int offset) {

  if (fnam == NULL || offset < 0 || !lock_storage()) {
    return -1;
  }

  int32_t bytes_read = -1;
  char filename[320];
  int length = snprintf(filename, sizeof(filename), SD_MOUNT_POINT "/%s", fnam);
  if (length < 0 || length >= (int)sizeof(filename)) {
    ESP_LOGE(STORAGE_TAG, "Recording path is too long");
    unlock_storage();
    return -1;
  }

  FILE *fin = fopen(filename, "rb");
  if (fin == NULL) {
    ESP_LOGE(STORAGE_TAG, "Unable to open file: %s, errno %d", filename, errno);
    unlock_storage();
    return -1;
  }

  if (fseek(fin, offset, SEEK_SET) == 0) {
    bytes_read = (int32_t)fread(txbuffer, 1, 1024, fin);
  } else {
    ESP_LOGE(STORAGE_TAG, "Could not seek to position %d", offset);
  }
  if (fclose(fin) != 0) {
    ESP_LOGE(STORAGE_TAG, "Failed to close recording file: errno %d", errno);
    bytes_read = -1;
  }
  unlock_storage();
  return (bytes_read);
}

/*  Timestamps can repeat when the clock is unsynced, so never reuse a pending or archived name.  */
/*  A clash changes the prefix to E and then steps the seconds, keeping the length unchanged.     */
/*  Must be called with storage locked.                                                           */
static bool make_unique_recording_name(const char *filespec, char *name, size_t name_size)
{
  int name_length = snprintf(name, name_size, "%s", filespec);
  if (name_length < 4 || name_length >= (int)name_size) {
    return false;
  }
  int seconds = (name[name_length - 2] - '0') * 10 + (name[name_length - 1] - '0');

  for (int attempt = 0; attempt <= 60; attempt++) {
    if (attempt > 0) {
      int stepped = (seconds + attempt - 1) % 60;
      name[0] = 'E';
      name[name_length - 2] = '0' + stepped / 10;
      name[name_length - 1] = '0' + stepped % 10;
    }

    char pending_path[160];
    char archive_path[160];
    struct stat st;
    int length = snprintf(pending_path, sizeof(pending_path), SD_MOUNT_POINT "/%s.wav", name);
    if (length < 0 || length >= (int)sizeof(pending_path)) {
      return false;
    }
    length = snprintf(archive_path, sizeof(archive_path), SD_MOUNT_POINT "/archive/%s.wav", name);
    if (length < 0 || length >= (int)sizeof(archive_path)) {
      return false;
    }
    if (stat(pending_path, &st) != 0 && stat(archive_path, &st) != 0) {
      return true;
    }
  }
  return false;
}



int save_recording(const char *filespec, const void *pcm_data, size_t pcm_length)
{


  if (filespec == NULL || pcm_data == NULL || strchr(filespec, '/') != NULL ||
      pcm_length > UINT32_MAX - sizeof(wav_header_t) || !lock_storage()) {
    ESP_LOGE(STORAGE_TAG, "Invalid recording or unable to lock storage");
    return -1;
  }

  wav_header_t wav_header = wav_header_template;
  wav_header.data_chunk.subchunk_size = (uint32_t)pcm_length;
  wav_header.descriptor_chunk.chunk_size = (uint32_t)(pcm_length + sizeof(wav_header_t) - 8);

 
  char unique_name[64];
  if (!make_unique_recording_name(filespec, unique_name, sizeof(unique_name))) {
    ESP_LOGE(STORAGE_TAG, "No unique recording name available for %s", filespec);
    unlock_storage();
    return -1;
  }

  char temporary_path[160];
  char recording_path[160];
  int temporary_length = snprintf(temporary_path, sizeof(temporary_path),
                                  SD_MOUNT_POINT "/%s.part", unique_name);
  int recording_length = snprintf(recording_path, sizeof(recording_path),
                                  SD_MOUNT_POINT "/%s.wav", unique_name);
  if (temporary_length < 0 || temporary_length >= (int)sizeof(temporary_path) ||
      recording_length < 0 || recording_length >= (int)sizeof(recording_path)) {
    ESP_LOGE(STORAGE_TAG, "Recording path is too long");
    unlock_storage();
    return -1;
  }

  FILE *recording = fopen(temporary_path, "wb");
  if (recording == NULL) {
    ESP_LOGE(STORAGE_TAG, "Failed to open %s: errno %d", temporary_path, errno);
    unlock_storage();
    return -1;
  }

  bool write_ok = fwrite(&wav_header, 1, sizeof(wav_header), recording) ==
                  sizeof(wav_header) &&
                  fwrite(pcm_data, 1, pcm_length, recording) == pcm_length;
  if (fclose(recording) != 0) {
    write_ok = false;
  }

  int result = -1;
  if (write_ok && rename(temporary_path, recording_path) == 0) {
    result = 0;
  } else {
    ESP_LOGE(STORAGE_TAG, "Failed to complete recording %s: errno %d",
             recording_path, errno);
    unlink(temporary_path);
  }
  unlock_storage();
  return result;
}



/**
 * @brief Reads the voltage from GPIO 8 (ADC1 Channel 7) in millivolts.
 * @return int Voltage in mV, or -1 on failure.
 */
static int s_battery_load_min_mv = 0;

void battery_load_reset(void)
{
    s_battery_load_min_mv = 0;
}

void battery_load_sample(void)
{
    int pin_mv = read_adc_voltage_gpio8();
    if (pin_mv > 0) {
        int battery_mv = pin_mv * 2;     /*  50/50 divider  */
        if (s_battery_load_min_mv == 0 || battery_mv < s_battery_load_min_mv) {
            s_battery_load_min_mv = battery_mv;
        }
    }
}

int battery_load_min_mv(void)
{
    return s_battery_load_min_mv;
}

int read_adc_voltage_gpio8(void)
{
    int voltage = -1;
    int raw_reading = 0;
    bool cali_enabled = false;

    // 1. Configure the ADC Unit (GPIO 8 is on ADC UNIT 1, Channel 7)
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&init_config, &adc1_handle) != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Failed to initialize ADC1 unit");
        return -1;
    }

    // 2. Configure the ADC Channel
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT, // 12-bit resolution for ESP32-S3
        .atten = ADC_ATTEN_DB_12,         // Allows full-scale voltage up to ~3.1V/3.3V
    };
    if (adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &config) != ESP_OK) {
        ESP_LOGE(STORAGE_TAG, "Failed to configure ADC1 Channel 7");
        adc_oneshot_del_unit(adc1_handle);
        return -1;
    }

    // 3. Set up the Calibration Scheme (Curve Fitting for ESP32-S3)
    adc_cali_handle_t cali_handle = NULL;
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_7,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    // Create the calibration handle using eFuse factory-burned values
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
        cali_enabled = true;
    } else {
        ESP_LOGW(STORAGE_TAG, "Calibration setup failed! Falling back to raw-only translation.");
    }

    // 4. Sample the raw value and convert it to voltage
    if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_7, &raw_reading) == ESP_OK) {
        if (cali_enabled) {
            // Converts raw 12-bit digital values directly to millivolts
            adc_cali_raw_to_voltage(cali_handle, raw_reading, &voltage);
        } else {
            // Crude mathematical fallback if calibration fails: Vout = Dout * Vmax / Dmax
            voltage = (raw_reading * 3300) / 4095; 
        }
    } else {
        ESP_LOGE(STORAGE_TAG, "Failed to read raw ADC data");
    }

    // 5. Clean up allocations to prevent memory leaks
    if (cali_enabled) {
        adc_cali_delete_scheme_curve_fitting(cali_handle);
    }
    adc_oneshot_del_unit(adc1_handle);

    return voltage;
}
