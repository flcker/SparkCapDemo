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
 * sparkcap_query_features 任意时刻可调（无句柄依赖），用于功能探测与 UI 降级。
 */

/* ==================== 通用层：库信息（无句柄依赖，最先可调） ==================== */

/* 版本字符串，如 "1.0.0" */
SPARKCAP_API const char* sparkcap_version(void);

/* 查询本构建的功能集（Spec §4.1）：返回 sparkcap_feature_t 位掩码。
 * 编译期裁剪（如 SPARKCAP_ENABLE_FFMPEG=OFF）在运行时的唯一可探测入口，
 * 应用于能力判断与 UI 降级；任意时刻可调，无副作用。 */
SPARKCAP_API uint32_t sparkcap_query_features(void);

/* ==================== 通用层：上下文生命周期与全局配置 ==================== */

/* 创建后端实例 */
SPARKCAP_API int sparkcap_create_backend(sparkcap_backend_t** out_backend);

/* 销毁后端实例 */
SPARKCAP_API int sparkcap_destroy_backend(sparkcap_backend_t* backend);

/* 注册日志回调（Spec §4.5）：诊断输出路由，进程级全局（FFmpeg av_log 等为进程级源）。
   任意时机可注册/取消（传 NULL 取消）。 */
SPARKCAP_API int sparkcap_set_log_callback(sparkcap_log_cb cb,
                                           void* userdata);

/* ==================== 摄像头域：设备发现 ==================== */

/* 枚举设备（F1）。两段式：infos 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际数量。 */
SPARKCAP_API int sparkcap_camera_list_devices(sparkcap_backend_t* backend,
                                       sparkcap_device_info_t* infos,
                                       int* in_out_count);

/* 查询设备能力（F11）。两段式：formats 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际总数。open 前后均可调用。 */
SPARKCAP_API int sparkcap_camera_get_formats(sparkcap_backend_t* backend,
                                      int device_id,
                                      sparkcap_device_format_t* formats,
                                      int* in_out_count);

/* ==================== 摄像头域：会话操作（按调用顺序） ==================== */

/* 打开设备（F2） */
SPARKCAP_API int sparkcap_camera_open(sparkcap_backend_t* backend,
                               int device_id,
                               sparkcap_device_t** out_device);

/* 注册帧回调（F5）；须在 start 前注册，start 后返回 UNSUPPORTED（Spec §4.5） */
SPARKCAP_API int sparkcap_camera_set_frame_callback(sparkcap_device_t* device,
                                             sparkcap_frame_cb cb,
                                             void* userdata);

/* 注册错误回调（Spec §4.5）：流断开时通知（STREAM_LOST）；
   不通知恢复（帧回调恢复交付即恢复）。须在 start 前注册。 */
SPARKCAP_API int sparkcap_camera_set_error_callback(sparkcap_device_t* device,
                                             sparkcap_error_cb cb,
                                             void* userdata);

/* 启动采集（F3）；width/height/fps 传 0、fmt 传 UNKNOWN 表示自动 */
SPARKCAP_API int sparkcap_camera_start(sparkcap_device_t* device,
                                int width,
                                int height,
                                sparkcap_pixfmt_t fmt,
                                int fps);

/* 拉取一帧（F4）；timeout_ms 毫秒超时；帧在下一次 get_frame 前有效（Spec §4.3） */
SPARKCAP_API int sparkcap_camera_get_frame(sparkcap_device_t* device,
                                    sparkcap_frame_t** out_frame,
                                    int timeout_ms);

/* 停止采集（F6）；等待在途回调返回后才完成，此后不再产生新回调（Spec §4.4）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_camera_stop(sparkcap_device_t* device);

/* 关闭设备（F7）；句柄立即失效（Spec §4.5）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_camera_close(sparkcap_device_t* device);

/* ==================== 屏幕域：桌面内容捕获（显示器/窗口） ==================== */

/* 枚举显示器（F12）。两段式：displays 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际总数。 */
SPARKCAP_API int sparkcap_screen_list_displays(sparkcap_backend_t* backend,
                                        sparkcap_display_info_t* displays,
                                        int* in_out_count);

