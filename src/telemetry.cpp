#include "telemetry.h"
#include "gamelayout.h"
#include "launchscan.h"
#include "layoutcheck.h"
#include "etrace.h"
#include "features.h"
#include "hookretry.h"
#include "log.h"
#include "settings.h"
#include "skillctx.h"
#include "timeamp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
// ------------------------------------------------------------------ JSON helpers
void JStr(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    o += '"';
}
void JHex(std::string& o, const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    o += '"';
    for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; }
    o += '"';
}
void JNum(std::string& o, double v) {
    if (!std::isfinite(v)) { o += "null"; return; }
    char b[40];
    snprintf(b, sizeof b, "%.7g", v);   // floats: 7 significant digits round-trip a float and print 0.6f as 0.6
    o += b;
}
void JU(std::string& o, unsigned long long v) { char b[32]; snprintf(b, sizeof b, "%llu", v); o += b; }
void JHexNum(std::string& o, unsigned long long v) { char b[32]; snprintf(b, sizeof b, "\"0x%llX\"", v); o += b; }
float AsF(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
uint32_t AsU(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
// rva of an address inside the main module, or -1
long long Rva(const TelModule& m, uint64_t a) {
    return (m.base && a >= m.base && a < m.base + (m.sizeOfImage ? m.sizeOfImage : 0xFFFFFFFFu)) ? static_cast<long long>(a - m.base) : -1;
}
void JRva(std::string& o, const TelModule& m, uint64_t a) {
    long long r = Rva(m, a);
    if (!a) o += "null";
    else if (r >= 0) JHexNum(o, static_cast<unsigned long long>(r));
    else { o += "\"abs:"; char b[24]; snprintf(b, sizeof b, "0x%llX", U64(a)); o += b; o += '"'; }
}

// ------------------------------------------------------------------ minimal strict JSON validator
struct JP {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool lit(const char* s) { size_t n = strlen(s); if (static_cast<size_t>(e - p) < n || strncmp(p, s, n)) return false; p += n; return true; }
    bool str() {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            if (static_cast<unsigned char>(*p) < 0x20) return false;
            if (*p == '\\') { ++p; if (p >= e) return false; if (*p == 'u') { for (int i = 0; i < 4; ++i) { ++p; if (p >= e || !isxdigit(static_cast<unsigned char>(*p))) return false; } } else if (!strchr("\"\\/bfnrt", *p)) return false; }
            ++p;
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool num() {
        const char* s = p;
        if (p < e && *p == '-') ++p;
        if (p >= e || !isdigit(static_cast<unsigned char>(*p))) return false;
        if (*p == '0') ++p; else while (p < e && isdigit(static_cast<unsigned char>(*p))) ++p;
        if (p < e && *p == '.') { ++p; if (p >= e || !isdigit(static_cast<unsigned char>(*p))) return false; while (p < e && isdigit(static_cast<unsigned char>(*p))) ++p; }
        if (p < e && (*p == 'e' || *p == 'E')) { ++p; if (p < e && (*p == '+' || *p == '-')) ++p; if (p >= e || !isdigit(static_cast<unsigned char>(*p))) return false; while (p < e && isdigit(static_cast<unsigned char>(*p))) ++p; }
        return p > s;
    }
    bool val() {
        if (++depth > 64) return false;
        ws();
        bool ok = false;
        if (p >= e) ok = false;
        else if (*p == '{') {
            ++p; ws();
            if (p < e && *p == '}') { ++p; ok = true; }
            else for (;;) {
                ws();
                if (!str()) break;
                ws();
                if (p >= e || *p != ':') break;
                ++p;
                if (!val()) break;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; ok = true; }
                break;
            }
        } else if (*p == '[') {
            ++p; ws();
            if (p < e && *p == ']') { ++p; ok = true; }
            else for (;;) {
                if (!val()) break;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; ok = true; }
                break;
            }
        } else if (*p == '"') ok = str();
        else if (*p == 't') ok = lit("true");
        else if (*p == 'f') ok = lit("false");
        else if (*p == 'n') ok = lit("null");
        else ok = num();
        --depth;
        return ok;
    }
};
}  // namespace

