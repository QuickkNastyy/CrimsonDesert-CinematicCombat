#pragma once
#include "util.h"

// Data watchpoints using the CPU debug registers (DR0-DR3) + a vectored exception handler. Max 4.
struct WatchHit {
    uint64_t idx;
    long long t_us;
    DWORD tid;
    uint64_t rip, rax, rcx, rdx, r8, r9;
    uint64_t stk[6];  // first 6 qwords at [rsp]
    uint64_t val;     // value at the watched address after the access
};

// onlyTid != 0 restricts the watch to that thread (used by selftest so no game thread is suspended).
bool WatchAdd(bool rw, uintptr_t addr, int size, std::string& msg, DWORD onlyTid = 0);
bool WatchRemove(uintptr_t addr, std::string& msg);
void WatchRemoveAll();
bool WatchHits(uintptr_t addr, size_t n, std::vector<WatchHit>& out, uint64_t& total);
std::string WatchesSummary();  // for `hooks`

std::string CmdWatch(const Args& t);
std::string CmdWatches();
std::string CmdWatchLog(const Args& t);
std::string CmdUnwatch(const Args& t);
