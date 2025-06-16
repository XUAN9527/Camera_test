/* WiFi SoftAP with UDP Server Example */
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "esp_mac.h"  // 添加这个头文件以支持 MAC 格式化

/* AP Configuration - From Kconfig */
#define AP_SSID            CONFIG_ESP_WIFI_AP_SSID
#define AP_PASSWORD        CONFIG_ESP_WIFI_AP_PASSWORD
#define AP_CHANNEL         CONFIG_ESP_WIFI_AP_CHANNEL
#define MAX_STA_CONN       CONFIG_ESP_MAX_STA_CONN_AP

/* UDP Configuration - From Kconfig */
#define UDP_PORT           CONFIG_UDP_STREAM_PORT

/* Logging Tags */
static const char *TAG_AP = "WiFi-AP";
static const char *TAG_UDP = "UDP-Server";

/* Event Group Bits */
#define AP_STARTED_BIT     BIT0
static EventGroupHandle_t s_wifi_event_group;

/* WiFi Event Handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG_AP, "Access Point Started");
                xEventGroupSetBits(s_wifi_event_group, AP_STARTED_BIT);
                break;
                
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                // 修复 MAC 地址格式化问题
                ESP_LOGI(TAG_AP, "Station " MACSTR " joined, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }
            
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                // 修复 MAC 地址格式化问题
                ESP_LOGI(TAG_AP, "Station " MACSTR " left, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }
            
            default:
                break;
        }
    }
}

/* Initialize WiFi Access Point */
static void wifi_init_softap(void)
{
    // Create default AP network interface
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG_AP, "Failed to create default AP netif");
        return;
    }

    // Configure AP settings
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASSWORD,
            .channel = AP_CHANNEL,
            .max_connection = MAX_STA_CONN,
            .authmode = (strlen(AP_PASSWORD) == 0) ? 
                        WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false
            }
        }
    };

    // Set AP configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_LOGI(TAG_AP, "AP Config: SSID=%s, Channel=%d", AP_SSID, AP_CHANNEL);
}

/* UDP Server Task */
static void udp_server_task(void *pvParameters)
{
    ESP_LOGI(TAG_UDP, "Waiting for AP to start...");
    xEventGroupWaitBits(s_wifi_event_group, AP_STARTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG_UDP, "Starting UDP server on port %d", UDP_PORT);

    int sock;
    struct sockaddr_in server_addr;
    char rx_buffer[128];
    char tx_buffer[128];
    char client_ip[16];
    
    // Create UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ESP_LOGE(TAG_UDP, "Socket creation failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind socket
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG_UDP, "Socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG_UDP, "UDP server listening on port %d", UDP_PORT);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (1) {
        // Receive data from clients
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&client_addr, &client_len);
        
        if (len < 0) {
            ESP_LOGE(TAG_UDP, "Receive failed: errno %d", errno);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        
        // Null-terminate received data
        rx_buffer[len] = '\0';
        
        // Get client IP
        inet_ntoa_r(client_addr.sin_addr.s_addr, client_ip, sizeof(client_ip));
        
        ESP_LOGI(TAG_UDP, "Received %d bytes from %s:%d", len, client_ip, ntohs(client_addr.sin_port));
        ESP_LOGI(TAG_UDP, "Data: %s", rx_buffer);
        
        // Prepare response
        int tx_len = snprintf(tx_buffer, sizeof(tx_buffer), "Echo: %s", rx_buffer);
        if (tx_len < 0 || tx_len >= sizeof(tx_buffer)) {
            ESP_LOGE(TAG_UDP, "Failed to format response");
            continue;
        }
        
        // Send response
        if (sendto(sock, tx_buffer, tx_len, 0, 
                  (struct sockaddr *)&client_addr, client_len) < 0) {
            ESP_LOGE(TAG_UDP, "Send failed: errno %d", errno);
        }
    }
    
    // Cleanup (should never reach here)
    close(sock);
    vTaskDelete(NULL);
}

/* Main WiFi and UDP Initialization */
void wifi_ap_udp_init(void)
{
    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG_AP, "Failed to create event group");
        return;
    }
    
    // Register WiFi event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                      &wifi_event_handler, NULL, NULL));
    
    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    
    // Configure AP
    wifi_init_softap();
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Start UDP server task
    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
}

/* Application Main Function */
// void app_main(void)
// {
//     // Initialize NVS
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);
    
//     // Initialize networking stack
//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());
    
//     // Start combined WiFi AP and UDP server
//     wifi_ap_udp_init();
    
//     ESP_LOGI("MAIN", "System initialized. AP SSID: %s", AP_SSID);
// }