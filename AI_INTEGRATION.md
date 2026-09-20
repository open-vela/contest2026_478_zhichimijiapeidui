# Mibot 语音链路对接指南（AI 侧）

面向对象：负责云端 ASR / TTS / 大模型的同学。

设备侧（SF32 + ESP32）已经打通并通过连续多轮实测。AI 侧需要提供的只有两样东西：

1. **一个云端音频网关**（WebSocket）：接收上行 PCM 做 ASR，接收文本做 TTS 并回传 PCM
2. **一个 MiMo API Key**（大模型走 ESP32 直连 HTTP，不需要你部署服务）

---

## 1. 当前状态

已实测通过（`sf32lb52/script/e2e_voice_test.py`，连续 5 轮全绿）：

```text
[PASS] agent linked (HELLO_ACK)        设备间 UART 链路
[PASS] mic uplink sent                 麦克风采集并上行
[PASS] cloud ASR text received         云端 ASR 文本回到对话
[PASS] LLM request sent / received     大模型请求与回复
[PASS] TTS requested / TTS audio played 云端 TTS PCM 播放
[PASS] no cloud failure
[PASS] ESP32 did not crash
```

完整链路：

```text
麦克风 → SF32 → [UART AA55] → ESP32 → [WebSocket] → 云端 ASR
                                                        ↓ 文本
        SF32 状态机 ← [UART] ← ESP32 ← ─────────────────┘
           ↓ 文本
        ESP32 → [HTTPS] → MiMo 大模型 → 回复文本 / tool_calls
           ↓
        SF32 → robot.speak → ESP32 → [WebSocket] → 云端 TTS
                                                        ↓ PCM
        扬声器 ← SF32 ← [UART] ← ESP32 ← ───────────────┘
```

**重要说明**：设备侧是用本地 mock 验证的（协议、时序、稳定性全部走通），尚未接过真实云服务和真实 MiMo Key。mock 就是按下面这份协议实现的，可直接当参考实现。

---

## 2. AI 侧要做的事（一）：云端音频网关

ESP32 作为 WebSocket **客户端**主动连接你的网关。你只需实现服务端。

### 2.1 连接

```text
URI      : wss://your-gateway/v1/audio      (本地联调可用 ws://)
鉴权     : HTTP 头 Authorization: Bearer <MIBOT_CLOUD_API_KEY>
```

### 2.2 音频格式（上下行一致，不可协商）

| 项 | 值 |
|---|---|
| 编码 | 裸 PCM，`pcm_s16le`（16 位小端） |
| 采样率 | 16000 Hz |
| 声道 | 1（单声道） |
| 帧长 | 20 ms = 320 采样 = **640 字节** |

**不要**做 Base64 或 JSON 封装音频，直接用 WebSocket 二进制帧。

### 2.3 上行：设备 → 你（ASR）

- **二进制帧**：聚合 5 个音频帧，即每条 WS 消息 **3200 字节**（= 100 ms 音频）
- **文本帧**：一句话说完时发送结束标记

```json
{"eos":true}
```

收到 `eos` 后请做最终识别并回传结果。

### 2.4 下行：你 → 设备（ASR 结果）

发送**文本帧**。两种格式都接受，任选其一：

```json
{"type":"asr_result","text":"你好，请介绍一下你自己","final":true}
```

```json
{"schema":"mibot.asr.v1","event":"asr_text","final":true,"text":"你好，请介绍一下你自己"}
```

约束：
- 只有 `final:true` 且 `text` 非空才会驱动对话；中间结果会被忽略（可以发，用于调试）
- 设备侧等待预算 **8 秒**，超时该轮按云端失败结束
- 单帧不超过 4096 字节

### 2.5 上行：设备 → 你（TTS 请求）

设备需要说话时，会发一个**文本帧**：

```json
{"type":"tts_request","text":"我是MiMo，很高兴认识你。","voice":"default","stream_id":"a1b2c3"}
```

