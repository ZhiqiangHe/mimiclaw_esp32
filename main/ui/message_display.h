#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Message display manager for ST7789 LCD with LVGL and Chinese Font Support */

typedef enum {
    MSG_TYPE_RECEIVED,  /* From user (Feishu) */
    MSG_TYPE_SENT       /* To user (AI reply) */
} msg_type_t;

/* Initialize message display with LVGL and Chinese font support */
void message_display_init(void);

/* Add a message to chat history and refresh display */
void message_display_add(const char *content, msg_type_t type);

/* Clear the screen and history */
void message_display_clear(void);

/* Check if display is initialized */
bool message_display_is_ready(void);
