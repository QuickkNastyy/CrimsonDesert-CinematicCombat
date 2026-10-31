#include "pause.h"
#include "gamelayout.h"
#include "layoutcheck.h"
#include "log.h"
#include "settings.h"
#include "sigscan.h"
#include <cstring>
#include <mutex>

namespace {
std::atomic<bool> g_open{false}, g_forced{false}, g_quit{false}, g_started{false};
std::atomic<bool> g_held{false};                 // we are holding the game (pause values written)
std::atomic<uint64_t> g_reasserts{0}, g_pauses{0}, g_resumes{0}, g_watchdog{0}, g_chainFails{0};
std::atomic<long long> g_heldSinceUs{0};
std::mutex g_mu;                                  // serialises writers (keeper thread vs. UI thread restore)
HANDLE g_wake = nullptr, g_thread = nullptr;
std::string g_lastNote;

// POD + SEH only (legal __try)
const char* PauseCtlClass(const float f[5]);
bool Peek20(uintptr_t a, float out[5]) {
    __try { memcpy(out, reinterpret_cast<const void*>(a), 20); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { memset(out, 0, 20); return false; }
}
bool Poke(uintptr_t a, const float* v, size_t n) {
    __try { memcpy(reinterpret_cast<void*>(a), v, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool PeekQ(uintptr_t a, uint64_t& v) { return SafeRead(a, &v, 8); }

struct Chain { uintptr_t obj[2] = {0, 0}; };

// The write guard (B1): the chain pointers being non-null and readable proves nothing (kCtlObj moved between builds while time_mgr_global kept matching).
// Before ANY write - the pause, the restore and the trigger engine all come through ReadChain - BOTH objects must look like time-controller objects:
// LayoutClassifyCtl() must say idle, a valid curve or our own pause signature. Otherwise the write is refused; the reason is logged once per change
// and shown by `ui status`, `ui pause status` and `layout check`.
std::mutex g_refMu;
std::string g_refusal;                      // the last refusal reason ("" = none since the guard last passed)
std::atomic<uint64_t> g_refusals{0};
void Refuse(const std::string& why) {
    ++g_refusals;
    std::lock_guard<std::mutex> l(g_refMu);
    if (g_refusal != why) { g_refusal = why; Log("pause: WRITE REFUSED - %s (pause and custom triggers stay inert until the controller objects look right; `layout check`)", why.c_str()); }
}
void GuardPassed() {
    std::lock_guard<std::mutex> l(g_refMu);
    if (!g_refusal.empty()) { Log("pause: the controller objects look like time controllers again - writes allowed"); g_refusal.clear(); }
}

// G (time_mgr_global) -> M -> P -> the two controller objects; every hop is checked, then the objects themselves (see above)
bool ReadChain(Chain& c, std::string* why = nullptr) {
    Sig sg;
    if (!FindSig("time_mgr_global", sg) || sg.st != SigStatus::Resolved) { if (why) *why = "signature time_mgr_global is not resolved"; return false; }
    uint64_t M = 0, P = 0, o1 = 0, o2 = 0;
    if (!PeekQ(sg.addr, M) || !M) { if (why) *why = "G is null or unreadable"; return false; }
    if (!PeekQ(static_cast<uintptr_t>(M), P) || !P) { if (why) *why = "M=*G is null or unreadable"; return false; }
    if (!PeekQ(static_cast<uintptr_t>(P) + gl::kCtlObj[0], o1) || !o1 || !PeekQ(static_cast<uintptr_t>(P) + gl::kCtlObj[1], o2) || !o2) { if (why) *why = "controller objects are null or unreadable"; return false; }
    float t[2][5];
    if (!Peek20(static_cast<uintptr_t>(o1), t[0]) || !Peek20(static_cast<uintptr_t>(o2), t[1])) { if (why) *why = "controller objects are not readable"; return false; }
    if (o1 == o2) { std::string r = F("both controller slots (P+0x%zX, P+0x%zX) point at the same object 0x%llX", static_cast<size_t>(gl::kCtlObj[0]), static_cast<size_t>(gl::kCtlObj[1]), U64(o1)); Refuse(r); if (why) *why = r; return false; }
    for (int i = 0; i < 2; ++i) {
        const char* cls = LayoutClassifyCtl(t[i]);
        if (!strcmp(cls, "unexpected")) {
            std::string r = F("the object at P+0x%zX (0x%llX) does not look like a time controller: f0..f4 = %g %g %g %g %g", static_cast<size_t>(gl::kCtlObj[i]), U64(i ? o2 : o1), static_cast<double>(t[i][0]),
                              static_cast<double>(t[i][1]), static_cast<double>(t[i][2]), static_cast<double>(t[i][3]), static_cast<double>(t[i][4]));
            Refuse(r);
            if (why) *why = r;
            return false;
        }
    }
    GuardPassed();
    c.obj[0] = static_cast<uintptr_t>(o1);
    c.obj[1] = static_cast<uintptr_t>(o2);
    return true;
}

// callers hold g_mu
void WriteBoth(const Chain& c, const PauseVals& v) {
    for (uintptr_t o : c.obj) Poke(o, v.f, sizeof v.f);
}

void RestoreLocked(const char* why) {
    Chain c;
    std::string e;
    if (ReadChain(c, &e)) WriteBoth(c, PauseIdle());
    else Log("pause: chain unreadable during restore (%s) - nothing to write", e.c_str());
    if (g_held.exchange(false)) { ++g_resumes; Log("pause: RESUMED (%s)", why); }
}

bool Wanted() { return g_forced.load() || (g_open.load() && CfgBool(UI_PauseGame)); }

DWORD WINAPI Keeper(LPVOID) {
    RegisterInternalThread();
    long long sigSince = 0;
    while (!g_quit) {
        bool want = Wanted();
        WaitForSingleObject(g_wake, want ? 8 : 100);
        if (g_quit) break;
        want = Wanted();
        try {
            std::lock_guard<std::mutex> l(g_mu);
            Chain c;
            std::string why;
            bool ok = ReadChain(c, &why);
            if (want) {
                if (!ok) { ++g_chainFails; if (g_held) Log("pause: chain became unreadable (%s) - not touching anything", why.c_str()); g_lastNote = why; g_held = false; continue; }
                PauseVals sig = PauseSignature();
                for (uintptr_t o : c.obj) {
                    PauseVals cur;
                    Peek20(o, cur.f);
                    PauseAction a = PauseDecide(cur);
                    if (a == PauseAction::WriteAll) { Poke(o, sig.f, sizeof sig.f); ++g_reasserts; }
                    else if (a == PauseAction::WriteProgress) { float z = 0.0f; Poke(o + 16, &z, 4); ++g_reasserts; }
                }
                if (!g_held.exchange(true)) { ++g_pauses; g_heldSinceUs = NowUs(); Log("pause: PAUSED (controller objects 0x%llX / 0x%llX)", U64(c.obj[0]), U64(c.obj[1])); }
                sigSince = 0;
            } else {
                if (g_held) { RestoreLocked("no longer wanted"); sigSince = 0; continue; }
                if (!ok) { sigSince = 0; continue; }
                bool ours = false;   // watchdog: our signature must never stay behind
                for (uintptr_t o : c.obj) { PauseVals cur; Peek20(o, cur.f); ours = ours || PauseIsOurs(cur); }
                if (!ours) sigSince = 0;
                else if (!sigSince) sigSince = NowUs();
                else if (NowUs() - sigSince > 1000000) {
                    WriteBoth(c, PauseIdle());
                    ++g_watchdog;
                    sigSince = 0;
                    Log("pause: WATCHDOG found our pause signature without a request for > 1 s and restored the idle values");
                }
            }
        } catch (...) {
            Log("pause: exception in the keeper (ignored)");
        }
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_held) RestoreLocked("keeper stopping");
    }
    UnregisterInternalThread();
    return 0;
}
}  // namespace

// The same controller objects, for the custom slow-motion triggers (triggers.cpp): read both objects / write one curve into both.
// They share the pause keeper's mutex, so a trigger write can never interleave with a keeper tick.
bool PauseCtlRead(float out[2][5]) {
    std::lock_guard<std::mutex> l(g_mu);
    Chain c;
    if (!ReadChain(c)) return false;
    for (int i = 0; i < 2; ++i) if (!Peek20(c.obj[i], out[i])) return false;
    return true;
}

bool PauseCtlWrite(const float v[5]) {
    std::lock_guard<std::mutex> l(g_mu);
    Chain c;
    if (!ReadChain(c)) return false;
    bool ok = true;
    for (uintptr_t o : c.obj) ok = Poke(o, v, 20) && ok;
    return ok;
}

std::string PauseGuardStatus() {
    std::lock_guard<std::mutex> l(g_refMu);
    return g_refusal.empty() ? F("ok (%llu writes refused so far)", U64(g_refusals.load())) : F("REFUSING WRITES: %s (%llu refusals)", g_refusal.c_str(), U64(g_refusals.load()));
}

std::string PauseCtlReport(bool& ok) {
    ok = true;
    std::string o;
    Sig sg;
    if (!FindSig("time_mgr_global", sg) || sg.st != SigStatus::Resolved) { ok = false; return "    signature time_mgr_global is not resolved: nothing to check (pause and custom triggers are inert)\n"; }
    uint64_t M = 0, P = 0;
    o += F("    G = 0x%llX (the qword the signature points at)\n", U64(sg.addr));
    if (!PeekQ(sg.addr, M) || !M) { ok = false; return o + "    M = *G is null or unreadable\n"; }
    if (!PeekQ(static_cast<uintptr_t>(M), P) || !P) { ok = false; return o + F("    M = 0x%llX, P = *M is null or unreadable\n", U64(M)); }
    o += F("    M = *G = 0x%llX, P = *M = 0x%llX\n", U64(M), U64(P));
    for (int i = 0; i < 2; ++i) {
        uint64_t obj = 0;
        float f[5] = {};
        if (!PeekQ(static_cast<uintptr_t>(P) + gl::kCtlObj[i], obj) || !obj || !Peek20(static_cast<uintptr_t>(obj), f)) {
            ok = false;
            o += F("    object %d at P+0x%zX: pointer null / unreadable  => gl::kCtlObj[%d] is probably wrong\n", i + 1, static_cast<size_t>(gl::kCtlObj[i]), i);
            continue;
        }
        const char* cls = LayoutClassifyCtl(f);
        if (!strcmp(cls, "unexpected")) ok = false;
        o += F("    object %d at P+0x%zX = 0x%llX: f0..f4 = %g %g %g %g %g  -> %s%s\n", i + 1, static_cast<size_t>(gl::kCtlObj[i]), U64(obj), static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2]), static_cast<double>(f[3]),
               static_cast<double>(f[4]), !strcmp(cls, "idle") ? "IDLE (-1,-1,0,0,0) as expected" : !strcmp(cls, "curve") ? "a valid running curve" : !strcmp(cls, "pause") ? "our own pause signature" : "UNEXPECTED values",
               !strcmp(cls, "unexpected") ? "  => gl::kCtlObj / the f0..f4 meaning is probably wrong" : "");
    }
    // which pointers in P point at an object that currently holds the idle pattern? (the right offsets must be among them while no slow motion runs)
    std::string cands;
    int nc = 0;
    std::vector<uint8_t> blk(gl::kCtlPScanLimit);
    size_t got = blk.size();
    while (got >= 0x100 && !SafeRead(static_cast<uintptr_t>(P), blk.data(), got)) got -= 0x100;
    for (size_t off = 0; off + 8 <= got && nc < 16; off += 8) {
        uint64_t q;
        memcpy(&q, blk.data() + off, 8);
        float f[5];
        if (q < 0x10000 || q > 0x00007FFFFFFFFFFFull || !Peek20(static_cast<uintptr_t>(q), f)) continue;
        if (f[0] == -1.0f && f[1] == -1.0f && f[2] == 0.0f && f[3] == 0.0f && f[4] == 0.0f) { cands += F(" +0x%zX", off); ++nc; }
    }
    o += F("    pointers in P (below +0x%zX) that point at an IDLE-looking object:%s  (expected: +0x%zX +0x%zX)\n", static_cast<size_t>(gl::kCtlPScanLimit), cands.empty() ? " none (a curve may be running right now: try again when nothing is slow)" : cands.c_str(),
           static_cast<size_t>(gl::kCtlObj[0]), static_cast<size_t>(gl::kCtlObj[1]));
    if (ok) o += "  => time-controller layout OK\n";
    else o += "  => time-controller layout WRONG: fix gl::kCtlObj (the candidates above are the likely offsets). The pause and the custom triggers REFUSE to write while this is wrong.\n";
    o += "    write guard: " + PauseGuardStatus() + "\n";
    return o;
}

PauseVals PauseSignature() { return PauseVals{{0.001f, 1.0f, 3600.0f, 1.0f, 0.0f}}; }
PauseVals PauseIdle() { return PauseVals{{-1.0f, -1.0f, 0.0f, 0.0f, 0.0f}}; }
bool PauseIsOurs(const PauseVals& v) { return v.f[0] == 0.001f && v.f[2] == 3600.0f; }

PauseAction PauseDecide(const PauseVals& cur) {
    PauseVals s = PauseSignature();
    if (cur.f[0] != s.f[0] || cur.f[1] != s.f[1] || cur.f[2] != s.f[2] || cur.f[3] != s.f[3]) return PauseAction::WriteAll;   // the engine changed the object
    if (cur.f[4] != 0.0f) return PauseAction::WriteProgress;                                                                  // progress advances in real time
    return PauseAction::None;
}

void PauseInit() {
    bool exp = false;
    if (!g_started.compare_exchange_strong(exp, true)) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, Keeper, nullptr, 0, nullptr);
    if (!g_thread) { Log("pause: could not start the keeper thread (pausing disabled)"); g_started = false; }
}

