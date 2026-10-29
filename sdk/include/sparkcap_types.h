#ifndef SPARKCAP_TYPES_H
#define SPARKCAP_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 导出宏（docs/naming-conventions.md §6） */
#if defined(SPARKCAP_EXPORTS)
#  define SPARKCAP_API __declspec(dllexport)
#else
#  define SPARKCAP_API __declspec(dllimport)
#endif

/* 版本（docs/naming-conventions.md §7） */
#define SPARKCAP_VERSION_MAJOR 1
#define SPARKCAP_VERSION_MINOR 0
#define SPARKCAP_VERSION_PATCH 0

/* 句柄（不透明） */
typedef struct sparkcap_backend sparkcap_backend_t;
typedef struct sparkcap_device  sparkcap_device_t;

/* 后端来源（F1） */
typedef enum sparkcap_backend_kind_t {
    SPARKCAP_BACKEND_MF     = 0,
    SPARKCAP_BACKEND_DSHOW  = 1,
    SPARKCAP_BACKEND_FFMPEG = 2,
} sparkcap_backend_kind_t;

/* 像素格式——输出侧格式；库端不做转换（Spec §2、技术选型 §5） */
typedef enum sparkcap_pixfmt_t {
    SPARKCAP_PIXFMT_UNKNOWN = 0,
    SPARKCAP_PIXFMT_NV12    = 1,
    SPARKCAP_PIXFMT_YUY2    = 2,
    SPARKCAP_PIXFMT_I420    = 3,
    SPARKCAP_PIXFMT_MJPG    = 4,
    SPARKCAP_PIXFMT_RGB32   = 5,  /* 仅当设备原生输出 */
    SPARKCAP_PIXFMT_RGB24   = 6,  /* DShow 虚拟摄像头常见原生输出 */
} sparkcap_pixfmt_t;

/* 错误码（Spec §4.2；函数返回 int，值域为本枚举） */
typedef enum sparkcap_err_t {
    SPARKCAP_ERR_OK             = 0,
    SPARKCAP_ERR_INVALID_ARG    = 1,
    SPARKCAP_ERR_NOT_FOUND      = 2,
    SPARKCAP_ERR_ALREADY_OPEN   = 3,
    SPARKCAP_ERR_DEVICE_BUSY    = 4,
    SPARKCAP_ERR_START_FAILED   = 5,
    SPARKCAP_ERR_TIMEOUT        = 6,
    SPARKCAP_ERR_STREAM_LOST    = 7,
    SPARKCAP_ERR_UNSUPPORTED    = 8,
    SPARKCAP_ERR_INVALID_HANDLE = 9,
    SPARKCAP_ERR_NO_MEMORY      = 10,
    SPARKCAP_ERR_IN_CALLBACK    = 11,
    SPARKCAP_ERR_INTERNAL       = 12,
    SPARKCAP_ERR_SOURCE_PAUSED  = 13, /* 源暂停（窗口最小化等）：经错误回调通知一次，
                                          期间 get_frame 返回 TIMEOUT，恢复后自动继续 */
} sparkcap_err_t;

/* 设备能力条目（F11）：一种分辨率/格式/帧率组合；fps 为四舍五入整数值 */
typedef struct sparkcap_device_format_t {
    int               width;
    int               height;
    sparkcap_pixfmt_t pixel_format;
    int               fps;
} sparkcap_device_format_t;

/* 设备信息（F1；能力经 sparkcap_camera_get_formats 按需查询） */
typedef struct sparkcap_device_info_t {
    int                      id;        /* deviceId，供 sparkcap_camera_open 使用 */
    char                     name[256];
    char                     path[512]; /* 设备符号链接或 RTSP URL */
    sparkcap_backend_kind_t  backend;
} sparkcap_device_info_t;

/* 显示器信息（F12；屏幕域枚举条目）。
 * id 为当次枚举内的稳定序号，供 sparkcap_screen_open 使用（显示拓扑变化后
 * 以重新枚举为准）；width/height 为当前桌面分辨率。 */
typedef struct sparkcap_display_info_t {
    int  id;         /* displayId */
    char name[256];  /* 显示器友好名（回退 GDI 设备名，如 \\.\DISPLAY1） */
    int  width;      /* 当前分辨率（DesktopCoordinates） */
    int  height;
    int  is_primary; /* 主显示器 */
} sparkcap_display_info_t;

