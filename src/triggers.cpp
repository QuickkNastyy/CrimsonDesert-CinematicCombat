#include "triggers.h"
#include "features.h"
#include "log.h"
#include "pause.h"
#include "settings.h"
#include "skillctx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
enum St { S_Matches, S_Fired, S_Burst, S_Cooldown, S_Pause, S_Busy, S_Unreadable, S_WriteFail };

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool ParseF(const std::string& v, float lo, float hi, const char* what, float& out, std::string& err) {
    float f;
    if (!ParseFloat(v, f) || !std::isfinite(f)) { err = std::string(what) + " needs a number"; return false; }
    if (f < lo || f > hi) { err = F("%s must be within %g .. %g", what, static_cast<double>(lo), static_cast<double>(hi)); return false; }
    out = f;
    return true;
}

bool ParseI(const std::string& v, int lo, int hi, const char* what, int32_t& out, std::string& err) {
    uint64_t n;
    if (!ParseNum(v, n) || n > 0x7FFFFFFFull) { err = std::string(what) + " needs a whole number"; return false; }
    if (static_cast<int>(n) < lo || static_cast<int>(n) > hi) { err = F("%s must be within %d .. %d", what, lo, hi); return false; }
    out = static_cast<int32_t>(n);
    return true;
}

bool ParseB(const std::string& v, uint8_t& out) {
    std::string l = Lower(v);
    if (l == "1" || l == "true" || l == "on" || l == "yes") { out = 1; return true; }
    if (l == "0" || l == "false" || l == "off" || l == "no") { out = 0; return true; }
    return false;
}
}  // namespace

// ------------------------------------------------------------------ rule text

bool TriggerParse(const std::string& textIn, TriggerRule& r, std::string& err) {
    r = TriggerRule();
    std::string text = Trim(textIn);
    if (text.size() > 7 && Lower(text.substr(0, 7)) == "trigger") {   // "Trigger3 = key=..." accepted for convenience
        size_t eq = text.find('=');
        size_t bar = text.find('|');
        bool isPrefix = eq != std::string::npos && (bar == std::string::npos || eq < bar);
        if (isPrefix) {
            std::string head = Lower(Trim(text.substr(0, eq)));
            bool digits = head.size() > 7;
            for (size_t i = 7; i < head.size(); ++i) digits = digits && isdigit(static_cast<unsigned char>(head[i]));
            if (digits) text = Trim(text.substr(eq + 1));
        }
    }
    bool haveKey = false, haveFirst = false, haveLast = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t bar = text.find('|', pos);
        std::string part = Trim(text.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
        pos = bar == std::string::npos ? text.size() + 1 : bar + 1;
        if (part.empty()) continue;
        size_t eq = part.find('=');
        if (eq == std::string::npos) { err = "'" + part + "' is not key=value"; return false; }
        std::string k = Lower(Trim(part.substr(0, eq))), v = Trim(part.substr(eq + 1));
        if (k == "key" || k == "keys") {
            std::string t = v;
            t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
            size_t p2 = 0;
            r.nkeys = 0;
            while (p2 <= t.size() && !t.empty()) {
                size_t comma = t.find(',', p2);
                std::string item = t.substr(p2, comma == std::string::npos ? std::string::npos : comma - p2);
                p2 = comma == std::string::npos ? t.size() + 1 : comma + 1;
                if (item == "0") continue;   // 0 = empty placeholder slot, never matches
                uint32_t enc;
                if (!ParseKeyEnc(item, enc, err)) return false;   // "key" or "key:slot"
                bool dup = false;
                for (int i = 0; i < r.nkeys; ++i) dup = dup || r.keys[i] == enc;
                if (dup) continue;
                if (r.nkeys >= 8) { err = "at most 8 keys per trigger"; return false; }
                r.keys[r.nkeys++] = enc;
            }
            haveKey = true;
        } else if (k == "owner") {
            if (Lower(v) != "player") { err = "owner must be 'player': triggers only ever react to the player's own skills, never to NPCs"; return false; }
        } else if (k == "scale") { if (!ParseF(v, 0.02f, 1.0f, "scale", r.scale, err)) return false; }
        else if (k == "in") { if (!ParseF(v, 0.0f, 5.0f, "in", r.in, err)) return false; }
        else if (k == "hold") { if (!ParseF(v, 0.0f, 5.0f, "hold", r.hold, err)) return false; }
        else if (k == "out") { if (!ParseF(v, 0.0f, 5.0f, "out", r.out, err)) return false; }
        else if (k == "cooldownms") { if (!ParseI(v, 0, 60000, "cooldownMs", r.cooldownMs, err)) return false; }
        else if (k == "delayms") { if (!ParseI(v, 0, 5000, "delayMs", r.delayMs, err)) return false; }
        else if (k == "idlems") { if (!ParseI(v, 20, 2000, "idleMs", r.idleMs, err)) return false; }
        else if (k == "firstonly") { haveFirst = true; if (!ParseB(v, r.firstOnly)) { err = "firstOnly is 0/1"; return false; } }
        else if (k == "lastonly") { haveLast = true; if (!ParseB(v, r.lastOnly)) { err = "lastOnly is 0/1"; return false; } }
        else if (k == "enabled") { if (!ParseB(v, r.enabled)) { err = "enabled is 0/1"; return false; } }
        else if (k == "name") {
            for (char c : v) if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ' || c == '.')) { err = "name may contain letters, digits, space, _ - ."; return false; }
            if (v.size() > 23) { err = "name is at most 23 characters"; return false; }
            memset(r.name, 0, sizeof r.name);
            memcpy(r.name, v.c_str(), v.size());
        } else {
            err = "unknown field '" + k + "' (key owner scale in hold out cooldownMs delayMs firstOnly lastOnly idleMs enabled name)";
            return false;
        }
    }
    if (!haveKey || r.nkeys == 0) { err = "key= needs at least one non-zero SkillKey (0 is a placeholder and is ignored)"; return false; }
    if (r.lastOnly && haveFirst && r.firstOnly) { err = "firstOnly and lastOnly are mutually exclusive"; return false; }
    if (r.lastOnly) r.firstOnly = 0;
    (void)haveLast;
    float A = r.in + r.hold, B = r.out;
    if (A + B < 0.05f || A + B > 10.0f) { err = F("in + hold + out must be within 0.05 .. 10 seconds (is %g)", static_cast<double>(A + B)); return false; }
    return true;
}

