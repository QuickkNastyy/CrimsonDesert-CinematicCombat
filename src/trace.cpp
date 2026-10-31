#include "trace.h"
#include "log.h"
#include "sigscan.h"
#include "MinHook.h"
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace {
const size_t kRing = 2000;   // entries kept per trace
const size_t kMaxCtx = 64;   // distinct target addresses ever traced (contexts/thunks are never freed, see below)
}

struct TraceEntry {
    std::atomic<uint64_t> stamp;  // == idx once the entry is complete (seqlock)
    long long t_us;
    DWORD tid;
    uintptr_t ret;
    uint64_t rcx, rdx, r8, r9, x[4];
};

struct TraceRegsC {  // must match the layout documented in trace_stub.asm
    uint64_t rcx, rdx, r8, r9, rax, r10;
    uint64_t xmm[6][2];
    uint64_t ret, rsp;
};
static_assert(sizeof(TraceRegsC) == 160, "TraceRegs layout is shared with trace_stub.asm");

struct TraceCtx {
    uintptr_t target = 0;
    std::string name;
    void* original = nullptr;  // MinHook trampoline
    uint8_t* thunk = nullptr;
    std::atomic<bool> active{false};
    bool regs = false;
    std::atomic<uint64_t> n{0};
    std::unique_ptr<TraceEntry[]> ring;
};

extern "C" void TraceCommon();
// Called from the asm stub on whatever thread runs the traced function. No locks, no allocation.
extern "C" void* TraceHit(TraceCtx* c, TraceRegsC* r) {
    if (c->active.load(std::memory_order_relaxed)) {
        uint64_t idx = c->n.fetch_add(1) + 1;
        TraceEntry& e = c->ring[(idx - 1) % kRing];
        e.stamp.store(0, std::memory_order_release);
        e.t_us = NowUs();
        e.tid = GetCurrentThreadId();
        e.ret = static_cast<uintptr_t>(r->ret);
        e.rcx = r->rcx; e.rdx = r->rdx; e.r8 = r->r8; e.r9 = r->r9;
        for (int i = 0; i < 4; ++i) e.x[i] = r->xmm[i][0];
        e.stamp.store(idx, std::memory_order_release);
    }
    return c->original;
}

// Design note: untrace only DISABLES the MinHook hook; the context, thunk and trampoline are kept forever.
// A thread that is already inside TraceCommon when we untrace would otherwise jump into freed memory.
static std::mutex g_mu;
static std::map<uintptr_t, std::unique_ptr<TraceCtx>> g_ctx;
static uint8_t* g_thunkPage = nullptr;
static size_t g_thunkUsed = 0;

bool EnsureMinHook(std::string& err) {
    static std::mutex m;
    static bool done = false;
    std::lock_guard<std::mutex> l(m);
    if (done) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { err = std::string("MH_Initialize: ") + MH_StatusToString(s); return false; }
    done = true;
    return true;
}

static uint8_t* AllocThunk() {
    if (!g_thunkPage) {
        g_thunkPage = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_thunkPage) return nullptr;
    }
    if (g_thunkUsed + 32 > 4096) return nullptr;
    uint8_t* p = g_thunkPage + g_thunkUsed;
    g_thunkUsed += 32;
    return p;
}

// mov r11, ctx ; jmp [rip+0] ; dq TraceCommon   (24 bytes)
static void WriteThunk(uint8_t* p, TraceCtx* ctx) {
    void* handler = reinterpret_cast<void*>(&TraceCommon);
    p[0] = 0x49; p[1] = 0xBB;
    memcpy(p + 2, &ctx, 8);
    p[10] = 0xFF; p[11] = 0x25;
    memset(p + 12, 0, 4);
    memcpy(p + 16, &handler, 8);
    FlushInstructionCache(GetCurrentProcess(), p, 24);
}

bool TraceAdd(uintptr_t addr, const std::string& name, bool regs, std::string& msg) {
    if (!EnsureMinHook(msg)) return false;
    if (!IsExecAddr(addr)) { msg = F("0x%llX is not in executable memory", U64(addr)); return false; }
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_ctx.find(addr);
    if (it != g_ctx.end()) {
        TraceCtx& c = *it->second;
        if (c.active) { msg = "already traced"; return false; }
        c.n = 0;  // fresh log on re-trace
        c.regs = regs;
        c.name = name;
        c.active = true;
        MH_STATUS s = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
        if (s != MH_OK) { c.active = false; msg = std::string("MinHook enable: ") + MH_StatusToString(s); return false; }
        Log("trace re-enabled %s at 0x%llX", name.c_str(), U64(addr));
        msg = "trace re-enabled";
        return true;
    }
    if (g_ctx.size() >= kMaxCtx) { msg = "too many distinct trace targets this session (64)"; return false; }
    uint8_t* thunk = AllocThunk();
    if (!thunk) { msg = "out of thunk space"; return false; }
    auto c = std::make_unique<TraceCtx>();
    c->target = addr;
    c->name = name;
    c->regs = regs;
    c->thunk = thunk;
    c->ring.reset(new TraceEntry[kRing]());
    WriteThunk(thunk, c.get());
    MH_STATUS s = MH_CreateHook(reinterpret_cast<LPVOID>(addr), thunk, &c->original);
    if (s != MH_OK) {
        msg = std::string("MinHook create: ") + MH_StatusToString(s);
        if (s == MH_ERROR_UNSUPPORTED_FUNCTION) msg += " (prologue has fewer than 5 relocatable bytes or an unsupported instruction; nothing was changed)";
        g_thunkUsed -= 32;  // thunk never published, reuse the slot
        return false;
    }
    c->active = true;
    s = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    if (s != MH_OK) {
        c->active = false;
        MH_RemoveHook(reinterpret_cast<LPVOID>(addr));
        msg = std::string("MinHook enable: ") + MH_StatusToString(s);
        g_thunkUsed -= 32;
        return false;
    }
    Log("trace installed %s at 0x%llX (%s)", name.c_str(), U64(addr), FmtAddr(addr).c_str());
    g_ctx[addr] = std::move(c);
    msg = "trace installed";
    return true;
}

