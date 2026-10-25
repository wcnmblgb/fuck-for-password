// -*- coding: utf-8 -*-
// 作者：lanzongnb666
// ---------------------------------------------------------------------------
// 数字密码穷举工具（C++ / Win32 API，无任何第三方依赖）
//
// 目标界面（手机锁屏样式的密码框）：数字键 1..9、0 + 底排 ← / 清零 / 确定
// 每轮动作：点 4 个数字 -> 点“确定” -> 等判定 -> 查监视点颜色 -> 关失败提示 -> 点“清零”
//
// 编译（详见 build.bat 与 使用说明.md）：
//   MinGW-w64:  g++ -O2 -std=c++17 -o 穷举密码.exe 穷举密码.cpp -luser32 -lgdi32 -static
//   MSVC:       cl /nologo /O2 /std:c++17 /utf-8 /EHsc 穷举密码.cpp /link user32.lib gdi32.lib
//
// 用法：
//   穷举密码.exe                      启动时询问初始密码，回车 = 0000
//   穷举密码.exe 1234                 从 1234 开始
//   穷举密码.exe --test               坐标自检（鼠标依次停在每个点）
//   穷举密码.exe --calibrate          坐标校准，生成 coords.json
//   穷举密码.exe --submit-wait=250    覆盖各项时间参数（毫秒）
//   穷举密码.exe --click-interval=10 --fail-wait=100 --clear-wait=0 --tolerance=40
//
// 急停：按 ESC，或把鼠标甩到屏幕左上角。
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <iostream>

// ============================ 配置结构 ============================

struct Config {
    int  length        = 4;      // 密码位数
    int  clickInterval = 20;     // 数字键之间间隔（毫秒）
    int  submitWait    = 350;    // 点“确定”后的等待（毫秒）
    int  failWait      = 150;    // 关掉失败提示后的等待（毫秒）
    int  clearWait     = 30;     // 点“清零”后的等待（毫秒），想极限可以改 0
    int  tolerance     = 30;     // 颜色容差（单通道最大差值）
    bool stopOnHit     = false;  // 变色后是否立刻停止；false = 只标记继续跑

    POINT key[10]      = {};     // 数字 0..9 的坐标
    bool  hasKey[10]   = {};

    POINT submitPt     = {};
    bool  hasSubmit    = false;
    POINT clearPt      = {};
    bool  hasClear     = false;
    POINT watchPt      = {};
    bool  hasWatch     = false;
    COLORREF baseColor = 0;
    bool  hasBase      = false;
    POINT failClosePt  = {};
    bool  hasFailClose = false;
    std::string clearKey;        // "enter" / "esc" / ""（空 = 不用键盘）
    std::string failCloseKey;
};

// 放大镜抓到的默认数字键坐标（快捷键后面的数字 = 键位）
static const POINT DEFAULT_KEYS[10] = {
    {1556, 825},   // 0
    {1460, 609},   // 1
    {1550, 610},   // 2
    {1654, 611},   // 3
    {1456, 687},   // 4
    {1552, 680},   // 5
    {1659, 684},   // 6
    {1457, 762},   // 7
    {1555, 762},   // 8
    {1662, 758},   // 9
};

static std::string g_dir;
static std::string g_coordsFile;
static std::string g_logFile;
static std::string g_hitFile;

// ============================ 小工具 ============================

static std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

static std::string exeDir() {
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string p(buf, n);
    size_t s = p.find_last_of("\\/");
    if (s == std::string::npos) return ".";
    return p.substr(0, s);
}

static bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string hexOf(COLORREF c) {
    char b[16];
    snprintf(b, sizeof(b), "#%02X%02X%02X",
             (unsigned)GetRValue(c), (unsigned)GetGValue(c), (unsigned)GetBValue(c));
    return std::string(b);
}

