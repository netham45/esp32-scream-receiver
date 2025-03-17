#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wnm.h"
#include "esp_rrm.h"
#include "esp_mbo.h"
#include "esp_mac.h"
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
#ifdef IS_USB
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#endif
// Include our new modules
#include "config_manager.h"
#include "wifi_manager.h"
#include "web_server.h"

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define BIT1_SPK_START          (0x01 << 0)
#define DEFAULT_VOLUME          45
#define RTC_CNTL_OPTION1_REG 0x6000812C
#define RTC_CNTL_FORCE_DOWNLOAD_BOOT 1
// Using DAC_CHECK_SLEEP_TIME_MS from config.h (2000ms)
#define NETWORK_SLEEP_TIME_MS 10       // Light sleep between network operations
bool g_neighbor_report_active = false;
bool device_sleeping = false;
typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

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

static inline uint32_t WPA_GET_LE32(const uint8_t *a)
{
	return ((uint32_t) a[3] << 24) | (a[2] << 16) | (a[1] << 8) | a[0];
}


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

static char * get_btm_neighbor_list(uint8_t *report, size_t report_len)
{
	size_t len = 0;
	const uint8_t *data;
	int ret = 0;

	char *lci = NULL;
	char *civic = NULL;
	/*
	 * Neighbor Report element (IEEE P802.11-REVmc/D5.0)
	 * BSSID[6]
	 * BSSID Information[4]
	 * Operating Class[1]
	 * Channel Number[1]
	 * PHY Type[1]
	 * Optional Subelements[variable]
	 */
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
			ESP_LOGI(TAG, "CTRL: Invalid Neighbor Report element: id=%" PRIu16 " len=%" PRIu16,
					data[0], nr_len);
			ret = -1;
			goto cleanup;
		}

		if (2U + nr_len > report_len) {
			ESP_LOGI(TAG, "CTRL: Invalid Neighbor Report element: id=%" PRIu16 " len=%" PRIu16 " nr_len=%" PRIu16,
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
				/* Measurement Token[1] */
				/* Measurement Report Mode[1] */
				/* Measurement Type[1] */
				/* Measurement Report[variable] */
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

		/* neighbor start */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, " neighbor=");
		/* bssid */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, MACSTR, MAC2STR(nr));
		/* , */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
		/* bssid info */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "0x%04" PRIx32 "", WPA_GET_LE32(nr + ETH_ALEN));
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
		/* operating class */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%" PRIu16, nr[ETH_ALEN + 4]);
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
		/* channel number */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%" PRIu16, nr[ETH_ALEN + 5]);
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
		/* phy type */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%" PRIu16, nr[ETH_ALEN + 6]);
		/* optional elements, skip */

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

static void esp_neighbor_report_recv_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
	if (!g_neighbor_report_active) {
		ESP_LOGV(TAG,"Neighbor report received but not triggered by us");
	    return;
    }
    if (!event_data) {
        ESP_LOGE(TAG, "No event data received for neighbor report");
        return;
    }
    g_neighbor_report_active = false;
    uint8_t cand_list = 0;
    wifi_event_neighbor_report_t *neighbor_report_event = (wifi_event_neighbor_report_t*)event_data;
    uint8_t *pos = (uint8_t *)neighbor_report_event->report;
    char * neighbor_list = NULL;
    if (!pos) {
        ESP_LOGE(TAG, "Neighbor report is empty");
        return;
    }
    uint8_t report_len = neighbor_report_event->report_len;
    /* dump report info */
    ESP_LOGD(TAG, "rrm: neighbor report len=%" PRIu16, report_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, pos, report_len, ESP_LOG_DEBUG);

    /* create neighbor list */
    neighbor_list = get_btm_neighbor_list(pos + 1, report_len - 1);
    /* In case neighbor list is not present issue a scan and get the list from that */
    if (!neighbor_list) {
        /* issue scan */
        wifi_scan_config_t params;
        memset(&params, 0, sizeof(wifi_scan_config_t));
        if (esp_wifi_scan_start(&params, true) < 0) {
		    goto cleanup;
	    }
	    /* cleanup from net802.11 */
        esp_wifi_clear_ap_list();
        cand_list = 1;
	}
	/* send AP btm query, this will cause STA to roam as well */
	esp_wnm_send_bss_transition_mgmt_query(REASON_FRAME_LOSS, neighbor_list, cand_list);
cleanup:
	if (neighbor_list)
		free(neighbor_list);

}

static void esp_bss_rssi_low_handler(void* arg, esp_event_base_t event_base,
		int32_t event_id, void* event_data)
{
	wifi_event_bss_rssi_low_t *event = event_data;

	//ESP_LOGI(TAG, "%s:bss rssi is=%d", __func__, event->rssi);
	/* Lets check channel conditions */
	//rrm_ctx++;
	if (esp_rrm_send_neighbor_report_request() < 0) {
		/* failed to send neighbor report request */
		ESP_LOGI(TAG, "failed to send neighbor report request");
		if (esp_wnm_send_bss_transition_mgmt_query(REASON_FRAME_LOSS, NULL, 0) < 0) {
			ESP_LOGI(TAG, "failed to send btm query");
		}
	} else {
		g_neighbor_report_active = true;
	}

}

