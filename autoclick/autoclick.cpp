#include "resource.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <locale>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;

// -----------------------------------------------------------------------------
// Theme
// -----------------------------------------------------------------------------
namespace Theme {
constexpr uint32_t Bg          = 0x0F0F12;
constexpr uint32_t Panel       = 0x1A1A20;
constexpr uint32_t Elevated    = 0x202027;
constexpr uint32_t Button      = 0x25252D;
constexpr uint32_t ButtonHover = 0x30303A;
constexpr uint32_t Border      = 0x303038;
constexpr uint32_t Accent      = 0x8B5CF6;
constexpr uint32_t AccentHover = 0x9F78FF;
constexpr uint32_t AccentDown  = 0x7546DF;
constexpr uint32_t Text        = 0xF5F5F7;
constexpr uint32_t SubText     = 0xA1A1AA;
constexpr uint32_t Muted       = 0x71717A;
constexpr uint32_t Success     = 0x34D399;
constexpr uint32_t Warning     = 0xFBBF24;
constexpr uint32_t Danger      = 0xF87171;
constexpr uint32_t Divider     = 0x1E1E25;
constexpr uint32_t CornerMark  = 0x2C2C35;
}

constexpr int kMouse4 = 0x101;
constexpr int kMouse5 = 0x102;

constexpr int kWindowWidthDip  = 380;
constexpr int kContentWidthDip = 348;
constexpr int kTitleHeightDip  = 40;
constexpr int kAutoHeightDip   = 341;
constexpr int kHoldHeightDip   = 402;

enum HotId : int {
    HotNone = 0,
    HotPin, HotMin, HotClose,
    HotMode0, HotMode1,
    HotTarget0, HotTarget1, HotTarget2,
    HotSpeedMinus, HotSpeedValue, HotSpeedPlus,
    HotPauseMinus, HotPauseValue, HotPausePlus,
    HotToggleKey, HotExitKey,
    HotCount
};

enum EditField { EditNone = 0, EditSpeed, EditPause };

enum : UINT {
    WM_APP_HOTKEY  = WM_APP + 1,
    WM_APP_BINDKEY = WM_APP + 2,
    WM_APP_STATE   = WM_APP + 3
};

enum : UINT_PTR {
    TIMER_ANIM = 1,
    TIMER_TICK = 2,
    TIMER_SAVE = 3,
    TIMER_REPEAT = 4
};

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------
struct AppState {
    std::atomic<bool> running{ false };
    std::atomic<bool> appActive{ true };
    std::atomic<bool> holding{ false };
    std::atomic<double> cps{ 20.0 };
    std::atomic<int> clickMode{ 0 };      // 0 Left, 1 Right, 2 Alternate
    std::atomic<int> actionType{ 0 };     // 0 Auto Click, 1 Hold Key
    std::atomic<long long> clickCount{ 0 };
    std::atomic<double> holdDuration{ 3.0 };
    std::atomic<double> holdPause{ 0.0 };
    std::atomic<int> toggleKey{ VK_F1 };
    std::atomic<int> exitKey{ kMouse5 };
    std::atomic<bool> alwaysOnTop{ false };
};

struct AppConfig {
    int actionType = 0;
    int clickMode = 0;
    double cps = 20.0;
    double holdDuration = 3.0;
    double holdPause = 0.0;
    int toggleKey = VK_F1;
    int exitKey = kMouse5;
    bool alwaysOnTop = false;
    int windowX = 0;
    int windowY = 0;
    bool hasPosition = false;
};

struct AnimValue {
    float value = 0.0f;
    float target = 0.0f;
};

struct FontSet {
    HFONT title = nullptr;
    HFONT body = nullptr;
    HFONT bodyMed = nullptr;
    HFONT section = nullptr;
    HFONT value = nullptr;
    HFONT status = nullptr;
    HFONT hotkey = nullptr;
    HFONT chip = nullptr;
    HFONT tooltip = nullptr;
    HFONT icon = nullptr;
};

struct UiState {
    HWND hwnd = nullptr;
    int dpi = 96;
    float scale = 1.0f;
    wchar_t fontFace[LF_FACESIZE] = L"Segoe UI";
    FontSet fonts;

    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    HBITMAP memOldBmp = nullptr;
    int memW = 0;
    int memH = 0;

    HotId hot = HotNone;
    HotId pressed = HotNone;
    bool trackingMouse = false;

    int bindingTarget = 0;          // 0 none, 1 toggle, 2 exit
    ULONGLONG bindMsgUntil = 0;

    EditField editField = EditNone;
    wchar_t editBuf[24] = {};
    int editLen = 0;
    bool caretOn = true;
    bool lastCaretOn = true;

    HotId repeatHot = HotNone;
    float repeatInterval = 140.0f;

    bool animTimer = false;
    bool tickTimer = false;
    ULONGLONG lastAnimTick = 0;
    ULONGLONG pinHoverStart = 0;
    bool pinTooltip = false;

    bool settingsReady = false;

    AnimValue hover[HotCount];
    AnimValue targetSel[3];
    AnimValue modePill;
    AnimValue pauseReveal;
    AnimValue valueFlash;

    Color statusColor{ 255, 161, 161, 170 };
    Color statusTarget{ 255, 161, 161, 170 };

    HHOOK kbdHook = nullptr;
    HHOOK mouseHook = nullptr;

    bool hasSavedPos = false;
    int savedX = 0;
    int savedY = 0;
};

struct Layout {
    int width = 0;
    int height = 0;
    int contentX = 0;
    int contentW = 0;

    RECT titleIcon{};
    RECT titleText{};
    RECT pin{};
    RECT minBtn{};
    RECT closeBtn{};

    RECT modeLabel{};
    RECT modeBox{};
    RECT modeSeg[2]{};

    RECT targetLabel{};
    RECT target[3]{};

    RECT speedLabel{};
    RECT speedMinus{};
    RECT speedValue{};
    RECT speedPlus{};

    RECT pauseLabel{};
    RECT pauseMinus{};
    RECT pauseValue{};
    RECT pausePlus{};

    RECT hotkeyLabel{};
    RECT hotkey[2]{};

    RECT statusSep{};
    RECT statusArea{};
    RECT statusDot{};
    RECT statusState{};
    RECT statusClicks{};
};

static AppState g_App;
static AppConfig g_Config;
static UiState g_Ui;
static Layout g_L;

static std::mutex g_WorkerMutex;
static std::condition_variable g_WorkerCv;
static HANDLE g_HiResTimer = nullptr;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void BuildLayout();
static void ResizeForMode(int mode);
static void SetActionMode(int mode);
static void SetClickMode(int mode);
static void StepNumeric(HotId id);
static void BeginEdit(EditField field);
static void CommitEdit();
static void CancelEdit();
static void BeginBind(int target);
static void CompleteBind(int vk);
static void RequestExit();
static void ToggleRunning();
static void MarkDirty();
static void InvalidateAll();
static void RefreshAnimTargets();
static void EnsureAnimTimer();
static void UpdateTickTimer();
static void RefreshHotFromCursor();
static HotId HitTest(POINT pt);
static bool HotEnabled(HotId id);
static void SaveConfig();
static void LoadConfig();
static void CreateUiFonts();
static std::wstring KeyName(int vk);
static void DrawUi(Graphics& g, HDC dc);
static void ApplyTheme(HWND hwnd);

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------
static int S(int dip) {
    return (int)std::lround((double)dip * (double)g_Ui.scale);
}

static float ClampF(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double ClampD(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static Color A(uint32_t rgb, BYTE alpha = 255) {
    return Color(alpha,
        (BYTE)((rgb >> 16) & 0xFF),
        (BYTE)((rgb >> 8) & 0xFF),
        (BYTE)(rgb & 0xFF));
}

static Color LerpColor(const Color& a, const Color& b, float t) {
    t = ClampF(t, 0.0f, 1.0f);
    auto mix = [t](BYTE x, BYTE y) -> BYTE {
        return (BYTE)std::lround((double)x + ((double)y - (double)x) * (double)t);
    };
    return Color(mix(a.GetA(), b.GetA()), mix(a.GetR(), b.GetR()),
        mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

static uint32_t ColorRgb(const Color& c) {
    return ((uint32_t)c.GetR() << 16) | ((uint32_t)c.GetG() << 8) | (uint32_t)c.GetB();
}

static std::wstring FormatValue(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", v);
    std::wstring s(buf);
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, L".0") == 0)
        s.erase(s.size() - 2);
    return s;
}

static std::wstring FormatCount(long long n) {
    if (n < 0) n = 0;
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3)
        s.insert((size_t)i, L",");
    return s;
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

static std::wstring GetConfigDir() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) {
        std::wstring dir = std::wstring(raw) + L"\\TuynfAutoClicker";
        CoTaskMemFree(raw);
        return dir;
    }
    return GetExeDir();
}

static double ParseDouble(const std::wstring& s, double fallback) {
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return fallback;
    return v;
}

static int ParseInt(const std::wstring& s, int fallback) {
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return fallback;
    return (int)v;
}

static bool ParseConfigFile(const std::wstring& path, bool& sawX, bool& sawY) {
    std::wifstream file(path);
    if (!file.is_open()) return false;
    file.imbue(std::locale::classic());

    std::wstring line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = Trim(line.substr(eq + 1));
        if (key == L"Cps") g_Config.cps = ParseDouble(val, g_Config.cps);
        else if (key == L"ClickMode") g_Config.clickMode = ParseInt(val, g_Config.clickMode);
        else if (key == L"ActionType") g_Config.actionType = ParseInt(val, g_Config.actionType);
        else if (key == L"ToggleKey") g_Config.toggleKey = ParseInt(val, g_Config.toggleKey);
        else if (key == L"ExitKey") g_Config.exitKey = ParseInt(val, g_Config.exitKey);
        else if (key == L"HoldDuration") g_Config.holdDuration = ParseDouble(val, g_Config.holdDuration);
        else if (key == L"HoldPause") g_Config.holdPause = ParseDouble(val, g_Config.holdPause);
        else if (key == L"AlwaysOnTop") g_Config.alwaysOnTop = ParseInt(val, 0) != 0;
        else if (key == L"X") { g_Config.windowX = ParseInt(val, 0); sawX = true; }
        else if (key == L"Y") { g_Config.windowY = ParseInt(val, 0); sawY = true; }
    }
    return true;
}

