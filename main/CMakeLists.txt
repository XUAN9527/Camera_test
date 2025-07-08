# IF (((IDF_VERSION_MAJOR EQUAL 4) AND (IDF_VERSION_MINOR GREATER 3)) OR (IDF_VERSION_MAJOR GREATER 4))
# ELSE()
# ENDIF()
idf_component_register(
    SRCS
        "main.c"    
        "lcd_camera.c"
        "wifi_softap.c"
		"rtsp_server.c"
		"http_server.c"
    INCLUDE_DIRS
        "."
    )
