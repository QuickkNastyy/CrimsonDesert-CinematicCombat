#include "ui.h"
#include "hookretry.h"
#include "launchboost.h"
#include "log.h"
#include "pause.h"
#include "settings.h"
#include "skillctx.h"
#include "timeamp.h"
#include "triggers.h"
#include "trace.h"
#include "uimodel.h"
#include "version.h"
#include "MinHook.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// ------------------------------------------------------------------ state shared with the bridge thread (atomics only)
struct UiConf {
    bool enabled = true;
    UiHotkey key;
    int alpha = 240;
    bool startOpen = false;
    bool testMode = false;      // [UI] TestMode=1: window parked off-screen, never takes focus (selftests / stand-in host)
    bool alwaysPoll = false;    // [UI] AlwaysPoll=1: poll hotkeys and pads even when the game is not the foreground window
} g_uc;

std::atomic<bool> g_open{false};        // read by the XInput detour: the panel is visible, the game gets an idle pad
std::atomic<bool> g_alive{false}, g_failed{false}, g_quit{false};
std::atomic<int> g_cmd{0};              // 1 show, 2 hide, 3 toggle (executed on the UI thread)
std::atomic<int> g_padMask{0};          // which pads answered last poll
std::atomic<uint64_t> g_gameCalls{0}, g_swallowed{0}, g_polls{0}, g_opens{0};
std::atomic<int> g_xhooks{0};
std::atomic<HWND> g_hwndA{nullptr}, g_helpA{nullptr};
HANDLE g_thread = nullptr, g_wake = nullptr;
std::string g_failMsg, g_xhookNote;

// UI-thread-only state
HWND g_hwnd = nullptr, g_game = nullptr, g_help = nullptr;
UiModel* g_model = nullptr;
double g_scale = 1.0;                // effective scale: the DPI scale, shrunk a little on a short screen
int g_dpi = 96;
DWORD g_uiTid = 0;                   // the UI thread (FitWindow only resizes from there)
int g_dragRow = -1, g_hover = -1;    // slider being dragged; row under the pointer (-1 = none)
bool g_tracking = false, g_helpRight = true;

const COLORREF kBg = RGB(28, 28, 34), kRow = RGB(42, 42, 52), kText = RGB(234, 234, 240), kDim = RGB(150, 150, 168), kAccent = RGB(79, 195, 247),
               kGreen = RGB(67, 160, 71), kRed = RGB(160, 62, 62), kAmber = RGB(255, 193, 7), kRail = RGB(72, 72, 90);
const wchar_t* kClass = L"CombatModPanel";
const wchar_t* kHelpClass = L"CombatModHelp";
const int kClientW = 480;

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// ------------------------------------------------------------------ layout + painting (shared by the window and `ui snapshot`)
// Base metrics are at 96 dpi and scaled by `s` (DPI scale, shrunk a little when the screen is too short). One font (Segoe UI 10 pt) everywhere;
// bold only for section headers (and the control's name in the help pane).
const int kPad = 16, kSecGap = 12, kRowH = 28, kHdrH = 26, kStatH = 18, kGutter = 26, kLabelW = 178, kValW = 58, kColGap = 10, kStatusLines = 7, kHelpW = 320;

struct Layout {
    std::vector<RECT> rows;
    int W = 0, height = 0, statusTop = 0;
    int pad = 0, labelX = 0, labelR = 0, trackX0 = 0, trackX1 = 0, valX0 = 0, valX1 = 0, statH = 0;
};
int Sc(double s, int v) { return static_cast<int>(std::lround(v * s)); }

Layout ComputeLayout(const UiModel& m, double s, bool allRows = false) {   // allRows: every Blinding Flash mode's rows (the worst-case height)
    Layout L;
    L.W = Sc(s, kClientW);
    L.pad = Sc(s, kPad);
    L.labelX = L.pad + Sc(s, kGutter);
    L.labelR = L.labelX + Sc(s, kLabelW);
    L.valX1 = L.W - L.pad;
    L.valX0 = L.valX1 - Sc(s, kValW);
    L.trackX0 = L.labelR + Sc(s, kColGap);
    L.trackX1 = L.valX0 - Sc(s, kColGap);
    L.statH = Sc(s, kStatH);
    int y = L.pad;
    const auto& rows = m.Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        bool hdr = rows[i].kind == UiKind::Header;
        if (!allRows && !m.Visible(static_cast<int>(i))) { L.rows.push_back(RECT{L.pad, y, L.W - L.pad, y}); continue; }   // another mode's row: no height, no hit area
        if (hdr && i > 0) y += Sc(s, kSecGap);
        if (!hdr && rows[i].kind == UiKind::ButtonRow && i > 0 && rows[i - 1].kind == UiKind::ButtonRow) y += Sc(s, 10);   // the footer buttons sit apart from the presets
        int h = Sc(s, hdr ? kHdrH : kRowH);
        L.rows.push_back(RECT{L.pad, y, L.W - L.pad, y + h});
        y += h + (hdr ? Sc(s, 2) : 0);
    }
    L.statusTop = y;
    L.height = y + kStatusLines * L.statH + L.pad;
    return L;
}

struct Fonts {
    HFONT n, b;
    explicit Fonts(double s) {
        auto mk = [&](int weight) { return CreateFontW(-static_cast<int>(std::lround(10.0 * 96.0 / 72.0 * s)), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"); };
        n = mk(FW_NORMAL); b = mk(FW_BOLD);
    }
    ~Fonts() { DeleteObject(n); DeleteObject(b); }
};

const COLORREF kSecColor[8] = {RGB(79, 195, 247), RGB(129, 199, 132), RGB(255, 138, 101), RGB(240, 98, 146), RGB(186, 104, 200), RGB(128, 203, 196), RGB(150, 150, 168), RGB(255, 213, 79)};
COLORREF SecColor(int accent) { return kSecColor[accent < 0 ? 0 : accent > 7 ? 7 : accent]; }

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void RRect(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border, int bw) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = bw > 0 ? CreatePen(PS_SOLID, bw, border) : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    if (bw > 0) DeleteObject(p);
}
void Txt(HDC dc, HFONT f, const std::string& s, RECT r, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    std::wstring w = Wide(s);
    DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &r, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}
int WrapH(HDC dc, HFONT f, const std::string& s, int width) {   // height of a wrapped paragraph
    if (s.empty()) return 0;
    HGDIOBJ of = SelectObject(dc, f);
    RECT r = {0, 0, width, 0};
    std::wstring w = Wide(s);
    DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
    return r.bottom - r.top;
}
int WrapDraw(HDC dc, HFONT f, const std::string& s, const RECT& r, COLORREF c) {   // draws a wrapped paragraph, returns its height
    int h = WrapH(dc, f, s, r.right - r.left);
    if (!h) return 0;
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    std::wstring w = Wide(s);
    RECT rr = r;
    DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &rr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
    return h;
}
void CheckBox(HDC dc, const RECT& rowRc, double s, bool on) {
    int sz = Sc(s, 16);
    int y = (rowRc.top + rowRc.bottom - sz) / 2;
    RECT b = {rowRc.left + Sc(s, 3), y, rowRc.left + Sc(s, 3) + sz, y + sz};
    RRect(dc, b, Sc(s, 4), on ? kAccent : kRow, on ? kAccent : kDim, 1);
    if (on) {
        HPEN p = CreatePen(PS_SOLID, std::max(2, Sc(s, 2)), RGB(20, 20, 26));
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, b.left + Sc(s, 3), y + sz / 2, nullptr);
        LineTo(dc, b.left + sz / 2 - Sc(s, 1), b.bottom - Sc(s, 4));
        LineTo(dc, b.right - Sc(s, 3), b.top + Sc(s, 4));
        SelectObject(dc, op);
        DeleteObject(p);
    }
}
void SliderTrack(const Layout& L, const RECT& row, double s, int& x0, int& x1, int& cy) {
    x0 = L.trackX0 + Sc(s, 7);
    x1 = L.trackX1 - Sc(s, 7);
    cy = (row.top + row.bottom) / 2;
}
void ButtonRects(const Layout& L, const RECT& row, size_t n, double s, std::vector<RECT>& out) {   // identical height, equal widths within a row
    out.clear();
    int gap = Sc(s, 8);
    int w = (L.W - 2 * L.pad - gap * static_cast<int>(n - 1)) / static_cast<int>(n);
    for (size_t j = 0; j < n; ++j) {
        int x = L.pad + static_cast<int>(j) * (w + gap);
        out.push_back(RECT{x, row.top, x + w, row.bottom});
    }
}

