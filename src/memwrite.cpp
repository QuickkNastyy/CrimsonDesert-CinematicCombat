#include "memwrite.h"
#include "log.h"
#include "etrace.h"
#include "skillctx.h"
#include "timeamp.h"
#include "trace.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
struct Entry {
    int id = 0;
    uintptr_t addr = 0;
    std::vector<uint8_t> oldb, newb;
    bool active = true, patch = false;
    long long revertAtUs = 0;  // 0 = no timer
};
std::mutex g_mu;
std::vector<Entry> g_j;
int g_nextId = 1;
std::atomic<bool> g_wd{false};

// POD-only so __try is legal.
bool RawCopy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Writes n bytes at addr, region by region: VirtualProtect -> copy -> restore protection.
bool PatchBytes(uintptr_t addr, const uint8_t* src, size_t n, std::string& err) {
    uintptr_t end = addr + n;
    for (uintptr_t a = addr; a < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) { err = F("0x%llX not committed", U64(a)); return false; }
        uintptr_t re = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        size_t chunk = std::min(re, end) - a;
        bool exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        DWORD old = 0, tmp = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(a), chunk, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) {
            err = F("VirtualProtect failed at 0x%llX (err %lu)", U64(a), GetLastError());
            return false;
        }
        bool ok = RawCopy(reinterpret_cast<void*>(a), src + (a - addr), chunk);
        VirtualProtect(reinterpret_cast<LPVOID>(a), chunk, old, &tmp);
        if (!ok) { err = F("fault while writing 0x%llX", U64(a)); return false; }
        a += chunk;
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), n);
    return true;
}

bool RestoreLocked(Entry& e, std::string& note) {
    size_t n = e.oldb.size();
    std::vector<uint8_t> cur(n);
    if (SafeRead(e.addr, cur.data(), n) && cur != e.newb) note = "current bytes differed from the written value; restored anyway";
    std::string err;
    if (!PatchBytes(e.addr, e.oldb.data(), n, err)) { note = err; return false; }
    e.active = false;
    e.revertAtUs = 0;
    Log("write #%d reverted at 0x%llX: %s", e.id, U64(e.addr), HexBytes(e.oldb.data(), n).c_str());
    return true;
}

void Watchdog() {
    RegisterInternalThread();
    while (!g_stop) {
        Sleep(5);
        long long now = NowUs();
        std::lock_guard<std::mutex> l(g_mu);
        for (Entry& e : g_j) {
            if (!e.active || !e.revertAtUs || now < e.revertAtUs) continue;
            std::string note;
            if (!RestoreLocked(e, note)) { e.revertAtUs = 0; Log("auto-revert of write #%d FAILED: %s", e.id, note.c_str()); }
        }
    }
    UnregisterInternalThread();
}

void EnsureWatchdog() {
    bool exp = false;
    if (g_wd.compare_exchange_strong(exp, true)) {
        try { std::thread(Watchdog).detach(); } catch (...) { g_wd = false; }
    }
}
}  // namespace

