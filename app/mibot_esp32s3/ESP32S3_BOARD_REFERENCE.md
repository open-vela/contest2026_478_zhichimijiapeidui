# ESP32-S3 开发板资料备份

> 用途：保存已经阅读并核对过的 ESP32-S3 载板、模组和工程配置事实，供 Mibot 后续硬件接线与 ESP-IDF 开发参考。
> 更新：2026-09-05

## 1. 资料来源

资料位于工程的 `reference/ESP32-S3 资料/ESP32-S3 资料/` 目录，已阅读：

- `ESP32-S3 原理图.pdf`
- `esp32-s3-wroom-1_wroom-1u_技术规格书.pdf`
- `ESP32 硬件介绍.PNG`
- `ESP32-S3 引脚图.jpg`
- `ESP32-S3 外观图.PNG`
- `PCB尺寸图（mil）.pdf`
- `PCB尺寸图（mm）.pdf`
- 目录内的 Arduino Wi-Fi 示例、MicroPython 例程和下载工具资料

## 2. 载板身份与板载电路

原理图标题为：

`YD-ESP32-S3-COREBOARD V1.4`

硬件介绍图片标注该载板可安装以下模组：

- `ESP32-S3-WROOM-1-N8R2`
- `ESP32-S3-WROOM-1-N8R8`
- `ESP32-S3-WROOM-1-N16R8`

原理图中的模组位号是通用的 `ESP32-S3-WROOM-1`，没有写死 Flash/PSRAM 容量。因此，实际使用的 N8R2、N8R8 还是 N16R8，必须以模组屏蔽罩上的完整丝印或购买型号为准。

载板已确认的功能：

| 电路/接口 | 资料中的连接关系 |
| --- | --- |
| USB1 | CH343P USB 转 UART，连接 UART0（GPIO43/GPIO44） |
| USB2 | ESP32-S3 原生 USB OTG |
| 原生 USB 信号 | GPIO19 = USB D-，GPIO20 = USB D+ |
| USB-JTAG | 原理图引到 GPIO3 |
| BOOT 按键 | GPIO0 |
| RST 按键 | CHIP_PU/EN |
| RGB 灯 | GPIO48 驱动板载 WS2812B，灯电源为 5V |
| 电源 | USB/5V 输入经 CJ6107A33GW 稳压为 VDD33/3.3V |
| 排针 | J1/J2 引出多组 GPIO、3V3、5V 和 GND |

原理图显示 USB1 和 USB2 都使用 Type-C 接口。UART0 由 CH343P 占用，适合保留给下载和日志；与 SF32 通信应使用 UART2 或其他未占用 UART。

## 3. 模组规格书结论

`ESP32-S3-WROOM-1` 系列主要能力：

- 2.4 GHz Wi-Fi 802.11 b/g/n
- Bluetooth LE 5，支持 BLE Mesh 等特性
- Xtensa 双核 LX7，最高 240 MHz
- 512 KB SRAM、384 KB ROM、16 KB RTC SRAM
- 最高 16 MB Quad SPI Flash
- 最高 16 MB Octal SPI PSRAM
- GPIO 支持 UART、I2C、SPI、I2S、LCD/Camera、LEDC PWM、MCPWM、USB、TWAI、ADC 等外设
- 模组供电范围：3.0-3.6V，典型系统电源为 3.3V
- `WROOM-1` 模组尺寸约 18 x 25.5 x 3.1 mm

常见型号含义：

| 型号 | Flash | PSRAM | 备注 |
| --- | ---: | ---: | --- |
| N8R2 | 8 MB Quad SPI | 2 MB Quad SPI | 普通 Quad PSRAM |
| N8R8 | 8 MB Quad SPI | 8 MB Octal SPI | GPIO35-37 被 Octal PSRAM 使用 |
| N16R8 | 16 MB Quad SPI | 8 MB Octal SPI | GPIO35-37 被 Octal PSRAM 使用 |
| N16R16VA8 | 16 MB Quad SPI | 16 MB Octal SPI | 具体 VDD_SPI/温度条件需按规格书确认 |

对于 `N8R8/N16R8/N16R16VA8`，不能把 Octal PSRAM 使用的 GPIO 当作普通排针 GPIO。模组级可用不等于载板一定引出，最终仍需同时满足模组和载板原理图条件。

## 4. Mibot 当前推荐 GPIO 分配

以下分配适合当前方案：ESP32-S3 负责 ToF、TB6612 电机、双舵机，并通过 UART2 与 SF32 通信。

