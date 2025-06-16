#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "sdkconfig.h"
#include "udp_transmitter.h"

#define UDP_PORT CONFIG_UDP_STREAM_PORT
#define MAX_CLIENTS CONFIG_MAX_UDP_CLIENTS
#define CLIENT_TIMEOUT_MS 10000  // 客户端超时时间(10秒)

static const char *TAG = "UDP_TX";

// 客户端结构
typedef struct {
    struct sockaddr_in addr;
    bool active;
    uint32_t last_active;  // 最后活动时间戳
} udp_client_t;

static udp_client_t clients[MAX_CLIENTS];
static int sock = -1;
static uint32_t frame_counter = 0;

// 查找客户端索引
static int find_client_index(struct sockaddr_in *addr)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && 
            clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            clients[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

// 添加新客户端
static int add_client(struct sockaddr_in *addr)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].addr = *addr;
            clients[i].active = true;
            clients[i].last_active = xTaskGetTickCount();
            return i;
        }
    }
    return -1; // 无可用槽位
}

// 清理超时客户端
static void cleanup_clients()
{
    uint32_t now = xTaskGetTickCount();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && 
            (now - clients[i].last_active) > pdMS_TO_TICKS(CLIENT_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "Client %d timeout", i);
            clients[i].active = false;
        }
    }
}

void udp_transmit_task(void *pvParameters)
{
    QueueHandle_t image_queue = (QueueHandle_t)pvParameters;
    
    // 创建 UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0) {
        ESP_LOGE(TAG, "Socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // 设置socket选项 - 解决EADDRINUSE
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        ESP_LOGE(TAG, "Set SO_REUSEADDR failed: %d", errno);
    }
    
    // 设置socket选项 - 增加缓冲区大小
    int rcvbuf = 65535;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    // 配置服务器地址
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    
    // 尝试绑定主端口
    int bind_ret = bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    
    // 如果绑定失败，尝试备用端口
    if (bind_ret < 0 && errno == EADDRINUSE) {
        ESP_LOGW(TAG, "Port %d in use, trying fallback port", UDP_PORT);
        server_addr.sin_port = htons(UDP_PORT + 1);
        bind_ret = bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    }
    
    if (bind_ret < 0) {
        ESP_LOGE(TAG, "Socket bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "UDP server started on port %d", ntohs(server_addr.sin_port));
    
    // 初始化客户端列表
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = false;
    }
    
    // 主传输循环
    while (1) {
        // 1. 检查新客户端
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        uint8_t buf[64];
        
        int len = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, 
                          (struct sockaddr *)&client_addr, &addr_len);
        
        if (len > 0) {
            // 处理连接请求
            if (len >= 7 && memcmp(buf, "CONNECT", 7) == 0) {
                int idx = find_client_index(&client_addr);
                if (idx == -1) { // 新客户端
                    idx = add_client(&client_addr);
                    if (idx >= 0) {
                        ESP_LOGI(TAG, "New client[%d]: %s:%d", 
                                 idx, inet_ntoa(client_addr.sin_addr), 
                                 ntohs(client_addr.sin_port));
                        
                        // 发送确认
                        const char *ack = "ACK";
                        sendto(sock, ack, strlen(ack), 0,
                              (struct sockaddr *)&client_addr, addr_len);
                    } else {
                        ESP_LOGW(TAG, "Client limit reached");
                    }
                } else { // 已有客户端，更新活动时间
                    clients[idx].last_active = xTaskGetTickCount();
                }
            }
        }
        
        // 2. 清理超时客户端
        cleanup_clients();
        
        // 3. 处理图像传输 - 使用阻塞接收确保及时处理
        camera_fb_t *pic = NULL;
        if (xQueueReceive(image_queue, &pic, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (pic && pic->format == PIXFORMAT_RGB565) {
                frame_counter++;
                
                // 准备帧头
                frame_header_t header = {
                    .frame_number = htonl(frame_counter),
                    .frame_size = htonl(pic->len),
                    .frame_offset = 0
                };
                
                // 分块发送减少丢包
                const size_t chunk_size = 1400;
                size_t offset = 0;
                
                while (offset < pic->len) {
                    size_t remaining = pic->len - offset;
                    size_t send_size = (remaining > chunk_size) ? chunk_size : remaining;
                    
                    // 准备块头
                    header.frame_offset = htonl(offset);
                    
                    // 创建发送缓冲区
                    uint8_t send_buf[FRAME_HEADER_SIZE + send_size];
                    memcpy(send_buf, &header, FRAME_HEADER_SIZE);
                    memcpy(send_buf + FRAME_HEADER_SIZE, pic->buf + offset, send_size);
                    
                    // 发送给所有活跃客户端
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].active) {
                            if (sendto(sock, send_buf, sizeof(send_buf), 0,
                                  (struct sockaddr *)&clients[i].addr, 
                                  sizeof(clients[i].addr)) < 0) {
                                ESP_LOGW(TAG, "Send failed to client %d", i);
                            }
                        }
                    }
                    
                    offset += send_size;
                    vTaskDelay(1); // 让出CPU
                }
            }
            
            // 立即释放图像缓冲区
            esp_camera_fb_return(pic);
        }
        
        // 4. 合理让出CPU
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    close(sock);
    vTaskDelete(NULL);
}