- `text` 最长 500 个 Unicode 码点
- `stream_id` 用于关联本次流，回传时不需要携带

### 2.6 下行：你 → 设备（TTS 音频）

1. 发若干**二进制帧**，内容为裸 PCM（格式同 2.2；分块大小可自由，设备会自己重组为 640 字节帧）
2. 结束时发一个**文本帧**：

```json
{"eos":true}
```

约束：
- 首个音频块必须在 **5 秒**内到达，否则设备判超时并中止本次播放
- 建议略快于实时地推送（例如 1.5 秒语音在 1 秒内推完），设备侧有约 240 ms 缓冲

### 2.7 会话保活（务必注意）

ESP32 的 WebSocket 客户端在**空闲**时会断开重连（实测约 10 秒）。请任选其一：

- 正常响应 WS ping（客户端每 15 秒发一次），**或**
- 每 3 秒发一个无害文本帧作为心跳，例如 `{"type":"noop"}`
  （设备会忽略既不含音频元数据也不含 `eos` 的文本帧）

参考实现 `esp32s3/mibot_esp32s3/tools/mock_cloud_audio.py` 采用后者。

---

## 3. AI 侧要做的事（二）：MiMo API Key

大模型由 ESP32 直接以 HTTPS 调用，**你不需要部署中间服务**，只要提供 Key。

默认配置（OpenAI Chat Completions 兼容）：

| 项 | 值 |
|---|---|
| 地址 | `https://api.xiaomimimo.com/v1/chat/completions` |
| 模型 | `mimo-v2.5-pro` |
| 鉴权 | 同时发送 `Authorization: Bearer <key>` 与 `api-key: <key>` |
| 参数 | `max_completion_tokens`（默认 256） |

设备侧行为：
- SF32 **不指定模型**，模型完全由 ESP32 侧配置决定，换模型不需要重新编译 SF32
- 若请求里没有 `system` 消息，ESP32 会自动注入 MiMo 官方推荐的系统提示词
- 请求中携带 `tools`（`robot_perform_action`），模型可返回原生 `tool_calls` 来驱动机器人动作
- 动作白名单是**恰好这 6 个**情绪动作，越界会被 SF32 直接拒绝（不会下发到 ESP32）：

  ```text
  happy  sad  confused  surprised  cute  greeting
  ```

  这个枚举已经写进发给模型的 `tools` 定义里，正常情况下模型不会返回别的值。注意 `warning` / `idle` / `listening` / `thinking` 虽然在设备侧存在动作表，但**不允许对话驱动**：前者由设备本地安全逻辑触发，后三个在进入对应状态时自动播放。

Token Plan 用户需要改基址，见第 5.1 节的可选覆盖项。

---

## 4. 环境准备

只想调协议、不打算自己编译固件的话，这一节可以跳过——让设备侧同学给你烧好板子即可。要自己改设备侧代码就得把两套工具链都装上。

**两套工具链是分开的**，互不干扰：ESP32 用 Windows 上的 ESP-IDF，SF32 用 WSL2 里的 openvela。下面所有版本号都是当前验证通过的这台机器上的实际版本。

### 4.1 硬件与主机

| 项 | 说明 |
|---|---|
| SF32 板 | SF32LB52-DevKit-LCD |
| ESP32 板 | ESP32-S3，**必须 16 MB flash**（分区表 `partitions.csv` 里 factory 占 15 M） |
| 主机 | Windows + WSL2 |
| 线材 | 两条 USB（各板一条），3 根杜邦线接板间 UART（见 5.4） |

两块板各占一个串口。本文示例用 `COM5`（SF32，1 Mbps）和 `COM7`（ESP32，115200），**你的编号大概率不同**，插拔后也可能变。查当前串口：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

分不清哪个是哪块板时，拔掉一块再跑一次对比即可。

### 4.2 Windows 侧公共准备

