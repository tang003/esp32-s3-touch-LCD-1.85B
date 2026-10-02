#include "ble_ota_receiver.h"

#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "moto_ota";
constexpr std::uint64_t kRebootDelayUs = 1'000'000;
// The ESP image header, first segment header and app descriptor must fit.
constexpr std::uint32_t kMinimumImageSize = 32U + sizeof(esp_app_desc_t);
#if CONFIG_MOTO_BOARD_WAVESHARE_1_85B
constexpr char kBoardMarker[] = "MOTO_OTA_TARGET_1_85B_V1";
#elif CONFIG_MOTO_BOARD_WAVESHARE_1_75C
constexpr char kBoardMarker[] = "MOTO_OTA_TARGET_1_75C_V1";
#else
#error "An OTA firmware build must select an exact Waveshare board"
#endif
constexpr std::size_t kBoardMarkerLength = sizeof(kBoardMarker) - 1U;
}  // namespace

BleOtaReceiver::BleOtaReceiver() { mbedtls_sha256_init(&sha256_); }

esp_err_t BleOtaReceiver::initialize() {
  if (reboot_timer_ != nullptr) return ESP_OK;
  esp_timer_create_args_t timer_args{};
  timer_args.callback = reboot_timer_callback;
  timer_args.arg = this;
  timer_args.dispatch_method = ESP_TIMER_TASK;
  timer_args.name = "moto_ota_reboot";
  return esp_timer_create(&timer_args, &reboot_timer_);
}

esp_err_t BleOtaReceiver::begin(
    std::uint32_t image_size, const std::uint8_t expected_sha256[32]) {
  if (expected_sha256 == nullptr || reboot_timer_ == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (active_ || reboot_pending_) return ESP_ERR_INVALID_STATE;

  const esp_partition_t* const partition =
      esp_ota_get_next_update_partition(nullptr);
  if (partition == nullptr || image_size < kMinimumImageSize ||
      image_size > partition->size) {
    ESP_LOGW(kTag, "image size %lu is invalid for OTA partition",
             static_cast<unsigned long>(image_size));
    return ESP_ERR_INVALID_SIZE;
  }

  // Sequential mode erases one flash sector as the corresponding bytes
  // arrive. Erasing the whole image in NimBLE's GATT callback can exceed the
  // Bluetooth supervision timeout before BEGIN receives its ATT response.
  esp_ota_handle_t handle = 0;
  const esp_err_t result = esp_ota_begin(
      partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "OTA begin failed: %s", esp_err_to_name(result));
    return result;
  }

  mbedtls_sha256_free(&sha256_);
  mbedtls_sha256_init(&sha256_);
  if (mbedtls_sha256_starts(&sha256_, 0) != 0) {
    esp_ota_abort(handle);
    return ESP_FAIL;
  }

  partition_ = partition;
  handle_ = handle;
  image_size_ = image_size;
  received_ = 0;
  board_marker_prefix_ = 0;
  board_marker_found_ = false;
  std::memcpy(expected_sha256_, expected_sha256,
              sizeof(expected_sha256_));
  active_ = true;
  ESP_LOGI(kTag, "receiving %lu bytes into %s at 0x%lx",
           static_cast<unsigned long>(image_size_), partition_->label,
           static_cast<unsigned long>(partition_->address));
  ESP_LOGI(kTag, "requiring board marker %s", kBoardMarker);
  return ESP_OK;
}

