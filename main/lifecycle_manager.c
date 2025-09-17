#include "lifecycle_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "global.h"
#include "wifi/wifi_manager.h"
#include "web/web_server.h"
#include "mdns/mdns_service.h"
#include "ntp/ntp_client.h"
#include "config/config_manager.h"
#include "sender/network_out.h"
#include "receiver/network_in.h"
#include "receiver/audio_out.h"
#include "receiver/buffer.h"
#ifdef IS_SPDIF
#include "sender/spdif_in/spdif_in.h"
#endif
#include "receiver/usb_in.h"
#include "receiver/spdif_out.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#ifdef IS_USB
#include "usb/usb_host.h"
#endif
#include "bq25895/bq25895_integration.h"


#define LIFECYCLE_EVENT_QUEUE_SIZE 10

// Import the network activity event group from esp32_scream.c
extern EventGroupHandle_t s_network_activity_event_group;

static QueueHandle_t s_lifecycle_event_queue = NULL;
static lifecycle_state_t s_current_state = LIFECYCLE_STATE_INITIALIZING;

// Task for monitoring network activity during silence sleep mode
static TaskHandle_t network_monitor_task_handle = NULL;
static volatile uint32_t packet_counter = 0;
static volatile bool monitoring_active = false;
static volatile TickType_t last_packet_time = 0;

// Forward declarations from audio.c for the silence tracking variables
// TODO: These should be replaced with a proper API
extern bool is_silent;
extern uint32_t silence_duration_ms;
extern TickType_t last_audio_time;


// Forward declarations for mode start/stop functions
static void start_mode_sender_usb(void);
static void stop_mode_sender_usb(void);
static void start_mode_sender_spdif(void);
static void stop_mode_sender_spdif(void);
static void start_mode_receiver_usb(void);
static void stop_mode_receiver_usb(void);
static void start_mode_receiver_spdif(void);
static void stop_mode_receiver_spdif(void);

// Forward declarations for state handlers
static void handle_state_initializing(lifecycle_event_t event);
static void handle_state_hw_init(lifecycle_event_t event);
static void handle_state_starting_services(lifecycle_event_t event);
static void handle_state_awaiting_mode_config(lifecycle_event_t event);
static void handle_state_mode_sender_usb(lifecycle_event_t event);
static void handle_state_mode_sender_spdif(lifecycle_event_t event);
static void handle_state_mode_receiver_usb(lifecycle_event_t event);
static void handle_state_mode_receiver_spdif(lifecycle_event_t event);
static void handle_state_sleeping(lifecycle_event_t event);
static void handle_state_error(lifecycle_event_t event);
static void enter_silence_sleep_mode(void);
static void exit_silence_sleep_mode(void);

// Forward declaration
static void set_state(lifecycle_state_t new_state);
static void evaluate_and_transition(void);

