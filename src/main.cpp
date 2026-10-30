// SparkCapDemo —— SparkCap 多会话并发测试台（Win32 GDI，零依赖）。
// 上部工具栏（每类一行）：设备下拉 + 格式下拉（摄像头/麦克风/回采有；
//   显示器/窗口原生 BGRA 无）+ [开]（重选设备再点开可累积多路，上限 8）
//   + [刷新]（只重枚举该行下拉，不打扰运行中会话；运行中设备标 " (running)"，
//   原选中设备仍在则保持选中）。
// 下部预览网格：1 全幅 / 2 左右 / 3-4 2x2 / 5-6 3x2 / 7-8 4x2；
//   视频 letterbox 缩放适配格子，音频波形包络；格内标注设备名 + 实际格式 +
//   帧计数；格右上 [×] 热区停止单路。
// 按键：ESC 退出。
// 回调集成：错误回调（STREAM_LOST 自动移除会话 + 日志；窗口最小化
//   SOURCE_PAUSED 保留观察）+ 日志回调（FFmpeg av_log 路由到预览区底部）。

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM（[×] 热区命中检测）

// GDI+（系统组件）解 MJPG 帧
#include <gdiplus.h>
#include <shlwapi.h>

// spdlog（文件日志）——必须在 windows.h/gdiplus 之后：
// 提前引入会破坏 GDI+ 头的 GUID 宏展开（GdiplusImaging.h C2440）
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

// ---- 会话种类 / 工具栏 / 网格 ----

enum {
    KIND_CAM = 0,    // 摄像头（MF/DShow）
    KIND_DISPLAY,    // 显示器（DXGI/GDI）
    KIND_WINDOW,     // 窗口（WGC/GDI）
    KIND_MIC,        // 麦克风（WASAPI）
    KIND_LOOP,       // 扬声器回采（WASAPI loopback）
    KIND_COUNT
};

#define ROW_H        28               // 工具栏行高
#define TOOLBAR_H    (KIND_COUNT * ROW_H + 12)

#define MAX_SESSIONS 8                // 预览网格上限（分格 1/2/4/6/8 路）

// 每类一行的控件 ID（设备下拉/格式下拉/开/刷新）
#define IDC_ROW_BASE 2000
#define ROW_ID(kind, which) (IDC_ROW_BASE + (kind) * 4 + (which))
enum { ROW_DEV = 0, ROW_FMT, ROW_OPEN, ROW_REFRESH };

// ---- 会话（一路独立采集：独立拉帧 + 独立上一画面缓存）----

typedef struct {
    int kind;                    // KIND_*
    sparkcap_device_t* dev;
    char name[128];              // 设备名（格内标注/日志）

    // 设备标识（下拉 " (running)" 标记与刷新后选中恢复的匹配键）
    int keyId;                   // 摄像头 deviceId / 显示器 displayId
    unsigned long long keyHwnd;  // 窗口句柄
    char keyEndpoint[256];       // 音频 endpoint ID

    unsigned long long frames;   // 帧计数（格内标注）
    int firstFrameLogged;        // 会话内首帧只记一次
    int lastError;               // 最近一次 get_frame 返回值（状态跃变才落盘）

    // 视频路：最近一帧的 RGB32 拷贝。帧仅在下次 get_frame 前有效（Spec §4.3），
    // 转换结果即"格内上一画面"缓存——超时不重写即保留上一画面。
    unsigned int* rgb;
    int rgbW, rgbH;
    int lastW, lastH;            // 标注用（实际交付尺寸/格式）
    sparkcap_pixfmt_t lastFmt;

    // 音频路：波形缓存（最近一帧第 0 声道样本 -1..1，超时保留）
    float* wave;
    int waveCount, waveCap;
    char waveText[64];           // 实际帧参数（"48kHz 2ch F32"）
} Session;

static Session g_sess[MAX_SESSIONS];
static int g_sessCount;

// ---- 工具栏行（每类一行的控件与该类枚举缓存）----

typedef struct {
    int kind;
    HWND labelCtl, dev, fmt, open, refresh;  // 显示器/窗口行 fmt 为 NULL

    // 该类枚举缓存（[刷新] 只重枚举本类，不打扰运行中会话）
    sparkcap_device_info_t cams[32];
    int camCount;
    sparkcap_display_info_t disp[8];
    int dispCount;
    sparkcap_window_info_t wins[64];
    int winCount;
    sparkcap_audio_device_info_t auds[8];    // 麦克风行/回采行各自一套
    int audCount;

    // 摄像头能力表（设备下拉变化时重查）
    sparkcap_device_format_t formats[64];
    int formatCount;

    // 当前选中设备标识（刷新后仍在列表则保持选中）
    int selId;
    unsigned long long selHwnd;
    char selEndpoint[256];
} Row;

static Row g_row[KIND_COUNT];
static sparkcap_backend_t* g_backend;
static HWND g_hwnd;                         // 回调线程经此 PostMessage

// ---- 日志与统计 ----

#define WM_APP_ERROR (WM_APP + 1)  // wp = 错误码, lp = device 句柄（仅比较不解引用）
#define WM_APP_LOG   (WM_APP + 2)

#define LOG_LINES 4
#define LOG_TEXT  160
static char g_logs[LOG_LINES][LOG_TEXT];  // 环形：最近 LOG_LINES 条
static int g_logHead, g_logCount;
static CRITICAL_SECTION g_logCs;

static unsigned int g_framesThisSec;  // 标题栏总拉帧率（全部会话合计）
static ULONGLONG g_secStart;

// ---- 回调（采集线程执行：只做快速记录 + PostMessage，不碰 UI）----

static void onErrorCb(sparkcap_device_t* dev, sparkcap_err_t code, void* userdata)
{
    (void)userdata;
    // SOURCE_PAUSED（窗口最小化）是可恢复暂停而非流断，如实区分提示
    const char* what = (code == SPARKCAP_ERR_SOURCE_PAUSED) ? "source paused (window minimized)"
                                                            : "stream lost";
    spdlog::error("error callback: code={} ({})", (int)code, what);
    // UI 线程按句柄反查会话：STREAM_LOST 自动移除；句柄仅比较不解引用
    PostMessageW(g_hwnd, WM_APP_ERROR, (WPARAM)code, (LPARAM)dev);
}

static void onLogCb(sparkcap_device_t* dev, sparkcap_log_level_t level,
                    const char* backend, const char* message, void* userdata)
{
    (void)dev; (void)userdata;
    const char tag = (level == SPARKCAP_LOG_ERROR) ? 'E'
                   : (level == SPARKCAP_LOG_WARN)  ? 'W' : 'I';
    // 落盘（spdlog 线程安全）+ 预览区显示
    spdlog::log(level == SPARKCAP_LOG_ERROR ? spdlog::level::err
              : level == SPARKCAP_LOG_WARN  ? spdlog::level::warn
              : spdlog::level::info,
              "[{}] {}", backend, message);
    EnterCriticalSection(&g_logCs);
    sprintf_s(g_logs[g_logHead], LOG_TEXT, "%c[%s] %s", tag, backend, message);
    // FFmpeg 日志自带尾换行，去掉
    char* nl = strchr(g_logs[g_logHead], '\n');
    if (nl != NULL)
        *nl = '\0';
    g_logHead = (g_logHead + 1) % LOG_LINES;
    if (g_logCount < LOG_LINES)
        ++g_logCount;
    LeaveCriticalSection(&g_logCs);
    PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);
}

