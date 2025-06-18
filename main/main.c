#include "string.h"
#include "softap_sta.h"
#include "lcd_camera.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"


void app_main(void)
{
    // 1. 统一系统初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. 初始化Wi-Fi (AP模式)
    wifi_ap_udp_init();
    
    // 3. 初始化摄像头和LCD显示
    my_lcd_camera_init();
}

// 创建图像队列 (用于UDP传输) - my_lcd_camera_task() while前
// image_queue = xQueueCreate(3, sizeof(camera_fb_t *));
// if (!image_queue) {
//     ESP_LOGE(TAG, "Failed to create image queue");
//     vTaskDelete(NULL);
//     return;
// }

// 启动UDP传输任务
// xTaskCreate(udp_transmit_task, "udp_transmit", 4096, (void *)image_queue, 5, NULL);
// ESP_LOGI(TAG, "UDP transmit task started");

// 将图像发送到UDP传输队列 - my_lcd_camera_task() esp_camera_fb_return(pic); 后
// if (xQueueSend(image_queue, &pic, 0) != pdTRUE) {
//     ESP_LOGW(TAG, "UDP queue full, discarding frame");
//     esp_camera_fb_return(pic);
// }