static void handle_state_entry(lifecycle_state_t state) {
    ESP_LOGI(TAG, "LIFECYCLE: Entering state %d", state);
    switch (state) {
        case LIFECYCLE_STATE_HW_INIT: {
            // Initialize NVS
            esp_err_t ret = nvs_flash_init();
            if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_ERROR_CHECK(nvs_flash_erase());
                ret = nvs_flash_init();
            }
            ESP_ERROR_CHECK(ret);

            // Initialize configuration manager
            ESP_LOGI(TAG, "Initializing configuration manager");
            ESP_ERROR_CHECK(config_manager_init());

            // Initialize BQ25895 Battery Charger
            ESP_LOGI(TAG, "Initializing BQ25895 battery charger");
            esp_err_t bq_err = bq25895_integration_init();
            if (bq_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize BQ25895: %s", esp_err_to_name(bq_err));
                // Non-critical, continue
            } else {
                ESP_LOGI(TAG, "BQ25895 initialized successfully");
            }

            // Power management configuration
            #if CONFIG_PM_ENABLE
            ESP_LOGI(TAG, "Configuring power management (reduced CPU clock)");
            #if CONFIG_IDF_TARGET_ESP32
            esp_pm_config_esp32_t pm_config = {
                .max_freq_mhz = 80,
                .min_freq_mhz = 40,
            #if CONFIG_FREERTOS_USE_TICKLESS_IDLE
                .light_sleep_enable = true
            #else
                .light_sleep_enable = false
            #endif
            };
            #elif CONFIG_IDF_TARGET_ESP32S3
            esp_pm_config_esp32s3_t pm_config = {
                .max_freq_mhz = 80,
                .min_freq_mhz = 40,
            #if CONFIG_FREERTOS_USE_TICKLESS_IDLE
                .light_sleep_enable = true
            #else
                .light_sleep_enable = false
            #endif
            };
            #endif
            esp_err_t err = esp_pm_configure(&pm_config);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Power management not supported or not enabled in menuconfig");
            } else {
                ESP_ERROR_CHECK(err);
            }
            #else
            ESP_LOGW(TAG, "Power management not enabled in menuconfig");
            #endif

            app_config_t *config = config_manager_get_config();
            
            // Log configuration at initialization
            ESP_LOGI(TAG, "========== CONFIGURATION AT HW_INIT ==========");
            ESP_LOGI(TAG, "Build configuration: %s",
                #ifdef IS_USB
                    "IS_USB"
                #elif defined(IS_SPDIF)
                    "IS_SPDIF"
                #else
                    "UNDEFINED"
                #endif
            );
            ESP_LOGI(TAG, "enable_usb_sender: %s", config->enable_usb_sender ? "true" : "false");
            ESP_LOGI(TAG, "enable_spdif_sender: %s", config->enable_spdif_sender ? "true" : "false");
            ESP_LOGI(TAG, "sample_rate: %d", config->sample_rate);
            ESP_LOGI(TAG, "spdif_data_pin: %d", config->spdif_data_pin);
            ESP_LOGI(TAG, "===============================================");

            // If S/PDIF sender is enabled, skip DAC detection
            if (config->enable_spdif_sender) {
                ESP_LOGI(TAG, "S/PDIF sender mode enabled, proceeding to start services");
                set_state(LIFECYCLE_STATE_STARTING_SERVICES);
                break;
            }

            #ifdef IS_USB
            // Only initialize USB host for DAC detection if sender mode is NOT enabled
            if (!config->enable_usb_sender) {
                ESP_LOGI(TAG, "USB sender mode disabled, initializing USB host for DAC detection");
                
                // Initialize USB host
                esp_err_t usb_err = usb_in_start();
                if (usb_err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to initialize USB host: %s", esp_err_to_name(usb_err));
                    set_state(LIFECYCLE_STATE_ERROR);
                    break;
                }
                
                // Give USB system enough time to detect and enumerate devices
                ESP_LOGI(TAG, "Waiting for USB device detection...");
                bool dac_detected = false;
                
                for (int i = 0; i < 10; i++) {
                    vTaskDelay(pdMS_TO_TICKS(200)); // Total 2 seconds waiting time
                    
                    // Check if device was detected
                    if (s_spk_dev_handle != NULL) {
                        ESP_LOGI(TAG, "DAC detected during enumeration");
                        dac_detected = true;
                        break;
                    }
                    
                    ESP_LOGI(TAG, "Waiting for DAC... %d/10", i+1);
                }
                
                // Check if DAC is connected
                if (!dac_detected) {
                    ESP_LOGI(TAG, "No DAC detected after waiting");
                    
                    // Only deep sleep if no DAC AND WiFi is configured
                    if (wifi_manager_has_credentials()) {
                        ESP_LOGI(TAG, "No DAC detected and WiFi is configured, going to deep sleep");
                        // Stop USB host before sleeping
                        usb_in_stop();
                        // Configure timer wakeup
                        esp_sleep_enable_timer_wakeup(DAC_CHECK_SLEEP_TIME_MS * 1000);
                        // Go to deep sleep
                        esp_deep_sleep_start();
                        // Never reached
                    } else {
                        ESP_LOGI(TAG, "No DAC detected but no WiFi configured, continuing with WiFi setup");
                        // Continue to services for WiFi configuration
                    }
                } else {
                    ESP_LOGI(TAG, "DAC detected, proceeding with full initialization");
                }
            } else {
                ESP_LOGI(TAG, "USB sender mode enabled, skipping DAC detection");
            }
            #else
            ESP_LOGI(TAG, "Not a USB build, proceeding to start services");
            #endif

            // Proceed to starting services
            set_state(LIFECYCLE_STATE_STARTING_SERVICES);
            break;
        }
        case LIFECYCLE_STATE_STARTING_SERVICES: {
            #ifdef IS_SPDIF
            app_config_t *config = config_manager_get_config();
            if (config->enable_spdif_sender) {
                ESP_LOGI(TAG, "Initializing S/PDIF receiver with pin %d", config->spdif_data_pin);
                spdif_receiver_init(6, NULL); // Use hardcoded pin from original implementation
            }
            #endif
            
            // Initialize WiFi manager
            ESP_LOGI(TAG, "Initializing WiFi manager...");
            ESP_ERROR_CHECK(wifi_manager_init());
            
            // Initialize roaming functionality (important for proper operation)
            ESP_LOGI(TAG, "Initializing WiFi roaming...");
            ESP_ERROR_CHECK(wifi_manager_init_roaming());
            
            // Try to connect to the strongest network first
            ESP_LOGI(TAG, "Attempting to connect to strongest WiFi network...");
            esp_err_t ret = wifi_manager_connect_to_strongest();
            
            // If connecting to strongest network fails, fall back to normal behavior:
            // 1. Connect using stored credentials if available
            // 2. Start AP mode with captive portal if no credentials are stored
            if (ret != ESP_OK) {
                ESP_LOGI(TAG, "Could not connect to strongest network, falling back to stored credentials or AP mode");
                ESP_ERROR_CHECK(wifi_manager_start());
            }
            
            // Start the web server (works in both AP mode and STA mode)
            ESP_LOGI(TAG, "Starting web server for configuration...");
            web_server_start();
            
            set_state(LIFECYCLE_STATE_AWAITING_MODE_CONFIG);
            break;
        }
        case LIFECYCLE_STATE_AWAITING_MODE_CONFIG: {
            // Check current WiFi state
            wifi_manager_state_t wifi_state = wifi_manager_get_state();
            ESP_LOGI(TAG, "Entering AWAITING_MODE_CONFIG, WiFi state: %d", wifi_state);
            
            if (wifi_state == WIFI_MANAGER_STATE_CONNECTED) {
                ESP_LOGI(TAG, "WiFi is connected, evaluating mode configuration");
                evaluate_and_transition();
            } else if (wifi_state == WIFI_MANAGER_STATE_AP_MODE) {
                ESP_LOGI(TAG, "WiFi is in AP mode, waiting for configuration via web portal");
                // Stay in this state until WiFi is configured
            } else {
                ESP_LOGI(TAG, "WiFi state: %d, waiting for connection or configuration", wifi_state);
            }
            break;
        }
        case LIFECYCLE_STATE_MODE_SENDER_USB:
            start_mode_sender_usb();
            break;
        case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
            start_mode_sender_spdif();
            break;
        case LIFECYCLE_STATE_MODE_RECEIVER_USB:
            start_mode_receiver_usb();
            break;
        case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
            start_mode_receiver_spdif();
            break;
        case LIFECYCLE_STATE_SLEEPING:
            enter_silence_sleep_mode();
            break;
        default:
            // No entry action for other states yet
            break;
    }
}

