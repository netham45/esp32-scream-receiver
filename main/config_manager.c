#include "config_manager.h"
#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

// Store the active configuration
static app_config_t s_app_config;

// NVS keys for different config parameters
#define NVS_KEY_PORT "port"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASSWORD "ap_password"
#define NVS_KEY_HIDE_AP_CONNECTED "hide_ap_conn"
#define NVS_KEY_INIT_BUF_SIZE "init_buf_sz"
#define NVS_KEY_BUF_GROW_STEP "buf_grow_step"
#define NVS_KEY_MAX_BUF_SIZE "max_buf_sz"
#define NVS_KEY_MAX_GROW_SIZE "max_grow_sz"
#define NVS_KEY_SAMPLE_RATE "sample_rate"
#define NVS_KEY_BIT_DEPTH "bit_depth"
#define NVS_KEY_VOLUME "volume"
#define NVS_KEY_SPDIF_DATA_PIN "spdif_pin"
#define NVS_KEY_SILENCE_THRES_MS "silence_ms"
#define NVS_KEY_NET_CHECK_MS "net_check_ms"
#define NVS_KEY_ACTIVITY_PACKETS "act_packets"
#define NVS_KEY_SILENCE_AMPLT "silence_amp"
#define NVS_KEY_NET_INACT_MS "net_inact_ms"
// USB Scream Sender keys
#define NVS_KEY_ENABLE_USB_SENDER "usb_sender"
#define NVS_KEY_SENDER_DEST_IP "sender_ip"
#define NVS_KEY_SENDER_DEST_PORT "sender_port"

// WiFi roaming keys
#define NVS_KEY_RSSI_THRESHOLD "rssi_thresh"

// Audio processing keys
#define NVS_KEY_USE_DIRECT_WRITE "direct_write"

/**
 * Initialize with default values from config.h
 */
static void set_default_config(void) {
    s_app_config.port = PORT;
    // Default AP SSID and password
    strcpy(s_app_config.ap_ssid, "ESP32-Scream");
    s_app_config.ap_password[0] = '\0'; // Default AP password is empty (open network)
    s_app_config.hide_ap_when_connected = true; // Hide AP when connected to WiFi by default
    s_app_config.initial_buffer_size = INITIAL_BUFFER_SIZE;
    s_app_config.buffer_grow_step_size = BUFFER_GROW_STEP_SIZE;
    s_app_config.max_buffer_size = MAX_BUFFER_SIZE;
    s_app_config.max_grow_size = MAX_GROW_SIZE;
    s_app_config.sample_rate = SAMPLE_RATE;
    s_app_config.bit_depth = BIT_DEPTH;
    s_app_config.volume = VOLUME;
    s_app_config.spdif_data_pin = 16; // Default SPDIF pin for ESP32-S3
    s_app_config.silence_threshold_ms = SILENCE_THRESHOLD_MS;
    s_app_config.network_check_interval_ms = NETWORK_CHECK_INTERVAL_MS;
    s_app_config.activity_threshold_packets = ACTIVITY_THRESHOLD_PACKETS;
    s_app_config.silence_amplitude_threshold = SILENCE_AMPLITUDE_THRESHOLD;
    s_app_config.network_inactivity_timeout_ms = NETWORK_INACTIVITY_TIMEOUT_MS;
    
    // USB Scream Sender defaults
    s_app_config.enable_usb_sender = false;
    strcpy(s_app_config.sender_destination_ip, "192.168.1.255"); // Default to broadcast
    s_app_config.sender_destination_port = 4010; // Default Scream port
    
    // WiFi roaming defaults
    s_app_config.rssi_threshold = -58; // Default RSSI threshold for roaming
    
    // Audio processing defaults
    s_app_config.use_direct_write = true; // Default to direct write mode
}

/**
 * Initialize configuration manager and load settings
 */