/* 枚举可见顶层窗口（F16）。两段式：windows 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际总数。列表为枚举时刻快照。 */
SPARKCAP_API int sparkcap_screen_list_windows(sparkcap_backend_t* backend,
                                       sparkcap_window_info_t* windows,
                                       int* in_out_count);

/* 打开显示器捕获会话（F13）；主后端 DXGI Desktop Duplication，失败自动回落
   GDI BitBlt（应用无感）；同屏第二会话返回 DEVICE_BUSY（系统单实例限制）。 */
SPARKCAP_API int sparkcap_screen_open(sparkcap_backend_t* backend,
                               int display_id,
                               sparkcap_device_t** out_device);

/* 打开窗口捕获会话（F17）；主后端 WGC（Win10 1903+），不可用自动回落
   GDI PrintWindow。快照后窗口已关闭返回 NOT_FOUND。 */
SPARKCAP_API int sparkcap_screen_open_window(sparkcap_backend_t* backend,
                                      uint64_t hwnd,
                                      sparkcap_device_t** out_device);

/* 注册帧回调（F5）；须在 start 前注册，start 后返回 UNSUPPORTED（Spec §4.5）。
   回调类型与帧结构复用视频域。 */
SPARKCAP_API int sparkcap_screen_set_frame_callback(sparkcap_device_t* device,
                                             sparkcap_frame_cb cb,
                                             void* userdata);

/* 注册错误回调（Spec §4.5）：ACCESS_LOST（模式切换/独占全屏/TDR，STREAM_LOST）、
   窗口最小化（SOURCE_PAUSED，只通知一次）、窗口关闭（STREAM_LOST）等通知；
   恢复不通知（帧回调恢复交付即恢复）。须在 start 前注册。 */
SPARKCAP_API int sparkcap_screen_set_error_callback(sparkcap_device_t* device,
                                             sparkcap_error_cb cb,
                                             void* userdata);

/* 设置会话选项（sparkcap_screen_option_t 位掩码；Spec §4.7 行为矩阵）。
   best-effort：不支持的后端/系统组合静默降级到该后端天然行为，不报错；
   未知位忽略（位掩码前向兼容）。须在 start 前设置，start 后返回 UNSUPPORTED。 */
SPARKCAP_API int sparkcap_screen_set_options(sparkcap_device_t* device,
                                      uint32_t options);

/* 启动捕获（F14/F18）——分辨率/格式由目标决定（原生分辨率、RGB32 固定）；
   fps：0 = 跟随源全速，>0 会话层统一节流（交付帧率 ≤ fps）。 */
SPARKCAP_API int sparkcap_screen_start(sparkcap_device_t* device,
                                int fps);

/* 拉取一帧（F14）；timeout_ms 毫秒超时；静止目标：事件驱动后端（DXGI/WGC）
   返回 TIMEOUT 为正常语义，轮询后端（GDI）定时交付重复帧——应用不假设帧
   唯一性；帧在下一次 get_frame 前有效（Spec §4.3）。 */
SPARKCAP_API int sparkcap_screen_get_frame(sparkcap_device_t* device,
                                    sparkcap_frame_t** out_frame,
                                    int timeout_ms);

/* 停止捕获（F6）；等待在途回调返回后才完成，此后不再产生新回调（Spec §4.4）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_screen_stop(sparkcap_device_t* device);

/* 关闭会话（F7）；句柄立即失效（Spec §4.5）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_screen_close(sparkcap_device_t* device);

/* ==================== 音频域：麦克风采集 / 扬声器回采（WASAPI） ==================== */

/* 枚举麦克风端点（F22）。两段式：infos 为 NULL 时 *in_out_count 返回总数；
   否则写入至多 *in_out_count 项并更新为实际总数。条目含 mix format 预览。 */
SPARKCAP_API int sparkcap_audio_list_microphones(sparkcap_backend_t* backend,
                                          sparkcap_audio_device_info_t* infos,
                                          int* in_out_count);

/* 枚举扬声器回采端点（F23，eRender 输出）。两段式，字段同上；
   is_default 为默认播放设备。 */
