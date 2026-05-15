/*
 * Solar Energy Click integration helpers.
 */

#ifndef SOLAR_CLICK_H_
#define SOLAR_CLICK_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*solar_click_vbat_ok_handler_t)(bool low_battery, int level, void *user_data);

int solar_click_init(void);
int solar_click_set_vout_enabled(bool enable);
int solar_click_set_en(bool enable);
int solar_click_get_vbat_ok_level(int *level);
int solar_click_register_vbat_ok_handler(solar_click_vbat_ok_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* SOLAR_CLICK_H_ */
