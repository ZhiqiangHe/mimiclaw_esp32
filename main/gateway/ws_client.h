#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WS_EVENT_CONNECTED,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_DATA,
    WS_EVENT_ERROR,
} ws_event_t;

typedef void (*ws_event_handler_t)(ws_event_t event, const char *data, size_t len, void *user_data);

esp_err_t ws_client_init(const char *uri, ws_event_handler_t handler, void *user_data);
esp_err_t ws_client_connect(void);
esp_err_t ws_client_send(const char *data, size_t len);
void ws_client_disconnect(void);
bool ws_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
