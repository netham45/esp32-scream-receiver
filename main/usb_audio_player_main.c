#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "string.h"
#include "audio.h"
#include "buffer.h"
#include "network.h"
#include "config.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "global.h"
#include "esp_pm.h" // For power management
#include "driver/i2c.h" // For I2C communication with USB-C power chip
#ifdef IS_USB
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#endif
// Include our new modules
#include "config_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "mdns_service.h"
#include "ntp_client.h"
#ifdef IS_USB
#include "scream_sender.h"
#endif
#include "bq25895_integration.h" // Include BQ25895 integration header

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define BIT1_SPK_START          (0x01 << 0)
#define DEFAULT_VOLUME          45
#define RTC_CNTL_OPTION1_REG 0x6000812C
#define RTC_CNTL_FORCE_DOWNLOAD_BOOT 1
// Using DAC_CHECK_SLEEP_TIME_MS from config.h (2000ms)
#define NETWORK_SLEEP_TIME_MS 10       // Light sleep between network operations

// I2C configuration for USB-C PMID power management
#define I2C_MASTER_SCL_IO           9       // GPIO for I2C SCL
#define I2C_MASTER_SDA_IO           8       // GPIO for I2C SDA
#define I2C_MASTER_NUM              0       // I2C port number
#define I2C_MASTER_FREQ_HZ          100000  // I2C master clock frequency (100kHz)
#define I2C_MASTER_TIMEOUT_MS       1000    // I2C timeout in milliseconds

// OTG control pin - must be high to activate boost mode
#define OTG_PIN                     13      // GPIO for OTG control

// USB-C Power Chip registers
#define POWER_CHIP_ADDR             0x6B    // I2C address of the USB-C power chip
#define REG_CONTROL2                0x02    // Control register 2 (boost frequency)
#define REG_CONTROL3                0x03    // Control register 3 (OTG config)
#define REG_BOOST_VOLTAGE           0x0A    // Boost voltage register
#define REG_STATUS                  0x0B    // Status register
bool device_sleeping = false;

// I2C initialization and communication functions
static esp_err_t i2c_master_init(void);
static esp_err_t i2c_write_reg(uint8_t reg_addr, uint8_t data);
static esp_err_t i2c_read_reg(uint8_t reg_addr, uint8_t *data);
static esp_err_t usb_c_pmid_init(void);
typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

static EventGroupHandle_t s_wifi_event_group;
EventGroupHandle_t s_network_activity_event_group = NULL; // Event group for network activity signal

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1



static int s_retry_num = 0;

// Forward declarations
void wifi_init_sta(void);
void enter_silence_sleep_mode(void);
void exit_silence_sleep_mode(void);
bool check_network_activity(void);

#ifdef IS_USB
uac_host_device_handle_t s_spk_dev_handle = NULL;
static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg);
static QueueHandle_t s_event_queue = NULL;
bool usb_host_running = true;

// Store device parameters for fast reconnection after sleep
typedef struct {
    uint8_t addr;
    uint8_t iface_num;
    uac_host_stream_config_t stream_config;
    bool valid;
} saved_usb_device_t;

static saved_usb_device_t saved_usb_device = {
    .valid = false
};

/**
 * @brief event queue
 *
 * This event is used for delivering the UAC Host event from callback to the uac_lib_task
 */
typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_driver_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;

/*static void uf2_update_complete_cb()
{
    TaskHandle_t main_task_hdl = xTaskGetHandle("main");
    xTaskNotifyGive(main_task_hdl);
}*/

// Set up deep sleep parameters and timer to wake periodically to check for DAC
void enter_deep_sleep_mode() {
    if (device_sleeping) {
        return; // Already in sleep mode
    }
    
    ESP_LOGI(TAG, "Entering deep sleep mode");
    device_sleeping = true;
    
    // Stop network activity and disconnect WiFi to save power
    esp_wifi_disconnect();
    esp_wifi_stop();
    
    // Configure ESP32 to wake up every few seconds to check for DAC
    esp_sleep_enable_timer_wakeup(DAC_CHECK_SLEEP_TIME_MS * 1000); // Convert to microseconds
    
    // Actually enter deep sleep (this function doesn't return)
    ESP_LOGI(TAG, "Going to deep sleep now");
    esp_deep_sleep_start();
    
    // This code is never reached
}

// Forward declarations from audio.c for the silence tracking variables
extern bool is_silent;
extern uint32_t silence_duration_ms;
extern TickType_t last_audio_time;

