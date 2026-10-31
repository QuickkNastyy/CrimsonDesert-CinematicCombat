#include "skillctx.h"
#include "hookretry.h"
#include "launchboost.h"
#include "telemetry.h"
#include "triggers.h"
#include "gamelayout.h"
#include "settings.h"
#include "log.h"
#include "sigscan.h"
#include "trace.h"
#include "MinHook.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <map>
#include <mutex>

// ------------------------------------------------------------------ raw capture
struct SkillEngine::RawSlot {
    std::atomic<uint64_t> stB, stA;   // == seq once the before / after dumps are complete (seqlock)
    uint64_t idx;
    long long t_us;
    DWORD tid;
    uint32_t key0;
    uint64_t base[4];
    uint32_t sz[4], validB[4], validA[4];
    bool hasAfter;
    uint8_t before[4][kRawMax];
    uint8_t after[4][kRawMax];
};

namespace {
// Copies n bytes from addr into dst, page by page; unreadable pages are zero-filled. Returns the number of readable bytes.
// POD/SEH only: no allocation, safe inside the detour.
uint32_t DumpRange(uint64_t addr, uint8_t* dst, uint32_t n) {
    if (!addr || !n) { if (n) memset(dst, 0, n); return 0; }
    uint32_t valid = 0, off = 0;
    while (off < n) {
        uint32_t chunk = 4096 - static_cast<uint32_t>((addr + off) & 4095);
        if (chunk > n - off) chunk = n - off;
        if (PeekMem(static_cast<uintptr_t>(addr + off), dst + off, chunk)) valid += chunk;
        off += chunk;
    }
    return valid;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

const char* const kRawNames[8] = {"a2", "S", "o160", "a1", "a2_after", "S_after", "o160_after", "a1_after"};
}  // namespace

const char* RawName(int which) { return which >= 0 && which < 8 ? kRawNames[which] : "?"; }
bool RawParseName(const std::string& s, int& which) {
    std::string l = Lower(s);
    for (int i = 0; i < 8; ++i) if (l == Lower(kRawNames[i])) { which = i; return true; }
    return false;
}

// Developer file output is confined to the plugin's own project folder: the nearest ancestor of the .asi folder that is called "CombatMod" (the stand-in host
// runs in CombatMod\\build\\harness), else the .asi folder itself. No machine-specific path is compiled in.
std::wstring DevOutputRoot() {
    static const std::wstring root = [] {
        std::wstring d = g_dir;
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        for (std::wstring cur = d; !cur.empty();) {
            size_t sl = cur.find_last_of(L"\\/");
            std::wstring name = sl == std::wstring::npos ? cur : cur.substr(sl + 1);
            if (_wcsicmp(name.c_str(), L"CombatMod") == 0) return cur + L"\\";
            if (sl == std::wstring::npos) break;
            cur = cur.substr(0, sl);
        }
        return d + L"\\";
    }();
    return root;
}

bool RawPathLexicalOk(const std::wstring& full) {
    const std::wstring base = DevOutputRoot();
    if (full.size() <= base.size()) return false;
    if (_wcsnicmp(full.c_str(), base.c_str(), base.size()) != 0) return false;
    if (full.find(L':', 2) != std::wstring::npos) return false;   // alternate data streams, device paths
    if (full.back() == L'\\' || full.back() == L'/') return false;
    return true;
}

SkillEngine::~SkillEngine() = default;

bool SkillEngine::RawPrepare() {
    if (raw_) return true;
    try {
        raw_.reset(new RawSlot[kRawSlots]());
        for (auto& z : rawSz_) z.store(0);
        return true;
    } catch (...) {
        return false;
    }
}

void SkillEngine::RawConfigure(int mode, const uint32_t sizes[4]) {
    for (int i = 0; i < 4; ++i) rawSz_[i].store(sizes[i] > kRawMax ? static_cast<uint32_t>(kRawMax) : sizes[i]);
    rawMode.store(raw_ ? mode : 0);
}

uint64_t SkillEngine::RawBegin(uint64_t a1, uint64_t a2) noexcept {
    if (!raw_) return 0;
    uint64_t seq = rawN_.fetch_add(1) + 1;
    RawSlot& s = raw_[(seq - 1) % kRawSlots];
    s.stB.store(0);
    s.stA.store(0);
    uint64_t S = 0, o160 = 0;
    uint32_t key0 = 0;
    if (a2) {
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyS, &S, 8);
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyO160, &o160, 8);
        if (S) PeekMem(static_cast<uintptr_t>(S) + gl::kSKeys, &key0, 4);
    }
    s.idx = seq; s.t_us = NowUs(); s.tid = GetCurrentThreadId(); s.key0 = key0; s.hasAfter = false;
    s.base[0] = a2; s.base[1] = S; s.base[2] = o160; s.base[3] = a1;
    for (int i = 0; i < 4; ++i) {
        s.sz[i] = rawSz_[i].load();
        s.validB[i] = DumpRange(s.base[i], s.before[i], s.sz[i]);
        s.validA[i] = 0;
    }
    s.stB.store(seq);
    return seq;
}

void SkillEngine::RawEnd(uint64_t seq) noexcept {
    if (!raw_ || !seq) return;
    RawSlot& s = raw_[(seq - 1) % kRawSlots];
    if (s.idx != seq || s.stB.load() != seq) return;   // slot was reused while the original ran
    for (int i = 0; i < 4; ++i) s.validA[i] = DumpRange(s.base[i], s.after[i], s.sz[i]);
    s.hasAfter = true;
    s.stA.store(seq);
}

std::vector<RawHdr> SkillEngine::RawList(size_t n) {
    std::vector<RawHdr> out;
    if (!raw_) return out;
    uint64_t total = rawN_.load();
    uint64_t first = total > kRawSlots ? total - kRawSlots + 1 : 1;
    for (uint64_t q = first; q <= total; ++q) {
        RawSlot& s = raw_[(q - 1) % kRawSlots];
        if (s.stB.load() != q) continue;
        RawHdr h;
        h.idx = q; h.t_us = s.t_us; h.tid = s.tid; h.key0 = s.key0; h.hasAfter = s.stA.load() == q;
        for (int i = 0; i < 4; ++i) { h.base[i] = s.base[i]; h.sz[i] = s.sz[i]; h.validB[i] = s.validB[i]; h.validA[i] = s.validA[i]; }
        if (s.stB.load() != q) continue;
        out.push_back(h);
    }
    if (out.size() > n) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(n));
    return out;
}

bool SkillEngine::RawGet(uint64_t seq, int which, RawHdr& h, std::vector<uint8_t>& data, std::string& err) {
    if (!raw_ || seq == 0) { err = "no raw capture"; return false; }
    RawSlot& s = raw_[(seq - 1) % kRawSlots];
    bool after = which >= 4;
    int i = which & 3;
    if (s.stB.load() != seq) { err = "no such capture (index overwritten or never recorded; the ring keeps the newest 128)"; return false; }
    if (after && s.stA.load() != seq) { err = "that capture has no after-dump (mode was not `both`, or the call has not returned yet)"; return false; }
    h = RawHdr();
    h.idx = seq; h.t_us = s.t_us; h.tid = s.tid; h.key0 = s.key0; h.hasAfter = s.stA.load() == seq;
    for (int k = 0; k < 4; ++k) { h.base[k] = s.base[k]; h.sz[k] = s.sz[k]; h.validB[k] = s.validB[k]; h.validA[k] = s.validA[k]; }
    const uint8_t* src = after ? s.after[i] : s.before[i];
    data.assign(src, src + s.sz[i]);
    if (s.stB.load() != seq || (after && s.stA.load() != seq)) { err = "capture was overwritten while reading"; return false; }
    return true;
}

