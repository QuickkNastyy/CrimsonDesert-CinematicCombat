#include "selftest.h"
#include "log.h"
#include "memwrite.h"
#include "etrace.h"
#include "gamelayout.h"
#include "launchboost.h"
#include "launchscan.h"
#include "layoutcheck.h"
#include "settings.h"
#include "hookretry.h"
#include "pause.h"
#include "telemetry.h"
#include "triggers.h"
#include "uimodel.h"
#include "sigscan.h"
#include "timeamp.h"
#include "trace.h"
#include "watch.h"
#include <bit>
#include <fstream>
#include <limits>
#include <sstream>
#include <cmath>
#include <cstring>

// Unoptimised on purpose: guarantees a prologue of >= 5 relocatable bytes for MinHook.
#pragma optimize("", off)
extern "C" __declspec(noinline) int CombatModTestFn(int a, int b) {
    volatile int x = a * 3;
    volatile int y = x + b;
    return y;
}
#pragma optimize("", on)

alignas(8) static volatile uint32_t g_testVar = 0;

namespace {
struct Report {
    std::string out;
    int pass = 0, fail = 0;
    void Add(bool ok, const char* name, const std::string& detail) {
        (ok ? pass : fail)++;
        out += F("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
        Log("selftest %s %s: %s", ok ? "PASS" : "FAIL", name, detail.c_str());
    }
};

bool Bytes(uintptr_t a, const std::vector<uint8_t>& want) {
    std::vector<uint8_t> got(want.size());
    return SafeRead(a, got.data(), got.size()) && got == want;
}

// A fake time-event data block D built ONLY from src/gamelayout.h (type id, scale / second float / A / B offsets), so the tests follow a layout change.
struct EvBlk {
    alignas(16) uint8_t b[0x40];
    EvBlk() { memset(b, 0, sizeof b); uint16_t t = static_cast<uint16_t>(gl::kTimeEventType); memcpy(b + gl::kEvType, &t, 2); }
    EvBlk(float s, float s2, float a, float bb) : EvBlk() { Scale() = s; Second() = s2; A() = a; B() = bb; }
    float& Scale() { return *reinterpret_cast<float*>(b + gl::kEvScale); }
    float& Second() { return *reinterpret_cast<float*>(b + gl::kEvScale2); }
    float& A() { return *reinterpret_cast<float*>(b + gl::kEvA); }
    float& B() { return *reinterpret_cast<float*>(b + gl::kEvB); }
    void Set(float s, float a, float bb) { Scale() = s; A() = a; B() = bb; }
    uint64_t Ptr() { return reinterpret_cast<uint64_t>(b); }
};

// The "a3" argument of the time_curve_start / event_dispatcher hooks: a block whose pointer at gl::kEvDataInArgs is the event data D.
struct EvArg {
    alignas(16) uint8_t b[0x20];
    explicit EvArg(uint64_t D) { memset(b, 0, sizeof b); memcpy(b + gl::kEvDataInArgs, &D, 8); }
    uint64_t Ptr() { return reinterpret_cast<uint64_t>(b); }
    operator uint64_t*() { return reinterpret_cast<uint64_t*>(b); }   // where a test passes the block as the hook's a3
};

void TestSigs(Report& r) {
    auto sigs = SigsSnapshot();
    std::string bad, optMissing;
    size_t ok = 0;
    for (auto& s : sigs) {
        bool optional = false;
        for (const char* o : gl::kOptionalSigs) optional = optional || s.name == o;
        if (s.st == SigStatus::Resolved) ++ok;
        else if (optional) optMissing += F(" %s=%s", s.name.c_str(), SigStatusName(s.st));   // debug-only signature: not an error
        else bad += F(" %s=%s", s.name.c_str(), SigStatusName(s.st));
    }
    r.Add(!sigs.empty() && bad.empty(), "sigs", F("%zu/%zu resolved%s%s", ok, sigs.size(), bad.c_str(), optMissing.empty() ? "" : (" (optional, unused by the plugin:" + optMissing + ")").c_str()));
}

void TestHeader(Report& r) {
    ModInfo m = MainModule();
    uint16_t mz = 0;
    uint32_t pe = 0, lfanew = 0;
    bool ok = m.base && Rd(m.base, mz) && mz == 0x5A4D && Rd(m.base + 0x3C, lfanew) && Rd(m.base + lfanew, pe) && pe == 0x4550;
    r.Add(ok, "module_header", F("base=0x%llX size=0x%llX MZ=%s PE=%s", U64(m.base), U64(m.size), mz == 0x5A4D ? "yes" : "no", pe == 0x4550 ? "yes" : "no"));
}

void TestWrite(Report& r) {
    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) { r.Add(false, "write_undo", "VirtualAlloc failed"); return; }
    memset(buf, 0xAA, 64);
    uintptr_t a = reinterpret_cast<uintptr_t>(buf);
    std::string m, why;
    int id1 = 0, id2 = 0, id3 = 0;
    bool ok = true;
    auto fail = [&](const char* w) { if (ok) why = w; ok = false; };

    std::vector<uint8_t> v1 = {1, 2, 3, 4}, aa4(4, 0xAA), nine(4, 9);
    if (!MemWrite(a, v1, &aa4, false, 0, m, &id1) || !Bytes(a, v1)) fail("guarded write failed");
    if (MemWrite(a, nine, &aa4, false, 0, m, &id3)) fail("expect mismatch was NOT refused");   // must refuse
    else if (!Bytes(a, v1)) fail("refused write still changed memory");
    bool uok = false;
    UndoLast(1, uok);
    if (!uok || !Bytes(a, aa4)) fail("undo did not restore");
    std::vector<uint8_t> v8(8, 0x77), aa8(8, 0xAA);
    if (!MemWrite(a + 8, v8, nullptr, false, 100, m, &id2) || !Bytes(a + 8, v8)) fail("revert_ms write failed");
    Sleep(350);
    if (!Bytes(a + 8, aa8)) fail("auto-revert did not fire");

    // writing into the exe's executable section must be refused without 'patch' (and nothing is written when refused)
    ModInfo mi = MainModule();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mi.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mi.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t code = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && !code; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) code = mi.base + sec[i].VirtualAddress;
    uint8_t cb = 0;
    if (code && Rd(code, cb)) {
        std::vector<uint8_t> same = {cb};
        if (MemWrite(code, same, nullptr, false, 0, m)) fail("write into exe code was NOT refused");
    } else fail("could not locate exe code section");

    for (int id : {id1, id2, id3}) if (id) JournalRemoveId(id);
    VirtualFree(buf, 0, MEM_RELEASE);
    r.Add(ok, "write_undo", ok ? "guarded write, expect-mismatch refusal, undo, revert_ms, code-write refusal all OK" : why);
}

void TestTrace(Report& r) {
    int (*volatile fp)(int, int) = CombatModTestFn;
    uintptr_t a = reinterpret_cast<uintptr_t>(&CombatModTestFn);
    std::string m;
    bool ok = true;
    std::string why;
    auto fail = [&](const std::string& w) { if (ok) why = w; ok = false; };
    if (fp(7, 5) != 26) fail("baseline function result wrong");
    if (!TraceAdd(a, "selftest_fn", true, m)) { r.Add(false, "trace", "TraceAdd: " + m); return; }
    int r1 = fp(7, 5), r2 = fp(1, 2);
    if (r1 != 26 || r2 != 5) fail(F("original not preserved while traced (got %d,%d)", r1, r2));
    std::vector<TraceRec> recs;
    uint64_t total = 0;
    TraceRecords(a, 10, recs, total);
    if (total != 2 || recs.size() != 2) fail(F("expected 2 logged calls, got total=%llu shown=%zu", U64(total), recs.size()));
    else if (recs[0].rcx != 7 || recs[0].rdx != 5 || recs[1].rcx != 1 || recs[1].rdx != 2 || recs[0].tid != GetCurrentThreadId() || !recs[0].ret)
        fail("logged args/tid/return address wrong");
    if (!TraceRemove(a, m)) fail("TraceRemove: " + m);
    if (fp(7, 5) != 26) fail("function broken after untrace");
    recs.clear();
    TraceRecords(a, 10, recs, total);
    if (total != 2) fail("call was logged after untrace");
    r.Add(ok, "trace", ok ? "installed, 2 calls logged with correct rcx/rdx/tid/ret, result preserved, removed cleanly" : why);
}

// ---- time amplifier: private AmpEngine + fake event data + fake "game function"; the real hook is never touched.
struct FakeCall { __int64 a1 = 0; uint32_t* a2 = nullptr; __int64 a3 = 0, a4 = 0; void** a5 = nullptr; int n = 0; };
FakeCall g_fake;
__int64* __fastcall FakeOrig(__int64 a1, uint32_t* a2, __int64 a3, __int64 a4, void** a5) {
    g_fake.a1 = a1; g_fake.a2 = a2; g_fake.a3 = a3; g_fake.a4 = a4; g_fake.a5 = a5; ++g_fake.n;
    return reinterpret_cast<__int64*>(static_cast<uintptr_t>(0x1234ABCD));
}
bool FEq(float a, float b) { return memcmp(&a, &b, 4) == 0; }

void TestAmp(Report& r) {
    AmpEngine e;
    e.logNew = false;
    AmpParams p;
    p.scaleMul = 0.5f; p.rampIn = p.rampOut = 2.0f; p.minScale = 0.02f;
    p.overrides.emplace_back(0.1f, 0.04f);
    e.SetParams(p);
    e.SetEnabled(true);
    // D layout (float index): [2]=+8 scale, [3]=+0xC, [4]=+0x10 A, [5]=+0x14 B, byte +0x18 path selector
    EvBlk D1(0.6f, 0.3f, 0.5f, 0.2f);
    EvBlk D2(0.1f, 0.3f, 0.0f, 0.2f);
    EvBlk D3(1.0f, 0.3f, 0.3f, 0.0f);
    EvBlk D4(0.03f, 0.3f, 0.5f, 0.2f);
    EvArg ev1(D1.Ptr()), ev2(D2.Ptr());
    EvArg ev3(D3.Ptr()), ev4(D4.Ptr());
    uint32_t a2v = 7;
    void* a5v = nullptr;
    bool ok = true;
    std::string why;
    auto fail = [&](const char* w) { if (ok) why = w; ok = false; };
    auto call = [&](uint64_t* ev) {
        return AmpCallThrough(e, FakeOrig, 11, &a2v, reinterpret_cast<__int64>(ev), 44, &a5v, 0x5555);
    };

    for (int i = 0; i < 6; ++i) {  // repeated calls must not compound
        __int64* rv = call(ev1);
        if (reinterpret_cast<uintptr_t>(rv) != 0x1234ABCD) fail("original's return value not passed through");
        call(ev2); call(ev3); call(ev4);
    }
    if (g_fake.a1 != 11 || g_fake.a2 != &a2v || g_fake.a3 != static_cast<__int64>(ev4.Ptr()) || g_fake.a4 != 44 || g_fake.a5 != &a5v) fail("original not called with identical args");
    if (!FEq(D1.Scale(), 0.6f * 0.5f) || !FEq(D1.A(), 0.5f * 2.0f) || !FEq(D1.B(), 0.2f * 2.0f)) fail("D1 amplified wrongly or compounded");
    if (!FEq(D1.Second(), 0.3f)) fail("untouched field D+0xC was modified");
    if (!FEq(D2.Scale(), 0.04f) || !FEq(D2.A(), 0.0f)) fail("scale override / zero timing wrong");
    if (!FEq(D3.Scale(), 1.0f) || !FEq(D3.A(), 0.3f)) fail("a no-op event (scale 1.0) must be left completely alone");
    if (!FEq(D4.Scale(), 0.02f)) fail("MinScale clamp wrong");
    if (e.Stats().distinct != 4 || e.Stats().events != 24) fail("event/distinct counters wrong");

    D1.Scale() = 0.25f;  // the game rewrites the field: new original must be adopted, still no compounding
    call(ev1); call(ev1);
    if (!FEq(D1.Scale(), 0.125f) || e.Stats().rebased == 0) fail("rebase after external change failed");

    e.SetEnabled(false);  // restores originals
    if (!FEq(D1.Scale(), 0.25f) || !FEq(D1.A(), 0.5f) || !FEq(D1.B(), 0.2f) || !FEq(D2.Scale(), 0.1f) || !FEq(D3.A(), 0.3f) || !FEq(D4.Scale(), 0.03f)) fail("restore did not bring originals back");
    float before = D1.Scale();
    call(ev1);
    if (!FEq(D1.Scale(), before) || g_fake.n == 0) fail("disabled engine still modified data (or skipped the original)");

    e.SetEnabled(true);
    call(ev1);
    D1.A() = 0.9f;  // game changes a field after our write; restore must not clobber it
    e.RestoreAll();
    if (!FEq(D1.A(), 0.9f) || !FEq(D1.Scale(), 0.25f)) fail("restore clobbered a value changed by the game");
    e.SetEnabled(false);
    r.Add(ok, "timeamp", ok ? "6x repeated calls idempotent, override/min-clamp/no-op rules, args+return passthrough, rebase, restore, disabled=pass-through" : why);
}

// ---- skill context capture + amp filters: private engines, fake "apply attack skill" function, fake amp event.
uint64_t g_sfA1 = 0;
__int64 g_sfA2 = 0;
int g_sfN = 0;
__int64 __fastcall FakeApply(uint64_t* a1, __int64 a2) {
    g_sfA1 = reinterpret_cast<uint64_t>(a1); g_sfA2 = a2; ++g_sfN;
    return 0x77EE;
}

void TestSkills(Report& r) {
    SkillEngine se, se2;
    se.enabled = true;
    alignas(16) uint8_t Sb[0x200] = {};
    alignas(16) uint8_t A2[0x200] = {};
    const uint32_t kd[8] = {10253, 7, 0xAABBCCDD, 9, 0x11, 0x22, 0x33, 0x44};
    memcpy(Sb + gl::kSKeys, kd, 32);
    uint64_t pS = reinterpret_cast<uint64_t>(Sb), v58 = 0x58, v60 = 0x60, o160 = 0xABCDEF;
    memcpy(A2 + gl::kApplyS, &pS, 8); memcpy(A2 + gl::kApplyQ58, &v58, 8); memcpy(A2 + gl::kApplyQ60, &v60, 8); memcpy(A2 + gl::kApplyO160, &o160, 8);
    const uint64_t kOwner = 0x22B8204FC00ull, kOther = 0x99ull;
    auto call = [&](SkillEngine& e, uint64_t a1) { return SkillCallThrough(e, FakeApply, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<__int64>(A2), 0x4242); };

    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };

    // 1. capture contents + identical-argument passthrough
    long long t0 = NowUs();
    __int64 rv = call(se, kOwner);
    need(rv == 0x77EE && g_sfA1 == kOwner && g_sfA2 == reinterpret_cast<__int64>(A2), "original not called with identical args / return value lost");
    std::vector<SkillRec> tail = se.Tail(5);
    if (tail.size() != 1) need(false, "expected exactly 1 captured record");
    else {
        const SkillRec& c = tail[0];
        need(c.a1 == kOwner && c.a2 == reinterpret_cast<uint64_t>(A2) && c.S == pS && !memcmp(c.dw, kd, 32) && c.o160 == o160 && c.q58 == v58 && c.q60 == v60,
             "captured a1/a2/S/dwords/a2+0x160/a2+0x58/a2+0x60 wrong");
        need(c.tid == GetCurrentThreadId() && c.ret == 0x4242 && c.t_us >= t0 && c.t_us <= NowUs(), "tid / return address / timestamp wrong");
    }
    // 2. unreadable a2 never crashes and records zeros
    se.OnCall(kOther, 8, 1);
    std::vector<SkillRec> tl = se.Tail(1);
    need(tl.size() == 1 && tl[0].S == 0 && tl[0].dw[0] == 0 && tl[0].a1 == kOther, "unreadable a2 not handled");
    // 3. disabled = pass-through, nothing recorded
    uint64_t before = se.Total();
    se.enabled = false;
    int n0 = g_sfN;
    call(se, kOwner);
    need(se.Total() == before && g_sfN == n0 + 1, "disabled engine recorded or skipped the original");
    se.enabled = true;

    // 4. amp correlation + filters
    AmpEngine ae;
    ae.logNew = false;
    ae.SetSkills(&se);
    AmpParams p;
    p.scaleMul = 0.5f; p.rampIn = p.rampOut = 2.0f; p.contextWindowMs = 600;
    ae.SetParams(p);
    ae.SetEnabled(true);
    EvBlk D(0.6f, 0.3f, 0.5f, 0.2f);
    EvArg ev(D.Ptr());
    auto isAmp = [&] { return FEq(D.Scale(), 0.6f * 0.5f) && FEq(D.A(), 1.0f) && FEq(D.B(), 0.4f); };
    auto isOrig = [&] { return FEq(D.Scale(), 0.6f) && FEq(D.A(), 0.5f) && FEq(D.B(), 0.2f); };
    auto fire = [&] { ae.OnEvent(ev.Ptr(), 0x5151); return ae.Tail(1); };
    auto setp = [&](auto&& edit) { AmpParams q = ae.Params(); edit(q); ae.SetParams(q); };

    call(se, kOwner);
    std::vector<AmpLogRec> lr = fire();
    need(!lr.empty() && lr[0].haveCtx && lr[0].ctxA1 == kOwner && lr[0].ctxKeys[0] == 10253 && lr[0].ctxKeys[3] == 9 && lr[0].ctxAgeUs >= 0 && lr[0].ctxAgeUs < 600000 && lr[0].reason == 0 && isAmp(),
         "amp event not correlated with the skill context / not amplified");
    setp([](AmpParams& q) { q.denyKeys = {10253}; });
    lr = fire();
    need(isOrig() && !lr.empty() && lr[0].reason == 2, "DenyKeys did not restore originals");
    setp([](AmpParams& q) { q.denyKeys.clear(); q.allowKeys = {99}; });
    lr = fire();
    need(isOrig() && !lr.empty() && lr[0].reason == 3, "AllowKeys miss did not restore originals");
    setp([](AmpParams& q) { q.allowKeys = {99, 10253}; });
    fire();
    need(isAmp(), "AllowKeys hit did not amplify again (compounding or stuck)");
    setp([](AmpParams& q) { q.allowKeys.clear(); q.ownerMode = 2; q.ownerFixed = 0x1; });
    lr = fire();
    need(isOrig() && !lr.empty() && lr[0].reason == 4, "OnlyOwner mismatch did not restore originals");
    setp([&](AmpParams& q) { q.ownerFixed = kOwner; });
    fire();
    need(isAmp(), "OnlyOwner match did not amplify");
    // auto owner (= the rolling player): blocks while unknown, allows only the player
    se.player.Reset();
    setp([](AmpParams& q) { q.ownerMode = 1; });
    lr = fire();
    need(isOrig() && !lr.empty() && lr[0].reason == 5, "OnlyOwner=auto should block until an owner is learned");
    se.player.Reset();   // the rolling player tracker forgets everything: the player is unknown until skills are seen
    lr = fire();
    need(isOrig() && !lr.empty() && lr[0].reason == 5, "OnlyOwner=auto should block while the player is unknown");
    call(se, kOwner); call(se, kOwner); call(se, kOwner); call(se, kOther);   // kOwner is the busiest a1 and becomes the player
    call(se, kOther);                                                         // the newest context is now kOther's
    uint64_t learned = 0;
    need(se.Owner(learned) && learned == kOwner, "the player is not the busiest a1");
    fire();
    need(isOrig(), "auto owner: event after a foreign-owner skill should not be amplified");
    call(se, kOwner);
    fire();
    need(isAmp(), "auto owner: event after the owner's skill should be amplified");
    // RequireCtx / no context / expired window
    setp([](AmpParams& q) { q.ownerMode = 0; q.requireCtx = true; });
    ae.SetSkills(&se2);
    lr = fire();
    need(isOrig() && !lr.empty() && !lr[0].haveCtx && lr[0].reason == 1, "RequireCtx without context should not amplify");
    setp([](AmpParams& q) { q.requireCtx = false; });
    lr = fire();
    need(isAmp() && !lr.empty() && !lr[0].haveCtx && lr[0].reason == 0, "no filters + no context should still amplify (ctx=none)");
    ae.SetSkills(&se);
    call(se, kOwner);
    setp([](AmpParams& q) { q.requireCtx = true; q.contextWindowMs = 50; });
    Sleep(80);
    lr = fire();
    need(isOrig() && !lr.empty() && !lr[0].haveCtx && lr[0].reason == 1, "expired context window not honoured");
    ae.SetEnabled(false);
    need(isOrig(), "disabling did not leave the original values");
    r.Add(ok, "skillctx", ok ? "capture contents, arg/return passthrough, bad-pointer safety, amp correlation, Deny/Allow/Owner(fixed+auto)/RequireCtx/window filters apply and restore without compounding" : why);
}

// ---- event-dispatcher tracer: private EtEngine + fake dispatcher + fake handler table (same logic as the real detour).
struct EtSeen { __int64 a1 = 0; uint64_t H = 0; uint32_t* a2 = nullptr; __int64 a3 = 0, a4 = 0; uint64_t* a5 = nullptr; int n = 0; };
EtSeen g_etSeen;
uint32_t* __fastcall FakeHandler(uint64_t H, uint32_t* a2, __int64 a3, __int64 a4, uint64_t* a5) {
    g_etSeen.H = H; g_etSeen.a2 = a2; g_etSeen.a3 = a3; g_etSeen.a4 = a4; g_etSeen.a5 = a5; ++g_etSeen.n;
    return reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(0x5EED0001));
}
// Mimics sub_1404F42B0: D = *(a3+8); type = *(u16*)D; H = *(a1 + 8*type + 16); fn = *(*H + 8); return fn(H, a2, a3, a4, a5)
uint32_t* __fastcall FakeDispatch(__int64 a1, uint32_t* a2, __int64 a3, __int64 a4, uint64_t* a5) {
    uint64_t D = *reinterpret_cast<uint64_t*>(a3 + gl::kEvDataInArgs);
    uint16_t type = *reinterpret_cast<uint16_t*>(D + gl::kEvType);
    uint64_t H = *reinterpret_cast<uint64_t*>(a1 + gl::kEvTableStride * type + gl::kEvTableBase);
    uint64_t fn = *reinterpret_cast<uint64_t*>(*reinterpret_cast<uint64_t*>(H) + gl::kHandlerFnInVtable);
    g_etSeen.a1 = a1;
    return reinterpret_cast<uint32_t*(__fastcall*)(uint64_t, uint32_t*, __int64, __int64, uint64_t*)>(fn)(H, a2, a3, a4, a5);
}
uint32_t* __fastcall FakeNoop(__int64, uint32_t*, __int64, __int64, uint64_t*) { return reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(0x77)); }

