#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ModInfo { uintptr_t base = 0; size_t size = 0; };
ModInfo MainModule();  // main exe image (from PE headers in memory)

struct Pattern { std::vector<uint8_t> bytes, mask; };  // mask: 1 = must match, 0 = wildcard
bool ParsePattern(const std::string& text, Pattern& out);  // "48 8B ?? ?? 89 05"

struct ScanResult { size_t count = 0; uintptr_t first = 0; };
ScanResult Scan(const Pattern& p);  // executable sections of the main module

enum class SigStatus { Resolved, Ambiguous, Missing, Error };
const char* SigStatusName(SigStatus s);

struct Sig {
    std::string name, pattern;
    int off = 0;     // added to the match address
    int rip = -1;    // >=0: rel32 lives at (match+off+rip); target = that+4+rel32
    int deref = 0;   // 1: read a pointer at the resolved address
    SigStatus st = SigStatus::Missing;
    size_t matches = 0;
    uintptr_t match = 0, addr = 0;  // addr valid only when st == Resolved
    uint8_t fp[24] = {};            // first 24 bytes at the match (a fingerprint of the code, taken when the sig was resolved)
    bool fpOk = false;
};

int ResolveSigs(const std::wstring& path);        // (re)load file, scan, log; returns resolved count
std::vector<Sig> SigsSnapshot();                  // thread-safe copy
bool FindSig(const std::string& name, Sig& out);  // true if the name exists (check out.st)
