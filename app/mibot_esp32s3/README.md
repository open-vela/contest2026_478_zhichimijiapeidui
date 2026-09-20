# Mibot ESP32-S3（ESP-IDF）

本目录现在使用原生 ESP-IDF 构建，不依赖 Arduino IDE、Arduino Core 或 ArduinoJson。

## 文件

- `main/mibot_controller.cpp`：ESP32-S3 主控制器，使用 ESP-IDF UART、I2C、LEDC、FreeRTOS 和 cJSON。
- `main/idf_component.yml`：通过 ESP-IDF Component Manager 引入 `espressif/cjson`（ESP-IDF v6.1 不再提供旧的内置 `json` 组件名）。
- `main/mibot_config.h`：板级 GPIO、UART、ToF 地址和安全参数配置。
- `ESP32S3_BOARD_REFERENCE.md`：已核对的开发板原理图、模组规格、GPIO 分配和 Flash/PSRAM 注意事项备份。
- `WIFI_LED_TEST.md`：电脑通过 ESP32 SoftAP/TCP 使用协议帧控制板载 WS2812B 的联调说明。
- `ACTION_API.md`：动作接口、云端调用格式、SF32 LCD 事件和预置动作定义。
- ESP32 GPIO48 RGB 也可用于 UART 联调：蓝色发送、黄色收到字节、绿色合法帧、红色 CRC/协议错误；SF32 端应对收到的 `HELLO_ACK`/`PONG`/`ACK` 点亮绿色确认。
- `tools/wifi_led_test.py`：零依赖电脑端测试脚本，支持 `on`、`off`、`toggle`。
- `wifi_led_test.cmd`：使用本机 ESP-IDF Python 启动电脑端测试脚本。
- `build_local.cmd`：本机 ESP-IDF 6.1 工具路径不在默认位置时使用的构建脚本。
- `flash_monitor.cmd`：自动加载 ESP-IDF 环境并烧录、打开串口监视器，默认使用 `COM6`。
- `CMakeLists.txt` / `main/CMakeLists.txt`：ESP-IDF 构建配置。
- `mibot_esp32s3.ino`：之前的 Arduino 版本，仅作参考，不参与 `idf.py build`。

## Wi-Fi 电脑联调

当前固件会连接家庭 Wi-Fi `YOUR_WIFI_SSID`，由路由器分配地址，并在 TCP 端口 `3333` 提供测试入口。电脑端使用与飞书接口规范相同的二进制帧和 CRC16，发送 `robot.set_led` 可控制 GPIO48 的板载 WS2812B。完整步骤见 `WIFI_LED_TEST.md`。

## 编译与烧录

### Host 侧 Audio_Link 测试

Host 测试不依赖 ESP32 硬件。Windows 原生 ESP-Clang 只提供固件目标编译器，
缺少生成 Windows 可执行文件所需的 MSVC/Windows SDK linker；请使用已安装的
WSL Ubuntu 环境运行：

```powershell
wsl.exe -d Ubuntu-20.04 -- bash -lc "cd /mnt/c/Users/popi/Desktop/mibot/esp32s3/mibot_esp32s3 && cmake -S test_host -B test_host/build-wsl -G Ninja && cmake --build test_host/build-wsl && cd test_host/build-wsl && ctest --output-on-failure"
```

当前套件覆盖帧切分、Audio_Meta、UTF-8、环形缓冲、命令分发、诊断、下行流生命周期和退避逻辑。

在 ESP-IDF 命令行中执行：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

如果 PowerShell 提示找不到 `idf.py`，直接在本工程目录执行：

```powershell
.\flash_monitor.cmd COM6
```

脚本会自动加载本机 ESP-IDF 6.1 环境。`COM6` 可替换为设备管理器中显示的实际 ESP32 串口。

如果希望在当前 PowerShell 窗口直接使用 `idf.py`，执行：

```powershell
. .\export_idf.ps1
idf.py -p COM6 flash monitor
```

本工程已在本机 ESP-IDF v6.1 下成功编译。若默认 ESP-IDF shell 找不到本机工具，可在工程目录执行 `build_local.cmd`。

固件产物位于 `build\\mibot_esp32s3.bin`，配套的 bootloader 和分区表分别位于 `build\\bootloader\\bootloader.bin`、`build\\partition_table\\partition-table.bin`。

## 编译前必须确认

1. `main/mibot_config.h` 已按 ESP32-S3-WROOM-1-N16R8 的可用 GPIO 给出一套推荐分配，但仍要对照你的载板丝印/原理图确认排针确实引出这些脚。
2. UART 使用 3.3 V TTL，SF32 TX 接 ESP32 RX，SF32 RX 接 ESP32 TX，两板共地。
3. 电机使用 TB6612FNG，舵机使用独立 5~6 V 稳压电源。
4. ToF 为 TOF050C（ST **VL6180X**）四路，驱动在 `main/mibot_tof.cpp`。四个模块共用一条 I2C，必须各接一条 XSHUT 线（默认 GPIO15/16/17/18，见 `mibot_config.h`）——VL6180X 的地址寄存器是易失的，每次上电都要逐个拉起重新分配地址。**驱动尚未上真机验证**，上电后看 `mibot_tof` 日志确认 4/4 就绪；未就绪的通道会让安全任务继续拒绝运动。
5. `battery_mv` 暂为 0，电池 ADC 分压和低电压阈值确定后再接入。

