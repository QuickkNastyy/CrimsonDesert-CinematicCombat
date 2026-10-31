// Parsing / formatting helpers shared by the bridge command modules.
#pragma once
#include "common.h"
#include <string>
#include <vector>

using Args = std::vector<std::string>;
#define U64(x) static_cast<unsigned long long>(x)

std::string F(const char* fmt, ...);                       // printf into std::string (2 KB max)
inline std::string Ok(const std::string& body = "") { return body + "OK\n"; }
inline std::string Err(const std::string& msg) { return "ERR " + msg + "\n"; }

bool ParseHex(const std::string& s, uint64_t& v);          // hex, optional 0x
bool ParseSHex(const std::string& s, int64_t& v);          // signed hex (offsets)
bool ParseNum(const std::string& s, uint64_t& v);          // decimal or 0x hex, optional leading '-'
bool ParseFloat(const std::string& s, float& v);
bool ParseHexBytes(const Args& t, size_t b, size_t e, std::vector<uint8_t>& out, std::string& err);  // tokens [b,e)
std::string HexBytes(const uint8_t* p, size_t n, size_t cap = 32);  // "90 90 C3 ..."
std::string Lower(std::string s);
bool TakeOpt(Args& t, const char* key, uint64_t& v);       // removes "key=N" token; true if found. v untouched if absent

// <addr|signame|rva:HEX> -> absolute address. Sig names win over hex.
bool ResolveAddr(const std::string& tok, uintptr_t& out, std::string& err);
std::string FmtAddr(uintptr_t a);                          // "main+0x1234" inside the exe, else "0x..."

// SkillKey match entries used by launch deny lists and trigger rules: "key" = any of the 4 keys equals it; "key:slot" = keys[0] == key AND keys[1] == slot
// (slot = the second key value, e.g. 1091:5 is the explosive-arrow record but not the 1091/2/1105 fire hit). Encoded in one uint32: key in the low
// 24 bits, slot + 1 in the high 8 bits (0 = no slot).
inline uint32_t KeyEnc(uint32_t key, int slot) { return (key & 0xFFFFFFu) | (slot >= 0 ? (static_cast<uint32_t>(slot) + 1u) << 24 : 0u); }
inline bool KeyEncMatch(uint32_t enc, const uint32_t keys[4]) {
    uint32_t key = enc & 0xFFFFFFu, hi = enc >> 24;
    if (!key) return false;
    if (hi) return keys[0] == key && keys[1] == hi - 1u;
    for (int i = 0; i < 4; ++i) if (keys[i] == key) return true;
    return false;
}
bool ParseKeyEnc(const std::string& item, uint32_t& enc, std::string& err);   // "10253" or "1091:5"
std::string KeyEncStr(uint32_t enc);                                          // back to text

bool IniGet(const std::string& key, std::string& out);     // key = "section.name" lowercase, e.g. "timeamp.scalemul"

long long NowUs();                                         // microseconds since plugin load (QPC)

// Threads owned by the plugin (excluded from hardware watchpoints).
void RegisterInternalThread();
void UnregisterInternalThread();
bool IsInternalThread(DWORD tid);

// Guarded read for hot paths (SEH, no syscall). On failure returns false and ZEROES `out`.
bool PeekMem(uintptr_t a, void* out, size_t n);

bool IsExecAddr(uintptr_t a);                              // committed page with execute protection
bool RangeIsCode(uintptr_t a, size_t n);                   // exe executable section or executable page anywhere in range