bool TelemetryJsonValid(const std::string& text) {
    JP j{text.data(), text.data() + text.size()};
    if (!j.val()) return false;
    j.ws();
    return j.p == j.e;
}

std::string TelemetryFileName(uint32_t stamp, const SYSTEMTIME& t) {
    return F("%08X_%04d%02d%02d_%02d%02d%02d.json", stamp, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// ------------------------------------------------------------------ feeds
void TelemetryStore::OnEvent(uint32_t type, uint64_t H, uint64_t fn, const uint8_t* raw, uint64_t a1) noexcept {
    if (a1) a1_.store(a1, std::memory_order_relaxed);
    if (type >= kEv) { overflow_.fetch_add(1, std::memory_order_relaxed); return; }
    Ev& e = ev_[type];
    int st = e.state.load(std::memory_order_acquire);
    if (st == 0) {
        int z = 0;
        if (e.state.compare_exchange_strong(z, 1)) {   // first sight: remember the handler, a fingerprint of its code and one sample of the data
            e.H = H; e.fn = fn;
            e.fpOk = fn && PeekMem(static_cast<uintptr_t>(fn), e.fp, 24);
            memcpy(e.d, raw, 96);
            e.state.store(2, std::memory_order_release);
        }
    }
    e.count.fetch_add(1, std::memory_order_relaxed);
}

void TelemetryStore::OnSkill(const uint32_t keys[4], bool player) noexcept {
    for (int i = 0; i < 4; ++i) {
        uint32_t k = keys[i];
        if (!k) continue;
        size_t h = (static_cast<size_t>(k) * 2654435761u >> 11) & (kKey - 1);
        for (size_t p = 0; p < 32; ++p) {
            Key& s = key_[(h + p) & (kKey - 1)];
            uint32_t cur = s.key.load(std::memory_order_acquire);
            if (cur == 0) {
                uint32_t z = 0;
                if (s.key.compare_exchange_strong(z, k)) cur = k; else cur = s.key.load();
            }
            if (cur == k) {
                s.n.fetch_add(1, std::memory_order_relaxed);
                if (player) s.player.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
}

void TelemetryStore::OnSlowmo(float scale, float a, float b, const uint32_t keys[4], bool haveCtx, bool modified, bool filtered) noexcept {
    uint32_t bs = AsU(scale), ba = AsU(a), bb = AsU(b);
    uint64_t h = Fnv(&bs, 4);
    h = Fnv(&ba, 4, h);
    h = Fnv(&bb, 4, h);
    size_t base = static_cast<size_t>(h) & (kSlow - 1);
    for (size_t p = 0; p < kSlow; ++p) {
        Slow& s = slow_[(base + p) & (kSlow - 1)];
        if (!s.used.load(std::memory_order_acquire)) {
            s.s = bs; s.a = ba; s.b = bb;
            s.used.store(1, std::memory_order_release);
        } else if (s.s.load() != bs || s.a.load() != ba || s.b.load() != bb) continue;
        s.count.fetch_add(1, std::memory_order_relaxed);
        if (modified) s.modified.fetch_add(1, std::memory_order_relaxed);
        if (filtered) s.filtered.fetch_add(1, std::memory_order_relaxed);
        uint32_t k = haveCtx ? keys[0] : 0;   // histogram of the context key0 (0 = no context)
        for (int i = 0; i < 8; ++i) {
            if (s.hn[i].load() == 0) { s.hk[i] = k; s.hn[i] = 1; break; }
            if (s.hk[i].load() == k) { s.hn[i].fetch_add(1); break; }
        }
        return;
    }
}

void TelemetryStore::OnHitStop(long long minute, uint32_t key0, bool haveCtx) noexcept {
    Hs& h = hs_[static_cast<size_t>(minute) % kHs];
    if (h.minute.load() != minute) {   // slot of an older minute: recycle
        h.minute.store(minute);
        h.count.store(0);
        for (int i = 0; i < 8; ++i) h.hn[i].store(0);
    }
    h.count.fetch_add(1);
    uint32_t k = haveCtx ? key0 : 0;
    for (int i = 0; i < 8; ++i) {
        if (h.hn[i].load() == 0) { h.hk[i] = k; h.hn[i] = 1; break; }
        if (h.hk[i].load() == k) { h.hn[i].fetch_add(1); break; }
    }
}

std::vector<TelemetryStore::HsMinute> TelemetryStore::HitStopMinutes() const {
    std::vector<HsMinute> v;
    for (const Hs& h : hs_) {
        if (h.minute.load() < 0 || h.count.load() == 0) continue;
        HsMinute m;
        m.minute = h.minute.load();
        m.count = h.count.load();
        for (int i = 0; i < 8; ++i) if (h.hn[i].load()) m.keys.emplace_back(h.hk[i].load(), h.hn[i].load());
        v.push_back(m);
    }
    std::sort(v.begin(), v.end(), [](const HsMinute& a, const HsMinute& b) { return a.minute < b.minute; });
    return v;
}

TelemetryStore::Counts TelemetryStore::Count() const {
    Counts c;
    for (const Ev& e : ev_) if (e.state.load() == 2) { ++c.eventTypes; c.events += e.count.load(); }
    for (const Key& k : key_) if (k.key.load()) ++c.skillKeys;
    for (const Slow& s : slow_) if (s.used.load()) ++c.slowLayouts;
    c.overflow = overflow_.load();
    return c;
}

// ------------------------------------------------------------------ JSON
std::string TelemetryStore::BuildJson(const TelInputs& in) const {
    const TelModule& m = in.module;
    std::string o;
    o.reserve(1 << 20);
    o += "{\n\"schema\":1,\n\"plugin\":";
    JStr(o, m.plugin);
    o += ",\n\"session\":{\"started\":";
    JStr(o, in.started);
    o += ",\"written\":";
    JStr(o, in.written);
    o += ",\"uptime_s\":";
    JU(o, static_cast<unsigned long long>(in.uptimeS < 0 ? 0 : in.uptimeS));
    o += "},\n\"module\":{\"exe\":";
    JStr(o, m.exePath);
    o += ",\"time_date_stamp\":";
    JHexNum(o, m.timeDateStamp);
    o += ",\"size_of_image\":";
    JU(o, m.sizeOfImage);
    o += ",\"file_size\":";
    JU(o, m.fileSize);
    o += ",\"base\":";
    JHexNum(o, m.base);
    o += "},\n\"layout\":";
    o += LayoutJson();   // the build-specific layout constants this plugin was built with (src/gamelayout.h)
    o += ",\n\"signatures\":[";
    for (size_t i = 0; i < in.sigs.size(); ++i) {
        const Sig& s = in.sigs[i];
        o += i ? ",\n" : "\n";
        o += "{\"name\":";
        JStr(o, s.name);
        o += ",\"status\":";
        JStr(o, SigStatusName(s.st));
        o += ",\"matches\":";
        JU(o, s.matches);
        o += ",\"rva\":";
        if (s.st == SigStatus::Resolved) JRva(o, m, s.addr); else o += "null";
        o += ",\"match_rva\":";
        if (s.matches == 1) JRva(o, m, s.match); else o += "null";
        o += ",\"fingerprint\":";
        if (s.fpOk) JHex(o, s.fp, 24); else o += "null";
        o += '}';
    }
    o += "\n],\n\"hooks\":[";
    for (size_t i = 0; i < in.hooks.size(); ++i) {
        const TelHook& h = in.hooks[i];
        o += i ? ",\n" : "\n";
        o += "{\"name\":";
        JStr(o, h.name);
        o += ",\"rva\":";
        JRva(o, m, h.addr);
        o += ",\"state\":";
        JStr(o, h.state);
        o += ",\"fingerprint\":";
        if (h.fpOk) JHex(o, h.fp, 24); else o += "null";
        o += '}';
    }
    o += "\n],\n\"event_table\":{\"a1\":";
    JHexNum(o, in.tableA1);
    o += ",\"slots\":[";
    for (size_t i = 0; i < in.table.size(); ++i) {
        const TelSlot& s = in.table[i];
        o += i ? ",\n" : "\n";
        o += "{\"type\":";
        JU(o, static_cast<unsigned long long>(s.slot));
        o += ",\"fn_rva\":";
        JRva(o, m, s.fn);
        o += ",\"fingerprint\":";
        if (s.fpOk) JHex(o, s.fp, 24); else o += "null";
        o += '}';
    }
    o += "\n]},\n\"events\":[";
    bool first = true;
    for (size_t t = 0; t < kEv; ++t) {
        const Ev& e = ev_[t];
        if (e.state.load(std::memory_order_acquire) != 2) continue;
        o += first ? "\n" : ",\n";
        first = false;
        o += "{\"type\":";
        JU(o, t);
        o += ",\"fn_rva\":";
        JRva(o, m, e.fn);
        o += ",\"fingerprint\":";
        if (e.fpOk) JHex(o, e.fp, 24); else o += "null";
        o += ",\"count\":";
        JU(o, e.count.load());
        o += ",\"sample_d\":";
        JHex(o, e.d, 96);
        o += ",\"sample_floats\":[";
        for (int k = 0; k < 6; ++k) {
            float f;
            memcpy(&f, e.d + gl::kEvSampleFloats + 4 * k, 4);
            if (k) o += ',';
            JNum(o, f);
        }
        o += "]}";
    }
    o += "\n],\n\"event_types_overflow\":";
    JU(o, overflow_.load());
    o += ",\n\"skill_keys\":[";
    first = true;
    for (const Key& k : key_) {
        if (!k.key.load()) continue;
        o += first ? "\n" : ",\n";
        first = false;
        o += "{\"key\":";
        JU(o, k.key.load());
        o += ",\"count\":";
        JU(o, k.n.load());
        o += ",\"player_count\":";
        JU(o, k.player.load());
        o += '}';
    }
    o += "\n],\n\"launch_records\":[";
    for (size_t i = 0; i < in.launch.size(); ++i) {
        const LaunchProbe& lp = in.launch[i];
        const LaunchRec& r = lp.r;
        uint64_t id = Fnv(r.keys, 16);
        id = Fnv(r.orig, 16, id);
        o += i ? ",\n" : "\n";
        o += "{\"id\":";
        JHexNum(o, id);
        o += ",\"keys\":[";
        for (int k = 0; k < 4; ++k) { if (k) o += ','; JU(o, r.keys[k]); }
        o += "],\"orig_strength\":[";
        JNum(o, r.orig[0]); o += ','; JNum(o, r.orig[1]);
        o += "],\"orig_angles\":[";
        JNum(o, r.orig[2]); o += ','; JNum(o, r.orig[3]);
        o += "],\"angle_ok\":";
        o += r.angleOk ? "true" : "false";
        o += ",\"calls\":";
        JU(o, r.calls);
        o += ",\"player\":";
        o += r.lastPlayer ? "true" : "false";
        o += ",\"marker\":[";
        for (int k = 0; k < 3; ++k) {
            uint32_t v;
            memcpy(&v, lp.bytes + gl::kSMark0 + 4 * k, 4);
            if (k) o += ',';
            JHexNum(o, v);
        }
        o += "],\"s_probe\":";
        if (lp.ok) JHex(o, lp.bytes, sizeof lp.bytes); else o += "null";
        o += '}';
    }
    o += "\n],\n\"launch_scan\":";
    o += in.launchScan.empty() ? std::string("null") : in.launchScan;
    o += ",\n\"blinding_flash_chains\":";
    o += in.bfChains.empty() ? std::string("[]") : in.bfChains;
    o += ",\n\"slowmo_layouts\":[";
    first = true;
    for (const Slow& s : slow_) {
        if (!s.used.load(std::memory_order_acquire)) continue;
        o += first ? "\n" : ",\n";
        first = false;
        o += "{\"scale\":";
        JNum(o, AsF(s.s.load()));
        o += ",\"a\":";
        JNum(o, AsF(s.a.load()));
        o += ",\"b\":";
        JNum(o, AsF(s.b.load()));
        o += ",\"count\":";
        JU(o, s.count.load());
        o += ",\"modified\":";
        JU(o, s.modified.load());
        o += ",\"filtered\":";
        JU(o, s.filtered.load());
        o += ",\"keys\":[";
        bool f2 = true;
        for (int i = 0; i < 8; ++i) {
            if (!s.hn[i].load()) continue;
            if (!f2) o += ',';
            f2 = false;
            o += "{\"key\":";
            JU(o, s.hk[i].load());
            o += ",\"count\":";
            JU(o, s.hn[i].load());
            o += '}';
        }
        o += "]}";
    }
    o += "\n],\n\"hitstop_per_minute\":[";
    first = true;
    for (const HsMinute& m : HitStopMinutes()) {
        o += first ? "\n" : ",\n";
        first = false;
        o += "{\"minute\":";
        JU(o, static_cast<unsigned long long>(m.minute));
        o += ",\"count\":";
        JU(o, m.count);
        o += ",\"keys\":[";
        for (size_t i = 0; i < m.keys.size(); ++i) {
            if (i) o += ',';
            o += "{\"key\":";
            JU(o, m.keys[i].first);
            o += ",\"count\":";
            JU(o, m.keys[i].second);
            o += '}';
        }
        o += "]}";
    }
    o += "\n]\n}\n";
    return o;
}

// ------------------------------------------------------------------ live session
namespace {
TelemetryStore g_store;
std::atomic<bool> g_on{false}, g_quit{false}, g_started{false};
std::mutex g_hm;
std::vector<TelHook> g_hooks;
HANDLE g_ev = nullptr, g_done = nullptr, g_thread = nullptr;
std::wstring g_tdir;
std::wstring g_file;
std::string g_startedStr, g_lastNote;
int g_maxMB = 20;
long long g_startUs = 0;
std::atomic<uint64_t> g_writes{0}, g_lastBytes{0};
std::atomic<bool> g_dumpReq{false};
std::mutex g_wm;   // one writer at a time

std::string NowStr() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return F("%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

TelModule LiveModule() {
    TelModule m;
    ModInfo mi = MainModule();
    m.base = mi.base;
    m.sizeOfImage = static_cast<uint32_t>(mi.size);
    m.plugin = std::string("CombatMod milestone-B2 built ") + __DATE__ + " " + __TIME__;
    if (mi.base) {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mi.base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mi.base + dos->e_lfanew);
        m.timeDateStamp = nt->FileHeader.TimeDateStamp;
    }
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring wp(p, n);
    m.exePath = Narrow(wp);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(wp.c_str(), GetFileExInfoStandard, &fa)) m.fileSize = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    return m;
}

std::vector<TelSlot> EnumerateTable(uint64_t a1) {
    std::vector<TelSlot> v;
    if (!a1) return v;
    int nullRun = 0;
    for (int i = 0; i < 256 && nullRun < 32; ++i) {   // slots 0..255; stop after a long run of empty/unreadable slots
        uint64_t H = 0, vt = 0, fn = 0;
        if (!PeekMem(static_cast<uintptr_t>(a1) + gl::kEvTableBase + gl::kEvTableStride * static_cast<uintptr_t>(i), &H, 8) || !H || !PeekMem(static_cast<uintptr_t>(H), &vt, 8) || !vt ||
            !PeekMem(static_cast<uintptr_t>(vt) + gl::kHandlerFnInVtable, &fn, 8) || !fn) { ++nullRun; continue; }
        nullRun = 0;
        TelSlot s;
        s.slot = i; s.H = H; s.fn = fn;
        s.fpOk = PeekMem(static_cast<uintptr_t>(fn), s.fp, 24);
        v.push_back(s);
    }
    return v;
}

TelInputs CollectLive() {   // (TelemetryDir() is defined below with the other public API)
    TelInputs in;
    in.module = LiveModule();
    in.sigs = SigsSnapshot();
    {
        std::lock_guard<std::mutex> l(g_hm);
        in.hooks = g_hooks;
    }
    for (TelHook& h : in.hooks) {
        if (h.name == "time_curve_start") h.state = AmpHooked() ? "installed" : "not installed";
        else if (h.name == "apply_attack_skill_to_position") h.state = SkillsHooked() ? "installed" : "not installed";
    }
    in.tableA1 = g_store.TableA1();
    in.table = EnumerateTable(in.tableA1);
    in.launch = Launch().Probes();
    in.launchScan = LaunchScanLastJson(4000);
    in.bfChains = AmpBfChainsJson(50);
    in.started = g_startedStr;
    in.written = NowStr();
    in.uptimeS = (NowUs() - g_startUs) / 1000000;
    return in;
}

void Prune(const std::wstring& dir, const std::wstring& keepFile, uint64_t maxBytes, uint64_t incoming) {
    struct F { std::wstring path; uint64_t size; FILETIME t; };
    std::vector<F> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    uint64_t total = incoming;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring full = dir + L"\\" + fd.cFileName;
            uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (_wcsicmp(full.c_str(), keepFile.c_str()) == 0) continue;   // the file being rewritten does not count twice
            files.push_back({full, sz, fd.ftLastWriteTime});
            total += sz;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) < 0; });   // oldest first
    for (const F& f : files) {
        if (total <= maxBytes) break;
        if (DeleteFileW(f.path.c_str())) { total -= f.size; Log("telemetry: removed old session file %s (size cap %d MB)", Narrow(f.path).c_str(), g_maxMB); }
    }
}

bool WriteSession(std::string& err) {
    std::lock_guard<std::mutex> l(g_wm);
    TelInputs in = CollectLive();
    std::string json = g_store.BuildJson(in);
    if (g_file.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        g_file = g_tdir + L"\\" + Wide(TelemetryFileName(in.module.timeDateStamp, t));
    }
    CreateDirectoryW(g_tdir.c_str(), nullptr);
    uint64_t cap = static_cast<uint64_t>(g_maxMB) * 1024 * 1024;
    if (json.size() > cap) { err = "the session data alone exceeds MaxMB"; return false; }
    Prune(g_tdir, g_file, cap, json.size());
    std::wstring tmp = g_file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = F("cannot create the file (error %lu)", GetLastError()); return false; }
    size_t off = 0;
    bool ok = true;
    while (off < json.size() && ok) {
        DWORD w = 0;
        ok = WriteFile(f, json.data() + off, static_cast<DWORD>(std::min<size_t>(json.size() - off, 1u << 20)), &w, nullptr) && w;
        off += w;
    }
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); err = "write failed"; return false; }
    ++g_writes;
    g_lastBytes = json.size();
    return true;
}

