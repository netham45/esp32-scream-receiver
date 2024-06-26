#include "global.h"
#include "buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/uac_host.h"
#include "esp_log.h"

uac_host_device_handle_t spkr_handle = NULL;

bool playing = false;

uint8_t volume = 2;
uint8_t silence[32] = {0};

void start_playback(uac_host_device_handle_t _spkr_handle) {
	spkr_handle = _spkr_handle;
	uac_host_stream_config_t stm_config = {
        .channels = 2,
        .bit_resolution = 16,
        .sample_freq = 48000,
    };
	playing = true;
    ESP_ERROR_CHECK(uac_host_device_start(spkr_handle, &stm_config));
	ESP_ERROR_CHECK(uac_host_device_set_volume(spkr_handle, volume));
}

void stop_playback() {
	playing = false;
	uac_host_device_stop(spkr_handle);
}

void pcm_handler(void*) {
  while (true) {
	  if (playing) {
		 uint8_t *data = pop_chunk();
		 if (data) {
			 int result = uac_host_device_write(spkr_handle, data, PCM_CHUNK_SIZE, portMAX_DELAY);
		 }
		 else
			for (int i=0;i<1152;i++)
			 uac_host_device_write(spkr_handle, silence, 32, portMAX_DELAY);
	  }
    vTaskDelay(1);
  }
}

void setup_audio() {
  xTaskCreatePinnedToCore(pcm_handler, "pcm_handler", 16384, NULL, 1, NULL, 1);
}