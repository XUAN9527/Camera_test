// lcd_camera_rtsp.c

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "esp_lcd_panel_ops.h"
#include "board.h"
#include "audio_mem.h"
#include "esp_heap_caps.h"

#define TAG "LCD_CAM_RTSP"
#define FRAME_QUEUE_LEN 2
#define LCD_H_RES 320
#define LCD_V_RES 240
#define MAX_UDP_PAYLOAD 1400
#define RTSP_PORT 554

// 需要根据你的板子定义正确摄像头引脚
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
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_LATEST,
    .fb_location = CAMERA_FB_IN_PSRAM,
};

static QueueHandle_t frame_queue;
static esp_lcd_panel_handle_t panel_handle;
static esp_ip4_addr_t latest_client_ip;
static uint16_t client_rtp_port = 0;
static bool rtsp_streaming = false;
static int rtsp_client_socket = -1;

// --- WiFi SoftAP 初始化 ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        latest_client_ip = event->ip;
        ESP_LOGI(TAG, "Client IP assigned: %s", ip4addr_ntoa((const ip4_addr_t*)&latest_client_ip));
    }
}

static void wifi_init_softap() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_CAM",
            .password = "12345678",
            .ssid_len = strlen("ESP32_CAM"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP started. SSID: %s", wifi_config.ap.ssid);
}

// --- RTSP 简易服务器代码 ---
static int parse_client_rtp_port(const char *setup_request)
{
    const char *p = strstr(setup_request, "client_port=");
    if (!p) return 0;
    int rtp_port = 0;
    sscanf(p, "client_port=%d", &rtp_port);
    return rtp_port;
}

static void send_rtsp_response(int sock, const char *response)
{
    send(sock, response, strlen(response), 0);
}

static void rtsp_server_task(void *arg)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buf[1024];
    int len;

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create RTSP socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(RTSP_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);

    while (1) {
        rtsp_client_socket = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (rtsp_client_socket < 0) {
            ESP_LOGE(TAG, "Accept failed");
            continue;
        }

        ESP_LOGI(TAG, "RTSP client connected");

        rtsp_streaming = false;
        client_rtp_port = 0;

        while ((len = recv(rtsp_client_socket, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[len] = 0;
            ESP_LOGI(TAG, "RTSP request:\n%s", buf);

            if (strstr(buf, "OPTIONS")) {
                const char *resp = 
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: 1\r\n"
                    "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n";
                send_rtsp_response(rtsp_client_socket, resp);
            }
            else if (strstr(buf, "DESCRIBE")) {
                const char *sdp = 
                    "v=0\r\n"
                    "o=- 0 0 IN IP4 0.0.0.0\r\n"
                    "s=ESP32-CAM MJPEG Stream\r\n"
                    "m=video 0 RTP/AVP 26\r\n"
                    "a=control:streamid=0\r\n"
                    "a=mimetype:string;\"video/MJPEG\"\r\n";

                char resp[512];
                snprintf(resp, sizeof(resp),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: 2\r\n"
                         "Content-Type: application/sdp\r\n"
                         "Content-Length: %d\r\n\r\n%s",
                         (int)strlen(sdp), sdp);
                send_rtsp_response(rtsp_client_socket, resp);
            }
            else if (strstr(buf, "SETUP")) {
                int port = parse_client_rtp_port(buf);
                if (port > 0) {
                    client_rtp_port = port;
                    ESP_LOGI(TAG, "Client RTP port: %d", client_rtp_port);

                    char resp[256];
                    snprintf(resp, sizeof(resp),
                             "RTSP/1.0 200 OK\r\n"
                             "CSeq: 3\r\n"
                             "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=8000-8001\r\n"
                             "Session: 12345678\r\n\r\n",
                             client_rtp_port, client_rtp_port + 1);
                    send_rtsp_response(rtsp_client_socket, resp);
                }
                else {
                    ESP_LOGE(TAG, "Failed to parse client RTP port");
                }
            }
            else if (strstr(buf, "PLAY")) {
                const char *resp =
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: 4\r\n"
                    "Session: 12345678\r\n\r\n";
                send_rtsp_response(rtsp_client_socket, resp);
                rtsp_streaming = true;
                ESP_LOGI(TAG, "RTSP PLAY received, start streaming");
            }
            else if (strstr(buf, "TEARDOWN")) {
                const char *resp =
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: 5\r\n"
                    "Session: 12345678\r\n\r\n";
                send_rtsp_response(rtsp_client_socket, resp);
                rtsp_streaming = false;
                break;
            }
            else {
                const char *resp = "RTSP/1.0 200 OK\r\n\r\n";
                send_rtsp_response(rtsp_client_socket, resp);
            }
        }

        ESP_LOGI(TAG, "RTSP client disconnected");
        close(rtsp_client_socket);
        rtsp_client_socket = -1;
        client_rtp_port = 0;
        rtsp_streaming = false;
    }
}

// --- RTP 发送 ---
static void send_mjpeg_rtp(uint8_t *jpeg, size_t len) {
    if (latest_client_ip.addr == 0 || client_rtp_port == 0 || !rtsp_streaming) return;

    struct sockaddr_in client_rtp_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(client_rtp_port),
        .sin_addr.s_addr = latest_client_ip.addr
    };

    static uint16_t seq = 0;
    static uint32_t ssrc = 0x12345678;
    uint32_t ts = xTaskGetTickCount() * 90;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    for (size_t offset = 0; offset < len;) {
        size_t chunk = (len - offset) > (MAX_UDP_PAYLOAD - 12) ? (MAX_UDP_PAYLOAD - 12) : (len - offset);
        uint8_t pkt[MAX_UDP_PAYLOAD];
        pkt[0] = 0x80;
        pkt[1] = (offset + chunk >= len ? 0x80 : 0) | 26;  // 26 = MJPEG
        pkt[2] = (seq >> 8) & 0xFF; pkt[3] = seq & 0xFF;
        pkt[4] = (ts >> 24) & 0xFF; pkt[5] = (ts >> 16) & 0xFF; pkt[6] = (ts >> 8) & 0xFF; pkt[7] = ts & 0xFF;
        pkt[8] = (ssrc >> 24) & 0xFF; pkt[9] = (ssrc >> 16) & 0xFF; pkt[10] = (ssrc >> 8) & 0xFF; pkt[11] = ssrc & 0xFF;
        memcpy(pkt + 12, jpeg + offset, chunk);

        sendto(sock, pkt, chunk + 12, 0, (struct sockaddr *)&client_rtp_addr, sizeof(client_rtp_addr));
        offset += chunk;
        seq++;
    }

    close(sock);
}

