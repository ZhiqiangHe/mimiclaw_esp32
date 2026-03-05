#include "ws_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "sys/socket.h"
#include "netdb.h"
#include "arpa/inet.h"
#include "unistd.h"
#include "errno.h"

static const char *TAG = "ws_client";

typedef struct {
    char host[128];
    int port;
    char path[128];
} ws_url_info_t;

static int s_sock = -1;
static ws_event_handler_t s_event_handler = NULL;
static void *s_user_data = NULL;
static bool s_connected = false;
static TaskHandle_t s_task_handle = NULL;
static bool s_running = true;

static char s_recv_buf[1024];
static char s_payload[1024];

static void parse_ws_url(const char *url, ws_url_info_t *info)
{
    memset(info, 0, sizeof(ws_url_info_t));
    
    const char *host_start = NULL;
    if (strncmp(url, "ws://", 5) == 0) {
        host_start = url + 5;
    } else if (strncmp(url, "wss://", 6) == 0) {
        host_start = url + 6;
    } else {
        host_start = url;
    }
    
    const char *path_start = strchr(host_start, '/');
    const char *port_start = strchr(host_start, ':');
    
    if (port_start && (!path_start || port_start < path_start)) {
        int host_len = port_start - host_start;
        if (host_len > 127) host_len = 127;
        strncpy(info->host, host_start, host_len);
        info->host[host_len] = '\0';
        sscanf(port_start + 1, "%d", &info->port);
    } else {
        int host_len = path_start ? (path_start - host_start) : strlen(host_start);
        if (host_len > 127) host_len = 127;
        strncpy(info->host, host_start, host_len);
        info->host[host_len] = '\0';
        info->port = (strncmp(url, "wss://", 6) == 0) ? 443 : 80;
    }
    
    if (path_start) {
        strncpy(info->path, path_start, sizeof(info->path) - 1);
    } else {
        strcpy(info->path, "/");
    }
    
    ESP_LOGI(TAG, "Parsed URL: host=%s, port=%d, path=%s", info->host, info->port, info->path);
}

static void generate_websocket_key(char *key_out)
{
    const char *seed = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    char buf[16];
    for (int i = 0; i < 16; i++) {
        buf[i] = seed[esp_random() % strlen(seed)];
    }
    const char *base64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    for (int i = 0; i < 16; i += 3) {
        int v = (buf[i] << 16) | (i+1 < 16 ? buf[i+1] << 8 : 0) | (i+2 < 16 ? buf[i+2] : 0);
        key_out[j++] = base64_table[(v >> 18) & 0x3F];
        key_out[j++] = base64_table[(v >> 12) & 0x3F];
        key_out[j++] = (i+1 < 16) ? base64_table[(v >> 6) & 0x3F] : '=';
        key_out[j++] = (i+2 < 16) ? base64_table[v & 0x3F] : '=';
    }
    key_out[j] = '\0';
}

static int send_all(int sock, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n < 0) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int recv_timeout(int sock, char *buf, int len, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    int n = recv(sock, buf, len, 0);
    return n;
}

static bool websocket_handshake(int sock, const char *host, const char *path)
{
    char ws_key[32];
    generate_websocket_key(ws_key);
    
    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "\r\n",
        path, host, ws_key);
    
    ESP_LOGI(TAG, "Sending WebSocket handshake...");
    ESP_LOGD(TAG, "Request:\n%s", request);
    
    if (send_all(sock, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Failed to send handshake");
        return false;
    }
    
    char response[512];
    int recv_len = recv_timeout(sock, response, sizeof(response) - 1, 5000);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "Failed to receive handshake response");
        return false;
    }
    response[recv_len] = '\0';
    
    ESP_LOGD(TAG, "Response:\n%s", response);
    
    if (strstr(response, "101 Switching Protocols") != NULL) {
        ESP_LOGI(TAG, "WebSocket handshake successful!");
        return true;
    }
    
    ESP_LOGE(TAG, "WebSocket handshake failed");
    return false;
}

static uint8_t ws_parse_frame(const char *data, size_t len, char *payload_out, size_t *payload_len_out)
{
    if (len < 2) return 0;
    
    uint8_t first_byte = (uint8_t)data[0];
    uint8_t second_byte = (uint8_t)data[1];
    
    uint8_t opcode = first_byte & 0x0F;
    bool masked = (second_byte & 0x80) != 0;
    uint64_t payload_len = second_byte & 0x7F;
    
    size_t header_len = 2;
    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = ((uint8_t)data[2] << 8) | (uint8_t)data[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (len < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | (uint8_t)data[2 + i];
        }
        header_len = 10;
    }
    
    size_t mask_len = masked ? 4 : 0;
    if (len < header_len + mask_len + payload_len) {
        return 0;
    }
    
    const char *payload_data = data + header_len + mask_len;
    
    if (masked) {
        const uint8_t *mask = (const uint8_t *)(data + header_len);
        for (size_t i = 0; i < payload_len && i < 1024; i++) {
            payload_out[i] = payload_data[i] ^ mask[i % 4];
        }
        *payload_len_out = payload_len;
    } else {
        if (payload_len > 1024) {
            payload_len = 1024;
        }
        memcpy(payload_out, payload_data, payload_len);
        *payload_len_out = payload_len;
    }
    
    return opcode;
}

