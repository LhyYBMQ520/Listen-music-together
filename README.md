# Listen-music-together

这是一个正在开发中的一起听歌网页前后端全栈项目，完成度未知，能否完工未知。。。

## 当前项目结构

```
Listen-music-together/
├── public/                         # 前端页面
│   └── index.html                  # Web 前端主页面，用户交互界面
├── src/                            # 后端源码
│   ├── audio/                      # C++ 音频捕获模块（基于 WASAPI）
│   │   ├── audio.cpp               # 核心源码：枚举系统音频会话、捕获音频数据、支持写入 WAV 文件
│   │   ├── audio.slnx / .vcxproj   # Visual Studio 解决方案与项目文件
│   │   └── README.md               # 模块自述文档（编译指南、API 说明）
│   ├── smtc/                       # C# SMTC 媒体信息采集模块（基于 Windows SMTC API）
│   │   ├── App.xaml                # WPF 应用程序定义（资源、启动窗口等）
│   │   ├── App.xaml.cs             # WPF 应用程序启动逻辑
│   │   ├── AssemblyInfo.cs         # 程序集元数据
│   │   ├── MainWindow.xaml         # WPF 主窗口界面布局
│   │   ├── MainWindow.xaml.cs      # 主窗口逻辑：每 200ms 轮询系统媒体会话，获取歌曲/歌手/进度等
│   │   ├── SmtcHttpServer.cs       # 轻量级 HTTP 服务器，监听 localhost:9863，对外暴露 JSON/Protobuf 数据
│   │   ├── SmtcData.cs             # Protobuf 数据模型（播放状态、时间线等）
│   │   ├── smtc_data.proto         # 跨语言 Proto 定义文件
│   │   ├── smtc.csproj             # .NET 8 项目配置文件
│   │   ├── smtc.slnx               # Visual Studio 解决方案文件
│   │   └── README.md               # 模块自述文档（API 端点说明、使用示例）
│   └── golang-server/              # Go 网页后端&模块组装服务（规划中，尚未实现）
├── LICENSE                         # 开源许可证
└── README.md                       # 项目总览文档
```

## 已完工模块自述文档
- [C++音频模块](./src/audio/README.md)
- [C#SMTC模块](./src/smtc/README.md)
