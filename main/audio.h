#pragma once
#include "usb/uac_host.h"
void process_audio_actions(bool is_startup);
void register_button(int button,void (*action)(bool, int, void *));
void setup_audio();
void start_playback(uac_host_device_handle_t _spkr_handle);
void stop_playback();
void audio_write(uint8_t* data);