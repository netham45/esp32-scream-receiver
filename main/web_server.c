#include "web_server.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "config.h"

// External function from audio.c to apply volume changes
extern void resume_playback(void);

// External declarations for embedded web files
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t script_js_start[] asm("_binary_script_js_start");
extern const uint8_t script_js_end[] asm("_binary_script_js_end");

// Simple HTML for redirecting to captive portal
const char html_redirect[] = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta http-equiv=\"refresh\" content=\"0;URL='/'\">\n"
"</head>\n"
"<body>\n"
"    <p>Redirecting to captive portal...</p>\n"
"</body>\n"
"</html>\n";

// HTML for Apple Captive Network Assistant detection
const char html_apple_cna[] = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <title>Success</title>\n"
"</head>\n"
"<body>\n"
"    <h1>Success</h1>\n"
"</body>\n"
"</html>\n";
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "lwip/dns.h"
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#ifdef IS_USB
#include "usb/uac_host.h"
#endif

#define TAG "web_server"

// DNS server for captive portal
#define DNS_PORT 53
static esp_netif_t *s_dns_netif = NULL;
static int s_dns_sock = -1;
static httpd_handle_t s_httpd_handle = NULL;

// Maximum number of networks to return in scan
#define MAX_SCAN_RESULTS 20

// Forward declarations
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t scan_get_handler(httpd_req_t *req);
static esp_err_t status_get_handler(httpd_req_t *req);
static esp_err_t settings_get_handler(httpd_req_t *req);
static esp_err_t settings_post_handler(httpd_req_t *req);
static esp_err_t connect_post_handler(httpd_req_t *req);
static esp_err_t reset_post_handler(httpd_req_t *req);
static esp_err_t redirect_get_handler(httpd_req_t *req);
static esp_err_t apple_cna_get_handler(httpd_req_t *req);
static void dns_server_task(void *pvParameters);

/**
 * Start the DNS server for captive portal functionality
 */
static void start_dns_server(void)
{
    ESP_LOGI(TAG, "Starting DNS server for captive portal");
    
    struct sockaddr_in server_addr;
    
    // Create a UDP socket
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: %d", errno);
        return;
    }
    
    // Bind the socket to port 53 (DNS)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);
    
    if (bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket: %d", errno);
        close(s_dns_sock);
        s_dns_sock = -1;
        return;
    }
    
    // Start DNS server task
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

/**
 * Stop the DNS server
 */
static void stop_dns_server(void)
{
    ESP_LOGI(TAG, "Stopping DNS server");
    
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}

/**
 * Simple DNS server implementation for captive portal
 * It responds to all DNS queries with the AP's IP address (192.168.4.1)
 */
