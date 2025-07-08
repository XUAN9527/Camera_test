#include "rtsp_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_timer.h"

#define TAG "RTSP_SERVER"
#define RTSP_PORT 554
#define RTP_PAYLOAD_TYPE_MJPEG 26
#define MAX_RTP_PAYLOAD 1500
#define FRAME_RATE 15

static uint32_t latest_client_ip = 0;
static bool rtsp_streaming = false;
static int rtsp_client_socket = -1;
static bool use_tcp_transport = true; // 默认 TCP（VLC 兼容性更高）

static void send_rtsp_response(int sock, int cseq, const char *response) {
    ESP_LOGD(TAG, "Sending RTSP response:\n%s", response);
    send(sock, response, strlen(response), 0);
}

static void rtsp_server_task(void *arg) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buf[2048];
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

        ESP_LOGI(TAG, "RTSP client connected from %s", inet_ntoa(client_addr.sin_addr));
        latest_client_ip = client_addr.sin_addr.s_addr;
        rtsp_streaming = false;
        use_tcp_transport = true;

        while ((len = recv(rtsp_client_socket, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[len] = 0;

            if (buf[0] == '$') {
                ESP_LOGD(TAG, "Received RTCP or interleaved packet, ignored.");
                continue;
            }

            ESP_LOGI(TAG, "RTSP request:\n%.*s", len, buf);

            int cseq = 0;
            const char *cseq_ptr = strstr(buf, "CSeq:");
            if (cseq_ptr) {
                cseq = atoi(cseq_ptr + 5);
            }

            if (strstr(buf, "OPTIONS")) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n"
                         "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n",
                         cseq);
                send_rtsp_response(rtsp_client_socket, cseq, resp);
            } else if (strstr(buf, "DESCRIBE")) {
                const char *sdp =
                    "v=0\r\n"
                    "o=- 0 0 IN IP4 0.0.0.0\r\n"
                    "s=ESP32-CAM Stream\r\n"
                    "m=video 0 RTP/AVP 26\r\n"
                    "c=IN IP4 0.0.0.0\r\n"
                    "a=control:streamid=0\r\n"
                    "a=framerate:15\r\n"
                    "a=rtpmap:26 JPEG/90000\r\n";

                char resp[512];
                snprintf(resp, sizeof(resp),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n"
                         "Content-Type: application/sdp\r\n"
                         "Content-Length: %d\r\n\r\n%s",
                         cseq, (int)strlen(sdp), sdp);
                send_rtsp_response(rtsp_client_socket, cseq, resp);
            } else if (strstr(buf, "SETUP")) {
                if (strstr(buf, "RTP/AVP/TCP")) {
                    use_tcp_transport = true;
                    char resp[256];
                    snprintf(resp, sizeof(resp),
                             "RTSP/1.0 200 OK\r\n"
                             "CSeq: %d\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                             "Session: 12345678\r\n"
                             "Timeout: 60\r\n\r\n",
                             cseq);
                    send_rtsp_response(rtsp_client_socket, cseq, resp);
                } else {
                    use_tcp_transport = false;
                    // UDP fallback: not implemented fully in this version
                    const char *resp =
                        "RTSP/1.0 461 Unsupported Transport\r\n"
                        "CSeq: %d\r\n\r\n";
                    char err[128];
                    snprintf(err, sizeof(err), resp, cseq);
                    send_rtsp_response(rtsp_client_socket, cseq, err);
                }
            } else if (strstr(buf, "PLAY")) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n"
                         "Session: 12345678\r\n"
                         "Range: npt=0.000-\r\n\r\n",
                         cseq);
                send_rtsp_response(rtsp_client_socket, cseq, resp);
                rtsp_streaming = true;
                ESP_LOGI(TAG, "RTSP streaming started");
            } else if (strstr(buf, "TEARDOWN")) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n"
                         "Session: 12345678\r\n\r\n",
                         cseq);
                send_rtsp_response(rtsp_client_socket, cseq, resp);
                rtsp_streaming = false;
                break;
            } else {
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n\r\n", cseq);
                send_rtsp_response(rtsp_client_socket, cseq, resp);
            }
        }

        ESP_LOGI(TAG, "RTSP client disconnected");
        close(rtsp_client_socket);
        rtsp_client_socket = -1;
        rtsp_streaming = false;
    }
}