SPARKCAP_API int sparkcap_audio_list_loopbacks(sparkcap_backend_t* backend,
                                        sparkcap_audio_device_info_t* infos,
                                        int* in_out_count);

/* 打开麦克风会话（F24）。endpoint_id 为枚举条目的 id（WASAPI endpoint ID）；
   format 双模式：NULL = mix format 原样交付（零转换）；指定三元组 = 系统侧
   AUTOCONVERTPCM 转换（上报恒为实际交付格式）。 */
SPARKCAP_API int sparkcap_audio_open(sparkcap_backend_t* backend,
                              const char* endpoint_id,
                              const sparkcap_audio_format_t* format,
                              sparkcap_device_t** out_device);

/* 打开扬声器回采会话（F24，loopback）。endpoint_id 为 eRender 端点 id；
   format 语义同 sparkcap_audio_open。 */
SPARKCAP_API int sparkcap_audio_open_loopback(sparkcap_backend_t* backend,
                                       const char* endpoint_id,
                                       const sparkcap_audio_format_t* format,
                                       sparkcap_device_t** out_device);

/* 注册音频帧回调（F25）；须在 start 前注册，start 后返回 UNSUPPORTED（Spec §4.5）。
   回调类型 sparkcap_audio_frame_cb / 帧结构 sparkcap_audio_frame_t（独立于视频域）。 */
SPARKCAP_API int sparkcap_audio_set_frame_callback(sparkcap_device_t* device,
                                            sparkcap_audio_frame_cb cb,
                                            void* userdata);

/* 注册错误回调（Spec §4.5）：端点拔出/失效时通知（STREAM_LOST）；
   不通知恢复（帧回调恢复交付即恢复）。须在 start 前注册。 */
SPARKCAP_API int sparkcap_audio_set_error_callback(sparkcap_device_t* device,
                                            sparkcap_error_cb cb,
                                            void* userdata);

/* 启动采集（F25）。fps 仅 0 合法（采样率即节流；参数为三域 start 签名同构
   保留），非 0 返回 INVALID_ARG。 */
SPARKCAP_API int sparkcap_audio_start(sparkcap_device_t* device,
                               int fps);

/* 拉取一帧（F25）；timeout_ms 毫秒超时。loopback 静默期交付合成静音帧
   （不超时，timestamp 单调连续）；帧在下一次 get_frame 前有效（Spec §4.3）。 */
SPARKCAP_API int sparkcap_audio_get_frame(sparkcap_device_t* device,
                                   sparkcap_audio_frame_t** out_frame,
                                   int timeout_ms);

/* 停止采集（F6）；等待在途回调返回后才完成，此后不再产生新回调（Spec §4.4）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_audio_stop(sparkcap_device_t* device);

/* 关闭会话（F7）；句柄立即失效（Spec §4.5）。
   禁止在回调内调用（返回 IN_CALLBACK）。 */
SPARKCAP_API int sparkcap_audio_close(sparkcap_device_t* device);

/* 深拷贝音频帧（F26）；跨线程/跨调用持有用，用后 sparkcap_audio_release_frame
   （Spec §4.3）。可在帧回调内调用。 */
SPARKCAP_API int sparkcap_audio_copy_frame(const sparkcap_audio_frame_t* src,
                                    sparkcap_audio_frame_t** out_copy);

/* 释放拷贝的音频帧 */
SPARKCAP_API int sparkcap_audio_release_frame(sparkcap_audio_frame_t* frame);

/* ==================== 帧工具（库级帧类型，v2 屏幕域复用） ==================== */

/* 深拷贝一帧；跨线程/跨调用持有用，用后 release_frame（Spec §4.3）。
   可在帧回调内调用。 */
SPARKCAP_API int sparkcap_copy_frame(const sparkcap_frame_t* src,
                                     sparkcap_frame_t** out_copy);

/* 释放拷贝的帧 */
SPARKCAP_API int sparkcap_release_frame(sparkcap_frame_t* frame);

#ifdef __cplusplus
}
#endif

#endif /* SPARKCAP_H */