std::string TriggerFormat(const TriggerRule& r) {
    std::string keys;
    for (int i = 0; i < r.nkeys; ++i) keys += (i ? "," : "") + KeyEncStr(r.keys[i]);
    std::string o;
    if (r.name[0]) o += F("name=%s|", r.name);
    if (!r.enabled) o += "enabled=0|";
    o += F("key=%s|owner=player|scale=%g|in=%g|hold=%g|out=%g|cooldownMs=%d|", keys.c_str(), static_cast<double>(r.scale), static_cast<double>(r.in), static_cast<double>(r.hold),
           static_cast<double>(r.out), r.cooldownMs);
    o += r.lastOnly ? F("lastOnly=1|idleMs=%d|", r.idleMs) : F("firstOnly=%d|idleMs=%d|", r.firstOnly ? 1 : 0, r.idleMs);
    o += F("delayMs=%d", r.delayMs);
    return o;
}

bool TrigCtlIdle(const float f[2][5]) { return f[0][0] < 0.0f && f[1][0] < 0.0f; }

// ------------------------------------------------------------------ engine
TriggerEngine::TriggerEngine() {
    for (auto& a : st_) a.store(0);
    sched_.reserve(64);
    log_.reserve(kTrigLog);
}

bool TriggerEngine::ReadRule(const Slot& s, TriggerRule& out) {
    uint32_t s1 = s.seq.load(std::memory_order_acquire);
    if ((s1 & 1) || !s.used.load(std::memory_order_acquire)) return false;
    memcpy(&out, &s.r, sizeof out);
    std::atomic_thread_fence(std::memory_order_acquire);
    return s.seq.load(std::memory_order_relaxed) == s1;
}

int TriggerEngine::Add(const TriggerRule& r, std::string& err) {
    std::lock_guard<std::mutex> l(ruleMu_);
    for (int i = 0; i < kTrigMax; ++i) {
        Slot& s = s_[i];
        if (s.used.load()) continue;
        s.seq.fetch_add(1);   // odd: the detour skips this slot meanwhile
        s.r = r;
        s.lastMatch.store(kNever); s.lastAccept.store(kNever); s.lastPending.store(false);
        s.matches.store(0); s.fired.store(0);
        s.used.store(1);
        s.seq.fetch_add(1);
        return i + 1;
    }
    err = F("at most %d triggers", kTrigMax);
    return 0;
}

bool TriggerEngine::Del(int n) {
    if (n < 1 || n > kTrigMax) return false;
    std::lock_guard<std::mutex> l(ruleMu_);
    Slot& s = s_[n - 1];
    if (!s.used.load()) return false;
    s.seq.fetch_add(1);
    s.used.store(0);
    s.lastPending.store(false);
    s.seq.fetch_add(1);
    return true;
}

