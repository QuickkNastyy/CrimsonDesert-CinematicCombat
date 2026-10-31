// Shared globals and safe-memory helpers. Include this before any other Windows header use.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

struct Settings {
    int enabled = 1;          // [General] Enabled
    int bridgeEnabled = 0;    // [General] BridgeEnabled: OFF unless the ini says 1 AND the marker file CombatMod.dev sits next to the .asi (a missing / unreadable ini must never open a port)
    int bridgePort = 6400;    // [General] BridgePort
    int enableHooks = 0;      // [General] EnableHooks (0 = strictly read-only)
    int sampleTimeCurve = 0;  // [General] SampleTimeCurve
    int initDelayMs = 0;      // [General] InitDelayMs (wait before scanning, for packed/late-decrypted exes)
    // Bridge v2 capability gates. Code defaults are OFF; dist\CombatMod.ini turns the dev ones on.
    int allowWrite = 0;       // write/writef/writeq/writed/patch/undo
    int allowTrace = 0;       // trace/untrace (MinHook detours)
    int allowWatch = 0;       // watch/unwatch (hardware breakpoints)
    int allowInput = 0;       // key/keys (SendInput)
    int allowCall = 0;        // call (DANGEROUS)
};

extern Settings g_cfg;
extern std::wstring g_dir;        // folder of the .asi, with trailing backslash
extern HMODULE g_hMod;            // this DLL
extern std::atomic<bool> g_stop;  // set on process detach

inline std::wstring SigsPath() { return g_dir + L"CombatMod.sigs"; }

// Safe read from our own process: fails (returns false) instead of faulting.
inline bool SafeRead(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}
template <class T> inline bool Rd(uintptr_t addr, T& v) { return SafeRead(addr, &v, sizeof(T)); }
