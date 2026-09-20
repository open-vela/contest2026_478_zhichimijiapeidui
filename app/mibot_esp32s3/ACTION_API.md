# Mibot 动作接口

本接口用于 `ESP32-S3 + SF32LB53-DevKit-LCD`。ESP32-S3 保存动作表并执行电机、舵机和安全检查；SF32 Agent 只负责把云端意图转换成命令，并根据 ESP32 的事件更新 LCD 表情。云端不能传入 GPIO、PWM、I2C 地址或无限时长。

## 1. 云端到 ESP32

云端 Tool 建议仍由 SF32 Agent 转发。通过 Wi-Fi TCP 或 SF32 UART 发送标准 `COMMAND (0x10)` 帧，payload 为 UTF-8 JSON：

```json
{
  "schema": "mibot.uart.v1",
  "msg_id": "msg_01",
  "trace_id": "trace_01",
  "command_id": "cmd_action_0001",
  "name": "robot.perform_action",
  "args": {
    "action": "happy",
    "intensity": 0.8
  }
}
```

字段约束：

| 字段 | 类型 | 约束 |
|---|---|---|
| `command_id` | string | 必须可重试且唯一；建议不超过 63 字符 |
| `name` | string | 固定为 `robot.perform_action` |
| `args.action` | string | `happy`、`sad`、`confused`、`greeting`、`thinking`、`warning` |
| `args.intensity` | number | `0.0..1.0`，缺省为 `1.0` |

### 1.1 `robot.move`

底盘移动的 canonical 参数是 `linear_mm_s` 和 `angular_deg_s`：

```json
{
  "schema": "mibot.uart.v1",
  "command_id": "cmd_move_0001",
  "name": "robot.move",
  "args": {
    "linear_mm_s": 60,
    "angular_deg_s": 0,
    "duration_ms": 800
  }
}
```

`linear_mm_s` 的范围为 `-120..120`，`angular_deg_s` 的范围为
`-90..90`，`duration_ms` 的范围为 `50..5000`（缺省值 `500`）。为兼容
早期 Skill，SF32 Tool 和 ESP32 控制器也接受 `direction` + `speed`：

| `direction` | 转换后的字段 |
|---|---|
| `forward` | `linear_mm_s = +abs(speed)` |
| `backward` | `linear_mm_s = -abs(speed)` |
| `left`、`turn_left` | `angular_deg_s = +abs(speed)` |
| `right`、`turn_right` | `angular_deg_s = -abs(speed)` |

`speed` 仅是兼容别名，范围为 `-100..100`；canonical 字段同时存在时优先
使用 canonical 字段。两种输入最终都经过同一套 ESP32 ToF、桌边、状态机和
动作时限检查，别名不会绕过安全限制。`robot.move` 不能传入左右轮 PWM。

### 1.2 `robot.set_text`

`robot.set_text`（兼容别名 `display.show_text`）只接受 LCD 状态文本，参数为
`args.text`。SF32 LCD 当前显示缓冲为 31 字节，允许字符为 ASCII 字母、数字、
`_`、`-`、`.`；建议 Tool 调用遵守该限制。ESP32 不执行显示逻辑，只通过同一
条业务 UART 转发命令，动作和音频帧仍可并行复用该链路。

ESP32 先返回 `ACK state=accepted`。动作完成后，再返回同一 `command_id` 的 `ACK state=completed`；被安全逻辑打断时返回 `NACK`，例如：

```json
{
  "schema": "mibot.uart.v1",
  "ok": false,
  "command_id": "cmd_action_0001",
  "state": "rejected",
  "error": {"code": "E_TOF_INVALID"}
}
```

动作不能并发。动作执行期间再次发送动作或 `set_arm_pose`，返回 `E_BUSY`。`robot.stop` 会取消当前动作；`emergency=true` 进入最高优先级刹车。

## 2. ESP32 到 SF32 LCD

每个动作开始、切换步骤、完成或中止时，ESP32 发送 `EVENT (0x20)`。如果动作由 Wi-Fi 发起，ESP32 会同时向 Wi-Fi 客户端和 UART 的 SF32 各发送一份事件。

事件格式：

```json
{
  "schema": "mibot.event.v1",
  "event": "action_update",
  "severity": "info",
  "source": "esp32.action",
  "requires_ack": false,
  "data": {
    "phase": "step",
    "action": "happy",
    "step": 2,
    "step_count": 4,
    "expression": "happy",
    "expression_intensity": 0.8,
    "left_servo_deg": 120.4,
    "right_servo_deg": 59.6,
    "left_motor": 22.4,
    "right_motor": -22.4
  }
}
```

`phase` 枚举：