void TriggerEngine::Clear() { for (int i = 1; i <= kTrigMax; ++i) Del(i); }

bool TriggerEngine::SetRuleEnabled(int n, bool on) {
    if (n < 1 || n > kTrigMax) return false;
    std::lock_guard<std::mutex> l(ruleMu_);
    Slot& s = s_[n - 1];
    if (!s.used.load()) return false;
    s.seq.fetch_add(1);
    s.r.enabled = on ? 1 : 0;
    s.seq.fetch_add(1);
    return true;
}

std::vector<std::pair<int, TriggerRule>> TriggerEngine::List() const {
    std::vector<std::pair<int, TriggerRule>> v;
    for (int i = 0; i < kTrigMax + 1; ++i) {
        TriggerRule r;
        for (int tries = 0; tries < 8; ++tries) if (ReadRule(s_[i], r)) { v.emplace_back(i + 1, r); break; }
    }
    return v;
}

int TriggerEngine::RuleCount() const { return static_cast<int>(List().size()); }

int TriggerEngine::ActiveCount() const {
    int n = 0;
    for (auto& p : List()) if (p.second.enabled && p.second.nkeys && (p.second.builtin ? towerOn.load() : userOn.load())) ++n;
    return n;
}

uint64_t TriggerEngine::RuleMatches(int n) const { return n >= 1 && n <= kTrigBuiltin ? s_[n - 1].matches.load() : 0; }
uint64_t TriggerEngine::RuleFired(int n) const { return n >= 1 && n <= kTrigBuiltin ? s_[n - 1].fired.load() : 0; }

bool TriggerEngine::SetBuiltin(const TriggerRule& r) {
    std::lock_guard<std::mutex> l(ruleMu_);
    Slot& s = s_[kTrigMax];
    TriggerRule want = r;
    want.builtin = 1;
    if (s.used.load() && memcmp(&s.r, &want, sizeof want) == 0) return false;   // unchanged: keep the cooldown / burst state
    s.seq.fetch_add(1);
    s.r = want;
    s.lastMatch.store(kNever); s.lastAccept.store(kNever); s.lastPending.store(false);
    s.used.store(1);
    s.seq.fetch_add(1);
    return true;
}

void TriggerEngine::Wake() noexcept { if (wake) SetEvent(wake); }

void TriggerEngine::Push(int slot, long long due, long long call, uint32_t key, uint64_t a1) noexcept {
    uint64_t i = qhead_.fetch_add(1);
    Req& q = q_[i % kTrigQueue];
    q.ready.store(0, std::memory_order_release);
    q.rule = slot; q.due = due; q.call = call; q.key = key; q.a1 = a1;
    q.ready.store(i + 1, std::memory_order_release);
    Wake();
}

void TriggerEngine::OnSkill(const uint32_t keys[4], uint64_t a1, uint64_t player, long long now) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (!a1 || !player || a1 != player) return;   // the player's own skills only; while the player is unknown nothing happens
    for (int i = 0; i < kTrigMax + 1; ++i) {
        Slot& s = s_[i];
        TriggerRule r;
        if (!ReadRule(s, r) || !r.enabled || !r.nkeys) continue;
        if (i == kTrigMax ? !towerOn.load(std::memory_order_relaxed) : !userOn.load(std::memory_order_relaxed)) continue;
        uint32_t hit = 0;
        for (int j = 0; j < r.nkeys && !hit; ++j) if (KeyEncMatch(r.keys[j], keys)) hit = r.keys[j] & 0xFFFFFFu;   // the key that matched (slot form: its primary key)
        if (!hit) continue;
        s.matches.fetch_add(1, std::memory_order_relaxed);
        st_[S_Matches].fetch_add(1, std::memory_order_relaxed);
        long long prev = s.lastMatch.exchange(now);
        bool newBurst = prev == kNever || now - prev > static_cast<long long>(r.idleMs) * 1000;
        if (r.lastOnly) {   // the worker fires it when idleMs pass without another matching call
            s.lastKey.store(hit); s.lastA1.store(a1); s.lastPending.store(true);
            Wake();
            continue;
        }
        if (r.firstOnly && !newBurst) { st_[S_Burst].fetch_add(1, std::memory_order_relaxed); continue; }
        long long la = s.lastAccept.load();
        if (la != kNever && now - la < static_cast<long long>(r.cooldownMs) * 1000) { st_[S_Cooldown].fetch_add(1, std::memory_order_relaxed); continue; }
        s.lastAccept.store(now);
        Push(i + 1, now + static_cast<long long>(r.delayMs) * 1000, now, hit, a1);
    }
}