// Function to exit sleep mode when DAC is connected - called after waking from deep sleep
void exit_sleep_mode() {
    ESP_LOGI(TAG, "Exiting sleep mode after deep sleep wake");
    device_sleeping = false;
    
    // Only try to disable wakeup sources if we're actually coming from deep sleep
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    if (wakeup_cause == ESP_SLEEP_WAKEUP_TIMER) {
        // Only disable timer wakeup if we're waking from timer
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    
    // Reset audio silence tracking variables to prevent going back to sleep immediately
    is_silent = false;
    silence_duration_ms = 0;
    last_audio_time = xTaskGetTickCount();
    
    // Restart WiFi - we disconnected before sleeping
    wifi_init_sta();
}

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    // Check for disconnect event - doesn't matter if we use UAC_HOST_DEVICE_EVENT_DISCONNECTED 
    // or UAC_HOST_DRIVER_EVENT_DISCONNECTED since they should be the same value
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        // stop audio player first
        s_spk_dev_handle = NULL;
        //audio_player_stop();
		stop_playback();
        ESP_LOGI(TAG, "UAC Device disconnected");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        vTaskDelay(2000); // Give two seconds for reconnect
        if (!is_playing())
            // Enter deep sleep mode when DAC disconnects
            enter_deep_sleep_mode();
        return;
    }
    
    // Device connections are handled in the driver callback, not here
    // The device callback can exit sleep mode on device activity
    if (device_sleeping && s_spk_dev_handle != NULL) {
        exit_sleep_mode();
    }
    // Send uac device event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    // should not block here
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    // Send uac driver event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    while (usb_host_running) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // In this example, there is only one client registered
        // So, once we deregister the client, this call must succeed with ESP_OK
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void uac_lib_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
    s_event_queue_t evt_queue = {0};
    while (usb_host_running) {
        if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
            if (UAC_DRIVER_EVENT ==  evt_queue.event_group) {
                uac_host_driver_event_t event = evt_queue.driver_evt.event;
                uint8_t addr = evt_queue.driver_evt.addr;
                uint8_t iface_num = evt_queue.driver_evt.iface_num;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_TX_CONNECTED: {
                    uac_host_dev_info_t dev_info;
                    uac_host_device_handle_t uac_device_handle = NULL;
                    const uac_host_device_config_t dev_config = {
                        .addr = addr,
                        .iface_num = iface_num,
                        .buffer_size = PCM_CHUNK_SIZE * 4,
                        .buffer_threshold = 0,
                        .callback = uac_device_callback,
                        .callback_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));
                    ESP_ERROR_CHECK(uac_host_get_device_info(uac_device_handle, &dev_info));
                    ESP_LOGI(TAG, "UAC Device connected: SPK");
                    uac_host_printf_device_param(uac_device_handle);
                    //ESP_ERROR_CHECK(uac_host_device_start(uac_device_handle, &stm_config));
                    s_spk_dev_handle = uac_device_handle;
                    start_playback(s_spk_dev_handle);
					//audio_player_play(s_fp);
                   
                    break;
                }
                case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                    // we don't support MIC in this example
                    ESP_LOGI(TAG, "UAC Device connected: MIC");
                    break;
                }
                default:
                    break;
                }
            } else if (UAC_DEVICE_EVENT == evt_queue.event_group) {
                uac_host_device_event_t event = evt_queue.device_evt.event;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "UAC Device disconnected");
                    break;
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                    break;
                default:
                    break;
                }
            } else if (APP_EVENT == evt_queue.event_group) {
                break;
            }
        }
    }

    ESP_LOGI(TAG, "UAC Driver uninstall");
    ESP_ERROR_CHECK(uac_host_uninstall());
	vTaskDelete(NULL);
}

#endif


// WiFi event handling is now managed by wifi_manager.c

// Task for monitoring network activity during silence sleep mode
TaskHandle_t network_monitor_task_handle = NULL;
volatile uint32_t packet_counter = 0;
volatile bool monitoring_active = false;
volatile TickType_t last_packet_time = 0;

