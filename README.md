# Mibot —— 双板桌面语音助手（SF32LB52-LCD + ESP32-S3）

## 一、作品简介

一块 SF32LB52-LCD（openvela 侧）+ 一块 ESP32-S3（Wi-Fi 控制器侧）组成的桌面语音助手。两块板子通过 AA55 板间 UART 以 1 Mbps 直连：

- SF32 侧用**板载真麦克风**采集语音，LCD 显示状态表情；
- ESP32 侧负责 Wi-Fi 与 AI：它把音频上行到音频网关做语音识别，把识别文本交给 MiMo 大模型（ESP32 直接 HTTPS 直连），拿到回复后经网关合成语音下行，由 SF32 播出来。
- ESP32 上还跑着一个 **agent 大脑（piagent）**：带人格、20 轮短记忆、NVS 断电不丢的长记忆，不需要借用 PC 算力。

完整的真实链路（真麦 → ASR → LLM → TTS → 扬声器）已在实物上全部跑通，采用对讲机式交互：在 SF32 控制台敲 `va_wake` 触发一轮对话。

技术协议、编译环境与排障的完整文档见 `AI_INTEGRATION.md`；参赛文档见 `docs/`。

## 二、选题方向

**AI 硬件产品创新。**

一个 AI 语音助手产品如何在「一块算力有限的小板 + 一块只有 WiFi 的板」上做成双板分工的形态：SF32 管交互（收音/播音/表情）、ESP32 管智能（Wi-Fi / ASR 上行 / LLM / TTS），板间用自研二进制帧协议（AA55 + CRC16）交换音频与结构化消息。亮点：

- 无需云上部署大模型推理服务——ESP32 直接对 MiMo 发 HTTPS；
- 设备端记忆（短记忆 + 断电持久化长记忆），对话有连续性；
- 音频网关可本地离线识别（sherpa-onnx Paraformer，零云 key）跑通全链路；
- 底盘（ToF050C ×4 + 电机）已留好安全层，未装配模块时运动锁定，不会误跑。

## 三、目录结构

```text
contest2026_478_zhichimijiapeidui/
├── README.md                    本作品说明
├── AI_INTEGRATION.md            AA55 协议 / WS 音频协议 / 编译环境 / 排障表
├── contest2026_478_....xml      仓库 manifest（软链映射见下）
├── app/
│   ├── mibot_voice_agent/       SF32 openvela 应用（NuttX app）
│   │                            → 软链到 packages/demos/contest2026_478_mibot_voice_agent
│   │   含 mibot_voice_agent_main.c / sf32lb_i2s.c（麦克风电平修复）/ va_face（LCD 表情）/ script/
│   ├── mibot_esp32s3/           ESP32-S3 ESP-IDF 控制器 + piagent（main/）+ 网关/联调工具（tools/）
│   ├── esp32s3_tools/           ESP32 侧独立辅助脚本（pc_sf32_test.py 等）
│   └── examples/deepseek_smoke/ 最小链路冒烟示例（sf32 + esp32）
├── firmware/                    预编译固件，可直接烧
│   ├── sf32/voice_agent_nuttx.bin       2026-09-19 麦克风电平修复版
│   └── esp32/{bootloader, partition-table, mibot_esp32s3}.bin
├── tools/                       烧录脚本（flash-sf32.ps1 / flash-esp32.ps1）+ SiFli 官方烧录器
├── docs/                        参赛文档《2026 首届 openvela AI 硬件开发者大赛·文档.md》+ 图片/录像
└── logs/                        AI Coding 日志（提交格式见 logs/README.md，待导出补齐）
```

## 四、运行方式

### 接线

| 链路 | 引脚 | 参数 |
|---|---|---|
| 板间业务 UART | SF32 PA27(TX) → ESP32 GPIO2(RX)；SF32 PA28(RX) ← ESP32 GPIO1(TX) | 3.3V TTL，1 Mbps 8N1 |
| SF32 控制台/烧录 | COM5 | 1000000 |
| ESP32 串口/烧录 | COM4 | 115200 |

