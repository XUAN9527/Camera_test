set(COMPONENT_ADD_INCLUDEDIRS ./include)

# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES esp_peripherals audio_sal audio_hal esp_dispatcher display_service)

if (CONFIG_ESP32_S3_KORVO2_V3_BOARD)
message(STATUS "Current board name is " CONFIG_ESP32_S3_KORVO2_V3_BOARD)
list(APPEND COMPONENT_ADD_INCLUDEDIRS)
set(COMPONENT_SRCS
./board.c
./board_pins_config.c
)
endif()

register_component()