// --- LCD 显示 ---
esp_err_t example_lcd_rgb_draw(esp_lcd_panel_handle_t handle, uint8_t *image) {
    int lines = 40;
    for (int i = 0; i < LCD_V_RES / lines; i++) {
        esp_lcd_panel_draw_bitmap(handle, 0, i * lines, LCD_H_RES, (i + 1) * lines, image + i * lines * LCD_H_RES * 2);
    }
    return ESP_OK;
}

// --- 采集+显示+发送任务 ---
static void display_and_send_task(void *arg) {
    while (1) {
        camera_fb_t *fb = NULL;
        if (xQueueReceive(frame_queue, &fb, portMAX_DELAY) == pdTRUE && fb) {
            example_lcd_rgb_draw(panel_handle, fb->buf);

            uint8_t *jpeg_buf = NULL;
            size_t jpeg_len = 0;
            if (frame2jpg(fb, 60, &jpeg_buf, &jpeg_len)) {
                send_mjpeg_rtp(jpeg_buf, jpeg_len);
                free(jpeg_buf);
            }

            esp_camera_fb_return(fb);
        }
    }
}

// --- 采集任务 ---
static void capture_task(void *arg) {
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (xQueueSend(frame_queue, &fb, 10) != pdTRUE) {
                esp_camera_fb_return(fb);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// --- 初始化入口 ---
void my_lcd_camera_init(void) {
    wifi_init_softap();

    ESP_ERROR_CHECK(esp_camera_init(&camera_config));

    frame_queue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(camera_fb_t *));
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    panel_handle = audio_board_lcd_init(set, NULL);

    xTaskCreatePinnedToCore(display_and_send_task, "display_send", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(rtsp_server_task, "rtsp_server", 8192, NULL, 5, NULL, 1);
}
