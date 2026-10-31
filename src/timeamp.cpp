#include "timeamp.h"
#include "gamelayout.h"
#include "log.h"
#include "sigscan.h"
#include "hookretry.h"
#include "settings.h"
#include "telemetry.h"
#include "trace.h"
#include "MinHook.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

namespace {
const size_t kRing = 2000;     // log records
const size_t kMaxMap = 1024;   // distinct D pointers remembered
const uintptr_t kOff[3] = {gl::kEvScale, gl::kEvA, gl::kEvB};  // scale, A, B inside D (src/gamelayout.h)

// POD-only helpers so __try is legal. Event data is game memory: never trust it.
bool Peek(uintptr_t a, void* out, size_t n) {
    __try { memcpy(out, reinterpret_cast<const void*>(a), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool PokeF(uintptr_t a, float v) {
    __try { memcpy(reinterpret_cast<void*>(a), &v, 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Same(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool AnyKey(const std::vector<uint32_t>& list, const uint32_t keys[4]) {
    for (int i = 0; i < 4; ++i)
        if (keys[i] && std::find(list.begin(), list.end(), keys[i]) != list.end()) return true;
    return false;
}

bool ReadFields(uintptr_t D, float out[3]) {  // scale, second float, A, B (kEvScale .. kEvB) in one guarded read
    float raw[4];
    static_assert(sizeof raw == gl::kEvSpan, "kEvSpan");
    if (!Peek(D + gl::kEvScale, raw, sizeof raw)) return false;
    out[0] = raw[0]; out[1] = raw[2]; out[2] = raw[3];
    return true;
}
}  // namespace

const char* AmpReasonName(int r) {
    switch (r) {
        case 0: return "amplified";
        case 1: return "no_ctx";
        case 2: return "deny_key";
        case 3: return "allow_miss";
        case 4: return "owner_mismatch";
        case 5: return "owner_unlearned";
        case 6: return "chain_cap";
        default: return "?";
    }
}

// ------------------------------------------------------------------ engine
AmpEngine::AmpEngine() { ring_.resize(kRing); }

void AmpEngine::SetParams(const AmpParams& p) {
    std::lock_guard<std::mutex> l(mu_);
    bool en = p_.enabled;
    p_ = p;
    p_.enabled = en;
    if (!p_.bfMode && bfActive_) BfEnd("off");
}
AmpParams AmpEngine::Params() { std::lock_guard<std::mutex> l(mu_); return p_; }
AmpStats AmpEngine::Stats() { std::lock_guard<std::mutex> l(mu_); AmpStats s = st_; s.distinct = map_.size(); return s; }

void AmpEngine::SetEnabled(bool on) {
    std::lock_guard<std::mutex> l(mu_);
    p_.enabled = on;
    if (!on) RestoreLocked();
}
void AmpEngine::RestoreAll() { std::lock_guard<std::mutex> l(mu_); RestoreLocked(); }
void AmpEngine::ResetMap() { std::lock_guard<std::mutex> l(mu_); RestoreLocked(); map_.clear(); }

// Put the original values back, but only where the field still holds what WE wrote (otherwise the game changed it; leave it).
void AmpEngine::RestoreLocked() {
    for (auto& kv : map_) {
        Entry& e = kv.second;
        float cur[3];
        if (!ReadFields(kv.first, cur)) continue;  // memory gone: nothing to restore
        for (int i = 0; i < 3; ++i) {
            if (Same(e.applied[i], e.orig[i])) continue;
            if (Same(cur[i], e.applied[i])) {
                if (PokeF(kv.first + kOff[i], e.orig[i])) e.applied[i] = e.orig[i];
                else ++st_.writeFail;
            }
        }
    }
}

std::vector<AmpLogRec> AmpEngine::Tail(size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<AmpLogRec> out;
    uint64_t first = seq_ > kRing ? seq_ - kRing + 1 : 1;
    if (n < seq_ - first + 1) first = seq_ - n + 1;
    for (uint64_t i = first; i <= seq_; ++i) out.push_back(ring_[(i - 1) % kRing]);
    return out;
}

void AmpEngine::OnEvent(uint64_t a3, uintptr_t ret) {
    try {
        std::lock_guard<std::mutex> l(mu_);
        if (!p_.enabled) return;
        ++st_.events;
        uint64_t D = 0;
        float cur[3];
        if (!a3 || !Peek(static_cast<uintptr_t>(a3) + gl::kEvDataInArgs, &D, 8) || !D || !ReadFields(static_cast<uintptr_t>(D), cur)) { ++st_.bad; return; }
        {   // a layout guard: only a block that really is a time-scale event (u16 at D+0 == gl::kTimeEventType) is ever touched
            uint16_t ty = 0;
            bool tyOk = Peek(static_cast<uintptr_t>(D) + gl::kEvType, &ty, 2) && ty == gl::kTimeEventType;
            recentD_[recentN_ % 8] = static_cast<uintptr_t>(D); recentOk_[recentN_ % 8] = tyOk; ++recentN_;   // for `layout check`
            if (!tyOk) {
                if (++st_.badType <= 3) Log("amp: an event block with type %u (expected %u) was left alone - is src/gamelayout.h right for this game build? (`layout check`)", static_cast<unsigned>(ty), gl::kTimeEventType);
                return;
            }
        }

        auto it = map_.find(static_cast<uintptr_t>(D));
        if (it == map_.end()) {
            if (map_.size() >= kMaxMap) { ++st_.mapFull; return; }
            Entry e;
            for (int i = 0; i < 3; ++i) e.orig[i] = e.applied[i] = cur[i];
            it = map_.emplace(static_cast<uintptr_t>(D), e).first;
            if (logNew) Log("amp: new event data D=0x%llX scale=%g A=%g B=%g", U64(D), static_cast<double>(cur[0]), static_cast<double>(cur[1]), static_cast<double>(cur[2]));
        } else {
            Entry& e = it->second;
            for (int i = 0; i < 3; ++i) {
                if (Same(cur[i], e.applied[i]) || Same(cur[i], e.orig[i])) continue;  // ours, or untouched original
                e.orig[i] = cur[i];  // the game rewrote this field: that is the new original
                ++st_.rebased;
            }
        }
        Entry& e = it->second;

        // which skill (if any) is this event attributed to?
        SkillEngine* se = skills_ ? skills_ : &Skills();
        SkillCtx cx;
        long long now = NowUs();
        long long win = static_cast<long long>(p_.contextWindowMs) * 1000;
        bool have = se->LastCtx(GetCurrentThreadId(), now, win, cx);
        if (!have && p_.ctxAnyThread) have = se->LastAny(now, win, cx);
        int reason = 0;
        bool viaGimmick = false;
        if (p_.requireCtx && !have) reason = 1;
        else if (have && AnyKey(p_.denyKeys, cx.keys)) reason = 2;
        else if (!p_.allowKeys.empty() && !(have && AnyKey(p_.allowKeys, cx.keys))) reason = 3;
        else if (p_.ownerMode != 0) {
            uint64_t owner = p_.ownerFixed;
            if (p_.ownerMode == 1 && !se->Owner(owner)) reason = 5;
            else if (!have || cx.a1 != owner) reason = 4;
        }
        if (!reason && p_.playerOnly) {   // SlowMo.PlayerOnly: only the player's own skills (needs a known player)
            uint64_t pl = 0;
            if (!se->Owner(pl)) reason = 5;
            else if (!have || cx.a1 != pl) {
                reason = 4;
                // a gimmick-owned event (bomb / tower) caused by the player: one of the gimmick keys in the context AND a player skill call just before it
                if (have && p_.gimmickOn && AnyKey(p_.gimmickKeys, cx.keys) && se->PlayerRecent(now, static_cast<long long>(p_.gimmickWindowMs) * 1000)) { reason = 0; viaGimmick = true; ++st_.gimmick; }
            }
        }
        if (reason) ++st_.filtered;

        float t[3] = {e.orig[0], e.orig[1], e.orig[2]};
        float o = e.orig[0];
        bool valid = std::isfinite(o) && o > 0.0f;
        bool slowClass = valid && o < p_.ignoreAbove;                       // real slow motion
        bool stopClass = valid && !slowClass && o < 0.999f;                 // the tiny near-1.0 hit-stop blips
        int bfKind = 0;   // Blinding Flash: 1 bf_hit, 2 bf_finale (3 = a per-hit event held back by the chain cap)
        if (p_.bfMode && !reason && (slowClass || stopClass)) {
            bfKind = BfStep(se, have, cx, slowClass, stopClass, o, now);
            if (bfKind == 3) { reason = 6; ++st_.chainCap; ++st_.filtered; bfKind = 0; }
        }
        if (bfKind == 0 && !reason && slowClass && p_.slowOn && p_.maxChainMs > 0) {   // SlowMo.MaxChainSeconds: a chain of hits must not stay in extreme slow motion for ever
            long long gapUs = static_cast<long long>(p_.chainGapMs) * 1000;
            if (chainLastUs_ == 0 || now - chainLastUs_ >= gapUs) chainStartUs_ = now;   // a pause long enough: a new chain
            chainLastUs_ = now;                                                           // capped events keep the chain alive too (it ends only with a real pause)
            if (now - chainStartUs_ >= static_cast<long long>(p_.maxChainMs) * 1000) { reason = 6; ++st_.chainCap; ++st_.filtered; }
        }
        if (bfKind == 1 || bfKind == 2) {   // inside a Blinding Flash chain (modes 1 / 2)
            if (bfKind == 2) {   // the finale: slow motion in both modes
                t[0] = Clamp(o * p_.bfFinaleScale, p_.minScale, 1.0f);
                for (int i = 1; i < 3; ++i) if (std::isfinite(e.orig[i]) && e.orig[i] > 0.0f) t[i] = e.orig[i] * p_.bfFinaleRamp;
                ++st_.bfFinale;
            } else if (p_.bfMode == 1) {   // fast chain: the per-hit scale is ChainSpeed (above 1 ONLY here), timings shortened to a brief speed-up
                t[0] = Clamp(p_.bfChainSpeed, p_.minScale, 2.0f);
                for (int i = 1; i < 3; ++i) if (std::isfinite(e.orig[i]) && e.orig[i] > 0.0f) t[i] = e.orig[i] * 0.5f;
                ++st_.bfHit;
            } else {   // smooth slow motion per hit
                t[0] = Clamp(o * p_.bfHitScale, p_.minScale, 1.0f);
                for (int i = 1; i < 3; ++i) if (std::isfinite(e.orig[i]) && e.orig[i] > 0.0f) t[i] = e.orig[i] * p_.bfHitRamp;
                ++st_.bfHit;
            }
        } else if (!reason && slowClass && p_.slowOn) {
            bool ov = false;
            float best = 0.005f + 1e-6f;
            for (auto& pr : p_.overrides)
                if (std::fabs(o - pr.first) <= best) { best = std::fabs(o - pr.first); t[0] = Clamp(pr.second, 0.01f, 1.0f); ov = true; }
            if (!ov) t[0] = Clamp(o * p_.scaleMul, p_.minScale, 1.0f);
            for (int i = 1; i < 3; ++i)
                if (std::isfinite(e.orig[i]) && e.orig[i] > 0.0f) t[i] = e.orig[i] * (i == 1 ? p_.rampIn : p_.rampOut);
        } else if (!reason && stopClass && p_.hitStopOn) {
            t[0] = Clamp(o * p_.hitStopMul, p_.minScale, 1.0f);            // hit-stop: scale x strength ...
            for (int i = 1; i < 3; ++i)
                if (std::isfinite(e.orig[i]) && e.orig[i] > 0.0f) t[i] = e.orig[i] * p_.hitStopDur;   // ... and timings A and B x length
        }
        bool wasModified = !Same(t[0], e.orig[0]) || !Same(t[1], e.orig[1]) || !Same(t[2], e.orig[2]);
        if (wasModified) ++st_.modified;
        if (stopClass) {
            ++st_.stopSeen;
            if (wasModified) ++st_.stopModified;
            if (feedTelemetry.load(std::memory_order_relaxed)) Telemetry().OnHitStop(NowUs() / 60000000LL, cx.keys[0], have);
        }
        if (feedTelemetry.load(std::memory_order_relaxed)) Telemetry().OnSlowmo(e.orig[0], e.orig[1], e.orig[2], cx.keys, have, wasModified, reason != 0);

        for (int i = 0; i < 3; ++i) {
            if (Same(t[i], cur[i])) { e.applied[i] = cur[i]; continue; }
            if (PokeF(static_cast<uintptr_t>(D) + kOff[i], t[i])) e.applied[i] = t[i];
            else { ++st_.writeFail; e.applied[i] = cur[i]; t[i] = cur[i]; }
        }

        AmpLogRec& r = ring_[seq_ % kRing];
        r.idx = ++seq_;
        r.t_us = NowUs();
        r.tid = GetCurrentThreadId();
        r.D = static_cast<uintptr_t>(D);
        r.ret = ret;
        for (int i = 0; i < 3; ++i) { r.orig[i] = e.orig[i]; r.applied[i] = t[i]; }
        r.haveCtx = have;
        r.reason = reason;
        r.viaGimmick = viaGimmick;
        r.bfKind = static_cast<uint8_t>(bfKind);
        r.ctxA1 = have ? cx.a1 : 0;
        r.ctxAgeUs = have ? std::max(0LL, now - cx.t_us) : 0;
        for (int i = 0; i < 4; ++i) r.ctxKeys[i] = have ? cx.keys[i] : 0;
    } catch (...) {
        // out of memory etc.: never let an exception escape into the game
    }
}

__int64* AmpCallThrough(AmpEngine& e, AmpFn fn, __int64 a1, uint32_t* a2, __int64 a3, __int64 a4, void** a5, uintptr_t ret) {
    e.OnEvent(static_cast<uint64_t>(a3), ret);
    return fn(a1, a2, a3, a4, a5);
}


// ------------------------------------------------------------------ Blinding Flash chains
void AmpEngine::BfEnd(const char* why) {
    if (!bfActive_) return;
    bf_.end = why;
    bf_.noFinale = !bf_.finaleSeen;
    ++st_.bfChains;
    if (bf_.noFinale) ++st_.bfNoFinale;
    Log("amp: Blinding Flash chain #%d (mode %d): %zu events (%d bf_hit, %d bf_finale, %d capped), %.2f s, end=%s%s", bf_.idx, bf_.mode, bf_.evs.size(), bf_.hits, bf_.finales, bf_.capped,
        static_cast<double>(bf_.lastUs - bf_.startUs) / 1e6, why, bf_.noFinale ? " - bf_nofinale (no finale key seen: per-hit rules only)" : "");
    chains_.push_back(bf_);
    if (chains_.size() > 50) chains_.erase(chains_.begin());
    bfActive_ = false;
}

void AmpEngine::BfCloseStale(long long now) {
    if (bfActive_ && now - bf_.lastUs >= static_cast<long long>(p_.bfGapMs) * 1000) BfEnd("timeout");
}

// Called for every player-owned slow-/stop-class event while a Blinding Flash mode is on. Returns 0 (not part of a chain / left alone), 1 (per-hit event),
// 2 (finale event) or 3 (per-hit event held back by the chain cap).
int AmpEngine::BfStep(SkillEngine* se, bool have, const SkillCtx& cx, bool slowClass, bool stopClass, float orig, long long now) {
    (void)stopClass;
    long long gap = static_cast<long long>(p_.bfGapMs) * 1000;
    BfCloseStale(now);
    long long cast = se->BfCastUs();
    if (cast > bfLastCast_ && cast <= now && now - cast <= gap) {   // the player cast Blinding Flash just now: a new chain
        if (bfActive_) BfEnd("newcast");
        bfLastCast_ = cast;
        bf_ = BfChain();
        bf_.idx = ++bfIdx_;
        bf_.mode = p_.bfMode;
        bf_.castUs = bf_.startUs = bf_.lastUs = cast;
        bfActive_ = true;
    }
    if (!bfActive_) return 0;   // no 10276 start seen: the chain cannot be identified, the normal amplifier decides
    bool finaleKey = have && cx.keys[0] && std::find(p_.bfFinaleKeys.begin(), p_.bfFinaleKeys.end(), cx.keys[0]) != p_.bfFinaleKeys.end();
    if (bf_.finaleSeen && !finaleKey) { BfEnd("finale"); return 0; }   // the finale is over: this is a different event
    BfEvent ev;
    if (have) memcpy(ev.keys, cx.keys, sizeof ev.keys);
    ev.tUs = now;
    ev.orig = orig;
    bf_.lastUs = now;
    int kind = 0;
    if (finaleKey) {
        bf_.finaleSeen = true;
        if (slowClass) { kind = 2; ++bf_.finales; ev.kind = 2; } else ev.kind = 3;   // the finale's own hit-stop blip stays under the HitStop settings
    } else if (slowClass) {
        kind = 1;
        if (p_.bfMode == 2 && p_.maxChainMs > 0 && now - bf_.startUs >= static_cast<long long>(p_.maxChainMs) * 1000) { kind = 3; ++bf_.capped; ev.kind = 0; }   // the chain cap (mode 2)
        else { ++bf_.hits; ev.kind = 1; }
    }
    if (bf_.evs.size() < 96) bf_.evs.push_back(ev);
    return kind;
}

std::vector<BfChain> AmpEngine::BfChains(size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    BfCloseStale(NowUs());
    std::vector<BfChain> v = chains_;
    if (bfActive_) v.push_back(bf_);
    if (v.size() > n) v.erase(v.begin(), v.end() - static_cast<std::ptrdiff_t>(n));
    return v;
}

std::vector<std::pair<uintptr_t, bool>> AmpEngine::RecentD(size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<std::pair<uintptr_t, bool>> v;
    for (unsigned i = 0; i < 8 && i < recentN_ && v.size() < n; ++i) {
        unsigned k = (recentN_ - 1 - i) % 8;
        bool dup = false;
        for (auto& e : v) dup = dup || e.first == recentD_[k];
        if (!dup) v.emplace_back(recentD_[k], recentOk_[k]);
    }
    return v;
}

bool AmpEngine::BfActive() { std::lock_guard<std::mutex> l(mu_); BfCloseStale(NowUs()); return bfActive_; }

void AmpEngine::BfClear() { std::lock_guard<std::mutex> l(mu_); chains_.clear(); bfActive_ = false; }

namespace {
struct BfRun { uint32_t keys[4]; uint8_t kind; int count; };
std::vector<BfRun> BfRuns(const BfChain& c) {
    std::vector<BfRun> runs;
    for (const BfEvent& e : c.evs) {
        if (!runs.empty() && !memcmp(runs.back().keys, e.keys, 16) && runs.back().kind == e.kind) ++runs.back().count;
        else { BfRun r; memcpy(r.keys, e.keys, 16); r.kind = e.kind; r.count = 1; runs.push_back(r); }
    }
    return runs;
}
const char* BfKindName(uint8_t k) { return k == 1 ? "hit" : k == 2 ? "finale" : k == 3 ? "finale_key" : "untouched"; }
}  // namespace

std::string AmpEngine::BfLogText(size_t n) {
    std::vector<BfChain> v = BfChains(n);
    bool open = false;
    {
        std::lock_guard<std::mutex> l(mu_);
        open = bfActive_;
    }
    if (v.empty()) return "(no Blinding Flash chains yet: they start with the player's apply-skill call with key 10276 while BlindingFlash.Mode is 1 or 2)\n";
    std::string o;
    for (size_t i = v.size(); i > 0; --i) {   // newest first
        const BfChain& c = v[i - 1];
        bool isOpen = open && i == v.size();
        o += F("#%d mode=%d cast_t=%.3fs events=%zu bf_hit=%d bf_finale=%d capped=%d duration=%.2fs finale_key_seen=%s end=%s%s\n", c.idx, c.mode, static_cast<double>(c.castUs) / 1e6, c.evs.size(), c.hits, c.finales, c.capped,
               static_cast<double>(c.lastUs - c.startUs) / 1e6, c.finaleSeen ? "yes" : "NO (bf_nofinale)", isOpen ? "(still open)" : c.end.c_str(), c.evs.size() >= 96 ? " (first 96 events shown)" : "");
        o += "    ";
        bool first = true;
        for (const BfRun& r : BfRuns(c)) {
            if (!first) o += " > ";
            first = false;
            o += F("%u/%u/%u/%u", r.keys[0], r.keys[1], r.keys[2], r.keys[3]);
            if (r.count > 1) o += F(" x%d", r.count);
            if (r.kind == 2) o += " [FINALE]"; else if (r.kind == 3) o += " [finale key, untouched]"; else if (r.kind == 0) o += " [untouched]";
        }
        o += "\n";
    }
    return o;
}

std::string AmpEngine::BfChainsJson(size_t n) {
    std::vector<BfChain> v = BfChains(n);
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        const BfChain& c = v[i];
        o += i ? ",\n" : "\n";
        o += F("{\"idx\":%d,\"mode\":%d,\"cast_ms\":%lld,\"duration_ms\":%lld,\"events\":%zu,\"bf_hit\":%d,\"bf_finale\":%d,\"capped\":%d,\"finale_key_seen\":%s,\"end\":\"%s\",\"sequence\":[", c.idx, c.mode, c.castUs / 1000, (c.lastUs - c.startUs) / 1000,
               c.evs.size(), c.hits, c.finales, c.capped, c.finaleSeen ? "true" : "false", c.end.empty() ? "open" : c.end.c_str());
        bool first = true;
        for (const BfRun& r : BfRuns(c)) {
            o += first ? "" : ",";
            first = false;
            o += F("{\"keys\":[%u,%u,%u,%u],\"kind\":\"%s\",\"count\":%d}", r.keys[0], r.keys[1], r.keys[2], r.keys[3], BfKindName(r.kind), r.count);
        }
        o += "]}";
    }
    o += "\n]";
    return o;
}

// ------------------------------------------------------------------ hook management
namespace {
AmpEngine g_amp;
std::mutex g_hm;
AmpFn g_orig = nullptr;
uintptr_t g_target = 0;
bool g_created = false, g_hooked = false;

__int64* __fastcall Detour(__int64 a1, uint32_t* a2, __int64 a3, __int64 a4, void** a5) {
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    return AmpCallThrough(g_amp, g_orig, a1, a2, a3, a4, a5, ret);
}

bool Install(std::string& msg) {
    std::lock_guard<std::mutex> l(g_hm);
    if (g_hooked) return true;
    if (HookTestShouldFail()) { msg = "MinHook create: MH_ERROR_MEMORY_ALLOC (injected by `hooktest`)"; return false; }
    Sig s;
    if (!FindSig("time_curve_start", s)) { msg = "signature time_curve_start not found in CombatMod.sigs"; return false; }
    if (s.st != SigStatus::Resolved || s.matches != 1) {
        msg = F("signature time_curve_start is %s (%zu matches); refusing to hook, a UNIQUE match is required", SigStatusName(s.st), s.matches);
        return false;
    }
    if (!IsExecAddr(s.addr)) { msg = "time_curve_start address is not in executable memory"; return false; }
    if (!EnsureMinHook(msg)) return false;
    if (g_created && g_target != s.addr) { msg = "time_curve_start address changed since the hook was created (rescan?); restart the game"; return false; }
    if (!g_created) {
        TelemetryNoteHook("time_curve_start", s.addr);
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(s.addr), reinterpret_cast<LPVOID>(&Detour), reinterpret_cast<LPVOID*>(&g_orig));
        if (st != MH_OK) {
            msg = std::string("MinHook create: ") + MH_StatusToString(st);
            return false;
        }
        g_created = true;
        g_target = s.addr;
    }
    MH_STATUS st = MH_EnableHook(reinterpret_cast<LPVOID>(g_target));
    if (st != MH_OK) { msg = std::string("MinHook enable: ") + MH_StatusToString(st); return false; }
    g_hooked = true;
    Log("amp: hook installed on time_curve_start 0x%llX (%s)", U64(g_target), FmtAddr(g_target).c_str());
    return true;
}

bool ParseOverrides(const std::string& s, std::vector<std::pair<float, float>>& out, std::string& err) {
    out.clear();
    std::string t = Lower(s);
    if (t.empty() || t == "none") return true;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(',', pos);
        std::string item = t.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? t.size() + 1 : comma + 1;
        size_t colon = item.find(':');
        float k, v;
        if (colon == std::string::npos || !ParseFloat(item.substr(0, colon), k) || !ParseFloat(item.substr(colon + 1), v)) { err = "bad ScaleOverrides item '" + item + "' (want orig:new,orig:new)"; return false; }
        if (!(k > 0.0f && k < 1.0f) || !(v >= 0.01f && v <= 1.0f)) { err = "ScaleOverrides: orig must be in (0,1) and new in 0.01..1: '" + item + "'"; return false; }
        out.emplace_back(k, v);
        if (out.size() > 32) { err = "more than 32 overrides"; return false; }
    }
    return true;
}

std::string OverridesStr(const AmpParams& p) {
    if (p.overrides.empty()) return "none";
    std::string o;
    for (auto& pr : p.overrides) o += F(o.empty() ? "%g:%g" : ",%g:%g", static_cast<double>(pr.first), static_cast<double>(pr.second));
    return o;
}

bool ParseKeyList(const std::string& s, std::vector<uint32_t>& out, std::string& err) {
    out.clear();
    std::string t = Lower(s);
    if (t.empty() || t == "none") return true;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(',', pos);
        std::string item = t.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? t.size() + 1 : comma + 1;
        uint64_t v;
        if (!ParseNum(item, v) || v == 0 || v > 0xFFFFFFFFull) { err = "bad SkillKey '" + item + "' (decimal, 1..4294967295)"; return false; }
        out.push_back(static_cast<uint32_t>(v));
        if (out.size() > 64) { err = "more than 64 keys"; return false; }
    }
    return true;
}

std::string KeysStr(const std::vector<uint32_t>& v) {
    if (v.empty()) return "none";
    std::string o;
    for (uint32_t k : v) o += F(o.empty() ? "%u" : ",%u", k);
    return o;
}

std::string OwnerStr(const AmpParams& p) {
    return p.ownerMode == 0 ? "none" : p.ownerMode == 1 ? "auto" : F("0x%llX", U64(p.ownerFixed));
}

std::string OneLine(std::string s) {
    for (char& c : s) if (c == '\n') c = ' ';
    return s;
}

bool NeedsCtx(const AmpParams& p) { return p.playerOnly || p.requireCtx || !p.allowKeys.empty() || !p.denyKeys.empty() || p.ownerMode != 0; }

std::string ParamsStr(const AmpParams& p) {
    return F("BlindingFlash mode=%d chainSpeed=%g hitScale=%g hitRamp=%g finaleScale=%g finaleRamp=%g finaleKeys=%s gap=%dms\nMaxChain=%dms (gap %dms) Gimmicks=%s keys=%s window=%dms\nScaleMul=%g RampInMul=%g RampOutMul=%g MinScale=%g IgnoreAbove=%g PlayerOnly=%d ScaleOverrides=%s HitStop=%s x%g\nContextWindowMs=%d ContextAnyThread=%d RequireCtx=%d AllowKeys=%s DenyKeys=%s OnlyOwner=%s\n",
             p.bfMode, static_cast<double>(p.bfChainSpeed), static_cast<double>(p.bfHitScale), static_cast<double>(p.bfHitRamp), static_cast<double>(p.bfFinaleScale), static_cast<double>(p.bfFinaleRamp), KeysStr(p.bfFinaleKeys).c_str(), p.bfGapMs,
             p.maxChainMs, p.chainGapMs, p.gimmickOn ? "on" : "off", KeysStr(p.gimmickKeys).c_str(), p.gimmickWindowMs,
             static_cast<double>(p.scaleMul), static_cast<double>(p.rampIn), static_cast<double>(p.rampOut), static_cast<double>(p.minScale), static_cast<double>(p.ignoreAbove),
             p.playerOnly ? 1 : 0, OverridesStr(p).c_str(), p.hitStopOn ? "on" : "off", static_cast<double>(p.hitStopMul), p.contextWindowMs, p.ctxAnyThread ? 1 : 0, p.requireCtx ? 1 : 0, KeysStr(p.allowKeys).c_str(), KeysStr(p.denyKeys).c_str(),
             OwnerStr(p).c_str());
}

// Applies "Key=Value" settings to p; all-or-nothing.
bool ApplySettings(AmpParams& p, const Args& kv, size_t from, std::string& err) {
    AmpParams n = p;
    for (size_t i = from; i < kv.size(); ++i) {
        size_t eq = kv[i].find('=');
        if (eq == std::string::npos) { err = "expected Key=Value, got '" + kv[i] + "'"; return false; }
        std::string k = Lower(kv[i].substr(0, eq)), v = kv[i].substr(eq + 1);
        float f;
        uint64_t u;
        if (k == "scaleoverrides") { if (!ParseOverrides(v, n.overrides, err)) return false; continue; }
        if (k == "allowkeys") { if (!ParseKeyList(v, n.allowKeys, err)) return false; continue; }
        if (k == "denykeys") { if (!ParseKeyList(v, n.denyKeys, err)) return false; continue; }
        if (k == "onlyowner") {
            std::string lv = Lower(v);
            if (lv.empty() || lv == "none") { n.ownerMode = 0; n.ownerFixed = 0; }
            else if (lv == "auto") { n.ownerMode = 1; n.ownerFixed = 0; }
            else if (ParseHex(v, u) && u != 0) { n.ownerMode = 2; n.ownerFixed = u; }
            else { err = "OnlyOwner must be auto, none, or a hex a1 value"; return false; }
            continue;
        }
        if (k == "requirectx" || k == "contextanythread") {
            if (!ParseNum(v, u) || u > 1) { err = k + " must be 0 or 1"; return false; }
            (k == "requirectx" ? n.requireCtx : n.ctxAnyThread) = u != 0;
            continue;
        }
        if (k == "contextwindowms") {
            if (!ParseNum(v, u) || u < 50 || u > 5000) { err = "ContextWindowMs must be 50..5000"; return false; }
            n.contextWindowMs = static_cast<int>(u);
            continue;
        }
        if (!ParseFloat(v, f)) { err = "bad number for " + k + ": " + v; return false; }
        if (k == "scalemul") { if (!(f >= 0.05f && f <= 2.0f)) { err = "ScaleMul must be 0.05..2"; return false; } n.scaleMul = f; }
        else if (k == "durmul") { if (!(f >= 0.25f && f <= 4.0f)) { err = "DurMul must be 0.25..4"; return false; } n.rampIn = n.rampOut = f; }
        else if (k == "rampinmul") { if (!(f >= 0.25f && f <= 4.0f)) { err = "RampInMul must be 0.25..4"; return false; } n.rampIn = f; }
        else if (k == "rampoutmul") { if (!(f >= 0.25f && f <= 4.0f)) { err = "RampOutMul must be 0.25..4"; return false; } n.rampOut = f; }

        else if (k == "minscale" || k == "ignoreabove" || k == "playeronly") { err = k + " was removed: it is a fixed internal value now"; return false; }
        else { err = "unknown setting '" + k + "' (ScaleMul, DurMul, RampInMul, RampOutMul, MinScale, IgnoreAbove, PlayerOnly, ScaleOverrides, ContextWindowMs, ContextAnyThread, RequireCtx, AllowKeys, DenyKeys, OnlyOwner)"; return false; }
    }
    p = n;
    return true;
}

// Context filters need the skill hook; start it on demand (the filter then sees real contexts).
std::string CtxNote(const AmpParams& p) {
    if (!NeedsCtx(p) || Skills().enabled) return "";
    if (!g_cfg.allowTrace) return "WARNING: context filters are set but skill capture is off and AllowTrace=0, so every event has ctx=none\n";
    std::string m;
    if (SkillsEnsureOn(m)) return "skill capture started automatically (context filters need it)\n";
    return "WARNING: skill capture could not start (" + m + "); context filters will see ctx=none\n";
}
}  // namespace

// The settings table is the source of truth for the mapped parameters; this pulls them into the engine and
// installs / removes the hook as SlowMo.Enabled demands. Works without the dev Allow* flags (it is the user feature).
void AmpSyncFromSettings() {
    AmpParams p = g_amp.Params();
    p.scaleMul = CfgGet(SlowMo_ScaleMul);
    p.rampIn = CfgGet(SlowMo_RampInMul);
    p.rampOut = CfgGet(SlowMo_RampOutMul);
    p.minScale = 0.02f;      // fixed internal constants (no longer user settings)
    p.ignoreAbove = 0.9f;
    p.playerOnly = true;     // the amplifier NEVER touches slow motion that was not caused by the player
    p.gimmickOn = CfgBool(SlowMo_Gimmicks);   // ... except explosions / towers the player set off (gimmick-owned events right after a player skill)
    p.gimmickKeys = CfgList(SlowMo_GimmickKeys);
    p.gimmickWindowMs = static_cast<int>(CfgGet(SlowMo_GimmickWindowMs));
    p.maxChainMs = static_cast<int>(CfgGet(SlowMo_MaxChainSeconds) * 1000.0f);
    p.bfMode = static_cast<int>(CfgGet(BlindingFlash_Mode) + 0.5f);
    p.bfChainSpeed = std::min(CfgGet(BlindingFlash_ChainSpeed), CfgMaxOf(BlindingFlash_ChainSpeed));   // above 1.0 only behind the [Advanced] unlock
    p.bfHitScale = CfgGet(BlindingFlash_HitScale);
    p.bfHitRamp = CfgGet(BlindingFlash_HitRamp);
    p.bfFinaleScale = CfgGet(BlindingFlash_FinaleScale);
    p.bfFinaleRamp = CfgGet(BlindingFlash_FinaleRamp);
    p.bfFinaleKeys = CfgList(BlindingFlash_FinaleKeys);
    p.denyKeys = CfgList(SlowMo_DenyKeys);
    p.slowOn = CfgBool(SlowMo_Enabled);
    p.hitStopOn = CfgBool(HitStop_Enabled);
    p.hitStopMul = CfgGet(HitStop_ScaleMul);
    p.hitStopDur = CfgGet(HitStop_DurMul);
    g_amp.SetParams(p);
    // The engine runs when a feature needs it or telemetry wants to observe the slow-mo layouts (observe-only: nothing is modified
    // while SlowMo and HitStop are both off).
    bool want = CfgBool(SlowMo_Enabled) || CfgBool(HitStop_Enabled) || p.bfMode != 0 || (TelemetryOn() && g_amp.feedTelemetry.load());
    if (!want) {
        if (g_amp.Params().enabled) Log("amp: disabled by settings, originals restored");
        g_amp.SetEnabled(false);
        return;
    }
    if (!AmpHooked()) {   // installed on the background thread with retries; this function runs again when it is done
        g_amp.SetEnabled(false);
        HookRetryRequest("amp", [](std::string& m) {
            bool ok = Install(m);
            if (ok) AmpSyncFromSettings();
            else CfgNoteSet("WARNING: slow-motion hook not installed yet: " + m + "\n");
            return HookClassify(ok, m);
        });
        return;
    }
    if (!g_amp.Params().enabled) Log("amp: enabled by settings%s", (CfgBool(SlowMo_Enabled) || CfgBool(HitStop_Enabled)) ? "" : " (observe only, for telemetry)");
    g_amp.SetEnabled(true);
    if (NeedsCtx(p) && !Skills().enabled) SkillsRequestOn();   // player / deny-key filters need skill contexts
}

void AmpFeedTelemetry(bool on) { g_amp.feedTelemetry = on; }

// Reads the extra [TimeAmp] keys (context window, filters, overrides) and maps the legacy ones (Enabled, ScaleMul, MinScale, DurMul,
// DenyKeys) onto the settings - but only where the new [Cinematic] key is absent (it wins).
void AmpInit() {
    std::string v, err;
    auto legacy = [&](const char* oldKey, const char* newKeys[], int n) {
        if (!IniGet(std::string("timeamp.") + oldKey, v) || v.empty()) return;   // absent or an empty placeholder: nothing to map
        for (int i = 0; i < n; ++i) {
            std::string probe;
            if (IniGet("cinematic." + Lower(newKeys[i]), probe)) continue;   // the new key is present: it wins
            if (!CfgSetStr(newKeys[i], v, err)) Log("amp: ini: [TimeAmp] %s: %s (ignored)", oldKey, err.c_str());
        }
    };
    const char* kEn[] = {"SlowMo.Enabled"}; legacy("enabled", kEn, 1);
    const char* kSc[] = {"SlowMo.ScaleMul"}; legacy("scalemul", kSc, 1);
    if (IniGet("timeamp.minscale", v)) Log("amp: ignored legacy key [TimeAmp] MinScale (fixed internal value now)");
    const char* kDm[] = {"SlowMo.RampInMul", "SlowMo.RampOutMul"}; legacy("durmul", kDm, 2);
    const char* kDk[] = {"SlowMo.DenyKeys"}; legacy("denykeys", kDk, 1);

    AmpParams p = g_amp.Params();
    static const char* keys[] = {"ScaleOverrides", "ContextWindowMs", "ContextAnyThread", "RequireCtx", "AllowKeys", "OnlyOwner"};
    for (const char* k : keys) {
        if (!IniGet(std::string("timeamp.") + Lower(k), v)) continue;
        Args single{std::string(k) + "=" + v};
        AmpParams q = p;
        if (ApplySettings(q, single, 0, err)) p = q; else Log("amp: ini: %s (key ignored)", err.c_str());
    }
    g_amp.SetParams(p);
    Log("amp: dev params (context window %d ms, any-thread %d, requireCtx %d; the settings-backed values are applied next)", p.contextWindowMs, p.ctxAnyThread ? 1 : 0, p.requireCtx ? 1 : 0);
}

AmpStats AmpStatsNow() { return g_amp.Stats(); }

bool AmpHooked() {
    std::lock_guard<std::mutex> l(g_hm);
    return g_hooked;
}

void AmpShutdown() {
    g_amp.SetEnabled(false);  // restores every remembered D
    std::lock_guard<std::mutex> l(g_hm);
    if (g_hooked) {
        MH_STATUS st = MH_DisableHook(reinterpret_cast<LPVOID>(g_target));
        g_hooked = false;
        Log("amp: hook removed (%s), originals restored", MH_StatusToString(st));
    }
}

bool AmpOverlaps(uintptr_t addr, size_t n) {
    std::lock_guard<std::mutex> l(g_hm);
    return g_hooked && addr < g_target + 16 && addr + n > g_target;
}

std::string AmpSummary() {
    std::lock_guard<std::mutex> l(g_hm);
    if (!g_hooked) return "";
    AmpStats s = g_amp.Stats();
    return F("amp hook 0x%llX (%s) %s events=%llu distinct_D=%llu\n", U64(g_target), FmtAddr(g_target).c_str(), g_amp.Params().enabled ? "enabled" : "pass-through", U64(s.events), U64(s.distinct));
}

// settings-backed keys of `amp set` (everything else is an amp-only dev parameter)
static bool MappedAmpKey(const std::string& k, std::vector<std::string>& cfgKeys) {
    cfgKeys.clear();
    if (k == "scalemul") cfgKeys = {"SlowMo.ScaleMul"};
    else if (k == "durmul") cfgKeys = {"SlowMo.RampInMul", "SlowMo.RampOutMul"};
    else if (k == "rampinmul") cfgKeys = {"SlowMo.RampInMul"};
    else if (k == "rampoutmul") cfgKeys = {"SlowMo.RampOutMul"};
    else if (k == "denykeys") cfgKeys = {"SlowMo.DenyKeys"};
    return !cfgKeys.empty();
}

std::string CmdHitStop(const Args& t) {
    if (t.size() > 2 || (t.size() == 2 && Lower(t[1]) != "status")) return Err("usage: hitstop status");
    AmpParams p = g_amp.Params();
    AmpStats s = g_amp.Stats();
    std::string o = F("HitStop.Enabled=%d strength(scale x)=%g length(A,B x)=%g | hook=%s engine=%s\n", CfgBool(HitStop_Enabled) ? 1 : 0, static_cast<double>(p.hitStopMul), static_cast<double>(p.hitStopDur),
                      AmpHooked() ? "installed" : "not installed", p.enabled ? "running" : "off");
    o += F("hit-stop-class events (original scale 0.9 .. 0.999 = the tiny impact dips): seen=%llu modified=%llu\n", U64(s.stopSeen), U64(s.stopModified));
    o += "native values are about scale 0.99 with timings A=B=0.033 s (66 ms in total); the effective pause is roughly (A+B) x length at scale x strength\n";
    std::vector<TelemetryStore::HsMinute> mins = Telemetry().HitStopMinutes();
    if (mins.empty()) o += "per minute (telemetry): nothing recorded yet (the hook feeds it while telemetry is on)\n";
    long long nowMin = NowUs() / 60000000LL;
    size_t shown = 0;
    for (size_t i = mins.size(); i-- > 0 && shown < 6; ++shown) {
        o += F("  minute %lld (%lld min ago): %llu events, context key0:", mins[i].minute, nowMin - mins[i].minute, U64(mins[i].count));
        for (auto& k : mins[i].keys) o += k.first ? F(" %u x%llu", k.first, U64(k.second)) : F(" none x%llu", U64(k.second));
        o += "\n";
    }
    return Ok(o);
}

std::string CmdAmp(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "status" && t.size() <= 2) {
        AmpParams p = g_amp.Params();
        AmpStats s = g_amp.Stats();
        bool hooked;
        uintptr_t tgt;
        { std::lock_guard<std::mutex> l(g_hm); hooked = g_hooked; tgt = g_target; }
        uint64_t pl = Skills().player.Current();
        std::string o = F("enabled=%d hook_installed=%d target=%s   (driven by the settings: SlowMo.Enabled=%d)\n", p.enabled ? 1 : 0, hooked ? 1 : 0, tgt ? FmtAddr(tgt).c_str() : "-",
                          CfgBool(SlowMo_Enabled) ? 1 : 0);
        o += ParamsStr(p);
        o += F("events_seen=%llu modified=%llu distinct_D=%llu filtered=%llu (chain_cap=%llu gimmick_accepted=%llu wrong_event_type=%llu) blinding_flash(chains=%llu bf_hit=%llu bf_finale=%llu bf_nofinale=%llu) rebased=%llu write_failures=%llu bad_events=%llu map_full_skips=%llu\n", U64(s.events), U64(s.modified), U64(s.distinct),
               U64(s.filtered), U64(s.chainCap), U64(s.gimmick), U64(s.badType), U64(s.bfChains), U64(s.bfHit), U64(s.bfFinale), U64(s.bfNoFinale), U64(s.rebased), U64(s.writeFail), U64(s.bad), U64(s.mapFull));
        o += F("skill_capture=%s player=%s", Skills().enabled ? "on" : "off", pl ? F("0x%llX", U64(pl)).c_str() : "unknown (nothing is amplified while PlayerOnly/OnlyOwner=auto and the player is unknown)");
        if (NeedsCtx(p) && !Skills().enabled) o += "  [context filters set but skill capture is off: events see ctx=none]";
        return Ok(o + "\n");
    }
    if (sub == "on" && t.size() == 2) {
        if (!g_cfg.allowTrace || !g_cfg.allowWrite) return Err("disabled");
        std::string e;
        CfgTakeNote();
        if (!CfgSet(SlowMo_Enabled, 1.0f, e)) return Err(e);
        std::string note = CfgTakeNote();
        return Ok(std::string("amp enabled (SlowMo.Enabled=1); ") + (AmpHooked() ? "hook installed\n" : "the hook on time_curve_start is being installed in the background (retried up to 8 times; see `hooks`)\n") + note);
    }
    if (sub == "off" && t.size() == 2) {
        std::string e;
        CfgSet(SlowMo_Enabled, 0.0f, e);
        g_amp.SetEnabled(false);
        Log("amp: disabled via bridge, originals restored");
        return Ok("amp disabled (SlowMo.Enabled=0); original values restored (the hook stays installed as a pass-through, `quit-bridge` removes it)\n");
    }
    if (sub == "set" && t.size() >= 3) {
        // all-or-nothing: validate the amp-only keys on a copy, apply the settings-backed keys inside a batch and roll back on error
        AmpParams p = g_amp.Params();
        std::string err;
        Args local{"amp", "set"};
        std::vector<std::pair<std::string, std::string>> mapped;
        for (size_t i = 2; i < t.size(); ++i) {
            size_t eq = t[i].find('=');
            std::vector<std::string> ck;
            if (eq != std::string::npos && MappedAmpKey(Lower(t[i].substr(0, eq)), ck))
                for (const std::string& c : ck) mapped.emplace_back(c, t[i].substr(eq + 1));
            else local.push_back(t[i]);
        }
        if (!ApplySettings(p, local, 2, err)) return Err(err);
        CfgTakeNote();
        {
            CfgSnapshot snap = CfgTake();
            CfgBatch batch;
            for (auto& m : mapped)
                if (!CfgSetStr(m.first, m.second, err)) { CfgRestore(snap); return Err(err); }
        }
        // p was computed from the engine params BEFORE the settings changed: keep only its amp-only fields
        AmpParams cur = g_amp.Params();
        cur.overrides = p.overrides; cur.contextWindowMs = p.contextWindowMs; cur.ctxAnyThread = p.ctxAnyThread; cur.requireCtx = p.requireCtx;
        cur.allowKeys = p.allowKeys; cur.ownerMode = p.ownerMode; cur.ownerFixed = p.ownerFixed;
        g_amp.SetParams(cur);
        std::string note = CfgTakeNote();
        if (cur.enabled && NeedsCtx(cur) && !Skills().enabled) {
            std::string m;
            note += SkillsEnsureOn(m) ? "skill capture started automatically (context filters need it)\n" : "WARNING: skill capture could not start (" + m + "); context filters will see ctx=none\n";
        }
        Log("amp: set %s", OneLine(ParamsStr(g_amp.Params())).c_str());
        return Ok(ParamsStr(g_amp.Params()) + "(applies from the next event)\n" + note);
    }
    if (sub == "learn_owner" && t.size() == 2) {
        Skills().player.Reset();
        Log("amp: player identity reset");
        return Ok("the player is now learned continuously (the a1 with the most apply-skill calls in the last 60 s); the identity was reset and is re-learned from the next skill calls. See `player status`.\n");
    }
    if (sub == "log" && t.size() <= 3) {
        uint64_t n = 20;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > kRing)) return Err("usage: amp log [n<=2000]");
        std::vector<AmpLogRec> recs = g_amp.Tail(static_cast<size_t>(n));
        std::string o = F("events_seen=%llu shown=%zu\n", U64(g_amp.Stats().events), recs.size());
        for (const AmpLogRec& r : recs) {
            o += F("#%llu t=%lldus tid=%lu D=0x%llX orig(s=%g A=%g B=%g) applied(s=%g A=%g B=%g) ret=%s ", U64(r.idx), r.t_us, r.tid, U64(r.D),
                   static_cast<double>(r.orig[0]), static_cast<double>(r.orig[1]), static_cast<double>(r.orig[2]), static_cast<double>(r.applied[0]),
                   static_cast<double>(r.applied[1]), static_cast<double>(r.applied[2]), FmtAddr(r.ret).c_str());
            if (r.haveCtx) o += F("ctx=(a1=0x%llX,keys=%u/%u/%u/%u,age_ms=%lld)", U64(r.ctxA1), r.ctxKeys[0], r.ctxKeys[1], r.ctxKeys[2], r.ctxKeys[3], r.ctxAgeUs / 1000);
            else o += "ctx=none";
            if (r.reason) o += F(" filtered=%s", AmpReasonName(r.reason));
            if (r.viaGimmick) o += " via_gimmick(player skill shortly before)";
            if (r.bfKind) o += r.bfKind == 1 ? " bf_hit" : " bf_finale";
            o += "\n";
        }
        return Ok(o);
    }
    if (sub == "reset_map" && t.size() == 2) {
        g_amp.ResetMap();
        Log("amp: map reset (originals restored, event memory forgotten)");
        return Ok("originals restored and event map cleared\n");
    }
    return Err("usage: amp status | on | off | set K=V.. (ScaleMul DurMul RampInMul RampOutMul DenyKeys | ScaleOverrides ContextWindowMs ContextAnyThread RequireCtx AllowKeys OnlyOwner) | learn_owner | log [n] | reset_map");
}

