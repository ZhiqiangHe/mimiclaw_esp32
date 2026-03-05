#include "tool_web_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static char s_search_key[128] = {0};
static char s_search_provider[16] = "brave";  /* brave or tavily */

#define SEARCH_BUF_SIZE     (16 * 1024)
#define SEARCH_RESULT_COUNT 5

static bool provider_is_tavily(void)
{
    return strcmp(s_search_provider, "tavily") == 0;
}

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

/* Clean invalid UTF-8 sequences from string */
static void clean_utf8(char *str)
{
    if (!str) return;
    
    char *src = str;
    char *dst = str;
    
    while (*src) {
        unsigned char c = (unsigned char)*src;
        
        /* Single byte ASCII (0x00-0x7F) */
        if (c < 0x80) {
            *dst++ = *src++;
        }
        /* 2-byte UTF-8 (0xC2-0xDF followed by 0x80-0xBF) */
        else if (c >= 0xC2 && c <= 0xDF) {
            unsigned char c2 = (unsigned char)*(src + 1);
            if (c2 >= 0x80 && c2 <= 0xBF) {
                *dst++ = *src++;
                *dst++ = *src++;
            } else {
                src++; /* Skip invalid byte */
            }
        }
        /* 3-byte UTF-8 (0xE0-0xEF followed by two 0x80-0xBF) */
        else if (c >= 0xE0 && c <= 0xEF) {
            unsigned char c2 = (unsigned char)*(src + 1);
            unsigned char c3 = (unsigned char)*(src + 2);
            if (c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF) {
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
            } else {
                src++; /* Skip invalid byte */
            }
        }
        /* 4-byte UTF-8 (0xF0-0xF4 followed by three 0x80-0xBF) */
        else if (c >= 0xF0 && c <= 0xF4) {
            unsigned char c2 = (unsigned char)*(src + 1);
            unsigned char c3 = (unsigned char)*(src + 2);
            unsigned char c4 = (unsigned char)*(src + 3);
            if (c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF && c4 >= 0x80 && c4 <= 0xBF) {
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
            } else {
                src++; /* Skip invalid byte */
            }
        }
        /* Invalid leading byte (0x80-0xBF, 0xF5-0xFF) */
        else {
            src++; /* Skip invalid byte */
        }
    }
    *dst = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *sb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = sb->len + evt->data_len;
        if (needed < sb->cap) {
            memcpy(sb->data + sb->len, evt->data, evt->data_len);
            sb->len += evt->data_len;
            sb->data[sb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }
    if (MIMI_SECRET_SEARCH_PROVIDER[0] != '\0') {
        strncpy(s_search_provider, MIMI_SECRET_SEARCH_PROVIDER, sizeof(s_search_provider) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        char provider_tmp[16] = {0};
        len = sizeof(provider_tmp);
        if (nvs_get_str(nvs, "search_provider", provider_tmp, &len) == ESP_OK && provider_tmp[0]) {
            strncpy(s_search_provider, provider_tmp, sizeof(s_search_provider) - 1);
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (provider: %s)", s_search_provider);
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
}

/* ── URL-encode a query string ────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Format results as readable text ──────────────────────────── */

static void format_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = cJSON_GetObjectItem(root, "web");
    if (!web) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON *results = cJSON_GetObjectItem(web, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *desc = cJSON_GetObjectItem(item, "description");

        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n",
            idx + 1,
            (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
            (url && cJSON_IsString(url)) ? url->valuestring : "",
            (desc && cJSON_IsString(desc)) ? desc->valuestring : "");

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t search_direct(const char *url, search_buf_t *sb)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t search_via_proxy(const char *path, search_buf_t *sb)
{
    proxy_conn_t *conn = proxy_conn_open("api.search.brave.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: api.search.brave.com\r\n"
        "Accept: application/json\r\n"
        "X-Subscription-Token: %s\r\n"
        "Connection: close\r\n\r\n",
        path, s_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response */
    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    /* Check status */
    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip headers */
    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Tavily Search ───────────────────────────────────────────── */

static esp_err_t tavily_search_direct(const char *query, search_buf_t *sb)
{
    /* Build JSON request body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "api_key", s_search_key);
    cJSON_AddStringToObject(body, "query", query);
    cJSON_AddNumberToObject(body, "max_results", SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(body, "include_answer", false);
    
    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    
    if (!post_data) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Tavily API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void format_tavily_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No search results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *content = cJSON_GetObjectItem(item, "content");

        /* Get strings and clean them */
        char title_buf[256] = "(no title)";
        char url_buf[512] = "";
        char content_buf[2048] = "";

        if (title && cJSON_IsString(title)) {
            strncpy(title_buf, title->valuestring, sizeof(title_buf) - 1);
            clean_utf8(title_buf);
        }
        if (url && cJSON_IsString(url)) {
            strncpy(url_buf, url->valuestring, sizeof(url_buf) - 1);
            clean_utf8(url_buf);
        }
        if (content && cJSON_IsString(content)) {
            strncpy(content_buf, content->valuestring, sizeof(content_buf) - 1);
            clean_utf8(content_buf);
        }

        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n",
            idx + 1, title_buf, url_buf, content_buf);

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size, "Error: No search API key configured. Set MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input to get query */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s (provider: %s)", query->valuestring, s_search_provider);

    /* Allocate response buffer from PSRAM */
    search_buf_t sb = {0};
    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    esp_err_t err;
    if (provider_is_tavily()) {
        /* Tavily search */
        err = tavily_search_direct(query->valuestring, &sb);
        cJSON_Delete(input);
        
        if (err != ESP_OK) {
            free(sb.data);
            snprintf(output, output_size, "Error: Tavily search request failed");
            return err;
        }

        /* Parse and format results */
        cJSON *root = cJSON_Parse(sb.data);
        free(sb.data);

        if (!root) {
            snprintf(output, output_size, "Error: Failed to parse Tavily search results");
            return ESP_FAIL;
        }

        format_tavily_results(root, output, output_size);
        cJSON_Delete(root);
        
        ESP_LOGI(TAG, "Tavily search complete, %d bytes result", (int)strlen(output));
        return ESP_OK;
    }

    /* Brave search (original code) */
    char encoded_query[256];
    url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
    cJSON_Delete(input);

    char path[384];
    snprintf(path, sizeof(path),
             "/res/v1/web/search?q=%s&count=%d", encoded_query, SEARCH_RESULT_COUNT);
    sb.cap = SEARCH_BUF_SIZE;

    /* Make HTTP request */
    if (http_proxy_is_enabled()) {
        err = search_via_proxy(path, &sb);
    } else {
        char url[512];
        snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
        err = search_direct(url, &sb);
    }

    if (err != ESP_OK) {
        free(sb.data);
        snprintf(output, output_size, "Error: Search request failed");
        return err;
    }

    /* Parse and format results */
    cJSON *root = cJSON_Parse(sb.data);
    free(sb.data);

    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse search results");
        return ESP_FAIL;
    }

    format_results(root, output, output_size);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved");
    return ESP_OK;
}
