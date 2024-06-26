#include "esp_wifi.h"                    // ESP IDF
#include "global.h"
#include "buffer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            // struct addrinfo
#include <arpa/inet.h>
#include "esp_netif.h"



const uint16_t HEADER_SIZE = 5;                         // Scream Header byte size, non-configurable (Part of Scream)
const uint16_t PACKET_SIZE = PCM_CHUNK_SIZE + HEADER_SIZE;

void tcp_handler(void *) {
  uint8_t in_buffer[PACKET_SIZE];  // TCP input buffer
  int connect_failure = 0;
  struct sockaddr_in dest_addr;
  bool connected = false;
	inet_pton(AF_INET, SERVER, &dest_addr.sin_addr);
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(PORT);
	int sock =  socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  ESP_LOGI(TAG, "Connecting to ScreamRouter");
  while (!connected) {
    ESP_LOGI(TAG, "Failed to connect");
	int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err != 0) {
		ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
	} else {
		connected = true;
		break;
	}
    vTaskDelay(250);
    if (connect_failure++ >= 50)
      ESP_LOGI(TAG, "wifi isn't connecting");
  }
  ESP_LOGI(TAG, "Connected to ScreamRouter");
  uint8_t data[1157*2];
  uint16_t datahead = 0;
  while (connected) {
	int result = read(sock, data + datahead, PACKET_SIZE);
	datahead += result;
	if (datahead >= PACKET_SIZE) {
		push_chunk(data + HEADER_SIZE);
		memcpy(data,data + PACKET_SIZE, PACKET_SIZE);
		datahead -= PACKET_SIZE;
	}
    vTaskDelay(1);
  }
}

void setup_network() {
  xTaskCreatePinnedToCore(tcp_handler, "tcp_handler", 8192, NULL, 1, NULL, 1);
}
