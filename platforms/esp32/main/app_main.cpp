#include <algorithm>
#include <cstdint>

#include "battery_gauge.h"
#include "ble_nav_transport_nimble.h"
#include "board_port.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "moto_nav_presenter.hpp"
#include "moto_nav_ui.h"
#include "motion_heading_sensor.h"
#include "nvs.h"
#include "phone_nav_bridge.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "moto_gps";
constexpr std::uint64_t kPowerHoldMs = 3'000;
constexpr std::uint64_t kPageDebounceMs = 40;
constexpr std::uint64_t kPageShortPressMaxMs = 900;
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
constexpr std::uint64_t kScreenHoldMs = 1'500;
constexpr std::uint8_t kScreenDimPercent = 25;
constexpr char kDisplaySettingsNamespace[] = "moto_display";
constexpr char kBrightnessKey[] = "brightness";
constexpr char kScreenOffKey[] = "off_minutes";

bool valid_brightness(std::uint8_t value) {
  return value == 25 || value == 50 || value == 75 || value == 100;
}

bool valid_screen_off(std::uint8_t value) {
  return value == 0 || value == 1 || value == 3 || value == 5;
}

moto::ble::DeviceSettings load_device_settings() {
  moto::ble::DeviceSettings settings;
  nvs_handle_t handle = 0;
  if (nvs_open(kDisplaySettingsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return settings;
  }
  std::uint8_t value = 0;
  if (nvs_get_u8(handle, kBrightnessKey, &value) == ESP_OK &&
      valid_brightness(value)) {
    settings.brightness_percent = value;
  }
  if (nvs_get_u8(handle, kScreenOffKey, &value) == ESP_OK &&
      valid_screen_off(value)) {
    settings.screen_off_minutes = value;
  }
  nvs_close(handle);
  return settings;
}

bool persist_device_settings(const moto::ble::DeviceSettings& settings,
                             void*) {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kDisplaySettingsNamespace, NVS_READWRITE,
                              &handle);
  if (result == ESP_OK) {
    result = nvs_set_u8(handle, kBrightnessKey,
                        settings.brightness_percent);
    if (result == ESP_OK) {
      result = nvs_set_u8(handle, kScreenOffKey,
                          settings.screen_off_minutes);
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "display settings could not be saved: %s",
             esp_err_to_name(result));
  }
  return result == ESP_OK;
}
#endif

moto::ble::AckStatus receive_phone_message(
    const moto::ble::ReassembledMessage& message, void* context) {
  return static_cast<PhoneNavBridge*>(context)->on_message(message);
}

void update_phone_link(bool active, void* context) {
  static_cast<PhoneNavBridge*>(context)->on_link_state(active);
}

void update_phone_ready(void* context) {
  static_cast<PhoneNavBridge*>(context)->on_protocol_ready();
}

bool allow_ota_while_idle(void* context) {
  return !static_cast<PhoneNavBridge*>(context)->navigation_active();
}

void update_motion_heading(float heading_rate_dps, std::uint64_t sample_ms,
                           void* context) {
  static_cast<PhoneNavBridge*>(context)->on_imu_sample(heading_rate_dps,
                                                       sample_ms);
}

