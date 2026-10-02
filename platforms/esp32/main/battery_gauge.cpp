#include "battery_gauge.h"

#include "board_port.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "battery_gauge";
constexpr std::uint8_t kGaugeAddress = 0x55;
constexpr std::uint32_t kI2cClockHz = 400'000;
constexpr int kI2cTimeoutMs = 100;
constexpr std::uint32_t kGaugeBusFreeUs = 100;
constexpr TickType_t kPollInterval = pdMS_TO_TICKS(5'000);

constexpr std::uint8_t kBatteryStatus = 0x0A;
constexpr std::uint8_t kOperationStatus = 0x3A;
constexpr std::uint8_t kStateOfCharge = 0x2C;
constexpr std::uint8_t kVoltage = 0x08;
constexpr std::uint8_t kCurrent = 0x0C;
constexpr std::uint16_t kBatteryPresent = 1U << 3;
constexpr std::uint16_t kInitializationComplete = 1U << 5;
constexpr std::uint16_t kMinPlausibleVoltageMv = 2'500;
constexpr std::uint16_t kMaxPlausibleVoltageMv = 5'000;

esp_err_t read_word(i2c_master_dev_handle_t device, std::uint8_t command,
                    std::uint16_t& value) {
  std::uint8_t bytes[2]{};
  const esp_err_t result = i2c_master_transmit_receive(
      device, &command, 1, bytes, sizeof(bytes), kI2cTimeoutMs);
  // TI requires at least 66 us between packets addressed to the gauge at
  // 400 kHz. Leave explicit margin before the next register read.
  esp_rom_delay_us(kGaugeBusFreeUs);
  if (result == ESP_OK) {
    value = static_cast<std::uint16_t>(bytes[0]) |
            (static_cast<std::uint16_t>(bytes[1]) << 8);
  }
  return result;
}

esp_err_t read_battery(i2c_master_dev_handle_t device,
                       BatteryReading& reading) {
  std::uint16_t status = 0;
  std::uint16_t operation = 0;
  std::uint16_t percent = 0;
  std::uint16_t voltage = 0;
  std::uint16_t current = 0;

  esp_err_t result = read_word(device, kBatteryStatus, status);
  if (result != ESP_OK) return result;
  result = read_word(device, kOperationStatus, operation);
  if (result != ESP_OK) return result;
  if ((status & kBatteryPresent) == 0 ||
      (operation & kInitializationComplete) == 0) {
    return ESP_ERR_INVALID_STATE;
  }
  result = read_word(device, kStateOfCharge, percent);
  if (result != ESP_OK) return result;
  result = read_word(device, kVoltage, voltage);
  if (result != ESP_OK) return result;
  result = read_word(device, kCurrent, current);
  if (result != ESP_OK) return result;
  if (percent > 100 || voltage < kMinPlausibleVoltageMv ||
      voltage > kMaxPlausibleVoltageMv) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  // The gauge reports current as a signed 16-bit little-endian value.
  const std::int32_t current_ma =
      current < 0x8000U ? static_cast<std::int32_t>(current)
                       : static_cast<std::int32_t>(current) - 0x1'0000;
  reading.available = true;
  reading.percent = static_cast<std::uint8_t>(percent);
  reading.charging = current_ma > 20;
  reading.voltage_mv = voltage;
  return ESP_OK;
}
}  // namespace

esp_err_t BatteryGauge::start(Callback callback, void* context) {
  if (callback == nullptr) return ESP_ERR_INVALID_ARG;
  if (started_) return ESP_ERR_INVALID_STATE;
  callback_ = callback;
  callback_context_ = context;
  if (xTaskCreate(task_entry, "moto_battery", 4'096, this, 1, nullptr) !=
      pdPASS) {
    callback_ = nullptr;
    callback_context_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  started_ = true;
  return ESP_OK;
}

void BatteryGauge::task_entry(void* context) {
  static_cast<BatteryGauge*>(context)->run();
  vTaskDelete(nullptr);
}

void BatteryGauge::run() {
  i2c_master_dev_handle_t device = nullptr;
  bool failure_reported = false;
  TickType_t next_poll = xTaskGetTickCount();

  while (true) {
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (device == nullptr) {
      const i2c_master_bus_handle_t bus = board_port_i2c_get_handle();
      if (bus != nullptr) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = kGaugeAddress;
        config.scl_speed_hz = kI2cClockHz;
        result = i2c_master_bus_add_device(bus, &config, &device);
      }
    }

    BatteryReading reading{};
    if (device != nullptr) result = read_battery(device, reading);
    if (result != ESP_OK && !failure_reported) {
      ESP_LOGW(kTag, "BQ27220 unavailable: %s", esp_err_to_name(result));
      failure_reported = true;
    } else if (result == ESP_OK && failure_reported) {
      ESP_LOGI(kTag, "BQ27220 readings restored");
      failure_reported = false;
    }
    callback_(reading, callback_context_);
    xTaskDelayUntil(&next_poll, kPollInterval);
  }
}