void TestEtrace(Report& r) {
    EtEngine e;
    bool ok = e.Prepare();
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    need(ok, "ring allocation failed");
    e.enabled = true;

    alignas(16) uint64_t tbl[64] = {};
    uint64_t vt[2] = {0, reinterpret_cast<uint64_t>(&FakeHandler)};
    uint64_t H3[2] = {reinterpret_cast<uint64_t>(vt), 0}, H5[2] = {reinterpret_cast<uint64_t>(vt), 0};
    tbl[gl::kEvTableBase / 8 + 3] = reinterpret_cast<uint64_t>(H3);   // a1 + kEvTableBase + kEvTableStride*3 (stride 8)
    tbl[gl::kEvTableBase / 8 + 5] = reinterpret_cast<uint64_t>(H5);
    alignas(16) uint8_t D3[128] = {}, D5[128] = {};
    uint16_t t3 = 3, t5 = 5;
    memcpy(D3, &t3, 2); memcpy(D5, &t5, 2);
    const float fl[6] = {1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f};
    memcpy(D3 + gl::kEvSampleFloats, fl, 24);
    for (size_t i = gl::kEvSampleFloats + 24; i < 96; ++i) { D3[i] = static_cast<uint8_t>(i); D5[i] = static_cast<uint8_t>(0xA0 + i); }   // after the 6 sample floats
    EvArg ev3(reinterpret_cast<uint64_t>(D3)), ev5(reinterpret_cast<uint64_t>(D5));
    uint64_t a5v = 0xDEAD5EEDull;
    uint32_t a2v = 9;
    const __int64 a1 = reinterpret_cast<__int64>(tbl);
    auto go = [&](uint64_t* ev) { return EtCallThrough(e, FakeDispatch, a1, &a2v, reinterpret_cast<__int64>(ev), 44, &a5v, 0x4242); };

    long long t0 = NowUs();
    uint32_t* rv = go(ev3);
    need(reinterpret_cast<uintptr_t>(rv) == 0x5EED0001 && g_etSeen.a1 == a1 && g_etSeen.H == reinterpret_cast<uint64_t>(H3) && g_etSeen.a2 == &a2v &&
         g_etSeen.a3 == static_cast<__int64>(ev3.Ptr()) && g_etSeen.a4 == 44 && g_etSeen.a5 == &a5v, "original not called with identical args / return value lost");
    std::vector<EtRec> tl = e.Tail(5);
    if (tl.size() != 1) need(false, "expected exactly 1 logged record");
    else {
        const EtRec& c = tl[0];
        float got[6];
        memcpy(got, c.raw + gl::kEvSampleFloats, 24);
        need(c.type == 3 && c.H == reinterpret_cast<uint64_t>(H3) && c.fn == reinterpret_cast<uint64_t>(&FakeHandler) && c.D == reinterpret_cast<uint64_t>(D3), "type / H / handler fn / D wrong");
        need(!memcmp(c.raw, D3, 96) && !memcmp(got, fl, 24), "raw D bytes or the 6 floats wrong");
        need(c.a5 == reinterpret_cast<uint64_t>(&a5v) && c.a5v == a5v && c.tid == GetCurrentThreadId() && c.ret == 0x4242 && c.t_us >= t0 && c.t_us <= NowUs(), "a5 / *a5 / tid / ret / time wrong");
    }
    // grouping by (type, fn)
    go(ev3); go(ev5); go(ev3);
    std::vector<EtGroup> gs = e.Summary(0);
    need(gs.size() == 2 && gs[0].type == 3 && gs[0].count == 3 && gs[1].type == 5 && gs[1].count == 1 && gs[0].fn == reinterpret_cast<uint64_t>(&FakeHandler), "summary grouping wrong");
    need(e.TopTypes(5).size() == 2 && e.TopTypes(5)[0].first == 3 && e.TopTypes(5)[0].second == 3, "lifetime type counts wrong");
    // time window
    e.Clear();
    go(ev3); Sleep(40); go(ev5); long long tB = e.Tail(1)[0].t_us; Sleep(40); go(ev3);
    std::vector<EtRec> w = e.Window(tB - 15000, tB + 15000);
    need(w.size() == 1 && w[0].type == 5 && e.Window(tB - 60000, tB + 60000).size() == 3, "window selection wrong");
    // exclude filter drops from the LOG only
    e.Clear();
    e.SetExcluded({3});
    int n0 = g_etSeen.n;
    uint64_t ex0 = e.ExcludedCalls();
    go(ev3); go(ev5);
    need(g_etSeen.n == n0 + 2 && e.ExcludedCalls() == ex0 + 1 && e.Logged() == 1 && e.Tail(1)[0].type == 5 && e.Excluded().size() == 1, "exclude filter wrong (must skip logging but still run the handler)");
    e.SetExcluded({});
    go(ev3);
    need(e.Logged() == 2, "clearing the filter did not resume logging");
    // bad pointers: never crash, record "unknown"
    e.Clear();
    EtCallThrough(e, FakeNoop, 0, nullptr, 8, 0, nullptr, 1);
    EtCallThrough(e, FakeNoop, 0x10, nullptr, 0, 0, reinterpret_cast<uint64_t*>(8), 1);
    std::vector<EtRec> bad = e.Tail(2);
    need(bad.size() == 2 && bad[0].type == 0xFFFFFFFFu && bad[0].D == 0 && bad[1].a5v == 0, "unreadable pointers not handled");
    // event data at the very end of readable memory: falls back to a shorter read
    uint8_t* pg = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    if (pg && VirtualAlloc(pg, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        uint8_t* d = pg + 4096 - 40;
        uint16_t t9 = 9;
        memcpy(d, &t9, 2);
        EvArg ev9(reinterpret_cast<uint64_t>(d));
        EtCallThrough(e, FakeNoop, a1, nullptr, static_cast<__int64>(ev9.Ptr()), 0, nullptr, 2);
        EtRec last = e.Tail(1)[0];
        need(last.type == 9 && last.D == reinterpret_cast<uint64_t>(d), "event data near a page end not read via the fallback");
    } else need(false, "VirtualAlloc for the page-end test failed");
    if (pg) VirtualFree(pg, 0, MEM_RELEASE);
    // disabled = pass-through
    e.enabled = false;
    uint64_t before = e.Total();
    int n1 = g_etSeen.n;
    go(ev3);
    need(e.Total() == before && g_etSeen.n == n1 + 1, "disabled engine logged or skipped the original");
    r.Add(ok, "etrace", ok ? "capture of type/H/handler fn/D/raw+floats/a5/*a5/tid/ret/time, arg+return passthrough, grouping, time window, exclude filter (log only), bad-pointer + page-end safety, disabled=pass-through" : why);
}

// ---- raw capture around the apply-skill call (fake call that WRITES to the dumped memory so before/after differ) + mem scanf + path guard
uint8_t* g_rawS = nullptr;
uint8_t* g_rawO = nullptr;
__int64 __fastcall FakeApplyRaw(uint64_t* a1, __int64 a2) {
    (void)a1; (void)a2;
    uint32_t m = 0xCAFEF00D;
    float f = 7.5f;
    memcpy(g_rawS + 0x180, &m, 4);
    memcpy(g_rawO + 0x1A0, &f, 4);
    return 0x1111;
}

void TestRaw(Report& r) {
    SkillEngine se;
    se.enabled = true;
    bool ok = se.RawPrepare();
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    need(ok, "raw ring allocation failed");
    alignas(16) static uint8_t A2[0x400], Sb[0x400], Ob[0x400], A1o[0x100];
    memset(A2, 0x11, sizeof A2); memset(Sb, 0x22, sizeof Sb); memset(Ob, 0x33, sizeof Ob); memset(A1o, 0x44, sizeof A1o);
    uint64_t pS = reinterpret_cast<uint64_t>(Sb), pO = reinterpret_cast<uint64_t>(Ob);
    uint32_t key = 10253;
    memcpy(A2 + gl::kApplyS, &pS, 8); memcpy(A2 + gl::kApplyO160, &pO, 8); memcpy(Sb + gl::kSKeys, &key, 4);
    memset(Sb + 0x180, 0, 4); memset(Ob + 0x1A0, 0, 4);
    g_rawS = Sb; g_rawO = Ob;
    const uint32_t sz[4] = {0x180, 0x200, 0x1C0, 0x40};   // a2, S, o160, a1
    se.RawConfigure(2, sz);
    auto call = [&](uint64_t* a1, uint8_t* a2) { return SkillCallThrough(se, FakeApplyRaw, a1, reinterpret_cast<__int64>(a2), 0x4242); };

    __int64 rv = call(reinterpret_cast<uint64_t*>(A1o), A2);
    need(rv == 0x1111, "return value not passed through");
    std::vector<RawHdr> hs = se.RawList(5);
    if (hs.size() != 1) need(false, "expected exactly 1 raw capture");
    else {
        const RawHdr& h = hs[0];
        need(h.key0 == 10253 && h.base[0] == reinterpret_cast<uint64_t>(A2) && h.base[1] == pS && h.base[2] == pO && h.base[3] == reinterpret_cast<uint64_t>(A1o), "header addresses / key0 wrong");
        need(h.sz[0] == 0x180 && h.sz[1] == 0x200 && h.sz[2] == 0x1C0 && h.sz[3] == 0x40 && h.validB[0] == 0x180 && h.validB[1] == 0x200 && h.hasAfter, "sizes / valid counts / after flag wrong");
        RawHdr hh;
        std::vector<uint8_t> b[8];
        std::string e;
        bool got = true;
        for (int w = 0; w < 8; ++w) got = got && se.RawGet(h.idx, w, hh, b[w], e);
        need(got, "RawGet failed for some dump");
        if (got) {
            need(!memcmp(b[0].data(), A2, 0x180) && !memcmp(b[3].data(), A1o, 0x40), "a2 / a1 dump differs from memory");
            uint32_t mb, ma;
            float fb, fa;
            memcpy(&mb, b[1].data() + 0x180, 4); memcpy(&ma, b[5].data() + 0x180, 4);
            memcpy(&fb, b[2].data() + 0x1A0, 4); memcpy(&fa, b[6].data() + 0x1A0, 4);
            need(mb == 0 && ma == 0xCAFEF00D && fb == 0.0f && fa == 7.5f, "before/after dumps do not show what the call wrote");
            need(!memcmp(b[0].data(), b[4].data(), 0x180) && memcmp(b[1].data(), b[5].data(), 0x200) != 0, "unchanged region differs or changed region identical");
            need(b[1].size() == 0x200 && b[2].size() == 0x1C0, "dump sizes wrong");
        }
    }
    // partially / fully unreadable ranges are zero-filled and reported
    uint8_t* pg = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    if (pg && VirtualAlloc(pg, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        memset(pg, 0x5A, 4096);
        alignas(16) static uint8_t A2b[0x400];
        memcpy(A2b, A2, sizeof A2b);
        uint64_t part = reinterpret_cast<uint64_t>(pg) + 4096 - 100;   // 100 readable bytes, then an unmapped page
        memcpy(A2b + gl::kApplyO160, &part, 8);
        call(reinterpret_cast<uint64_t*>(8), A2b);   // a1 = 8 is unreadable too
        std::vector<RawHdr> h2 = se.RawList(1);
        std::vector<uint8_t> d;
        RawHdr hh;
        std::string e;
        need(h2.size() == 1 && h2[0].validB[2] == 100 && h2[0].validB[3] == 0, "partial / unreadable valid counts wrong");
        if (se.RawGet(h2[0].idx, 2, hh, d, e)) need(d[99] == 0x5A && d[100] == 0 && d[0x1BF] == 0, "unreadable bytes not zero-filled");
        else need(false, "RawGet of partial dump failed");
    } else need(false, "VirtualAlloc for the partial-read test failed");
    if (pg) VirtualFree(pg, 0, MEM_RELEASE);
    // mode 1 = before only
    se.RawConfigure(1, sz);
    call(reinterpret_cast<uint64_t*>(A1o), A2);
    {
        std::vector<RawHdr> h3 = se.RawList(1);
        RawHdr hh;
        std::vector<uint8_t> d;
        std::string e;
        need(h3.size() == 1 && !h3[0].hasAfter && !se.RawGet(h3[0].idx, 5, hh, d, e), "mode `on` must not produce after-dumps");
    }
    // sizes are capped at 4096; mode off captures nothing but the call still runs
    const uint32_t big[4] = {99999, 5000, 4097, 0};
    se.RawConfigure(1, big);
    uint32_t got4[4];
    se.RawSizes(got4);
    need(got4[0] == 4096 && got4[1] == 4096 && got4[2] == 4096 && got4[3] == 0, "sizes not capped at 4096");
    se.RawConfigure(0, sz);
    uint64_t cnt = se.RawCount();
    need(call(reinterpret_cast<uint64_t*>(A1o), A2) == 0x1111 && se.RawCount() == cnt, "raw off still captured or broke the call");
    // ring wrap: only the newest 128 survive, in order
    se.RawConfigure(1, sz);
    for (int i = 0; i < 140; ++i) call(reinterpret_cast<uint64_t*>(A1o), A2);
    {
        std::vector<RawHdr> all = se.RawList(500);
        RawHdr hh;
        std::vector<uint8_t> d;
        std::string e;
        need(all.size() == 128 && all.back().idx == se.RawCount() && all.front().idx == se.RawCount() - 127, "ring wrap kept the wrong captures");
        need(!se.RawGet(se.RawCount() - 128, 0, hh, d, e), "an overwritten capture was still readable");
        // serialized image must be self-consistent
        std::vector<uint8_t> img;
        se.RawSerialize(img);
        auto rd32 = [&](size_t o) { uint32_t v; memcpy(&v, img.data() + o, 4); return v; };
        bool fmt = img.size() > 24 && !memcmp(img.data(), "CMRAW001", 8) && rd32(8) == 1 && rd32(12) == 128 && rd32(16) == 112;
        size_t off = 24;
        uint32_t seen = 0;
        while (fmt && off + 112 <= img.size() && seen < 128) {
            uint64_t idx;
            memcpy(&idx, img.data() + off, 8);
            uint32_t s0 = rd32(off + 56), s1 = rd32(off + 60), s2 = rd32(off + 64), s3 = rd32(off + 68), has = rd32(off + 104);
            size_t pay = static_cast<size_t>(s0) + s1 + s2 + s3;
            if (idx != all[seen].idx) fmt = false;
            off += 112 + pay * (has ? 2 : 1);
            ++seen;
        }
        need(fmt && seen == 128 && off == img.size(), "rawsave file image inconsistent");
    }
    // rawsave path guard (lexical part; the real command also resolves junctions)
    {   // the root is derived from the plugin's own folder (no compiled-in machine path): the nearest ancestor called "CombatMod", else the .asi folder
        const std::wstring root = DevOutputRoot();   // with trailing backslash
        std::wstring parent = root.substr(0, root.size() - 1);
        parent = parent.substr(0, parent.find_last_of(L'\\') + 1);
        need(root.size() > 3 && root.back() == L'\\' && !root.empty(), "DevOutputRoot");
        need(RawPathLexicalOk(root + L"build\\x.bin") && !RawPathLexicalOk(parent + L"evil.bin") && !RawPathLexicalOk(root.substr(0, root.size() - 1) + L"X\\a.bin") && !RawPathLexicalOk(L"C:\\Windows\\a.bin") &&
             !RawPathLexicalOk(root + L"a.bin:stream") && !RawPathLexicalOk(root) && !RawPathLexicalOk(L"Z:\\nowhere\\a.bin"), "rawsave path guard wrong");
        wchar_t norm[MAX_PATH];
        GetFullPathNameW((root + L"..\\evil.bin").c_str(), MAX_PATH, norm, nullptr);
        need(!RawPathLexicalOk(norm), "'..' escape not normalised away");
    }
    // mem scanf
    alignas(16) float fv[24] = {};
    fv[4] = 0; fv[5] = 0; fv[6] = 5.0f;         // |v| = 5 at +16
    fv[10] = 3.0f; fv[11] = 4.0f; fv[12] = 0;    // |v| = 5 at +40 (also overlaps with a smaller triple)
    fv[20] = 100.0f; fv[21] = 0; fv[22] = 0;     // |v| = 100 outside the range
    char a[32], len[16];
    snprintf(a, sizeof a, "%llX", U64(reinterpret_cast<uintptr_t>(fv)));
    snprintf(len, sizeof len, "%zu", sizeof fv);
    std::string out = CmdMemScanF(Args{"mem", "scanf", a, len, "4.9", "5.1"});
    need(out.find("+0x10 ") != std::string::npos && out.find("+0x28 ") != std::string::npos && out.find("(0, 0, 5)") != std::string::npos && out.find("100") == std::string::npos &&
         out.find("OK") != std::string::npos, "mem scanf missed or mis-reported vectors");
    need(CmdMemScanF(Args{"mem", "scanf", a, len, "5", "1"}).compare(0, 3, "ERR") == 0, "mem scanf accepted min > max");
    r.Add(ok, "rawcapture", ok ? "before/after dumps show what the call wrote, zero-filled unreadable bytes, mode on/both/off, 4096 cap, 128-slot wrap, file image consistent, path guard, mem scanf" : why);
}

// ---- runtime patch rules: fake apply function that records the values it SEES at call time.
struct PSeen { float a2f = 0, sf = 0, of = 0; uint32_t su = 0, a1u = 0; };
PSeen g_pSeen;
uint8_t* g_pA2 = nullptr;
uint8_t* g_pS = nullptr;
uint8_t* g_pO = nullptr;
bool g_pGameChanges = false;
__int64 __fastcall FakePatchApply(uint64_t* a1, __int64 a2) {
    (void)a2;
    memcpy(&g_pSeen.a2f, g_pA2 + 0xF8, 4);
    memcpy(&g_pSeen.sf, g_pS + 0x70, 4);
    memcpy(&g_pSeen.su, g_pS + 0x80, 4);
    memcpy(&g_pSeen.of, g_pO + 0x20, 4);
    memcpy(&g_pSeen.a1u, reinterpret_cast<uint8_t*>(a1) + 0x10, 4);
    if (g_pGameChanges) { float f = 1.5f; memcpy(g_pS + 0x70, &f, 4); }   // the game itself writes the field during the call
    return 0x2222;
}

void TestPatch(Report& r) {
    SkillEngine se;
    se.enabled = true;
    alignas(16) static uint8_t A2[0x400], S[0x400], O[0x400];
    alignas(16) static uint8_t A1x[0x100], A1y[0x100];
    g_pA2 = A2; g_pS = S; g_pO = O;
    uint64_t pS = reinterpret_cast<uint64_t>(S), pO = reinterpret_cast<uint64_t>(O);
    memcpy(A2 + gl::kApplyS, &pS, 8); memcpy(A2 + gl::kApplyO160, &pO, 8);
    const uint64_t kP = 0x1000, kQ = 0x2000;   // fake owner ids (a1 is only dereferenced via the real buffers below)
    auto prep = [&] {
        float a = -9.8f, s = 6.0f, o = 1.5f;
        uint32_t keys[4] = {15033, 1, 2, 3}, u = 10, x = 7;
        memcpy(A2 + 0xF8, &a, 4); memcpy(S + 0x70, &s, 4); memcpy(S + 0x80, &u, 4); memcpy(S + gl::kSKeys, keys, 16); memcpy(O + 0x20, &o, 4);
        memcpy(A1x + 0x10, &x, 4); memcpy(A1y + 0x10, &x, 4);
        g_pGameChanges = false;
    };
    auto F32 = [](const uint8_t* p) { float f; memcpy(&f, p, 4); return f; };
    auto U32 = [](const uint8_t* p) { uint32_t u; memcpy(&u, p, 4); return u; };
    // owner ids are matched against a1; use the real buffers' addresses as owner ids
    const uint64_t oX = reinterpret_cast<uint64_t>(A1x), oY = reinterpret_cast<uint64_t>(A1y);
    (void)kP; (void)kQ;
    auto call = [&](uint64_t a1v) { return SkillCallThrough(se, FakePatchApply, reinterpret_cast<uint64_t*>(a1v), reinterpret_cast<__int64>(A2), 0x4242); };
    std::string lastErr;
    auto add = [&](std::initializer_list<const char*> toks) -> uint32_t {
        Args a{"skills", "patch", "add"};
        for (const char* tk : toks) a.push_back(tk);
        PatchRule pr;
        if (!ParsePatchRule(a, 3, pr, lastErr)) return 0;
        uint32_t id = 0;
        if (!se.PatchAdd(pr, id, lastErr)) return 0;
        return id;
    };

    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };

    // 1. a2 mul (left patched: a2 is a temporary struct) + 2. S set (restored)
    prep();
    uint32_t r1 = add({"k0=15033", "a2", "F8", "f32", "mul", "2.0"});
    uint32_t r2 = add({"*", "S", "70", "f32", "set", "99.5"});
    need(r1 && r2, "valid rules refused");
    need(call(oX) == 0x2222, "return value lost");
    need(g_pSeen.a2f == -9.8f * 2.0f && g_pSeen.sf == 99.5f, "original did not see the patched a2 / S values");
    need(F32(A2 + 0xF8) == -9.8f * 2.0f, "a2 should stay patched (no restore for the stack struct)");
    need(F32(S + 0x70) == 6.0f, "S was not restored after the call");
    for (int i = 0; i < 3; ++i) { prep(); call(oX); need(g_pSeen.sf == 99.5f && F32(S + 0x70) == 6.0f && g_pSeen.a2f == -9.8f * 2.0f, "repeated calls compound or leave S modified"); }
    // 3. u32 add on S with an `any=` key rule
    se.PatchClear();
    uint32_t r3 = add({"any=2", "S", "80", "u32", "add", "5"});
    prep(); call(oX);
    need(r3 && g_pSeen.su == 15 && U32(S + 0x80) == 10, "u32 add on S (any= key slot match) / restore wrong");
    // 4. two rules on the same field apply in order and restore to the true original
    se.PatchClear();
    add({"*", "S", "70", "f32", "mul", "2"});
    add({"*", "S", "70", "f32", "add", "1"});
    prep(); call(oX);
    need(g_pSeen.sf == 13.0f && F32(S + 0x70) == 6.0f, "stacked rules on one field wrong / not restored to the original");
    // 5. o160 and a1 regions are patched and restored too
    se.PatchClear();
    add({"*", "o160", "20", "f32", "mul", "4"});
    add({"*", "a1", "10", "u32", "set", "0x55"});
    prep(); call(oX);
    need(g_pSeen.of == 6.0f && g_pSeen.a1u == 0x55 && F32(O + 0x20) == 1.5f && U32(A1x + 0x10) == 7, "o160 / a1 region patch or restore wrong");
    // 6. key filters
    se.PatchClear();
    add({"k0=15033", "S", "70", "f32", "set", "50"});
    prep(); { uint32_t k = 1002; memcpy(S + gl::kSKeys, &k, 4); } call(oX);
    need(g_pSeen.sf == 6.0f, "k0 rule applied to a different key0");
    prep(); call(oX);
    need(g_pSeen.sf == 50.0f, "k0 rule did not apply to its key");
    se.PatchClear();
    add({"k0=2", "S", "70", "f32", "set", "50"});   // 2 is key slot 2, NOT key0
    prep(); call(oX);
    need(g_pSeen.sf == 6.0f, "k0= must only look at key0");
    // 7. owner filters: fixed, player (most calls), player (learned)
    se.PatchClear();
    char oxs[32], oys[32];
    snprintf(oxs, sizeof oxs, "owner=%llX", U64(oX));
    snprintf(oys, sizeof oys, "owner=%llX", U64(oY));
    add({"*", "S", "70", "f32", "set", "50", oxs});
    prep(); call(oY);
    need(g_pSeen.sf == 6.0f, "fixed-owner rule applied to another owner");
    prep(); call(oX);
    need(g_pSeen.sf == 50.0f, "fixed-owner rule did not apply to its owner");
    se.PatchClear();
    for (int i = 0; i < 6; ++i) call(oX);   // oX becomes the busiest a1
    call(oY);
    add({"*", "S", "70", "f32", "set", "50", "owner=player"});
    prep(); call(oY);
    need(g_pSeen.sf == 6.0f, "owner=player applied to a non-busiest a1");
    prep(); call(oX);
    need(g_pSeen.sf == 50.0f, "owner=player did not apply to the busiest a1");
    se.player.Reset();
    call(oY);   // after a reset the first caller becomes the player
    prep(); call(oX);
    need(g_pSeen.sf == 6.0f, "the player identity change was not followed (oX is no longer the player)");
    prep(); call(oY);
    need(g_pSeen.sf == 50.0f, "owner=player did not follow the new player");
    // 8. off / on / del / clear
    se.patchOn = false;
    prep(); call(oY);
    need(g_pSeen.sf == 6.0f, "patching=off still applied a rule");
    se.patchOn = true;
    prep(); call(oY);
    need(g_pSeen.sf == 50.0f, "patching=on did not resume");
    auto lst = se.PatchList();
    need(lst.size() == 1 && lst[0].second >= 1, "rule list / hit counter wrong");
    need(se.PatchDel(lst[0].first.id) && !se.PatchDel(lst[0].first.id) && se.PatchRuleCount() == 0, "delete wrong");
    prep(); call(oY);
    need(g_pSeen.sf == 6.0f, "deleted rule still applied");
    // 9. bad rules refused
    need(!add({"*", "S", "1000", "f32", "set", "1"}) && !add({"*", "S", "FFD", "f32", "set", "1"}) && add({"*", "S", "FFC", "u32", "set", "1"}), "offset bounds wrong (0xFFC is the last valid)");
    se.PatchClear();
    need(!add({"*", "S", "70", "f32", "set", "inf"}) && !add({"*", "S", "70", "f32", "set", "nan"}), "non-finite f32 value accepted");
    need(!add({"*", "zz", "70", "f32", "set", "1"}) && !add({"*", "S", "70", "f64", "set", "1"}) && !add({"*", "S", "70", "f32", "div", "1"}) && !add({"5", "S", "70", "f32", "set", "1"}) &&
         !add({"k0=0", "S", "70", "f32", "set", "1"}) && !add({"k0=1,2,3,4,5,6,7,8,9", "S", "70", "f32", "set", "1"}) && !add({"*", "S", "70", "u32", "set", "0x1FFFFFFFF"}) &&
         !add({"*", "S", "70", "f32", "set"}) && !add({"*", "S", "70", "f32", "set", "1", "owner=zz"}), "malformed rules accepted");
    // 10. rule cap
    int added = 0;
    for (int i = 0; i < 40; ++i) if (add({"*", "S", "70", "f32", "set", "1"})) ++added;
    need(added == 32 && se.PatchRuleCount() == 32, "rule cap is not 32");
    se.PatchClear();
    need(se.PatchRuleCount() == 0, "clear left rules behind");
    // 11. non-finite results are ignored
    add({"*", "a2", "F8", "f32", "mul", "100"});
    prep(); { float big = 3e38f; memcpy(A2 + 0xF8, &big, 4); } call(oX);
    need(g_pSeen.a2f == 3e38f, "an f32 rule produced inf and was applied");
    se.PatchClear();
    // 12. the game changing the field during the call: restore must not clobber it
    add({"*", "S", "70", "f32", "set", "99.5"});
    prep(); g_pGameChanges = true;
    uint64_t nr0 = se.PatchNotRestored();
    call(oX);
    need(g_pSeen.sf == 99.5f && F32(S + 0x70) == 1.5f && se.PatchNotRestored() == nr0 + 1, "restore clobbered a value the game wrote during the call");
    g_pGameChanges = false;
    // 13. patch log content
    prep();
    call(oX);
    std::vector<PatchLogRec> pl = se.PatchTail(1);
    need(pl.size() == 1 && pl[0].region == 1 && pl[0].offset == 0x70 && pl[0].isFloat && pl[0].key0 == 15033 && std::bit_cast<float>(pl[0].before) == 6.0f &&
         std::bit_cast<float>(pl[0].after) == 99.5f && pl[0].tid == GetCurrentThreadId(), "patch log record wrong");
    // 14. raw capture is taken BEFORE patching and the after-dump AFTER the restore: both show the original
    uint32_t sz[4] = {0x100, 0x200, 0x40, 0x40};
    se.RawPrepare();
    se.RawConfigure(2, sz);
    prep();
    call(oX);
    {
        std::vector<RawHdr> hs = se.RawList(1);
        RawHdr hh;
        std::vector<uint8_t> b, a;
        std::string e;
        bool got = hs.size() == 1 && se.RawGet(hs[0].idx, 1, hh, b, e) && se.RawGet(hs[0].idx, 5, hh, a, e);
        need(got && F32(b.data() + 0x70) == 6.0f && F32(a.data() + 0x70) == 6.0f && g_pSeen.sf == 99.5f, "raw dumps must show the original values while the call saw the patched ones");
    }
    se.RawConfigure(0, sz);
    // 15. unreadable a2 never crashes
    __int64 rv = SkillCallThrough(se, FakePatchApply, reinterpret_cast<uint64_t*>(A1x), 8, 1);
    need(rv == 0x2222, "bad a2 broke the call");
    se.PatchClear();
    r.Add(ok, "patch", ok ? "mul/set/add f32+u32 on a2 (stays) and S/o160/a1 (restored), stacked rules, k0/any key filters, fixed/player/learned owner, on/off/del/clear, bounds + malformed refusal, 32 cap, non-finite ignored, game-written value not clobbered, log, raw sees originals" : why);
}

// ---- settings table: defaults, validation, list parsing, presets, ini round trip that keeps the other sections, batching
int g_cfgCalls = 0;
void CountingHandler() { ++g_cfgCalls; }

void TestSettings(Report& r) {
    CfgSnapshot snap = CfgTake();   // the real settings must survive the selftest untouched
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    std::string e;
    {
        CfgNotifyOff quiet;   // no live application (hooks!) while the table is exercised
        CfgResetAll();
        need(CfgGet(SlowMo_ScaleMul) == 1.0f && !CfgBool(SlowMo_Enabled) && !CfgBool(Launch_Enabled) && CfgGet(Launch_Mult) == 1.0f && CfgGet(SlowMo_RampInMul) == 1.0f && CfgGet(HitStop_DurMul) == 1.0f &&
             !CfgBool(HitStop_Enabled) && CfgGet(HitStop_ScaleMul) == 1.0f && CfgDefOf(HitStop_DurMul).min == 1.0f && CfgDefOf(HitStop_DurMul).max == 10.0f, "defaults wrong");
        // the simplified panel: these are fixed internal values now, not settings (old keys are recognised and refused / ignored)
        need(CfgFind("SlowMo.MinScale") == -1 && CfgFind("SlowMo.IgnoreAbove") == -1 && CfgFind("SlowMo.PlayerOnly") == -1 && CfgFind("Launch.PlayerOnly") == -1 && CfgFind("Launch.MaxValue") == -1 &&
             CfgIsLegacyKey("slowmo.minscale") && CfgIsLegacyKey("Launch.MaxValue") && !CfgIsLegacyKey("Launch.Mult"), "removed settings must not exist as settings");
        need(!CfgSetStr("Launch.MaxValue", "50", e) && e.find("removed") != std::string::npos && !CfgSetStr("SlowMo.PlayerOnly", "0", e), "cfg set of a removed key must be refused with an explanation");
        need(CfgDefOf(SlowMo_DenyKeys).advanced && !CfgDefOf(SlowMo_ScaleMul).advanced, "DenyKeys is an [Advanced] (not panel) setting");
        std::vector<uint32_t> dk = CfgList(SlowMo_DenyKeys), want = {10010, 10031, 10072, 10278, 10279, 10086};
        need(dk == want && CfgListHas(SlowMo_DenyKeys, 10072) && !CfgListHas(SlowMo_DenyKeys, 1021), "default DenyKeys wrong");
        need(CfgFind("slowmo.SCALEMUL") == SlowMo_ScaleMul && CfgFind("nope") == -1, "key lookup wrong");
        need(CfgSetStr("SlowMo.ScaleMul", "0.5", e) && CfgGet(SlowMo_ScaleMul) == 0.5f && CfgDirty(), "valid set rejected / dirty flag not set");
        need(!CfgSetStr("SlowMo.ScaleMul", "3", e) && !CfgSetStr("SlowMo.ScaleMul", "0.01", e) && !CfgSetStr("SlowMo.ScaleMul", "abc", e) && !CfgSetStr("SlowMo.ScaleMul", "nan", e) &&
             CfgGet(SlowMo_ScaleMul) == 0.5f, "out-of-range / malformed values accepted or changed the value");
        need(!CfgSetStr("Launch.Mult", "21", e) && !CfgSetStr("Launch.Mult", "0.4", e) && CfgSetStr("Launch.Mult", "20", e) && CfgSetStr("Launch.Mult", "0.5", e), "range edges wrong");
        need(CfgSetStr("HitStop.Enabled", "on", e) && CfgBool(HitStop_Enabled) && CfgSetStr("HitStop.Enabled", "FALSE", e) && !CfgBool(HitStop_Enabled) && !CfgSetStr("HitStop.Enabled", "maybe", e), "bool parsing wrong");
        need(!CfgSetStr("HitStop.DurMul", "0.5", e) && !CfgSetStr("HitStop.DurMul", "11", e) && CfgSetStr("HitStop.DurMul", "10", e) && CfgSetStr("HitStop.DurMul", "1", e), "HitStop.DurMul range 1..10");
        need(CfgSetStr("SlowMo.DenyKeys", "1021, 7,7,9", e) && CfgList(SlowMo_DenyKeys) == std::vector<uint32_t>({1021, 7, 9}) && !CfgSetStr("SlowMo.DenyKeys", "1,0", e) &&
             !CfgSetStr("SlowMo.DenyKeys", "x", e) && CfgSetStr("SlowMo.DenyKeys", "none", e) && CfgList(SlowMo_DenyKeys).empty(), "list parsing wrong");
        std::string many;
        for (int i = 1; i <= 33; ++i) many += (i > 1 ? "," : "") + std::to_string(i);
        need(!CfgSetStr("SlowMo.DenyKeys", many, e), "33 keys accepted");
        need(!CfgSetStr("No.Such", "1", e), "unknown key accepted");
        // presets
        need(CfgPreset("cinematic", e) && CfgBool(SlowMo_Enabled) && CfgGet(SlowMo_ScaleMul) == 0.5f && CfgGet(SlowMo_RampInMul) == 1.6f && CfgGet(SlowMo_RampOutMul) == 1.6f && CfgGet(Launch_Mult) == 2.0f &&
             CfgBool(Launch_Enabled) && CfgList(SlowMo_DenyKeys).empty(), "cinematic preset wrong (or it reset the [Advanced] DenyKeys we had set to none: presets keep [Advanced] tuning)");
        need(CfgPreset("EXTREME", e) && CfgGet(SlowMo_ScaleMul) == 0.1f && CfgGet(SlowMo_RampInMul) == 3.0f && CfgGet(Launch_Mult) == 10.0f, "extreme preset wrong");
        CfgPreset("cinematic", e);
        need(CfgPreset("heavy", e) && CfgBool(HitStop_Enabled) && CfgGet(HitStop_ScaleMul) == 0.15f && CfgGet(HitStop_DurMul) == 5.0f && CfgBool(SlowMo_Enabled) && CfgGet(SlowMo_ScaleMul) == 0.5f && CfgBool(Launch_Enabled) &&
             CfgGet(Launch_Mult) == 2.0f && CfgPreset("heavy-hits", e), "the heavy-hits preset sets only the hit-stop values and leaves slow motion / launch as they are");
        need(CfgPreset("vanilla", e) && !CfgBool(SlowMo_Enabled) && !CfgBool(Launch_Enabled) && CfgGet(SlowMo_ScaleMul) == 1.0f && CfgGet(Launch_Mult) == 1.0f && !CfgPreset("nope", e), "vanilla preset / unknown preset wrong");
        // ini round trip: other sections and comments survive, exactly one [Cinematic] block
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring path = std::wstring(tmp) + L"combatmod_selftest.ini";
        {
            std::ofstream f(path, std::ios::trunc);
            f << "; my comment\n[General]\nEnabled=1\nBridgePort=6400\n\n[TimeAmp]\nScaleMul=0.7\n";
        }
        need(CfgPreset("cinematic", e) && CfgSetStr("SlowMo.DenyKeys", "42,43", e) && CfgSave(path, e) && !CfgDirty(), "save failed");
        need(CfgSave(path, e), "second save failed");
        std::string txt;
        { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); txt = ss.str(); }
        size_t blocks = 0, pos = 0;
        while ((pos = txt.find("[Cinematic]", pos)) != std::string::npos) { ++blocks; ++pos; }
        need(blocks == 1 && txt.find("; my comment") != std::string::npos && txt.find("[General]\nEnabled=1\nBridgePort=6400") != std::string::npos && txt.find("[TimeAmp]\nScaleMul=0.7") != std::string::npos &&
             txt.find("SlowMo.ScaleMul=0.5") != std::string::npos && txt.find("SlowMo.DenyKeys=42,43") != std::string::npos, "saved file lost another section or duplicated [Cinematic]");
        {   // DenyKeys lives in [Advanced], everything else in [Cinematic]; no removed key is ever written
            size_t adv = txt.find("\n[Advanced]\n"), den = txt.find("SlowMo.DenyKeys=42,43"), cin = txt.find("[Cinematic]");
            need(adv != std::string::npos && den > adv && cin < adv && txt.find("\n[Advanced]\n", adv + 1) == std::string::npos && txt.find("SlowMo.PlayerOnly") == std::string::npos && txt.find("Launch.MaxValue") == std::string::npos &&
                 txt.find("MinScale") == std::string::npos && txt.find("IgnoreAbove") == std::string::npos && txt.find("HitStop.DurMul=1") != std::string::npos, "[Advanced] / [Cinematic] layout of the saved ini");
        }
        CfgResetAll();
        need(CfgLoad(path, e) && CfgGet(SlowMo_ScaleMul) == 0.5f && CfgBool(Launch_Enabled) && CfgList(SlowMo_DenyKeys) == std::vector<uint32_t>({42, 43}) && !CfgDirty(), "load did not restore the saved values");
        { std::ofstream f(path, std::ios::app); f << "SlowMo.PlayerOnly=0\nLaunch.MaxValue=5\nSlowMo.ScaleMul=9\nBogus.Key=1\n"; }   // legacy keys are ignored (logged), invalid ones too   // invalid entries after the block are ignored, the rest still loads
        CfgSetStr("SlowMo.ScaleMul", "0.25", e);
        CfgLoad(path, e);
        need(CfgGet(SlowMo_ScaleMul) == 0.5f || CfgGet(SlowMo_ScaleMul) == 0.25f, "bad ini value corrupted the setting");
        DeleteFileW(path.c_str());
        need(!CfgLoad(path, e), "loading a missing file should fail");
        {   // an old ini (milestone A/B) with DenyKeys in [Cinematic] and the removed keys still loads cleanly
            std::ofstream f(path, std::ios::trunc);
            f << "[Cinematic]\nSlowMo.Enabled=1\nSlowMo.MinScale=0.5\nSlowMo.IgnoreAbove=0.5\nSlowMo.PlayerOnly=0\nLaunch.PlayerOnly=0\nLaunch.MaxValue=5\nSlowMo.DenyKeys=77\nLaunch.Mult=3\n";
        }
        CfgResetAll();
        need(CfgLoad(path, e) && CfgBool(SlowMo_Enabled) && CfgGet(Launch_Mult) == 3.0f && CfgList(SlowMo_DenyKeys) == std::vector<uint32_t>({77}) && e.find("ignored") == std::string::npos, "old ini with removed keys must load, ignoring them quietly");
        DeleteFileW(path.c_str());
    }
    // batching: one notification per batch, none while silenced
    CfgHandler prev = CfgSetChangeHandler(&CountingHandler);
    g_cfgCalls = 0;
    {
        CfgBatch b;
        CfgSetStr("Launch.Mult", "2", e); CfgSetStr("Launch.MaxValue", "50", e); CfgSetStr("SlowMo.ScaleMul", "0.4", e);
        need(g_cfgCalls == 0, "notified inside a batch");
    }
    need(g_cfgCalls == 1, "a batch should notify exactly once");
    CfgSetStr("Launch.Mult", "3", e);
    need(g_cfgCalls == 2, "single set should notify");
    CfgSetStr("Launch.Mult", "999", e);
    need(g_cfgCalls == 2, "a rejected set must not notify");
    { CfgNotifyOff quiet; CfgSetStr("Launch.Mult", "4", e); }
    need(g_cfgCalls == 2, "notified while silenced");
    CfgSetChangeHandler(prev);
    CfgRestore(snap);
    r.Add(ok, "settings", ok ? "defaults, case-insensitive keys, range/bool/list validation, presets, ini save/load keeps other sections (one [Cinematic] block), batching + notification rules" : why);
}