static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    ESP_LOGI(TAG, "DNS server task started");
    
    while (s_dns_sock >= 0) {
        // Wait for DNS query
        int len = recvfrom(s_dns_sock, rx_buffer, sizeof(rx_buffer), 0, 
                         (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (len < 0) {
            ESP_LOGE(TAG, "DNS recv error: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        if (len < 12) {
            // DNS header is at least 12 bytes
            continue;
        }
        
        // Log DNS request details (for debugging)
        ESP_LOGD(TAG, "Received DNS query of length %d", len);
        
        // Prepare DNS response
        // Copy the request header and modify it for response
        memcpy(tx_buffer, rx_buffer, len);
        
        // Set the response bit (QR)
        tx_buffer[2] |= 0x80;
        
        // Set authoritative answer bit (AA)
        tx_buffer[2] |= 0x04;
        
        // Clear the recursion available bit (RA)
        tx_buffer[3] |= 0x80;
        
        // Set the response code to 0 (no error) - clear the lower 4 bits of byte 3
        tx_buffer[3] &= 0xF0;
        
        // Get the number of queries
        uint16_t query_count = (rx_buffer[4] << 8) | rx_buffer[5];
        
        // Set answer count equal to query count
        tx_buffer[6] = rx_buffer[4];
        tx_buffer[7] = rx_buffer[5];
        
        // Set authority count and additional count to 0
        tx_buffer[8] = 0;
        tx_buffer[9] = 0;
        tx_buffer[10] = 0;
        tx_buffer[11] = 0;
        
        // Track position in the response buffer
        int response_len = len;
        
        // Find end of each query to add answers
        int query_pos = 12; // Start position of the queries section
        
        // Process all queries
        for (int i = 0; i < query_count && query_pos < len; i++) {
            // Find the end of the domain name
            int question_start = query_pos;
            
            // Print domain being queried for debugging
            char domain_name[256] = {0};
            int domain_pos = 0;
            int current_pos = query_pos;
            
            while (current_pos < len && rx_buffer[current_pos] != 0) {
                uint8_t label_len = rx_buffer[current_pos++];
                for (int j = 0; j < label_len && current_pos < len; j++) {
                    domain_name[domain_pos++] = rx_buffer[current_pos++];
                }
                if (rx_buffer[current_pos] != 0) {
                    domain_name[domain_pos++] = '.';
                }
            }
            domain_name[domain_pos] = '\0';
            ESP_LOGI(TAG, "DNS Query for domain: %s", domain_name);
            
            // Skip the domain name
            while (query_pos < len && rx_buffer[query_pos] != 0) {
                query_pos += rx_buffer[query_pos] + 1;
            }
            query_pos++; // Skip the terminating zero length
            
            // Skip QTYPE and QCLASS (4 bytes)
            uint16_t qtype = (rx_buffer[query_pos] << 8) | rx_buffer[query_pos + 1];
            query_pos += 4;
            
            // Only respond to A record queries
            if (qtype == 1) { // 1 = A record
                // Add answer section
                // NAME: pointer to the domain name in the question
                tx_buffer[response_len++] = 0xC0; // Compression pointer
                tx_buffer[response_len++] = 0x0C; // Pointer to position 12 (start of queries)
                
                // TYPE: A record (0x0001)
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x01;
                
                // CLASS: IN (0x0001)
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x01;
                
                // TTL: 300 seconds (5 minutes)
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x01;
                tx_buffer[response_len++] = 0x2C;
                
                // RDLENGTH: 4 bytes for IPv4 address
                tx_buffer[response_len++] = 0x00;
                tx_buffer[response_len++] = 0x04;
                
                // RDATA: IP address (192.168.4.1)
                tx_buffer[response_len++] = 192;
                tx_buffer[response_len++] = 168;
                tx_buffer[response_len++] = 4;
                tx_buffer[response_len++] = 1;
            }
        }
        
        // Send the response
        int sent = sendto(s_dns_sock, tx_buffer, response_len, 0, 
                        (struct sockaddr *)&client_addr, client_addr_len);
        
        if (sent < 0) {
            ESP_LOGE(TAG, "DNS send error: %d", errno);
        } else {
            ESP_LOGD(TAG, "Sent DNS response, length: %d", sent);
        }
    }
    
    ESP_LOGI(TAG, "DNS server task ended");
    vTaskDelete(NULL);
}

/**
 * Replace placeholders in HTML template with actual values
 */
static char* html_replace_placeholders(const char* template_html, size_t template_len)
{
    // Create a null-terminated string from the embedded HTML file
    char *original_html = malloc(template_len + 1);
    if (!original_html) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTML template");
        return NULL;
    }
    memcpy(original_html, template_html, template_len);
    original_html[template_len] = '\0';
    
    // Allocate buffer for the filled-in HTML
    char* html = malloc(template_len + 512); // Add extra space for replacements
    if (!html) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTML template");
        free(original_html);
        return NULL;
    }
    
    strcpy(html, original_html);
    free(original_html);
    
    // Replace device name
    const char* device_name = "ESP32 Scream Receiver";
    char* pos = strstr(html, "{{DEVICE_NAME}}");
    if (pos) {
        char* buffer = malloc(strlen(html) + strlen(device_name) + 1);
        if (buffer) {
            *pos = '\0';
            strcpy(buffer, html);
            strcat(buffer, device_name);
            strcat(buffer, pos + strlen("{{DEVICE_NAME}}"));
            free(html);
            html = buffer;
        }
    }
    
    // Replace current SSID
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = "Not configured";
    wifi_manager_get_current_ssid(ssid, sizeof(ssid));
    
    pos = strstr(html, "{{CURRENT_SSID}}");
    if (pos) {
        char* buffer = malloc(strlen(html) + strlen(ssid) + 1);
        if (buffer) {
            *pos = '\0';
            strcpy(buffer, html);
            strcat(buffer, ssid);
            strcat(buffer, pos + strlen("{{CURRENT_SSID}}"));
            free(html);
            html = buffer;
        }
    }
    
    // Replace connection status with status and IP if connected
    const char* status;
    char status_with_ip[64] = {0};
    
    wifi_manager_state_t state = wifi_manager_get_state();
    switch (state) {
        case WIFI_MANAGER_STATE_CONNECTED:
            // If connected, include the IP address in the status
            {
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    snprintf(status_with_ip, sizeof(status_with_ip), 
                            "Connected (IP: %s)", ip_str);
                    status = status_with_ip;
                } else {
                    status = "Connected";
                }
            }
            break;
        case WIFI_MANAGER_STATE_CONNECTING:
            status = "Connecting...";
            break;
        case WIFI_MANAGER_STATE_CONNECTION_FAILED:
            status = "Connection failed";
            break;
        case WIFI_MANAGER_STATE_AP_MODE:
            status = "Access Point Mode (192.168.4.1)";
            break;
        default:
            status = "Unknown";
            break;
    }
    
    pos = strstr(html, "{{CONNECTION_STATUS}}");
    if (pos) {
        char* buffer = malloc(strlen(html) + strlen(status) + 1);
        if (buffer) {
            *pos = '\0';
            strcpy(buffer, html);
            strcat(buffer, status);
            strcat(buffer, pos + strlen("{{CONNECTION_STATUS}}"));
            free(html);
            html = buffer;
        }
    }
    
    // Handle IS_SPDIF flag