- `started`：ESP32 已接受并执行第一步，SF32 显示动作对应表情。
- `step`：进入下一步，SF32 更新表情或动画参数。
- `completed`：所有步骤完成，SF32 可以恢复待机表情。
- `action_aborted`：被 ToF、急停、UART 超时或其他安全条件抢占，SF32 显示 `warning` 或 `error`。

SF32 不应在收到 `accepted` 时显示“动作完成”，必须等 `phase=completed` 或最终 `ACK state=completed`。LCD 表情只使用固定枚举：

```text
idle, listening, thinking, happy, sad, confused, speaking, warning, error
```

单独设置表情时可发送：

```json
{
  "schema": "mibot.uart.v1",
  "command_id": "cmd_expression_0001",
  "name": "robot.set_expression",
  "args": {
    "name": "thinking",
    "duration_ms": 1500,
    "intensity": 0.8
  }
}
```

ESP32 会以 `event=expression_update` 转发给 SF32。这个命令不驱动电机和舵机。

## 3. 内置动作表

动作表位于 `main/mibot_controller.cpp` 的 `ACTION_PLANS`。每一步包含：

```text
duration_ms       步骤时长
left_motor        左电机目标速度，-100..100
right_motor       右电机目标速度，-100..100
left_servo_deg    左舵机基准角度，0..180
right_servo_deg   右舵机基准角度，0..180
expression        LCD 固定表情名
expression_intensity 该步骤的表情强度
```

当前预置动作：

| 动作 | 视觉/机械含义 |
|---|---|
| `happy` | 双臂抬起，底盘小幅左右摆动，再回中 |
| `sad` | 双臂下垂，底盘低速后退，再回到低头姿态 |
| `confused` | 头/手臂左右交替倾斜，底盘左右小幅转向 |
| `greeting` | 右臂连续挥手，表情为 `happy` |
| `thinking` | 双臂轻微收拢，底盘小幅摆动，表情为 `thinking` |
| `warning` | 双臂张开并短暂后退，表情为 `warning` |

`intensity` 只缩放动作表中的电机速度以及舵机相对 90° 的偏移，不会扩大动作范围：

```text
实际舵机角度 = 90 + (表中角度 - 90) * intensity
实际电机速度 = 表中速度 * intensity
```

## 4. 安全行为

- 含电机的动作步骤必须通过四路 ToF 有效性检查。
- 前方 ToF 检测到桌面边缘时，ESP32 本地立即刹车，不等待云端、SF32 或 LCD。
- ToF 无效、动作超时、UART 租约超时、低电压或运动状态为 `FAULT` 时，动作中止。
- `robot.stop` 优先级高于普通动作；急停不会等待当前动作自然结束。
- 舵机角度在 ESP32 内部再次限幅到 `0..180`，实际机械限位应在代码表中进一步收紧。
- 动作完成以 ESP32 的最终 ACK 为准，云端收到 Tool Call 不代表动作已经执行。

`tof_read_mm()` 已接入真实驱动：TOF050C（ST **VL6180X**，注意 TOF200C 是 VL53L0X、TOF400C 是 VL53L1X，寄存器互不兼容），四路连续测距，实现在 `main/mibot_tof.cpp` + `main/mibot_vl6180x.c`。

**该驱动尚未在真实硬件上验证过**（无板期间盲写）。上电看这行日志确认实际情况：

```text
I mibot: ToF real mode: VL6180X x4, 4/4 channels ready
```

少于 4/4 时，未就绪的通道读数保持无效，含电机的动作仍会以 `E_TOF_INVALID` 被拒；`mibot_tof` 标签的日志会说明是哪一路、失败在哪一步。若打印 `MODEL_ID ... is not a VL6180X`，说明装的不是 TOF050C。

不要为了演示而关闭安全检查；如需台架验证动作时序，可以临时在 `mibot_config.h` 设置 `MIBOT_SIMULATE_TOF=1`，接入真实底盘前必须恢复为 `0`。

## 5. 新增动作

1. 在 `main/mibot_controller.cpp` 增加一个 `constexpr ActionStep[]` 数组。
2. 每个步骤设置有限的 `duration_ms`，不要使用无限循环。
3. 将数组加入 `ACTION_PLANS`，名称使用小写 ASCII 并同步更新本文档。
4. 重新编译并验证：`accepted -> action_update(started) -> action_update(step...) -> completed`。
5. 在桌面边缘、ToF 无效、UART 拔线和 `robot.stop` 场景下验证动作会停止。

不要修改 `set_motor()`、`set_servo()` 的安全限幅来实现新表情；动作表是云端和 Agent 可见的唯一高层动作接口。

## 6. UART RGB 联调指示

