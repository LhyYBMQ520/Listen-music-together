# SMTC 媒体信息采集模块

基于 Windows SMTC（System Media Transport Controls）API 的桌面应用，实时捕获系统当前正在播放的媒体信息（如网易云音乐、QQ 音乐、Foobar2000 等），并通过内置 HTTP 服务器对外暴露数据，供 Listen-music-together 项目的前端或其他服务消费。

## 技术栈

| 项 | 说明 |
| :--- | :--- |
| 运行时 | .NET 8（`net8.0-windows10.0.19041.0`） |
| UI 框架 | WPF（Windows Presentation Foundation） |
| 序列化 | protobuf-net 3.2.56 |
| HTTP 服务 | 自建 `TcpListener` 轻量 HTTP 服务器 |
| 数据模型 | Protocol Buffers（`.proto` 文件可跨语言生成客户端代码） |

## 项目结构

```
smtc/
├── smtc.csproj          # 项目文件
├── smtc_data.proto      # 跨语言参考的 Proto 定义
├── SmtcData.cs          # protobuf 数据模型（C# 实现）
├── SmtcHttpServer.cs    # 轻量 HTTP 服务器
├── MainWindow.xaml.cs   # 主窗口逻辑 & SMTC 采集核心
└── MainWindow.xaml      # WPF 界面布局
```

## 工作原理

```
┌──────────────────────┐     ┌───────────────────┐     ┌──────────────────────┐
│  Windows SMTC API    │────▶│  MainWindow 定时器 │────▶│  SmtcHttpServer      │
│  (系统媒体控制接口)    │     │  每 200ms 轮询采集   │     │  http://localhost:9863 │
└──────────────────────┘     └───────────────────┘     └──────────────────────┘
                                                              │
                                                    ┌─────────┴─────────┐
                                                    ▼                   ▼
                                              GET /              GET /json
                                         protobuf 二进制        JSON 文本
```

1. **MainWindow** 通过 `GlobalSystemMediaTransportControlsSessionManager` 获取系统当前所有媒体会话
2. **每 200ms 定时刷新**：自动跟踪正在播放的会话，采集歌曲标题、歌手、封面、播放进度等信息
3. **多会话支持**：当多个媒体应用同时运行时，自动跟随"正在播放"的会话；也支持手动切换
4. **SmtcHttpServer** 将采集到的数据通过 HTTP 暴露，供局域网或本机其他程序调用

## 项目文件变更与 API 说明

### 新增文件

| 文件 | 说明 |
| :--- | :--- |
| SmtcData.cs | protobuf 数据模型，包含 `SmtcData`、`PlaybackStatus`、`SmtcTimeline` 三个类型 |
| SmtcHttpServer.cs | 基于 `TcpListener` 的轻量 HTTP 服务器，监听 `http://localhost:9863/` |
| smtc_data.proto | 跨语言参考的 proto 定义文件 |

### 修改文件

- **smtc.csproj** — 添加了 protobuf-net NuGet 包
- **MainWindow.xaml.cs** — 集成 HTTP 服务器，每 200ms 推送最新 SMTC 数据

## 快速使用方式

### 方式 1：浏览器直接查看（JSON 端点）

程序运行后，浏览器打开：

```
http://localhost:9863/json
```

会返回可读的 JSON，示例输出：

```json
{
  "SourceApp": "网易云音乐",
  "SourceAppId": "NetEase.CloudMusic...",
  "Title": "青花瓷",
  "Artist": "周杰伦",
  "PlaybackStatus": "Playing",
  "Timeline": {
    "PositionSeconds": 45.2,
    "EndTimeSeconds": 240.0,
    "PositionTicks": 4520000000,
    "EndTimeTicks": 24000000000
  },
  "ThumbnailSize": 28476,
  "SessionIndex": 1,
  "SessionCount": 1
}
```

### 方式 2：Protobuf 二进制（代码调用）

**请求地址**：`GET http://localhost:9863/`
**返回格式**：`Content-Type: application/x-protobuf`，Body 是 `SmtcData` 消息的 protobuf 二进制编码。

其他程序可通过 `smtc_data.proto` 文件生成对应语言的客户端解码数据，**C# 调用示例**：

```csharp
using var client = new HttpClient();
var bytes = await client.GetByteArrayAsync("http://localhost:9863/");
var data = ProtoBuf.Serializer.Deserialize<SmtcData>(new MemoryStream(bytes));
Console.WriteLine($"{data.Title} - {data.Artist}");
```

### 方式 3：JavaScript 前端调用

```javascript
// JSON 端点，浏览器可直接用 fetch
const res = await fetch('http://localhost:9863/json');
const data = await res.json();
console.log(`${data.Title} - ${data.Artist} [${data.PlaybackStatus}]`);
```

## 数据结构

```protobuf
message SmtcData {
  string source_app       = 1;  // 应用友好名称（如"网易云音乐"）
  string source_app_id    = 2;  // 系统 AppUserModelId
  string title            = 3;  // 歌曲标题
  string artist           = 4;  // 歌手
  PlaybackStatus playback_status = 5;  // 播放状态
  SmtcTimeline timeline   = 6;  // 进度信息（秒 + ticks）
  bytes thumbnail         = 7;  // 封面图片原始字节
  int32 session_index     = 8;  // 当前会话序号
  int32 session_count     = 9;  // 总会话数
}

enum PlaybackStatus {
  UNKNOWN  = 0;
  PLAYING  = 1;
  PAUSED   = 2;
  STOPPED  = 3;
  CHANGING = 4;
  CLOSED   = 5;
}

message SmtcTimeline {
  double position_seconds  = 1;  // 当前播放位置（秒）
  double end_time_seconds  = 2;  // 总时长（秒）
  int64  position_ticks    = 3;  // 当前播放位置（ticks）
  int64  end_time_ticks    = 4;  // 总时长（ticks）
}
```

## 跨域支持

`/` 和 `/json` 两个端点均支持 CORS 跨域（`Access-Control-Allow-Origin: *`），浏览器可直接用 fetch 请求。

## 构建与运行

### 环境要求

- Windows 10 19041+ 或 Windows 11
- .NET 8 SDK

### 构建

```bash
cd smtc
dotnet build
```

### 发布为单文件

```bash
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

### 运行

```bash
dotnet run
```

程序启动后会显示 WPF 窗口，同时 HTTP 服务器在 `localhost:9863` 开始监听。

## 启动提示

先启动程序，然后在浏览器访问 `http://localhost:9863/json` 就能直接看到实时数据了。

## 扩展：用其他语言消费 Protobuf 数据

以 `smtc_data.proto` 为蓝本，可生成各语言的客户端代码：

**Python：**
```bash
protoc --python_out=. smtc_data.proto
```

**TypeScript：**
```bash
protoc --ts_out=. smtc_data.proto
```

**Go：**
```bash
protoc --go_out=. smtc_data.proto
```
