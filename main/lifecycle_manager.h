#pragma once

#include "esp_err.h"

/**
 * @brief System lifecycle states
 */
typedef enum {
    LIFECYCLE_STATE_INITIALIZING,
    LIFECYCLE_STATE_HW_INIT,
    LIFECYCLE_STATE_STARTING_SERVICES,
    LIFECYCLE_STATE_AWAITING_MODE_CONFIG,
    LIFECYCLE_STATE_MODE_SENDER_USB,
    LIFECYCLE_STATE_MODE_SENDER_SPDIF,
    LIFECYCLE_STATE_MODE_RECEIVER_USB,
    LIFECYCLE_STATE_MODE_RECEIVER_SPDIF,
    LIFECYCLE_STATE_SLEEPING,
    LIFECYCLE_STATE_ERROR,
} lifecycle_state_t;

/**
 * @brief Events that can be sent to the lifecycle manager
 */
typedef enum {
    LIFECYCLE_EVENT_WIFI_CONNECTED,
    LIFECYCLE_EVENT_WIFI_DISCONNECTED,
    LIFECYCLE_EVENT_CONFIGURATION_CHANGED,
    LIFECYCLE_EVENT_USB_DAC_CONNECTED,
    LIFECYCLE_EVENT_USB_DAC_DISCONNECTED,
    LIFECYCLE_EVENT_ENTER_SLEEP,
    LIFECYCLE_EVENT_WAKE_UP,
} lifecycle_event_t;

/**
 * @brief Initialize and start the lifecycle manager.
 *
 * This function creates the lifecycle manager task and event queue.
 * It should be called once from app_main.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t lifecycle_manager_init(void);

/**
 * @brief Post an event to the lifecycle manager's event queue.
 *
 * This function is thread-safe and can be called from any task or ISR.
 *
 * @param event The event to post.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t lifecycle_manager_post_event(lifecycle_event_t event);

/**
 * @brief Report network activity to the lifecycle manager.
 *
 * This function is called by the network input module when a packet is received.
 * It is used to wake the device from sleep.
 */
void lifecycle_manager_report_network_activity(void);