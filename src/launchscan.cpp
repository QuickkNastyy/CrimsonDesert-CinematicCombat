#include "launchscan.h"
#include "gamelayout.h"
#include "launchboost.h"
#include "log.h"
#include "telemetry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace {
const uint32_t kFlt = gl::kFltMaxBits;
const size_t kChunk = 1u << 20, kMaxHits = 4096;

// POD + SEH only (legal __try, nothing destructible): positions i in [0, n-12] where the 12-byte pattern FLT_MAX x3 starts (byte-wise, any alignment).
int ScanChunk(const uint8_t* p, size_t n, uint32_t* hits, int maxHits, bool* fault) {
    int k = 0;
    *fault = false;
    if (n < 12) return 0;
    __try {
        size_t end = n - 12;
        for (size_t i = 0; i <= end; ++i) {
            if (*reinterpret_cast<const uint32_t*>(p + i) != kFlt) continue;
            if (*reinterpret_cast<const uint32_t*>(p + i + 4) != kFlt) continue;
            if (*reinterpret_cast<const uint32_t*>(p + i + 8) != kFlt) continue;
            if (k < maxHits) hits[k++] = static_cast<uint32_t>(i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
    }
    return k;
}

bool RegionOk(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT) return false;
    if (m.Type != MEM_PRIVATE && m.Type != MEM_IMAGE) return false;   // heap / image data; file mappings (possibly device memory) are never touched
    if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) return false;
    DWORD b = m.Protect & 0xFF;
    return b == PAGE_READWRITE || b == PAGE_WRITECOPY || b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
}

float F32(const uint8_t* b, size_t off) { float v; memcpy(&v, b + (off - gl::kSKeys), 4); return v; }
uint32_t U32(const uint8_t* b, size_t off) { uint32_t v; memcpy(&v, b + (off - gl::kSKeys), 4); return v; }
}  // namespace

bool LaunchScanValidate(uint64_t S, LaunchScanRec& o) {
    uint8_t b[gl::kSScanSpan];   // S+kSKeys .. S+kSAng1+3
    if (S < 0x10000 || !PeekMem(static_cast<uintptr_t>(S) + gl::kSKeys, b, sizeof b)) return false;
    if (U32(b, gl::kSMark0) != kFlt || U32(b, gl::kSMark1) != kFlt || U32(b, gl::kSMark2) != kFlt) return false;
    float f138 = F32(b, gl::kSNeg), l0 = F32(b, gl::kSStr0), l1 = F32(b, gl::kSStr1), a0 = F32(b, gl::kSAng0), a1 = F32(b, gl::kSAng1);
    if (!(f138 >= -1.0f && f138 < 0.0f)) return false;
    if (!(std::isfinite(l0) && l0 >= 0.0f && l0 < 500.0f && std::isfinite(l1) && l1 >= 0.0f && l1 < 500.0f)) return false;
    o = LaunchScanRec();
    o.S = S;
    memcpy(o.keys, b, 16);
    o.launch[0] = l0; o.launch[1] = l1; o.angles[0] = a0; o.angles[1] = a1; o.f138 = f138;
    o.angleOk = std::isfinite(a0) && a0 > 0.0f && a0 < 3.1416f && std::isfinite(a1) && a1 > 0.0f && a1 < 3.1416f;
    return true;
}

LaunchScan::LaunchScan(uintptr_t lo, uintptr_t hi) {
    lo_ = lo ? lo : 0x10000;
    hi_ = hi ? hi : static_cast<uintptr_t>(0x00007FFFFFFE0000ull);
    scanPos_ = lo_;
    hitBuf_.resize(kMaxHits);
}

void LaunchScan::Exclude(uintptr_t lo, uintptr_t hi) { if (excl_.size() < 8 && hi > lo) excl_.push_back(Range{lo, hi}); }

