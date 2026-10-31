#include "common.h"
#include "sigscan.h"
#include "util.h"
#include "log.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>

static std::mutex g_mu;
static std::vector<Sig> g_sigs;

ModInfo MainModule() {
    ModInfo m;
    HMODULE h = GetModuleHandleW(nullptr);
    m.base = reinterpret_cast<uintptr_t>(h);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return m;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return m;
    m.size = nt->OptionalHeader.SizeOfImage;
    return m;
}

const char* SigStatusName(SigStatus s) {
    switch (s) {
        case SigStatus::Resolved: return "RESOLVED";
        case SigStatus::Ambiguous: return "AMBIGUOUS";
        case SigStatus::Missing: return "MISSING";
        default: return "ERROR";
    }
}

bool ParsePattern(const std::string& text, Pattern& out) {
    out.bytes.clear();
    out.mask.clear();
    size_t i = 0, n = text.size();
    while (i < n) {
        while (i < n && isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && !isspace(static_cast<unsigned char>(text[j]))) ++j;
        std::string tok = text.substr(i, j - i);
        i = j;
        if (tok == "?" || tok == "??") {
            out.bytes.push_back(0);
            out.mask.push_back(0);
        } else {
            if (tok.size() != 2 || !isxdigit(static_cast<unsigned char>(tok[0])) || !isxdigit(static_cast<unsigned char>(tok[1])))
                return false;
            out.bytes.push_back(static_cast<uint8_t>(strtoul(tok.c_str(), nullptr, 16)));
            out.mask.push_back(1);
        }
    }
    // need at least one fixed byte, and the first fixed byte anchors the search
    return std::find(out.mask.begin(), out.mask.end(), 1) != out.mask.end();
}

// Scan one readable run. POD arguments only so __try is legal here.
static void ScanRun(const uint8_t* p, size_t n, const uint8_t* b, const uint8_t* m, size_t pl, size_t k,
                    size_t* count, uintptr_t* first) {
    __try {
        if (n < pl) return;
        const uint8_t* end = p + (n - pl);  // last valid start
        const uint8_t* s = p;
        while (s <= end) {
            const uint8_t* q = static_cast<const uint8_t*>(memchr(s + k, b[k], static_cast<size_t>(end - s) + 1));
            if (!q) break;
            s = q - k;
            size_t j = 0;
            while (j < pl && (!m[j] || s[j] == b[j])) ++j;
            if (j == pl) {
                if (!*count) *first = reinterpret_cast<uintptr_t>(s);
                ++*count;
            }
            ++s;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

ScanResult Scan(const Pattern& pt) {
    ScanResult r;
    ModInfo mi = MainModule();
    if (!mi.base || pt.bytes.empty()) return r;
    size_t k = 0;
    while (!pt.mask[k]) ++k;  // ParsePattern guarantees a fixed byte exists
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mi.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mi.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t s = mi.base + sec[i].VirtualAddress;
        uintptr_t e = s + (sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        uintptr_t runS = 0, runE = 0;
        auto flush = [&]() {
            if (runE > runS)
                ScanRun(reinterpret_cast<const uint8_t*>(runS), runE - runS, pt.bytes.data(), pt.mask.data(),
                        pt.bytes.size(), k, &r.count, &r.first);
            runS = runE = 0;
        };
        for (uintptr_t a = s; a < e;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi)) break;
            uintptr_t re = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            uintptr_t ce = std::min(re, e);
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok) {
                if (runE && runE == a) runE = ce;  // extend contiguous readable run
                else { flush(); runS = a; runE = ce; }
            } else {
                flush();
            }
            a = ce;
        }
        flush();
    }
    return r;
}

static void ResolveOne(Sig& s) {
    Pattern p;
    if (!ParsePattern(s.pattern, p)) { s.st = SigStatus::Error; return; }
    ScanResult r = Scan(p);
    s.matches = r.count;
    s.match = r.first;
    s.fpOk = r.count == 1 && PeekMem(r.first, s.fp, 24);
    if (r.count == 0) { s.st = SigStatus::Missing; return; }
    if (r.count > 1) { s.st = SigStatus::Ambiguous; return; }  // unique matches only
    uintptr_t a = r.first + static_cast<intptr_t>(s.off);
    if (s.rip >= 0) {
        int32_t rel = 0;
        if (!Rd(a + static_cast<uintptr_t>(s.rip), rel)) { s.st = SigStatus::Error; return; }
        a = a + static_cast<uintptr_t>(s.rip) + 4 + static_cast<intptr_t>(rel);
    }
    if (s.deref) {
        uintptr_t v = 0;
        if (!Rd(a, v)) { s.st = SigStatus::Error; return; }
        a = v;
    }
    s.addr = a;
    s.st = SigStatus::Resolved;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

int ResolveSigs(const std::wstring& path) {
    std::vector<Sig> sigs;
    std::ifstream f(path);
    if (!f) {
        Log("sigs: cannot open sigs file, nothing to resolve");
    } else {
        std::string line;
        while (std::getline(f, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> parts;
            size_t pos = 0, bar;
            while ((bar = line.find('|', pos)) != std::string::npos) { parts.push_back(line.substr(pos, bar - pos)); pos = bar + 1; }
            parts.push_back(line.substr(pos));
            size_t eq = parts[0].find('=');
            if (eq == std::string::npos) { Log("sigs: ignoring malformed line: %s", line.c_str()); continue; }
            Sig s;
            s.name = Trim(parts[0].substr(0, eq));
            s.pattern = Trim(parts[0].substr(eq + 1));
            for (size_t i = 1; i < parts.size(); ++i) {
                size_t e2 = parts[i].find('=');
                if (e2 == std::string::npos) continue;
                std::string key = Trim(parts[i].substr(0, e2)), val = Trim(parts[i].substr(e2 + 1));
                int v = static_cast<int>(strtol(val.c_str(), nullptr, 0));
                if (key == "off") s.off = v;
                else if (key == "rip") s.rip = v;
                else if (key == "deref") s.deref = v;
            }
            sigs.push_back(std::move(s));
        }
    }
    int ok = 0;
    for (Sig& s : sigs) {
        ResolveOne(s);
        switch (s.st) {
            case SigStatus::Resolved:
                ++ok;
                Log("sig %s: RESOLVED addr=0x%llX (match=0x%llX)", s.name.c_str(), (unsigned long long)s.addr, (unsigned long long)s.match);
                break;
            case SigStatus::Ambiguous: Log("sig %s: AMBIGUOUS (%zu matches, not used)", s.name.c_str(), s.matches); break;
            case SigStatus::Missing: Log("sig %s: MISSING (0 matches)", s.name.c_str()); break;
            default: Log("sig %s: ERROR (bad pattern/placeholder or unreadable target): %s", s.name.c_str(), s.pattern.c_str()); break;
        }
    }
    Log("sigs: %d/%zu resolved", ok, sigs.size());
    std::lock_guard<std::mutex> lk(g_mu);
    g_sigs = std::move(sigs);
    return ok;
}

std::vector<Sig> SigsSnapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_sigs;
}

bool FindSig(const std::string& name, Sig& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const Sig& s : g_sigs)
        if (s.name == name) { out = s; return true; }
    return false;
}
