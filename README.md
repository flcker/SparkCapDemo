# SparkCapDemo

[SparkCap](../SparkCap) 的下游接入示例：**摄像头实时预览**（Win32 GDI，零第三方依赖）。

![License](https://img.shields.io/badge/license-LGPL--3.0-blue)

## 特性

- 统一设备枚举（USB / 虚拟摄像头 / IP Camera，含 `N` 键循环切换）
- 输出侧全格式软件转换与渲染：YUY2 / I420 / NV12 / RGB24 / RGB32
- 拉取模式取帧（`sparkcap_get_frame`，~30fps）
- 标题栏实时显示设备名、分辨率、帧率

## 依赖

SparkCap 以 **SDK 形式内置**（`sdk/`：头文件 + 导入库 + DLL）——克隆本仓库即可构建，无需 SparkCap 源码。

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
src/main.c         预览应用（~400 行，可直接作为接入参考）
```

## 许可

LGPL-3.0（与 SparkCap 同族；依赖声明见 SparkCap 的 [THIRD-PARTY-NOTICES.md](../SparkCap/THIRD-PARTY-NOTICES.md)，FFmpeg LGPL-2.1+ 静态内嵌于 sparkcap.dll）。