#ifdef IS_SPDIF
    // Add IS_SPDIF_ENABLED flag to the template
    pos = strstr(html, "{{#IS_SPDIF}}");
    while (pos) {
        char *end_tag = strstr(pos, "{{/IS_SPDIF}}");
        if (end_tag) {
            // Replace the start tag with empty string (keep content)
            size_t start_tag_len = strlen("{{#IS_SPDIF}}");
            memmove(pos, pos + start_tag_len, strlen(pos + start_tag_len) + 1);
            
            // Find the end tag again (position changed after removing start tag)
            end_tag = strstr(pos, "{{/IS_SPDIF}}");
            if (end_tag) {
                // Replace the end tag with empty string
                memmove(end_tag, end_tag + strlen("{{/IS_SPDIF}}"), 
                        strlen(end_tag + strlen("{{/IS_SPDIF}}")) + 1);
            }
        }
        pos = strstr(html, "{{#IS_SPDIF}}");
    }
#else
    // Remove IS_SPDIF sections
    pos = strstr(html, "{{#IS_SPDIF}}");
    while (pos) {
        char *end_tag = strstr(pos, "{{/IS_SPDIF}}");
        if (end_tag) {
            // Remove the entire section including tags
            end_tag += strlen("{{/IS_SPDIF}}");
            memmove(pos, end_tag, strlen(end_tag) + 1);
        } else {
            // Incomplete tag, just remove the start tag
            size_t start_tag_len = strlen("{{#IS_SPDIF}}");
            memmove(pos, pos + start_tag_len, strlen(pos + start_tag_len) + 1);
        }
        pos = strstr(html, "{{#IS_SPDIF}}");
    }
#endif

    // Handle IS_USB flag
#ifdef IS_USB
    // Add IS_USB_ENABLED flag to the template
    pos = strstr(html, "{{#IS_USB}}");
    while (pos) {
        char *end_tag = strstr(pos, "{{/IS_USB}}");
        if (end_tag) {
            // Replace the start tag with empty string (keep content)
            size_t start_tag_len = strlen("{{#IS_USB}}");
            memmove(pos, pos + start_tag_len, strlen(pos + start_tag_len) + 1);
            
            // Find the end tag again (position changed after removing start tag)
            end_tag = strstr(pos, "{{/IS_USB}}");
            if (end_tag) {
                // Replace the end tag with empty string
                memmove(end_tag, end_tag + strlen("{{/IS_USB}}"), 
                        strlen(end_tag + strlen("{{/IS_USB}}")) + 1);
            }
        }
        pos = strstr(html, "{{#IS_USB}}");
    }
#else
    // Remove IS_USB sections
    pos = strstr(html, "{{#IS_USB}}");
    while (pos) {
        char *end_tag = strstr(pos, "{{/IS_USB}}");
        if (end_tag) {
            // Remove the entire section including tags
            end_tag += strlen("{{/IS_USB}}");
            memmove(pos, end_tag, strlen(end_tag) + 1);
        } else {
            // Incomplete tag, just remove the start tag
            size_t start_tag_len = strlen("{{#IS_USB}}");
            memmove(pos, pos + start_tag_len, strlen(pos + start_tag_len) + 1);
        }
        pos = strstr(html, "{{#IS_USB}}");
    }
#endif
    
    return html;
}

/**
 * GET handler for the root page (index.html)
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /");
    
    // Get the size of the embedded index.html file
    size_t index_html_size = index_html_end - index_html_start;
    
    // Fill in the HTML template with actual values
    char* html = html_replace_placeholders((const char*)index_html_start, index_html_size);
    if (!html) {
        return httpd_resp_send_500(req);
    }
    
    // Set content type
    httpd_resp_set_type(req, "text/html");
    
    // Send the response
    esp_err_t ret = httpd_resp_send(req, html, strlen(html));
    
    // Free the allocated memory
    free(html);
    
    return ret;
}

/**
 * GET handler for the CSS file (styles.css)
 */
static esp_err_t css_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /styles.css");
    
    // Set content type
    httpd_resp_set_type(req, "text/css");
    
    // Send the CSS file
    size_t css_size = styles_css_end - styles_css_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)styles_css_start, css_size);
    
    return ret;
}

/**
 * GET handler for the JavaScript file (script.js)
 */
static esp_err_t js_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /script.js");
    
    // Set content type
    httpd_resp_set_type(req, "application/javascript");
    
    // Send the JavaScript file
    size_t js_size = script_js_end - script_js_start;
    esp_err_t ret = httpd_resp_send(req, (const char*)script_js_start, js_size);
    
    return ret;
}