bool MemWrite(uintptr_t addr, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>* expect, bool patch,
              uint64_t revertMs, std::string& msg, int* idOut) {
    size_t n = bytes.size();
    if (n == 0 || n > 4096) { msg = "size must be 1..4096"; return false; }
    std::vector<uint8_t> cur(n);
    if (!SafeRead(addr, cur.data(), n)) { msg = F("unreadable memory at 0x%llX (+%zu)", U64(addr), n); return false; }
    bool code = RangeIsCode(addr, n);
    if (code && !patch) { msg = "target is executable code; use 'patch' (explicit) to modify code"; return false; }
    if (code && (TraceOverlaps(addr, n) || AmpOverlaps(addr, n) || SkillsOverlaps(addr, n) || EtOverlaps(addr, n))) { msg = "range overlaps an active hook (trace, time-amp, skills or etrace); remove it first"; return false; }
    if (expect) {
        if (expect->size() != n) { msg = "expect length differs from value length"; return false; }
        if (memcmp(cur.data(), expect->data(), n) != 0) {
            msg = "expect mismatch, current=" + HexBytes(cur.data(), n) + " (nothing written)";
            return false;
        }
    }
    std::lock_guard<std::mutex> l(g_mu);
    std::string err;
    if (!PatchBytes(addr, bytes.data(), n, err)) { msg = err; return false; }
    std::vector<uint8_t> back(n);
    if (!SafeRead(addr, back.data(), n) || back != bytes) Log("WARNING: write at 0x%llX read back different bytes (game may overwrite it)", U64(addr));
    Entry e;
    e.id = g_nextId++;
    e.addr = addr;
    e.oldb = cur;
    e.newb = bytes;
    e.patch = patch;
    if (revertMs) e.revertAtUs = NowUs() + static_cast<long long>(revertMs) * 1000;
    Log("%s #%d at 0x%llX (%s): %s -> %s%s", patch ? "patch" : "write", e.id, U64(addr), FmtAddr(addr).c_str(),
        HexBytes(cur.data(), n).c_str(), HexBytes(bytes.data(), n).c_str(), revertMs ? F(" (revert in %llu ms)", U64(revertMs)).c_str() : "");
    if (idOut) *idOut = e.id;
    msg = F("id=%d addr=0x%llX len=%zu old=%s new=%s", e.id, U64(addr), n, HexBytes(cur.data(), n, 16).c_str(), HexBytes(bytes.data(), n, 16).c_str());
    if (revertMs) { msg += F(" revert_in_ms=%llu", U64(revertMs)); EnsureWatchdog(); }
    g_j.push_back(std::move(e));
    return true;
}

std::string UndoLast(int n, bool& ok) {
    std::lock_guard<std::mutex> l(g_mu);
    std::string out;
    int done = 0;
    ok = true;
    for (size_t i = g_j.size(); i-- > 0 && done < n;) {
        Entry& e = g_j[i];
        if (!e.active) continue;
        std::string note;
        if (!RestoreLocked(e, note)) { out += F("FAILED #%d: %s\n", e.id, note.c_str()); ok = false; return out; }
        out += F("restored #%d addr=0x%llX len=%zu%s%s\n", e.id, U64(e.addr), e.oldb.size(), note.empty() ? "" : " note: ", note.c_str());
        ++done;
    }
    if (!done) { out = "nothing to undo"; ok = false; }
    return out;
}

void RevertPending() {
    std::lock_guard<std::mutex> l(g_mu);
    for (Entry& e : g_j) {
        if (!e.active || !e.revertAtUs) continue;
        std::string note;
        if (!RestoreLocked(e, note)) Log("shutdown revert of write #%d FAILED: %s", e.id, note.c_str());
    }
}

int JournalRemoveId(int id) {
    std::lock_guard<std::mutex> l(g_mu);
    for (size_t i = 0; i < g_j.size(); ++i)
        if (g_j[i].id == id) { g_j.erase(g_j.begin() + static_cast<std::ptrdiff_t>(i)); return 1; }
    return 0;
}

bool JournalActive(int id) {
    std::lock_guard<std::mutex> l(g_mu);
    for (const Entry& e : g_j) if (e.id == id) return e.active;
    return false;
}

std::string WritesSummary() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    long long now = NowUs();
    for (const Entry& e : g_j)
        if (e.active && e.revertAtUs) o += F("write #%d 0x%llX len=%zu auto-revert in %lld ms\n", e.id, U64(e.addr), e.oldb.size(), std::max(0LL, (e.revertAtUs - now) / 1000));
    return o;
}