测试脚本和 mock 服务需要 Python 3.8+ 以及 `pyserial`、`websockets`。

**装的时候用 `python -m pip`，不要用裸 `pip`。** Windows 上经常同时存在多个解释器（系统 Python、ESP-IDF 自带的两个、Store 存根），裸 `pip` 很可能装进一个你实际不会调用的解释器里，然后跑脚本报 `ModuleNotFoundError: No module named 'serial'`：

```powershell
python -c "import sys; print(sys.executable)"    # 确认你调的是哪个解释器
python -m pip install pyserial websockets        # 装进同一个解释器
python -c "import serial, websockets; print(serial.__version__, websockets.__version__)"
```

验证通过的版本：Python `3.11.15`、`pyserial 3.5`、`websockets 17.1`。

> 本机上没有独立的系统 Python，上面这些脚本实际跑在 ESP-IDF 的虚拟环境解释器里（`C:\Espressif\tools\python_env\idf6.1_py3.11_env`）。这完全可行，只要装包和跑脚本用的是同一个解释器就行。你那边有自己的 Python 就用自己的。

### 4.3 ESP32 环境：Windows + ESP-IDF v6.1

用 Espressif 官方安装器装 **ESP-IDF v6.1**，target 选 `esp32s3`。安装器下载地址见 [ESP-IDF 官方入门文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html)。

本机的实际布局（构建脚本按这套路径写死）：

```text
IDF_PATH            C:\esp\v6.1\esp-idf
IDF_TOOLS_PATH      C:\Espressif\tools
Python 虚拟环境     C:\Espressif\tools\python_env\idf6.1_py3.11_env
编译器              xtensa-esp-elf  esp-15.2.0_20251204
cmake / ninja       4.0.3 / 1.12.1
esp-rom-elfs        20241011
```

**装在别的位置怎么办**：`build_deepseek_smoke.cmd` 和 `flash_deepseek_smoke.cmd` 顶部各有一段 `set "..."` 环境变量块，把里面的路径改成你的即可。这是唯一需要改的地方，两个文件改法一致。

**不要在普通 PowerShell 里直接敲 `idf.py`。** 缺少 `ESP_IDF_VERSION` 之类的变量时它会崩在依赖解析上：

```text
TypeError: expected string or bytes-like object, got 'NoneType'
```

这两个 `.cmd` 脚本就是为了把环境变量一次性设好，直接用它们。

**首次构建需要联网**：IDF 组件管理器会拉两个依赖（`main/idf_component.yml`），之后缓存在 `managed_components/`：

```text
espressif/cjson                ^1.7.19
espressif/esp_websocket_client ~1.2.0
```

### 4.4 SF32 环境：WSL2 + openvela

在 WSL2 里装 **Ubuntu 20.04**（验证通过的发行版名是 `Ubuntu-20.04`，WSL 版本 2）：

```powershell
wsl --install -d Ubuntu-20.04
wsl -l -v                      # 确认 VERSION 是 2
```

进 WSL 装工具链。下面这份是实际验证过的最小集合（NuttX 传统文档里的 `gperf` / `flex` / `bison` 在这套 CMake + kconfiglib 流程里用不到，本机没装也能编过）：

```bash
sudo apt update
sudo apt install -y git gcc-arm-none-eabi cmake ninja-build genromfs gawk python3-pip
pip3 install kconfiglib pyelftools cxxfilt pyserial
```

验证通过的版本：

```text
arm-none-eabi-gcc  9.2.1 (15:9-2019-q4)
cmake / ninja      3.16.3 / 1.10.0
python3            3.8.10
kconfiglib         14.1.0
pyelftools 0.32    cxxfilt 0.3.0    pyserial 3.4
```

#### 拉取 openvela 源码树

三个仓库，全部用 **`dev-ai-contest-2026`** 分支。目标目录结构（本机放在 `/home/popi/openvela`）：

