#include "util.h"
#include "sigscan.h"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

static LARGE_INTEGER g_qf, g_q0;
static struct TimeInit {
    TimeInit() { QueryPerformanceFrequency(&g_qf); QueryPerformanceCounter(&g_q0); }
} g_timeInit;  // dynamic init at DLL load, so NowUs() has no lazy-init guard in hot paths

long long NowUs() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    long long q = n.QuadPart - g_q0.QuadPart;
    return (q / g_qf.QuadPart) * 1000000LL + (q % g_qf.QuadPart) * 1000000LL / g_qf.QuadPart;
}

std::string F(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ParseHex(const std::string& s, uint64_t& v) {
    if (s.empty() || s[0] == '-' || s[0] == '+') return false;
    char* e = nullptr;
    v = strtoull(s.c_str(), &e, 16);
    return *e == 0;
}
bool ParseSHex(const std::string& s, int64_t& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtoll(s.c_str(), &e, 16);
    return *e == 0;
}
bool ParseNum(const std::string& s, uint64_t& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    if (s[0] == '-') v = static_cast<uint64_t>(strtoll(s.c_str(), &e, 0));
    else v = strtoull(s.c_str(), &e, 0);
    return *e == 0;
}
bool ParseKeyEnc(const std::string& item, uint32_t& enc, std::string& err) {
    size_t colon = item.find(':');
    uint64_t k = 0, sl = 0;
    if (!ParseNum(colon == std::string::npos ? item : item.substr(0, colon), k) || k == 0 || k > 0xFFFFFFull) { err = "bad key '" + item + "' (decimal 1..16777215, or key:slot)"; return false; }
    if (colon == std::string::npos) { enc = KeyEnc(static_cast<uint32_t>(k), -1); return true; }
    if (!ParseNum(item.substr(colon + 1), sl) || sl > 254) { err = "bad slot in '" + item + "' (0..254: the second key value)"; return false; }
    enc = KeyEnc(static_cast<uint32_t>(k), static_cast<int>(sl));
    return true;
}
std::string KeyEncStr(uint32_t enc) {
    uint32_t hi = enc >> 24;
    return hi ? F("%u:%u", enc & 0xFFFFFFu, hi - 1u) : F("%u", enc & 0xFFFFFFu);
}
bool ParseFloat(const std::string& s, float& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = strtof(s.c_str(), &e);
    return *e == 0;
}

bool ParseHexBytes(const Args& t, size_t b, size_t e, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    for (size_t i = b; i < e; ++i) {
        std::string tok = t[i];
        if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) tok = tok.substr(2);
        if (tok.empty() || tok.size() % 2) { err = "hex bytes must be whole bytes (even digit count): " + t[i]; return false; }
        for (size_t k = 0; k < tok.size(); k += 2) {
            if (!isxdigit(static_cast<unsigned char>(tok[k])) || !isxdigit(static_cast<unsigned char>(tok[k + 1]))) { err = "bad hex byte in: " + t[i]; return false; }
            out.push_back(static_cast<uint8_t>(strtoul(tok.substr(k, 2).c_str(), nullptr, 16)));
        }
        if (out.size() > 4096) { err = "more than 4096 bytes"; return false; }
    }
    if (out.empty()) { err = "no bytes given"; return false; }
    return true;
}

std::string HexBytes(const uint8_t* p, size_t n, size_t cap) {
    std::string o;
    for (size_t i = 0; i < n && i < cap; ++i) o += F(i ? " %02X" : "%02X", p[i]);
    if (n > cap) o += F(" ...(+%zu)", n - cap);
    return o;
}

bool TakeOpt(Args& t, const char* key, uint64_t& v) {
    std::string pre = std::string(key) + "=";
    for (size_t i = 0; i < t.size(); ++i) {
        if (Lower(t[i]).compare(0, pre.size(), pre) == 0) {
            uint64_t x;
            if (!ParseNum(t[i].substr(pre.size()), x)) return false;
            v = x;
            t.erase(t.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool ResolveAddr(const std::string& tok, uintptr_t& out, std::string& err) {
    if (Lower(tok).compare(0, 4, "rva:") == 0) {
        uint64_t r;
        if (!ParseHex(tok.substr(4), r)) { err = "bad rva: " + tok; return false; }
        out = MainModule().base + static_cast<uintptr_t>(r);
        return true;
    }
    Sig s;
    if (FindSig(tok, s)) {
        if (s.st != SigStatus::Resolved) { err = "sig " + tok + " is " + SigStatusName(s.st); return false; }
        out = s.addr;
        return true;
    }
    uint64_t a;
    if (!ParseHex(tok, a) || a == 0) { err = "bad address or unknown signature: " + tok; return false; }
    out = static_cast<uintptr_t>(a);
    return true;
}

std::string FmtAddr(uintptr_t a) {
    ModInfo m = MainModule();
    if (a >= m.base && a < m.base + m.size) return F("main+0x%llX", U64(a - m.base));
    return F("0x%llX", U64(a));
}

static std::mutex g_tmu;
static std::set<DWORD> g_internal;
void RegisterInternalThread() { std::lock_guard<std::mutex> l(g_tmu); g_internal.insert(GetCurrentThreadId()); }
void UnregisterInternalThread() { std::lock_guard<std::mutex> l(g_tmu); g_internal.erase(GetCurrentThreadId()); }
bool IsInternalThread(DWORD tid) { std::lock_guard<std::mutex> l(g_tmu); return g_internal.count(tid) != 0; }

bool PeekMem(uintptr_t a, void* out, size_t n) {
    __try { memcpy(out, reinterpret_cast<const void*>(a), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { memset(out, 0, n); return false; }
}

static bool ExecProt(DWORD p) {
    return (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
bool IsExecAddr(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && ExecProt(mbi.Protect);
}
bool RangeIsCode(uintptr_t a, size_t n) {
    ModInfo m = MainModule();
    if (m.base) {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t s = m.base + sec[i].VirtualAddress;
            uintptr_t e = s + (sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
            if (a < e && a + n > s) return true;
        }
    }
    uintptr_t end = a + n;
    for (uintptr_t p = a; p < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && ExecProt(mbi.Protect)) return true;
        p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return false;
}