bool TraceRemove(uintptr_t addr, std::string& msg) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_ctx.find(addr);
    if (it == g_ctx.end() || !it->second->active) { msg = "not traced"; return false; }
    it->second->active = false;
    MH_STATUS s = MH_DisableHook(reinterpret_cast<LPVOID>(addr));
    if (s != MH_OK) { msg = std::string("MinHook disable: ") + MH_StatusToString(s); return false; }
    Log("trace removed at 0x%llX (%llu calls logged)", U64(addr), U64(it->second->n.load()));
    msg = "trace removed";
    return true;
}

void TraceRemoveAll() {
    std::lock_guard<std::mutex> l(g_mu);
    bool any = false;
    for (auto& kv : g_ctx)
        if (kv.second->active) {  // only OUR trace hooks (the time-amp hook has its own lifecycle), applied in one thread freeze
            kv.second->active = false;
            MH_QueueDisableHook(reinterpret_cast<LPVOID>(kv.first));
            any = true;
        }
    if (any) {
        MH_STATUS s = MH_ApplyQueued();
        Log("untrace_all: MinHook -> %s", MH_StatusToString(s));
    }
}

bool TraceRecords(uintptr_t addr, size_t n, std::vector<TraceRec>& out, uint64_t& total) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_ctx.find(addr);
    if (it == g_ctx.end()) return false;
    TraceCtx& c = *it->second;
    total = c.n.load();
    uint64_t first = total > kRing ? total - kRing + 1 : 1;
    if (n < total - first + 1) first = total - n + 1;
    for (uint64_t idx = first; idx <= total; ++idx) {
        TraceEntry& e = c.ring[(idx - 1) % kRing];
        if (e.stamp.load(std::memory_order_acquire) != idx) continue;  // being written or overwritten
        TraceRec r{};
        r.idx = idx; r.t_us = e.t_us; r.tid = e.tid; r.ret = e.ret;
        r.rcx = e.rcx; r.rdx = e.rdx; r.r8 = e.r8; r.r9 = e.r9;
        for (int i = 0; i < 4; ++i) r.x[i] = e.x[i];
        r.regs = c.regs;
        if (e.stamp.load(std::memory_order_acquire) != idx) continue;  // torn while copying
        out.push_back(r);
    }
    return true;
}

bool TraceOverlaps(uintptr_t addr, size_t n) {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& kv : g_ctx)
        if (kv.second->active && addr < kv.first + 16 && addr + n > kv.first) return true;
    return false;
}

std::string TracesSummary() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    for (auto& kv : g_ctx)
        if (kv.second->active) o += F("trace 0x%llX (%s) %s calls=%llu\n", U64(kv.first), FmtAddr(kv.first).c_str(), kv.second->name.c_str(), U64(kv.second->n.load()));
    return o;
}

std::string CmdTrace(const Args& t) {
    if (t.size() < 2 || t.size() > 3 || (t.size() == 3 && Lower(t[2]) != "regs")) return Err("usage: trace <addr|signame> [regs]");
    uintptr_t a;
    std::string e;
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (!TraceAdd(a, t[1], t.size() == 3, e)) return Err(e);
    return Ok(F("%s at 0x%llX (%s)\n", e.c_str(), U64(a), FmtAddr(a).c_str()));
}

std::string CmdTraces() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    for (auto& kv : g_ctx)
        o += F("0x%llX (%s) %s %s calls=%llu%s\n", U64(kv.first), FmtAddr(kv.first).c_str(), kv.second->name.c_str(),
               kv.second->active ? "active" : "removed", U64(kv.second->n.load()), kv.second->regs ? " regs" : "");
    if (o.empty()) o = "(no traces)\n";
    return Ok(o);
}

std::string CmdTraceLog(const Args& t) {
    uint64_t n = 20;
    if (t.size() < 2 || t.size() > 3 || (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > kRing))) return Err("usage: tracelog <addr|signame> [n<=2000]");
    uintptr_t a;
    std::string e;
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    std::vector<TraceRec> recs;
    uint64_t total = 0;
    if (!TraceRecords(a, static_cast<size_t>(n), recs, total)) return Err("no trace at that address");
    std::string o = F("total_calls=%llu shown=%zu\n", U64(total), recs.size());
    for (const TraceRec& r : recs) {
        o += F("#%llu t=%lldus tid=%lu ret=%s rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX", U64(r.idx), r.t_us, r.tid, FmtAddr(r.ret).c_str(),
               U64(r.rcx), U64(r.rdx), U64(r.r8), U64(r.r9));
        if (r.regs)
            for (int i = 0; i < 4; ++i) {
                float f;
                double d;
                uint32_t lo = static_cast<uint32_t>(r.x[i]);
                memcpy(&f, &lo, 4);
                memcpy(&d, &r.x[i], 8);
                o += F(" xmm%d=f32:%g/f64:%g", i, static_cast<double>(f), d);
            }
        o += "\n";
    }
    return Ok(o);
}

std::string CmdUntrace(const Args& t) {
    if (t.size() != 2) return Err("usage: untrace <addr|signame>");
    uintptr_t a;
    std::string e;
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (!TraceRemove(a, e)) return Err(e);
    return Ok(e + "\n");
}

std::string CmdUntraceAll() {
    TraceRemoveAll();
    return Ok("all traces removed\n");
}