```text
/home/popi/openvela/
├── nuttx/                  open-vela/nuttx          709 MB
├── nuttx-apps/             open-vela/nuttx-apps     123 MB
└── vendor/
    └── sifli/              open-vela/vendor_sifli    41 MB
        └── boards/sf32lb52/sf32lb52_devkit_lcd/     ← 板级支持包，含音频驱动
```

**在 WSL 的原生文件系统里 clone**（`~/openvela`），不要放在 `/mnt/c/...` 下——跨文件系统访问会让编译慢几倍，还可能出现权限和换行符问题。

```bash
mkdir -p ~/openvela/vendor && cd ~/openvela

git clone -b dev-ai-contest-2026 https://github.com/open-vela/nuttx.git
git clone -b dev-ai-contest-2026 https://github.com/open-vela/nuttx-apps.git
git clone -b dev-ai-contest-2026 https://github.com/open-vela/vendor_sifli.git vendor/sifli
```

> 最后一条的目标目录名 **必须是 `vendor/sifli`**（不是默认的 `vendor_sifli`）。构建脚本按 `$OpenVela/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd` 定位板级目录，名字不对会找不到板子。

三个仓库共约 875 MB，加上编译产物预留 1.5 GB 以上。只想编译、不需要历史记录的话可以浅克隆，快很多：

```bash
git clone --depth 1 -b dev-ai-contest-2026 https://github.com/open-vela/nuttx.git
```

拉完确认分支和 commit：

```bash
cd ~/openvela
for d in nuttx nuttx-apps vendor/sifli; do
  printf '%-16s %s %s\n' "$d" "$(git -C $d rev-parse --abbrev-ref HEAD)" "$(git -C $d rev-parse --short HEAD)"
done
```

本文档验证通过的版本是：

```text
nuttx            dev-ai-contest-2026  dd92bcf4257
nuttx-apps       dev-ai-contest-2026  dcc6a95c3
vendor/sifli     dev-ai-contest-2026  af6f365
```

commit 不完全一致通常没问题（同分支后续提交），但**分支必须是 `dev-ai-contest-2026`**。`nuttx` 仓库的默认分支是 `dev`，直接 `git clone` 不带 `-b` 会拉错分支，板级配置对不上。已经拉错了就切过来：

```bash
cd ~/openvela/nuttx
git fetch origin dev-ai-contest-2026
git checkout dev-ai-contest-2026
```

切分支后**必须清理构建缓存重编**，否则 CMake 会复用上一分支的 defconfig：

```powershell
powershell -ExecutionPolicy Bypass -File .\build-openvela.ps1 -VoiceAgent -Clean
```

> 完整的 openvela 发行还有 `frameworks/` 和 `packages/` 两个目录。**voice_agent 构型用不到**（已确认编译产物里没有任何对这两个目录的引用），不用拉。`packages/ai_agent` 只在 `-AiAgent` 构型下会被校验。

#### 路径不一样怎么办

不用改脚本，`build-openvela.ps1` 有参数：

```powershell
powershell -ExecutionPolicy Bypass -File .\build-openvela.ps1 -VoiceAgent `
  -Distro "Ubuntu-22.04" -OpenVela "/home/you/openvela"
```

构建脚本会把工作区 `sf32lb52/openvela-app/` 下的源码同步进 openvela 树再编译，所以**改代码改工作区里的文件**，不要直接改 WSL 里的副本。

### 4.5 SF32 烧录工具

烧录用 SiFli SDK 里的 `ImgDownUart.exe`：

```powershell
git clone https://github.com/OpenSiFli/SiFli-SDK.git
# 工具位置：<SDK>\tools\uart_download\ImgDownUart.exe
```

验证通过的版本是 `main` 分支 `b5c5f7f1`，工具自报 `cur version 3.9, driver_external/internal_20260328`。

> 本机这份 SDK 落在 `%LOCALAPPDATA%\Temp\sifli-sdk-audio\` 下。**Temp 目录会被系统清理**，建议 clone 到固定位置，并把 5.3 节烧录命令里的 `$dir` 改成你的路径。

### 4.6 环境自检

两条命令，能跑通就说明装好了，都不需要接板子：

```powershell
# ESP32 工具链
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
.\build_deepseek_smoke.cmd

