#include "mdns_service.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"

#include <string.h>

static const char *TAG = "MDNS_SERVICE";

#define MDNS_INSTANCE_NAME "ESP32 Scream Receiver"
#define MDNS_SERVICE_TYPE "_scream"
#define MDNS_PROTO "_udp"
#define MDNS_PORT 4010 // Scream data port, not mDNS port (5353)
#define MDNS_SCREAM_HOST "_sink._scream._udp"

// Function to get the local IP address that would be used to reach a remote IP
static esp_ip4_addr_t get_local_ip_for_remote(const esp_ip4_addr_t *remote_ip)
{
    esp_ip4_addr_t local_ip = {0};
    
    // Create a socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return local_ip;
    }
    
    // Set up the remote address
    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_addr.s_addr = remote_ip->addr;
    remote_addr.sin_port = htons(65530); // Use a high port number that's likely not in use
    
    // Connect the socket to the remote address
    // This doesn't actually establish a connection for UDP, but it sets the default destination
    // and more importantly, it makes the socket select a source IP address
    int err = connect(sock, (struct sockaddr*)&remote_addr, sizeof(remote_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket connect failed: %d", errno);
        close(sock);
        return local_ip;
    }
    
    // Get the local address that was selected
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    err = getsockname(sock, (struct sockaddr*)&local_addr, &addr_len);
    if (err < 0) {
        ESP_LOGE(TAG, "getsockname failed: %d", errno);
        close(sock);
        return local_ip;
    }
    
    // Copy the IP address
    local_ip.addr = local_addr.sin_addr.s_addr;
    
    // Clean up
    close(sock);
    
    ESP_LOGI(TAG, "Local IP for remote %s is %s", 
             ip4addr_ntoa((const ip4_addr_t*)remote_ip),
             ip4addr_ntoa((const ip4_addr_t*)&local_ip));
    
    return local_ip;
}

/**
 * @brief Initializes and starts the mDNS service.
 */
void mdns_service_start(void)
{
    esp_err_t err;

    // Initialize mDNS service
    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS Init failed: %d", err);
        return;
    }

    // Set default instance
    err = mdns_instance_name_set(MDNS_INSTANCE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %d", err);
        mdns_free(); // Clean up
        return;
    }

    // Structure with TXT records
    mdns_txt_item_t serviceTxtData[] = {
        {"type", "sink"},
        {"bit_depth", "16"},
        {"sample_rate", "48000"},
        {"channels", "2"},
        {"channel_layout", "stereo"}
    }; 

    // Add service
    err = mdns_service_add(MDNS_INSTANCE_NAME, MDNS_SERVICE_TYPE, MDNS_PROTO, MDNS_PORT, serviceTxtData, sizeof(serviceTxtData) / sizeof(serviceTxtData[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %d", err);
        mdns_free(); // Clean up
        return;
    }

    // Register a delegate hostname for "_sink._screamrouter"
    // Get our IP address
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to get default netif");
    } else {
        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get IP info");
        } else {
            // Create an IP address list with our IP
            mdns_ip_addr_t addr;
            addr.addr.type = ESP_IPADDR_TYPE_V4;
            addr.addr.u_addr.ip4 = ip_info.ip;
            addr.next = NULL;
            
            // Add the delegate hostname
            err = mdns_delegate_hostname_add(MDNS_SCREAM_HOST, &addr);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "mdns_delegate_hostname_add for %s failed: %d", MDNS_SCREAM_HOST, err);
                // Continue anyway, as the main service is still working
            } else {
                ESP_LOGI(TAG, "Added delegate hostname: %s with IP %s", 
                         MDNS_SCREAM_HOST, ip4addr_ntoa((const ip4_addr_t*)&ip_info.ip));
            }
        }
    }

    ESP_LOGI(TAG, "mDNS service started: %s.%s.%s port %d", MDNS_INSTANCE_NAME, MDNS_SERVICE_TYPE, MDNS_PROTO, MDNS_PORT);
}

/**
 * @brief Stops the mDNS service.
 */
void mdns_service_stop(void)
{
    // Check if mDNS is initialized before trying to free it
    // Note: mdns_free() handles the case where it wasn't initialized,
    // but checking avoids unnecessary log messages if it wasn't started.
    // A more robust check would involve a state variable set in mdns_service_start.
    mdns_free();
    ESP_LOGI(TAG, "mDNS service stopped.");
}