static void LoadConfig() {
    g_Config = AppConfig{};

    bool sawX = false, sawY = false;
    std::wstring primary = GetConfigDir() + L"\\settings.ini";
    std::wstring legacy = GetExeDir() + L"\\settings.ini";

    if (!ParseConfigFile(primary, sawX, sawY) && legacy != primary)
        ParseConfigFile(legacy, sawX, sawY);

    g_Config.cps = ClampD(g_Config.cps, 1.0, 100.0);
    g_Config.holdDuration = ClampD(g_Config.holdDuration, 0.1, 60.0);
    g_Config.holdPause = ClampD(g_Config.holdPause, 0.0, 60.0);
    if (g_Config.actionType < 0 || g_Config.actionType > 1) g_Config.actionType = 0;
    if (g_Config.clickMode < 0 || g_Config.clickMode > 2) g_Config.clickMode = 0;

    auto validKey = [](int k) {
        return (k >= 1 && k <= 254) || k == kMouse4 || k == kMouse5;
    };
    if (!validKey(g_Config.toggleKey)) g_Config.toggleKey = VK_F1;
    if (!validKey(g_Config.exitKey)) g_Config.exitKey = kMouse5;
    if (g_Config.exitKey == g_Config.toggleKey) {
        g_Config.exitKey = kMouse5;
        if (g_Config.exitKey == g_Config.toggleKey) g_Config.toggleKey = VK_F1;
    }

    // 0,0 is treated as "no saved position" to keep first launch centered.
    g_Config.hasPosition = (sawX || sawY) && (g_Config.windowX != 0 || g_Config.windowY != 0);

    g_App.cps = g_Config.cps;
    g_App.holdDuration = g_Config.holdDuration;
    g_App.holdPause = g_Config.holdPause;
    g_App.actionType = g_Config.actionType;
    g_App.clickMode = g_Config.clickMode;
    g_App.toggleKey = g_Config.toggleKey;
    g_App.exitKey = g_Config.exitKey;
    g_App.alwaysOnTop = g_Config.alwaysOnTop;

    g_Ui.hasSavedPos = g_Config.hasPosition;
    g_Ui.savedX = g_Config.windowX;
    g_Ui.savedY = g_Config.windowY;
}

static void SaveConfig() {
    if (!g_Ui.settingsReady) return;

    int x = g_Config.windowX;
    int y = g_Config.windowY;
    if (g_Ui.hwnd) {
        WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
        if (GetWindowPlacement(g_Ui.hwnd, &wp)) {
            x = wp.rcNormalPosition.left;
            y = wp.rcNormalPosition.top;
        }
    }
    g_Config.windowX = x;
    g_Config.windowY = y;

    std::wstring dir = GetConfigDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring path = dir + L"\\settings.ini";

    std::wofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        path = GetExeDir() + L"\\settings.ini";
        file.open(path, std::ios::trunc);
    }
    if (!file.is_open()) return;
    file.imbue(std::locale::classic());

    wchar_t num[64];
    auto writeDouble = [&](const wchar_t* key, double v) {
        swprintf_s(num, L"%.6f", v);
        file << key << L"=" << num << L"\n";
    };

    writeDouble(L"Cps", g_App.cps.load());
    file << L"ClickMode=" << g_App.clickMode.load() << L"\n";
    file << L"ActionType=" << g_App.actionType.load() << L"\n";
    file << L"ToggleKey=" << g_App.toggleKey.load() << L"\n";
    file << L"ExitKey=" << g_App.exitKey.load() << L"\n";
    writeDouble(L"HoldDuration", g_App.holdDuration.load());
    writeDouble(L"HoldPause", g_App.holdPause.load());
    file << L"AlwaysOnTop=" << (g_App.alwaysOnTop.load() ? 1 : 0) << L"\n";
    file << L"X=" << x << L"\n";
    file << L"Y=" << y << L"\n";
    file.flush();
}

static void MarkDirty() {
    if (!g_Ui.settingsReady || !g_Ui.hwnd) return;
    SetTimer(g_Ui.hwnd, TIMER_SAVE, 650, nullptr);
}

// -----------------------------------------------------------------------------
// Fonts
// -----------------------------------------------------------------------------
static int CALLBACK FontEnumProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *reinterpret_cast<bool*>(lp) = true;
    return 0;
}

static bool FontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, FontEnumProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static HFONT MakeUiFont(int px, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(px, g_Ui.dpi, 96);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(lf.lfFaceName, g_Ui.fontFace);
    return CreateFontIndirectW(&lf);
}

static void DestroyFonts() {
    HFONT* fonts[] = {
        &g_Ui.fonts.title, &g_Ui.fonts.body, &g_Ui.fonts.bodyMed,
        &g_Ui.fonts.section, &g_Ui.fonts.value, &g_Ui.fonts.status,
        &g_Ui.fonts.hotkey, &g_Ui.fonts.chip, &g_Ui.fonts.tooltip,
        &g_Ui.fonts.icon
    };
    for (HFONT* f : fonts) {
        if (*f) { DeleteObject(*f); *f = nullptr; }
    }
}

static void CreateUiFonts() {
    DestroyFonts();
    g_Ui.fonts.title = MakeUiFont(14, FW_SEMIBOLD);
    g_Ui.fonts.body = MakeUiFont(13, FW_NORMAL);
    g_Ui.fonts.bodyMed = MakeUiFont(13, FW_SEMIBOLD);
    g_Ui.fonts.section = MakeUiFont(10, FW_SEMIBOLD);
    g_Ui.fonts.value = MakeUiFont(15, FW_SEMIBOLD);
    g_Ui.fonts.status = MakeUiFont(11, FW_SEMIBOLD);
    g_Ui.fonts.hotkey = MakeUiFont(12, FW_SEMIBOLD);
    g_Ui.fonts.chip = MakeUiFont(10, FW_SEMIBOLD);
    g_Ui.fonts.tooltip = MakeUiFont(11, FW_NORMAL);
    g_Ui.fonts.icon = MakeUiFont(11, FW_BOLD);
}

// -----------------------------------------------------------------------------
// Animation
// -----------------------------------------------------------------------------
static bool AnyAnimActive() {
    for (int i = 1; i < HotCount; ++i)
        if (std::fabs(g_Ui.hover[i].value - g_Ui.hover[i].target) > 0.002f) return true;
    if (std::fabs(g_Ui.modePill.value - g_Ui.modePill.target) > 0.002f) return true;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(g_Ui.targetSel[i].value - g_Ui.targetSel[i].target) > 0.002f) return true;
    if (std::fabs(g_Ui.pauseReveal.value - g_Ui.pauseReveal.target) > 0.002f) return true;
    if (std::fabs(g_Ui.valueFlash.value - g_Ui.valueFlash.target) > 0.002f) return true;
    if (g_Ui.statusColor.GetA() != g_Ui.statusTarget.GetA() ||
        g_Ui.statusColor.GetR() != g_Ui.statusTarget.GetR() ||
        g_Ui.statusColor.GetG() != g_Ui.statusTarget.GetG() ||
        g_Ui.statusColor.GetB() != g_Ui.statusTarget.GetB()) return true;
    return false;
}

static Color StatusColor() {
    if (g_App.actionType.load() == 0)
        return g_App.running.load() ? A(Theme::Success) : A(Theme::Muted);
    return g_App.running.load() ? A(Theme::Success) : A(Theme::Warning);
}

static void EnsureAnimTimer() {
    if (!g_Ui.hwnd || IsIconic(g_Ui.hwnd)) return;
    if (!g_Ui.animTimer) {
        g_Ui.lastAnimTick = GetTickCount64();
        SetTimer(g_Ui.hwnd, TIMER_ANIM, 15, nullptr);
        g_Ui.animTimer = true;
    }
}

static void RefreshAnimTargets() {
    for (int i = 1; i < HotCount; ++i) {
        bool on = (g_Ui.hot == (HotId)i) && HotEnabled((HotId)i);
        g_Ui.hover[i].target = on ? 1.0f : 0.0f;
    }
    g_Ui.modePill.target = (g_App.actionType.load() == 1) ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i)
        g_Ui.targetSel[i].target = (g_App.clickMode.load() == i) ? 1.0f : 0.0f;
    g_Ui.pauseReveal.target = (g_App.actionType.load() == 1) ? 1.0f : 0.0f;
    g_Ui.statusTarget = StatusColor();
    if (AnyAnimActive()) EnsureAnimTimer();
}

static void StepAnims(float dt) {
    auto smooth = [dt](AnimValue& a, float speed) {
        if (a.value == a.target) return;
        float k = 1.0f - std::exp(-speed * dt);
        a.value += (a.target - a.value) * k;
        if (std::fabs(a.target - a.value) < 0.0015f) a.value = a.target;
    };
    for (int i = 1; i < HotCount; ++i) smooth(g_Ui.hover[i], 18.0f);
    smooth(g_Ui.modePill, 22.0f);
    for (int i = 0; i < 3; ++i) smooth(g_Ui.targetSel[i], 22.0f);
    smooth(g_Ui.pauseReveal, 18.0f);
    smooth(g_Ui.valueFlash, 12.0f);

    float k = 1.0f - std::exp(-14.0f * dt);
    g_Ui.statusColor = LerpColor(g_Ui.statusColor, g_Ui.statusTarget, k);
    if (std::abs((int)g_Ui.statusColor.GetR() - (int)g_Ui.statusTarget.GetR()) <= 1 &&
        std::abs((int)g_Ui.statusColor.GetG() - (int)g_Ui.statusTarget.GetG()) <= 1 &&
        std::abs((int)g_Ui.statusColor.GetB() - (int)g_Ui.statusTarget.GetB()) <= 1)
        g_Ui.statusColor = g_Ui.statusTarget;
}

