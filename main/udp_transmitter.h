#ifndef UDP_TRANSMITTER_H
#define UDP_TRANSMITTER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_camera.h"

// 帧头结构
typedef struct {
    uint32_t frame_number;
    uint32_t frame_size;
    uint32_t frame_offset;
} frame_header_t;

#define FRAME_HEADER_SIZE sizeof(frame_header_t)

// UDP传输任务函数声明
void udp_transmit_task(void *pvParameters);

#endif // UDP_TRANSMITTER_H