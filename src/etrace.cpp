#include "etrace.h"
#include "log.h"
#include "hookretry.h"
#include "sigscan.h"
#include "telemetry.h"
#include "trace.h"
#include "MinHook.h"
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <map>
#include <mutex>

// ------------------------------------------------------------------ engine
EtEngine::EtEngine() {
    for (auto& c : counts_) c.store(0);
    for (auto& e : excl_) e.store(0);
}

bool EtEngine::Prepare() {
    if (ring_) return true;
    try {
        std::unique_ptr<EtRec[]> r(new EtRec[kRing]());
        std::unique_ptr<std::atomic<uint64_t>[]> s(new std::atomic<uint64_t>[kRing]());
        stamp_ = std::move(s);
        ring_ = std::move(r);  // the detour only runs after `enabled` is set, which happens after this returns
        return true;
    } catch (...) {
        return false;
    }
}

void EtEngine::OnCall(__int64 a1, __int64 a3, uint64_t* a5, uintptr_t ret) noexcept {
    if (!ring_ && !tap.load(std::memory_order_relaxed)) return;
    long long now = NowUs();
    uint64_t D = 0, H = 0, vt = 0, fn = 0, a5v = 0;
    uint8_t raw[96] = {};
    uint32_t type = 0xFFFFFFFFu;
    if (a3) PeekMem(static_cast<uintptr_t>(a3) + gl::kEvDataInArgs, &D, 8);
    if (D) {
        uint8_t tmp[96];
        size_t got = 0;
        if (PeekMem(static_cast<uintptr_t>(D), tmp, 96)) got = 96;       // the event data may end near a page boundary:
        else if (PeekMem(static_cast<uintptr_t>(D), tmp, 32)) got = 32;  // fall back to smaller reads
        else if (PeekMem(static_cast<uintptr_t>(D), tmp, 8)) got = 8;
        if (got) {
            memcpy(raw, tmp, got);
            type = static_cast<uint32_t>(raw[gl::kEvType]) | (static_cast<uint32_t>(raw[gl::kEvType + 1]) << 8);
        }
    }
    if (type != 0xFFFFFFFFu && a1) {
        PeekMem(static_cast<uintptr_t>(a1) + gl::kEvTableBase + gl::kEvTableStride * static_cast<uintptr_t>(type), &H, 8);
        if (H && PeekMem(static_cast<uintptr_t>(H), &vt, 8) && vt) PeekMem(static_cast<uintptr_t>(vt) + gl::kHandlerFnInVtable, &fn, 8);
    }
    if (a5) PeekMem(reinterpret_cast<uintptr_t>(a5), &a5v, 8);

    total_.fetch_add(1);
    if (feedTelemetry.load(std::memory_order_relaxed) && type != 0xFFFFFFFFu) Telemetry().OnEvent(type, H, fn, raw, static_cast<uint64_t>(a1));
    if (!ring_ || !enabled.load(std::memory_order_relaxed)) return;   // only the ring log depends on `etrace on`
    if (type < kCount) counts_[type].fetch_add(1); else overflow_.fetch_add(1);
    if (type != 0xFFFFFFFFu && type < 65536 && ((excl_[type >> 6].load(std::memory_order_relaxed) >> (type & 63)) & 1)) {
        excludedCalls_.fetch_add(1);
        return;
    }
    uint64_t idx = n_.fetch_add(1) + 1;
    size_t slot = static_cast<size_t>((idx - 1) % kRing);
    stamp_[slot].store(0);
    EtRec& r = ring_[slot];
    r.idx = idx; r.t_us = now; r.tid = GetCurrentThreadId(); r.type = type;
    r.H = H; r.fn = fn; r.D = D; r.a5 = reinterpret_cast<uint64_t>(a5); r.a5v = a5v; r.ret = ret;
    memcpy(r.raw, raw, 96);
    stamp_[slot].store(idx);
}

std::vector<EtRec> EtEngine::Since(long long cutoff_us) {
    std::vector<EtRec> out;
    if (!ring_) return out;
    uint64_t total = n_.load(), base = clearBase_.load();
    uint64_t first = std::max<uint64_t>(base + 1, total > kRing ? total - kRing + 1 : 1);
    for (uint64_t i = first; i <= total; ++i) {
        size_t slot = static_cast<size_t>((i - 1) % kRing);
        if (stamp_[slot].load() != i) continue;
        EtRec r = ring_[slot];
        if (stamp_[slot].load() != i) continue;  // torn while copying
        if (r.t_us >= cutoff_us) out.push_back(r);
    }
    return out;
}

