// SparkCapDemo —— SparkCap 接入示例：摄像头预览调试工具（Win32 GDI，零依赖）。
// 布局：顶部工具栏（设备/刷新/分辨率/变换）+ 预览区（letterbox + 变换渲染）。
// 按键：ESC 退出 | 空格 暂停 | N 下一台设备

#include <sparkcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// GDI+（系统组件）解 MJPG 帧
#include <gdiplus.h>
#include <shlwapi.h>

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
    {IDC_FORMAT_COMBO, 160, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL},
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

static void closeDevice(void)
{
    if (g_app.dev != NULL) {
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
        g_app.dev = NULL;
        return -1;
    }
    int w = 0, h = 0;
    sparkcap_pixfmt_t fmtPref = SPARKCAP_PIXFMT_UNKNOWN;
    if (g_app.formatSel > 0 && g_app.formatSel <= g_app.formatCount) {
        w = g_app.formats[g_app.formatSel - 1].width;
        h = g_app.formats[g_app.formatSel - 1].height;
    } else if (g_app.formatCount > 0) {
        // Default：MJPG 优先（USB 摄像头行业默认，Windows 相机应用同策略；
        // GDI+ 逐帧解码）+ 带宽最小（w*h*fps 最小）——高分辨率未压缩格式
        // 在 USB 2.0 上会因带宽不足丢包或完全无数据
        long long bestCost = 0;
        int bestIdx = -1;
        for (int pass = 0; pass < 2 && bestIdx < 0; ++pass) {
            const bool wantMjpg = (pass == 0);  // 第一轮 MJPG，第二轮非 MJPG
            for (int i = 0; i < g_app.formatCount; ++i) {
                const bool isMjpg = g_app.formats[i].pixelFormat == SPARKCAP_PIXFMT_MJPG;
                if (isMjpg != wantMjpg)
                    continue;
                const long long cost = (long long)g_app.formats[i].width *
                                       g_app.formats[i].height * g_app.formats[i].fps;
                if (bestIdx < 0 || cost < bestCost) {
                    bestCost = cost;
                    bestIdx = i;
                }
            }
        }
        if (bestIdx >= 0) {
            w = g_app.formats[bestIdx].width;
            h = g_app.formats[bestIdx].height;
            fmtPref = g_app.formats[bestIdx].pixelFormat;
        }
    }
    if (sparkcap_start(g_app.dev, w, h, fmtPref, 0) != SPARKCAP_ERR_OK) {
        sparkcap_close(g_app.dev);
        g_app.dev = NULL;
        return -1;
    }
    g_app.paused = 0;
    return 0;
}

static void refreshDeviceCombo(void)
{
    int count = (int)(sizeof(g_app.devices) / sizeof(g_app.devices[0]));
    if (sparkcap_list_devices(g_app.backend, g_app.devices, &count) != SPARKCAP_ERR_OK)
        count = 0;
    g_app.deviceCount = count;

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
        char text[96];
        sprintf_s(text, sizeof(text), "%dx%d @%d", g_app.formats[i].width,
                  g_app.formats[i].height, g_app.formats[i].fps);
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
    if (!g_app.paused && g_app.dev != NULL)
    {
        const int grc = sparkcap_get_frame(g_app.dev, &f, 200);
        lastError = grc;
        if (grc == SPARKCAP_ERR_OK && f != NULL) {
            lastPixelFormat = (int)f->pixelFormat;
            lastStride = f->stride;
            haveFrame = (frameToRgb32(f) == 0);
            ++g_app.framesThisSec;
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

    Gdiplus::GdiplusStartupInput gdiplusInput = {0};
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);

    if (sparkcap_create_backend(&g_app.backend) != SPARKCAP_ERR_OK) {
        MessageBoxA(NULL, "sparkcap_create_backend failed", "SparkCapDemo", MB_ICONERROR);
        return 1;
    }

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
    sparkcap_destroy_backend(g_app.backend);
    free(g_app.rgb);
    free(g_app.tRgb);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