/* 窗口信息（F16；屏幕域枚举条目）。列表为枚举时刻快照；width/height 为客户区
 * 尺寸；窗口关闭后句柄失效，open_window 对已关闭窗口返回 NOT_FOUND。 */
typedef struct sparkcap_window_info_t {
    uint64_t hwnd;        /* 窗口句柄，sparkcap_screen_open_window 直传 */
    char     title[256];  /* 窗口标题（无标题窗口为空串） */
    int      width;       /* 当前客户区尺寸（跟随 resize） */
    int      height;
} sparkcap_window_info_t;

/* 屏幕域选项（sparkcap_screen_set_options，会话级、start 前设置；默认 0 =
 * 帧不含光标 + 尽力关闭系统提示边框）。best-effort 契约：不支持的后端/系统
 * 组合静默降级到该后端天然行为，不报错（行为矩阵见 Spec §4.7）。 */
typedef enum sparkcap_screen_option_t {
    SPARKCAP_SCREEN_CURSOR = (1 << 0),  /* 帧含鼠标光标（仅 WGC 后端支持） */
    SPARKCAP_SCREEN_BORDER = (1 << 1),  /* 保留系统提示边框（默认尽力关闭） */
} sparkcap_screen_option_t;

/* 帧（技术选型 §5；内存由库分配、库释放，应用不得 free）。
 * stride 符号表示行序：正 = top-down，负 = bottom-up（data 指向最底行）；
 * RGB 系格式通常为 bottom-up。压缩格式（MJPG）stride 恒 0。 */
typedef struct sparkcap_frame_t {
    void*             data;
    int               width;
    int               height;
    sparkcap_pixfmt_t pixel_format;
    int               stride;
    int               size;        /* 帧数据字节长度：压缩格式必需；2D 格式 = |stride| × height */
    int64_t           timestamp;
} sparkcap_frame_t;

/* 帧回调（F5）：在采集线程执行，须快速返回；帧仅回调期间有效（Spec §4.3） */
typedef void (*sparkcap_frame_cb)(sparkcap_device_t* device,
                                  const sparkcap_frame_t* frame,
                                  void* userdata);

/* 错误回调（Spec §4.5）：流断开时通知；不通知恢复（帧回调恢复交付即恢复）。
 * 在采集线程执行，须快速返回。 */
typedef void (*sparkcap_error_cb)(sparkcap_device_t* device,
                                  sparkcap_err_t error_code,
                                  void* userdata);

/* 日志级别 */
typedef enum sparkcap_log_level_t {
    SPARKCAP_LOG_INFO  = 0,
    SPARKCAP_LOG_WARN  = 1,
    SPARKCAP_LOG_ERROR = 2,
} sparkcap_log_level_t;

/* 日志回调（Spec §4.5）：诊断输出；backend 为来源后端标识（可为空串）。
 * 注册后库内部日志（含 FFmpeg av_log）路由到该回调；未注册时库静默。
 * 任意时机可注册/取消（传 NULL 取消）。 */
typedef void (*sparkcap_log_cb)(sparkcap_device_t* device,
                                sparkcap_log_level_t level,
                                const char* backend,
                                const char* message,
                                void* userdata);

/* 功能集位（Spec §4.1，技术选型 §10.4 编译期功能集的运行时查询）。
 * 位值是公开 ABI：已发布不可变更，新能力追加新位。
 * 能力域命名（接入方视角），与 CMake option（构建者视角）故意不同名：
 * ENABLE_FFMPEG option 对应 FEATURE_IP_CAMERA 位。 */
typedef enum sparkcap_feature_t {
    SPARKCAP_FEATURE_CAMERA    = (1 << 0),  /* 本地摄像头（USB/虚拟设备，MF/DShow） */
    SPARKCAP_FEATURE_IP_CAMERA = (1 << 1),  /* IP 摄像头（网络流接入，当前 rtsp://） */
    SPARKCAP_FEATURE_SCREEN    = (1 << 2),  /* 屏幕抓屏（DXGI Desktop Duplication） */
    /* 追加位预留：v2 域（AUDIO）与未来接入技术（如 NDI）落地时加性扩展 */
} sparkcap_feature_t;

#ifdef __cplusplus
}
#endif

#endif /* SPARKCAP_TYPES_H */