bool LaunchScan::NextRegion() {
    for (;;) {
        if (scanPos_ >= hi_) return false;
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(scanPos_), &m, sizeof m)) return false;
        uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
        if (end <= scanPos_) { scanPos_ += 0x1000; continue; }
        uintptr_t s = std::max(base, scanPos_);
        scanPos_ = end;
        if (end > hi_) end = hi_;
        if (s >= end) return false;
        if (!RegionOk(m)) { ++st_.regionsSkipped; continue; }
        cur_ = s;
        regEnd_ = end;
        ++st_.regions;
        return true;
    }
}

void LaunchScan::ScanSegment(uintptr_t a, uintptr_t b) {
    bool fault = false;
    int n = ScanChunk(reinterpret_cast<const uint8_t*>(a), b - a, hitBuf_.data(), static_cast<int>(kMaxHits), &fault);
    std::vector<uint64_t> addrs;
    if (!fault) {
        for (int i = 0; i < n; ++i) addrs.push_back(a + hitBuf_[static_cast<size_t>(i)]);
    } else {   // a page vanished under us: redo the segment page by page so only that page is lost
        ++st_.faults;
        for (uintptr_t p = a; p < b; p += 0x1000) {
            uintptr_t e = std::min(b, p + 0x1000 + 11);
            bool f2 = false;
            int m = ScanChunk(reinterpret_cast<const uint8_t*>(p), e - p, hitBuf_.data(), static_cast<int>(kMaxHits), &f2);
            for (int i = 0; i < m; ++i) addrs.push_back(p + hitBuf_[static_cast<size_t>(i)]);
        }
    }
    for (uint64_t hit : addrs) {
        ++st_.hits;
        LaunchScanRec r;
        if (hit >= gl::kSMark0 && LaunchScanValidate(hit - gl::kSMark0, r)) { recs_.push_back(r); ++st_.accepted; }
        else ++st_.rejected;
    }
}

bool LaunchScan::Step(long long budgetUs) {
    if (st_.done) return true;
    long long t0 = NowUs();
    for (;;) {
        if (cur_ >= regEnd_) {
            if (!NextRegion()) { st_.done = true; break; }
        } else {
            uintptr_t a = cur_, b = std::min(regEnd_, a + kChunk);
            bool inEx = false, cut = false;
            for (const Range& ex : excl_) if (a >= ex.lo && a < ex.hi) { cur_ = std::min(ex.hi, regEnd_); inEx = true; break; }
            if (!inEx) {
                for (const Range& ex : excl_) if (ex.lo > a && ex.lo < b) { b = ex.lo; cut = true; }
                ScanSegment(a, b);
                st_.bytes += b - a;
                cur_ = (b < regEnd_ && !cut && b - a > 11) ? b - 11 : b;   // overlap 11 bytes when the chunk (not a region / exclusion) ended the segment
                if ((++chunks_ & 15) == 0) SwitchToThread();
            }
        }
        if (NowUs() - t0 >= budgetUs) break;
    }
    st_.elapsedUs += NowUs() - t0;
    st_.cursor = cur_;
    return st_.done;
}

std::vector<LaunchScanRec> LaunchScan::Records() const {
    std::vector<LaunchScanRec> v = recs_;
    auto less = [](const LaunchScanRec& x, const LaunchScanRec& y) {
        for (int i = 0; i < 4; ++i) if (x.keys[i] != y.keys[i]) return x.keys[i] < y.keys[i];
        return x.S < y.S;
    };
    std::sort(v.begin(), v.end(), less);
    v.erase(std::unique(v.begin(), v.end(), [](const LaunchScanRec& x, const LaunchScanRec& y) { return x.S == y.S; }), v.end());
    return v;
}

// ------------------------------------------------------------------ output
namespace {
void JNum(std::string& o, float f) { o += std::isfinite(f) ? F("%.7g", static_cast<double>(f)) : std::string("null"); }
}

