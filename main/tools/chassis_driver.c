#include "chassis_driver.h"
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "chassis_driver";

#define CHASSIS_UART_PORT      UART_NUM_1
#define CHASSIS_UART_BAUD_RATE 115200
#define CHASSIS_UART_TX_PIN    GPIO_NUM_38
#define CHASSIS_UART_RX_PIN    GPIO_NUM_48
#define CHASSIS_UART_RTS_PIN   UART_PIN_NO_CHANGE
#define CHASSIS_UART_CTS_PIN   UART_PIN_NO_CHANGE
#define CHASSIS_BUF_SIZE       128

static bool s_initialized = false;

esp_err_t chassis_driver_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Chassis driver already initialized");
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = CHASSIS_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(CHASSIS_UART_PORT, CHASSIS_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(CHASSIS_UART_PORT, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        uart_driver_delete(CHASSIS_UART_PORT);
        return ret;
    }

    ret = uart_set_pin(CHASSIS_UART_PORT, CHASSIS_UART_TX_PIN, CHASSIS_UART_RX_PIN, 
                       CHASSIS_UART_RTS_PIN, CHASSIS_UART_CTS_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(CHASSIS_UART_PORT);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Chassis driver initialized (TX:%d, RX:%d)", CHASSIS_UART_TX_PIN, CHASSIS_UART_RX_PIN);

    return ESP_OK;
}

void chassis_driver_deinit(void)
{
    if (s_initialized) {
        uart_driver_delete(CHASSIS_UART_PORT);
        s_initialized = false;
        ESP_LOGI(TAG, "Chassis driver deinitialized");
    }
}

static esp_err_t send_command(const char *command)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Chassis driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    size_t len = strlen(command);
    int written = uart_write_bytes(CHASSIS_UART_PORT, command, len);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to send command: %s", command);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sent command: %s", command);
    return ESP_OK;
}

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
        case CHASSIS_DIR_STOP:
            command = "x0.0 y0.0";
            break;
        case CHASSIS_DIR_DANCE:
            command = "d1";
            break;
        default:
            ESP_LOGE(TAG, "Invalid direction: %d", direction);
            return ESP_ERR_INVALID_ARG;
    }

    return send_command(command);
}

esp_err_t chassis_stop(void)
{
    return chassis_move(CHASSIS_DIR_STOP);
}

esp_err_t chassis_dance(void)
{
    return chassis_move(CHASSIS_DIR_DANCE);
}

esp_err_t chassis_set_light_mode(chassis_light_mode_t mode)
{
    char command[8];

    switch (mode) {
        case CHASSIS_LIGHT_OFF:
        case CHASSIS_LIGHT_MODE_1:
        case CHASSIS_LIGHT_MODE_2:
        case CHASSIS_LIGHT_MODE_3:
        case CHASSIS_LIGHT_MODE_4:
        case CHASSIS_LIGHT_MODE_5:
        case CHASSIS_LIGHT_MODE_6:
        case CHASSIS_LIGHT_MODE_7:
        case CHASSIS_LIGHT_MODE_8:
            snprintf(command, sizeof(command), "w%d", mode);
            break;
        case CHASSIS_LIGHT_FLASHLIGHT_ON:
            snprintf(command, sizeof(command), "w2");
            break;
        case CHASSIS_LIGHT_FLASHLIGHT_OFF:
            snprintf(command, sizeof(command), "w8");
            break;
        default:
            ESP_LOGE(TAG, "Invalid light mode: %d", mode);
            return ESP_ERR_INVALID_ARG;
    }

    return send_command(command);
}

esp_err_t chassis_get_battery(int *battery_level)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Chassis driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (battery_level == NULL) {
        ESP_LOGE(TAG, "battery_level is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    char buf[CHASSIS_BUF_SIZE];
    const char *prefix = "tb:";

    send_command("b0");

    for (int i = 0; i < 10; i++) {
        int len = uart_read_bytes(CHASSIS_UART_PORT, (uint8_t *)buf, sizeof(buf) - 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            buf[len] = '\0';
            ESP_LOGI(TAG, "Received: %s", buf);

            char *found = strstr(buf, prefix);
            if (found != NULL) {
                int value = atoi(found + strlen(prefix));
                if (value > 100) value = 100;
                if (value < 0) value = 0;
                *battery_level = value;
                ESP_LOGI(TAG, "Battery level: %d%%", value);
                return ESP_OK;
            }
        }
    }

    ESP_LOGW(TAG, "Failed to get battery level");
    *battery_level = 0;
    return ESP_ERR_NOT_FOUND;
}
