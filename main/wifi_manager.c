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
#include "config_manager.h"
#include "esp_wnm.h"
#include "esp_rrm.h"
#include "esp_mbo.h"
#include "esp_mac.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "wifi_manager";

// Roaming-related definitions
#ifndef WLAN_EID_MEASURE_REPORT
#define WLAN_EID_MEASURE_REPORT 39
#endif
#ifndef MEASURE_TYPE_LCI
#define MEASURE_TYPE_LCI 9
#endif
#ifndef MEASURE_TYPE_LOCATION_CIVIC
#define MEASURE_TYPE_LOCATION_CIVIC 11
#endif
#ifndef WLAN_EID_NEIGHBOR_REPORT
#define WLAN_EID_NEIGHBOR_REPORT 52
#endif
#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#define MAX_LCI_CIVIC_LEN 256 * 2 + 1
#define MAX_NEIGHBOR_LEN 512

// Default RSSI threshold for roaming
#define DEFAULT_RSSI_THRESHOLD -58

// Flag to track if neighbor report is active - moved from main file
bool g_neighbor_report_active = false;

// Helper function to get 32-bit LE value
static inline uint32_t WPA_GET_LE32(const uint8_t *a)
{
    return ((uint32_t) a[3] << 24) | (a[2] << 16) | (a[1] << 8) | a[0];
}