std::vector<EtRec> EtEngine::Tail(size_t n) {
    std::vector<EtRec> all = Since(0);
    if (all.size() > n) all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(n));
    return all;
}

std::vector<EtRec> EtEngine::Window(long long lo_us, long long hi_us) {
    std::vector<EtRec> all = Since(lo_us), out;
    for (const EtRec& r : all) if (r.t_us <= hi_us) out.push_back(r);
    return out;
}

std::vector<EtGroup> EtEngine::Summary(long long cutoff_us) {
    std::map<std::pair<uint32_t, uint64_t>, EtGroup> m;
    for (const EtRec& r : Since(cutoff_us)) {
        auto key = std::make_pair(r.type, r.fn);
        auto it = m.find(key);
        if (it == m.end()) {
            EtGroup g;
            g.type = r.type; g.fn = r.fn; g.first_us = r.t_us; g.firstRec = r;
            it = m.emplace(key, g).first;
        }
        EtGroup& g = it->second;
        ++g.count;
        g.last_us = r.t_us;
        g.lastRec = r;
    }
    std::vector<EtGroup> v;
    for (auto& kv : m) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const EtGroup& a, const EtGroup& b) { return a.count > b.count; });
    return v;
}

void EtEngine::Clear() { clearBase_.store(n_.load()); }

void EtEngine::SetExcluded(const std::vector<uint32_t>& types) {
    for (auto& e : excl_) e.store(0);
    for (uint32_t t : types) if (t < 65536) excl_[t >> 6].fetch_or(1ull << (t & 63));
}

std::vector<uint32_t> EtEngine::Excluded() const {
    std::vector<uint32_t> v;
    for (uint32_t t = 0; t < 65536; ++t) if ((excl_[t >> 6].load() >> (t & 63)) & 1) v.push_back(t);
    return v;
}

std::vector<std::pair<uint32_t, uint64_t>> EtEngine::TopTypes(size_t n) const {
    std::vector<std::pair<uint32_t, uint64_t>> v;
    for (uint32_t t = 0; t < kCount; ++t) if (uint64_t c = counts_[t].load()) v.emplace_back(t, c);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > n) v.resize(n);
    return v;
}

uint32_t* EtCallThrough(EtEngine& e, EtFn fn, __int64 a1, uint32_t* a2, __int64 a3, __int64 a4, uint64_t* a5, uintptr_t ret) {
    if (e.enabled.load(std::memory_order_relaxed) || e.tap.load(std::memory_order_relaxed)) e.OnCall(a1, a3, a5, ret);
    return fn(a1, a2, a3, a4, a5);
}

// ------------------------------------------------------------------ formatting
std::string EtFormat(const EtRec& r) {
    float f[6];
    memcpy(f, r.raw + gl::kEvSampleFloats, 24);
    std::string raw;
    for (int i = 0; i < 96; ++i) raw += F("%02X", r.raw[i]);
    std::string type = r.type == 0xFFFFFFFFu ? "?" : F("%u", r.type);
    return F("#%llu t=%lldus tid=%lu type=%s H=0x%llX fn=%s D=0x%llX f=[%g,%g,%g,%g,%g,%g] a5=0x%llX *a5=0x%llX ret=%s raw=%s", U64(r.idx), r.t_us, r.tid,
             type.c_str(), U64(r.H), r.fn ? FmtAddr(static_cast<uintptr_t>(r.fn)).c_str() : "0", U64(r.D), static_cast<double>(f[0]), static_cast<double>(f[1]),
             static_cast<double>(f[2]), static_cast<double>(f[3]), static_cast<double>(f[4]), static_cast<double>(f[5]), U64(r.a5), U64(r.a5v),
             FmtAddr(r.ret).c_str(), raw.c_str());
}

// ------------------------------------------------------------------ hook management
namespace {
EtEngine g_et;
std::mutex g_hm;
EtFn g_orig = nullptr;
std::atomic<bool> g_etWanted{false};
uintptr_t g_target = 0;
bool g_created = false, g_hooked = false;

uint32_t* __fastcall Detour(__int64 a1, uint32_t* a2, __int64 a3, __int64 a4, uint64_t* a5) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    return EtCallThrough(g_et, g_orig, a1, a2, a3, a4, a5, ret);
}

