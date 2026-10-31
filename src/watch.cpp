#include "watch.h"
#include "log.h"
#include <TlHelp32.h>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace {
const size_t kRing = 500;

struct Watch {
    std::atomic<bool> used{false};
    uintptr_t addr = 0;
    int size = 0;
    bool rw = false;
    DWORD onlyTid = 0;
};
Watch g_w[4];
WatchHit g_hits[4][kRing];
std::atomic<uint64_t> g_stamp[4][kRing];  // seqlock stamp per hit slot
std::atomic<uint64_t> g_n[4];

struct DrCfg {
    uint64_t dr[4] = {0, 0, 0, 0};
    uint64_t dr7 = 0;
    bool operator==(const DrCfg& o) const { return dr7 == o.dr7 && !memcmp(dr, o.dr, sizeof dr); }
};

std::mutex g_mu;                    // guards watch config, g_applied, poller state
std::map<DWORD, DrCfg> g_applied;   // what each thread currently has (absent = nothing)
int g_failed = 0;
void* g_veh = nullptr;
std::atomic<bool> g_poller{false};

// ---- VEH (runs on the faulting thread: no locks, no allocation) ----
bool ReadQwords(const uint64_t* p, uint64_t* out, int n) {
    __try { for (int i = 0; i < n; ++i) out[i] = p[i]; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t ReadVal(uintptr_t a, int size) {
    uint64_t v = 0;
    __try { memcpy(&v, reinterpret_cast<const void*>(a), static_cast<size_t>(size)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = ~0ull; }
    return v;
}

thread_local bool t_inVeh = false;  // the handler's own reads of a watched address (rw mode) re-trigger the breakpoint

LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6 & 0xF;
    if (!dr6) return EXCEPTION_CONTINUE_SEARCH;  // not a debug-register hit (e.g. someone else's trap flag)
    if (t_inVeh) { c->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }  // nested hit caused by this handler: ignore
    t_inVeh = true;
    for (int i = 0; i < 4; ++i) {
        if (!((dr6 >> i) & 1)) continue;
        Watch& w = g_w[i];
        if (!w.used.load(std::memory_order_relaxed)) {
            c->Dr7 &= ~((3ull << (i * 2)) | (0xFull << (16 + i * 4)));  // stale register on a thread we have not cleaned yet
            continue;
        }
        uint64_t idx = g_n[i].fetch_add(1) + 1;
        size_t s = (idx - 1) % kRing;
        g_stamp[i][s].store(0, std::memory_order_release);
        WatchHit& h = g_hits[i][s];
        h.idx = idx;
        h.t_us = NowUs();
        h.tid = GetCurrentThreadId();
        h.rip = c->Rip; h.rax = c->Rax; h.rcx = c->Rcx; h.rdx = c->Rdx; h.r8 = c->R8; h.r9 = c->R9;
        if (!ReadQwords(reinterpret_cast<const uint64_t*>(c->Rsp), h.stk, 6)) memset(h.stk, 0, sizeof h.stk);
        h.val = ReadVal(w.addr, w.size);
        g_stamp[i][s].store(idx, std::memory_order_release);
    }
    // Data breakpoints are traps: the accessing instruction has already completed, RIP points past it, so
    // execution simply continues. (RF/TF handling only matters for instruction breakpoints, which we never use.)
    // Clear the sticky status bits so the next hit is attributed correctly.
    c->Dr6 = 0;
    t_inVeh = false;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- applying debug registers to threads ----
DrCfg ConfigFor(DWORD tid) {  // caller holds g_mu
    DrCfg c;
    for (int i = 0; i < 4; ++i) {
        Watch& w = g_w[i];
        if (!w.used || (w.onlyTid && w.onlyTid != tid)) continue;
        c.dr[i] = w.addr;
        uint64_t len = w.size == 1 ? 0 : w.size == 2 ? 1 : w.size == 4 ? 3 : 2;
        uint64_t rw = w.rw ? 3 : 1;
        c.dr7 |= (1ull << (i * 2)) | (rw << (16 + i * 4)) | (len << (18 + i * 4));
    }
    return c;
}

bool ApplyToThread(DWORD tid, const DrCfg& cfg) {  // only kernel calls between Suspend and Resume
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            ctx.Dr0 = cfg.dr[0]; ctx.Dr1 = cfg.dr[1]; ctx.Dr2 = cfg.dr[2]; ctx.Dr3 = cfg.dr[3];
            ctx.Dr6 = 0;
            ctx.Dr7 = cfg.dr7;
            ok = SetThreadContext(h, &ctx) != 0;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

std::vector<DWORD> EnumThreads() {
    std::vector<DWORD> v;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s == INVALID_HANDLE_VALUE) return v;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te))
        if (te.th32OwnerProcessID == pid) v.push_back(te.th32ThreadID);
    CloseHandle(s);
    return v;
}

// Bring every (non-plugin) thread's debug registers in line with the current watch set. caller holds g_mu.
void Reconcile() {
    std::vector<DWORD> tids = EnumThreads();
    std::map<DWORD, DrCfg> next;
    for (DWORD tid : tids) {
        if (IsInternalThread(tid)) continue;
        DrCfg want = ConfigFor(tid), have;
        auto it = g_applied.find(tid);
        if (it != g_applied.end()) have = it->second;
        if (!(want == have)) {
            if (!ApplyToThread(tid, want)) { ++g_failed; Log("watch: could not set debug registers on thread %lu", tid); }
        }
        if (want.dr7) next[tid] = want;  // threads with nothing armed are not tracked
    }
    g_applied.swap(next);
}

bool AnyUsed() { for (auto& w : g_w) if (w.used) return true; return false; }

void Poller() {
    RegisterInternalThread();
    while (!g_stop) {
        Sleep(250);
        std::lock_guard<std::mutex> l(g_mu);
        if (AnyUsed() || !g_applied.empty()) Reconcile();
    }
    UnregisterInternalThread();
}
}  // namespace