esp_err_t config_manager_init(void) {
    ESP_LOGI(TAG, "Initializing configuration manager");
    
    // Set default values first
    set_default_config();
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved configuration found, using defaults");
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Read network settings
    uint16_t port;
    err = nvs_get_u16(nvs_handle, NVS_KEY_PORT, &port);
    if (err == ESP_OK) {
        s_app_config.port = port;
    }
    
    // Read AP SSID
    size_t ssid_len = WIFI_SSID_MAX_LENGTH;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, s_app_config.ap_ssid, &ssid_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading AP SSID: %s", esp_err_to_name(err));
    }
    
    // Read AP password
    size_t len = WIFI_PASSWORD_MAX_LENGTH;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASSWORD, s_app_config.ap_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading AP password: %s", esp_err_to_name(err));
    }
    
    // Read hide AP when connected setting
    uint8_t hide_ap;
    err = nvs_get_u8(nvs_handle, NVS_KEY_HIDE_AP_CONNECTED, &hide_ap);
    if (err == ESP_OK) {
        s_app_config.hide_ap_when_connected = (bool)hide_ap;
    }
    
    // Read buffer settings
    uint8_t u8_value;
    err = nvs_get_u8(nvs_handle, NVS_KEY_INIT_BUF_SIZE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.initial_buffer_size = u8_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_BUF_GROW_STEP, &u8_value);
    if (err == ESP_OK) {
        s_app_config.buffer_grow_step_size = u8_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_MAX_BUF_SIZE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.max_buffer_size = u8_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_MAX_GROW_SIZE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.max_grow_size = u8_value;
    }
    
    // Read audio settings
    uint32_t sample_rate;
    err = nvs_get_u32(nvs_handle, NVS_KEY_SAMPLE_RATE, &sample_rate);
    if (err == ESP_OK) {
        s_app_config.sample_rate = sample_rate;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_BIT_DEPTH, &u8_value);
    if (err == ESP_OK) {
        s_app_config.bit_depth = u8_value;
    }
    
    // Read volume as u32 (stored as integer representation of float * 100)
    uint32_t volume_int;
    err = nvs_get_u32(nvs_handle, NVS_KEY_VOLUME, &volume_int);
    if (err == ESP_OK) {
        s_app_config.volume = (float)volume_int / 100.0f;
    }
    
    // Read SPDIF data pin
    err = nvs_get_u8(nvs_handle, NVS_KEY_SPDIF_DATA_PIN, &u8_value);
    if (err == ESP_OK) {
        s_app_config.spdif_data_pin = u8_value;
        ESP_LOGI(TAG, "Loaded SPDIF data pin: %d", s_app_config.spdif_data_pin);
    }
    
    // Read sleep settings
    uint32_t u32_value;
    err = nvs_get_u32(nvs_handle, NVS_KEY_SILENCE_THRES_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.silence_threshold_ms = u32_value;
    }
    
    err = nvs_get_u32(nvs_handle, NVS_KEY_NET_CHECK_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.network_check_interval_ms = u32_value;
    }
    
    err = nvs_get_u8(nvs_handle, NVS_KEY_ACTIVITY_PACKETS, &u8_value);
    if (err == ESP_OK) {
        s_app_config.activity_threshold_packets = u8_value;
    }
    
    uint16_t u16_value;
    err = nvs_get_u16(nvs_handle, NVS_KEY_SILENCE_AMPLT, &u16_value);
    if (err == ESP_OK) {
        s_app_config.silence_amplitude_threshold = u16_value;
    }
    
    err = nvs_get_u32(nvs_handle, NVS_KEY_NET_INACT_MS, &u32_value);
    if (err == ESP_OK) {
        s_app_config.network_inactivity_timeout_ms = u32_value;
    }
    
    // Read USB Scream Sender settings
    err = nvs_get_u8(nvs_handle, NVS_KEY_ENABLE_USB_SENDER, &u8_value);
    if (err == ESP_OK) {
        s_app_config.enable_usb_sender = (bool)u8_value;
    }
    
    char ip_str[16];
    size_t ip_len = sizeof(ip_str);
    err = nvs_get_str(nvs_handle, NVS_KEY_SENDER_DEST_IP, ip_str, &ip_len);
    if (err == ESP_OK) {
        strncpy(s_app_config.sender_destination_ip, ip_str, 15);
        s_app_config.sender_destination_ip[15] = '\0'; // Ensure null termination
    }
    
    err = nvs_get_u16(nvs_handle, NVS_KEY_SENDER_DEST_PORT, &u16_value);
    if (err == ESP_OK) {
        s_app_config.sender_destination_port = u16_value;
    }
    
    // Read WiFi roaming settings
    int8_t rssi_threshold;
    err = nvs_get_i8(nvs_handle, NVS_KEY_RSSI_THRESHOLD, &rssi_threshold);
    if (err == ESP_OK) {
        s_app_config.rssi_threshold = rssi_threshold;
    }
    
    // Read audio processing settings
    err = nvs_get_u8(nvs_handle, NVS_KEY_USE_DIRECT_WRITE, &u8_value);
    if (err == ESP_OK) {
        s_app_config.use_direct_write = (bool)u8_value;
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Configuration loaded");
    return ESP_OK;
}

/**
 * Get the current configuration
 */