### 烧录

```powershell
# SF32（预编译修复固件）
.\tools\flash-sf32.ps1 -Port COM5

# ESP32（预编译镜像，无需装 ESP-IDF）
.\tools\flash-esp32.ps1 -Port COM4
```

> 预编译 ESP32 镜像内的凭据是**占位符**（Wi-Fi / MiMo key）。要直接使用预编译固件跑通对话，有两个选择：
> 1. 从源码重编：复制 `app/mibot_esp32s3/main/mibot_secrets.h.example` 为 `mibot_secrets.h` 填入你的 WiFi 与 MiMo key；
> 2. 或保持二进制补丁方式：用 `tools/patch_piagent_creds.py`（同级压缩包工具，槽位限长，自动重算校验和）就地写入。
> 固件内 `ws://<网关地址>:8765` 需指向运行音频网关的那台电脑。

### 起 PC 音频网关（语音功能的依赖，一直开着）

```powershell
cd app\mibot_esp32s3\tools
python -X utf8 -u volc_gateway.py --host 0.0.0.0 --port 8765 --asr-engine local
```

本地离线中文 ASR（sherpa-onnx Paraformer）+ edge TTS（zh-CN-XiaoxiaoNeural），不需要任何云语音 key。

### SF32 上电后启动并对话

```
nsh> mibot_voice_agent &     ← 语音代理（每次上电都要）
nsh> va_wake                  ← 触发一轮，听到提示后开口说话
… ASR 听写 → MiMo 回答 → TTS 播报 → LCD 表情 …
```

固件说明了变化：SF32 烧录走 RAM 加载是**易失**的，断电后需重烧一次；修复固件下第二个采集窗存在已知挂起，临时用第一个窗口（见 `AI_INTEGRATION.md` §已知问题）。

## 五、AI Coding 使用说明

本作品全程使用 **Claude Code** 辅助开发，AI 参与的环节覆盖整个生命周期：

- **需求拆解与方案设计**：双板分工（SF32 交互 / ESP32 智能）、板间 AA55 帧协议与 WS 音频协议由 AI 协助梳理成 `AI_INTEGRATION.md` 对接文档；
- **编码**：板间协议固件（HELLO/命令/音频帧/CRC16）、ESP32 piagent（人格 + 记忆 + NVS）、PC 音频网关（本地 ASR + TTS）的初版代码由 AI 生成并逐轮修改；
- **调试**：麦克风电平问题的根因定位（VAD 早停 + ADC 冷启动 2.8s 爬升，而非增益）、二次采集窗 DMA 挂起边界排查、ESDF 二进制检查/固件消毒等，均以对话形式展开；
- **测试与文档**：M2（板间链路）/ M3（记忆）/ M5（网关双向）自动化验收用例、发布打包与 README 说明均为 AI 产出。

完整对话日志见 `logs/` 目录（提交格式见 `logs/README.md`，将按官方工具导出补齐）。

## 六、状态与已知问题

| 能力 | 状态 |
|---|---|
| 板间 AA55 链路（HELLO/命令/音频帧/CRC16） | ✅ 全 PASS |
| piagent 记忆（人格 + 短记忆 + NVS 长记忆） | ✅ 全 PASS（记住/断电恢复/回忆） |
| PC 音频网关（本地 ASR + edge TTS） | ✅ 双向调通 |
| MiMo LLM 直连（ESP32 → HTTPS） | ✅ 实测 status=200 |
| 真麦 → ASR → 回答 → 出声 全闭环 | ✅ 实物跑通 |
| 麦克风电平修复（ADC 常开） | 🔶 首个采集窗已验证，二次采集窗待修 |
| 离线唤醒词 | ⛔ 未做，每轮手动 `va_wake` |
| 底盘运动（ToF 未装） | ⛔ 预期 NACK（安全锁定） |