// ---- rolling player identity (synthetic clock) + PlayerOnly in the amplifier
void TestPlayer(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    PlayerTracker pt;
    pt.bucketUs = 1000000; pt.evalUs = 1000000;   // 12 x 1 s window
    const uint64_t A = 0xA000, B = 0xB000, C = 0xC000;
    long long t = 100000000;
    need(pt.Current() == 0, "a fresh tracker must not know a player");
    pt.Note(A, t);
    need(pt.Current() == A, "the first caller should become the player immediately");
    for (int i = 0; i < 9; ++i) pt.Note(A, t + i * 1000);     // A: 10 calls
    for (int i = 0; i < 5; ++i) pt.Note(B, t + 10000 + i);    // B: 5 calls
    pt.Evaluate(t + 2000000);
    need(pt.Current() == A, "A (10 calls) must stay the player against B (5)");
    for (int i = 0; i < 7; ++i) pt.Note(B, t + 3000000 + i);  // B: 12 calls = 1.2x A, below the 1.25x challenge ratio
    pt.Evaluate(t + 4000000);
    need(pt.Current() == A, "hysteresis: 1.2x is not enough to take over");
    pt.Note(B, t + 5000000); pt.Note(B, t + 5000001);         // B: 14 calls (>= 1.25 x 10 and >= 10 + 3)
    pt.Evaluate(t + 6000000);
    need(pt.Current() == B && pt.Switches() == 2, "a clear leader should take over");
    for (int i = 0; i < 3; ++i) pt.Note(A, t + 7000000 + i); // A: 13 vs B: 14 -> no flapping back
    pt.Evaluate(t + 8000000);
    need(pt.Current() == B, "identity flapped back without a clear lead");
    std::vector<PlayerTracker::Entry> c = pt.Counts(t + 8000000);
    need(c.size() == 2 && c[0].a1 == B && c[0].count == 14 && c[1].a1 == A && c[1].count == 13, "window counts wrong");
    // the window slides: after 13 s without calls nobody is inside it
    pt.Evaluate(t + 25000000);
    need(pt.Current() == 0 && pt.Counts(t + 25000000).empty(), "old calls did not expire from the window");
    pt.Note(C, t + 25000100);
    need(pt.Current() == C, "a new caller after silence should become the player");
    pt.Reset();
    need(pt.Current() == 0 && pt.Counts(t + 25000200).empty(), "reset did not clear the tracker");
    // old calls of the previous player age out while a newcomer keeps playing
    PlayerTracker p2;
    p2.bucketUs = 1000000; p2.evalUs = 1000000;
    for (int i = 0; i < 20; ++i) p2.Note(A, t + i * 100);
    for (int s = 13; s < 20; ++s) for (int i = 0; i < 3; ++i) p2.Note(B, t + s * 1000000LL + i);
    p2.Evaluate(t + 19500000);
    need(p2.Current() == B, "the previous player's expired calls still counted");

    // PlayerOnly in the amplifier: unknown player -> nothing; player's skill -> amplified; another owner's skill -> restored
    SkillEngine se;
    se.enabled = true;
    alignas(16) static uint8_t Sb[0x200], A2[0x200];
    uint64_t pS = reinterpret_cast<uint64_t>(Sb);
    memcpy(A2 + gl::kApplyS, &pS, 8);
    uint32_t key = 10253;
    memcpy(Sb + gl::kSKeys, &key, 4);
    auto call = [&](uint64_t a1) { return SkillCallThrough(se, FakeApply, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<__int64>(A2), 1); };
    AmpEngine ae;
    ae.logNew = false;
    ae.SetSkills(&se);
    AmpParams ap;
    ap.scaleMul = 0.5f; ap.playerOnly = true;
    ae.SetParams(ap);
    ae.SetEnabled(true);
    EvBlk D(0.6f, 0.3f, 0.5f, 0.2f);
    EvArg ev(D.Ptr());
    ae.OnEvent(ev.Ptr(), 1);
    std::vector<AmpLogRec> lg = ae.Tail(1);
    need(FEq(D.Scale(), 0.6f) && !lg.empty() && lg[0].reason == 5, "PlayerOnly with an unknown player must not amplify");
    call(A); call(A); call(A); call(B);   // A is the player; the newest context is B's
    ae.OnEvent(ev.Ptr(), 1);
    lg = ae.Tail(1);
    need(FEq(D.Scale(), 0.6f) && !lg.empty() && lg[0].reason == 4, "an event after another owner's skill must not be amplified");
    call(A);
    ae.OnEvent(ev.Ptr(), 1);
    lg = ae.Tail(1);
    need(FEq(D.Scale(), 0.3f) && !lg.empty() && lg[0].reason == 0, "an event after the player's skill must be amplified");
    ae.SetEnabled(false);
    need(FEq(D.Scale(), 0.6f), "disabling did not restore");
    r.Add(ok, "player", ok ? "rolling window: immediate first player, 1.25x hysteresis, no flapping, expiry of old calls, reset, the built-in player-only rule (unknown -> nothing, other owner -> restored, player -> amplified)" : why);
}

// ---- launch boost with a fake S record
float g_lbSeen[2];
__int64 __fastcall FakeLaunchApply(uint64_t* a1, __int64 a2) {
    (void)a1;
    uint64_t S;
    memcpy(&S, reinterpret_cast<uint8_t*>(a2) + gl::kApplyS, 8);
    memcpy(g_lbSeen, reinterpret_cast<uint8_t*>(S) + gl::kSStr0, 8);
    return 0x3333;
}

void TestLaunch(Report& r) {
    SkillEngine se;
    LaunchEngine le;
    LaunchCfg cfg;
    cfg.enabled = true; cfg.playerOnly = false; cfg.mult = 2.0f; cfg.maxValue = 120.0f;
    se.enabled = true;
    se.launch = &le;
    se.launchCfg = &cfg;
    alignas(16) static uint8_t S[0x400], A2[0x400];
    uint64_t pS = reinterpret_cast<uint64_t>(S);
    memcpy(A2 + gl::kApplyS, &pS, 8);
    auto setf = [&](size_t off, float f) { memcpy(S + off, &f, 4); };
    auto getf = [&](size_t off) { float f; memcpy(&f, S + off, 4); return f; };
    auto setu = [&](size_t off, uint32_t u) { memcpy(S + off, &u, 4); };
    auto prep = [&](float a, float b) {
        memset(S, 0, sizeof S);
        for (size_t o : {gl::kSMark0, gl::kSMark1, gl::kSMark2}) setu(o, 0x7F7FFFFFu);
        setf(gl::kSStr0, a); setf(gl::kSStr1, b);
        uint32_t keys[4] = {1021, 0, 7, 0};
        memcpy(S + gl::kSKeys, keys, 16);
    };
    const uint64_t kP = 0x1000, kQ = 0x2000;
    auto call = [&](uint64_t a1v) { return SkillCallThrough(se, FakeLaunchApply, reinterpret_cast<uint64_t*>(a1v), reinterpret_cast<__int64>(A2), 1); };
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };

    // 1. idempotent persistent boost, derived from the stored originals
    prep(15.0f, 15.0f);
    for (int i = 0; i < 5; ++i) call(kP);
    need(getf(gl::kSStr0) == 30.0f && getf(gl::kSStr1) == 30.0f && g_lbSeen[0] == 30.0f, "boost not applied before the original, or compounded over repeated calls");
    cfg.mult = 3.0f;
    call(kP);
    need(getf(gl::kSStr0) == 45.0f, "changing Mult must re-derive from the ORIGINAL (15 x 3), not from the boosted value");
    // 2. cap: MaxValue clamps, never below the original when Mult >= 1
    cfg.mult = 8.0f; call(kP);
    need(getf(gl::kSStr0) == 120.0f, "MaxValue clamp wrong");
    cfg.maxValue = 100.0f; call(kP);
    need(getf(gl::kSStr1) == 100.0f, "MaxValue change not applied");
    prep(150.0f, 150.0f); le.Clear();
    cfg.mult = 1.0f; cfg.maxValue = 120.0f; call(kP);
    need(getf(gl::kSStr0) == 150.0f, "a cap below the original must not LOWER a Mult=1 record");
    cfg.mult = 0.5f; call(kP);
    need(getf(gl::kSStr0) == 75.0f, "Mult < 1 should reduce");
    // 3. disabling restores, enabling re-applies
    prep(15.0f, 15.0f); le.Clear();
    cfg.mult = 2.0f; call(kP);
    need(getf(gl::kSStr0) == 30.0f, "boost not re-applied after clear");
    cfg.enabled = false; call(kP);
    need(getf(gl::kSStr0) == 15.0f && getf(gl::kSStr1) == 15.0f, "disabling did not write the originals back");
    cfg.enabled = true; call(kP);
    need(getf(gl::kSStr0) == 30.0f, "re-enabling did not boost again");
    need(le.RestoreAll() == 2 && getf(gl::kSStr0) == 15.0f && le.RestoreAll() == 0, "RestoreAll wrong");
    // 4. PlayerOnly: only the player's calls boost, other callers' calls put the original back
    cfg.playerOnly = true;
    prep(15.0f, 15.0f); le.Clear();
    le.OnCall(kP, reinterpret_cast<uint64_t>(A2), 0, cfg);        // player unknown (0): nothing is boosted
    need(getf(gl::kSStr0) == 15.0f, "PlayerOnly boosted while the player is unknown");
    le.OnCall(kP, reinterpret_cast<uint64_t>(A2), kP, cfg);
    need(getf(gl::kSStr0) == 30.0f, "PlayerOnly did not boost the player's call");
    le.OnCall(kQ, reinterpret_cast<uint64_t>(A2), kP, cfg);
    need(getf(gl::kSStr0) == 15.0f, "a non-player call must restore the original");
    se.player.Reset();
    call(kP);   // through the detour logic: the first caller becomes the player and is boosted
    need(getf(gl::kSStr0) == 30.0f && se.player.Current() == kP, "detour: the player's call was not boosted");
    cfg.playerOnly = false;
    // 5. things that are not launch records stay untouched
    prep(15.0f, 15.0f); le.Clear();
    setu(gl::kSMark1, 0x7F7FFFFEu); call(kP);
    need(getf(gl::kSStr0) == 15.0f, "marker mismatch (+gl::kSMark1) was boosted");
    prep(15.0f, 15.0f); setu(gl::kSMark0, 0); call(kP);
    need(getf(gl::kSStr0) == 15.0f, "marker mismatch (+gl::kSMark0) was boosted");
    prep(15.0f, 0.3f); call(kP);
    need(getf(gl::kSStr0) == 15.0f && getf(gl::kSStr1) == 0.3f, "value <= 0.5 accepted");
    prep(600.0f, 15.0f); call(kP);
    need(getf(gl::kSStr0) == 600.0f, "value >= 500 accepted");
    prep(std::numeric_limits<float>::infinity(), 15.0f); call(kP);
    prep(std::numeric_limits<float>::quiet_NaN(), 15.0f); call(kP);
    prep(-15.0f, 15.0f); call(kP);
    need(getf(gl::kSStr1) == 15.0f && le.Stats().records == 0, "non-finite / negative values were accepted");
    // 6. the game rewrites the field: that value becomes the new original
    prep(15.0f, 15.0f); le.Clear(); call(kP);
    setf(gl::kSStr0, 20.0f);
    call(kP); call(kP);
    need(getf(gl::kSStr0) == 40.0f && le.List(1)[0].orig[0] == 20.0f && le.List(1)[0].rebased == 1, "rewrite by the game not adopted as the new original (or compounded)");
    // 7. the record stops looking like one (memory reused): forgotten, nothing written
    setu(gl::kSMark0, 0);
    setf(gl::kSStr0, 99.0f);
    call(kP);
    need(getf(gl::kSStr0) == 99.0f && le.Stats().records == 0 && le.Stats().dropped >= 1, "a record that no longer matches must be dropped untouched");
    // 8. discovery table content
    prep(15.0f, 12.0f); le.Clear(); call(kP); call(kP);
    std::vector<LaunchRec> lst = le.List(10);
    need(lst.size() == 1 && lst[0].S == pS && lst[0].lastA1 == kP && lst[0].keys[0] == 1021 && lst[0].keys[2] == 7 && lst[0].orig[0] == 15.0f && lst[0].orig[1] == 12.0f &&
         lst[0].applied[1] == 24.0f && lst[0].calls == 2, "launch table content wrong");
    // 9. disabled boost still discovers records (log only)
    cfg.enabled = false; prep(15.0f, 15.0f); le.Clear(); call(kP);
    need(getf(gl::kSStr0) == 15.0f && le.Stats().records == 1, "discovery must work while the boost is off");
    cfg.enabled = true;
    // 10. unreadable memory never crashes
    le.OnCall(kP, 8, 0, cfg);
    uint64_t badS = 0x10;
    uint8_t a2b[0x100] = {};
    memcpy(a2b + gl::kApplyS, &badS, 8);
    le.OnCall(kP, reinterpret_cast<uint64_t>(a2b), 0, cfg);
    // 11. table capacity: records beyond the cap are ignored, never modified
    le.Clear();
    cfg.playerOnly = false;
    static std::vector<uint8_t> many;
    const size_t N = 2200;
    many.assign(N * 0x200, 0);
    uint8_t a2c[0x100] = {};
    for (size_t i = 0; i < N; ++i) {
        uint8_t* s = many.data() + i * 0x200;
        uint32_t m = 0x7F7FFFFFu;
        float v = 15.0f;
        memcpy(s + gl::kSMark0, &m, 4); memcpy(s + gl::kSMark1, &m, 4); memcpy(s + gl::kSMark2, &m, 4); memcpy(s + gl::kSStr0, &v, 4); memcpy(s + gl::kSStr1, &v, 4);
        uint64_t ps = reinterpret_cast<uint64_t>(s);
        memcpy(a2c + gl::kApplyS, &ps, 8);
        le.OnCall(kP, reinterpret_cast<uint64_t>(a2c), 0, cfg);
    }
    LaunchStats st = le.Stats();
    size_t boosted = 0, untouched = 0;
    for (size_t i = 0; i < N; ++i) { float v; memcpy(&v, many.data() + i * 0x200 + gl::kSStr0, 4); if (v == 30.0f) ++boosted; else if (v == 15.0f) ++untouched; }
    need(st.records == 2048 && st.mapFull == N - 2048 && boosted == 2048 && untouched == N - 2048, "2048-record cap wrong");
    le.RestoreAll();
    size_t back = 0;
    for (size_t i = 0; i < N; ++i) { float v; memcpy(&v, many.data() + i * 0x200 + gl::kSStr0, 4); if (v == 15.0f) ++back; }
    need(back == N, "RestoreAll missed records");
    le.Clear();
    r.Add(ok, "launch", ok ? "marker+range gate, idempotent persistent boost from stored originals, internal cap, disable/restore, player-only, game-rewrite adoption, dropped records, discovery table, 2048 cap" : why);
}

// ---- control panel logic without a window: model <-> settings binding, hotkeys, controller navigation, XInput swallow
int RowOfCfg(const UiModel& m, int cfg) {
    for (size_t i = 0; i < m.Rows().size(); ++i) if (m.Rows()[i].cfg == cfg) return static_cast<int>(i);
    return -1;
}
int RowOfKind(const UiModel& m, UiKind k, size_t nth = 0) {
    for (size_t i = 0; i < m.Rows().size(); ++i) if (m.Rows()[i].kind == k && nth-- == 0) return static_cast<int>(i);
    return -1;
}

