#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "string.h"
#include "receiver/audio_out.h"
#include "receiver/buffer.h"
#include "receiver/network_in.h"
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
#include "config/config_manager.h"
#include "wifi/wifi_manager.h"
#include "web/web_server.h"
#include "mdns/mdns_service.h"
#include "ntp/ntp_client.h"
#include "sender/network_out.h"
#include "bq25895/bq25895_integration.h" // Include BQ25895 integration header
#ifdef IS_SPDIF
#include "sender/spdif_in/spdif_in.h"
#endif
#include "lifecycle_manager.h"

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

EventGroupHandle_t s_network_activity_event_group = NULL; // Event group for network activity signal

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1



static int s_retry_num = 0;

// Forward declarations
void enter_silence_sleep_mode(void);
void exit_silence_sleep_mode(void);

#ifdef IS_USB
// All USB logic has been moved to main/receiver/usb_in.c
#endif


// WiFi event handling is now managed by wifi_manager.c



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

static esp_err_t usb_switch_init(void)
{
    return 0;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 16),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Sel pin configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    
    err = gpio_set_level(16, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set USB Sel pin high: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB Sel pin (GPIO %d) set high", 16);
    io_conf.pin_bit_mask = (1ULL << 17);
    
    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB OE pin configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    
    err = gpio_set_level(17, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set USB OE pin high: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB OE pin (GPIO %d) set high", 17);
    return ESP_OK;
}

// Initialize USB-C PMID output for 5V
static esp_err_t usb_c_pmid_init(void)
{
    esp_err_t err;
    uint8_t status;
    
    // Initialize OTG control pin
    err = usb_switch_init();
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

    // Start the lifecycle manager
    ESP_LOGI(TAG, "Starting lifecycle manager");
    ESP_ERROR_CHECK(lifecycle_manager_init());
    
    // The main loop is now handled by the lifecycle manager.
    // This task can now exit.
}


