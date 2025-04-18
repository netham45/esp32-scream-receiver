# Set up web files for embedding into the firmware
set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_FILES 
    "${WEB_SRC_DIR}/index.html"
    "${WEB_SRC_DIR}/styles.css"
    "${WEB_SRC_DIR}/script.js"
)

# BQ25895 battery charger files
set(BQ25895_SRCS
    "bq25895/bq25895.c"
    "bq25895/bq25895_web.c"
    "bq25895_integration.c"
)

idf_component_register(SRCS "mdns_service.c" "web_server.c" "wifi_manager.c" "audio.c" "buffer.c" "network.c" "usb_audio_player_main.c" "spdif.c" "config_manager.c" "scream_sender.c" "ntp_client.cpp" ${BQ25895_SRCS}
                    INCLUDE_DIRS "."
                    EMBED_FILES ${WEB_FILES})

target_link_libraries(${COMPONENT_LIB} "-L ${CMAKE_CURRENT_SOURCE_DIR}")