// Network packet monitoring task - uses Event Group for packet notification
void network_monitor_task(void *params) {
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
                    exit_silence_sleep_mode(); // This sets monitoring_active = false
                    // Loop will continue, check monitoring_active, and suspend
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

// Function to check for network activity (by sampling recent packets)
bool check_network_activity() {
    // Simple check - just see if we've received packets since last check
    if (packet_counter >= ACTIVITY_THRESHOLD_PACKETS) {
        ESP_LOGI(TAG, "Network activity detected (%" PRIu32 " packets)", packet_counter);
        return true;
    }
    return false;
}

// Silence sleep mode - detach USB device but keep WiFi running in light sleep
void enter_silence_sleep_mode() {
    if (device_sleeping) {
        return; // Already in sleep mode
    }
    
    ESP_LOGI(TAG, "Entering silence sleep mode");
    device_sleeping = true;
    
#ifdef IS_USB
    if (s_spk_dev_handle != NULL) {
        // Save device parameters for reconnection
        uac_host_dev_info_t dev_info;
        ESP_ERROR_CHECK(uac_host_get_device_info(s_spk_dev_handle, &dev_info));
        
        // No direct access to addr/iface_num here, use a different approach
        // We don't actually need to save these parameters since we already have the device handle
        // Just set some reasonable defaults
        saved_usb_device.addr = 0;
        saved_usb_device.iface_num = 0;
        saved_usb_device.stream_config.channels = 2;
        saved_usb_device.stream_config.bit_resolution = 16;
        saved_usb_device.stream_config.sample_freq = 48000;
        saved_usb_device.valid = true;
        
        // Stop audio playback and detach USB device
        stop_playback();
        ESP_LOGI(TAG, "Detaching USB DAC device");
        
        // We don't actually uninstall the USB host, just stop the device
        // This allows for faster reconnection later
        uac_host_device_stop(s_spk_dev_handle);
    }
#endif
    
    // Configure WiFi for max power saving
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));

    // Suppress WiFi warnings (including "exceed max band" messages)
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    // Create network monitoring task if it doesn't exist yet
    if (network_monitor_task_handle == NULL) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            network_monitor_task,
            "network_monitor",
            4096,
            NULL,
            1,  // Low priority
            &network_monitor_task_handle,
            0   // Core 0
        );
        assert(ret == pdTRUE);
    }
    
    // Start network monitoring
    monitoring_active = true;
    packet_counter = 0;
    if (eTaskGetState(network_monitor_task_handle) == eSuspended) {
        vTaskResume(network_monitor_task_handle);
    }
    
    // Instead of using deep sleep, use light sleep to maintain WiFi connection
    ESP_LOGI(TAG, "Entering light sleep mode with network monitoring");
    
    // Enable automatic light sleep - CPU will sleep between task operations
    #if CONFIG_PM_ENABLE
    // Light sleep is handled by the power management module
    ESP_LOGI(TAG, "Light sleep enabled through power management");
    #else
    // No power management, use manual light sleep in the monitoring task
    ESP_LOGW(TAG, "Power management not enabled, using manual light sleep in monitor task");
    #endif
}

// External declaration for audio silence tracking variables that need to be reset
extern bool is_silent;
extern uint32_t silence_duration_ms;
extern TickType_t last_audio_time;

// Exit silence sleep mode and reconnect USB device
void exit_silence_sleep_mode() {
    
    ESP_LOGI(TAG, "Exiting silence sleep mode");
    device_sleeping = false;
    
    // Stop the network monitoring
    monitoring_active = false;
    
    // Set WiFi back to normal power saving mode
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    // Suppress WiFi warnings (including "exceed max band" messages)
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    
    // Reset silence tracking variables to prevent immediate re-entry into sleep mode
    is_silent = false;
    silence_duration_ms = 0;
    last_audio_time = xTaskGetTickCount(); // Reset to current time
    
#ifdef IS_USB
    // Check if we have saved device parameters
    if (saved_usb_device.valid && s_spk_dev_handle != NULL) {
        ESP_LOGI(TAG, "Reconnecting USB DAC device");
        
        // Restart the device with saved parameters
        ESP_ERROR_CHECK(uac_host_device_start(s_spk_dev_handle, &saved_usb_device.stream_config));
        ESP_ERROR_CHECK(uac_host_device_set_volume(s_spk_dev_handle, VOLUME)); // Use the volume variable from audio.c
        
        resume_playback();
    }
#endif
    
    ESP_LOGI(TAG, "Resumed normal operation");
}

// Check if we need to go to deep sleep - returns true if we should continue running
bool check_dac_or_sleep() {
    // Check if we have a DAC connected
#ifdef IS_USB
    if (s_spk_dev_handle == NULL) {
        // No DAC connected, prepare for deep sleep
        ESP_LOGI(TAG, "No DAC detected on wake, going back to deep sleep");
        
        // Give a small delay for any pending logs to flush
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Go to deep sleep again
        enter_deep_sleep_mode();
        
        // This won't be reached due to deep sleep
        return false;
    }
#endif
    
    // DAC is connected or we're not using USB, so continue normal operation
    return true;
}