# SF32 工具链
cd C:\Users\popi\Desktop\mibot\sf32lb52
powershell -ExecutionPolicy Bypass -File .\build-openvela.ps1 -VoiceAgent
```

ESP32 侧最后应打印 `Project build complete`，SF32 侧应打印 `Build complete` 和 `Staged for flash`。再加上 6.4 的主机单元测试，就能在完全没有硬件的情况下确认环境可用。

---

## 5. 配置与编译

### 5.1 填写凭据

复制模板并填写（该文件已被 git 忽略，**不要提交**）：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3\main
copy mibot_secrets.h.example mibot_secrets.h
```

```c
#define MIBOT_WIFI_STA_SSID     "你的WiFi"
#define MIBOT_WIFI_STA_PASSWORD "你的密码"

// 云端音频网关（ASR + TTS）
#define MIBOT_CLOUD_WS_URI  "wss://your-gateway/v1/audio"
#define MIBOT_CLOUD_API_KEY "your-gateway-key"

// MiMo 大模型
#define MIBOT_MIMO_API_KEY  "sk-xxxx"      // Token Plan 是 tp-xxxx

// 可选覆盖（默认值即上表，Token Plan 需要改基址）
// #define MIBOT_LLM_URL   "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
// #define MIBOT_LLM_MODEL "mimo-v2.5-pro"
```

`MIBOT_CLOUD_WS_URI` 留空则关闭云端音频（设备退回本地桩模式，不影响启动）。

### 5.2 编译并烧录 ESP32

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3
.\build_deepseek_smoke.cmd
.\flash_deepseek_smoke.cmd COM7
```

> 脚本名沿用历史名称，构建出的固件已是 MiMo 网关。

### 5.3 编译并烧录 SF32

两种构型，二选一：

```powershell
cd C:\Users\popi\Desktop\mibot\sf32lb52

# 真实语音链路：ASR/TTS 走云端（联调用这个）
powershell -ExecutionPolicy Bypass -File .\build-openvela.ps1 -VoiceAgent -RealSpeech

# 全桩：不依赖云端，固定识别文本 + 本地提示音（验证硬件用这个）
powershell -ExecutionPolicy Bypass -File .\build-openvela.ps1 -VoiceAgent
```

构建脚本会打印当前语音路径和暂存路径，两行都确认一下：

```text
Voice agent speech path: REAL cloud ASR/TTS
Staged for flash: C:\...\sf32lb52\voice_agent_nuttx.bin (968424 bytes, 2026-09-17 09:11:39)
```

> 编译产物在 WSL 里，脚本会自动把它复制到 Windows 侧的 `voice_agent_nuttx.bin`（烧录清单指向这个文件）。看到 `Staged for flash` 的时间戳是刚才，才说明烧的是新固件。

烧录（固件地址 `0x12010000`）：

```powershell
$dir="$env:LOCALAPPDATA\Temp\sifli-sdk-audio\tools\uart_download"
cd $dir
.\ImgDownUart.exe --port COM5 --baund 1000000 --device SF32LB52X `
  --file "C:\Users\popi\Desktop\mibot\sf32lb52\voice_agent_ImgBurnList.ini" `
  --loadram 1 --postact 1
