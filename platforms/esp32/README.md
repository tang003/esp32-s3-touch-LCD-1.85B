> **语言 / Language:** 中文 · [English](README.en.md)

# MOTO GPS 微雪 ESP32-S3 固件

本目录支持以下两款板。当前默认配置是用户手中的 **ESP32-S3-Touch-LCD-1.85B**。
两款板的屏幕、触摸芯片和 Flash 容量不同，编译前应确认板身的完整型号。
1.85B 旧版固件已刷入实机，用户确认画面、触摸、手机蓝牙连接与电动车导航正常。
短滑门槛已实测可用，指南针页滑动仍待优化。本次 BOOT 短按切页、电量角标与屏幕省电策略已在本地编译，尚待刷入实机验证。

| 板型 | 屏幕 / 触摸 | 分辨率 | Flash | PSRAM |
| --- | --- | ---: | ---: | ---: |
| ESP32-S3-Touch-LCD-1.85B（默认） | ST77916 QSPI / CST816S | 360×360 | 16 MB | 8 MB Octal |
| ESP32-S3-Touch-AMOLED-1.75C | CO5300 QSPI / CST9217 | 466×466 | 32 MB | 8 MB Octal |

使用 ESP-IDF **5.5.5** 和根目录固定版本的 LVGL 子模块。旧版的
[微雪版 DIY 完整教程](../../docs/WAVESHARE_DIY_GUIDE.md)与
[功能说明书](../../docs/USER_MANUAL.md)针对 **1.75C**，其中引脚、备份容量和电源按键说明
不能直接用于 1.85B。

## 编译，不会刷板

先按 Espressif 的安装说明准备并激活 ESP-IDF 5.5.5 环境。首次构建默认选择
**1.85B / 16 MB**。若改用 1.75C，先运行 `idf.py -C platforms/esp32 menuconfig`，
在 `MOTO GPS board` 中选 1.75C，并在 `Serial flasher config` 中将 Flash 容量设为
**32 MB**。从 1.75C 切回 1.85B 时，同样要恢复 **16 MB**。已有的 `sdkconfig`
会保留上次选择；`sdkconfig.defaults` 只提供初始默认值。

在仓库根目录执行：

```sh
git submodule update --init --recursive
idf.py -C platforms/esp32 set-target esp32s3
idf.py -C platforms/esp32 build
```

首次构建会下载 Component Manager 依赖，产物在 `platforms/esp32/build/`。
`dependencies.lock` 中 LVGL 是项目根目录下的相对路径，若本地工具重写成绝对路径，
不要把个人路径提交回仓库。不要手动修改 `managed_components` 来维持编译。

固件现在包含出厂应用和两个蓝牙更新分区。**第一次启用更新必须执行完整工程刷写**，
让 bootloader、分区表、应用及初始 OTA 数据一起写入；单独烧录应用 `.bin` 不够。
新版分区表把 `storage` 固定在旧版的 `0x810000`，正常工程刷写不擦除此区。
完成首次 USB 刷写后，后续应用更新可按[蓝牙更新教程](../../docs/OTA_UPDATE.md)从 iPhone“文件”选择 `.bin`。

## 备份和刷写

刷写会替换原厂应用，必须先由设备所有者明确确认。先核对准确板型、USB 串口、
Flash 容量与加密/安全启动状态，再将原厂全片备份保存在仓库外；备份可能包含设备凭证，
**不要上传 GitHub**。本次连接的 1.85B 已在本机 `E:\Projects\esp32\device-backups\`
保存 16 MB 全片备份；更换设备后仍须重新备份。

以下 PORT 和 BACKUP_FILE 是占位符，请换成实际 USB 串口和仓库外的备份文件路径；
命令需要已激活 IDF 的 Python 环境。

```sh
python -m esptool --chip esp32s3 --port PORT flash_id
python -m esptool --chip esp32s3 --port PORT get_security_info
# 仅限已确认 16 MB、未启用 Flash 加密/安全启动的 1.85B：
python -m esptool --chip esp32s3 --port PORT read_flash 0 0x1000000 BACKUP_FILE
# 已确认 32 MB 的 1.75C 应把读取长度改为 0x2000000。
```

上述语法对应 ESP-IDF 5.5.5 环境中的 esptool 4.x。若自行使用 esptool 5.x，子命令
改为 `flash-id` / `get-security-info` / `read-flash`，以该版本 `--help` 为准。
安全状态、容量或型号不符时停止；1.85B 的备份文件应为 **16,777,216 字节**，
1.75C 应为 **33,554,432 字节**，并另存校验值。获得明确刷写确认后才执行：

```sh
idf.py -C platforms/esp32 -p PORT flash monitor
```

不需要 `erase-flash`；串口日志用 Ctrl+] 退出。只使用本机这次编译生成的 flash 参数，
不要套用其他板卡镜像偏移。首次使用在 iPhone 中完成系统蓝牙配对。

## 行为与限制

- 广播名称 `MOTO GPS`；iPhone 通过 BLE 提供定位、路线、场景与音乐状态。
- 开机黑白动画后进入连接页；连接成功后等待手机选路线，不把空路线画成 `0 m`。
- 左右滑动换页，指示点五秒后隐藏。音乐页是否可用取决于手机声明的能力。
- 1.85B：亮屏时短按 BOOT 切页，长按约 1.5 秒熄屏；熄屏时短按 BOOT 仅唤醒。触摸也可唤醒，首次触摸不触发页面操作。
- 1.85B：导航中不会自动熄屏，但可手动熄屏；非导航空闲 60 秒背光降至 25%，空闲 180 秒熄屏。熄屏仅关闭背光，设备与蓝牙仍在运行。
- 1.75C：PWR 持续按住约三秒请求 AXP2101 关机；USB 供电下有深睡兜底。
  1.85B 的 PWR 是板载供电按键，不适用这套 AXP2101 逻辑。
- QMI8658 相对角速度辅助转向；行驶航向由手机定位锚定，不提供静止绝对北向。
- 1.75C 使用 PSRAM 双缓冲和 CO5300 TE 同步降低撕裂；目标节拍不等于实测持续帧率保证。
- 1.85B 使用单个 10 行片内绘图缓冲；实机上 50 行 PSRAM 缓冲触发 SPI 临时 DMA 内存不足。

1.85B 关键引脚：LCD QSPI D0–D3 为 GPIO46/45/42/41，PCLK40、CS21、RESET3、
背光5；I2C SDA11/SCL10，触摸 INT4/RESET1。
1.75C 关键引脚：QSPI D0–D3 为 GPIO4–7，SCLK38、CS12、RESET1、TE13；
I2C SDA15/SCL14，触摸 INT11/RESET2。不要把这些引脚当作可任意外接的空闲 GPIO。

上游：[微雪 1.85B 官方资料](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B)、
[微雪 1.75C 官方工程](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)、
[Waveshare BSP](https://github.com/waveshareteam/Waveshare-ESP32-components)。
已知后台断连和路线问题见根目录 `docs/KNOWN_ISSUES.md`。构建成功不等于骑行验收通过。