std::string LaunchScanJson(const std::vector<LaunchScanRec>& recs, const LaunchScanStats& st, const std::string& when, size_t cap) {
    std::string o = F("{\"schema\":1,\"when\":\"%s\",\"complete\":%s,\"bytes\":%llu,\"regions\":%llu,\"regions_skipped\":%llu,\"pattern_hits\":%llu,\"rejected\":%llu,\"faults\":%llu,\"elapsed_ms\":%lld,\"count\":%zu,\"zero_count\":%zu,",
                      when.c_str(), st.done ? "true" : "false", U64(st.bytes), U64(st.regions), U64(st.regionsSkipped), U64(st.hits), U64(st.rejected), U64(st.faults), st.elapsedUs / 1000, recs.size(),
                      static_cast<size_t>(std::count_if(recs.begin(), recs.end(), [](const LaunchScanRec& r) { return r.Zero(); })));
    o += F("\"truncated\":%s,\"records\":[", recs.size() > cap ? "true" : "false");
    size_t n = std::min(cap, recs.size());
    for (size_t i = 0; i < n; ++i) {
        const LaunchScanRec& r = recs[i];
        o += i ? ",\n" : "\n";
        o += F("{\"S\":\"0x%llX\",\"keys\":[%u,%u,%u,%u],\"launch\":[", U64(r.S), r.keys[0], r.keys[1], r.keys[2], r.keys[3]);
        JNum(o, r.launch[0]); o += ','; JNum(o, r.launch[1]);
        o += "],\"angles\":[";
        JNum(o, r.angles[0]); o += ','; JNum(o, r.angles[1]);
        o += "],\"f138\":";
        JNum(o, r.f138);
        o += F(",\"angle_ok\":%s,\"zero\":%s}", r.angleOk ? "true" : "false", r.Zero() ? "true" : "false");
    }
    o += "\n]}";
    return o;
}

std::string LaunchScanTable(const std::vector<LaunchScanRec>& recs, int mode, size_t cap) {
    std::string o;
    size_t lines = 0;
    if (mode == 0) {   // grouped by the 4 SkillKeys
        std::map<std::vector<uint32_t>, std::vector<const LaunchScanRec*>> g;
        for (const LaunchScanRec& r : recs) g[std::vector<uint32_t>(r.keys, r.keys + 4)].push_back(&r);
        for (const auto& kv : g) {
            if (lines++ >= cap) { o += F("... %zu more key groups (use `launch scan all` or the JSON file)\n", g.size() - cap); break; }
            std::map<std::pair<float, float>, int> vals;
            for (const LaunchScanRec* r : kv.second) ++vals[{r->launch[0], r->launch[1]}];
            o += F("keys=[%u,%u,%u,%u] records=%zu launch:", kv.first[0], kv.first[1], kv.first[2], kv.first[3], kv.second.size());
            for (const auto& v : vals) o += F(" (%g,%g)x%d", static_cast<double>(v.first.first), static_cast<double>(v.first.second), v.second);
            o += "  S:";
            for (size_t i = 0; i < kv.second.size() && i < 3; ++i) o += F(" 0x%llX", U64(kv.second[i]->S));
            if (kv.second.size() > 3) o += " ...";
            o += "\n";
        }
        return o;
    }
    for (const LaunchScanRec& r : recs) {
        if (mode == 2 && !r.Zero()) continue;
        if (lines++ >= cap) { o += "... more (use the JSON file)\n"; break; }
        o += F("keys=[%u,%u,%u,%u] launch=(%g,%g) angles=(%g,%g)%s S=0x%llX\n", r.keys[0], r.keys[1], r.keys[2], r.keys[3], static_cast<double>(r.launch[0]), static_cast<double>(r.launch[1]), static_cast<double>(r.angles[0]),
               static_cast<double>(r.angles[1]), r.angleOk ? "" : " (angles not sane)", U64(r.S));
    }
    return o;
}

