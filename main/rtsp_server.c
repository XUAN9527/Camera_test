// rtsp_server.c
#include "rtsp_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_timer.h"

#define TAG "RTSP_SERVER"
#define RTSP_PORT 554
#define RTP_PAYLOAD_TYPE_MJPEG 26
#define MAX_RTP_PAYLOAD 1400   // 保守点，避免IP包分片
#define FRAME_RATE 10
#define FRAME_INTERVAL_US (1000000 / FRAME_RATE)

#define RTP_PORT 5004
#define RTCP_PORT 5005
static int udp_rtcp_sock = -1;  // RTCP socket

static uint32_t latest_client_ip = 0;
static bool rtsp_streaming = false;
static int rtsp_client_socket = -1;
static bool use_tcp_transport = false;

static int udp_sock = -1;
static struct sockaddr_in udp_client_addr = {0};
static uint16_t client_rtp_port = 0;

// 帧统计
static uint32_t frame_count = 0;
static uint32_t packet_count = 0;
static uint32_t error_count = 0;
static uint64_t last_stat_time = 0;

bool rtsp_stream_flag_get(void) {
    return rtsp_streaming;
}

static void send_rtsp_response(int sock, int cseq, const char *response) {
    ESP_LOGD(TAG, "Sending RTSP response:\n%s", response);
    send(sock, response, strlen(response), 0);
}

// 解析 client_port=xxxx-xxxx 中的第一个端口号
static uint16_t parse_client_rtp_port_and_ip(const char *buf, uint32_t *ip_out) {
    const char *p = strstr(buf, "client_port=");
    if (!p) return 0;

    int port1 = 0;
    sscanf(p, "client_port=%d", &port1);

    if (port1 <= 0 || port1 > 65535) return 0;
    if (ip_out) *ip_out = latest_client_ip;  // 暂时使用 TCP 连接 IP
    return (uint16_t)port1;
}

static bool enable_tcp = false;  // TCP传输开关
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
        use_tcp_transport = true;  // 默认TCP，可根据SETUP覆盖
        client_rtp_port = 0;

        // 关闭旧UDP socket
        if (udp_sock >= 0) {
            close(udp_sock);
            udp_sock = -1;
        }
        if (udp_rtcp_sock >= 0) {
            close(udp_rtcp_sock);
            udp_rtcp_sock = -1;
        }

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
                    "a=framerate:10\r\n"
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
                    if (!enable_tcp) {
                        ESP_LOGW(TAG, "TCP transport requested but disabled by server");
                        char err[128];
                        snprintf(err, sizeof(err),
                                "RTSP/1.0 461 Unsupported Transport\r\n"
                                "CSeq: %d\r\n\r\n", cseq);
                        send_rtsp_response(rtsp_client_socket, cseq, err);
                        continue;  // 不支持TCP，继续等待客户端其他请求
                    }

                    use_tcp_transport = true;
                    ESP_LOGI(TAG, "Using TCP transport for RTP");

                    if (udp_sock >= 0) {
                        close(udp_sock);
                        udp_sock = -1;
                    }
                    if (udp_rtcp_sock >= 0) {
                        close(udp_rtcp_sock);
                        udp_rtcp_sock = -1;
                    }

                    char resp[256];
                    snprintf(resp, sizeof(resp),
                            "RTSP/1.0 200 OK\r\n"
                            "CSeq: %d\r\n"
                            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                            "Session: 12345678\r\n"
                            "Timeout: 60\r\n\r\n",
                            cseq);
                    send_rtsp_response(rtsp_client_socket, cseq, resp);

                } else if (strstr(buf, "RTP/AVP")) {
                    use_tcp_transport = false;
                    ESP_LOGI(TAG, "Using UDP transport for RTP");

                    uint32_t parsed_ip = 0;
                    client_rtp_port = parse_client_rtp_port_and_ip(buf, &parsed_ip);
                    if (client_rtp_port == 0) {
                        ESP_LOGE(TAG, "Failed to parse client RTP port");
                        char err[128];
                        snprintf(err, sizeof(err),
                                "RTSP/1.0 400 Bad Request\r\n"
                                "CSeq: %d\r\n\r\n", cseq);
                        send_rtsp_response(rtsp_client_socket, cseq, err);
                        break;
                    }

                    // 关闭旧UDP socket
                    if (udp_sock >= 0) close(udp_sock);
                    if (udp_rtcp_sock >= 0) close(udp_rtcp_sock);

                    // 创建RTP socket
                    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
                    if (udp_sock < 0) {
                        ESP_LOGE(TAG, "Failed to create RTP socket");
                        break;
                    }

                    // 创建RTCP socket
                    udp_rtcp_sock = socket(AF_INET, SOCK_DGRAM, 0);
                    if (udp_rtcp_sock < 0) {
                        ESP_LOGE(TAG, "Failed to create RTCP socket");
                        close(udp_sock);
                        udp_sock = -1;
                        break;
                    }

                    // 设置发送缓冲区大小
                    int send_buf_size = 32 * 1024; // 32KB
                    setsockopt(udp_sock, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size));
					
                    struct sockaddr_in local_rtp_addr = {
                        .sin_family = AF_INET,
                        .sin_port = htons(RTP_PORT),
                        .sin_addr.s_addr = htonl(INADDR_ANY),
                    };
                    
                    struct sockaddr_in local_rtcp_addr = {
                        .sin_family = AF_INET,
                        .sin_port = htons(RTCP_PORT),
                        .sin_addr.s_addr = htonl(INADDR_ANY),
                    };

                    // 绑定RTP端口
                    if (bind(udp_sock, (struct sockaddr *)&local_rtp_addr, sizeof(local_rtp_addr)) < 0) {
                        ESP_LOGE(TAG, "RTP bind failed on port %d, errno=%d", RTP_PORT, errno);
                        close(udp_sock);
                        close(udp_rtcp_sock);
                        udp_sock = -1;
                        udp_rtcp_sock = -1;
                        break;
                    }

                    // 绑定RTCP端口
                    if (bind(udp_rtcp_sock, (struct sockaddr *)&local_rtcp_addr, sizeof(local_rtcp_addr)) < 0) {
                        ESP_LOGE(TAG, "RTCP bind failed on port %d, errno=%d", RTCP_PORT, errno);
                        close(udp_sock);
                        close(udp_rtcp_sock);
                        udp_sock = -1;
                        udp_rtcp_sock = -1;
                        break;
                    }

                    udp_client_addr.sin_family = AF_INET;
                    udp_client_addr.sin_addr.s_addr = parsed_ip;
                    udp_client_addr.sin_port = htons(client_rtp_port);

                    ESP_LOGI(TAG, "UDP client IP: %s, RTP port: %d, RTCP port: %d",
                            inet_ntoa(udp_client_addr.sin_addr),
                            client_rtp_port, client_rtp_port + 1);

                    char resp[256];
                    snprintf(resp, sizeof(resp),
                            "RTSP/1.0 200 OK\r\n"
                            "CSeq: %d\r\n"
                            "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                            "Session: 12345678\r\n\r\n",
                            cseq, client_rtp_port, client_rtp_port + 1, RTP_PORT, RTCP_PORT);
                    send_rtsp_response(rtsp_client_socket, cseq, resp);
                } else {
                    char err[128];
                    snprintf(err, sizeof(err),
                            "RTSP/1.0 461 Unsupported Transport\r\n"
                            "CSeq: %d\r\n\r\n", cseq);
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
                frame_count = 0;
                last_stat_time = esp_timer_get_time() / 1000;
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
        if (udp_sock >= 0) {
            close(udp_sock);
            udp_sock = -1;
        }
        if (udp_rtcp_sock >= 0) {
            close(udp_rtcp_sock);
            udp_rtcp_sock = -1;
        }
        close(rtsp_client_socket);
        rtsp_client_socket = -1;
        rtsp_streaming = false;
    }
}