// ---- 像素格式转换（输出侧全格式 → RGB32，写入会话缓存）----

static void yuvToRgb(int y, int u, int v, unsigned int* out)
{
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    *out = (unsigned int)(r << 16 | g << 8 | b);
}

static int frameToRgb32(Session* s, const sparkcap_frame_t* f)
{
    const int w = f->width;
    const int h = f->height;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return -1;
    if (s->rgbW != w || s->rgbH != h) {
        free(s->rgb);
        s->rgb = (unsigned int*)malloc(sizeof(unsigned int) * (size_t)w * h);
        if (s->rgb == NULL) {
            s->rgbW = 0;
            s->rgbH = 0;
            return -1;
        }
        s->rgbW = w;
        s->rgbH = h;
    }

    const unsigned char* data = (const unsigned char*)f->data;
    switch (f->pixel_format) {
    case SPARKCAP_PIXFMT_YUY2: {
        const int stride = f->stride > 0 ? f->stride : w * 2;
        for (int row = 0; row < h; ++row) {
            const unsigned char* src = data + (size_t)stride * row;
            unsigned int* dst = s->rgb + (size_t)w * row;
            for (int col = 0; col + 1 < w; col += 2) {
                const int y0 = src[col * 2];
                const int u = src[col * 2 + 1];
                const int y1 = src[col * 2 + 2];
                const int v = src[col * 2 + 3];
                yuvToRgb(y0, u, v, &dst[col]);
                yuvToRgb(y1, u, v, &dst[col + 1]);
            }
        }
        break;
    }
    case SPARKCAP_PIXFMT_I420: {
        const int ys = f->stride > 0 ? f->stride : w;
        const int cs = ys / 2;
        const unsigned char* yP = data;
        const unsigned char* uP = data + (size_t)ys * h;
        const unsigned char* vP = uP + (size_t)cs * (h / 2);
        for (int row = 0; row < h; ++row) {
            const unsigned char* yRow = yP + (size_t)ys * row;
            const unsigned char* uRow = uP + (size_t)cs * (row / 2);
            const unsigned char* vRow = vP + (size_t)cs * (row / 2);
            unsigned int* dst = s->rgb + (size_t)w * row;
            for (int col = 0; col < w; ++col)
                yuvToRgb(yRow[col], uRow[col / 2], vRow[col / 2], &dst[col]);
        }
        break;
    }
    case SPARKCAP_PIXFMT_NV12: {
        const int ys = f->stride > 0 ? f->stride : w;
        const unsigned char* yP = data;
        const unsigned char* uvP = data + (size_t)ys * h;
        for (int row = 0; row < h; ++row) {
            const unsigned char* yRow = yP + (size_t)ys * row;
            const unsigned char* uvRow = uvP + (size_t)ys * (row / 2);
            unsigned int* dst = s->rgb + (size_t)w * row;
            for (int col = 0; col < w; ++col) {
                const int uv = (col & ~1);
                yuvToRgb(yRow[col], uvRow[uv], uvRow[uv + 1], &dst[col]);
            }
        }
        break;
    }
    case SPARKCAP_PIXFMT_RGB24: {
        // 负 stride = bottom-up：data 指向最底行，按 |stride| 读、显示行翻转
        const int stride = f->stride != 0 ? (f->stride > 0 ? f->stride : -f->stride) : w * 3;
        const int flipRow = f->stride < 0;
        for (int row = 0; row < h; ++row) {
            const unsigned char* src = data + (size_t)stride * row;
            unsigned int* dst = s->rgb + (size_t)w * (flipRow ? (h - 1 - row) : row);
            for (int col = 0; col < w; ++col) {
                const unsigned char* p = src + (size_t)3 * col;
                dst[col] = (unsigned int)(p[2] << 16 | p[1] << 8 | p[0]);
            }
        }
        break;
    }
    case SPARKCAP_PIXFMT_RGB32: {
        const int stride = f->stride != 0 ? (f->stride > 0 ? f->stride : -f->stride) : w * 4;
        const int flipRow = f->stride < 0;
        for (int row = 0; row < h; ++row) {
            const unsigned char* src = data + (size_t)stride * row;
            unsigned int* dst = s->rgb + (size_t)w * (flipRow ? (h - 1 - row) : row);
            memcpy(dst, src, sizeof(unsigned int) * (size_t)w);
        }
        break;
    }
    case SPARKCAP_PIXFMT_MJPG: {
        // MJPG = 逐帧 JPEG：系统 GDI+ 解码（零第三方依赖）
        if (f->size <= 0)
            return -1;
        IStream* stream = SHCreateMemStream(data, (UINT)f->size);
        if (stream == NULL)
            return -1;
        Gdiplus::Bitmap bmp(stream, FALSE);
        if (bmp.GetLastStatus() != Gdiplus::Ok) {
            stream->Release();
            return -1;
        }
        Gdiplus::BitmapData bd = {0};
        Gdiplus::Rect rect(0, 0, bmp.GetWidth(), bmp.GetHeight());
        if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB,
                         &bd) == Gdiplus::Ok) {
            if ((int)bd.Width == w && (int)bd.Height == h) {
                for (int row = 0; row < h; ++row) {
                    const unsigned int* src = (const unsigned int*)
                        ((const unsigned char*)bd.Scan0 + (size_t)bd.Stride * row);
                    unsigned int* dst = s->rgb + (size_t)w * row;
                    memcpy(dst, src, sizeof(unsigned int) * (size_t)w);
                }
            }
            bmp.UnlockBits(&bd);
        }
        stream->Release();
        break;
    }
    default:
        return -1;
    }
    return 0;
}

// ---- 名称与文本小工具 ----

static const char* pixfmtName(sparkcap_pixfmt_t f)
{
    switch (f) {
    case SPARKCAP_PIXFMT_NV12:  return "NV12";
    case SPARKCAP_PIXFMT_I420:  return "I420";
    case SPARKCAP_PIXFMT_YUY2:  return "YUY2";
    case SPARKCAP_PIXFMT_RGB24: return "RGB24";
    case SPARKCAP_PIXFMT_RGB32: return "RGB32";
    case SPARKCAP_PIXFMT_MJPG:  return "MJPG";
    default: return "?";
    }
}

// 未压缩格式的估算带宽（MB/s）；压缩/未知格式返回 0（无法估算）。
// bpp 用 ×2 定点（NV12/I420=3、YUY2=4、RGB24=6、RGB32=8）避免浮点。
// USB 2.0 等时传输有效带宽 ~35 MB/s：超出的未压缩格式会花屏/停流。
static long long formatBandwidthMBs(int w, int h, sparkcap_pixfmt_t f, int fps)
{
    long long bpp2;
    switch (f) {
    case SPARKCAP_PIXFMT_NV12:
    case SPARKCAP_PIXFMT_I420:  bpp2 = 3; break;
    case SPARKCAP_PIXFMT_YUY2:  bpp2 = 4; break;
    case SPARKCAP_PIXFMT_RGB24: bpp2 = 6; break;
    case SPARKCAP_PIXFMT_RGB32: bpp2 = 8; break;
    default: return 0;
    }
    return (long long)w * h * bpp2 * fps / 2 / 1000000;
}