| 功能 | GPIO | 说明 |
| --- | ---: | --- |
| ToF I2C SDA | GPIO8 | 4 个 ToF 共用 SDA 总线 |
| ToF I2C SCL | GPIO9 | 4 个 ToF 共用 SCL 总线 |
| 左电机 AIN1 | GPIO4 | TB6612 方向 |
| 左电机 AIN2 | GPIO5 | TB6612 方向 |
| 左电机 PWMA | GPIO6 | LEDC PWM |
| 右电机 BIN1 | GPIO7 | TB6612 方向 |
| 右电机 BIN2 | GPIO10 | TB6612 方向 |
| 右电机 PWMB | GPIO11 | LEDC PWM |
| TB6612 STBY | GPIO12 | 高电平使能，低电平关闭 |
| 左舵机 PWM | GPIO13 | LEDC，约 50 Hz |
| 右舵机 PWM | GPIO14 | LEDC，约 50 Hz |
| SF32 UART2 TX | GPIO1 | ESP32 TX -> SF32 RX |
| SF32 UART2 RX | GPIO2 | ESP32 RX <- SF32 TX |

UART 接线必须交叉并共地：

```text
ESP32 GPIO1 / UART2 TX  -> SF32 RX
ESP32 GPIO2 / UART2 RX  <- SF32 TX
ESP32 GND               <-> SF32 GND
```

UART 和 I2C 均按 3.3V TTL 逻辑设计。电机和舵机不能由 ESP32 的 3.3V 引脚供电，必须使用独立的电机/舵机电源并与 ESP32 共地。

## 5. 应避免复用的 GPIO

| GPIO | 原因 |
| ---: | --- |
| GPIO0 | BOOT/启动按键，外部上拉下拉可能影响下载和启动 |
| GPIO3 | USB-JTAG/JTAG 相关 |
| GPIO19/20 | 原生 USB D-/D+，使用 USB 时不可复用 |
| GPIO43/44 | UART0，连接 CH343P，保留给下载/日志 |
| GPIO45/46 | 启动/供电/日志相关 strap，避免接带固定电平的负载 |
| GPIO35/36/37 | N8R8/N16R8 等 Octal PSRAM 连接脚 |
| GPIO26-32 | 模组内部 Flash 使用脚，不应接外部设备 |
| GPIO48 | 板载 WS2812B RGB 灯 |
| EN/CHIP_PU | 芯片使能/复位，不作为普通外设 GPIO |

GPIO1、2 虽具有 ADC/Touch 等复用功能，但在当前方案中只作为 UART2 使用，没有已知冲突。ESP32-S3 GPIO Matrix 允许 UART2 映射到这些普通 GPIO。

## 6. 当前工程状态与风险

工程路径：

`C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3`

当前工程使用 ESP-IDF 原生构建，主要代码在 `main/mibot_controller.cpp`，板级宏在 `main/mibot_config.h`。

已确认：

- GPIO 分配与 `YD-ESP32-S3-COREBOARD V1.4` 原理图的排针连接匹配。
- UART2、I2C、LEDC PWM、FreeRTOS 任务和 UART 帧协议已经可以编译。
- 当前固件已成功生成 `build/mibot_esp32s3.bin`。
- ToF 函数目前是安全占位实现；在确定 TOF050C/200C/400C 的实际寄存器协议前，运动安全逻辑会拒绝运动。

仍需处理：

1. 从实物模组丝印确认具体型号，不要仅依据载板宣传图假设为 N16R8。
2. 当前 `sdkconfig` 曾显示 `2MB Flash`、未启用 PSRAM、CPU 160 MHz；若实物是 N16R8，应在烧录前改为 16MB Flash，并根据需要启用 8MB Octal PSRAM。
3. 修改 Flash/PSRAM 后应重新生成分区表、bootloader 和烧录参数，并检查 `build/flasher_args.json` 中的 Flash size。
4. 具体 ToF 模块要补齐真实 I2C 驱动；多个同地址模块需要使用 XSHUT、地址修改流程或 I2C 复用器。
5. 电池 ADC 分压、低电压阈值、Wi-Fi/云端接口和实际 UART 连接还需实物验证。

## 7. 使用规则

- 这份文档是工程知识备份，不替代原始 PDF、原理图和实际测量。
- 讨论 GPIO 时，先检查本文件的“应避免复用”表，再检查 `main/mibot_config.h`。
- 讨论 Flash/PSRAM 时，先确认模组完整型号，再修改 `sdkconfig`；不要把 N16R8 的配置直接套给 N8R2。
- 讨论排针时，以原理图的 J1/J2 网络为准；模组数据手册只说明芯片/模组能力。
- 任何电机、舵机或电池接线改动，都必须确认电压、电流和共地关系后再上电。

## 8. 原始资料

- `reference/ESP32-S3 资料/ESP32-S3 资料/ESP32-S3 原理图.pdf`
- `reference/ESP32-S3 资料/ESP32-S3 资料/esp32-s3-wroom-1_wroom-1u_技术规格书.pdf`
- `reference/ESP32-S3 资料/ESP32-S3 资料/ESP32 硬件介绍.PNG`
- `reference/ESP32-S3 资料/ESP32-S3 资料/ESP32-S3 引脚图.jpg`