## 已实现的本地闭环

- UART 帧：`AA 55 | version | type | flags | seq | length | payload | CRC16-CCITT`。
- 消息：HELLO/HELLO_ACK、COMMAND、ACK/NACK、EVENT、TELEMETRY、PING/PONG。
- 命令：`robot.move`、`robot.stop`、`robot.set_arm_pose`、`robot.get_status`、`robot.read_floor_sensors`。
- 安全：动作时限、ToF 无效禁止运动、前方边缘刹车、UART 1500 ms 失联刹车。
- 任务：UART 解析、运动安全/ToF 采样、遥测/心跳分别运行在 FreeRTOS 任务中。

云端 HTTPS/WebSocket、Wi-Fi/BLE 配网、摄像头和具体 ToF 驱动暂未绑定到某个厂商协议；这些功能应在确定接口后加入 ESP-IDF 任务，不应削弱本地安全任务。

## N16R8 引脚依据和推荐分配

`ESP32-S3-WROOM-1-N16R8` 表示 16 MB Quad SPI Flash + 8 MB Octal SPI PSRAM。根据 Espressif《ESP32-S3-WROOM-1 & WROOM-1U Datasheet》：

- GPIO35、GPIO36、GPIO37 已连接 Octal PSRAM，不可用于外设；
- GPIO0、GPIO3、GPIO45、GPIO46 属于启动/供电相关 strap，避免接电机、舵机等上电会改变电平的负载；
- GPIO19、GPIO20 是 USB D-/D+，保留给 USB 时不要复用；
- GPIO43、GPIO44 是 UART0，保留给下载/日志更方便。

当前配置如下：

| 功能 | GPIO |
|---|---:|
| I2C SDA / SCL | 8 / 9 |
| TB6612 AIN1 / AIN2 / PWMA | 4 / 5 / 6 |
| TB6612 BIN1 / BIN2 / PWMB | 7 / 10 / 11 |
| TB6612 STBY | 12 |
| 左/右舵机 PWM | 13 / 14 |
| SF32 UART2 RX / TX | 2 / 1 |

这组分配避开了 N16R8 的 PSRAM 脚、启动 strap、USB 脚和 UART0 调试脚。GPIO1/2 虽兼有 ADC/Touch 功能，但作为 UART2 的 RX/TX 没有冲突。GPIO 矩阵允许 UART2 映射到这两个普通 GPIO。

官方资料：

- [ESP32-S3 系列数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ESP32-S3-WROOM-1 / WROOM-1U 数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)

注意：这些是模组级 GPIO 能力，不代表你的具体开发板一定把所有脚引出。若你使用的是第三方 N16R8 开发板，请再提供板子链接或正反面照片，我可以按它的实际排针重新排一版。
## 电脑 -> ESP32 -> SF32 LCD 验证

ESP32 的 USB 串口接电脑，使用 `115200 8N1`。业务链路使用 UART2：

```text
ESP32 GPIO1 (TX) -> SF32 PA20 (RX)
ESP32 GPIO2 (RX) <- SF32 PA27 (TX)
GND              <-> GND
```

ESP32 会把电脑发来的每行 JSON 转成 `AA 55 + CRC16` 的 `COMMAND` 帧发给 SF32，SF32 的 ACK/EVENT 会原样打印回电脑。

```powershell
python .\tools\pc_sf32_test.py COM6 HELLO
```

预期电脑串口输出包含 `PC_TX`，随后是 `mibot.uart.v1` 的 ACK；LCD 应显示 `HELLO`。将 `COM6` 换成 ESP32 实际 USB 串口号。

## Route-A 网络桥接

Route-A 复用 SF32/ESP32 之间现有的 UART2 AA55 链路，不占用第二个 UART。`0x70`（SF32 -> ESP32）和 `0x71`（ESP32 -> SF32）的 payload 各自是一整个 IPv4 包；AA55 的序号、CRC 和 UART 互斥仍由控制器统一处理。ESP32 为 SF32 建立 `10.42.0.1/30` 的静态 lwIP 接口，并通过 Wi-Fi STA 转发/NAPT 到互联网。

使用独立配置和构建目录验证 Route-A：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
.\build_routea.cmd
```

脚本把当前 `sdkconfig` 作为硬件配置层，再叠加 `sdkconfig.defaults.routea`，不会改写普通 `build` 的配置。构建前请确认 `main/mibot_config.h` 中的 Wi-Fi 凭据，以及 SF32 端使用相同的 `10.42.0.2/30`、MTU 1200 设置。Route-A 的网络开关只有在日志出现 `raw IPv4 bridge ready` 后才算启动；尚未连接 Wi-Fi 时，SF32 的包会留在有界队列中并按 TCP 重传机制恢复。