static const char* sampfmtName(sparkcap_sample_format_t f)
{
    switch (f) {
    case SPARKCAP_SAMPLE_F32: return "F32";
    case SPARKCAP_SAMPLE_S16: return "S16";
    default: return "?";
    }
}

static const char* errName(int e)
{
    switch (e) {
    case SPARKCAP_ERR_OK:             return "OK";
    case SPARKCAP_ERR_INVALID_ARG:    return "INVALID_ARG";
    case SPARKCAP_ERR_NOT_FOUND:      return "NOT_FOUND";
    case SPARKCAP_ERR_ALREADY_OPEN:   return "ALREADY_OPEN";
    case SPARKCAP_ERR_DEVICE_BUSY:    return "DEVICE_BUSY";
    case SPARKCAP_ERR_START_FAILED:   return "START_FAILED";
    case SPARKCAP_ERR_TIMEOUT:        return "TIMEOUT";
    case SPARKCAP_ERR_STREAM_LOST:    return "STREAM_LOST";
    case SPARKCAP_ERR_UNSUPPORTED:    return "UNSUPPORTED";
    case SPARKCAP_ERR_INVALID_HANDLE: return "INVALID_HANDLE";
    case SPARKCAP_ERR_NO_MEMORY:      return "NO_MEMORY";
    case SPARKCAP_ERR_IN_CALLBACK:    return "IN_CALLBACK";
    case SPARKCAP_ERR_INTERNAL:       return "INTERNAL";
    default: return "?";
    }
}

static const char* kindName(int kind)
{
    switch (kind) {
    case KIND_CAM:     return "camera";
    case KIND_DISPLAY: return "display";
    case KIND_WINDOW:  return "window";
    case KIND_MIC:     return "mic";
    case KIND_LOOP:    return "loopback";
    default: return "?";
    }
}

// UTF-8 char* → wchar（SDK 侧设备名是 UTF-8）
static void toW(const char* src, wchar_t* dst, int cap)
{
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
}

// UTF-8 文本绘制：TextOutA 走 ANSI 代码页，UTF-8 设备名会乱码，故转宽字符
static void drawUtf8(HDC dc, int x, int y, const char* s)
{
    wchar_t buf[512];
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 512);
    if (n > 1)
        TextOutW(dc, x, y, buf, n - 1);
}

// ---- 会话管理 ----

// 该设备是否已有运行中会话（下拉 " (running)" 标记；同设备重复点 [开]
// 不做前置拦截——库会如实返回 DEVICE_BUSY，MessageBox 展示错误码）
static int sessionRunning(int kind, int id, unsigned long long hwnd, const char* endpoint)
{
    for (int i = 0; i < g_sessCount; ++i) {
        const Session* s = &g_sess[i];
        if (s->kind != kind)
            continue;
        if (kind == KIND_CAM || kind == KIND_DISPLAY) {
            if (s->keyId == id)
                return 1;
        } else if (kind == KIND_WINDOW) {
            if (s->keyHwnd == hwnd)
                return 1;
        } else {
            if (strcmp(s->keyEndpoint, endpoint) == 0)
                return 1;
        }
    }
    return 0;
}

static void fillDevCombo(Row* r);  // 前置：stopSession 后刷新该行 " (running)" 标记

static void stopSession(int index)
{
    Session* s = &g_sess[index];
    const int kind = s->kind;
    spdlog::info("close {}: {} (frames={})", kindName(kind), s->name, s->frames);
    switch (kind) {
    case KIND_MIC:
    case KIND_LOOP:
        sparkcap_audio_stop(s->dev);
        sparkcap_audio_close(s->dev);
        break;
    case KIND_DISPLAY:
    case KIND_WINDOW:
        sparkcap_screen_stop(s->dev);
        sparkcap_screen_close(s->dev);
        break;
    default:
        sparkcap_camera_stop(s->dev);
        sparkcap_camera_close(s->dev);
        break;
    }
    free(s->rgb);
    free(s->wave);
    for (int i = index; i < g_sessCount - 1; ++i)
        g_sess[i] = g_sess[i + 1];
    --g_sessCount;
    if (IsWindow(g_hwnd)) {
        InvalidateRect(g_hwnd, NULL, FALSE);
        fillDevCombo(&g_row[kind]);  // 标记即时摘除
    }
}

static void stopAllSessions(void)
{
    while (g_sessCount > 0)
        stopSession(0);
}

// 音频格式下拉的固定组合：3 采样率 × 2 采样格式 × 2 声道（条目 1..12，
// 索引映射 i = (rate)*4 + (fmt)*2 + (ch)）
static const int kAudioRates[3] = {48000, 44100, 16000};
static const sparkcap_sample_format_t kAudioFmts[2] = {SPARKCAP_SAMPLE_F32,
                                                       SPARKCAP_SAMPLE_S16};
static const int kAudioChs[2] = {2, 1};