```

> 烧录报 `DownLoadUart fail` 时，先确认没有别的程序占用 COM5（测试脚本、串口终端都会占用）。

### 5.4 硬件连线

业务 UART，3.3 V TTL，1 Mbps 8N1，只需三根线：

```text
SF32 PA27 (TX) -> ESP32 GPIO2 (RX)
SF32 PA20 (RX) <- ESP32 GPIO1 (TX)
GND            <-> GND
```

---

## 6. 自测

### 6.1 一条命令跑端到端

```powershell
cd C:\Users\popi\Desktop\mibot\sf32lb52
python script\e2e_voice_test.py --sf32 COM5 --esp32 COM7 --reset-esp32 --turns 3
```

它会自己启动语音 Agent、触发多轮对话、同时抓两块板日志，并逐项给出 PASS/FAIL；日志落在 `e2e_sf32.log` / `e2e_esp32.log`。全桩固件加 `--stub`。

### 6.2 没有云服务时用 mock 联调

两个 mock 完全按第 2、3 节协议实现，可直接作为参考实现：

```powershell
cd C:\Users\popi\Desktop\mibot\esp32s3\mibot_esp32s3\tools

# 云端音频网关（ASR + TTS）
python mock_cloud_audio.py --port 8765 --transcript "你好，请用一句话介绍你自己"

