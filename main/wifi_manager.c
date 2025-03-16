#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "wifi_manager";

// NVS namespace and keys for WiFi credentials
#define WIFI_NVS_NAMESPACE "wifi_config"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"

// Event group to signal WiFi connection events
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Static netif pointers for both STA and AP modes
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// Current state of the WiFi manager
static wifi_manager_state_t s_wifi_manager_state = WIFI_MANAGER_STATE_NOT_INITIALIZED;

// Flag to indicate we're in scan mode - used to prevent connection attempts
static bool s_in_scan_mode = false;

// Forward declarations for internal functions
static void wifi_event_handler(void *arg, esp_event_base_t event_base, 
                              int32_t event_id, void *event_data);
static esp_err_t start_ap_mode(void);

/**
 * Initialize the WiFi manager
 */
esp_err_t wifi_manager_init(void) {
    ESP_LOGI(TAG, "Initializing WiFi manager");
    
    // Check if already initialized
    if (s_wifi_manager_state != WIFI_MANAGER_STATE_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }
    
    // Initialize NVS for WiFi credential storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Create the event group for WiFi events
    s_wifi_event_group = xEventGroupCreate();
    
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default netif instances for both STA and AP
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    
    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers for WiFi events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler,
                                                       NULL,
                                                       NULL));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler,
                                                       NULL,
                                                       NULL));
    
    // Set WiFi mode to APSTA (both AP and STA can be active)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTING;
    
    return ESP_OK;
}

/**
 * Event handler for WiFi events
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, 
                              int32_t event_id, void *event_data) {
    
    static int s_retry_num = 0;
    
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            // WiFi station started, attempt to connect, but only if not in scan mode
            if (!s_in_scan_mode) {
                ESP_LOGI(TAG, "STA started, connecting to AP");
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disconn = event_data;
            
            if (s_retry_num < 5) {
                ESP_LOGI(TAG, "Failed to connect to AP (attempt %d/5), reason: %" PRIu16, 
                        s_retry_num + 1, disconn->reason);
                s_retry_num++;
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "Failed to connect to AP after 5 attempts");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTION_FAILED;
            }
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "Station connected to AP, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                   event->mac[0], event->mac[1], event->mac[2],
                   event->mac[3], event->mac[4], event->mac[5]);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "Station disconnected from AP, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                   event->mac[0], event->mac[1], event->mac[2],
                   event->mac[3], event->mac[4], event->mac[5]);
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP address: " IPSTR, 
                    IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTED;
        }
    }
}

/**
 * Start the WiFi manager
 */
esp_err_t wifi_manager_start(void) {
    ESP_LOGI(TAG, "Starting WiFi manager");
    
    // Initialize if not already initialized
    if (s_wifi_manager_state == WIFI_MANAGER_STATE_NOT_INITIALIZED) {
        ESP_ERROR_CHECK(wifi_manager_init());
    }
    
    // Check if we have stored credentials
    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Found stored WiFi credentials, trying to connect");
        
        // Read stored credentials
        char ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
        char password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};
        
        nvs_handle_t nvs_handle;
        ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle));
        
        size_t required_size = sizeof(ssid);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid, &required_size));
        
        required_size = sizeof(password);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password, &required_size));
        
        nvs_close(nvs_handle);
        
        // Configure WiFi station with the stored credentials
        wifi_config_t wifi_sta_config = {0};
        
        strncpy((char*)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
        strncpy((char*)wifi_sta_config.sta.password, password, sizeof(wifi_sta_config.sta.password));
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
        
        // Temporarily disable AP
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        
        // Start WiFi
        ESP_ERROR_CHECK(esp_wifi_start());
        
        // Wait for connection with timeout
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECTION_TIMEOUT_MS));
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGI(TAG, "Failed to connect to SSID: %s", ssid);
            ESP_ERROR_CHECK(esp_wifi_stop());
            
            // Start AP mode since connection failed
            return start_ap_mode();
        } else {
            ESP_LOGE(TAG, "Connection timeout");
            ESP_ERROR_CHECK(esp_wifi_stop());
            
            // Start AP mode since connection timed out
            return start_ap_mode();
        }
    } else {
        ESP_LOGI(TAG, "No stored WiFi credentials, starting AP mode");
        
        // No credentials, start AP mode directly
        return start_ap_mode();
    }
}

/**
 * Start AP mode with captive portal
 */
static esp_err_t start_ap_mode(void) {
    ESP_LOGI(TAG, "Starting AP mode");
    
    // Configure AP
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASSWORD,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(WIFI_AP_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_AP_MODE;
    
    ESP_LOGI(TAG, "AP started with SSID: %s", WIFI_AP_SSID);
    
    // Web server will be started by the web_server module
    
    return ESP_OK;
}

/**
 * Get the current state of the WiFi manager
 */
wifi_manager_state_t wifi_manager_get_state(void) {
    return s_wifi_manager_state;
}

/**
 * Get the current connected SSID (if any)
 */
esp_err_t wifi_manager_get_current_ssid(char *ssid, size_t max_length) {
    if (s_wifi_manager_state != WIFI_MANAGER_STATE_CONNECTED) {
        strncpy(ssid, "Not configured", max_length);
        ssid[max_length - 1] = '\0';
        return ESP_FAIL;
    }
    
    wifi_config_t wifi_config;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
    
    strncpy(ssid, (char*)wifi_config.sta.ssid, max_length);
    ssid[max_length - 1] = '\0'; // Ensure null-termination
    
    return ESP_OK;
}

/**
 * Check if credentials are stored in NVS
 */
bool wifi_manager_has_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "WiFi credentials not found in NVS (namespace not found)");
        } else {
            ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        }
        return false;
    }
    
    size_t required_size = 0;
    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, NULL, &required_size);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "WiFi credentials not found in NVS (no SSID key)");
        } else {
            ESP_LOGE(TAG, "Error reading SSID from NVS: %s", esp_err_to_name(ret));
        }
        return false;
    }
    
    return true;
}