// ------------------------------------------------------------------ bridge command
namespace {
std::mutex g_mu;
std::unique_ptr<LaunchScan> g_scan;
std::vector<LaunchScanRec> g_last;
LaunchScanStats g_lastSt;
std::string g_when;

std::string NowStamp(bool forFile) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return forFile ? F("%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) : F("%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

std::string Summary(const std::vector<LaunchScanRec>& recs, const LaunchScanStats& st) {
    size_t zero = static_cast<size_t>(std::count_if(recs.begin(), recs.end(), [](const LaunchScanRec& r) { return r.Zero(); }));
    return F("%s: %.2f GB scanned in %.1f s, %llu regions (%llu skipped), %llu pattern hits, %zu launch records (S unique), %zu with launch 0/0, %llu hits rejected, %llu read faults\n", st.done ? "scan complete" : "scan INCOMPLETE (time budget)",
             static_cast<double>(st.bytes) / 1073741824.0, static_cast<double>(st.elapsedUs) / 1e6, U64(st.regions), U64(st.regionsSkipped), U64(st.hits), recs.size(), zero, U64(st.rejected), U64(st.faults));
}
}  // namespace

std::string LaunchScanLastJson(size_t cap) {
    std::unique_lock<std::mutex> l(g_mu, std::try_to_lock);
    if (!l.owns_lock() || g_when.empty()) return "null";
    return LaunchScanJson(g_last, g_lastSt, g_when, cap);
}

std::string CmdLaunchScan(const Args& t) {
    std::string sub = t.size() > 2 ? Lower(t[2]) : "";
    auto modeOf = [&](const std::string& w) { return w == "zero" ? 2 : w == "all" ? 1 : 0; };
    if (sub == "show") {
        std::string w = t.size() > 3 ? Lower(t[3]) : "";
        if (t.size() > 4 || (!w.empty() && w != "zero" && w != "all")) return Err("usage: launch scan show [zero|all]");
        std::lock_guard<std::mutex> l(g_mu);
        if (g_when.empty()) return Err("no scan yet (`launch scan`)");
        return Ok(Summary(g_last, g_lastSt) + LaunchScanTable(g_last, modeOf(w), w.empty() ? 300 : 1000));
    }
    bool more = sub == "more";
    if (!(sub.empty() || sub == "zero" || sub == "all" || more) || t.size() > 3) return Err("usage: launch scan [zero|all|more] | launch scan show [zero|all]");
    std::unique_lock<std::mutex> l(g_mu, std::try_to_lock);
    if (!l.owns_lock()) return Err("a scan is already running");
    if (more && !g_scan) return Err("no unfinished scan (`launch scan` starts one)");
    if (!more || !g_scan) {
        g_scan.reset(new LaunchScan());
        uintptr_t lo = 0, hi = 0;
        Launch().OwnRange(lo, hi);
        g_scan->Exclude(lo, hi);   // our own first-seen copies of S must not be reported as records
    }
    int prio = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // never compete with the game threads
    bool done = g_scan->Step(25LL * 1000000);
    SetThreadPriority(GetCurrentThread(), prio);
    std::vector<LaunchScanRec> recs = g_scan->Records();
    LaunchScanStats st = g_scan->Stats();
    g_last = recs;
    g_lastSt = st;
    g_when = NowStamp(false);
    std::string o = Summary(recs, st);
    if (!done) return Ok(o + F("continue with `launch scan more` (cursor 0x%llX)\n", U64(st.cursor)));
    g_scan.reset();
    std::wstring dir = TelemetryDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string stamp = NowStamp(true);
    std::wstring wp = dir + L"\\launch_scan_" + std::wstring(stamp.begin(), stamp.end()) + L".json";
    std::string json = LaunchScanJson(recs, st, g_when, 100000);
    std::wstring tmp = wp + L".tmp";
    bool wrote = false;
    {
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            wrote = WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &w, nullptr) && w == json.size();
            CloseHandle(f);
        }
    }
    if (wrote) wrote = MoveFileExW(tmp.c_str(), wp.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!wrote) DeleteFileW(tmp.c_str());
    std::string path;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), static_cast<int>(wp.size()), nullptr, 0, nullptr, nullptr);
        path.assign(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), static_cast<int>(wp.size()), &path[0], n, nullptr, nullptr);
    }
    o += wrote ? "full table written to " + path + " (and into the next telemetry session file)\n" : "WARNING: could not write the JSON file\n";
    Log("launch scan: %zu records, %.1f s, json %s", recs.size(), static_cast<double>(st.elapsedUs) / 1e6, wrote ? path.c_str() : "NOT written");
    int mode = modeOf(sub);
    return Ok(o + LaunchScanTable(recs, mode, mode == 0 ? 300 : 1000));
}
