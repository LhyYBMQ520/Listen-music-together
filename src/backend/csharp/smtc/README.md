# 项目文件变更与API说明
## 新增文件
| 文件 | 说明 |
| :--- | :--- |
| SmtcData.cs | protobuf 数据模型，包含 SmtcData、PlaybackStatus、SmtcTimeline 三个类型 |
| SmtcHttpServer.cs | 基于 TcpListener 的轻量 HTTP 服务器，监听 `http://localhost:9863/` |
| smtc_data.proto | 跨语言参考的 proto 定义文件 |

## 修改文件
- smtc.csproj — 添加了 protobuf-net NuGet 包
- MainWindow.xaml.cs — 集成 HTTP 服务器，每 200ms 推送最新 SMTC 数据

## 快速使用方式
### 方式 1：浏览器直接查看（JSON 端点）
程序运行后，浏览器打开：
```
http://localhost:9863/json
```
会返回可读的 JSON，示例输出：
```json
{
  "SourceApp": "\u7F51\u6613\u4E91\u97F3\u4E50",
  "SourceAppId": "NetEase.CloudMusic...",
  "Title": "\u9752\u82B1\u74F7",
  "Artist": "\u5468\u6770\u4F26",
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
**返回格式**：`Content-Type: application/x-protobuf`，Body 是 SmtcData 消息的 protobuf 二进制编码。

其他程序可通过 `smtc_data.proto` 文件生成对应语言的客户端解码数据，**C# 调用示例**：
```csharp
using var client = new HttpClient();
var bytes = await client.GetByteArrayAsync("http://localhost:9863/");
var data = ProtoBuf.Serializer.Deserialize<SmtcData>(new MemoryStream(bytes));
Console.WriteLine($"{data.Title} - {data.Artist}");
```

## 数据结构
```protobuf
message SmtcData {
  string source_app       // 应用友好名称（如"网易云音乐"）
  string source_app_id    // 系统 AppUserModelId
  string title            // 歌曲标题
  string artist           // 歌手
  PlaybackStatus playback_status  // 播放状态
  SmtcTimeline timeline   // 进度信息（秒 + ticks）
  bytes thumbnail         // 封面图片原始字节
  int32 session_index     // 当前会话序号
  int32 session_count     // 总会话数
}
```

## 跨域支持
`/` 和 `/json` 两个端点均支持 CORS 跨域（`Access-Control-Allow-Origin: *`），浏览器可直接用 fetch 请求。

## 启动提示
先启动程序，然后在浏览器访问 `http://localhost:9863/json` 就能直接看到实时数据了。