/**
 * GET handler for scanning available WiFi networks
 */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /scan");
    
    // Scan for networks
    wifi_network_info_t networks[MAX_SCAN_RESULTS];
    size_t networks_found = 0;
    
    esp_err_t ret = wifi_manager_scan_networks(networks, MAX_SCAN_RESULTS, &networks_found);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to scan networks");
        return ESP_FAIL;
    }
    
    // Create JSON response
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }
    
    // Deduplicate networks in-place - O(n²) but with no extra memory usage
    // Mark duplicates by setting their SSID to empty string
    for (size_t i = 0; i < networks_found; i++) {
        // Skip if this network is already marked as a duplicate or has an empty SSID
        if (strlen(networks[i].ssid) == 0) {
            continue;
        }
        
        // Look for duplicates of this network
        for (size_t j = i + 1; j < networks_found; j++) {
            if (strlen(networks[j].ssid) > 0 && strcmp(networks[i].ssid, networks[j].ssid) == 0) {
                // Same SSID - keep the one with stronger signal
                if (networks[j].rssi > networks[i].rssi) {
                    // j has stronger signal, copy to i and mark j as duplicate
                    networks[i].rssi = networks[j].rssi;
                    networks[i].authmode = networks[j].authmode;
                    networks[j].ssid[0] = '\0'; // Mark as duplicate
                } else {
                    // i has stronger signal, mark j as duplicate
                    networks[j].ssid[0] = '\0'; // Mark as duplicate
                }
            }
        }
    }
    
    // Add networks to JSON array (only the non-duplicates)
    for (size_t i = 0; i < networks_found; i++) {
        // Skip networks marked as duplicates (empty SSID)
        if (strlen(networks[i].ssid) == 0) {
            continue;
        }
        
        cJSON *network = cJSON_CreateObject();
        if (!network) {
            continue;
        }
        
        cJSON_AddStringToObject(network, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", networks[i].rssi);
        cJSON_AddNumberToObject(network, "auth", networks[i].authmode);
        
        cJSON_AddItemToArray(root, network);
    }
    
    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }
    
    // Set content type
    httpd_resp_set_type(req, "application/json");
    
    // Send the response
    ret = httpd_resp_send(req, json_str, strlen(json_str));
    
    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);
    
    return ret;
}

// External declarations for deep sleep functionality
extern void enter_deep_sleep_mode(void);
#ifdef IS_USB
extern uac_host_device_handle_t s_spk_dev_handle; // DAC handle from usb_audio_player_main.c
#endif

#ifdef IS_SPDIF
// Add external declaration for SPDIF functions
extern esp_err_t spdif_set_sample_rates(int rate);
#endif

/**
 * POST handler for connecting to a WiFi network
 */
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /connect");
    
    // Get content length
    size_t content_len = req->content_len;
    if (content_len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }
    
    // Read the data
    char buf[512];
    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[content_len] = '\0';
    
    // Parse the form data
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LENGTH + 1] = {0};
    
    char *param = strstr(buf, "ssid=");
    if (param) {
        param += 5; // Skip "ssid="
        char *end = strchr(param, '&');
        if (end) {
            *end = '\0';
        }
        
        // URL decode
        char *decoded = malloc(strlen(param) + 1);
        if (!decoded) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        // Simple URL decoding
        char *src = param;
        char *dst = decoded;
        while (*src) {
            if (*src == '+') {
                *dst = ' ';
            } else if (*src == '%' && src[1] && src[2]) {
                // Convert hex %xx to character
                char hex[3] = {src[1], src[2], 0};
                *dst = (char)strtol(hex, NULL, 16);
                src += 2;
            } else {
                *dst = *src;
            }
            src++;
            dst++;
        }
        *dst = '\0';
        
        strncpy(ssid, decoded, WIFI_SSID_MAX_LENGTH);
        free(decoded);
        
        if (end) {
            *end = '&';
        }
    }
    
    param = strstr(buf, "password=");
    if (param) {
        param += 9; // Skip "password="
        char *end = strchr(param, '&');
        if (end) {
            *end = '\0';
        }
        
        // URL decode
        char *decoded = malloc(strlen(param) + 1);
        if (!decoded) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_FAIL;
        }
        
        // Simple URL decoding
        char *src = param;
        char *dst = decoded;
        while (*src) {
            if (*src == '+') {
                *dst = ' ';
            } else if (*src == '%' && src[1] && src[2]) {
                // Convert hex %xx to character
                char hex[3] = {src[1], src[2], 0};
                *dst = (char)strtol(hex, NULL, 16);
                src += 2;
            } else {
                *dst = *src;
            }
            src++;
            dst++;
        }
        *dst = '\0';
        
        strncpy(password, decoded, WIFI_PASSWORD_MAX_LENGTH);
        free(decoded);
    }
    
    // Check if SSID is provided
    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    
    // Check if this is the first time an SSID is being saved
    bool first_time_config = !wifi_manager_has_credentials();
    
    // Save the credentials to NVS and try to connect
    wifi_manager_connect(ssid, password);
    
    // Respond with success even if connection failed
    // The client will be redirected when the device restarts or changes mode
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    
    // If this is the first time setting up WiFi and no DAC is connected,
    // put the device into deep sleep mode after a short delay
    if (first_time_config) {
        ESP_LOGI(TAG, "First-time WiFi configuration detected");
        
#ifdef IS_USB
        // Check if a DAC is connected (s_spk_dev_handle is NULL when no DAC is connected)
        if (s_spk_dev_handle == NULL) {
            ESP_LOGI(TAG, "No DAC connected after initial WiFi setup, preparing for deep sleep");
            // Give a small delay for web request to complete and logs to be sent
            vTaskDelay(pdMS_TO_TICKS(2000));
            // Go to deep sleep
            enter_deep_sleep_mode();
        } else {
            ESP_LOGI(TAG, "DAC is connected, staying awake after WiFi setup");
        }
#else
        // If USB support is not enabled, we don't check for DAC
        ESP_LOGI(TAG, "USB support not enabled, staying awake after WiFi setup");
#endif
    }
    
    return ESP_OK;
}