std::string AmpBfChainsJson(size_t n) { return g_amp.BfChainsJson(n); }
std::vector<std::pair<uintptr_t, bool>> AmpRecentD(size_t n) { return g_amp.RecentD(n); }

// `bf status|log [n]|clear`: the Blinding Flash chains seen by the amplifier (verify other weapons from one session; extend BlindingFlash.FinaleKeys live with `cfg set`)
std::string CmdBf(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "status" && t.size() <= 2) {
        AmpParams p = g_amp.Params();
        AmpStats s = g_amp.Stats();
        return Ok(F("BlindingFlash.Mode=%d (0 off, 1 fast chain + slow finale, 2 smooth slow motion per hit + slow finale) ChainSpeed=%g HitScale=%g HitRamp=%g FinaleScale=%g FinaleRamp=%g FinaleKeys=%s\n"
                    "chains=%llu (no finale key seen: %llu) bf_hit=%llu bf_finale=%llu chain_cap(all)=%llu | chain open now: %s | amplifier %s, skill capture %s\n",
                    p.bfMode, static_cast<double>(p.bfChainSpeed), static_cast<double>(p.bfHitScale), static_cast<double>(p.bfHitRamp), static_cast<double>(p.bfFinaleScale), static_cast<double>(p.bfFinaleRamp), KeysStr(p.bfFinaleKeys).c_str(),
                    U64(s.bfChains), U64(s.bfNoFinale), U64(s.bfHit), U64(s.bfFinale), U64(s.chainCap), g_amp.BfActive() ? "yes" : "no", p.enabled ? "enabled" : "off", Skills().enabled ? "on" : "OFF"));
    }
    if (sub == "log" && t.size() <= 3) {
        uint64_t n = 10;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > 50)) return Err("usage: bf log [n 1..50]");
        return Ok(g_amp.BfLogText(static_cast<size_t>(n)));
    }
    if (sub == "clear" && t.size() == 2) { g_amp.BfClear(); return Ok("Blinding Flash chain log cleared\n"); }
    return Err("usage: bf status | log [n] | clear");
}
