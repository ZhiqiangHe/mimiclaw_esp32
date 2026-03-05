#include "feishu_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"
#include "gateway/ws_client.h"
#include "ui/message_display.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";

/* Feishu API endpoints */
#define FEISHU_API_BASE         "https://open.feishu.cn/open-apis"
#define FEISHU_AUTH_URL         FEISHU_API_BASE "/auth/v3/tenant_access_token/internal"
#define FEISHU_EVENT_URL        FEISHU_API_BASE "/im/v1/messages"

/* WebSocket server URI (xiaozhi-esp32-server) */
#define XIAOZHI_WS_URI         "ws://172.20.30.46:28000/xiaozhi/v1/?client=mimiclaw"

static char s_app_id[64] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[128] = MIMI_SECRET_FEISHU_APP_SECRET;

/* Check if string contains Chinese characters (UTF-8) */
static bool contains_chinese(const char *str)
{
    if (str == NULL) return false;
    
    for (int i = 0; str[i] != '\0'; i++) {
        unsigned char c = (unsigned char)str[i];
        
        /* UTF-8 Chinese characters start with 0xE4-0xE9 (3 bytes) or 0xF0-0xF7 (4 bytes) */
        if (c >= 0xE4 && c <= 0xF7) {
            return true;
        }
    }
    return false;
}
static char s_tenant_token[512] = {0};
static int64_t s_token_expire_time = 0;

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/* ── Get tenant access token ────────────────────────────── */
static esp_err_t feishu_get_tenant_token(void)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "No Feishu credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Check if token is still valid (with 5 min buffer) */
    int64_t now = esp_timer_get_time() / 1000000LL;
    if (s_tenant_token[0] != '\0' && s_token_expire_time > now + 300) {
        return ESP_OK;
    }

    /* Build request body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "app_id", s_app_id);
    cJSON_AddStringToObject(body, "app_secret", s_app_secret);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    http_resp_t resp = {
        .buf = calloc(1, 2048),
        .len = 0,
        .cap = 2048,
    };
    if (!resp.buf) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = FEISHU_AUTH_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(json_str);
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    /* Parse response */
    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse token response");
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || code->valueint != 0) {
        ESP_LOGE(TAG, "Token request failed: code=%d", code ? code->valueint : -1);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");
    
    if (token && cJSON_IsString(token)) {
        strncpy(s_tenant_token, token->valuestring, sizeof(s_tenant_token) - 1);
        s_token_expire_time = now + (expire ? expire->valueint : 7200) - 300;
        ESP_LOGI(TAG, "Got tenant access token, expires in %d seconds", 
                 expire ? expire->valueint : 7200);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Feishu API call (direct path) ──────────────────────── */
static char *feishu_api_call(const char *url, const char *method, const char *post_data)
{
    /* Ensure we have a valid token */
    if (feishu_get_tenant_token() != ESP_OK) {
        return NULL;
    }

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return NULL;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return NULL;
    }

    /* Set headers */
    char auth_header[600];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (post_data) {
            esp_http_client_set_post_field(client, post_data, strlen(post_data));
        }
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    return resp.buf;
}

/* ── WebSocket Client Task ── */
/* Connect to xiaozhi-esp32-server and receive Feishu messages */

static void feishu_ws_event_handler(ws_event_t event, const char *data, size_t len, void *user_data)
{
    switch (event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to xiaozhi-esp32-server, sending hello...");
            
            /* Display connection success */
            message_display_add("Feishu websocket is ready!!!", MSG_TYPE_RECEIVED);
            
            /* Send hello message per xiaozhi protocol */
            {
                cJSON *hello = cJSON_CreateObject();
                cJSON_AddStringToObject(hello, "type", "hello");
                cJSON_AddNumberToObject(hello, "version", 1);
                cJSON_AddStringToObject(hello, "transport", "websocket");
                char *hello_str = cJSON_PrintUnformatted(hello);
                if (hello_str) {
                    ws_client_send(hello_str, strlen(hello_str));
                    ESP_LOGI(TAG, "Hello message sent: %s", hello_str);
                    free(hello_str);
                }
                cJSON_Delete(hello);
            }
            break;

        case WS_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from xiaozhi-esp32-server");
            
            /* Display connection error */
            message_display_add("Feishu websocket init error..", MSG_TYPE_SENT);
            break;

        case WS_EVENT_DATA:
            ESP_LOGI(TAG, "Received data from xiaozhi-esp32-server: %.*s", len, data);
            
            /* Parse JSON message */
            cJSON *root = cJSON_ParseWithLength(data, len);
            if (root) {
                /* Check message type */
                cJSON *type = cJSON_GetObjectItem(root, "type");
                
                if (type && cJSON_IsString(type)) {
                    if (strcmp(type->valuestring, "welcome") == 0 || 
                        strcmp(type->valuestring, "message") == 0 ||
                        strcmp(type->valuestring, "hello") == 0) {
                        ESP_LOGI(TAG, "Received %s, sending initial ping...", type->valuestring);
                        
                        /* Send initial ping immediately */
                        int64_t now = esp_timer_get_time() / 1000;
                        char ping_json[64];
                        int len = snprintf(ping_json, sizeof(ping_json), 
                            "{\"type\":\"ping\",\"timestamp\":%lld}", (long long)now);
                        ws_client_send(ping_json, len);
                    } else if (strcmp(type->valuestring, "response") == 0) {
                        cJSON *content = cJSON_GetObjectItem(root, "content");
                        if (content && cJSON_IsString(content) && strcmp(content->valuestring, "ready") == 0) {
                            ESP_LOGI(TAG, "Server confirmed ready, sending initial ping...");
                            int64_t now = esp_timer_get_time() / 1000;
                            char ping_json[64];
                            int len = snprintf(ping_json, sizeof(ping_json), 
                                "{\"type\":\"ping\",\"timestamp\":%lld}", (long long)now);
                            ws_client_send(ping_json, len);
                        }
                    } else if (strcmp(type->valuestring, "ping") == 0) {
                        ESP_LOGD(TAG, "Received ping, sending pong...");
                        
                        /* Get timestamp from ping message if available */
                        int64_t timestamp = 0;
                        cJSON *ts = cJSON_GetObjectItem(root, "timestamp");
                        if (ts && cJSON_IsNumber(ts)) {
                            timestamp = ts->valuedouble;
                        }
                        
                        /* Send pong with timestamp */
                        cJSON *pong = cJSON_CreateObject();
                        cJSON_AddStringToObject(pong, "type", "pong");
                        if (timestamp > 0) {
                            cJSON_AddNumberToObject(pong, "timestamp", timestamp);
                        }
                        char *pong_str = cJSON_PrintUnformatted(pong);
                        if (pong_str) {
                            ws_client_send(pong_str, strlen(pong_str));
                            free(pong_str);
                        }
                        cJSON_Delete(pong);
                    } else if (strcmp(type->valuestring, "close") == 0) {
                        ESP_LOGW(TAG, "Server sent close");
                    }
                }
                
                /* Check if this is a Feishu message from xiaozhi-server */
                cJSON *msg_type = cJSON_GetObjectItem(root, "type");
                cJSON *content = cJSON_GetObjectItem(root, "content");
                cJSON *chat_id = cJSON_GetObjectItem(root, "chat_id");
                (void)cJSON_GetObjectItem(root, "sender_id"); /* Available if needed */
                
                /* Handle response type messages with chat_id (from Feishu) */
                if (msg_type && cJSON_IsString(msg_type) && 
                    strcmp(msg_type->valuestring, "response") == 0 &&
                    content && cJSON_IsString(content) &&
                    chat_id && cJSON_IsString(chat_id)) {
                    
                    const char *text = content->valuestring;
                    
                    /* Filter out server confirmation messages */
                    if (strncmp(text, "消息已发送到飞书:", 14) == 0 ||
                        strncmp(text, "Sorry, I encountered an error", 29) == 0 ||
                        strncmp(text, "🐱mimi is working", 17) == 0) {
                        ESP_LOGD(TAG, "Ignoring server confirmation: %.50s...", text);
                    } else {
                        ESP_LOGI(TAG, "Feishu message: %s", text);

                        /* Check if message contains Chinese characters */
                        if (contains_chinese(text)) {
                            /* Show fixed message for Chinese content */
                            message_display_add("Received a message in Chinese.", MSG_TYPE_RECEIVED);
                        } else {
                            /* Show original message */
                            message_display_add(text, MSG_TYPE_RECEIVED);
                        }

                        /* Push to inbound bus */
                        mimi_msg_t msg = {0};
                        strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
                        strncpy(msg.chat_id, chat_id->valuestring, sizeof(msg.chat_id) - 1);
                        msg.content = strdup(text);
                        
                        if (msg.content) {
                            message_bus_push_inbound(&msg);
                            ESP_LOGI(TAG, "Feishu message pushed to inbound bus");
                        }
                    }
                }
                cJSON_Delete(root);
            }
            break;

        case WS_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

static void feishu_ws_task(void *arg)
{
    ESP_LOGI(TAG, "Feishu WebSocket task started");
    ESP_LOGI(TAG, "Connecting to xiaozhi-esp32-server at %s", XIAOZHI_WS_URI);

    /* Initialize WebSocket client */
    if (ws_client_init(XIAOZHI_WS_URI, feishu_ws_event_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        vTaskDelete(NULL);
        return;
    }

    /* Connect to server */
    if (ws_client_connect() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to xiaozhi-esp32-server");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Waiting for Feishu messages from xiaozhi-esp32-server...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        /* Check connection status and reconnect if needed */
        if (!ws_client_is_connected()) {
            ESP_LOGW(TAG, "WebSocket disconnected, attempting to reconnect...");
            ws_client_connect();
        }
    }
}

/* ── Public API ─────────────────────────────────────────── */

esp_err_t feishu_bot_init(void)
{
    /* NVS overrides take highest priority */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_id[64] = {0};
        char tmp_secret[128] = {0};
        size_t len_id = sizeof(tmp_id);
        size_t len_secret = sizeof(tmp_secret);
        
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, tmp_id, &len_id) == ESP_OK && tmp_id[0]) {
            strncpy(s_app_id, tmp_id, sizeof(s_app_id) - 1);
        }
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_SECRET, tmp_secret, &len_secret) == ESP_OK && tmp_secret[0]) {
            strncpy(s_app_secret, tmp_secret, sizeof(s_app_secret) - 1);
        }
        nvs_close(nvs);
    }

    if (s_app_id[0] && s_app_secret[0]) {
        ESP_LOGI(TAG, "Feishu credentials loaded (app_id=%s)", s_app_id);
    } else {
        ESP_LOGW(TAG, "No Feishu credentials. Use CLI: set_feishu_creds <APP_ID> <APP_SECRET>");
    }
    
    return ESP_OK;
}

esp_err_t feishu_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        feishu_ws_task, "feishu_ws",
        MIMI_FEISHU_POLL_STACK, NULL,
        MIMI_FEISHU_POLL_PRIO, NULL, MIMI_FEISHU_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    /* Send message via WebSocket to xiaozhi-server */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "message");
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddStringToObject(msg, "chat_id", chat_id);
    
    char *json_str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Sending message via WebSocket: %s", json_str);
    
    /* Show on LCD screen before sending */
    message_display_add(text, MSG_TYPE_SENT);
    
    esp_err_t err = ws_client_send(json_str, strlen(json_str));
    free(json_str);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message via WebSocket");
        return err;
    }
    
    ESP_LOGI(TAG, "Message sent via WebSocket successfully");
    return ESP_OK;
}

esp_err_t feishu_set_credentials(const char *app_id, const char *app_secret)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, app_id));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_SECRET, app_secret));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    strncpy(s_app_secret, app_secret, sizeof(s_app_secret) - 1);
    
    /* Clear cached token */
    s_tenant_token[0] = '\0';
    s_token_expire_time = 0;
    
    ESP_LOGI(TAG, "Feishu credentials saved");
    return ESP_OK;
}