bool Install(std::string& msg) {
    std::lock_guard<std::mutex> l(g_hm);
    if (g_hooked) return true;
    if (HookTestShouldFail()) { msg = "MinHook create: MH_ERROR_MEMORY_ALLOC (injected by `hooktest`)"; return false; }
    Sig s;
    if (!FindSig("event_dispatcher", s)) { msg = "signature event_dispatcher not found in CombatMod.sigs"; return false; }
    if (s.st != SigStatus::Resolved || s.matches != 1) {
        msg = F("signature event_dispatcher is %s (%zu matches); refusing to hook, a UNIQUE match is required", SigStatusName(s.st), s.matches);
        return false;
    }
    if (!IsExecAddr(s.addr)) { msg = "event_dispatcher address is not in executable memory"; return false; }
    if (!g_et.Prepare()) { msg = "could not allocate the event ring"; return false; }
    if (!g_created) TelemetryNoteHook("event_dispatcher", s.addr);
    if (!EnsureMinHook(msg)) return false;
    if (g_created && g_target != s.addr) { msg = "signature address changed since the hook was created (rescan?); restart the game"; return false; }
    if (!g_created) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(s.addr), reinterpret_cast<LPVOID>(&Detour), reinterpret_cast<LPVOID*>(&g_orig));
        if (st != MH_OK) { msg = std::string("MinHook create: ") + MH_StatusToString(st); return false; }
        g_created = true;
        g_target = s.addr;
    }
    MH_STATUS st = MH_EnableHook(reinterpret_cast<LPVOID>(g_target));
    if (st != MH_OK) { msg = std::string("MinHook enable: ") + MH_StatusToString(st); return false; }
    g_hooked = true;
    Log("etrace: hook installed on event_dispatcher 0x%llX (%s)", U64(g_target), FmtAddr(g_target).c_str());
    return true;
}

std::string ListStr(const std::vector<uint32_t>& v) {
    if (v.empty()) return "none";
    std::string o;
    for (uint32_t k : v) o += F(o.empty() ? "%u" : ",%u", k);
    return o;
}
}  // namespace

EtEngine& Etrace() { return g_et; }

void EtRequestTap() {
    HookRetryRequest("event_dispatcher", [](std::string& m) {
        bool ok = Install(m);
        if (ok) { g_et.tap = true; g_et.enabled = g_etWanted.load(); }
        return HookClassify(ok, m);
    });
}

void EtShutdown() {
    g_et.enabled = false;
    std::lock_guard<std::mutex> l(g_hm);
    if (g_hooked) {
        MH_STATUS st = MH_DisableHook(reinterpret_cast<LPVOID>(g_target));
        g_hooked = false;
        Log("etrace: hook removed (%s)", MH_StatusToString(st));
    }
}

bool EtOverlaps(uintptr_t addr, size_t n) {
    std::lock_guard<std::mutex> l(g_hm);
    return g_hooked && addr < g_target + 16 && addr + n > g_target;
}

std::string EtSummaryLine() {
    std::lock_guard<std::mutex> l(g_hm);
    if (!g_hooked) return "";
    return F("etrace hook 0x%llX (%s) %s calls=%llu\n", U64(g_target), FmtAddr(g_target).c_str(), g_et.enabled ? "logging" : "pass-through", U64(g_et.Total()));
}

