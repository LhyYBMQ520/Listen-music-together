# Listen Music Together

一起听歌网页全栈项目。Windows 桌面端捕获系统音频并通过 WebSocket 推送到浏览器实时播放，同时展示 SMTC 媒体元数据（歌名/歌手/专辑封面/进度）。

## 架构

```
audio.exe (C++ WASAPI) ──PCM帧协议──> go-server (Go TUI) ──WebSocket──> browser (JS)
                                            │
                              smtc.exe (C# WPF, :9863 HTTP)
```

| 模块 | 语言 | 职责 |
| :--- | :--- | :--- |
| [audio](./src/audio/) | C++ | WASAPI loopback 音频捕获，二进制帧协议输出到 stdout |
| [smtc](./src/smtc/) | C# | Windows SMTC API 轮询，HTTP 暴露 JSON/Protobuf |
| [golang-server](./src/golang-server/) | Go | 主控调度、WebSocket 广播、Bubbletea TUI |
| [public](./public/) | JS | Web Audio API 播放、SMTC UI 展示 |

## 项目结构

```
Listen-music-together/
├── public/
│   ├── index.html          # 前端页面（Tailwind CSS + 歌曲信息 UI）
│   └── script.js           # 前端逻辑（WebSocket、AudioPlayer）
├── src/
│   ├── audio/              # C++ 音频捕获模块
│   ├── smtc/               # C# SMTC 媒体信息采集模块
│   └── golang-server/      # Go 后端主控服务
├── README.md
```

## 快速开始

### 前置条件

- Visual Studio 2022+ (C++ 桌面开发工作负载)
- .NET 8 SDK
- Go 1.26+
- Windows 10 2004+ (build ≥ 19041)

### 构建

```powershell
# 1. C# smtc.exe
Visual Studio 轮椅一键 Release|x64 构建，发布一条龙（推荐打开单文件发布）

# 2. C++ audio.exe 
Visual Studio 轮椅 Release|x64 编译

# 3. Go server
go build
```

### 运行

```
将 smtc.exe 及其其他几个无法打包入exe的dll全部复制粘贴，放在 bin/smtc 文件夹下
将 audio.exe 复制粘贴，放在 bin/audio 文件夹下
将 Go server 编译产出的exe放在 bin 文件夹下
# 浏览器打开 http://ip:9090
# 运行Go server 并在 TUI 中输入会话序号开始捕获
```

## 模块文档

- [C++ 音频模块](./src/audio/README.md)
- [C# SMTC 模块](./src/smtc/README.md)
- [Go Server 模块](./src/golang-server/README.md)
