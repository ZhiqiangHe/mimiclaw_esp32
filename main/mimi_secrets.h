/*
 * MimiClaw Build-time Secrets
 *
 * This is the ONLY way to configure MimiClaw.
 * Copy this file to mimi_secrets.h and fill in your values.
 * After any change, rebuild: idf.py fullclean && idf.py build
 *
 *   cp mimi_secrets.h.example mimi_secrets.h
 */

#pragma once

/* WiFi */
#define MIMI_SECRET_WIFI_SSID       "YOUR WIFI SSID"
#define MIMI_SECRET_WIFI_PASS       "wifi password"

/* Telegram Bot */
/* #define MIMI_SECRET_TG_TOKEN        "" */

/* 飞书机器人 */
#define MIMI_SECRET_FEISHU_APP_ID        "FEISHU_APP_ID"
#define MIMI_SECRET_FEISHU_APP_SECRET    "FEISHU_APP_SECRET"
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN  "FEISHU_VERIFY_TOKEN"
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY   "FEISHU_ENCRYPT_KEY"

/* Anthropic API */
#define MIMI_SECRET_API_KEY         "YOUR-API-Key"
#define MIMI_SECRET_MODEL           "glm-4-flash"
#define MIMI_SECRET_MODEL_PROVIDER  "zhipu"

/* HTTP Proxy (leave empty or set both) */
#define MIMI_SECRET_PROXY_HOST      ""
#define MIMI_SECRET_PROXY_PORT      ""
#define MIMI_SECRET_PROXY_TYPE      ""   /* "http" or "socks5" */

/* Brave Search API */
#define MIMI_SECRET_SEARCH_KEY      "tavily key"              // 可选：Brave Search API key
#define MIMI_SECRET_SEARCH_PROVIDER "tavily"
