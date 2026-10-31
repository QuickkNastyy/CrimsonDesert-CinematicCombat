// Telemetry / patch-diff database: maps the whole system as the game is played so two builds can be compared after a game patch.
// The hooks feed preallocated tables (no allocation, no locks in the detours); a background thread writes one JSON file per session
// (<exe TimeDateStamp hex>_<yyyymmdd_hhmmss>.json) at start, every 60 s, on `telemetry dump` and once more on quit-bridge / unload.
// tools\telemetry_diff.py compares two such files and prints the post-patch triage report.
#pragma once
#include "launchboost.h"
#include "sigscan.h"
#include "util.h"
#include <atomic>
#include <vector>

struct TelModule {
    std::string exePath, plugin;
    uint32_t timeDateStamp = 0, sizeOfImage = 0;
    uint64_t fileSize = 0, base = 0;
};
struct TelHook {
    std::string name, state;
    uint64_t addr = 0;
    uint8_t fp[24] = {};      // first 24 bytes of the function BEFORE it was hooked
    bool fpOk = false;
};
struct TelSlot {              // one registered handler of the dispatcher table
    int slot = 0;
    uint64_t H = 0, fn = 0;
    uint8_t fp[24] = {};
    bool fpOk = false;
};
struct TelInputs {            // everything that is not accumulated by the hooks (gathered live, or synthetic in the selftest)
    TelModule module;
    std::vector<Sig> sigs;
    std::vector<TelHook> hooks;
    std::vector<TelSlot> table;
    uint64_t tableA1 = 0;
    std::vector<LaunchProbe> launch;
    std::string launchScan;   // the newest `launch scan` as JSON (empty = none)
    std::string bfChains;     // the last Blinding Flash chains (ordered ctx key tuples per chain) as a JSON array (empty = none)
    std::string started, written;
    long long uptimeS = 0;
};

class TelemetryStore {
public:
    // ---- feed (hook context: lock-free, allocation-free)
    void OnEvent(uint32_t type, uint64_t H, uint64_t fn, const uint8_t* raw96, uint64_t a1) noexcept;
    void OnSkill(const uint32_t keys[4], bool player) noexcept;
    void OnSlowmo(float scale, float a, float b, const uint32_t keys[4], bool haveCtx, bool modified, bool filtered) noexcept;   // caller serialises (amp engine mutex)
    void OnHitStop(long long minute, uint32_t key0, bool haveCtx) noexcept;   // a hit-stop-class event happened in this minute of the session (amp engine mutex serialises)
    uint64_t TableA1() const { return a1_.load(); }
    struct HsMinute { long long minute = 0; uint64_t count = 0; std::vector<std::pair<uint32_t, uint64_t>> keys; };
    std::vector<HsMinute> HitStopMinutes() const;   // ascending by minute
    // ---- output
    std::string BuildJson(const TelInputs& in) const;
    struct Counts { uint64_t eventTypes = 0, events = 0, skillKeys = 0, slowLayouts = 0, overflow = 0; };
    Counts Count() const;

private:
    struct Ev {
        std::atomic<int> state{0};      // 0 empty, 1 being filled, 2 ready
        std::atomic<uint64_t> count{0};
        uint64_t fn = 0, H = 0;
        uint8_t fp[24] = {};
        bool fpOk = false;
        uint8_t d[96] = {};
    };
    struct Key { std::atomic<uint32_t> key{0}; std::atomic<uint64_t> n{0}, player{0}; };
    struct Slow {
        std::atomic<int> used{0};
        std::atomic<uint32_t> s{0}, a{0}, b{0};
        std::atomic<uint64_t> count{0}, modified{0}, filtered{0};
        std::atomic<uint32_t> hk[8];
        std::atomic<uint64_t> hn[8];
    };
    struct Hs {
        std::atomic<long long> minute{-1};
        std::atomic<uint64_t> count{0};
        std::atomic<uint32_t> hk[8];
        std::atomic<uint64_t> hn[8];
    };
    static const size_t kEv = 1024, kKey = 2048, kSlow = 512, kHs = 128;
    Hs hs_[kHs];
    Ev ev_[kEv];
    Key key_[kKey];
    Slow slow_[kSlow];
    std::atomic<uint64_t> a1_{0}, overflow_{0};
};

TelemetryStore& Telemetry();
void TelemetryInit();                            // [Telemetry] ini section; requests the hooks it needs; starts the writer thread
void TelemetryRequestHooks();                    // (re)requests the hooks the recorder needs (after a late signature rescan)
void TelemetryShutdown(bool wait);               // final write (unless in the loader lock), stop the thread
bool TelemetryOn();
std::wstring TelemetryDir();                      // where the session files and launch_scan_*.json go
void TelemetryNoteHook(const std::string& name, uint64_t addr);   // reads the 24 prologue bytes NOW (call before the hook is created)
std::string CmdTelemetry(const Args& t);         // telemetry status|dump|on|off
bool TelemetryJsonValid(const std::string& text);   // minimal strict JSON validator (selftest)
std::string TelemetryFileName(uint32_t timeDateStamp, const SYSTEMTIME& t);   // "<stamp hex>_<yyyymmdd_hhmmss>.json"
