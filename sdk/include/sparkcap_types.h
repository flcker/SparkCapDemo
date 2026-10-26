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
} sparkcap_err_t;

/* 设备能力条目（F11）：一种分辨率/格式/帧率组合；fps 为四舍五入整数值 */
typedef struct sparkcap_device_format_t {
    int               width;
    int               height;
    sparkcap_pixfmt_t pixelFormat;
    int               fps;
} sparkcap_device_format_t;

/* 设备信息（F1；能力经 sparkcap_get_formats 按需查询） */
typedef struct sparkcap_device_info_t {
    int                      id;        /* deviceId，供 sparkcap_open 使用 */
    char                     name[256];
    char                     path[512]; /* 设备符号链接或 RTSP URL */
    sparkcap_backend_kind_t  backend;
} sparkcap_device_info_t;

/* 帧（技术选型 §5；内存由库分配、库释放，应用不得 free）。
 * stride 符号表示行序：正 = top-down，负 = bottom-up（data 指向最底行）；
 * RGB 系格式通常为 bottom-up。压缩格式（MJPG）stride 恒 0。 */
typedef struct sparkcap_frame_t {
    void*             data;
    int               width;
    int               height;
    sparkcap_pixfmt_t pixelFormat;
    int               stride;
    int               size;        /* 帧数据字节长度：压缩格式必需；2D 格式 = |stride| × height */
    int64_t           timestamp;
} sparkcap_frame_t;

/* 帧回调（F5）：在采集线程执行，须快速返回；帧仅回调期间有效（Spec §4.3） */
typedef void (*sparkcap_frame_cb)(sparkcap_device_t* device,
                                  const sparkcap_frame_t* frame,
                                  void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* SPARKCAP_TYPES_H */