/**
 * POST handler for resetting WiFi configuration
 */
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /reset");
    
    // Clear the stored credentials
    esp_err_t err = wifi_manager_clear_credentials();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset WiFi configuration");
        return ESP_FAIL;
    }
    
    // Reset app configuration to defaults
    err = config_manager_reset();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reset app configuration: %s", esp_err_to_name(err));
        // Continue anyway, WiFi reset is more important
    } else {
        ESP_LOGI(TAG, "App configuration reset to defaults");
    }
    
    // Start AP mode
    wifi_manager_stop();
    wifi_manager_start();
    
    // Respond with success
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    
    return ESP_OK;
}

/**
 * GET handler for all other URIs (redirect to captive portal)
 */
static esp_err_t redirect_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for URI: %s", req->uri);
    
    // Special case for Apple captive portal detection (handled by a separate handler)
    if (strcmp(req->uri, "/hotspot-detect.html") == 0) {
        return apple_cna_get_handler(req);
    }
    
    // Handle favicon.ico requests silently (browsers often request this)
    if (strcmp(req->uri, "/favicon.ico") == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }
    
    // For all other URIs, redirect to the captive portal
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_redirect, strlen(html_redirect));
    
    return ESP_OK;
}

/**
 * GET handler for connection status
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /status");
    
    // Get connection status
    wifi_manager_state_t state = wifi_manager_get_state();
    
    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }
    
    // Add status string
    const char *status;
    switch (state) {
        case WIFI_MANAGER_STATE_CONNECTED:
            status = "Connected";
            break;
        case WIFI_MANAGER_STATE_CONNECTING:
            status = "Connecting...";
            break;
        case WIFI_MANAGER_STATE_CONNECTION_FAILED:
            status = "Connection failed";
            break;
        case WIFI_MANAGER_STATE_AP_MODE:
            status = "Access Point Mode";
            break;
        default:
            status = "Unknown";
            break;
    }
    cJSON_AddStringToObject(root, "status", status);
    
    // If connected, add the IP address
    if (state == WIFI_MANAGER_STATE_CONNECTED) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ip_str);
        }
    }
    
    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }
    
    // Set content type
    httpd_resp_set_type(req, "application/json");
    
    // Send the response
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    
    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);
    
    return ret;
}

/**
 * GET handler for device settings
 */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /api/settings");
    
    // Get the current configuration
    app_config_t *config = config_manager_get_config();
    
    // Create JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON response");
        return ESP_FAIL;
    }
    
    // Network settings
    cJSON_AddNumberToObject(root, "port", config->port);
    cJSON_AddStringToObject(root, "ap_ssid", config->ap_ssid);
    cJSON_AddStringToObject(root, "ap_password", config->ap_password);
    cJSON_AddBoolToObject(root, "hide_ap_when_connected", config->hide_ap_when_connected);
    
    // Buffer settings
    cJSON_AddNumberToObject(root, "initial_buffer_size", config->initial_buffer_size);
    cJSON_AddNumberToObject(root, "buffer_grow_step_size", config->buffer_grow_step_size);
    cJSON_AddNumberToObject(root, "max_buffer_size", config->max_buffer_size);
    cJSON_AddNumberToObject(root, "max_grow_size", config->max_grow_size);
    
    // Audio settings
    cJSON_AddNumberToObject(root, "sample_rate", config->sample_rate);
    cJSON_AddNumberToObject(root, "bit_depth", config->bit_depth);
    cJSON_AddNumberToObject(root, "volume", config->volume);
    
    // SPDIF settings (only relevant when IS_SPDIF is defined)
#ifdef IS_SPDIF
    cJSON_AddNumberToObject(root, "spdif_data_pin", config->spdif_data_pin);