// NVS namespace and keys for WiFi credentials
#define WIFI_NVS_NAMESPACE "wifi_config"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"
#define WIFI_NVS_KEY_RSSI_THRESHOLD "rssi_threshold"

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
    
    // Initialize the TCP/IP stack (safely - it might be initialized already)
    esp_err_t net_err = esp_netif_init();
    if (net_err != ESP_OK && net_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(net_err);
    }
    
    // Create the default event loop, but don't error if it already exists
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    
    // Create default netif instances for both STA and AP - check if they already exist
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    
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
            
            // Re-enable AP mode if it was hidden while connected
            app_config_t* config = config_manager_get_config();
            if (config->hide_ap_when_connected) {
                ESP_LOGI(TAG, "Re-enabling AP interface after disconnection");
                esp_wifi_set_mode(WIFI_MODE_APSTA);
            }
            
            // Check if this is a roaming disconnect
            if (disconn->reason == WIFI_REASON_ROAMING) {
                ESP_LOGI(TAG, "Disconnected due to roaming, waiting for reconnection");
                // No need to manually reconnect, the system will handle it
                return;
            }
            
            // Implement indefinite retries with backoff
            s_retry_num++;
            
            // Calculate backoff delay (exponential with cap)
            int delay_ms = 1000; // Base delay of 1 second
            if (s_retry_num > 1) {
                // Exponential backoff with a maximum of 30 seconds
                delay_ms = (1 << (s_retry_num > 5 ? 5 : s_retry_num)) * 1000;
                if (delay_ms > 30000) {
                    delay_ms = 30000;
                }
            }
            
            ESP_LOGI(TAG, "Connection attempt %d failed, reason: %" PRIu16 ", retrying in %d ms", 
                    s_retry_num, disconn->reason, delay_ms);
            
            // Wait before reconnecting
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            esp_wifi_connect();
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
            
            // If configured to hide AP when connected, disable AP interface
            app_config_t* config = config_manager_get_config();
            if (config->hide_ap_when_connected) {
                ESP_LOGI(TAG, "Disabling AP interface when connected (as configured)");
                esp_wifi_set_mode(WIFI_MODE_STA);
            }
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
    
    // Get AP configuration from config manager
    app_config_t* config = config_manager_get_config();
    const char* ap_password = config->ap_password;
    
    // Configure AP mode
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Use custom AP SSID from config
    strncpy((char*)wifi_ap_config.ap.ssid, config->ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(config->ap_ssid);
    
    // Copy password if provided
    if (strlen(ap_password) > 0) {
        strncpy((char*)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    
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
        
        // Use APSTA mode to have both interfaces active
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        
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
    
    // Get AP password from config manager
    app_config_t* config = config_manager_get_config();
    const char* ap_password = config->ap_password;
    
    // Configure AP
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Use custom AP SSID from config
    strncpy((char*)wifi_ap_config.ap.ssid, config->ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(config->ap_ssid);
    
    // Copy password if provided
    if (strlen(ap_password) > 0) {
        strncpy((char*)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
        ESP_LOGI(TAG, "Using configured AP password (password protected)");
    } else {
        ESP_LOGI(TAG, "Using open AP (no password)");
    }
    
    // Use APSTA mode instead of AP mode only to allow future STA connections 
    // without disabling the AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_AP_MODE;
    
    ESP_LOGI(TAG, "AP started with SSID: %s", config->ap_ssid);
    
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
    
    // Get AP password from config manager
    app_config_t* config = config_manager_get_config();
    const char* ap_password = config->ap_password;
    
    // Configure AP mode
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = strlen(ap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };
    
    // Use custom AP SSID from config
    strncpy((char*)wifi_ap_config.ap.ssid, config->ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(config->ap_ssid);
    
    // Copy password if provided
    if (strlen(ap_password) > 0) {
        strncpy((char*)wifi_ap_config.ap.password, ap_password, sizeof(wifi_ap_config.ap.password));
    }
    
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
 * Connect to strongest available network
 */
esp_err_t wifi_manager_connect_to_strongest(void) {
    ESP_LOGI(TAG, "Scanning and connecting to strongest network");
    
    // First, stop current WiFi connection if any
    esp_wifi_disconnect();
    
    // Set APSTA mode and start WiFi if not already started
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    // Try to start WiFi
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err == ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        return err;
    }
    
    // Clear the status bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Scan for networks - use maximum scan results (30 is a reasonable limit)
    #define MAX_SCAN_RESULTS 30
    wifi_network_info_t networks[MAX_SCAN_RESULTS];
    size_t networks_found = 0;
    
    esp_err_t ret = wifi_manager_scan_networks(networks, MAX_SCAN_RESULTS, &networks_found);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to scan networks: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (networks_found == 0) {
        ESP_LOGI(TAG, "No networks found");
        return ESP_FAIL;
    }
    
    // Get stored SSID to only connect to previously configured networks
    nvs_handle_t nvs_handle;
    char stored_ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char stored_password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};
    bool has_stored_credentials = false;
    
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t required_size = sizeof(stored_ssid);
        if (nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, stored_ssid, &required_size) == ESP_OK) {
            // Also get the password
            required_size = sizeof(stored_password);
            nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, stored_password, &required_size);
            has_stored_credentials = true;
        }
        nvs_close(nvs_handle);
    }
    
    if (!has_stored_credentials) {
        ESP_LOGI(TAG, "No stored WiFi credentials found, cannot connect to any network");
        return ESP_FAIL;
    }
    
    // Find the stored network with the strongest signal
    int strongest_index = -1;
    int strongest_rssi = -128; // Minimum possible RSSI
    
    for (int i = 0; i < networks_found; i++) {
        // Only consider the network that matches our stored SSID
        if (strcmp(networks[i].ssid, stored_ssid) == 0) {
            // This is our stored network, check signal strength
            if (networks[i].rssi > strongest_rssi) {
                strongest_rssi = networks[i].rssi;
                strongest_index = i;
            }
        }
    }
    
    if (strongest_index == -1) {
        ESP_LOGI(TAG, "Stored network '%s' not found in scan results", stored_ssid);
        return ESP_FAIL;
    }
    
    // Connect to the stored network
    ESP_LOGI(TAG, "Connecting to stored network: %s (RSSI: %d)", 
             networks[strongest_index].ssid, networks[strongest_index].rssi);
    
    // Configure WiFi station with the stored network
    wifi_config_t wifi_sta_config = {0};
    
    strncpy((char*)wifi_sta_config.sta.ssid, networks[strongest_index].ssid, sizeof(wifi_sta_config.sta.ssid));
    strncpy((char*)wifi_sta_config.sta.password, stored_password, sizeof(wifi_sta_config.sta.password));
    
    // Update WiFi configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    
    // Connect to the network
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    // Update state
    s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTING;
    
    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                        pdFALSE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(WIFI_CONNECTION_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to stored network: %s", networks[strongest_index].ssid);
        s_wifi_manager_state = WIFI_MANAGER_STATE_CONNECTED;
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "Failed to connect to stored network: %s", networks[strongest_index].ssid);
        return ESP_FAIL;
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

/**
 * Process neighbor report received from AP
 */
void wifi_manager_neighbor_report_recv_handler(void* arg, esp_event_base_t event_base, 
                                              int32_t event_id, void* event_data)
{
    if (!g_neighbor_report_active) {
        ESP_LOGV(TAG, "Neighbor report received but not triggered by us");
        return;
    }
    
    if (!event_data) {
        ESP_LOGE(TAG, "No event data received for neighbor report");
        return;
    }
    
    g_neighbor_report_active = false;
    wifi_event_neighbor_report_t *neighbor_report_event = (wifi_event_neighbor_report_t*)event_data;
    uint8_t *pos = (uint8_t *)neighbor_report_event->report;
    char *neighbor_list = NULL;
    
    if (!pos) {
        ESP_LOGE(TAG, "Neighbor report is empty");
        return;
    }
    
    uint8_t report_len = neighbor_report_event->report_len;
    
    // Dump report info for debugging
    ESP_LOGD(TAG, "rrm: neighbor report len=%" PRIu16, report_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, pos, report_len, ESP_LOG_DEBUG);

    // Create neighbor list
    neighbor_list = wifi_manager_get_btm_neighbor_list(pos + 1, report_len - 1);
    
    // Send BTM query with neighbor list
    if (neighbor_list) {
        ESP_LOGI(TAG, "Sending BTM query with neighbor list");
        esp_wnm_send_bss_transition_mgmt_query(REASON_FRAME_LOSS, neighbor_list, 0);
        free(neighbor_list);
    } else {
        // Send BTM query without candidates
        ESP_LOGI(TAG, "Sending BTM query without neighbor list");
        esp_wnm_send_bss_transition_mgmt_query(REASON_FRAME_LOSS, NULL, 0);
    }
}

/**
 * Handle low RSSI event from AP
 */
void wifi_manager_bss_rssi_low_handler(void* arg, esp_event_base_t event_base,
                                      int32_t event_id, void* event_data)
{
    wifi_event_bss_rssi_low_t *event = event_data;
    ESP_LOGI(TAG, "BSS RSSI is low: %d", event->rssi);
    
    // Check if RRM is supported
    if (esp_rrm_is_rrm_supported_connection()) {
        ESP_LOGI(TAG, "Sending neighbor report request (RRM supported)");
        if (esp_rrm_send_neighbor_report_request() == ESP_OK) {
            g_neighbor_report_active = true;
        } else {
            ESP_LOGI(TAG, "Failed to send neighbor report request, sending BTM query without candidates");
            // Send BTM query directly without neighbor list
            esp_wnm_send_bss_transition_mgmt_query(REASON_FRAME_LOSS, NULL, 0);
        }
    } else if (esp_wnm_is_btm_supported_connection()) {
        // If RRM not supported but BTM is supported, send BTM query directly
        ESP_LOGI(TAG, "RRM not supported but BTM is, sending BTM query without candidates");
        esp_wnm_send_bss_transition_mgmt_query(REASON_FRAME_LOSS, NULL, 0);
    } else {
        ESP_LOGI(TAG, "Neither RRM nor BTM supported by current AP");
    }
}

/**
 * Create a BTM neighbor list from a neighbor report
 */
char* wifi_manager_get_btm_neighbor_list(uint8_t *report, size_t report_len)
{
    size_t len = 0;
    const uint8_t *data;
    int ret = 0;
    char *lci = NULL;
    char *civic = NULL;
    
    // Minimum length for a neighbor report element
    #define NR_IE_MIN_LEN (ETH_ALEN + 4 + 1 + 1 + 1)

    if (!report || report_len == 0) {
        ESP_LOGI(TAG, "RRM neighbor report is not valid");
        return NULL;
    }

    char *buf = calloc(1, MAX_NEIGHBOR_LEN);
    if (!buf) {
        ESP_LOGE(TAG, "Memory allocation for neighbor list failed");
        goto cleanup;
    }
    
    data = report;

    while (report_len >= 2 + NR_IE_MIN_LEN) {
        const uint8_t *nr;
        lci = (char *)malloc(sizeof(char)*MAX_LCI_CIVIC_LEN);
        if (!lci) {
            ESP_LOGE(TAG, "Memory allocation for lci failed");
            goto cleanup;
        }
        
        civic = (char *)malloc(sizeof(char)*MAX_LCI_CIVIC_LEN);
        if (!civic) {
            ESP_LOGE(TAG, "Memory allocation for civic failed");
            goto cleanup;
        }
        
        uint8_t nr_len = data[1];
        const uint8_t *pos = data, *end;

        if (pos[0] != WLAN_EID_NEIGHBOR_REPORT ||
            nr_len < NR_IE_MIN_LEN) {
            ESP_LOGI(TAG, "Invalid Neighbor Report element: id=%" PRIu16 " len=%" PRIu16,
                    data[0], nr_len);
            ret = -1;
            goto cleanup;
        }

        if (2U + nr_len > report_len) {
            ESP_LOGI(TAG, "Invalid Neighbor Report element: id=%" PRIu16 " len=%" PRIu16 " nr_len=%" PRIu16,
                    data[0], report_len, nr_len);
            ret = -1;
            goto cleanup;
        }
        
        pos += 2;
        end = pos + nr_len;

        nr = pos;
        pos += NR_IE_MIN_LEN;

        lci[0] = '\0';
        civic[0] = '\0';
        
        while (end - pos > 2) {
            uint8_t s_id, s_len;

            s_id = *pos++;
            s_len = *pos++;
            
            if (s_len > end - pos) {
                ret = -1;
                goto cleanup;
            }
            
            if (s_id == WLAN_EID_MEASURE_REPORT && s_len > 3) {
                // Measurement Token[1]
                // Measurement Report Mode[1]
                // Measurement Type[1]
                // Measurement Report[variable]
                switch (pos[2]) {
                    case MEASURE_TYPE_LCI:
                        if (lci[0])
                            break;
                        memcpy(lci, pos, s_len);
                        break;
                    case MEASURE_TYPE_LOCATION_CIVIC:
                        if (civic[0])
                            break;
                        memcpy(civic, pos, s_len);
                        break;
                }
            }

            pos += s_len;
        }

        ESP_LOGI(TAG, "RMM neighbor report bssid=" MACSTR
                " info=0x%" PRIx32 " op_class=%" PRIu16 " chan=%" PRIu16 " phy_type=%" PRIu16 "%s%s%s%s",
                MAC2STR(nr), WPA_GET_LE32(nr + ETH_ALEN),
                nr[ETH_ALEN + 4], nr[ETH_ALEN + 5],
                nr[ETH_ALEN + 6],
                lci[0] ? " lci=" : "", lci,
                civic[0] ? " civic=" : "", civic);

        // neighbor start
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, " neighbor=");
        // bssid
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, MACSTR, MAC2STR(nr));
        // ,
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
        // bssid info
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "0x%04" PRIx32 "", WPA_GET_LE32(nr + ETH_ALEN));
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
        // operating class
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%" PRIu16, nr[ETH_ALEN + 4]);
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
        // channel number
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%" PRIu16, nr[ETH_ALEN + 5]);
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
        // phy type
        len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%" PRIu16, nr[ETH_ALEN + 6]);
        // optional elements, skip

        data = end;
        report_len -= 2 + nr_len;

        if (lci) {
            free(lci);
            lci = NULL;
        }
        
        if (civic) {
            free(civic);
            civic = NULL;
        }
    }

