#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

// Configuration for WiFi AP mode
#define WIFI_AP_SSID "ESP32-Scream"
#define WIFI_AP_PASSWORD ""  // Empty password for open networks
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONNECTIONS 4

// Maximum lengths for WiFi credentials
#define WIFI_SSID_MAX_LENGTH 32
#define WIFI_PASSWORD_MAX_LENGTH 64

// Timeout for connection attempts (in milliseconds)
#define WIFI_CONNECTION_TIMEOUT_MS 10000

// WiFi manager states
typedef enum {
    WIFI_MANAGER_STATE_NOT_INITIALIZED,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_CONNECTION_FAILED,
    WIFI_MANAGER_STATE_AP_MODE
} wifi_manager_state_t;

// Structure to hold WiFi network scan result info
typedef struct {
    char ssid[WIFI_SSID_MAX_LENGTH + 1];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_network_info_t;

/**
 * @brief Initialize the WiFi manager
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Start the WiFi manager. Will attempt to connect using stored credentials or start AP mode
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Stop the WiFi manager and release resources
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Get the current state of the WiFi manager
 * 
 * @return wifi_manager_state_t Current state
 */
wifi_manager_state_t wifi_manager_get_state(void);

/**
 * @brief Check if WiFi credentials are stored in NVS
 * 
 * @return true If credentials are stored
 * @return false If no credentials are stored
 */
bool wifi_manager_has_credentials(void);

/**
 * @brief Save WiFi credentials to NVS
 * 
 * @param ssid The WiFi SSID to save
 * @param password The WiFi password to save (can be NULL for open networks)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

/**
 * @brief Clear stored WiFi credentials from NVS
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_clear_credentials(void);

/**
 * @brief Connect to a specific WiFi network
 * 
 * @param ssid The SSID to connect to
 * @param password The password (can be NULL for open networks)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Get the currently connected SSID (if connected)
 * 
 * @param ssid Buffer to store the SSID
 * @param max_length Maximum length of the buffer
 * @return esp_err_t ESP_OK if connected and SSID returned
 */
esp_err_t wifi_manager_get_current_ssid(char *ssid, size_t max_length);

/**
 * @brief Scan for available WiFi networks
 * 
 * @param networks Array to store the found networks
 * @param max_networks Maximum number of networks to return
 * @param networks_found Number of networks actually found
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_scan_networks(wifi_network_info_t *networks, size_t max_networks, size_t *networks_found);
