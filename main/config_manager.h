#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifi_manager.h" // For WIFI_PASSWORD_MAX_LENGTH

// NVS namespace for storing configuration
#define CONFIG_NVS_NAMESPACE "app_config"

typedef struct {
    // Network
    uint16_t port;
    
    // WiFi AP configuration
    char ap_ssid[WIFI_SSID_MAX_LENGTH + 1];         // AP mode SSID
    char ap_password[WIFI_PASSWORD_MAX_LENGTH + 1]; // AP mode password (empty for open network)
    bool hide_ap_when_connected;                    // Hide AP when connected to WiFi
    
    // Buffer configuration
    uint8_t initial_buffer_size;
    uint8_t buffer_grow_step_size;
    uint8_t max_buffer_size;
    uint8_t max_grow_size;
    
    // Audio configuration
    uint32_t sample_rate;
    uint8_t bit_depth;
    float volume;
    uint8_t spdif_data_pin;  // Only used when IS_SPDIF is defined
    
    // Sleep configuration
    uint32_t silence_threshold_ms;
    uint32_t network_check_interval_ms;
    uint8_t activity_threshold_packets;
    uint16_t silence_amplitude_threshold;
    uint32_t network_inactivity_timeout_ms;
    
    // USB Scream Sender configuration
    bool enable_usb_sender;                // Enable USB Scream Sender functionality
    char sender_destination_ip[16];        // Destination IP for audio packets
    uint16_t sender_destination_port;      // Destination port for audio packets
    
    // Audio processing configuration
    bool use_direct_write;                 // Use direct write instead of buffering
} app_config_t;

// Initialize configuration (load from NVS or use defaults)
esp_err_t config_manager_init(void);

// Get current configuration
app_config_t* config_manager_get_config(void);

// Save configuration to NVS
esp_err_t config_manager_save_config(void);

// Save specific configuration to NVS
esp_err_t config_manager_save_setting(const char* key, void* value, size_t size);

// Reset configuration to defaults
esp_err_t config_manager_reset(void);