std::string CmdJournal() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o;
    long long now = NowUs();
    for (const Entry& e : g_j) {
        o += F("#%d %s addr=0x%llX (%s) len=%zu old=%s new=%s%s", e.id, e.active ? "active" : "reverted", U64(e.addr), FmtAddr(e.addr).c_str(),
               e.oldb.size(), HexBytes(e.oldb.data(), e.oldb.size(), 16).c_str(), HexBytes(e.newb.data(), e.newb.size(), 16).c_str(), e.patch ? " [patch]" : "");
        if (e.active && e.revertAtUs) o += F(" revert_in_ms=%lld", std::max(0LL, (e.revertAtUs - now) / 1000));
        o += "\n";
    }
    if (o.empty()) o = "(journal empty)\n";
    return Ok(o);
}

std::string CmdUndo(const Args& t) {
    uint64_t n = 1;
    if (t.size() > 2 || (t.size() == 2 && (!ParseNum(t[1], n) || n < 1 || n > 1000))) return Err("usage: undo [n]");
    bool ok;
    std::string r = UndoLast(static_cast<int>(n), ok);
    return ok ? Ok(r) : Err(r.back() == '\n' ? r.substr(0, r.size() - 1) : r);
}

std::string CmdJournalClear(const Args& t) {
    bool force = t.size() == 2 && Lower(t[1]) == "force";
    if (t.size() > 2 || (t.size() == 2 && !force)) return Err("usage: journal_clear [force]");
    std::lock_guard<std::mutex> l(g_mu);
    size_t before = g_j.size();
    if (force) g_j.clear();
    else g_j.erase(std::remove_if(g_j.begin(), g_j.end(), [](const Entry& e) { return !e.active; }), g_j.end());
    size_t kept = g_j.size();
    return Ok(F("removed=%zu kept_active=%zu%s\n", before - kept, kept, force ? " (force: active entries are no longer undoable, timers cancelled)" : ""));
}

static bool Encode(char kind, const std::string& tok, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (kind == 'f') {
        float f;
        if (!ParseFloat(tok, f)) { err = "bad float: " + tok; return false; }
        out.resize(4);
        memcpy(out.data(), &f, 4);
    } else {
        uint64_t v;
        if (!ParseNum(tok, v)) { err = "bad integer (decimal or 0x hex): " + tok; return false; }
        size_t sz = kind == 'q' ? 8 : 4;
        int64_t sv = static_cast<int64_t>(v);
        if (sz == 4 && v > 0xFFFFFFFFull && !(sv < 0 && sv >= INT32_MIN)) { err = "value does not fit in 32 bits"; return false; }
        out.resize(sz);
        memcpy(out.data(), &v, sz);
    }
    return true;
}

std::string CmdWrite(Args t, char kind) {
    uint64_t revert = 0;
    TakeOpt(t, "revert_ms", revert);
    if (revert > 3600000) return Err("revert_ms max is 3600000");
    if (t.size() < 3) return Err("usage: " + t[0] + " <addr> <value> [expect <value>] [revert_ms=N]");
    uintptr_t addr;
    std::string e;
    if (!ResolveAddr(t[1], addr, e)) return Err(e);
    size_t ex = std::string::npos;
    for (size_t i = 2; i < t.size(); ++i) if (Lower(t[i]) == "expect") { ex = i; break; }
    size_t vend = ex == std::string::npos ? t.size() : ex;
    std::vector<uint8_t> val, exp;
    if (kind == 'b' || kind == 'p') {
        if (!ParseHexBytes(t, 2, vend, val, e)) return Err(e);
        if (ex != std::string::npos && !ParseHexBytes(t, ex + 1, t.size(), exp, e)) return Err(e);
    } else {
        if (vend != 3 || (ex != std::string::npos && t.size() != ex + 2)) return Err("usage: " + t[0] + " <addr> <value> [expect <value>] [revert_ms=N]");
        if (!Encode(kind, t[2], val, e)) return Err(e);
        if (ex != std::string::npos && !Encode(kind, t[ex + 1], exp, e)) return Err(e);
    }
    std::string msg;
    if (!MemWrite(addr, val, ex != std::string::npos ? &exp : nullptr, kind == 'p', revert, msg)) return Err(msg);
    return Ok(msg + "\n");
}