// Function to check if a GPIO pin is pressed (connected to ground)
bool is_gpio_pressed(gpio_num_t pin) {
    return gpio_get_level(pin) == 0; // Returns true if pin is low (pressed)
}

// I2C master initialization
static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C parameter configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    
    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver installation failed: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "I2C master initialized successfully");
    return ESP_OK;
}

// Write to I2C register
static esp_err_t i2c_write_reg(uint8_t reg_addr, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (POWER_CHIP_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

// Read from I2C register
static esp_err_t i2c_read_reg(uint8_t reg_addr, uint8_t *data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (POWER_CHIP_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (POWER_CHIP_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

// Configure OTG pin as output and pull high
static esp_err_t otg_pin_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OTG_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTG pin configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Set OTG pin high to enable boost mode
    err = gpio_set_level(OTG_PIN, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set OTG pin high: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "OTG pin (GPIO %d) set high to enable boost mode", OTG_PIN);
    return ESP_OK;
}

// Initialize USB-C PMID output for 5V
static esp_err_t usb_c_pmid_init(void)
{
    esp_err_t err;
    uint8_t status;
    
    // Initialize OTG control pin
    err = otg_pin_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize OTG pin");
        return err;
    }
    
    // Initialize I2C master
    err = i2c_master_init();
    if (err != ESP_OK) {
        return err;
    }
    
    // 1. Enable OTG (Boost) Mode: REG03 = 0x3A
    err = i2c_write_reg(REG_CONTROL3, 0x3A);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable OTG mode");
        return err;
    }
    ESP_LOGI(TAG, "OTG (Boost) mode enabled");
    
    // 2. Configure Boost Voltage (5.126V): REG0A = 0x93
    err = i2c_write_reg(REG_BOOST_VOLTAGE, 0x93);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boost voltage");
        return err;
    }
    ESP_LOGI(TAG, "Boost voltage set to 5.126V");
    
    // 3. Set Boost Frequency: REG02 = 0x38
    err = i2c_write_reg(REG_CONTROL2, 0x38);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boost frequency");
        return err;
    }
    ESP_LOGI(TAG, "Boost frequency set to 500kHz");
    
    // 4. Read status register to verify boost mode
    err = i2c_read_reg(REG_STATUS, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read status register");
        return err;
    }
    
    // Check VBUS_STAT[2:0] bits (bits 5:3) - should be 111 in boost mode
    if (((status >> 3) & 0x07) == 0x07) {
        ESP_LOGI(TAG, "USB-C PMID output enabled successfully (status: 0x%02x)", status);
    } else {
        ESP_LOGW(TAG, "USB-C PMID output may not be in boost mode (status: 0x%02x)", status);
    }
    
    return ESP_OK;
}

void app_main(void)
{
    // Initialize NVS (required for USB subsystem)
    BaseType_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize BQ25895 Battery Charger
    ESP_LOGI(TAG, "Initializing BQ25895 battery charger");
    esp_err_t bq_err = bq25895_integration_init();
    if (bq_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BQ25895: %s", esp_err_to_name(bq_err));
        // Decide if this is a critical failure or if the app can continue
    } else {
        ESP_LOGI(TAG, "BQ25895 initialized successfully");
    }
    
    // USB-C PMID output is now handled by the BQ25895 driver

    // Create the event group for network activity signaling
    s_network_activity_event_group = xEventGroupCreate();
    if (s_network_activity_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create network activity event group");
        // Handle error appropriately, maybe restart
        esp_restart();
    }
    
    // Get the wake cause (why the device booted)
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    
    // Only check GPIO pins for reset if this was a power-on or hard reset, not waking from sleep
    if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // This is a power-on or hard reset, not a wake from sleep
        
        // Configure GPIO pins 0 and 1 as inputs with pull-up resistors
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_1),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        
        // Wait 3 seconds and check if pins 0 or 1 are pressed
        ESP_LOGI(TAG, "Starting 3-second WiFi reset window. Press GPIO 0 or 1 to reset WiFi config...");
        for (int i = 0; i < 30; i++) { // 30 * 100ms = 3 seconds
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // Check if either pin is pressed
            if (is_gpio_pressed(GPIO_NUM_0) || is_gpio_pressed(GPIO_NUM_1)) {
                ESP_LOGI(TAG, "GPIO pin pressed! Wiping WiFi configuration...");
                
                // Initialize WiFi manager if it's not already initialized
                wifi_manager_init();
                
                // Clear WiFi credentials from NVS
                wifi_manager_clear_credentials();
                
                // Clear WiFi credentials from ESP's internal WiFi storage
                esp_wifi_restore();
                
                // Reset all settings to defaults
                config_manager_reset();
                
                // To be extra safe, erase the entire NVS (all namespaces)
                nvs_flash_erase();
                // Reset all settings to defaults
                config_manager_reset();
                
                // To be extra safe, erase the entire NVS (all namespaces)
                nvs_flash_erase();
                
                ESP_LOGI(TAG, "All settings reset to defaults. Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second for logs to be printed
                esp_restart(); // Restart the ESP32
            }
        }
        ESP_LOGI(TAG, "WiFi reset window closed. Continuing with normal startup...");
    } else {
        // This is a wake from sleep, skip the GPIO reset check
        ESP_LOGI(TAG, "Waking from sleep (cause: %d), skipping WiFi reset window", wakeup_cause);
    }
    

    // Initialize configuration manager
    ESP_LOGI(TAG, "Initializing configuration manager");
    ESP_ERROR_CHECK(config_manager_init());
    app_config_t *current_config = config_manager_get_config();

