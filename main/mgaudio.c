/*
 * mgaudio.c
 *
 *  Created on: 7 Apr 2025
 *      Author: Mike.Gardner
 */
#include "mgaudio.h"
#include "driver/i2s_common.h"
#include "math.h"
#include "storage.h"
#include <stdlib.h>
#include "esp_psram.h"
#include "base.h"



i2s_std_config_t config = {
    .clk_cfg = {
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .sample_rate_hz = 44100,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256
    },
    .gpio_cfg = {
        .bclk = CONFIG_I2S_BCK_PIN,
        .din = CONFIG_I2S_DATA_PIN,
        .ws = CONFIG_I2S_LRCK_PIN,
        .dout = I2S_PIN_NO_CHANGE,
        .mclk = I2S_PIN_NO_CHANGE,
        .invert_flags = {
            .bclk_inv = false,
            .mclk_inv = false,
            .ws_inv = false ,           
        },
    },
    .slot_cfg = {
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
        .slot_mask = I2S_STD_SLOT_LEFT,
        .slot_mode = I2S_SLOT_MODE_MONO,
        .bit_shift = true,
        .ws_pol = false,
        .ws_width = 32,
        .bit_order_lsb = false,
        
//      .msb_right = false
    }
};

static const char *TAG = "AUDIO";
int32_t i2s_buff[SAMPLE_BUFFER_SIZE];
uint16_t * p_out_buff;
uint8_t right_shift = 10;

/*  @brief  Initialise and enable the I2S Channel  */
void i2s_start(i2s_chan_handle_t rx_handle) {
    /* Initialise the channel */
    i2s_channel_init_std_mode(rx_handle, &config);
    /* Before reading data, start the RX channel first */
    i2s_channel_enable(rx_handle);
}

/*  @brief  Absolute difference of two samples, computed unsigned so a full-scale swing cannot overflow  */
static inline uint32_t abs_diff_i32(int32_t a, int32_t b) {
    return (a > b) ? (uint32_t) a - (uint32_t) b : (uint32_t) b - (uint32_t) a;
}

/*  @brief  Magnitude of a sample, safe for INT32_MIN  */
static inline uint32_t abs_i32(int32_t a) {
    return (a < 0) ? 0u - (uint32_t) a : (uint32_t) a;
}

/*  @brief  Scans the cI2S buffer contents and accumulates the number of high frequency portions of sound  */
uint32_t get_max_slope(i2s_chan_handle_t rx_handle)  {

    /*  Required number of bytes per sample is 4 times the number of samples, as the i2s output is 32 bits  */
    uint32_t bytes_to_read = SAMPLE_BUFFER_SIZE * sizeof(int32_t);
    uint32_t bytes_read = 0, samples_read = 0, slope =0, max_slope = 0, attempt = 0, accum_slope = 0, max_volume = 0;
    int32_t ticks_to_wait = 20,  cur_magnitude = 0,  prv_magnitude = 0;
    int  index;
    /*  HIST_DEPTH defines the cut-off duration of the high frequency block within the clip  */
    max_volume = 0;
    for (attempt = 0; attempt < HIST_DEPTH; attempt ++) {
        max_slope = 0;
        /*  read the entire contents of the I2S device buffer (typically 4096 32-bit words)  */
        i2s_channel_read(rx_handle, i2s_buff, bytes_to_read, (size_t *) &bytes_read, ticks_to_wait);
        samples_read = bytes_read / sizeof(int32_t);
        /*  Compare each element in the sample with the previous element in the sample to determine the slope  */
        for (index=1; index < samples_read; index++) {                  
            cur_magnitude = i2s_buff[index];
            prv_magnitude = i2s_buff[index-1];
            slope = abs_diff_i32(cur_magnitude, prv_magnitude);

            /*  Is this slope a record breaker?  */
            if (slope > max_slope) {
                max_slope = slope;
            }

            if (abs_i32(cur_magnitude) > max_volume) {
                max_volume = abs_i32(cur_magnitude);
            }
        }
        /*  Saturate rather than wrap, so the loudest input cannot produce a tiny score  */
        accum_slope = (max_slope > UINT32_MAX - accum_slope) ? UINT32_MAX : accum_slope + max_slope;
        //ESP_LOGI(TAG, "Max Delta:  %d", (int) max_slope);
    }

    /*   007da000, Right Shift: 9  */
    if (max_volume >= 0x02000000) {
        right_shift = 12;
    } else {
        if (max_volume >= 0x01000000) {
            right_shift = 11;
        } else {
            if (max_volume >= 0x00800000) {
                right_shift = 10;
            } else {
                right_shift = 9;
            }
        }
    }
    ESP_LOGI(TAG, "Acc Slope %08x, Acc DELTA: %d, Max Vol:  %08x, Right Shift: %d", (int) accum_slope, (int) (accum_slope / BW_DELTA_THRESHOLD), (int) max_volume, (int) right_shift);
    //dft_forward((int32_t *) &i2s_buff);
    return (accum_slope / BW_DELTA_THRESHOLD); 
}

