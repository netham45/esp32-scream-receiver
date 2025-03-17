#include <config.h>
#ifdef IS_USB
#include "scream_sender.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_device_uac.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include <string.h>
#include <math.h>
#include "esp_rom_sys.h" // For ets_delay_us

#define TAG "scream_sender"

// Scream header for 16-bit 48KHz stereo audio
static const char header[] = {1, 16, 2, 0, 0};
#define HEADER_SIZE sizeof(header)
#define CHUNK_SIZE 1152
#define PACKET_SIZE (CHUNK_SIZE + HEADER_SIZE)

// Socket options
#define UDP_TX_BUFFER_SIZE (1024 * 32)
#define UDP_SEND_TIMEOUT_MS 10
#define MAX_SEND_RETRIES 3

// State variables
static bool s_is_sender_initialized = false;
static bool s_is_sender_running = false;
static bool s_is_muted = false;
static uint32_t s_volume = 100;
static int s_sock = -1;
static struct sockaddr_in s_dest_addr;

// Buffer for audio data
static char s_data_out[PACKET_SIZE];
static char s_data_in[CHUNK_SIZE * 16]; // Increased buffer size
static int s_data_in_head = 0;

// UAC callbacks
static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    if (s_is_muted || !s_is_sender_running)
        return ESP_OK;
    
    // Copy the received data to our buffer
    memcpy(s_data_in + s_data_in_head, buf, len);
    s_data_in_head += len;
    
    // Process the data in chunks
    while (s_data_in_head >= CHUNK_SIZE) {
        memcpy(s_data_out + HEADER_SIZE, s_data_in, CHUNK_SIZE);
        
        // Send with retry logic
        int sent = -1;
        int retry_count = 0;
        
        while (sent < 0 && retry_count < MAX_SEND_RETRIES) {
            sent = sendto(s_sock, s_data_out, PACKET_SIZE, 0, 
                         (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
                         
            if (sent < 0) {
                ESP_LOGW(TAG, "Failed to send UDP packet: errno %d, retry %d", 
                         errno, retry_count + 1);
                retry_count++;
                
                // Small delay before retry (500 microseconds)
                esp_rom_delay_us(500);
            } else if (sent != PACKET_SIZE) {
                ESP_LOGW(TAG, "Incomplete UDP packet sent: %d of %d bytes", 
                         sent, PACKET_SIZE);
            }
        }
        
        // Move remaining data to the beginning of the buffer
        s_data_in_head -= CHUNK_SIZE;
        if (s_data_in_head > 0) {
            memmove(s_data_in, s_data_in + CHUNK_SIZE, s_data_in_head);
        }
    }
    
    return ESP_OK;
}

static void uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    ESP_LOGI(TAG, "UAC mute change: %"PRIu32"", mute);
    s_is_muted = !!mute;
}

static void uac_device_set_volume_cb(uint32_t volume, void *arg)
{
    ESP_LOGI(TAG, "UAC volume change: %"PRIu32"", volume);
    
    // Convert Windows UAC volume scale (which is not linear) to a 0-100 scale
    if (volume <= 18) {
        s_volume = round(volume / 6.0);
    } else if (volume <= 26) {
        s_volume = 4 + round((volume - 22) / 4.0);
    } else if (volume <= 56) {
        s_volume = 5 + round((volume - 26) / 2.0);
    } else if (volume <= 80) {
        s_volume = 20 + round((volume - 56) / 0.8);
    } else if (volume <= 94) {
        s_volume = 50 + round((volume - 80) / 0.47);
    } else if (volume <= 100) {
        s_volume = 80 + round((volume - 94) / 0.3);
    }
    
    ESP_LOGI(TAG, "Mapped volume: %"PRIu32"", s_volume);
}

esp_err_t scream_sender_init(void)
{
    if (s_is_sender_initialized) {
        ESP_LOGW(TAG, "Scream sender already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing Scream sender");
    
    // Initialize the socket
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Configure socket options for better reliability
    int opt_val;
    
    // Increase send buffer size
    opt_val = UDP_TX_BUFFER_SIZE;
    if (setsockopt(s_sock, SOL_SOCKET, SO_SNDBUF, &opt_val, sizeof(opt_val)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_SNDBUF: errno %d", errno);
    }
    
    // Set timeout to prevent blocking too long on send
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = UDP_SEND_TIMEOUT_MS * 1000;
    if (setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_SNDTIMEO: errno %d", errno);
    }
    
    // Set QoS priority for audio traffic
    #if defined(IP_TOS) && defined(IPTOS_DSCP_EF)
    // Use Expedited Forwarding (EF) for real-time audio
    opt_val = IPTOS_DSCP_EF;
    if (setsockopt(s_sock, IPPROTO_IP, IP_TOS, &opt_val, sizeof(opt_val)) < 0) {
        ESP_LOGW(TAG, "Failed to set IP_TOS: errno %d", errno);
    }
    #endif
    
    // Initialize the destination address from settings
    app_config_t *config = config_manager_get_config();
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_addr.s_addr = inet_addr(config->sender_destination_ip);
    s_dest_addr.sin_port = htons(config->sender_destination_port);
    
    // Initialize the Scream header in output buffer
    memcpy(s_data_out, header, HEADER_SIZE);
    
    // Initialize the UAC device
    uac_device_config_t uac_config = {
        .output_cb = uac_device_output_cb,
        .set_mute_cb = uac_device_set_mute_cb,
        .set_volume_cb = uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };
    
    esp_err_t ret = uac_device_init(&uac_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UAC device: %s", esp_err_to_name(ret));
        close(s_sock);
        s_sock = -1;
        return ret;
    }
    
    s_is_sender_initialized = true;
    return ESP_OK;
}

esp_err_t scream_sender_start(void)
{
    if (!s_is_sender_initialized) {
        ESP_LOGE(TAG, "Scream sender not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_is_sender_running) {
        ESP_LOGW(TAG, "Scream sender already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting Scream sender");
    
    // Reset the data buffer
    s_data_in_head = 0;
    
    s_is_sender_running = true;
    return ESP_OK;
}

esp_err_t scream_sender_stop(void)
{
    if (!s_is_sender_initialized) {
        ESP_LOGE(TAG, "Scream sender not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_is_sender_running) {
        ESP_LOGW(TAG, "Scream sender not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping Scream sender");
    
    s_is_sender_running = false;
    return ESP_OK;
}

bool scream_sender_is_running(void)
{
    return s_is_sender_running;
}

void scream_sender_set_mute(bool mute)
{
    s_is_muted = mute;
    ESP_LOGI(TAG, "Scream sender mute set to %d", mute);
}

void scream_sender_set_volume(uint32_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    
    s_volume = volume;
    ESP_LOGI(TAG, "Scream sender volume set to %"PRIu32"", volume);
}

esp_err_t scream_sender_update_destination(void)
{
    if (!s_is_sender_initialized) {
        ESP_LOGE(TAG, "Scream sender not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Update the destination address from settings
    app_config_t *config = config_manager_get_config();
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_addr.s_addr = inet_addr(config->sender_destination_ip);
    s_dest_addr.sin_port = htons(config->sender_destination_port);
    
    ESP_LOGI(TAG, "Updated Scream sender destination to %s:%u", 
             config->sender_destination_ip, config->sender_destination_port);
    
    return ESP_OK;
}
#endif