// -----------------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------------
static void BuildLayout() {
    if (!g_Ui.hwnd) return;
    RECT rc{};
    GetClientRect(g_Ui.hwnd, &rc);
    g_L.width = rc.right;
    g_L.height = rc.bottom;
    g_L.contentW = S(kContentWidthDip);
    g_L.contentX = (g_L.width - g_L.contentW) / 2;

    const int x = g_L.contentX;
    const int cw = g_L.contentW;

    // Title buttons
    int btnW = S(30);
    int btnH = S(28);
    int gap = S(2);
    int right = g_L.width - S(8);
    g_L.closeBtn = { right - btnW, S(6), right, S(6) + btnH };
    right -= btnW + gap;
    g_L.minBtn = { right - btnW, S(6), right, S(6) + btnH };
    right -= btnW + gap;
    g_L.pin = { right - btnW, S(6), right, S(6) + btnH };
    g_L.titleIcon = { x, S(10), x + S(20), S(10) + S(20) };
    g_L.titleText = { x + S(28), S(0), g_L.pin.left - S(8), S(kTitleHeightDip) };

    int y = S(kTitleHeightDip + 12);
    auto sectionControl = [&](int labelH, int controlH) {
        y += labelH + S(6);
        int top = y;
        y += controlH + S(12);
        return top;
    };

    // Action mode
    g_L.modeLabel = { x, y, x + cw, y + S(12) };
    int modeTop = sectionControl(S(12), S(32));
    g_L.modeBox = { x, modeTop, x + cw, modeTop + S(32) };
    {
        int inner = S(2);
        int igap = S(4);
        int iw = cw - inner * 2;
        int segW = (iw - igap) / 2;
        int sx = x + inner;
        g_L.modeSeg[0] = { sx, modeTop + inner, sx + segW, modeTop + inner + S(28) };
        sx += segW + igap;
        g_L.modeSeg[1] = { sx, modeTop + inner, sx + segW, modeTop + inner + S(28) };
    }

    // Target
    g_L.targetLabel = { x, y, x + cw, y + S(12) };
    int targetTop = sectionControl(S(12), S(32));
    {
        int tgap = S(8);
        int w = (cw - tgap * 2) / 3;
        int lastW = cw - (w + tgap) * 2;
        int tx = x;
        g_L.target[0] = { tx, targetTop, tx + w, targetTop + S(32) };
        tx += w + tgap;
        g_L.target[1] = { tx, targetTop, tx + w, targetTop + S(32) };
        tx += w + tgap;
        g_L.target[2] = { tx, targetTop, tx + lastW, targetTop + S(32) };
    }

    // Primary value (CPS or hold time)
    g_L.speedLabel = { x, y, x + cw, y + S(12) };
    int speedTop = sectionControl(S(12), S(32));
    {
        int bw = S(44);
        int sg = S(8);
        int vw = cw - bw * 2 - sg * 2;
        g_L.speedMinus = { x, speedTop, x + bw, speedTop + S(32) };
        g_L.speedValue = { x + bw + sg, speedTop, x + bw + sg + vw, speedTop + S(32) };
        g_L.speedPlus = { x + bw + sg + vw + sg, speedTop, x + cw, speedTop + S(32) };
    }

    // Hold pause
    if (g_App.actionType.load() == 1) {
        g_L.pauseLabel = { x, y, x + cw, y + S(12) };
        int pauseTop = sectionControl(S(12), S(32));
        int bw = S(44);
        int sg = S(8);
        int vw = cw - bw * 2 - sg * 2;
        g_L.pauseMinus = { x, pauseTop, x + bw, pauseTop + S(32) };
        g_L.pauseValue = { x + bw + sg, pauseTop, x + bw + sg + vw, pauseTop + S(32) };
        g_L.pausePlus = { x + bw + sg + vw + sg, pauseTop, x + cw, pauseTop + S(32) };
    } else {
        g_L.pauseLabel = {};
        g_L.pauseMinus = {};
        g_L.pauseValue = {};
        g_L.pausePlus = {};
    }

    // Hotkeys
    g_L.hotkeyLabel = { x, y, x + cw, y + S(12) };
    y += S(12) + S(6);
    {
        int hgap = S(8);
        int w = (cw - hgap) / 2;
        g_L.hotkey[0] = { x, y, x + w, y + S(36) };
        g_L.hotkey[1] = { x + w + hgap, y, x + cw, y + S(36) };
    }
    y += S(36);

    // Status bar
    int statusH = S(24);
    int bottom = S(10);
    int statusTop = g_L.height - bottom - statusH;
    int sepY = statusTop - S(7);
    g_L.statusSep = { x, sepY, x + cw, sepY + 1 };
    g_L.statusArea = { x, statusTop, x + cw, statusTop + statusH };
    g_L.statusDot = { x, statusTop + statusH / 2 - S(3), x + S(6), statusTop + statusH / 2 + S(3) };
    g_L.statusState = { x + S(12), statusTop, x + cw, statusTop + statusH };
    g_L.statusClicks = { x, statusTop, x + cw, statusTop + statusH };
}

