# ESP32-S3 Wi-Fi LED 测试

这个测试使用当前工程的 ESP-IDF 固件，让 ESP32-S3 连接家庭 Wi-Fi，电脑通过同一局域网控制载板上的 WS2812B RGB 灯（GPIO48）。电脑端不需要安装第三方 Python 包。

## 1. 烧录固件

如果 PowerShell 已加载 ESP-IDF 环境，在命令行执行：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
idf.py -p COM6 flash monitor
```

如果提示找不到 `idf.py`，使用工程自带脚本即可：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
.\flash_monitor.cmd COM6
```

它会自动加载本机 ESP-IDF 6.1 环境并执行烧录、串口监视。也可以先执行 `build_local.cmd` 生成固件，再使用对应的烧录命令。

串口日志正常时应看到类似内容：

```text
Wi-Fi connected: IP=192.168.x.x TCP=3333
Mibot ESP-IDF controller started
```

## 2. 获取 ESP32 地址

电脑和 ESP32 都连接家庭 Wi-Fi `YOUR_WIFI_SSID`。从串口监视器读取 `Wi-Fi connected: IP=...` 中的地址，例如 `192.168.1.120`，然后将该地址传给测试脚本：

```powershell
wifi_led_test.cmd --host 192.168.1.120 on
```

## 3. 发送 LED 测试命令

打开另一个 PowerShell：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
wifi_led_test.cmd on
wifi_led_test.cmd off
wifi_led_test.cmd toggle
```

同一个脚本也可以验证动作接口。动作会等待最终 `ACK state=completed`：

```powershell
wifi_led_test.cmd happy
wifi_led_test.cmd confused --intensity 0.6
```

`wifi_led_test.cmd` 会使用本机已安装的 ESP-IDF Python，不依赖系统 `python` 命令。成功时脚本会打印 `HELLO_ACK` 和 `ACK`。`on` 时板载 RGB 灯显示低亮度绿色，`off` 时熄灭。

台架测试 A 电机（履带架空）使用：

```powershell
wifi_led_test.cmd --host 192.168.1.120 motor_a --speed 30 --duration-ms 500
```

命令显式启用本次测试的模拟 ToF，最长运行 1 秒，结束后自动停机；普通 `robot.move` 仍保持 ToF 安全检查。

## 4. 协议定义

TCP 不是另造一套控制协议，而是复用飞书文档定义的 UART 帧格式：

```text
AA 55 | VERSION | TYPE | FLAGS | SEQ(LE) | LENGTH(LE) | PAYLOAD | CRC16(LE)
```

其中 `VERSION=0x01`，帧头为 7 字节，CRC 是覆盖 `VERSION` 到 `PAYLOAD` 的 `CRC-16/CCITT-FALSE`。控制 payload 是 UTF-8 JSON，测试命令为：

```json
{
  "schema": "mibot.uart.v1",
  "command_id": "pc_led_...",
  "name": "robot.set_led",
  "args": {"on": true}
}
```

ESP32 返回标准 `ACK (0x11)` 或 `NACK (0x12)`。`robot.set_led` 是当前硬件联调的测试扩展，不会驱动电机、舵机或 ToF。

## 5. 连接失败排查

- 电脑必须与 ESP32 连接到同一个家庭 Wi-Fi；确认路由器没有开启客户端隔离。
- 关闭 VPN 或检查 Windows 防火墙是否拦截 Python 的局域网 TCP 连接。
- 确认串口日志显示已获取 IP，且 TCP 端口为 `3333`。
- GPIO48 连接的是板载 WS2812B，不是普通高低电平 LED；不要用万用表按普通 LED 方式判断。
- 如果固件启动失败，先确认实物模组型号和 `sdkconfig` 的 Flash 配置，再重新烧录。