void PaintPanel(HDC dc, int W, int H, double s, const UiModel& m, const Fonts& f, int hover) {
    RECT all = {0, 0, W, H};
    Fill(dc, all, kBg);
    Layout L = ComputeLayout(m, s);
    const auto& rows = m.Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        const UiRow& r = rows[i];
        RECT rc = L.rows[i];
        if (rc.bottom <= rc.top) continue;   // hidden row
        bool sel = static_cast<int>(i) == m.Selected(), hov = static_cast<int>(i) == hover && !sel;
        RECT hl = {rc.left - Sc(s, 6), rc.top, rc.right + Sc(s, 6), rc.bottom};
        if (r.kind != UiKind::Header && r.kind != UiKind::ButtonRow) {
            if (sel) RRect(dc, hl, Sc(s, 8), kRow, kAccent, 1);
            else if (hov) RRect(dc, hl, Sc(s, 8), RGB(35, 35, 44), RGB(35, 35, 44), 0);
        }
        RECT lab = {L.labelX, rc.top, L.labelR + (r.kind == UiKind::Slider ? 0 : L.trackX1 - L.labelR), rc.bottom};
        COLORREF lc = r.experimental ? kAmber : kText;
        switch (r.kind) {
            case UiKind::Master: {
                bool on = m.MasterOn();
                CheckBox(dc, rc, s, on);
                Txt(dc, f.n, r.label, lab, kText, DT_LEFT | DT_VCENTER);
                RECT v = {L.valX0, rc.top, L.valX1, rc.bottom};
                Txt(dc, f.n, on ? "ON" : "OFF", v, on ? kGreen : kRed, DT_RIGHT | DT_VCENTER);
                break;
            }
            case UiKind::Check:
                CheckBox(dc, rc, s, m.CheckOn(static_cast<int>(i)));
                Txt(dc, f.n, r.label, lab, lc, DT_LEFT | DT_VCENTER);
                break;
            case UiKind::Slider: {
                Txt(dc, f.n, r.label, lab, lc, DT_LEFT | DT_VCENTER);
                RECT v = {L.valX0, rc.top, L.valX1, rc.bottom};
                Txt(dc, f.n, m.ValueText(static_cast<int>(i)), v, kAccent, DT_RIGHT | DT_VCENTER);
                int x0, x1, cy;
                SliderTrack(L, rc, s, x0, x1, cy);
                RECT rail = {x0, cy - Sc(s, 2), x1, cy + Sc(s, 2)};
                RRect(dc, rail, Sc(s, 4), kRail, kRail, 0);
                int kx = x0 + static_cast<int>(std::lround(m.Fraction(static_cast<int>(i)) * (x1 - x0)));
                RECT done = {x0, cy - Sc(s, 2), kx, cy + Sc(s, 2)};
                if (kx > x0) RRect(dc, done, Sc(s, 4), kAccent, kAccent, 0);
                RECT knob = {kx - Sc(s, 7), cy - Sc(s, 7), kx + Sc(s, 7), cy + Sc(s, 7)};
                RRect(dc, knob, Sc(s, 14), RGB(240, 240, 248), kAccent, 2);
                break;
            }
            case UiKind::ButtonRow: {
                std::vector<RECT> bs;
                ButtonRects(L, rc, r.buttons.size(), s, bs);
                for (size_t j = 0; j < bs.size(); ++j) {
                    bool hot = sel && static_cast<int>(j) == m.SubSel();
                    RRect(dc, bs[j], Sc(s, 8), hot ? RGB(55, 70, 86) : kRow, hot ? kAccent : kRail, 1);
                    Txt(dc, f.n, r.buttons[j].label, bs[j], kText, DT_CENTER | DT_VCENTER);
                }
                break;
            }
            case UiKind::Header: {
                COLORREF c = SecColor(r.accent);
                RECT t = {rc.left, rc.top, rc.right, rc.bottom - Sc(s, 2)};
                Txt(dc, f.b, r.label, t, c, DT_LEFT | DT_VCENTER);
                if (!r.tag.empty()) Txt(dc, f.n, r.tag, t, kAmber, DT_RIGHT | DT_VCENTER);
                RECT ln = {rc.left, rc.bottom - 1, rc.right, rc.bottom};
                Fill(dc, ln, kRail);
                break;
            }
        }
    }
    // Status: uniform two-column key / value text
    int y = L.statusTop + Sc(s, 2);
    for (size_t i = 0; i < static_cast<size_t>(kStatusLines); ++i) {
        RECT k = {L.pad, y, L.pad + Sc(s, 104), y + L.statH};
        RECT v = {L.pad + Sc(s, 108), y, L.W - L.pad, y + L.statH};
        if (i < m.status.size()) {
            Txt(dc, f.n, m.status[i].first, k, kDim, DT_LEFT | DT_VCENTER);
            Txt(dc, f.n, m.status[i].second, v, kText, DT_LEFT | DT_VCENTER);
        }
        y += L.statH;
    }
}