bool WatchAdd(bool rw, uintptr_t addr, int size, std::string& msg, DWORD onlyTid) {
    if (size != 1 && size != 2 && size != 4 && size != 8) { msg = "size must be 1, 2, 4 or 8"; return false; }
    if (addr % static_cast<uintptr_t>(size)) { msg = F("address must be aligned to its size (0x%llX %% %d != 0)", U64(addr), size); return false; }
    uint8_t probe;
    if (!SafeRead(addr, &probe, 1)) { msg = "address is not readable"; return false; }
    std::lock_guard<std::mutex> l(g_mu);
    int slot = -1;
    for (int i = 0; i < 4; ++i) {
        if (g_w[i].used && g_w[i].addr == addr) { msg = "address already watched"; return false; }
        if (!g_w[i].used && slot < 0) slot = i;
    }
    if (slot < 0) { msg = "all 4 debug registers are in use"; return false; }
    if (!g_veh) {
        g_veh = AddVectoredExceptionHandler(1, Veh);
        if (!g_veh) { msg = "AddVectoredExceptionHandler failed"; return false; }
    }
    Watch& w = g_w[slot];
    g_n[slot] = 0;
    for (size_t k = 0; k < kRing; ++k) g_stamp[slot][k] = 0;
    w.addr = addr; w.size = size; w.rw = rw; w.onlyTid = onlyTid;
    w.used = true;
    int failedBefore = g_failed;
    Reconcile();
    bool exp = false;
    if (g_poller.compare_exchange_strong(exp, true)) {
        try { std::thread(Poller).detach(); } catch (...) { g_poller = false; }
    }
    Log("watch %s set: slot %d addr=0x%llX size=%d (%d thread(s) failed)", rw ? "rw" : "w", slot, U64(addr), size, g_failed - failedBefore);
    msg = F("slot=%d threads=%zu failed=%d", slot, g_applied.size(), g_failed - failedBefore);
    return true;
}

bool WatchRemove(uintptr_t addr, std::string& msg) {
    std::lock_guard<std::mutex> l(g_mu);
    for (int i = 0; i < 4; ++i) {
        if (g_w[i].used && g_w[i].addr == addr) {
            g_w[i].used = false;
            Reconcile();  // clears the registers on all threads; the VEH stays installed so a late stale hit cannot crash
            Log("watch removed: slot %d addr=0x%llX (%llu hits)", i, U64(addr), U64(g_n[i].load()));
            msg = F("removed slot %d (%llu hits kept in log until the slot is reused)", i, U64(g_n[i].load()));
            return true;
        }
    }
    msg = "address not watched";
    return false;
}