static void ws_client_task(void *arg)
{
    ws_url_info_t *url_info = (ws_url_info_t *)arg;
    
    ESP_LOGI(TAG, "WebSocket client task started, connecting to %s:%d%s", 
             url_info->host, url_info->port, url_info->path);
    
    while (s_running) {
        struct hostent *he = gethostbyname(url_info->host);
        if (!he) {
            ESP_LOGE(TAG, "Failed to resolve host: %s", url_info->host);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        s_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (s_sock < 0) {
            ESP_LOGE(TAG, "Failed to create socket");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(url_info->port);
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
        
        if (connect(s_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            ESP_LOGE(TAG, "Failed to connect to %s:%d", url_info->host, url_info->port);
            close(s_sock);
            s_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        ESP_LOGI(TAG, "TCP connected to %s:%d", url_info->host, url_info->port);
        
        if (!websocket_handshake(s_sock, url_info->host, url_info->path)) {
            close(s_sock);
            s_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        s_connected = true;
        if (s_event_handler) {
            s_event_handler(WS_EVENT_CONNECTED, NULL, 0, s_user_data);
        }
        
        int64_t last_ping_time = esp_timer_get_time() / 1000;
        
        while (s_running && s_connected) {
            int recv_len = recv_timeout(s_sock, s_recv_buf, sizeof(s_recv_buf) - 1, 1000);
            
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_ping_time > 25000) {
                ESP_LOGI(TAG, "Sending JSON ping...");
                char ping_json[64];
                int len = snprintf(ping_json, sizeof(ping_json), 
                    "{\"type\":\"ping\",\"timestamp\":%lld}", (long long)now);
                ws_client_send(ping_json, len);
                last_ping_time = now;
            }
            
            if (recv_len > 0) {
                size_t payload_len = 0;
                uint8_t opcode = ws_parse_frame(s_recv_buf, recv_len, s_payload, &payload_len);
                
                if (opcode == 0x08) {
                    ESP_LOGI(TAG, "WebSocket close frame received");
                    break;
                } else if (opcode == 0x01 || opcode == 0x00) {
                    s_payload[payload_len] = '\0';
                    ESP_LOGD(TAG, "WebSocket text frame: %.*s", payload_len, s_payload);
                    if (s_event_handler) {
                        s_event_handler(WS_EVENT_DATA, s_payload, payload_len, s_user_data);
                    }
                } else if (opcode == 0x09) {
                    ESP_LOGD(TAG, "WebSocket ping frame, sending pong");
                    char pong[2] = {0x8A, 0x00};
                    send(s_sock, pong, 2, 0);
                }
            } else if (recv_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "Socket error: %d", errno);
                break;
            }
        }
        
        s_connected = false;
        if (s_event_handler) {
            s_event_handler(WS_EVENT_DISCONNECTED, NULL, 0, s_user_data);
        }
        
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        
        ESP_LOGI(TAG, "Disconnected, will reconnect in 5 seconds...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    free(url_info);
    vTaskDelete(NULL);
}

esp_err_t ws_client_init(const char *uri, ws_event_handler_t handler, void *user_data)
{
    if (s_task_handle) {
        ESP_LOGW(TAG, "WebSocket client already initialized");
        return ESP_OK;
    }

    s_event_handler = handler;
    s_user_data = user_data;
    s_connected = false;
    s_running = true;

    ESP_LOGI(TAG, "Initializing WebSocket client with URI: %s", uri);

    ws_url_info_t *url_info = malloc(sizeof(ws_url_info_t));
    if (!url_info) {
        return ESP_ERR_NO_MEM;
    }
    parse_ws_url(uri, url_info);

    BaseType_t ret = xTaskCreatePinnedToCore(
        ws_client_task, "ws_client",
        4096, url_info,
        5, &s_task_handle, 1);

    if (ret != pdPASS) {
        free(url_info);
        ESP_LOGE(TAG, "Failed to create WebSocket task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebSocket client initialized successfully");
    return ESP_OK;
}

esp_err_t ws_client_connect(void)
{
    if (!s_task_handle) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ws_client_send(const char *data, size_t len)
{
    if (!s_connected || s_sock < 0) {
        ESP_LOGE(TAG, "WebSocket client not connected");
        return ESP_FAIL;
    }

    if (len > 1400) {
        ESP_LOGE(TAG, "Data too large for WebSocket frame");
        return ESP_FAIL;
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    
    if (len > 125) {
        uint8_t frame[1400];
        frame[0] = 0x81;
        frame[1] = 126 | 0x80;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        
        memcpy(frame + 4, mask, 4);
        for (size_t i = 0; i < len; i++) {
            frame[8 + i] = data[i] ^ mask[i % 4];
        }
        
        if (send_all(s_sock, (char *)frame, 8 + len) < 0) {
            return ESP_FAIL;
        }
    } else {
        uint8_t frame[1400];
        frame[0] = 0x81;
        frame[1] = len | 0x80;
        
        memcpy(frame + 2, mask, 4);
        for (size_t i = 0; i < len; i++) {
            frame[6 + i] = data[i] ^ mask[i % 4];
        }
        
        if (send_all(s_sock, (char *)frame, 6 + len) < 0) {
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

void ws_client_disconnect(void)
{
    s_running = false;
    
    if (s_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (eTaskGetState(s_task_handle) != eDeleted) {
            vTaskDelete(s_task_handle);
        }
        s_task_handle = NULL;
    }
    
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    
    s_connected = false;
    ESP_LOGI(TAG, "WebSocket client disconnected");
}

bool ws_client_is_connected(void)
{
    return s_connected;
}
