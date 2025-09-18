#include "lcd_camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "esp_timer.h"

#define TAG "lcd_camera"

#define LCD_DISPLAY_EN

#define LCD_H_RES 320
#define LCD_V_RES 240
#define DISPLAY_STREAM_FRAME_RATE 15
#define DISPLAY_SW_QUALITY 80 // 0~100

static esp_lcd_panel_handle_t panel_handle = NULL;
static lcd_camera_config_t user_config;

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_YUV422,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 5,
    .fb_count = 3,
    .grab_mode = CAMERA_GRAB_LATEST,
    .fb_location = CAMERA_FB_IN_PSRAM,
};

#define SWAP_RGB565_BYTES 0
static inline int clamp(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void yuv422_to_rgb565(const uint8_t *src, uint8_t *dst, int width, int height) {
    const uint8_t *s = src;
    uint8_t *d = dst;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x += 2) {
            int Y0 = s[0], U = s[1], Y1 = s[2], V = s[3];
            s += 4;
            int C0 = Y0-16, C1 = Y1-16, D=U-128, E=V-128;
            int R0=(298*C0+409*E+128)>>8, G0=(298*C0-100*D-208*E+128)>>8, B0=(298*C0+516*D+128)>>8;
            int R1=(298*C1+409*E+128)>>8, G1=(298*C1-100*D-208*E+128)>>8, B1=(298*C1+516*D+128)>>8;
            R0=clamp(R0); G0=clamp(G0); B0=clamp(B0);
            R1=clamp(R1); G1=clamp(G1); B1=clamp(B1);
            uint16_t p0 = ((R0>>3)<<11)|((G0>>2)<<5)|(B0>>3);
            uint16_t p1 = ((R1>>3)<<11)|((G1>>2)<<5)|(B1>>3);
            if(SWAP_RGB565_BYTES){
                d[0]=(uint8_t)(p0&0xFF); d[1]=(uint8_t)(p0>>8);
                d[2]=(uint8_t)(p1&0xFF); d[3]=(uint8_t)(p1>>8);
            } else {
                d[0]=(uint8_t)(p0>>8); d[1]=(uint8_t)(p0&0xFF);
                d[2]=(uint8_t)(p1>>8); d[3]=(uint8_t)(p1&0xFF);
            }
            d += 4;
        }
    }
}

static volatile bool fb_used = false;

// 精准帧间隔延时
static inline void delay_frame_us(int frame_interval_us, uint64_t *last_time) {
    uint64_t now = esp_timer_get_time();
    int64_t delta = frame_interval_us - (now - *last_time);
    if(delta > 0){
        vTaskDelay(pdMS_TO_TICKS(delta/1000));
    }
    *last_time = esp_timer_get_time();
}

static void display_task(void *arg) {
    const int lines = 40;
    uint64_t last_time = esp_timer_get_time();
    const int frame_interval_us = 1000000 / DISPLAY_STREAM_FRAME_RATE;

    while(1){
        if(fb_used){
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        fb_used = true;

        camera_fb_t *fb = esp_camera_fb_get();
        if(!fb){
            fb_used = false;
            delay_frame_us(frame_interval_us, &last_time);
            continue;
        }

        if(fb->format == PIXFORMAT_YUV422){
            size_t rgb_len = fb->width*fb->height*2;
            uint8_t *rgb_buf = heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!rgb_buf){
                esp_camera_fb_return(fb);
                fb_used = false;
                delay_frame_us(frame_interval_us, &last_time);
                continue;
            }
            yuv422_to_rgb565(fb->buf, rgb_buf, fb->width, fb->height);
            for(int i=0;i<fb->height/lines;i++){
                esp_lcd_panel_draw_bitmap(panel_handle,0,i*lines,fb->width,(i+1)*lines,
                                          rgb_buf+i*lines*fb->width*2);
            }
            heap_caps_free(rgb_buf);
        } else if(fb->format==PIXFORMAT_RGB565){
            for(int i=0;i<fb->height/lines;i++){
                esp_lcd_panel_draw_bitmap(panel_handle,0,i*lines,fb->width,(i+1)*lines,
                                          fb->buf+i*lines*fb->width*2);
            }
        } else {
            ESP_LOGE(TAG,"Unsupported FB format:%d",fb->format);
        }

        esp_camera_fb_return(fb);
        fb_used=false;
        delay_frame_us(frame_interval_us, &last_time);
    }
}

static void stream_task(void *arg){
    uint64_t last_time = esp_timer_get_time();
    const int frame_interval_us = 1000000 / DISPLAY_STREAM_FRAME_RATE;

    while(1){
        if(user_config.send_jpeg && user_config.stream_flag && user_config.stream_flag()){
            if(fb_used){
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            fb_used = true;
            camera_fb_t *fb = esp_camera_fb_get();
            if(!fb){
                fb_used=false;
                delay_frame_us(frame_interval_us,&last_time);
                continue;
            }

            uint8_t *jpeg_buf=NULL;
            size_t jpeg_len=0;

            if(fb->format==PIXFORMAT_YUV422 || fb->format==PIXFORMAT_RGB565){
                if(frame2jpg(fb,DISPLAY_SW_QUALITY,&jpeg_buf,&jpeg_len)){
                    user_config.send_jpeg(jpeg_buf,jpeg_len,1);
                    free(jpeg_buf);
                } else {
                    ESP_LOGW(TAG,"SW JPEG encode failed");
                }
            } else {
                ESP_LOGE(TAG,"Unsupported FB format:%d",fb->format);
            }

            esp_camera_fb_return(fb);
            fb_used=false;
        }
        delay_frame_us(frame_interval_us,&last_time);
    }
}

esp_err_t lcd_camera_start(const lcd_camera_config_t *config){
    if(config==NULL || config->send_jpeg==NULL || config->stream_flag==NULL){
        ESP_LOGE(TAG,"Invalid lcd_camera_config! send_jpeg or stream_flag is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    user_config = *config;
    ESP_ERROR_CHECK(esp_camera_init(&camera_config));
    vTaskDelay(pdMS_TO_TICKS(300));
#ifdef LCD_DISPLAY_EN
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    panel_handle = audio_board_lcd_init(set,NULL);
    xTaskCreatePinnedToCore(display_task,"lcd_display",8192,NULL,4,NULL,0);
#endif
    xTaskCreatePinnedToCore(stream_task,"stream_task",8192,NULL,5,NULL,1);

    return ESP_OK;
}
