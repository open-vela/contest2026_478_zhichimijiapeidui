# DeepSeek Tool Call 与语音 smoke

该 example 验证三条组合链路：

1. SF32 经 `AI_REQUEST (0x40)` 调用 ESP32 上的 DeepSeek HTTPS 网关。
2. DeepSeek 原生返回 `tool_calls`，SF32 只执行白名单工具，再以 `role=tool` 续轮。
3. 语音模式经 `AUDIO_UP` 裸 PCM → 音频云端 ASR → DeepSeek → `robot.speak` → TTS → `AUDIO_DOWN` 裸 PCM。

DeepSeek 不直接接收 PCM。音频理解由现有独立 WSS 音频服务完成 ASR，DeepSeek 只处理识别文本；语音回复由同一音频服务完成 TTS。HTTPS 与 WSS 仍是两个独立 transport。

## 1. 准备 ESP32

复制 `main/mibot_secrets.h.example` 为本地 `main/mibot_secrets.h`，按需配置 Wi-Fi、DeepSeek 和音频 WSS。不要提交、打印或通过 UART 发送真实凭据。

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
.\build_deepseek_smoke.cmd
.\flash_deepseek_smoke.cmd COM7
```

看到 Wi-Fi 已连接、音频 WSS 已连接后再启动语音测试。纯文本和 Tool Call 模式只要求 Wi-Fi 与 DeepSeek 可用。

## 2. 准备 SF32

```powershell
cd C:\Users\popi\Desktop\mibot\sf32lb52
.\build-openvela.ps1 -DeepSeekSmoke -Clean
```

烧录生成的 `nuttx.bin` 到 `0x12010000`，在 NSH 中运行以下命令。

普通问答：

```text
nsh> deepseek_smoke 请只回复 DEEPSEEK_SMOKE_OK
```

原生 Tool Call，默认只开放状态查询、LED 和停止：

```text
nsh> deepseek_smoke --tool 请打开状态灯
```

显式开放预定义动作。动作仍经过 ESP32 的参数、ToF、急停与安全状态机校验：

```text
nsh> deepseek_smoke --tool --allow-motion 请做一个 greeting 动作
```

语音理解与语音回复。设备录音 4 秒，等待 ASR 文本，执行最多一轮 Tool Call，然后把最终回复送 TTS 播放：

```text
nsh> deepseek_smoke --voice
```

`--voice --allow-motion` 会同时开放预定义动作，联调初期不建议启用。

## 3. UART 接线

业务 UART 是 3.3 V TTL：

```text
SF32 PA27 (TX) -> ESP32 GPIO2 (RX)
SF32 PA20 (RX) <- ESP32 GPIO1 (TX)
GND            <-> GND
```

## 4. Tool Call 边界

- 使用 DeepSeek Chat Completions 原生 `tools` / `tool_calls` / `role=tool` 格式。
- 单轮最多 4 个 Tool Call，最多执行一轮工具续传；模型第二次仍返回工具调用时以 `ELOOP` 终止。
- 默认白名单：`robot_get_status`、`robot_set_led`、`robot_stop`。
- `--allow-motion` 额外开放 `robot_perform_action`，只允许固件内已有的预定义动作。
- Tool 名使用 API 兼容的下划线形式，SF32 映射到既有 `robot.*` 命令。
- 模型不直接访问 GPIO、PWM、UART 或云端凭据，不绕过 ESP32 本地安全检查。

## 5. 有界限制

- 同时只允许一个 DeepSeek 请求 in-flight。
- UART 请求和响应各不超过 4096 字节。
- DeepSeek HTTP 响应不超过 8192 字节。
- 当前非流式；每轮等待完整 Chat Completion。
- 音频保持 16 kHz、单声道、16-bit、20 ms、640 字节裸 PCM，禁止 Base64/JSON 音频封装。

DeepSeek Tool Call 格式参考[官方文档](https://api-docs.deepseek.com/guides/tool_calls)。

Content was rephrased for compliance with licensing restrictions.
