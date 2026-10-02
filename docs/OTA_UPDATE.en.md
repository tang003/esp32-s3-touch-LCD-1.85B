# Bluetooth firmware updates for the 1.85B display

This feature is for the **Waveshare ESP32-S3-Touch-LCD-1.85B** and a compatible MOTO GPS iPhone app. Select the `moto_gps_esp32.bin` application image built for the **1.85B**. An iPhone `.ipa` is not a device firmware image.

## One-time USB migration

The previous firmware has one application partition and no Bluetooth update service. The first upgrade must therefore flash the new **bootloader, partition table and application together over USB**. Flashing the application `.bin` alone does not enable Bluetooth updates. Confirm the board and flash size and retain a full-flash backup as described in the [firmware guide](../platforms/esp32/README.en.md#backup-and-flashing). Do not run `erase-flash`; the new layout keeps the existing map-storage partition at its original offset.

Subsequent application updates can use the iPhone without a USB connection to the display. They do not need the navigation gateway or display Wi-Fi.

## Updating from iPhone

1. Build the firmware with the **1.85B / 16 MB** configuration. The application image is `platforms/esp32/build/moto_gps_esp32.bin`. Do not choose `bootloader.bin`, `partition-table.bin`, a full-flash backup or a build for another board.
2. Copy the `.bin` to the iPhone Files app by a trusted method.
3. Give the display stable power, open MOTO GPS and connect to the display. In “My Display”, choose firmware update and select the `.bin` from Files.
4. Perform the update while stopped. Keep the app in the foreground and the phone close to the display until the transfer finishes, then wait for the display to restart and reconnect.

The app checks the image format, project identity and slot-size limit. The device checks the received length, SHA-256 and ESP application image before selecting it for boot. SHA-256 detects corruption but is **not a publisher signature**. This first version is intended for personal testing; commercial distribution needs signed firmware, secure boot and a stronger update authorization design.

If the link breaks or validation fails, reconnect and retry the transfer from the beginning. The previous application should remain bootable. ESP-IDF rollback should recover a previous version if a new one fails to start. The USB partition migration and rollback still need physical-device validation; keep the full-flash backup for USB recovery.

