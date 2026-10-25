// -*- coding: utf-8 -*-
// 作者：lanzongnb666
// ---------------------------------------------------------------------------
// 坐标校准工具（液态玻璃风 GUI，C++ / Win32 + GDI+，无第三方依赖）
//
// UI 参照同目录的「自动刷屏液态玻璃版.py」：
//   - 背景：bg.png 拉伸 + 模糊 + 原图居中铺一层（找不到就用暗色渐变球）
//   - 面板：圆角 + 顶部高光 + 渐变描边，半透明压暗
//   - 图标：icon.ico；底部展示 reward.png，点击放大
//
// 功能：
//   依次记录【数字 1..9、0、确定、清零、颜色监视点、失败弹窗关闭】
//   的屏幕坐标，写进 exe 同目录的 coords.json（和 穷举密码.exe 完全兼容）。
//   还能点【测试】把鼠标依次移到每个已记录的点，肉眼核对。
//
// 编译：
//   g++ -O2 -std=c++17 -mwindows -static -o 校准坐标.exe 校准坐标.cpp -luser32 -lgdi32 -lgdiplus
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Gdiplus;

// 链接 gdiplus 由编译命令行的 -lgdiplus 负责（MinGW 不支持 #pragma comment）

// ============================ 布局常量 ============================

#define WIN_W        640
#define WIN_H        800
#define MARGIN       18
#define PANEL_X      MARGIN
#define PANEL_Y      MARGIN
#define PANEL_W      (WIN_W - MARGIN * 2)
#define PANEL_H      (WIN_H - MARGIN * 2)
#define PANEL_RADIUS 26.0f

#define TITLE_H      56
#define ROW_H        30
#define ROW_TOP      116
#define ROW_COUNT    13
#define ROW_X        32
#define ROW_W        (WIN_W - 64)

#define BTN_Y        596
#define BTN_H        44
#define BTN_W        112
#define BTN1_X       24
#define BTN2_X       (BTN1_X + BTN_W + 8)
#define BTN3_X       (BTN2_X + BTN_W + 8)
#define BTN4_X       (BTN3_X + BTN_W + 8)
#define BTN5_X       (BTN4_X + BTN_W + 8)

#define REWARD_X     270
#define REWARD_Y     662
#define REWARD_SIZE  96

#define TMR_TICK     1

// ============================ 目标定义 ============================

enum {
    T_KEY1 = 0, T_KEY2, T_KEY3, T_KEY4, T_KEY5, T_KEY6, T_KEY7, T_KEY8, T_KEY9,
    T_KEY0, T_SUBMIT, T_CLEAR, T_WATCH, T_FAILCLOSE,
    T_COUNT
};

static const wchar_t* g_targetName[T_COUNT] = {
    L"数字 1", L"数字 2", L"数字 3", L"数字 4", L"数字 5",
    L"数字 6", L"数字 7", L"数字 8", L"数字 9", L"数字 0",
    L"确定按钮", L"清零按钮", L"颜色监视点", L"失败弹窗关闭（可跳过）"
};

static const int g_digitOf[T_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 0, -1, -2, -3, -4
};

// ============================ 状态 ============================

struct State {
    POINT    pt[T_COUNT] = {};
    bool     has[T_COUNT] = {};
    COLORREF col[T_COUNT] = {};
    bool     hasCol[T_COUNT] = {};

    int      cur = -1;           // 正在抓取的目标下标
    DWORD    deadline = 0;       // 倒计时结束时刻
    bool     capturing = false;

    bool     testing = false;
    int      testIdx = 0;
    DWORD    testTick = 0;

    int      hover = -1;         // 0=开始 1=测试 2=保存 3=关闭 4=reward 5=行
    int      hoverRow = -1;
    POINT    mouse = {};

    std::wstring hint = L"点击【开始校准】，然后按提示移动鼠标";
    bool     saved = false;
    bool     dirty = false;
};

// 速度参数：一轮 = 6 次点击（4 个数字 + 确定 + 清零）
// 单轮总时长 = 6*(holdMs + gapMs) + submitMs + clearMs
struct SpeedCfg {
    int holdMs   = 15;      // 每次点击按住时长
    int gapMs    = 20;      // 两次点击之间的间隔
    int submitMs = 350;     // 点确定后等待判定（固定）
    int clearMs  = 30;      // 点清零后等待（固定）
};
static SpeedCfg g_speed;

static int SpeedRoundMs() {
    return 6 * (g_speed.holdMs + g_speed.gapMs) + g_speed.submitMs + g_speed.clearMs;
}

static State     g_st;
static HWND      g_hwnd = NULL;
static HINSTANCE g_inst = NULL;

static Bitmap*   g_bg = NULL;       // 合成好的背景（模糊 + 居中）
static Bitmap*   g_reward = NULL;   // reward.png
static HICON     g_icon = NULL;
static ULONG_PTR g_gdiToken = 0;
static std::wstring g_exeDir;

// ============================ 小工具 ============================

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n == 0) return L".";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"." : p.substr(0, s);
}