static void ResizeForMode(int mode) {
    if (!g_Ui.hwnd) return;
    RECT wr{};
    GetWindowRect(g_Ui.hwnd, &wr);
    int newW = S(kWindowWidthDip);
    int newH = S(mode == 1 ? kHoldHeightDip : kAutoHeightDip);

    HMONITOR mon = MonitorFromWindow(g_Ui.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    int top = wr.top;
    if (GetMonitorInfoW(mon, &mi)) {
        if (top + newH > mi.rcWork.bottom)
            top = std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - newH);
    }
    SetWindowPos(g_Ui.hwnd, nullptr, wr.left, top, newW, newH,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

// -----------------------------------------------------------------------------
// Input worker
// -----------------------------------------------------------------------------
static void PreciseSleepUs(long long us) {
    if (us <= 0) return;
    if (g_HiResTimer) {
        LARGE_INTEGER due{};
        due.QuadPart = -(us * 10); // 100 ns units
        if (SetWaitableTimer(g_HiResTimer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(g_HiResTimer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

static bool WaitAbortable(double seconds) {
    using clock = std::chrono::steady_clock;
    if (seconds <= 0.0) return g_App.running.load() && g_App.appActive.load();
    auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(seconds));
    std::unique_lock<std::mutex> lock(g_WorkerMutex);
    while (g_App.appActive.load() && g_App.running.load()) {
        auto now = clock::now();
        if (now >= deadline) return true;
        auto remain = deadline - now;
        if (remain > std::chrono::milliseconds(12)) {
            g_WorkerCv.wait_for(lock, std::chrono::milliseconds(10));
        } else {
            lock.unlock();
            PreciseSleepUs((long long)std::chrono::duration_cast<std::chrono::microseconds>(remain).count());
            lock.lock();
        }
    }
    return false;
}

static bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
    using clock = std::chrono::steady_clock;
    std::unique_lock<std::mutex> lock(g_WorkerMutex);
    while (g_App.appActive.load() && g_App.running.load()) {
        auto now = clock::now();
        if (now >= deadline) return true;
        auto remain = deadline - now;
        if (remain > std::chrono::milliseconds(10)) {
            g_WorkerCv.wait_for(lock, std::chrono::milliseconds(10));
        } else {
            lock.unlock();
            PreciseSleepUs((long long)std::chrono::duration_cast<std::chrono::microseconds>(remain).count());
            lock.lock();
        }
    }
    return false;
}

static void SendButton(int side, bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    if (side == 1)
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

static void RunAutoClick() {
    using clock = std::chrono::steady_clock;
    bool alternateRight = false;
    auto next = clock::now();

    while (g_App.appActive.load() && g_App.running.load() && g_App.actionType.load() == 0) {
        double cps = ClampD(g_App.cps.load(), 1.0, 100.0);
        long long intervalUs = (long long)std::llround(1000000.0 / cps);

        int mode = g_App.clickMode.load();
        int side = (mode == 2) ? (alternateRight ? 1 : 0) : (mode == 1 ? 1 : 0);
        SendButton(side, true);
        PreciseSleepUs(2000);
        SendButton(side, false);
        if (mode == 2) alternateRight = !alternateRight;
        g_App.clickCount.fetch_add(1, std::memory_order_relaxed);

        next += std::chrono::microseconds(intervalUs);
        auto now = clock::now();
        if (next < now - std::chrono::milliseconds(5)) next = now;
        if (!WaitUntil(next)) break;
    }
}

static void RunHold() {
    while (g_App.appActive.load() && g_App.running.load() && g_App.actionType.load() == 1) {
        int mode = g_App.clickMode.load();
        int side = (mode == 1) ? 1 : 0;

        SendButton(side, true);
        g_App.holding = true;
        g_App.clickCount.fetch_add(1, std::memory_order_relaxed);
        PostMessageW(g_Ui.hwnd, WM_APP_STATE, 0, 0);

        double holdSec = ClampD(g_App.holdDuration.load(), 0.05, 600.0);
        bool completed = WaitAbortable(holdSec);

        SendButton(side, false);
        g_App.holding = false;
        PostMessageW(g_Ui.hwnd, WM_APP_STATE, 0, 0);
        if (!completed) break;

        double pauseSec = ClampD(g_App.holdPause.load(), 0.0, 600.0);
        if (pauseSec > 0.0005 && !WaitAbortable(pauseSec)) break;
    }

    // Never leave a mouse button stuck down when stopping.
    if (g_App.holding.exchange(false)) {
        int side = (g_App.clickMode.load() == 1) ? 1 : 0;
        SendButton(side, false);
        PostMessageW(g_Ui.hwnd, WM_APP_STATE, 0, 0);
    }
}

static void ClickerThread() {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000001
#endif
    g_HiResTimer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_HiResTimer)
        g_HiResTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    while (g_App.appActive.load()) {
        if (!g_App.running.load()) {
            std::unique_lock<std::mutex> lock(g_WorkerMutex);
            g_WorkerCv.wait(lock, [] {
                return !g_App.appActive.load() || g_App.running.load();
            });
            continue;
        }
        if (g_App.actionType.load() == 0) RunAutoClick();
        else RunHold();
    }

    if (g_HiResTimer) {
        CloseHandle(g_HiResTimer);
        g_HiResTimer = nullptr;
    }
}

// -----------------------------------------------------------------------------
// Global hotkeys
// -----------------------------------------------------------------------------
static bool g_KeyHeld[256] = {};

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT vk = kb->vkCode;
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (!g_KeyHeld[vk & 0xFF]) {
                g_KeyHeld[vk & 0xFF] = true;
                if (g_Ui.bindingTarget != 0) {
                    PostMessageW(g_Ui.hwnd, WM_APP_BINDKEY, vk, 0);
                    return 1; // consume while rebinding
                }
                if (g_Ui.editField == EditNone) {
                    int key = (int)vk;
                    if (key == g_App.toggleKey.load() || key == g_App.exitKey.load())
                        PostMessageW(g_Ui.hwnd, WM_APP_HOTKEY, vk, 0);
                }
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            g_KeyHeld[vk & 0xFF] = false;
        }
    }
    return CallNextHookEx(g_Ui.kbdHook, code, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_XBUTTONDOWN) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        WORD btn = HIWORD(ms->mouseData);
        int key = (btn == XBUTTON1) ? kMouse4 : (btn == XBUTTON2 ? kMouse5 : 0);
        if (key) {
            if (g_Ui.bindingTarget != 0) {
                PostMessageW(g_Ui.hwnd, WM_APP_BINDKEY, key, 0);
                return 1;
            }
            if (g_Ui.editField == EditNone) {
                if (key == g_App.toggleKey.load() || key == g_App.exitKey.load())
                    PostMessageW(g_Ui.hwnd, WM_APP_HOTKEY, key, 0);
            }
        }
    }
    return CallNextHookEx(g_Ui.mouseHook, code, wParam, lParam);
}

// -----------------------------------------------------------------------------
// UI actions
// -----------------------------------------------------------------------------
static void InvalidateAll() {
    if (g_Ui.hwnd) InvalidateRect(g_Ui.hwnd, nullptr, FALSE);
}

static void UpdateTickTimer() {
    if (!g_Ui.hwnd) return;
    bool need = !IsIconic(g_Ui.hwnd) &&
        (g_App.running.load() ||
         g_Ui.editField != EditNone ||
         (g_Ui.hot == HotPin && !g_Ui.pinTooltip) ||
         GetTickCount64() < g_Ui.bindMsgUntil);
    if (need) {
        if (!g_Ui.tickTimer) {
            SetTimer(g_Ui.hwnd, TIMER_TICK, 50, nullptr);
            g_Ui.tickTimer = true;
        }
    } else if (g_Ui.tickTimer) {
        KillTimer(g_Ui.hwnd, TIMER_TICK);
        g_Ui.tickTimer = false;
    }
}

static bool HotEnabled(HotId id) {
    if (id == HotNone) return false;
    if ((id == HotMode0 || id == HotMode1) && g_App.running.load()) return false;
    if ((id == HotPauseMinus || id == HotPauseValue || id == HotPausePlus) &&
        g_App.actionType.load() == 0) return false;
    return true;
}

static HotId HitTest(POINT pt) {
    if (PtInRect(&g_L.pin, pt)) return HotPin;
    if (PtInRect(&g_L.minBtn, pt)) return HotMin;
    if (PtInRect(&g_L.closeBtn, pt)) return HotClose;

    if (PtInRect(&g_L.modeBox, pt) && HotEnabled(HotMode0))
        return pt.x < g_L.modeSeg[1].left ? HotMode0 : HotMode1;

    for (int i = 0; i < 3; ++i)
        if (PtInRect(&g_L.target[i], pt)) return (HotId)(HotTarget0 + i);

    if (PtInRect(&g_L.speedMinus, pt)) return HotSpeedMinus;
    if (PtInRect(&g_L.speedValue, pt)) return HotSpeedValue;
    if (PtInRect(&g_L.speedPlus, pt)) return HotSpeedPlus;

    if (g_App.actionType.load() == 1) {
        if (PtInRect(&g_L.pauseMinus, pt)) return HotPauseMinus;
        if (PtInRect(&g_L.pauseValue, pt)) return HotPauseValue;
        if (PtInRect(&g_L.pausePlus, pt)) return HotPausePlus;
    }

    if (PtInRect(&g_L.hotkey[0], pt)) return HotToggleKey;
    if (PtInRect(&g_L.hotkey[1], pt)) return HotExitKey;
    return HotNone;
}

static void RefreshHotFromCursor() {
    if (!g_Ui.hwnd) return;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(g_Ui.hwnd, &pt);
    HotId h = HitTest(pt);
    if (h != g_Ui.hot) {
        g_Ui.hot = h;
        g_Ui.pinTooltip = false;
        if (h == HotPin) g_Ui.pinHoverStart = GetTickCount64();
        RefreshAnimTargets();
        UpdateTickTimer();
        InvalidateAll();
    }
}

static void SetActionMode(int mode) {
    if (g_App.running.load()) return;
    mode = (mode == 1) ? 1 : 0;
    if (g_App.actionType.load() == mode) return;
    if (g_Ui.editField != EditNone) CommitEdit();

    g_App.actionType = mode;
    if (mode == 0) g_Ui.pauseReveal.value = 0.0f;
    else g_Ui.pauseReveal.value = 0.0f;
    ResizeForMode(mode);
    RefreshAnimTargets();
    RefreshHotFromCursor();
    UpdateTickTimer();
    MarkDirty();
    InvalidateAll();
}

static void SetClickMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    if (g_App.clickMode.load() == mode) return;
    g_App.clickMode = mode;
    RefreshAnimTargets();
    MarkDirty();
    InvalidateAll();
}

static void StepNumeric(HotId id) {
    if (g_Ui.editField != EditNone) CommitEdit();

    if (id == HotSpeedMinus || id == HotSpeedPlus) {
        if (g_App.actionType.load() == 0) {
            double v = ClampD(g_App.cps.load(), 1.0, 100.0);
            v += (id == HotSpeedPlus) ? 1.0 : -1.0;
            g_App.cps = ClampD(v, 1.0, 100.0);
        } else {
            double v = ClampD(g_App.holdDuration.load(), 0.1, 60.0);
            v += (id == HotSpeedPlus) ? 0.5 : -0.5;
            g_App.holdDuration = ClampD(v, 0.1, 60.0);
        }
    } else if (id == HotPauseMinus || id == HotPausePlus) {
        double v = ClampD(g_App.holdPause.load(), 0.0, 60.0);
        v += (id == HotPausePlus) ? 0.1 : -0.1;
        g_App.holdPause = ClampD(v, 0.0, 60.0);
    } else {
        return;
    }

    g_Ui.valueFlash.value = 1.0f;
    g_Ui.valueFlash.target = 0.0f;
    EnsureAnimTimer();
    MarkDirty();
    InvalidateAll();
}

static void BeginEdit(EditField field) {
    if (field == EditNone) return;
    if (g_Ui.editField == field) return;
    if (g_Ui.editField != EditNone) CommitEdit();

    g_Ui.editField = field;
    double v = (field == EditSpeed)
        ? (g_App.actionType.load() == 0 ? g_App.cps.load() : g_App.holdDuration.load())
        : g_App.holdPause.load();
    std::wstring s = FormatValue(v);
    wcsncpy_s(g_Ui.editBuf, s.c_str(), _TRUNCATE);
    g_Ui.editLen = (int)s.size();
    g_Ui.caretOn = true;
    g_Ui.lastCaretOn = true;
    UpdateTickTimer();
    InvalidateAll();
}

static void CommitEdit() {
    if (g_Ui.editField == EditNone) return;

    wchar_t* end = nullptr;
    double v = wcstod(g_Ui.editBuf, &end);
    bool valid = end != g_Ui.editBuf && std::isfinite(v);

    if (valid) {
        if (g_Ui.editField == EditSpeed) {
            if (g_App.actionType.load() == 0)
                g_App.cps = ClampD(v, 1.0, 100.0);
            else
                g_App.holdDuration = ClampD(v, 0.1, 60.0);
        } else {
            g_App.holdPause = ClampD(v, 0.0, 60.0);
        }
        MarkDirty();
    }

    g_Ui.editField = EditNone;
    g_Ui.editLen = 0;
    g_Ui.editBuf[0] = 0;
    UpdateTickTimer();
    InvalidateAll();
}

static void CancelEdit() {
    if (g_Ui.editField == EditNone) return;
    g_Ui.editField = EditNone;
    g_Ui.editLen = 0;
    g_Ui.editBuf[0] = 0;
    UpdateTickTimer();
    InvalidateAll();
}

static void BeginBind(int target) {
    if (g_Ui.editField != EditNone) CommitEdit();
    g_Ui.bindingTarget = (target == 1) ? 1 : 2;
    g_Ui.bindMsgUntil = 0;
    UpdateTickTimer();
    InvalidateAll();
}

static void CompleteBind(int vk) {
    if (g_Ui.bindingTarget == 0) return;

    if (vk == VK_ESCAPE) {
        g_Ui.bindingTarget = 0;
        g_Ui.bindMsgUntil = 0;
        UpdateTickTimer();
        InvalidateAll();
        return;
    }

    bool valid = (vk >= 1 && vk <= 254) || vk == kMouse4 || vk == kMouse5;
    if (!valid) return;

    int target = g_Ui.bindingTarget;
    int other = (target == 1) ? g_App.exitKey.load() : g_App.toggleKey.load();
    if (vk == other) {
        g_Ui.bindMsgUntil = GetTickCount64() + 1300;
        UpdateTickTimer();
        InvalidateAll();
        return;
    }

    if (target == 1) g_App.toggleKey = vk;
    else g_App.exitKey = vk;

    g_Ui.bindingTarget = 0;
    g_Ui.bindMsgUntil = 0;
    UpdateTickTimer();
    MarkDirty();
    InvalidateAll();
}

static void ToggleRunning() {
    bool next = !g_App.running.load();
    g_App.running = next;
    if (!next) g_App.holding = false;
    g_WorkerCv.notify_all();
    RefreshAnimTargets();
    UpdateTickTimer();
    InvalidateAll();
}

static void RequestExit() {
    g_App.running = false;
    g_App.appActive = false;
    g_WorkerCv.notify_all();
    if (g_Ui.hwnd) PostMessageW(g_Ui.hwnd, WM_CLOSE, 0, 0);
}

static std::wstring KeyName(int vk) {
    if (vk == kMouse4) return L"Mouse 4";
    if (vk == kMouse5) return L"Mouse 5";
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_SHIFT: return L"Shift";
    case VK_CONTROL: return L"Ctrl";
    case VK_MENU: return L"Alt";
    case VK_CAPITAL: return L"Caps Lock";
    case VK_NUMLOCK: return L"Num Lock";
    case VK_SCROLL: return L"Scroll Lock";
    default: break;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    wchar_t name[64] = {};
    if (GetKeyNameTextW((LONG)(scan << 16), name, 64) > 0)
        return name;
    return L"Key " + std::to_wstring(vk);
}

// -----------------------------------------------------------------------------
// Drawing helpers
// -----------------------------------------------------------------------------
static void AddRoundedPath(GraphicsPath& path, const RectF& r, float radius) {
    float d = std::min(radius * 2.0f, std::min(r.Width, r.Height));
    if (d <= 0.0f) { path.AddRectangle(r); return; }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRounded(Graphics& g, const RECT& r, float radius, Color fill) {
    if (r.right <= r.left || r.bottom <= r.top || fill.GetA() == 0) return;
    RectF rf((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
    GraphicsPath path;
    AddRoundedPath(path, rf, radius);
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
}

static void StrokeRounded(Graphics& g, const RECT& r, float radius, Color stroke, float width) {
    if (r.right <= r.left || r.bottom <= r.top || stroke.GetA() == 0) return;
    float inset = width * 0.5f;
    RectF rf((REAL)r.left + inset, (REAL)r.top + inset,
        (REAL)(r.right - r.left) - inset * 2.0f,
        (REAL)(r.bottom - r.top) - inset * 2.0f);
    if (rf.Width <= 0.0f || rf.Height <= 0.0f) return;
    GraphicsPath path;
    AddRoundedPath(path, rf, radius);
    Pen pen(stroke, width);
    g.DrawPath(&pen, &path);
}

static void Panel(Graphics& g, const RECT& r, float radius, Color fill, Color border, float borderWidth = 1.0f) {
    FillRounded(g, r, radius, fill);
    StrokeRounded(g, r, radius, border, borderWidth);
}

static Color AlphaColor(const Color& c, float alpha) {
    float a = ClampF(alpha, 0.0f, 1.0f) * (float)c.GetA();
    return Color((BYTE)std::lround(a), c.GetR(), c.GetG(), c.GetB());
}

static RECT ShiftRect(const RECT& r, int dy) {
    return RECT{ r.left, r.top + dy, r.right, r.bottom + dy };
}

static float SF(int dip) {
    return (float)S(dip);
}

static int MeasureText(HDC dc, HFONT font, const std::wstring& text) {
    if (!dc || !font || text.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static RECT ChipRect(HDC dc, const RECT& field, const std::wstring& key) {
    int w = MeasureText(dc, g_Ui.fonts.chip, key) + S(14);
    int h = S(22);
    int cy = (field.top + field.bottom) / 2;
    int right = field.right - S(8);
    return RECT{ right - w, cy - h / 2, right, cy + h / 2 };
}

struct PauseRects {
    RECT label;
    RECT minus;
    RECT value;
    RECT plus;
    int dy;
    float alpha;
};

static PauseRects GetPauseRects() {
    PauseRects p{};
    p.alpha = g_Ui.pauseReveal.value;
    p.dy = (int)std::lround((1.0f - p.alpha) * (float)(-S(6)));
    p.label = ShiftRect(g_L.pauseLabel, p.dy);
    p.minus = ShiftRect(g_L.pauseMinus, p.dy);
    p.value = ShiftRect(g_L.pauseValue, p.dy);
    p.plus = ShiftRect(g_L.pausePlus, p.dy);
    return p;
}

static void DrawPlusMinus(Graphics& g, const RECT& r, bool plus, Color color) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float half = (float)S(5);
    Pen pen(color, std::max(1.0f, 1.5f * g_Ui.scale));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - half, cy, cx + half, cy);
    if (plus) g.DrawLine(&pen, cx, cy - half, cx, cy + half);
}

static void DrawPinIcon(Graphics& g, const RECT& r, Color color) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float s = g_Ui.scale;
    SolidBrush brush(color);
    g.FillEllipse(&brush, cx - 3.2f * s, cy - 5.8f * s, 6.4f * s, 6.4f * s);
    Pen pen(color, std::max(1.0f, 1.6f * s));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx, cy + 0.2f * s, cx, cy + 5.2f * s);
}

static void DrawMinIcon(Graphics& g, const RECT& r, Color color) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float half = (float)S(4);
    Pen pen(color, std::max(1.0f, 1.5f * g_Ui.scale));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - half, cy, cx + half, cy);
}