cleanup:
    if (lci) {
        free(lci);
    }
    
    if (civic) {
        free(civic);
    }

    if (ret < 0) {
        free(buf);
        buf = NULL;
    }
    
    return buf;
}

/**
 * Set the RSSI threshold for roaming
 */
esp_err_t wifi_manager_set_rssi_threshold(int8_t rssi_threshold) {
    ESP_LOGI(TAG, "Setting RSSI threshold to %d", rssi_threshold);
    
    // Save the threshold to NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_i8(nvs_handle, WIFI_NVS_KEY_RSSI_THRESHOLD, rssi_threshold);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving RSSI threshold to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // Commit the changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing changes to NVS: %s", esp_err_to_name(ret));
    }
    
    nvs_close(nvs_handle);
    
    // Apply the threshold to the WiFi driver
    return esp_wifi_set_rssi_threshold(rssi_threshold);
}

/**
 * Get the current RSSI threshold for roaming
 */
esp_err_t wifi_manager_get_rssi_threshold(int8_t *rssi_threshold) {
    if (!rssi_threshold) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Try to read from NVS first
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (ret == ESP_OK) {
        ret = nvs_get_i8(nvs_handle, WIFI_NVS_KEY_RSSI_THRESHOLD, rssi_threshold);
        nvs_close(nvs_handle);
        
        if (ret == ESP_OK) {
            // Successfully read from NVS
            return ESP_OK;
        }
    }
    
    // If not found in NVS, use the default value
    *rssi_threshold = DEFAULT_RSSI_THRESHOLD;
    return ESP_OK;
}