// The help pane: the control's name (bold), the plain-language explanation, range / default / current, and a tip.
void PaintHelp(HDC dc, int W, int H, double s, const UiHelp& h, int accent, const Fonts& f) {
    RECT all = {0, 0, W, H};
    Fill(dc, all, RGB(22, 22, 28));
    RECT edge = {0, 0, 1, H};
    Fill(dc, edge, kRail);
    int x = Sc(s, 18), w = W - 2 * Sc(s, 18), y = Sc(s, 18);
    if (h.title.empty()) return;
    y += WrapDraw(dc, f.b, h.title, RECT{x, y, x + w, y + 1000}, SecColor(accent)) + Sc(s, 10);
    y += WrapDraw(dc, f.n, h.body, RECT{x, y, x + w, y + 2000}, kText) + Sc(s, 14);
    if (!h.facts.empty()) {
        RECT ln = {x, y, x + w, y + 1};
        Fill(dc, ln, kRail);
        y += Sc(s, 8);
        for (const auto& kv : h.facts) {
            Txt(dc, f.n, kv.first, RECT{x, y, x + Sc(s, 80), y + Sc(s, 22)}, kDim, DT_LEFT | DT_VCENTER);
            Txt(dc, f.n, kv.second, RECT{x + Sc(s, 84), y, x + w, y + Sc(s, 22)}, kText, DT_LEFT | DT_VCENTER);
            y += Sc(s, 22);
        }
        y += Sc(s, 8);
    }
    if (!h.tip.empty()) {
        RECT ln = {x, y, x + w, y + 1};
        Fill(dc, ln, kRail);
        y += Sc(s, 10);
        Txt(dc, f.n, "Tip", RECT{x, y, x + w, y + Sc(s, 20)}, SecColor(accent), DT_LEFT | DT_VCENTER);
        y += Sc(s, 22);
        WrapDraw(dc, f.n, h.tip, RECT{x, y, x + w, y + 1000}, kDim);
    }
}

// ------------------------------------------------------------------ XInput (poll + swallow)
typedef DWORD(WINAPI* XGetStateFn)(DWORD, XState*);
struct XHook { void* target = nullptr; XGetStateFn orig = nullptr; };
XHook g_xh[6];
int g_xhn = 0;
XGetStateFn g_pollFn = nullptr;     // what OUR poller calls: the original (trampoline) once hooked, else the export
HMODULE g_xmod = nullptr;

template <int N>
DWORD WINAPI XDetour(DWORD idx, XState* st) {
    g_gameCalls.fetch_add(1, std::memory_order_relaxed);
    DWORD r = g_xh[N].orig(idx, st);
    if (r == ERROR_SUCCESS && st && g_open.load(std::memory_order_relaxed)) {   // the panel owns the pad: the game sees it idle
        XInputSwallow(*st);
        g_swallowed.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}
XGetStateFn const g_detours[6] = {&XDetour<0>, &XDetour<1>, &XDetour<2>, &XDetour<3>, &XDetour<4>, &XDetour<5>};

void LoadXInput() {
    if (g_pollFn) return;
    g_xmod = GetModuleHandleW(L"xinput1_4.dll");
    if (!g_xmod) g_xmod = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_xmod) g_pollFn = reinterpret_cast<XGetStateFn>(GetProcAddress(g_xmod, "XInputGetState"));
    if (!g_pollFn) g_xhookNote = "xinput1_4.dll not available: no controller support";
}

// Hooks XInputGetState (+ XInputGetStateEx, ordinal 100) of every XInput module the process has loaded. Idempotent; called at each open
// so a module the game loaded later is picked up too. Fail-soft: any failure only means the game is not fed an idle pad.
HookResult InstallXHooks(std::string& msg) {
    if (HookTestShouldFail()) { msg = "MinHook create: MH_ERROR_MEMORY_ALLOC (injected by `hooktest`)"; return HookResult::Retry; }
    LoadXInput();
    if (!EnsureMinHook(msg)) { g_xhookNote = msg; return HookResult::Retry; }
    bool anyFail = false;
    std::string failMsg;
    static const wchar_t* mods[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* name : mods) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) continue;
        Log("ui: %S found (XInputGetState %s, ordinal 100 %s)", name, GetProcAddress(m, "XInputGetState") ? "yes" : "no", GetProcAddress(m, MAKEINTRESOURCEA(100)) ? "yes" : "no");
        void* targets[2] = {reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")), reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(100)))};
        for (void* t : targets) {
            if (!t) continue;
            bool have = false;
            for (int i = 0; i < g_xhn; ++i) have = have || g_xh[i].target == t;
            if (have || g_xhn >= 6) continue;
            XHook& h = g_xh[g_xhn];
            MH_STATUS st = MH_CreateHook(t, reinterpret_cast<LPVOID>(g_detours[g_xhn]), reinterpret_cast<LPVOID*>(&h.orig));
            if (st != MH_OK) { anyFail = true; failMsg = std::string("MinHook create: ") + MH_StatusToString(st); continue; }
            h.target = t;
            st = MH_EnableHook(t);
            if (st != MH_OK) { MH_RemoveHook(t); h.target = nullptr; h.orig = nullptr; anyFail = true; failMsg = std::string("MinHook enable: ") + MH_StatusToString(st); continue; }
            if (t == reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")) && m == g_xmod) g_pollFn = h.orig;   // our own polling bypasses the detour
            ++g_xhn;
            g_xhooks.store(g_xhn);
            Log("ui: hooked %S %s at 0x%llX", name, t == targets[0] ? "XInputGetState" : "XInputGetStateEx", U64(reinterpret_cast<uintptr_t>(t)));
        }
    }
    if (anyFail) { msg = failMsg; g_xhookNote = failMsg; return HookResult::Retry; }
    return HookResult::Ok;
}

void RemoveXHooks() {
    for (int i = 0; i < g_xhn; ++i) if (g_xh[i].target) MH_DisableHook(g_xh[i].target);   // trampolines stay allocated (a game thread may still be inside)
}

struct PadSnap { bool connected = false; uint16_t buttons = 0; int16_t lx = 0; };
PadSnap PollPads() {
    PadSnap p;
    int mask = 0;
    if (!g_pollFn) { g_padMask = 0; return p; }
    for (DWORD i = 0; i < 4; ++i) {
        XState st;
        memset(&st, 0, sizeof st);
        if (g_pollFn(i, &st) != ERROR_SUCCESS) continue;
        mask |= 1 << i;
        p.connected = true;
        p.buttons |= st.buttons;
        if (std::abs(st.lx) > std::abs(p.lx)) p.lx = st.lx;
    }
    g_padMask = mask;
    g_polls.fetch_add(1, std::memory_order_relaxed);
    return p;
}