#ifdef IS_USB
    // Only initialize USB host for DAC detection if sender mode is NOT enabled
    if (!current_config->enable_usb_sender) {
        ESP_LOGI(TAG, "USB sender mode disabled, initializing USB host for DAC detection");
        s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
        assert(s_event_queue != NULL);
        static TaskHandle_t uac_task_handle = NULL;
        ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_events", 4096, NULL,
                                    USER_TASK_PRIORITY, &uac_task_handle, 0);
        assert(ret == pdTRUE);
        ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                                    USB_HOST_TASK_PRIORITY, NULL, 0);
        assert(ret == pdTRUE);
    
        // Give USB system enough time to detect and enumerate devices
        ESP_LOGI(TAG, "Waiting for USB device detection...");
        // USB enumeration can take time, need to wait for it to complete
        for (int i = 0; i < 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(200)); // Total 2 seconds waiting time, checking every 200ms
            
            // Check if device was detected during the delay
            if (s_spk_dev_handle != NULL) {
                ESP_LOGI(TAG, "DAC detected during enumeration");
                break;
            }
            
            ESP_LOGI(TAG, "Waiting for DAC... %" PRIu16 "/10", i+1);
        }
        
        // Check if DAC is connected
        if (s_spk_dev_handle == NULL) {
            ESP_LOGI(TAG, "No DAC detected after waiting");
            
            // Only deep sleep if no DAC AND WiFi is configured
            if (wifi_manager_has_credentials()) {
                ESP_LOGI(TAG, "No DAC detected and WiFi is configured, going to deep sleep");
                // Configure timer wakeup
                //esp_sleep_enable_timer_wakeup(DAC_CHECK_SLEEP_TIME_MS * 1000);
                // Go to deep sleep immediately
                //esp_deep_sleep_start();
                //return; // Never reached
            } else {
                ESP_LOGI(TAG, "No DAC detected but no WiFi configured, continuing with WiFi setup");
            }
        }
        // Only initialize the rest of the system if DAC is connected
        ESP_LOGI(TAG, "DAC detected, initializing full system with power optimizations");
    }