// 按当前下拉选择开一路会话（多路靠"重选设备再点 [开]"累积）
static void openSession(int kind, int devSel, int fmtSel)
{
    if (g_sessCount >= MAX_SESSIONS) {
        MessageBoxA(g_hwnd, "session limit reached (8)", "SparkCapDemo", MB_ICONINFORMATION);
        return;
    }
    Row* r = &g_row[kind];
    sparkcap_device_t* dev = NULL;
    int rc = SPARKCAP_ERR_INVALID_ARG;
    char name[128];
    int keyId = 0;
    unsigned long long keyHwnd = 0;
    char keyEndpoint[256] = {0};

    switch (kind) {
    case KIND_CAM: {
        if (devSel < 0 || devSel >= r->camCount)
            return;
        const sparkcap_device_info_t* di = &r->cams[devSel];
        sprintf_s(name, sizeof(name), "%s", di->name);
        keyId = di->id;
        rc = sparkcap_camera_open(g_backend, di->id, &dev);
        if (rc == SPARKCAP_ERR_OK) {
            // 错误回调须在 start 前注册（running 后注册返回 UNSUPPORTED）
            sparkcap_camera_set_error_callback(dev, onErrorCb, NULL);
            // [Auto] = 全零参（库自动选型）；手选 = 完整传格式偏好与帧率
            int w = 0, h = 0, fps = 0;
            sparkcap_pixfmt_t fmtPref = SPARKCAP_PIXFMT_UNKNOWN;
            if (fmtSel > 0 && fmtSel <= r->formatCount) {
                const sparkcap_device_format_t* fm = &r->formats[fmtSel - 1];
                w = fm->width;
                h = fm->height;
                fmtPref = fm->pixel_format;
                fps = fm->fps;
            }
            spdlog::info("[{}] camera start: w={} h={} fmt={} fps={}",
                         name, w, h, (int)fmtPref, fps);
            rc = sparkcap_camera_start(dev, w, h, fmtPref, fps);
        }
        break;
    }
    case KIND_DISPLAY: {
        if (devSel < 0 || devSel >= r->dispCount)
            return;
        const sparkcap_display_info_t* di = &r->disp[devSel];
        sprintf_s(name, sizeof(name), "%s", di->name);
        keyId = di->id;
        rc = sparkcap_screen_open(g_backend, di->id, &dev);
        if (rc == SPARKCAP_ERR_OK) {
            sparkcap_screen_set_error_callback(dev, onErrorCb, NULL);
            rc = sparkcap_screen_start(dev, 0);  // 分辨率/格式由显示器决定
        }
        break;
    }
    case KIND_WINDOW: {
        if (devSel < 0 || devSel >= r->winCount)
            return;
        const sparkcap_window_info_t* wi = &r->wins[devSel];
        sprintf_s(name, sizeof(name), "%s", wi->title[0] ? wi->title : "(untitled)");
        keyHwnd = wi->hwnd;
        rc = sparkcap_screen_open_window(g_backend, wi->hwnd, &dev);
        if (rc == SPARKCAP_ERR_OK) {
            sparkcap_screen_set_error_callback(dev, onErrorCb, NULL);
            rc = sparkcap_screen_start(dev, 0);
        }
        break;
    }
    case KIND_MIC:
    case KIND_LOOP: {
        if (devSel < 0 || devSel >= r->audCount)
            return;
        const sparkcap_audio_device_info_t* ai = &r->auds[devSel];
        sprintf_s(name, sizeof(name), "%s", ai->name);
        sprintf_s(keyEndpoint, sizeof(keyEndpoint), "%s", ai->id);
        // 格式下拉 0 = mix format 原样交付（零转换）；>0 = 指定三元组
        //（系统侧 AUTOCONVERTPCM 转换，上报恒为实际交付格式）
        sparkcap_audio_format_t af;
        const sparkcap_audio_format_t* afp = NULL;
        if (fmtSel > 0 && fmtSel <= 12) {
            const int i = fmtSel - 1;
            af.sample_rate = kAudioRates[i / 4];
            af.sample_format = kAudioFmts[(i / 2) % 2];
            af.channels = kAudioChs[i % 2];
            afp = &af;
        }
        rc = (kind == KIND_LOOP)
            ? sparkcap_audio_open_loopback(g_backend, ai->id, afp, &dev)
            : sparkcap_audio_open(g_backend, ai->id, afp, &dev);
        if (rc == SPARKCAP_ERR_OK) {
            sparkcap_audio_set_error_callback(dev, onErrorCb, NULL);
            rc = sparkcap_audio_start(dev, 0);  // fps 仅 0 合法
        }
        break;
    }
    default:
        return;
    }

    if (rc != SPARKCAP_ERR_OK) {
        char msg[320];
        sprintf_s(msg, sizeof(msg), "%s open/start failed: %s (%d)\n%s",
                  kindName(kind), errName(rc), rc, name);
        MessageBoxA(g_hwnd, msg, "SparkCapDemo", MB_ICONWARNING);
        if (dev != NULL) {
            // start 失败：关掉半开会话
            switch (kind) {
            case KIND_MIC:
            case KIND_LOOP:
                sparkcap_audio_close(dev);
                break;
            case KIND_DISPLAY:
            case KIND_WINDOW:
                sparkcap_screen_close(dev);
                break;
            default:
                sparkcap_camera_close(dev);
                break;
            }
        }
        return;
    }

    Session* s = &g_sess[g_sessCount++];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->dev = dev;
    sprintf_s(s->name, sizeof(s->name), "%s", name);
    s->keyId = keyId;
    s->keyHwnd = keyHwnd;
    sprintf_s(s->keyEndpoint, sizeof(s->keyEndpoint), "%s", keyEndpoint);
    spdlog::info("open {}: {} (session {}/{})", kindName(kind), s->name,
                 g_sessCount, MAX_SESSIONS);
    InvalidateRect(g_hwnd, NULL, FALSE);
    fillDevCombo(&g_row[kind]);  // 即时标 " (running)"
}

// ---- 枚举与下拉填充 ----

// 重新枚举该行设备（[刷新]：不打扰运行中会话，已开会话继续跑）
static void enumRow(Row* r)
{
    switch (r->kind) {
    case KIND_CAM: {
        int n = (int)(sizeof(r->cams) / sizeof(r->cams[0]));
        if (sparkcap_camera_list_devices(g_backend, r->cams, &n) != SPARKCAP_ERR_OK)
            n = 0;
        r->camCount = n;
        spdlog::info("camera list: {} entries", n);
        for (int i = 0; i < n; ++i)
            spdlog::info("  [{}] {} (backend={})", i, r->cams[i].name,
                         (int)r->cams[i].backend);
        break;
    }
    case KIND_DISPLAY: {
        int n = (int)(sizeof(r->disp) / sizeof(r->disp[0]));
        if (sparkcap_screen_list_displays(g_backend, r->disp, &n) != SPARKCAP_ERR_OK)
            n = 0;
        r->dispCount = n;
        for (int i = 0; i < n; ++i)
            spdlog::info("  [display {}] {} {}x{}{}", i, r->disp[i].name,
                         r->disp[i].width, r->disp[i].height,
                         r->disp[i].is_primary ? " (primary)" : "");
        break;
    }
    case KIND_WINDOW: {
        int n = (int)(sizeof(r->wins) / sizeof(r->wins[0]));
        if (sparkcap_screen_list_windows(g_backend, r->wins, &n) != SPARKCAP_ERR_OK)
            n = 0;
        r->winCount = n;
        spdlog::info("  window list: {} entries", n);
        break;
    }
    case KIND_MIC: {
        int n = (int)(sizeof(r->auds) / sizeof(r->auds[0]));
        if (sparkcap_audio_list_microphones(g_backend, r->auds, &n) != SPARKCAP_ERR_OK)
            n = 0;
        r->audCount = n;
        for (int i = 0; i < n; ++i)
            spdlog::info("  [mic {}] {} ({}Hz {}ch {}){}", i, r->auds[i].name,
                         r->auds[i].sample_rate, r->auds[i].channels,
                         sampfmtName(r->auds[i].format),
                         r->auds[i].is_default ? " default" : "");
        break;
    }
    default: {
        int n = (int)(sizeof(r->auds) / sizeof(r->auds[0]));
        if (sparkcap_audio_list_loopbacks(g_backend, r->auds, &n) != SPARKCAP_ERR_OK)
            n = 0;
        r->audCount = n;
        for (int i = 0; i < n; ++i)
            spdlog::info("  [loopback {}] {} ({}Hz {}ch {}){}", i, r->auds[i].name,
                         r->auds[i].sample_rate, r->auds[i].channels,
                         sampfmtName(r->auds[i].format),
                         r->auds[i].is_default ? " default" : "");
        break;
    }
    }
}

// 记录当前选中设备标识（刷新后仍在列表则保持选中）
static void updateRowSel(Row* r)
{
    const int sel = (int)SendMessageW(r->dev, CB_GETCURSEL, 0, 0);
    r->selId = 0;
    r->selHwnd = 0;
    r->selEndpoint[0] = '\0';
    switch (r->kind) {
    case KIND_CAM:
        if (sel >= 0 && sel < r->camCount)
            r->selId = r->cams[sel].id;
        break;
    case KIND_DISPLAY:
        if (sel >= 0 && sel < r->dispCount)
            r->selId = r->disp[sel].id;
        break;
    case KIND_WINDOW:
        if (sel >= 0 && sel < r->winCount)
            r->selHwnd = r->wins[sel].hwnd;
        break;
    default:
        if (sel >= 0 && sel < r->audCount)
            sprintf_s(r->selEndpoint, sizeof(r->selEndpoint), "%s", r->auds[sel].id);
        break;
    }
}

