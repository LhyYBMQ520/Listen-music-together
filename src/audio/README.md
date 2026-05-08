# 音频管理模块 (Audio Module)

## 项目概述

这是 **Listen Music Together** 项目的Windows音频后端模块，使用C++和Windows WASAPI（Windows Audio Session API）实现。该模块提供了音频会话列表显示以及音频数据捕获功能。

## 核心功能

### 1. 音频会话枚举与监控
- 获取系统中所有活跃的音频会话
- 显示每个音频会话的详细信息，包括：
  - 进程ID和进程名称
  - 会话显示名称
  - 会话标识符
  - 会话状态（活跃/非活跃/已过期）
  - 实时音量和静音状态

### 2. 音频设备管理
- 检测和识别默认音频渲染设备
- 获取设备友好名称
- 查询音频设备的混合格式信息

### 3 音频数据捕获
- 支持将音频数据写入WAV文件
- 自动处理WAV文件格式头
- 支持多种音频格式（由操作系统支持）

### 4. 系统信息收集
- 获取Windows版本号和构建号
- 进程名称和路径解析
- 错误信息格式化和输出

## 技术架构

### 使用的Windows API
- **WASAPI (Windows Audio Session API)**
  - `IMMDeviceEnumerator` - 音频设备枚举
  - `IAudioSessionManager2` - 音频会话管理
  - `ISimpleAudioVolume` - 音量控制
  - `IAudioClient` - 音频客户端

- **多媒体设备API**
  - `mmdeviceapi.h` - 多媒体设备接口
  - `audioclient.h` - 音频客户端接口
  - `audiopolicy.h` - 音频策略接口

- **系统API**
  - `Windows.h` - 核心Windows功能
  - `tlhelp32.h` - 进程快照工具
  - `winternl.h` - 内部Windows函数

### 编译依赖
- `mmdevapi.lib` - 多媒体设备库
- `ole32.lib` - OLE库
- `propsys.lib` - 属性系统库

## 项目结构

```
audio/
├── audio.cpp           # 主程序源代码
├── audio.slnx          # Visual Studio解决方案文件
├── audio.vcxproj       # Visual Studio项目文件
├── audio.vcxproj.filters # 项目筛选器文件
├── audio.vcxproj.user  # 用户特定项目设置
└── README.md           # 本模块自述文件
```

## 关键数据结构

### AudioSessionInfo 结构体
```cpp
struct AudioSessionInfo {
    DWORD processId;           // 进程ID
    std::wstring processName;  // 进程名称
    std::wstring sessionName;  // 会话显示名称
    std::wstring sessionId;    // 会话唯一标识符
    AudioSessionState state;   // 会话状态
    float volume;              // 音量级别 (0.0 - 1.0)
    bool muted;                // 是否静音
};
```

## 主要功能模块说明

### 会话枚举函数
**函数：** `EnumerateAudioSessions()`
- 获取所有音频会话列表
- 返回完整的会话信息及设备名称
- 包含错误处理和资源清理

### WAV文件写入类
**类：** `WavWriter`
- 创建标准WAV格式文件
- 写入音频数据和格式信息
- 自动处理RIFF、fmt、data等数据块

### 工具函数
- `GetProcessName()` - 获取进程名称
- `GetDeviceName()` - 获取设备友好名称
- `GetDefaultRenderMixFormat()` - 获取默认混合格式
- `FormatBytes()` - 格式化字节大小显示
- `Trim()` - 字符串修剪

## 编译指南

### 环境要求
- Visual Studio 2019 或更高版本（推荐2026）
- Windows 10 Build 2004 或更高版本（由于`#define _WIN32_WINNT 0x0A00`）
- C++17标准库支持

### 编译步骤

1. 使用Visual Studio 2026（推荐）打开 `audio.slnx`
2. 选择配置：Debug 或 Release
3. 选择平台：x64
4. 编译

## 使用场景

本模块适用于以下场景：

1. **音乐应用同步** - 检测系统中正在播放的音乐应用
2. **音频监控工具** - 实时监控系统音频会话
3. **音频录制工具** - 捕获系统或特定应用的音频输出
4. **协作听音** - Listen Music Together项目的核心音频后端

## 网络集成

此模块可通过以下方式与前端集成：

- **REST API** - 通过HTTP endpoints暴露音频信息
- **WebSocket** - 实时推送音频会话变化
- **IPC通信** - 进程间通信获取音频数据
- **命令行工具** - 作为系统工具直接调用

## 注意事项

### 权限要求
- 某些音频操作可能需要管理员权限
- 获取某些进程信息可能受到权限限制

### 兼容性
- 仅支持Windows平台
- 需要WASAPI支持（Windows Vista及以上）
- 针对64位系统优化

### 虚拟音频设备
- 项目中定义了虚拟音频设备支持
- 常量 `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` 用于处理虚拟设备

## 错误处理

模块使用HRESULT错误代码和广泛的错误消息处理：
- 所有COM操作都包含错误检查
- 提供可读的错误信息输出
- 资源总是通过`SAFE_RELEASE`宏正确释放