void TestUiModel(Report& r) {
    CfgSnapshot snap = CfgTake();
    std::unique_ptr<CfgNotifyOff> quiet(new CfgNotifyOff);   // no live application while the model is exercised ...
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    CfgResetAll();
    UiModel m;
    // every setting the panel promises is bound, with the right control type
    struct B { int cfg; UiKind kind; };
    const B want[] = {{SlowMo_Enabled, UiKind::Check}, {SlowMo_ScaleMul, UiKind::Slider}, {SlowMo_RampInMul, UiKind::Slider}, {SlowMo_RampOutMul, UiKind::Slider},
                      {HitStop_Enabled, UiKind::Check}, {HitStop_ScaleMul, UiKind::Slider}, {HitStop_DurMul, UiKind::Slider}, {Launch_Enabled, UiKind::Check},
                      {Launch_Mult, UiKind::Slider}, {Launch_AngleMul, UiKind::Slider}, {UI_PauseGame, UiKind::Check}, {Triggers_Enabled, UiKind::Check}, {SlowMo_Gimmicks, UiKind::Check}, {Launch_MinValue, UiKind::Slider}, {Triggers_TowerBlast, UiKind::Check}, {Reaction_Finale, UiKind::Slider}, {Reaction_TurningSlash, UiKind::Slider}, {SlowMo_MaxChainSeconds, UiKind::Slider}, {BlindingFlash_Mode, UiKind::Slider}, {BlindingFlash_ChainSpeed, UiKind::Slider}, {BlindingFlash_HitScale, UiKind::Slider},
                      {BlindingFlash_HitRamp, UiKind::Slider}, {BlindingFlash_FinaleScale, UiKind::Slider}, {BlindingFlash_FinaleRamp, UiKind::Slider}};
    for (const B& b : want) {
        int row = RowOfCfg(m, b.cfg);
        need(row >= 0 && m.Rows()[static_cast<size_t>(row)].kind == b.kind && !m.Rows()[static_cast<size_t>(row)].label.empty(), "a promised setting is not bound to the right control");
    }
    const int mrow = RowOfKind(m, UiKind::Master);
    need(mrow >= 0 && RowOfKind(m, UiKind::ButtonRow, 0) >= 0 && m.Rows()[static_cast<size_t>(RowOfKind(m, UiKind::ButtonRow, 0))].buttons.size() == 4 &&
         m.Rows()[static_cast<size_t>(RowOfKind(m, UiKind::ButtonRow, 1))].buttons.size() == 2, "master / preset / save+close rows missing");
    // defaults of the new settings keep the current behaviour
    need(CfgGet(Launch_AngleMul) == 1.0f && !CfgBool(HitStop_Enabled) && CfgGet(HitStop_ScaleMul) == 1.0f && CfgDefOf(Launch_AngleMul).min == 0.25f && CfgDefOf(Launch_AngleMul).max == 3.0f &&
         CfgDefOf(HitStop_ScaleMul).min == 0.05f && CfgDefOf(HitStop_ScaleMul).max == 1.0f, "new settings (AngleMul / HitStop) have the wrong defaults or ranges");
    // selection
    need(m.Selected() == mrow && m.Rows()[0].kind == UiKind::Header, "the initial selection is the Mod ON / OFF row (section headers are not selectable)");
    m.Select(0);
    need(m.Selected() == mrow, "selecting a section header must be ignored");
    m.Move(1);
    need(m.Selected() == RowOfCfg(m, UI_PauseGame), "Move(+1) from the master row should land on the next control, skipping headers");
    m.Move(-1); m.Move(-1);
    need(m.Rows()[static_cast<size_t>(m.Selected())].kind == UiKind::ButtonRow && m.Selected() == RowOfKind(m, UiKind::ButtonRow, 1), "Move wraps around to the last interactive row (Save/Close)");
    m.Select(static_cast<int>(m.Rows().size()) - 1);
    need(m.Selected() == RowOfKind(m, UiKind::ButtonRow, 1), "a non-interactive row (status header) must not be selectable");
    // sliders: steps, clamping, snapping, value text
    int sc = RowOfCfg(m, SlowMo_ScaleMul);
    m.Select(sc);
    m.Adjust(1);
    need(std::fabs(CfgGet(SlowMo_ScaleMul) - 1.05f) < 1e-4, "Adjust(+1) should add one step");
    m.Adjust(-3);
    need(std::fabs(CfgGet(SlowMo_ScaleMul) - 0.90f) < 1e-4 && m.ValueText(sc) == "0.90x", "Adjust(-3) / value text wrong");
    m.Adjust(1000);
    need(CfgGet(SlowMo_ScaleMul) == 2.0f && m.ValueText(sc) == "2.00x", "Adjust must clamp at max");
    m.Adjust(-1000);
    need(CfgGet(SlowMo_ScaleMul) == 0.05f, "Adjust must clamp at min");
    need(m.SetFraction(sc, 0.0) && CfgGet(SlowMo_ScaleMul) == 0.05f && m.SetFraction(sc, 1.0) && CfgGet(SlowMo_ScaleMul) == 2.0f && m.SetFraction(sc, 2.0) && CfgGet(SlowMo_ScaleMul) == 2.0f, "SetFraction ends / clamp wrong");
    m.SetFraction(sc, 0.5);
    double steps = CfgGet(SlowMo_ScaleMul) / 0.05;
    need(std::fabs(steps - std::round(steps)) < 1e-3 && std::fabs(m.Fraction(sc) - 0.5) < 0.03, "mouse position is not snapped to the step grid");
    m.SetValueBound(sc, true);
    need(CfgGet(SlowMo_ScaleMul) == 0.05f, "Home should go to the minimum");
    m.SetValueBound(sc, false);
    need(CfgGet(SlowMo_ScaleMul) == 2.0f, "End should go to the maximum");
    int mx = RowOfCfg(m, HitStop_DurMul);
    std::string e;
    CfgSet(HitStop_DurMul, 2.2f, e);   // off the 0.5-step grid
    m.Select(mx);
    m.Adjust(1);
    need(CfgGet(HitStop_DurMul) == 2.5f, "off-grid value: +1 should go to the next grid value");
    CfgSet(HitStop_DurMul, 2.2f, e);
    m.Adjust(-1);
    need(CfgGet(HitStop_DurMul) == 2.0f && m.ValueText(mx) == "2.0x", "off-grid value: -1 should go to the previous grid value");
    m.Adjust(-100);
    need(CfgGet(HitStop_DurMul) == 1.0f, "hit-stop length never goes below 1.0");
    CfgSet(Launch_Mult, 2.0f, e);
    need(m.ValueText(RowOfCfg(m, Launch_Mult)) == "2.0x", "Launch.Mult value text wrong");
    // checks
    int po = RowOfCfg(m, UI_PauseGame);
    m.Select(po);
    bool was = CfgBool(UI_PauseGame);
    need(m.Activate() == UiResult::None && CfgBool(UI_PauseGame) == !was && m.CheckOn(po) == !was, "Activate on a checkbox should toggle it");
    m.Adjust(1);
    need(CfgBool(UI_PauseGame), "Right on a checkbox should switch it on");
    m.Adjust(-1);
    need(!CfgBool(UI_PauseGame), "Left on a checkbox should switch it off");
    CfgSet(UI_PauseGame, 1.0f, e);
    m.Select(RowOfCfg(m, HitStop_Enabled));
    m.Activate();
    need(CfgBool(HitStop_Enabled), "the hit-stop header checkbox did not toggle HitStop.Enabled");
    // master = SlowMo.Enabled + Launch.Enabled together, features stay independent
    CfgResetAll();
    m.Select(mrow);
    need(!m.MasterOn() && m.ValueText(mrow) == "OFF", "master should be off with everything off");
    m.Activate();
    need(CfgBool(SlowMo_Enabled) && CfgBool(Launch_Enabled) && m.MasterOn(), "master ON must switch both features on");
    m.Select(RowOfCfg(m, Launch_Enabled));
    m.Activate();
    need(CfgBool(SlowMo_Enabled) && !CfgBool(Launch_Enabled) && !m.MasterOn(), "features must stay independently toggleable (master turns off when one is off)");
    m.Select(mrow);
    m.Activate();
    need(CfgBool(SlowMo_Enabled) && CfgBool(Launch_Enabled) && m.ValueText(mrow) == "ON", "master with one feature on should switch both on");
    m.Activate();
    need(!CfgBool(SlowMo_Enabled) && !CfgBool(Launch_Enabled), "master OFF must switch both off");
    // preset / save / close buttons
    int pr = RowOfKind(m, UiKind::ButtonRow, 0), sv = RowOfKind(m, UiKind::ButtonRow, 1);
    // categorised layout: one section header per group, in this order; the preset buttons are Vanilla / Cinematic / Heavy hits / Extreme
    {
        const char* secs[] = {"General", "Slow motion", "Blinding Flash", "Hit-stop", "Launch", "Custom triggers", "Presets", "Status"};
        size_t k = 0;
        bool order = true;
        for (const UiRow& row : m.Rows()) if (row.kind == UiKind::Header) { order = order && k < 8 && row.label == secs[k]; ++k; }
        bool sections = k == 8 && m.Rows().front().kind == UiKind::Header && m.Rows().back().label == "Status" && m.Rows()[static_cast<size_t>(sv)].buttons[0].label == "Save & Close" &&
                        m.Rows()[static_cast<size_t>(sv)].buttons[1].label == "Close without saving";
        const UiRow& prow = m.Rows()[static_cast<size_t>(pr)];
        need(order && sections && prow.buttons[0].label == "Vanilla" && prow.buttons[1].label == "Cinematic" && prow.buttons[2].label == "Heavy hits" && prow.buttons[3].label == "Extreme", "section headers / button order");
        need(RowOfCfg(m, UI_PauseGame) == mrow + 1 && RowOfCfg(m, SlowMo_Enabled) > RowOfCfg(m, UI_PauseGame) && RowOfCfg(m, HitStop_Enabled) > RowOfCfg(m, SlowMo_RampOutMul) &&
             RowOfCfg(m, Launch_Enabled) > RowOfCfg(m, HitStop_DurMul) && RowOfCfg(m, Triggers_Enabled) > RowOfCfg(m, Launch_AngleMul) && RowOfCfg(m, SlowMo_Gimmicks) == RowOfCfg(m, SlowMo_Enabled) + 1 &&
             pr > RowOfCfg(m, Triggers_Enabled), "controls are not grouped under their sections");
        // the help pane: title, explanation, range / default / current and a tip for every control
        bool all = true;
        for (size_t i = 0; i < m.Rows().size(); ++i) {
            const UiRow& row = m.Rows()[i];
            if (!row.Interactive()) continue;
            UiHelp h = m.HelpFor(static_cast<int>(i));
            all = all && h.title.size() > 3 && h.body.size() > 30 && h.tip.size() > 10;
            if (row.kind == UiKind::Slider) all = all && h.facts.size() == 3 && h.facts[0].first == "Range" && h.facts[1].first == "Default" && h.facts[2].first == "Current";
            if (row.kind == UiKind::Check) all = all && h.facts.size() == 2 && h.facts[0].first == "Default" && h.facts[1].first == "Current";
        }
        need(all, "the help pane lacks a title / explanation / tip / facts for a control");
        UiHelp hs = m.HelpFor(RowOfCfg(m, SlowMo_ScaleMul));
        need(hs.title == "Slow motion: Scale" && hs.facts[0].second == "0.05x to 2.00x" && hs.facts[1].second == "1.00x" && hs.facts[2].second == m.ValueText(RowOfCfg(m, SlowMo_ScaleMul)), "help pane facts for a slider");
        CfgSet(HitStop_DurMul, 3.5f, e);
        need(m.HelpFor(RowOfCfg(m, HitStop_DurMul)).facts[2].second == "3.5x" && m.HelpFor(RowOfCfg(m, HitStop_DurMul)).facts[0].second == "1.0x to 10.0x", "the help pane shows the CURRENT value");
        CfgSet(HitStop_DurMul, 1.0f, e);
        need(m.HelpFor(RowOfCfg(m, SlowMo_Gimmicks)).body.find("1.5 seconds") != std::string::npos && m.HelpFor(RowOfCfg(m, SlowMo_Gimmicks)).facts[0].second == "On", "explosions / towers help text");
    }
    // Blinding Flash section: the Mode slider reads as a word and each mode shows only its own rows
    {
        CfgSet(BlindingFlash_Mode, 0.0f, e);
        int mr = RowOfCfg(m, BlindingFlash_Mode), cs = RowOfCfg(m, BlindingFlash_ChainSpeed), hs = RowOfCfg(m, BlindingFlash_HitScale), hr = RowOfCfg(m, BlindingFlash_HitRamp), fs = RowOfCfg(m, BlindingFlash_FinaleScale), fr = RowOfCfg(m, BlindingFlash_FinaleRamp);
        need(mr > RowOfCfg(m, SlowMo_MaxChainSeconds) && cs > mr && hs > cs && hr > hs && fs > hr && fr > fs && fr < RowOfCfg(m, HitStop_Enabled), "the Blinding Flash rows sit between Slow motion and Hit-stop");
        need(m.Visible(mr) && !m.Visible(cs) && !m.Visible(hs) && !m.Visible(hr) && !m.Visible(fs) && !m.Visible(fr) && m.ValueText(mr) == "Off", "mode 0 (off) shows only the Mode slider");
        CfgSet(BlindingFlash_Mode, 1.0f, e);
        need(m.Visible(cs) && !m.Visible(hs) && !m.Visible(hr) && m.Visible(fs) && m.Visible(fr) && m.ValueText(mr) == "Fast", "mode 1 shows Chain speed + the finale rows");
        CfgSet(BlindingFlash_Mode, 2.0f, e);
        need(!m.Visible(cs) && m.Visible(hs) && m.Visible(hr) && m.Visible(fs) && m.Visible(fr) && m.ValueText(mr) == "Smooth", "mode 2 shows the per-hit rows + the finale rows");
        CfgSet(BlindingFlash_Mode, 0.0f, e);
        m.Select(mr);
        m.Move(1);
        need(m.Selected() == RowOfCfg(m, HitStop_Enabled), "Move skips the hidden rows of an inactive mode");
        m.Select(cs);
        need(m.Selected() == RowOfCfg(m, HitStop_Enabled), "a hidden row cannot be selected");
        CfgSet(BlindingFlash_Mode, 1.0f, e);
        m.Select(cs);
        need(m.Selected() == cs, "visible in mode 1");
        CfgSet(BlindingFlash_Mode, 2.0f, e);
        need(m.Selected() == m.FirstVisible() && m.Selected() != cs, "when the selected row disappears (mode change) the selection falls back to the first control");
        m.Select(mr);
        m.Adjust(-2);
        need(CfgGet(BlindingFlash_Mode) == 0.0f, "the Mode slider steps 0 / 1 / 2");
        m.Adjust(1);
        need(CfgGet(BlindingFlash_Mode) == 1.0f && m.HelpFor(mr).facts[0].second == "Off to Smooth" && m.HelpFor(mr).facts[2].second == "Fast", "mode help facts show the words");
        need(CfgGet(BlindingFlash_ChainSpeed) == 1.0f && CfgDefOf(BlindingFlash_ChainSpeed).min == 0.5f && CfgMaxOf(BlindingFlash_ChainSpeed) == 1.0f && CfgDefOf(BlindingFlash_ChainSpeed).max == 2.0f && !CfgBool(BlindingFlash_UnlockSpeedUp) && CfgDefOf(BlindingFlash_UnlockSpeedUp).advanced && CfgGet(BlindingFlash_HitScale) == 0.5f && CfgDefOf(BlindingFlash_HitScale).min == 0.2f &&
             CfgGet(BlindingFlash_HitRamp) == 1.5f && CfgDefOf(BlindingFlash_HitRamp).max == 3.0f && CfgGet(BlindingFlash_FinaleScale) == 0.25f && CfgDefOf(BlindingFlash_FinaleScale).min == 0.05f && CfgGet(BlindingFlash_FinaleRamp) == 2.5f &&
             CfgDefOf(BlindingFlash_FinaleRamp).max == 4.0f && CfgList(BlindingFlash_FinaleKeys) == std::vector<uint32_t>({1023}) && CfgDefOf(BlindingFlash_FinaleKeys).advanced, "Blinding Flash defaults and ranges (FinaleKeys 1023, [Advanced])");
        need(m.HelpFor(cs).body.find("ONE clock") != std::string::npos && m.HelpFor(cs).body.find("locked") != std::string::npos && m.HelpFor(RowOfCfg(m, SlowMo_MaxChainSeconds)).body.find("only SHORTENS") != std::string::npos && m.Rows()[static_cast<size_t>(RowOfCfg(m, SlowMo_MaxChainSeconds))].label == "Slow-motion cap per chain" && m.HelpFor(RowOfCfg(m, Launch_MinValue)).body.find("HIGH minimum") != std::string::npos, "help texts: chain speed locked (one global clock), the cap only shortens and is called Slow-motion cap per chain, the high minimum warning");
        // the speed lock: above 1.0 only behind the [Advanced] unlock (the game clock is global: it would speed up EVERYONE)
        {
            std::string e2;
            need(!CfgSet(BlindingFlash_ChainSpeed, 1.3f, e2) && e2.find("UnlockSpeedUp") != std::string::npos && CfgSet(BlindingFlash_ChainSpeed, 1.0f, e2) && CfgSet(BlindingFlash_ChainSpeed, 0.5f, e2) && !CfgSet(BlindingFlash_ChainSpeed, 0.4f, e2), "ChainSpeed is capped at 1.0 (0.5 .. 1.0) while locked");
            m.Select(RowOfCfg(m, BlindingFlash_Mode)); CfgSet(BlindingFlash_Mode, 1.0f, e2);
            int cs2 = RowOfCfg(m, BlindingFlash_ChainSpeed);
            m.Select(cs2);
            m.SetValueBound(cs2, false);
            need(CfgGet(BlindingFlash_ChainSpeed) == 1.0f && m.ValueText(cs2) == "1.00x" && std::fabs(m.Fraction(cs2) - 1.0) < 1e-6 && m.HelpFor(cs2).facts[0].second == "0.50x to 1.00x", "the panel slider ends at 1.0 while locked");
            CfgSet(BlindingFlash_UnlockSpeedUp, 1.0f, e2);
            need(CfgSet(BlindingFlash_ChainSpeed, 1.5f, e2) && CfgMaxOf(BlindingFlash_ChainSpeed) == 2.0f && !CfgSet(BlindingFlash_ChainSpeed, 2.1f, e2), "unlocked: up to 2.0");
            m.SetValueBound(cs2, false);
            need(CfgGet(BlindingFlash_ChainSpeed) == 2.0f && m.HelpFor(cs2).facts[0].second == "0.50x to 2.00x", "the slider follows the unlock");
            CfgSet(BlindingFlash_UnlockSpeedUp, 0.0f, e2);
            need(CfgGet(BlindingFlash_ChainSpeed) == 1.0f && CfgMaxOf(BlindingFlash_ChainSpeed) == 1.0f, "locking again clamps the speed back to 1.0");
            // an ini that lists ChainSpeed BEFORE the unlock key still loads the unlocked value
            wchar_t tmp2[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp2);
            std::wstring pth = std::wstring(tmp2) + L"combatmod_selftest_unlock.ini";
            { std::ofstream f(pth, std::ios::trunc); f << "[Cinematic]\nBlindingFlash.ChainSpeed=1.5\n[Advanced]\nBlindingFlash.UnlockSpeedUp=1\n"; }
            need(CfgLoad(pth, e2) && CfgGet(BlindingFlash_ChainSpeed) == 1.5f, "the unlock key is applied before the speed it widens (file order does not matter)");
            CfgSet(BlindingFlash_UnlockSpeedUp, 0.0f, e2);
            { std::ofstream f(pth, std::ios::trunc); f << "[Cinematic]\nBlindingFlash.ChainSpeed=1.5\n"; }
            need(CfgLoad(pth, e2) && CfgGet(BlindingFlash_ChainSpeed) == 1.0f && e2.find("ignored") != std::string::npos, "a locked ini with ChainSpeed 1.5 keeps 1.0 and says it ignored the value");
            DeleteFileW(pth.c_str());
        }
        // presets: Vanilla switches it off, the other presets leave the choice alone
        CfgSet(BlindingFlash_Mode, 2.0f, e);
        CfgPreset("cinematic", e);
        need(CfgGet(BlindingFlash_Mode) == 2.0f, "the cinematic preset keeps the Blinding Flash mode");
        CfgPreset("vanilla", e);
        need(CfgGet(BlindingFlash_Mode) == 0.0f, "vanilla switches Blinding Flash off");
        CfgSetStr("BlindingFlash.FinaleKeys", "1023,1024", e);
        need(CfgList(BlindingFlash_FinaleKeys).size() == 2 && !CfgSetStr("BlindingFlash.FinaleKeys", "x", e), "FinaleKeys is a live plain key list");
        CfgSetStr("BlindingFlash.FinaleKeys", "1023", e);
        m.Select(mr);
    }
    // docking of the help window: flush right of the panel when it fits, else flush left
    {
        UiRect work = {0, 0, 1920, 1040}, panel = {12, 12, 492, 880};
        UiDock d = UiDockHelp(panel, work, 320);
        need(d.onRight && d.x == 492, "help window should dock flush to the right edge of the panel");
        d = UiDockHelp(UiRect{1500, 12, 1900, 880}, work, 320);
        need(!d.onRight && d.x == 1500 - 320, "no room on the right: dock flush to the left edge");
        need(UiDockHelp(UiRect{1100, 12, 1600, 880}, work, 320).onRight && UiDockHelp(UiRect{1100, 12, 1601, 880}, work, 320).onRight == false, "dock side boundary (exactly fits / one pixel short)");
    }
    // custom triggers: off by default, vanilla switches them off, the other presets leave them alone
    need(!CfgBool(Triggers_Enabled) && !CfgBool(Triggers_Override) && CfgDefOf(Triggers_Override).advanced, "trigger settings default off; Override is advanced");
    need(CfgBool(SlowMo_Gimmicks) && CfgDefOf(SlowMo_GimmickKeys).advanced && CfgDefOf(SlowMo_GimmickWindowMs).advanced && CfgList(SlowMo_GimmickKeys) == std::vector<uint32_t>({60002}) && CfgGet(SlowMo_GimmickWindowMs) == 1500.0f,
         "gimmick settings: on by default, keys {60002}, window 1500 ms, keys / window are [Advanced]");
    CfgSet(Triggers_Enabled, 1.0f, e); CfgSet(Triggers_Override, 1.0f, e);
    CfgPreset("cinematic", e);
    need(CfgBool(Triggers_Enabled), "the cinematic preset must leave the custom triggers alone");
    CfgPreset("heavy", e);
    need(CfgBool(Triggers_Enabled) && CfgBool(Triggers_Override), "the heavy preset must leave the custom triggers alone");
    CfgPreset("vanilla", e);
    need(!CfgBool(Triggers_Enabled) && CfgBool(Triggers_Override), "vanilla switches the custom triggers off (Override is advanced and untouched)");
    CfgSet(Triggers_Override, 0.0f, e);
    // every interactive row explains itself in plain language (accordion under the selected row, tooltip on hover)
    bool allHelp = true;
    for (const UiRow& row : m.Rows()) if (row.Interactive() && (row.help.size() < 30 || row.tip.size() < 10 || row.title.empty())) allHelp = false;
    need(allHelp, "an interactive row has no help text");
    need(m.Help(RowOfCfg(m, SlowMo_ScaleMul)).find("How strong the slow motion is. Multiplies the game's own slow-motion speed. 0.5 = twice as slow as normal; lower = slower.") == 0 &&
         m.Help(RowOfCfg(m, UI_PauseGame)) == "Freezes the game world while this panel is open." &&
         m.Help(RowOfCfg(m, SlowMo_RampInMul)).find("first part") != std::string::npos && m.Help(RowOfCfg(m, SlowMo_RampOutMul)).find("second part") != std::string::npos &&
         m.Help(RowOfCfg(m, HitStop_DurMul)).find("about 0.4 s of near-freeze") != std::string::npos && m.Help(RowOfCfg(m, Launch_Mult)).find("Force Palm finisher") != std::string::npos &&
         m.Help(RowOfCfg(m, Launch_AngleMul)).find("25 degrees on Force Palm") != std::string::npos, "help wording");
    // the simplified panel has no controls for the removed options
    bool noRemoved = true;
    for (const UiRow& row : m.Rows()) {
        std::string l = Lower(row.label);
        if (l.find("min scale") != std::string::npos || l.find("ignore events") != std::string::npos || l.find("max value") != std::string::npos || l.find("player attacks only") != std::string::npos) noRemoved = false;
    }
    need(noRemoved, "a removed option is still in the panel");
    need(CfgBool(UI_PauseGame) && CfgDefOf(UI_PauseGame).def == 1.0f, "UI.PauseGame defaults to on");
    CfgSet(UI_PauseGame, 0.0f, e);
    CfgPreset("cinematic", e);
    need(!CfgBool(UI_PauseGame), "presets must not touch UI.PauseGame");
    CfgSet(UI_PauseGame, 1.0f, e);
    CfgResetAll();
    need(m.ActivateButton(pr, 1) == UiResult::None && CfgGet(SlowMo_ScaleMul) == 0.5f && CfgBool(Launch_Enabled) && !m.message.empty(), "Cinematic preset button");
    need(m.ActivateButton(pr, 3) == UiResult::None && CfgGet(SlowMo_ScaleMul) == 0.1f && CfgGet(Launch_Mult) == 10.0f, "Extreme preset button (4th)");
    need(m.ActivateButton(pr, 0) == UiResult::None && !CfgBool(SlowMo_Enabled) && !CfgBool(Launch_Enabled) && CfgGet(SlowMo_ScaleMul) == 1.0f, "Vanilla preset button");
    CfgSet(SlowMo_Enabled, 1.0f, e);
    need(m.ActivateButton(pr, 2) == UiResult::None && CfgBool(HitStop_Enabled) && CfgGet(HitStop_ScaleMul) == 0.15f && CfgGet(HitStop_DurMul) == 5.0f && CfgBool(SlowMo_Enabled), "Heavy hits preset button (hit-stop only, the rest unchanged)");
    m.ActivateButton(pr, 0);
    m.Select(pr);
    m.Adjust(1);
    need(m.SubSel() == 1, "Right on a button row should move the sub selection");
    m.Adjust(10); m.Adjust(10); m.Adjust(10);
    need(m.SubSel() == 3, "the sub selection must not run past the last button");
    m.Adjust(-1); m.Adjust(-1); m.Adjust(-1); m.Adjust(-1); m.Adjust(-1);
    need(m.SubSel() == 0, "the sub selection must not run before the first button");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    m.savePath = std::wstring(tmp) + L"combatmod_selftest_ui.ini";
    DeleteFileW(m.savePath.c_str());
    // Save & Close: writes the ini and asks the window to close; a failing save keeps the panel open and says why
    m.BeginSession();
    CfgSet(Launch_Mult, 3.0f, e);
    need(m.ActivateButton(sv, 0) == UiResult::Close && GetFileAttributesW(m.savePath.c_str()) != INVALID_FILE_ATTRIBUTES && m.message.find("Saved") == 0 && !CfgDirty() && CfgGet(Launch_Mult) == 3.0f, "Save & Close should write the ini, keep the values and close");
    DeleteFileW(m.savePath.c_str());
    std::wstring badPath = std::wstring(tmp) + L"combatmod_no_such_dir\\x.ini";
    m.savePath = badPath;
    need(m.ActivateButton(sv, 0) == UiResult::None && m.message.find("Save failed") == 0, "a failing save must keep the panel open with a message");
    // Close without saving: EVERY setting returns to its value when the panel was opened (live), dirty flag included
    m.savePath = std::wstring(tmp) + L"combatmod_selftest_ui.ini";
    CfgResetAll();
    m.BeginSession();
    bool dirty0 = CfgDirty();
    CfgSet(SlowMo_ScaleMul, 0.3f, e); CfgSet(Launch_Enabled, 1.0f, e); CfgSet(HitStop_DurMul, 4.0f, e); CfgSet(UI_PauseGame, 0.0f, e); CfgSetStr("SlowMo.DenyKeys", "1,2", e); CfgSet(Launch_AngleMul, 2.0f, e);
    need(m.ActivateButton(sv, 1) == UiResult::Close, "Close without saving should close");
    need(CfgGet(SlowMo_ScaleMul) == 1.0f && !CfgBool(Launch_Enabled) && CfgGet(HitStop_DurMul) == 1.0f && CfgBool(UI_PauseGame) && CfgList(SlowMo_DenyKeys).size() == 6 && CfgGet(Launch_AngleMul) == 1.0f && CfgDirty() == dirty0 &&
         GetFileAttributesW(m.savePath.c_str()) == INVALID_FILE_ATTRIBUTES, "Close without saving must restore every setting and write nothing");
    quiet.reset();   // ... except for this one check: Discard must re-apply live
    CfgHandler prevH = CfgSetChangeHandler(&CountingHandler);
    m.BeginSession();
    CfgSet(SlowMo_ScaleMul, 0.3f, e);
    int before = g_cfgCalls;
    m.Discard();
    need(g_cfgCalls == before + 1, "Discard must re-apply the restored settings live (one notification)");
    CfgSetChangeHandler(prevH);
    quiet.reset(new CfgNotifyOff);
    m.Select(sv);
    m.Adjust(1);
    need(m.Activate() == UiResult::Close, "keyboard/pad activation on the second button should close");
    CfgRestore(snap);
    r.Add(ok, "ui_model", ok ? "rows bound to the settings table, selection + wrap, step/clamp/snap incl. off-grid, mouse fraction, Home/End, checkboxes, master+independent toggles, presets, Save, Close" : why);
}

void TestUiInput(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    UiHotkey k;
    need(UiParseHotkey("Ctrl+Enter", k) && k.ctrl && !k.shift && !k.alt && k.vk == 0x0D, "Ctrl+Enter");
    need(UiParseHotkey(" ctrl + SHIFT + f5 ", k) && k.ctrl && k.shift && k.vk == 0x74, "Ctrl+Shift+F5 (case, spaces)");
    need(UiParseHotkey("Alt+Space", k) && k.alt && k.vk == 0x20 && UiParseHotkey("g", k) && k.vk == 'G' && !k.ctrl && UiParseHotkey("Ctrl+7", k) && k.vk == '7', "Alt+Space / G / Ctrl+7");
    need(!UiParseHotkey("", k) && !UiParseHotkey("Ctrl+", k) && !UiParseHotkey("Ctrl+Enter+Space", k) && !UiParseHotkey("Ctrl+Shift", k) && !UiParseHotkey("Foo", k) && !UiParseHotkey("F25", k), "invalid hotkeys must be rejected");
    // debounced edge detector: one toggle per press, held keys do not repeat, bounces inside the debounce window are ignored
    EdgeToggle e(300);
    need(!e.Update(false, 0) && e.Update(true, 100) && !e.Update(true, 150) && !e.Update(true, 900), "first press fires once, holding does not repeat");
    need(!e.Update(false, 950) && e.Update(true, 1000), "a press after release (and after the debounce) fires again");
    need(!e.Update(false, 1010) && !e.Update(true, 1100), "a bounce inside the debounce window is ignored");
    need(!e.Update(false, 1150) && e.Update(true, 1500), "...and the next real press works");
    // both pad buttons are required (LB + DPad-Down)
    EdgeToggle p(300);
    need(!p.Update(PadOpenCombo(pad::LB), 0) && !p.Update(PadOpenCombo(pad::LB | pad::Down), 0 + 0) == false, "LB then Down: fires when both are held");
    EdgeToggle p2(300);
    need(!p2.Update(PadOpenCombo(pad::Down), 0) && !p2.Update(PadOpenCombo(pad::LB), 10) && !p2.Update(PadOpenCombo(pad::RB | pad::Down), 20) && p2.Update(PadOpenCombo(pad::Down | pad::LB | pad::A), 400), "only LB+Down counts");
    need(PadOpenCombo(pad::LB | pad::Down) && !PadOpenCombo(pad::LB) && !PadOpenCombo(pad::Down) && !PadOpenCombo(0), "PadOpenCombo");

    // controller navigation
    PadNav n;
    PadEvents ev = n.Update(pad::Down, 0, 0);
    need(ev.moveY == 1, "DPad-Down press moves the selection down at once");
    int moves = 0;
    for (long long t = 16; t < 340; t += 16) moves += n.Update(pad::Down, 0, t).moveY != 0;
    need(moves == 0, "no auto-repeat before 350 ms");
    int rep = 0;
    for (long long t = 340; t < 1000; t += 16) rep += n.Update(pad::Down, 0, t).moveY != 0;
    need(rep >= 3 && rep <= 6, "auto-repeat every ~140 ms after the initial delay");
    n.Update(0, 0, 1010);
    need(n.Update(pad::Up, 0, 1020).moveY == -1, "release then press: immediate again, upwards");
    n.Update(0, 0, 1040);
    need(n.Update(pad::Up | pad::Down, 0, 1050).moveY == 0, "Up+Down together do nothing");
    n.Update(0, 0, 1100);
    // horizontal: first step at once, repeat after 350 ms, accelerating the longer it is held
    need(n.Update(pad::Right, 0, 2000).adjust == 1, "Right adjusts by one step at once");
    int early = 0, maxMult = 0, last = 0;
    bool monotone = true;
    for (long long t = 2016; t <= 5200; t += 16) {
        int a = n.Update(pad::Right, 0, t).adjust;
        if (!a) continue;
        if (t - 2000 < 350) ++early;
        maxMult = std::max(maxMult, a);
        if (a < last) monotone = false;
        last = a;
    }
    need(early == 0 && monotone && maxMult == 20, "slider auto-repeat: delayed, monotonically accelerating up to x20");
    n.Update(0, 0, 5300);
    need(n.Update(0, -20000, 6000).adjust == -1 && n.Update(0, -20000, 6016).adjust == 0, "left stick adjusts (negative), then waits for the repeat delay");
    n.Update(0, 0, 6100);
    need(n.Update(0, 20000, 6200).adjust == 1, "right stick adjusts");
    n.Update(0, 0, 6300);
    need(n.Update(0, 5000, 7000).adjust == 0 && n.Update(0, -8999, 7016).adjust == 0, "stick inside the dead zone does nothing");
    n.Update(0, 0, 7100);
    PadEvents a1 = n.Update(pad::A, 0, 8000), a2 = n.Update(pad::A, 0, 8016);
    need(a1.activate && !a2.activate, "A activates once per press");
    n.Update(0, 0, 8100);
    PadEvents b1 = n.Update(pad::B, 0, 9000);
    n.Update(0, 0, 9100);
    PadEvents b2 = n.Update(pad::Start, 0, 9200);
    need(b1.back && !b1.start && b2.start && !b2.back && !b1.activate, "B = close without saving, Start = save and close");
    // LB is the combo modifier: nothing happens while it is held, nor right after it is released until everything is let go
    n.Update(0, 0, 9300);
    PadEvents c1 = n.Update(pad::LB | pad::Down, 0, 10000);
    PadEvents c2 = n.Update(pad::Down, 0, 10016);    // LB released first, DPad-Down still held
    need(!c1.moveY && !c2.moveY && !c1.activate, "the DPad-Down of the LB combo must not move the selection");
    n.Update(0, 0, 10100);
    need(n.Update(pad::Down, 0, 10200).moveY == 1, "navigation resumes once the buttons were released");
    n.Reset();
    n.Latch();   // buttons that are down while the panel opens must not press anything in it
    need(!n.Update(pad::A, 0, 20000).activate && !n.Update(pad::A | pad::Down, 0, 20016).moveY, "a latched pad ignores buttons that were already down");
    n.Update(0, 0, 20100);
    need(n.Update(pad::A, 0, 20200).activate, "...and works again after everything was released");
    n.Reset();
    // the game sees an idle pad while the panel is open
    XState s = {7, 0xFFFF, 255, 255, 1000, -2000, 3, 4};
    XInputSwallow(s);
    need(s.packet == 7 && s.buttons == 0 && s.lt == 0 && s.rt == 0 && s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0 && sizeof(XState) == 16, "XInput swallow must zero buttons/triggers/sticks and keep the packet number");
    r.Add(ok, "ui_input", ok ? "hotkey parsing, debounced edge detector (held / bounce / both-buttons), controller navigation (repeat delay, acceleration, stick dead zone, A/B/Start, LB latch), XInput swallow" : why);
}

