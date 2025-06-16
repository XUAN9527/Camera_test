# IF (((IDF_VERSION_MAJOR EQUAL 4) AND (IDF_VERSION_MINOR GREATER 3)) OR (IDF_VERSION_MAJOR GREATER 4))
# ELSE()
# ENDIF()
idf_component_register(
    SRCS
        "main.c"    
        "lcd_camera.c"
        "softap_sta.c"
        "udp_transmitter"
    INCLUDE_DIRS
        ".")
