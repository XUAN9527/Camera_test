#ifndef __HTTP_SERVER_H
#define __HTTP_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void http_server_start(void);
void http_server_send_frame(uint8_t *jpeg, size_t len);
bool http_stream_flag_get(void);

#endif