static void handle_state_exit(lifecycle_state_t state) {
    ESP_LOGI(TAG, "LIFECYCLE: Exiting state %d", state);
    switch (state) {
        case LIFECYCLE_STATE_MODE_SENDER_USB:
            stop_mode_sender_usb();
            break;
        case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
            stop_mode_sender_spdif();
            break;
        case LIFECYCLE_STATE_MODE_RECEIVER_USB:
            stop_mode_receiver_usb();
            break;
        case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
            stop_mode_receiver_spdif();
            break;
        case LIFECYCLE_STATE_SLEEPING:
            exit_silence_sleep_mode();
            break;
        default:
            // No exit action for other states yet
            break;
    }
}

static void set_state(lifecycle_state_t new_state) {
    if (s_current_state == new_state) {
        return;
    }
    handle_state_exit(s_current_state);
    ESP_LOGI(TAG, "LIFECYCLE: Transitioning from state %d to %d", s_current_state, new_state);
    s_current_state = new_state;
    handle_state_entry(s_current_state);
}

static void lifecycle_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "Lifecycle manager task started.");
    
    // Initial state transition
    set_state(LIFECYCLE_STATE_HW_INIT);

    while (1) {
        lifecycle_event_t event;
        if (xQueueReceive(s_lifecycle_event_queue, &event, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "LIFECYCLE: Received event %d in state %d", event, s_current_state);
            switch (s_current_state) {
                case LIFECYCLE_STATE_INITIALIZING:
                    handle_state_initializing(event);
                    break;
                case LIFECYCLE_STATE_HW_INIT:
                    handle_state_hw_init(event);
                    break;
                case LIFECYCLE_STATE_STARTING_SERVICES:
                    handle_state_starting_services(event);
                    break;
                case LIFECYCLE_STATE_AWAITING_MODE_CONFIG:
                    handle_state_awaiting_mode_config(event);
                    break;
                case LIFECYCLE_STATE_MODE_SENDER_USB:
                    handle_state_mode_sender_usb(event);
                    break;
                case LIFECYCLE_STATE_MODE_SENDER_SPDIF:
                    handle_state_mode_sender_spdif(event);
                    break;
                case LIFECYCLE_STATE_MODE_RECEIVER_USB:
                    handle_state_mode_receiver_usb(event);
                    break;
                case LIFECYCLE_STATE_MODE_RECEIVER_SPDIF:
                    handle_state_mode_receiver_spdif(event);
                    break;
                case LIFECYCLE_STATE_SLEEPING:
                    handle_state_sleeping(event);
                    break;
                case LIFECYCLE_STATE_ERROR:
                    handle_state_error(event);
                    break;
            }
        }
    }
}