void demo_tick_task(void* context) {
  auto* bridge = static_cast<PhoneNavBridge*>(context);
  while (true) {
    const auto now_ms = static_cast<std::uint64_t>(esp_timer_get_time()) /
                        1'000U;
    bridge->update_demo(now_ms);
    // Match the display and route interpolation at 40 Hz. Keeping all three
    // clocks phase-compatible avoids periodic long frame gaps.
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void power_button_task(void* context) {
  auto* transport = static_cast<BleNavTransport*>(context);
  std::uint64_t pressed_since_ms = 0;
  // The AXP2101 needs roughly a one-second press to power the board on, so
  // the task usually starts while the user is still holding PWR. Arm the
  // hold-to-shutdown detector only after the line has been seen low once;
  // otherwise the tail of the power-on press is counted as a new 3-second
  // hold and the freshly booted unit switches itself off.
  bool released_once = false;
  while (true) {
    const std::uint64_t now_ms =
        static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
    if (!board_port_power_button_pressed()) {
      if (!released_once) {
        released_once = true;
        ESP_LOGI(kTag, "PWR ready: button released, hold detection armed");
      }
      pressed_since_ms = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!released_once) {
      // Startup press still in progress; do not start the hold timer.
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (transport != nullptr && transport->ota_update_in_progress()) {
      // Do not execute software power-off during a firmware transfer. The
      // bootloader will keep the old image if power is lost regardless.
      pressed_since_ms = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (pressed_since_ms == 0) {
      pressed_since_ms = now_ms;
      ESP_LOGI(kTag, "PWR press started");
    } else if (now_ms - pressed_since_ms >= kPowerHoldMs) {
      ESP_LOGI(kTag, "PWR held for 3 seconds; requesting shutdown");
      if (board_port_lock(UINT32_MAX)) {
        moto_nav_ui_show_power_off_screen();
        board_port_unlock();
      }
      vTaskDelay(pdMS_TO_TICKS(300));
      const esp_err_t result = board_port_power_off();
      if (result != ESP_OK) {
        ESP_LOGE(kTag, "AXP2101 software power-off failed: %s",
                 esp_err_to_name(result));
      }

      // AXP2101 should remove the switched rails here. If this particular
      // board remains alive while USB is attached, blank the panel and enter
      // deep sleep after key release. GPIO3 high wakes it on the next press.
      vTaskDelay(pdMS_TO_TICKS(500));
      while (board_port_power_button_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
          1ULL << 3, ESP_EXT1_WAKEUP_ANY_HIGH));
      esp_deep_sleep_start();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
void update_battery(const BatteryReading& reading, void*) {
  moto_battery_state_t state{};
  state.available = reading.available ? 1U : 0U;
  state.percent = reading.percent;
  state.charging = reading.charging ? 1U : 0U;
  if (board_port_lock(100)) {
    moto_nav_ui_set_battery_state(&state);
    board_port_unlock();
  }
}
#endif

void page_button_task(void* context) {
  auto* bridge = static_cast<PhoneNavBridge*>(context);
  bool last_raw_pressed = board_port_page_button_pressed();
  bool stable_pressed = last_raw_pressed;
  bool released_once = !stable_pressed;
  std::uint64_t last_change_ms =
      static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
  std::uint64_t pressed_since_ms = 0;
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
  bool press_consumed = false;
  bool manual_dark = false;
  bool was_navigating = bridge->navigation_active();
  std::uint8_t brightness = 100;
  std::uint32_t last_interaction_ms =
      static_cast<std::uint32_t>(last_change_ms);
  std::uint32_t last_touch_ms = board_port_last_touch_ms();
  std::uint32_t last_brightness_failure_ms = 0;
  auto set_brightness = [&](std::uint8_t percent, std::uint32_t now_ms) {
    if (brightness == percent ||
        (last_brightness_failure_ms != 0 &&
         now_ms - last_brightness_failure_ms < 1'000U)) {
      return;
    }
    const esp_err_t result = board_port_set_display_brightness(percent);
    if (result == ESP_OK) {
      brightness = percent;
      last_brightness_failure_ms = 0;
      ESP_LOGI(kTag, "screen brightness %u%%",
               static_cast<unsigned>(percent));
    } else {
      last_brightness_failure_ms = now_ms;
      ESP_LOGW(kTag, "screen brightness change failed: %s",
               esp_err_to_name(result));
    }
  };
#endif
  while (true) {
    const std::uint64_t now_ms =
        static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
    const std::uint32_t now32 = static_cast<std::uint32_t>(now_ms);
    const moto::ble::DeviceSettings device_settings = bridge->device_settings();
    const std::uint8_t awake_brightness = device_settings.brightness_percent;
    const std::uint32_t touch_ms = board_port_last_touch_ms();
    if (touch_ms != last_touch_ms) {
      last_touch_ms = touch_ms;
      last_interaction_ms = touch_ms;
    }
    if (board_port_take_touch_wake_request()) {
      manual_dark = false;
      last_interaction_ms = now32;
      set_brightness(awake_brightness, now32);
    }
#endif
    const bool raw_pressed = board_port_page_button_pressed();
    if (raw_pressed != last_raw_pressed) {
      last_raw_pressed = raw_pressed;
      last_change_ms = now_ms;
    }
    if (raw_pressed != stable_pressed &&
        now_ms - last_change_ms >= kPageDebounceMs) {
      stable_pressed = raw_pressed;
      if (stable_pressed) {
        if (released_once) {
          pressed_since_ms = now_ms;
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
          press_consumed = false;
          last_interaction_ms = now32;
          if (brightness == 0) {
            // Waking from a short press must not also change the page.
            manual_dark = false;
            last_interaction_ms = now32;
            set_brightness(awake_brightness, now32);
            press_consumed = true;
          }
#endif
        }
      } else {
        if (released_once && pressed_since_ms != 0 &&
            now_ms - pressed_since_ms <= kPageShortPressMaxMs
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
            && !press_consumed
#endif
        ) {
          bridge->select_next_page();
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
          last_interaction_ms = now32;
          set_brightness(awake_brightness, now32);
#endif
        }
        released_once = true;
        pressed_since_ms = 0;
      }
    }
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
    if (stable_pressed && released_once && pressed_since_ms != 0 &&
        !press_consumed && now_ms - pressed_since_ms >= kScreenHoldMs) {
      manual_dark = true;
      set_brightness(0, now32);
      press_consumed = true;
    }

    const bool navigating = bridge->navigation_active();
    if (navigating && !was_navigating) {
      // A newly started route should be visible even after manual screen-off.
      manual_dark = false;
      last_interaction_ms = now32;
    }
    if (navigating || was_navigating) last_interaction_ms = now32;
    was_navigating = navigating;
    std::uint8_t desired_brightness = awake_brightness;
    if (manual_dark) {
      desired_brightness = 0;
    } else if (!navigating && device_settings.screen_off_minutes != 0) {
      const std::uint32_t idle_ms = now32 - last_interaction_ms;
      const std::uint32_t screen_off_after_ms =
          static_cast<std::uint32_t>(device_settings.screen_off_minutes) *
          60'000U;
      if (idle_ms >= screen_off_after_ms) {
        desired_brightness = 0;
      } else if (idle_ms >= screen_off_after_ms / 3U) {
        desired_brightness =
            std::min(awake_brightness, kScreenDimPercent);
      }
    }
    set_brightness(desired_brightness, now32);
#endif
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void rollback_pending_ota_on_startup_failure() {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  const esp_partition_t* const running = esp_ota_get_running_partition();
  esp_ota_img_states_t image_state{};
  if (running == nullptr ||
      esp_ota_get_state_partition(running, &image_state) != ESP_OK ||
      image_state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }
  ESP_LOGE(kTag, "updated app failed startup; rolling back now");
  const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
  ESP_LOGE(kTag, "could not roll back updated app: %s",
           esp_err_to_name(result));
#endif
}

void stop_after_fatal_startup_error() {
  rollback_pending_ota_on_startup_failure();
  vTaskDelete(nullptr);
}
} // namespace

extern "C" void app_main(void) {
  static_assert(MOTO_DISPLAY_WIDTH == MOTO_UI_CANVAS_WIDTH &&
                    MOTO_DISPLAY_HEIGHT == MOTO_UI_CANVAS_HEIGHT,
                "The board and shared UI canvas must have one resolution");
  static_assert(LV_COLOR_DEPTH == MOTO_DISPLAY_BITS_PER_PIXEL,
                "Firmware and Web must both build the shared UI as RGB565");

  ESP_LOGI(kTag, "starting %dx%d RGB565 firmware target",
           MOTO_DISPLAY_WIDTH, MOTO_DISPLAY_HEIGHT);

  const esp_err_t init_result = board_port_init();
  if (init_result != ESP_OK) {
    ESP_LOGE(kTag, "board initialization stopped before shared UI startup: %s",
             esp_err_to_name(init_result));
    stop_after_fatal_startup_error();
    return;
  }

  lv_display_t *const display = board_port_get_display();
  if (display == nullptr) {
    ESP_LOGE(kTag, "board port returned no LVGL display");
    stop_after_fatal_startup_error();
    return;
  }

  if (lv_display_get_horizontal_resolution(display) != MOTO_DISPLAY_WIDTH ||
      lv_display_get_vertical_resolution(display) != MOTO_DISPLAY_HEIGHT ||
      lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) {
    ESP_LOGE(kTag, "board display violates the shared RGB565 portability "
                   "contract");
    stop_after_fatal_startup_error();
    return;
  }

  if (!board_port_lock(UINT32_MAX)) {
    ESP_LOGE(kTag, "could not acquire LVGL lock");
    stop_after_fatal_startup_error();
    return;
  }

  moto_nav_ui_show_boot_screen();
  // Commit the deliberately black first animation frame while the physical
  // panel is still hidden, then reveal it.  This removes the white frame that
  // used to leak from LVGL's default startup screen.
  lv_refr_now(display);
  const esp_err_t reveal_result = board_port_reveal_display();
  if (reveal_result != ESP_OK) {
    ESP_LOGE(kTag, "could not reveal boot animation: %s",
             esp_err_to_name(reveal_result));
  }
  board_port_unlock();

  // Give the display worker enough time to commit the monochrome power-on
  // frame before constructing the full production UI.
  vTaskDelay(pdMS_TO_TICKS(1'250));

  if (!board_port_lock(UINT32_MAX)) {
    ESP_LOGE(kTag, "could not reacquire LVGL lock after boot screen");
    stop_after_fatal_startup_error();
    return;
  }

  moto_nav_ui_create();
  static moto::nav::NavPresenter presenter;
  static PhoneNavBridge phone_bridge(presenter);
  static BleNavTransport transport;
  // NavSnapshot contains the complete bounded roads/buildings window and is
  // several kilobytes.  A temporary here inflates app_main's stack frame for
  // the entire display/BSP startup call chain, which can trip the FreeRTOS
  // main-task canary before this line is even reached.  Keep the immutable
  // initial state in static storage just like the bridge's retained snapshots.
  static const moto::nav::NavSnapshot initial_snapshot{};
  presenter.update(initial_snapshot);
  presenter.apply_to_lvgl();
  moto_nav_ui_set_music_page_enabled(0);
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
  moto_nav_ui_set_settings_page_enabled(1);
  phone_bridge.set_settings_page_enabled(true);
  phone_bridge.set_settings_persist_callback(persist_device_settings,
                                              nullptr);
#endif
  phone_bridge.install_ui_callbacks();
  board_port_unlock();

  if (!phone_bridge.start_renderer()) {
    ESP_LOGE(kTag, "UI renderer startup failed; BLE was not started");
    stop_after_fatal_startup_error();
    return;
  }

  phone_bridge.set_sender(BleNavTransport::send_from_bridge, &transport);
  transport.set_callbacks(receive_phone_message, update_phone_link,
                          update_phone_ready, &phone_bridge,
                          allow_ota_while_idle);
  const esp_err_t ble_result = transport.start();
  if (ble_result != ESP_OK) {
    ESP_LOGE(kTag, "BLE startup failed; display remains in offline mode: %s",
             esp_err_to_name(ble_result));
  }
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
  // BLE startup initializes NVS. The device owns the saved value and sends it
  // to the phone after the encrypted session reaches Ready.
  phone_bridge.restore_device_settings(load_device_settings());
#endif

  static MotionHeadingSensor motion_sensor;
  const esp_err_t motion_result = motion_sensor.start(update_motion_heading,
                                                      &phone_bridge);
  if (motion_result != ESP_OK) {
    ESP_LOGW(kTag, "QMI8658 heading assist could not start: %s",
             esp_err_to_name(motion_result));
  }

  // Demo generation fills the retained snapshot in place, but geometry and
  // LVGL projection still use deeper C++ call frames than a trivial task.
  if (xTaskCreate(demo_tick_task, "moto_demo", 6'144, &phone_bridge, 2,
                  nullptr) != pdPASS) {
    ESP_LOGW(kTag, "navigation demo task could not start");
  }
  if (board_port_has_power_button()) {
    if (xTaskCreate(power_button_task, "moto_power", 3'072, &transport, 3,
                    nullptr) != pdPASS) {
      ESP_LOGW(kTag, "PWR long-hold task could not start");
    }
  }
  bool start_screen_task = board_port_has_page_button();
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
  // Idle dimming and touch wake still work if BOOT GPIO initialization fails.
  start_screen_task = true;
#endif
  if (start_screen_task) {
    if (xTaskCreate(page_button_task, "moto_page_button", 3'072,
                     &phone_bridge, 3, nullptr) != pdPASS) {
      ESP_LOGW(kTag, "screen and BOOT control task could not start");
    }
  }
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
  static BatteryGauge battery_gauge;
  const esp_err_t battery_result = battery_gauge.start(update_battery, nullptr);
  if (battery_result != ESP_OK) {
    ESP_LOGW(kTag, "battery gauge task could not start: %s",
             esp_err_to_name(battery_result));
  }
#endif

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  // A freshly updated slot is provisional until the display and BLE have
  // both initialized. Confirm only a healthy boot so the bootloader can
  // restore the previous image after a failed startup.
  if (reveal_result != ESP_OK || ble_result != ESP_OK) {
    rollback_pending_ota_on_startup_failure();
  } else {
    const esp_partition_t* const running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state{};
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY) {
      const esp_err_t confirmed = esp_ota_mark_app_valid_cancel_rollback();
      if (confirmed != ESP_OK) {
        ESP_LOGE(kTag, "could not confirm healthy OTA boot: %s",
                 esp_err_to_name(confirmed));
      }
    }
  }
#endif

  ESP_LOGI(kTag,
           "iPhone BLE + QMI heading -> NavPresenter -> shared LVGL running");
  vTaskDelete(nullptr);
}
