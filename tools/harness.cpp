#define _CRT_SECURE_NO_WARNINGS
// Test host for CombatMod (NOT the game). Built as build\harness\CrimsonDesert.exe so the plugin's host check passes.
// Loads CombatMod.asi, then keeps a worker thread writing a counter and calling a function, so trace/watch/write/call can be tested.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../src/gamelayout.h"   // every game-structure offset of the stand-in comes from here, like the plugin's

alignas(8) volatile int g_counter = 0;
float g_float = 0.6f;

#pragma optimize("", off)
extern "C" __declspec(noinline) __int64 HarnessFn(__int64 a, __int64 b) { return a * 10 + b; }
#pragma optimize("", on)

// Stand-in for the game's time_curve_start: reads the event data D = *(a3+8) and records what it saw.
struct EvBlock {   // a time-event data block D: type u16 at D+kEvType, floats at kEvScale / kEvScale2 / kEvA / kEvB
    alignas(16) unsigned char b[0x40];
    EvBlock(float s, float s2, float a, float bb) {
        memset(b, 0, sizeof b);
        unsigned short t = (unsigned short)gl::kTimeEventType;
        memcpy(b + gl::kEvType, &t, 2);
        memcpy(b + gl::kEvScale, &s, 4); memcpy(b + gl::kEvScale2, &s2, 4); memcpy(b + gl::kEvA, &a, 4); memcpy(b + gl::kEvB, &bb, 4);
    }
};
EvBlock g_D(0.6f, 0.3f, 0.5f, 0.2f);
unsigned long long g_ev[2] = {0, (unsigned long long)g_D.b};   // the hook's a3: the pointer sits at gl::kEvDataInArgs (8)
static_assert(gl::kEvDataInArgs == 8, "g_ev / g_ev2 are {0, D} pairs");
// a "hit-stop blip": scale 0.99, timings A = B = 0.0333 (the native impact pause)
EvBlock g_D2(0.99f, 0.99f, 0.0333f, 0.0333f);
unsigned long long g_ev2[2] = {0, (unsigned long long)g_D2.b};
volatile float g_used[3];
#pragma optimize("", off)
extern "C" __declspec(noinline) __int64* HarnessCurve(__int64 a1, unsigned* a2, __int64 a3, __int64 a4, void** a5) {
    unsigned char* D = (unsigned char*)*(unsigned long long*)(a3 + gl::kEvDataInArgs);
    g_used[0] = *(float*)(D + gl::kEvScale); g_used[1] = *(float*)(D + gl::kEvA); g_used[2] = *(float*)(D + gl::kEvB);
    (void)a2; (void)a4; (void)a5;
    return (__int64*)a1;
}
#pragma optimize("", on)

// Stand-in for apply_attack_skill_to_position: __int64 f(_QWORD* a1, __int64 a2); S = *(a2+0x70); keys at S+0x100.
volatile float g_lseen[2];
#pragma optimize("", off)
extern "C" __declspec(noinline) __int64 HarnessApply(unsigned long long* a1, __int64 a2) {
    unsigned long long S = *(unsigned long long*)(a2 + gl::kApplyS);
    volatile unsigned k = *(unsigned*)(S + gl::kSKeys);
    g_lseen[0] = *(float*)(S + gl::kSStr0); g_lseen[1] = *(float*)(S + gl::kSStr1);   // what the "engine" sees of the launch record (persistent boost)
    return (__int64)(k + (unsigned long long)a1);
}
#pragma optimize("", on)

// Stand-in for the event dispatcher sub_1404F42B0: D = *(a3+8), type = *(u16*)D, H = *(a1 + 8*type + 16), fn = *(*H + 8).
#pragma optimize("", off)
extern "C" __declspec(noinline) unsigned* HarnessHandlerA(unsigned long long H, unsigned* a2, __int64 a3, __int64 a4, unsigned long long* a5) { (void)H; (void)a3; (void)a4; (void)a5; return a2; }
extern "C" __declspec(noinline) unsigned* HarnessHandlerB(unsigned long long H, unsigned* a2, __int64 a3, __int64 a4, unsigned long long* a5) { (void)H; (void)a3; (void)a4; (void)a5; return a2 + 1; }
extern "C" __declspec(noinline) unsigned* HarnessDispatch(__int64 a1, unsigned* a2, __int64 a3, __int64 a4, unsigned long long* a5) {
    unsigned long long D = *(unsigned long long*)(a3 + gl::kEvDataInArgs);
    unsigned short type = *(unsigned short*)(D + gl::kEvType);
    unsigned long long H = *(unsigned long long*)(a1 + gl::kEvTableStride * type + gl::kEvTableBase);
    unsigned long long fn = *(unsigned long long*)(*(unsigned long long*)H + gl::kHandlerFnInVtable);
    return ((unsigned* (__fastcall*)(unsigned long long, unsigned*, __int64, __int64, unsigned long long*))fn)(H, a2, a3, a4, a5);
}
#pragma optimize("", on)
unsigned long long g_vtA[2] = {0, (unsigned long long)&HarnessHandlerA}, g_vtB[2] = {0, (unsigned long long)&HarnessHandlerB};
unsigned long long g_HA[2] = {(unsigned long long)g_vtA, 0}, g_HB[2] = {(unsigned long long)g_vtB, 0};
unsigned long long g_tbl[32];