DWORD WINAPI Writer(LPVOID) {
    RegisterInternalThread();
    bool firstDone = false;
    DWORD waitMs = 4000;   // let the signatures and hooks settle, then write the static parts
    while (!g_quit) {
        WaitForSingleObject(g_ev, waitMs);
        if (g_quit) break;
        waitMs = 60000;
        if (!g_on && !g_dumpReq) continue;
        std::string err;
        try {
            if (!WriteSession(err)) { Log("telemetry: write failed: %s", err.c_str()); g_lastNote = err; }
            else if (!firstDone) { firstDone = true; Log("telemetry: session file %s written (%llu bytes), refreshed every 60 s", Narrow(g_file).c_str(), U64(g_lastBytes.load())); }
        } catch (...) {
            Log("telemetry: exception while writing (ignored)");
        }
        if (g_dumpReq.exchange(false) && g_done) SetEvent(g_done);
    }
    UnregisterInternalThread();
    return 0;
}

void RequestHooks() {   // the event catalog / skill keys / slow-mo layouts need the three hooks; installed in the background with retries
    SkillsRequestOn();
    EtRequestTap();
    FeaturesApply();   // the amplifier hook (in observe-only mode when SlowMo/HitStop are off)
}
}  // namespace

TelemetryStore& Telemetry() { return g_store; }
bool TelemetryOn() { return g_on.load(); }
void TelemetryRequestHooks() { if (g_on.load()) RequestHooks(); }
std::wstring TelemetryDir() { return g_tdir.empty() ? g_dir + L"CombatMod_telemetry" : g_tdir; }