// ---- hit-stop class + launch angle (the two new features)
void TestHitStopAngle(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    AmpEngine e;
    e.logNew = false;
    AmpParams p;
    p.scaleMul = 0.5f; p.ignoreAbove = 0.9f; p.hitStopOn = true; p.hitStopMul = 0.5f; p.slowOn = false; p.rampIn = p.rampOut = 2.0f;
    e.SetParams(p);
    e.SetEnabled(true);
    EvBlk slow(0.6f, 0.3f, 0.5f, 0.2f), blip(0.99f, 0.3f, 0.1f, 0.05f), noop(1.0f, 0.3f, 0.1f, 0.05f);
    EvArg es(slow.Ptr()), eb(blip.Ptr()), en(noop.Ptr());
    for (int i = 0; i < 4; ++i) { e.OnEvent(es.Ptr(), 1); e.OnEvent(eb.Ptr(), 1); e.OnEvent(en.Ptr(), 1); }
    need(FEq(slow.Scale(), 0.6f) && FEq(slow.A(), 0.5f), "hit-stop only: a slow-motion event must be untouched");
    need(FEq(blip.Scale(), 0.99f * 0.5f) && FEq(blip.A(), 0.1f) && FEq(blip.B(), 0.05f), "hit-stop: the blip's scale is multiplied (once - no compounding), timings untouched");
    need(FEq(noop.Scale(), 1.0f), "scale 1.0 events are never touched");
    p.slowOn = true; p.hitStopOn = false;
    e.SetParams(p);
    for (int i = 0; i < 3; ++i) { e.OnEvent(es.Ptr(), 1); e.OnEvent(eb.Ptr(), 1); }
    need(FEq(slow.Scale(), 0.3f) && FEq(slow.A(), 1.0f) && FEq(slow.B(), 0.4f), "slow-mo only: scale and both ramps applied");
    need(FEq(blip.Scale(), 0.99f) && FEq(blip.A(), 0.1f), "slow-mo only: the blip must be restored to its original (hit-stop switched off)");
    p.hitStopOn = true;
    e.SetParams(p);
    e.OnEvent(es.Ptr(), 1); e.OnEvent(eb.Ptr(), 1);
    need(FEq(slow.Scale(), 0.3f) && FEq(blip.Scale(), 0.99f * 0.5f) && FEq(blip.A(), 0.1f), "both classes at once");
    e.SetEnabled(false);
    need(FEq(slow.Scale(), 0.6f) && FEq(slow.A(), 0.5f) && FEq(blip.Scale(), 0.99f), "disabling restores both classes");
    // hit-stop length: A and B of the impact pause are multiplied too (strength 0.1 + length x6 = the "heavy hit")
    p.hitStopMul = 0.1f; p.hitStopDur = 6.0f; p.slowOn = false; p.hitStopOn = true;
    e.SetParams(p);
    e.SetEnabled(true);
    AmpStats before = e.Stats();
    for (int i = 0; i < 3; ++i) { e.OnEvent(eb.Ptr(), 1); e.OnEvent(es.Ptr(), 1); }
    need(FEq(blip.Scale(), 0.99f * 0.1f) && FEq(blip.A(), 0.1f * 6.0f) && FEq(blip.B(), 0.05f * 6.0f) && FEq(slow.A(), 0.5f), "hit-stop length multiplies A and B of the blip once (no compounding) and leaves slow motion alone");
    AmpStats as = e.Stats();
    need(as.stopSeen - before.stopSeen == 3 && as.stopModified - before.stopModified >= 1, "hit-stop counters (seen / modified)");
    e.SetEnabled(false);
    need(FEq(blip.A(), 0.1f) && FEq(blip.B(), 0.05f) && FEq(blip.Scale(), 0.99f), "disabling restores the blip timings");

    // launch angle: persistent, from the stored originals, restored when off
    LaunchEngine le;
    LaunchCfg cfg;
    cfg.enabled = true; cfg.playerOnly = false; cfg.mult = 2.0f; cfg.maxValue = 120.0f; cfg.angleMul = 2.0f;
    alignas(16) static uint8_t S[0x400], A2[0x400];
    uint64_t pS = reinterpret_cast<uint64_t>(S);
    memcpy(A2 + gl::kApplyS, &pS, 8);
    auto setf = [&](size_t o, float f) { memcpy(S + o, &f, 4); };
    auto getf = [&](size_t o) { float f; memcpy(&f, S + o, 4); return f; };
    auto prep = [&](float ang) {
        memset(S, 0, sizeof S);
        uint32_t m = 0x7F7FFFFFu;
        memcpy(S + gl::kSMark0, &m, 4); memcpy(S + gl::kSMark1, &m, 4); memcpy(S + gl::kSMark2, &m, 4);
        setf(gl::kSStr0, 15.0f); setf(gl::kSStr1, 15.0f); setf(gl::kSAng0, ang); setf(gl::kSAng1, ang);
    };
    auto call = [&] { le.OnCall(0x1000, reinterpret_cast<uint64_t>(A2), 0, cfg); };
    prep(0.436332f); le.Clear();
    for (int i = 0; i < 4; ++i) call();
    need(getf(gl::kSStr0) == 30.0f && FEq(getf(gl::kSAng0), 0.436332f * 2.0f) && FEq(getf(gl::kSAng1), 0.436332f * 2.0f), "angle floats not scaled once from the original (or strength broken)");
    cfg.angleMul = 3.0f; call();
    need(FEq(getf(gl::kSAng0), 0.436332f * 3.0f), "changing AngleMul must re-derive from the original");
    cfg.angleMul = 1.0f; call();
    need(FEq(getf(gl::kSAng0), 0.436332f) && getf(gl::kSStr0) == 30.0f, "AngleMul 1.0 leaves the angles at their original (strength still boosted)");
    cfg.angleMul = 2.0f; call();
    need(le.RestoreAll() == 4 && getf(gl::kSStr0) == 15.0f && FEq(getf(gl::kSAng0), 0.436332f) && FEq(getf(gl::kSAng1), 0.436332f), "RestoreAll must restore strength AND angles");
    cfg.enabled = false; call();
    need(getf(gl::kSStr0) == 15.0f && FEq(getf(gl::kSAng0), 0.436332f), "boost off: nothing written");
    cfg.enabled = true; cfg.angleMul = 3.0f;
    prep(1.5f); le.Clear(); call();
    need(FEq(getf(gl::kSAng0), 3.14159f), "the angle is capped at pi");
    prep(0.0f); le.Clear(); call();   // an angle of 0 is not a sane angle: only the strength is touched
    need(getf(gl::kSStr0) == 30.0f && getf(gl::kSAng0) == 0.0f && getf(gl::kSAng1) == 0.0f, "insane angle fields must be left alone");
    prep(0.436332f); le.Clear(); call();
    setf(gl::kSAng0, 0.5f);   // the game rewrites an angle itself
    call();
    need(FEq(getf(gl::kSAng0), 1.5f), "a game-rewritten angle becomes the new original (0.5 x 3)");
    cfg.enabled = false; cfg.angleMul = 1.0f;
    le.Clear();
    r.Add(ok, "hitstop_angle", ok ? "hit-stop class (scale only, no compounding, off restores, slow-mo and hit-stop independent), Launch.AngleMul (persistent, from originals, cap pi, restore, sanity gate, rewrite adoption)" : why);
}

// ---- pause decision logic, background hook retry schedule, telemetry JSON
void TestPauseRetry(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    // pause: the signature / idle values and what the keeper does with an object in each state
    PauseVals sig = PauseSignature(), idle = PauseIdle();
    need(sig.f[0] == 0.001f && sig.f[1] == 1.0f && sig.f[2] == 3600.0f && sig.f[3] == 1.0f && sig.f[4] == 0.0f, "pause signature values");
    need(idle.f[0] == -1.0f && idle.f[1] == -1.0f && idle.f[2] == 0.0f && idle.f[3] == 0.0f && idle.f[4] == 0.0f, "idle values");
    need(PauseIsOurs(sig) && !PauseIsOurs(idle), "signature detection");
    need(PauseDecide(sig) == PauseAction::None, "an object holding exactly our values needs nothing");
    PauseVals v = sig;
    v.f[4] = 0.016f;
    need(PauseDecide(v) == PauseAction::WriteProgress, "the engine advanced the progress: only f4 is rewritten");
    v = idle;
    need(PauseDecide(v) == PauseAction::WriteAll, "the engine reset the object to idle: everything is re-asserted");
    v = sig; v.f[2] = 1.5f;
    need(PauseDecide(v) == PauseAction::WriteAll, "the engine started its own effect: everything is re-asserted");
    v = sig; v.f[0] = 0.5f; v.f[4] = 0.3f;
    need(PauseDecide(v) == PauseAction::WriteAll, "changed scale: full rewrite wins over progress-only");
    // retry schedule: 8 tries, 400 ms apart, only transient errors are retried
    std::vector<int> sleeps;
    auto sl = [&](int ms) { sleeps.push_back(ms); };
    int tries = 0;
    std::string msg;
    int calls = 0;
    HookResult res = HookRunSchedule([&](std::string& m) { ++calls; if (calls < 4) { m = "MinHook create: MH_ERROR_MEMORY_ALLOC"; return HookResult::Retry; } return HookResult::Ok; }, msg, tries, sl);
    need(res == HookResult::Ok && tries == 4 && sleeps.size() == 3 && sleeps[0] == 400, "succeeds on the 4th try after three 400 ms waits");
    sleeps.clear(); calls = 0;
    res = HookRunSchedule([&](std::string& m) { ++calls; m = "MinHook enable: MH_ERROR_MEMORY_ALLOC"; return HookResult::Retry; }, msg, tries, sl);
    need(res == HookResult::Retry && tries == 8 && calls == 8 && sleeps.size() == 7 && msg.find("MEMORY_ALLOC") != std::string::npos, "gives up after exactly 8 tries (7 waits)");
    sleeps.clear(); calls = 0;
    res = HookRunSchedule([&](std::string& m) { ++calls; m = "signature x is MISSING"; return HookResult::Fatal; }, msg, tries, sl);
    need(res == HookResult::Fatal && tries == 1 && sleeps.empty(), "a missing signature is not retried");
    need(HookClassify(false, "MinHook create: MH_ERROR_MEMORY_ALLOC") == HookResult::Retry && HookClassify(false, "MH_Initialize: x") == HookResult::Retry &&
         HookClassify(false, "signature time_curve_start is MISSING (0 matches)") == HookResult::Fatal && HookClassify(true, "") == HookResult::Ok, "error classification");
    need(kHookMaxTries == 8 && kHookRetryMs == 400, "retry constants");
    r.Add(ok, "pause_retry", ok ? "pause keeper decisions (none / progress-only / full rewrite, our signature vs idle) and the hook retry schedule (8 tries, 400 ms, transient only)" : why);
}

void TestTelemetry(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    auto contains = [](const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; };
    TelemetryStore st;
    uint8_t raw[96] = {};
    uint16_t t3 = 3;
    memcpy(raw, &t3, 2);
    float fl[6] = {1.5f, 2.5f, std::numeric_limits<float>::quiet_NaN(), 4.5f, 5.5f, 6.5f};   // NaN must become `null`, never an invalid token
    memcpy(raw + gl::kEvSampleFloats, fl, 24);
    const uint64_t fnReal = reinterpret_cast<uint64_t>(&FakeHandler);
    for (int i = 0; i < 5; ++i) st.OnEvent(3, 0x1111, fnReal, raw, 0xA000);
    uint16_t t7 = 7;
    memcpy(raw, &t7, 2);
    st.OnEvent(7, 0x2222, 0, raw, 0xA000);   // unreadable handler: no fingerprint, still listed
    st.OnEvent(5000, 1, 2, raw, 0xA000);     // types beyond the table are only counted
    const uint32_t k1[4] = {10253, 7, 0, 0}, k2[4] = {10253, 9, 0, 0};
    st.OnSkill(k1, true); st.OnSkill(k2, false); st.OnSkill(k1, true);
    st.OnSlowmo(0.6f, 0.5f, 0.2f, k1, true, true, false);
    st.OnSlowmo(0.6f, 0.5f, 0.2f, k2, true, false, true);
    st.OnSlowmo(0.6f, 0.5f, 0.2f, k1, false, false, false);
    st.OnSlowmo(0.25f, 0.0f, 0.1f, k1, true, true, false);
    TelemetryStore::Counts c = st.Count();
    need(c.eventTypes == 2 && c.events == 6 && c.overflow == 1 && c.skillKeys == 3 && c.slowLayouts == 2 && st.TableA1() == 0xA000, "counters wrong");

    TelInputs in;
    in.module.exePath = std::string("X") + ":\\Test Dir\\Crimson \"Desert\"\\bin64\\CrimsonDesert.exe";   // quotes and backslashes must be escaped
    in.module.plugin = "CombatMod test";
    in.module.timeDateStamp = 0x6AB28F00; in.module.sizeOfImage = 0x2000000; in.module.fileSize = 123456789; in.module.base = 0x140000000ull;
    in.started = "2026-10-25T12:00:00"; in.written = "2026-10-25T12:01:00"; in.uptimeS = 60;
    Sig a, b;
    a.name = "time_curve_start"; a.st = SigStatus::Resolved; a.matches = 1; a.addr = a.match = 0x140000000ull + 0x1234; a.fpOk = true; memset(a.fp, 0xAB, 24);
    b.name = "time_event_apply"; b.st = SigStatus::Missing; b.matches = 0;
    in.sigs = {a, b};
    TelHook h;
    h.name = "time_curve_start"; h.addr = a.addr; h.state = "installed"; h.fpOk = true; memset(h.fp, 0xCD, 24);
    in.hooks = {h};
    TelSlot sl;
    sl.slot = 3; sl.H = 0x1111; sl.fn = 0x140000000ull + 0x5000; sl.fpOk = true; memset(sl.fp, 0x11, 24);
    in.table = {sl};
    in.tableA1 = 0xA000;
    LaunchProbe lp{};
    lp.r.S = 0x7FF000; lp.r.keys[0] = 1021; lp.r.orig[0] = lp.r.orig[1] = 15.0f; lp.r.orig[2] = lp.r.orig[3] = 0.436332f; lp.r.angleOk = true; lp.r.calls = 9;
    uint32_t mk = 0x7F7FFFFFu;
    memcpy(lp.bytes + gl::kSMark0, &mk, 4); memcpy(lp.bytes + gl::kSMark1, &mk, 4); memcpy(lp.bytes + gl::kSMark2, &mk, 4);
    lp.ok = true;
    in.launch = {lp};
    std::string j = st.BuildJson(in);
    need(TelemetryJsonValid(j), "the generated JSON is not valid");
    need(j == st.BuildJson(in), "the JSON is not deterministic");
    need(contains(j, "\"schema\":1") && contains(j, "\"time_date_stamp\":\"0x6AB28F00\"") && contains(j, "\"size_of_image\":33554432") && contains(j, "\"file_size\":123456789"), "module identity missing");
    need(contains(j, "Crimson \\\"Desert\\\"") && contains(j, (std::string("X") + ":\\\\Test Dir").c_str()), "string escaping");
    need(contains(j, "\"name\":\"time_curve_start\",\"status\":\"RESOLVED\",\"matches\":1,\"rva\":\"0x1234\"") && contains(j, "\"name\":\"time_event_apply\",\"status\":\"MISSING\"") && contains(j, "abababababababababababababababababababababababab"), "signature table / fingerprint");
    need(contains(j, "\"hooks\":[") && contains(j, "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd") && contains(j, "\"fn_rva\":\"0x5000\""), "hook / event table entries");
    need(contains(j, "\"events\":[") && contains(j, "\"type\":3,\"fn_rva\":\"abs:0x") && contains(j, "\"count\":5") && contains(j, "\"type\":7,\"fn_rva\":null,\"fingerprint\":null"), "event catalog (real handler gets an absolute address + fingerprint; unreadable one is listed without)");
    need(contains(j, "\"sample_floats\":[1.5,2.5,null,4.5,5.5,6.5]") && contains(j, "\"event_types_overflow\":1"), "float interpretation / NaN as null / overflow counter");
    need(contains(j, "\"key\":10253,\"count\":3,\"player_count\":2") && contains(j, "\"key\":7,\"count\":2,\"player_count\":2") && contains(j, "\"key\":9,\"count\":1,\"player_count\":0"), "skill key counts");
    need(contains(j, "\"launch_records\":[") && contains(j, "\"orig_strength\":[15,15]") && contains(j, "\"orig_angles\":[0.436332,0.436332]") && contains(j, "\"marker\":[\"0x7F7FFFFF\",\"0x7F7FFFFF\",\"0x7F7FFFFF\"]") && contains(j, "\"calls\":9"), "launch record");
    need(contains(j, "\"slowmo_layouts\":[") && contains(j, "\"scale\":0.6,\"a\":0.5,\"b\":0.2,\"count\":3,\"modified\":1,\"filtered\":1") && contains(j, "\"scale\":0.25"), "slow-mo layouts");
    need(contains(j, "\"keys\":[{\"key\":10253,\"count\":1},{\"key\":10253,\"count\":1}]") == false, "histogram must merge equal keys");
    // the validator itself must reject broken documents
    need(!TelemetryJsonValid("{\"a\":}") && !TelemetryJsonValid("{\"a\":nan}") && !TelemetryJsonValid("[1,2,]") && !TelemetryJsonValid("{\"a\":1} x") && !TelemetryJsonValid("{\"a\":\"\x01\"}") && TelemetryJsonValid("{\"a\":[1,-2.5e3,true,null,\"x\\u00e9\"]}"), "the JSON validator is too lenient or too strict");
    SYSTEMTIME t = {};
    t.wYear = 2026; t.wMonth = 10; t.wDay = 5; t.wHour = 7; t.wMinute = 8; t.wSecond = 9;
    need(TelemetryFileName(0x6AB28F00, t) == "6AB28F00_20261005_070809.json", "session file name");
    r.Add(ok, "telemetry", ok ? "counters, valid + deterministic JSON (escaping, NaN as null, fingerprints, RVAs, event catalog + table, skill keys, launch records with S probe, slow-mo layouts), validator, file name" : why);
}

struct TW { HANDLE go, done; };
DWORD WINAPI WatchThread(LPVOID p) {
    TW* w = static_cast<TW*>(p);
    WaitForSingleObject(w->go, 5000);
    g_testVar = 0x1234;
    SetEvent(w->done);
    return 0;
}

void TestWatch(Report& r) {
    TW w;
    w.go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD tid = 0;
    g_testVar = 0;
    HANDLE th = CreateThread(nullptr, 0, WatchThread, &w, 0, &tid);
    if (!th) { r.Add(false, "watch", "CreateThread failed"); CloseHandle(w.go); CloseHandle(w.done); return; }
    uintptr_t a = reinterpret_cast<uintptr_t>(&g_testVar);
    std::string m, why;
    bool ok = true;
    auto fail = [&](const std::string& s) { if (ok) why = s; ok = false; };
    if (!WatchAdd(false, a, 4, m, tid)) fail("WatchAdd: " + m);
    else {
        SetEvent(w.go);
        if (WaitForSingleObject(w.done, 3000) != WAIT_OBJECT_0) fail("test thread did not finish");
        std::vector<WatchHit> hits;
        uint64_t total = 0;
        WatchHits(a, 10, hits, total);
        if (total != 1 || hits.size() != 1) fail(F("expected 1 hit, got total=%llu shown=%zu (%s)", U64(total), hits.size(), m.c_str()));
        else if (hits[0].tid != tid || (hits[0].val & 0xFFFFFFFF) != 0x1234 || !hits[0].rip) fail(F("hit content wrong: tid=%lu val=0x%llX", hits[0].tid, U64(hits[0].val)));
        std::string rm;
        if (!WatchRemove(a, rm)) fail("WatchRemove: " + rm);
    }
    WaitForSingleObject(th, 2000);
    CloseHandle(th);
    CloseHandle(w.go);
    CloseHandle(w.done);
    r.Add(ok, "watch", ok ? "hardware write watch fired once on the writer thread with the right tid/value (no game thread touched)" : why);
}
// ---- explosions / towers: a GIMMICK-owned time event counts as player-caused when the player used a skill shortly before (everything else non-player stays untouched)
void TestGimmick(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    SkillEngine se;
    se.enabled = true;
    alignas(16) uint8_t SbP[0x200] = {}, SbG[0x200] = {}, SbN[0x200] = {}, A2P[0x200] = {}, A2G[0x200] = {}, A2N[0x200] = {};
    const uint32_t kp[4] = {10253, 0, 0, 0}, kg[4] = {60002, 0, 0, 0}, kn[4] = {10999, 0, 0, 0}, kng[4] = {10999, 60002, 0, 0};
    memcpy(SbP + gl::kSKeys, kp, 16); memcpy(SbG + gl::kSKeys, kg, 16); memcpy(SbN + gl::kSKeys, kn, 16);
    uint64_t pP = reinterpret_cast<uint64_t>(SbP), pG = reinterpret_cast<uint64_t>(SbG), pN = reinterpret_cast<uint64_t>(SbN);
    memcpy(A2P + gl::kApplyS, &pP, 8); memcpy(A2G + gl::kApplyS, &pG, 8); memcpy(A2N + gl::kApplyS, &pN, 8);
    const uint64_t kPlayer = 0x1000, kGimmick = 0x7000, kNpc = 0x9000;
    auto call = [&](uint64_t a1, uint8_t* a2) { SkillCallThrough(se, FakeApply, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<__int64>(a2), 1); };
    AmpEngine ae;
    ae.logNew = false;
    ae.SetSkills(&se);
    AmpParams p;
    p.scaleMul = 0.5f; p.playerOnly = true; p.contextWindowMs = 800; p.gimmickOn = true; p.gimmickKeys = {60002}; p.gimmickWindowMs = 1500;
    ae.SetParams(p);
    ae.SetEnabled(true);
    EvBlk D(0.6f, 0.2f, 0.2f, 0.333f);   // native Gimmick_Active_Bomb layout: scale 0.6, A 0.2, B 0.333
    EvArg ev(D.Ptr());
    auto isAmp = [&] { return FEq(D.Scale(), 0.6f * 0.5f); };
    auto isOrig = [&] { return FEq(D.Scale(), 0.6f) && FEq(D.A(), 0.2f) && FEq(D.B(), 0.333f); };
    auto fire = [&] { ae.OnEvent(ev.Ptr(), 0x5151); return ae.Tail(1); };
    auto setp = [&](auto&& edit) { AmpParams q = ae.Params(); edit(q); ae.SetParams(q); };
    call(kPlayer, A2P);   // the first caller is the player
    call(kGimmick, A2G);  // the bomb's own apply-skill call: newest context = the gimmick (key 60002)
    std::vector<AmpLogRec> lg = fire();
    need(isAmp() && !lg.empty() && lg[0].reason == 0 && lg[0].viaGimmick && lg[0].ctxA1 == kGimmick && lg[0].ctxKeys[0] == 60002 && ae.Stats().gimmick == 1, "a gimmick event right after a player skill must be amplified (and logged via_gimmick)");
    setp([](AmpParams& q) { q.gimmickOn = false; });
    lg = fire();
    need(isOrig() && !lg.empty() && lg[0].reason == 4 && !lg[0].viaGimmick, "SlowMo.Gimmicks off: the gimmick event is a non-player event again (originals restored)");
    setp([](AmpParams& q) { q.gimmickOn = true; });
    fire();
    need(isAmp(), "switching it back on amplifies again (no compounding)");
    // an NPC's skill right after the player's: never, whatever the timing
    call(kPlayer, A2P);
    call(kNpc, A2N);
    lg = fire();
    need(isOrig() && !lg.empty() && lg[0].reason == 4 && !lg[0].viaGimmick, "an NPC's slow motion must stay untouched even right after a player skill");
    // a gimmick event with NO recent player skill: untouched (window shortened to keep the test fast)
    setp([](AmpParams& q) { q.gimmickWindowMs = 50; });
    call(kPlayer, A2P);
    Sleep(120);
    call(kGimmick, A2G);
    lg = fire();
    need(isOrig() && !lg.empty() && lg[0].reason == 4 && !lg[0].viaGimmick, "a gimmick event without a player skill inside the window must stay untouched");
    setp([](AmpParams& q) { q.gimmickWindowMs = 1500; });
    call(kPlayer, A2P);
    call(kGimmick, A2G);
    fire();
    need(isAmp(), "inside the window it is amplified again");
    // only the configured gimmick keys count (an unknown owner key is an NPC)
    setp([](AmpParams& q) { q.gimmickKeys = {777}; });
    lg = fire();
    need(isOrig() && !lg.empty() && lg[0].reason == 4, "a gimmick key that is not in SlowMo.GimmickKeys is not accepted");
    setp([](AmpParams& q) { q.gimmickKeys = {60002}; });
    // the player's own skill context still wins and is not counted as a gimmick
    call(kPlayer, A2P);
    uint64_t g0 = ae.Stats().gimmick;
    lg = fire();
    need(isAmp() && !lg.empty() && !lg[0].viaGimmick && ae.Stats().gimmick == g0, "the player's own event is amplified directly (not via the gimmick rule)");
    // unknown player: nothing, not even gimmicks
    se.player.Reset();
    call(kGimmick, A2G);
    se.player.Reset();
    lg = fire();
    need(isOrig() && !lg.empty() && lg[0].reason == 5, "player unknown: nothing is amplified");
    (void)kng;
    r.Add(ok, "gimmick", ok ? "gimmick-owned slow motion (key 60002 bomb / tower) is attributed to the player only with a player skill inside the window; NPC skills, other keys, a stale window, the setting off and an unknown player all stay untouched; no compounding" : why);
}