namespace {
void Put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void Put64(std::vector<uint8_t>& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
}  // namespace

// File format (little endian): header {char[8] "CMRAW001"; u32 version=1; u32 nslots; u32 recHdrSize=112; u32 reserved}
// then per slot (oldest first): {u64 idx; i64 t_us; u32 tid; u32 key0; u64 a2,S,o160,a1; u32 sz[4]; u32 validBefore[4];
// u32 validAfter[4]; u32 hasAfter; u32 pad} (112 bytes), then the 4 before dumps (sz[i] bytes each), then, if hasAfter, the 4 after dumps.
bool SkillEngine::RawSerialize(std::vector<uint8_t>& out) {
    out.clear();
    std::vector<RawHdr> hs = RawList(kRawSlots);
    const char magic[8] = {'C', 'M', 'R', 'A', 'W', '0', '0', '1'};
    out.insert(out.end(), magic, magic + 8);
    Put32(out, 1);
    size_t countPos = out.size();
    Put32(out, 0);
    Put32(out, 112);
    Put32(out, 0);
    uint32_t written = 0;
    for (const RawHdr& h : hs) {
        RawHdr hh;
        std::vector<uint8_t> blocks[8];
        std::string err;
        bool ok = true;
        for (int w = 0; w < 4 && ok; ++w) ok = RawGet(h.idx, w, hh, blocks[w], err);
        if (!ok) continue;
        bool after = false;
        if (h.hasAfter) {
            after = true;
            for (int w = 4; w < 8 && after; ++w) after = RawGet(h.idx, w, hh, blocks[w], err);
        }
        Put64(out, h.idx); Put64(out, static_cast<uint64_t>(h.t_us)); Put32(out, h.tid); Put32(out, h.key0);
        for (int i = 0; i < 4; ++i) Put64(out, h.base[i]);
        for (int i = 0; i < 4; ++i) Put32(out, h.sz[i]);
        for (int i = 0; i < 4; ++i) Put32(out, h.validB[i]);
        for (int i = 0; i < 4; ++i) Put32(out, after ? h.validA[i] : 0);
        Put32(out, after ? 1 : 0);
        Put32(out, 0);
        for (int w = 0; w < 4; ++w) out.insert(out.end(), blocks[w].begin(), blocks[w].end());
        if (after) for (int w = 4; w < 8; ++w) out.insert(out.end(), blocks[w].begin(), blocks[w].end());
        ++written;
    }
    for (int i = 0; i < 4; ++i) out[countPos + static_cast<size_t>(i)] = static_cast<uint8_t>(written >> (8 * i));
    return true;
}

// ------------------------------------------------------------------ engine
SkillEngine::SkillEngine() {
    for (auto& z : rawSz_) z.store(0);
    ring_.reset(new SkillRec[kRing]());
    stamp_.reset(new std::atomic<uint64_t>[kRing]());
    plog_.reset(new PatchLogRec[kPatchLog]());
    pstamp_.reset(new std::atomic<uint64_t>[kPatchLog]());
}

SkillEngine::TSlot* SkillEngine::Find(DWORD tid, bool create) {
    size_t h = (static_cast<size_t>(tid) * 2654435761u) >> 8;
    for (size_t p = 0; p < 8; ++p) {
        TSlot& s = ts_[(h + p) % kTS];
        DWORD cur = s.tid.load();
        if (cur == tid) return &s;
        if (cur == 0 && create) {
            DWORD z = 0;
            if (s.tid.compare_exchange_strong(z, tid) || s.tid.load() == tid) return &s;
        }
    }
    return nullptr;
}

void SkillEngine::Publish(TSlot& s, DWORD tid, long long t, uint64_t a1, const uint32_t* keys) {
    s.seq.fetch_add(1);  // odd = write in progress
    s.tid.store(tid);
    s.t.store(t);
    s.a1.store(a1);
    s.k01.store(static_cast<uint64_t>(keys[0]) | (static_cast<uint64_t>(keys[1]) << 32));
    s.k23.store(static_cast<uint64_t>(keys[2]) | (static_cast<uint64_t>(keys[3]) << 32));
    s.seq.fetch_add(1);
}

bool SkillEngine::ReadSlot(const TSlot& s, SkillCtx& out) {
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t s1 = s.seq.load();
        if (s1 & 1) continue;
        SkillCtx c;
        c.tid = s.tid.load();
        c.t_us = s.t.load();
        c.a1 = s.a1.load();
        uint64_t k01 = s.k01.load(), k23 = s.k23.load();
        c.keys[0] = static_cast<uint32_t>(k01); c.keys[1] = static_cast<uint32_t>(k01 >> 32);
        c.keys[2] = static_cast<uint32_t>(k23); c.keys[3] = static_cast<uint32_t>(k23 >> 32);
        if (s.seq.load() == s1) { out = c; return s1 != 0; }  // seq 0 = never written
    }
    return false;
}