void WatchRemoveAll() {
    std::lock_guard<std::mutex> l(g_mu);
    if (!AnyUsed() && g_applied.empty()) return;
    for (auto& w : g_w) w.used = false;
    Reconcile();
    Log("all watches removed");
}

bool WatchHits(uintptr_t addr, size_t n, std::vector<WatchHit>& out, uint64_t& total) {
    std::lock_guard<std::mutex> l(g_mu);
    for (int i = 0; i < 4; ++i) {
        if (g_w[i].addr != addr || (!g_w[i].used && g_n[i] == 0)) continue;
        total = g_n[i].load();
        uint64_t first = total > kRing ? total - kRing + 1 : 1;
        if (n < total - first + 1) first = total - n + 1;
        for (uint64_t idx = first; idx <= total; ++idx) {
            size_t s = (idx - 1) % kRing;
            if (g_stamp[i][s].load(std::memory_order_acquire) != idx) continue;
            WatchHit h = g_hits[i][s];
            if (g_stamp[i][s].load(std::memory_order_acquire) != idx) continue;
            out.push_back(h);
        }
        return true;
    }
    return false;
}

std::string WatchesSummary() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    for (int i = 0; i < 4; ++i)
        if (g_w[i].used) o += F("watch slot=%d 0x%llX size=%d %s hits=%llu\n", i, U64(g_w[i].addr), g_w[i].size, g_w[i].rw ? "rw" : "w", U64(g_n[i].load()));
    return o;
}

std::string CmdWatch(const Args& t) {
    uint64_t size;
    std::string mode = t.size() > 1 ? Lower(t[1]) : "";
    if (t.size() != 4 || (mode != "w" && mode != "rw") || !ParseNum(t[3], size)) return Err("usage: watch w|rw <addr> <size 1|2|4|8>");
    uintptr_t a;
    std::string e;
    if (!ResolveAddr(t[2], a, e)) return Err(e);
    if (!WatchAdd(mode == "rw", a, static_cast<int>(size), e)) return Err(e);
    return Ok(F("watching 0x%llX size=%llu mode=%s %s\n", U64(a), U64(size), mode.c_str(), e.c_str()));
}

std::string CmdWatches() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    for (int i = 0; i < 4; ++i)
        if (g_w[i].used)
            o += F("slot=%d addr=0x%llX size=%d mode=%s hits=%llu\n", i, U64(g_w[i].addr), g_w[i].size, g_w[i].rw ? "rw" : "w", U64(g_n[i].load()));
    if (o.empty()) o = "(no watches)\n";
    o += F("threads_armed=%zu failed_total=%d\n", g_applied.size(), g_failed);
    return Ok(o);
}

std::string CmdWatchLog(const Args& t) {
    uint64_t n = 20;
    if (t.size() < 2 || t.size() > 3 || (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > kRing))) return Err("usage: watchlog <addr> [n<=500]");
    uintptr_t a;
    std::string e;
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    std::vector<WatchHit> hits;
    uint64_t total = 0;
    if (!WatchHits(a, static_cast<size_t>(n), hits, total)) return Err("address not watched");
    std::string o = F("total_hits=%llu shown=%zu (rip = instruction AFTER the access)\n", U64(total), hits.size());
    for (const WatchHit& h : hits) {
        o += F("#%llu t=%lldus tid=%lu rip=0x%llX (%s) rax=0x%llX rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX val=0x%llX stk=[", U64(h.idx), h.t_us, h.tid,
               U64(h.rip), FmtAddr(static_cast<uintptr_t>(h.rip)).c_str(), U64(h.rax), U64(h.rcx), U64(h.rdx), U64(h.r8), U64(h.r9), U64(h.val));
        for (int k = 0; k < 6; ++k) o += F(k ? ",%s" : "%s", FmtAddr(static_cast<uintptr_t>(h.stk[k])).c_str());
        o += "]\n";
    }
    return Ok(o);
}

std::string CmdUnwatch(const Args& t) {
    if (t.size() != 2) return Err("usage: unwatch <addr>");
    uintptr_t a;
    std::string e;
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (!WatchRemove(a, e)) return Err(e);
    return Ok(e + "\n");
}
