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
#include "secrets.h"
#include "audio.h"

const uint16_t HEADER_SIZE = 0;                         // Scream Header byte size, non-configurable (Part of Scream)
const uint16_t PACKET_SIZE = PCM_CHUNK_SIZE + HEADER_SIZE;
bool use_tcp = true;
bool connected = false;

char server[16] = {0};

void udp_handler(void *);
void tcp_handler(void *);  

void tcp_handler(void *) {
  int connect_failure = 0;
  struct sockaddr_in dest_addr;
  inet_pton(AF_INET, server, &dest_addr.sin_addr);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(PORT);
  int sock =  socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  const int ip_precedence_vi = 6;
  const int ip_precedence_offset = 5;
  int priority = (ip_precedence_vi << ip_precedence_offset);
  setsockopt(sock, IPPROTO_IP, IP_TOS, &priority, sizeof(priority));
  int val = 1;
  setsockopt(sock, 6/*SOL_TCP*/, TCP_NODELAY, &val, sizeof(val));
  empty_buffer();
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
  uint8_t data[PACKET_SIZE * 2];
  uint16_t datahead = 0;
  resume_playback();
  while (connected) {
	int result = recv(sock, data + datahead, PACKET_SIZE, 0);
	if (!result)
		connected = false;
	datahead += result;
	if (datahead >= PACKET_SIZE) {
		//push_chunk(data + HEADER_SIZE);
		audio_direct_write(data + HEADER_SIZE);
		memcpy(data, data + PACKET_SIZE, PACKET_SIZE);
		datahead -= PACKET_SIZE;
	}
    vTaskDelay(1);
  }
  close(sock);
  stop_playback();
  xTaskCreatePinnedToCore(udp_handler, "udp_handler", 8192, NULL, 1, NULL, 1);
  vTaskDelete(NULL);
}

void udp_handler(void *) {
	uint8_t data[PACKET_SIZE * 2];
	uint16_t datahead = 0;
	empty_buffer();
    while (1) {
		struct sockaddr_in dest_addr_ip4;
		dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
		dest_addr_ip4.sin_family = AF_INET;
		dest_addr_ip4.sin_port = htons(PORT);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	    const int ip_precedence_vi = 6;
		const int ip_precedence_offset = 5;
		int priority = (ip_precedence_vi << ip_precedence_offset);
	    setsockopt(sock, IPPROTO_IP, IP_TOS, &priority, sizeof(priority));
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        int err = bind(sock, (struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket bound, port %d", PORT);
		resume_playback();
        while (1) {
            int result = recv(sock, data + datahead, PACKET_SIZE, 0);
			if (result && use_tcp) {
				struct sockaddr_in addr;
				socklen_t addrlen = sizeof(struct sockaddr_in);
				recvfrom(sock, data + datahead, PACKET_SIZE, 0, (struct sockaddr *)&addr, &addrlen);
				strcpy(server, inet_ntoa(addr.sin_addr));
				xTaskCreatePinnedToCore(tcp_handler, "tcp_handler", 8192, NULL, 1, NULL, 1);
				close(sock);
				stop_playback();
				vTaskDelete(NULL);
				return;
			}
		 	datahead += result;
			if (datahead >= PACKET_SIZE) {
				//push_chunk(data + HEADER_SIZE);
				audio_direct_write(data + HEADER_SIZE);
				memcpy(data,data + PACKET_SIZE, PACKET_SIZE);
				datahead -= PACKET_SIZE;
			}
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
	stop_playback();
    vTaskDelete(NULL);
}

void setup_network() {
  xTaskCreatePinnedToCore(udp_handler, "udp_handler", 8192, NULL, 1, NULL, 1);
}	

void restart_network() {
  if (use_tcp)
    connected = false;
}
