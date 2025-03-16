idf_component_register(SRCS "web_server.c" "wifi_manager.c" "audio.c" "buffer.c" "network.c" "usb_audio_player_main.c" "spdif.c"
                    INCLUDE_DIRS ".")

target_link_libraries(${COMPONENT_LIB} "-L ${CMAKE_CURRENT_SOURCE_DIR}")
