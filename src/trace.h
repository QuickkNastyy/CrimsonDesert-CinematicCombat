#pragma once
#include "util.h"

// Call tracing: MinHook detour -> per-trace thunk -> TraceCommon (asm) -> TraceHit (logs) -> tail-jump to original.
struct TraceRec {
    uint64_t idx;
    long long t_us;
    DWORD tid;
    uintptr_t ret;
    uint64_t rcx, rdx, r8, r9;
    uint64_t x[4];  // low 64 bits of xmm0..xmm3
    bool regs;      // trace was created with `regs` (xmm values are printed)
};

bool EnsureMinHook(std::string& err);
bool TraceAdd(uintptr_t addr, const std::string& name, bool regs, std::string& msg);
bool TraceRemove(uintptr_t addr, std::string& msg);
void TraceRemoveAll();
bool TraceRecords(uintptr_t addr, size_t n, std::vector<TraceRec>& out, uint64_t& total);
bool TraceOverlaps(uintptr_t addr, size_t n);  // does [addr,addr+n) touch the first bytes of an active trace target?
std::string TracesSummary();                   // active traces only (for `hooks`)

std::string CmdTrace(const Args& t);
std::string CmdTraces();
std::string CmdTraceLog(const Args& t);
std::string CmdUntrace(const Args& t);
std::string CmdUntraceAll();
