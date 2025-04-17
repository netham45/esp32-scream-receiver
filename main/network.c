#include "esp_wifi.h"                    // ESP IDF
#include "global.h"
#include "buffer.h"
#include "config_manager.h"             // Added for configuration
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            // struct addrinfo
#include <arpa/inet.h>
#include <sys/select.h>       // Added for select()
#include "esp_netif.h"
#include "audio.h"
#include "wifi_manager.h"

// Reference to sleep mode monitoring variables
extern volatile uint32_t packet_counter;
extern volatile bool monitoring_active;
extern volatile TickType_t last_packet_time;

const uint16_t HEADER_SIZE = 5;                         // Scream Header byte size, non-configurable (Part of Scream)
const uint16_t PACKET_SIZE = PCM_CHUNK_SIZE + HEADER_SIZE;
bool use_tcp = false;
bool connected = false;

char server[16] = {0};

void udp_handler(void *);
void tcp_handler(void *);

// We should NOT sleep during active audio processing - removed network_light_sleep function

void tcp_handler(void *) {
  int connect_failure = 0;
  // Get configuration
  app_config_t *config = config_manager_get_config();
  
  struct sockaddr_in dest_addr;
  inet_pton(AF_INET, server, &dest_addr.sin_addr);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(config->port);
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
    fd_set read_fds;
    struct timeval tv;
    int select_result;

    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms

    select_result = select(sock + 1, &read_fds, NULL, NULL, &tv);

    if (select_result < 0) {
        ESP_LOGE(TAG, "TCP select error: errno %d", errno);
        connected = false; // Or handle error differently
        continue;
    } else if (select_result == 0) {
        // Timeout occurred, no data ready, loop continues (replaces vTaskDelay)
        continue;
    }
    // Only proceed if select indicates data is ready (select_result > 0 and FD_ISSET)
    if (!FD_ISSET(sock, &read_fds)) {
        continue; // Should not happen if select_result > 0, but good practice
    }

	int result = recv(sock, data + datahead, PACKET_SIZE, 0);
	if (result <= 0) { // Handle error or closed connection
        if (result < 0) {
            ESP_LOGE(TAG, "TCP recv error: errno %d", errno);
        } else {
            ESP_LOGI(TAG, "TCP connection closed by peer");
        }
		connected = false;
        continue;
    }
	datahead += result;
	
	// Track packet reception for activity detection during sleep mode
	if (result > 0 && monitoring_active) {
	    packet_counter++;
	    last_packet_time = xTaskGetTickCount(); // Update last packet time
        // Signal the network monitor task that a packet was received
        if (s_network_activity_event_group != NULL) {
            xEventGroupSetBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
        }
	}
	
	if (datahead >= PACKET_SIZE) {
	    // Use direct write or buffered mode based on configuration
	    if (config->use_direct_write) {
		    audio_direct_write(data + HEADER_SIZE);
	    } else {
		    //push_chunk(data + HEADER_SIZE);
            audio_direct_write(data + HEADER_SIZE);
	    }
		memcpy(data, data + PACKET_SIZE, PACKET_SIZE);
		datahead -= PACKET_SIZE;
	}
    // vTaskDelay(1) removed, replaced by select timeout
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
		// Get configuration
		app_config_t *config = config_manager_get_config();
		
		struct sockaddr_in dest_addr_ip4;
		dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
		dest_addr_ip4.sin_family = AF_INET;
		dest_addr_ip4.sin_port = htons(config->port);

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
        ESP_LOGI(TAG, "Socket bound, port %" PRIu16, config->port);
        
        // Only try to resume playback if we're not in sleep mode
        if (!device_sleeping) {
            resume_playback();
        } else {
            ESP_LOGI(TAG, "Device is in sleep mode - not resuming playback");
        }
        while (1) {
            fd_set read_fds;
            struct timeval tv;
            int select_result;

            FD_ZERO(&read_fds);
            FD_SET(sock, &read_fds);

            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms

            select_result = select(sock + 1, &read_fds, NULL, NULL, &tv);

            if (select_result < 0) {
                ESP_LOGE(TAG, "UDP select error: errno %d", errno);
                // Decide how to handle UDP select error, maybe break or continue
                vTaskDelay(pdMS_TO_TICKS(100)); // Avoid busy-looping on error
                continue;
            } else if (select_result == 0) {
                // Timeout occurred, no data ready, loop continues (replaces vTaskDelay)
                continue;
            }
            // Only proceed if select indicates data is ready (select_result > 0 and FD_ISSET)
             if (!FD_ISSET(sock, &read_fds)) {
                continue; // Should not happen if select_result > 0
            }

            // Data is ready, proceed with recv
            int result = recv(sock, data + datahead, PACKET_SIZE, 0);

            if (result < 0) {
                ESP_LOGE(TAG, "UDP recv error: errno %d", errno);
                // Handle recv error if necessary, maybe continue
                continue;
            }
            if (result == 0) {
                // For UDP, recv returning 0 is unusual but might indicate an issue.
                // Unlike TCP, it doesn't mean connection closed. Log it?
                ESP_LOGW(TAG, "UDP recv returned 0 bytes");
                continue;
            }

			// Track packet reception for activity detection during sleep mode
			if (result > 0 && monitoring_active) {
			    packet_counter++;
			    last_packet_time = xTaskGetTickCount(); // Update last packet time
                // Signal the network monitor task that a packet was received
                if (s_network_activity_event_group != NULL) {
                    xEventGroupSetBits(s_network_activity_event_group, NETWORK_PACKET_RECEIVED_BIT);
                }
			    // If we're in sleep mode and detected sufficient activity,
			    // this will be handled by the monitor task
			}
			
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
			    // Use direct write or buffered mode based on configuration
			    if (config->use_direct_write) {
				    audio_direct_write(data + HEADER_SIZE);
			    } else {
				    //push_chunk(data + HEADER_SIZE);
                    audio_direct_write(data + HEADER_SIZE);
			    }
				memcpy(data,data + PACKET_SIZE, PACKET_SIZE);
				datahead -= PACKET_SIZE;
			}
             // vTaskDelay(1) removed, replaced by select timeout
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
