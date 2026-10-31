#include "call.h"
#include "log.h"
#include <memory>

namespace {
struct CallCtx {
    uintptr_t fn = 0;
    uint64_t a[4] = {0, 0, 0, 0};
    uint64_t rax = 0;
    DWORD exc = 0;
    HANDLE done = nullptr;
};
std::atomic<bool> g_busy{false};  // stays true while a timed-out call is still running

typedef uint64_t(__fastcall* Fn4)(uint64_t, uint64_t, uint64_t, uint64_t);

// POD only so __try is legal.
void DoCall(CallCtx* c) {
    __try { c->rax = reinterpret_cast<Fn4>(c->fn)(c->a[0], c->a[1], c->a[2], c->a[3]); }
    __except (c->exc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
}

DWORD WINAPI CallThread(LPVOID p) {
    auto* sp = static_cast<std::shared_ptr<CallCtx>*>(p);
    std::shared_ptr<CallCtx> c = *sp;
    delete sp;
    DoCall(c.get());
    g_busy = false;
    SetEvent(c->done);
    return 0;
}
}  // namespace

std::string CmdCall(const Args& t) {
    if (t.size() < 2 || t.size() > 6) return Err("usage: call <addr|signame> [a1 a2 a3 a4]   (args hex, or d:<decimal>)");
    auto c = std::make_shared<CallCtx>();
    std::string e;
    if (!ResolveAddr(t[1], c->fn, e)) return Err(e);
    if (!IsExecAddr(c->fn)) return Err("target is not in executable memory");
    for (size_t i = 2; i < t.size(); ++i) {
        uint64_t v = 0;
        bool ok = Lower(t[i]).compare(0, 2, "d:") == 0 ? ParseNum(t[i].substr(2), v) : ParseHex(t[i], v);
        if (!ok) return Err("bad argument: " + t[i]);
        c->a[i - 2] = v;
    }
    bool exp = false;
    if (!g_busy.compare_exchange_strong(exp, true)) return Err("busy: a previous call is still running (it timed out)");
    c->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto* sp = new std::shared_ptr<CallCtx>(c);
    Log("call 0x%llX(0x%llX, 0x%llX, 0x%llX, 0x%llX)", U64(c->fn), U64(c->a[0]), U64(c->a[1]), U64(c->a[2]), U64(c->a[3]));
    HANDLE th = CreateThread(nullptr, 0, CallThread, sp, 0, nullptr);
    if (!th) { delete sp; CloseHandle(c->done); g_busy = false; return Err("CreateThread failed"); }
    CloseHandle(th);
    DWORD w = WaitForSingleObject(c->done, 2000);
    if (w != WAIT_OBJECT_0) {  // c->done is intentionally leaked here: the worker may still signal it
        Log("call 0x%llX TIMED OUT after 2000 ms", U64(c->fn));
        return Err("timeout after 2000 ms (call still running on its worker; further calls refused until it returns)");
    }
    CloseHandle(c->done);
    if (c->exc) {
        Log("call 0x%llX raised exception 0x%08lX", U64(c->fn), c->exc);
        return Err(F("exception 0x%08lX inside call", c->exc));
    }
    Log("call 0x%llX returned 0x%llX", U64(c->fn), U64(c->rax));
    return Ok(F("rax=0x%llX (%lld)\n", U64(c->rax), static_cast<long long>(c->rax)));
}