# 大模型（OpenAI 兼容，替代 MiMo）
python mock_llm.py --port 8766
python mock_llm.py --port 8766 --tool-call --action happy   # 测试动作分支
```

对应 secrets 改成本机地址：

```c
#define MIBOT_CLOUD_WS_URI "ws://192.168.1.3:8765"
#define MIBOT_LLM_URL      "http://192.168.1.3:8766/v1/chat/completions"
```

依赖装法见 4.2；Windows 防火墙要放行这两个端口。

### 6.3 单板硬件自测

```text
nsh> va_test spk 3        扬声器（2 kHz 测试音）
nsh> va_test mic 3        麦克风（应报 ~150 帧、dropped=0）
nsh> va_test hw 2         codec 内置 1 kHz，绕过 PCM/DMA，用于隔离模拟链路
nsh> va_test spkmic 3     播放同时录音，输出 5 ms 分辨率能量包络（客观判断断续）
nsh> va_wake              手动触发一轮对话（等价于按 KEY2）
nsh> buttons 5            读 5 次按键事件，用于单独验证 KEY2/PA11 是否接通
```

> `buttons` 命令打印的名字是 `PA43_KEY2`，这是厂商 defconfig 里的一处笔误——驱动实际读的是 **PA11**，别照着 PA43 去量。

物理触发是板上的 **KEY2（PA11）**，按下为高。板上另一个键 PA34 是电源键（HOME / 长按复位），接在复位通路上，不能当普通按键用。

### 6.4 主机单元测试（不需要硬件）

```powershell
wsl bash -lc "cd /mnt/c/Users/popi/Desktop/mibot/sf32lb52/openvela-app/test_host && cmake -S . -B build-wsl && cmake --build build-wsl && sh run-wsl-tests.sh"
python esp32s3\mibot_esp32s3\tools\test_mibot_ai_response.py
```

当前 SF32 侧 7 项、ESP32 契约 5 项全部通过。

---

## 7. 排障

| 现象 | 原因与处理 |
|---|---|
| 设备一直 `cloud ASR timeout` | 网关没在 8 秒内回 `final:true` 的 ASR 文本；先用 `mock_cloud_audio.py` 确认设备侧正常 |
| ESP32 日志 `cloud endpoint not configured` | `MIBOT_CLOUD_WS_URI` 为空 |
| WS 每约 10 秒断开一次（close 1006） | 缺少保活，见 2.7 |
| `robot.speak` 被回 `E_CLOUD_TIMEOUT` | 网关未连上；ESP32 只在云端已连接时接受 TTS 请求 |
| 上行音频到不了云端 | 看 ESP32 遥测 `audio_up_frames_received` / `uplink_send_error`；把 `mibot_config.h` 的 `MIBOT_AUDIO_DIAG_TRACE` 置 1 可拿到详细计数 |
| 偶发一轮无 LLM 回复 | 1 Mbps UART 偶发误码，设备侧已自动重发一次（日志 `retry 1/1`） |
| 多个 Agent 实例互相抢设备 | 不要重复 `mibot_voice_agent &`；重烧或断电复位 SF32 |

---

## 8. 已知限制

- **未接真实云服务与真实 MiMo Key**：设备侧全部用本地 mock 验证。协议一致的前提下应可直接对接，但首次联调仍需你这边配合确认。
- **UART 偶发误码**：实测约 3 轮出现 1 次帧 CRC 失败，已用应用层重发兜住。如果后续要更硬的可靠性，方向是提波特率（需两侧同时改，SF32 波特率是白名单式映射）或加硬件流控（需再接 2 根线）。
- **上行丢帧已实测为 0**（2026-09-19，真机 + 本地 mock 网关）：SF32 报 `sent=401`，网关收到 `401 frames / 8.02 s`，一帧未丢。早期版本确实丢帧，应已被 32 KB UART RX 环与元数据 fail-open 那批修复解决。若你那边发现识别率低，**先看电平而不是丢帧**（见下条）。
- **上行电平偏低**：同一次实测，正常说话下整段 `rms≈171`、语音段帧峰值 200~660，约 -45 dBFS。ASR 通常期望 -30~-20 dBFS。必要时设备侧可上调 `ADC_CH0_CFG` 的 `ROUGH_VOL`（当前 `0xa`），或你那边在服务端做增益归一化。
- **每句开头 1~2 帧有启动瞬态**：ADC 按需使能导致首帧出现较大直流冲击（实测首帧峰值 9434，随后稳定在 400~660）。对整句识别影响很小，但**基于能量的 VAD/AGC 会在开头误判**，建议丢弃或忽略前 40 ms。
- **ToF 驱动已写但未上真机验证**：TOF050C（VL6180X）四路驱动已接入 `tof_read_mm()`，但是在没有硬件的情况下盲写的，只做了主机侧逻辑测试。任一通道未就绪时底盘运动仍保持安全锁定，含电机的 `robot.perform_action` 会被 NACK（这是预期的安全行为，不影响语音链路）。上电后看 ESP32 日志 `ToF real mode: VL6180X x4, N/4 channels ready` 判断实际状态。
- **没有离线唤醒词**：触发一轮对话有两种方式——按板上 KEY2（PA11，物理按键）或在 NSH 敲 `va_wake`。语音唤醒词尚未实现。按键路径同样是无硬件盲写的，未上真机验证。
- **流式播放喂帧节奏偏粗**：会看到少量 `underrun` / `dropped` 计数，听感正常，但正式接入前建议改为按帧稳定节奏。
- **不支持流式 LLM**：每轮等完整回复，暂不支持 `stream:true`。

---

## 9. 关键源码位置

| 功能 | 文件 |
|---|---|
| SF32 对话状态机与设备胶合层 | `sf32lb52/openvela-app/mibot_voice_agent_main.c` |
| SF32 ASR/TTS 桩与真实路径开关 | `sf32lb52/openvela-app/va_stubs.c` |
| SF32 音频驱动（AUDPRC/AUDCODEC） | `sf32lb52/openvela-app/sf32lb_i2s.c` |
| ESP32 音频链路（上下行、云端对接） | `esp32s3/mibot_esp32s3/main/mibot_audio.cpp` |
| ESP32 WebSocket 云端适配器 | `esp32s3/mibot_esp32s3/main/mibot_cloud_ws.cpp` |
| ESP32 MiMo 大模型网关 | `examples/deepseek_smoke/esp32/deepseek_gateway.cpp` |
| UART AA55 协议与机器人控制 | `esp32s3/mibot_esp32s3/main/mibot_controller.cpp` |
| 协议常量（帧类型、schema） | `esp32s3/mibot_esp32s3/main/mibot_protocol.h` |