// 添加RTCP发送函数
static void send_rtcp_sr_report(uint32_t ntp_ts, uint32_t rtp_ts) {
    if (udp_rtcp_sock < 0 || client_rtp_port == 0) {
        ESP_LOGW(TAG, "Cannot send RTCP: no client port");
        return;
    }
    
    struct sockaddr_in rtcp_addr = udp_client_addr;
    rtcp_addr.sin_port = htons(client_rtp_port + 1); // RTCP端口
    
    uint8_t rtcp_pkt[28] = {
        0x80, 0xC8, 0x00, 0x06, // SR header
        0x12, 0x34, 0x56, 0x78, // SSRC
        (ntp_ts >> 24) & 0xFF, (ntp_ts >> 16) & 0xFF, (ntp_ts >> 8) & 0xFF, ntp_ts & 0xFF,
        0x00, 0x00, 0x00, 0x00, // NTP timestamp fractional
        (rtp_ts >> 24) & 0xFF, (rtp_ts >> 16) & 0xFF, (rtp_ts >> 8) & 0xFF, rtp_ts & 0xFF,
        0x00, 0x00, 0x00, 0x00, // Packet count
        0x00, 0x00, 0x00, 0x00  // Octet count
    };
    
    int sent = sendto(udp_rtcp_sock, rtcp_pkt, sizeof(rtcp_pkt), 0,
          (struct sockaddr *)&rtcp_addr, sizeof(rtcp_addr));
    
    if (sent > 0) {
        ESP_LOGD(TAG, "Sent RTCP SR report to %s:%d", 
                inet_ntoa(rtcp_addr.sin_addr), ntohs(rtcp_addr.sin_port));
    } else {
        ESP_LOGE(TAG, "Failed to send RTCP, errno=%d", errno);
    }
}