void TriggerEngine::Record(const TrigLogRec& r) {
    std::lock_guard<std::mutex> l(logMu_);
    if (log_.size() >= static_cast<size_t>(kTrigLog)) log_.erase(log_.begin());
    log_.push_back(r);
}

// The controller shows an active object. If every active object still holds EXACTLY the curve we wrote last and that curve ran out long ago
// (A + B + 300 ms), it is a stale leftover of ours (an engine that resets only one of the two objects must not block the triggers forever).
bool TriggerEngine::OwnCurveExpired(const float f[2][5], long long now) {
    std::lock_guard<std::mutex> l(fireMu_);
    if (lastAt_ == kNever) return false;
    long long endUs = lastAt_ + static_cast<long long>((lastA_ + lastB_) * 1e6f) + 300000;
    if (now < endUs) return false;
    for (int i = 0; i < 2; ++i) {
        if (f[i][0] < 0.0f) continue;   // idle object
        if (f[i][0] != lastScale_ || f[i][2] != lastA_ || f[i][3] != lastB_) return false;
    }
    return true;
}

bool TriggerEngine::Attempt(int slotNumber, const char* name, uint32_t key, uint64_t a1, float scale, float A, float B, long long callUs, long long nowUs, TrigCtl& ctl, bool paused, bool force,
                            std::string* outcome) {
    TrigLogRec rec;
    rec.t_us = nowUs; rec.rule = slotNumber; rec.key = key; rec.a1 = a1; rec.scale = scale; rec.A = A; rec.B = B; rec.ageUs = nowUs - callUs;
    if (name) strncpy_s(rec.name, sizeof rec.name, name, _TRUNCATE);
    const char* why = nullptr;
    float f[2][5];
    bool fire = false;
    if (paused) { why = "skipped: pause is active"; st_[S_Pause].fetch_add(1); }
    else if (!ctl.Read(f)) { why = "skipped: controller unreadable"; st_[S_Unreadable].fetch_add(1); }
    else if (!force && !allowOverride.load() && !TrigCtlIdle(f) && !OwnCurveExpired(f, nowUs)) { why = "skipped: another time effect is active (controller not idle)"; st_[S_Busy].fetch_add(1); }
    else {
        const float v[5] = {scale, 1.0f, A, B, 0.0f};
        if (ctl.Write(v)) {
            fire = true;
            std::lock_guard<std::mutex> l(fireMu_);
            lastScale_ = scale; lastA_ = A; lastB_ = B; lastAt_ = nowUs;
        }
        else { why = "WRITE FAILED"; st_[S_WriteFail].fetch_add(1); }
    }
    if (fire) {
        why = "FIRED";
        st_[S_Fired].fetch_add(1);
        if (slotNumber >= 1 && slotNumber <= kTrigBuiltin) s_[slotNumber - 1].fired.fetch_add(1);
    }
    strncpy_s(rec.outcome, sizeof rec.outcome, why, _TRUNCATE);
    rec.fired = fire;
    Record(rec);
    Log("triggers: %s #%d %s key=%u a1=0x%llX scale=%g A=%g B=%g age=%.1f ms", why, slotNumber, rec.name, key, U64(a1), static_cast<double>(scale), static_cast<double>(A), static_cast<double>(B),
        static_cast<double>(rec.ageUs) / 1000.0);
    if (outcome) *outcome = why;
    return fire;
}

int TriggerEngine::Pump(long long now, TrigCtl& ctl, bool paused) {
    // 1. drain the detour's queue
    uint64_t head = qhead_.load(std::memory_order_acquire);
    if (head - qtail_ > kTrigQueue) qtail_ = head - kTrigQueue;   // overrun: the oldest requests are gone
    while (qtail_ < head) {
        Req& q = q_[qtail_ % kTrigQueue];
        if (q.ready.load(std::memory_order_acquire) != qtail_ + 1) break;   // still being written
        sched_.push_back(Sched{q.rule, q.due, q.call, q.key, q.a1});
        ++qtail_;
    }
    // 2. "last hit of a chain" rules: idleMs without another matching call
    for (int i = 0; i < kTrigMax + 1; ++i) {
        Slot& s = s_[i];
        if (!s.lastPending.load()) continue;
        TriggerRule r;
        if (!ReadRule(s, r)) { s.lastPending.store(false); continue; }
        long long lm = s.lastMatch.load();
        if (now - lm < static_cast<long long>(r.idleMs) * 1000) continue;
        s.lastPending.store(false);
        if (s.lastMatch.load() != lm) { s.lastPending.store(true); continue; }   // a newer call arrived meanwhile
        long long la = s.lastAccept.load();
        if (la != kNever && lm - la < static_cast<long long>(r.cooldownMs) * 1000) { st_[S_Cooldown].fetch_add(1); continue; }
        s.lastAccept.store(lm);
        sched_.push_back(Sched{i + 1, now + static_cast<long long>(r.delayMs) * 1000, lm, s.lastKey.load(), s.lastA1.load()});
    }
    // 3. everything that is due
    int fired = 0;
    for (size_t k = 0; k < sched_.size();) {
        if (sched_[k].due > now) { ++k; continue; }
        Sched sc = sched_[k];
        sched_.erase(sched_.begin() + static_cast<std::ptrdiff_t>(k));
        TriggerRule r;
        if (!ReadRule(s_[sc.rule - 1], r)) continue;   // deleted meanwhile
        if (!enabled.load()) continue;
        if (Attempt(sc.rule, r.name, sc.key, sc.a1, r.scale, r.in + r.hold, r.out, sc.call, now, ctl, paused, false, nullptr)) ++fired;
    }
    return fired;
}