void TelemetryNoteHook(const std::string& name, uint64_t addr) {
    TelHook h;
    h.name = name; h.addr = addr; h.state = "installed";
    h.fpOk = PeekMem(static_cast<uintptr_t>(addr), h.fp, 24);
    std::lock_guard<std::mutex> l(g_hm);
    for (TelHook& x : g_hooks) if (x.name == name) { if (!x.fpOk) x = h; return; }   // keep the first (pre-hook) bytes
    g_hooks.push_back(h);
}

void TelemetryInit() {
    std::string v;
    bool enabled = false;   // OFF unless [Telemetry] Enabled=1 (a missing / unreadable ini must not turn the recorder and its hook on)
    g_maxMB = 20;
    if (IniGet("telemetry.enabled", v)) enabled = strtol(v.c_str(), nullptr, 0) != 0;
    if (IniGet("telemetry.maxmb", v)) g_maxMB = std::min(500, std::max(1, static_cast<int>(strtol(v.c_str(), nullptr, 0))));
    if (IniGet("telemetry.dir", v) && !v.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), static_cast<int>(v.size()), nullptr, 0);
        g_tdir.assign(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, v.c_str(), static_cast<int>(v.size()), &g_tdir[0], n);
    } else {
        std::wstring base = ::g_dir;
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
        g_tdir = base + L"\\CombatMod_telemetry";
    }
    g_startedStr = NowStr();
    g_startUs = NowUs();
    bool exp = false;
    if (g_started.compare_exchange_strong(exp, true)) {
        g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr);
        if (!g_thread) { Log("telemetry: could not start the writer thread (disabled)"); g_started = false; return; }
    }
    g_on = enabled;
    if (!enabled) { Log("telemetry: Enabled=0"); return; }
    Skills().feedTelemetry = true;
    Etrace().feedTelemetry = true;
    AmpFeedTelemetry(true);
    RequestHooks();
    Log("telemetry: on, directory %s, size cap %d MB", Narrow(g_tdir).c_str(), g_maxMB);
}

