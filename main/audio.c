#include "global.h"
#include "buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#ifdef IS_SPDIF
#include "spdif.h"
#endif
#ifdef IS_USB
#include "usb/uac_host.h"
uac_host_device_handle_t spkr_handle = NULL;
#endif

bool playing = false;

uint8_t volume = 100;
uint8_t silence[32] = {0};
bool is_silent = false;

void resume_playback() {
#ifdef IS_USB
    // Only try to resume if we have a valid DAC handle
    if (spkr_handle != NULL) {
        uac_host_stream_config_t stm_config = {
            .channels = 2,
            .bit_resolution = 16,
            .sample_freq = 48000,
        };
        ESP_LOGI(TAG, "Resume Playback with DAC");
        ESP_ERROR_CHECK(uac_host_device_start(spkr_handle, &stm_config));
        ESP_ERROR_CHECK(uac_host_device_set_volume(spkr_handle, volume));
        playing = true;
    } else {
        ESP_LOGI(TAG, "Cannot resume playback - No DAC connected");
        // Do NOT set playing to true if no DAC is available
    }
#endif
}

#ifdef IS_USB
void start_playback(uac_host_device_handle_t _spkr_handle) {
	spkr_handle = _spkr_handle;
}
#endif

void stop_playback() {
	playing = false;
	ESP_LOGI(TAG, "Stop Playback");
#ifdef IS_USB
	uac_host_device_stop(spkr_handle);
#endif
}

void audio_direct_write(uint8_t *data) {
#ifdef IS_USB
  // Check if we have a valid DAC handle before writing
  if (spkr_handle != NULL) {
    uac_host_device_write(spkr_handle, data, PCM_CHUNK_SIZE, portMAX_DELAY);
  } else {
    // DAC is not connected - we should be in sleep mode
    ESP_LOGD(TAG, "Attempted write with no DAC");
  }
#endif
#ifdef IS_SPDIF
  spdif_write(data, PCM_CHUNK_SIZE);
#endif
}

void pcm_handler(void*) {
  while (true) {
	  if (playing) {
		uint8_t *data = pop_chunk();
		if (data) {
#ifdef IS_USB
			if (spkr_handle != NULL) {
				uac_host_device_write(spkr_handle, data, PCM_CHUNK_SIZE, portMAX_DELAY);
				is_silent = false;
			} else {
				// DAC is not connected but we're trying to play - should enter sleep
				// This shouldn't happen if our start/stop logic is correct
				ESP_LOGW(TAG, "PCM handler tried to write with no DAC");
				playing = false; // Force playback to stop
				is_silent = true;
			}
#endif
#ifdef IS_SPDIF
			spdif_write(data, PCM_CHUNK_SIZE);
			is_silent = false;
#endif
		}
		else if (!is_silent) {
			ESP_LOGI(TAG, "Silent");
			is_silent = true;
		}
		//else
//		    for (int i=0;i<1152/32;i++)
	//		    uac_host_device_write(spkr_handle, silence, 32, portMAX_DELAY);
		//	vTaskDelay(7);
	  }
    vTaskDelay(1);
  }
}

void setup_audio() {
#ifdef IS_SPDIF
  spdif_init(48000);
#endif
  xTaskCreatePinnedToCore(pcm_handler, "pcm_handler", 16384, NULL, 1, NULL, 1);
}