#endif
    
    // USB Scream Sender settings
    cJSON_AddBoolToObject(root, "enable_usb_sender", config->enable_usb_sender);
    cJSON_AddStringToObject(root, "sender_destination_ip", config->sender_destination_ip);
    cJSON_AddNumberToObject(root, "sender_destination_port", config->sender_destination_port);
    
    // Sleep settings
    cJSON_AddNumberToObject(root, "silence_threshold_ms", config->silence_threshold_ms);
    cJSON_AddNumberToObject(root, "network_check_interval_ms", config->network_check_interval_ms);
    cJSON_AddNumberToObject(root, "activity_threshold_packets", config->activity_threshold_packets);
    cJSON_AddNumberToObject(root, "silence_amplitude_threshold", config->silence_amplitude_threshold);
    cJSON_AddNumberToObject(root, "network_inactivity_timeout_ms", config->network_inactivity_timeout_ms);
    
    // Convert JSON to string
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON string");
        return ESP_FAIL;
    }
    
    // Set content type
    httpd_resp_set_type(req, "application/json");
    
    // Send the response
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    
    // Free allocated memory
    free(json_str);
    cJSON_Delete(root);
    
    return ret;
}

/**
 * POST handler for updating device settings
 */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling POST request for /api/settings");
    
    // Get content length
    size_t content_len = req->content_len;
    if (content_len >= 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }
    
    // Read the data
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, buf, content_len);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[content_len] = '\0';
    
    // Parse the JSON data
    cJSON *root = cJSON_Parse(buf);
    free(buf); // We don't need the raw data anymore
    
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // Get the current configuration
    app_config_t *config = config_manager_get_config();
    
    // Update configuration with new values if present
    cJSON *port = cJSON_GetObjectItem(root, "port");
    if (port && cJSON_IsNumber(port)) {
        config->port = (uint16_t)port->valueint;
    }
    
    // WiFi AP SSID
    cJSON *ap_ssid = cJSON_GetObjectItem(root, "ap_ssid");
    if (ap_ssid && cJSON_IsString(ap_ssid)) {
        strncpy(config->ap_ssid, ap_ssid->valuestring, WIFI_SSID_MAX_LENGTH);
        config->ap_ssid[WIFI_SSID_MAX_LENGTH] = '\0'; // Ensure null-termination
    }
    
    // WiFi AP password
    cJSON *ap_password = cJSON_GetObjectItem(root, "ap_password");
    if (ap_password && cJSON_IsString(ap_password)) {
        strncpy(config->ap_password, ap_password->valuestring, WIFI_PASSWORD_MAX_LENGTH);
        config->ap_password[WIFI_PASSWORD_MAX_LENGTH] = '\0'; // Ensure null-termination
    }
    
    // Hide AP when connected setting
    cJSON *hide_ap_when_connected = cJSON_GetObjectItem(root, "hide_ap_when_connected");
    if (hide_ap_when_connected && cJSON_IsBool(hide_ap_when_connected)) {
        bool old_value = config->hide_ap_when_connected;
        config->hide_ap_when_connected = cJSON_IsTrue(hide_ap_when_connected);
        
        // If the setting changed and we're connected, apply the change immediately
        if (old_value != config->hide_ap_when_connected && 
            wifi_manager_get_state() == WIFI_MANAGER_STATE_CONNECTED) {
            ESP_LOGI(TAG, "AP visibility setting changed, updating WiFi mode");
            if (config->hide_ap_when_connected) {
                ESP_LOGI(TAG, "Hiding AP interface");
                esp_wifi_set_mode(WIFI_MODE_STA);
            } else {
                ESP_LOGI(TAG, "Showing AP interface");
                esp_wifi_set_mode(WIFI_MODE_APSTA);
            }
        }
    }
    
    // If we're in AP mode, we need to restart the AP with the new password
    if (ap_password && cJSON_IsString(ap_password) && 
        wifi_manager_get_state() == WIFI_MANAGER_STATE_AP_MODE) {
        ESP_LOGI(TAG, "AP password changed, restarting wifi manager");
        wifi_manager_stop();
        wifi_manager_start();
    }
    
    // Buffer settings
    cJSON *initial_buffer_size = cJSON_GetObjectItem(root, "initial_buffer_size");
    if (initial_buffer_size && cJSON_IsNumber(initial_buffer_size)) {
        config->initial_buffer_size = (uint8_t)initial_buffer_size->valueint;
    }
    
    cJSON *buffer_grow_step_size = cJSON_GetObjectItem(root, "buffer_grow_step_size");
    if (buffer_grow_step_size && cJSON_IsNumber(buffer_grow_step_size)) {
        config->buffer_grow_step_size = (uint8_t)buffer_grow_step_size->valueint;
    }
    
    cJSON *max_buffer_size = cJSON_GetObjectItem(root, "max_buffer_size");
    if (max_buffer_size && cJSON_IsNumber(max_buffer_size)) {
        config->max_buffer_size = (uint8_t)max_buffer_size->valueint;
    }
    
    cJSON *max_grow_size = cJSON_GetObjectItem(root, "max_grow_size");
    if (max_grow_size && cJSON_IsNumber(max_grow_size)) {
        config->max_grow_size = (uint8_t)max_grow_size->valueint;
    }
    
    // Audio settings
    bool sample_rate_changed = false;
    uint32_t old_sample_rate = config->sample_rate;
    
    cJSON *sample_rate = cJSON_GetObjectItem(root, "sample_rate");
    if (sample_rate && cJSON_IsNumber(sample_rate)) {
        uint32_t new_rate = (uint32_t)sample_rate->valueint;
        if (new_rate != config->sample_rate) {
            config->sample_rate = new_rate;
            sample_rate_changed = true;
            ESP_LOGI(TAG, "Sample rate changed from %" PRIu32 " to %" PRIu32, old_sample_rate, new_rate);
        }
    }
    
    cJSON *bit_depth = cJSON_GetObjectItem(root, "bit_depth");
    if (bit_depth && cJSON_IsNumber(bit_depth)) {
        config->bit_depth = 16;
    }
    
    // Track if volume was changed
    bool volume_changed = false;
    float old_volume = config->volume;
    
    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (volume && cJSON_IsNumber(volume)) {
        config->volume = (float)volume->valuedouble;
        volume_changed = (old_volume != config->volume);
    }
    
    // Sleep settings
    cJSON *silence_threshold_ms = cJSON_GetObjectItem(root, "silence_threshold_ms");
    if (silence_threshold_ms && cJSON_IsNumber(silence_threshold_ms)) {
        config->silence_threshold_ms = (uint32_t)silence_threshold_ms->valueint;
    }
    
    cJSON *network_check_interval_ms = cJSON_GetObjectItem(root, "network_check_interval_ms");
    if (network_check_interval_ms && cJSON_IsNumber(network_check_interval_ms)) {
        config->network_check_interval_ms = (uint32_t)network_check_interval_ms->valueint;
    }
    
    cJSON *activity_threshold_packets = cJSON_GetObjectItem(root, "activity_threshold_packets");
    if (activity_threshold_packets && cJSON_IsNumber(activity_threshold_packets)) {
        config->activity_threshold_packets = (uint8_t)activity_threshold_packets->valueint;
    }
    
    cJSON *silence_amplitude_threshold = cJSON_GetObjectItem(root, "silence_amplitude_threshold");
    if (silence_amplitude_threshold && cJSON_IsNumber(silence_amplitude_threshold)) {
        config->silence_amplitude_threshold = (uint16_t)silence_amplitude_threshold->valueint;
    }
    
    cJSON *network_inactivity_timeout_ms = cJSON_GetObjectItem(root, "network_inactivity_timeout_ms");
    if (network_inactivity_timeout_ms && cJSON_IsNumber(network_inactivity_timeout_ms)) {
        config->network_inactivity_timeout_ms = (uint32_t)network_inactivity_timeout_ms->valueint;
    }
    
    // USB Sender settings
    cJSON *enable_usb_sender = cJSON_GetObjectItem(root, "enable_usb_sender");
    if (enable_usb_sender) {
        if (cJSON_IsBool(enable_usb_sender)) {
            config->enable_usb_sender = cJSON_IsTrue(enable_usb_sender);
            ESP_LOGI(TAG, "Updating USB sender enabled to: %d", config->enable_usb_sender);
        }
    }
    
    cJSON *sender_destination_ip = cJSON_GetObjectItem(root, "sender_destination_ip");
    if (sender_destination_ip && cJSON_IsString(sender_destination_ip)) {
        strncpy(config->sender_destination_ip, sender_destination_ip->valuestring, 15);
        config->sender_destination_ip[15] = '\0'; // Ensure null termination
        ESP_LOGI(TAG, "Updating sender destination IP to: %s", config->sender_destination_ip);
    }
    
    cJSON *sender_destination_port = cJSON_GetObjectItem(root, "sender_destination_port");
    if (sender_destination_port && cJSON_IsNumber(sender_destination_port)) {
        config->sender_destination_port = (uint16_t)sender_destination_port->valueint;
        ESP_LOGI(TAG, "Updating sender destination port to: %d", config->sender_destination_port);
    }
    
    // SPDIF settings
