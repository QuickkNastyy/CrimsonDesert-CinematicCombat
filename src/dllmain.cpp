#include "common.h"
#include "bridge.h"
#include "features.h"
#include "skillctx.h"
#include "timeamp.h"
#include "telemetry.h"
#include "triggers.h"
#include "version.h"
#include "ui.h"
#include "log.h"
#include "sigscan.h"
#include "timecurve.h"
#include "trace.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>

Settings g_cfg;
std::wstring g_dir;
HMODULE g_hMod = nullptr;
std::atomic<bool> g_stop{false};

static std::map<std::string, std::string> g_ini;  // "section.key" (lowercase) -> value
bool IniGet(const std::string& key, std::string& out) {
    auto it = g_ini.find(key);
    if (it == g_ini.end()) return false;
    out = it->second;
    return true;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Minimal INI: [Section], key=value, ; or # comments. Keys are matched case-insensitively, section ignored.
static void LoadIni(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) { Log("ini: not found, using defaults"); return; }
    std::map<std::string, std::string>& kv = g_ini;
    std::string line, section = "general";
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t e = line.find(']');
            if (e != std::string::npos) section = Lower(Trim(line.substr(1, e - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[section + "." + Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    auto get = [&](const char* key, int& dst) {
        auto it = kv.find(std::string("general.") + key);
        if (it != kv.end()) dst = static_cast<int>(strtol(it->second.c_str(), nullptr, 0));
    };
    get("enabled", g_cfg.enabled);
    get("bridgeenabled", g_cfg.bridgeEnabled);
    get("bridgeport", g_cfg.bridgePort);
    get("enablehooks", g_cfg.enableHooks);
    get("sampletimecurve", g_cfg.sampleTimeCurve);
    get("initdelayms", g_cfg.initDelayMs);
    get("allowwrite", g_cfg.allowWrite);
    get("allowtrace", g_cfg.allowTrace);
    get("allowwatch", g_cfg.allowWatch);
    get("allowinput", g_cfg.allowInput);
    get("allowcall", g_cfg.allowCall);
    if (g_cfg.bridgePort < 1 || g_cfg.bridgePort > 65535) g_cfg.bridgePort = 6400;
}

static std::wstring DirOf(HMODULE h) {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(h, p, MAX_PATH * 2);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

static void InitSafe() {
    try {
        g_dir = DirOf(g_hMod);
        LogOpen(g_dir + L"CombatMod.log");
        Log("CombatMod starting (pid %lu)", GetCurrentProcessId());
        Log("%s", VersionLine().c_str());   // which release, which game build: the first thing a bug report needs
        LoadIni(g_dir + L"CombatMod.ini");
        if (!g_cfg.enabled) { Log("Enabled=0: doing nothing"); return; }
        Log("config: BridgePort=%d EnableHooks=%d SampleTimeCurve=%d InitDelayMs=%d", g_cfg.bridgePort, g_cfg.enableHooks,
            g_cfg.sampleTimeCurve, g_cfg.initDelayMs);
        Log("config: AllowWrite=%d AllowTrace=%d AllowWatch=%d AllowInput=%d AllowCall=%d", g_cfg.allowWrite, g_cfg.allowTrace, g_cfg.allowWatch,
            g_cfg.allowInput, g_cfg.allowCall);
        if (g_cfg.initDelayMs > 0) Sleep(static_cast<DWORD>(g_cfg.initDelayMs));
        if (g_cfg.enableHooks) {
            std::string err;
            Log("EnableHooks=1: MinHook init %s %s (hooks are only installed by the `trace` command)", EnsureMinHook(err) ? "ok" : "FAILED", err.c_str());
        } else {
            Log("EnableHooks=0: no hooks at startup (the `trace` command can still install hooks if AllowTrace=1)");
        }
        if (g_cfg.bridgeEnabled && GetFileAttributesW((g_dir + L"CombatMod.dev").c_str()) == INVALID_FILE_ATTRIBUTES) {   // a hard gate on top of the ini: no marker file, no port
            Log("BridgeEnabled=1 IGNORED: the developer marker file CombatMod.dev is not next to the .asi (no TCP bridge)");
            g_cfg.bridgeEnabled = 0;
        }
        if (g_cfg.bridgeEnabled) StartBridge(g_cfg.bridgePort);
        else Log("BridgeEnabled=0: no TCP bridge (the settings in the ini are still applied)");  // before the (slow) scan so `rescan` / `log` work even if sigs fail
        ResolveSigs(SigsPath());
        if (g_cfg.sampleTimeCurve) StartTimeCurveSampler();
        FeaturesInit();  // settings ([Cinematic]), slow-motion amplifier, launch boost, skill capture - hooks only for enabled features  // reads [TimeAmp]; installs the hook only if Enabled=1
        UiInit();        // [UI]: the control panel thread (window is created lazily on first open)
        TriggersInit();  // [Triggers]: custom slow-motion rules + the worker that starts curves through the engine's time controller
        TelemetryInit(); // [Telemetry]: the patch-diff database (requests the hooks it needs in the background)
        SigRescanLater();   // cold-start safety: ONE rescan ~10 s later if a required signature was missing (never hooks on a partial match)
        Log("init complete");
    } catch (const std::exception& e) {
        Log("init exception: %s", e.what());
    } catch (...) {
        Log("init: unknown exception");
    }
}

static DWORD WINAPI InitThread(LPVOID) {
    RegisterInternalThread();
    __try {
        InitSafe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("init: SEH exception 0x%08lX (continuing without it)", GetExceptionCode());
    }
    return 0;
}

static bool HostIsGame() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p, n);
    std::wstring name = s.substr(s.find_last_of(L"\\/") + 1);
    return _wcsicmp(name.c_str(), L"CrimsonDesert.exe") == 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        if (!HostIsGame()) return TRUE;  // loaded by something else: stay inert
        g_hMod = h;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && g_hMod) {
        // reserved != NULL: the process is terminating - everything dies with it, and taking locks here could hang
        // the exit if a killed thread held one, so do nothing. reserved == NULL: FreeLibrary - clean up fully.
        if (reserved == nullptr) ShutdownAll(true, true);
        g_stop = true;
    }
    return TRUE;
}