esp_err_t lifecycle_manager_init(void) {
    // Create the network activity event group if it doesn't exist
    if (s_network_activity_event_group == NULL) {
        s_network_activity_event_group = xEventGroupCreate();
        if (s_network_activity_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create network activity event group");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created network activity event group");
    }

    s_lifecycle_event_queue = xQueueCreate(LIFECYCLE_EVENT_QUEUE_SIZE, sizeof(lifecycle_event_t));
    if (s_lifecycle_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create lifecycle event queue");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(lifecycle_manager_task, "lifecycle_mgr", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create lifecycle manager task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t lifecycle_manager_post_event(lifecycle_event_t event) {
    if (s_lifecycle_event_queue == NULL) {
        ESP_LOGE(TAG, "Lifecycle event queue not initialized");
        return ESP_FAIL;
    }

    if (xQueueSend(s_lifecycle_event_queue, &event, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to post event to lifecycle queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void lifecycle_manager_report_network_activity(void) {
    if (monitoring_active) {
        packet_counter++;
        last_packet_time = xTaskGetTickCount();
        // Signal the network monitor task that a packet has been received
        if (s_network_activity_event_group) {
            xEventGroupSetBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
        }
    }
}

// --- State Handler Implementations ---

static void handle_state_initializing(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: INITIALIZING");
}

static void handle_state_hw_init(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: HW_INIT");
    // Only process on specific events, not all events
    if (event != LIFECYCLE_EVENT_WIFI_CONNECTED &&
        event != LIFECYCLE_EVENT_USB_DAC_CONNECTED &&
        event != LIFECYCLE_EVENT_USB_DAC_DISCONNECTED &&
        event != LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        // Ignore other events while in HW_INIT
        return;
    }
}

static void handle_state_starting_services(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: STARTING_SERVICES");
    // Entry actions are handled in handle_state_entry,
    // this function will handle events received *while* in this state.
}

static void evaluate_and_transition(void) {
    app_config_t *config = config_manager_get_config();
    
    // Log the current configuration
    ESP_LOGI(TAG, "========== CURRENT CONFIGURATION ==========");
    ESP_LOGI(TAG, "Build type: %s",
        #ifdef IS_USB
            "USB"
        #elif defined(IS_SPDIF)
            "S/PDIF"
        #else
            "UNKNOWN"
        #endif
    );
    ESP_LOGI(TAG, "USB sender enabled: %s", config->enable_usb_sender ? "YES" : "NO");
    ESP_LOGI(TAG, "S/PDIF sender enabled: %s", config->enable_spdif_sender ? "YES" : "NO");
    ESP_LOGI(TAG, "Sample rate: %d Hz", config->sample_rate);
    ESP_LOGI(TAG, "Bit depth: %d bits", config->bit_depth);
    ESP_LOGI(TAG, "Volume: %.2f", config->volume);
    ESP_LOGI(TAG, "Sender destination IP: %s", config->sender_destination_ip);
    ESP_LOGI(TAG, "Sender destination port: %d", config->sender_destination_port);
    ESP_LOGI(TAG, "S/PDIF data pin: %d", config->spdif_data_pin);
    ESP_LOGI(TAG, "==========================================");
    
    // Check WiFi state first
    wifi_manager_state_t wifi_state = wifi_manager_get_state();
    ESP_LOGI(TAG, "Current WiFi state: %d", wifi_state);
    
    // Only transition to operational modes if WiFi is connected or we're in sender mode
    if (wifi_state != WIFI_MANAGER_STATE_CONNECTED &&
        !config->enable_usb_sender && !config->enable_spdif_sender) {
        ESP_LOGI(TAG, "WiFi not connected and not in sender mode, staying in AWAITING_MODE_CONFIG");
        return;
    }

    // Determine which mode to enter based on configuration
    if (config->enable_usb_sender) {
        ESP_LOGI(TAG, "Transitioning to USB sender mode (enable_usb_sender=true)");
        set_state(LIFECYCLE_STATE_MODE_SENDER_USB);
    } else if (config->enable_spdif_sender) {
        ESP_LOGI(TAG, "Transitioning to S/PDIF sender mode (enable_spdif_sender=true)");
        set_state(LIFECYCLE_STATE_MODE_SENDER_SPDIF);
    } else {
        ESP_LOGI(TAG, "No sender mode enabled, selecting receiver mode based on build type");
        // Default to receiver mode based on build configuration
        #ifdef IS_USB
        ESP_LOGI(TAG, "Transitioning to USB receiver mode (IS_USB defined)");
        set_state(LIFECYCLE_STATE_MODE_RECEIVER_USB);
        #elif defined(IS_SPDIF)
        ESP_LOGI(TAG, "Transitioning to S/PDIF receiver mode (IS_SPDIF defined)");
        set_state(LIFECYCLE_STATE_MODE_RECEIVER_SPDIF);
        #else
        ESP_LOGW(TAG, "No valid mode configuration found (neither IS_USB nor IS_SPDIF defined)");
        #endif
    }
}

static void handle_state_awaiting_mode_config(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: AWAITING_MODE_CONFIG");
    switch (event) {
        case LIFECYCLE_EVENT_WIFI_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected, starting network services.");
            mdns_service_start();
            initialize_ntp_client();
            evaluate_and_transition();
            break;
        case LIFECYCLE_EVENT_WIFI_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi disconnected, stopping network services.");
            mdns_service_stop();
            // Note: No stop function for NTP client.
            break;
        case LIFECYCLE_EVENT_CONFIGURATION_CHANGED:
            ESP_LOGI(TAG, "Configuration changed, re-evaluating mode.");
            // Check if we now have WiFi credentials
            if (!wifi_manager_has_credentials()) {
                ESP_LOGI(TAG, "No WiFi credentials, staying in AP mode");
                // Stay in AP mode for configuration
            } else {
                ESP_LOGI(TAG, "WiFi credentials available, attempting connection");
                // Try to connect with new credentials
                wifi_manager_state_t state = wifi_manager_get_state();
                if (state == WIFI_MANAGER_STATE_AP_MODE || state == WIFI_MANAGER_STATE_CONNECTION_FAILED) {
                    // Restart WiFi with new credentials
                    wifi_manager_stop();
                    wifi_manager_start();
                }
            }
            evaluate_and_transition();
            break;
        default:
            break;
    }
}

static void handle_state_mode_sender_usb(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_SENDER_USB");
    if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        evaluate_and_transition();
    }
}

static void handle_state_mode_sender_spdif(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_SENDER_SPDIF");
    if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        evaluate_and_transition();
    }
}

static void handle_state_mode_receiver_usb(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_RECEIVER_USB");
    if (event == LIFECYCLE_EVENT_ENTER_SLEEP) {
        set_state(LIFECYCLE_STATE_SLEEPING);
    } else if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        evaluate_and_transition();
    }
}

static void handle_state_mode_receiver_spdif(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: MODE_RECEIVER_SPDIF");
    if (event == LIFECYCLE_EVENT_ENTER_SLEEP) {
        set_state(LIFECYCLE_STATE_SLEEPING);
    } else if (event == LIFECYCLE_EVENT_CONFIGURATION_CHANGED) {
        evaluate_and_transition();
    }
}

static void handle_state_sleeping(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: SLEEPING");
    if (event == LIFECYCLE_EVENT_WAKE_UP) {
        set_state(LIFECYCLE_STATE_AWAITING_MODE_CONFIG);
    }
}

static void handle_state_error(lifecycle_event_t event) {
    ESP_LOGI(TAG, "LIFECYCLE: Handling state: ERROR");
}

// --- Mode Start/Stop Implementations ---

static void start_mode_sender_usb(void) {
    ESP_LOGI(TAG, "Starting USB sender mode...");
    #ifdef IS_USB
    esp_err_t ret = scream_sender_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB sender: %s", esp_err_to_name(ret));
        return;
    }
    ret = scream_sender_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB sender: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "USB sender mode started successfully");
    #else
    ESP_LOGW(TAG, "USB support not enabled in build");
    #endif
}
static void stop_mode_sender_usb(void) {
    ESP_LOGI(TAG, "Stopping USB sender mode...");
    #ifdef IS_USB
    esp_err_t ret = scream_sender_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop USB sender: %s", esp_err_to_name(ret));
    }
    #endif
}
static void start_mode_sender_spdif(void) {
    ESP_LOGI(TAG, "Starting S/PDIF sender mode...");
    #ifdef IS_SPDIF
    app_config_t *config = config_manager_get_config();
    ESP_LOGI(TAG, "Initializing Scream sender");
    esp_err_t ret = scream_sender_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Scream sender: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Starting S/PDIF receiver");
    ret = spdif_receiver_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start S/PDIF receiver: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Starting Scream sender");
    ret = scream_sender_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Scream sender: %s", esp_err_to_name(ret));
        spdif_receiver_stop();
        return;
    }
    ESP_LOGI(TAG, "S/PDIF sender mode started successfully");
    #else
    ESP_LOGW(TAG, "S/PDIF support not enabled in build");
    #endif
}
static void stop_mode_sender_spdif(void) {
    ESP_LOGI(TAG, "Stopping S/PDIF sender mode...");
    #ifdef IS_SPDIF
    esp_err_t ret = scream_sender_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop Scream sender: %s", esp_err_to_name(ret));
    }
    ret = spdif_receiver_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop S/PDIF receiver: %s", esp_err_to_name(ret));
    }
    #endif
}
static void start_mode_receiver_usb(void) {
    ESP_LOGI(TAG, "Starting USB receiver mode...");
    #ifdef IS_USB
    // Setup audio output first
    setup_audio();
    
    // Setup buffer for network->USB streaming
    setup_buffer();
    
    // Setup network receiver
    setup_network();
    
    // Start USB host for DAC output
    esp_err_t ret = usb_in_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB host: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "USB receiver mode started successfully");
    #else
    ESP_LOGW(TAG, "USB support not enabled in build");
    #endif
}
static void stop_mode_receiver_usb(void) {
    ESP_LOGI(TAG, "Stopping USB receiver mode...");
    #ifdef IS_USB
    esp_err_t ret = usb_in_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop USB host: %s", esp_err_to_name(ret));
    }
    // TODO: Stop network and audio tasks properly
    // For now, these don't have clean stop functions
    #endif
}
static void start_mode_receiver_spdif(void) {
    ESP_LOGI(TAG, "Starting S/PDIF receiver mode...");
    #ifdef IS_SPDIF
    app_config_t *config = config_manager_get_config();
    
    // Initialize S/PDIF output
    esp_err_t ret = spdif_init(config->sample_rate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize S/PDIF output: %s", esp_err_to_name(ret));
        return;
    }
    
    // Setup buffer for network->S/PDIF streaming
    setup_buffer();
    
    // Setup network receiver
    setup_network();
    
    ESP_LOGI(TAG, "S/PDIF receiver mode started successfully");
    #else
    ESP_LOGW(TAG, "S/PDIF support not enabled in build");
    #endif
}
static void stop_mode_receiver_spdif(void) {
    ESP_LOGI(TAG, "Stopping S/PDIF receiver mode...");
    #ifdef IS_SPDIF
    // TODO: Stop network, buffer, and S/PDIF output properly
    // For now, these don't have clean stop functions
    #endif
}