#ifdef IS_SPDIF
    bool spdif_pin_changed = false;
    uint8_t old_spdif_pin = config->spdif_data_pin;
    
    cJSON *spdif_data_pin = cJSON_GetObjectItem(root, "spdif_data_pin");
    if (spdif_data_pin && cJSON_IsNumber(spdif_data_pin)) {
        // Limit the pin number to valid GPIO range (0-39 for ESP32)
        uint8_t pin = (uint8_t)spdif_data_pin->valueint;
        if (pin <= 39) {
            if (pin != config->spdif_data_pin) {
                spdif_pin_changed = true;
                config->spdif_data_pin = pin;
                ESP_LOGI(TAG, "SPDIF pin changed from %d to %d", old_spdif_pin, pin);
            }
        }
    }
#endif
    
    // Free the JSON object
    cJSON_Delete(root);
    
    // Save the configuration to NVS
    esp_err_t err = config_manager_save_config();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
        return ESP_FAIL;
    }
    
    // Apply SPDIF changes if needed
#ifdef IS_SPDIF
    if (spdif_pin_changed || sample_rate_changed) {
        ESP_LOGI(TAG, "SPDIF configuration changed, reinitializing SPDIF with pin %d and sample rate %" PRIu32,
                config->spdif_data_pin, config->sample_rate);
        
        // Reinitialize SPDIF with the new settings
        esp_err_t spdif_err = spdif_set_sample_rates(config->sample_rate);
        if (spdif_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize SPDIF: %s", esp_err_to_name(spdif_err));
            // Continue anyway - the user can retry or use different settings later
        } else {
            ESP_LOGI(TAG, "Successfully reinitialized SPDIF");
        }
    }
