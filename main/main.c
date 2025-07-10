#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "lcd_camera.h"
#include "wifi_softap.h"
#include "rtsp_server.h"
#include "http_server.h"

#define TAG "main"

// #define HTTP_STREAM

// MJPEG 推送回调函数
static void send_jpeg_callback(uint8_t *jpeg, size_t len) {
#ifdef HTTP_STREAM
	http_server_send_frame(jpeg, len);
#else
    rtsp_server_send_frame(jpeg, len);
#endif
}

static bool stream_flag_callback(void) {
#ifdef HTTP_STREAM
	return http_stream_flag_get();
#else
    return rtsp_stream_flag_get();
#endif	
}

void app_main(void) {
    
    wifi_init_softap();		// 初始化 SoftAP
#ifdef HTTP_STREAM
	http_server_start();    // 启动HTTP服务器
#else
    rtsp_server_start();	// 初始化 RTSP Server
#endif

    // 启动摄像头+LCD，注册 MJPEG 推送函数
    lcd_camera_config_t lcd_config = {
		.stream_flag = stream_flag_callback,
        .send_jpeg = send_jpeg_callback
    };

    esp_err_t ret = lcd_camera_start(&lcd_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start lcd_camera");
    }
}
