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

static uint32_t latest_client_ip = 0;
static bool rtsp_streaming = false;
static int rtsp_client_socket = -1;
static bool use_tcp_transport = true;

static int udp_sock = -1;
static struct sockaddr_in udp_client_addr = {0};
static uint16_t client_rtp_port = 0;

bool rtsp_stream_flag_get(void) {
    return rtsp_streaming;
}

static void send_rtsp_response(int sock, int cseq, const char *response) {
    ESP_LOGD(TAG, "Sending RTSP response:\n%s", response);
    send(sock, response, strlen(response), 0);
}

// 解析 client_port=xxxx-xxxx 中的第一个端口号
static uint16_t parse_client_rtp_port(const char *buf) {
    const char *p = strstr(buf, "client_port=");
    if (!p) return 0;
    int port1 = 0;
    sscanf(p, "client_port=%d", &port1);
    if (port1 <= 0 || port1 > 65535) return 0;
    return (uint16_t)port1;
}

static bool enable_tcp = true;  // 新增开关，调试时设true或false
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
					// 保持UDP处理不变
					use_tcp_transport = false;
					ESP_LOGI(TAG, "Using UDP transport for RTP");
					client_rtp_port = parse_client_rtp_port(buf);
					ESP_LOGI(TAG, "Client RTP port (UDP): %d", client_rtp_port);
					if (client_rtp_port == 0) {
						char err[128];
						snprintf(err, sizeof(err),
								"RTSP/1.0 400 Bad Request\r\n"
								"CSeq: %d\r\n\r\n", cseq);
						send_rtsp_response(rtsp_client_socket, cseq, err);
						break;
					}

					if (udp_sock >= 0) close(udp_sock);
					udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
					if (udp_sock < 0) {
						ESP_LOGE(TAG, "Failed to create UDP socket");
						break;
					}

					struct sockaddr_in local_addr = {
						.sin_family = AF_INET,
						.sin_port = htons(5004),
						.sin_addr.s_addr = htonl(INADDR_ANY),
					};
					if (bind(udp_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
						ESP_LOGE(TAG, "UDP bind failed");
						close(udp_sock);
						udp_sock = -1;
						break;
					}

					udp_client_addr.sin_family = AF_INET;
					udp_client_addr.sin_addr.s_addr = latest_client_ip;
					udp_client_addr.sin_port = htons(client_rtp_port);

					char resp[256];
					snprintf(resp, sizeof(resp),
							"RTSP/1.0 200 OK\r\n"
							"CSeq: %d\r\n"
							"Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=5004-5005\r\n"
							"Session: 12345678\r\n\r\n",
							cseq, client_rtp_port, client_rtp_port + 1);
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
        close(rtsp_client_socket);
        rtsp_client_socket = -1;
        rtsp_streaming = false;
    }
}

void rtsp_server_send_frame(uint8_t *jpeg, size_t len) {
    if (!rtsp_streaming || rtsp_client_socket < 0 || len < 2) return;

    static uint16_t seq = 0;
    static uint32_t ssrc = 0x12345678;
    static uint32_t last_frame_time = 0;

    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_frame_time < 1000 / FRAME_RATE) return;
    last_frame_time = now;

    uint32_t ts = now * 90;

    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) return;

    uint8_t type = 1, q = 255;
    uint8_t width8 = 320 / 8, height8 = 240 / 8;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > MAX_RTP_PAYLOAD - 8) ? (MAX_RTP_PAYLOAD - 8) : (len - offset);
        uint8_t pkt[4 + 12 + 8 + chunk];
        int i = 0;

        if (use_tcp_transport) {
            pkt[i++] = '$';
            pkt[i++] = 0x00;
            uint16_t pkt_len = 12 + 8 + chunk;
            pkt[i++] = (pkt_len >> 8) & 0xFF;
            pkt[i++] = pkt_len & 0xFF;
        }

        pkt[i++] = 0x80;
        pkt[i++] = ((offset + chunk) >= len ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE_MJPEG;
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

        pkt[i++] = 0x00;
        pkt[i++] = (offset >> 16) & 0xFF;
        pkt[i++] = (offset >> 8) & 0xFF;
        pkt[i++] = offset & 0xFF;
        pkt[i++] = type;
        pkt[i++] = q;
        pkt[i++] = width8;
        pkt[i++] = height8;
		
        memcpy(pkt + i, jpeg + offset, chunk);

        int sent = -1;
        if (use_tcp_transport) {
			// ESP_LOGI(TAG, "Sending RTP frame via TCP, seq=%u, size=%d", seq, i + chunk);
            sent = send(rtsp_client_socket, pkt, i + chunk, 0);
        } else {
            uint16_t rtp_pkt_len = 12 + 8 + chunk;
			// ESP_LOGI(TAG, "Sending RTP frame via UDP, seq=%u, size=%d", seq, i + chunk);
            sent = sendto(udp_sock, pkt + 4, rtp_pkt_len, 0,
                          (struct sockaddr *)&udp_client_addr,
                          sizeof(udp_client_addr));
        }

        if (sent < 0) {
            ESP_LOGE(TAG, "Send failed, errno=%d", errno);
            rtsp_streaming = false;
            break;
        }

        seq++;
        offset += chunk;
    }
}

void rtsp_server_start(void) {
    xTaskCreatePinnedToCore(rtsp_server_task, "rtsp_server", 8192, NULL, 5, NULL, 1);
}

void rtsp_server_on_ip_assigned(uint32_t client_ip) {
    latest_client_ip = client_ip;
    ESP_LOGI(TAG, "RTSP server got client IP: %s", inet_ntoa(*(struct in_addr *)&client_ip));
}