/**
 * Save WiFi credentials to NVS
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Saving WiFi credentials for SSID: %s", ssid);
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving SSID to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Save password (even if empty, which is valid for open networks)
    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password ? password : "");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving password to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    return ret;
}

/**
 * Clear stored WiFi credentials from NVS
 */
esp_err_t wifi_manager_clear_credentials(void) {
    ESP_LOGI(TAG, "Clearing stored WiFi credentials");
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            // Namespace doesn't exist, which means no credentials are stored
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_erase_all(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error erasing NVS namespace: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    return ret;
}

/**
 * Connect to a specific WiFi network
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    
    // Save the credentials to NVS first
    esp_err_t ret = wifi_manager_save_credentials(ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials");
        return ret;
    }
    
    // Stop current WiFi mode
    ESP_ERROR_CHECK(esp_wifi_stop());
    
    // Clear the status bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Configure WiFi station with the new credentials
    wifi_config_t wifi_sta_config = {0};
    
    strncpy((char*)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
    if (password) {
        strncpy((char*)wifi_sta_config.sta.password, password, sizeof(wifi_sta_config.sta.password));
    }
    
    // Configure AP mode (use default settings)
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASSWORD,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(WIFI_AP_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Set APSTA mode (both AP and STA active)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTING;
    
    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                        pdFALSE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(WIFI_CONNECTION_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);
        s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTED;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", ssid);
        // Start AP mode again
        start_ap_mode();
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        // Start AP mode again
        start_ap_mode();
        return ESP_ERR_TIMEOUT;
    }
}

/**
 * Stop the WiFi manager and release resources
 */
esp_err_t wifi_manager_stop(void) {
    ESP_LOGI(TAG, "Stopping WiFi manager");
    
    // Stop WiFi
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_NOT_INITIALIZED;
    
    return ESP_OK;
}

/**
 * Scan for available WiFi networks
 */
esp_err_t wifi_manager_scan_networks(wifi_network_info_t *networks, size_t max_networks, 
                                    size_t *networks_found) {
    ESP_LOGI(TAG, "Scanning for WiFi networks");
    
    if (!networks || max_networks == 0 || !networks_found) {
        ESP_LOGE(TAG, "Invalid arguments for network scan");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize to 0 in case we return early
    *networks_found = 0;
    
    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set scanning flag to prevent connection attempts
    s_in_scan_mode = true;
    
    // If we're in AP mode only, we need to switch to APSTA mode temporarily
    bool mode_changed = false;
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Temporarily switching to APSTA mode for scanning");
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to APSTA mode: %s", esp_err_to_name(ret));
            s_in_scan_mode = false;
            return ret;
        }
        mode_changed = true;
    }
    
    // Initialize scan configuration
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 0
    };
    
    // Start scan - use normal error handling instead of ESP_ERROR_CHECK
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi scan: %s", esp_err_to_name(ret));
        
        // Restore original mode if needed
        if (mode_changed) {
            esp_wifi_set_mode(current_mode);
        }
        
        return ret;
    }
    
    // Get scan results - use normal error handling
    uint16_t num_ap = 0;
    ret = esp_wifi_scan_get_ap_num(&num_ap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP scan count: %s", esp_err_to_name(ret));
        
        // Restore original mode if needed
        if (mode_changed) {
            esp_wifi_set_mode(current_mode);
        }
        
        return ret;
    }
    
    if (num_ap == 0) {
        ESP_LOGI(TAG, "No networks found");
        *networks_found = 0;
        
        // Restore original mode if needed
        if (mode_changed) {
            esp_wifi_set_mode(current_mode);
        }
        
        return ESP_OK;
    }
    
    // Limit the number of results to max_networks
    if (num_ap > max_networks) {
        num_ap = max_networks;
    }
    
    // Allocate memory for scan results
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * num_ap);
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        
        // Restore original mode if needed
        if (mode_changed) {
            esp_wifi_set_mode(current_mode);
        }
        
        return ESP_ERR_NO_MEM;
    }
    
    // Get the actual scan results - use normal error handling
    ret = esp_wifi_scan_get_ap_records(&num_ap, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP scan records: %s", esp_err_to_name(ret));
        free(ap_records);
        
        // Restore original mode if needed
        if (mode_changed) {
            esp_wifi_set_mode(current_mode);
        }
        
        return ret;
    }
    
    // Copy the results to the output array
    for (int i = 0; i < num_ap; i++) {
        strncpy(networks[i].ssid, (char *)ap_records[i].ssid, WIFI_SSID_MAX_LENGTH);
        networks[i].ssid[WIFI_SSID_MAX_LENGTH] = '\0'; // Ensure null-termination
        networks[i].rssi = ap_records[i].rssi;
        networks[i].authmode = ap_records[i].authmode;
    }
    
    // Free the allocated memory
    free(ap_records);
    
    // Restore original mode if needed
    if (mode_changed) {
        ESP_LOGI(TAG, "Restoring original WiFi mode after scan");
        esp_wifi_set_mode(current_mode);
    }
    
    // Reset the scan mode flag
    s_in_scan_mode = false;
    
    // Return the actual number of networks found
    *networks_found = num_ap;
    
    ESP_LOGI(TAG, "Found %" PRIu16 " networks", num_ap);
    return ESP_OK;
}