static void DrawCloseIcon(Graphics& g, const RECT& r, Color color) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    float half = (float)S(4);
    Pen pen(color, std::max(1.0f, 1.5f * g_Ui.scale));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - half, cy - half, cx + half, cy + half);
    g.DrawLine(&pen, cx - half, cy + half, cx + half, cy - half);
}

static std::wstring HotkeyText(int target) {
    bool binding = (g_Ui.bindingTarget == target);
    if (binding) {
        if (GetTickCount64() < g_Ui.bindMsgUntil) return L"Key already in use";
        return L"Press any key...";
    }
    return KeyName(target == 1 ? g_App.toggleKey.load() : g_App.exitKey.load());
}

// -----------------------------------------------------------------------------
// Shapes pass
// -----------------------------------------------------------------------------
static void DrawUiShapes(Graphics& g, HDC dc) {
    g.Clear(A(Theme::Bg));

    // Title icon and divider
    FillRounded(g, g_L.titleIcon, SF(6), A(Theme::Accent));
    {
        Pen divider(A(Theme::Divider), 1.0f);
        g.DrawLine(&divider, 0.0f, (REAL)S(kTitleHeightDip) + 0.5f,
            (REAL)g_L.width, (REAL)S(kTitleHeightDip) + 0.5f);
    }

    // Title bar buttons
    auto titleButton = [&](const RECT& r, HotId id, bool danger) {
        float hover = g_Ui.hover[id].value;
        bool pressed = (g_Ui.pressed == id);
        if (hover <= 0.002f && !pressed) return;
        Color fill = danger ? A(Theme::Danger) : A(Theme::Elevated);
        float a = pressed ? 1.0f : hover;
        fill = AlphaColor(fill, a);
        if (pressed) fill = LerpColor(fill, A(0x000000), 0.12f);
        FillRounded(g, r, SF(6), fill);
    };
    titleButton(g_L.pin, HotPin, false);
    titleButton(g_L.minBtn, HotMin, false);
    titleButton(g_L.closeBtn, HotClose, true);

    {
        float hover = g_Ui.hover[HotPin].value;
        bool active = g_App.alwaysOnTop.load();
        Color icon = active ? A(Theme::AccentHover) : LerpColor(A(Theme::SubText), A(Theme::Text), hover);
        DrawPinIcon(g, g_L.pin, icon);
    }
    {
        float hover = g_Ui.hover[HotMin].value;
        DrawMinIcon(g, g_L.minBtn, LerpColor(A(Theme::SubText), A(Theme::Text), hover));
    }
    {
        float hover = g_Ui.hover[HotClose].value;
        bool pressed = (g_Ui.pressed == HotClose);
        Color icon = (hover > 0.05f || pressed) ? A(0xFFFFFF) : A(Theme::SubText);
        DrawCloseIcon(g, g_L.closeBtn, icon);
    }

    // Action mode segmented control
    {
        Panel(g, g_L.modeBox, SF(8), A(Theme::Panel), A(Theme::Border), 1.0f);
        bool enabled = !g_App.running.load();
        int selected = g_App.actionType.load();
        for (int i = 0; i < 2; ++i) {
            if (!enabled || i == selected) continue;
            float hover = g_Ui.hover[HotMode0 + i].value;
            if (hover > 0.002f)
                FillRounded(g, g_L.modeSeg[i], SF(6), AlphaColor(A(Theme::Elevated), hover * 0.7f));
        }

        RECT pill = g_L.modeSeg[0];
        int x0 = g_L.modeSeg[0].left;
        int x1 = g_L.modeSeg[1].left;
        pill.left = (int)std::lround((double)x0 + ((double)x1 - (double)x0) * (double)g_Ui.modePill.value);
        pill.right = pill.left + (g_L.modeSeg[0].right - g_L.modeSeg[0].left);

        Color pillColor = A(Theme::Accent);
        if (!enabled) pillColor = LerpColor(pillColor, A(Theme::Bg), 0.45f);
        FillRounded(g, pill, SF(6), pillColor);
    }

    // Target buttons
    for (int i = 0; i < 3; ++i) {
        float sel = g_Ui.targetSel[i].value;
        float hover = g_Ui.hover[HotTarget0 + i].value;
        bool pressed = (g_Ui.pressed == (HotId)(HotTarget0 + i));

        Color fill = LerpColor(A(Theme::Button), A(Theme::Accent), sel);
        Color hoverFill = LerpColor(A(Theme::ButtonHover), A(Theme::AccentHover), sel);
        fill = LerpColor(fill, hoverFill, hover);
        if (pressed) fill = LerpColor(fill, A(0x000000), 0.12f);

        Color border = LerpColor(A(Theme::Border), A(Theme::Accent), sel);
        border = LerpColor(border, A(Theme::AccentHover), hover * 0.55f);
        Panel(g, g_L.target[i], SF(6), fill, border, 1.0f);
    }

    // Primary numeric stepper
    auto stepButton = [&](const RECT& r, HotId id, bool plus, bool disabled, float alpha) {
        float hover = g_Ui.hover[id].value;
        bool pressed = (g_Ui.pressed == id);
        Color fill = LerpColor(A(Theme::Button), A(Theme::ButtonHover), hover);
        if (pressed) fill = LerpColor(fill, A(0x000000), 0.14f);
        if (disabled) fill = LerpColor(fill, A(Theme::Bg), 0.45f);
        FillRounded(g, r, SF(6), AlphaColor(fill, alpha));
        Color icon = disabled ? A(Theme::Muted) : LerpColor(A(Theme::Text), A(0xFFFFFF), hover * 0.6f);
        DrawPlusMinus(g, r, plus, AlphaColor(icon, alpha));
    };
    auto valueBox = [&](const RECT& r, HotId id, bool editing, bool disabled, float alpha) {
        float hover = g_Ui.hover[id].value;
        Color fill = disabled ? LerpColor(A(Theme::Elevated), A(Theme::Bg), 0.45f) : A(Theme::Elevated);
        Color border = LerpColor(A(Theme::Border), A(Theme::Accent), std::max(hover * 0.5f, g_Ui.valueFlash.value * 0.35f));
        if (editing) border = A(Theme::Accent);
        if (disabled) border = AlphaColor(A(Theme::Border), 0.55f);
        Panel(g, r, SF(6), AlphaColor(fill, alpha), AlphaColor(border, alpha), 1.0f);
    };

    stepButton(g_L.speedMinus, HotSpeedMinus, false, false, 1.0f);
    stepButton(g_L.speedPlus, HotSpeedPlus, true, false, 1.0f);
    valueBox(g_L.speedValue, HotSpeedValue, g_Ui.editField == EditSpeed, false, 1.0f);

    PauseRects pause = GetPauseRects();
    if (g_App.actionType.load() == 1 && pause.alpha > 0.002f) {
        stepButton(pause.minus, HotPauseMinus, false, false, pause.alpha);
        stepButton(pause.plus, HotPausePlus, true, false, pause.alpha);
        valueBox(pause.value, HotPauseValue, g_Ui.editField == EditPause, false, pause.alpha);
    }

    // Hotkey fields
    for (int i = 0; i < 2; ++i) {
        int target = i + 1;
        bool binding = (g_Ui.bindingTarget == target);
        float hover = g_Ui.hover[HotToggleKey + i].value;
        float dim = (g_Ui.bindingTarget != 0 && !binding) ? 0.55f : 1.0f;

        Color fill = A(Theme::Elevated);
        Color border = LerpColor(A(Theme::Border), A(Theme::Accent), hover * 0.55f);
        if (binding) {
            fill = LerpColor(A(Theme::Elevated), A(Theme::Accent), 0.14f);
            border = A(Theme::Accent);
        }
        Panel(g, g_L.hotkey[i], SF(7), AlphaColor(fill, dim), AlphaColor(border, dim), 1.0f);

        if (!binding) {
            std::wstring key = HotkeyText(target);
            RECT chip = ChipRect(dc, g_L.hotkey[i], key);
            FillRounded(g, chip, SF(5), AlphaColor(A(Theme::Button), dim));
            StrokeRounded(g, chip, SF(5), AlphaColor(A(Theme::Border), dim), 1.0f);
        }
    }

    // Status separator and dot
    {
        FillRounded(g, g_L.statusSep, 0.0f, A(Theme::Divider));
        float pulse = 1.0f;
        if (g_App.running.load()) {
            double t = (double)GetTickCount64() / 1000.0;
            pulse = 0.70f + 0.30f * (float)(0.5 + 0.5 * std::sin(t * 2.0 * 3.141592653589793 / 1.6));
        }
        Color dot = AlphaColor(g_Ui.statusColor, pulse);
        SolidBrush brush(dot);
        g.FillEllipse(&brush, (REAL)g_L.statusDot.left, (REAL)g_L.statusDot.top,
            (REAL)(g_L.statusDot.right - g_L.statusDot.left),
            (REAL)(g_L.statusDot.bottom - g_L.statusDot.top));
    }

    // Decorative corner marks
    {
        float inset = (float)S(5);
        float arm = (float)S(8);
        float w = (float)g_L.width;
        float h = (float)g_L.height;
        Pen pen(Color(185, 0x2C, 0x2C, 0x35), 1.0f);
        g.DrawLine(&pen, inset, inset, inset + arm, inset);
        g.DrawLine(&pen, inset, inset, inset, inset + arm);
        g.DrawLine(&pen, w - inset, inset, w - inset - arm, inset);
        g.DrawLine(&pen, w - inset, inset, w - inset, inset + arm);
        g.DrawLine(&pen, inset, h - inset, inset + arm, h - inset);
        g.DrawLine(&pen, inset, h - inset, inset, h - inset - arm);
        g.DrawLine(&pen, w - inset, h - inset, w - inset - arm, h - inset);
        g.DrawLine(&pen, w - inset, h - inset, w - inset, h - inset - arm);
    }

    // Outer border
    RECT outer{ 0, 0, g_L.width, g_L.height };
    StrokeRounded(g, outer, SF(8), Color(225, 0x30, 0x30, 0x38),
        std::max(1.0f, 1.0f * g_Ui.scale));

    // Pin tooltip
    if (g_Ui.pinTooltip && !g_Ui.bindingTarget) {
        std::wstring text = g_App.alwaysOnTop.load() ? L"Always on Top: On" : L"Always on Top: Off";
        int tw = MeasureText(dc, g_Ui.fonts.tooltip, text);
        int w = tw + S(18);
        int h = S(24);
        int right = g_L.pin.right;
        int left = right - w;
        int top = S(kTitleHeightDip) + S(6);
        RECT tip{ left, top, right, top + h };
        Panel(g, tip, SF(6), A(Theme::Elevated), A(Theme::Border), 1.0f);
    }
}