// 填设备下拉：默认设备 ★ 前缀（audio is_default / screen is_primary），
// 运行中设备 " (running)" 后缀；恢复原选中（无匹配回退首条）
static void fillDevCombo(Row* r)
{
    SendMessageW(r->dev, CB_RESETCONTENT, 0, 0);
    char line[320];
    wchar_t text[352];
    int select = -1;
    switch (r->kind) {
    case KIND_CAM:
        // 摄像头无默认设备概念，不加 ★；后端标签保留（MF/DShow/RTSP）
        for (int i = 0; i < r->camCount; ++i) {
            const char* backend = r->cams[i].backend == SPARKCAP_BACKEND_MF ? " [MF]"
                : r->cams[i].backend == SPARKCAP_BACKEND_DSHOW ? " [DShow]" : " [RTSP]";
            sprintf_s(line, sizeof(line), "%s%s%s", r->cams[i].name, backend,
                      sessionRunning(KIND_CAM, r->cams[i].id, 0, "") ? " (running)" : "");
            toW(line, text, 352);
            SendMessageW(r->dev, CB_ADDSTRING, 0, (LPARAM)text);
            if (r->selId == r->cams[i].id)
                select = i;
        }
        break;
    case KIND_DISPLAY:
        for (int i = 0; i < r->dispCount; ++i) {
            const sparkcap_display_info_t* di = &r->disp[i];
            sprintf_s(line, sizeof(line), "%s%s %dx%d%s", di->is_primary ? "★ " : "",
                      di->name, di->width, di->height,
                      sessionRunning(KIND_DISPLAY, di->id, 0, "") ? " (running)" : "");
            toW(line, text, 352);
            SendMessageW(r->dev, CB_ADDSTRING, 0, (LPARAM)text);
            if (r->selId == di->id)
                select = i;
        }
        break;
    case KIND_WINDOW:
        for (int i = 0; i < r->winCount; ++i) {
            const sparkcap_window_info_t* wi = &r->wins[i];
            const char* title = wi->title[0] ? wi->title : "(untitled)";
            sprintf_s(line, sizeof(line), "%s (%dx%d)%s", title, wi->width, wi->height,
                      sessionRunning(KIND_WINDOW, 0, wi->hwnd, "") ? " (running)" : "");
            toW(line, text, 352);
            SendMessageW(r->dev, CB_ADDSTRING, 0, (LPARAM)text);
            if (r->selHwnd == wi->hwnd)
                select = i;
        }
        break;
    default:
        for (int i = 0; i < r->audCount; ++i) {
            const sparkcap_audio_device_info_t* ai = &r->auds[i];
            sprintf_s(line, sizeof(line), "%s%s%s", ai->is_default ? "★ " : "", ai->name,
                      sessionRunning(r->kind, 0, 0, ai->id) ? " (running)" : "");
            toW(line, text, 352);
            SendMessageW(r->dev, CB_ADDSTRING, 0, (LPARAM)text);
            if (strcmp(r->selEndpoint, ai->id) == 0)
                select = i;
        }
        break;
    }
    if (select < 0)
        select = 0;  // 原选中设备已不在列表：回退首条（空表则为无选中）
    SendMessageW(r->dev, CB_SETCURSEL, select, 0);
    updateRowSel(r);
}

// 摄像头格式下拉：[Auto] + get_formats 完整组合（"1280x720 YUY2 30fps" 式）
static void fillCamFormats(Row* r)
{
    SendMessageW(r->fmt, CB_RESETCONTENT, 0, 0);
    wchar_t text[96];
    toW("[Auto]", text, 96);
    SendMessageW(r->fmt, CB_ADDSTRING, 0, (LPARAM)text);

    r->formatCount = 0;
    const int sel = (int)SendMessageW(r->dev, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < r->camCount) {
        const int cap = (int)(sizeof(r->formats) / sizeof(r->formats[0]));
        int n = cap;
        if (sparkcap_camera_get_formats(g_backend, r->cams[sel].id, r->formats, &n)
            == SPARKCAP_ERR_OK)
            r->formatCount = n > cap ? cap : n;  // get_formats 更新为总数，可能 > 容量
    }
    for (int i = 0; i < r->formatCount; ++i) {
        const sparkcap_device_format_t* fm = &r->formats[i];
        char line[96];
        const long long mbps = formatBandwidthMBs(fm->width, fm->height, fm->pixel_format,
                                                  fm->fps);
        if (mbps > 35)  // 超 USB 2.0 有效带宽的未压缩档：警示后缀
            sprintf_s(line, sizeof(line), "%dx%d %s %dfps (%lldMB/s!)",
                      fm->width, fm->height, pixfmtName(fm->pixel_format), fm->fps, mbps);
        else
            sprintf_s(line, sizeof(line), "%dx%d %s %dfps",
                      fm->width, fm->height, pixfmtName(fm->pixel_format), fm->fps);
        wchar_t wline[96];
        toW(line, wline, 96);
        SendMessageW(r->fmt, CB_ADDSTRING, 0, (LPARAM)wline);
    }
    SendMessageW(r->fmt, CB_SETCURSEL, 0, 0);
}

// 音频格式下拉（固定表）：[Follow device (mix format)] + 12 组合
static void fillAudioFormats(Row* r)
{
    SendMessageW(r->fmt, CB_RESETCONTENT, 0, 0);
    wchar_t text[96];
    toW("Follow device (mix format)", text, 96);
    SendMessageW(r->fmt, CB_ADDSTRING, 0, (LPARAM)text);
    for (int ri = 0; ri < 3; ++ri)
        for (int fi = 0; fi < 2; ++fi)
            for (int ci = 0; ci < 2; ++ci) {
                char line[96];
                sprintf_s(line, sizeof(line), "%dHz %s %dch", kAudioRates[ri],
                          sampfmtName(kAudioFmts[fi]), kAudioChs[ci]);
                wchar_t wline[96];
                toW(line, wline, 96);
                SendMessageW(r->fmt, CB_ADDSTRING, 0, (LPARAM)wline);
            }
    SendMessageW(r->fmt, CB_SETCURSEL, 0, 0);
}

// [刷新]：重枚举该类 + 填下拉（恢复选中）；摄像头行重查能力表
static void refreshRow(Row* r)
{
    enumRow(r);
    fillDevCombo(r);
    if (r->kind == KIND_CAM)
        fillCamFormats(r);
}

// ---- 拉帧（paint 定时器每 tick 遍历全部会话各拉一帧）----

