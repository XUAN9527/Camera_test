/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#ifndef __WEB_MSERVER_H
#define __WEB_MSERVER_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

void web_mjpeg_server_start(void);
bool web_mjpeg_server_is_client_connected(void);
void web_mjpeg_server_send_jpeg(const uint8_t *jpeg_buf, size_t jpeg_len);

#ifdef __cplusplus
}
#endif  /* __cplusplus */

#endif