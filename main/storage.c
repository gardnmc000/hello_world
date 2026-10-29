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
#include <fcntl.h>
#include "freertos/semphr.h"

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
extern struct BW_CONFIG bw_config;
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
     

static bool send_all(int fd, const void *data, size_t length) {
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

static bool receive_exact(int fd, void *data, size_t length) {
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

static bool connect_with_timeout(int fd, const struct sockaddr *address, socklen_t address_length) {
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

int socket_send(char *filename) {
    static const uint8_t signature[8] = {0x19, 0x57, 0x55, 0xaa, 0xc0, 0x03, 0xf0, 0x0f};
    struct timeval io_timeout = {.tv_sec = 15, .tv_usec = 0};
    int result = -1;
    int packet_number = 0;
    int offset = 0;
    uint16_t clip_len;
    uint16_t clip_interval;
    uint16_t freq_slope;
    uint16_t vol_threshold;
    uint16_t reserved_1;
    uint16_t reserved_2;

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

    if (!send_all(sock, signature, sizeof(signature)) || 
        !receive_exact(sock, rxbuffer, 16) ||
        memcmp(rxbuffer, "CFG_", 4) != 0) 
    {
        ESP_LOGE(STORAGE_TAG, "Server did not provide CFG_");
        goto cleanup;
    }
    
    memcpy(&clip_len, &rxbuffer[4], 2);
    memcpy(&clip_interval, &rxbuffer[6], 2);
    memcpy(&freq_slope, &rxbuffer[8], 2);
    memcpy(&vol_threshold, &rxbuffer[10], 2);
    memcpy(&reserved_1, &rxbuffer[12], 2);
    memcpy(&reserved_2, &rxbuffer[14], 2);
    bw_config.clip_len = clip_len;
    bw_config.clip_interval = clip_interval;
    bw_config.freq_slope = freq_slope;
    bw_config.vol_threshold = vol_threshold;
    bw_config.reserved_1 = reserved_1;
    bw_config.reserved_2 = reserved_2;
    
    store_config(bw_config);        

    size_t filename_length = strlen(filename);
    if (filename_length >= 64 ||
        !send_all(sock, filename, filename_length) ||
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
        if (packet_number >= 500 || !send_all(sock, txbuffer, (size_t)bytes_read)) {
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    result = 0;

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
        strcmp(extension, ".wav") == 0) {
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

  char temporary_path[160];
  char recording_path[160];
  int temporary_length = snprintf(temporary_path, sizeof(temporary_path),
                                  SD_MOUNT_POINT "/%s.part", filespec);
  int recording_length = snprintf(recording_path, sizeof(recording_path),
                                  SD_MOUNT_POINT "/%s.wav", filespec);
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


int store_config(struct BW_CONFIG cfg) {
 
  char cfg_path[32];

  if (!lock_storage()) {
    ESP_LOGE(STORAGE_TAG, "Unable to lock storage for config write");
    return -1;
  }
  if (cfg.clip_len == 0 || cfg.clip_interval == 0 || cfg.freq_slope == 0 || cfg.vol_threshold == 0) {
    ESP_LOGE(STORAGE_TAG, "Invalid configuration values provided");
    unlock_storage();
    return -1;
  }
  if (cfg.reserved_1 != 0 || cfg.reserved_2 != 0) {
    ESP_LOGW(STORAGE_TAG, "Reserved fields should be zero; received reserved_1: %d, reserved_2: %d", cfg.reserved_1, cfg.reserved_2);
  }

  snprintf(cfg_path, sizeof(cfg_path),  SD_MOUNT_POINT "/config.cfg");

  FILE *config_file = fopen(cfg_path, "wb");
  if (config_file == NULL) {
    ESP_LOGE(STORAGE_TAG, "Failed to open config file for writing: errno %d", errno);
    unlock_storage();
    return -1;
  }

  int write_bytes = fwrite(&cfg, 1, sizeof(cfg), config_file);
  if (fclose(config_file) != 0) {
    write_bytes = 0;
  }
  unlock_storage();

  ESP_LOGI(STORAGE_TAG, "store_config() wrote clip_len: %d, clip_interval: %d, freq_slope: %d, vol_threshold: %d, reserved_1: %d, reserved_2: %d", cfg.clip_len, cfg.clip_interval, cfg.freq_slope, cfg.vol_threshold, cfg.reserved_1, cfg.reserved_2);
    
  return (write_bytes);  
}