namespace {
thread_local bool t_chainOpen = false;   // set by OnCall for the calling thread: a Blinding Flash chain was open at that call (player calls only)
bool ReactPoke(uint64_t a, uint32_t v) {   // POD + SEH only
    __try { memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool KeyIn(const uint32_t* list, int n, uint32_t key) { for (int i = 0; i < n; ++i) if (list[i] == key) return true; return false; }
}  // namespace
bool SkillEngine::ThreadChainOpen() { return t_chainOpen; }

ReactCfg ReactCfgFromSettings() {
    ReactCfg c;
    auto id = [](int v) -> uint32_t { return v == 1 ? gl::kReactKnockDown : v == 2 ? gl::kReactGroundThrow : v == 3 ? gl::kReactRagdoll : 0u; };
    c.finale = id(static_cast<int>(CfgGet(Reaction_Finale) + 0.5f));
    c.turning = id(static_cast<int>(CfgGet(Reaction_TurningSlash) + 0.5f));
    if (c.finale) c.nFinale = CfgListCopy(BlindingFlash_FinaleKeys, c.finaleKeys, 16);
    if (c.turning) c.nTs = CfgListCopy(Reaction_TurningSlashKeys, c.tsKeys, 16);
    return c;
}

// The reaction override for ONE call: the new key2 is written into the shared record now and put back by RestorePatches() right after the original returns
// (and only if the game did not change the field meanwhile). Player calls only; the record itself is never changed persistently.
int SkillEngine::ApplyReaction(uint64_t a1, uint64_t a2, const ReactCfg& cfg, long long now, PatchUndo* undo, int n, int maxUndo) noexcept {
    if (!a1 || a1 != player.Current() || n >= maxUndo) return n;
    if (!cfg.finale && !cfg.turning) return n;
    uint64_t S = 0;
    uint32_t keys[4] = {0, 0, 0, 0};
    if (!PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyS, &S, 8) || !S || !PeekMem(static_cast<uintptr_t>(S) + gl::kSKeys, keys, 16)) return n;
    uint32_t target = 0;
    bool finaleCall = cfg.finale && t_chainOpen && KeyIn(cfg.finaleKeys, cfg.nFinale, keys[0]);
    if (cfg.turning && KeyIn(cfg.tsKeys, cfg.nTs, keys[0])) tsCastUs.store(now, std::memory_order_relaxed);   // the player started Turning Slash: its hits follow
    if (finaleCall) {
        target = cfg.finale;
        bfChainOpen.store(false, std::memory_order_relaxed);   // the finale ends the chain
    } else if (cfg.turning && (keys[2] == gl::kReactPushLight || keys[2] == gl::kReactPushNormal || keys[2] == gl::kReactKnockDown)) {
        long long cast = tsCastUs.load(std::memory_order_relaxed);
        if (cast > 0 && now >= cast && now - cast <= cfg.tsWindowUs) target = cfg.turning;
    }
    if (!target || keys[2] == target) return n;
    uint64_t addr = static_cast<uint64_t>(S) + gl::kSKey2;
    uint32_t before = keys[2];
    uint32_t cur = 0;
    if (!PeekMem(static_cast<uintptr_t>(addr), &cur, 4) || cur != before) return n;
    if (!ReactPoke(addr, target)) return n;
    undo[n] = PatchUndo{addr, before, target, 1};
    if (finaleCall) reactFinale.fetch_add(1, std::memory_order_relaxed); else reactTurning.fetch_add(1, std::memory_order_relaxed);
    return n + 1;
}

void SkillEngine::OnCall(uint64_t a1, uint64_t a2, uintptr_t ret) noexcept {
    long long now = NowUs();
    DWORD tid = GetCurrentThreadId();
    uint64_t S = 0, o160 = 0, q[2] = {0, 0};
    uint32_t dw[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (a2) {
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyQ58, q, 16);  // kApplyQ58, kApplyQ60 (consecutive)
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyS, &S, 8);
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyO160, &o160, 8);
        if (S) PeekMem(static_cast<uintptr_t>(S) + gl::kSKeys, dw, sizeof dw);
    }
    uint64_t idx = n_.fetch_add(1) + 1;
    size_t slot = static_cast<size_t>((idx - 1) % kRing);
    stamp_[slot].store(0);
    SkillRec& r = ring_[slot];
    r.idx = idx; r.t_us = now; r.tid = tid; r.a1 = a1; r.a2 = a2; r.S = S;
    memcpy(r.dw, dw, 32);
    r.o160 = o160; r.q58 = q[0]; r.q60 = q[1]; r.ret = ret;
    stamp_[slot].store(idx);

    if (TSlot* ts = Find(tid, true)) Publish(*ts, tid, now, a1, dw);
    Publish(any_, tid, now, a1, dw);
    player.Note(a1, now);   // rolling player identity
    t_chainOpen = false;
    if (player.Current() == a1) {
        playerLastUs.store(now, std::memory_order_relaxed);
        if (dw[0] == kBlindingFlashKey) {   // a Blinding Flash chain starts here
            bfCastUs.store(now, std::memory_order_relaxed);
            bfChainOpen.store(true, std::memory_order_relaxed);
            bfChainLastUs.store(now, std::memory_order_relaxed);
        } else if (bfChainOpen.load(std::memory_order_relaxed)) {   // the chain stays open while the player's calls keep coming; a gap closes it
            if (now - bfChainLastUs.load(std::memory_order_relaxed) > bfGapUs.load(std::memory_order_relaxed)) bfChainOpen.store(false, std::memory_order_relaxed);
            else bfChainLastUs.store(now, std::memory_order_relaxed);
        }
        t_chainOpen = bfChainOpen.load(std::memory_order_relaxed);
    }   // for "caused by the player shortly before" (gimmick events)
    if (feedTelemetry.load(std::memory_order_relaxed)) Telemetry().OnSkill(dw, player.Current() == a1);
}

bool SkillEngine::LastCtx(DWORD tid, long long now_us, long long windowUs, SkillCtx& out) {
    TSlot* s = Find(tid, false);
    SkillCtx c;
    if (!s || !ReadSlot(*s, c)) return false;
    if (now_us - c.t_us > windowUs) return false;
    out = c;
    return true;
}

bool SkillEngine::LastAny(long long now_us, long long windowUs, SkillCtx& out) {
    SkillCtx c;
    if (!ReadSlot(any_, c) || now_us - c.t_us > windowUs) return false;
    out = c;
    return true;
}

std::vector<SkillRec> SkillEngine::Since(long long cutoff_us) {
    std::vector<SkillRec> out;
    uint64_t total = n_.load(), base = clearBase_.load();
    uint64_t first = std::max<uint64_t>(base + 1, total > kRing ? total - kRing + 1 : 1);
    for (uint64_t i = first; i <= total; ++i) {
        size_t slot = static_cast<size_t>((i - 1) % kRing);
        if (stamp_[slot].load() != i) continue;
        SkillRec r = ring_[slot];
        if (stamp_[slot].load() != i) continue;  // torn while copying
        if (r.t_us >= cutoff_us) out.push_back(r);
    }
    return out;
}

std::vector<SkillRec> SkillEngine::Tail(size_t n) {
    std::vector<SkillRec> all = Since(0);
    if (all.size() > n) all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(n));
    return all;
}

void SkillEngine::Clear() { clearBase_.store(n_.load()); }

size_t SkillEngine::ThreadsSeen() const {
    size_t c = 0;
    for (const TSlot& s : ts_) if (s.tid.load()) ++c;
    return c;
}

