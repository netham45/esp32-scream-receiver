#include "global.h"
#include "buffer.h"
#include "config_manager.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
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
uint64_t silence_duration_ms = 0;
TickType_t last_audio_time = 0;

// Forward declaration of the sleep function we'll define in usb_audio_player_main.c
extern void enter_silence_sleep_mode();

bool is_playing() {
  return playing;
}

void resume_playback() {
#ifdef IS_USB
    // Only try to resume if we have a valid DAC handle
    if (spkr_handle != NULL) {
        // Get current configuration
        app_config_t *config = config_manager_get_config();
        
        uac_host_stream_config_t stm_config = {
            .channels = 2,
            .bit_resolution = config->bit_depth,
            .sample_freq = config->sample_rate,
        };
        ESP_LOGI(TAG, "Resume Playback with DAC (SR: %" PRIu32 ", BD: %" PRIu8 ")", 
                 config->sample_rate, config->bit_depth);
        
        ESP_ERROR_CHECK(uac_host_device_start(spkr_handle, &stm_config));
        ESP_ERROR_CHECK(uac_host_device_set_volume(spkr_handle, config->volume * 100.0f));
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
  // Reset silence tracking
  is_silent = false;
  silence_duration_ms = 0;
  last_audio_time = xTaskGetTickCount(); // Reset to current time
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
  // Initialize the last audio time to current time
  last_audio_time = xTaskGetTickCount();
  
  while (true) {
      if (playing) {
          uint8_t *data = pop_chunk();
          TickType_t current_time = xTaskGetTickCount();
          
          if (data) {
              if (is_silent) {
                  is_silent = false;
              }
              silence_duration_ms = 0;
              last_audio_time = xTaskGetTickCount(); // Reset to current time

              
              // Process the audio data
#ifdef IS_USB
              if (spkr_handle != NULL) {
                  uac_host_device_write(spkr_handle, data, PCM_CHUNK_SIZE, portMAX_DELAY);
              } else {
                  // DAC is not connected but we're trying to play - should enter sleep
                  ESP_LOGW(TAG, "PCM handler tried to write with no DAC");
                  playing = false; // Force playback to stop
              }
#endif
#ifdef IS_SPDIF
              spdif_write(data, PCM_CHUNK_SIZE);
#endif
          } else {
              // pop_chunk() returned NULL - NO PACKETS RECEIVED - THIS IS SILENCE!
              if (!is_silent) {
                  is_silent = true;
                  last_audio_time = current_time; // Start the silence timer
              }
              
              // Calculate how long we've been in silence, handling tick counter rollover
              // When current_time < last_audio_time, it means the counter has rolled over
              if (current_time < last_audio_time) {
                  // Handle rollover: calculate time until max value, then add time since 0
                  silence_duration_ms = ((portMAX_DELAY - last_audio_time) + current_time) * portTICK_PERIOD_MS;
              } else {
                  silence_duration_ms = (current_time - last_audio_time) * portTICK_PERIOD_MS;
              }
              
              // Only log occasionally to avoid spamming
              if (silence_duration_ms % 5000 == 0 && silence_duration_ms > 0) {
                  ESP_LOGI(TAG, "Silence duration: %" PRIu32 " ms", silence_duration_ms);
              }
              
              // Check if silence threshold is reached - use config value
              app_config_t *config = config_manager_get_config();
              if (silence_duration_ms < 30000) {
                if (silence_duration_ms >= config->silence_threshold_ms) {
                    ESP_LOGI(TAG, "Silence threshold reached (%" PRIu32 " ms), entering sleep mode", 
                            silence_duration_ms);
                    
                    // Trigger sleep mode
                    enter_silence_sleep_mode();
                }
              } else {
                ESP_LOGI(TAG, "Absurd silence threshold ignored (%" PRIu32 " ms)", 
                  silence_duration_ms);
                  last_audio_time = current_time;
              }
          }
      }
      vTaskDelay(1);
  }
}

void setup_audio() {
#ifdef IS_SPDIF
  // Get configuration for sample rate
  app_config_t *config = config_manager_get_config();
  esp_err_t err = spdif_init(config->sample_rate);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Initialized SPDIF with pin %d and sample rate: %" PRIu32, config->spdif_data_pin, config->sample_rate);
  } else {
    ESP_LOGE(TAG, "Failed to initialize SPDIF with pin %d and sample rate %" PRIu32 ": %s", 
             config->spdif_data_pin, config->sample_rate, esp_err_to_name(err));
    ESP_LOGW(TAG, "Audio output will not be available. Please check the SPDIF pin configuration in the web UI.");
    // Continue running - we won't have audio output but the web UI will still work
  }
#endif
  xTaskCreatePinnedToCore(pcm_handler, "pcm_handler", 16384, NULL, 1, NULL, 1);
}