/**
 * Configure WiFi for fast roaming
 */
esp_err_t wifi_manager_configure_fast_roaming(void) {
    ESP_LOGI(TAG, "Configuring fast roaming (802.11r, PMF)");
    
    // Get current WiFi configuration
    wifi_config_t wifi_config;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
    
    // Enable 802.11r Fast Transition
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;  // PMF capable but not required
    wifi_config.sta.ft_enabled = true;         // Enable Fast Transition (802.11r)
    
    // Apply the updated configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Set RSSI threshold from stored value or default
    int8_t rssi_threshold;
    wifi_manager_get_rssi_threshold(&rssi_threshold);
    ESP_ERROR_CHECK(esp_wifi_set_rssi_threshold(rssi_threshold));
    
    return ESP_OK;
}

/**
 * Initialize roaming functionality
 */
esp_err_t wifi_manager_init_roaming(void) {
    ESP_LOGI(TAG, "Initializing WiFi roaming");
    
    // Register event handlers for roaming-related events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_NEIGHBOR_REP,
                                              &wifi_manager_neighbor_report_recv_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW,
                                              &wifi_manager_bss_rssi_low_handler, NULL));
    
    // Configure fast roaming
    ESP_ERROR_CHECK(wifi_manager_configure_fast_roaming());
    
    return ESP_OK;
}