ESP32 板载 GPIO48 WS2812B 已加入 UART 指示。颜色只反映 ESP32 侧状态：

| ESP32 灯色 | 含义 |
|---|---|
| 蓝色 | ESP32 正在向 SF32 发送 UART 帧 |
| 黄色 | ESP32 UART 收到字节，正在组帧 |
| 绿色 | 收到完整帧且 CRC 正确，已交给协议处理 |
| 红色 | 收到完整帧但 CRC 错误 |

SF32 端的 RGB 应使用相同的语义，但方向以 SF32 自身为准：

| SF32 灯色 | 含义 |
|---|---|
| 蓝色 | SF32 正在向 ESP32 发送帧 |
| 黄色 | SF32 UART 收到 ESP32 字节 |
| 绿色 | SF32 收到完整且合法的 `HELLO_ACK`、`PONG` 或 `ACK` |
| 红色 | SF32 CRC/长度/版本校验失败，或收到 `NACK` |

因此一次完整的 `HELLO` 测试应观察到：

```text
SF32 蓝色 -> ESP32 黄色 -> ESP32 绿色
ESP32 蓝色 -> SF32 黄色 -> SF32 绿色
```

只有两块板都出现绿色，才表示双向帧通信和协议解析均成功。若 ESP32 绿、SF32 不绿，优先检查 SF32 的帧格式、CRC16 和 UART 参数；若两边都只有黄色，检查帧头 `AA 55`、长度字段和波特率；若红色，先修正 CRC 或数据位配置。

建议 SF32 在 `HELLO_ACK`、`PONG`、命令 `ACK` 和 `NACK` 后分别保持指示灯 300 ms，避免瞬间闪烁难以观察。安全事件和 RGB 指示不得阻塞 UART 接收任务。

## 7. 音频命令

音频命令沿用 `mibot.uart.v1` 的 `COMMAND`/`ACK`/`NACK` 封装。回复固定包含 `ok`、`command_id`、`state`、`result`、`error` 五个字段；`state` 为 `completed` 或 `rejected`。

| 命令 | 参数 | 说明 |
|---|---|---|
| `robot.speak` | `text`（1-500 个 UTF-8 码点）、`voice`（缺省 `default`）、`interruptible`（缺省 `true`） | 云端 TTS 下行流 |
| `robot.stop_audio` | 无 | 停止当前下行流；可重复调用 |
| `robot.get_audio_stats` | 无 | 返回 `result.audio` 诊断对象 |
| `robot.reset_audio_stats` | 无 | 清零诊断计数器 |
| `robot.set_audio_mode` | `mode`: `cloud` / `loopback` / `capture` | 切换模式并清空双向缓冲 |
| `robot.play_test_tone` | `duration_ms` 20..10000、`freq_hz` 20..8000 | 本地 16 kHz 正弦测试音 |

常见错误码为 `E_INVALID_ARG`、`E_EXPIRED`、`E_DUPLICATE`、`E_CLOUD_TIMEOUT` 和 `E_UNSUPPORTED`。`robot.stop_audio` 与急停在云端降级期间仍可执行。

## 8. 音频遥测

`TELEMETRY` 的 `audio` 对象默认只发送低带宽运行状态：`buffer_profile`、`direction`、`mode`、`cloud_state`、`degraded` 和 `stream_id`。生产固件将 `MIBOT_AUDIO_DIAG_TRACE` 设为 0，避免 1 Hz/10 Hz 遥测把 UART 挤满；需要阶段 1 打点时临时设为 1，遥测会追加 13 个累计计数器及全部诊断度量。`robot.get_audio_stats` 始终返回完整诊断对象，不受该开关影响。

音频实现与测试使用以下稳定的线协议标识符：上行帧计数器
`audio_up_frames_received`、下行欠载计数器 `downlink_underrun`，以及
PCM 元数据 schema `mibot.audio.v1`。这些名称属于诊断/兼容性契约，云端
适配器不应改写。

## 9. 音频事件

音频事件使用 `schema="mibot.event.v1"`、`source="esp32"`、`requires_ack=false`。`cloud_disconnected` 为 `warning`；`cloud_reconnected` 为 `info`；`audio_codec_unsupported`、`audio_no_mem` 为 `error`；`audio_buffer_degraded` 为 `warning`。新增事件的具体诊断字段放在 `data` 对象中。

## 10. 待回写上游规范

以下条目状态为“待提交”：五条音频控制命令及其返回结构、四个新增音频事件、`TELEMETRY.audio` 字段、ESP32 与云端的 WebSocket over TLS 约定，以及 UART 默认波特率由 921600 更正为 1000000（以两侧现有代码实测为依据）。