void PauseSetOpen(bool open) {
    g_open = open;
    if (g_wake) SetEvent(g_wake);
}

void PauseRestoreNow() {
    g_open = false;
    g_forced = false;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_held) RestoreLocked("restore requested");
}

bool PauseActive() { return g_held.load(); }

void PauseShutdown(bool wait) {
    g_open = false;
    g_forced = false;
    PauseRestoreNow();
    g_quit = true;
    if (g_wake) SetEvent(g_wake);
    if (g_thread && wait) { WaitForSingleObject(g_thread, 1500); CloseHandle(g_thread); g_thread = nullptr; }
}

std::string CmdPause(const Args& t) {
    std::string sub = t.size() > 2 ? Lower(t[2]) : "status";
    if ((sub == "on" || sub == "off") && t.size() == 3) {
        if (!g_started) return Err("the pause keeper is not running");
        if (sub == "on") { Chain c; std::string why; if (!ReadChain(c, &why)) return Err("cannot pause: " + why); g_forced = true; }
        else { g_forced = false; }
        SetEvent(g_wake);
        for (int i = 0; i < 60 && g_held.load() != (sub == "on"); ++i) Sleep(10);
        return g_held.load() == (sub == "on") ? Ok(std::string("pause ") + sub + "\n") : Err("the keeper did not respond in time");
    }
    if (sub == "status" && t.size() <= 3) {
        Chain c;
        std::string why;
        bool ok = ReadChain(c, &why);
        std::string o = F("write_guard=%s\n", PauseGuardStatus().c_str());
        o += F("keeper=%s held=%d wanted=%d (panel_open=%d UI.PauseGame=%d forced=%d)\n", g_started ? "running" : "stopped", g_held ? 1 : 0, Wanted() ? 1 : 0, g_open ? 1 : 0, CfgBool(UI_PauseGame) ? 1 : 0, g_forced ? 1 : 0);
        o += F("pauses=%llu resumes=%llu reasserts=%llu watchdog_restores=%llu chain_failures=%llu\n", U64(g_pauses.load()), U64(g_resumes.load()), U64(g_reasserts.load()), U64(g_watchdog.load()), U64(g_chainFails.load()));
        if (ok) {
            for (int i = 0; i < 2; ++i) {
                PauseVals v;
                Peek20(c.obj[i], v.f);
                o += F("obj%d=0x%llX f0..f4 = %g %g %g %g %g%s\n", i + 1, U64(c.obj[i]), static_cast<double>(v.f[0]), static_cast<double>(v.f[1]), static_cast<double>(v.f[2]),
                       static_cast<double>(v.f[3]), static_cast<double>(v.f[4]), PauseIsOurs(v) ? "  (our pause signature)" : "");
            }
        } else o += "chain: " + why + "\n";
        return Ok(o);
    }
    return Err("usage: ui pause on|off|status");
}
