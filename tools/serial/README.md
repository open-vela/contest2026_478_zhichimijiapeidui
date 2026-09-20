# 串口工具

两块板的串口参数不同，别接错：

| 板 | 波特率 | 用途 |
|---|---|---|
| SF32 | **1000000** (1 Mbps) | NSH 交互控制台，同时也是烧录口 |
| ESP32 | **115200** | 日志输出（`ESP_LOGx`） |

**同一时刻只有一个程序能打开一个串口。** 下面这些工具、烧录脚本、串口终端互相冲突，用之前先关掉别的。查占用：

```powershell
Get-Process python, putty, ImgDownUart -ErrorAction SilentlyContinue
```

## 包内自带的工具

都是 Python 脚本，位置在 `src\` 下。先装依赖（用 `python -m pip`，别用裸 `pip`，否则容易装进另一个解释器）：

```powershell
python -m pip install pyserial websockets
```

### 端到端测试（最常用，一条命令跑完整对话）

同时打开两块板的串口，自动启动语音 Agent、触发多轮对话、抓两边日志、逐项给 PASS/FAIL。

```powershell
cd src\sf32lb52
python script\e2e_voice_test.py --sf32 COM5 --esp32 COM7 --turns 2 --turn-timeout 30 --stub
```

日志落在当前目录的 `e2e_sf32.log` / `e2e_esp32.log`。常用参数：

| 参数 | 说明 |
|---|---|
| `--stub` | 板上是全桩固件时加（包内预编译固件就是全桩） |
| `--reset-esp32` | 先复位 ESP32，日志从干净状态开始 |
| `--echo` | 实时把原始日志打到屏幕 |
| `--turns N` | 跑 N 轮对话 |

**优先用这个。** 它把两个串口收在一个进程里，避免了两个终端抢同一个口的问题。

### 给 SF32 发 NSH 命令

```powershell
cd src\sf32lb52
python script\sf32_cmd.py --port COM5 --cmd "va_wake"
```

发完命令会读一小段回显再退出，读取时长用 `--read-ms` 调（默认较短，等一整轮对话要给足，比如 `--read-ms 20000`）。

常用的板上命令（也可以在串口终端里直接敲）：

```text
mibot_voice_agent &   启动语音 Agent（一次就够，别重复启动）
va_wake               手动触发一轮对话
va_test spk 3         扬声器测试音
va_test mic 3         麦克风采集（应报 ~150 帧、dropped=0）
va_test hw 2          codec 内置 1 kHz，绕过 PCM/DMA，隔离模拟链路
va_test spkmic 3      边放边录，输出能量包络
```

### 只看 ESP32 日志

抓固定时长的日志到文件（不是持续终端，抓完就退出）：

```powershell
cd src\esp32s3\mibot_esp32s3
python tools\mon.py --port COM7 --seconds 30 --out esp32.log
```

装了 ESP-IDF 的话，官方监视器功能更全（`Ctrl+]` 退出，能自动解析崩溃回溯）：

```powershell
idf.py -B build-deepseek-smoke -p COM7 monitor
```

### 同时观察两块板

发一条 SF32 命令，同时抓两块板的日志（注意参数名是 `--sf-port` / `--esp-port`）：

```powershell
cd src\sf32lb52
python script\dual_probe.py --sf-port COM5 --esp-port COM7 --cmd "va_wake"
```

## 用现成的串口终端

想要 GUI 的话，任意串口终端都行（包里没带，避免分发第三方二进制）：PuTTY、MobaXterm、Windows Terminal + plink、VS Code 的 Serial Monitor 插件都可以。

注意两点：SF32 要设成 **1000000** 波特率（有些终端下拉框里没有，需要手填）；接 SF32 时**关掉本地回显**，否则你敲的命令会被 NSH 看到两遍。
