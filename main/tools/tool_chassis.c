#include "tool_chassis.h"
#include "chassis_driver.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "tool_chassis";

esp_err_t tool_chassis_init(void)
{
    esp_err_t ret = chassis_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize chassis driver (continuing without chassis control)");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Chassis tool initialized");
    return ESP_OK;
}

esp_err_t tool_chassis_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *args = cJSON_Parse(input_json);
    if (args == NULL) {
        ESP_LOGE(TAG, "Failed to parse args: %s", input_json);
        snprintf(output, output_size, "{\"error\":\"Invalid JSON\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *action_obj = cJSON_GetObjectItem(args, "action");
    if (action_obj == NULL || !cJSON_IsString(action_obj)) {
        cJSON_Delete(args);
        snprintf(output, output_size, "{\"error\":\"Missing or invalid 'action' parameter\"}");
        return ESP_ERR_INVALID_ARG;
    }

    const char *action = cJSON_GetStringValue(action_obj);
    ESP_LOGI(TAG, "Executing chassis action: %s", action);

    cJSON *result = cJSON_CreateObject();
    bool success = false;

    if (strcmp(action, "move_forward") == 0) {
        success = (chassis_move(CHASSIS_DIR_FORWARD) == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "move_forward");
    }
    else if (strcmp(action, "move_backward") == 0) {
        success = (chassis_move(CHASSIS_DIR_BACKWARD) == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "move_backward");
    }
    else if (strcmp(action, "turn_left") == 0) {
        success = (chassis_move(CHASSIS_DIR_LEFT) == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "turn_left");
    }
    else if (strcmp(action, "turn_right") == 0) {
        success = (chassis_move(CHASSIS_DIR_RIGHT) == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "turn_right");
    }
    else if (strcmp(action, "stop") == 0) {
        success = (chassis_stop() == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "stop");
    }
    else if (strcmp(action, "dance") == 0) {
        success = (chassis_dance() == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "dance");
    }
    else if (strcmp(action, "set_light_mode") == 0) {
        cJSON *mode_obj = cJSON_GetObjectItem(args, "mode");
        if (mode_obj == NULL || !cJSON_IsNumber(mode_obj)) {
            cJSON_Delete(args);
            cJSON_Delete(result);
            snprintf(output, output_size, "{\"error\":\"Missing or invalid 'mode' parameter\"}");
            return ESP_ERR_INVALID_ARG;
        }

        int mode = cJSON_GetNumberValue(mode_obj);
        if (mode < CHASSIS_LIGHT_OFF || mode > CHASSIS_LIGHT_FLASHLIGHT_OFF) {
            cJSON_Delete(args);
            cJSON_Delete(result);
            snprintf(output, output_size, "{\"error\":\"Invalid light mode\"}");
            return ESP_ERR_INVALID_ARG;
        }

        success = (chassis_set_light_mode((chassis_light_mode_t)mode) == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "set_light_mode");
        cJSON_AddNumberToObject(result, "mode", mode);
    }
    else if (strcmp(action, "get_battery") == 0) {
        int battery_level = 0;
        success = (chassis_get_battery(&battery_level) == ESP_OK);
        cJSON_AddStringToObject(result, "status", success ? "success" : "failed");
        cJSON_AddStringToObject(result, "action", "get_battery");
        cJSON_AddNumberToObject(result, "battery_level", battery_level);
        cJSON_AddStringToObject(result, "battery_level_str", success ? "OK" : "Unknown");
    }
    else {
        cJSON_Delete(args);
        cJSON_Delete(result);
        snprintf(output, output_size, "{\"error\":\"Unknown action\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(args);
    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    if (result_str != NULL) {
        snprintf(output, output_size, "%s", result_str);
        free(result_str);
    } else {
        snprintf(output, output_size, "{\"error\":\"Failed to create result\"}");
    }

    ESP_LOGI(TAG, "Chassis action result: %s", output);
    return ESP_OK;
}