#endif
    
    // Apply volume changes immediately if volume was changed
    if (volume_changed) {
        ESP_LOGI(TAG, "Volume changed, applying immediately");
        resume_playback();
    }
    
    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Settings saved successfully\"}");
    
    return ESP_OK;
}

/**
 * GET handler for Apple Captive Network Assistant detection
 */
static esp_err_t apple_cna_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for Apple CNA");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_apple_cna, strlen(html_apple_cna));
    
    return ESP_OK;
}

/**
 * Start the web server
 */
esp_err_t web_server_start(void)
{
    ESP_LOGI(TAG, "Starting web server");
    
    // If server is already running, stop it first
    if (s_httpd_handle != NULL) {
        ESP_LOGI(TAG, "Web server already running, stopping first");
        web_server_stop();
    }
    
    // Configure the HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 15;
    
    // Increase buffer size for requests
    config.recv_wait_timeout = 30;      // Longer timeout for processing requests
    config.send_wait_timeout = 30;      // Longer timeout for sending responses
    
    // Bind the server to ANY address instead of just the AP interface
    // This allows it to be accessible from both AP and STA interfaces
    config.server_port = 80;
    config.ctrl_port = 32768;
    
    // You might need to modify sdkconfig to increase HTTPD_MAX_REQ_HDR_LEN and HTTPD_MAX_URI_LEN
    
    // Start the HTTP server
    esp_err_t ret = httpd_start(&s_httpd_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Define URI handlers
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t scan = {
        .uri       = "/scan",
        .method    = HTTP_GET,
        .handler   = scan_get_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t connect = {
        .uri       = "/connect",
        .method    = HTTP_POST,
        .handler   = connect_post_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t reset = {
        .uri       = "/reset",
        .method    = HTTP_POST,
        .handler   = reset_post_handler,
        .user_ctx  = NULL
    };
    
    // Special handlers for captive portal functionality
    httpd_uri_t redirect = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = redirect_get_handler,
        .user_ctx  = NULL
    };
    
    // Handle Apple captive portal detection
    httpd_uri_t apple_cna = {
        .uri       = "/hotspot-detect.html",
        .method    = HTTP_GET,
        .handler   = apple_cna_get_handler,
        .user_ctx  = NULL
    };
    
    // Register URI handlers
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &root));
    
    // Add handlers for CSS and JS files
    httpd_uri_t css = {
        .uri       = "/styles.css",
        .method    = HTTP_GET,
        .handler   = css_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &css));
    
    httpd_uri_t js = {
        .uri       = "/script.js",
        .method    = HTTP_GET,
        .handler   = js_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &js));
    
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &scan));
    
    // Add status endpoint
    httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &status));
    
    // Add settings endpoints
    httpd_uri_t settings_get = {
        .uri       = "/api/settings",
        .method    = HTTP_GET,
        .handler   = settings_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &settings_get));
    
    httpd_uri_t settings_post = {
        .uri       = "/api/settings",
        .method    = HTTP_POST,
        .handler   = settings_post_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &settings_post));
    
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &connect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &reset));
    
    // Register captive portal handlers
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &apple_cna));
    
    // Register catch-all handler (must be registered last)
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &redirect));
    
    // Only start DNS server for captive portal if in AP mode
    if (wifi_manager_get_state() == WIFI_MANAGER_STATE_AP_MODE) {
        start_dns_server();
    }
    
    ESP_LOGI(TAG, "Web server started successfully");
    return ESP_OK;
}

/**
 * Stop the web server
 */
esp_err_t web_server_stop(void)
{
    ESP_LOGI(TAG, "Stopping web server");
    
    // Stop DNS server
    stop_dns_server();
    
    if (s_httpd_handle == NULL) {
        ESP_LOGW(TAG, "Web server not running");
        return ESP_OK;
    }
    
    // Stop HTTP server
    esp_err_t ret = httpd_stop(s_httpd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_httpd_handle = NULL;
    return ESP_OK;
}

/**
 * Check if the web server is running
 */
bool web_server_is_running(void)
{
    return s_httpd_handle != NULL;
}
