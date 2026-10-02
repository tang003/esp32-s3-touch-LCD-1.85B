#pragma once

#include <cstdint>

#include "esp_err.h"

struct BatteryReading {
  bool available = false;
  std::uint8_t percent = 0;
  bool charging = false;
  std::uint16_t voltage_mv = 0;
};

class BatteryGauge {
 public:
  using Callback = void (*)(const BatteryReading& reading, void* context);

  BatteryGauge() = default;
  BatteryGauge(const BatteryGauge&) = delete;
  BatteryGauge& operator=(const BatteryGauge&) = delete;

  // Read the onboard gauge in a background task. The callback runs on that
  // task, so callers must hand UI changes to the render task or lock LVGL.
  esp_err_t start(Callback callback, void* context);

 private:
  static void task_entry(void* context);
  void run();

  Callback callback_ = nullptr;
  void* callback_context_ = nullptr;
  bool started_ = false;
};