// ------------------------------------------------------------------ patch rules
namespace {
// POD + SEH only (legal __try), no allocation.
bool PokeU32(uint64_t a, uint32_t v) {
    __try { memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float AsF(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
uint32_t AsU(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
const char* const kRegion[4] = {"a2", "S", "o160", "a1"};
const char* const kOp[3] = {"mul", "set", "add"};
}  // namespace

std::string PatchValueStr(bool isFloat, uint32_t bits) {
    return isFloat ? F("%.9g", static_cast<double>(AsF(bits))) : F("%u(0x%X)", bits, bits);
}

std::string PatchRuleStr(const PatchRule& r) {
    std::string keys = r.keyMode == 0 ? "*" : std::string(r.keyMode == 1 ? "k0=" : "any=");
    for (int i = 0; i < r.nkeys; ++i) keys += F(i ? ",%u" : "%u", r.keys[i]);
    std::string owner = r.ownerMode == 0 ? "any" : r.ownerMode == 1 ? "player" : F("0x%llX", U64(r.owner));
    return F("keys=%s %s+0x%X %s %s %s owner=%s", keys.c_str(), kRegion[r.region & 3], r.offset, r.isFloat ? "f32" : "u32", kOp[r.op % 3],
             PatchValueStr(r.isFloat, r.val).c_str(), owner.c_str());
}

// <keys|*> <a2|S|o160|a1> <offset_hex> <f32|u32> <mul|set|add> <value> [owner=<hex a1>|player|any]   (tokens t[from..])
bool ParsePatchRule(const Args& t, size_t from, PatchRule& r, std::string& err) {
    size_t n = t.size() > from ? t.size() - from : 0;
    if (n != 6 && n != 7) { err = "usage: skills patch add <*|k0=a,b|any=a,b> <a2|S|o160|a1> <offset_hex> <f32|u32> <mul|set|add> <value> [owner=<hex a1>|player|any]"; return false; }
    r = PatchRule();
    std::string k = t[from];
    std::string lk = Lower(k);
    if (k == "*") r.keyMode = 0;
    else {
        size_t eq = k.find('=');
        std::string kind = eq == std::string::npos ? "" : Lower(k.substr(0, eq));
        if (kind == "k0") r.keyMode = 1; else if (kind == "any") r.keyMode = 2;
        else { err = "keys must be * or k0=<list> (key0 only) or any=<list> (any of the 4 keys at gl::kSKeys: S+0x108..0x114 on build 26308950); list = comma separated decimals"; return false; }
        std::string list = k.substr(eq + 1);
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
            uint64_t v;
            if (!ParseNum(item, v) || v == 0 || v > 0xFFFFFFFFull) { err = "bad key '" + item + "' (decimal 1..4294967295)"; return false; }
            if (r.nkeys >= 8) { err = "at most 8 keys per rule"; return false; }
            r.keys[r.nkeys++] = static_cast<uint32_t>(v);
        }
    }
    std::string reg = Lower(t[from + 1]);
    if (reg == "a2") r.region = 0; else if (reg == "s") r.region = 1; else if (reg == "o160") r.region = 2; else if (reg == "a1") r.region = 3;
    else { err = "region must be a2, S, o160 or a1"; return false; }
    uint64_t off;
    if (!ParseHex(t[from + 2], off) || off > 4092) { err = "offset must be hex 0..FFC (offset+4 must stay within 4096)"; return false; }
    r.offset = static_cast<uint32_t>(off);
    std::string ty = Lower(t[from + 3]);
    if (ty == "f32") r.isFloat = 1; else if (ty == "u32") r.isFloat = 0; else { err = "type must be f32 or u32"; return false; }
    std::string op = Lower(t[from + 4]);
    if (op == "mul") r.op = 0; else if (op == "set") r.op = 1; else if (op == "add") r.op = 2; else { err = "op must be mul, set or add"; return false; }
    if (r.isFloat) {
        float f;
        if (!ParseFloat(t[from + 5], f) || !std::isfinite(f)) { err = "value must be a finite number"; return false; }
        r.val = AsU(f);
    } else {
        uint64_t v;
        int64_t sv;
        if (!ParseNum(t[from + 5], v)) { err = "value must be an integer (decimal or 0x hex)"; return false; }
        sv = static_cast<int64_t>(v);
        if (v > 0xFFFFFFFFull && !(sv < 0 && sv >= INT32_MIN)) { err = "u32 value does not fit in 32 bits"; return false; }
        r.val = static_cast<uint32_t>(v);
    }
    if (n == 7) {
        std::string o = t[from + 6];
        if (Lower(o).compare(0, 6, "owner=") != 0) { err = "last argument must be owner=<hex a1>|player|any"; return false; }
        std::string ov = Lower(o.substr(6));
        uint64_t a;
        if (ov == "any") r.ownerMode = 0;
        else if (ov == "player") r.ownerMode = 1;
        else if (ParseHex(o.substr(6), a) && a) { r.ownerMode = 2; r.owner = a; }
        else { err = "owner must be a hex a1 value, player or any"; return false; }
    }
    return true;
}

bool SkillEngine::PatchAdd(const PatchRule& in, uint32_t& id, std::string& err) {
    std::lock_guard<std::mutex> l(ruleMu_);
    for (RuleSlot& s : rules_) {
        if (s.r.active) continue;
        id = nextRuleId_.fetch_add(1);
        s.seq.fetch_add(1);          // odd: the detour ignores this rule while it is rewritten
        s.r = in;
        s.r.id = id;
        s.r.active = 1;
        s.hits.store(0);
        s.seq.fetch_add(1);
        ruleCount_.fetch_add(1);
        return true;
    }
    err = "all 32 patch rule slots are in use (`skills patch del <id>` or `clear`)";
    return false;
}

bool SkillEngine::PatchDel(uint32_t id) {
    std::lock_guard<std::mutex> l(ruleMu_);
    for (RuleSlot& s : rules_)
        if (s.r.active && s.r.id == id) {
            s.seq.fetch_add(1);
            s.r.active = 0;
            s.seq.fetch_add(1);
            ruleCount_.fetch_sub(1);
            return true;
        }
    return false;
}

void SkillEngine::PatchClear() {
    std::lock_guard<std::mutex> l(ruleMu_);
    for (RuleSlot& s : rules_)
        if (s.r.active) {
            s.seq.fetch_add(1);
            s.r.active = 0;
            s.seq.fetch_add(1);
        }
    ruleCount_.store(0);
}

std::vector<std::pair<PatchRule, uint64_t>> SkillEngine::PatchList() {
    std::lock_guard<std::mutex> l(ruleMu_);
    std::vector<std::pair<PatchRule, uint64_t>> v;
    for (RuleSlot& s : rules_) if (s.r.active) v.emplace_back(s.r, s.hits.load());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first.id < b.first.id; });
    return v;
}

int SkillEngine::ApplyPatches(uint64_t a1, uint64_t a2, PatchUndo* undo, int maxUndo) noexcept {
    if (!ruleCount_.load(std::memory_order_relaxed) || !plog_) return 0;
    uint64_t S = 0, o160 = 0;
    uint32_t keys[4] = {0, 0, 0, 0};
    if (a2) {
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyS, &S, 8);
        PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyO160, &o160, 8);
        if (S) PeekMem(static_cast<uintptr_t>(S) + gl::kSKeys, keys, 16);
    }
    const uint64_t base[4] = {a2, S, o160, a1};
    uint64_t player = 0;
    bool playerKnown = false;
    int nu = 0;
    for (size_t i = 0; i < kMaxRules && nu < maxUndo; ++i) {
        RuleSlot& s = rules_[i];
        uint32_t q = s.seq.load();
        if ((q & 1) || q == 0) continue;
        PatchRule r = s.r;                       // copy, then verify nobody rewrote it meanwhile
        if (s.seq.load() != q || !r.active) continue;
        if (r.keyMode == 1) {
            bool m = false;
            for (int k = 0; k < r.nkeys; ++k) m = m || r.keys[k] == keys[0];
            if (!m) continue;
        } else if (r.keyMode == 2) {
            bool m = false;
            for (int k = 0; k < r.nkeys; ++k) for (int j = 0; j < 4; ++j) m = m || (keys[j] && r.keys[k] == keys[j]);
            if (!m) continue;
        }
        if (r.ownerMode == 1) {
            if (!playerKnown) { player = PlayerA1(); playerKnown = true; }
            if (!player || a1 != player) continue;
        } else if (r.ownerMode == 2 && a1 != r.owner) continue;
        uint64_t b = base[r.region & 3];
        if (!b || r.offset > 4092) continue;
        uint64_t addr = b + r.offset;
        uint32_t cur;
        if (!PeekMem(static_cast<uintptr_t>(addr), &cur, 4)) continue;
        uint32_t nv;
        if (r.isFloat) {
            float c = AsF(cur), v = AsF(r.val), res;
            res = r.op == 0 ? c * v : r.op == 1 ? v : c + v;
            if (!std::isfinite(res)) continue;     // never write NaN/inf
            nv = AsU(res);
        } else {
            nv = r.op == 0 ? cur * r.val : r.op == 1 ? r.val : cur + r.val;
        }
        if (nv == cur) continue;
        if (!PokeU32(addr, nv)) continue;
        undo[nu].addr = addr; undo[nu].before = cur; undo[nu].patched = nv; undo[nu].restore = (r.region & 3) != 0;   // a2 is a temporary struct
        ++nu;
        s.hits.fetch_add(1);
        uint64_t idx = plogN_.fetch_add(1) + 1;
        size_t slot = static_cast<size_t>((idx - 1) % kPatchLog);
        pstamp_[slot].store(0);
        PatchLogRec& L = plog_[slot];
        L.idx = idx; L.t_us = NowUs(); L.tid = GetCurrentThreadId(); L.key0 = keys[0]; L.ruleId = r.id; L.offset = r.offset;
        L.before = cur; L.after = nv; L.region = r.region; L.isFloat = r.isFloat; L.op = r.op;
        pstamp_[slot].store(idx);
    }
    return nu;
}

