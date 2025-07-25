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

// #define TCP_STREAM_ENABLE		 // TCP/UDP传输开关
// #define JPEG_DQT_CHECK

#define RTSP_PORT 554
#define RTP_PAYLOAD_TYPE_MJPEG 26
#define RTP_HEADER_SIZE     12
#define JPEG_HEADER_SIZE    8
#define MAX_PACKET_SIZE     1400
#define RTP_MAX_PAYLOAD     (MAX_PACKET_SIZE - RTP_HEADER_SIZE - JPEG_HEADER_SIZE)

#define FRAME_RATE 15
#define FRAME_INTERVAL_US (1000000 / FRAME_RATE)

// JPEG参数
#define JPEG_QUALITY  0x3F      // 质量因子（0-255，建议0x3F-0x7F）
#define DISPLAY_H_RES (320 / 8)
#define DISPLAY_V_RES (240 / 8)

#define RTP_PORT 5004
#define RTCP_PORT 5005

// 网络优化参数
#define UDP_SEND_BUF_SIZE  (64 * 1024)  // UDP发送缓冲区

#define RTP_RETRY_DELAY_MS    6       // 每次失败后延迟 1ms
#define RTP_RETRY_LIMIT       3       // 每个包最多重试 3 次
#define RTP_FRAME_TIMEOUT_US  80000   // 单帧最长耗时 80ms，超过直接跳帧
#define RTP_ENABLE_DROP_FRAME 1       // 启用超时丢帧策略

// ✅ 在全局变量中新增客户端连接状态标志
static bool rtsp_client_connected = false;
static bool rtsp_streaming = false;
static bool use_tcp_transport = false;
static int rtsp_client_socket = -1;
static int udp_rtcp_sock = -1;  // RTCP socket
static int udp_sock = -1;
static struct sockaddr_in udp_client_addr = {0};
static uint16_t client_rtp_port = 0;
static uint32_t latest_client_ip = 0;

// 帧统计
static uint32_t frame_count = 0;
static uint32_t packet_count = 0;
static uint32_t error_count = 0;
static uint64_t last_stat_time = 0;

bool rtsp_stream_flag_get(void) {
    return rtsp_streaming && rtsp_client_connected;
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
		rtsp_client_connected = true;

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
#ifndef TCP_STREAM_ENABLE					
					ESP_LOGW(TAG, "TCP transport requested but disabled by server");
					char err[128];
					snprintf(err, sizeof(err),
							"RTSP/1.0 461 Unsupported Transport\r\n"
							"CSeq: %d\r\n\r\n", cseq);
					send_rtsp_response(rtsp_client_socket, cseq, err);
					continue;  // 不支持TCP，继续等待客户端其他请求
#endif
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

					// ✅ 设置 UDP 发送超时（防止 sendto 永久阻塞）
					struct timeval timeout = {.tv_sec = 0, .tv_usec = 50000};
					setsockopt(udp_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

					// ✅ 设置发送缓冲区大小
					int send_buf_size = UDP_SEND_BUF_SIZE;
					setsockopt(udp_sock, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size));

                    // 创建RTCP socket
                    udp_rtcp_sock = socket(AF_INET, SOCK_DGRAM, 0);
                    if (udp_rtcp_sock < 0) {
                        ESP_LOGE(TAG, "Failed to create RTCP socket");
                        close(udp_sock);
                        udp_sock = -1;
                        break;
                    }
					
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
		rtsp_streaming = false;
    	rtsp_client_connected = false;

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

#ifdef JPEG_DQT_CHECK
// 检测 JPEG 是否包含 DQT 段 (0xFFDB)
static bool jpeg_has_dqt(const uint8_t *data, size_t len) {
    size_t i = 2; // 跳过 SOI 0xFFD8
    while (i + 4 <= len) {
        if (data[i] != 0xFF) {
            // 非法 marker
            break;
        }

        uint8_t marker = data[i + 1];
        // 0xFFDA 是 SOS，之后是压缩数据段，不再查找
        if (marker == 0xDA) {
            break;
        }

        // 跳过填充的 0xFF（合法 JPEG 允许多个）
        while (marker == 0xFF && i + 2 < len) {
            i++;
            marker = data[i + 1];
        }

        // marker 长度字段位于 i+2，2 字节
        if (i + 4 > len) break;
        uint16_t segment_length = (data[i + 2] << 8) | data[i + 3];
        if (segment_length < 2) break;

        if (marker == 0xDB) {
            return true; // DQT 找到了
        }

        i += 2 + segment_length;
    }

    return false;
}
#endif

void rtsp_server_send_frame(uint8_t *jpeg, size_t len, uint8_t type) {
    if (!rtsp_streaming || (!use_tcp_transport && udp_sock < 0) || len < 2) {
        return;
    }

    static uint16_t seq = 0;
    static uint32_t ssrc = 0x12345678;
    static uint32_t rtp_timestamp = 0;

    uint64_t now_us = esp_timer_get_time();
    uint64_t frame_start_us = now_us;
    rtp_timestamp += 90000 / FRAME_RATE;

    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "Invalid JPEG header: %02X %02X", jpeg[0], jpeg[1]);
        return;
    }