// 拷贝最近音频帧的第 0 声道样本为 -1..1 浮点（帧仅在下次 get_frame 前有效，
// 必须立即拷贝；超时保留上一画面 = 不动这份缓存）。interleaved 布局：
// 第 i 帧第 0 声道样本在 i*channels。
static void storeWave(Session* s, const sparkcap_audio_frame_t* af)
{
    const int n = af->samples_per_channel;
    const int ch = af->channels > 0 ? af->channels : 1;
    if (n <= 0 || af->data == NULL
        || (af->sample_format != SPARKCAP_SAMPLE_F32
            && af->sample_format != SPARKCAP_SAMPLE_S16)) {
        s->waveCount = 0;
        return;
    }
    if (s->waveCap < n) {
        free(s->wave);
        s->wave = (float*)malloc(sizeof(float) * (size_t)n);
        if (s->wave == NULL) {
            s->waveCap = 0;
            s->waveCount = 0;
            return;
        }
        s->waveCap = n;
    }
    for (int i = 0; i < n; ++i) {
        float v;
        if (af->sample_format == SPARKCAP_SAMPLE_F32)
            v = ((const float*)af->data)[(size_t)i * ch];
        else  // S16 → float（-32768..32767 → -1..1）
            v = ((const short*)af->data)[(size_t)i * ch] / 32768.0f;
        if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
        s->wave[i] = v;
    }
    s->waveCount = n;
    if (af->sample_rate % 1000 == 0)
        sprintf_s(s->waveText, sizeof(s->waveText), "%dkHz %dch %s",
                  af->sample_rate / 1000, af->channels, sampfmtName(af->sample_format));
    else
        sprintf_s(s->waveText, sizeof(s->waveText), "%.1fkHz %dch %s",
                  af->sample_rate / 1000.0, af->channels, sampfmtName(af->sample_format));
}

// 多会话串行拉帧：单路不能久等——统一短超时（队列有帧立即返回，不丢帧；
// 超时保留上一画面：视频 rgb 缓存/音频 wave 缓存均不清不重写）
static void pullSession(Session* s)
{
    if (s->kind == KIND_MIC || s->kind == KIND_LOOP) {
        sparkcap_audio_frame_t* af = NULL;
        const int rc = sparkcap_audio_get_frame(s->dev, &af, 30);
        if (rc != s->lastError)  // 状态跃变才落盘，不逐帧刷日志
            spdlog::info("[{}] audio get_frame {} -> {}", s->name, s->lastError, rc);
        s->lastError = rc;
        if (rc == SPARKCAP_ERR_OK && af != NULL) {
            if (!s->firstFrameLogged) {
                spdlog::info("[{}] first audio frame: {}Hz {}ch {} samples={} size={}",
                             s->name, af->sample_rate, af->channels,
                             sampfmtName(af->sample_format), af->samples_per_channel,
                             af->size);
                s->firstFrameLogged = 1;
            }
            storeWave(s, af);  // 帧仅在下一次 get_frame 前有效：立即拷贝第 0 声道
            ++s->frames;
            ++g_framesThisSec;
        }  // TIMEOUT/真错误：保留上一画面（STREAM_LOST 由错误回调路径自动移除）
    } else {
        // 屏幕域静止桌面 TIMEOUT 为正常语义（F14）；摄像头低帧率会话帧间隔
        // 本来就大于短超时，超时只是"本 tick 没新帧"，下一 tick 继续拉
        const int timeoutMs = (s->kind == KIND_CAM) ? 50 : 30;
        sparkcap_frame_t* f = NULL;
        const int rc = (s->kind == KIND_CAM)
            ? sparkcap_camera_get_frame(s->dev, &f, timeoutMs)
            : sparkcap_screen_get_frame(s->dev, &f, timeoutMs);
        if (rc != s->lastError)
            spdlog::info("[{}] get_frame {} -> {}", s->name, s->lastError, rc);
        s->lastError = rc;
        if (rc == SPARKCAP_ERR_OK && f != NULL) {
            if (!s->firstFrameLogged) {
                spdlog::info("[{}] first frame: {}x{} fmt={} stride={} size={}",
                             s->name, f->width, f->height, (int)f->pixel_format,
                             f->stride, f->size);
                s->firstFrameLogged = 1;
            }
            s->lastW = f->width;   // 标注用实际交付参数（转换失败也记录）
            s->lastH = f->height;
            s->lastFmt = f->pixel_format;
            frameToRgb32(s, f);    // 失败时 rgb 保持旧缓存（保留上一画面）
            ++s->frames;
            ++g_framesThisSec;
        }
    }
}

// ---- 预览网格 ----

// 活跃会话数 → 分格：1 全幅 / 2 左右 / 3-4 2x2 / 5-6 3x2 / 7-8 4x2
static void gridFor(int n, int* cols, int* rows)
{
    if (n <= 1)      { *cols = 1; *rows = 1; }
    else if (n == 2) { *cols = 2; *rows = 1; }
    else if (n <= 4) { *cols = 2; *rows = 2; }
    else if (n <= 6) { *cols = 3; *rows = 2; }
    else             { *cols = 4; *rows = 2; }
}

// 第 i 格的矩形（余数给末列/末行；按需计算，窗口伸缩自动重排）
static void cellRect(int i, const RECT* view, RECT* out)
{
    int cols, rows;
    gridFor(g_sessCount, &cols, &rows);
    const int cw = (view->right - view->left) / cols;
    const int ch = (view->bottom - view->top) / rows;
    const int cx = i % cols;
    const int cy = i / cols;
    out->left = view->left + cx * cw;
    out->top = view->top + cy * ch;
    out->right = (cx == cols - 1) ? view->right : out->left + cw;
    out->bottom = (cy == rows - 1) ? view->bottom : out->top + ch;
}

// 格子绘制区（背景边框内缩）
static void cellInner(const RECT* cell, RECT* inner)
{
    *inner = *cell;
    InflateRect(inner, -3, -3);
}

// 格右上 [×] 热区（绘制与 WM_LBUTTONDOWN 命中共用同一计算）
static void cellCloseZone(const RECT* cell, RECT* out)
{
    RECT inner;
    cellInner(cell, &inner);
    out->left = inner.right - 20;
    out->top = inner.top + 2;
    out->right = inner.right - 2;
    out->bottom = inner.top + 20;
}

