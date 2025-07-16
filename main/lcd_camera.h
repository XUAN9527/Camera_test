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