// ---- Launch.MinValue (raise before the multiplier) and Launch.DenyKeys (key / key:slot), settings parsing of the pair lists
void TestLaunchMinDeny(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    SkillEngine se;
    LaunchEngine le;
    LaunchCfg cfg;
    cfg.enabled = true; cfg.playerOnly = false; cfg.mult = 2.0f; cfg.maxValue = 1000.0f;
    se.enabled = true;
    se.launch = &le;
    se.launchCfg = &cfg;
    alignas(16) static uint8_t S[0x400], A2[0x400];
    uint64_t pS = reinterpret_cast<uint64_t>(S);
    memcpy(A2 + gl::kApplyS, &pS, 8);
    auto setf = [&](size_t off, float f) { memcpy(S + off, &f, 4); };
    auto getf = [&](size_t off) { float f; memcpy(&f, S + off, 4); return f; };
    auto prep = [&](float a, float b, float f138, uint32_t k0, uint32_t k1, uint32_t k2 = 0) {
        memset(S, 0, sizeof S);
        for (size_t o : {gl::kSMark0, gl::kSMark1, gl::kSMark2}) { uint32_t u = 0x7F7FFFFFu; memcpy(S + o, &u, 4); }
        setf(gl::kSNeg, f138); setf(gl::kSStr0, a); setf(gl::kSStr1, b);
        uint32_t keys[4] = {k0, k1, k2, 0};
        memcpy(S + gl::kSKeys, keys, 16);
    };
    const uint64_t kP = 0x1000;
    auto call = [&]() { return SkillCallThrough(se, FakeLaunchApply, reinterpret_cast<uint64_t*>(kP), reinterpret_cast<__int64>(A2), 1); };
    auto recs = [&] { return le.List(10); };

    // 0 / 0 record, MinValue off: created (it is a real record) but nothing to multiply
    prep(0.0f, 0.0f, -0.5f, 10253, 1);
    call();
    need(getf(gl::kSStr0) == 0.0f && getf(gl::kSStr1) == 0.0f && recs().size() == 1 && le.Stats().records == 1, "a 0/0 record must be discovered and left alone while MinValue is off");
    // MinValue raises BEFORE the multiplier, persistently, idempotently
    cfg.minValue = 40.0f; cfg.mult = 1.0f;
    call();
    need(getf(gl::kSStr0) == 40.0f && getf(gl::kSStr1) == 40.0f && recs()[0].raised && le.Stats().raised == 1, "MinValue must raise a 0/0 record to the minimum");
    cfg.mult = 2.0f;
    for (int i = 0; i < 4; ++i) call();
    need(getf(gl::kSStr0) == 80.0f && getf(gl::kSStr1) == 80.0f, "the minimum is applied BEFORE the multiplier (40 x 2), never compounded over repeated calls");
    cfg.enabled = false;
    call();
    need(getf(gl::kSStr0) == 0.0f && getf(gl::kSStr1) == 0.0f && !recs()[0].raised, "boost off: the raised record goes back to its 0/0 original");
    cfg.enabled = true;
    call();
    need(getf(gl::kSStr0) == 80.0f, "boost on again: raised again");
    need(le.RestoreAll() == 2 && getf(gl::kSStr0) == 0.0f && getf(gl::kSStr1) == 0.0f, "launch restore puts the 0/0 originals back");
    // per field: below the minimum is lifted, above is kept
    prep(13.0f, 60.0f, -0.4f, 1021, 5, 1105);
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 80.0f && getf(gl::kSStr1) == 120.0f, "(13, 60) with minimum 40, x2 = (80, 120): each field on its own");
    cfg.minValue = 0.0f;
    call();
    need(getf(gl::kSStr0) == 26.0f && getf(gl::kSStr1) == 120.0f, "minimum 0 = off: plain multiplier again");
    cfg.minValue = 40.0f;
    // records without markers / without a sane S+gl::kSNeg are not touched
    prep(0.0f, 0.0f, 0.0f, 10253, 1);   // S+gl::kSNeg = 0: not in [-1, 0)
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 0.0f && recs().empty(), "a 0/0 record whose S+gl::kSNeg is not in [-1, 0) is not a launch record");
    prep(0.0f, 0.0f, 0.7f, 10253, 1);
    call();
    need(getf(gl::kSStr0) == 0.0f && recs().empty(), "S+gl::kSNeg = +0.7: not a launch record");
    prep(0.0f, 0.0f, -0.5f, 10253, 1);
    { uint32_t zero = 0; memcpy(S + gl::kSMark1, &zero, 4); }
    call();
    need(getf(gl::kSStr0) == 0.0f && recs().empty(), "no FLT_MAX markers: not a launch record");
    // Launch.DenyKeys: "key:slot" and plain "key"
    cfg.nDeny = 2; cfg.deny[0] = KeyEnc(1091, 5); cfg.deny[1] = KeyEnc(60002, -1);
    prep(13.0f, 13.0f, -0.5f, 1091, 5);   // the explosive arrow
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 13.0f && getf(gl::kSStr1) == 13.0f && recs()[0].denied && le.Stats().denied == 1, "the explosive arrow record (1091:5) must never be boosted");
    prep(0.0f, 0.0f, -0.5f, 1091, 5);
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 0.0f && recs()[0].denied && !recs()[0].raised, "a denied 0/0 record is never raised either");
    prep(13.0f, 13.0f, -0.5f, 1091, 2, 1105);   // the greatsword fire hit: same primary key, other slot
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 80.0f && !recs()[0].denied, "1091:2 (greatsword fire) is NOT denied by 1091:5 and is boosted (minimum 40 x2)");
    prep(13.0f, 13.0f, -0.5f, 2, 60002, 0);   // bomb gimmick: plain key, any of the 4
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 13.0f && recs()[0].denied, "a plain key entry denies a record that carries it in ANY of the 4 key slots");
    // deny added AFTER a boost: the record is restored
    cfg.nDeny = 0;
    prep(13.0f, 13.0f, -0.5f, 1091, 5);
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 80.0f, "setup: boosted");
    cfg.nDeny = 1; cfg.deny[0] = KeyEnc(1091, 5);
    call();
    need(getf(gl::kSStr0) == 13.0f && recs()[0].denied, "a record that becomes denied is restored to its original");
    // the game rewriting a 0 record: the new value is adopted as the original
    cfg.nDeny = 0;
    prep(0.0f, 0.0f, -0.5f, 10253, 1);
    le.Clear();
    call();
    need(getf(gl::kSStr0) == 80.0f, "setup: raised");
    setf(gl::kSStr0, 7.0f); setf(gl::kSStr1, 7.0f);
    call();
    need(getf(gl::kSStr0) == 80.0f && recs()[0].orig[0] == 7.0f && recs()[0].rebased >= 1, "a rewritten value is adopted as the new original (7 < 40: raised again)");
    le.Clear();

    // ---- the key encodings and the Launch.DenyKeys setting
    uint32_t arrow[4] = {1091, 5, 0, 0}, fire[4] = {1091, 2, 1105, 0}, odd[4] = {5, 1091, 0, 0}, bomb[4] = {7, 60002, 0, 0}, p1091_0[4] = {1091, 0, 0, 0}, p10253_1[4] = {10253, 1, 0, 0}, p10253_2[4] = {10253, 2, 0, 0};
    need(KeyEncMatch(KeyEnc(1091, 5), arrow) && !KeyEncMatch(KeyEnc(1091, 5), fire) && !KeyEncMatch(KeyEnc(1091, 5), odd) && KeyEncMatch(KeyEnc(1091, -1), fire) && KeyEncMatch(KeyEnc(1091, -1), odd) &&
         KeyEncMatch(KeyEnc(60002, -1), bomb) && !KeyEncMatch(0, arrow) && KeyEncMatch(KeyEnc(1091, 0), p1091_0), "KeyEnc matching: plain = any of the 4, key:slot = keys[0] and keys[1]");
    uint32_t enc = 0;
    std::string e;
    need(ParseKeyEnc("1091:5", enc, e) && enc == KeyEnc(1091, 5) && KeyEncStr(enc) == "1091:5" && ParseKeyEnc("60002", enc, e) && KeyEncStr(enc) == "60002" && !ParseKeyEnc("1091:", enc, e) && !ParseKeyEnc("x:5", enc, e) &&
         !ParseKeyEnc(":5", enc, e) && !ParseKeyEnc("1091:999", enc, e) && !ParseKeyEnc("0:5", enc, e) && !ParseKeyEnc("99999999", enc, e), "ParseKeyEnc / KeyEncStr");
    CfgSnapshot snap = CfgTake();
    {
        CfgNotifyOff quiet;
        need(CfgValueStr(Launch_DenyKeys) == "1091:5,60002" && CfgKeysMatch(Launch_DenyKeys, arrow) && !CfgKeysMatch(Launch_DenyKeys, fire) && CfgKeysMatch(Launch_DenyKeys, bomb) && CfgDefOf(Launch_DenyKeys).advanced,
             "Launch.DenyKeys default 1091:5,60002 ([Advanced])");
        LaunchCfg fromSettings = LaunchCfgFromSettings();
        need(fromSettings.nDeny == 2 && fromSettings.deny[0] == KeyEnc(1091, 5) && fromSettings.deny[1] == KeyEnc(60002, -1) && fromSettings.minValue == 0.0f, "LaunchCfgFromSettings carries the deny list and MinValue");
        need(CfgSetStr("Launch.DenyKeys", "1091:5, 10253:1,60002", e) && CfgValueStr(Launch_DenyKeys) == "1091:5,10253:1,60002" && CfgKeysMatch(Launch_DenyKeys, p10253_1) && !CfgKeysMatch(Launch_DenyKeys, p10253_2),
             "the pair list parses and round-trips");
        need(!CfgSetStr("Launch.DenyKeys", "1091:", e) && !CfgSetStr("Launch.DenyKeys", "abc", e) && CfgValueStr(Launch_DenyKeys) == "1091:5,10253:1,60002", "a bad entry is refused and changes nothing");
        need(!CfgSetStr("SlowMo.DenyKeys", "1091:5", e), "SlowMo.DenyKeys stays a plain key list");
        uint32_t cp[16];
        need(CfgListCopy(Launch_DenyKeys, cp, 16) == 3 && cp[1] == KeyEnc(10253, 1) && CfgListCopy(Launch_DenyKeys, cp, 2) == 2, "CfgListCopy");
        need(CfgSet(Launch_MinValue, 25.0f, e) && LaunchCfgFromSettings().minValue == 25.0f && !CfgSet(Launch_MinValue, 61.0f, e) && !CfgSet(Launch_MinValue, -1.0f, e) && CfgDefOf(Launch_MinValue).def == 0.0f, "Launch.MinValue 0..60, default 0");
        // a saved ini round-trips the pair list; presets never touch the [Advanced] tuning
        CfgPreset("cinematic", e);
        need(CfgValueStr(Launch_DenyKeys) == "1091:5,10253:1,60002" && CfgGet(Launch_MinValue) == 25.0f, "presets keep [Advanced] values (the deny list) and the personal minimum launch (25 stays 25)");
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring path = std::wstring(tmp) + L"combatmod_selftest_pairs.ini";
        DeleteFileW(path.c_str());
        need(CfgSave(path, e), "save");
        CfgResetAll();
        need(CfgValueStr(Launch_DenyKeys) == "1091:5,60002" && CfgLoad(path, e) && CfgValueStr(Launch_DenyKeys) == "1091:5,10253:1,60002", "the pair list survives an ini save / load");
        DeleteFileW(path.c_str());
    }
    CfgRestore(snap);
    r.Add(ok, "launch_min_deny", ok ? "Launch.MinValue raises 0/0 and weak records before the multiplier (per field, idempotent, restorable, only real records), Launch.DenyKeys key / key:slot (arrow 1091:5 denied, 1091:2 not, plain key in any slot, restore on deny, rewrite adoption), key encodings, settings parse / save / presets" : why);
}

// ---- `launch scan`: byte-wise scan of committed RW memory for skill records, against planted records in real (fake-data) memory
void TestLaunchScan(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    const size_t kRw = 3u << 20, kRo = 0x10000, kTotal = kRw + kRo;
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTotal, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!base) { r.Add(false, "launch_scan", "VirtualAlloc failed"); return; }
    auto plant = [&](size_t patternAt, float f138, float l0, float l1, uint32_t k0, uint32_t k1, uint32_t k2, float a0, float a1) -> uint8_t* {
        uint8_t* s = base + patternAt - gl::kSMark0;
        uint32_t keys[4] = {k0, k1, k2, 0}, fm = 0x7F7FFFFFu;
        memcpy(s + gl::kSKeys, keys, 16);
        memcpy(s + gl::kSNeg, &f138, 4);
        for (int i = 0; i < 3; ++i) memcpy(s + gl::kSMark0 + 4 * i, &fm, 4);
        memcpy(s + gl::kSStr0, &l0, 4); memcpy(s + gl::kSStr1, &l1, 4); memcpy(s + gl::kSAng0, &a0, 4); memcpy(s + gl::kSAng1, &a1, 4);
        return s;
    };
    auto scanAll = [&](uintptr_t lo, uintptr_t hi, std::pair<uintptr_t, uintptr_t> excl = {0, 0}, long long budget = 60000000) {
        LaunchScan sc(lo, hi);
        if (excl.second) sc.Exclude(excl.first, excl.second);
        while (!sc.Step(budget)) {}
        return std::make_pair(sc.Records(), sc.Stats());
    };
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base), hi = lo + kTotal;
    // 1. a record whose 12-byte marker sits at every awkward place (odd S, chunk edges, overlaps, page edges) is found exactly once
    const size_t M = 1u << 20;
    const size_t spots[] = {0x1001 + gl::kSMark0, 0x2000 - 5 + 0, M - 13, M - 12, M - 11, M - 5, M - 1, M, M + 1, 2 * M - 23, 2 * M - 22, 2 * M - 12, 2 * M - 11, 3 * M - 0x200, 0x7FF3};
    bool allSpots = true;
    std::string badSpot;
    for (size_t at : spots) {
        memset(base, 0, kRw);
        uint8_t* s = plant(at, -0.5f, 13.0f, 13.0f, 10253, 1, 0, 0.4f, 0.5f);
        auto res = scanAll(lo, lo + kRw);
        bool good = res.first.size() == 1 && res.first[0].S == reinterpret_cast<uint64_t>(s) && res.first[0].launch[0] == 13.0f && res.first[0].keys[0] == 10253 && res.first[0].angleOk;
        if (!good) { allSpots = false; badSpot = F("marker at +0x%zX found %zu", at, res.first.size()); break; }
    }
    need(allSpots, badSpot.c_str());
    // 2. many records, rejects, dedupe, ordering, zero records, read-only memory, no writes
    memset(base, 0, kRw + kRo);
    uint8_t* r1 = plant(0x1001 + gl::kSMark0, -0.5f, 13.0f, 13.0f, 10253, 1, 0, 0.4f, 0.5f);      // odd-aligned S
    uint8_t* r2 = plant(0x5003 + gl::kSMark0, -0.4f, 0.0f, 0.0f, 10253, 1, 0, 0.0f, 0.0f);        // launch 0/0, angles 0
    uint8_t* r3 = plant(0x9007 + gl::kSMark0, -0.5f, 0.0f, 40.0f, 1021, 5, 1105, 0.3f, 0.3f);
    plant(0xB001 + gl::kSMark0, 0.7f, 13.0f, 13.0f, 1, 2, 3, 0.4f, 0.5f);                          // S+gl::kSNeg = +0.7: reject
    plant(0xD003 + gl::kSMark0, -0.5f, 1000.0f, 13.0f, 4, 5, 6, 0.4f, 0.5f);                       // launch 1000: reject
    { uint8_t* x = plant(0xF005 + gl::kSMark0, -0.5f, 13.0f, 13.0f, 7, 8, 9, 0.4f, 0.5f); uint32_t fm = 0x7F7FFFFFu; memcpy(x + gl::kSMark0 - 4, &fm, 4); }   // an extra FLT_MAX in front: not a record
    uint8_t* r10 = plant(0x200003 + gl::kSMark0, -0.5f, 26.0f, 26.0f, 60002, 2, 0, 0.4f, 0.5f);
    uint8_t* ro = plant(kRw + 0x2001 + gl::kSMark0, -0.5f, 13.0f, 13.0f, 99, 1, 0, 0.4f, 0.5f);     // in the read-only part: must not be scanned
    DWORD old;
    VirtualProtect(base + kRw, kRo, PAGE_READONLY, &old);
    uint64_t sumBefore = 0;
    for (size_t i = 0; i < kRw; i += 8) sumBefore += *reinterpret_cast<uint64_t*>(base + i) * (i / 8 + 1);
    auto res = scanAll(lo, hi);
    uint64_t sumAfter = 0;
    for (size_t i = 0; i < kRw; i += 8) sumAfter += *reinterpret_cast<uint64_t*>(base + i) * (i / 8 + 1);
    need(sumBefore == sumAfter, "the scan must never write");
    const auto& recs = res.first;
    need(recs.size() == 4 && recs[0].keys[0] == 1021 && recs[1].keys[0] == 10253 && recs[2].keys[0] == 10253 && recs[3].keys[0] == 60002 && recs[1].S == reinterpret_cast<uint64_t>(r1) && recs[2].S == reinterpret_cast<uint64_t>(r2) &&
         recs[0].S == reinterpret_cast<uint64_t>(r3) && recs[3].S == reinterpret_cast<uint64_t>(r10), "exactly the 4 real records, sorted by keys then S (rejects, the read-only record and duplicates absent)");
    need(recs[0].launch[0] == 0.0f && recs[0].launch[1] == 40.0f && !recs[0].Zero() && recs[2].Zero() && !recs[2].angleOk && recs[1].angleOk && recs[1].angles[0] == 0.4f, "launch / angle fields and the zero flag");
    need(res.second.done && res.second.regionsSkipped >= 1 && res.second.rejected >= 3 && res.second.accepted == 4 && res.second.hits >= 7 && res.second.bytes >= kRw && res.second.regions >= 1, "scan statistics");
    (void)ro;
    // 3. resumable: tiny time budgets give the same answer
    {
        LaunchScan sc(lo, hi);
        int steps = 0;
        while (!sc.Step(1) && steps < 100000) ++steps;
        need(sc.Done() && steps >= 2 && sc.Records().size() == 4, "a scan resumed in many tiny steps finds the same records");
    }
    // 4. exclusion (our own probe copies): a record inside an excluded range is skipped, the rest are found
    auto ex = scanAll(lo, hi, {reinterpret_cast<uintptr_t>(r10) - 0x100, reinterpret_cast<uintptr_t>(r10) + 0x1000});
    need(ex.first.size() == 3 && ex.first[0].keys[0] == 1021 && ex.first[2].keys[0] == 10253, "an excluded range is not reported");
    // 5. the validator
    LaunchScanRec one;
    need(LaunchScanValidate(reinterpret_cast<uint64_t>(r1), one) && one.keys[1] == 1 && !LaunchScanValidate(reinterpret_cast<uint64_t>(r1) + 1, one) && !LaunchScanValidate(0x20000, one) && !LaunchScanValidate(0, one), "LaunchScanValidate");
    // 6. outputs: JSON (valid), grouped table, zero filter
    std::string json = LaunchScanJson(recs, res.second, "2026-01-01 00:00:00", 100000);
    need(TelemetryJsonValid(json) && json.find("\"count\":4") != std::string::npos && json.find("\"zero_count\":1") != std::string::npos && json.find("\"keys\":[10253,1,0,0]") != std::string::npos && json.find("\"truncated\":false") != std::string::npos &&
         TelemetryJsonValid(LaunchScanJson(recs, res.second, "x", 2)) && LaunchScanJson(recs, res.second, "x", 2).find("\"truncated\":true") != std::string::npos, "scan JSON");
    std::string grouped = LaunchScanTable(recs, 0, 300), zero = LaunchScanTable(recs, 2, 300), all = LaunchScanTable(recs, 1, 300);
    need(grouped.find("keys=[10253,1,0,0] records=2 launch: (0,0)x1 (13,13)x1") != std::string::npos && zero.find("keys=[10253,1,0,0] launch=(0,0)") != std::string::npos && zero.find("(13,13)") == std::string::npos &&
         all.find("keys=[60002,2,0,0] launch=(26,26)") != std::string::npos, "grouped / zero / all tables");
    // 7. the telemetry session JSON carries the scan
    {
        TelemetryStore st;
        TelInputs in;
        in.launchScan = json;
        std::string tj = st.BuildJson(in);
        TelInputs none;
        std::string tn = st.BuildJson(none);
        need(TelemetryJsonValid(tj) && tj.find("\"launch_scan\":{\"schema\":1") != std::string::npos && TelemetryJsonValid(tn) && tn.find("\"launch_scan\":null") != std::string::npos, "telemetry session JSON launch_scan section");
    }
    VirtualFree(base, 0, MEM_RELEASE);
    r.Add(ok, "launch_scan", ok ? "byte-wise scan of committed RW memory: planted records at odd S and at every chunk / page edge found once, rejects (S+gl::kSNeg, huge launch, extra marker), read-only memory skipped, no writes, resumable in tiny steps, exclusion, 0/0 flag, JSON / tables / telemetry section" : why);
}

// ---- SlowMo.MaxChainSeconds: a chain of slow-motion events must not stay amplified for ever
void TestChainCap(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    AmpEngine ae;
    ae.logNew = false;
    AmpParams p;
    p.scaleMul = 0.1f; p.playerOnly = false; p.maxChainMs = 300; p.chainGapMs = 150;   // scaled down for the test: 300 ms chain cap, 150 ms pause resets
    ae.SetParams(p);
    ae.SetEnabled(true);
    EvBlk D(0.3f, 0.333f, 0.333f, 0.167f);   // the Blinding Flash layout: scale 0.3, A 0.333, B 0.167
    EvArg ev(D.Ptr());
    auto isAmp = [&] { return FEq(D.Scale(), 0.3f * 0.1f); };
    auto isOrig = [&] { return FEq(D.Scale(), 0.3f) && FEq(D.A(), 0.333f) && FEq(D.B(), 0.167f); };
    auto fire = [&] { ae.OnEvent(ev.Ptr(), 0x5151); return ae.Tail(1); };
    // a chain of hits 50 ms apart: amplified until the cap, then native
    int amplified = 0, capped = 0;
    for (int i = 0; i < 14; ++i) {
        std::vector<AmpLogRec> lg = fire();
        if (isAmp()) ++amplified;
        else if (isOrig() && !lg.empty() && lg[0].reason == 6) ++capped;
        Sleep(50);
    }
    need(amplified >= 3 && amplified <= 9 && capped >= 4 && amplified + capped == 14, "a continuous chain is amplified until MaxChainSeconds and then left native (reason chain_cap)");
    need(ae.Stats().chainCap == static_cast<uint64_t>(capped) && std::string(AmpReasonName(6)) == "chain_cap", "chain_cap is counted and named");
    // after the cap the chain stays capped while hits keep coming (even a slower rhythm below the gap)
    Sleep(60);
    fire();
    need(isOrig(), "still the same chain (gap < chainGapMs): stays native");
    // a real pause ends the chain: amplified again
    Sleep(200);
    std::vector<AmpLogRec> lg = fire();
    need(isAmp() && !lg.empty() && lg[0].reason == 0, "a pause >= chainGapMs starts a new chain: amplified again");
    // maxChainMs = 0 -> no cap at all
    AmpParams q = ae.Params();
    q.maxChainMs = 0;
    ae.SetParams(q);
    int amp2 = 0;
    for (int i = 0; i < 10; ++i) { fire(); if (isAmp()) ++amp2; Sleep(40); }
    need(amp2 == 10, "MaxChainSeconds disabled (0): never capped");
    // the setting and its meaning: seconds in the table, ScaleMul unchanged
    CfgSnapshot snap = CfgTake();
    {
        CfgNotifyOff quiet;
        std::string e;
        need(CfgGet(SlowMo_MaxChainSeconds) == 6.0f && CfgDefOf(SlowMo_MaxChainSeconds).min == 1.0f && CfgDefOf(SlowMo_MaxChainSeconds).max == 30.0f && !CfgDefOf(SlowMo_MaxChainSeconds).advanced && CfgSet(SlowMo_MaxChainSeconds, 12.5f, e) &&
             !CfgSet(SlowMo_MaxChainSeconds, 0.5f, e) && !CfgSet(SlowMo_MaxChainSeconds, 31.0f, e) && CfgDefOf(SlowMo_ScaleMul).min == 0.05f && CfgDefOf(SlowMo_ScaleMul).max == 2.0f, "SlowMo.MaxChainSeconds 1..30 default 6 (ScaleMul range unchanged)");
    }
    CfgRestore(snap);
    r.Add(ok, "chain_cap", ok ? "chains of slow-motion events are amplified only for MaxChainSeconds of real time (capped events stay native, logged chain_cap, counted), a pause >= the gap starts a new chain, 0 disables it" : why);
}

