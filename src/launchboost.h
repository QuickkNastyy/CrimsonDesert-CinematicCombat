// Launch (knockback) boost. Inside the apply-skill detour, BEFORE the original runs: if the skill record S = *(a2+0x70) looks like a
// launch record (the three marker dwords gl::kSMark0..2 == FLT_MAX 0x7F7FFFFF and the floats gl::kSStr0 / kSStr1 finite and in (0.5, 500), or - for records
// with little or no launch of their own - finite in [0, 500) with gl::kSNeg in [-1, 0)), the two floats are rewritten PERSISTENTLY (the engine
// reads the real loaded record asynchronously) as clamp(max(original, Launch.MinValue) * Launch.Mult). Records matching Launch.DenyKeys
// (entries "key" or "key:slot") are never boosted or raised.
// The originals are remembered per S pointer, so the written value is always derived from them (never compounds); when the boost is
// off, or the caller is not the player (Launch.PlayerOnly), the originals are written back.
#pragma once
#include "gamelayout.h"
#include "util.h"
#include <atomic>
#include <memory>
#include <vector>

struct LaunchCfg {
    bool enabled = false;
    bool playerOnly = true;
    float mult = 1.0f;
    float maxValue = 120.0f;
    float angleMul = 1.0f;     // experimental: scales the angle floats gl::kSAng0 / kSAng1
    float minValue = 0.0f;     // Launch.MinValue: a launch below this (incl. 0) is raised to it BEFORE the multiplier (0 = off)
    uint32_t deny[16] = {};    // Launch.DenyKeys entries (KeyEnc: "key" or "key:slot")
    int nDeny = 0;
};
LaunchCfg LaunchCfgFromSettings();

struct LaunchRec {
    uint64_t S = 0, lastA1 = 0;
    uint32_t keys[4] = {0, 0, 0, 0};          // the 4 SkillKeys (gl::kSKeys) as seen at the last call (may be 0)
    float orig[4] = {0, 0, 0, 0};              // gl::kSStr0, kSStr1 (strength), gl::kSAng0, kSAng1 (angles)
    float applied[4] = {0, 0, 0, 0};           // what we last wrote (== orig while the boost is off)
    bool angleOk = false;                      // the angle floats looked sane (finite, 0 < a < 6.3) when the record was first seen
    uint64_t calls = 0, rebased = 0;
    long long first_us = 0, last_us = 0;
    bool lastPlayer = false;
    bool denied = false;                       // matches Launch.DenyKeys: never boosted, never raised
    bool raised = false;                       // Launch.MinValue lifted at least one of the two strength floats
};

struct LaunchProbe {            // a launch record with the raw bytes (gl::kSProbeBytes) of its S as first seen (BEFORE any boost): lets field offsets be re-found after a patch
    LaunchRec r;
    uint8_t bytes[gl::kSProbeBytes];
    bool ok = false;
};

struct LaunchStats { uint64_t denied = 0, raised = 0, boosted = 0, records = 0, calls = 0, writes = 0, rebased = 0, mapFull = 0, dropped = 0, restored = 0; };

class LaunchEngine {
public:
    LaunchEngine();
    // Detour hook (before the original). player = current player a1 (0 = unknown). Never blocks: if another thread holds the table, the call is skipped.
    void OnCall(uint64_t a1, uint64_t a2, uint64_t player, const LaunchCfg& cfg) noexcept;
    int RestoreAll() noexcept;                  // originals back wherever the field still holds our value; returns #fields restored
    void Clear();                               // restore, then forget all records
    std::vector<LaunchRec> List(size_t n);      // most recently seen first
    std::vector<LaunchProbe> Probes();          // every live record with its first-seen S bytes (telemetry)
    void OwnRange(uintptr_t& lo, uintptr_t& hi) const;   // this engine's own table (holds first-seen copies of S: the launch scan must not report them)
    LaunchStats Stats();

private:
    struct Slot { uint64_t S = 0; LaunchRec r; bool used = false; bool dead = false; bool probeOk = false; uint8_t probe[gl::kSProbeBytes]; };
    static const size_t kSlots = 4096, kCap = 2048;
    std::unique_ptr<Slot[]> t_;
    size_t used_ = 0;
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    LaunchStats st_;
    Slot* FindLocked(uint64_t S, bool create);
    void Lock();
};

void LaunchSyncFromSettings();                   // Launch.Enabled: start skill capture / restore originals when turned off
LaunchEngine& Launch();                          // the process-wide engine
std::string CmdLaunch(const Args& t);            // launch status|on|off|log [n]|restore|clear