static void network_monitor_task(void *params) {
    ESP_LOGI(TAG, "Network monitor task started");

    // Initialize the last packet time
    last_packet_time = xTaskGetTickCount();

    while (true) {
        if (monitoring_active) {
            // Wait for a packet notification OR timeout
            EventBits_t bits = xEventGroupWaitBits(
                s_network_activity_event_group,   // The event group being tested.
                NETWORK_PACKET_RECEIVED_BIT,      // The bits within the event group to wait for.
                pdTRUE,                           // NETWORK_PACKET_RECEIVED_BIT should be cleared before returning.
                pdFALSE,                          // Don't wait for all bits, any bit will do (we only have one).
                pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS) // Wait time in ticks.
            );

            // Check if still monitoring after the wait (could have been disabled by exit_silence_sleep_mode)
            if (!monitoring_active) {
                continue; // Exit loop iteration if monitoring was stopped during wait
            }

            TickType_t current_time = xTaskGetTickCount();
            TickType_t time_since_last_packet = (current_time - last_packet_time) * portTICK_PERIOD_MS;

            // Did we receive a packet notification?
            if (bits & NETWORK_PACKET_RECEIVED_BIT) {
                ESP_LOGD(TAG, "Monitor: Packet received event bit set.");
                // packet_counter and last_packet_time are updated in network.c when the bit is set
                // Check if the activity threshold is met
                if (packet_counter >= ACTIVITY_THRESHOLD_PACKETS) {
                    ESP_LOGI(TAG, "Network activity threshold met (%" PRIu32 " packets >= %d), exiting sleep mode",
                            packet_counter, ACTIVITY_THRESHOLD_PACKETS);
                    lifecycle_manager_post_event(LIFECYCLE_EVENT_WAKE_UP);
                } else {
                    ESP_LOGD(TAG, "Monitor: Packet count %" PRIu32 " < threshold %d", packet_counter, ACTIVITY_THRESHOLD_PACKETS);
                }
            } else {
                // No packet notification bit set, timeout occurred. Check for inactivity timeout.
                ESP_LOGD(TAG, "Monitor: Wait timeout. Packets=%" PRIu32 ", time_since_last=%lu ms", packet_counter, (unsigned long)time_since_last_packet);
                if (time_since_last_packet >= NETWORK_INACTIVITY_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "Network inactivity timeout reached (%lu ms >= %d ms), maintaining sleep mode",
                            (unsigned long)time_since_last_packet, NETWORK_INACTIVITY_TIMEOUT_MS);
                    // Update timestamp to prevent continuous logging of the same timeout event
                    // Note: last_packet_time is only updated here on timeout, or in network.c on packet arrival.
                    last_packet_time = current_time;
                }
            }
            // The loop continues, waiting again with xEventGroupWaitBits which includes the delay

        } else {
            // When not actively monitoring, suspend the task to save CPU
            ESP_LOGD(TAG, "Monitoring inactive, suspending monitor task.");
            // Clear any pending event bits before suspending
            if (s_network_activity_event_group) {
                xEventGroupClearBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
            }
            vTaskSuspend(NULL);
            // --- Task resumes here when vTaskResume is called (in enter_silence_sleep_mode) ---
            ESP_LOGD(TAG, "Monitor task resumed.");
            // Reset state when resuming
            last_packet_time = xTaskGetTickCount();
            packet_counter = 0; // Reset packet counter when monitoring starts/resumes
            // Clear event bits again on resume just in case
            if (s_network_activity_event_group) {
                xEventGroupClearBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
            }
        }
    }
}