/*  @brief  Builds an entire 16 bit PCM wav file  */
void build_clip16(i2s_chan_handle_t rx_handle, const char * filespec)  {

    /*  Required number of bytes per sample is 4 times the number of samples, as the i2s output is 32 bits  */
    uint32_t bytes_to_read = SAMPLE_BUFFER_SIZE * sizeof(int32_t);
    uint32_t bytes_read = 0;
    int32_t ticks_to_wait = 20;
    int counter = 0,  clip_size;
    bool read_active = true;
    /*  Typically = 4096 / 44100 = 0.0928 seconds or 10.8 samples per second  */
    float seconds_per_sample = (float) ((float) SAMPLE_BUFFER_SIZE / config.clk_cfg.sample_rate_hz);
    /*  Typically 5 seconds  */
    float required_clip_length = BW_DEFAULT_CLIP_LENGTH;
    /*  Typically  = 54 ( 5 seconds * 10.8 = 53.8 rounded up)  */
    float samples_per_clip = 1 + (required_clip_length / seconds_per_sample);
    /*  Convert to int for later calculations  */
    clip_size = (int) samples_per_clip;
    /*  Note:  maximum length is 64 characters including file path etc  */ 
    int32_t index;
    /*  Typically 4096 x 2 x 54  Bytes  */
    size_t clip_byte_size = SAMPLE_BUFFER_SIZE * sizeof(int16_t) *
                            ((size_t)clip_size + 1);
    /*  Set aside appx 44k of RAM for capturing the sample entirely in SPI RAM  */    
    p_out_buff = calloc(clip_byte_size, 1);
    if (p_out_buff != NULL ) {
        // ESP_LOGI(TAG, "Output buffer allocated: %08x  bytes: %04x ", (int) &p_out_buff,  (int) clip_byte_size);
    }
    else {
        ESP_LOGI(TAG, "Output buffer allocation failed");  
        return;      
    }

    esp_err_t err;

    /*  Capture nn blocks from the I2S interface  */
    while (read_active) {
        /*  read from the I2S device and immediately write to the RAM as is  */
        err = i2s_channel_read(rx_handle, i2s_buff, bytes_to_read, (size_t *) &bytes_read, ticks_to_wait);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read from I2S: %s", esp_err_to_name(err));
            break;
        }
        if (bytes_read != bytes_to_read) {
            ESP_LOGE(TAG, "Bytes Read:  %08d", (int) bytes_read);
        }
        
        /*  Transcode the entire block from 32-bit to 16-bit on the fly.*/
        /*  (32- bit) b31 : b0  =>  (16-bit)  b15 : b0   note: sign is preserved b31:b23 are lost  */
        for (index = 0; index < SAMPLE_BUFFER_SIZE; index ++) {
            p_out_buff[counter * SAMPLE_BUFFER_SIZE + index] = (i2s_buff[index] >> right_shift) & 0xffff;
        }
        /*  Proceed to the next block from the I2S module  */
        if (counter < clip_size) {
            counter++;
        }
        /*  All (54) blocks have been captured,  move on  */
        else {
            read_active = false;
        }
        
    }
    
    //ESP_LOGI(TAG, "Capture complete");
    if (save_recording(filespec, p_out_buff, clip_byte_size) != 0) {
        ESP_LOGE(TAG, "Failed to save captured clip %s", filespec);
    }
    else {
        ESP_LOGI(TAG, "Captured: %s", filespec);
    }
    free(p_out_buff);
    p_out_buff = NULL;
    
        //        for (index = 0; index < SAMPLE_BUFFER_SIZE; index++) {
        //            printf("%08x\t\t\t\t%04x\r\n", (int) i2s_buff[index], (unsigned int) p_out_buff[index] );
        //            if (p_out_buff[index] == 0) {
        //                ESP_LOGI(TAG, "Zero Value");   
        //            }
        //        }

}