// ------------------------------------------------------------------ status text
std::vector<std::pair<std::string, std::string>> BuildStatus() {
    std::vector<std::pair<std::string, std::string>> v;
    uint64_t pl = Skills().player.Current();
    AmpStats a = AmpStatsNow();
    LaunchStats l = Launch().Stats();
    v.push_back({"Player", pl ? F("known (a1 0x%llX)", U64(pl)) : "not known yet - use a skill once"});
    v.push_back({"Slow motion", F("%llu seen, %llu modified, %llu skipped", U64(a.events), U64(a.modified), U64(a.filtered))});
    v.push_back({"Launch", F("%llu records, %llu boosted now", U64(l.records), U64(l.boosted))});
    v.push_back({"Triggers", TriggerStatusText()});
    {
        AmpStats bs = AmpStatsNow();
        int mode = static_cast<int>(CfgGet(BlindingFlash_Mode) + 0.5f);
        v.push_back({"Blinding Flash", F("%s, %llu chains, %llu without finale key", mode == 1 ? "Fast chain" : mode == 2 ? "Smooth" : "off", U64(bs.bfChains), U64(bs.bfNoFinale))});
    }
    v.push_back({"System", F("hooks: slow-mo %s, skills %s | pad: %s", AmpHooked() ? "on" : "off", SkillsHooked() ? "on" : "off", g_padMask.load() ? "connected" : "none")});
    v.push_back({"Last action", ""});   // filled with the model's message
    return v;
}

// ------------------------------------------------------------------ the windows
bool FgIsOurs() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return false;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void FitWindow();   // below: the window height follows the visible rows (Blinding Flash modes)

void Repaint() {
    FitWindow();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_help) InvalidateRect(g_help, nullptr, FALSE);
}

void RefreshStatus() {
    if (!g_model) return;
    g_model->status = BuildStatus();
    g_model->status.back().second = g_model->message;
    Repaint();
}

int HelpSubject() {   // the row the help pane explains: the hovered row, else the selected one
    if (!g_model) return 0;
    if (g_hover >= 0 && g_hover < static_cast<int>(g_model->Rows().size()) && g_model->Rows()[static_cast<size_t>(g_hover)].Interactive()) return g_hover;
    return g_model->Selected();
}

enum class CloseMode { Discard, Quiet };   // Discard = "Close without saving" (every setting goes back to its value at open); Quiet = nothing to undo

void ClosePanel(CloseMode mode = CloseMode::Discard) {
    if (!g_open.load() && !(g_hwnd && IsWindowVisible(g_hwnd))) return;
    if (mode == CloseMode::Discard && g_model) g_model->Discard();
    g_open = false;
    g_dragRow = -1;
    g_hover = -1;
    PauseSetOpen(false);
    PauseRestoreNow();   // the game resumes right now (instant), not at the next keeper tick
    if (g_help) ShowWindow(g_help, SW_HIDE);
    if (g_hwnd) { KillTimer(g_hwnd, 1); ShowWindow(g_hwnd, SW_HIDE); }
    if (!g_uc.testMode && g_game && IsWindow(g_game)) SetForegroundWindow(g_game);   // give the game its focus back
    Log("ui: panel closed (%s)", mode == CloseMode::Discard ? "settings reverted" : "settings kept");
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HelpProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

// ---- DPI: the UI thread is made per-monitor DPI aware (thread scope only: the game's own windows and process awareness are untouched)
typedef HANDLE(WINAPI* SetThreadDpiCtxFn)(HANDLE);
typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
typedef BOOL(WINAPI* AdjustRectDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

void MakeThreadDpiAware() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetThreadDpiCtxFn f = u ? reinterpret_cast<SetThreadDpiCtxFn>(GetProcAddress(u, "SetThreadDpiAwarenessContext")) : nullptr;
    if (f) f(reinterpret_cast<HANDLE>(-4));   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

UINT DpiOfMonitor(HMONITOR mon) {
    static GetDpiForMonitorFn fn = [] {
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        return sh ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(sh, "GetDpiForMonitor")) : nullptr;
    }();
    UINT x = 96, y = 96;
    if (fn && mon && SUCCEEDED(fn(mon, 0, &x, &y)) && x) return x;
    HDC sdc = GetDC(nullptr);
    UINT d = static_cast<UINT>(GetDeviceCaps(sdc, LOGPIXELSX));
    ReleaseDC(nullptr, sdc);
    return d ? d : 96;
}

void OuterSize(const Layout& L, DWORD style, DWORD ex, UINT dpi, int& w, int& h) {
    RECT rc = {0, 0, L.W, L.height};
    static AdjustRectDpiFn fn = [] {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        return u ? reinterpret_cast<AdjustRectDpiFn>(GetProcAddress(u, "AdjustWindowRectExForDpi")) : nullptr;
    }();
    if (fn) fn(&rc, style, FALSE, ex, dpi); else AdjustWindowRectEx(&rc, style, FALSE, ex);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
}

const DWORD kMainStyle = WS_CAPTION | WS_SYSMENU;

// The effective scale: the DPI scale, shrunk a little (never below 60 %) when the panel would not fit the height of the work area.
double ChooseScale(UINT dpi, const RECT& work, DWORD ex) {
    double full = dpi / 96.0, s = full;
    int avail = (work.bottom - work.top) - static_cast<int>(std::lround(24 * full));
    while (s > full * 0.6) {
        int w, h;
        OuterSize(ComputeLayout(*g_model, s, true), kMainStyle, ex, dpi, w, h);
        if (h <= avail) break;
        s -= 0.02;
    }
    return s;
}

RECT WorkAreaOf(HMONITOR mon) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    return RECT{0, 0, 1920, 1040};
}

// Dock the help window flush to the right edge of the panel (or the left edge when there is no room), same top and height.
void DockHelp() {
    if (!g_hwnd || !g_help) return;
    RECT mr;
    GetWindowRect(g_hwnd, &mr);
    RECT work = WorkAreaOf(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST));
    int hw = Sc(g_scale, kHelpW);
    UiDock d = UiDockHelp(UiRect{mr.left, mr.top, mr.right, mr.bottom}, UiRect{work.left, work.top, work.right, work.bottom}, hw);
    g_helpRight = d.onRight;
    SetWindowPos(g_help, HWND_TOPMOST, d.x, mr.top, hw, mr.bottom - mr.top, SWP_NOACTIVATE);
}

void FitWindow() {
    if (!g_hwnd || !g_model || GetCurrentThreadId() != g_uiTid) return;
    Layout L = ComputeLayout(*g_model, g_scale);
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    if (cr.bottom == L.height) return;
    DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE));
    int w, h;
    OuterSize(L, kMainStyle, ex, static_cast<UINT>(g_dpi), w, h);
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    SetWindowPos(g_hwnd, nullptr, wr.left, wr.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);   // WM_WINDOWPOSCHANGED re-docks the help window to the new height
}

