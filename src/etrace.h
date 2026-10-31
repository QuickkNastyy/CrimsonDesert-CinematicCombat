// Event-dispatcher tracer: hooks the game's central timeline-event dispatcher (sig `event_dispatcher`, sub_1404F42B0)
//   _DWORD* __fastcall f(__int64 a1, _DWORD* a2, __int64 a3, __int64 a4, _QWORD* a5)
//   a1 = handler table, D = *(u64*)(a3+8) = event data, typeIndex = *(u16*)D, H = *(u64*)(a1 + 8*typeIndex + 16),
//   fn = *(u64*)(*(u64*)H + 8) is what gets called as fn(H, a2, a3, a4, a5)  (time_curve_start is one such fn).
// Each call is logged to an 8192-entry lock-free ring (same QPC clock as the amp and skills logs), then the original runs
// with identical arguments. The detour allocates nothing and takes no locks.
#pragma once
#include "util.h"
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

struct EtRec {
    uint64_t idx = 0;
    long long t_us = 0;
    DWORD tid = 0;
    uint32_t type = 0xFFFFFFFFu;   // 0xFFFFFFFF = event data unreadable
    uint64_t H = 0, fn = 0, D = 0, a5 = 0, a5v = 0;
    uintptr_t ret = 0;
    uint8_t raw[96] = {};          // first 96 bytes of D (zero-filled if unreadable)
};

struct EtGroup {
    uint32_t type = 0;
    uint64_t fn = 0;
    uint64_t count = 0;
    long long first_us = 0, last_us = 0;
    EtRec firstRec, lastRec;
};

class EtEngine {
public:
    std::atomic<bool> enabled{false};   // log into the ring (`etrace on`)
    std::atomic<bool> tap{false};       // the hook is wanted by telemetry (event catalog) even when the ring log is off
    std::atomic<bool> feedTelemetry{false};
    EtEngine();
    bool Prepare();                                                  // allocate the ring (call before enabling)
    void OnCall(__int64 a1, __int64 a3, uint64_t* a5, uintptr_t ret) noexcept;   // lock-free, allocation-free
    std::vector<EtRec> Since(long long cutoff_us);
    std::vector<EtRec> Tail(size_t n);
    std::vector<EtRec> Window(long long lo_us, long long hi_us);
    std::vector<EtGroup> Summary(long long cutoff_us);               // by (type, fn), most frequent first
    void Clear();                                                    // forget logged records (lifetime counts are kept)
    void SetExcluded(const std::vector<uint32_t>& types);
    std::vector<uint32_t> Excluded() const;
    uint64_t Total() const { return total_.load(); }
    uint64_t Logged() const { return n_.load() - clearBase_.load(); }
    uint64_t ExcludedCalls() const { return excludedCalls_.load(); }
    std::vector<std::pair<uint32_t, uint64_t>> TopTypes(size_t n) const;   // lifetime counts (types < 1024)

private:
    static const size_t kRing = 8192, kCount = 1024;
    std::unique_ptr<EtRec[]> ring_;
    std::unique_ptr<std::atomic<uint64_t>[]> stamp_;
    std::atomic<uint64_t> n_{0}, clearBase_{0}, total_{0}, excludedCalls_{0}, overflow_{0};
    std::atomic<uint64_t> counts_[kCount];
    std::atomic<uint64_t> excl_[1024];   // 65536-bit bitmap of type indices dropped from the LOG (never from execution)
};

typedef uint32_t*(__fastcall* EtFn)(__int64, uint32_t*, __int64, __int64, uint64_t*);
// What the detour does: log (if enabled), then call the original with IDENTICAL arguments and return its value.
uint32_t* EtCallThrough(EtEngine& e, EtFn fn, __int64 a1, uint32_t* a2, __int64 a3, __int64 a4, uint64_t* a5, uintptr_t ret);

EtEngine& Etrace();                       // the process-wide engine used by the real hook
void EtShutdown();                        // disable the hook (quit-bridge / unload)
bool EtOverlaps(uintptr_t addr, size_t n);
void EtRequestTap();                      // telemetry: install the hook in the background (with retries) and feed the event catalog
std::string EtSummaryLine();              // for `hooks`
std::string CmdEtrace(const Args& t);     // etrace on|off|status|log|summary|window|clear|filter
std::string EtFormat(const EtRec& r);     // one compact log line (also used by the selftest)