// -----------------------------------------------------------------------------
// Text pass
// -----------------------------------------------------------------------------
static bool g_EditFresh = false;
static HCURSOR g_CursorHand = nullptr;
static HCURSOR g_CursorArrow = nullptr;

static void DrawTextIn(HDC dc, HFONT font, const RECT& box,
                       const std::wstring& text, Color color, UINT flags) {
    if (!dc || !font || text.empty()) return;
    HGDIOBJ oldFont = SelectObject(dc, font);
    int oldBk = SetBkMode(dc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dc, RGB(color.GetR(), color.GetG(), color.GetB()));
    RECT r = box;
    DrawTextW(dc, text.c_str(), (int)text.size(), &r,
              flags | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldBk);
    SelectObject(dc, oldFont);
}

// Nhãn section in hoa, giãn chữ nhẹ.
static void DrawSectionLabel(HDC dc, const RECT& box, const std::wstring& text,
                             float alpha = 1.0f) {
    if (!dc || text.empty() || alpha <= 0.01f) return;
    Color color = LerpColor(A(Theme::Bg), A(Theme::SubText), ClampF(alpha, 0.0f, 1.0f));
    HGDIOBJ oldFont = SelectObject(dc, g_Ui.fonts.section);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(color.GetR(), color.GetG(), color.GetB()));

    SIZE whole{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &whole);
    int y = box.top + ((box.bottom - box.top) - whole.cy) / 2;
    int x = box.left;
    int tracking = std::max(1, (int)std::lround(0.7 * (double)g_Ui.scale));
    for (wchar_t ch : text) {
        TextOutW(dc, x, y, &ch, 1);
        SIZE sz{};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        x += sz.cx + tracking;
    }
    SelectObject(dc, oldFont);
}

static void DrawCaret(HDC dc, const RECT& box, const std::wstring& text) {
    if (!dc) return;
    if (((GetTickCount64() / 530) & 1) != 0) return;
    int tw = MeasureText(dc, g_Ui.fonts.value, text);
    int cx = (box.left + box.right) / 2;
    int x = cx + tw / 2 + S(2);
    int cy = (box.top + box.bottom) / 2;
    RECT caret{ x, cy - S(8), x + std::max(1, S(1)), cy + S(8) };
    HBRUSH brush = CreateSolidBrush(RGB(0x8B, 0x5C, 0xF6));
    FillRect(dc, &caret, brush);
    DeleteObject(brush);
}

