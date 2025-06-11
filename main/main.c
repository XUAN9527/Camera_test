#include <stdio.h>
#include "string.h"
#include "softap_sta.h"
#include "lcd_camera.h"

void app_main(void)
{
    my_lcd_camera_init();
	my_softap_sta_init();
}