#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_system.h"
#include "wifi_softap.h"

// 定义是否使用 WiFi STA 模式，否则使用 SoftAP 模式
#define USE_WIFI_STA 1

static const char *TAG = "WiFi_Module";

// AP 模式下记录连接客户端 IP
esp_ip4_addr_t latest_client_ip;

// ====== STA 模式配置 ======
#define WIFI_STA_SSID      "Breoguest"
#define WIFI_STA_PASSWORD  "www.breo.com"

// ====== AP 模式配置 ======
#define WIFI_AP_SSID       "ESP32_CAM"
#define WIFI_AP_PASSWORD   "12345678"

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
#if !USE_WIFI_STA
    // AP 模式下，记录客户端 IP
    if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        latest_client_ip = event->ip;
        ESP_LOGI(TAG, "Client IP assigned: %s", ip4addr_ntoa((const ip4_addr_t *)&latest_client_ip));
		ESP_LOGI(TAG, "Client RTSP access: rtsp://%s:554/mjpeg/1", ip4addr_ntoa((const ip4_addr_t *)&latest_client_ip));
    }
#else
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, trying to connect...");
        esp_wifi_connect();
    }

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
		ESP_LOGW(TAG, "WiFi disconnect reason: %d", disconn->reason);
		esp_wifi_connect(); // 尝试自动重连
	}

	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WiFi", "Got IP: %s", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
		ESP_LOGI(TAG, "RTSP address for client: rtsp://%s:554/mjpeg/1", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
    }
#endif
}

void wifi_user_init() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if USE_WIFI_STA
    // STA 模式
    esp_netif_create_default_wifi_sta();
#else
    // AP 模式
    esp_netif_create_default_wifi_ap();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
#if USE_WIFI_STA
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
#else
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                        &wifi_event_handler, NULL, NULL));
#endif
	

#if USE_WIFI_STA
    // STA 模式配置
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_STA_SSID,
            .password = WIFI_STA_PASSWORD,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
			.scan_method = WIFI_ALL_CHANNEL_SCAN,
    		.bssid_set = false,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started. Connecting to SSID: %s, PASSWORD: %s", WIFI_STA_SSID, WIFI_STA_PASSWORD);
	ESP_LOGI(TAG, "Waiting for WiFi to connect...");
#else
    // AP 模式配置
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASSWORD,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP started. SSID: %s", WIFI_AP_SSID);
#endif
}
