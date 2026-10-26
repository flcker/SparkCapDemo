# SparkCapDemo 项目约定

SparkCap（Windows 摄像头统一采集库）的**下游接入示例应用**：摄像头实时预览（Win32 GDI，零第三方依赖）。

## 与 SparkCap 的关系

- 本项目为**独立仓库**，不依赖 SparkCap 源码在同机——依赖以 SDK 形式内置（`sdk/`）：
  - `sdk/include/`：公开头（sparkcap.h / sparkcap_types.h）
  - `sdk/lib/sparkcap.lib`：导入库（Release x64）
  - `sdk/bin/sparkcap.dll`：运行时 DLL（Release x64，构建后自动拷贝到输出目录）
- **升级 SparkCap**：从 SparkCap 仓库 `out/x64/Release/` 拷贝新的 lib/dll（及如接口变更则更新头文件）覆盖 `sdk/`，在此记录版本：

| 项 | 值 |
|---|---|
| SDK 来源 | SparkCap @ 6f2bfcd（LGPL-3.0 落地版） |
| 架构 | x64 / Release |
| 许可 | 本项目同样 LGPL-3.0（与依赖同族，见 LICENSE） |

## 构建

    cmake --preset default
    cmake --build --preset release

产物：`build/Release/sparkcap_demo.exe`（DLL 已自动就位）。

## 应用说明

- 拉取模式（`sparkcap_get_frame`），~30fps 定时器节奏
- 输出侧全格式软件转换 → RGB32 渲染：YUY2 / I420 / NV12 / RGB24（含负 stride bottom-up）/ RGB32；MJPG 等压缩格式提示不支持
- 按键：**ESC** 退出 | **空格** 暂停/恢复 | **N** 切换下一台设备
- 标题栏实时显示：设备序号/名称、分辨率、fps

## 约定

- 零第三方依赖（Win32 + 内置 SDK），与 SparkCap 库哲学一致
- 接口用法以 SparkCap 的 `docs/spec.md`（§4.5 调用顺序）为准
