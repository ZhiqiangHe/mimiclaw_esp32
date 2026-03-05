# MimiClaw 项目修改详解

## 概述

文档详细介绍基于esp32-s3-sparkbot刷入 MimiClaw 来控制xiaozhi机器人项目从原始版本到当前版本的修改内容，重点介绍飞书SDK接入、底盘控制、消息显示等新增功能的实现原理和架构设计。

---


## 目录

1. [项目背景](#项目背景)
2. [修改概览](#修改概览)
3. [核心功能详解](#核心功能详解)
4. [架构设计](#架构设计)
5. [技术实现细节](#技术实现细节)
6. [总结与展望](#总结与展望)

---

## 项目背景

### 原始项目
MimiClaw 是一个基于 ESP32-S3 的智能机器人项目，原始版本主要功能包括：
- Telegram 机器人集成
- AI Agent 智能对话
- 本地内存管理
- WebSocket 服务端
- 工具调用系统

### 修改目标
为了适应更广泛的应用场景，项目进行了以下扩展：
- ✅ 接入飞书机器人平台
- ✅ 添加底盘移动能力
- ✅ 实现本地消息显示
- ✅ 支持多通道消息架构
- ✅ 集成小智服务端

---

## 修改概览

### 新增文件统计

| 模块 | 文件数 | 功能描述 |
|------|--------|----------|
| 飞书集成 | 3 | 飞书机器人核心实现、接口定义、文档 |
| WebSocket客户端 | 2 | WebSocket客户端实现、接口定义 |
| 底盘控制 | 4 | 底盘控制工具、UART驱动 |
| 消息显示 | 2 | LCD消息显示实现、接口定义 |
| **总计** | **11** | **新增代码约2000行** |

### 修改文件统计

| 文件 | 修改类型 | 主要变更 |
|------|----------|----------|
| CMakeLists.txt | 构建配置 | 添加新模块源文件和依赖 |
| mimi.c | 主程序 | 集成飞书、消息显示，修改导入路径 |
| message_bus.h | 消息总线 | 新增飞书通道定义 |

---

## 核心功能详解

### 1. 飞书SDK接入 ⭐

#### 1.1 功能概述

通过小智服务端实现飞书机器人的消息收发功能，ESP32无需直接处理飞书API的复杂逻辑。

#### 1.2 架构设计

```
┌─────────────┐         ┌──────────────┐         ┌─────────────┐
│  飞书用户    │ ←─────→ │  小智服务端   │ ←─────→ │  ESP32设备  │
│  (Lark)     │  HTTPS  │  (WebSocket) │  WS     │  (MimiClaw) │
└─────────────┘         └──────────────┘         └─────────────┘
                              ↓
                        飞书API调用
                        - 消息发送
                        - Token管理
                        - 事件订阅
```

#### 1.3 核心实现

**文件位置**: `main/channels/feishu/`

**关键功能**:

1. **Tenant Access Token 管理**
   ```c
   // 自动获取和刷新Token
   static esp_err_t feishu_get_tenant_token(void)
   {
       // 检查Token是否过期（5分钟缓冲）
       if (s_token_expire_time > now + 300) {
           return ESP_OK;
       }
       // 向飞书API请求新Token
       // 缓存Token和过期时间
   }
   ```

2. **消息发送**
   ```c
   esp_err_t feishu_send_message(const char *chat_id, const char *text)
   {
       // 自动分片：超过4096字符分多条发送
       // 使用Tenant Token认证
       // 调用飞书消息API
   }
   ```

3. **WebSocket 客户端连接**
   ```c
   #define XIAOZHI_WS_URI "ws://127.0.0.1:28000/xiaozhi/v1/?client=mimiclaw"
   
   // 连接小智服务端，接收飞书消息
   static void feishu_ws_event_handler(ws_event_t event, ...)
   {
       switch (event) {
           case WS_EVENT_CONNECTED:
               // 发送hello消息
               // 显示连接成功
               break;
           case WS_EVENT_DATA:
               // 解析消息类型
               // 处理飞书消息
               break;
       }
   }
   ```

#### 1.4 优势

| 对比项 | 直接接入飞书API | 通过小智服务端 |
|--------|----------------|----------------|
| HTTPS请求复杂度 | 高 | 低 |
| Token管理 | 需要自己实现 | 服务端处理 |
| 事件订阅 | 需要Webhook | 服务端处理 |
| ESP32资源占用 | 高 | 低 |
| 维护成本 | 高 | 低 |

---

### 2. 底盘控制功能 🚗

#### 2.1 功能概述

为机器人添加移动能力，支持多种运动模式和灯光控制。

#### 2.2 硬件连接

```
ESP32-S3 ←→ 底盘控制器 (UART)
├─ TX: GPIO38
├─ RX: GPIO48
└─ 波特率: 115200
```

#### 2.3 功能列表

| 功能 | 命令 | 说明 |
|------|------|------|
| 前进 | move_forward | 向前移动 |
| 后退 | move_backward | 向后移动 |
| 左转 | turn_left | 原地左转 |
| 右转 | turn_right | 原地右转 |
| 停止 | stop | 停止运动 |
| 跳舞 | dance | 执行舞蹈动作 |
| 灯光模式 | set_light_mode | 控制LED灯光 |
| 电量查询 | get_battery | 获取电池电量 |

#### 2.4 核心实现

**文件位置**: `main/tools/`

**驱动层** (`chassis_driver.c`):
```c
// UART初始化
esp_err_t chassis_driver_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    // 安装UART驱动
    // 配置引脚
}

// 发送运动命令
esp_err_t chassis_move(chassis_direction_t direction)
{
    const char *command = NULL;
    switch (direction) {
        case CHASSIS_DIR_FORWARD:
            command = "x0.0 y1.0";  // 前进
            break;
        case CHASSIS_DIR_BACKWARD:
            command = "x0.0 y-1.0"; // 后退
            break;
        // ...
    }
    return send_command(command);
}
```

**工具层** (`tool_chassis.c`):
```c
// 注册为AI Agent可调用的工具
esp_err_t tool_chassis_execute(const char *input_json, char *output, ...)
{
    cJSON *args = cJSON_Parse(input_json);
    cJSON *action = cJSON_GetObjectItem(args, "action");
    
    if (strcmp(action->valuestring, "move_forward") == 0) {
        chassis_move(CHASSIS_DIR_FORWARD);
    }
    // ...
    
    // 返回执行结果
    cJSON_AddStringToObject(result, "status", "success");
}
```

#### 2.5 使用示例

**通过AI Agent调用**:
```
用户: 前进两米
AI: 调用底盘工具 → 执行前进命令 → 返回结果
```

**直接API调用**:
```c
// 前进
chassis_move(CHASSIS_DIR_FORWARD);
vTaskDelay(pdMS_TO_TICKS(2000));
// 停止
chassis_stop();
```

---

### 3. 消息显示功能 📺

#### 3.1 功能概述

在LCD屏幕上显示聊天消息和系统状态，提供本地可视化反馈。

#### 3.2 硬件配置

```
ST7789 LCD (240x240) ←→ ESP32-S3 (SPI)
├─ SCLK: GPIO21
├─ MOSI: GPIO47
├─ DC:   GPIO43
├─ CS:   GPIO44
├─ BL:   GPIO46
└─ RST:  NC
```

#### 3.3 设计特点

| 特性 | 说明 |
|------|------|
| 驱动方式 | 直接SPI驱动（不使用LVGL） |
| 字体 | 内置8x16 ASCII字体 |
| 消息历史 | 最多20条 |
| 屏幕容量 | 30字符/行 × 15行 |
| 颜色区分 | 接收消息（绿色前缀）、发送消息（红色前缀） |

#### 3.4 核心实现

**文件位置**: `main/ui/message_display.c`

**初始化**:
```c
void message_display_init(void)
{
    // 初始化SPI总线
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .sclk_io_num = LCD_PIN_SCLK,
        // ...
    };
    
    // 初始化LCD面板
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    // 配置ST7789参数
    // 清屏
}
```

**消息显示**:
```c
void message_display_add(const char *content, msg_type_t type)
{
    // 添加到历史记录
    chat_msg_t msg;
    msg.type = type;
    strncpy(msg.content, content, MSG_MAX_LEN);
    
    // 更新显示
    refresh_display();
}

static void refresh_display(void)
{
    // 清屏
    // 遍历历史记录
    for (int i = 0; i < s_history_count; i++) {
        // 根据类型选择颜色
        lv_color_t color = (msg.type == MSG_TYPE_RECEIVED) 
                          ? COLOR_PREFIX_R : COLOR_PREFIX_S;
        
        // 绘制前缀
        // 绘制内容
    }
}
```

**显示内容示例**:
```
> Feishu websocket is ready!!!
< 收到消息
> 发送回复
```

---

### 4. 多通道消息架构 📡

#### 4.1 架构设计

```
┌─────────────────────────────────────────────────┐
│              消息总线 (Message Bus)             │
│  ┌─────────────┐      ┌─────────────┐          │
│  │  入队队列   │      │  出队队列   │          │
│  └─────────────┘      └─────────────┘          │
└─────────────────────────────────────────────────┘
           ↓                       ↓
    ┌──────┴──────┐        ┌──────┴──────┐
    ↓             ↓        ↓             ↓
┌───────┐    ┌───────┐ ┌───────┐    ┌───────┐
│Telegram│    │飞书   │ │WebSocket│  │  CLI  │
│ Bot   │    │ Bot   │ │ Server │  │       │
└───────┘    └───────┘ └───────┘    └───────┘
```

#### 4.2 通道定义

**文件**: `main/bus/message_bus.h`

```c
// 通道标识符
#define MIMI_CHAN_TELEGRAM   "telegram"
#define MIMI_CHAN_FEISHU     "feishu"      // 新增
#define MIMI_CHAN_WEBSOCKET  "websocket"
#define MIMI_CHAN_CLI        "cli"
#define MIMI_CHAN_SYSTEM     "system"

// 消息结构
typedef struct {
    char channel[16];       // 通道名称
    char chat_id[32];      // 聊天ID
    char *content;         // 消息内容（堆分配）
} mimi_msg_t;
```

#### 4.3 消息流转

**入站流程**（用户 → AI）:
```
飞书用户消息
    ↓
小智服务端
    ↓
WebSocket接收
    ↓
消息总线入队
    ↓
AI Agent处理
    ↓
生成回复
```

**出站流程**（AI → 用户）:
```
AI生成回复
    ↓
消息总线出队
    ↓
分发任务
    ↓
根据通道路由
    ↓
发送到飞书/Telegram
```

#### 4.4 分发实现

**文件**: `main/mimi.c`

```c
static void outbound_dispatch_task(void *arg)
{
    while (1) {
        mimi_msg_t msg;
        message_bus_pop_outbound(&msg, UINT32_MAX);
        
        // 根据通道类型路由
        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            telegram_send_message(msg.chat_id, msg.content);
        } 
        else if (strcmp(msg.channel, MIMI_CHAN_FEISHU) == 0) {
            feishu_send_message(msg.chat_id, msg.content);
        }
        else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content);
        }
        
        free(msg.content);
    }
}
```

---

### 5. WebSocket客户端 🔌

#### 5.1 功能概述

实现原生WebSocket客户端，与小智服务端建立实时双向通信。

#### 5.2 协议实现

**握手过程**:
```c
static bool websocket_handshake(int sock, const char *host, const char *path)
{
    // 生成WebSocket Key
    char ws_key[32];
    generate_websocket_key(ws_key);
    
    // 发送握手请求
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, ws_key);
    
    send_all(sock, request, strlen(request));
    
    // 接收响应
    recv_timeout(sock, response, sizeof(response), 5000);
    
    // 检查是否成功切换协议
    return strstr(response, "101 Switching Protocols") != NULL;
}
```

**帧解析**:
```c
static uint8_t ws_parse_frame(const char *data, size_t len, ...)
{
    // 解析帧头
    uint8_t first_byte = data[0];
    uint8_t second_byte = data[1];
    
    uint8_t opcode = first_byte & 0x0F;
    bool masked = (second_byte & 0x80) != 0;
    uint64_t payload_len = second_byte & 0x7F;
    
    // 处理扩展长度
    // 解析掩码
    // 解码负载数据
    
    return opcode;
}
```

#### 5.3 心跳机制

```c
// 接收ping
if (strcmp(type->valuestring, "ping") == 0) {
    // 发送pong响应
    cJSON *pong = cJSON_CreateObject();
    cJSON_AddStringToObject(pong, "type", "pong");
    cJSON_AddNumberToObject(pong, "timestamp", timestamp);
    
    char *pong_str = cJSON_PrintUnformatted(pong);
    ws_client_send(pong_str, strlen(pong_str));
    free(pong_str);
}

// 发送ping
int64_t now = esp_timer_get_time() / 1000;
char ping_json[64];
snprintf(ping_json, sizeof(ping_json), 
    "{\"type\":\"ping\",\"timestamp\":%lld}", now);
ws_client_send(ping_json, strlen(ping_json));
```

#### 5.4 自动重连

```c
static void ws_client_task(void *arg)
{
    while (s_running) {
        // 尝试连接
        if (connect(...) < 0) {
            ESP_LOGE(TAG, "连接失败，5秒后重试");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // 握手
        if (!websocket_handshake(...)) {
            close(s_sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // 接收数据
        while (s_running) {
            int n = recv(s_sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                // 连接断开，重连
                break;
            }
            // 处理数据
        }
    }
}
```

---

## 架构设计

### 系统整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        用户层                                │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │ 飞书用户  │  │Telegram  │  │  Web客户端 │  │  CLI用户 │  │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │
└─────────────────────────────────────────────────────────────┘
         ↓              ↓              ↓              ↓
┌─────────────────────────────────────────────────────────────┐
│                      服务层                                  │
│  ┌────────────────────────────────────────────────────┐    │
│  │              小智服务端 (xiaozhi-esp32-server)     │    │
│  │  - 飞书API集成                                     │    │
│  │  - WebSocket服务                                   │    │
│  │  - 消息转发                                       │    │
│  └────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              ↓ WebSocket
┌─────────────────────────────────────────────────────────────┐
│                   ESP32 (MimiClaw)                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ WebSocket   │  │  消息总线   │  │  AI Agent   │         │
│  │  客户端     │  │             │  │             │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│         ↓              ↓              ↓                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ 飞书Bot     │  │ Telegram   │  │  工具系统   │         │
│  │             │  │  Bot       │  │             │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│                                              ↓               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  LCD显示    │  │  底盘控制   │  │  LLM代理    │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
└─────────────────────────────────────────────────────────────┘
```

### 模块依赖关系

```
mimi.c (主程序)
├── message_bus (消息总线)
│   ├── telegram_bot (Telegram机器人)
│   └── feishu_bot (飞书机器人)
│       ├── ws_client (WebSocket客户端)
│       └── http_proxy (HTTP代理)
├── agent_loop (AI Agent)
│   ├── llm_proxy (LLM代理)
│   ├── memory_store (内存存储)
│   └── tool_registry (工具注册)
│       ├── tool_chassis (底盘控制)
│       ├── tool_cron (定时任务)
│       └── tool_web_search (网络搜索)
├── message_display (消息显示)
└── chassis_driver (底盘驱动)
```

---

## 技术实现细节

### 1. 构建系统修改

**文件**: `main/CMakeLists.txt`

**修改内容**:
```cmake
idf_component_register(
    SRCS
        # 原有文件...
        "channels/telegram/telegram_bot.c"
        "channels/feishu/feishu_bot.c"      # 新增
        "gateway/ws_server.c"
        "gateway/ws_client.c"               # 新增
        "tools/tool_chassis.c"              # 新增
        "tools/chassis_driver.c"           # 新增
        "ui/message_display.c"              # 新增
    INCLUDE_DIRS "."
    REQUIRES
        # 原有依赖...
        esp_lcd                            # 新增
)
```

### 2. 主程序初始化流程

**文件**: `main/mimi.c`

```c
void app_main(void)
{
    // 1. 初始化基础服务
    init_nvs();
    init_spiffs();
    
    // 2. 初始化消息总线
    message_bus_init();
    
    // 3. 初始化WiFi
    wifi_manager_init();
    wifi_manager_connect();
    
    // 4. 初始化工具系统
    tool_registry_init();
    tool_chassis_init();              // 新增
    tool_cron_init();
    tool_web_search_init();
    // ...
    
    // 5. 初始化消息通道
    telegram_bot_init();
    feishu_bot_init();                // 新增
    
    // 6. 初始化显示
    message_display_init();            // 新增
    
    // 7. 启动服务
    feishu_bot_start();               // 新增
    telegram_bot_start();
    ws_server_start();
    
    // 8. 启动AI Agent
    agent_loop_start();
}
```

### 3. 中文消息处理

**问题**: LCD使用ASCII字体，无法显示中文

**解决方案**:
```c
// 检测中文字符
static bool contains_chinese(const char *str)
{
    for (int i = 0; str[i] != '\0'; i++) {
        unsigned char c = (unsigned char)str[i];
        // UTF-8中文字符范围
        if (c >= 0xE4 && c <= 0xF7) {
            return true;
        }
    }
    return false;
}

// 显示处理
if (contains_chinese(text)) {
    // 显示固定提示
    message_display_add("Received Chinese message", MSG_TYPE_RECEIVED);
} else {
    // 显示实际内容
    message_display_add(text, MSG_TYPE_RECEIVED);
}
```

### 4. UART通信协议

**底盘控制命令格式**:
```
x<速度X> y<速度Y>

示例:
- x0.0 y1.0   前进
- x0.0 y-1.0  后退
- x-1.0 y0.0  左转
- x1.0 y0.0   右转
- x0.0 y0.0   停止
```

**实现**:
```c
esp_err_t chassis_move(chassis_direction_t direction)
{
    const char *command = NULL;
    switch (direction) {
        case CHASSIS_DIR_FORWARD:
            command = "x0.0 y1.0";
            break;
        case CHASSIS_DIR_BACKWARD:
            command = "x0.0 y-1.0";
            break;
        case CHASSIS_DIR_LEFT:
            command = "x-1.0 y0.0";
            break;
        case CHASSIS_DIR_RIGHT:
            command = "x1.0 y0.0";
            break;
    }
    
    size_t len = strlen(command);
    int written = uart_write_bytes(CHASSIS_UART_PORT, command, len);
    
    return (written == len) ? ESP_OK : ESP_FAIL;
}
```

---

## 总结与展望

### 项目成果

✅ **功能完整性**
- 支持多平台消息接入（Telegram、飞书）
- 具备移动能力（底盘控制）
- 本地可视化反馈（LCD显示）
- 完善的工具系统

✅ **架构合理性**
- 模块化设计，易于扩展
- 统一的消息总线架构
- 清晰的分层设计

✅ **技术先进性**
- WebSocket实时通信
- 原生协议实现
- 资源优化（不使用LVGL）

### 技术亮点

1. **服务端代理模式**
   - 简化ESP32端实现
   - 降低资源占用
   - 提高可维护性

2. **多通道消息架构**
   - 统一的消息总线
   - 灵活的路由机制
   - 易于扩展新平台

3. **原生协议实现**
   - WebSocket客户端
   - 不依赖第三方库
   - 完全自主可控

### 未来展望

🔮 **功能扩展**
- [ ] 支持更多消息平台（微信、钉钉等）
- [ ] 增强底盘控制能力（避障、路径规划）
- [ ] 添加语音交互功能
- [ ] 集成摄像头视觉识别

🔮 **技术优化**
- [ ] 优化WebSocket连接稳定性
- [ ] 添加消息加密传输
- [ ] 实现离线语音识别
- [ ] 优化LCD显示效果

🔮 **生态建设**
- [ ] 开发配套移动应用
- [ ] 建立插件系统
- [ ] 提供云服务支持
- [ ] 构建开发者社区

---

## 参考资料

### 相关文档
- [飞书开放平台文档](https://open.feishu.cn/document/home/index)
- [ESP-IDF编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/)
- [WebSocket协议RFC 6455](https://tools.ietf.org/html/rfc6455)

### 项目地址
- 原始项目: `https://github.com/memovai/mimiclaw`
- 修改后项目: `https://github.com/ZhiqiangHe/mimiclaw_esp32/upload/main`

---

## 附录

### A. 文件清单

#### 新增文件
```
main/channels/feishu/
├── feishu_bot.c
├── feishu_bot.h
└── README.md

main/gateway/
├── ws_client.c
└── ws_client.h

main/tools/
├── tool_chassis.c
├── tool_chassis.h
├── chassis_driver.c
└── chassis_driver.h

main/ui/
├── message_display.c
└── message_display.h
```

#### 修改文件
```
main/
├── CMakeLists.txt
├── mimi.c
└── bus/message_bus.h
```

### B. 配置参数

#### 飞书配置
```c
#define MIMI_SECRET_FEISHU_APP_ID     "cli_xxxxxxxxxxxxxx"
#define MIMI_SECRET_FEISHU_APP_SECRET "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

#### WebSocket配置
```c
#define XIAOZHI_WS_URI "ws://127.0.0.1:28000/xiaozhi/v1/?client=mimiclaw"
```

#### 底盘配置
```c
#define CHASSIS_UART_PORT      UART_NUM_1
#define CHASSIS_UART_BAUD_RATE 115200
#define CHASSIS_UART_TX_PIN    GPIO_NUM_38
#define CHASSIS_UART_RX_PIN    GPIO_NUM_48
```

#### LCD配置
```c
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_SPI_FREQ        40000000
#define LCD_PIN_SCLK        GPIO_NUM_21
#define LCD_PIN_MOSI        GPIO_NUM_47
#define LCD_PIN_DC          GPIO_NUM_43
#define LCD_PIN_BL          GPIO_NUM_46
#define LCD_PIN_CS          GPIO_NUM_44
#define LCD_WIDTH           240
#define LCD_HEIGHT          240
```

---

**文档版本**: 1.0  
**最后更新**: 2026-03-05  
**作者**: ZhiqiangHe
