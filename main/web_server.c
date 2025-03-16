#include "web_server.h"
#include "wifi_manager.h"
#include "html_content.h"
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

static const char *TAG = "web_server";

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
    char rx_buffer[512];
    char tx_buffer[512];
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
        
        // Prepare DNS response
        // Copy the request header and modify it for response
        memcpy(tx_buffer, rx_buffer, len);
        
        // Set the response bit (QR)
        tx_buffer[2] |= 0x80;
        
        // Set authoritative answer bit
        tx_buffer[2] |= 0x04;
        
        // Set the response code to 0 (no error)
        tx_buffer[3] &= 0xF0;
        
        // Count the number of queries
        uint16_t query_count = (rx_buffer[4] << 8) | rx_buffer[5];
        
        // Set answer count to query count
        tx_buffer[6] = rx_buffer[4];
        tx_buffer[7] = rx_buffer[5];
        
        // Set authority count and additional count to 0
        tx_buffer[8] = 0;
        tx_buffer[9] = 0;
        tx_buffer[10] = 0;
        tx_buffer[11] = 0;
        
        // Skip the header (12 bytes) to process queries
        int response_len = len;
        int query_end = 12; // Start of queries
        
        // Process all queries
        for (int i = 0; i < query_count && query_end < len; i++) {
            // Skip the query name
            while (query_end < len && rx_buffer[query_end] != 0) {
                query_end += rx_buffer[query_end] + 1;
            }
            query_end += 5; // Skip null byte and query type/class
            
            // Add answer section for this query
            // Pointer to the query name
            tx_buffer[response_len++] = 0xC0;
            tx_buffer[response_len++] = 0x0C;
            
            // Type: A record (0x0001)
            tx_buffer[response_len++] = 0x00;
            tx_buffer[response_len++] = 0x01;
            
            // Class: IN (0x0001)
            tx_buffer[response_len++] = 0x00;
            tx_buffer[response_len++] = 0x01;
            
            // TTL: 60 seconds
            tx_buffer[response_len++] = 0x00;
            tx_buffer[response_len++] = 0x00;
            tx_buffer[response_len++] = 0x00;
            tx_buffer[response_len++] = 0x3C;
            
            // Data length: 4 bytes (IP address)
            tx_buffer[response_len++] = 0x00;
            tx_buffer[response_len++] = 0x04;
            
            // IP address: 192.168.4.1 (AP's IP)
            tx_buffer[response_len++] = 192;
            tx_buffer[response_len++] = 168;
            tx_buffer[response_len++] = 4;
            tx_buffer[response_len++] = 1;
        }
        
        // Send the response
        int sent = sendto(s_dns_sock, tx_buffer, response_len, 0, 
                        (struct sockaddr *)&client_addr, client_addr_len);
        
        if (sent < 0) {
            ESP_LOGE(TAG, "DNS send error: %d", errno);
        }
    }
    
    ESP_LOGI(TAG, "DNS server task ended");
    vTaskDelete(NULL);
}

/**
 * Replace placeholders in HTML template with actual values
 */
static char* html_replace_placeholders(const char* template_html)
{
    // Allocate buffer for the filled-in HTML
    char* html = malloc(strlen(template_html) + 512); // Add extra space for replacements
    if (!html) {
        ESP_LOGE(TAG, "Failed to allocate memory for HTML template");
        return NULL;
    }
    
    strcpy(html, template_html);
    
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
    
    // Replace connection status
    const char* status;
    switch (wifi_manager_get_state()) {
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
    
    return html;
}

/**
 * GET handler for the root page
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling GET request for /");
    
    // Fill in the HTML template with actual values
    char* html = html_replace_placeholders(html_config_page);
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
    
    // Add networks to JSON array
    for (size_t i = 0; i < networks_found; i++) {
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
    
    // Save the credentials to NVS and try to connect
    wifi_manager_connect(ssid, password);
    
    // Respond with success even if connection failed
    // The client will be redirected when the device restarts or changes mode
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    
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
    config.max_uri_handlers = 10;
    
    // Increase buffer size for requests
    config.recv_wait_timeout = 30;      // Longer timeout for processing requests
    config.send_wait_timeout = 30;      // Longer timeout for sending responses
    
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &scan));
    
    // Add status endpoint
    httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &status));
    
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &connect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &reset));
    
    // Register captive portal handlers
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &apple_cna));
    
    // Register catch-all handler (must be registered last)
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd_handle, &redirect));
    
    // Start DNS server for captive portal
    start_dns_server();
    
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