// ---- Blinding Flash: chains started by the player's key-10276 cast, per-hit vs finale events, modes 1 and 2, any weapon (FinaleKeys), fallback, chain end, chain log
void TestBlindingFlash(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    alignas(16) static uint8_t Sb[0x200], A2b[0x200];
    uint64_t pS = reinterpret_cast<uint64_t>(Sb);
    memcpy(A2b + gl::kApplyS, &pS, 8);
    const uint64_t kP = 0x1000, kNpc = 0x9000;
    auto setf = [](EvBlk& D, float s, float a, float b) { D.Set(s, a, b); };
    EvBlk Dc1, Dc2, Dh, Df, Db, Do;
    auto resetD = [&] {
        setf(Dc1, 0.3f, 0.333f, 0.5f); setf(Dc2, 0.3f, 0.333f, 0.167f); setf(Dh, 0.3f, 0.333f, 0.167f); setf(Df, 0.3f, 0.333f, 0.5f);
        setf(Db, 0.99f, 0.0333f, 0.0333f); setf(Do, 0.6f, 0.2f, 0.333f);
    };
    auto orig = [&](EvBlk& D, float s, float a, float b) { return FEq(D.Scale(), s) && FEq(D.A(), a) && FEq(D.B(), b); };
    auto setup = [&](SkillEngine& se, AmpEngine& ae, int mode) {
        se.enabled = true;
        ae.logNew = false;
        ae.SetSkills(&se);
        AmpParams p;
        p.playerOnly = true; p.slowOn = false; p.contextWindowMs = 800; p.ignoreAbove = 0.9f;   // as AmpSyncFromSettings: 0.9..0.999 is the hit-stop class
        p.bfMode = mode; p.bfChainSpeed = 1.5f; p.bfHitScale = 0.5f; p.bfHitRamp = 1.5f; p.bfFinaleScale = 0.25f; p.bfFinaleRamp = 2.5f; p.bfFinaleKeys = {1023}; p.bfGapMs = 150; p.maxChainMs = 6000;
        ae.SetParams(p);
        ae.SetEnabled(true);
    };
    auto call = [&](SkillEngine& se, uint64_t a1, uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3) {
        uint32_t k[4] = {k0, k1, k2, k3};
        memcpy(Sb + gl::kSKeys, k, 16);
        SkillCallThrough(se, FakeApply, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<__int64>(A2b), 1);
    };
    auto fire = [&](AmpEngine& ae, EvBlk& D) {
        EvArg evp(D.Ptr());
        ae.OnEvent(evp.Ptr(), 1);
        return ae.Tail(1).back();
    };

    // ---- the cast is recorded by the skill engine, only for the player
    {
        SkillEngine se;
        se.enabled = true;
        call(se, kP, 1021, 3, 1104, 1);   // the first caller is the player
        need(se.BfCastUs() == 0, "no cast yet");
        call(se, kNpc, 10276, 1, 0, 0);
        need(se.BfCastUs() == 0, "an NPC's key-10276 call is not a cast");
        call(se, kP, 10276, 1, 0, 0);
        need(se.BfCastUs() > 0, "the player's key-10276 call is the cast");
    }

    // ---- mode 1: fast chain, slow finale (any weapon: per-hit = every non-finale event)
    {
        SkillEngine se; AmpEngine ae;
        resetD();
        setup(se, ae, 1);
        call(se, kP, 10276, 1, 0, 0);
        AmpLogRec a = fire(ae, Dc1);
        need(a.bfKind == 1 && a.reason == 0 && FEq(Dc1.Scale(), 1.5f) && FEq(Dc1.A(), 0.333f * 0.5f) && FEq(Dc1.B(), 0.5f * 0.5f), "mode 1: the cast's own event is a per-hit event at ChainSpeed (1.5, ABOVE 1), timings halved");
        fire(ae, Dc2);
        for (int i = 0; i < 3; ++i) { call(se, kP, 1021, 3, 1104, 1); fire(ae, Dh); }   // dual wield hits
        need(FEq(Dh.Scale(), 1.5f) && FEq(Dh.A(), 0.333f * 0.5f) && FEq(Dh.B(), 0.167f * 0.5f), "mode 1: per-hit events of a chain run at ChainSpeed");
        call(se, kP, 1021, 3, 1105, 1);   // another weapon's per-hit key tuple: still a per-hit event (no weapon-specific keys)
        setf(Dh, 0.3f, 0.333f, 0.167f);
        a = fire(ae, Dh);
        need(a.bfKind == 1 && FEq(Dh.Scale(), 1.5f), "any non-finale event of the chain is per-hit (greatsword tuple)");
        call(se, kP, 1023, 6, 1109, 1);   // the finale (throw-back hit)
        a = fire(ae, Df);
        need(a.bfKind == 2 && FEq(Df.Scale(), 0.3f * 0.25f) && FEq(Df.A(), 0.333f * 2.5f) && FEq(Df.B(), 0.5f * 2.5f), "the finale (ctx key0 1023) is slow motion: scale x FinaleScale, timings x FinaleRamp");
        a = fire(ae, Db);   // its hit-stop blip: untouched by Blinding Flash (HitStop is off here)
        need(a.bfKind == 0 && orig(Db, 0.99f, 0.0333f, 0.0333f), "the finale's own hit-stop blip stays under the HitStop settings");
        call(se, kP, 1021, 3, 1104, 1);
        a = fire(ae, Do);   // afterwards: the chain is over, the normal rules apply (SlowMo off here)
        need(a.bfKind == 0 && orig(Do, 0.6f, 0.2f, 0.333f), "after the finale the chain is over: normal rules (untouched with SlowMo off)");
        std::vector<BfChain> ch = ae.BfChains(10);
        need(ch.size() == 1 && ch[0].mode == 1 && ch[0].hits == 6 && ch[0].finales == 1 && ch[0].finaleSeen && !ch[0].noFinale && ch[0].end == "finale" && ch[0].evs.size() == 8 && ch[0].evs[0].keys[0] == 10276 && ch[0].evs[6].kind == 2 && ch[0].evs[7].kind == 3,
             "chain record: 6 per-hit, 1 finale, the blip as an untreated finale-key event, ended by the finale");
        AmpStats st = ae.Stats();
        need(st.bfHit == 6 && st.bfFinale == 1 && st.bfChains == 1 && st.bfNoFinale == 0, "Blinding Flash counters");
        std::string lg = ae.BfLogText(10);
        need(lg.find("10276/1/0/0 x2") != std::string::npos && lg.find("1021/3/1104/1 x3") != std::string::npos && lg.find("1021/3/1105/1") != std::string::npos && lg.find("1023/6/1109/1 [FINALE]") != std::string::npos &&
             lg.find("[finale key, untouched]") != std::string::npos && lg.find("end=finale") != std::string::npos, "`bf log`: ordered key tuples with run lengths and the finale marked");
        std::string js = ae.BfChainsJson(10);
        need(TelemetryJsonValid(js) && js.find("\"kind\":\"finale\"") != std::string::npos && js.find("\"keys\":[1021,3,1104,1]") != std::string::npos && js.find("\"end\":\"finale\"") != std::string::npos, "telemetry JSON of the chains");
        // ChainSpeed range: up to 2, down to 0.5, anything else is clamped
        for (float sp : {2.0f, 0.5f, 3.0f}) {
            AmpParams p = ae.Params(); p.bfChainSpeed = sp; ae.SetParams(p);
            call(se, kP, 10276, 1, 0, 0);
            setf(Dc1, 0.3f, 0.333f, 0.5f);
            fire(ae, Dc1);
            need(FEq(Dc1.Scale(), sp > 2.0f ? 2.0f : sp), "ChainSpeed 2.0 / 0.5 applied, 3.0 clamped to 2.0");
        }
        // the log lines of `amp log`
        need(ae.Tail(1).back().bfKind == 1, "amp log record carries bfKind");
    }

    // ---- mode 2: smooth slow motion per hit, slow finale
    {
        SkillEngine se; AmpEngine ae;
        resetD();
        setup(se, ae, 2);
        call(se, kP, 10276, 1, 0, 0);
        fire(ae, Dc1);
        need(FEq(Dc1.Scale(), 0.3f * 0.5f) && FEq(Dc1.A(), 0.333f * 1.5f) && FEq(Dc1.B(), 0.5f * 1.5f), "mode 2: per-hit = scale x HitScale, A and B x HitRamp");
        call(se, kP, 1021, 3, 1104, 1);
        fire(ae, Dh);
        need(FEq(Dh.Scale(), 0.3f * 0.5f) && FEq(Dh.B(), 0.167f * 1.5f), "mode 2: dual-wield hit");
        call(se, kP, 1023, 4, 1109, 1);
        AmpLogRec a = fire(ae, Df);
        need(a.bfKind == 2 && FEq(Df.Scale(), 0.3f * 0.25f) && FEq(Df.B(), 0.5f * 2.5f), "mode 2: the finale is slow motion too");
        // idempotent / no compounding: the same D fired again keeps the same values
        call(se, kP, 10276, 1, 0, 0);
        fire(ae, Dc1);
        fire(ae, Dc1);
        need(FEq(Dc1.Scale(), 0.3f * 0.5f) && FEq(Dc1.A(), 0.333f * 1.5f), "no compounding over repeated events");
    }

    // ---- fallback: no key-10276 start seen -> the normal amplifier decides
    {
        SkillEngine se; AmpEngine ae;
        resetD();
        setup(se, ae, 1);
        call(se, kP, 1021, 3, 1104, 1);
        AmpLogRec a = fire(ae, Dh);
        need(a.bfKind == 0 && orig(Dh, 0.3f, 0.333f, 0.167f) && ae.BfChains(5).empty(), "no cast seen: Blinding Flash does nothing (SlowMo off -> untouched)");
        AmpParams p = ae.Params(); p.slowOn = true; p.scaleMul = 0.5f; ae.SetParams(p);
        a = fire(ae, Dh);
        need(a.bfKind == 0 && FEq(Dh.Scale(), 0.3f * 0.5f), "no cast seen: the normal amplifier (SlowMo x0.5) applies");
        p.bfMode = 0; ae.SetParams(p);
        call(se, kP, 10276, 1, 0, 0);
        setf(Dc1, 0.3f, 0.333f, 0.5f);
        a = fire(ae, Dc1);
        need(a.bfKind == 0 && ae.BfChains(5).empty() && FEq(Dc1.Scale(), 0.3f * 0.5f), "mode 0: Blinding Flash is off even right after a cast");
    }

    // ---- chain end by timeout, finale-less chain (bf_nofinale), new cast, custom FinaleKeys, NPC / deny gating
    {
        SkillEngine se; AmpEngine ae;
        resetD();
        setup(se, ae, 1);
        call(se, kP, 10276, 1, 0, 0);
        fire(ae, Dc1);
        call(se, kP, 1021, 3, 1104, 1);
        fire(ae, Dh);
        Sleep(220);   // > bfGapMs: the chain times out
        call(se, kP, 1021, 3, 1104, 1);
        setf(Dh, 0.3f, 0.333f, 0.167f);
        AmpLogRec a = fire(ae, Dh);
        std::vector<BfChain> ch = ae.BfChains(10);
        need(a.bfKind == 0 && orig(Dh, 0.3f, 0.333f, 0.167f) && ch.size() == 1 && ch[0].end == "timeout" && ch[0].noFinale && !ch[0].finaleSeen && ae.Stats().bfNoFinale == 1, "1.5 s (here 150 ms) without a chain event ends the chain; no finale key = finale-less (bf_nofinale)");
        need(ae.BfLogText(5).find("NO (bf_nofinale)") != std::string::npos, "`bf log` marks the finale-less chain");
        // a new cast in the middle of a chain starts a new chain
        // (a long gap here: this check is about the new-cast rule, not the timeout, so a slow or loaded machine must not turn it into a timeout)
        { AmpParams pg = ae.Params(); pg.bfGapMs = 5000; ae.SetParams(pg); }
        call(se, kP, 10276, 1, 0, 0);
        setf(Dc1, 0.3f, 0.333f, 0.5f);
        fire(ae, Dc1);
        call(se, kP, 10276, 1, 0, 0);
        setf(Dc2, 0.3f, 0.333f, 0.167f);
        fire(ae, Dc2);
        ch = ae.BfChains(10);
        need(ch.size() == 3 && ch[1].end == "newcast" && ch[2].end.empty() && ch[2].mode == 1, "a new cast ends the running chain and starts another");
        // custom FinaleKeys: extend without a rebuild
        AmpParams p = ae.Params(); p.bfFinaleKeys = {1099}; ae.SetParams(p);
        call(se, kP, 1099, 2, 1111, 1);
        setf(Df, 0.3f, 0.333f, 0.5f);
        a = fire(ae, Df);
        need(a.bfKind == 2 && FEq(Df.Scale(), 0.3f * 0.25f), "FinaleKeys {1099}: another weapon's finale key is the finale");
        call(se, kP, 10276, 1, 0, 0);
        fire(ae, Dc1);
        call(se, kP, 1023, 6, 1109, 1);   // 1023 is no finale key any more: a per-hit event
        setf(Df, 0.3f, 0.333f, 0.5f);
        a = fire(ae, Df);
        need(a.bfKind == 1 && FEq(Df.Scale(), 1.5f), "with FinaleKeys {1099} a 1023 event is just a per-hit event");
        // gating: NPC events and DenyKeys never take part
        size_t before = ae.BfChains(1).back().evs.size();
        call(se, kNpc, 1021, 3, 1104, 1);
        setf(Dh, 0.3f, 0.333f, 0.167f);
        a = fire(ae, Dh);
        need(a.reason == 4 && a.bfKind == 0 && orig(Dh, 0.3f, 0.333f, 0.167f) && ae.BfChains(1).back().evs.size() == before, "an NPC's event is never part of a chain");
        p = ae.Params(); p.denyKeys = {7777}; ae.SetParams(p);
        call(se, kP, 7777, 1, 0, 0);
        a = fire(ae, Dh);
        need(a.reason == 2 && a.bfKind == 0 && orig(Dh, 0.3f, 0.333f, 0.167f), "DenyKeys still win inside a chain");
    }

    // ---- the chain cap in mode 2 (finale exempt); mode 1 is not capped
    {
        SkillEngine se; AmpEngine ae;
        resetD();
        setup(se, ae, 2);
        AmpParams p = ae.Params(); p.maxChainMs = 200; p.bfGapMs = 1500; ae.SetParams(p);
        call(se, kP, 10276, 1, 0, 0);
        fire(ae, Dc1);
        Sleep(260);
        call(se, kP, 1021, 3, 1104, 1);
        setf(Dh, 0.3f, 0.333f, 0.167f);
        AmpLogRec a = fire(ae, Dh);
        need(a.reason == 6 && a.bfKind == 0 && orig(Dh, 0.3f, 0.333f, 0.167f) && ae.Stats().chainCap == 1, "mode 2: per-hit events past MaxChainSeconds stay native (chain_cap)");
        call(se, kP, 1023, 6, 1109, 1);
        a = fire(ae, Df);
        need(a.bfKind == 2 && FEq(Df.Scale(), 0.3f * 0.25f), "the finale is exempt from the chain cap");
        need(ae.BfChains(1).back().capped == 1, "capped events are counted in the chain record");
    }
    r.Add(ok, "blinding_flash", ok ? "Blinding Flash chains: cast by key 10276 (player only), per-hit vs finale by ctx key0 / FinaleKeys (any weapon), mode 1 (ChainSpeed incl. >1 and clamp, finale slow) and mode 2 (per-hit slow, finale slower, chain cap), fallback without a cast, mode 0, timeout / newcast / finale end, finale-less chains, NPC / DenyKeys gating, `bf log`, telemetry JSON" : why);
}

// ---- src/gamelayout.h and the `layout check` helpers: every verdict is built from the constants, with fake blocks built from the same constants
void TestLayout(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    // the constants themselves: the build 26308950 values the live game confirmed
    need(gl::kBuild == 26308950 && gl::kTimeEventType == 125 && gl::kEvScale == 0x0C && gl::kEvScale2 == 0x10 && gl::kEvA == 0x14 && gl::kEvB == 0x18 && gl::kCtlObj[0] == 0xCD8 && gl::kCtlObj[1] == 0xCE8 && gl::kSKeys == 0x108 &&
         gl::kSHalf == 0x13C && gl::kSNeg == 0x140 && gl::kSMark0 == 0x144 && gl::kSMark1 == 0x148 && gl::kSMark2 == 0x14C && gl::kSStr0 == 0x15C && gl::kSStr1 == 0x160 && gl::kSAng0 == 0x16C && gl::kSAng1 == 0x170,
         "gamelayout.h must hold the verified values of build 26308950");
    // time-event blocks
    {
        EvBlk good(0.3f, 1.0f, 0.3333f, 0.1667f);
        LayoutVerdict v = LayoutCheckEventBlock(good.b, sizeof good.b);
        need(v.ok && v.text.find("OK: type 125") != std::string::npos, "a block built from the constants is OK");
        EvBlk blip(0.99f, 0.99f, 0.03333f, 0.03333f);
        need(LayoutCheckEventBlock(blip.b, sizeof blip.b).ok, "the hit-stop blip is OK");
        EvBlk oldType;
        oldType.Scale() = 0.3f; oldType.A() = 0.333f; oldType.B() = 0.167f;
        uint16_t t123 = 123;
        memcpy(oldType.b + gl::kEvType, &t123, 2);
        v = LayoutCheckEventBlock(oldType.b, sizeof oldType.b);
        need(!v.ok && v.text.find("is 123") != std::string::npos && v.text.find("expected 125") != std::string::npos, "the previous build's type id (123) is reported as a mismatch");
        // the previous build's layout (floats 4 bytes lower, type 123): type AND floats are reported, and the plausible floats are listed
        uint8_t oldLayout[0x40] = {};
        float sc = 0.3f, a = 0.333f, b = 0.167f;
        memcpy(oldLayout, &t123, 2);
        memcpy(oldLayout + 0x08, &sc, 4); memcpy(oldLayout + 0x10, &a, 4); memcpy(oldLayout + 0x14, &b, 4);
        v = LayoutCheckEventBlock(oldLayout, sizeof oldLayout);
        need(!v.ok && v.text.find("expected 125") != std::string::npos && v.text.find("+0x08=0.3") != std::string::npos && v.text.find("raw dwords") != std::string::npos, "an old-layout block: mismatch with the floats found at their old offsets");
        EvBlk nan;
        nan.Scale() = std::numeric_limits<float>::quiet_NaN();
        need(!LayoutCheckEventBlock(nan.b, sizeof nan.b).ok, "a NaN scale is not plausible");
    }
    // skill records
    {
        alignas(16) uint8_t rec[gl::kSCheckSpan] = {};
        auto putf = [&](uintptr_t off, float f) { memcpy(rec + off, &f, 4); };
        auto putu = [&](uintptr_t off, uint32_t u) { memcpy(rec + off, &u, 4); };
        const uint32_t keys[4] = {1002, 5, 1107, 1};
        memcpy(rec + gl::kSKeys, keys, 16);
        putf(gl::kSHalf, 0.5f); putf(gl::kSNeg, -0.4f);
        putu(gl::kSMark0, gl::kFltMaxBits); putu(gl::kSMark1, gl::kFltMaxBits); putu(gl::kSMark2, gl::kFltMaxBits);
        putf(gl::kSStr0, 12.5f); putf(gl::kSStr1, 12.5f); putf(gl::kSAng0, 0.436332f); putf(gl::kSAng1, 0.959931f);
        SkillRecCheck c = LayoutCheckSkillRecord(rec, sizeof rec);
        need(c.readable && c.keysPlausible && c.markers && c.negOk && c.strengthOk && c.markerAt == static_cast<int>(gl::kSMark0) && c.keys[0] == 1002 && c.keys[1] == 5 && c.keys[2] == 1107 && c.keys[3] == 1, "a record built from the constants passes every check");
        // a record in the PREVIOUS build's layout: keys at +0x100, markers at +0x13C: the markers are found at the old offset, the keys are not plausible at the new one
        alignas(16) uint8_t old[gl::kSCheckSpan] = {};
        const uint32_t pushKeys[4] = {7, 2, 0, 0};   // the push record
        memcpy(old + 0x100, pushKeys, 16);
        uint32_t fm = gl::kFltMaxBits;
        memcpy(old + 0x13C, &fm, 4); memcpy(old + 0x140, &fm, 4); memcpy(old + 0x144, &fm, 4);
        float s12 = 12.5f;
        memcpy(old + 0x148, &s12, 4); memcpy(old + 0x14C, &s12, 4);
        c = LayoutCheckSkillRecord(old, sizeof old);
        need(!c.markers && c.markerAt == 0x13C && !c.keysPlausible, "an old-layout record: markers reported at the old offset (shift -8), keys implausible at the new offset");
        alignas(16) uint8_t plain[gl::kSCheckSpan] = {};
        memcpy(plain + gl::kSKeys, keys, 16);
        c = LayoutCheckSkillRecord(plain, sizeof plain);
        need(c.keysPlausible && !c.markers && c.markerAt == -1, "a plain attack record has keys but no marker block");
        need(!LayoutCheckSkillRecord(rec, 0x40).readable, "a too short block is not readable");
    }
    // controller objects
    {
        const float idle[5] = {-1, -1, 0, 0, 0}, curve[5] = {0.1f, 1.0f, 0.38f, 1.4f, 0.2f}, pause[5] = {0.001f, 1.0f, 3600.0f, 1.0f, 0.0f}, junk[5] = {7e9f, 3, 2, 1, -5}, nan[5] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0};
        // B1: objects that are NOT time controllers must never classify as one (an event block, zeroed memory, a pointer table)
        EvBlk evAsCtl(0.99f, 0.99f, 0.0333f, 0.0333f);
        float asFloats[5];
        memcpy(asFloats, evAsCtl.b, 20);
        const float zeros[5] = {0, 0, 0, 0, 0}, ptrs[5] = {1.4e-42f, 3.0e-41f, 7.0e-44f, 9.0e-44f, 0.0f};
        need(!strcmp(LayoutClassifyCtl(asFloats), "unexpected") && !strcmp(LayoutClassifyCtl(zeros), "unexpected") && !strcmp(LayoutClassifyCtl(ptrs), "unexpected"),
             "an event block / zeroed memory / pointer-like bytes must not pass as a time-controller object");
        need(!strcmp(LayoutClassifyCtl(idle), "idle") && !strcmp(LayoutClassifyCtl(curve), "curve") && !strcmp(LayoutClassifyCtl(pause), "pause") && !strcmp(LayoutClassifyCtl(junk), "unexpected") && !strcmp(LayoutClassifyCtl(nan), "unexpected"), "controller object classification");
    }
    // the constants as JSON (telemetry) and `layout show`
    {
        std::string js = LayoutJson();
        need(TelemetryJsonValid(js) && js.find("\"build\":26308950") != std::string::npos && js.find("\"time_event_type\":125") != std::string::npos && js.find("\"ctl_obj\":[3288,3304]") != std::string::npos && js.find("\"s_keys\":264") != std::string::npos,
             "the layout JSON of the telemetry file");
        TelemetryStore st;
        TelInputs in;
        std::string tj = st.BuildJson(in);
        need(TelemetryJsonValid(tj) && tj.find("\"layout\":{\"build\":26308950") != std::string::npos, "the telemetry session JSON carries the layout section");
        need(CmdLayout(Args{"layout", "show"}).find("time event D = *(a3+0x8): type u16 at +0x0 == 125") != std::string::npos && CmdLayout(Args{"layout", "bogus"}).find("ERR") == 0, "`layout show` prints the constants");
    }
    // the amplifier's layout guard: a block with another type id is never touched (the old build's id 123 must not be written to)
    {
        AmpEngine ae;
        ae.logNew = false;
        AmpParams p;
        p.scaleMul = 0.5f; p.playerOnly = false;
        ae.SetParams(p);
        ae.SetEnabled(true);
        EvBlk wrong(0.6f, 0.3f, 0.5f, 0.2f), right(0.6f, 0.3f, 0.5f, 0.2f);
        uint16_t t123 = 123;
        memcpy(wrong.b + gl::kEvType, &t123, 2);
        EvArg ew(wrong.Ptr()), er(right.Ptr());
        ae.OnEvent(ew.Ptr(), 1);
        need(FEq(wrong.Scale(), 0.6f) && FEq(wrong.A(), 0.5f) && FEq(wrong.B(), 0.2f) && ae.Stats().badType == 1 && ae.Tail(1).empty(), "a block with the wrong type id is left alone and counted");
        ae.OnEvent(er.Ptr(), 1);
        need(FEq(right.Scale(), 0.6f * 0.5f) && ae.Stats().badType == 1, "a block with the right type id is amplified");
        auto rd = ae.RecentD(4);
        need(rd.size() == 2 && rd[0].first == reinterpret_cast<uintptr_t>(right.b) && rd[0].second && rd[1].first == reinterpret_cast<uintptr_t>(wrong.b) && !rd[1].second, "RecentD lists the newest blocks with their type verdict (for `layout check`)");
    }
    r.Add(ok, "layout", ok ? "gamelayout.h holds the build-26308950 values; layout-check helpers (event block: type id 125 / floats / old-build mismatch with hints; skill record: keys / marker block / shifted marker offset; controller object classes), layout JSON in the telemetry file, `layout show`, the amplifier's type-id guard and RecentD" : why);
}

// ---- hit-reaction override: key2 of the damage record patched for ONE call only (finale in a Blinding Flash chain, Turning Slash hits), player only
uint32_t g_rxSeenKey2 = 0;
bool g_rxThrow = false;
uint32_t g_rxGameWrites = 0;   // if nonzero the fake game rewrites key2 with this value during the call
__int64 __fastcall FakeReactApply(uint64_t*, __int64 a2) {
    uint64_t S = 0;
    memcpy(&S, reinterpret_cast<uint8_t*>(a2) + gl::kApplyS, 8);
    memcpy(&g_rxSeenKey2, reinterpret_cast<uint8_t*>(S) + gl::kSKey2, 4);   // what the game's hit resolution would read DURING the call
    if (g_rxGameWrites) memcpy(reinterpret_cast<uint8_t*>(S) + gl::kSKey2, &g_rxGameWrites, 4);
    if (g_rxThrow) throw 42;
    return 7;
}

void TestReaction(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    alignas(16) static uint8_t Sb[0x200], A2b[0x200];
    uint64_t pS = reinterpret_cast<uint64_t>(Sb);
    memcpy(A2b + gl::kApplyS, &pS, 8);
    const uint64_t kP = 0x1000, kNpc = 0x9000;
    SkillEngine se;
    se.enabled = true;
    ReactCfg cfg;
    cfg.finale = gl::kReactGroundThrow; cfg.finaleKeys[0] = 1023; cfg.nFinale = 1;
    cfg.turning = gl::kReactGroundThrow; cfg.tsKeys[0] = 10253; cfg.nTs = 1;
    cfg.tsWindowUs = 150000;
    se.reactCfg = &cfg;
    se.bfGapUs = 150000;   // scaled down for the test
    auto key2 = [&] { uint32_t v; memcpy(&v, Sb + gl::kSKey2, 4); return v; };
    auto call = [&](uint64_t a1, uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3 = 0) {
        uint32_t k[4] = {k0, k1, k2, k3};
        memcpy(Sb + gl::kSKeys, k, 16);
        g_rxSeenKey2 = 0;
        return SkillCallThrough(se, FakeReactApply, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<__int64>(A2b), 1);
    };
    call(kP, 1, 1, 0);   // the first caller is the player

    // ---- Blinding Flash finale: patched for that call only, only inside a chain
    call(kP, 1023, 6, gl::kReactPushHard);
    need(g_rxSeenKey2 == gl::kReactPushHard && se.reactFinale.load() == 0, "a finale-key call OUTSIDE a Blinding Flash chain is not touched");
    call(kP, 10276, 1, 0);                      // the cast opens the chain
    call(kP, 1021, 3, gl::kReactPushLight, 1);  // a per-hit call: not the finale
    need(g_rxSeenKey2 == gl::kReactPushLight && se.reactFinale.load() == 0, "per-hit calls of a chain are not touched");
    call(kP, 1023, 6, gl::kReactPushHard, 1);   // the finale
    need(g_rxSeenKey2 == gl::kReactGroundThrow && se.reactFinale.load() == 1, "the finale call sees the ground-throw reaction (1109) during the call");
    need(key2() == gl::kReactPushHard, "... and the shared record is back to its own reaction right after the call (no persistent change)");
    call(kP, 1023, 4, gl::kReactKnockDown, 1);
    need(g_rxSeenKey2 == gl::kReactKnockDown && se.reactFinale.load() == 1, "the finale ends the chain: a second 1023 call is not patched");
    // an enemy never gets it, even while the player's chain is open
    call(kP, 10276, 1, 0);
    call(kNpc, 1023, 6, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactPushHard && se.reactFinale.load() == 1, "an enemy's call is never touched (even with a finale key while the player's chain is open)");
    // the chain closes after the gap
    Sleep(230);
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactPushHard && se.reactFinale.load() == 1, "a chain that timed out (no player call within the gap) is closed: a late 1023 call is not patched");
    // a chain kept open by the player's hits
    call(kP, 10276, 1, 0);
    for (int i = 0; i < 4; ++i) { Sleep(60); call(kP, 1021, 3, gl::kReactPushLight, 1); }
    call(kP, 1023, 6, gl::kReactKnockDown, 1);
    need(g_rxSeenKey2 == gl::kReactGroundThrow && se.reactFinale.load() == 2, "a chain longer than the gap stays open while the hits keep coming");
    // already the target / other finale keys / reaction values
    call(kP, 10276, 1, 0);
    call(kP, 1023, 6, gl::kReactGroundThrow, 1);
    need(g_rxSeenKey2 == gl::kReactGroundThrow && se.reactFinale.load() == 2, "key2 already equal to the target: nothing to patch");
    cfg.finale = gl::kReactRagdoll;
    call(kP, 10276, 1, 0);
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactRagdoll && key2() == gl::kReactPushHard, "ragdoll (1110) value");
    cfg.finale = gl::kReactKnockDown;
    call(kP, 10276, 1, 0);
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactKnockDown && key2() == gl::kReactPushHard, "knock-down (1107) value");
    cfg.finale = gl::kReactGroundThrow;
    cfg.finaleKeys[0] = 1099;   // FinaleKeys extended live: another weapon's finale key
    call(kP, 10276, 1, 0);
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactPushHard, "1023 is no finale key any more");
    call(kP, 1099, 2, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactGroundThrow && key2() == gl::kReactPushHard, "the configured finale key is patched");
    cfg.finaleKeys[0] = 1023;

    // ---- Turning Slash: damage calls with key2 in {1104,1105,1107} within the window after the player's cast
    uint64_t t0 = se.reactTurning.load();
    call(kP, 1002, 1, gl::kReactKnockDown);   // no cast yet
    need(g_rxSeenKey2 == gl::kReactKnockDown && se.reactTurning.load() == t0, "a damage call without a Turning Slash cast is not touched");
    call(kP, 10253, 1, 0);                    // the cast
    need(se.reactTurning.load() == t0, "the cast itself (key2 0) is not patched");
    call(kP, 1002, 2, gl::kReactKnockDown);
    need(g_rxSeenKey2 == gl::kReactGroundThrow && key2() == gl::kReactKnockDown && se.reactTurning.load() == t0 + 1, "a 1107 damage call inside the window is patched for the call only");
    call(kP, 1091, 2, gl::kReactPushNormal, 1);
    need(g_rxSeenKey2 == gl::kReactGroundThrow && key2() == gl::kReactPushNormal, "1105 (greatsword gear hit) too");
    call(kP, 1092, 2, gl::kReactPushLight, 1);
    need(g_rxSeenKey2 == gl::kReactGroundThrow, "1104 too");
    call(kP, 1002, 2, gl::kReactPushHard);
    need(g_rxSeenKey2 == gl::kReactPushHard, "key2 1106 is not in the set: untouched");
    call(kP, 1002, 2, gl::kReactRagdoll);
    need(g_rxSeenKey2 == gl::kReactRagdoll, "key2 1110 is not in the set: untouched");
    call(kP, 1002, 2, 0);
    need(g_rxSeenKey2 == 0, "key2 0 is not a reaction: untouched");
    call(kNpc, 1002, 2, gl::kReactKnockDown);
    need(g_rxSeenKey2 == gl::kReactKnockDown, "an enemy's damage call inside the window is never touched");
    Sleep(220);
    call(kP, 1002, 2, gl::kReactKnockDown);
    need(g_rxSeenKey2 == gl::kReactKnockDown, "after the window (1.2 s, here 150 ms) the same call is untouched");
    call(kP, 10253, 1, 0);
    call(kP, 1002, 2, gl::kReactKnockDown);
    need(g_rxSeenKey2 == gl::kReactGroundThrow, "a new cast opens the window again");
    cfg.tsKeys[1] = 10254; cfg.nTs = 2;   // the fist variant's key added to Reaction.TurningSlashKeys
    Sleep(220);
    call(kP, 10254, 1, 0);
    call(kP, 1002, 3, gl::kReactKnockDown);
    need(g_rxSeenKey2 == gl::kReactGroundThrow, "an additional Turning Slash key starts the window as well");

    // ---- off = pass-through, no persistent change, exceptions, the game writing the field itself
    cfg.finale = 0; cfg.turning = 0;
    uint64_t f0 = se.reactFinale.load(), t1 = se.reactTurning.load();
    call(kP, 10276, 1, 0);
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    call(kP, 10253, 1, 0);
    call(kP, 1002, 2, gl::kReactKnockDown);
    need(g_rxSeenKey2 == gl::kReactKnockDown && se.reactFinale.load() == f0 && se.reactTurning.load() == t1, "Reaction off (0 / 0): pure pass-through");
    cfg.finale = gl::kReactGroundThrow; cfg.turning = gl::kReactGroundThrow;
    call(kP, 10276, 1, 0);
    g_rxThrow = true;
    bool caught = false;
    try { call(kP, 1023, 6, gl::kReactPushHard, 1); } catch (int) { caught = true; }
    g_rxThrow = false;
    need(caught && g_rxSeenKey2 == gl::kReactGroundThrow && key2() == gl::kReactPushHard, "an exception inside the original: the record's reaction is restored (unwound)");
    call(kP, 10276, 1, 0);
    g_rxGameWrites = 1999;
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    g_rxGameWrites = 0;
    need(g_rxSeenKey2 == gl::kReactGroundThrow && key2() == 1999, "if the game rewrote the field during the call, the restore does not clobber it");
    se.enabled = false;
    call(kP, 10276, 1, 0);
    call(kP, 1023, 6, gl::kReactPushHard, 1);
    need(g_rxSeenKey2 == gl::kReactPushHard, "a disabled skill engine is a pass-through");
    se.enabled = true;

    // ---- the settings
    CfgSnapshot snap = CfgTake();
    {
        CfgNotifyOff quiet;
        std::string e;
        need(CfgGet(Reaction_Finale) == 0.0f && CfgGet(Reaction_TurningSlash) == 0.0f && CfgDefOf(Reaction_Finale).max == 3.0f && CfgDefOf(Reaction_TurningSlashKeys).advanced && CfgList(Reaction_TurningSlashKeys) == std::vector<uint32_t>({10253}), "Reaction defaults: 0 / 0, keys {10253} ([Advanced])");
        ReactCfg none = ReactCfgFromSettings();
        need(none.finale == 0 && none.turning == 0, "off in the settings = off in the config");
        CfgSet(Reaction_Finale, 1.0f, e); CfgSet(Reaction_TurningSlash, 3.0f, e);
        ReactCfg c1 = ReactCfgFromSettings();
        need(c1.finale == gl::kReactKnockDown && c1.turning == gl::kReactRagdoll && c1.nFinale == 1 && c1.finaleKeys[0] == 1023 && c1.nTs == 1 && c1.tsKeys[0] == 10253, "settings -> 1 = 1107, 3 = 1110, keys from BlindingFlash.FinaleKeys / Reaction.TurningSlashKeys");
        CfgSet(Reaction_Finale, 2.0f, e);
        need(ReactCfgFromSettings().finale == gl::kReactGroundThrow && !CfgSet(Reaction_Finale, 4.0f, e) && !CfgSet(Reaction_TurningSlash, -1.0f, e), "2 = 1109; range 0..3");
        CfgPreset("cinematic", e);
        need(CfgGet(Reaction_Finale) == 2.0f && CfgGet(Reaction_TurningSlash) == 3.0f, "the cinematic preset keeps the reaction choices");
        CfgSet(Launch_MinValue, 25.0f, e);
        CfgPreset("extreme", e);
        need(CfgGet(Launch_MinValue) == 25.0f, "presets other than Vanilla keep Launch.MinValue");
        CfgPreset("vanilla", e);
        need(CfgGet(Reaction_Finale) == 0.0f && CfgGet(Reaction_TurningSlash) == 0.0f && CfgGet(Launch_MinValue) == 0.0f, "Vanilla (everything off) resets the reactions and the minimum launch");
    }
    CfgRestore(snap);
    r.Add(ok, "reaction", ok ? "hit-reaction override: finale only inside a Blinding Flash chain, Turning Slash window (1104/1105/1107, extra keys), player only, window / chain expiry, patched for the single call and restored (exception, game rewrite, already-target, off = pass-through), settings mapping and presets" : why);
}