// 经典波形包络：样本按格宽分桶，每列画桶内 min/max 竖线（-1..1 映射到
// 格高、中线居中）；静音帧 min=max=0 → 中线上 1px 平线（契约可目检）。
static void drawWaveform(HDC dc, const Session* s, const RECT* cell)
{
    const int vw = cell->right - cell->left;
    const int vh = cell->bottom - cell->top;
    const int mid = cell->top + vh / 2;
    const int half = vh / 2 - 8;  // 上下留边
    if (vw <= 0 || half <= 0 || s->waveCount <= 0)
        return;

    HPEN gray = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));   // 中线（静音参考）
    HGDIOBJ oldPen = SelectObject(dc, gray);
    MoveToEx(dc, cell->left, mid, NULL);
    LineTo(dc, cell->right, mid);

    HPEN green = CreatePen(PS_SOLID, 1, RGB(80, 220, 120));  // 波形
    SelectObject(dc, green);
    const int n = s->waveCount;
    for (int col = 0; col < vw; ++col) {
        // 整数分桶（区间单调不重叠；样本数少于列数时稀疏列留空）
        const int begin = (int)((long long)n * col / vw);
        const int end = (int)((long long)n * (col + 1) / vw);
        if (end <= begin)
            continue;
        float mn = 1.f, mx = -1.f;
        for (int i = begin; i < end; ++i) {
            const float v = s->wave[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int yTop = mid - (int)(mx * half);
        int yBot = mid - (int)(mn * half);
        if (yBot == yTop)
            yBot = yTop + 1;  // 至少 1px：静音平线可见
        MoveToEx(dc, cell->left + col, yTop, NULL);
        LineTo(dc, cell->left + col, yBot);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gray);
    DeleteObject(green);
}

// 单格绘制：视频 letterbox 缩放（限格内）/ 音频波形 + 格内标注 + [×] 热区
static void drawCell(HDC dc, const RECT* cell, const Session* s)
{
    // 格背景 + 边框（与邻格/黑底区分）
    RECT bg = *cell;
    InflateRect(&bg, -2, -2);
    FillRect(dc, &bg, (HBRUSH)GetStockObject(BLACK_BRUSH));
    FrameRect(dc, &bg, (HBRUSH)GetStockObject(GRAY_BRUSH));

    RECT inner;
    cellInner(cell, &inner);

    if (s->kind == KIND_MIC || s->kind == KIND_LOOP) {
        drawWaveform(dc, s, &inner);
    } else if (s->rgb != NULL && s->rgbW > 0 && s->rgbH > 0) {
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = s->rgbW;
        bi.bmiHeader.biHeight = -s->rgbH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        const int vw = inner.right - inner.left;
        const int vh = inner.bottom - inner.top;
        if (vw > 0 && vh > 0) {
            const double scale = (vw < (int)((double)vh * s->rgbW / s->rgbH))
                ? (double)vw / s->rgbW : (double)vh / s->rgbH;
            const int dw = (int)(s->rgbW * scale);
            const int dh = (int)(s->rgbH * scale);
            // 缩小必须改拉伸模式：默认 BLACKONWHITE 对源像素做 AND 合并，
            // 视频逐帧变化时产生闪烁方格纹；COLORONCOLOR 丢弃法为视频预览
            // 标准做法（放大路径本就无合并）
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, inner.left + (vw - dw) / 2, inner.top + (vh - dh) / 2,
                          dw, dh, 0, 0, s->rgbW, s->rgbH, s->rgb, &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
    } else {
        // 尚无可用画面：状态文本（超时等待/真错误/不支持的格式）
        char msg[160];
        if (s->lastError == SPARKCAP_ERR_TIMEOUT)
            strcpy_s(msg, sizeof(msg), "waiting for frames...");
        else if (s->lastError != SPARKCAP_ERR_OK)
            sprintf_s(msg, sizeof(msg), "error: %s (%d)", errName(s->lastError),
                      s->lastError);
        else if (s->lastW > 0)
            sprintf_s(msg, sizeof(msg), "unsupported pixel format: %s (stride n/a)",
                      pixfmtName(s->lastFmt));
        else
            strcpy_s(msg, sizeof(msg), "no frames yet");
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        drawUtf8(dc, inner.left + 16, inner.top + 16, msg);
    }

    // 格内标注：设备名 + 实际格式 + 帧计数
    //（视频 "name 1280x720 YUY2 帧:12345"；音频 "name 48kHz 2ch F32 帧:678"）
    char tag[288];
    if (s->kind == KIND_MIC || s->kind == KIND_LOOP) {
        if (s->waveText[0])
            sprintf_s(tag, sizeof(tag), "%s  %s  帧:%llu", s->name, s->waveText,
                      s->frames);
        else
            sprintf_s(tag, sizeof(tag), "%s  帧:%llu", s->name, s->frames);
    } else if (s->rgb != NULL) {
        sprintf_s(tag, sizeof(tag), "%s  %dx%d %s  帧:%llu", s->name, s->lastW,
                  s->lastH, pixfmtName(s->lastFmt), s->frames);
    } else {
        sprintf_s(tag, sizeof(tag), "%s", s->name);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(210, 210, 210));
    drawUtf8(dc, inner.left + 8, inner.top + 6, tag);

    // 右上角 [×] 热区（点击停止单路）
    RECT xz;
    cellCloseZone(cell, &xz);
    FrameRect(dc, &xz, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(170, 170, 170));
    TextOutW(dc, xz.left + 6, xz.top + 2, L"×", 1);
}

// ---- 渲染主入口（双缓冲外层在 WM_PAINT）----

static void paint(HDC dc, HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT view = {0, TOOLBAR_H, rc.right, rc.bottom};

    // 每 tick 遍历全部会话各拉一帧（视频超时保留上一画面、音频超时冻结波形）
    for (int i = 0; i < g_sessCount; ++i)
        pullSession(&g_sess[i]);

    FillRect(dc, &view, (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (g_sessCount == 0) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        drawUtf8(dc, view.left + 16, view.top + 16,
                 "multi-session bench: pick a device in a row above and press [开] "
                 "(up to 8 sessions; click the × in a cell corner to stop it)");
    }
    for (int i = 0; i < g_sessCount; ++i) {
        RECT cell;
        cellRect(i, &view, &cell);
        drawCell(dc, &cell, &g_sess[i]);
    }

    // 底部：最近日志（日志回调路由，FFmpeg/后端诊断）
    if (g_logCount > 0) {
        SetBkMode(dc, TRANSPARENT);
        EnterCriticalSection(&g_logCs);
        for (int i = 0; i < g_logCount; ++i) {
            // 从旧到新：环形缓冲中 head 前第 (count-i) 条
            const int idx = (g_logHead - (g_logCount - i) + LOG_LINES) % LOG_LINES;
            SetTextColor(dc, RGB(150, 150, 150));
            drawUtf8(dc, view.left + 16, view.bottom - 18 * (g_logCount - i),
                     g_logs[idx]);
        }
        LeaveCriticalSection(&g_logCs);
    }

    // 标题栏：每秒更新会话数与总拉帧率（全部会话合计）
    const ULONGLONG now = GetTickCount64();
    if (now - g_secStart >= 1000) {
        char title[96];
        sprintf_s(title, sizeof(title), "SparkCapDemo  sessions=%d  %u fps",
                  g_sessCount, g_framesThisSec);
        wchar_t wtitle[96];
        toW(title, wtitle, 96);
        SetWindowTextW(hwnd, wtitle);
        g_framesThisSec = 0;
        g_secStart = now;
    }
}

// ---- Win32 ----

// 工具栏：每类一行（标签 + 设备下拉 [+ 格式下拉] + [开] + [刷新]）
static void createToolbar(HWND hwnd, HINSTANCE inst)
{
    static const wchar_t* kLabels[KIND_COUNT] = {
        L"摄像头", L"显示器", L"窗口", L"麦克风", L"回采"
    };
    for (int k = 0; k < KIND_COUNT; ++k) {
        Row* r = &g_row[k];
        r->kind = k;
        const int y = 6 + k * ROW_H;
        int x = 8;
        r->labelCtl = CreateWindowExW(0, L"STATIC", kLabels[k],
                                      WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                      x, y + 3, 44, 20, hwnd, NULL, inst, NULL);
        x += 44 + 6;
        r->dev = CreateWindowExW(0, L"COMBOBOX", NULL,
                                 WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 x, y, 280, 300, hwnd, (HMENU)(INT_PTR)ROW_ID(k, ROW_DEV),
                                 inst, NULL);
        SendMessageW(r->dev, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
        x += 280 + 6;
        if (k == KIND_CAM || k == KIND_MIC || k == KIND_LOOP) {
            // 显示器/窗口行无格式下拉（原生 BGRA，无协商项）
            r->fmt = CreateWindowExW(0, L"COMBOBOX", NULL,
                                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     x, y, 210, 300, hwnd,
                                     (HMENU)(INT_PTR)ROW_ID(k, ROW_FMT), inst, NULL);
            SendMessageW(r->fmt, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
            x += 210 + 6;
        }
        r->open = CreateWindowExW(0, L"BUTTON", L"开",
                                  WS_CHILD | WS_VISIBLE,
                                  x, y, 56, ROW_H - 6, hwnd,
                                  (HMENU)(INT_PTR)ROW_ID(k, ROW_OPEN), inst, NULL);
        x += 56 + 6;
        r->refresh = CreateWindowExW(0, L"BUTTON", L"刷新",
                                     WS_CHILD | WS_VISIBLE,
                                     x, y, 64, ROW_H - 6, hwnd,
                                     (HMENU)(INT_PTR)ROW_ID(k, ROW_REFRESH), inst, NULL);
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        createToolbar(hwnd, cs->hInstance);
        return 0;
    }
    case WM_TIMER: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top = TOOLBAR_H;  // 只刷新预览区，不触碰工具栏子控件（消除闪烁）
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;  // 双缓冲下不需要系统擦背景（消除闪烁源）
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        // 双缓冲：只画预览区（工具栏区域留给子控件自绘，避免覆盖闪烁）
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top = TOOLBAR_H;
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ old = SelectObject(mem, bmp);
            SetWindowOrgEx(mem, rc.left, rc.top, NULL);  // paint() 用全客户区坐标
            paint(mem, hwnd);
            BitBlt(dc, rc.left, rc.top, w, h, mem, rc.left, rc.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        // 工具栏固定高；网格区随客户区伸缩（格子按需计算，重绘即重排）
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id < IDC_ROW_BASE || id >= IDC_ROW_BASE + KIND_COUNT * 4)
            break;
        const int kind = (id - IDC_ROW_BASE) / 4;
        const int which = (id - IDC_ROW_BASE) % 4;
        Row* r = &g_row[kind];
        if (which == ROW_DEV && HIWORD(wp) == CBN_SELCHANGE) {
            updateRowSel(r);  // 记录选中标识（刷新后恢复选中用）
            if (kind == KIND_CAM)
                fillCamFormats(r);  // 摄像头：按新设备重查能力表
        } else if (which == ROW_OPEN && HIWORD(wp) == BN_CLICKED) {
            const int sel = (int)SendMessageW(r->dev, CB_GETCURSEL, 0, 0);
            const int fsel = (r->fmt != NULL)
                ? (int)SendMessageW(r->fmt, CB_GETCURSEL, 0, 0) : 0;
            openSession(kind, sel, fsel);
        } else if (which == ROW_REFRESH && HIWORD(wp) == BN_CLICKED) {
            refreshRow(r);  // 只重枚举该行下拉，不打扰运行中会话
        }
        return 0;
    }
    case WM_APP_ERROR: {
        // 错误回调线程转投：按句柄反查会话（仅比较不解引用，会话可能已停）
        sparkcap_device_t* dev = (sparkcap_device_t*)lp;
        for (int i = 0; i < g_sessCount; ++i) {
            if (g_sess[i].dev == dev) {
                if ((int)wp == (int)SPARKCAP_ERR_STREAM_LOST) {
                    // 流断（设备拔出/窗口关闭/独占全屏）：自动移除 + 日志；
                    // stop/close 在 UI 线程执行（不在回调内，无 IN_CALLBACK 问题）
                    spdlog::info("[{}] stream lost: auto-remove session",
                                 g_sess[i].name);
                    stopSession(i);
                }
                // SOURCE_PAUSED（窗口最小化）等可恢复状态：会话保留观察
                break;
            }
        }
        return 0;
    }
    case WM_APP_LOG: {  // 回调线程 PostMessage：刷新预览区（日志）
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top = TOOLBAR_H;
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // 格右上 [×] 热区命中 → 停止单路（与 drawCell 共用 cellCloseZone 计算）
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT view = {0, TOOLBAR_H, rc.right, rc.bottom};
        for (int i = 0; i < g_sessCount; ++i) {
            RECT cell, xz;
            cellRect(i, &view, &cell);
            cellCloseZone(&cell, &xz);
            if (x >= xz.left && x < xz.right && y >= xz.top && y < xz.bottom) {
                stopSession(i);
                break;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;

    // 文件日志（运行目录，1MB 轮转保留一份）——报障第一手资料
    try {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "sparkcap_demo.log", 1024 * 1024, 1);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("demo", sink));
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);  // 每条即刷：崩溃时日志必在盘
        spdlog::info("SparkCapDemo start (multi-session bench)");
        // DLL 能力集落日志：报障时一眼排除"误用裁剪版"类环境问题
        const uint32_t feats = sparkcap_query_features();
        spdlog::info("sparkcap features: 0x{:x} ({}{}{}{})", feats,
                     (feats & SPARKCAP_FEATURE_CAMERA) ? "camera" : "-",
                     (feats & SPARKCAP_FEATURE_IP_CAMERA) ? " +ip_camera" : "",
                     (feats & SPARKCAP_FEATURE_SCREEN) ? " +screen" : "",
                     (feats & SPARKCAP_FEATURE_AUDIO) ? " +audio" : "");
    } catch (...) {
        // 日志初始化失败（目录只读等）不阻断预览
    }

    Gdiplus::GdiplusStartupInput gdiplusInput = {0};
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);

    if (sparkcap_create_backend(&g_backend) != SPARKCAP_ERR_OK) {
        MessageBoxA(NULL, "sparkcap_create_backend failed", "SparkCapDemo", MB_ICONERROR);
        return 1;
    }
    InitializeCriticalSection(&g_logCs);
    // 日志回调：进程级全局路由（FFmpeg av_log WARN+ 等），任意时机可注册
    sparkcap_set_log_callback(onLogCb, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SparkCapDemo";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SparkCapDemo", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, NULL, NULL, inst,
                                NULL);
    g_hwnd = hwnd;  // 回调线程经此 PostMessage

    // 初始枚举：五类各自填下拉（音频格式下拉为固定表）；不自动开会话，
    // 多会话测试台从空网格开始，由用户逐路 [开] 累积
    for (int k = 0; k < KIND_COUNT; ++k)
        refreshRow(&g_row[k]);
    fillAudioFormats(&g_row[KIND_MIC]);
    fillAudioFormats(&g_row[KIND_LOOP]);

    SetTimer(hwnd, 1, 33, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    stopAllSessions();
    sparkcap_set_log_callback(NULL, NULL);  // 注销后再销毁（回调不再进入）
    spdlog::info("SparkCapDemo exit");
    sparkcap_destroy_backend(g_backend);
    DeleteCriticalSection(&g_logCs);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
