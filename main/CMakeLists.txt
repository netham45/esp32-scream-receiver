idf_component_register(SRCS "audio.c" "buffer.c" "network.c" "usb_audio_player_main.c"
                    INCLUDE_DIRS ".")

target_link_libraries(${COMPONENT_LIB} "-L ${CMAKE_CURRENT_SOURCE_DIR}")

