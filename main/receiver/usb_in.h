#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the USB host tasks for UAC.
 */
esp_err_t usb_in_start(void);

/**
 * @brief Stop the USB host tasks for UAC.
 */
esp_err_t usb_in_stop(void);

#ifdef __cplusplus
}
#endif