long long TriggerEngine::NextDueUs(long long now) {
    long long best = -1;
    auto take = [&](long long due) { long long d = due - now; if (d < 0) d = 0; if (best < 0 || d < best) best = d; };
    if (qhead_.load() != qtail_) take(now);
    for (const Sched& s : sched_) take(s.due);
    for (int i = 0; i < kTrigMax + 1; ++i) {
        Slot& s = s_[i];
        if (!s.lastPending.load()) continue;
        TriggerRule r;
        if (!ReadRule(s, r)) continue;
        take(s.lastMatch.load() + static_cast<long long>(r.idleMs) * 1000);
    }
    return best;
}

bool TriggerEngine::FireNow(float scale, float A, float B, TrigCtl& ctl, bool paused, bool force, long long now, std::string& outcome) {
    if (!(scale >= 0.02f && scale <= 1.0f)) { outcome = "scale must be within 0.02 .. 1"; return false; }
    if (!(A >= 0.0f && B >= 0.0f && A + B >= 0.05f && A + B <= 10.0f)) { outcome = "A and B must be >= 0 with 0.05 <= A + B <= 10 seconds"; return false; }
    return Attempt(0, "manual", 0, 0, scale, A, B, now, now, ctl, paused, force, &outcome);
}

TrigStats TriggerEngine::Stats() const {
    TrigStats s;
    s.matches = st_[S_Matches].load(); s.fired = st_[S_Fired].load(); s.burstSuppressed = st_[S_Burst].load(); s.cooldownSuppressed = st_[S_Cooldown].load();
    s.skippedPause = st_[S_Pause].load(); s.skippedBusy = st_[S_Busy].load(); s.skippedUnreadable = st_[S_Unreadable].load(); s.writeFailed = st_[S_WriteFail].load();
    return s;
}

std::vector<TrigLogRec> TriggerEngine::LogTail(size_t n) const {
    std::lock_guard<std::mutex> l(logMu_);
    std::vector<TrigLogRec> v;
    for (size_t i = log_.size(); i > 0 && v.size() < n; --i) v.push_back(log_[i - 1]);
    return v;
}

void TriggerEngine::ClearLog() { std::lock_guard<std::mutex> l(logMu_); log_.clear(); }

// ------------------------------------------------------------------ the process-wide engine + worker thread
namespace {
TriggerEngine g_trig;
HANDLE g_thread = nullptr;
std::atomic<bool> g_quit{false}, g_started{false};

class RealCtl : public TrigCtl {
public:
    bool Read(float f[2][5]) override { return PauseCtlRead(f); }
    bool Write(const float v[5]) override { return PauseCtlWrite(v); }
};

DWORD WINAPI Worker(LPVOID) {
    RegisterInternalThread();
    RealCtl ctl;
    while (!g_quit) {
        long long now = NowUs();
        long long due = g_trig.NextDueUs(now);
        DWORD ms = due < 0 ? 250 : static_cast<DWORD>(std::min<long long>(250, due / 1000));
        WaitForSingleObject(g_trig.wake, ms);
        if (g_quit) break;
        try {
            g_trig.Pump(NowUs(), ctl, PauseActive());
        } catch (...) {
            Log("triggers: exception in the worker (ignored)");
        }
    }
    UnregisterInternalThread();
    return 0;
}

}  // namespace

TriggerEngine& Triggers() { return g_trig; }

void TriggersInit() {
    bool exp = false;
    if (!g_started.compare_exchange_strong(exp, true)) return;
    int loaded = 0;
    bool any = false;
    for (int n = 1; n <= 64; ++n) {
        std::string v;
        if (!IniGet(F("triggers.trigger%d", n), v)) continue;
        any = true;
        TriggerRule r;
        std::string e;
        if (!TriggerParse(v, r, e)) { Log("triggers: Trigger%d ignored: %s", n, e.c_str()); continue; }
        if (g_trig.Add(r, e)) ++loaded; else Log("triggers: Trigger%d ignored: %s", n, e.c_str());
    }
    (void)any;
    Log("triggers: %d rule(s) loaded", loaded);
    g_trig.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Skills().trig = &g_trig;   // the apply-skill detour feeds the engine (the process-wide skill engine only)
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_thread || !g_trig.wake) { Log("triggers: could not start the worker thread (triggers disabled)"); Skills().trig = nullptr; return; }
    TriggersSyncFromSettings();
}

void TriggersShutdown(bool wait) {
    g_trig.enabled = false;
    g_quit = true;
    if (g_trig.wake) SetEvent(g_trig.wake);
    if (g_thread && wait) { WaitForSingleObject(g_thread, 1500); CloseHandle(g_thread); g_thread = nullptr; }
}

TriggerRule TriggerTowerRule(float scale, float in, float hold, float out, int cooldownMs) {
    TriggerRule r;
    strncpy_s(r.name, sizeof r.name, "TowerArrow", _TRUNCATE);
    r.keys[0] = KeyEnc(1091, 5);   // the player's explosive-arrow record (keys 1091/5/0/0) - NOT the greatsword fire hits (1091/2/1105)
    r.nkeys = 1;
    r.enabled = 1; r.firstOnly = 1; r.lastOnly = 0; r.builtin = 1;
    r.scale = scale; r.in = in; r.hold = hold; r.out = out;
    r.cooldownMs = cooldownMs; r.delayMs = 0; r.idleMs = 250;
    return r;
}

void TriggersSyncFromSettings() {
    g_trig.allowOverride = CfgBool(Triggers_Override);
    bool user = CfgBool(Triggers_Enabled), tower = CfgBool(Triggers_TowerBlast);
    g_trig.userOn = user;
    g_trig.towerOn = tower;
    {   // the built-in rule follows its [Advanced] curve settings; an invalid combination disables it (and says so once per change)
        TriggerRule r = TriggerTowerRule(CfgGet(Triggers_TowerScale), CfgGet(Triggers_TowerIn), CfgGet(Triggers_TowerHold), CfgGet(Triggers_TowerOut), static_cast<int>(CfgGet(Triggers_TowerCooldownMs)));
        float sum = r.in + r.hold + r.out;
        if (sum >= 0.05f && sum <= 10.0f) g_trig.SetBuiltin(r);
        else { r.enabled = 0; g_trig.SetBuiltin(r); if (tower) Log("triggers: TowerBlast curve in + hold + out = %g s is outside 0.05 .. 10 s: the built-in rule is disabled", static_cast<double>(sum)); }
    }
    bool on = user || tower;
    g_trig.enabled = on && g_started.load();
    if (on && g_trig.ActiveCount() > 0 && (!SkillsHooked() || !Skills().enabled.load())) SkillsRequestOn();   // the apply-skill hook feeds the engine
}

std::string TriggersSummary() {
    if (!g_started.load()) return "";
    TrigStats s = g_trig.Stats();
    return F("triggers: %s (user rules %s, TowerBlast %s), %d rule(s) (%d active), fired %llu, skipped %llu\n", g_trig.enabled.load() ? "ON" : "off", g_trig.userOn.load() ? "on" : "off", g_trig.towerOn.load() ? "on" : "off", g_trig.RuleCount(), g_trig.ActiveCount(), U64(s.fired),
             U64(s.skippedPause + s.skippedBusy + s.skippedUnreadable + s.writeFailed));
}

std::string TriggerStatusText() {
    if (!g_started.load()) return "not running";
    TrigStats s = g_trig.Stats();
    if (!g_trig.enabled.load()) return F("off (%d rule(s))", g_trig.RuleCount());
    return F("%llu fired, %llu skipped, %d active", U64(s.fired), U64(s.skippedPause + s.skippedBusy + s.skippedUnreadable + s.writeFailed), g_trig.ActiveCount());
}

// ------------------------------------------------------------------ ini + bridge
static bool SaveTriggers(std::string& err) {
    std::wstring path = g_dir + L"CombatMod.ini";
    std::string keep;
    {
        std::ifstream f(path);
        std::string line;
        bool skipping = false;
        while (f && std::getline(f, line)) {
            std::string t = Trim(line);
            if (!t.empty() && t[0] == '[') {
                size_t e = t.find(']');
                skipping = e != std::string::npos && Lower(Trim(t.substr(1, e - 1))) == "triggers";
            }
            if (!skipping) keep += line + "\n";
        }
    }
    while (keep.size() >= 2 && keep[keep.size() - 1] == '\n' && keep[keep.size() - 2] == '\n') keep.pop_back();
    std::string blk = "\n[Triggers]\n; Custom slow-motion triggers (written by `trigger save`). One line per rule:\n"
                      ";   Trigger<n> = key=<SkillKey list>|owner=player|scale=0.15|in=0.10|hold=0.5|out=0.45|cooldownMs=1500|firstOnly=1|delayMs=0|name=<text>\n"
                      "; Optional: lastOnly=1|idleMs=250 (fire on the last hit of a chain), enabled=0.\n";
    auto rules = g_trig.List();
    rules.erase(std::remove_if(rules.begin(), rules.end(), [](const std::pair<int, TriggerRule>& p) { return p.first > kTrigMax; }), rules.end());   // the built-in rule lives in [Advanced] settings
    int k = 0;
    for (auto& p : rules) blk += F("Trigger%d = %s\n", ++k, TriggerFormat(p.second).c_str());
    if (rules.empty()) blk += "Defaults=0\n";
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write the ini file"; return false; }
        o << keep << blk;
        if (!o) { err = "write failed"; return false; }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); err = F("cannot replace the ini file (error %lu)", GetLastError()); return false; }
    return true;
}

std::string CmdTrigger(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "status";
    auto needWrite = [&]() { return g_cfg.allowWrite != 0; };
    if (sub == "list" && t.size() == 2) {
        std::string o;
        for (auto& p : g_trig.List()) {
            const TriggerRule& r = p.second;
            const char* state = !r.enabled ? "DISABLED (enabled=0)" : p.first == kTrigBuiltin ? (g_trig.towerOn.load() ? "BUILT-IN, ACTIVE (Triggers.TowerBlast=1)" : "BUILT-IN, off (Triggers.TowerBlast=0)") : (g_trig.userOn.load() ? "ACTIVE" : "off (Triggers.Enabled=0)");
            o += F("#%d %s  [%s]  hits=%llu fired=%llu\n    %s\n    curve: scale %g, A = in+hold = %g s, B = out = %g s (provisional mapping); %s\n", p.first, r.name[0] ? r.name : "(unnamed)", state, U64(g_trig.RuleMatches(p.first)),
                   U64(g_trig.RuleFired(p.first)), TriggerFormat(r).c_str(), static_cast<double>(r.scale), static_cast<double>(r.in + r.hold), static_cast<double>(r.out),
                   r.lastOnly ? "fires on the LAST matching call of a chain" : r.firstOnly ? "fires on the FIRST matching call of a burst" : "fires on every matching call (after the cooldown)");
        }
        if (o.empty()) o = "(no triggers; `trigger add <line>`)\n";
        return Ok(o);
    }
    if (sub == "status" && t.size() <= 2) {
        TrigStats s = g_trig.Stats();
        std::string o = F("Triggers.Enabled=%d Triggers.TowerBlast=%d Triggers.Override=%d | engine=%s | rules=%d (%d active) | skill hook %s\n", CfgBool(Triggers_Enabled) ? 1 : 0, CfgBool(Triggers_TowerBlast) ? 1 : 0, CfgBool(Triggers_Override) ? 1 : 0,
                          g_started.load() ? "running" : "stopped", g_trig.RuleCount(), g_trig.ActiveCount(), SkillsHooked() ? "installed" : "NOT installed");
        o += F("matches=%llu fired=%llu burst_suppressed=%llu cooldown_suppressed=%llu | skipped: pause=%llu busy=%llu unreadable=%llu | write_failed=%llu\n", U64(s.matches), U64(s.fired),
               U64(s.burstSuppressed), U64(s.cooldownSuppressed), U64(s.skippedPause), U64(s.skippedBusy), U64(s.skippedUnreadable), U64(s.writeFailed));
        o += F("pause active=%d\n", PauseActive() ? 1 : 0);
        return Ok(o);
    }
    if ((sub == "enable" || sub == "disable") && t.size() <= 3) {
        bool on = sub == "enable";
        if (t.size() == 2) {
            std::string e;
            CfgTakeNote();
            if (!CfgSet(Triggers_Enabled, on ? 1.0f : 0.0f, e)) return Err(e);
            return Ok(F("Triggers.Enabled = %d\n", on ? 1 : 0) + CfgTakeNote());
        }
        uint64_t n;
        if (ParseNum(t[2], n) && n == static_cast<uint64_t>(kTrigBuiltin)) return Err("#17 is the built-in TowerArrow rule: use `cfg set Triggers.TowerBlast 0|1`");
        if (!ParseNum(t[2], n) || !g_trig.SetRuleEnabled(static_cast<int>(n), on)) return Err("no such trigger (`trigger list`)");
        return Ok(F("trigger #%llu %s\n", U64(n), on ? "enabled" : "disabled"));
    }
    if (sub == "log" && t.size() <= 3) {
        uint64_t n = 20;
        if (t.size() == 3 && (!ParseNum(t[2], n) || n == 0 || n > static_cast<uint64_t>(kTrigLog))) return Err(F("usage: trigger log [n 1..%d]", kTrigLog));
        std::string o;
        for (const TrigLogRec& r : g_trig.LogTail(static_cast<size_t>(n)))
            o += F("t=%.3fs #%d %s key=%u a1=0x%llX scale=%g A=%g B=%g age=%.1fms  %s\n", static_cast<double>(r.t_us) / 1e6, r.rule, r.name, r.key, U64(r.a1), static_cast<double>(r.scale), static_cast<double>(r.A),
                   static_cast<double>(r.B), static_cast<double>(r.ageUs) / 1000.0, r.outcome);
        if (o.empty()) o = "(nothing logged yet)\n";
        return Ok(o);
    }
    if (sub == "add" && t.size() >= 3) {
        if (!needWrite()) return Err("disabled");
        std::string line;
        for (size_t i = 2; i < t.size(); ++i) line += (i > 2 ? " " : "") + t[i];
        TriggerRule r;
        std::string e;
        if (!TriggerParse(line, r, e)) return Err(e);
        int n = g_trig.Add(r, e);
        if (!n) return Err(e);
        TriggersSyncFromSettings();
        return Ok(F("added as #%d: %s\n(runtime only: `trigger save` writes it to the ini)\n", n, TriggerFormat(r).c_str()));
    }
    if (sub == "del" && t.size() == 3) {
        if (!needWrite()) return Err("disabled");
        uint64_t n;
        if (ParseNum(t[2], n) && n == static_cast<uint64_t>(kTrigBuiltin)) return Err("#17 is the built-in TowerArrow rule: switch it off with `cfg set Triggers.TowerBlast 0`");
        if (!ParseNum(t[2], n) || !g_trig.Del(static_cast<int>(n))) return Err("no such trigger (`trigger list`)");
        return Ok(F("trigger #%llu deleted\n", U64(n)));
    }
    if (sub == "clear" && t.size() == 2) {
        if (!needWrite()) return Err("disabled");
        g_trig.Clear();
        return Ok("all triggers deleted (the ini is unchanged until `trigger save`)\n");
    }
    if (sub == "save" && t.size() == 2) {
        if (!needWrite()) return Err("disabled");
        std::string e;
        if (!SaveTriggers(e)) return Err(e);
        int users = 0;
        for (auto& p : g_trig.List()) if (p.first <= kTrigMax) ++users;
        Log("triggers: saved %d rule(s) to CombatMod.ini", users);
        return Ok(F("saved %d rule(s) to the [Triggers] section of CombatMod.ini\n", users));
    }
    if (sub == "fire" && (t.size() == 5 || t.size() == 6)) {
        if (!needWrite()) return Err("disabled");
        float sc, a, b;
        if (!ParseFloat(t[2], sc) || !ParseFloat(t[3], a) || !ParseFloat(t[4], b)) return Err("usage: trigger fire <scale 0.02..1> <A sec> <B sec> [force]");
        bool force = t.size() == 6 && Lower(t[5]) == "force";
        if (t.size() == 6 && !force) return Err("usage: trigger fire <scale> <A> <B> [force]");
        RealCtl ctl;
        std::string outcome;
        bool ok = g_trig.FireNow(sc, a, b, ctl, PauseActive(), force, NowUs(), outcome);
        return ok ? Ok(F("%s: curve (scale %g, 1.0, A %g, B %g, 0) written into both controller objects; the engine ends it by itself after A+B = %g s\n", outcome.c_str(), static_cast<double>(sc),
                         static_cast<double>(a), static_cast<double>(b), static_cast<double>(a + b)))
                  : Err(outcome);
    }
    return Err("usage: trigger list | status | add <line> | del <n> | clear | enable [n] | disable [n] | fire <scale> <A> <B> [force] | log [n] | save");
}
