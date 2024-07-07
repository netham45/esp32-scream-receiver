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
#include "global.h"
#ifdef IS_USB
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#endif
#include "secrets.h"

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define BIT1_SPK_START          (0x01 << 0)
#define DEFAULT_VOLUME          45
#define RTC_CNTL_OPTION1_REG 0x6000812C
#define RTC_CNTL_FORCE_DOWNLOAD_BOOT 1
bool g_neighbor_report_active = false;
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
#ifdef IS_USB
static uac_host_device_handle_t s_spk_dev_handle = NULL;
static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg);
static QueueHandle_t s_event_queue = NULL;
bool usb_host_running = true;

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

static void uf2_update_complete_cb()
{
    TaskHandle_t main_task_hdl = xTaskGetHandle("main");
    xTaskNotifyGive(main_task_hdl);
}

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        // stop audio player first
        s_spk_dev_handle = NULL;
        //audio_player_stop();
		stop_playback();
        ESP_LOGI(TAG, "UAC Device disconnected");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        return;
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
			ESP_LOGI(TAG, "CTRL: Invalid Neighbor Report element: id=%u len=%u",
					data[0], nr_len);
			ret = -1;
			goto cleanup;
		}

		if (2U + nr_len > report_len) {
			ESP_LOGI(TAG, "CTRL: Invalid Neighbor Report element: id=%u len=%zu nr_len=%u",
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
				" info=0x%" PRIx32 " op_class=%u chan=%u phy_type=%u%s%s%s%s",
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
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%u", nr[ETH_ALEN + 4]);
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
		/* channel number */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%u", nr[ETH_ALEN + 5]);
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, ",");
		/* phy type */
		len += snprintf(buf + len, MAX_NEIGHBOR_LEN - len, "%u", nr[ETH_ALEN + 6]);
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
    ESP_LOGD(TAG, "rrm: neighbor report len=%d", report_len);
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

static void event_handler(void* arg, esp_event_base_t event_base,
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
                ESP_LOGI(TAG, "station disconnected with reason %d", disconn->reason);
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
}
void wifi_init_sta(void);

void app_main(void)
{
	    //Initialize NVS
    BaseType_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
#ifdef IS_USB
    s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
    assert(s_event_queue != NULL);
    static TaskHandle_t uac_task_handle = NULL;
    ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_events", 4096, NULL,
                                             USER_TASK_PRIORITY, &uac_task_handle, 0);
    assert(ret == pdTRUE);
    ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                                  USB_HOST_TASK_PRIORITY, NULL, 0);
    assert(ret == pdTRUE);
#endif

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
	setup_buffer();
	setup_audio();
	setup_network();
			bool status = false;
    while (1) {
		if (!gpio_get_level(0)) {
		}
        vTaskDelay(100);
    }
}


void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );;
											
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_NEIGHBOR_REP,
				&esp_neighbor_report_recv_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW,
				&esp_bss_rssi_low_handler, NULL));


    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
			.sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
			.scan_method = WIFI_ALL_CHANNEL_SCAN,
			.rm_enabled =1,
			.btm_enabled =1,
			.mbo_enabled =1,
			.pmf_cfg.capable = 1,
			.ft_enabled =1,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
	

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
			
	esp_wifi_set_rssi_threshold(-58);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap"
                 );
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to wifi");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}