void SkillEngine::RestorePatches(const PatchUndo* undo, int n) noexcept {
    for (int i = n - 1; i >= 0; --i) {
        if (!undo[i].restore) continue;
        uint32_t cur;
        if (PeekMem(static_cast<uintptr_t>(undo[i].addr), &cur, 4) && cur == undo[i].patched) PokeU32(undo[i].addr, undo[i].before);
        else notRestored_.fetch_add(1);     // the game changed (or unmapped) the field during the call: leave its value alone
    }
}

std::vector<PatchLogRec> SkillEngine::PatchTail(size_t n) {
    std::vector<PatchLogRec> out;
    if (!plog_) return out;
    uint64_t total = plogN_.load();
    uint64_t first = total > kPatchLog ? total - kPatchLog + 1 : 1;
    for (uint64_t i = first; i <= total; ++i) {
        size_t slot = static_cast<size_t>((i - 1) % kPatchLog);
        if (pstamp_[slot].load() != i) continue;
        PatchLogRec r = plog_[slot];
        if (pstamp_[slot].load() != i) continue;
        out.push_back(r);
    }
    if (out.size() > n) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(n));
    return out;
}

namespace {
// Restores patched persistent fields when the detour scope ends (also on a C++ exception from the original).
struct PatchGuard {
    SkillEngine& e;
    PatchUndo undo[kMaxRules];
    int n = 0;
    explicit PatchGuard(SkillEngine& eng) : e(eng) {}
    void Restore() noexcept { if (n) { e.RestorePatches(undo, n); n = 0; } }
    ~PatchGuard() { Restore(); }
};
}  // namespace

const char* kRegionName(int r) { return kRegion[r & 3]; }

__int64 SkillCallThrough(SkillEngine& e, SkillFn fn, uint64_t* a1, __int64 a2, uintptr_t ret) {
    uint64_t rawSeq = 0;
    PatchGuard guard(e);
    if (e.enabled.load(std::memory_order_relaxed)) {
        e.OnCall(reinterpret_cast<uint64_t>(a1), static_cast<uint64_t>(a2), ret);
        if (e.trig && e.trig->enabled.load(std::memory_order_relaxed)) {   // custom slow-motion triggers: match only (the worker thread does the timing and the controller access)
            SkillCtx c;
            long long tnow = NowUs();
            if (e.LastCtx(GetCurrentThreadId(), tnow, 1000000, c)) e.trig->OnSkill(c.keys, c.a1, e.player.Current(), tnow);
        }
        if (e.rawMode.load(std::memory_order_relaxed)) rawSeq = e.RawBegin(reinterpret_cast<uint64_t>(a1), static_cast<uint64_t>(a2));   // raw dumps see the ORIGINAL values
        {   // launch boost: persistent, derived from remembered originals (see launchboost.h)
            LaunchEngine& le = e.launch ? *e.launch : Launch();
            LaunchCfg cfg = e.launchCfg ? *e.launchCfg : LaunchCfgFromSettings();
            le.OnCall(reinterpret_cast<uint64_t>(a1), static_cast<uint64_t>(a2), e.player.Current(), cfg);
        }
        if (e.patchOn.load(std::memory_order_relaxed) && e.PatchRuleCount() > 0) guard.n = e.ApplyPatches(reinterpret_cast<uint64_t>(a1), static_cast<uint64_t>(a2), guard.undo, static_cast<int>(kMaxRules));
        {   // hit-reaction override: the key2 of THIS call only (restored by the guard right after the original returns, also when it throws)
            ReactCfg rc = e.reactCfg ? *e.reactCfg : ReactCfgFromSettings();
            if (rc.finale || rc.turning) guard.n = e.ApplyReaction(reinterpret_cast<uint64_t>(a1), static_cast<uint64_t>(a2), rc, NowUs(), guard.undo, guard.n, static_cast<int>(kMaxRules));
        }
    }
    __int64 rv = fn(a1, a2);   // nothing is held or locked while the original runs
    guard.Restore();           // S / o160 / a1 go back to their original bytes before anything else can observe them
    if (rawSeq && e.rawMode.load(std::memory_order_relaxed) == 2) e.RawEnd(rawSeq);
    return rv;
}

// ------------------------------------------------------------------ hook management
namespace {
SkillEngine g_skills;
std::mutex g_hm;
SkillFn g_orig = nullptr;
std::atomic<bool> g_skillsWanted{false};
uintptr_t g_target = 0;
bool g_created = false, g_hooked = false;

__int64 __fastcall Detour(uint64_t* a1, __int64 a2) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    return SkillCallThrough(g_skills, g_orig, a1, a2, ret);
}

bool Install(std::string& msg) {
    std::lock_guard<std::mutex> l(g_hm);
    if (g_hooked) return true;
    if (HookTestShouldFail()) { msg = "MinHook create: MH_ERROR_MEMORY_ALLOC (injected by `hooktest`)"; return false; }
    Sig s;
    if (!FindSig("apply_attack_skill_to_position", s)) { msg = "signature apply_attack_skill_to_position not found in CombatMod.sigs"; return false; }
    if (s.st != SigStatus::Resolved || s.matches != 1) {
        msg = F("signature apply_attack_skill_to_position is %s (%zu matches); refusing to hook, a UNIQUE match is required", SigStatusName(s.st), s.matches);
        return false;
    }
    if (!IsExecAddr(s.addr)) { msg = "apply_attack_skill_to_position address is not in executable memory"; return false; }
    if (!EnsureMinHook(msg)) return false;
    if (g_created && g_target != s.addr) { msg = "signature address changed since the hook was created (rescan?); restart the game"; return false; }
    if (!g_created) {
        TelemetryNoteHook("apply_attack_skill_to_position", s.addr);
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(s.addr), reinterpret_cast<LPVOID>(&Detour), reinterpret_cast<LPVOID*>(&g_orig));
        if (st != MH_OK) { msg = std::string("MinHook create: ") + MH_StatusToString(st); return false; }
        g_created = true;
        g_target = s.addr;
    }
    MH_STATUS st = MH_EnableHook(reinterpret_cast<LPVOID>(g_target));
    if (st != MH_OK) { msg = std::string("MinHook enable: ") + MH_StatusToString(st); return false; }
    g_hooked = true;
    Log("skills: hook installed on apply_attack_skill_to_position 0x%llX (%s)", U64(g_target), FmtAddr(g_target).c_str());
    return true;
}
}  // namespace

