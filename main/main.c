#include <stdio.h>
#include "esp_log.h"
#include "lcd_camera.h"
#include "wifi_softap.h"
#include "rtsp_server.h"
#include "http_server.h"

#define TAG "main"

// MJPEG 推送回调函数
static void send_jpeg_callback(uint8_t *jpeg, size_t len) {
    // rtsp_server_send_frame(jpeg, len);
	http_server_send_frame(jpeg, len);
}

void app_main(void) {
    
    wifi_init_softap();		// 初始化 SoftAP
	http_server_start();    // 启动HTTP服务器
    rtsp_server_start();	// 初始化 RTSP Server

    // 启动摄像头+LCD，注册 MJPEG 推送函数
    lcd_camera_config_t lcd_config = {
        .send_jpeg = send_jpeg_callback
    };

    esp_err_t ret = lcd_camera_start(&lcd_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start lcd_camera");
    }
}
