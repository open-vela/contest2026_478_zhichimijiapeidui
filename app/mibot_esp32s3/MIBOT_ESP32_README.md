# Mibot ESP32-S3 Arduino 旧版参考

> 此文件和 `mibot_esp32s3.ino` 是迁移到 ESP-IDF 前的 Arduino 版本，仅作协议和功能对照。当前构建入口是 `main/mibot_controller.cpp`，请阅读同目录的 `README.md`。

这是 ESP32-S3 侧的第一版 Arduino 草图，负责 UART、双电机、双舵机、ToF 安全和遥测。它是本地控制闭环的基础版本，不是假定已经适配任意 ESP32-S3 载板。编译前请在 Arduino IDE 安装 `ArduinoJson`，并按实际载板修改 `esp32s3.ino` 顶部的 GPIO。

## 硬件约束

- UART 是 3.3 V TTL：SF32 TX -> ESP32 RX，SF32 RX -> ESP32 TX，两板共地；默认 460800 8N1。
- 电机使用 TB6612FNG，`STBY` 必须由 ESP32 控制；舵机使用独立 5~6 V 稳压电源。
- 四个同型号 TOF 模块不能直接并联在同一 I2C 地址。需要 XSHUT 逐个改地址或使用 TCA9548A。
- `readTof()` 目前是安全占位实现。TOF050C/200C/400C 的寄存器协议随模块厂商不同，必须依据实际数据手册补上；默认无效读数会拒绝运动。
- 代码现在没有写死 Wi-Fi SSID、云端地址或密钥，也没有擅自假设云端厂商协议。ESP32 的 Wi-Fi/HTTPS/WebSocket 适配应在确认云端接口后接入；云端返回的仍然只能是这里定义的高层 `robot.*` 命令。

## UART 命令

外层帧为 `AA 55 | version | type | flags | seq | length | payload | CRC16-CCITT`，payload 是 UTF-8 JSON。支持 `robot.move`、`robot.stop`、`robot.set_arm_pose`、`robot.get_status` 和 `robot.read_floor_sensors`。

示例 payload：

```json
{"schema":"mibot.uart.v1","command_id":"cmd_0001","name":"robot.move","args":{"linear_mm_s":60,"angular_deg_s":0,"duration_ms":800}}
```

ESP32 本地强制执行 ToF 有效性、边缘保护、动作 TTL 和 UART 失联刹车；云端不能关闭这些保护。`battery_mv=0` 也是占位值，接入电池 ADC 分压后再启用低压策略。

## 当前完成度

- 已完成：UART 帧解析和 CRC16、HELLO/PING、命令 ACK/NACK、动作时限、UART 超时刹车、ToF 无效禁止运动、前方边缘刹车、TB6612 电机 PWM、双舵机 PWM、周期遥测。
- 待按实物补齐：ESP32-S3 载板 GPIO、TOF200C（或其他型号）寄存器驱动、多模块地址/XSHUT 或 TCA9548A、摄像头驱动、电池 ADC、云端 HTTPS/WebSocket、履带方向与速度标定。

在 `SIMULATE_TOF` 改为 `true` 后可以在桌面上先测试 UART、运动命令和舵机 PWM；接真实电机前必须改回 `false`，并完成 ToF 驱动和急停测试。
