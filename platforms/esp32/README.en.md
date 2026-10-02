> **Language:** English · [中文](README.md)

> English edition of the Chinese document. The Chinese file is authoritative if the two differ.

# MOTO GPS Waveshare ESP32-S3 firmware

This directory supports two boards. The default configuration targets the user's
**ESP32-S3-Touch-LCD-1.85B**. Check the full model name before building: the display,
touch controller and flash capacity differ.
An earlier 1.85B firmware has been flashed to the physical board. The user confirmed
the display, touch, phone BLE connection and e-bike navigation. The shorter swipe
threshold works, although swiping from the compass page still needs refinement.
The new BOOT page button, battery badge and screen power policy compile locally and await flashing
and on-device verification.

| Board | Display / touch | Resolution | Flash | PSRAM |
| --- | --- | ---: | ---: | ---: |
| ESP32-S3-Touch-LCD-1.85B (default) | ST77916 QSPI / CST816S | 360×360 | 16 MB | 8 MB Octal |
| ESP32-S3-Touch-AMOLED-1.75C | CO5300 QSPI / CST9217 | 466×466 | 32 MB | 8 MB Octal |

Use ESP-IDF **5.5.5** and the pinned LVGL submodule in the repository root. The existing
[complete Waveshare edition DIY guide](../../docs/WAVESHARE_DIY_GUIDE.en.md) and
[user manual](../../docs/USER_MANUAL.en.md) describe the **1.75C**. Their pinout, backup size,
and power-button instructions do not apply to the 1.85B.

## Build without flashing the board

Prepare and activate an ESP-IDF 5.5.5 environment following Espressif's instructions. A fresh
configuration selects **1.85B / 16 MB**. For a 1.75C, first run
`idf.py -C platforms/esp32 menuconfig`, choose 1.75C under `MOTO GPS board`, and set the flash
size to **32 MB** under `Serial flasher config`. When switching back to the 1.85B, restore
**16 MB**. An existing `sdkconfig` retains the previous selection; `sdkconfig.defaults` only
supplies the initial defaults.

Then run this in the repository root:

```sh
git submodule update --init --recursive
idf.py -C platforms/esp32 set-target esp32s3
idf.py -C platforms/esp32 build
```

The first build downloads the Component Manager dependencies; the output is in `platforms/esp32/build/`.
In `dependencies.lock` LVGL is a relative path under the project root; if a local tool rewrites it to
an absolute path, do not commit your personal paths back to the repository. Do not modify
`managed_components` by hand to keep the build going.

## Backup and flashing

Flashing replaces the factory application and requires the device owner's explicit approval. First
confirm the exact model, USB serial port, flash capacity and encryption/secure boot state, then save
a complete factory backup outside the repository. A backup may contain device credentials, so
**do not upload it to GitHub**. The connected 1.85B has already had a 16 MB full-flash backup
saved locally under `E:\Projects\esp32\device-backups\`; back up any other device separately.

PORT and BACKUP_FILE below are placeholders for the actual USB serial port and an out-of-repository
backup path; the commands need the Python environment of an activated IDF.

```sh
python -m esptool --chip esp32s3 --port PORT flash_id
python -m esptool --chip esp32s3 --port PORT get_security_info
# Only for a 1.85B confirmed to be 16 MB with Flash encryption/secure boot disabled:
python -m esptool --chip esp32s3 --port PORT read_flash 0 0x1000000 BACKUP_FILE
# For a 1.75C confirmed to be 32 MB, change the read length to 0x2000000.
```

The syntax above corresponds to esptool 4.x in an ESP-IDF 5.5.5 environment. If you use esptool 5.x
yourself, the subcommands become `flash-id` / `get-security-info` / `read-flash`; rely on that version's
`--help`.
Stop if the security state, capacity or model does not match. The backup must be **16,777,216 bytes**
for a 1.85B or **33,554,432 bytes** for a 1.75C; save a checksum separately. Run the following
only after receiving explicit approval to flash:

```sh
idf.py -C platforms/esp32 -p PORT flash monitor
```

`erase-flash` is not needed; leave the serial log with Ctrl+]. Use only the flash parameters generated
by this local build, and do not apply another board's image offsets. On first use complete the system
Bluetooth pairing in the iPhone.

## Behaviour and limitations

- Advertised name `MOTO GPS`; the iPhone provides positioning, routes, scene and music state over BLE.
- After the black-and-white boot animation it enters the connection page; once connected it waits for
  the phone to select a route and does not draw an empty route as `0 m`.
- Swipe left and right to change pages; the page dots hide after five seconds. Whether the music page
  is available depends on the capabilities the phone declares.
- On the 1.85B, a short BOOT press changes pages while lit; holding it for about 1.5 seconds turns
  off the backlight. When dark, a short BOOT press only wakes the screen. Touch also wakes it, and
  the first touch does not activate a page control.
- The 1.85B does not turn off automatically during active navigation; manual screen-off remains available. With no active navigation, it dims the backlight to
  25% after 60 seconds of inactivity and turns it off after 180 seconds. The board and BLE keep running.
- On the 1.75C, holding PWR for about three seconds requests shutdown from the AXP2101; on USB
  power there is a deep-sleep fallback. The 1.85B's PWR is a hardware power button and does not
  use this AXP2101 behavior.
- The QMI8658 relative angular rate assists turning; the heading while moving is anchored by phone
  positioning, and no absolute north reference at rest is provided.
- On the 1.75C, PSRAM double buffering and CO5300 TE synchronisation reduce tearing; the target
  tick is not a guarantee of the measured sustained frame rate.
- The 1.85B uses one 10-row internal draw buffer. A 50-row PSRAM buffer caused SPI temporary DMA
  memory allocation failures on the physical board.

1.85B key pins: LCD QSPI D0–D3 are GPIO46/45/42/41, PCLK40, CS21, RESET3, backlight5;
I2C SDA11/SCL10, touch INT4/RESET1.
1.75C key pins: QSPI D0–D3 are GPIO4–7, SCLK38, CS12, RESET1, TE13; I2C SDA15/SCL14;
touch INT11/RESET2. Do not treat these pins as free GPIOs you can connect to anything.

Upstream: [Waveshare 1.85B documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B),
[Waveshare 1.75C official project](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C),
[Waveshare BSP](https://github.com/waveshareteam/Waveshare-ESP32-components).
For the known background disconnection and route issues see `docs/KNOWN_ISSUES.md` in the repository
root. A successful build does not mean riding acceptance has passed.