#endif
    
    // Set CPU to lower frequency to save power
    #if CONFIG_PM_ENABLE
    // Configure dynamic frequency scaling based on chip type
    ESP_LOGI(TAG, "Configuring power management (reduced CPU clock)");
    
    #if CONFIG_IDF_TARGET_ESP32
    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = 80,  // Reduced from 240MHz
        .min_freq_mhz = 40,
    #if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
    #else
        .light_sleep_enable = false
    #endif
    };
    #elif CONFIG_IDF_TARGET_ESP32S3
    esp_pm_config_esp32s3_t pm_config = {
        .max_freq_mhz = 80,  // Reduced from 240MHz
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
    
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA with power saving");
    wifi_init_sta();
    
    // Enable WiFi power save mode
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    // Suppress WiFi warnings (including "exceed max band" messages)
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    
    setup_buffer();
    setup_audio();
    setup_network();
    initialize_ntp_client();
    ESP_LOGI(TAG, "Starting mDNS service for Scream discovery");
    mdns_service_start();
    
#ifdef IS_USB
    // Initialize the USB Scream Sender if USB mode is enabled
    app_config_t *config = config_manager_get_config();
    if (config->enable_usb_sender) {
        ESP_LOGI(TAG, "Initializing USB Scream Sender");
        ESP_ERROR_CHECK(scream_sender_init());
        ESP_ERROR_CHECK(scream_sender_start());
        ESP_LOGI(TAG, "USB Scream Sender started, sending to %s:%d", 
                 config->sender_destination_ip, config->sender_destination_port);
    }
#endif
    
    // Main loop - check for configuration changes that need to be applied
    while (1) {
#ifdef IS_USB
        static bool previous_sender_state = false;
        static char previous_dest_ip[16] = {0};
        static uint16_t previous_dest_port = 0;
        
        // Get current config
        app_config_t *current_config = config_manager_get_config();
        
        // Check if USB sender state has changed
        if (previous_sender_state != current_config->enable_usb_sender) {
            if (current_config->enable_usb_sender) {
                // Sender was turned on
                ESP_LOGI(TAG, "USB Scream Sender enabled, initializing");
                ESP_ERROR_CHECK(scream_sender_init());
                ESP_ERROR_CHECK(scream_sender_start());
                ESP_LOGI(TAG, "USB Scream Sender started, sending to %s:%d", 
                        current_config->sender_destination_ip, current_config->sender_destination_port);
            } else {
                // Sender was turned off
                ESP_LOGI(TAG, "USB Scream Sender disabled, stopping");
                if (scream_sender_is_running()) {
                    ESP_ERROR_CHECK(scream_sender_stop());
                }
            }
            previous_sender_state = current_config->enable_usb_sender;
        }
        
        // Check if destination has changed while sender is running
        if (current_config->enable_usb_sender && scream_sender_is_running() &&
            (strcmp(previous_dest_ip, current_config->sender_destination_ip) != 0 ||
             previous_dest_port != current_config->sender_destination_port)) {
             
            ESP_LOGI(TAG, "USB Scream Sender destination changed to %s:%d", 
                    current_config->sender_destination_ip, current_config->sender_destination_port);
            ESP_ERROR_CHECK(scream_sender_update_destination());
            
            // Update previous values
            strncpy(previous_dest_ip, current_config->sender_destination_ip, sizeof(previous_dest_ip) - 1);
            previous_dest_ip[sizeof(previous_dest_ip) - 1] = '\0'; // Ensure null termination
            previous_dest_port = current_config->sender_destination_port;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void wifi_init_sta(void)
{
    ESP_LOGI(TAG, "Starting WiFi with manager");
    
    s_wifi_event_group = xEventGroupCreate();
    
    // Initialize the WiFi manager
    ESP_ERROR_CHECK(wifi_manager_init());
    
    // Initialize roaming functionality
    ESP_ERROR_CHECK(wifi_manager_init_roaming());
    
    // Try to connect to the strongest network first
    esp_err_t ret = wifi_manager_connect_to_strongest();
    
    // If connecting to strongest network fails, fall back to normal behavior:
    // 1. Connect using stored credentials if available
    // 2. Start AP mode with captive portal if no credentials are stored
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Could not connect to strongest network, falling back to stored credentials");
        ESP_ERROR_CHECK(wifi_manager_start());
    }
    
    // Start the web server in both AP mode and STA (connected) mode
    ESP_LOGI(TAG, "Starting web server for configuration");
    web_server_start();
    
    ESP_LOGI(TAG, "WiFi initialization completed");
    
    // If we're connected, configure roaming
    if (wifi_manager_get_state() == WIFI_MANAGER_STATE_CONNECTED) {
        // Get and apply the RSSI threshold from config
        int8_t rssi_threshold;
        wifi_manager_get_rssi_threshold(&rssi_threshold);
        ESP_LOGI(TAG, "Setting RSSI threshold to %d", rssi_threshold);
        
        // Notify the network module that WiFi is connected
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Get the current SSID for logging
        char ssid[WIFI_SSID_MAX_LENGTH + 1];
        if (wifi_manager_get_current_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to AP: %s", ssid);
        }
        
        // Start mDNS service for Scream discovery
        ESP_LOGI(TAG, "Starting mDNS service for Scream discovery");
        mdns_service_start();
    } else {
        ESP_LOGI(TAG, "WiFi not connected, waiting for configuration via AP portal");
    }
}
