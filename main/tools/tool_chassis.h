#ifndef TOOL_CHASSIS_H
#define TOOL_CHASSIS_H

#include "tool_registry.h"
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tool_chassis_init(void);
esp_err_t tool_chassis_execute(const char *input_json, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif
