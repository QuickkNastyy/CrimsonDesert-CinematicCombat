#include "launchboost.h"
#include "launchscan.h"
#include "log.h"
#include "settings.h"
#include "skillctx.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace {
const uint32_t kFltMax = gl::kFltMaxBits;
const uintptr_t kOff[4] = {gl::kSStr0, gl::kSStr1, gl::kSAng0, gl::kSAng1};   // strength x2, angles x2 (src/gamelayout.h)
bool AngleSane(float f) { return std::isfinite(f) && f > 0.0f && f < 6.3f; }

// POD + SEH only (legal __try), no allocation.
bool PokeU32(uint64_t a, uint32_t v) {
    __try { memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float AsF(uint32_t b) { return std::bit_cast<float>(b); }
uint32_t AsU(float f) { return std::bit_cast<uint32_t>(f); }
bool Sane(float f) { return std::isfinite(f) && f > 0.5f && f < 500.0f; }
bool LaunchOk(float f) { return std::isfinite(f) && f >= 0.0f && f < 500.0f; }   // a launch value, 0 included (records with no launch of their own)
bool Denied(const LaunchCfg& c, const uint32_t keys[4]) {
    for (int i = 0; i < c.nDeny; ++i) if (KeyEncMatch(c.deny[i], keys)) return true;
    return false;
}
struct Unlock {
    std::atomic_flag& f;
    ~Unlock() { f.clear(std::memory_order_release); }
};
}  // namespace

LaunchCfg LaunchCfgFromSettings() {
    LaunchCfg c;
    c.enabled = CfgBool(Launch_Enabled);
    c.playerOnly = true;   // fixed: the boost never touches launches caused by anything but the player
    c.mult = CfgGet(Launch_Mult);
    c.maxValue = 1000.0f;  // fixed internal safety cap (not a user limit)
    c.angleMul = CfgGet(Launch_AngleMul);
    c.minValue = CfgGet(Launch_MinValue);
    c.nDeny = CfgListCopy(Launch_DenyKeys, c.deny, 16);
    return c;
}

LaunchEngine::LaunchEngine() {
    t_.reset(new Slot[kSlots]());
}

void LaunchEngine::Lock() {
    int spins = 0;
    while (lock_.test_and_set(std::memory_order_acquire)) {
        if (++spins > 64) { SwitchToThread(); spins = 0; }
    }
}

LaunchEngine::Slot* LaunchEngine::FindLocked(uint64_t S, bool create) {
    size_t h = static_cast<size_t>((S >> 4) * 11400714819323198485ull >> 40) & (kSlots - 1);
    for (size_t p = 0; p < kSlots; ++p) {
        Slot& s = t_[(h + p) & (kSlots - 1)];
        if (s.used && s.S == S) {
            if (create && s.dead) { s.dead = false; s.r = LaunchRec(); s.r.S = S; ++st_.records; }
            return (s.dead && !create) ? nullptr : &s;
        }
        if (!s.used) {
            if (!create) return nullptr;
            if (used_ >= kSlots * 3 / 4 || st_.records >= kCap) return nullptr;   // table full
            s.used = true; s.dead = false; s.S = S; s.r = LaunchRec(); s.r.S = S;
            ++used_; ++st_.records;
            return &s;
        }
    }
    return nullptr;
}

void LaunchEngine::OnCall(uint64_t a1, uint64_t a2, uint64_t player, const LaunchCfg& cfg) noexcept {
    if (!a2 || !t_) return;
    uint64_t S = 0;
    if (!PeekMem(static_cast<uintptr_t>(a2) + gl::kApplyS, &S, 8) || !S) return;
    uint32_t blk[gl::kSLaunchSpan / 4];   // S+kSNeg .. S+kSStr1 in one guarded read (unknown fields in between are ignored)
    if (!PeekMem(static_cast<uintptr_t>(S) + gl::kSNeg, blk, sizeof blk)) return;
    auto U = [&](uintptr_t off) { return blk[(off - gl::kSNeg) / 4]; };
    float f138 = AsF(U(gl::kSNeg));
    uint32_t g[2] = {0, 0};   // the angles (optional)
    bool haveAng = PeekMem(static_cast<uintptr_t>(S) + gl::kSAng0, g, 8);
    bool marker = U(gl::kSMark0) == kFltMax && U(gl::kSMark1) == kFltMax && U(gl::kSMark2) == kFltMax;
    if (lock_.test_and_set(std::memory_order_acquire)) return;   // another thread is using the table: never wait inside the game's call
    Unlock ul{lock_};
    Slot* sl = FindLocked(S, false);
    if (!sl && !marker) return;
    long long now = NowUs();
    float cur[4] = {AsF(U(gl::kSStr0)), AsF(U(gl::kSStr1)), AsF(g[0]), AsF(g[1])};
    if (!sl) {
        if (!LaunchOk(cur[0]) || !LaunchOk(cur[1])) return;
        if ((!Sane(cur[0]) || !Sane(cur[1])) && !(f138 >= -1.0f && f138 < 0.0f)) return;   // a record with little / no launch of its own must look like a real one (S+0x138 in [-1, 0))
        sl = FindLocked(S, true);
        if (!sl) { ++st_.mapFull; return; }
        LaunchRec& r = sl->r;
        sl->probeOk = PeekMem(static_cast<uintptr_t>(S), sl->probe, sizeof sl->probe);   // raw S as first seen (before we write anything)
        for (int i = 0; i < 4; ++i) r.orig[i] = r.applied[i] = cur[i];
        r.angleOk = haveAng && AngleSane(cur[2]) && AngleSane(cur[3]);
        r.first_us = now;
    } else {
        LaunchRec& r = sl->r;
        if (!marker) {  // memory no longer looks like a launch record (freed / reused): forget it without writing anything
            sl->dead = true; --st_.records; ++st_.dropped;
            return;
        }
        for (int i = 0; i < 2; ++i) {
            uint32_t c = AsU(cur[i]);
            if (c == AsU(r.applied[i]) || c == AsU(r.orig[i])) continue;   // our value, or the untouched original
            if (!LaunchOk(cur[i])) { sl->dead = true; --st_.records; ++st_.dropped; return; }
            r.orig[i] = r.applied[i] = cur[i];   // the game rewrote the field itself: that is the new original
            ++r.rebased; ++st_.rebased;
        }
        for (int i = 2; i < 4 && r.angleOk; ++i) {
            uint32_t c = AsU(cur[i]);
            if (c == AsU(r.applied[i]) || c == AsU(r.orig[i])) continue;
            if (AngleSane(cur[i])) { r.orig[i] = r.applied[i] = cur[i]; ++r.rebased; ++st_.rebased; }
            else r.angleOk = false;   // not an angle any more: leave the angle fields alone from now on
        }
    }
    LaunchRec& r = sl->r;
    PeekMem(static_cast<uintptr_t>(S) + gl::kSKeys, r.keys, 16);
    r.lastA1 = a1;
    r.lastPlayer = player && a1 == player;
    r.last_us = now;
    ++r.calls; ++st_.calls;
    r.denied = cfg.nDeny > 0 && Denied(cfg, r.keys);   // e.g. the explosive arrow 1091:5 and the bomb gimmick 60002: untouched, restored if we had touched them
    bool boost = cfg.enabled && (!cfg.playerOnly || r.lastPlayer) && !r.denied;
    bool raised = false;
    for (int i = 0; i < 4; ++i) {
        if (i >= 2 && !r.angleOk) break;
        float tgt = r.orig[i];
        if (boost && i < 2) {
            float base = r.orig[i];
            if (cfg.minValue > 0.0f && base < cfg.minValue) { base = cfg.minValue; raised = true; }   // Launch.MinValue: raised BEFORE the multiplier
            float v = base * cfg.mult;
            float cap = std::max(cfg.maxValue, base);   // the cap never pushes a value below its base when Mult >= 1
            if (v > cap) v = cap;
            if (std::isfinite(v) && v > 0.0f) tgt = v;
        } else if (boost && cfg.angleMul != 1.0f) {
            float v = r.orig[i] * cfg.angleMul;
            float cap = std::max(3.14159f, r.orig[i]);
            if (v > cap) v = cap;
            if (std::isfinite(v) && v > 0.01f) tgt = v;
        }
        if (AsU(cur[i]) != AsU(tgt)) {
            if (PokeU32(S + kOff[i], AsU(tgt))) { r.applied[i] = tgt; ++st_.writes; }
        } else {
            r.applied[i] = tgt;
        }
    }
    r.raised = raised;
}

int LaunchEngine::RestoreAll() noexcept {
    Lock();
    Unlock ul{lock_};
    int n = 0;
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& s = t_[i];
        if (!s.used || s.dead) continue;
        for (int k = 0; k < 4; ++k) {
            uint32_t c = 0;
            if (!PeekMem(static_cast<uintptr_t>(s.S + kOff[k]), &c, 4)) continue;
            if (c == AsU(s.r.applied[k]) && AsU(s.r.applied[k]) != AsU(s.r.orig[k]))
                if (PokeU32(s.S + kOff[k], AsU(s.r.orig[k]))) { s.r.applied[k] = s.r.orig[k]; ++n; ++st_.restored; }
        }
    }
    return n;
}

void LaunchEngine::Clear() {
    RestoreAll();
    Lock();
    Unlock ul{lock_};
    for (size_t i = 0; i < kSlots; ++i) t_[i] = Slot();
    used_ = 0;
    st_.records = 0;
}

std::vector<LaunchRec> LaunchEngine::List(size_t n) {
    Lock();
    Unlock ul{lock_};
    std::vector<LaunchRec> v;
    for (size_t i = 0; i < kSlots; ++i) if (t_[i].used && !t_[i].dead) v.push_back(t_[i].r);
    std::sort(v.begin(), v.end(), [](const LaunchRec& a, const LaunchRec& b) { return a.last_us > b.last_us; });
    if (v.size() > n) v.resize(n);
    return v;
}

std::vector<LaunchProbe> LaunchEngine::Probes() {
    Lock();
    Unlock ul{lock_};
    std::vector<LaunchProbe> v;
    for (size_t i = 0; i < kSlots; ++i) {
        const Slot& s = t_[i];
        if (!s.used || s.dead) continue;
        LaunchProbe p;
        p.r = s.r;
        p.ok = s.probeOk;
        memcpy(p.bytes, s.probe, sizeof p.bytes);
        v.push_back(p);
    }
    return v;
}

void LaunchEngine::OwnRange(uintptr_t& lo, uintptr_t& hi) const {
    lo = reinterpret_cast<uintptr_t>(t_.get());
    hi = t_ ? lo + kSlots * sizeof(Slot) : lo;
}