static void logPrint(FILE* f, const char* fmt, ...) {
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

// ============================ 极简 JSON 读取 ============================
// 只认 Python 版校准脚本写出来的那种结构，够用就行

static bool readTextFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// 跳到下一个数字（跳过逗号、空白等分隔符）
static const char* skipToNumber(const char* p) {
    while (*p && !isdigit((unsigned char)*p) && *p != '-' && *p != '+') ++p;
    return p;
}

// 在 key 之后找一对整数 [x, y]
static bool jsonPair(const std::string& j, const std::string& key, int& x, int& y) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    size_t b = j.find('[', pos + key.size());
    if (b == std::string::npos) return false;
    size_t e = j.find(']', b);
    if (e == std::string::npos) return false;
    std::string s = j.substr(b + 1, e - b - 1);

    const char* p = skipToNumber(s.c_str());
    if (!*p) return false;
    char* p1 = NULL;
    long a = strtol(p, &p1, 10);
    if (p1 == p) return false;
    p = skipToNumber(p1);
    if (!*p) return false;
    char* p2 = NULL;
    long c = strtol(p, &p2, 10);
    if (p2 == p) return false;
    x = (int)a;
    y = (int)c;
    return true;
}

// 在 key 之后找三个整数 [r, g, b]
static bool jsonTriple(const std::string& j, const std::string& key, int& r, int& g, int& b) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    size_t bs = j.find('[', pos + key.size());
    if (bs == std::string::npos) return false;
    size_t be = j.find(']', bs);
    if (be == std::string::npos) return false;
    std::string s = j.substr(bs + 1, be - bs - 1);

    const char* p = skipToNumber(s.c_str());
    if (!*p) return false;
    char* p1 = NULL;
    long v1 = strtol(p, &p1, 10);
    if (p1 == p) return false;
    p = skipToNumber(p1);
    if (!*p) return false;
    char* p2 = NULL;
    long v2 = strtol(p, &p2, 10);
    if (p2 == p) return false;
    p = skipToNumber(p2);
    if (!*p) return false;
    char* p3 = NULL;
    long v3 = strtol(p, &p3, 10);
    if (p3 == p) return false;
    r = (int)v1; g = (int)v2; b = (int)v3;
    return true;
}

static bool jsonInt(const std::string& j, const std::string& key, int& v) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    const char* p = j.c_str() + pos + key.size();
    char* end = NULL;
    long a = strtol(p, &end, 10);
    if (end == p) return false;
    v = (int)a;
    return true;
}

static bool jsonBool(const std::string& j, const std::string& key, bool& v) {
    size_t pos = j.find(key);
    if (pos == std::string::npos) return false;
    size_t p = pos + key.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n' || j[p] == ':')) ++p;
    if (p >= j.size()) return false;
    v = (j.compare(p, 4, "true") == 0);
    return true;
}

// ============================ 鼠标 / 键盘 / 屏幕 ============================

static bool stopRequested() {
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return true;
    POINT p;
    if (GetCursorPos(&p)) {
        if (p.x <= 3 && p.y <= 3) return true;   // 鼠标甩到左上角 = 保险绳
    }
    return false;
}

static void moveTo(int x, int y) {
    SetCursorPos(x, y);
}

