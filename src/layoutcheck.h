// `layout check` / `layout show`: does the game still look like src/gamelayout.h says? Read-only, SEH-guarded; meant for the first minutes after a game patch:
// it reports for the live process (1) whether recent time-event blocks have the expected type id and plausible floats at the expected offsets,
// (2) whether the time-controller objects hold the idle pattern or a valid curve (and, if not, which pointers in P point at idle-looking objects),
// (3) whether recently captured skill records still carry the keys / FLT_MAX marker block / launch fields at the expected offsets (and where the
// marker block is instead). Every verdict names the gamelayout.h constant that is probably wrong.
#pragma once
#include "util.h"
#include <vector>

std::string CmdLayout(const Args& t);       // layout check | show
std::string LayoutJson();                   // the constants as a JSON object (telemetry session file: "layout")

// ---- pure helpers (selftest drives them with fake memory)
struct LayoutVerdict {
    bool ok = true;
    std::string text;
};
// One time-event block: `blk` = bytes of D starting at D+0 (at least 0x40).
LayoutVerdict LayoutCheckEventBlock(const uint8_t* blk, size_t n);
// One skill record: `blk` = bytes of S starting at S+0 (kSCheckSpan). Fields: keys plausible? markers at the expected offsets? where else is the marker triple?
struct SkillRecCheck {
    bool keysPlausible = false, markers = false, negOk = false, strengthOk = false, readable = false;
    int markerAt = -1;          // offset of the FLT_MAX triple found anywhere in the record (-1 = none); equals gl::kSMark0 when the layout is right
    uint32_t keys[4] = {0, 0, 0, 0};
};
SkillRecCheck LayoutCheckSkillRecord(const uint8_t* blk, size_t n);
// Controller object floats classification: "idle", "curve", "pause" or "unexpected".
const char* LayoutClassifyCtl(const float f[5]);