static void DrawUiText(HDC dc) {
    if (!dc) return;

    // Title bar
    DrawTextIn(dc, g_Ui.fonts.icon, g_L.titleIcon, L"T", A(0xFFFFFF),
               DT_CENTER | DT_VCENTER);
    DrawTextIn(dc, g_Ui.fonts.title, g_L.titleText, L"Tuynf Auto Clicker",
               A(Theme::Text), DT_LEFT | DT_VCENTER);

    // Section labels
    DrawSectionLabel(dc, g_L.modeLabel, L"ACTION MODE");
    DrawSectionLabel(dc, g_L.targetLabel, L"TARGET BUTTON");
    DrawSectionLabel(dc, g_L.speedLabel,
                     g_App.actionType.load() == 0 ? L"SPEED (CPS)" : L"HOLD TIME (s)");

    bool holdingMode = g_App.actionType.load() == 1;
    PauseRects pause = GetPauseRects();
    if (holdingMode && pause.alpha > 0.01f)
        DrawSectionLabel(dc, pause.label, L"HOLD PAUSE (s)", pause.alpha);

    DrawSectionLabel(dc, g_L.hotkeyLabel, L"HOTKEYS");

    // Mode segmented labels
    {
        bool enabled = !g_App.running.load();
        int selected = g_App.actionType.load();
        const wchar_t* names[2] = { L"Auto Click", L"Hold Key" };
        for (int i = 0; i < 2; ++i) {
            Color color = (i == selected) ? A(Theme::Text)
                : (enabled ? A(Theme::SubText) : A(Theme::Muted));
            DrawTextIn(dc, g_Ui.fonts.bodyMed, g_L.modeSeg[i], names[i], color,
                       DT_CENTER | DT_VCENTER);
        }
    }

    // Target buttons
    {
        const wchar_t* names[3] = { L"Left", L"Right", L"Alternate" };
        int selected = g_App.clickMode.load();
        for (int i = 0; i < 3; ++i) {
            Color color = (i == selected) ? A(Theme::Text) : A(Theme::SubText);
            DrawTextIn(dc, g_Ui.fonts.bodyMed, g_L.target[i], names[i], color,
                       DT_CENTER | DT_VCENTER);
        }
    }

    // Numeric values
    auto drawValue = [&](const RECT& box, double value, EditField field) {
        if (g_Ui.editField == field) {
            std::wstring edit(g_Ui.editBuf);
            DrawTextIn(dc, g_Ui.fonts.value, box, edit, A(Theme::Text),
                       DT_CENTER | DT_VCENTER);
            DrawCaret(dc, box, edit);
        } else {
            DrawTextIn(dc, g_Ui.fonts.value, box, FormatValue(value), A(Theme::Text),
                       DT_CENTER | DT_VCENTER);
        }
    };

    drawValue(g_L.speedValue,
              holdingMode ? g_App.holdDuration.load() : g_App.cps.load(), EditSpeed);
    if (holdingMode && pause.alpha > 0.01f) {
        if (g_Ui.editField == EditPause) {
            std::wstring edit(g_Ui.editBuf);
            Color c = LerpColor(A(Theme::Bg), A(Theme::Text), pause.alpha);
            DrawTextIn(dc, g_Ui.fonts.value, pause.value, edit, c,
                       DT_CENTER | DT_VCENTER);
            DrawCaret(dc, pause.value, edit);
        } else {
            Color c = LerpColor(A(Theme::Bg), A(Theme::Text), pause.alpha);
            DrawTextIn(dc, g_Ui.fonts.value, pause.value,
                       FormatValue(g_App.holdPause.load()), c,
                       DT_CENTER | DT_VCENTER);
        }
    }

    // Hotkey fields
    for (int i = 0; i < 2; ++i) {
        int target = i + 1;
        bool binding = (g_Ui.bindingTarget == target);
        float dim = (g_Ui.bindingTarget != 0 && !binding) ? 0.55f : 1.0f;
        if (dim >= 0.999f) dim = 1.0f;

        if (binding) {
            bool duplicate = (GetTickCount64() < g_Ui.bindMsgUntil);
            std::wstring msg = duplicate ? L"Key already in use" : L"Press any key...";
            Color c = duplicate ? A(Theme::Danger) : A(Theme::AccentHover);
            DrawTextIn(dc, g_Ui.fonts.hotkey, g_L.hotkey[i], msg, c,
                       DT_CENTER | DT_VCENTER);
            continue;
        }

        Color labelColor = LerpColor(A(Theme::Bg), A(Theme::SubText), dim);
        RECT labelRect = g_L.hotkey[i];
        labelRect.left += S(10);
        DrawTextIn(dc, g_Ui.fonts.body, labelRect,
                   target == 1 ? L"Toggle" : L"Exit", labelColor,
                   DT_LEFT | DT_VCENTER);

        std::wstring key = HotkeyText(target);
        RECT chip = ChipRect(dc, g_L.hotkey[i], key);
        Color keyColor = LerpColor(A(Theme::Bg), A(Theme::Text), dim);
        DrawTextIn(dc, g_Ui.fonts.chip, chip, key, keyColor,
                   DT_CENTER | DT_VCENTER);
    }

    // Status bar
    {
        bool autoMode = g_App.actionType.load() == 0;
        bool running = g_App.running.load();
        std::wstring state = autoMode
            ? (running ? L"RUNNING" : L"STOPPED")
            : (running ? L"HOLDING" : L"READY TO HOLD");
        Color stateColor(255, g_Ui.statusColor.GetR(),
                         g_Ui.statusColor.GetG(), g_Ui.statusColor.GetB());
        DrawTextIn(dc, g_Ui.fonts.status, g_L.statusState, state, stateColor,
                   DT_LEFT | DT_VCENTER);
        if (autoMode) {
            std::wstring clicks = L"Clicks: " + FormatCount(g_App.clickCount.load());
            DrawTextIn(dc, g_Ui.fonts.status, g_L.statusClicks, clicks,
                       A(Theme::SubText), DT_RIGHT | DT_VCENTER);
        }
    }

    // Pin tooltip
    if (g_Ui.pinTooltip && g_Ui.bindingTarget == 0) {
        std::wstring text = g_App.alwaysOnTop.load()
            ? L"Always on Top: On" : L"Always on Top: Off";
        int tw = MeasureText(dc, g_Ui.fonts.tooltip, text);
        int w = tw + S(18);
        int h = S(24);
        int right = g_L.pin.right;
        int left = right - w;
        int top = S(kTitleHeightDip) + S(6);
        RECT tip{ left, top, right, top + h };
        DrawTextIn(dc, g_Ui.fonts.tooltip, tip, text, A(Theme::Text),
                   DT_CENTER | DT_VCENTER);
    }
}

static void DrawUi(Graphics& g, HDC dc) {
    DrawUiShapes(g, dc);
    g.Flush();
    DrawUiText(dc);
}

// -----------------------------------------------------------------------------
// Back buffer + paint
// -----------------------------------------------------------------------------
static void ReleaseBackBuffer() {
    if (g_Ui.memDC) {
        if (g_Ui.memOldBmp) SelectObject(g_Ui.memDC, g_Ui.memOldBmp);
        if (g_Ui.memBmp) DeleteObject(g_Ui.memBmp);
        DeleteDC(g_Ui.memDC);
    }
    g_Ui.memDC = nullptr;
    g_Ui.memBmp = nullptr;
    g_Ui.memOldBmp = nullptr;
    g_Ui.memW = 0;
    g_Ui.memH = 0;
}

static bool EnsureBackBuffer(HDC ref, int w, int h) {
    if (g_Ui.memDC && g_Ui.memW == w && g_Ui.memH == h) return true;
    ReleaseBackBuffer();

    g_Ui.memDC = CreateCompatibleDC(ref);
    if (!g_Ui.memDC) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_Ui.memBmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_Ui.memBmp) g_Ui.memBmp = CreateCompatibleBitmap(ref, w, h);
    if (!g_Ui.memBmp) {
        DeleteDC(g_Ui.memDC);
        g_Ui.memDC = nullptr;
        return false;
    }

    g_Ui.memOldBmp = (HBITMAP)SelectObject(g_Ui.memDC, g_Ui.memBmp);
    g_Ui.memW = w;
    g_Ui.memH = h;
    return true;
}

static void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;

    if (w > 0 && h > 0 && EnsureBackBuffer(dc, w, h)) {
        {
            Graphics g(g_Ui.memDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetCompositingQuality(CompositingQualityHighSpeed);
            DrawUi(g, g_Ui.memDC);
        }
        BitBlt(dc, 0, 0, w, h, g_Ui.memDC, 0, 0, SRCCOPY);
    } else {
        HBRUSH brush = CreateSolidBrush(RGB(0x0F, 0x0F, 0x12));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
    }
    EndPaint(hwnd, &ps);
}

// -----------------------------------------------------------------------------
// Window helpers
// -----------------------------------------------------------------------------
static bool g_UseWindowRegion = false;

static void ApplyWindowRegion() {
    if (!g_UseWindowRegion || !g_Ui.hwnd) return;
    RECT rc{};
    GetClientRect(g_Ui.hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1,
                                  S(16), S(16));
    if (rgn) SetWindowRgn(g_Ui.hwnd, rgn, TRUE);
}

static void ApplyTheme(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // immersive dark
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // older builds
    int corner = 2; // DWMWCP_ROUND
    HRESULT cornerResult = DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    COLORREF border = RGB(0x30, 0x30, 0x38);
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));

    // Win10 không hỗ trợ DWM rounded corner: tự cắt góc bằng region.
    g_UseWindowRegion = FAILED(cornerResult);
    ApplyWindowRegion();
}

static void ToggleTopmost() {
    if (!g_Ui.hwnd) return;
    bool on = !g_App.alwaysOnTop.load();
    g_App.alwaysOnTop = on;
    SetWindowPos(g_Ui.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MarkDirty();
    InvalidateAll();
}

static void PlaceInitialWindow() {
    if (!g_Ui.hwnd) return;
    int w = S(kWindowWidthDip);
    int h = S(g_App.actionType.load() == 1 ? kHoldHeightDip : kAutoHeightDip);
    int x = 0;
    int y = 0;
    bool placed = false;

    if (g_Ui.hasSavedPos) {
        x = g_Ui.savedX;
        y = g_Ui.savedY;
        HMONITOR mon = MonitorFromPoint(POINT{ x + w / 2, y + h / 2 },
                                        MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (GetMonitorInfoW(mon, &mi)) {
            RECT test{ x, y, x + w, y + h };
            RECT inter{};
            if (IntersectRect(&inter, &test, &mi.rcWork)) {
                x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w));
                y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - h));
                placed = true;
            }
        }
    }

    if (!placed) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }

    SetWindowPos(g_Ui.hwnd,
                 g_App.alwaysOnTop.load() ? HWND_TOPMOST : HWND_NOTOPMOST,
                 x, y, w, h, SWP_NOACTIVATE);
}