bool CreatePanel() {
    if (g_hwnd) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = g_hMod;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { g_failMsg = F("RegisterClass failed (%lu)", GetLastError()); return false; }
    WNDCLASSEXW hc = {};
    hc.cbSize = sizeof hc;
    hc.lpfnWndProc = HelpProc;
    hc.hInstance = g_hMod;
    hc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    hc.lpszClassName = kHelpClass;
    if (!RegisterClassExW(&hc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { g_failMsg = F("RegisterClass (help) failed (%lu)", GetLastError()); return false; }

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | (g_uc.alpha < 255 ? WS_EX_LAYERED : 0);
    HMONITOR mon = (g_game && IsWindow(g_game)) ? MonitorFromWindow(g_game, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    RECT work = WorkAreaOf(mon);
    g_dpi = static_cast<int>(DpiOfMonitor(mon));
    g_scale = ChooseScale(static_cast<UINT>(g_dpi), work, ex);
    Layout L = ComputeLayout(*g_model, g_scale);
    int w, h;
    OuterSize(L, kMainStyle, ex, static_cast<UINT>(g_dpi), w, h);
    int x = -20000, y = -20000;
    if (!g_uc.testMode) { x = work.left + Sc(g_scale, 12); y = work.top + Sc(g_scale, 12); }   // top-left of the monitor that holds the game window
    g_hwnd = CreateWindowExW(ex, kClass, L"Cinematic Combat", kMainStyle, x, y, w, h, nullptr, nullptr, g_hMod, nullptr);
    if (!g_hwnd) { g_failMsg = F("CreateWindow failed (%lu)", GetLastError()); return false; }
    g_hwndA = g_hwnd;
    DWORD hex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (g_uc.alpha < 255 ? WS_EX_LAYERED : 0);
    g_help = CreateWindowExW(hex, kHelpClass, L"Cinematic Combat help", WS_POPUP, x + w, y, Sc(g_scale, kHelpW), h, g_hwnd, nullptr, g_hMod, nullptr);   // owned by the panel: closes / hides / stacks with it
    if (!g_help) { g_failMsg = F("CreateWindow (help) failed (%lu)", GetLastError()); DestroyWindow(g_hwnd); g_hwnd = nullptr; g_hwndA = nullptr; return false; }
    g_helpA = g_help;
    if (ex & WS_EX_LAYERED) {
        SetLayeredWindowAttributes(g_hwnd, 0, static_cast<BYTE>(std::max(60, g_uc.alpha)), LWA_ALPHA);
        SetLayeredWindowAttributes(g_help, 0, static_cast<BYTE>(std::max(60, g_uc.alpha)), LWA_ALPHA);
    }
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {   // dark title bar where available; silently skipped otherwise
        typedef HRESULT(WINAPI * DwmSet)(HWND, DWORD, LPCVOID, DWORD);
        DwmSet f = reinterpret_cast<DwmSet>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        BOOL on = TRUE;
        if (f && FAILED(f(g_hwnd, 20, &on, sizeof on))) f(g_hwnd, 19, &on, sizeof on);
    }
    DockHelp();
    Log("ui: window created (%dx%d, dpi %d, scale %.2f) + help window (%d px, docked %s)", w, h, g_dpi, g_scale, Sc(g_scale, kHelpW), g_helpRight ? "right" : "left");
    return true;
}

void OpenPanel() {
    if (g_failed) return;
    if (g_open) return;
    try {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (fg && pid == GetCurrentProcessId() && fg != g_hwnd) g_game = fg;   // the window to give focus back to
        if (!CreatePanel()) { Log("ui: %s - the panel is disabled", g_failMsg.c_str()); g_failed = true; return; }
        HookRetryRequest("xinput", [](std::string& m) { return InstallXHooks(m); });   // lazily, first open, in the background with retries; stays installed (pass-through) afterwards
        g_model->BeginSession();   // "Close without saving" returns to these values
        g_hover = -1;
        RefreshStatus();
        g_open = true;
        ++g_opens;
        DockHelp();
        ShowWindow(g_hwnd, g_uc.testMode ? SW_SHOWNOACTIVATE : SW_SHOW);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | (g_uc.testMode ? SWP_NOACTIVATE : 0));
        ShowWindow(g_help, SW_SHOWNOACTIVATE);   // never takes the focus
        SetWindowPos(g_help, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!g_uc.testMode) { SetForegroundWindow(g_hwnd); SetFocus(g_hwnd); }
        SetTimer(g_hwnd, 1, 500, nullptr);
        Repaint();
        PauseSetOpen(true);   // only now that the panel really is open: the keeper freezes the game if UI.PauseGame is on
        Log("ui: panel opened");
    } catch (...) {
        g_failMsg = "exception while opening";
        g_failed = true;
        g_open = false;
        PauseSetOpen(false);
        PauseRestoreNow();
        Log("ui: exception while opening the panel - disabled");
    }
}

int HitRow(const Layout& L, int x, int y) {
    POINT p = {x, y};
    for (size_t i = 0; i < L.rows.size(); ++i) if (PtInRect(&L.rows[i], p)) return static_cast<int>(i);
    return -1;
}

void DragSlider(int row, int x) {
    Layout L = ComputeLayout(*g_model, g_scale);
    int x0, x1, cy;
    SliderTrack(L, L.rows[static_cast<size_t>(row)], g_scale, x0, x1, cy);
    g_model->SetFraction(row, x1 > x0 ? static_cast<double>(x - x0) / (x1 - x0) : 0.0);
}

void HandleKey(WPARAM vk) {
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    UiResult res = UiResult::None;
    g_hover = -1;   // keyboard navigation: the help pane follows the selection again
    switch (vk) {
        case VK_UP: g_model->Move(-1); break;
        case VK_DOWN: g_model->Move(1); break;
        case VK_TAB: g_model->Move(shift ? -1 : 1); break;
        case VK_LEFT: g_model->Adjust(shift ? -5 : -1); break;
        case VK_RIGHT: g_model->Adjust(shift ? 5 : 1); break;
        case VK_PRIOR: g_model->Adjust(10); break;
        case VK_NEXT: g_model->Adjust(-10); break;
        case VK_HOME: g_model->SetValueBound(g_model->Selected(), true); break;
        case VK_END: g_model->SetValueBound(g_model->Selected(), false); break;
        case VK_SPACE: res = g_model->Activate(); break;
        case VK_RETURN: if (!ctrl) res = g_model->Activate(); break;   // Ctrl+Enter is the global toggle
        case VK_ESCAPE: ClosePanel(CloseMode::Discard); return;   // Esc = close without saving
        default: return;
    }
    if (res == UiResult::Close) ClosePanel(CloseMode::Quiet);   // the model already saved (Save & Close) or reverted (Close without saving)
    else Repaint();
}

void PaintInto(HWND h, bool help) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    if (g_model && rc.right > 0 && rc.bottom > 0) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        {
            Fonts fonts(g_scale);
            if (help) {
                int row = HelpSubject();
                PaintHelp(mem, rc.right, rc.bottom, g_scale, g_model->HelpFor(row), g_model->Rows()[static_cast<size_t>(row)].accent, fonts);
            } else {
                PaintPanel(mem, rc.right, rc.bottom, g_scale, *g_model, fonts, g_hover);
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(h, &ps);
}

LRESULT CALLBACK HelpProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: PaintInto(h, true); return 0;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // the help window never takes the focus
            case WM_DESTROY: g_help = nullptr; g_helpA = nullptr; return 0;
        }
    } catch (...) {
        Log("ui: exception in the help window procedure (ignored)");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: PaintInto(h, false); return 0;
            case WM_LBUTTONDOWN: {
                if (!g_model) return 0;
                SetFocus(h);
                Layout L = ComputeLayout(*g_model, g_scale);
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), row = HitRow(L, x, y);
                if (row < 0 || !g_model->Rows()[static_cast<size_t>(row)].Interactive()) return 0;
                g_model->Select(row);
                const UiRow& r = g_model->Rows()[static_cast<size_t>(row)];
                UiResult res = UiResult::None;
                if (r.kind == UiKind::Slider) { g_dragRow = row; SetCapture(h); DragSlider(row, x); }
                else if (r.kind == UiKind::ButtonRow) {
                    std::vector<RECT> bs;
                    ButtonRects(L, L.rows[static_cast<size_t>(row)], r.buttons.size(), g_scale, bs);
                    for (size_t j = 0; j < bs.size(); ++j) {
                        POINT p = {x, y};
                        if (PtInRect(&bs[j], p)) { g_model->SetSub(static_cast<int>(j)); res = g_model->ActivateButton(row, static_cast<int>(j)); break; }
                    }
                } else res = g_model->Activate();
                if (res == UiResult::Close) ClosePanel(CloseMode::Quiet); else Repaint();
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!g_model) return 0;
                if (!g_tracking) {   // ask for WM_MOUSELEAVE so the help pane returns to the selection when the pointer leaves
                    TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, h, 0};
                    g_tracking = TrackMouseEvent(&te) != 0;
                }
                if (g_dragRow >= 0 && (wp & MK_LBUTTON)) {
                    DragSlider(g_dragRow, GET_X_LPARAM(lp));
                    Repaint();
                    return 0;
                }
                Layout L = ComputeLayout(*g_model, g_scale);
                int row = HitRow(L, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (row >= 0 && !g_model->Rows()[static_cast<size_t>(row)].Interactive()) row = -1;
                if (row != g_hover) { g_hover = row; Repaint(); }   // the help pane follows the pointer
                return 0;
            }
            case WM_MOUSELEAVE:
                g_tracking = false;
                if (g_hover != -1) { g_hover = -1; Repaint(); }
                return 0;
            case WM_LBUTTONUP:
                if (g_dragRow >= 0) { g_dragRow = -1; ReleaseCapture(); }
                return 0;
            case WM_CAPTURECHANGED: g_dragRow = -1; return 0;
            case WM_MOUSEWHEEL: {
                if (!g_model) return 0;
                POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(h, &p);
                Layout L = ComputeLayout(*g_model, g_scale);
                int row = HitRow(L, p.x, p.y);
                if (row >= 0 && g_model->Rows()[static_cast<size_t>(row)].kind == UiKind::Slider) {
                    g_model->Select(row);
                    int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                    g_model->Adjust(notches * ((GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) ? 5 : 1));
                    Repaint();
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (g_model) HandleKey(wp);
                return 0;
            case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
            case WM_TIMER:
                if (wp == 1) RefreshStatus();
                return 0;
            case WM_WINDOWPOSCHANGED:   // the help window follows the panel (move, size, show) and re-docks on the other side when the right side is full
                DockHelp();
                break;
            case WM_DPICHANGED: {
                g_dpi = LOWORD(wp);
                const RECT* sug = reinterpret_cast<const RECT*>(lp);
                DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE));
                g_scale = ChooseScale(static_cast<UINT>(g_dpi), WorkAreaOf(MonitorFromRect(sug, MONITOR_DEFAULTTONEAREST)), ex);
                int w, hh;
                OuterSize(ComputeLayout(*g_model, g_scale), kMainStyle, ex, static_cast<UINT>(g_dpi), w, hh);
                SetWindowPos(h, nullptr, sug->left, sug->top, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
                DockHelp();
                Repaint();
                return 0;
            }
            case WM_CLOSE:
                ClosePanel(CloseMode::Discard);   // the window X = close without saving
                return 0;
            case WM_DESTROY:
                g_hwnd = nullptr;
                g_hwndA = nullptr;
                return 0;
        }
    } catch (...) {
        Log("ui: exception in the window procedure (ignored)");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------ the UI thread
void Toggle() { if (g_open) ClosePanel(CloseMode::Discard); else OpenPanel(); }   // hotkey / combo while open = close without saving (like Esc)

void UiThreadBody() {
    g_uiTid = GetCurrentThreadId();
    MakeThreadDpiAware();   // per-monitor DPI for OUR windows only (thread scope), before any window exists
    UiModel model;
    g_model = &model;
    LoadXInput();
    EdgeToggle kbToggle(300), padToggle(300);
    PadNav nav;
    long long startMs = NowUs() / 1000;
    bool startOpenDone = !g_uc.startOpen, wasOpen = false;
    while (!g_quit) {
        bool active = g_open || g_uc.alwaysPoll || FgIsOurs();
        MsgWaitForMultipleObjectsEx(1, &g_wake, active ? 16 : 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;
        long long now = NowUs() / 1000;
        try {
            if (!startOpenDone && now - startMs > 1500) { startOpenDone = true; OpenPanel(); }
            int cmd = g_cmd.exchange(0);
            if (cmd == 1) OpenPanel(); else if (cmd == 2) ClosePanel(CloseMode::Discard); else if (cmd == 3) Toggle();
            if (g_failed) continue;
            active = g_open || g_uc.alwaysPoll || FgIsOurs();
            if (g_open && !wasOpen) nav.Latch();   // buttons that are already down when the panel opens must not press anything in it
            wasOpen = g_open;
            if (!active) { kbToggle.Reset(); padToggle.Reset(); continue; }
            const UiHotkey& k = g_uc.key;
            bool kd = k.vk && (GetAsyncKeyState(k.vk) & 0x8000) && (!k.ctrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) && (!k.shift || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) &&
                      (!k.alt || (GetAsyncKeyState(VK_MENU) & 0x8000));
            if (kbToggle.Update(kd, now)) Toggle();
            PadSnap ps = PollPads();
            if (ps.connected) {
                if (padToggle.Update(PadOpenCombo(ps.buttons), now)) { nav.Reset(); Toggle(); }
                else if (g_open) {
                    PadEvents ev = nav.Update(ps.buttons, ps.lx, now);
                    UiResult res = UiResult::None;
                    if (ev.moveY || ev.adjust || ev.activate) g_hover = -1;   // controller navigation: the help pane follows the selection
                    if (ev.moveY) model.Move(ev.moveY);
                    if (ev.adjust) model.Adjust(ev.adjust);
                    if (ev.activate) res = model.Activate();
                    if (ev.start) { if (model.SaveAndClose()) res = UiResult::Close; }   // Start = Save & Close
                    if (res == UiResult::Close) ClosePanel(CloseMode::Quiet);
                    else if (ev.back) ClosePanel(CloseMode::Discard);                        // B = close without saving
                    else if (ev.moveY || ev.adjust || ev.activate || ev.start) Repaint();
                }
            } else {
                padToggle.Reset();
                nav.Reset();
            }
        } catch (...) {
            Log("ui: exception in the UI loop (ignored)");
        }
    }
    g_open = false;
    PauseSetOpen(false);
    PauseRestoreNow();
    if (g_hwnd) { KillTimer(g_hwnd, 1); DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    UnregisterClassW(kHelpClass, g_hMod);
    UnregisterClassW(kClass, g_hMod);
    RemoveXHooks();
    g_model = nullptr;
}

DWORD WINAPI UiThreadProc(LPVOID) {
    RegisterInternalThread();
    g_alive = true;
    __try {
        UiThreadBody();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_open = false;
        g_failed = true;
        PauseSetOpen(false);
        PauseRestoreNow();   // never leave the game frozen if the UI thread dies
        Log("ui: SEH exception 0x%08lX in the UI thread - panel disabled", GetExceptionCode());
    }
    g_alive = false;
    return 0;
}

bool SetCmd(int c) {
    if (!g_alive || g_failed) return false;
    g_cmd = c;
    SetEvent(g_wake);
    return true;
}

bool WaitOpen(bool want) {
    for (int i = 0; i < 60; ++i) {   // up to ~600 ms for the UI thread to carry the request out
        if (g_open.load() == want) return true;
        Sleep(10);
    }
    return g_open.load() == want;
}

bool WriteBmp(const std::wstring& path, int W, int H, const void* bits) {
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih; ih.biWidth = W; ih.biHeight = -H; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    ih.biSizeImage = static_cast<DWORD>(W * H * 4);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, &fh, sizeof fh, &w, nullptr) && WriteFile(f, &ih, sizeof ih, &w, nullptr) && WriteFile(f, bits, ih.biSizeImage, &w, nullptr);
    CloseHandle(f);
    return ok;
}
}  // namespace

// ------------------------------------------------------------------ public API
bool UiIsOpen() { return g_open.load(); }

void UiInit() {
    std::string v;
    if (IniGet("ui.enabled", v)) g_uc.enabled = strtol(v.c_str(), nullptr, 0) != 0;
    if (IniGet("ui.alpha", v)) g_uc.alpha = std::min(255, std::max(60, static_cast<int>(strtol(v.c_str(), nullptr, 0))));
    if (IniGet("ui.startopen", v)) g_uc.startOpen = strtol(v.c_str(), nullptr, 0) != 0;
    if (IniGet("ui.testmode", v)) g_uc.testMode = strtol(v.c_str(), nullptr, 0) != 0;
    if (IniGet("ui.alwayspoll", v)) g_uc.alwaysPoll = strtol(v.c_str(), nullptr, 0) != 0;
    std::string hk = "Ctrl+Enter";
    IniGet("ui.togglekeyboard", hk);
    if (!UiParseHotkey(hk, g_uc.key)) { Log("ui: bad ToggleKeyboard '%s', using Ctrl+Enter", hk.c_str()); UiParseHotkey("Ctrl+Enter", g_uc.key); }
    if (!g_uc.enabled) { Log("ui: disabled by [UI] Enabled=0"); return; }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, UiThreadProc, nullptr, 0, nullptr);
    if (!g_thread || !g_wake) { Log("ui: could not start the UI thread"); g_failed = true; return; }
    PauseInit();
    Log("ui: ready (toggle: %s or LB+DPad-Down, alpha %d)", hk.c_str(), g_uc.alpha);
}