void TelemetryShutdown(bool wait) {
    bool wasOn = g_on.exchange(false);
    Skills().feedTelemetry = false;
    Etrace().feedTelemetry = false;
    AmpFeedTelemetry(false);
    if (wait && wasOn && g_started) {   // final write on the calling thread (not in the loader lock)
        std::string err;
        try { if (WriteSession(err)) Log("telemetry: final session file written"); else Log("telemetry: final write failed: %s", err.c_str()); } catch (...) {}
    }
    g_quit = true;
    if (g_ev) SetEvent(g_ev);
    if (wait && g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
}

std::string CmdTelemetry(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "on" && t.size() == 2) {
        if (!g_started) return Err("the telemetry writer is not running");
        g_on = true;
        Skills().feedTelemetry = true;
        Etrace().feedTelemetry = true;
        AmpFeedTelemetry(true);
        RequestHooks();
        return Ok("telemetry on (hooks are being installed in the background; see `hooks`)\n");
    }
    if (sub == "off" && t.size() == 2) {
        g_on = false;
        Skills().feedTelemetry = false;
        Etrace().feedTelemetry = false;
        AmpFeedTelemetry(false);
        FeaturesApply();
        return Ok("telemetry off (collected data stays in memory; the hooks stay as pass-through)\n");
    }
    if (sub == "dump" && t.size() == 2) {
        if (!g_started) return Err("the telemetry writer is not running");
        g_dumpReq = true;
        ResetEvent(g_done);
        SetEvent(g_ev);
        if (WaitForSingleObject(g_done, 5000) != WAIT_OBJECT_0) return Err("the writer did not finish in time (see `telemetry status`)");
        return Ok(F("written %llu bytes to %s\n", U64(g_lastBytes.load()), Narrow(g_file).c_str()));
    }
    if (sub == "status" && t.size() <= 2) {
        TelemetryStore::Counts c = g_store.Count();
        return Ok(F("enabled=%d writer=%s dir=%s maxMB=%d\nfile=%s writes=%llu last_size=%llu bytes%s\nevent_types=%llu (calls %llu, types>=1024: %llu) skill_keys=%llu slowmo_layouts=%llu dispatcher_table_a1=0x%llX\n",
                    g_on ? 1 : 0, g_started ? "running" : "stopped", Narrow(g_tdir).c_str(), g_maxMB, g_file.empty() ? "(none yet)" : Narrow(g_file).c_str(), U64(g_writes.load()), U64(g_lastBytes.load()),
                    g_lastNote.empty() ? "" : (" last_error=" + g_lastNote).c_str(), U64(c.eventTypes), U64(c.events), U64(c.overflow), U64(c.skillKeys), U64(c.slowLayouts), U64(g_store.TableA1())));
    }
    return Err("usage: telemetry status | dump | on | off");
}