// Each worker plays "owner" id (a1 = 0x1000 * id) and alternates skill keys 10253 / 10258; it fires an amp event right after each skill.
static DWORD WINAPI Worker(LPVOID arg) {
    unsigned id = (unsigned)(unsigned long long)arg;
    alignas(16) unsigned char Sb[0x200] = {};
    alignas(16) unsigned char A2[0x200] = {};
    *(unsigned long long*)(A2 + gl::kApplyS) = (unsigned long long)Sb;
    *(unsigned long long*)(A2 + gl::kApplyO160) = 0xABC00000ull + id;
    *(float*)(Sb + gl::kSHalf) = 0.5f;
    *(float*)(Sb + gl::kSNeg) = -0.5f;   // a real launch record has S+kSNeg in [-1, 0)
    *(unsigned*)(Sb + gl::kSMark0) = gl::kFltMaxBits; *(unsigned*)(Sb + gl::kSMark1) = gl::kFltMaxBits; *(unsigned*)(Sb + gl::kSMark2) = gl::kFltMaxBits;   // launch-record markers
    *(float*)(Sb + gl::kSStr0) = 15.0f; *(float*)(Sb + gl::kSStr1) = 15.0f;
    unsigned n = 0;
    for (;;) {
        g_counter = g_counter + 1;
        HarnessFn(g_counter, 2);
        *(unsigned*)(Sb + gl::kSKeys) = (n++ % 2) ? 10253 : 10258;
        *(unsigned*)(Sb + gl::kSKeys + 4) = id;
        HarnessApply((unsigned long long*)(0x1000ull * id), (__int64)A2);
        HarnessCurve(7, nullptr, (__int64)g_ev, 0, nullptr);
        HarnessCurve(7, nullptr, (__int64)g_ev2, 0, nullptr);
        {   // an event through the dispatcher: type 3 -> handler A, type 7 -> handler B
            alignas(16) unsigned char Ed[128] = {};
            unsigned short type = (n % 2) ? 3 : 7;
            *(unsigned short*)Ed = type;
            *(float*)(Ed + 8) = 0.25f * (float)id; *(float*)(Ed + 12) = 0.5f; *(unsigned*)(Ed + 40) = n;
            unsigned long long ev[2] = {0, (unsigned long long)Ed}, a5v = 0xC0DE0000ull + id;
            unsigned dummy[2] = {0, 0};
            HarnessDispatch((__int64)g_tbl, dummy, (__int64)ev, 0, &a5v);
        }
        Sleep(20);
    }
}

// ---- scripted fake pad: xinput1_4.dll (fake_xinput.cpp) next to the exe is loaded BEFORE the plugin; pad.txt = "<buttons> <lx> <connected>"
typedef DWORD(WINAPI* XGet)(DWORD, void*);
typedef void(WINAPI* PadSetFn)(LONG, LONG, LONG);
volatile unsigned long long g_padSeen = 0;   // what the "game" last read from XInputGetState: buttons | (lx << 16) | (1 << 40 when connected)
volatile long long g_padCalls = 0;
XGet g_gameXGet = nullptr;
static DWORD WINAPI GamePadThread(LPVOID) {
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
    for (;;) {
        memset(&st, 0, sizeof st);
        DWORD r = g_gameXGet ? g_gameXGet(0, &st) : 1;
        g_padSeen = (r == 0) ? ((unsigned long long)st.buttons | ((unsigned long long)(unsigned short)st.lx << 16) | (1ull << 40)) : 0;
        ++g_padCalls;
        Sleep(4);
    }
}