SkillEngine& Skills() { return g_skills; }

bool SkillsEnsureOn(std::string& msg) {
    g_skillsWanted = true;
    if (!Install(msg)) {
        if (HookClassify(false, msg) == HookResult::Retry) { SkillsRequestOn(); msg += " (retrying in the background, up to 8 times; see `hooks`)"; }
        return false;
    }
    g_skills.enabled = true;
    return true;
}

void SkillsRequestOn() {
    g_skillsWanted = true;
    HookRetryRequest("skills", [](std::string& m) {
        bool ok = Install(m);
        if (ok) g_skills.enabled = g_skillsWanted.load();
        return HookClassify(ok, m);
    });
}

void SkillsOff() { g_skillsWanted = false; g_skills.enabled = false; }

void SkillsInit() {
    std::string v, err;
    if (!IniGet("skills.enabled", v) || strtol(v.c_str(), nullptr, 0) == 0) { Log("skills: Enabled=0 (use `skills on`)"); return; }
    if (!g_cfg.allowTrace) { Log("skills: Enabled=1 ignored because AllowTrace=0"); return; }
    if (!SkillsEnsureOn(err)) { Log("skills: NOT started: %s", err.c_str()); return; }
    Log("skills: capture enabled from ini");
}

void SkillsShutdown() {
    g_skills.enabled = false;
    std::lock_guard<std::mutex> l(g_hm);
    if (g_hooked) {
        MH_STATUS st = MH_DisableHook(reinterpret_cast<LPVOID>(g_target));
        g_hooked = false;
        Log("skills: hook removed (%s)", MH_StatusToString(st));
    }
}

bool SkillsOverlaps(uintptr_t addr, size_t n) {
    std::lock_guard<std::mutex> l(g_hm);
    return g_hooked && addr < g_target + 16 && addr + n > g_target;
}

std::string SkillsSummary() {
    std::lock_guard<std::mutex> l(g_hm);
    if (!g_hooked) return "";
    return F("skills hook 0x%llX (%s) %s calls=%llu\n", U64(g_target), FmtAddr(g_target).c_str(), g_skills.enabled ? "capturing" : "pass-through", U64(g_skills.Total()));
}

// ------------------------------------------------------------------ commands
// Shared by every command that writes a file for the developer (rawsave, ui snapshot): absolute path, under
// the CombatMod project folder (DevOutputRoot) after normalisation AND after resolving junctions/symlinks of the (existing) parent directory.
bool ResolveSavePath(const std::string& in8, std::wstring& fp, std::string& err) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, in8.c_str(), -1, nullptr, 0);
    if (wn <= 1) { err = "empty path"; return false; }
    std::wstring in(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in8.c_str(), -1, &in[0], wn);
    in.resize(static_cast<size_t>(wn) - 1);
    wchar_t full[MAX_PATH * 2];
    if (in.size() < 3 || in[1] != L':' || (in[2] != L'\\' && in[2] != L'/')) { err = "give an absolute path inside the CombatMod project folder"; return false; }
    DWORD fl = GetFullPathNameW(in.c_str(), MAX_PATH * 2, full, nullptr);
    if (!fl || fl >= MAX_PATH * 2) { err = "bad path"; return false; }
    fp.assign(full, fl);
    if (!RawPathLexicalOk(fp)) { err = "path must be a file inside the CombatMod project folder (" + Narrow(DevOutputRoot()) + ")"; return false; }
    size_t slash = fp.find_last_of(L'\\');
    std::wstring dir = fp.substr(0, slash);
    HANDLE dh = CreateFileW(dir.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dh == INVALID_HANDLE_VALUE) { err = "directory does not exist: " + Narrow(dir); return false; }
    wchar_t fin[MAX_PATH * 2];
    DWORD fn = GetFinalPathNameByHandleW(dh, fin, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    CloseHandle(dh);
    std::wstring finalDir(fin, fn && fn < MAX_PATH * 2 ? fn : 0);
    if (finalDir.compare(0, 4, L"\\\\?\\") == 0) finalDir = finalDir.substr(4);
    if (!RawPathLexicalOk(finalDir + L"\\x")) { err = "directory resolves outside the CombatMod project folder (junction/symlink?)"; return false; }
    return true;
}

bool SkillsHooked() {
    std::lock_guard<std::mutex> l(g_hm);
    return g_hooked;
}