// -----------------------------------------------------------------------------
// Window procedure
// -----------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_Ui.hwnd = hwnd;
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi < 96) dpi = 96;
        g_Ui.dpi = (int)dpi;
        g_Ui.scale = (float)g_Ui.dpi / 96.0f;
        wcscpy_s(g_Ui.fontFace, FontExists(L"Inter") ? L"Inter" : L"Segoe UI");
        CreateUiFonts();
        ApplyTheme(hwnd);

        g_Ui.kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                         GetModuleHandleW(nullptr), 0);
        g_Ui.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                           GetModuleHandleW(nullptr), 0);

        // Khởi tạo trạng thái animation theo config.
        int mode = g_App.actionType.load() == 1 ? 1 : 0;
        g_Ui.modePill.value = g_Ui.modePill.target = mode ? 1.0f : 0.0f;
        g_Ui.pauseReveal.value = g_Ui.pauseReveal.target = mode ? 1.0f : 0.0f;
        int sel = g_App.clickMode.load();
        for (int i = 0; i < 3; ++i)
            g_Ui.targetSel[i].value = g_Ui.targetSel[i].target =
                (i == sel) ? 1.0f : 0.0f;

        PlaceInitialWindow();
        BuildLayout();
        RefreshAnimTargets();
        g_Ui.statusColor = g_Ui.statusTarget;
        UpdateTickTimer();
        g_Ui.settingsReady = true;
        return 0;
    }

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) {
            if (g_Ui.animTimer) { KillTimer(hwnd, TIMER_ANIM); g_Ui.animTimer = false; }
            if (g_Ui.tickTimer) { KillTimer(hwnd, TIMER_TICK); g_Ui.tickTimer = false; }
            return 0;
        }
        BuildLayout();
        ApplyWindowRegion();
        RefreshAnimTargets();
        UpdateTickTimer();
        InvalidateAll();
        return 0;
    }

    case WM_DPICHANGED: {
        UINT dpi = HIWORD(wParam);
        if (dpi < 96) dpi = 96;
        g_Ui.dpi = (int)dpi;
        g_Ui.scale = (float)g_Ui.dpi / 96.0f;
        CreateUiFonts();
        const RECT* target = reinterpret_cast<const RECT*>(lParam);
        int w = S(kWindowWidthDip);
        int h = S(g_App.actionType.load() == 1 ? kHoldHeightDip : kAutoHeightDip);
        SetWindowPos(hwnd, nullptr, target->left, target->top, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        BuildLayout();
        ApplyWindowRegion();
        RefreshAnimTargets();
        UpdateTickTimer();
        InvalidateAll();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        int w = S(kWindowWidthDip);
        int h = S(g_App.actionType.load() == 1 ? kHoldHeightDip : kAutoHeightDip);
        mmi->ptMinTrackSize = POINT{ w, h };
        mmi->ptMaxTrackSize = POINT{ w, h };
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MAXIMIZE || (wParam & 0xFFF0) == SC_SIZE)
            return 0;
        break;

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < S(kTitleHeightDip)) {
            if (PtInRect(&g_L.pin, pt) || PtInRect(&g_L.minBtn, pt) ||
                PtInRect(&g_L.closeBtn, pt))
                return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            HotId h = HitTest(pt);
            if (h != HotNone && HotEnabled(h)) {
                SetCursor(g_CursorHand);
            } else {
                SetCursor(g_CursorArrow);
            }
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        if (!g_Ui.trackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_Ui.trackingMouse = true;
        }
        RefreshHotFromCursor();
        return 0;
    }

    case WM_MOUSELEAVE:
        g_Ui.trackingMouse = false;
        g_Ui.hot = HotNone;
        g_Ui.pinTooltip = false;
        RefreshAnimTargets();
        UpdateTickTimer();
        InvalidateAll();
        return 0;

    case WM_CAPTURECHANGED:
        if (GetCapture() != hwnd) {
            g_Ui.pressed = HotNone;
            g_Ui.repeatHot = HotNone;
            KillTimer(hwnd, TIMER_REPEAT);
            InvalidateAll();
        }
        return 0;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        KillTimer(hwnd, TIMER_REPEAT);
        g_Ui.repeatHot = HotNone;

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HotId h = HitTest(pt);

        if (g_Ui.editField != EditNone) {
            bool keep = (g_Ui.editField == EditSpeed && h == HotSpeedValue) ||
                        (g_Ui.editField == EditPause && h == HotPauseValue);
            if (!keep) CommitEdit();
        }

        if (g_Ui.bindingTarget != 0) {
            bool onBinding = (g_Ui.bindingTarget == 1 && h == HotToggleKey) ||
                             (g_Ui.bindingTarget == 2 && h == HotExitKey);
            if (!onBinding) {
                g_Ui.bindingTarget = 0;
                g_Ui.bindMsgUntil = 0;
                UpdateTickTimer();
            }
        }

        g_Ui.pressed = h;
        if (h != HotNone) SetCapture(hwnd);

        switch (h) {
        case HotPin:
            ToggleTopmost();
            break;
        case HotMin:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case HotClose:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case HotMode0:
            SetActionMode(0);
            break;
        case HotMode1:
            SetActionMode(1);
            break;
        case HotTarget0:
            SetClickMode(0);
            break;
        case HotTarget1:
            SetClickMode(1);
            break;
        case HotTarget2:
            SetClickMode(2);
            break;
        case HotSpeedMinus:
        case HotSpeedPlus:
        case HotPauseMinus:
        case HotPausePlus:
            StepNumeric(h);
            g_Ui.repeatHot = h;
            g_Ui.repeatInterval = 350.0f;
            SetTimer(hwnd, TIMER_REPEAT, 350, nullptr);
            break;
        case HotSpeedValue: {
            bool was = (g_Ui.editField == EditSpeed);
            BeginEdit(EditSpeed);
            if (!was) g_EditFresh = true;
            break;
        }
        case HotPauseValue: {
            bool was = (g_Ui.editField == EditPause);
            BeginEdit(EditPause);
            if (!was) g_EditFresh = true;
            break;
        }
        case HotToggleKey:
            BeginBind(1);
            break;
        case HotExitKey:
            BeginBind(2);
            break;
        default:
            break;
        }

        RefreshAnimTargets();
        UpdateTickTimer();
        InvalidateAll();
        return 0;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        KillTimer(hwnd, TIMER_REPEAT);
        g_Ui.repeatHot = HotNone;
        g_Ui.pressed = HotNone;
        RefreshHotFromCursor();
        InvalidateAll();
        return 0;

    case WM_KEYDOWN:
        if (g_Ui.editField != EditNone) {
            if (wParam == VK_RETURN) CommitEdit();
            else if (wParam == VK_ESCAPE) CancelEdit();
            return 0;
        }
        return 0;

    case WM_CHAR:
        if (g_Ui.editField != EditNone) {
            wchar_t ch = (wchar_t)wParam;
            int len = (int)wcslen(g_Ui.editBuf);
            if (ch == L'\b') {
                if (len > 0) {
                    g_Ui.editBuf[len - 1] = 0;
                    g_Ui.editLen = len - 1;
                }
                g_EditFresh = false;
            } else if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L',') {
                if (ch == L',') ch = L'.';
                if (g_EditFresh) {
                    len = 0;
                    g_Ui.editBuf[0] = 0;
                    g_EditFresh = false;
                }
                if (ch == L'.' && wcschr(g_Ui.editBuf, L'.')) {
                    InvalidateAll();
                    return 0;
                }
                if (len < 6) {
                    g_Ui.editBuf[len] = ch;
                    g_Ui.editBuf[len + 1] = 0;
                    g_Ui.editLen = len + 1;
                }
            }
            UpdateTickTimer();
            InvalidateAll();
            return 0;
        }
        return 0;

    case WM_KILLFOCUS:
        if (g_Ui.editField != EditNone) CommitEdit();
        if (g_Ui.bindingTarget != 0) {
            g_Ui.bindingTarget = 0;
            g_Ui.bindMsgUntil = 0;
            UpdateTickTimer();
            InvalidateAll();
        }
        return 0;

    case WM_TIMER:
        switch (wParam) {
        case TIMER_ANIM: {
            ULONGLONG now = GetTickCount64();
            float dt = (float)(now - g_Ui.lastAnimTick) / 1000.0f;
            g_Ui.lastAnimTick = now;
            dt = ClampF(dt, 0.0f, 0.1f);
            StepAnims(dt);
            if (!AnyAnimActive()) {
                KillTimer(hwnd, TIMER_ANIM);
                g_Ui.animTimer = false;
            }
            InvalidateAll();
            return 0;
        }

        case TIMER_TICK: {
            if (IsIconic(hwnd)) return 0;
            ULONGLONG now = GetTickCount64();
            if (g_Ui.bindMsgUntil != 0 && now >= g_Ui.bindMsgUntil) {
                g_Ui.bindMsgUntil = 0;
                UpdateTickTimer();
                InvalidateAll();
                return 0;
            }
            if (g_Ui.hot == HotPin && !g_Ui.pinTooltip &&
                now - g_Ui.pinHoverStart > 480) {
                g_Ui.pinTooltip = true;
                UpdateTickTimer();
                InvalidateAll();
                return 0;
            }
            if (g_Ui.editField != EditNone) {
                InvalidateAll();
            } else if (g_App.running.load()) {
                InvalidateRect(hwnd, &g_L.statusArea, FALSE);
            }
            return 0;
        }

        case TIMER_REPEAT:
            if (g_Ui.repeatHot != HotNone) {
                StepNumeric(g_Ui.repeatHot);
                if (g_Ui.repeatInterval > 90.0f) {
                    g_Ui.repeatInterval = 90.0f;
                    SetTimer(hwnd, TIMER_REPEAT, 90, nullptr);
                }
            }
            return 0;

        case TIMER_SAVE:
            KillTimer(hwnd, TIMER_SAVE);
            SaveConfig();
            return 0;
        }
        return 0;

    case WM_APP_HOTKEY:
        if (g_Ui.bindingTarget != 0) return 0;
        {
            int key = (int)wParam;
            if (key == g_App.toggleKey.load()) ToggleRunning();
            else if (key == g_App.exitKey.load()) RequestExit();
        }
        return 0;

    case WM_APP_BINDKEY:
        CompleteBind((int)wParam);
        return 0;

    case WM_APP_STATE:
        if (g_App.running.load())
            InvalidateRect(hwnd, &g_L.statusArea, FALSE);
        return 0;

    case WM_EXITSIZEMOVE:
        MarkDirty();
        return 0;

    case WM_CLOSE:
        if (g_Ui.editField != EditNone) CommitEdit();
        g_Ui.bindingTarget = 0;
        KillTimer(hwnd, TIMER_SAVE);
        SaveConfig();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_App.running = false;
        g_App.appActive = false;
        g_WorkerCv.notify_all();
        KillTimer(hwnd, TIMER_ANIM);
        KillTimer(hwnd, TIMER_TICK);
        KillTimer(hwnd, TIMER_SAVE);
        KillTimer(hwnd, TIMER_REPEAT);
        if (g_Ui.kbdHook) { UnhookWindowsHookEx(g_Ui.kbdHook); g_Ui.kbdHook = nullptr; }
        if (g_Ui.mouseHook) { UnhookWindowsHookEx(g_Ui.mouseHook); g_Ui.mouseHook = nullptr; }
        ReleaseBackBuffer();
        DestroyFonts();
        g_Ui.hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------
static const wchar_t* kWindowClass = L"TuynfAutoClickerV2";

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Ok) {
        CoUninitialize();
        return 1;
    }

    g_CursorHand = LoadCursorW(nullptr, IDC_HAND);
    g_CursorArrow = LoadCursorW(nullptr, IDC_ARROW);

    UINT sysDpi = GetDpiForSystem();
    if (sysDpi < 96) sysDpi = 96;
    g_Ui.dpi = (int)sysDpi;
    g_Ui.scale = (float)sysDpi / 96.0f;

    LoadConfig();

    int initW = S(kWindowWidthDip);
    int initH = S(g_App.actionType.load() == 1 ? kHoldHeightDip : kAutoHeightDip);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    HICON icon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON1));
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = g_CursorArrow;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, L"Tuynf Auto Clicker",
        WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, initW, initH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    std::thread worker;
    try {
        worker = std::thread(ClickerThread);
    } catch (...) {
        DestroyWindow(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_App.running = false;
    g_App.appActive = false;
    g_WorkerCv.notify_all();
    if (worker.joinable()) worker.join();

    GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return (int)msg.wParam;
}