#ifdef JPEG_DQT_CHECK
	// 检测是否包含 DQT，决定 RTP JPEG Type
	bool has_dqt = jpeg_has_dqt(jpeg, len);
	uint8_t type = has_dqt ? 0 : 1;
	ESP_LOGI(TAG, "JPEG header: %02X %02X, has_dqt=%s → type=%d", jpeg[0], jpeg[1], has_dqt ? "YES" : "NO", type);
#endif
    // 帧统计
    frame_count++;
    uint64_t now_ms = now_us / 1000;
    if (now_ms - last_stat_time >= 1000) {
        float loss_rate = error_count ? (error_count * 100.0f) / (packet_count + error_count) : 0;
        ESP_LOGI(TAG, "Streaming: %u FPS, %u pkts, %u bytes, %u errs (%.1f%%)",
                 (unsigned int)frame_count,
                 (unsigned int)packet_count,
				 (unsigned int)len,
                 (unsigned int)error_count,
                 loss_rate);
        frame_count = 0;
        packet_count = 0;
        error_count = 0;
        last_stat_time = now_ms;
    }

    size_t offset = 0;
    uint16_t frame_start_seq = seq;
    bool frame_failed = false;

    while (offset < len) {
        // 超时检查（整个帧）
        if (esp_timer_get_time() - frame_start_us > RTP_FRAME_TIMEOUT_US) {
            ESP_LOGW(TAG, "Drop frame due to timeout (>%d us)", RTP_FRAME_TIMEOUT_US);
            error_count++;
            return;
        }

        size_t chunk = (len - offset > RTP_MAX_PAYLOAD) ? RTP_MAX_PAYLOAD : (len - offset);

		// 调试日志（临时添加）
		// ESP_LOGI(TAG, "Preparing pkt: offset=%d, chunk=%d, total=%d", 
		// 		(int)offset, (int)chunk, (int)len);

		// 验证分包合理性
		if (chunk == 0 || chunk > RTP_MAX_PAYLOAD) {
			ESP_LOGE(TAG, "Invalid chunk size: %d", (int)chunk);
			break;
		}

        bool is_first = (offset == 0);
        bool is_last = ((offset + chunk) >= len);

        uint8_t pkt[12 + 8 + chunk];
        int i = 0;

        // RTP Header
        pkt[i++] = 0x80;
        pkt[i++] = (is_last ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE_MJPEG;
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

        // JPEG Payload Header
        pkt[i++] = is_first ? 0x00 : 0x80;
        pkt[i++] = (offset >> 16) & 0xFF;
        pkt[i++] = (offset >> 8) & 0xFF;
        pkt[i++] = offset & 0xFF;
        pkt[i++] = type;
        pkt[i++] = JPEG_QUALITY;
        pkt[i++] = DISPLAY_H_RES;
        pkt[i++] = DISPLAY_V_RES;

        memcpy(pkt + i, jpeg + offset, chunk);
        i += chunk;

        int retry = 0;
        int delay = RTP_RETRY_DELAY_MS;
        int sent = -1;
        bool fatal = false;

        do {
            if (use_tcp_transport) {
                uint8_t tcp_pkt[4 + i];
                tcp_pkt[0] = '$';
                tcp_pkt[1] = 0x00;
                tcp_pkt[2] = (i >> 8) & 0xFF;
                tcp_pkt[3] = i & 0xFF;
                memcpy(tcp_pkt + 4, pkt, i);
                sent = send(rtsp_client_socket, tcp_pkt, i + 4, 0);
            } else {
                sent = sendto(udp_sock, pkt, i, 0,
                              (struct sockaddr *)&udp_client_addr,
                              sizeof(udp_client_addr));
            }

            if (sent < 0) {
				int err = errno;

				// 针对 UDP 特定错误进行分级处理
				if (err == EAGAIN || err == ENOMEM || err == ENOBUFS) {
					if (retry < RTP_RETRY_LIMIT) {  // 限制最大重试次数
						ESP_LOGW(TAG, "Send retry #%d at offset %u: errno=%d (%s)",
								retry, (unsigned int)offset, err, strerror(err));
						vTaskDelay(pdMS_TO_TICKS(delay));
						retry++;
						delay = delay * 2 > 50 ? 50 : delay * 2;  // 延迟指数退避，最大不超 50ms
					} else {
						ESP_LOGE(TAG, "Retry limit reached (%d) at offset %u, drop packet",
								retry, (unsigned int)offset);
						break;  // 超过重试次数，丢弃该 RTP 包
					}
				} else {
					fatal = true;
					rtsp_streaming = false;
					ESP_LOGE(TAG, "Fatal send error: errno=%d (%s)", err, strerror(err));
					break;
				}
    		}
		} while (sent < 0 && !fatal);

        if (sent < 0) {
            error_count++;
#if RTP_ENABLE_DROP_FRAME
			// 11=EAGAIN:socket buffer 满; 12=ENOMEM,内存不足（通常是 LWIP 内部）;104=ECONNRESET,客户端断开连接；113=EHOSTUNREACH，客户端地址不可达
           	ESP_LOGW(TAG, "Sendto failed at offset %u, errno=%d (%s)", (unsigned int)offset, errno, strerror(errno));
            frame_failed = true;
#endif
			if (offset == 0) {
				// 首包失败，整帧不可用
				ESP_LOGW(TAG, "Drop frame due to send failure at offset 0 (critical)");
				vTaskDelay(pdMS_TO_TICKS(100));  // 加一段等待，缓解发送拥堵
				frame_failed = true;
				break;
			} else {
				// 非首包失败，标记但继续传输（可选）
				ESP_LOGW(TAG, "Non-first packet failed at offset %u, continuing", offset);
			}
        } else {
            packet_count++;
        }

        seq++;
        offset += chunk;
    }

    if (!frame_failed) {
        ESP_LOGD(TAG, "Frame complete: start_seq=%u, end_seq=%u, packets=%d",
                 frame_start_seq, seq - 1, (seq - frame_start_seq));
		// ✅ 在帧成功发送后延迟 1ms 让出 CPU，降低系统负载
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    static uint64_t last_rtcp = 0;
    if (now_us - last_rtcp > 5000000) {
        send_rtcp_sr_report(now_us / 1000, rtp_timestamp);
        last_rtcp = now_us;
    }
	
	// 根据帧处理时间动态延迟，避免帧率过快导致负载高
    uint64_t frame_used_us = esp_timer_get_time() - frame_start_us;
    uint64_t frame_interval_us = 1000000 / FRAME_RATE;
    int wait_ms = (int)((frame_interval_us > frame_used_us) ? (frame_interval_us - frame_used_us) / 1000 : 0);
	if (wait_ms > 0 && wait_ms < 50) {
		vTaskDelay(pdMS_TO_TICKS(wait_ms));
	}
}


void rtsp_server_start(void) {
    xTaskCreatePinnedToCore(rtsp_server_task, "rtsp_server", 8192, NULL, 5, NULL, 1);
}

void rtsp_server_on_ip_assigned(uint32_t client_ip) {
    latest_client_ip = client_ip;
    ESP_LOGI(TAG, "RTSP server got client IP: %s", inet_ntoa(*(struct in_addr *)&client_ip));
}