void UiShutdown(bool wait) {
    g_open = false;     // the game gets its pad back immediately
    PauseSetOpen(false);
    PauseRestoreNow();  // ... and its time
    g_quit = true;
    if (g_wake) SetEvent(g_wake);
    if (g_thread) {
        if (wait) WaitForSingleObject(g_thread, 3000);
        if (wait) { CloseHandle(g_thread); g_thread = nullptr; }
    }
}

std::string CmdUi(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if ((sub == "show" || sub == "hide" || sub == "toggle") && t.size() == 2) {
        if (!g_uc.enabled) return Err("the panel is disabled ([UI] Enabled=0)");
        if (g_failed) return Err("the panel failed and is disabled: " + g_failMsg);
        if (!SetCmd(sub == "show" ? 1 : sub == "hide" ? 2 : 3)) return Err("the UI thread is not running");
        bool want = sub == "show" ? true : sub == "hide" ? false : !g_open.load();
        if (!WaitOpen(want)) return Err(g_failed ? "could not open: " + g_failMsg : "the UI thread did not respond in time");
        return Ok(std::string("panel ") + (want ? "open" : "closed") + "\n");
    }
    if (sub == "status" && t.size() <= 2) {
        UiHotkey k = g_uc.key;
        std::string o = F("enabled=%d thread=%s open=%d failed=%d%s window=%s opens=%llu\n", g_uc.enabled ? 1 : 0, g_alive ? "running" : "stopped", g_open.load() ? 1 : 0, g_failed ? 1 : 0,
                          g_failed ? (" (" + g_failMsg + ")").c_str() : "", g_hwndA.load() ? "created" : "not yet", U64(g_opens.load()));
        if (UiModel* mdl = g_model) o += F("selected_row=%d sub=%d\n", mdl->Selected(), mdl->SubSel());
        o += F("version=%s\n", VersionLine().c_str());
        o += F("pause_write_guard=%s\n", PauseGuardStatus().c_str());
        o += F("toggle=%s (vk 0x%02X) + pad LB+DPad-Down | alpha=%d testmode=%d alwayspoll=%d paused=%d\n", UiHotkeyStr(k).c_str(), k.vk, g_uc.alpha, g_uc.testMode ? 1 : 0, g_uc.alwaysPoll ? 1 : 0, PauseActive() ? 1 : 0);
        o += F("xinput: module=%s hooks=%d game_XInputGetState_calls=%llu swallowed_while_open=%llu our_polls=%llu pads_connected_mask=0x%X%s\n", g_xmod ? "loaded" : "none", g_xhooks.load(),
               U64(g_gameCalls.load()), U64(g_swallowed.load()), U64(g_polls.load()), g_padMask.load(), g_xhookNote.empty() ? "" : (" note=" + g_xhookNote).c_str());
        if (g_gameCalls.load() == 0) o += "note: the game has not called XInputGetState (yet). If it stays 0 during play the game reads the pad through another API; the panel then cannot swallow pad input (use the pause menu).\n";
        return Ok(o);
    }
    if (sub == "pause") return CmdPause(t);
    if (sub == "rows" && t.size() == 2) {
        UiModel m;
        std::string o;
        for (size_t i = 0; i < m.Rows().size(); ++i) {
            const UiRow& r = m.Rows()[i];
            o += F("%zu %s %s%s%s\n", i, r.kind == UiKind::Master ? "master" : r.kind == UiKind::Check ? "check" : r.kind == UiKind::Slider ? "slider" :
                   r.kind == UiKind::ButtonRow ? "buttons" : "header", r.cfg >= 0 ? CfgDefOf(r.cfg).key : "-", r.label.empty() ? "" : "  ", r.label.c_str());
        }
        return Ok(o);
    }
    if ((sub == "click" && (t.size() == 3 || t.size() == 4)) || (sub == "key" && t.size() == 3)) {   // developer: injects into the real window procedure
        HWND h = g_hwndA.load();
        if (!h || !g_open.load()) return Err("the panel is not open (`ui show` first)");
        if (sub == "key") {
            uint64_t vk;
            if (!ParseNum(t[2], vk) || vk > 255) return Err("usage: ui key <vk>");
            PostMessageW(h, WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
            PostMessageW(h, WM_KEYUP, static_cast<WPARAM>(vk), 0);
            return Ok("key posted\n");
        }
        uint64_t row;
        float frac = 0.5f;
        if (!ParseNum(t[2], row) || (t.size() == 4 && !ParseFloat(t[3], frac))) return Err("usage: ui click <row> [fraction 0..1 | button index]");
        UiModel m;
        Layout L = ComputeLayout(m, g_scale);
        if (row >= L.rows.size()) return Err("no such row (`ui rows`)");
        RECT r = L.rows[static_cast<size_t>(row)];
        int x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
        const UiRow& ur = m.Rows()[static_cast<size_t>(row)];
        if (ur.kind == UiKind::Slider) { int x0, x1, cy; SliderTrack(L, r, g_scale, x0, x1, cy); x = x0 + static_cast<int>(frac * (x1 - x0)); y = cy; }
        else if (ur.kind == UiKind::ButtonRow) { std::vector<RECT> bs; ButtonRects(L, r, ur.buttons.size(), g_scale, bs); size_t j = std::min(bs.size() - 1, static_cast<size_t>(frac)); x = (bs[j].left + bs[j].right) / 2; y = (bs[j].top + bs[j].bottom) / 2; }
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        return Ok(F("click posted at client (%d,%d)\n", x, y));
    }
    if (sub == "help" && t.size() == 2) {   // developer: where the help window is and what it explains
        UiModel* mdl = g_model;
        HWND mw = g_hwndA.load(), hw = g_helpA.load();
        if (!mw || !hw || !mdl) return Ok("help window: not created yet (`ui show` first)\n");
        RECT mr, hr;
        GetWindowRect(mw, &mr);
        GetWindowRect(hw, &hr);
        int row = HelpSubject();
        UiHelp h = mdl->HelpFor(row);
        std::string facts;
        for (auto& kv : h.facts) facts += (facts.empty() ? "" : "; ") + kv.first + "=" + kv.second;
        return Ok(F("help_visible=%d side=%s subject_row=%d hover_row=%d title=%s\npanel_rect=%ld,%ld,%ld,%ld help_rect=%ld,%ld,%ld,%ld flush=%d same_height=%d noactivate=%d owned=%d\nfacts: %s\ntip: %s\n",
                    IsWindowVisible(hw) ? 1 : 0, g_helpRight ? "right" : "left", row, g_hover, h.title.c_str(), mr.left, mr.top, mr.right, mr.bottom, hr.left, hr.top, hr.right, hr.bottom,
                    (g_helpRight ? hr.left == mr.right : hr.right == mr.left) ? 1 : 0, (hr.bottom - hr.top) == (mr.bottom - mr.top) ? 1 : 0,
                    (GetWindowLongPtrW(hw, GWL_EXSTYLE) & WS_EX_NOACTIVATE) ? 1 : 0, GetWindow(hw, GW_OWNER) == mw ? 1 : 0, facts.c_str(), h.tip.c_str()));
    }
    if (sub == "hover" && t.size() == 3) {   // developer: pretend the pointer is over a row (or -1)
        HWND mw = g_hwndA.load();
        uint64_t row;
        if (!mw || !g_model) return Err("the panel has not been created (`ui show` first)");
        if (t[2] == "-1") g_hover = -1;
        else if (ParseNum(t[2], row) && row < g_model->Rows().size() && g_model->Rows()[static_cast<size_t>(row)].Interactive()) g_hover = static_cast<int>(row);
        else return Err("usage: ui hover <row | -1>");
        InvalidateRect(mw, nullptr, FALSE);
        HWND hw = g_helpA.load();
        if (hw) InvalidateRect(hw, nullptr, FALSE);
        return Ok(F("hover row %d\n", g_hover));
    }
    if (sub == "snapshot" && t.size() >= 3) {
        // ui snapshot <path> [row=N] [scale=X]: the panel AND its help pane side by side in one BMP (row = the explained row, default the selected one)
        int selRow = -1;
        double s = 1.0;
        Args words(t.begin() + 2, t.end());
        while (!words.empty() && (Lower(words.back()).rfind("row=", 0) == 0 || Lower(words.back()).rfind("scale=", 0) == 0)) {
            std::string w = Lower(words.back());
            if (w.rfind("row=", 0) == 0) selRow = atoi(w.c_str() + 4); else s = std::min(3.0, std::max(0.5, atof(w.c_str() + 6)));
            words.pop_back();
        }
        std::string joined;
        for (size_t i = 0; i < words.size(); ++i) joined += (i ? " " : "") + words[i];
        std::wstring fp;
        std::string e;
        if (!ResolveSavePath(joined, fp, e)) return Err(e);
        UiModel m;
        m.status = BuildStatus();
        if (UiModel* live = g_model) { m.Select(live->Selected()); m.message = live->message; }
        if (selRow >= 0) m.Select(selRow);
        m.status.back().second = m.message;
        Layout L = ComputeLayout(m, s);
        int PW = L.W, HW = Sc(s, kHelpW), W = PW + HW, H = L.height;
        HDC sdc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(sdc);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { DeleteDC(mem); ReleaseDC(nullptr, sdc); return Err("could not create the bitmap"); }
        HGDIOBJ ob = SelectObject(mem, bmp);
        {
            Fonts fonts(s);
            PaintPanel(mem, PW, H, s, m, fonts, -1);
            POINT old;
            SetViewportOrgEx(mem, PW, 0, &old);
            int row = m.Selected();
            PaintHelp(mem, HW, H, s, m.HelpFor(row), m.Rows()[static_cast<size_t>(row)].accent, fonts);
            SetViewportOrgEx(mem, old.x, old.y, nullptr);
        }
        GdiFlush();
        bool ok = WriteBmp(fp, W, H, bits);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, sdc);
        if (!ok) return Err("could not write the file");
        return Ok(F("snapshot %dx%d written (BMP): panel %d px + help pane %d px\n", W, H, PW, HW));
    }
    return Err("usage: ui show | hide | toggle | status | help | pause on|off|status | rows | click <row> [frac] | hover <row> | key <vk> | snapshot <absolute path inside the CombatMod project folder> [row=N] [scale=X]");
}
