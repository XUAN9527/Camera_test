#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "lcd_camera.h"
#include "wifi_softap.h"
#include "rtsp_server.h"
#include "http_server.h"
#include "web_mjpeg_server.h"

#define TAG "main"

#define PUSH_STREAM_MODE	2

// MJPEG 推送回调函数
static void send_jpeg_callback(uint8_t *jpeg, size_t len, uint8_t type) {
#if PUSH_STREAM_MODE == 1
	http_server_send_frame(jpeg, len);
#elif PUSH_STREAM_MODE == 2
    rtsp_server_send_frame(jpeg, len, type);
#elif PUSH_STREAM_MODE == 3
	web_mjpeg_server_send_jpeg(jpeg, len);
#endif
}

static bool stream_flag_callback(void) {
#if PUSH_STREAM_MODE == 1
	return http_stream_flag_get();
#elif PUSH_STREAM_MODE == 2
    return rtsp_stream_flag_get();
#elif PUSH_STREAM_MODE == 3	
	return web_mjpeg_server_is_client_connected();
#endif	
}

void app_main(void) {
    
    wifi_user_init();		// 初始化 SoftAP
#if PUSH_STREAM_MODE == 1
	http_server_start();    // 启动HTTP服务器
#elif PUSH_STREAM_MODE == 2
    rtsp_server_start();	// 初始化 RTSP Server
#elif PUSH_STREAM_MODE == 3	
	web_mjpeg_server_start();	// 初始化 WEB Server
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
