#ifndef __RTSP_SERVER_H__
#define __RTSP_SERVER_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void rtsp_server_start(void);
void rtsp_server_send_frame(uint8_t *jpeg, size_t len);
void rtsp_server_on_ip_assigned(uint32_t client_ip);

#ifdef __cplusplus
}
#endif

#endif // __RTSP_SERVER_H__
