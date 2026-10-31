#include "common.h"
#include "timecurve.h"
#include "util.h"
#include "gamelayout.h"
#include "log.h"
#include "sigscan.h"
#include <cstdio>
#include <cstring>
#include <share.h>
#include <thread>

namespace {
struct Sample {
    uintptr_t G = 0, M = 0, P = 0, obj[2] = {0, 0};
    float f[2][5] = {};
};

bool ReadSample(Sample& s, std::string& err) {
    Sig sg;
    if (!FindSig("time_mgr_global", sg) || sg.st != SigStatus::Resolved) { err = "sig time_mgr_global not resolved"; return false; }
    s.G = sg.addr;
    if (!Rd(s.G, s.M) || !s.M) { err = "G (time_mgr_global qword) unreadable or null"; return false; }
    if (!Rd(s.M, s.P) || !s.P) { err = "M=*G unreadable or null"; return false; }
    const uintptr_t offs[2] = {gl::kCtlObj[0], gl::kCtlObj[1]};
    for (int i = 0; i < 2; ++i) {
        if (!Rd(s.P + offs[i], s.obj[i]) || !s.obj[i]) { err = "obj pointer at P+off unreadable or null"; return false; }
        if (!SafeRead(s.obj[i], s.f[i], sizeof s.f[i])) { err = "obj floats unreadable"; return false; }
    }
    return true;
}

std::atomic<bool> g_on{false};
std::atomic<bool> g_thread{false};
bool g_firstOpen = true;

void Sampler() {
    RegisterInternalThread();
    FILE* csv = nullptr;
    float last[2][5];
    bool have[2] = {false, false};
    bool wasOk = false;
    while (!g_stop) {
        if (!g_on) {
            if (csv) { fclose(csv); csv = nullptr; Log("timecurve sampler off"); }
            Sleep(50);
            continue;
        }
        if (!csv) {
            csv = _wfsopen((g_dir + L"CombatMod_timecurve.csv").c_str(), g_firstOpen ? L"w" : L"a", _SH_DENYNO);
            if (!csv) { Log("timecurve sampler: cannot open csv"); g_on = false; continue; }
            if (g_firstOpen) fputs("time_ms,obj,f0,f1,f2,f3,f4\n", csv);
            g_firstOpen = false;
            fflush(csv);
            have[0] = have[1] = false;
            wasOk = false;
            Log("timecurve sampler running (8 ms)");
        }
        Sleep(8);
        Sample s;
        std::string err;
        bool ok = ReadSample(s, err);
        if (ok != wasOk) { Log("timecurve sampler: chain %s%s%s", ok ? "readable" : "unavailable", ok ? "" : ": ", ok ? "" : err.c_str()); wasOk = ok; }
        if (!ok) continue;
        long long ms = NowUs() / 1000;  // ms since plugin load (same clock as trace/watch logs)
        for (int i = 0; i < 2; ++i) {
            if (have[i] && memcmp(last[i], s.f[i], sizeof last[i]) == 0) continue;
            memcpy(last[i], s.f[i], sizeof last[i]);
            have[i] = true;
            fprintf(csv, "%lld,%d,%.9g,%.9g,%.9g,%.9g,%.9g\n", ms, i + 1, s.f[i][0], s.f[i][1], s.f[i][2], s.f[i][3], s.f[i][4]);
            fflush(csv);
        }
    }
    if (csv) fclose(csv);
    UnregisterInternalThread();
}
}  // namespace

bool TimeCurveReport(std::string& out) {
    Sample s;
    std::string err;
    if (!ReadSample(s, err)) { out = err; return false; }
    char b[256];
    snprintf(b, sizeof b, "G=0x%llX M=0x%llX P=0x%llX\n", (unsigned long long)s.G, (unsigned long long)s.M, (unsigned long long)s.P);
    out = b;
    for (int i = 0; i < 2; ++i) {
        snprintf(b, sizeof b, "obj%d=0x%llX f0=%.6g f1=%.6g f2=%.6g f3=%.6g f4=%.6g\n", i + 1, (unsigned long long)s.obj[i],
                 s.f[i][0], s.f[i][1], s.f[i][2], s.f[i][3], s.f[i][4]);
        out += b;
    }
    return true;
}

void SetTimeCurveSampling(bool on) {
    if (on) {
        bool exp = false;
        if (g_thread.compare_exchange_strong(exp, true)) {
            try { std::thread(Sampler).detach(); } catch (...) { g_thread = false; Log("timecurve sampler: could not start thread"); return; }
        }
    }
    g_on = on;
}
bool TimeCurveSampling() { return g_on; }
void StartTimeCurveSampler() { SetTimeCurveSampling(true); }