void enter_silence_sleep_mode(void) {
    app_config_t *config = config_manager_get_config();
    if (config->enable_spdif_sender || config->enable_usb_sender) {
        ESP_LOGI(TAG, "Sender mode enabled, silence sleep is disabled");
        return;
    }
    
    ESP_LOGI(TAG, "Entering silence sleep mode");
    
    // Make sure event group exists
    if (s_network_activity_event_group == NULL) {
        ESP_LOGE(TAG, "Network activity event group not initialized, cannot enter sleep mode");
        return;
    }
    
    // Configure WiFi for max power saving
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi power save mode: %s", esp_err_to_name(ret));
    }

    // Suppress WiFi warnings
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    // Create network monitoring task if it doesn't exist yet
    if (network_monitor_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            network_monitor_task,
            "network_monitor",
            4096,
            NULL,
            1,  // Low priority
            &network_monitor_task_handle,
            0   // Core 0
        );
        if (task_ret != pdTRUE) {
            ESP_LOGE(TAG, "Failed to create network monitor task");
            return;
        }
    }
    
    // Start network monitoring
    monitoring_active = true;
    packet_counter = 0;
    if (eTaskGetState(network_monitor_task_handle) == eSuspended) {
        vTaskResume(network_monitor_task_handle);
    }
    
    ESP_LOGI(TAG, "Entered light sleep mode with network monitoring");
}

void exit_silence_sleep_mode(void) {
    ESP_LOGI(TAG, "Exiting silence sleep mode");
    
    // Stop the network monitoring
    monitoring_active = false;
    
    // Set WiFi back to normal power saving mode
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi normal mode: %s", esp_err_to_name(ret));
    }

    // Reset silence tracking variables to prevent immediate re-entry into sleep mode
    is_silent = false;
    silence_duration_ms = 0;
    last_audio_time = xTaskGetTickCount(); // Reset to current time
    
    ESP_LOGI(TAG, "Resumed normal operation");
}
