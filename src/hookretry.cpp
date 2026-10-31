#include "hookretry.h"
#include "log.h"
#include <mutex>
#include <vector>

namespace {
struct Task {
    std::string name;
    HookAttempt fn;
    int tries = 0;
    long long nextUs = 0;
    enum State { Pending, Ok, Failed } state = Pending;
    std::string lastErr;
};
std::mutex g_mu;
std::vector<Task> g_tasks;
HANDLE g_ev = nullptr, g_thread = nullptr;
std::atomic<bool> g_quit{false}, g_started{false};

DWORD WINAPI Worker(LPVOID) {
    RegisterInternalThread();
    while (!g_quit) {
        WaitForSingleObject(g_ev, 100);
        for (;;) {
            Task job;
            {
                std::lock_guard<std::mutex> l(g_mu);
                Task* due = nullptr;
                for (Task& t : g_tasks) if (t.state == Task::Pending && t.nextUs <= NowUs()) { due = &t; break; }
                if (!due) break;
                due->nextUs = NowUs() + 3600LL * 1000000LL;   // claimed: no other round picks it while it runs
                job.name = due->name;
                job.fn = due->fn;
                job.tries = due->tries + 1;
            }
            std::string msg;
            HookResult r = HookResult::Fatal;
            try { r = job.fn(msg); } catch (...) { msg = "exception during the install"; r = HookResult::Retry; }
            std::lock_guard<std::mutex> l(g_mu);
            for (Task& t : g_tasks) {
                if (t.name != job.name || t.state != Task::Pending) continue;
                t.tries = job.tries;
                if (r == HookResult::Ok) { t.state = Task::Ok; t.lastErr.clear(); Log("hook %s: installed (attempt %d/%d)", t.name.c_str(), t.tries, kHookMaxTries); }
                else if (r == HookResult::Fatal) { t.state = Task::Failed; t.lastErr = msg; Log("hook %s: attempt %d failed, not retrying: %s", t.name.c_str(), t.tries, msg.c_str()); }
                else if (t.tries >= kHookMaxTries) { t.state = Task::Failed; t.lastErr = msg; Log("hook %s: attempt %d/%d failed: %s - GIVING UP", t.name.c_str(), t.tries, kHookMaxTries, msg.c_str()); }
                else { t.lastErr = msg; t.nextUs = NowUs() + kHookRetryMs * 1000LL; Log("hook %s: attempt %d/%d failed: %s - retrying in %d ms", t.name.c_str(), t.tries, kHookMaxTries, msg.c_str(), kHookRetryMs); }
            }
        }
    }
    UnregisterInternalThread();
    return 0;
}
}  // namespace

namespace { std::atomic<int> g_failNext{0}; }
void HookTestFailNext(int n) { g_failNext = n; }
bool HookTestShouldFail() {
    int v = g_failNext.load();
    while (v > 0) if (g_failNext.compare_exchange_weak(v, v - 1)) return true;
    return false;
}

HookResult HookClassify(bool ok, const std::string& msg) {
    if (ok) return HookResult::Ok;
    if (msg.rfind("MinHook", 0) == 0 || msg.rfind("MH_", 0) == 0) return HookResult::Retry;
    return HookResult::Fatal;
}

HookResult HookRunSchedule(const HookAttempt& attempt, std::string& lastMsg, int& triesUsed, const std::function<void(int)>& sleep) {
    triesUsed = 0;
    HookResult r = HookResult::Fatal;
    for (int i = 0; i < kHookMaxTries; ++i) {
        ++triesUsed;
        r = attempt(lastMsg);
        if (r != HookResult::Retry) return r;
        if (i + 1 < kHookMaxTries) sleep(kHookRetryMs);
    }
    return r;
}

void HookRetryRequest(const std::string& name, HookAttempt attempt) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        bool exp = false;
        if (g_started.compare_exchange_strong(exp, true)) {
            g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
            if (!g_thread) { g_started = false; Log("hook %s: cannot start the installer thread", name.c_str()); return; }
        }
        for (Task& t : g_tasks) {
            if (t.name != name) continue;
            if (t.state == Task::Pending) return;   // already running / waiting for its next try
            t.fn = attempt; t.tries = 0; t.nextUs = 0; t.state = Task::Pending; t.lastErr.clear();
            SetEvent(g_ev);
            return;
        }
        Task t;
        t.name = name; t.fn = attempt;
        g_tasks.push_back(std::move(t));
    }
    SetEvent(g_ev);
}

bool HookRetryPending(const std::string& name) {
    std::lock_guard<std::mutex> l(g_mu);
    for (const Task& t : g_tasks) if (t.name == name && t.state == Task::Pending) return true;
    return false;
}

std::string HookRetrySummary() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    for (const Task& t : g_tasks) {
        if (t.state == Task::Pending) o += F("hook-install %s: PENDING, attempt %d/%d%s%s\n", t.name.c_str(), t.tries + 1, kHookMaxTries, t.lastErr.empty() ? "" : ", last error: ", t.lastErr.c_str());
        else if (t.state == Task::Ok) o += F("hook-install %s: ok (after %d attempt%s)\n", t.name.c_str(), t.tries, t.tries == 1 ? "" : "s");
        else o += F("hook-install %s: FAILED after %d attempt%s: %s\n", t.name.c_str(), t.tries, t.tries == 1 ? "" : "s", t.lastErr.c_str());
    }
    return o;
}

void HookRetryShutdown(bool wait) {
    g_quit = true;
    if (g_ev) SetEvent(g_ev);
    if (g_thread && wait) { WaitForSingleObject(g_thread, 1500); CloseHandle(g_thread); g_thread = nullptr; }
}