esp_err_t BleOtaReceiver::write(std::uint32_t offset,
                                 const std::uint8_t* bytes,
                                 std::size_t length) {
  if (!active_ || bytes == nullptr || length == 0 ||
      offset != received_ || length > image_size_ - received_) {
    return ESP_ERR_INVALID_ARG;
  }

  const esp_err_t result = esp_ota_write(handle_, bytes, length);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "OTA flash write failed at %lu: %s",
             static_cast<unsigned long>(received_), esp_err_to_name(result));
    abort();
    return result;
  }
  if (mbedtls_sha256_update(&sha256_, bytes, length) != 0) {
    abort();
    return ESP_FAIL;
  }
  // This marker is compiled into each board-specific application image.
  // Scanning the incoming stream also handles the marker crossing BLE writes.
  // It prevents an accidental 1.75C image from being booted on a 1.85B.
  if (!board_marker_found_) {
    for (std::size_t i = 0; i < length; ++i) {
      if (bytes[i] == static_cast<std::uint8_t>(
                          kBoardMarker[board_marker_prefix_])) {
        ++board_marker_prefix_;
        if (board_marker_prefix_ == kBoardMarkerLength) {
          board_marker_found_ = true;
          break;
        }
      } else {
        board_marker_prefix_ = bytes[i] ==
                                       static_cast<std::uint8_t>(
                                           kBoardMarker[0])
                                   ? 1U
                                   : 0U;
      }
    }
  }
  received_ += static_cast<std::uint32_t>(length);
  return ESP_OK;
}

esp_err_t BleOtaReceiver::commit() {
  if (!active_ || received_ != image_size_) return ESP_ERR_INVALID_STATE;

  if (!board_marker_found_) {
    ESP_LOGE(kTag, "OTA image does not match this Waveshare board");
    abort();
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  std::uint8_t digest[32]{};
  if (mbedtls_sha256_finish(&sha256_, digest) != 0 ||
      std::memcmp(digest, expected_sha256_, sizeof(digest)) != 0) {
    ESP_LOGE(kTag, "OTA file SHA-256 mismatch");
    abort();
    return ESP_ERR_INVALID_CRC;
  }

  const esp_partition_t* const completed_partition = partition_;
  const esp_ota_handle_t completed_handle = handle_;
  active_ = false;
  handle_ = 0;
  partition_ = nullptr;
  received_ = 0;
  image_size_ = 0;
  board_marker_prefix_ = 0;
  board_marker_found_ = false;
  mbedtls_sha256_free(&sha256_);
  mbedtls_sha256_init(&sha256_);

  // This validates the ESP image, including the chip target, before the
  // bootloader is asked to run it. esp_ota_end always releases the handle.
  esp_err_t result = esp_ota_end(completed_handle);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "OTA image validation failed: %s",
             esp_err_to_name(result));
    return result;
  }

  esp_app_desc_t incoming{};
  result = esp_ota_get_partition_description(completed_partition, &incoming);
  const esp_app_desc_t* const running = esp_app_get_description();
  if (result != ESP_OK || running == nullptr ||
      std::strncmp(incoming.project_name, running->project_name,
                   sizeof(incoming.project_name)) != 0) {
    ESP_LOGE(kTag, "OTA image is not a MOTO GPS firmware build");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  result = esp_ota_set_boot_partition(completed_partition);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "could not select OTA boot partition: %s",
             esp_err_to_name(result));
    return result;
  }

  // Let NimBLE send the successful ATT write response before restarting.
  reboot_pending_ = true;
  result = esp_timer_start_once(reboot_timer_, kRebootDelayUs);
  if (result != ESP_OK) {
    reboot_pending_ = false;
    const esp_partition_t* const current = esp_ota_get_running_partition();
    if (current != nullptr) esp_ota_set_boot_partition(current);
    ESP_LOGE(kTag, "could not schedule OTA reboot: %s",
             esp_err_to_name(result));
    return result;
  }
  ESP_LOGI(kTag, "OTA image accepted; rebooting in one second");
  return ESP_OK;
}

void BleOtaReceiver::abort() {
  if (!active_) return;
  ESP_LOGW(kTag, "aborting incomplete OTA at %lu/%lu bytes",
           static_cast<unsigned long>(received_),
           static_cast<unsigned long>(image_size_));
  esp_ota_abort(handle_);
  active_ = false;
  handle_ = 0;
  partition_ = nullptr;
  image_size_ = 0;
  received_ = 0;
  board_marker_prefix_ = 0;
  board_marker_found_ = false;
  mbedtls_sha256_free(&sha256_);
  mbedtls_sha256_init(&sha256_);
}

void BleOtaReceiver::reboot_timer_callback(void*) { esp_restart(); }