static void sendClick() {
    INPUT in[2];
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

static void clickAt(int x, int y) {
    moveTo(x, y);
    Sleep(6);
    sendClick();
    Sleep(15);          // 按住时间
}

static void sendKey(WORD vk) {
    INPUT in[2];
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static bool getPixelColor(int x, int y, COLORREF& out) {
    HDC hdc = GetDC(NULL);
    if (!hdc) return false;
    COLORREF c = GetPixel(hdc, x, y);
    ReleaseDC(NULL, hdc);
    if (c == CLR_INVALID) return false;
    out = c;
    return true;
}

static int colorDiff(COLORREF a, COLORREF b) {
    int dr = (int)GetRValue(a) - (int)GetRValue(b);
    int dg = (int)GetGValue(a) - (int)GetGValue(b);
    int db = (int)GetBValue(a) - (int)GetBValue(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    int m = dr > dg ? dr : dg;
    return m > db ? m : db;
}

// ============================ coords.json 读写 ============================

static void loadCoords(Config& cfg) {
    std::string j;
    if (!readTextFile(g_coordsFile, j)) return;

    // keys
    for (int d = 0; d <= 9; ++d) {
        char k[16];
        snprintf(k, sizeof(k), "\"%d\":", d);
        int x = 0, y = 0;
        if (jsonPair(j, k, x, y)) {
            cfg.key[d].x = x;
            cfg.key[d].y = y;
            cfg.hasKey[d] = true;
        }
    }
    int x = 0, y = 0;
    if (jsonPair(j, "\"submit\"", x, y)) { cfg.submitPt.x = x; cfg.submitPt.y = y; cfg.hasSubmit = true; }
    if (jsonPair(j, "\"clear_point\"", x, y)) { cfg.clearPt.x = x; cfg.clearPt.y = y; cfg.hasClear = true; }
    if (jsonPair(j, "\"watch_point\"", x, y)) { cfg.watchPt.x = x; cfg.watchPt.y = y; cfg.hasWatch = true; }
    if (jsonPair(j, "\"fail_close_point\"", x, y)) { cfg.failClosePt.x = x; cfg.failClosePt.y = y; cfg.hasFailClose = true; }

    int r = 0, g = 0, b = 0;
    if (jsonTriple(j, "\"base_color\"", r, g, b)) {
        cfg.baseColor = RGB(r, g, b);
        cfg.hasBase = true;
    }
    int v = 0;
    if (jsonInt(j, "\"tolerance\"", v)) cfg.tolerance = v;
    bool bv = false;
    if (jsonBool(j, "\"stop_on_hit\"", bv)) cfg.stopOnHit = bv;

    printf("[i] 已加载坐标文件：%s\n", g_coordsFile.c_str());
}

static void saveCoords(const Config& cfg) {
    FILE* f = fopen(g_coordsFile.c_str(), "w");
    if (!f) {
        printf("[!] 无法写入 %s\n", g_coordsFile.c_str());
        return;
    }
    fprintf(f, "{\n  \"keys\": {\n");
    for (int d = 0; d <= 9; ++d) {
        fprintf(f, "    \"%d\": [%ld, %ld]%s\n", d,
                cfg.key[d].x, cfg.key[d].y, d == 9 ? "" : ",");
    }
    fprintf(f, "  },\n");
    if (cfg.hasSubmit)
        fprintf(f, "  \"submit\": [%ld, %ld],\n", cfg.submitPt.x, cfg.submitPt.y);
    else
        fprintf(f, "  \"submit\": null,\n");
    if (cfg.hasClear)
        fprintf(f, "  \"clear_point\": [%ld, %ld],\n", cfg.clearPt.x, cfg.clearPt.y);
    else
        fprintf(f, "  \"clear_point\": null,\n");
    if (cfg.hasWatch)
        fprintf(f, "  \"watch_point\": [%ld, %ld],\n", cfg.watchPt.x, cfg.watchPt.y);
    else
        fprintf(f, "  \"watch_point\": null,\n");
    if (cfg.hasBase)
        fprintf(f, "  \"base_color\": [%u, %u, %u],\n",
                (unsigned)GetRValue(cfg.baseColor),
                (unsigned)GetGValue(cfg.baseColor),
                (unsigned)GetBValue(cfg.baseColor));
    else
        fprintf(f, "  \"base_color\": null,\n");
    fprintf(f, "  \"tolerance\": %d,\n", cfg.tolerance);
    fprintf(f, "  \"stop_on_hit\": %s,\n", cfg.stopOnHit ? "true" : "false");
    if (cfg.hasFailClose)
        fprintf(f, "  \"fail_close_point\": [%ld, %ld],\n", cfg.failClosePt.x, cfg.failClosePt.y);
    else
        fprintf(f, "  \"fail_close_point\": null,\n");
    if (cfg.failCloseKey.empty())
        fprintf(f, "  \"fail_close_key\": null\n");
    else
        fprintf(f, "  \"fail_close_key\": \"%s\"\n", cfg.failCloseKey.c_str());
    fprintf(f, "}\n");
    fclose(f);
    printf("\n=== 已保存：%s ===\n", g_coordsFile.c_str());
}

// ============================ 动作 ============================

static void doSubmit(const Config& cfg) {
    if (cfg.hasSubmit) clickAt((int)cfg.submitPt.x, (int)cfg.submitPt.y);
    else               sendKey(VK_RETURN);
}

static void doCloseFail(const Config& cfg) {
    if (cfg.hasFailClose) {
        clickAt((int)cfg.failClosePt.x, (int)cfg.failClosePt.y);
        Sleep(cfg.failWait);
        return;
    }
    if (cfg.failCloseKey == "enter") {
        sendKey(VK_RETURN);
        Sleep(cfg.failWait);
    } else if (cfg.failCloseKey == "esc") {
        sendKey(VK_ESCAPE);
        Sleep(cfg.failWait);
    }
}

static void doClear(const Config& cfg) {
    if (cfg.hasClear) {
        // 和上一步点的“关闭失败提示”如果是同一个点，不重复点
        if (cfg.hasFailClose &&
            cfg.clearPt.x == cfg.failClosePt.x && cfg.clearPt.y == cfg.failClosePt.y) {
            return;
        }
        clickAt((int)cfg.clearPt.x, (int)cfg.clearPt.y);
        Sleep(cfg.clearWait);
        return;
    }
    if (cfg.clearKey == "enter") {
        sendKey(VK_RETURN);
        Sleep(cfg.clearWait);
    } else if (cfg.clearKey == "esc") {
        sendKey(VK_ESCAPE);
        Sleep(cfg.clearWait);
    }
}

// ============================ 校准 / 自检 ============================

static bool capturePoint(const char* what, POINT& p, bool optional, int waitSec) {
    if (optional) {
        printf("要抓【%s】吗？直接回车 = 是，输入 n = 跳过：", what);
        fflush(stdout);
        std::string ans;
        if (!std::getline(std::cin, ans)) return false;
        ans = trim(ans);
        if (!ans.empty() && (ans[0] == 'n' || ans[0] == 'N')) return false;
    }
    printf("请在 %d 秒内把鼠标移到【%s】上，然后不要动……\n", waitSec, what);
    for (int s = waitSec; s > 0; --s) {
        printf("  %d\n", s);
        Sleep(1000);
    }
    GetCursorPos(&p);
    printf("  -> 记录：%s = (%ld, %ld)\n", what, p.x, p.y);
    return true;
}

static int doCalibrate(Config& cfg) {
    printf("=== 坐标校准 ===\n");
    printf("先把目标程序的密码轮盘显示出来，校准期间不要移动窗口。\n");

    for (int d = 0; d <= 9; ++d) {
        int order[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
        char what[64];
        snprintf(what, sizeof(what), "数字 %d 键", order[d]);
        POINT p = {};
        if (capturePoint(what, p, false, 3)) {
            cfg.key[order[d]].x = p.x;
            cfg.key[order[d]].y = p.y;
            cfg.hasKey[order[d]] = true;
        }
    }

    POINT p = {};
    if (capturePoint("确定按钮（底排右边那个）", p, true, 3)) {
        cfg.submitPt = p; cfg.hasSubmit = true;
    }
    if (capturePoint("清零按钮（底排中间那个）", p, true, 3)) {
        cfg.clearPt = p; cfg.hasClear = true;
    }
    if (capturePoint("颜色监视点（密码正确、轮盘关掉后颜色会明显变化的位置）", p, true, 3)) {
        cfg.watchPt = p; cfg.hasWatch = true;
        COLORREF c = 0;
        if (getPixelColor((int)p.x, (int)p.y, c)) {
            cfg.baseColor = c;
            cfg.hasBase = true;
            printf("  -> 该点当前颜色（基准色）：%s\n", hexOf(c).c_str());
        } else {
            printf("  [!] 颜色读取失败，请确认监视点在屏幕范围内\n");
        }
    }
    if (capturePoint("失败弹窗的关闭按钮（没有弹窗就输 n 跳过）", p, true, 3)) {
        cfg.failClosePt = p; cfg.hasFailClose = true;
    }

    saveCoords(cfg);
    printf("下一步：穷举密码.exe --test 先自检，再直接运行 穷举密码.exe\n");
    return 0;
}

static int doTest(const Config& cfg) {
    printf("=== 坐标自检：鼠标会依次停在每个点，眼睛看着屏幕确认 ===\n");
    Sleep(1000);
    int order[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
    for (int i = 0; i < 10; ++i) {
        int d = order[i];
        SetCursorPos((int)cfg.key[d].x, (int)cfg.key[d].y);
        printf("    数字 %d -> (%ld, %ld)\n", d, cfg.key[d].x, cfg.key[d].y);
        Sleep(900);
    }
    if (cfg.hasSubmit) {
        SetCursorPos((int)cfg.submitPt.x, (int)cfg.submitPt.y);
        printf("    确定   -> (%ld, %ld)\n", cfg.submitPt.x, cfg.submitPt.y);
        Sleep(900);
    } else printf("    确定   -> 未配置（用回车提交）\n");

    if (cfg.hasClear) {
        SetCursorPos((int)cfg.clearPt.x, (int)cfg.clearPt.y);
        printf("    清零   -> (%ld, %ld)\n", cfg.clearPt.x, cfg.clearPt.y);
        Sleep(900);
    } else printf("    清零   -> 未配置（第二组起会叠加残留数字）\n");

    if (cfg.hasFailClose) {
        SetCursorPos((int)cfg.failClosePt.x, (int)cfg.failClosePt.y);
        printf("    关弹窗 -> (%ld, %ld)\n", cfg.failClosePt.x, cfg.failClosePt.y);
        Sleep(900);
    } else printf("    关弹窗 -> 未配置\n");

    if (cfg.hasWatch) {
        SetCursorPos((int)cfg.watchPt.x, (int)cfg.watchPt.y);
        COLORREF c = 0;
        std::string cs = getPixelColor((int)cfg.watchPt.x, (int)cfg.watchPt.y, c) ? hexOf(c) : "(读不到)";
        printf("    监视点 -> (%ld, %ld)  当前颜色 %s  基准 %s  容差 %d\n",
               cfg.watchPt.x, cfg.watchPt.y, cs.c_str(),
               cfg.hasBase ? hexOf(cfg.baseColor).c_str() : "(未采样)", cfg.tolerance);
        Sleep(900);
    } else printf("    监视点 -> 未配置（不做颜色判定）\n");

    printf("=== 自检结束 ===\n");
    return 0;
}

// ============================ 配置查看 ============================

static int doShow(const Config& cfg) {
    printf("=== 当前配置（读自 %s）===\n", g_coordsFile.c_str());
    printf("  密码位数 : %d\n", cfg.length);
    int order[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
    printf("  数字键   : ");
    for (int i = 0; i < 10; ++i) {
        int d = order[i];
        printf("%d(%ld,%ld) ", d, cfg.key[d].x, cfg.key[d].y);
    }
    printf("\n");
    if (cfg.hasSubmit)    printf("  确定     : (%ld, %ld)\n", cfg.submitPt.x, cfg.submitPt.y);
    else                  printf("  确定     : 未配置（用回车提交）\n");
    if (cfg.hasClear)     printf("  清零     : (%ld, %ld)\n", cfg.clearPt.x, cfg.clearPt.y);
    else                  printf("  清零     : 未配置\n");
    if (cfg.hasFailClose) printf("  关弹窗   : (%ld, %ld)\n", cfg.failClosePt.x, cfg.failClosePt.y);
    else                  printf("  关弹窗   : 未配置\n");
    if (cfg.hasWatch) {
        COLORREF c = 0;
        std::string cs = getPixelColor((int)cfg.watchPt.x, (int)cfg.watchPt.y, c)
                             ? hexOf(c) : std::string("(读不到)");
        printf("  监视点   : (%ld, %ld)  当前颜色 %s  基准 %s  容差 %d\n",
               cfg.watchPt.x, cfg.watchPt.y, cs.c_str(),
               cfg.hasBase ? hexOf(cfg.baseColor).c_str() : "(未采样)", cfg.tolerance);
    } else {
        printf("  监视点   : 未配置\n");
    }
    printf("  变色后   : %s\n", cfg.stopOnHit ? "停止" : "只标记不停");
    printf("  时间参数 : 点击间隔 %dms  确定后 %dms  关弹窗后 %dms  清零后 %dms\n",
           cfg.clickInterval, cfg.submitWait, cfg.failWait, cfg.clearWait);
    return 0;
}

// ============================ 主循环 ============================

static int askStart(int length, int defValue) {
    char def[32];
    snprintf(def, sizeof(def), "%0*d", length, defValue);
    for (;;) {
        printf("请输入初始密码（%d 位数字，直接回车 = %s）：", length, def);
        fflush(stdout);
        std::string s;
        if (!std::getline(std::cin, s)) return defValue;
        s = trim(s);
        if (s.empty()) return defValue;
        if ((int)s.size() <= length && allDigits(s)) return atoi(s.c_str());
        printf("  [!] 请输入不超过 %d 位的纯数字\n", length);
    }
}

static int doRun(Config& cfg, const std::string& initCode) {
    int length = cfg.length;
    long long total = 1;
    for (int i = 0; i < length; ++i) total *= 10;

    int start = 0;
    if (!initCode.empty() && allDigits(initCode)) {
        start = atoi(initCode.c_str());
    } else {
        start = askStart(length, 0);
    }
    if (start < 0) start = 0;
    if (start > total - 1) start = (int)(total - 1);

    char startCode[32];
    snprintf(startCode, sizeof(startCode), "%0*d", length, start);

    printf("\n==== 运行配置 ====\n");
    printf("  密码位数   : %d（共 %lld 组）\n", length, total);
    printf("  初始密码   : %s\n", startCode);
    printf("  提交方式   : ");
    if (cfg.hasSubmit) printf("点击 (%ld, %ld)\n", cfg.submitPt.x, cfg.submitPt.y);
    else               printf("回车键\n");
    printf("  清零方式   : ");
    if (cfg.hasClear) printf("点击 (%ld, %ld)\n", cfg.clearPt.x, cfg.clearPt.y);
    else if (!cfg.clearKey.empty()) printf("按键 %s\n", cfg.clearKey.c_str());
    else printf("！未配置（第二组起会叠加残留数字）\n");
    printf("  关失败提示 : ");
    if (cfg.hasFailClose) printf("点击 (%ld, %ld)\n", cfg.failClosePt.x, cfg.failClosePt.y);
    else if (!cfg.failCloseKey.empty()) printf("按键 %s\n", cfg.failCloseKey.c_str());
    else printf("不处理\n");
    printf("  颜色检测   : ");
    if (cfg.hasWatch) printf("监视点 (%ld, %ld) 容差 %d，变色后%s\n",
                             cfg.watchPt.x, cfg.watchPt.y, cfg.tolerance,
                             cfg.stopOnHit ? "停止" : "只标记不停");
    else printf("未配置（不做判定）\n");
    double perRound = (length * (cfg.clickInterval + 21.0) + cfg.submitWait + cfg.failWait + cfg.clearWait) / 1000.0;
    printf("  每轮耗时   : 约 %.2f 秒（全部跑完约 %.1f 小时）\n",
           perRound, perRound * total / 3600.0);
    printf("  日志文件   : %s\n", g_logFile.c_str());
    printf("  变色记录   : %s\n", g_hitFile.c_str());
    printf("  急停方式   : ESC 键 或 鼠标甩到屏幕左上角\n\n");

    FILE* log = fopen(g_logFile.c_str(), "a");
    FILE* hit = fopen(g_hitFile.c_str(), "a");
    if (!log) printf("[!] 无法写日志文件 %s\n", g_logFile.c_str());
    if (!hit) printf("[!] 无法写变色记录 %s\n", g_hitFile.c_str());

    time_t now = time(NULL);
    struct tm* lt = localtime(&now);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", lt);
    logPrint(log, "\n===== 新一轮 %s  起始=%s =====\n", stamp, startCode);

    printf("3 秒后开始，请切到目标窗口……\n");
    for (int s = 3; s > 0; --s) {
        printf("  %d\n", s);
        Sleep(1000);
    }

    // 基准色：此刻轮盘还开着，趁现在采样
    COLORREF base = cfg.baseColor;
    bool hasBase = cfg.hasBase;
    if (cfg.hasWatch && !hasBase) {
        if (getPixelColor((int)cfg.watchPt.x, (int)cfg.watchPt.y, base)) {
            hasBase = true;
            printf("[i] 自动采样基准颜色：%s（监视点 (%ld, %ld)）\n",
                   hexOf(base).c_str(), cfg.watchPt.x, cfg.watchPt.y);
        } else {
            printf("[!] 基准颜色采样失败，颜色检测关闭\n");
        }
    } else if (hasBase && cfg.hasWatch) {
        printf("[i] 使用配置的基准颜色：%s\n", hexOf(base).c_str());
    }

    int done = start;
    int hitCount = 0;
    ULONGLONG t0 = GetTickCount64();

    for (int idx = start; idx < (int)total; ++idx) {
        if (stopRequested()) break;

        char code[32];
        snprintf(code, sizeof(code), "%0*d", length, idx);

        for (int i = 0; i < length; ++i) {
            if (stopRequested()) break;
            int d = code[i] - '0';
            if (d < 0 || d > 9) d = 0;
            clickAt((int)cfg.key[d].x, (int)cfg.key[d].y);
            Sleep(cfg.clickInterval);
        }
        if (stopRequested()) break;

        doSubmit(cfg);
        Sleep(cfg.submitWait);

        bool changed = false;
        if (cfg.hasWatch && hasBase) {
            COLORREF cur = 0;
            if (getPixelColor((int)cfg.watchPt.x, (int)cfg.watchPt.y, cur)) {
                int diff = colorDiff(cur, base);
                if (diff > cfg.tolerance) {
                    changed = true;
                    ++hitCount;
                    time_t t2 = time(NULL);
                    struct tm* l2 = localtime(&t2);
                    char st2[64];
                    strftime(st2, sizeof(st2), "%Y-%m-%d %H:%M:%S", l2);
                    logPrint(hit, "%s  组合=%s  颜色=%s  基准=%s  差值=%d\n",
                             st2, code, hexOf(cur).c_str(), hexOf(base).c_str(), diff);
                    printf("\n  *** [HIT] 第 %d 次变色：组合 %s  颜色 %s（基准 %s，差值 %d）***\n",
                           hitCount, code, hexOf(cur).c_str(), hexOf(base).c_str(), diff);
                }
            }
        }

        doCloseFail(cfg);
        doClear(cfg);

        done = idx;
        {
            time_t t3 = time(NULL);
            struct tm* l3 = localtime(&t3);
            char st3[64];
            strftime(st3, sizeof(st3), "%H:%M:%S", l3);
            logPrint(log, "%s  %s  %s elapsed=%llus\n", st3, code,
                     changed ? "[HIT 变色]" : "         ",
                     (unsigned long long)((GetTickCount64() - t0) / 1000));
        }
        printf("\r  正在尝试第 %d/%lld 组：%s  已变色 %d 次   ", idx + 1, total, code, hitCount);
        fflush(stdout);

        if (changed && cfg.stopOnHit) {
            printf("\n  STOP_ON_HIT 已开启，变色后停止\n");
            break;
        }
    }

    logPrint(log, "----- 停止于 %s（已尝试到 %0*d，变色 %d 次）-----\n",
             stamp, length, done, hitCount);
    if (log) fclose(log);
    if (hit) fclose(hit);

    printf("\n\n==== 结束 ====\n");
    printf("  最后尝试的组合：%0*d\n", length, done);
    printf("  变色（可能是对的）次数：%d\n", hitCount);
    printf("  日志：%s\n", g_logFile.c_str());
    if (hitCount) printf("  变色记录：%s（挑时间最早那条，通常就是密码）\n", g_hitFile.c_str());
    return 0;
}

// ============================ 入口 ============================

static bool argValue(const std::string& a, const char* name, std::string& out) {
    std::string pre = std::string(name) + "=";
    if (a.size() > pre.size() && a.compare(0, pre.size(), pre) == 0) {
        out = a.substr(pre.size());
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetProcessDPIAware();

    g_dir        = exeDir();
    g_coordsFile = pathJoin(g_dir, "coords.json");
    g_logFile    = pathJoin(g_dir, "brute_log.txt");
    g_hitFile    = pathJoin(g_dir, "hits.txt");

    Config cfg;
    for (int d = 0; d <= 9; ++d) {
        cfg.key[d] = DEFAULT_KEYS[d];
        cfg.hasKey[d] = true;
    }

    bool modeTest = false;
    bool modeShow = false;
    bool modeCal = false;
    std::string initCode;
    std::string v;

    loadCoords(cfg);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--test")                modeTest = true;
        else if (a == "--show")           modeShow = true;
        else if (a == "--calibrate" || a == "-c") modeCal = true;
        else if (argValue(a, "--length", v))         cfg.length        = atoi(v.c_str());
        else if (argValue(a, "--click-interval", v)) cfg.clickInterval = atoi(v.c_str());
        else if (argValue(a, "--submit-wait", v))    cfg.submitWait    = atoi(v.c_str());
        else if (argValue(a, "--fail-wait", v))      cfg.failWait      = atoi(v.c_str());
        else if (argValue(a, "--clear-wait", v))     cfg.clearWait     = atoi(v.c_str());
        else if (argValue(a, "--tolerance", v))      cfg.tolerance     = atoi(v.c_str());
        else if (a == "--stop-on-hit")               cfg.stopOnHit     = true;
        else if (a.rfind("--", 0) != 0 && allDigits(a)) initCode = a;
    }

    if (cfg.length < 1) cfg.length = 1;
    if (cfg.length > 8) cfg.length = 8;

    if (modeCal) return doCalibrate(cfg);
    if (modeShow) return doShow(cfg);
    if (modeTest) return doTest(cfg);
    return doRun(cfg, initCode);
}
