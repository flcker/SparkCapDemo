// SparkCapDemo —— SparkCap 接入示例：摄像头预览调试工具（Win32 GDI，零依赖）。
// 布局：顶部工具栏（设备/刷新/分辨率/变换）+ 预览区（letterbox + 变换渲染）。
// 按键：ESC 退出 | 空格 暂停 | N 下一台设备
// 回调集成：错误回调（流断即时提示，替代轮询超时猜测）+ 日志回调
// （FFmpeg av_log 路由到预览区底部——RTSP 连接失败的真实原因可视化）。

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// GDI+（系统组件）解 MJPG 帧
#include <gdiplus.h>
#include <shlwapi.h>

// spdlog（文件日志）——必须在 windows.h/gdiplus 之后：
// 提前引入会破坏 GDI+ 头的 GUID 宏展开（GdiplusImaging.h C2440）
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

// ---- 控件与布局 ----

#define BAR_H 44
#define IDC_DEVICE_COMBO 1001
#define IDC_REFRESH_BTN  1002
#define IDC_FORMAT_COMBO 1003
#define IDC_ROTATE_BTN   1004
#define IDC_MIRROR_BTN   1005
#define IDC_FLIP_BTN     1006

// 工具栏布局表（x 宽度 依次排开）
typedef struct { int id; int w; const wchar_t* cls; const wchar_t* text; DWORD style; } CtlSpec;

static const CtlSpec kControls[] = {
    {IDC_DEVICE_COMBO, 300, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL},
    {IDC_REFRESH_BTN,   70, L"BUTTON",  L"Refresh",   0},
    {IDC_FORMAT_COMBO, 230, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL},
    {IDC_ROTATE_BTN,   100, L"BUTTON",  L"Rotate 0",  0},
    {IDC_MIRROR_BTN,    86, L"BUTTON",  L"Mirror",    0},
    {IDC_FLIP_BTN,      70, L"BUTTON",  L"Flip",      0},
};
#define CONTROL_COUNT (int)(sizeof(kControls) / sizeof(kControls[0]))

// ---- 应用状态 ----

typedef struct {
    sparkcap_backend_t* backend;
    sparkcap_device_t* dev;
    sparkcap_device_info_t devices[32];
    int deviceCount;
    int current;
    int paused;

    sparkcap_device_format_t formats[64];
    int formatCount;
    int formatSel;

    int rotate;           // 0/1/2/3 = 0/90/180/270
    int mirror;
    int flip;
    int curFps;           // 当前会话协商帧率（get_frame 超时适配；0 = 未知）

    unsigned int* rgb;
    int rgbW, rgbH;
    unsigned int* tRgb;
    int tRgbW, tRgbH;

    char fpsText[96];
    int framesThisSec;
    ULONGLONG secStart;
} App;

static App g_app = {0};
static HWND g_ctl[CONTROL_COUNT];
static HWND g_hwnd;

// ---- 回调（采集线程执行：只做快速记录 + PostMessage，不碰 UI）----

#define WM_APP_ERROR  (WM_APP + 1)  // wp = 错误码
#define WM_APP_LOG    (WM_APP + 2)

static volatile LONG g_errorNotified;  // 1 = 错误回调已触发（流断确认）
static int g_firstFrameLogged;         // 会话内首帧只记一次（openDevice 重置）

#define LOG_LINES 4
#define LOG_TEXT  160
static char g_logs[LOG_LINES][LOG_TEXT];  // 环形：最近 LOG_LINES 条
static int g_logHead, g_logCount;
static CRITICAL_SECTION g_logCs;

