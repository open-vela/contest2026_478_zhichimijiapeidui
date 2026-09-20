# ESP32-S3-N16R8 推荐引脚表

本表针对 `ESP32-S3-WROOM-1-N16R8` 模组能力制定，最终接线仍需对照你购买的载板原理图和丝印。N16R8 表示 16 MB Quad SPI Flash + 8 MB Octal SPI PSRAM。

## Mibot 分配

| 功能 | ESP32-S3 GPIO | 方向/说明 |
|---|---:|---|
| I2C SDA | GPIO8 | ToF 总线数据 |
| I2C SCL | GPIO9 | ToF 总线时钟 |
| TB6612 AIN1 | GPIO4 | 左电机方向 |
| TB6612 AIN2 | GPIO5 | 左电机方向 |
| TB6612 PWMA | GPIO6 | 左电机 PWM |
| TB6612 BIN1 | GPIO7 | 右电机方向 |
| TB6612 BIN2 | GPIO10 | 右电机方向 |
| TB6612 PWMB | GPIO11 | 右电机 PWM |
| TB6612 STBY | GPIO12 | 驱动器使能，低电平关闭 |
| 左舵机 PWM | GPIO13 | 50 Hz |
| 右舵机 PWM | GPIO14 | 50 Hz |
| SF32 UART2 RX | GPIO2 | 接 SF32 TX |
| SF32 UART2 TX | GPIO1 | 接 SF32 RX |

对应配置文件：`main/mibot_config.h`。

## 不要占用的脚

- GPIO35、GPIO36、GPIO37：N16R8 的 Octal PSRAM 连接脚，不可作为普通 GPIO。
- GPIO26~GPIO32：模组内部 Quad SPI Flash 使用的脚，不要连接外设。
- GPIO0、GPIO3、GPIO45、GPIO46：启动/供电相关 strap，避免接电机、舵机或带强上拉/下拉的模块。
- GPIO19、GPIO20：USB D-/D+。如果使用 USB Serial/JTAG 或 USB OTG，不能复用。
- GPIO43、GPIO44：UART0 TX/RX，保留给下载、日志和调试。
- EN：芯片使能脚，不能悬空；由载板的复位/使能电路处理。

## 连接规则

```text
ESP32 GPIO1 (UART2 TX)  -> SF32 RX
ESP32 GPIO2 (UART2 RX)  <- SF32 TX
ESP32 GND               <-> SF32 GND
ESP32 GPIO8             <-> ToF SDA
ESP32 GPIO9             <-> ToF SCL
```

UART 和 I2C 信号都是 3.3 V 逻辑。电机电源、舵机 5~6 V 电源必须独立供电，并与 ESP32 共地；不能从 ESP32 的 3.3 V 引脚给电机或舵机供电。

## 依据

- Espressif [ESP32-S3 系列数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- Espressif [ESP32-S3-WROOM-1 / WROOM-1U 数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