std::string CmdSkills(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "on" && t.size() == 2) {
        if (!g_cfg.allowTrace) return Err("disabled");
        std::string msg;
        if (!SkillsEnsureOn(msg)) return Err(msg);
        return Ok("skill capture on (hook installed on apply_attack_skill_to_position)\n");
    }
    if (sub == "off" && t.size() == 2) {
        SkillsOff();
        return Ok("skill capture off (hook stays installed as a pass-through; `quit-bridge` removes it)\n");
    }
    if (sub == "clear" && t.size() == 2) {
        g_skills.Clear();
        return Ok("skill log cleared\n");
    }
    if (sub == "status" && t.size() <= 2) {
        bool hooked;
        uintptr_t tgt;
        { std::lock_guard<std::mutex> l(g_hm); hooked = g_hooked; tgt = g_target; }
        std::string o = F("capturing=%d hook_installed=%d target=%s\ncalls_total=%llu since_clear=%llu threads_with_context=%zu\n", g_skills.enabled ? 1 : 0,
                          hooked ? 1 : 0, tgt ? FmtAddr(tgt).c_str() : "-", U64(g_skills.Total()), U64(g_skills.Since_clear()), g_skills.ThreadsSeen());
        o += F("patching=%s rules=%d not_restored=%llu raw_mode=%d\n", g_skills.patchOn ? "on" : "OFF", g_skills.PatchRuleCount(), U64(g_skills.PatchNotRestored()), g_skills.rawMode.load());
        o += F("player=0x%llX (rolling 60 s window; see `player status`)\n", U64(g_skills.player.Current()));
        return Ok(o);
    }
    if (sub == "log" && t.size() <= 3) {
        uint64_t n = 20;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > 4096)) return Err("usage: skills log [n<=4096]");
        std::vector<SkillRec> recs = g_skills.Tail(static_cast<size_t>(n));
        std::string o = F("calls_total=%llu shown=%zu\n", U64(g_skills.Total()), recs.size());
        for (const SkillRec& r : recs)
            o += F("#%llu t=%lldus tid=%lu a1=0x%llX key=[%u,%u,%u,%u] dw4..7=[0x%X,0x%X,0x%X,0x%X] o160=0x%llX a2=0x%llX S=0x%llX q58=0x%llX q60=0x%llX ret=%s\n",
                   U64(r.idx), r.t_us, r.tid, U64(r.a1), r.dw[0], r.dw[1], r.dw[2], r.dw[3], r.dw[4], r.dw[5], r.dw[6], r.dw[7], U64(r.o160), U64(r.a2),
                   U64(r.S), U64(r.q58), U64(r.q60), FmtAddr(r.ret).c_str());
        return Ok(o);
    }
    if (sub == "summary" && t.size() <= 3) {
        uint64_t secs = 60;
        if (t.size() == 3 && (!ParseNum(t[2], secs) || secs < 1 || secs > 3600)) return Err("usage: skills summary [seconds 1..3600]");
        long long now = NowUs();
        std::vector<SkillRec> recs = g_skills.Since(now - static_cast<long long>(secs) * 1000000LL);
        struct Agg { uint64_t n = 0; long long last = 0; };
        std::map<std::pair<uint64_t, uint32_t>, Agg> m;
        for (const SkillRec& r : recs) { Agg& a = m[{r.a1, r.dw[0]}]; ++a.n; a.last = std::max(a.last, r.t_us); }
        std::vector<std::pair<std::pair<uint64_t, uint32_t>, Agg>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.n > y.second.n; });
        std::string o = F("window_s=%llu calls=%zu distinct(a1,key0)=%zu\n", U64(secs), recs.size(), v.size());
        for (auto& e : v) o += F("a1=0x%llX key0=%u count=%llu last_age_ms=%lld\n", U64(e.first.first), e.first.second, U64(e.second.n), (now - e.second.last) / 1000);
        return Ok(o);
    }
    if (sub == "patch" && t.size() >= 3) {
        std::string ps = Lower(t[2]);
        if (ps == "add") {
            if (!g_cfg.allowWrite || !g_cfg.allowTrace) return Err("disabled");
            PatchRule r;
            std::string e;
            if (!ParsePatchRule(t, 3, r, e)) return Err(e);
            std::string msg;
            if (!SkillsEnsureOn(msg)) return Err(msg);
            uint32_t id;
            if (!g_skills.PatchAdd(r, id, e)) return Err(e);
            Log("skills: patch rule %u added: %s", id, PatchRuleStr(r).c_str());
            return Ok(F("rule id=%u: %s%s\n", id, PatchRuleStr(r).c_str(), g_skills.patchOn ? "" : "\n(patching is OFF: `skills patch on`)"));
        }
        if (ps == "list" && t.size() == 3) {
            std::string o = F("patching=%s rules=%d not_restored=%llu (a2 is never restored; S/o160/a1 are restored after the original returns)\n", g_skills.patchOn ? "on" : "OFF",
                              g_skills.PatchRuleCount(), U64(g_skills.PatchNotRestored()));
            for (auto& pr : g_skills.PatchList()) o += F("#%u %s applied=%llu\n", pr.first.id, PatchRuleStr(pr.first).c_str(), U64(pr.second));
            return Ok(o);
        }
        if (ps == "del" && t.size() == 4) {
            uint64_t id;
            if (!ParseNum(t[3], id)) return Err("usage: skills patch del <id>");
            if (!g_skills.PatchDel(static_cast<uint32_t>(id))) return Err("no such rule");
            Log("skills: patch rule %llu deleted", U64(id));
            return Ok("deleted\n");
        }
        if (ps == "clear" && t.size() == 3) {
            g_skills.PatchClear();
            Log("skills: all patch rules cleared");
            return Ok("all rules removed\n");
        }
        if ((ps == "on" || ps == "off") && t.size() == 3) {
            if (ps == "on" && (!g_cfg.allowWrite || !g_cfg.allowTrace)) return Err("disabled");
            g_skills.patchOn = ps == "on";
            Log("skills: patching %s", ps.c_str());
            return Ok(F("patching %s (%d rules)\n", ps.c_str(), g_skills.PatchRuleCount()));
        }
        if (ps == "log" && t.size() <= 4) {
            uint64_t n = 20;
            if (t.size() == 4 && (!ParseNum(t[3], n) || n < 1 || n > kPatchLog)) return Err("usage: skills patch log [n<=512]");
            std::vector<PatchLogRec> recs = g_skills.PatchTail(static_cast<size_t>(n));
            std::string o = F("shown=%zu\n", recs.size());
            for (const PatchLogRec& r : recs)
                o += F("#%llu t=%lldus tid=%lu key0=%u rule=%u %s+0x%X %s %s -> %s\n", U64(r.idx), r.t_us, r.tid, r.key0, r.ruleId, kRegionName(r.region), r.offset,
                       r.isFloat ? "f32" : "u32", PatchValueStr(r.isFloat, r.before).c_str(), PatchValueStr(r.isFloat, r.after).c_str());
            return Ok(o);
        }
        return Err("usage: skills patch add <*|k0=a,b|any=a,b> <a2|S|o160|a1> <offset_hex> <f32|u32> <mul|set|add> <value> [owner=<hex a1>|player|any] | list | del <id> | clear | on | off | log [n]");
    }
    if (sub == "raw" && t.size() >= 3 && t.size() <= 7) {
        std::string m = Lower(t[2]);
        if (m != "on" && m != "off" && m != "both") return Err("usage: skills raw on|off|both [bytes_a2 bytes_S bytes_o160 bytes_a1]   (defaults 1024 768 512 512, max 4096 each)");
        uint32_t sz[4] = {1024, 768, 512, 512};
        if (t.size() > 3 && t.size() != 7) return Err("give all four sizes (a2 S o160 a1) or none");
        for (size_t i = 3; i < t.size(); ++i) {
            uint64_t v;
            if (!ParseNum(t[i], v) || v > kRawMax) return Err("size must be 0..4096: " + t[i]);
            sz[i - 3] = static_cast<uint32_t>(v);
        }
        if (m == "off") {
            g_skills.rawMode = 0;
            return Ok(F("raw capture off (%llu captures kept; `skills rawlist`)\n", U64(g_skills.RawCount())));
        }
        if (!g_cfg.allowTrace) return Err("disabled");
        std::string msg;
        if (!g_skills.RawPrepare()) return Err("could not allocate the raw ring (~4 MB)");
        if (!SkillsEnsureOn(msg)) return Err(msg);
        g_skills.RawConfigure(m == "both" ? 2 : 1, sz);
        return Ok(F("raw capture %s: dumps a2=%u S=%u o160=%u a1=%u bytes per call, 128 slots (%s)\n", m == "both" ? "BEFORE+AFTER" : "BEFORE only", sz[0], sz[1], sz[2], sz[3],
                    m == "both" ? "after-dumps are taken when the original returns" : "use `both` to also dump after the call"));
    }
    if (sub == "rawlist" && t.size() <= 3) {
        uint64_t n = 20;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > kRawSlots)) return Err("usage: skills rawlist [n<=128]");
        std::vector<RawHdr> hs = g_skills.RawList(static_cast<size_t>(n));
        std::string o = F("raw_mode=%d captures_total=%llu shown=%zu\n", g_skills.rawMode.load(), U64(g_skills.RawCount()), hs.size());
        for (const RawHdr& h : hs)
            o += F("[%llu] t=%lldus tid=%lu a1=0x%llX key0=%u sizes(a2/S/o160/a1)=%u/%u/%u/%u valid_before=%u/%u/%u/%u after=%s\n", U64(h.idx), h.t_us, h.tid,
                   U64(h.base[3]), h.key0, h.sz[0], h.sz[1], h.sz[2], h.sz[3], h.validB[0], h.validB[1], h.validB[2], h.validB[3], h.hasAfter ? "yes" : "no");
        return Ok(o);
    }
    if (sub == "rawget" && (t.size() == 4 || t.size() == 6)) {
        uint64_t idx = 0;
        if (Lower(t[2]) == "last") {
            std::vector<RawHdr> hs = g_skills.RawList(1);
            if (hs.empty()) return Err("no raw captures");
            idx = hs[0].idx;
        } else if (!ParseNum(t[2], idx)) return Err("usage: skills rawget <index|last> a2|S|o160|a1|a2_after|S_after|o160_after|a1_after [offset len]");
        int which;
        if (!RawParseName(t[3], which)) return Err("which must be one of a2, S, o160, a1, a2_after, S_after, o160_after, a1_after");
        RawHdr h;
        std::vector<uint8_t> data;
        std::string e;
        if (!g_skills.RawGet(idx, which, h, data, e)) return Err(e);
        uint64_t off = 0, len = data.size();
        if (t.size() == 6 && (!ParseNum(t[4], off) || !ParseNum(t[5], len))) return Err("bad offset/len");
        if (off > data.size()) off = data.size();
        if (len > data.size() - off) len = data.size() - off;
        uint32_t valid = which >= 4 ? h.validA[which & 3] : h.validB[which & 3];
        std::string o = F("[%llu] %s base=0x%llX size=%zu valid=%u offset=%llu len=%llu t=%lldus tid=%lu\n", U64(h.idx), RawName(which), U64(h.base[which & 3]), data.size(),
                          valid, U64(off), U64(len), h.t_us, h.tid);
        for (uint64_t i = 0; i < len; i += 32) {
            o += F("%04llX:", U64(off + i));
            for (uint64_t j = i; j < i + 32 && j < len; ++j) o += F(" %02X", data[static_cast<size_t>(off + j)]);
            o += "\n";
        }
        return Ok(o);
    }
    if (sub == "rawsave" && t.size() >= 3) {
        std::string joined;
        for (size_t i = 2; i < t.size(); ++i) joined += (i > 2 ? " " : "") + t[i];
        std::wstring fp;
        std::string perr;
        if (!ResolveSavePath(joined, fp, perr)) return Err(perr);
        std::vector<uint8_t> img;
        g_skills.RawSerialize(img);
        HANDLE fh = CreateFileW(fp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) return Err(F("cannot create file (error %lu)", GetLastError()));
        size_t off = 0;
        bool ok = true;
        while (off < img.size() && ok) {
            DWORD w = 0;
            ok = WriteFile(fh, img.data() + off, static_cast<DWORD>(std::min<size_t>(img.size() - off, 1u << 20)), &w, nullptr) && w;
            off += w;
        }
        CloseHandle(fh);
        if (!ok) return Err("write failed");
        Log("skills: raw capture saved (%zu bytes)", img.size());
        return Ok(F("saved %zu bytes (%llu captures) to %s\nformat: see README (CMRAW001); tools\\skillraw.py reads it\n", img.size(), U64(g_skills.RawCount() > kRawSlots ? kRawSlots : g_skills.RawCount()),
                    Narrow(fp).c_str()));
    }
    return Err("usage: skills on | off | status | log [n] | summary [seconds] | clear | raw on|off|both [a2 S o160 a1 bytes] | rawlist [n] | rawget <index|last> <what> [offset len] | rawsave <path> | patch add|list|del|clear|on|off|log ...");
}

