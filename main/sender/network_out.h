#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Initialize the Scream sender functionality
 * This sets up the necessary components but doesn't start sending
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t scream_sender_init(void);

/**
 * Start the Scream sender
 * This begins capture from USB audio and sending to the network
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t scream_sender_start(void);

/**
 * Stop the Scream sender
 * This halts capture and network sending
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t scream_sender_stop(void);

/**
 * Check if the Scream sender is currently running
 *
 * @return true if running, false otherwise
 */
bool scream_sender_is_running(void);

/**
 * Set the mute state for the Scream sender
 *
 * @param mute true to mute, false to unmute
 */
void scream_sender_set_mute(bool mute);

/**
 * Set the volume for the Scream sender (0-100)
 *
 * @param volume Volume level (0-100)
 */
void scream_sender_set_volume(uint32_t volume);

/**
 * Update the destination IP and port for the Scream sender
 * This can be called while the sender is running to change the destination
 *
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t scream_sender_update_destination(void);
