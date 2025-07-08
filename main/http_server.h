#ifndef __HTTP_SERVER_H
#define __HTTP_SERVER_H

#include <stdint.h>
#include <stddef.h>

void http_server_start(void);
void http_server_send_frame(uint8_t *jpeg, size_t len);

#endif
