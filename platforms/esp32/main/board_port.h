#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#else
// Host-side bridge checks do not use the ESP-IDF bus implementation.
typedef void *i2c_master_bus_handle_t;
#endif
#include "esp_err.h"
#include "lvgl.h"
#include "moto_nav_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MOTO_DISPLAY_WIDTH = MOTO_UI_CANVAS_WIDTH,
  MOTO_DISPLAY_HEIGHT = MOTO_UI_CANVAS_HEIGHT,
  MOTO_DISPLAY_BITS_PER_PIXEL = 16,
};

/**
 * Initialize only board/platform facilities required by the shared UI.
 *
 * A real implementation must:
 * - initialize LVGL exactly once;
 * - create one display matching MOTO_UI_CANVAS_WIDTH x
 *   MOTO_UI_CANVAS_HEIGHT using LV_COLOR_FORMAT_RGB565;
 * - attach the physical panel flush callback and compatible draw buffers;
 * - provide LVGL's millisecond tick;
 * - run lv_timer_handler from one board-owned FreeRTOS task; and
 * - serialize all other LVGL access through board_port_lock/unlock.
 *
 * It must not contain route parsing, navigation, rerouting, or UI layout logic.
 */
esp_err_t board_port_init(void);

/** Return the initialized LVGL display, or NULL before successful init. */
lv_display_t *board_port_get_display(void);

/**
 * Reveal the panel after the caller has drawn the first intentional frame.
 * board_port_init keeps the panel dark so the LVGL default white screen can
 * never appear between panel reset and the boot animation.
 */
esp_err_t board_port_reveal_display(void);

/**
 * Lock LVGL for calls made outside the board-owned LVGL task.
 * timeout_ms == UINT32_MAX means wait indefinitely.
 */
bool board_port_lock(uint32_t timeout_ms);

/** Release a lock previously acquired with board_port_lock. */
void board_port_unlock(void);

/** Return whether the application-visible case PWR key is held. */
bool board_port_power_button_pressed(void);

/** Return whether this board exposes a case power button to the application. */
bool board_port_has_power_button(void);

/** Return the initialized shared I2C bus used by the onboard QMI8658. */
i2c_master_bus_handle_t board_port_i2c_get_handle(void);

/**
 * Request board-specific software power-off, if supported.
 */
esp_err_t board_port_power_off(void);

#ifdef __cplusplus
}
#endif
