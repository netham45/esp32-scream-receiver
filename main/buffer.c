#include "esp_psram.h"
#include "global.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_log.h"
#include "esp_psram.h"

// Flag if the stream is currently underrun and rebuffering
bool is_underrun                        = true;
// Number of received packets since last underflow
uint64_t received_packets               = 0;
// Number of packets in ring buffer
unsigned int packet_buffer_size             = 0;
// Position of ring buffer read head
uint64_t packet_buffer_pos              = 0;
// Number of bytes to buffer
uint64_t target_buffer_size             = INITIAL_BUFFER_SIZE;
// Buffer of packets to send
uint8_t *packet_buffer[MAX_BUFFER_SIZE] = { 0 };
portMUX_TYPE buffer_mutex = portMUX_INITIALIZER_UNLOCKED;

void set_underrun() {
  if (!is_underrun) {
    received_packets = 0;
    target_buffer_size += BUFFER_GROW_STEP_SIZE;
    if (target_buffer_size >= MAX_BUFFER_SIZE)
      target_buffer_size = MAX_BUFFER_SIZE;
    ESP_LOGI(TAG, "Buffer Underflow");
  }
  is_underrun = true;
}

bool push_chunk(uint8_t *chunk) {
  int write_position = (packet_buffer_pos + packet_buffer_size) % MAX_BUFFER_SIZE;
//  ESP_LOGI(TAG, "memcpy(%p, %p, %i)", packet_buffer[write_position], chunk, PCM_CHUNK_SIZE); 
  //taskENTER_CRITICAL(&buffer_mutex);
  if (packet_buffer_size == MAX_BUFFER_SIZE) {
    packet_buffer_size = target_buffer_size;
    //taskEXIT_CRITICAL(&buffer_mutex);
    ESP_LOGI(TAG, "Buffer Overflow");
    return false;
  }

  write_position = (packet_buffer_pos + packet_buffer_size) % MAX_BUFFER_SIZE;
  memcpy(packet_buffer[write_position], chunk, PCM_CHUNK_SIZE);
  packet_buffer_size++;
  received_packets++;
  if (received_packets >= target_buffer_size)
    is_underrun = false;
  //taskEXIT_CRITICAL(&buffer_mutex);
  return true;
}

uint8_t *pop_chunk() {
  //taskENTER_CRITICAL(&buffer_mutex);
  if (packet_buffer_size == 0) {
    //taskEXIT_CRITICAL(&buffer_mutex);
    set_underrun();
    return NULL;
  }
  if (is_underrun) {
    //taskEXIT_CRITICAL(&buffer_mutex);
    return NULL;
  }
  uint8_t *return_chunk = packet_buffer[packet_buffer_pos];
  packet_buffer_size--;
  packet_buffer_pos = (packet_buffer_pos + 1) % MAX_BUFFER_SIZE;
  //taskEXIT_CRITICAL(&buffer_mutex);
  return return_chunk;
}

void setup_buffer() {
  ESP_LOGI(TAG, "Allocating buffer");
  uint8_t *buffer = 0;

  buffer = (uint8_t *)malloc(PCM_CHUNK_SIZE * MAX_BUFFER_SIZE);
  memset(buffer, 0, PCM_CHUNK_SIZE * MAX_BUFFER_SIZE);
  for (int i = 0; i < MAX_BUFFER_SIZE; i++)
    packet_buffer[i] = (uint8_t *)buffer + i * PCM_CHUNK_SIZE;
  ESP_LOGI(TAG, "Buffer allocated");
}