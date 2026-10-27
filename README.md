# SparkCapDemo

[SparkCap](../SparkCap) 的下游接入示例：**摄像头实时预览**（Win32 GDI + spdlog 日志）。

![License](https://img.shields.io/badge/license-LGPL--3.0-blue)

## 特性

- 统一设备枚举（USB / 虚拟摄像头 / IP Camera，含 `N` 键循环切换）
- 输出侧全格式软件转换与渲染：YUY2 / I420 / NV12 / RGB24 / RGB32 / MJPG（GDI+ 解码）
- 格式下拉显示格式名与带宽估算（超 USB 2.0 预算标 `!`）
- 错误/日志回调集成：流断提示 + FFmpeg 诊断日志（预览区底部 + 文件落盘）
- 运行日志：`sparkcap_demo.log`（设备列表 / start 参数 / 首帧格式 / get_frame 状态跃变 / 回调事件）
- 标题栏实时显示设备名、分辨率、帧率

## 依赖

SparkCap 以 **SDK 形式内置**（`sdk/`：头文件 + 导入库 + DLL）。

| 要求 | 版本 |
|---|---|
| Visual Studio 2022 | 工具集 ≥ 14.30 |
| CMake | ≥ 3.24 |
| 架构 | x64 |

## 构建

```
cmake --preset default
cmake --build --preset release
```

运行 `build/Release/sparkcap_demo.exe`（DLL 已自动就位）。

> spdlog（日志，MIT）经 CMake FetchContent 获取（锁定 v1.15.1）——configure 阶段需要网络访问 GitHub。

## 按键

| 键 | 功能 |
|---|---|
| `ESC` | 退出 |
| `空格` | 暂停 / 恢复 |
| `N` | 切换下一台设备 |

## 目录

```
sdk/               SparkCap SDK（内置依赖）
  include/         公开头
  lib/             导入库（x64 Release）
  bin/             运行时 DLL（构建后自动拷贝）
src/main.cpp       预览应用（可直接作为接入参考）
tools/             诊断工具（probe 逐格式探针 / stride_diag 帧布局验证）
```

## 许可

LGPL-3.0（与 SparkCap 同族；依赖声明见 SparkCap 的 [THIRD-PARTY-NOTICES.md](../SparkCap/THIRD-PARTY-NOTICES.md)，FFmpeg LGPL-2.1+ 静态内嵌于 sparkcap.dll）。构建期依赖 spdlog（MIT，header-only）。
