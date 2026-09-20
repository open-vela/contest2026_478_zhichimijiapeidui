# 预编译固件

用 `..\tools\flash-sf32.ps1` 和 `..\tools\flash-esp32.ps1` 烧录，路径已经写好，不用改。

## sf32/voice_agent_nuttx.bin

SF32 语音 Agent，**全桩构型**（`build-openvela.ps1 -VoiceAgent`，不带 `-RealSpeech`）：

- 识别文本固定为 `[asr_stub] 你好`，不走云端 ASR
- 回复用本地提示音占位，不走云端 TTS
- 板间 UART、麦克风采集上行、大模型请求这条链路是真实的

烧录地址 `0x12010000`。固件里不含任何凭据（SF32 侧没有 Wi-Fi，也不直接访问云端）。

等你的云端网关就绪后，要换成真实语音链路构型重新编译：

```powershell
powershell -ExecutionPolicy Bypass -File .\build-openvela.ps1 -VoiceAgent -RealSpeech
```

## esp32/

ESP32-S3 控制器 + MiMo 大模型网关，三个镜像按固定地址烧：

| 地址 | 文件 |
|---|---|
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0x10000` | `mibot_esp32s3.bin` |

需要 16 MB flash（分区表里 factory 占 15 M）。

**这份固件里烧入的凭据是开发用的占位值，你需要换成自己的**：

- Wi-Fi SSID / 密码：是原开发环境的家庭网络，你那边连不上
- 云端音频网关地址：**空**，所以上行音频不会发往任何地方
- 大模型 Key：不是有效的 MiMo Key，请求会返回 HTTP 401

所以开箱烧完跑测试，预期结果是「链路全通、最后一步 401」。这是正常的，说明设备侧没问题。

凭据是**编进固件**的，不是运行时配置，改完必须重新编译烧录：

```powershell
# 1. 改 src\esp32s3\mibot_esp32s3\main\mibot_secrets.h
# 2. 重新编译并烧录
cd src\esp32s3\mibot_esp32s3
.\build_deepseek_smoke.cmd
.\flash_deepseek_smoke.cmd COM7
```

编译环境的装法见 `..\AI_INTEGRATION.md` 第 4.3 节。