app_config_t* config_manager_get_config(void) {
    return &s_app_config;
}

/**
 * Save the entire configuration to NVS
 */
esp_err_t config_manager_save_config(void) {
    ESP_LOGI(TAG, "Saving configuration to NVS");
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Save network settings
    err = nvs_set_u16(nvs_handle, NVS_KEY_PORT, s_app_config.port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving port: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP SSID
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, s_app_config.ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving AP SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save AP password
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASSWORD, s_app_config.ap_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving AP password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save hide AP when connected setting
    err = nvs_set_u8(nvs_handle, NVS_KEY_HIDE_AP_CONNECTED, (uint8_t)s_app_config.hide_ap_when_connected);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving hide AP setting: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save buffer settings
    err = nvs_set_u8(nvs_handle, NVS_KEY_INIT_BUF_SIZE, s_app_config.initial_buffer_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving initial buffer size: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_BUF_GROW_STEP, s_app_config.buffer_grow_step_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving buffer grow step: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_MAX_BUF_SIZE, s_app_config.max_buffer_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving max buffer size: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_MAX_GROW_SIZE, s_app_config.max_grow_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving max grow size: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save audio settings
    err = nvs_set_u32(nvs_handle, NVS_KEY_SAMPLE_RATE, s_app_config.sample_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving sample rate: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_BIT_DEPTH, s_app_config.bit_depth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving bit depth: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Store volume as integer (float * 100) for NVS
    uint32_t volume_int = (uint32_t)(s_app_config.volume * 100.0f);
    err = nvs_set_u32(nvs_handle, NVS_KEY_VOLUME, volume_int);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving volume: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save SPDIF data pin
    err = nvs_set_u8(nvs_handle, NVS_KEY_SPDIF_DATA_PIN, s_app_config.spdif_data_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving SPDIF data pin: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Saved SPDIF data pin: %d", s_app_config.spdif_data_pin);
    
    // Save sleep settings
    err = nvs_set_u32(nvs_handle, NVS_KEY_SILENCE_THRES_MS, s_app_config.silence_threshold_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving silence threshold: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u32(nvs_handle, NVS_KEY_NET_CHECK_MS, s_app_config.network_check_interval_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving network check interval: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_ACTIVITY_PACKETS, s_app_config.activity_threshold_packets);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving activity threshold packets: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u16(nvs_handle, NVS_KEY_SILENCE_AMPLT, s_app_config.silence_amplitude_threshold);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving silence amplitude threshold: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u32(nvs_handle, NVS_KEY_NET_INACT_MS, s_app_config.network_inactivity_timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving network inactivity timeout: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save USB Scream Sender settings
    err = nvs_set_u8(nvs_handle, NVS_KEY_ENABLE_USB_SENDER, (uint8_t)s_app_config.enable_usb_sender);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving USB sender enable: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SENDER_DEST_IP, s_app_config.sender_destination_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving sender destination IP: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u16(nvs_handle, NVS_KEY_SENDER_DEST_PORT, s_app_config.sender_destination_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving sender destination port: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save WiFi roaming settings
    err = nvs_set_i8(nvs_handle, NVS_KEY_RSSI_THRESHOLD, s_app_config.rssi_threshold);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving RSSI threshold: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit the changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Configuration saved successfully");
    return ESP_OK;
}

/**
 * Save a specific setting to NVS
 */
esp_err_t config_manager_save_setting(const char* key, void* value, size_t size) {
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving setting %s to NVS", key);
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Update in-memory configuration
    if (strcmp(key, NVS_KEY_PORT) == 0 && size == sizeof(uint16_t)) {
        s_app_config.port = *(uint16_t*)value;
        err = nvs_set_u16(nvs_handle, key, *(uint16_t*)value);
    } else if (strcmp(key, NVS_KEY_AP_SSID) == 0) {
        strncpy(s_app_config.ap_ssid, (char*)value, WIFI_SSID_MAX_LENGTH);
        s_app_config.ap_ssid[WIFI_SSID_MAX_LENGTH] = '\0'; // Ensure null termination
        err = nvs_set_str(nvs_handle, key, s_app_config.ap_ssid);
    } else if (strcmp(key, NVS_KEY_AP_PASSWORD) == 0) {
        strncpy(s_app_config.ap_password, (char*)value, WIFI_PASSWORD_MAX_LENGTH);
        s_app_config.ap_password[WIFI_PASSWORD_MAX_LENGTH] = '\0'; // Ensure null termination
        err = nvs_set_str(nvs_handle, key, s_app_config.ap_password);
    } else if (strcmp(key, NVS_KEY_HIDE_AP_CONNECTED) == 0 && size == sizeof(bool)) {
        s_app_config.hide_ap_when_connected = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.hide_ap_when_connected);
    } else if (strcmp(key, NVS_KEY_INIT_BUF_SIZE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.initial_buffer_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_BUF_GROW_STEP) == 0 && size == sizeof(uint8_t)) {
        s_app_config.buffer_grow_step_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_MAX_BUF_SIZE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.max_buffer_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_MAX_GROW_SIZE) == 0 && size == sizeof(uint8_t)) {
        s_app_config.max_grow_size = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_SAMPLE_RATE) == 0 && size == sizeof(uint32_t)) {
        s_app_config.sample_rate = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_BIT_DEPTH) == 0 && size == sizeof(uint8_t)) {
        s_app_config.bit_depth = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_VOLUME) == 0 && size == sizeof(float)) {
        s_app_config.volume = *(float*)value;
        uint32_t volume_int = (uint32_t)(s_app_config.volume * 100.0f);
        err = nvs_set_u32(nvs_handle, key, volume_int);
    } else if (strcmp(key, NVS_KEY_SPDIF_DATA_PIN) == 0 && size == sizeof(uint8_t)) {
        s_app_config.spdif_data_pin = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, s_app_config.spdif_data_pin);
        ESP_LOGI(TAG, "Saving SPDIF data pin value: %d", s_app_config.spdif_data_pin);
    } else if (strcmp(key, NVS_KEY_SILENCE_THRES_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.silence_threshold_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_NET_CHECK_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.network_check_interval_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_ACTIVITY_PACKETS) == 0 && size == sizeof(uint8_t)) {
        s_app_config.activity_threshold_packets = *(uint8_t*)value;
        err = nvs_set_u8(nvs_handle, key, *(uint8_t*)value);
    } else if (strcmp(key, NVS_KEY_SILENCE_AMPLT) == 0 && size == sizeof(uint16_t)) {
        s_app_config.silence_amplitude_threshold = *(uint16_t*)value;
        err = nvs_set_u16(nvs_handle, key, *(uint16_t*)value);
    } else if (strcmp(key, NVS_KEY_NET_INACT_MS) == 0 && size == sizeof(uint32_t)) {
        s_app_config.network_inactivity_timeout_ms = *(uint32_t*)value;
        err = nvs_set_u32(nvs_handle, key, *(uint32_t*)value);
    } else if (strcmp(key, NVS_KEY_ENABLE_USB_SENDER) == 0 && size == sizeof(bool)) {
        s_app_config.enable_usb_sender = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.enable_usb_sender);
    } else if (strcmp(key, NVS_KEY_SENDER_DEST_IP) == 0) {
        strncpy(s_app_config.sender_destination_ip, (char*)value, 15);
        s_app_config.sender_destination_ip[15] = '\0'; // Ensure null termination
        err = nvs_set_str(nvs_handle, key, s_app_config.sender_destination_ip);
    } else if (strcmp(key, NVS_KEY_SENDER_DEST_PORT) == 0 && size == sizeof(uint16_t)) {
        s_app_config.sender_destination_port = *(uint16_t*)value;
        err = nvs_set_u16(nvs_handle, key, s_app_config.sender_destination_port);
    } else if (strcmp(key, NVS_KEY_RSSI_THRESHOLD) == 0 && size == sizeof(int8_t)) {
        s_app_config.rssi_threshold = *(int8_t*)value;
        err = nvs_set_i8(nvs_handle, key, s_app_config.rssi_threshold);
    } else if (strcmp(key, NVS_KEY_USE_DIRECT_WRITE) == 0 && size == sizeof(bool)) {
        s_app_config.use_direct_write = *(bool*)value;
        err = nvs_set_u8(nvs_handle, key, (uint8_t)s_app_config.use_direct_write);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving setting %s: %s", key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit the changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    return err;
}

/**
 * Reset configuration to defaults
 */
esp_err_t config_manager_reset(void) {
    ESP_LOGI(TAG, "Resetting configuration to defaults");
    
    // Reset in-memory configuration to defaults
    set_default_config();
    
    // Open NVS handle
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // Namespace doesn't exist, so already at defaults
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // Erase all settings in this namespace
    err = nvs_erase_all(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error erasing NVS namespace: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // Commit the changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(err));
    }
    
    // Close NVS handle
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Configuration reset to defaults");
    return ESP_OK;
}