// ---- stand-in for the engine's time controller chain (time_mgr_global): G -> M -> P -> two objects with f0..f4 at +0x00..+0x10.
// SimThread plays "the engine": it advances a simulated game clock by realDt * scale of the active controller (or realDt when idle),
// advances the controller's progress f4 in REAL time and resets it to idle when f4 >= A + B.
struct TimeObj { float f[5]; };
TimeObj g_tobj[2] = {{{-1, -1, 0, 0, 0}}, {{-1, -1, 0, 0, 0}}};
alignas(16) unsigned char g_P[0x1000];
void* g_Mvar = g_P;          // M holds P
void* g_timeG = &g_Mvar;     // G holds M
volatile unsigned g_pauseMarker = 0;
volatile double g_simTime = 0.0;
volatile long long g_simTicks = 0;
extern "C" __declspec(noinline) void* HarnessGetG() { g_pauseMarker = 0xABCD1234u; return g_timeG; }   // `mov [rip+x], imm32 ; mov rax,[rip+G] ; ret` -> a signature with rip=13
static DWORD WINAPI SimThread(LPVOID) {
    *(void**)(g_P + gl::kCtlObj[0]) = &g_tobj[0];
    *(void**)(g_P + gl::kCtlObj[1]) = &g_tobj[1];
    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(4);
        QueryPerformanceCounter(&t1);
        double dt = (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;   // REAL time since the last tick
        t0 = t1;
        float scale = 1.0f;
        TimeObj& o = g_tobj[0];
        if (o.f[0] > -0.5f) {
            scale = o.f[0];
            o.f[4] += (float)dt;   // progress advances in real time, like the engine's
            if (o.f[4] >= o.f[2] + o.f[3]) { o.f[0] = -1; o.f[1] = -1; o.f[2] = 0; o.f[3] = 0; o.f[4] = 0; }
        }
        g_simTime = g_simTime + dt * scale;
        ++g_simTicks;
    }
}

int main() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    SetCurrentDirectoryA(dir);
    HMODULE fake = LoadLibraryA("xinput1_4.dll");   // the fake one next to the exe (SetCurrentDirectory above)
    PadSetFn padSet = fake ? (PadSetFn)GetProcAddress(fake, "FakePadSet") : nullptr;
    g_gameXGet = fake ? (XGet)GetProcAddress(fake, "XInputGetState") : nullptr;
    HMODULE h = LoadLibraryA("CombatMod.asi");
    HMODULE self = GetModuleHandleA(nullptr);
    printf("loaded=%p base=%p counter=%llx float=%llx fn=%llx curve=%llx D=%llx used=%llx apply=%llx disp=%llx lseen=%llx padseen=%llx padcalls=%llx simtime=%llx tobj=%llx getg=%llx d2=%llx\n", (void*)h, (void*)self,
           (unsigned long long)&g_counter, (unsigned long long)&g_float, (unsigned long long)&HarnessFn,
           (unsigned long long)&HarnessCurve, (unsigned long long)g_D.b, (unsigned long long)g_used, (unsigned long long)&HarnessApply, (unsigned long long)&HarnessDispatch, (unsigned long long)g_lseen, (unsigned long long)&g_padSeen, (unsigned long long)&g_padCalls, (unsigned long long)&g_simTime, (unsigned long long)g_tobj, (unsigned long long)&HarnessGetG, (unsigned long long)g_D2.b);
    fflush(stdout);
    g_tbl[gl::kEvTableBase / 8 + 3] = (unsigned long long)g_HA; g_tbl[gl::kEvTableBase / 8 + 7] = (unsigned long long)g_HB;   // a1 + kEvTableBase + 8 * type
    CreateThread(nullptr, 0, Worker, (LPVOID)1, 0, nullptr);
    CreateThread(nullptr, 0, Worker, (LPVOID)2, 0, nullptr);  // second thread also bumps the counter
    CreateThread(nullptr, 0, GamePadThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, SimThread, nullptr, 0, nullptr);
    for (int i = 0; i < 12000; ++i) {   // ~120 s; every 10 ms apply the scripted pad
        if (i == 800) CreateThread(nullptr, 0, Worker, (LPVOID)3, 0, nullptr);  // thread created LATER: tests the watch poller
        if (padSet) {
            FILE* f = fopen("pad.txt", "r");
            long b = 0, lx = 0, c = 0;
            if (f) { if (fscanf(f, "%ld %ld %ld", &b, &lx, &c) != 3) { b = 0; lx = 0; c = 0; } fclose(f); }
            padSet(b, lx, c);
        }
        Sleep(10);
    }
    return 0;
}