void rtsp_server_send_frame(uint8_t *jpeg, size_t len) {
    if (!rtsp_streaming || rtsp_client_socket < 0) return;

    static uint16_t seq = 0;
    static uint32_t ssrc = 0x12345678;
    static uint32_t last_frame_time = 0;

    // 帧率控制
    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_frame_time < 1000 / FRAME_RATE) {
        return;
    }
    last_frame_time = now;

    uint32_t ts = now * 90; // 90kHz 时钟（RFC规定）

    // 验证 JPEG 起始标志
    if (len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG frame");
        return;
    }

    ESP_LOGD(TAG, "Sending JPEG frame: %d bytes, seq: %d", len, seq);

    size_t offset = 0;

    // ⚠️ 修复关键参数（确保兼容 VLC）
    uint8_t type = 0;  // baseline JPEG type (标准类型)
    uint8_t q = 255;   // 默认量化表（不传表）
    uint8_t width8 = 320 / 8;  // 以 8 为单位，VLC 必须和实际 JPEG 匹配
    uint8_t height8 = 240 / 8;

    while (offset < len) {
        size_t chunk = (len - offset > MAX_RTP_PAYLOAD - 20) ? MAX_RTP_PAYLOAD - 20 : len - offset;

        uint8_t pkt[4 + 12 + 8 + chunk]; // interleaved + RTP + JPEG header + data
        int i = 0;

        // Interleaved header (TCP over RTSP)
        pkt[i++] = '$';
        pkt[i++] = 0x00; // channel 0 for RTP
        pkt[i++] = ((12 + 8 + chunk) >> 8) & 0xFF;
        pkt[i++] = (12 + 8 + chunk) & 0xFF;

        // RTP header (12 bytes)
        pkt[i++] = 0x80; // Version: 2
        pkt[i++] = ((offset + chunk) >= len ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE_MJPEG; // Marker bit + payload type
        pkt[i++] = (seq >> 8) & 0xFF;
        pkt[i++] = seq & 0xFF;
        pkt[i++] = (ts >> 24) & 0xFF;
        pkt[i++] = (ts >> 16) & 0xFF;
        pkt[i++] = (ts >> 8) & 0xFF;
        pkt[i++] = ts & 0xFF;
        pkt[i++] = (ssrc >> 24) & 0xFF;
        pkt[i++] = (ssrc >> 16) & 0xFF;
        pkt[i++] = (ssrc >> 8) & 0xFF;
        pkt[i++] = ssrc & 0xFF;

        // JPEG RTP header (RFC 2435 - 8 bytes)
        pkt[i++] = 0x00; // Type-specific = 0
        pkt[i++] = (offset >> 8) & 0xFF;
        pkt[i++] = offset & 0xFF;
        pkt[i++] = type;
        pkt[i++] = q;
        pkt[i++] = width8;
        pkt[i++] = height8;
        pkt[i++] = 0x00; // Restart interval / Reserved

        // JPEG 数据拷贝
        memcpy(pkt + i, jpeg + offset, chunk);

        // 发送整个 RTP 包
        int sent = send(rtsp_client_socket, pkt, i + chunk, 0);
        if (sent < 0) {
            ESP_LOGE(TAG, "Send failed, errno=%d", errno);
            rtsp_streaming = false;
            break;
        }

        offset += chunk;
    }

    seq++;
}

void rtsp_server_start(void) {
    xTaskCreatePinnedToCore(rtsp_server_task, "rtsp_server", 8192, NULL, 5, NULL, 1);
}

void rtsp_server_on_ip_assigned(uint32_t client_ip) {
    latest_client_ip = client_ip;
    ESP_LOGI(TAG, "RTSP server got client IP: %s", inet_ntoa(*(struct in_addr *)&client_ip));
}