/*static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = event_data;
        if (disconn->reason == WIFI_REASON_ROAMING) {
            ESP_LOGI(TAG, "station disconnected during roaming");
        } else {
            if (s_retry_num < 50) {
                ESP_LOGI(TAG, "station disconnected with reason %" PRIu16, disconn->reason);
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "retry to connect to the AP");
            } else {
                esp_restart();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		restart_network();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		ESP_LOGI(TAG, "setting rssi threshold as -58");
		esp_wifi_set_rssi_threshold(-58);
		if (esp_rrm_is_rrm_supported_connection()) {
			ESP_LOGI(TAG,"RRM supported");
                        esp_rrm_send_neighbor_report_request();
                        g_neighbor_report_active = true;
		} else {
			ESP_LOGI(TAG,"RRM not supported");
		}
		if (esp_wnm_is_btm_supported_connection()) {
			ESP_LOGI(TAG,"BTM supported");
		} else {
			ESP_LOGI(TAG,"BTM not supported");
		}

	}
}*/

// Task for monitoring network activity during silence sleep mode
TaskHandle_t network_monitor_task_handle = NULL;
volatile uint32_t packet_counter = 0;
volatile bool monitoring_active = false;
volatile TickType_t last_packet_time = 0;

// Network packet monitoring task that runs during light sleep
void network_monitor_task(void *params) {
    ESP_LOGI(TAG, "Network monitor task started");
    
    // Initialize the last packet time
    last_packet_time = xTaskGetTickCount();
    
    while (true) {
        if (monitoring_active) {
            TickType_t current_time = xTaskGetTickCount();
            
            // Check for activity - either enough packets received or packet timeout
            if (packet_counter >= ACTIVITY_THRESHOLD_PACKETS) {
                ESP_LOGI(TAG, "Network activity detected (%" PRIu32 " packets), exiting sleep mode", 
                        packet_counter);
                
                // Exit sleep mode
                exit_silence_sleep_mode();
                
                // Reset counter for next sleep cycle
                packet_counter = 0;
                last_packet_time = current_time;
            } 
            // Check for network inactivity timeout
            else if ((current_time - last_packet_time) * portTICK_PERIOD_MS >= NETWORK_INACTIVITY_TIMEOUT_MS) {
                // If we've been in silence mode for a while with no packets, 
                // this suggests the audio source is truly idle (not streaming)
                ESP_LOGI(TAG, "Network inactivity timeout reached (%" PRIu16 " ms), maintaining sleep mode",
                        NETWORK_INACTIVITY_TIMEOUT_MS);
                
                // Remain in sleep mode, but update timestamp to avoid log spam
                last_packet_time = current_time;
            }
            
            // Sleep for a short interval before checking again
            vTaskDelay(pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS));
        } else {
            // When not actively monitoring, just wait until activated
            vTaskSuspend(NULL);
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

void app_main(void)
{
    // Initialize NVS (required for USB subsystem)
    BaseType_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
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
    
#ifdef IS_USB
    // Initialize USB host first to detect DAC as early as possible
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
            esp_sleep_enable_timer_wakeup(DAC_CHECK_SLEEP_TIME_MS * 1000);
            // Go to deep sleep immediately
            esp_deep_sleep_start();
            return; // Never reached
        } else {
            ESP_LOGI(TAG, "No DAC detected but no WiFi configured, continuing with WiFi setup");
        }
    }
#endif

    // Only initialize the rest of the system if DAC is connected
    ESP_LOGI(TAG, "DAC detected, initializing full system with power optimizations");
    
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
    
    // Initialize configuration manager
    ESP_LOGI(TAG, "Initializing configuration manager");
    ESP_ERROR_CHECK(config_manager_init());
    
    setup_buffer();
    setup_audio();
    setup_network();
    
    // Main loop just keeps system alive - deep sleep is managed by event callbacks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void wifi_init_sta(void)
{
    ESP_LOGI(TAG, "Starting WiFi with manager");
    
    s_wifi_event_group = xEventGroupCreate();
    
    // Initialize the WiFi manager
    ESP_ERROR_CHECK(wifi_manager_init());
    
    // Register our existing event handlers for specific WiFi events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_NEIGHBOR_REP,
                                              &esp_neighbor_report_recv_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW,
                                              &esp_bss_rssi_low_handler, NULL));
    
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
    
    // If we're connected, set the RSSI threshold
    if (wifi_manager_get_state() == WIFI_MANAGER_STATE_CONNECTED) {
        esp_wifi_set_rssi_threshold(-58);
        
        // Notify the network module that WiFi is connected
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Get the current SSID for logging
        char ssid[WIFI_SSID_MAX_LENGTH + 1];
        if (wifi_manager_get_current_ssid(ssid, sizeof(ssid)) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to AP: %s", ssid);
        }
    } else {
        ESP_LOGI(TAG, "WiFi not connected, waiting for configuration via AP portal");
    }
}