static void onErrorCb(sparkcap_device_t* dev, int code, void* userdata)
{
    (void)dev; (void)userdata;
    spdlog::error("error callback: code={} ({})", code, "stream lost");
    InterlockedExchange(&g_errorNotified, 1);
    PostMessageW(g_hwnd, WM_APP_ERROR, (WPARAM)code, 0);  // 唤醒重绘（跨线程安全）
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

// ---- 像素格式转换（输出侧全格式 → RGB32）----

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

static int frameToRgb32(const sparkcap_frame_t* f)
{
    const int w = f->width;
    const int h = f->height;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return -1;
    if (g_app.rgbW != w || g_app.rgbH != h) {
        free(g_app.rgb);
        g_app.rgb = (unsigned int*)malloc(sizeof(unsigned int) * (size_t)w * h);
        if (g_app.rgb == NULL)
            return -1;
        g_app.rgbW = w;
        g_app.rgbH = h;
    }

    const unsigned char* data = (const unsigned char*)f->data;
    switch (f->pixelFormat) {
    case SPARKCAP_PIXFMT_YUY2: {
        const int stride = f->stride > 0 ? f->stride : w * 2;
        for (int row = 0; row < h; ++row) {
            const unsigned char* src = data + (size_t)stride * row;
            unsigned int* dst = g_app.rgb + (size_t)w * row;
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
            unsigned int* dst = g_app.rgb + (size_t)w * row;
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
            unsigned int* dst = g_app.rgb + (size_t)w * row;
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
            unsigned int* dst = g_app.rgb + (size_t)w * (flipRow ? (h - 1 - row) : row);
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
            unsigned int* dst = g_app.rgb + (size_t)w * (flipRow ? (h - 1 - row) : row);
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
                    unsigned int* dst = g_app.rgb + (size_t)w * row;
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

// ---- 变换 ----

static const unsigned int* applyTransform(int* outW, int* outH)
{
    const int w = g_app.rgbW;
    const int h = g_app.rgbH;
    const int quarter = g_app.rotate & 3;
    const int tw = (quarter & 1) ? h : w;
    const int th = (quarter & 1) ? w : h;

    *outW = tw;
    *outH = th;
    if (quarter == 0 && !g_app.mirror && !g_app.flip)
        return g_app.rgb;

    if (g_app.tRgbW != tw || g_app.tRgbH != th) {
        free(g_app.tRgb);
        g_app.tRgb = (unsigned int*)malloc(sizeof(unsigned int) * (size_t)tw * th);
        if (g_app.tRgb == NULL)
            return g_app.rgb;
        g_app.tRgbW = tw;
        g_app.tRgbH = th;
    }

    // 行宽（镜像在旋转后的坐标系里取）
    const int rowW = (quarter & 1) ? th : tw;
    const int colH = (quarter & 1) ? tw : th;
    for (int y = 0; y < th; ++y) {
        unsigned int* dst = g_app.tRgb + (size_t)tw * y;
        for (int x = 0; x < tw; ++x) {
            int sx, sy;
            switch (quarter) {
            case 1:  sx = y;         sy = h - 1 - x; break;
            case 2:  sx = w - 1 - x; sy = h - 1 - y; break;
            case 3:  sx = w - 1 - y; sy = x;         break;
            default: sx = x;         sy = y;         break;
            }
            if (g_app.mirror)
                sx = rowW - 1 - sx;
            if (g_app.flip)
                sy = colH - 1 - sy;
            dst[x] = g_app.rgb[(size_t)sy * w + sx];
        }
    }
    return g_app.tRgb;
}

// ---- 设备与能力 ----

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

static void closeDevice(void)
{
    if (g_app.dev != NULL) {
        spdlog::info("close: {}", g_app.devices[g_app.current].name);
        sparkcap_stop(g_app.dev);
        sparkcap_close(g_app.dev);
        g_app.dev = NULL;
    }
}

static void updateTransformButtons(void)
{
    const wchar_t* rot[] = {L"Rotate 0", L"Rotate 90", L"Rotate 180", L"Rotate 270"};
    SetWindowTextW(g_ctl[3], rot[g_app.rotate & 3]);
    SetWindowTextW(g_ctl[4], g_app.mirror ? L"Mirror ON" : L"Mirror");
    SetWindowTextW(g_ctl[5], g_app.flip ? L"Flip ON" : L"Flip");
}

static int openDevice(int index)
{
    if (g_app.deviceCount == 0)
        return -1;
    closeDevice();
    g_app.current = index % g_app.deviceCount;
    const sparkcap_device_info_t* info = &g_app.devices[g_app.current];

    if (sparkcap_open(g_app.backend, info->id, &g_app.dev) != SPARKCAP_ERR_OK) {
        spdlog::warn("open failed: {}", info->name);
        g_app.dev = NULL;
        return -1;
    }
    spdlog::info("open: {}", info->name);
    // 错误回调须在 start 前注册（running 后注册返回 UNSUPPORTED）
    InterlockedExchange(&g_errorNotified, 0);
    sparkcap_set_error_callback(g_app.dev, onErrorCb, NULL);
    int w = 0, h = 0, fps = 0;
    sparkcap_pixfmt_t fmtPref = SPARKCAP_PIXFMT_UNKNOWN;
    if (g_app.formatSel > 0 && g_app.formatSel <= g_app.formatCount) {
        // 手选：完整传格式偏好与帧率（此前只传 w/h，库在同分辨率组内自由选型，
        // 手选 MJPG 可能实际协商成 NV12——带宽目的落空）
        const sparkcap_device_format_t* fm = &g_app.formats[g_app.formatSel - 1];
        w = fm->width;
        h = fm->height;
        fmtPref = fm->pixelFormat;
        fps = fm->fps;
    } else if (g_app.formatCount > 0) {
        // Default：720p 档内开箱即用——fps 最高 → 面积最大 → MJPG 优先。
        // 档位上限 1280×720 是 USB 2.0 的行业安全默认（J.ZAO 实测 720p MJPG@30 满帧；
        // 2560×1440@30 带宽超载无数据），虚拟设备也不再被压到最小画面。
        // fps<10 的省电/病态条目剔除（旧"带宽最小"策略曾选中 fps=2 → 实际 1-2fps）。
        // 回落：设备最低档就超 720p 时取设备最低档（有得选才避高规格）。
        int hasFast = 0;  // 存在 ≥10fps 条目时，低帧率条目不参与 Default
        for (int i = 0; i < g_app.formatCount; ++i)
            if (g_app.formats[i].fps >= 10) {
                hasFast = 1;
                break;
            }
        const long long kBandMax = 1280LL * 720;
        long long capArea = kBandMax;
        int anyInBand = 0;
        long long minArea = 0;
        for (int i = 0; i < g_app.formatCount; ++i) {
            const sparkcap_device_format_t* fm = &g_app.formats[i];
            if (hasFast && fm->fps < 10)
                continue;
            const long long area = (long long)fm->width * fm->height;
            if (area <= kBandMax)
                anyInBand = 1;
            else if (minArea == 0 || area < minArea)
                minArea = area;
        }
        if (!anyInBand && minArea > 0)
            capArea = minArea;  // 设备全部超档：回落最低档

        int bestFps = -1;
        long long bestArea = 0;
        int bestIdx = -1;
        for (int pass = 0; pass < 2 && bestIdx < 0; ++pass) {  // 第一轮 MJPG，第二轮非 MJPG
            for (int i = 0; i < g_app.formatCount; ++i) {
                const sparkcap_device_format_t* fm = &g_app.formats[i];
                if (hasFast && fm->fps < 10)
                    continue;
                const int isMjpg = fm->pixelFormat == SPARKCAP_PIXFMT_MJPG;
                if (isMjpg != (pass == 0))
                    continue;
                const long long area = (long long)fm->width * fm->height;
                if (area > capArea)
                    continue;
                if (fm->fps > bestFps || (fm->fps == bestFps && area > bestArea)) {
                    bestFps = fm->fps;
                    bestArea = area;
                    bestIdx = i;
                }
            }
        }
        if (bestIdx >= 0) {
            w = g_app.formats[bestIdx].width;
            h = g_app.formats[bestIdx].height;
            fmtPref = g_app.formats[bestIdx].pixelFormat;
            fps = g_app.formats[bestIdx].fps;
        }
    }
    spdlog::info("start: w={} h={} fmt={} fps={}", w, h, (int)fmtPref, fps);
    if (sparkcap_start(g_app.dev, w, h, fmtPref, fps) != SPARKCAP_ERR_OK) {
        spdlog::warn("start failed");
        sparkcap_close(g_app.dev);
        g_app.dev = NULL;
        return -1;
    }
    g_app.curFps = fps;
    g_app.paused = 0;
    g_firstFrameLogged = 0;  // 新会话：首帧日志重新生效
    return 0;
}

static void refreshDeviceCombo(void)
{
    int count = (int)(sizeof(g_app.devices) / sizeof(g_app.devices[0]));
    if (sparkcap_list_devices(g_app.backend, g_app.devices, &count) != SPARKCAP_ERR_OK)
        count = 0;
    g_app.deviceCount = count;
    spdlog::info("device list: {} entries", count);
    for (int i = 0; i < count; ++i)
        spdlog::info("  [{}] {} (backend={})", i, g_app.devices[i].name,
                     (int)g_app.devices[i].backend);

    SendMessageW(g_ctl[0], CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) {
        wchar_t text[320];
        const char* backend = g_app.devices[i].backend == SPARKCAP_BACKEND_MF ? " [MF]"
            : g_app.devices[i].backend == SPARKCAP_BACKEND_DSHOW ? " [DShow]" : " [RTSP]";
        char line[300];
        sprintf_s(line, sizeof(line), "%s%s", g_app.devices[i].name, backend);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, text, 320);
        SendMessageW(g_ctl[0], CB_ADDSTRING, 0, (LPARAM)text);
    }
}

static void fillFormatCombo(void)
{
    SendMessageW(g_ctl[2], CB_RESETCONTENT, 0, 0);
    SendMessageW(g_ctl[2], CB_ADDSTRING, 0, (LPARAM)L"Default");

    g_app.formatCount = 0;
    if (g_app.deviceCount > 0) {
        const int cap = (int)(sizeof(g_app.formats) / sizeof(g_app.formats[0]));
        int n = cap;
        const int devId = g_app.devices[g_app.current].id;
        if (sparkcap_get_formats(g_app.backend, devId, g_app.formats, &n) == SPARKCAP_ERR_OK)
            g_app.formatCount = n > cap ? cap : n;  // get_formats 更新为总数，可能 > 容量
    }
    for (int i = 0; i < g_app.formatCount; ++i) {
        const sparkcap_device_format_t* fm = &g_app.formats[i];
        char text[96];
        const long long mbps = formatBandwidthMBs(fm->width, fm->height, fm->pixelFormat, fm->fps);
        if (mbps > 35)
            sprintf_s(text, sizeof(text), "%dx%d %s @%d (%lldMB/s!)",
                      fm->width, fm->height, pixfmtName(fm->pixelFormat), fm->fps, mbps);
        else
            sprintf_s(text, sizeof(text), "%dx%d %s @%d",
                      fm->width, fm->height, pixfmtName(fm->pixelFormat), fm->fps);
        wchar_t wtext[96];
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, 96);
        SendMessageW(g_ctl[2], CB_ADDSTRING, 0, (LPARAM)wtext);
    }
    g_app.formatSel = 0;
    SendMessageW(g_ctl[2], CB_SETCURSEL, 0, 0);
}

// ---- 渲染 ----

static void paint(HDC dc, HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT view = {0, BAR_H, rc.right, rc.bottom};

    sparkcap_frame_t* f = NULL;
    int haveFrame = 0;
    static int lastPixelFormat = -1;
    static int lastStride = 0;
    static int lastError = 0;  // 最近的 get_frame 错误（0 = 有帧）
    static int waitStreak = 0;  // 连续超时计数（防抖：偶发超时重画上一帧，不闪 waiting 文本）
    if (!g_app.paused && g_app.dev != NULL)
    {
        // 超时随协商帧率适配：低帧率会话的帧间隔本来就 >200ms，不能算超时
        const int byFps = (g_app.curFps > 0) ? 2500 / g_app.curFps : 0;
        const int timeoutMs = (byFps > 200) ? byFps : 200;
        const int grc = sparkcap_get_frame(g_app.dev, &f, timeoutMs);
        if (grc != lastError)  // 状态跃变（含首帧/恢复/流断）才落盘，不逐帧刷日志
            spdlog::info("get_frame {} -> {}", lastError, grc);
        lastError = grc;
        if (grc == SPARKCAP_ERR_OK && f != NULL) {
            waitStreak = 0;
            if (!g_firstFrameLogged) {
                spdlog::info("first frame: {}x{} fmt={} stride={} size={}",
                             f->width, f->height, (int)f->pixelFormat, f->stride, f->size);
                g_firstFrameLogged = 1;
            }
            lastPixelFormat = (int)f->pixelFormat;
            lastStride = f->stride;
            haveFrame = (frameToRgb32(f) == 0);
            ++g_app.framesThisSec;
            InterlockedExchange(&g_errorNotified, 0);  // 帧恢复交付 = 流已恢复
        } else if (grc == SPARKCAP_ERR_TIMEOUT) {
            // 帧间隔抖动（驱动输出不规律）是常态：短窗口内保持上一帧画面
            if (++waitStreak < 5 && g_app.rgbW > 0)
                haveFrame = 1;
        } else {
            waitStreak = 0;  // 真错误（STREAM_LOST 等）立即如实提示
        }
    }

    FillRect(dc, &view, (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (haveFrame && g_app.rgb != NULL) {
        int tw, th;
        const unsigned int* pixels = applyTransform(&tw, &th);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = tw;
        bi.bmiHeader.biHeight = -th;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        const int vw = view.right - view.left;
        const int vh = view.bottom - view.top;
        if (vw > 0 && vh > 0 && tw > 0 && th > 0) {
            const double scale = (vw < (int)((double)vh * tw / th))
                ? (double)vw / tw : (double)vh / th;
            const int dw = (int)(tw * scale);
            const int dh = (int)(th * scale);
            const int dx = view.left + (vw - dw) / 2;
            const int dy = view.top + (vh - dh) / 2;
            // 缩小必须改拉伸模式：默认 BLACKONWHITE 对源像素做 AND 合并，
            // 视频逐帧变化时产生闪烁方格纹（1280x720 在默认窗口 0.92x 缩放实测）；
            // COLORONCOLOR 丢弃法为视频预览标准做法（放大路径本就无合并，最大化即消失）
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, dx, dy, dw, dh, 0, 0, tw, th, pixels, &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        char msg[160];
        if (g_app.dev == NULL)
            strcpy_s(msg, sizeof(msg), "select a device");
        else if (g_app.paused)
            strcpy_s(msg, sizeof(msg), "paused");
        else if (g_errorNotified)
            strcpy_s(msg, sizeof(msg),
                     "stream lost (error callback) - RTSP auto-reconnects; camera needs "
                     "re-open. See log below for details");
        else if (lastError == SPARKCAP_ERR_TIMEOUT)
            strcpy_s(msg, sizeof(msg),
                     "waiting for frames... (no data - check bandwidth: USB2 cannot "
                     "carry high-res high-fps; try lower format in combo)");
        else if (lastError == SPARKCAP_ERR_STREAM_LOST)
            strcpy_s(msg, sizeof(msg),
                     "stream lost: this format delivers no data (device/firmware issue "
                     "or bandwidth). Pick another from the format combo ->");
        else if (lastPixelFormat >= 0)
            sprintf_s(msg, sizeof(msg), "unsupported pixel format: %d (stride=%d)",
                      lastPixelFormat, lastStride);
        else
            strcpy_s(msg, sizeof(msg), "no frames yet");
        TextOutA(dc, view.left + 16, view.top + 16, msg, (int)lstrlenA(msg));
    }

    // 底部：最近日志（日志回调路由，FFmpeg/后端诊断）
    if (g_logCount > 0) {
        SetBkMode(dc, TRANSPARENT);
        EnterCriticalSection(&g_logCs);
        for (int i = 0; i < g_logCount; ++i) {
            // 从旧到新：环形缓冲中 head 前第 (count-i) 条
            const int idx = (g_logHead - (g_logCount - i) + LOG_LINES) % LOG_LINES;
            SetTextColor(dc, RGB(150, 150, 150));
            TextOutA(dc, view.left + 16, view.bottom - 18 * (g_logCount - i),
                     g_logs[idx], (int)lstrlenA(g_logs[idx]));
        }
        LeaveCriticalSection(&g_logCs);
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_app.secStart >= 1000) {
        sprintf_s(g_app.fpsText, sizeof(g_app.fpsText),
                  "SparkCapDemo  %s  %dx%d  %d fps",
                  g_app.deviceCount > 0 ? g_app.devices[g_app.current].name : "-",
                  g_app.rgbW, g_app.rgbH, g_app.framesThisSec);
        wchar_t wtext[96];
        MultiByteToWideChar(CP_UTF8, 0, g_app.fpsText, -1, wtext, 96);
        SetWindowTextW(hwnd, wtext);
        g_app.framesThisSec = 0;
        g_app.secStart = now;
    }
}

// ---- Win32 ----

static void layout(void)
{
    int x = 8;
    for (int i = 0; i < CONTROL_COUNT; ++i) {
        // COMBOBOX 的高度参数 = 下拉列表总高（创建/MoveWindow 时生效）；
        // 按钮高度 = 编辑框部分
        const int id = kControls[i].id;
        const int isCombo = (id == IDC_DEVICE_COMBO || id == IDC_FORMAT_COMBO);
        const int h = isCombo ? 300 : BAR_H - 16;
        MoveWindow(g_ctl[i], x, 8, kControls[i].w, h, TRUE);
        x += kControls[i].w + 8;
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        for (int i = 0; i < CONTROL_COUNT; ++i) {
            g_ctl[i] = CreateWindowExW(0, kControls[i].cls, kControls[i].text,
                                       WS_CHILD | WS_VISIBLE | kControls[i].style, 0, 0,
                                       0, 0, hwnd, (HMENU)(INT_PTR)kControls[i].id,
                                       cs->hInstance, NULL);
            if (i == 0 || i == 2)  // combo 下拉列表部分需要更高
                SendMessageW(g_ctl[i], CB_SETITEMHEIGHT, (WPARAM)-1, 20);
        }
        layout();
        return 0;
    }
    case WM_TIMER: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top = BAR_H;  // 只刷新预览区，不触碰工具栏子控件（消除按钮闪烁）
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
        rc.top = BAR_H;
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DEVICE_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageW(g_ctl[0], CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < g_app.deviceCount) {
                    g_app.current = sel;  // 先更新索引：fillFormatCombo 按新设备查能力
                    fillFormatCombo();
                    openDevice(sel);
                }
            }
            return 0;
        case IDC_REFRESH_BTN:
            closeDevice();
            refreshDeviceCombo();
            if (g_app.deviceCount > 0) {
                SendMessageW(g_ctl[0], CB_SETCURSEL, 0, 0);
                fillFormatCombo();
                openDevice(0);
            }
            return 0;
        case IDC_FORMAT_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageW(g_ctl[2], CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    g_app.formatSel = sel;
                    openDevice(g_app.current);
                }
            }
            return 0;
        case IDC_ROTATE_BTN:
            g_app.rotate = (g_app.rotate + 1) & 3;
            updateTransformButtons();
            return 0;
        case IDC_MIRROR_BTN:
            g_app.mirror = !g_app.mirror;
            updateTransformButtons();
            return 0;
        case IDC_FLIP_BTN:
            g_app.flip = !g_app.flip;
            updateTransformButtons();
            return 0;
        }
        break;
    case WM_APP_ERROR:
    case WM_APP_LOG: {  // 回调线程 PostMessage：刷新预览区（提示/日志）
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top = BAR_H;
        InvalidateRect(hwnd, &rc, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            PostQuitMessage(0);
            return 0;
        case VK_SPACE:
            g_app.paused = !g_app.paused;
            return 0;
        case 'N':
            if (g_app.deviceCount > 0) {
                fillFormatCombo();
                openDevice(g_app.current + 1);
                SendMessageW(g_ctl[0], CB_SETCURSEL, g_app.current, 0);
            }
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
        spdlog::info("SparkCapDemo start");
    } catch (...) {
        // 日志初始化失败（目录只读等）不阻断预览
    }

    Gdiplus::GdiplusStartupInput gdiplusInput = {0};
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);

    if (sparkcap_create_backend(&g_app.backend) != SPARKCAP_ERR_OK) {
        MessageBoxA(NULL, "sparkcap_create_backend failed", "SparkCapDemo", MB_ICONERROR);
        return 1;
    }
    InitializeCriticalSection(&g_logCs);
    // 日志回调：进程级全局路由（FFmpeg av_log WARN+ 等），任意时机可注册
    sparkcap_set_log_callback(NULL, onLogCb, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SparkCapDemo";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SparkCapDemo", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, NULL, NULL, inst,
                                NULL);
    g_hwnd = hwnd;  // 回调线程经此 PostMessage

    refreshDeviceCombo();
    if (g_app.deviceCount > 0) {
        SendMessageW(g_ctl[0], CB_SETCURSEL, 0, 0);
        fillFormatCombo();
        openDevice(0);
    }
    updateTransformButtons();

    SetTimer(hwnd, 1, 33, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    closeDevice();
    sparkcap_set_log_callback(NULL, NULL, NULL);  // 注销后再销毁（回调不再进入）
    spdlog::info("SparkCapDemo exit");
    sparkcap_destroy_backend(g_app.backend);
    DeleteCriticalSection(&g_logCs);
    free(g_app.rgb);
    free(g_app.tRgb);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
