#ifndef SPARKCAP_H
#define SPARKCAP_H

#include "sparkcap_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 调用顺序（Spec §4.5）：
 *   create_backend → list_devices → open → set_frame_callback（可选，start 前）
 *   → start → get_frame / 回调交付 → stop → close → destroy_backend
 */

/* 枚举设备（F1）。两段式：infos 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际数量。 */
SPARKCAP_API int sparkcap_list_devices(sparkcap_backend_t* backend,
                                       sparkcap_device_info_t* infos,
                                       int* in_out_count);

/* 创建后端实例 */
SPARKCAP_API int sparkcap_create_backend(sparkcap_backend_t** out_backend);

/* 查询设备能力（F11）。两段式：formats 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际总数。open 前后均可调用。 */
SPARKCAP_API int sparkcap_get_formats(sparkcap_backend_t* backend,
                                      int device_id,
                                      sparkcap_device_format_t* formats,
                                      int* in_out_count);

/* 打开设备（F2） */
SPARKCAP_API int sparkcap_open(sparkcap_backend_t* backend,
                               int device_id,
                               sparkcap_device_t** out_device);

/* 注册帧回调（F5）；须在 start 前注册，start 后返回 UNSUPPORTED（Spec §4.5） */
SPARKCAP_API int sparkcap_set_frame_callback(sparkcap_device_t* device,
                                             sparkcap_frame_cb cb,
                                             void* userdata);

/* 启动采集（F3）；width/height/fps 传 0、fmt 传 UNKNOWN 表示自动 */
SPARKCAP_API int sparkcap_start(sparkcap_device_t* device,
                                int width,
                                int height,
                                sparkcap_pixfmt_t fmt,
                                int fps);

/* 拉取一帧（F4）；timeout_ms 毫秒超时；帧在下一次 get_frame 前有效（Spec §4.3） */
SPARKCAP_API int sparkcap_get_frame(sparkcap_device_t* device,
                                    sparkcap_frame_t** out_frame,
                                    int timeout_ms);

/* 深拷贝一帧；跨线程/跨调用持有用，用后 release_frame（Spec §4.3）。
   可在帧回调内调用。 */
SPARKCAP_API int sparkcap_copy_frame(const sparkcap_frame_t* src,
                                     sparkcap_frame_t** out_copy);

/* 释放拷贝的帧 */
SPARKCAP_API int sparkcap_release_frame(sparkcap_frame_t* frame);

/* 停止采集（F6）；等待在途回调返回后才完成，此后不再产生新回调（Spec §4.4）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_stop(sparkcap_device_t* device);

/* 关闭设备（F7）；句柄立即失效（Spec §4.5）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_close(sparkcap_device_t* device);

/* 销毁后端实例 */
SPARKCAP_API int sparkcap_destroy_backend(sparkcap_backend_t* backend);

/* 版本字符串，如 "1.0.0" */
SPARKCAP_API const char* sparkcap_version(void);

#ifdef __cplusplus
}
#endif

#endif /* SPARKCAP_H */
