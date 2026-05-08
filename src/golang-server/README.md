# Go Server — 模块拼装+主控 & WebSocket 服务端

这是 **Listen Music Together** 的核心调度模块，使用 Go 编写。负责管理 audio.exe 和 smtc.exe 子进程生命周期，通过 WebSocket 将音频流和 SMTC 元数据广播到浏览器客户端，并提供 Bubbletea TUI 控制面板。

## 技术栈

| 项 | 说明 |
| :--- | :--- |
| 运行时 | Go 1.26+ |
| WebSocket | gorilla/websocket |
| TUI | charmbracelet/bubbletea + lipgloss |
| 音频子进程 | Windows WASAPI 捕获 (C++ audio.exe) |
| 媒体元数据 | SMTC HTTP 轮询 (C# smtc.exe at :9863) |

## 项目结构

```
golang-server/
├── main.go       # 入口：flag 解析、定位 binary、启动 TUI
├── manager.go    # AudioManager（音频进程管理）+ SMTCManager + 帧协议解析
├── server.go     # Hub（WebSocket 广播）+ HTTP 路由 + SMTC 轮询 + protobuf 缩略图提取
├── tui.go        # Bubbletea TUI 界面（会话列表、日志、捕获状态）
├── go.mod        # Go 模块定义 (go 1.26.1)
└── go.sum        # 依赖校验
```

## 架构与数据流

```
audio.exe stdout (帧协议) ──> AudioManager ──> Hub ──WebSocket──> browser
                                                 │
smtc.exe :9863/json ──HTTP──> PollSmtcAndBroadcast ─┤
                                                 │
                                          HTTP :9090
                                          ├── /         静态文件 (public/)
                                          ├── /ws       WebSocket 端点
                                          ├── /api/smtc  SMTC 代理
                                          └── /api/thumbnail  封面图片
```

## 核心组件

### AudioManager (`manager.go`)

管理 audio.exe 子进程。audio.exe 通过 WASAPI 捕获系统音频并以二进制帧协议写入 stdout；AudioManager 解析帧并转发到 Hub。

- `Start(audioPath)` — 启动 audio.exe，自动进入 slave 模式
- `ListSessions()` / `RefreshAndListSessions()` — 读取音频会话列表
- `SelectSession(index)` — 选择捕获目标会话
- `ReadAudioAndBroadcast()` — 阻塞循环，分派帧类型到 Hub 广播
- `StopCapture()` / `Shutdown()` — 控制捕获生命周期

### 二进制帧协议

```
Wire format: [type:1][length:3 big-endian][payload:N]
```

| Type | Name | Payload |
|------|------|---------|
| `0x01` | TEXT | UTF-8 协议行 |
| `0x02` | AUDIO_CHUNK | `[timestamp_us:8 LE][PCM:N]` |
| `0x03` | AUDIO_FORMAT | 10–12 字节格式描述 |
| `0xFF` | SHUTDOWN | 空, 进程干净退出 |

### Hub (`server.go`)

WebSocket 广播中心，维护客户端连接集合并分发 Text/Binary 消息。

### SMTC 集成

- `PollSmtcAndBroadcast` — 每 200ms 轮询 `localhost:9863/json`，通过 WebSocket 广播媒体元数据
- `/api/smtc` — 代理 SMTC JSON 到前端
- `/api/thumbnail` — 从 protobuf 二进制中提取封面图片 (field 7)

### TUI (`tui.go`)

基于 Bubbletea 的终端控制面板：
- 左侧：音频会话列表（自动刷新 3s），输入序号选择捕获
- 右侧：服务器日志
- 快捷键：`Enter` 选择/停止捕获，`Backspace` 删除数字，`Ctrl+C`/`ESC`/`q` 退出

## 构建与运行

### 前置条件

- Go 1.26+
- 同一目录下需存在编译好的 `audio/audio.exe` 和 `smtc/smtc.exe` 及其附带的 **dll**

### 编译&运行参考项目根目录下的README.md

### 命令行参数（可选择不添加，默认值已给出）

| 参数 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `-port` | `9090` | HTTP 服务端口 |
| `-public` | `../public` (相对于 binary) | 前端静态文件目录 |

## 退出行为

- `Ctrl+C` / `ESC` / `q`：通过 `Process.Kill()` 终止 audio 和 smtc 子进程，然后退出
- audio.exe 退出时会先发送 `EXIT` 命令，随后 `Process.Kill()` 确保清理