std::string CmdEtrace(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "on" && t.size() == 2) {
        if (!g_cfg.allowTrace) return Err("disabled");
        std::string msg;
        g_etWanted = true;
        if (!Install(msg)) {
            if (HookClassify(false, msg) == HookResult::Retry) {
                HookRetryRequest("event_dispatcher", [](std::string& m) { bool ok = Install(m); if (ok) g_et.enabled = g_etWanted.load(); return HookClassify(ok, m); });
                return Err(msg + " (retrying in the background, up to 8 times; see `hooks`)");
            }
            return Err(msg);
        }
        g_et.enabled = true;
        return Ok("event tracing on (hook installed on event_dispatcher)\n");
    }
    if (sub == "off" && t.size() == 2) {
        g_etWanted = false;
        g_et.enabled = false;
        return Ok("event tracing off (hook stays installed as a pass-through; `quit-bridge` removes it)\n");
    }
    if (sub == "clear" && t.size() == 2) {
        g_et.Clear();
        return Ok("event log cleared\n");
    }
    if (sub == "status" && t.size() <= 2) {
        bool hooked;
        uintptr_t tgt;
        { std::lock_guard<std::mutex> l(g_hm); hooked = g_hooked; tgt = g_target; }
        std::string o = F("logging=%d hook_installed=%d target=%s\ncalls_total=%llu logged_since_clear=%llu excluded_from_log=%llu ring=8192\nexclude=%s\n",
                          g_et.enabled ? 1 : 0, hooked ? 1 : 0, tgt ? FmtAddr(tgt).c_str() : "-", U64(g_et.Total()), U64(g_et.Logged()),
                          U64(g_et.ExcludedCalls()), ListStr(g_et.Excluded()).c_str());
        std::string top;
        for (auto& pr : g_et.TopTypes(10)) top += F(top.empty() ? "%u:%llu" : " %u:%llu", pr.first, U64(pr.second));
        o += "top_types_by_lifetime_count (type:count)=" + (top.empty() ? std::string("none") : top) + "\n";
        return Ok(o);
    }
    if (sub == "log" && t.size() <= 3) {
        uint64_t n = 20;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > 8192)) return Err("usage: etrace log [n<=8192]");
        std::vector<EtRec> recs = g_et.Tail(static_cast<size_t>(n));
        std::string o = F("calls_total=%llu shown=%zu\n", U64(g_et.Total()), recs.size());
        for (const EtRec& r : recs) o += EtFormat(r) + "\n";
        return Ok(o);
    }
    if (sub == "summary" && t.size() <= 3) {
        uint64_t secs = 10;
        if (t.size() == 3 && (!ParseNum(t[2], secs) || secs < 1 || secs > 3600)) return Err("usage: etrace summary [seconds 1..3600]");
        long long now = NowUs();
        std::vector<EtGroup> gs = g_et.Summary(now - static_cast<long long>(secs) * 1000000LL);
        std::string o = F("window_s=%llu groups=%zu now_t=%lldus (ring keeps the newest 8192 logged events)\n", U64(secs), gs.size(), now);
        for (const EtGroup& g : gs) {
            o += F("type=%s fn=%s count=%llu first_t=%lldus last_t=%lldus last_age_ms=%lld\n", g.type == 0xFFFFFFFFu ? "?" : F("%u", g.type).c_str(),
                   g.fn ? FmtAddr(static_cast<uintptr_t>(g.fn)).c_str() : "0", U64(g.count), g.first_us, g.last_us, (now - g.last_us) / 1000);
            o += "  first " + EtFormat(g.firstRec) + "\n";
            if (g.count > 1) o += "  last  " + EtFormat(g.lastRec) + "\n";
        }
        return Ok(o);
    }
    if (sub == "window") {
        if (t.size() != 5) return Err("usage: etrace window <t_us> <before_ms<=60000> <after_ms<=60000>");
        uint64_t tc, before, after;
        if (!ParseNum(t[2], tc) || !ParseNum(t[3], before) || !ParseNum(t[4], after) || before > 60000 || after > 60000) return Err("usage: etrace window <t_us> <before_ms<=60000> <after_ms<=60000>");
        long long lo = static_cast<long long>(tc) - static_cast<long long>(before) * 1000, hi = static_cast<long long>(tc) + static_cast<long long>(after) * 1000;
        std::vector<EtRec> recs = g_et.Window(lo, hi);
        std::string o = F("window=[%lld,%lld]us events=%zu\n", lo, hi, recs.size());
        for (const EtRec& r : recs) o += EtFormat(r) + F(" dt=%+lldus\n", r.t_us - static_cast<long long>(tc));
        return Ok(o);
    }
    if (sub == "filter" && t.size() <= 3) {
        if (t.size() == 3) {
            std::string a = Lower(t[2]);
            if (a.compare(0, 8, "exclude=") != 0) return Err("usage: etrace filter exclude=<typeIndex,..>|none");
            std::string list = a.substr(8);
            std::vector<uint32_t> v;
            if (list != "none" && !list.empty()) {
                size_t pos = 0;
                while (pos <= list.size()) {
                    size_t comma = list.find(',', pos);
                    std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
                    uint64_t x;
                    if (!ParseNum(item, x) || x > 65535) return Err("bad type index '" + item + "' (0..65535)");
                    v.push_back(static_cast<uint32_t>(x));
                }
            }
            g_et.SetExcluded(v);
            Log("etrace: exclude filter = %s", ListStr(v).c_str());
        }
        return Ok("exclude=" + ListStr(g_et.Excluded()) + " (these types are dropped from the log only; the game still runs them)\n");
    }
    return Err("usage: etrace on | off | status | log [n] | summary [seconds] | window <t_us> <before_ms> <after_ms> | clear | filter exclude=<types>|none");
}