static std::wstring JoinPath(const std::wstring& a, const wchar_t* b) {
    if (a.empty()) return b;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int ClampI(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static COLORREF ScreenColor(int x, int y) {
    HDC dc = GetDC(NULL);
    if (!dc) return CLR_INVALID;
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(NULL, dc);
    return c;
}

static void RectToRectF(const RECT& r, RectF& out) {
    out = RectF((REAL)r.left, (REAL)r.top,
                (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
}

static void MakeRoundRect(RectF rc, float radius, GraphicsPath& path) {
    path.Reset();
    float d = radius * 2.0f;
    path.AddArc(rc.X, rc.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rc.GetRight() - d, rc.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rc.X, rc.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// ============================ 图片 ============================

static Bitmap* LoadPng(const std::wstring& path) {
    if (!FileExists(path)) return NULL;
    Bitmap* bmp = new Bitmap(path.c_str());
    if (!bmp || bmp->GetLastStatus() != Ok) {
        delete bmp;
        return NULL;
    }
    if (bmp->GetWidth() == 0 || bmp->GetHeight() == 0) {
        delete bmp;
        return NULL;
    }
    return bmp;
}

// 简易高斯：多重盒式模糊
static void BoxBlur(Bitmap* bmp, int radius, int passes) {
    if (!bmp || radius <= 0) return;
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();
    if (w <= 0 || h <= 0) return;

    Rect rc(0, 0, w, h);
    BitmapData bd;
    if (bmp->LockBits(&rc, ImageLockModeRead | ImageLockModeWrite,
                      PixelFormat32bppARGB, &bd) != Ok) {
        return;
    }
    int bpl = bd.Stride;
    BYTE* base = (BYTE*)bd.Scan0;
    std::vector<BYTE> tmp((size_t)bpl * h);

    for (int p = 0; p < passes; ++p) {
        // 横向
        for (int y = 0; y < h; ++y) {
            BYTE* row = base + (size_t)y * bpl;
            BYTE* dst = &tmp[(size_t)y * bpl];
            for (int x = 0; x < w; ++x) {
                int sum[4] = {0, 0, 0, 0};
                int cnt = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int xx = x + k;
                    if (xx < 0) xx = 0;
                    if (xx >= w) xx = w - 1;
                    BYTE* s = row + xx * 4;
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
                    ++cnt;
                }
                BYTE* d = dst + x * 4;
                d[0] = (BYTE)(sum[0] / cnt);
                d[1] = (BYTE)(sum[1] / cnt);
                d[2] = (BYTE)(sum[2] / cnt);
                d[3] = (BYTE)(sum[3] / cnt);
            }
        }
        // 纵向：tmp -> base
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                int sum[4] = {0, 0, 0, 0};
                int cnt = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int yy = y + k;
                    if (yy < 0) yy = 0;
                    if (yy >= h) yy = h - 1;
                    BYTE* s = &tmp[(size_t)yy * bpl + x * 4];
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
                    ++cnt;
                }
                BYTE* d = base + (size_t)y * bpl + x * 4;
                d[0] = (BYTE)(sum[0] / cnt);
                d[1] = (BYTE)(sum[1] / cnt);
                d[2] = (BYTE)(sum[2] / cnt);
                d[3] = (BYTE)(sum[3] / cnt);
            }
        }
    }
    bmp->UnlockBits(&bd);
}

// 让一块区域（清晰层）的边缘渐隐，避免和模糊背景之间出现硬边
static void FadeRectEdges(Bitmap* bmp, float l, float t, float w, float h, float fade) {
    if (!bmp || fade <= 1.0f) return;
    int W = (int)bmp->GetWidth();
    int H = (int)bmp->GetHeight();
    if (W <= 0 || H <= 0) return;
    Rect rc(0, 0, W, H);
    BitmapData bd;
    if (bmp->LockBits(&rc, ImageLockModeRead | ImageLockModeWrite,
                      PixelFormat32bppARGB, &bd) != Ok) {
        return;
    }
    BYTE* base = (BYTE*)bd.Scan0;
    int stride = bd.Stride;
    for (int y = 0; y < H; ++y) {
        BYTE* row = base + (size_t)y * stride;
        for (int x = 0; x < W; ++x) {
            float dx = x - l;
            float dy = y - t;
            float dr = (l + w) - x;
            float db = (t + h) - y;
            float d = std::min(std::min(dx, dy), std::min(dr, db));
            if (d <= 0) {
                row[x * 4 + 3] = 0;
                continue;
            }
            float f = d / fade;
            if (f >= 1.0f) continue;
            row[x * 4 + 3] = (BYTE)(row[x * 4 + 3] * f);
        }
    }
    bmp->UnlockBits(&bd);
}

// 没背景图时的暗色渐变球
static Bitmap* MakeGradientBg(int w, int h) {
    Bitmap* bmp = new Bitmap(w, h, PixelFormat32bppARGB);
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush back(Color(255, 22, 24, 34));
    g.FillRectangle(&back, 0, 0, w, h);

    struct Blob { float cx, cy, r; BYTE cr, cg, cb; };
    Blob blobs[3] = {
        { w * 0.22f, h * 0.25f, w * 0.70f, 70, 100, 170 },
        { w * 0.85f, h * 0.55f, w * 0.60f, 120, 70, 160 },
        { w * 0.45f, h * 0.95f, w * 0.75f, 50, 110, 140 },
    };
    for (int i = 0; i < 3; ++i) {
        GraphicsPath path;
        path.AddEllipse(blobs[i].cx - blobs[i].r, blobs[i].cy - blobs[i].r,
                        blobs[i].r * 2, blobs[i].r * 2);
        PathGradientBrush pgb(&path);
        pgb.SetCenterPoint(PointF(blobs[i].cx, blobs[i].cy));
        Color center(200, blobs[i].cr, blobs[i].cg, blobs[i].cb);
        Color edge(0, blobs[i].cr, blobs[i].cg, blobs[i].cb);
        pgb.SetCenterColor(center);
        int cnt = 1;
        pgb.SetSurroundColors(&edge, &cnt);
        g.FillPath(&pgb, &path);
    }
    return bmp;
}

// 合成背景：拉伸模糊 + 居中清晰
static Bitmap* BuildBackground(int w, int h) {
    // 背景优先用 bg1.png（用户指定），没有就退回 bg.png
    Bitmap* src = LoadPng(JoinPath(g_exeDir, L"bg1.png"));
    if (!src) src = LoadPng(JoinPath(g_exeDir, L"bg.png"));
    if (!src) return MakeGradientBg(w, h);

    Bitmap* out = new Bitmap(w, h, PixelFormat32bppARGB);
    Graphics g(out);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetCompositingQuality(CompositingQualityHighQuality);

    // 1) 拉伸铺满（模糊底）
    g.DrawImage(src, 0, 0, (int)w, (int)h);
    g.Flush();
    BoxBlur(out, 6, 3);

    // 2) 原图按比例居中叠一层清晰版（边缘羽化，避免和模糊层之间出现硬边）
    REAL sw = (REAL)src->GetWidth();
    REAL sh = (REAL)src->GetHeight();
    REAL scale = std::min((REAL)w / sw, (REAL)h / sh);
    REAL dw = sw * scale;
    REAL dh = sh * scale;
    REAL px = (REAL)((w - dw) / 2.0f);
    REAL py = (REAL)((h - dh) / 2.0f);

    Bitmap* clear = new Bitmap(w, h, PixelFormat32bppARGB);
    {
        Graphics g2(clear);
        g2.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g2.SetCompositingQuality(CompositingQualityHighQuality);
        g2.DrawImage(src, px, py, dw, dh);
        g2.Flush();
        FadeRectEdges(clear, px, py, dw, dh, 110.0f);
    }
    g.DrawImage(clear, 0, 0, w, h);
    delete clear;

    delete src;
    return out;
}

// ============================ 布局 ============================

static RECT BtnRect(int id) {
    int x = BTN1_X;
    if (id == 1) x = BTN2_X;
    if (id == 2) x = BTN3_X;
    if (id == 3) x = BTN4_X;
    if (id == 4) x = BTN5_X;
    RECT r = { x, BTN_Y, x + BTN_W, BTN_Y + BTN_H };
    return r;
}

static RECT RowRect(int i) {
    RECT r = { ROW_X, ROW_TOP + i * ROW_H, ROW_X + ROW_W, ROW_TOP + (i + 1) * ROW_H };
    return r;
}

static RECT RewardRect() {
    RECT r = { REWARD_X, REWARD_Y, REWARD_X + REWARD_SIZE, REWARD_Y + REWARD_SIZE };
    return r;
}

static RECT CloseRect() {
    RECT r = { WIN_W - MARGIN - 44, MARGIN + 12, WIN_W - MARGIN - 12, MARGIN + 44 };
    return r;
}

static bool PtInRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ============================ 绘制 ============================

static void DrawGlassPanel(Graphics& g, RectF rc, float radius, BYTE tintTop, BYTE tintBot) {
    GraphicsPath path;
    MakeRoundRect(rc, radius, path);

    // 压暗 + 微白染色
    LinearGradientBrush tint(
        PointF(rc.X, rc.Y), PointF(rc.X, rc.Y + rc.Height),
        Color(tintTop, 18, 20, 28), Color(tintBot, 18, 20, 28));
    g.FillPath(&tint, &path);

    LinearGradientBrush tint2(
        PointF(rc.X, rc.Y), PointF(rc.X, rc.Y + rc.Height),
        Color(18, 255, 255, 255), Color(5, 255, 255, 255));
    g.FillPath(&tint2, &path);

    // 顶部高光带
    {
        GraphicsPath clip;
        MakeRoundRect(rc, radius, clip);
        g.SetClip(&clip);
        float hl = std::min(rc.Height * 0.5f, 26.0f);
        LinearGradientBrush hlBrush(
            PointF(rc.X, rc.Y), PointF(rc.X, rc.Y + hl),
            Color(70, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillRectangle(&hlBrush, rc.X, rc.Y, rc.Width, hl);
        g.ResetClip();
    }

    // 渐变描边：顶部亮 → 底部暗
    LinearGradientBrush penGrad(
        PointF(rc.X, rc.Y), PointF(rc.X, rc.Y + rc.Height),
        Color(230, 255, 255, 255), Color(16, 255, 255, 255));
    Pen pen(&penGrad, 1.4f);
    g.DrawPath(&pen, &path);
}

static void DrawTextGlass(Graphics& g, const wchar_t* text, RectF rc,
                          float px, StringAlignment align, BYTE alpha,
                          bool bold) {
    FontFamily fam(L"Microsoft YaHei UI");
    INT style = bold ? FontStyleBold : FontStyleRegular;
    Font font(&fam, px, style, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    SolidBrush shadow(Color(225, 0, 0, 0));
    RectF rcShadow(rc.X, rc.Y + 1.0f, rc.Width, rc.Height);
    g.DrawString(text, -1, &font, rcShadow, &fmt, &shadow);
    SolidBrush brush(Color(alpha, 255, 255, 255));
    g.DrawString(text, -1, &font, rc, &fmt, &brush);
}

static void DrawButton(Graphics& g, const RECT& r, const wchar_t* label,
                       int btnId, bool enabled) {
    RectF rc;
    RectToRectF(r, rc);
    bool hot = (g_st.hover == btnId) && enabled;
    GraphicsPath path;
    MakeRoundRect(rc, rc.Height / 2.0f, path);

    LinearGradientBrush fill(
        PointF(rc.X, rc.Y), PointF(rc.X, rc.Y + rc.Height),
        Color(hot ? 110 : 72, 24, 30, 46),
        Color(hot ? 70 : 40, 12, 14, 24));
    g.FillPath(&fill, &path);

    LinearGradientBrush penGrad(
        PointF(rc.X, rc.Y), PointF(rc.X, rc.Y + rc.Height),
        Color(enabled ? 235 : 90, 255, 255, 255),
        Color(enabled ? 30 : 20, 255, 255, 255));
    Pen pen(&penGrad, 1.2f);
    g.DrawPath(&pen, &path);

    DrawTextGlass(g, label, rc, 15.0f, StringAlignmentCenter,
                  enabled ? 250 : 110, true);
}

static void Paint(HDC hdc) {
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetCompositingQuality(CompositingQualityHighQuality);

    // 背景
    if (g_bg) {
        g.DrawImage(g_bg, 0, 0, WIN_W, WIN_H);
    } else {
        g.Clear(Color(255, 18, 20, 28));
    }
    // 整体压暗，保证白字可读
    {
        SolidBrush dim(Color(92, 6, 8, 14));
        g.FillRectangle(&dim, 0, 0, WIN_W, WIN_H);
    }

    // 主面板
    RectF panel((REAL)PANEL_X, (REAL)PANEL_Y, (REAL)PANEL_W, (REAL)PANEL_H);
    DrawGlassPanel(g, panel, PANEL_RADIUS, 160, 120);

    // 标题
    {
        RectF rcTitle((REAL)(MARGIN + 18), (REAL)(MARGIN + 6),
                      (REAL)(PANEL_W - 120), 34.0f);
        DrawTextGlass(g, L"坐标校准 · lanzongnb666", rcTitle, 19.0f,
                      StringAlignmentNear, 245, true);
        // 小副标题
        RectF rcSub((REAL)(MARGIN + 18), (REAL)(MARGIN + 42),
                    (REAL)(PANEL_W - 140), 20.0f);
        DrawTextGlass(g, L"点【开始校准】后，把鼠标移到提示的按钮上，3 秒自动记录",
                      rcSub, 12.5f, StringAlignmentNear, 150, false);
    }

    // 关闭按钮
    {
        RECT rc = CloseRect();
        RectF rf;
        RectToRectF(rc, rf);
        bool hot = (g_st.hover == 10);
        GraphicsPath path;
        MakeRoundRect(rf, 9.0f, path);
        SolidBrush fill(Color(static_cast<BYTE>(hot ? 110 : 40), 255, 120, 120));
        g.FillPath(&fill, &path);
        DrawTextGlass(g, L"✕", rf, 14.0f, StringAlignmentCenter, 230, false);
    }

    // 状态行
    {
        RectF rcStatus((REAL)(MARGIN + 18), (REAL)ROW_TOP - 34,
                       (REAL)(PANEL_W - 36), 26.0f);
        int a = g_st.capturing ? 255 : 200;
        DrawTextGlass(g, g_st.hint.c_str(), rcStatus, 13.5f,
                      StringAlignmentNear, (BYTE)a, true);
    }

    // 列表
    for (int i = 0; i < T_COUNT; ++i) {
        RECT r = RowRect(i);
        RectF rc;
        RectToRectF(r, rc);
        bool isCur = g_st.capturing && g_st.cur == i;
        bool isRow = (g_st.hoverRow == i);

        GraphicsPath path;
        MakeRoundRect(rc, 10.0f, path);
        Color fillC;
        if (isCur)      fillC = Color(120, 90, 150, 255);
        else if (isRow) fillC = Color(70, 30, 36, 52);
        else            fillC = Color(48, 10, 12, 20);
        SolidBrush fill(fillC);
        g.FillPath(&fill, &path);

        if (isCur) {
            Pen pen(Color(200, 150, 200, 255), 1.2f);
            g.DrawPath(&pen, &path);
        }

        RectF rcName(rc.X + 14, rc.Y, rc.Width * 0.46f, rc.Height);
        DrawTextGlass(g, g_targetName[i], rcName, 14.0f,
                      StringAlignmentNear, isCur ? 255 : 225, false);

        std::wstring val;
        if (g_st.has[i]) {
            wchar_t buf[96];
            if (i == T_WATCH && g_st.hasCol[i] && g_st.col[i] != CLR_INVALID) {
                swprintf(buf, 96, L"(%ld, %ld)  #%02X%02X%02X",
                         g_st.pt[i].x, g_st.pt[i].y,
                         (unsigned)GetRValue(g_st.col[i]),
                         (unsigned)GetGValue(g_st.col[i]),
                         (unsigned)GetBValue(g_st.col[i]));
            } else {
                swprintf(buf, 96, L"(%ld, %ld)", g_st.pt[i].x, g_st.pt[i].y);
            }
            val = buf;
        } else {
            val = L"未记录";
        }
        RectF rcVal(rc.X + rc.Width * 0.46f, rc.Y, rc.Width * 0.5f, rc.Height);
        DrawTextGlass(g, val.c_str(), rcVal, 13.5f, StringAlignmentFar,
                      g_st.has[i] ? 230 : 110, false);

        // 右侧圆点状态
        RectF dot(rc.GetRight() - 16, rc.Y + rc.Height / 2 - 3, 6, 6);
        SolidBrush dotBrush(g_st.has[i] ? Color(255, 90, 220, 140)
                                        : Color(120, 200, 200, 200));
        g.FillEllipse(&dotBrush, dot);
    }

    // 倒计时进度条
    if (g_st.capturing) {
        DWORD now = GetTickCount();
        float left = (float)((int)(g_st.deadline - now) / 1000.0f);
        float frac = left / 3.0f;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        RectF track((REAL)(BTN1_X), (REAL)(BTN_Y - 26), (REAL)(WIN_W - 64), 8.0f);
        GraphicsPath trackPath;
        MakeRoundRect(track, 4.0f, trackPath);
        SolidBrush trackFill(Color(50, 255, 255, 255));
        g.FillPath(&trackFill, &trackPath);

        RectF bar(track.X, track.Y, track.Width * frac, track.Height);
        GraphicsPath barPath;
        MakeRoundRect(bar, 4.0f, barPath);
        LinearGradientBrush barFill(
            PointF(bar.X, bar.Y), PointF(bar.X + bar.Width, bar.Y),
            Color(255, 110, 170, 255), Color(255, 190, 140, 255));
        g.FillPath(&barFill, &barPath);
    }

    // 按钮
    DrawButton(g, BtnRect(0), g_st.capturing ? L"取消校准" : L"开始校准", 0, true);
    DrawButton(g, BtnRect(1), L"测试移动", 1, !g_st.capturing);
    DrawButton(g, BtnRect(2), L"保存配置", 2, true);
    DrawButton(g, BtnRect(3), L"速度设置", 3, !g_st.capturing);
    DrawButton(g, BtnRect(4), L"开始穷举", 4, !g_st.capturing);

    // reward 图
    {
        RECT rr = RewardRect();
        RectF rcf;
        RectToRectF(rr, rcf);
        bool hot = (g_st.hover == 5);
        if (g_reward) {
            GraphicsPath clip;
            MakeRoundRect(rcf, 18.0f, clip);
            g.SetClip(&clip);
            g.DrawImage(g_reward, rcf);
            g.ResetClip();
            GraphicsPath border;
            MakeRoundRect(rcf, 18.0f, border);
            Pen pen(Color(hot ? 220 : 90, 255, 255, 255), 1.2f);
            g.DrawPath(&pen, &border);
        } else {
            SolidBrush ph(Color(40, 255, 255, 255));
            g.FillRectangle(&ph, rcf);
            DrawTextGlass(g, L"reward.png", rcf, 13.0f,
                          StringAlignmentCenter, 120, false);
        }

        RectF rcTip((REAL)(REWARD_X - 60), (REAL)(REWARD_Y + REWARD_SIZE + 6),
                    (REAL)(REWARD_SIZE + 120), 22.0f);
        DrawTextGlass(g, L"扫码赞赏支持", rcTip, 13.5f,
                      StringAlignmentCenter, 200, true);
    }
}

// ============================ 逻辑 ============================

static void UpdateHover() {
    GetCursorPos(&g_st.mouse);
    POINT p = g_st.mouse;
    ScreenToClient(g_hwnd, &p);

    int oldHover = g_st.hover;
    int oldRow = g_st.hoverRow;
    g_st.hover = -1;
    g_st.hoverRow = -1;

    if (PtInRect(CloseRect(), p.x, p.y)) g_st.hover = 10;
    else if (PtInRect(BtnRect(0), p.x, p.y)) g_st.hover = 0;
    else if (PtInRect(BtnRect(1), p.x, p.y)) g_st.hover = 1;
    else if (PtInRect(BtnRect(2), p.x, p.y)) g_st.hover = 2;
    else if (PtInRect(BtnRect(3), p.x, p.y)) g_st.hover = 3;
    else if (PtInRect(BtnRect(4), p.x, p.y)) g_st.hover = 4;
    else if (PtInRect(RewardRect(), p.x, p.y)) g_st.hover = 5;
    else {
        for (int i = 0; i < T_COUNT; ++i) {
            if (PtInRect(RowRect(i), p.x, p.y)) { g_st.hoverRow = i; break; }
        }
    }

    if (oldHover != g_st.hover || oldRow != g_st.hoverRow) {
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void StartCapture(int idx) {
    if (idx < 0 || idx >= T_COUNT) return;
    g_st.cur = idx;
    g_st.capturing = true;
    g_st.deadline = GetTickCount() + 3000;
    g_st.hint = std::wstring(L"请把鼠标移到【") + g_targetName[idx] +
                L"】，3 秒后自动记录";
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void StartAll() {
    int first = -1;
    for (int i = 0; i < T_COUNT; ++i) {
        if (!g_st.has[i]) { first = i; break; }
    }
    if (first < 0) { g_st.hint = L"全部已记录，可点【测试移动】核对后保存"; return; }
    StartCapture(first);
}

static void CaptureDone() {
    int i = g_st.cur;
    if (i < 0) return;
    POINT p;
    GetCursorPos(&p);
    g_st.pt[i] = p;
    g_st.has[i] = true;
    if (i == T_WATCH) {
        g_st.col[i] = ScreenColor((int)p.x, (int)p.y);
        g_st.hasCol[i] = true;
    }
    g_st.capturing = false;
    g_st.cur = -1;

    // 找下一个未记录的
    int next = -1;
    for (int k = i + 1; k < T_COUNT; ++k) {
        if (!g_st.has[k]) { next = k; break; }
    }
    if (next >= 0) {
        StartCapture(next);
    } else {
        g_st.hint = L"全部记录完成，点【保存配置】写进 coords.json";
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void StartTest() {
    int first = -1;
    for (int i = 0; i < T_COUNT; ++i) {
        if (g_st.has[i]) { first = i; break; }
    }
    if (first < 0) { g_st.hint = L"还没有记录任何坐标"; return; }
    g_st.testing = true;
    g_st.testIdx = first;
    g_st.testTick = GetTickCount();
    SetCursorPos((int)g_st.pt[first].x, (int)g_st.pt[first].y);
    g_st.hint = std::wstring(L"测试中：") + g_targetName[first];
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void TestStep() {
    if (!g_st.testing) return;
    DWORD now = GetTickCount();
    if (now - g_st.testTick < 700) return;
    g_st.testTick = now;
    ++g_st.testIdx;
    while (g_st.testIdx < T_COUNT && !g_st.has[g_st.testIdx]) ++g_st.testIdx;
    if (g_st.testIdx >= T_COUNT) {
        g_st.testing = false;
        g_st.hint = L"测试结束，鼠标回到原位即可";
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    SetCursorPos((int)g_st.pt[g_st.testIdx].x, (int)g_st.pt[g_st.testIdx].y);
    g_st.hint = std::wstring(L"测试中：") + g_targetName[g_st.testIdx];
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ============================ 保存 ============================

static void SaveCoords() {
    std::wstring path = JoinPath(g_exeDir, L"coords.json");

    int keyX[10], keyY[10];
    for (int d = 0; d <= 9; ++d) {
        int idx = (d == 0) ? T_KEY0 : (T_KEY1 + d - 1);
        keyX[d] = g_st.has[idx] ? (int)g_st.pt[idx].x : 0;
        keyY[d] = g_st.has[idx] ? (int)g_st.pt[idx].y : 0;
    }

    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) {
        MessageBoxW(g_hwnd, L"无法写入 coords.json", L"错误", MB_ICONERROR);
        return;
    }
    fprintf(f, "{\n  \"keys\": {\n");
    for (int d = 0; d <= 9; ++d) {
        fprintf(f, "    \"%d\": [%d, %d]%s\n", d, keyX[d], keyY[d], d == 9 ? "" : ",");
    }
    fprintf(f, "  },\n");

    if (g_st.has[T_SUBMIT])
        fprintf(f, "  \"submit\": [%ld, %ld],\n", g_st.pt[T_SUBMIT].x, g_st.pt[T_SUBMIT].y);
    else
        fprintf(f, "  \"submit\": null,\n");

    if (g_st.has[T_CLEAR])
        fprintf(f, "  \"clear_point\": [%ld, %ld],\n", g_st.pt[T_CLEAR].x, g_st.pt[T_CLEAR].y);
    else
        fprintf(f, "  \"clear_point\": null,\n");

    if (g_st.has[T_WATCH])
        fprintf(f, "  \"watch_point\": [%ld, %ld],\n", g_st.pt[T_WATCH].x, g_st.pt[T_WATCH].y);
    else
        fprintf(f, "  \"watch_point\": null,\n");

    if (g_st.has[T_WATCH] && g_st.hasCol[T_WATCH] && g_st.col[T_WATCH] != CLR_INVALID)
        fprintf(f, "  \"base_color\": [%u, %u, %u],\n",
                (unsigned)GetRValue(g_st.col[T_WATCH]),
                (unsigned)GetGValue(g_st.col[T_WATCH]),
                (unsigned)GetBValue(g_st.col[T_WATCH]));
    else
        fprintf(f, "  \"base_color\": null,\n");

    fprintf(f, "  \"tolerance\": 30,\n");
    fprintf(f, "  \"stop_on_hit\": false,\n");
    fprintf(f, "  \"round_ms\": %d,\n", SpeedRoundMs());
    fprintf(f, "  \"hold_ms\": %d,\n", g_speed.holdMs);
    fprintf(f, "  \"gap_ms\": %d,\n", g_speed.gapMs);

    if (g_st.has[T_FAILCLOSE])
        fprintf(f, "  \"fail_close_point\": [%ld, %ld],\n",
                g_st.pt[T_FAILCLOSE].x, g_st.pt[T_FAILCLOSE].y);
    else
        fprintf(f, "  \"fail_close_point\": null,\n");

    fprintf(f, "  \"fail_close_key\": null\n}\n");
    fclose(f);

    g_st.saved = true;
    g_st.dirty = false;
    g_st.hint = std::wstring(L"已保存到 ") + path;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ============================ 读配置 / 穷举 ============================

static int KeyIdx(int d) { return (d == 0) ? T_KEY0 : (T_KEY1 + d - 1); }

static bool ReadTextFile2(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static const char* SkipToNum(const char* p) {
    while (*p && !isdigit((unsigned char)*p) && *p != '-' && *p != '+') ++p;
    return p;
}

static bool JsonGetPair(const std::string& j, const char* key, long& x, long& y) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    size_t b = j.find('[', pos + strlen(key));
    if (b == std::string::npos) return false;
    size_t e = j.find(']', b);
    if (e == std::string::npos) return false;
    std::string s = j.substr(b + 1, e - b - 1);
    const char* p = SkipToNum(s.c_str());
    if (!*p) return false;
    char* p1 = NULL;
    long a = strtol(p, &p1, 10);
    if (p1 == p) return false;
    p = SkipToNum(p1);
    if (!*p) return false;
    char* p2 = NULL;
    long c = strtol(p, &p2, 10);
    if (p2 == p) return false;
    x = a;
    y = c;
    return true;
}

static bool JsonGetTriple(const std::string& j, const char* key, int& r, int& g, int& b) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    size_t bs = j.find('[', pos + strlen(key));
    if (bs == std::string::npos) return false;
    size_t be = j.find(']', bs);
    if (be == std::string::npos) return false;
    std::string s = j.substr(bs + 1, be - bs - 1);
    const char* p = SkipToNum(s.c_str());
    if (!*p) return false;
    char* p1 = NULL;
    long v1 = strtol(p, &p1, 10);
    if (p1 == p) return false;
    p = SkipToNum(p1);
    if (!*p) return false;
    char* p2 = NULL;
    long v2 = strtol(p, &p2, 10);
    if (p2 == p) return false;
    p = SkipToNum(p2);
    if (!*p) return false;
    char* p3 = NULL;
    long v3 = strtol(p, &p3, 10);
    if (p3 == p) return false;
    r = (int)v1;
    g = (int)v2;
    b = (int)v3;
    return true;
}

static bool JsonGetInt(const std::string& j, const char* key, int& v) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    const char* p = SkipToNum(j.c_str() + pos + strlen(key));
    if (!*p) return false;
    char* e = NULL;
    long a = strtol(p, &e, 10);
    if (e == p) return false;
    v = (int)a;
    return true;
}

static int ColorDiff(COLORREF a, COLORREF b) {
    int dr = (int)GetRValue(a) - (int)GetRValue(b);
    int dg = (int)GetGValue(a) - (int)GetGValue(b);
    int db = (int)GetBValue(a) - (int)GetBValue(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    int m = dr > dg ? dr : dg;
    return m > db ? m : db;
}

static void LoadCoordsToUI() {
    std::string j;
    if (!ReadTextFile2(JoinPath(g_exeDir, L"coords.json"), j)) return;

    for (int d = 0; d <= 9; ++d) {
        char k[16];
        snprintf(k, sizeof(k), "\"%d\":", d);
        long x = 0, y = 0;
        if (JsonGetPair(j, k, x, y)) {
            int idx = KeyIdx(d);
            g_st.pt[idx].x = x;
            g_st.pt[idx].y = y;
            g_st.has[idx] = true;
        }
    }
    long x = 0, y = 0;
    if (JsonGetPair(j, "\"submit\"", x, y)) {
        g_st.pt[T_SUBMIT].x = x; g_st.pt[T_SUBMIT].y = y; g_st.has[T_SUBMIT] = true;
    }
    if (JsonGetPair(j, "\"clear_point\"", x, y)) {
        g_st.pt[T_CLEAR].x = x; g_st.pt[T_CLEAR].y = y; g_st.has[T_CLEAR] = true;
    }
    if (JsonGetPair(j, "\"watch_point\"", x, y)) {
        g_st.pt[T_WATCH].x = x; g_st.pt[T_WATCH].y = y; g_st.has[T_WATCH] = true;
    }
    if (JsonGetPair(j, "\"fail_close_point\"", x, y)) {
        g_st.pt[T_FAILCLOSE].x = x; g_st.pt[T_FAILCLOSE].y = y; g_st.has[T_FAILCLOSE] = true;
    }
    int r = 0, g = 0, b = 0;
    if (JsonGetTriple(j, "\"base_color\"", r, g, b)) {
        g_st.col[T_WATCH] = RGB(r, g, b);
        g_st.hasCol[T_WATCH] = true;
    }

    int sp = 0;
    if (JsonGetInt(j, "\"hold_ms\"", sp)) g_speed.holdMs = ClampI(sp, 5, 500);
    if (JsonGetInt(j, "\"gap_ms\"", sp)) g_speed.gapMs = ClampI(sp, 0, 2000);

    int rec = 0;
    for (int i = 0; i < T_COUNT; ++i) {
        if (g_st.has[i]) ++rec;
    }
    if (rec) {
        g_st.hint = std::wstring(L"已从 coords.json 载入 ") + std::to_wstring(rec) + L" 个坐标";
    }
}

static void DoClick(int x, int y) {
    SetCursorPos(x, y);
    Sleep(6);
    INPUT in[2];
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
    Sleep(g_speed.holdMs);
}

// ============================ 初始密码对话框 ============================

struct AskState {
    HWND edit;
    bool done;
    bool ok;
    int value;
};
static AskState g_ask;

static LRESULT CALLBACK AskProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1) {                       // 开始
            wchar_t buf[32] = {0};
            GetWindowTextW(g_ask.edit, buf, 32);
            std::wstring s(buf);
            size_t b = s.find_first_not_of(L" \t");
            size_t e = s.find_last_not_of(L" \t");
            if (b == std::wstring::npos) {
                s = L"";
            } else {
                s = s.substr(b, e - b + 1);
            }
            bool valid = true;
            if (s.empty()) {
                g_ask.value = 0;
            } else {
                if (s.size() > 4) valid = false;
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] < L'0' || s[i] > L'9') valid = false;
                }
                if (valid) g_ask.value = _wtoi(s.c_str());
            }
            if (!valid) {
                MessageBoxW(hwnd, L"请输入不超过 4 位的纯数字", L"提示", MB_ICONWARNING);
                return 0;
            }
            g_ask.ok = true;
            g_ask.done = true;
            DestroyWindow(hwnd);
        } else if (id == 2) {                // 取消
            g_ask.ok = false;
            g_ask.done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        g_ask.ok = false;
        g_ask.done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool AskStartCode(int& out) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AskProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AskStartWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    g_ask.ok = false;
    g_ask.done = false;
    g_ask.value = 0;

    int W = 380, H = 176;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AskStartWnd", L"初始密码",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, W, H, g_hwnd, NULL, hi, NULL);
    if (!dlg) return false;

    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    SetWindowPos(dlg, NULL, pr.left + ((pr.right - pr.left) - W) / 2,
                 pr.top + ((pr.bottom - pr.top) - H) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    CreateWindowExW(0, L"STATIC", L"从哪一组开始？直接回车 = 0000",
                    WS_CHILD | WS_VISIBLE, 20, 16, 340, 22, dlg, NULL, hi, NULL);
    g_ask.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0000",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                 20, 46, 340, 30, dlg, (HMENU)10, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"开始穷举",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    150, 100, 100, 34, dlg, (HMENU)1, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"取消",
                    WS_CHILD | WS_VISIBLE,
                    262, 100, 100, 34, dlg, (HMENU)2, hi, NULL);

    ShowWindow(dlg, SW_SHOW);
    SetFocus(g_ask.edit);
    SendMessageW(g_ask.edit, EM_SETSEL, 0, -1);
    EnableWindow(g_hwnd, FALSE);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (g_ask.ok) out = g_ask.value;
    return g_ask.ok;
}

static void RunBrute() {
    for (int d = 0; d <= 9; ++d) {
        if (!g_st.has[KeyIdx(d)]) {
            wchar_t m[128];
            swprintf(m, 128, L"数字键 %d 还没记录，先点【开始校准】。", d);
            MessageBoxW(g_hwnd, m, L"提示", MB_ICONINFORMATION);
            return;
        }
    }
    if (!g_st.has[T_SUBMIT]) {
        MessageBoxW(g_hwnd, L"还没记录【确定按钮】，先校准。", L"提示", MB_ICONINFORMATION);
        return;
    }
    if (!g_st.has[T_CLEAR]) {
        MessageBoxW(g_hwnd, L"还没记录【清零按钮】，先校准。", L"提示", MB_ICONINFORMATION);
        return;
    }

    COLORREF base = 0;
    bool hasBase = false;
    if (g_st.has[T_WATCH]) {
        if (g_st.hasCol[T_WATCH] && g_st.col[T_WATCH] != CLR_INVALID) {
            base = g_st.col[T_WATCH];
            hasBase = true;
        } else {
            base = ScreenColor((int)g_st.pt[T_WATCH].x, (int)g_st.pt[T_WATCH].y);
            hasBase = (base != CLR_INVALID);
        }
    }

    int confirm = MessageBoxW(g_hwnd,
        L"即将开始自动穷举（0000 ~ 9999）\n\n"
        L"· 点确定后窗口会隐藏，请在 3 秒内把目标密码界面切到前台\n"
        L"· 运行中按 ESC，或把鼠标甩到屏幕左上角，立即停止\n"
        L"· 日志写入 brute_log.txt，变色记录写入 hits.txt\n\n"
        L"开始吗？", L"开始穷举", MB_OKCANCEL | MB_ICONQUESTION);
    if (confirm != IDOK) return;

    int startIdx = 0;
    if (!AskStartCode(startIdx)) return;
    if (startIdx < 0) startIdx = 0;
    if (startIdx > 9999) startIdx = 9999;

    ShowWindow(g_hwnd, SW_HIDE);

    std::wstring logPath = JoinPath(g_exeDir, L"brute_log.txt");
    std::wstring hitPath = JoinPath(g_exeDir, L"hits.txt");
    FILE* log = _wfopen(logPath.c_str(), L"a");
    FILE* hits = _wfopen(hitPath.c_str(), L"a");

    if (log) {
        time_t now = time(NULL);
        struct tm* lt = localtime(&now);
        char st[64];
        strftime(st, sizeof(st), "%Y-%m-%d %H:%M:%S", lt);
        fprintf(log, "\n===== GUI 新一轮 %s =====\n", st);
        fflush(log);
    }

    int hitCount = 0;
    int done = startIdx;
    Sleep(3000);
    DWORD t0 = GetTickCount();

    for (int idx = startIdx; idx < 10000; ++idx) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;
        POINT mp;
        GetCursorPos(&mp);
        if (mp.x <= 3 && mp.y <= 3) break;

        char code[8];
        snprintf(code, sizeof(code), "%04d", idx);

        bool aborted = false;
        for (int i = 0; i < 4; ++i) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { aborted = true; break; }
            int d = code[i] - '0';
            int ti = KeyIdx(d);
            DoClick((int)g_st.pt[ti].x, (int)g_st.pt[ti].y);
            Sleep(g_speed.gapMs);
        }
        if (aborted) break;

        DoClick((int)g_st.pt[T_SUBMIT].x, (int)g_st.pt[T_SUBMIT].y);
        Sleep(g_speed.submitMs);

        if (hasBase && g_st.has[T_WATCH]) {
            COLORREF cur = ScreenColor((int)g_st.pt[T_WATCH].x, (int)g_st.pt[T_WATCH].y);
            if (cur != CLR_INVALID && ColorDiff(cur, base) > 30) {
                ++hitCount;
                if (hits) {
                    time_t now2 = time(NULL);
                    struct tm* lt2 = localtime(&now2);
                    char st2[64];
                    strftime(st2, sizeof(st2), "%Y-%m-%d %H:%M:%S", lt2);
                    fprintf(hits, "%s  组合=%s  #%02X%02X%02X\n", st2, code,
                            (unsigned)GetRValue(cur), (unsigned)GetGValue(cur),
                            (unsigned)GetBValue(cur));
                    fflush(hits);
                }
            }
        }

        DoClick((int)g_st.pt[T_CLEAR].x, (int)g_st.pt[T_CLEAR].y);
        Sleep(g_speed.clearMs);

        done = idx;
        if (log) {
            fprintf(log, "%04d  elapsed=%lus\n", idx,
                    (unsigned long)((GetTickCount() - t0) / 1000));
            fflush(log);
        }
    }

    if (log) fclose(log);
    if (hits) fclose(hits);

    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);

    wchar_t msg[512];
    swprintf(msg, 512,
        L"穷举结束\n\n最后尝试：%04d\n变色（可能正确）：%d 次\n\n"
        L"日志：brute_log.txt\n变色记录：hits.txt",
        done, hitCount);
    MessageBoxW(g_hwnd, msg, L"完成", MB_ICONINFORMATION);
    g_st.hint = L"穷举结束，可点【开始穷举】重来";
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ============================ 速度设置对话框 ============================

struct SpeedDlgState {
    HWND roundEdit;
    HWND holdEdit;
    HWND gapEdit;
    HWND advChk;
    bool updating;
};
static SpeedDlgState g_sd;

static int EditInt(HWND e) {
    wchar_t b[32] = {0};
    GetWindowTextW(e, b, 32);
    return _wtoi(b);
}

static void EditSetInt(HWND e, int v) {
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    SetWindowTextW(e, b);
}

// 由「点击时长 + 间隔」反推总时长
static void SpeedApplyFromHoldGap() {
    g_speed.holdMs = ClampI(g_speed.holdMs, 5, 500);
    g_speed.gapMs = ClampI(g_speed.gapMs, 0, 2000);
    int t = SpeedRoundMs();
    g_sd.updating = true;
    EditSetInt(g_sd.roundEdit, t);
    g_sd.updating = false;
}

// 由「单轮总时长」按当前比例拆出点击时长 + 间隔
static void SpeedApplyFromRound(int roundMs) {
    roundMs = ClampI(roundMs, 200, 60000);
    int clickPart = roundMs - g_speed.submitMs - g_speed.clearMs;
    int minPart = 6 * 5;
    if (clickPart < minPart) clickPart = minPart;
    int per = clickPart / 6;
    int total = g_speed.holdMs + g_speed.gapMs;
    int h = (total > 0) ? (per * g_speed.holdMs) / total : per / 2;
    if (h < 5) h = 5;
    int g = per - h;
    if (g < 0) g = 0;
    g_speed.holdMs = h;
    g_speed.gapMs = g;
    g_sd.updating = true;
    EditSetInt(g_sd.holdEdit, h);
    EditSetInt(g_sd.gapEdit, g);
    g_sd.updating = false;
}

static LRESULT CALLBACK SpeedProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == 1) {
            g_speed.holdMs = ClampI(EditInt(g_sd.holdEdit), 5, 500);
            g_speed.gapMs = ClampI(EditInt(g_sd.gapEdit), 0, 2000);
            SaveCoords();
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == 2) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == 20 && code == BN_CLICKED) {
            bool on = (SendMessageW(g_sd.advChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(g_sd.holdEdit, on);
            EnableWindow(g_sd.gapEdit, on);
            return 0;
        }
        if (g_sd.updating) return 0;
        if (id == 10 && code == EN_CHANGE) {
            SpeedApplyFromRound(EditInt(g_sd.roundEdit));
            return 0;
        }
        if ((id == 11 || id == 12) && code == EN_CHANGE) {
            g_speed.holdMs = EditInt(g_sd.holdEdit);
            g_speed.gapMs = EditInt(g_sd.gapEdit);
            SpeedApplyFromHoldGap();
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSpeedDlg() {
    HINSTANCE hi = GetModuleHandleW(NULL);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SpeedProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"SpeedDlgWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    g_sd.updating = true;

    int W = 460, H = 316;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SpeedDlgWnd", L"速度设置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, W, H, g_hwnd, NULL, hi, NULL);
    if (!dlg) return;

    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    SetWindowPos(dlg, NULL, pr.left + ((pr.right - pr.left) - W) / 2,
                 pr.top + ((pr.bottom - pr.top) - H) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    CreateWindowExW(0, L"STATIC", L"单轮总时长（毫秒）", WS_CHILD | WS_VISIBLE,
                    20, 18, 200, 22, dlg, NULL, hi, NULL);
    g_sd.roundEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0000",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                     230, 16, 90, 26, dlg, (HMENU)10, hi, NULL);
    CreateWindowExW(0, L"STATIC", L"越小越快", WS_CHILD | WS_VISIBLE,
                    330, 18, 100, 22, dlg, NULL, hi, NULL);

    g_sd.advChk = CreateWindowExW(0, L"BUTTON", L"高级选项：单独设置每次点击时长与间隔",
                                  WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  20, 58, 400, 24, dlg, (HMENU)20, hi, NULL);

    CreateWindowExW(0, L"STATIC", L"每次点击按住时长（毫秒）", WS_CHILD | WS_VISIBLE,
                    20, 96, 200, 22, dlg, NULL, hi, NULL);
    g_sd.holdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0000",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                    230, 94, 90, 26, dlg, (HMENU)11, hi, NULL);

    CreateWindowExW(0, L"STATIC", L"两次点击之间间隔（毫秒）", WS_CHILD | WS_VISIBLE,
                    20, 134, 200, 22, dlg, NULL, hi, NULL);
    g_sd.gapEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0000",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                   230, 132, 90, 26, dlg, (HMENU)12, hi, NULL);

    CreateWindowExW(0, L"STATIC",
                    L"判定等待 350ms 和清零等待 30ms 是固定值，不参与加速。",
                    WS_CHILD | WS_VISIBLE, 20, 176, 420, 22, dlg, NULL, hi, NULL);
    CreateWindowExW(0, L"STATIC",
                    L"改总时长会自动折算高级选项，改高级选项会自动更新总时长。",
                    WS_CHILD | WS_VISIBLE, 20, 200, 420, 22, dlg, NULL, hi, NULL);

    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    170, 255, 110, 34, dlg, (HMENU)1, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                    300, 255, 110, 34, dlg, (HMENU)2, hi, NULL);

    EditSetInt(g_sd.roundEdit, SpeedRoundMs());
    EditSetInt(g_sd.holdEdit, g_speed.holdMs);
    EditSetInt(g_sd.gapEdit, g_speed.gapMs);
    EnableWindow(g_sd.holdEdit, FALSE);
    EnableWindow(g_sd.gapEdit, FALSE);

    g_sd.updating = false;

    ShowWindow(dlg, SW_SHOW);
    SetFocus(g_sd.roundEdit);
    SendMessageW(g_sd.roundEdit, EM_SETSEL, 0, -1);
    EnableWindow(g_hwnd, FALSE);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ============================ 窗口过程 ============================

static void OnClick(int x, int y) {
    if (g_st.testing) { g_st.testing = false; InvalidateRect(g_hwnd, NULL, FALSE); }

    if (PtInRect(CloseRect(), x, y)) { DestroyWindow(g_hwnd); return; }

    if (PtInRect(BtnRect(0), x, y)) {
        if (g_st.capturing) {
            g_st.capturing = false;
            g_st.cur = -1;
            g_st.hint = L"已取消当前抓取";
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            StartAll();
        }
        return;
    }
    if (PtInRect(BtnRect(1), x, y)) { StartTest(); return; }
    if (PtInRect(BtnRect(2), x, y)) { SaveCoords(); return; }
    if (PtInRect(BtnRect(3), x, y)) { ShowSpeedDlg(); return; }
    if (PtInRect(BtnRect(4), x, y)) { RunBrute(); return; }

    if (PtInRect(RewardRect(), x, y)) {
        std::wstring p = JoinPath(g_exeDir, L"reward.png");
        if (FileExists(p)) {
            ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
        } else {
            MessageBoxW(g_hwnd, L"同目录下没有 reward.png", L"提示", MB_ICONINFORMATION);
        }
        return;
    }

    for (int i = 0; i < T_COUNT; ++i) {
        if (PtInRect(RowRect(i), x, y)) {
            StartCapture(i);
            return;
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hwnd, TMR_TICK, 50, NULL);
        return 0;
    }
    case WM_TIMER: {
        if (wp == TMR_TICK) {
            UpdateHover();
            if (g_st.capturing && GetTickCount() >= g_st.deadline) {
                CaptureDone();
            }
            if (g_st.capturing) InvalidateRect(hwnd, NULL, FALSE);
            if (g_st.testing) TestStep();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_KEYDOWN: {
        if (wp == VK_F2) {
            if (g_st.capturing) { g_st.capturing = false; g_st.cur = -1; }
            else StartAll();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == VK_ESCAPE) {
            if (g_st.capturing) {
                g_st.capturing = false;
                g_st.cur = -1;
                g_st.hint = L"已取消当前抓取";
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, WIN_W, WIN_H);
        HGDIOBJ old = SelectObject(mem, bmp);

        Paint(mem);

        BitBlt(hdc, 0, 0, WIN_W, WIN_H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit != HTCLIENT) return hit;
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        if (PtInRect(CloseRect(), p.x, p.y)) return HTCLIENT;
        if (PtInRect(BtnRect(0), p.x, p.y)) return HTCLIENT;
        if (PtInRect(BtnRect(1), p.x, p.y)) return HTCLIENT;
        if (PtInRect(BtnRect(2), p.x, p.y)) return HTCLIENT;
        if (PtInRect(BtnRect(3), p.x, p.y)) return HTCLIENT;
        if (PtInRect(BtnRect(4), p.x, p.y)) return HTCLIENT;
        if (PtInRect(RewardRect(), p.x, p.y)) return HTCLIENT;
        for (int i = 0; i < T_COUNT; ++i) {
            if (PtInRect(RowRect(i), p.x, p.y)) return HTCLIENT;
        }
        return HTCAPTION;   // 其余区域拖动窗口
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TMR_TICK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================ 入口 ============================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_inst = hInst;

    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiToken, &gsi, NULL) != Ok) {
        MessageBoxW(NULL, L"GDI+ 初始化失败", L"错误", MB_ICONERROR);
        return 1;
    }

    g_exeDir = ExeDir();
    SetProcessDPIAware();       // 坐标必须是物理像素，和穷举程序保持一致
    LoadCoordsToUI();           // 打开就把 coords.json 里的坐标显示出来
    g_bg = BuildBackground(WIN_W, WIN_H);
    g_reward = LoadPng(JoinPath(g_exeDir, L"reward.png"));
    if (g_reward) {
        // 缩到 96x96 居中显示用原图，绘制时直接拉伸即可
    }
    g_icon = (HICON)LoadImageW(NULL, JoinPath(g_exeDir, L"icon.ico").c_str(),
                               IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"CalibGlassWindow";
    wc.hIcon = g_icon;
    wc.hIconSm = g_icon;
    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - WIN_W) / 2;
    int y = (sh - WIN_H) / 2;
    if (y < 0) y = 0;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"CalibGlassWindow",
        L"坐标校准 · lanzongnb666",
        WS_POPUP,
        x, y, WIN_W, WIN_H,
        NULL, NULL, hInst, NULL);
    if (!hwnd) {
        GdiplusShutdown(g_gdiToken);
        return 1;
    }
    g_hwnd = hwnd;

    // Win11 圆角（老系统会失败，忽略）
    {
        typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            PFN_DwmSetWindowAttribute p =
                (PFN_DwmSetWindowAttribute)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (p) {
                int pref = 2;   // DWMWCP_ROUND
                p(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
            }
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_bg) { delete g_bg; g_bg = NULL; }
    if (g_reward) { delete g_reward; g_reward = NULL; }
    if (g_icon) { DestroyIcon(g_icon); g_icon = NULL; }
    GdiplusShutdown(g_gdiToken);
    return 0;
}
