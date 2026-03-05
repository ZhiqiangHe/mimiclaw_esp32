#ifndef CHASSIS_DRIVER_H
#define CHASSIS_DRIVER_H

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHASSIS_DIR_FORWARD = 0,
    CHASSIS_DIR_BACKWARD,
    CHASSIS_DIR_LEFT,
    CHASSIS_DIR_RIGHT,
    CHASSIS_DIR_STOP,
    CHASSIS_DIR_DANCE
} chassis_direction_t;

typedef enum {
    CHASSIS_LIGHT_OFF = 0,
    CHASSIS_LIGHT_MODE_1,
    CHASSIS_LIGHT_MODE_2,
    CHASSIS_LIGHT_MODE_3,
    CHASSIS_LIGHT_MODE_4,
    CHASSIS_LIGHT_MODE_5,
    CHASSIS_LIGHT_MODE_6,
    CHASSIS_LIGHT_MODE_7,
    CHASSIS_LIGHT_MODE_8,
    CHASSIS_LIGHT_FLASHLIGHT_ON,
    CHASSIS_LIGHT_FLASHLIGHT_OFF
} chassis_light_mode_t;

esp_err_t chassis_driver_init(void);
void chassis_driver_deinit(void);

esp_err_t chassis_move(chassis_direction_t direction);
esp_err_t chassis_stop(void);
esp_err_t chassis_dance(void);

esp_err_t chassis_set_light_mode(chassis_light_mode_t mode);
esp_err_t chassis_get_battery(int *battery_level);

#ifdef __cplusplus
}
#endif

#endif
