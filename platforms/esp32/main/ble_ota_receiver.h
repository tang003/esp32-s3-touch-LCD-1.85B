#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

// One sequential OTA transfer at a time. All methods are called by NimBLE's
// host task; write-with-response is the flow-control and durability ACK.
class BleOtaReceiver {
 public:
  BleOtaReceiver();
  BleOtaReceiver(const BleOtaReceiver&) = delete;
  BleOtaReceiver& operator=(const BleOtaReceiver&) = delete;

  esp_err_t initialize();
  esp_err_t begin(std::uint32_t image_size,
                  const std::uint8_t expected_sha256[32]);
  esp_err_t write(std::uint32_t offset, const std::uint8_t* bytes,
                  std::size_t length);
  esp_err_t commit();
  void abort();
  bool active() const noexcept { return active_; }
  bool reboot_pending() const noexcept { return reboot_pending_; }

 private:
  static void reboot_timer_callback(void* argument);

  const esp_partition_t* partition_ = nullptr;
  esp_ota_handle_t handle_ = 0;
  esp_timer_handle_t reboot_timer_ = nullptr;
  mbedtls_sha256_context sha256_{};
  std::uint8_t expected_sha256_[32]{};
  std::uint32_t image_size_ = 0;
  std::uint32_t received_ = 0;
  std::size_t board_marker_prefix_ = 0;
  bool board_marker_found_ = false;
  bool active_ = false;
  bool reboot_pending_ = false;
};
