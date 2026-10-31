// Background installer for hooks: every hook installation (amp, skills, event dispatcher, XInput, startup auto-install of enabled
// features) runs on ONE background thread with up to 8 attempts, 400 ms apart, because MinHook occasionally fails once with
// MH_ERROR_MEMORY_ALLOC (a retry succeeds). The UI and the bridge threads only post a request and never wait for it.
#pragma once
#include "util.h"
#include <functional>

enum class HookResult { Ok, Retry, Fatal };           // Retry = transient (MinHook), Fatal = retrying cannot help (missing/ambiguous signature)
typedef std::function<HookResult(std::string&)> HookAttempt;

void HookRetryRequest(const std::string& name, HookAttempt attempt);   // async; ignored while a task with that name is pending
HookResult HookClassify(bool ok, const std::string& msg);               // MinHook / MH_ errors are transient, everything else is fatal
bool HookRetryPending(const std::string& name);
std::string HookRetrySummary();                                         // one line per task (for `hooks`)
void HookRetryShutdown(bool wait);                                               // stop the worker (quit-bridge / unload)

// Developer test aid (`hooktest failnext <n>`): the next n hook installs fail with a transient MinHook error, to exercise the retry schedule.
void HookTestFailNext(int n);
bool HookTestShouldFail();                                              // consumes one injected failure

constexpr int kHookMaxTries = 8;
constexpr int kHookRetryMs = 400;
// The same schedule as a pure function (selftest): runs `attempt` up to kHookMaxTries times; `sleep` is called between tries.
HookResult HookRunSchedule(const HookAttempt& attempt, std::string& lastMsg, int& triesUsed, const std::function<void(int)>& sleep);
