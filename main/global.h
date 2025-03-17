#pragma once
#include "stdint.h"
#include "config.h"
#include "stdbool.h"
// PCM Bytes per chunk, non-configurable (Part of Scream)
#define PCM_CHUNK_SIZE 1152

// Global sleep state
extern bool device_sleeping;

#ifdef IS_USB
#include "freertos/FreeRTOS.h"
#include "usb/uac_host.h"
// Global USB speaker device handle
extern uac_host_device_handle_t s_spk_dev_handle;
#endif
