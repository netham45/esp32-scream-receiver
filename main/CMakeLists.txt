# Set up web files for embedding into the firmware
set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_FILES 
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/styles.css"
    "${WEB_SRC_DIR}/script.js"
)

idf_component_register(SRCS "web_server.c" "wifi_manager.c" "audio.c" "buffer.c" "network.c" "usb_audio_player_main.c" "spdif.c" "config_manager.c" "scream_sender.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES ${WEB_FILES})

target_link_libraries(${COMPONENT_LIB} "-L ${CMAKE_CURRENT_SOURCE_DIR}")
