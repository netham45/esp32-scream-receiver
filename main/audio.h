#pragma once
#include "config.h"
#ifdef IS_USB
#include "usb/uac_host.h"
#endif
void process_audio_actions(bool is_startup);
void register_button(int button,void (*action)(bool, int, void *));
void setup_audio();
#ifdef IS_USB
void start_playback(uac_host_device_handle_t _spkr_handle);
#endif
void stop_playback();
void audio_write(uint8_t* data);
void audio_direct_write(uint8_t *data);
void resume_playback();