LaunchStats LaunchEngine::Stats() {
    Lock();
    Unlock ul{lock_};
    LaunchStats st = st_;
    st.boosted = 0;
    for (size_t i = 0; i < kSlots; ++i) {
        const Slot& s = t_[i];
        if (!s.used || s.dead) continue;
        if (s.r.denied) ++st.denied;
        if (s.r.raised) ++st.raised;
        for (int k = 0; k < 4; ++k) if (AsU(s.r.applied[k]) != AsU(s.r.orig[k])) { ++st.boosted; break; }
    }
    return st;
}

void LaunchSyncFromSettings() {
    if (CfgBool(Launch_Enabled)) {
        if (!Skills().enabled) SkillsRequestOn();   // installed in the background (retried); the boost starts with the first apply-skill call after that
    } else {
        int n = Launch().RestoreAll();   // switching it off puts every boosted record back
        if (n) Log("launch: disabled, %d fields restored", n);
    }
}

LaunchEngine& Launch() {
    static LaunchEngine e;
    return e;
}

std::string CmdLaunch(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    if (sub == "status" && t.size() <= 2) {
        LaunchCfg c = LaunchCfgFromSettings();
        LaunchStats s = Launch().Stats();
        uint64_t pl = Skills().player.Current();
        return Ok(F("launch.enabled=%d playerOnly=%d mult=%g maxValue=%g angleMul=%g minValue=%g denyKeys=%s | skill_capture=%s player=0x%llX\n"
                    "records=%llu boosted_now=%llu raised_now=%llu denied=%llu calls=%llu writes=%llu rebased=%llu restored=%llu dropped=%llu table_full_skips=%llu\n",
                    c.enabled ? 1 : 0, c.playerOnly ? 1 : 0, static_cast<double>(c.mult), static_cast<double>(c.maxValue), static_cast<double>(c.angleMul), static_cast<double>(c.minValue), CfgValueStr(Launch_DenyKeys).c_str(),
                    Skills().enabled ? "on" : "OFF (records are only discovered while it is on: `skills on`)",
                    U64(pl), U64(s.records), U64(s.boosted), U64(s.raised), U64(s.denied), U64(s.calls), U64(s.writes), U64(s.rebased), U64(s.restored), U64(s.dropped), U64(s.mapFull)));
    }
    if ((sub == "on" || sub == "off") && t.size() == 2) {
        std::string e;
        CfgTakeNote();
        if (!CfgSet(Launch_Enabled, sub == "on" ? 1.0f : 0.0f, e)) return Err(e);
        return Ok(std::string("Launch.Enabled = ") + (sub == "on" ? "1" : "0") + (sub == "off" ? " (all boosted records were restored)" : "") + "\n" + CfgTakeNote());
    }
    if (sub == "scan") return CmdLaunchScan(t);   // byte-wise scan of every skill record in memory (works with skill capture on or off)
    if (sub == "restore" && t.size() == 2) {
        int n = Launch().RestoreAll();
        return Ok(F("restored %d fields to their original values%s\n", n, CfgBool(Launch_Enabled) ? " (Launch.Enabled is still on: the next apply-skill call boosts again; use `launch off`)" : ""));
    }
    if (sub == "clear" && t.size() == 2) {
        Launch().Clear();
        return Ok("restored and forgot all launch records\n");
    }
    if (sub == "log" && t.size() <= 3) {
        uint64_t n = 20;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n < 1 || n > 512)) return Err("usage: launch log [n<=512]");
        std::vector<LaunchRec> v = Launch().List(static_cast<size_t>(n));
        long long now = NowUs();
        std::string o = F("records_shown=%zu (newest call first)\n", v.size());
        for (const LaunchRec& r : v)
            o += F("S=0x%llX a1=0x%llX player=%s keys=[%u,%u,%u,%u] orig=(%g,%g) now=(%g,%g) angles=(%g->%g,%g->%g)%s%s%s calls=%llu rebased=%llu last_age_ms=%lld\n", U64(r.S), U64(r.lastA1), r.lastPlayer ? "yes" : "no",
                   r.keys[0], r.keys[1], r.keys[2], r.keys[3], static_cast<double>(r.orig[0]), static_cast<double>(r.orig[1]), static_cast<double>(r.applied[0]),
                   static_cast<double>(r.applied[1]), static_cast<double>(r.orig[2]), static_cast<double>(r.applied[2]), static_cast<double>(r.orig[3]), static_cast<double>(r.applied[3]), r.angleOk ? "" : "(ignored)", r.denied ? " DENIED(Launch.DenyKeys)" : "", r.raised ? " RAISED(Launch.MinValue)" : "", U64(r.calls), U64(r.rebased), (now - r.last_us) / 1000);
        return Ok(o);
    }
    return Err("usage: launch status | on | off | log [n] | restore | clear | scan [zero|all|more] | scan show [zero|all]");
}
