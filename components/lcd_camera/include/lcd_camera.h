/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#ifndef __LCD_CAMERA_H
#define __LCD_CAMERA_H

#pragma once

#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief Camera Function Definition
 */
#define FUNC_CAMERA_EN              (1)
#define CAM_PIN_PWDN                -1
#define CAM_PIN_RESET               -1
#define CAM_PIN_XCLK                GPIO_NUM_40
#define CAM_PIN_SIOD                GPIO_NUM_17
#define CAM_PIN_SIOC                GPIO_NUM_18

#define CAM_PIN_D7                  GPIO_NUM_39
#define CAM_PIN_D6                  GPIO_NUM_41
#define CAM_PIN_D5                  GPIO_NUM_42
#define CAM_PIN_D4                  GPIO_NUM_12
#define CAM_PIN_D3                  GPIO_NUM_3
#define CAM_PIN_D2                  GPIO_NUM_14
#define CAM_PIN_D1                  GPIO_NUM_47
#define CAM_PIN_D0                  GPIO_NUM_13
#define CAM_PIN_VSYNC               GPIO_NUM_21
#define CAM_PIN_HREF                GPIO_NUM_38
#define CAM_PIN_PCLK                GPIO_NUM_11

typedef struct {
	bool (*stream_flag)(void);					   // 转换标志
    void (*send_jpeg)(uint8_t *jpeg, size_t len, uint8_t type);  // 注册 MJPEG 回调
} lcd_camera_config_t;

esp_err_t lcd_camera_start(const lcd_camera_config_t *config);
esp_lcd_panel_handle_t lcd_camera_get_panel(void);

#ifdef __cplusplus
}
#endif  /* __cplusplus */

#endif