void rtsp_server_send_frame(uint8_t *jpeg, size_t len) {
    if (!rtsp_streaming || (!use_tcp_transport && udp_sock < 0) || len < 2) {
        return;
    }

    static uint16_t seq = 0;
    static uint32_t ssrc = 0x12345678;
    static uint64_t next_frame_time = 0;
    static uint32_t rtp_timestamp = 0;
    
    uint64_t now_us = esp_timer_get_time();
    
    // 精确帧率控制
    if (now_us < next_frame_time) {
        return;
    }
    next_frame_time = now_us + FRAME_INTERVAL_US;
    
    // 更新时间戳 (90kHz时钟)
    uint32_t frame_duration = 90000 / FRAME_RATE;
    rtp_timestamp += frame_duration;
    
    // 检查JPEG头有效性
    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "Invalid JPEG header: %02X %02X", jpeg[0], jpeg[1]);
        return;
    }
    
    // 帧统计
    frame_count++;
    uint64_t now_ms = now_us / 1000;
    if (now_ms - last_stat_time >= 1000) {
        ESP_LOGI(TAG, "Streaming: %u FPS, %u packets, %u errors", 
                (unsigned int)frame_count, (unsigned int)packet_count, (unsigned int)error_count);
        frame_count = 0;
        packet_count = 0;
        error_count = 0;
        last_stat_time = now_ms;
    }

    uint8_t type = 0x01;       // 基础JPEG类型 (Table 3.1)
    uint8_t q = 0x5F;          // Q因子 (中等质量)
    uint8_t width8 = 320 / 8;  // 40
    uint8_t height8 = 240 / 8; // 30

    size_t offset = 0;
    uint16_t frame_seq = seq;  // 记录帧起始序列号
    
    ESP_LOGD(TAG, "Sending JPEG frame: size=%u, ts=%u, seq=%u", (unsigned int)len, (unsigned int)rtp_timestamp, (unsigned int)seq);

    while (offset < len) {
        size_t chunk = (len - offset > MAX_RTP_PAYLOAD - 8) ? 
                      (MAX_RTP_PAYLOAD - 8) : (len - offset);
        
        bool is_first = (offset == 0);
        bool is_last = ((offset + chunk) >= len);
        
        // UDP不需要TCP通道头
        uint8_t pkt[12 + 8 + chunk];
        int i = 0;

        // RTP Header
        pkt[i++] = 0x80; // Version=2, Padding=0, Extension=0, CSRC=0
        pkt[i++] = (is_last ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE_MJPEG; // Marker位 + Payload Type
        pkt[i++] = (seq >> 8) & 0xFF;
        pkt[i++] = seq & 0xFF;
        pkt[i++] = (rtp_timestamp >> 24) & 0xFF;
        pkt[i++] = (rtp_timestamp >> 16) & 0xFF;
        pkt[i++] = (rtp_timestamp >> 8) & 0xFF;
        pkt[i++] = rtp_timestamp & 0xFF;
        pkt[i++] = (ssrc >> 24) & 0xFF;
        pkt[i++] = (ssrc >> 16) & 0xFF;
        pkt[i++] = (ssrc >> 8) & 0xFF;
        pkt[i++] = ssrc & 0xFF;

        // JPEG Payload Header (RFC2435)
        pkt[i++] = is_first ? 0x00 : 0x80; // Type specific (分片标识)
        pkt[i++] = (offset >> 16) & 0xFF;
        pkt[i++] = (offset >> 8) & 0xFF;
        pkt[i++] = offset & 0xFF;
        pkt[i++] = type;  // Type (JPEG)
        pkt[i++] = q;     // Quality factor
        pkt[i++] = width8;
        pkt[i++] = height8;
        
        memcpy(pkt + i, jpeg + offset, chunk);
        i += chunk;

        int sent = -1;
        if (use_tcp_transport) {
            // TCP传输需要添加通道头
            uint8_t tcp_pkt[4 + i];
            tcp_pkt[0] = '$';
            tcp_pkt[1] = 0x00;  // 通道0
            tcp_pkt[2] = (i >> 8) & 0xFF;
            tcp_pkt[3] = i & 0xFF;
            memcpy(tcp_pkt + 4, pkt, i);
            
            sent = send(rtsp_client_socket, tcp_pkt, i + 4, 0);
        } else {
            // UDP直接发送
            sent = sendto(udp_sock, pkt, i, 0, 
                         (struct sockaddr *)&udp_client_addr, 
                         sizeof(udp_client_addr));
            
            if (sent > 0) {
                packet_count++;
                ESP_LOGD(TAG, "Sent RTP packet: size=%d, offset=%d/%d, seq=%u, marker=%d",
                         sent, offset, len, seq, is_last ? 1 : 0);
            }
        }

        if (sent < 0) {
            error_count++;
            ESP_LOGE(TAG, "Send failed, errno=%d", errno);
            // 轻度错误不中断流
        }

        seq++;
        offset += chunk;
    }
    
    // 每5秒发送RTCP报告
    static uint64_t last_rtcp = 0;
    if (now_us - last_rtcp > 5000000) {
        send_rtcp_sr_report(now_us / 1000, rtp_timestamp);
        last_rtcp = now_us;
    }
}

void rtsp_server_start(void) {
    xTaskCreatePinnedToCore(rtsp_server_task, "rtsp_server", 8192, NULL, 5, NULL, 1);
}

void rtsp_server_on_ip_assigned(uint32_t client_ip) {
    latest_client_ip = client_ip;
    ESP_LOGI(TAG, "RTSP server got client IP: %s", inet_ntoa(*(struct in_addr *)&client_ip));
}