// mem scanf <addr> <len> <minabs> <maxabs>: 4-byte-aligned float triples (x,y,z) whose magnitude is within [min,max].
std::string CmdMemScanF(const Args& t) {
    if (t.size() != 6 || Lower(t[1]) != "scanf") return Err("usage: mem scanf <addr> <len> <minabs> <maxabs>");
    uintptr_t a;
    std::string e;
    uint64_t len;
    float lo, hi;
    if (!ResolveAddr(t[2], a, e)) return Err(e);
    if (!ParseNum(t[3], len) || len < 12 || len > (16u << 20)) return Err("len must be 12..16777216");
    if (!ParseFloat(t[4], lo) || !ParseFloat(t[5], hi) || !(lo >= 0.0f) || !(hi >= lo)) return Err("need 0 <= minabs <= maxabs");
    std::string o;
    size_t hits = 0, unreadable = 0;
    const size_t kChunk = 65536;
    std::vector<uint8_t> buf(kChunk + 8);
    uint64_t pos = 0;
    while (pos + 12 <= len) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(len - pos, kChunk + 8));  // 8 bytes of overlap so triples spanning chunks are found
        if (!SafeRead(a + static_cast<uintptr_t>(pos), buf.data(), want)) {
            unreadable += want;
            pos += kChunk;
            continue;
        }
        size_t lastStart = want >= 12 ? want - 12 : 0;
        for (size_t i = 0; i <= lastStart && i < kChunk && pos + i + 12 <= len; i += 4) {
            float v[3];
            memcpy(v, buf.data() + i, 12);
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
            double m = std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]);
            if (m < lo || m > hi) continue;
            if (hits < 200) o += F("+0x%llX (0x%llX) = (%g, %g, %g) |v|=%g\n", U64(pos + i), U64(a + pos + i), static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), m);
            ++hits;
        }
        pos += kChunk;
    }
    o += F("matches=%zu%s unreadable_bytes=%zu scanned=%llu\n", hits, hits > 200 ? " (first 200 shown)" : "", unreadable, U64(len));
    return Ok(o);
}

std::string CmdPlayer(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "reset" && t.size() == 2) {
        g_skills.player.Reset();
        Log("player: identity reset");
        return Ok("player identity reset; it is re-learned from the next apply-skill calls\n");
    }
    if (sub == "status" && t.size() <= 2) {
        long long now = NowUs();
        uint64_t cur = g_skills.player.Current();
        std::string o;
        if (cur) o = F("player a1=0x%llX selected %lld s ago, %llu identity switches so far\n", U64(cur), (now - g_skills.player.SinceUs()) / 1000000, U64(g_skills.player.Switches()));
        else o = F("player unknown (no apply-skill call seen in the last 60 s%s); PlayerOnly features do nothing until it is known\n", g_skills.enabled ? "" : "; skill capture is OFF: `skills on`");
        o += "rule: the a1 with the most apply-skill calls in the sliding 60 s window, re-checked every ~3 s; a challenger needs >= 1.25x and 3+ more calls\n";
        std::vector<PlayerTracker::Entry> c = g_skills.player.Counts(now);
        for (size_t i = 0; i < c.size() && i < 8; ++i) o += F("  a1=0x%llX calls_in_window=%llu%s\n", U64(c[i].a1), U64(c[i].count), c[i].a1 == cur ? "  <- player" : "");
        return Ok(o);
    }
    return Err("usage: player status | reset");
}