// ---- custom slow-motion triggers: rule parsing, matching, burst / cooldown / last-hit / delay logic, gates, pause / busy / unreadable handling, with a FAKE controller
class FakeCtl : public TrigCtl {
public:
    float f[2][5] = {{-1, -1, 0, 0, 0}, {-1, -1, 0, 0, 0}};
    bool readable = true, writable = true;
    int writes = 0;
    bool Read(float o[2][5]) override { if (!readable) return false; memcpy(o, f, sizeof f); return true; }
    bool Write(const float v[5]) override { if (!writable) return false; memcpy(f[0], v, 20); memcpy(f[1], v, 20); ++writes; return true; }
    void Idle() { for (auto& o : f) { o[0] = o[1] = -1.0f; o[2] = o[3] = o[4] = 0.0f; } }   // the engine finished the curve by itself
};

void TestTriggers(Report& r) {
    bool ok = true;
    std::string why;
    auto need = [&](bool c, const char* w) { if (!c && ok) { why = w; ok = false; } };
    std::string e;
    TriggerRule p;
    // ---- parser / formatter
    need(TriggerParse("key=10276,0|owner=player|scale=0.15|in=0.10|hold=0.5|out=0.45|cooldownMs=1500|firstOnly=1|delayMs=0|name=BlindingFlash", p, e) && p.nkeys == 1 && p.keys[0] == 10276 && FEq(p.scale, 0.15f) &&
         FEq(p.in, 0.10f) && FEq(p.hold, 0.5f) && FEq(p.out, 0.45f) && p.cooldownMs == 1500 && p.firstOnly && !p.lastOnly && p.delayMs == 0 && !strcmp(p.name, "BlindingFlash") && p.enabled,
         "the documented rule line does not parse (key 0 must be an ignored placeholder)");
    need(TriggerParse("Trigger3 = key=1, 2 ,2|scale=0.3", p, e) && p.nkeys == 2 && p.keys[0] == 1 && p.keys[1] == 2 && FEq(p.scale, 0.3f), "'TriggerN =' prefix / spaces / duplicate keys");
    TriggerRule q;
    need(TriggerParse("name=X|enabled=0|key=7,8,9|scale=0.4|in=0.2|hold=0.3|out=0.6|cooldownMs=900|lastOnly=1|idleMs=300|delayMs=120", p, e) && p.lastOnly && !p.firstOnly && !p.enabled && p.idleMs == 300 &&
         TriggerParse(TriggerFormat(p), q, e) && !memcmp(&p, &q, sizeof p), "rule text must round-trip through TriggerFormat");
    const char* bad[] = {"scale=0.3", "key=0", "key=5|owner=any", "key=5|scale=5", "key=5|scale=0.01", "key=5|firstOnly=1|lastOnly=1", "key=5|in=5|hold=5|out=5", "key=5|in=0|hold=0|out=0", "key=5|bogus=1",
                         "key=abc", "key=5|name=bad;name", "key=5|idleMs=5", "key=5|cooldownMs=-1", "key=1,2,3,4,5,6,7,8,9", "key=5|preset=Nope", "key=5|delayMs=99999", "key"};
    bool allBad = true;
    for (const char* b : bad) { TriggerRule t; allBad = allBad && !TriggerParse(b, t, e) && !e.empty(); }
    need(allBad, "an invalid rule line was accepted");
    need(TriggerParse("key=5|owner=player", p, e) && p.firstOnly && !p.lastOnly, "defaults: firstOnly on, lastOnly off");

    // ---- engine with explicit time
    TriggerEngine te;
    FakeCtl ctl;
    te.enabled = true;
    const long long T0 = 10000000, MS = 1000;
    const uint64_t P = 0x1000, N = 0x2000;
    uint32_t kHit[4] = {5, 111, 0, 0}, kMiss[4] = {5, 6, 0, 0};
    TriggerRule a;
    need(TriggerParse("name=A|key=111,222|scale=0.2|in=0.1|hold=0.5|out=0.4|cooldownMs=1000|firstOnly=1", a, e) && te.Add(a, e) == 1, "add rule A");
    // only the player's own matching calls count
    te.OnSkill(kHit, N, P, T0); te.OnSkill(kHit, P, 0, T0); te.OnSkill(kMiss, P, P, T0); te.OnSkill(kHit, 0, P, T0);
    need(te.Pump(T0 + 1000 * MS, ctl, false) == 0 && ctl.writes == 0 && te.Stats().matches == 0, "a non-player / unknown-player / non-matching call must never fire");
    te.enabled = false; te.OnSkill(kHit, P, P, T0); te.enabled = true;
    need(te.Pump(T0 + 1000 * MS, ctl, false) == 0, "a disabled engine must not fire");
    // fires with the mapped curve (A = in + hold, B = out), age = call -> write
    te.OnSkill(kHit, P, P, T0);
    need(te.Pump(T0 + 2 * MS, ctl, false) == 1 && ctl.writes == 1, "a matching player call must start a curve");
    bool shape = true;
    for (int i = 0; i < 2; ++i) shape = shape && FEq(ctl.f[i][0], 0.2f) && ctl.f[i][1] == 1.0f && FEq(ctl.f[i][2], 0.6f) && FEq(ctl.f[i][3], 0.4f) && ctl.f[i][4] == 0.0f;
    need(shape, "curve must be (scale, 1.0, A = in+hold, B = out, 0) in BOTH objects");
    std::vector<TrigLogRec> lg = te.LogTail(1);
    need(lg.size() == 1 && lg[0].fired && lg[0].rule == 1 && lg[0].key == 111 && lg[0].a1 == P && !strcmp(lg[0].name, "A") && lg[0].ageUs == 2 * MS && !strcmp(lg[0].outcome, "FIRED"), "log record of a firing");
    // the controller still runs OUR curve: the next burst (after the cooldown) is skipped as busy
    te.OnSkill(kHit, P, P, T0 + 1100 * MS);
    need(te.Pump(T0 + 1101 * MS, ctl, false) == 0 && ctl.writes == 1 && te.Stats().skippedBusy == 1, "another time effect running -> skip (controller not idle)");
    ctl.Idle();
    te.OnSkill(kHit, P, P, T0 + 3000 * MS);
    need(te.Pump(T0 + 3001 * MS, ctl, false) == 1 && ctl.writes == 2, "idle controller after the cooldown -> fires again");
    // cooldown: a new burst 500 ms later is suppressed (cooldown 1000 ms)
    ctl.Idle();
    te.OnSkill(kHit, P, P, T0 + 3500 * MS);
    need(te.Pump(T0 + 3501 * MS, ctl, false) == 0 && te.Stats().cooldownSuppressed == 1, "cooldownMs must suppress a re-trigger");
    // firstOnly: a burst of 4 calls 100 ms apart fires once, on the first call
    ctl.Idle();
    for (int i = 0; i < 4; ++i) te.OnSkill(kHit, P, P, T0 + 5000 * MS + i * 100 * MS);
    need(te.Pump(T0 + 5400 * MS, ctl, false) == 1 && te.Stats().burstSuppressed == 3, "firstOnly: one curve per burst, on its first call");
    lg = te.LogTail(1);
    need(lg[0].ageUs == 400 * MS, "the age of the firing is measured from the first call of the burst (pumped later here)");
    // Override: fire even while another time effect (a native one) is active
    ctl.f[0][0] = ctl.f[1][0] = 0.3f; ctl.f[0][1] = ctl.f[1][1] = 1.0f; ctl.f[0][2] = ctl.f[1][2] = 0.9f;
    te.OnSkill(kHit, P, P, T0 + 8000 * MS);
    need(te.Pump(T0 + 8001 * MS, ctl, false) == 0, "native effect active: skipped without Override");
    te.allowOverride = true;
    te.OnSkill(kHit, P, P, T0 + 9500 * MS);
    need(te.Pump(T0 + 9501 * MS, ctl, false) == 1 && FEq(ctl.f[0][0], 0.2f), "Triggers.Override: fires over a running effect");
    te.allowOverride = false;
    // an engine that never resets one of the two objects: our own expired curve must not block the triggers forever
    {
        TriggerEngine t3;
        t3.enabled = true;
        TriggerRule ar;
        need(TriggerParse("key=111|scale=0.2|in=0.1|hold=0.1|out=0.1|cooldownMs=0|firstOnly=0", ar, e) && t3.Add(ar, e) == 1, "add stale-test rule");
        FakeCtl c3;
        t3.OnSkill(kHit, P, P, T0);
        need(t3.Pump(T0 + MS, c3, false) == 1, "stale test: first fire");
        for (int i = 0; i < 5; ++i) c3.f[1][i] = i == 0 ? 0.2f : c3.f[0][i];       // pretend the engine reset only object 1 ...
        for (int i = 0; i < 5; ++i) c3.f[0][i] = i < 2 ? -1.0f : 0.0f;
        t3.OnSkill(kHit, P, P, T0 + 100 * MS);   // object 2 still holds our curve while it is still running: busy
        need(t3.Pump(T0 + 101 * MS, c3, false) == 0 && t3.Stats().skippedBusy == 1, "stale test: a running curve of ours is busy");
        t3.OnSkill(kHit, P, P, T0 + 900 * MS);   // ... and once A + B + 300 ms have passed it is just a leftover
        need(t3.Pump(T0 + 901 * MS, c3, false) == 1, "stale test: an expired leftover of ours must not block the next trigger");
        // a DIFFERENT (native) curve in an object is never treated as ours
        for (int i = 0; i < 2; ++i) { c3.f[i][0] = 0.5f; c3.f[i][1] = 1.0f; c3.f[i][2] = 0.3f; c3.f[i][3] = 0.3f; c3.f[i][4] = 0.0f; }
        t3.OnSkill(kHit, P, P, T0 + 5000 * MS);
        need(t3.Pump(T0 + 5001 * MS, c3, false) == 0 && t3.Stats().skippedBusy == 2, "stale test: a native curve stays busy");
    }
    // pause active / controller unreadable / write failure
    ctl.Idle();
    uint64_t sp0 = te.Stats().skippedPause;
    te.OnSkill(kHit, P, P, T0 + 12000 * MS);
    int w0 = ctl.writes;
    need(te.Pump(T0 + 12001 * MS, ctl, true) == 0 && te.Stats().skippedPause == sp0 + 1 && ctl.writes == w0, "pause active: never fire");
    ctl.readable = false;
    te.OnSkill(kHit, P, P, T0 + 14000 * MS);
    need(te.Pump(T0 + 14001 * MS, ctl, false) == 0 && te.Stats().skippedUnreadable == 1, "unreadable controller chain: nothing written");
    ctl.readable = false; ctl.writable = false;
    ctl.readable = true;
    te.OnSkill(kHit, P, P, T0 + 16000 * MS);
    need(te.Pump(T0 + 16001 * MS, ctl, false) == 0 && te.Stats().writeFailed == 1, "write failure is counted, not fatal");
    ctl.writable = true;
    // delayMs
    TriggerRule b;
    need(TriggerParse("name=B|key=333|delayMs=300|cooldownMs=0|firstOnly=0|scale=0.5|in=0.1|hold=0|out=0.2", b, e) && te.Add(b, e) == 2, "add rule B");
    uint32_t kB[4] = {333, 0, 0, 0};
    ctl.Idle();
    te.OnSkill(kB, P, P, T0 + 20000 * MS);
    need(te.NextDueUs(T0 + 20000 * MS) == 0 && te.Pump(T0 + 20000 * MS, ctl, false) == 0 && te.NextDueUs(T0 + 20000 * MS) == 300 * MS, "NextDueUs: 0 while a request is queued, then the delay");
    need(te.Pump(T0 + 20299 * MS, ctl, false) == 0 && te.Pump(T0 + 20300 * MS, ctl, false) == 1 && FEq(ctl.f[0][0], 0.5f) && FEq(ctl.f[0][2], 0.1f) && FEq(ctl.f[0][3], 0.2f), "delayMs holds the curve back exactly that long");
    lg = te.LogTail(1);
    need(lg[0].ageUs == 300 * MS && lg[0].rule == 2, "age includes the delay");
    // every-call mode (firstOnly=0, cooldown 0): each call fires
    ctl.Idle();
    te.OnSkill(kB, P, P, T0 + 21000 * MS); te.OnSkill(kB, P, P, T0 + 21010 * MS);
    need(te.Pump(T0 + 21400 * MS, ctl, false) >= 1, "firstOnly=0 fires on every matching call");
    // lastOnly: fires when idleMs pass without a further matching call, once per chain
    TriggerRule c;
    need(TriggerParse("name=C|key=444|lastOnly=1|idleMs=250|cooldownMs=0|scale=0.25|in=0.1|hold=0.1|out=0.3", c, e) && te.Add(c, e) == 3, "add rule C");
    uint32_t kC[4] = {0, 444, 0, 0};
    ctl.Idle();
    for (int i = 0; i < 3; ++i) te.OnSkill(kC, P, P, T0 + 30000 * MS + i * 100 * MS);
    need(te.Pump(T0 + 30200 * MS + 249 * MS, ctl, false) == 0, "lastOnly: nothing while the chain is still going");
    need(te.NextDueUs(T0 + 30200 * MS + 249 * MS) == 1 * MS, "NextDueUs for a pending last-hit rule");
    need(te.Pump(T0 + 30200 * MS + 250 * MS, ctl, false) == 1 && FEq(ctl.f[0][0], 0.25f) && te.Pump(T0 + 31000 * MS, ctl, false) == 0, "lastOnly: exactly one curve, after idleMs, on the last hit");
    lg = te.LogTail(1);
    need(lg[0].rule == 3 && lg[0].ageUs == 250 * MS, "lastOnly age is measured from the LAST call");
    ctl.Idle();
    te.OnSkill(kC, P, P, T0 + 40000 * MS);
    need(te.Pump(T0 + 40200 * MS, ctl, false) == 0, "lastOnly: not yet");
    te.OnSkill(kC, P, P, T0 + 40200 * MS);   // a new matching call resets the idle timer
    need(te.Pump(T0 + 40400 * MS, ctl, false) == 0 && te.Pump(T0 + 40450 * MS, ctl, false) == 1, "lastOnly: a further matching call restarts the wait");
    // a lastOnly rule is still limited by its cooldown
    TriggerRule cd;
    need(TriggerParse("name=CD|key=445|lastOnly=1|idleMs=100|cooldownMs=5000", cd, e) && te.Add(cd, e) == 4, "add rule CD");
    uint32_t kCD[4] = {445, 0, 0, 0};
    ctl.Idle();
    te.OnSkill(kCD, P, P, T0 + 50000 * MS);
    need(te.Pump(T0 + 50100 * MS, ctl, false) == 1, "lastOnly first chain fires");
    ctl.Idle();
    te.OnSkill(kCD, P, P, T0 + 52000 * MS);
    need(te.Pump(T0 + 52100 * MS, ctl, false) == 0, "lastOnly respects cooldownMs");
    te.Del(4);
    // per-rule enable
    TriggerRule g;
    need(TriggerParse("name=G|key=555|cooldownMs=0", g, e) && te.Add(g, e) == 4, "add rule G");
    uint32_t kG[4] = {555, 0, 0, 0};
    ctl.Idle();
    te.OnSkill(kG, P, P, T0 + 61000 * MS);
    need(te.Pump(T0 + 61001 * MS, ctl, false) == 1 && te.ActiveCount() == 4, "rule G fires");
    ctl.Idle();
    need(te.SetRuleEnabled(4, false) && te.ActiveCount() == 3, "disable one rule");
    te.OnSkill(kG, P, P, T0 + 62000 * MS);
    need(te.Pump(T0 + 62001 * MS, ctl, false) == 0, "a disabled rule must not fire");
    // del / clear / list / capacity
    need(te.Del(4) && !te.Del(4) && te.RuleCount() == 3 && te.List().size() == 3, "del / list");
    te.Clear();
    need(te.RuleCount() == 0 && te.ActiveCount() == 0, "clear");
    bool cap = true;
    for (int i = 0; i < kTrigMax; ++i) cap = cap && te.Add(a, e) == i + 1;
    need(cap && te.Add(a, e) == 0 && !e.empty(), "at most 16 rules");
    te.Clear();
    // `trigger fire`: validation, pause / busy handling
    ctl.Idle();
    std::string out;
    need(!te.FireNow(0.01f, 0.5f, 0.5f, ctl, false, false, T0, out) && !te.FireNow(0.5f, 6.0f, 6.0f, ctl, false, false, T0, out) && !te.FireNow(0.5f, 0.0f, 0.0f, ctl, false, false, T0, out), "manual fire argument validation");
    int w1 = ctl.writes;
    need(te.FireNow(0.3f, 0.2f, 0.4f, ctl, false, false, T0, out) && ctl.writes == w1 + 1 && FEq(ctl.f[1][3], 0.4f), "manual fire writes the curve");
    need(!te.FireNow(0.3f, 0.2f, 0.4f, ctl, false, false, T0, out) && out.find("not idle") != std::string::npos && te.FireNow(0.3f, 0.2f, 0.4f, ctl, false, true, T0, out) && !te.FireNow(0.3f, 0.2f, 0.4f, ctl, true, true, T0, out),
         "manual fire: busy controller needs `force`, an active pause is never overridden");
    // ---- key:slot rules and the built-in TowerArrow rule
    {
        uint32_t kArrow[4] = {1091, 5, 0, 0}, kFire[4] = {1091, 2, 1105, 0}, kOdd[4] = {5, 1091, 0, 0};
        TriggerRule slotRule, plainRule;
        need(TriggerParse("name=S|key=1091:5|cooldownMs=0|firstOnly=0", slotRule, e) && slotRule.nkeys == 1 && slotRule.keys[0] == KeyEnc(1091, 5) && TriggerFormat(slotRule).find("key=1091:5|") != std::string::npos &&
             TriggerParse("key=1091,0|cooldownMs=0|firstOnly=0", plainRule, e) && plainRule.keys[0] == KeyEnc(1091, -1) && plainRule.nkeys == 1, "key:slot and plain keys parse; 0 stays an ignored placeholder");
        TriggerRule rt;
        need(TriggerParse(TriggerFormat(slotRule), rt, e) && !memcmp(&rt, &slotRule, sizeof rt) && !TriggerParse("key=1091:", rt, e) && !TriggerParse("key=1091:300", rt, e) && !TriggerParse("key=0:5", rt, e) && !TriggerParse("key=a:5", rt, e), "key:slot round trip / rejects");
        TriggerEngine t4;
        FakeCtl c4;
        t4.enabled = true;
        need(t4.Add(slotRule, e) == 1, "add key:slot rule");
        t4.OnSkill(kArrow, P, P, T0); need(t4.Pump(T0 + MS, c4, false) == 1, "1091:5 fires on the explosive-arrow record");
        c4.Idle(); t4.OnSkill(kFire, P, P, T0 + 5000 * MS); need(t4.Pump(T0 + 5001 * MS, c4, false) == 0, "1091:5 must NOT fire on the greatsword fire hit (1091/2/1105)");
        c4.Idle(); t4.OnSkill(kOdd, P, P, T0 + 6000 * MS); need(t4.Pump(T0 + 6001 * MS, c4, false) == 0, "key:slot needs the key in the FIRST slot");
        t4.Clear();
        need(t4.Add(plainRule, e) == 1, "add plain rule");
        c4.Idle(); t4.OnSkill(kArrow, P, P, T0 + 10000 * MS); need(t4.Pump(T0 + 10001 * MS, c4, false) == 1, "old plain rule: arrow");
        c4.Idle(); t4.OnSkill(kFire, P, P, T0 + 11000 * MS); need(t4.Pump(T0 + 11001 * MS, c4, false) == 1, "old plain rule: still matches the fire hit (any of the 4 keys: unchanged behaviour)");
        c4.Idle(); t4.OnSkill(kOdd, P, P, T0 + 12000 * MS); need(t4.Pump(T0 + 12001 * MS, c4, false) == 1, "old plain rule: still matches the key in another slot");
        t4.Clear();
        // the built-in rule: exactly the live-tested curve, only the arrow record, own switch, never saved as a user rule
        TriggerRule tw = TriggerTowerRule(0.1f, 0.03f, 0.35f, 1.4f, 3000);
        need(tw.builtin && !strcmp(tw.name, "TowerArrow") && tw.nkeys == 1 && tw.keys[0] == KeyEnc(1091, 5) && FEq(tw.scale, 0.1f) && FEq(tw.in, 0.03f) && FEq(tw.hold, 0.35f) && FEq(tw.out, 1.4f) && tw.cooldownMs == 3000 &&
             tw.firstOnly && !tw.lastOnly && tw.delayMs == 0 && tw.enabled, "TowerArrow rule fields (key 1091:5 scale 0.1 in 0.03 hold 0.35 out 1.4 cooldown 3000 firstOnly)");
        need(t4.SetBuiltin(tw) && !t4.SetBuiltin(tw) && t4.RuleCount() == 1 && t4.List()[0].first == kTrigBuiltin && t4.List()[0].second.builtin, "SetBuiltin: slot 17, unchanged rule keeps its state");
        t4.userOn = false; t4.towerOn = false;
        t4.OnSkill(kArrow, P, P, T0 + 20000 * MS);
        need(t4.Pump(T0 + 20001 * MS, c4, false) == 0 && t4.ActiveCount() == 0, "TowerBlast off: the built-in rule never fires");
        t4.towerOn = true;
        c4.Idle(); t4.OnSkill(kArrow, P, P, T0 + 21000 * MS);
        need(t4.Pump(T0 + 21001 * MS, c4, false) == 1 && t4.ActiveCount() == 1 && FEq(c4.f[0][0], 0.1f) && FEq(c4.f[0][2], tw.in + tw.hold) && FEq(c4.f[0][3], 1.4f) && c4.f[1][4] == 0.0f, "TowerBlast on (user rules off): the arrow starts (0.1, 1, A = 0.03 + 0.35, B = 1.4, 0) in both objects");
        c4.Idle(); t4.OnSkill(kFire, P, P, T0 + 30000 * MS);
        need(t4.Pump(T0 + 30001 * MS, c4, false) == 0, "TowerBlast ignores the greatsword fire hit 1091/2/1105");
        c4.Idle(); t4.OnSkill(kArrow, P, P, T0 + 22000 * MS);
        need(t4.Pump(T0 + 22001 * MS, c4, false) == 0 && t4.Stats().cooldownSuppressed >= 1, "cooldown 3000 ms between two tower slow motions");
        c4.Idle(); t4.OnSkill(kArrow, P, P, T0 + 40000 * MS);
        need(t4.Pump(T0 + 40001 * MS, c4, false) == 1, "... and fires again after it");
        need(!t4.Del(kTrigBuiltin) && !t4.SetRuleEnabled(kTrigBuiltin, false), "the built-in rule cannot be deleted or disabled through the user-rule commands");
        t4.Clear();
        need(t4.RuleCount() == 1, "`trigger clear` leaves the built-in rule");
        // user rules and the built-in rule are gated separately
        need(t4.Add(plainRule, e) == 1, "add user rule next to the built-in one");
        t4.towerOn = false; t4.userOn = true;
        c4.Idle(); t4.OnSkill(kArrow, P, P, T0 + 50000 * MS);
        need(t4.Pump(T0 + 50001 * MS, c4, false) == 1 && t4.Stats().fired >= 1, "user rules on / TowerBlast off: only the user rule fires");
        TriggerRule bad = tw;
        bad.out = 1.0f;
        need(t4.SetBuiltin(bad) && t4.List().size() == 2, "changing the curve settings rewrites the built-in rule");
    }
    // ---- integration: the apply-skill detour feeds the engine (private skill engine, fake apply function, REAL clock)
    {
        TriggerEngine t2;
        t2.enabled = true;
        TriggerRule hit;
        need(TriggerParse("name=I|key=10253|cooldownMs=0|firstOnly=0|scale=0.3|in=0.1|hold=0.1|out=0.2", hit, e) && t2.Add(hit, e) == 1, "add integration rule");
        SkillEngine se;
        se.enabled = true;
        se.trig = &t2;
        alignas(16) uint8_t Sb[0x200] = {};
        alignas(16) uint8_t A2[0x200] = {};
        const uint32_t kd[4] = {7, 10253, 0, 0};
        memcpy(Sb + gl::kSKeys, kd, 16);
        uint64_t pS = reinterpret_cast<uint64_t>(Sb);
        memcpy(A2 + gl::kApplyS, &pS, 8);
        const uint64_t kOwner = 0x22B8204FC00ull, kOther = 0x99ull;
        FakeCtl c2;
        SkillCallThrough(se, FakeApply, reinterpret_cast<uint64_t*>(kOwner), reinterpret_cast<__int64>(A2), 0x1);   // the first caller becomes the player at once
        need(t2.Pump(NowUs() + 1000, c2, false) == 1 && c2.writes == 1 && FEq(c2.f[0][0], 0.3f) && FEq(c2.f[0][2], 0.2f), "the apply-skill detour must feed the trigger engine (keys at S+0x100..0x10C, any of the 4)");
        c2.Idle();
        SkillCallThrough(se, FakeApply, reinterpret_cast<uint64_t*>(kOther), reinterpret_cast<__int64>(A2), 0x1);   // another character, same skill: never
        need(t2.Pump(NowUs() + 1000, c2, false) == 0 && c2.writes == 1, "another character's skill must never fire a trigger");
        se.trig = nullptr;
        SkillCallThrough(se, FakeApply, reinterpret_cast<uint64_t*>(kOwner), reinterpret_cast<__int64>(A2), 0x1);
        need(t2.Pump(NowUs() + 1000, c2, false) == 0, "an engine without a trigger engine (tests) never fires");
    }
    r.Add(ok, "triggers", ok ? "rule parser/formatter (+ rejects), player-only matching, curve mapping A=in+hold B=out into both objects, firstOnly bursts, cooldown, delayMs, lastOnly (idle timer, reset, cooldown), gates, Override, pause / busy / unreadable / write failure, manual fire, capacity, apply-skill detour integration (fake controller)" : why);
}

}  // namespace

std::string CmdSelfTest() {
    Report r;
    r.out += F("flags: write=%d trace=%d watch=%d input=%d call=%d\n", g_cfg.allowWrite, g_cfg.allowTrace, g_cfg.allowWatch, g_cfg.allowInput, g_cfg.allowCall);
    TestSigs(r);
    TestHeader(r);
    TestWrite(r);
    TestTrace(r);
    TestWatch(r);
    TestAmp(r);
    TestSkills(r);
    TestEtrace(r);
    TestRaw(r);
    TestPatch(r);
    TestSettings(r);
    TestPlayer(r);
    TestLaunch(r);
    TestHitStopAngle(r);
    TestUiModel(r);
    TestUiInput(r);
    TestPauseRetry(r);
    TestTelemetry(r);
    TestTriggers(r);
    TestGimmick(r);
    TestLaunchMinDeny(r);
    TestLaunchScan(r);
    TestChainCap(r);
    TestBlindingFlash(r);
    TestLayout(r);
    TestReaction(r);
    if (r.fail) return r.out + F("ERR selftest: %d of %d checks failed\n", r.fail, r.pass + r.fail);
    return Ok(r.out + F("selftest: %d passed\n", r.pass));
}
