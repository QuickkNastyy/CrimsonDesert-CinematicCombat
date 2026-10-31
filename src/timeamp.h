// Slow-motion amplifier: hooks the game's GamePlayEvent_TimeScale timeline handler (sig `time_curve_start`,
// sub_149219F60) and rewrites the event data (scale / timing A / timing B) before the original runs.
//   __int64* time_curve_start(__int64 a1, uint32_t* a2, __int64 a3, __int64 a4, void** a5);   D = *(u64*)(a3 + 8)
//   D+0x08 scale, D+0x0C (untouched), D+0x10 timing A, D+0x14 timing B, D+0x18 selects a second code path (untouched).
// D is persistent shared data, so each D's ORIGINAL values are remembered and every call writes
// f(original), never f(current): repeated calls do not compound.
#pragma once
#include "skillctx.h"
#include "util.h"
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

struct AmpParams {
    bool enabled = false;
    float scaleMul = 1.0f;   // 0.05 .. 2
    float minScale = 0.02f;  // 0.01 .. 1
    float rampIn = 1.0f;     // 0.25 .. 4  multiplier of event timing A (SlowMo.RampInMul)
    float rampOut = 1.0f;    // 0.25 .. 4  multiplier of event timing B (SlowMo.RampOutMul)
    float ignoreAbove = 0.999f;   // events whose ORIGINAL scale is >= this are left alone (SlowMo.IgnoreAbove)
    bool slowOn = true;      // slow-motion class (original scale < ignoreAbove): scale / ramps are applied (SlowMo.Enabled)
    bool hitStopOn = false;  // hit-stop class (ignoreAbove <= original scale < 0.999): scale * hitStopMul (HitStop.Enabled)
    float hitStopMul = 1.0f; // 0.05 .. 1 (HitStop.ScaleMul)
    float hitStopDur = 1.0f; // 1 .. 10 (HitStop.DurMul): multiplies timings A and B of hit-stop-class events
    bool playerOnly = false; // only events attributed to the current player (PlayerTracker); unknown player => nothing is amplified
    // Explosions / towers: a GIMMICK-owned event (its skill context carries one of gimmickKeys, e.g. 60002 Gimmick_Active_Bomb) counts as player-caused when the
    // player used an apply-skill (arrow / skill) within gimmickWindowMs before it. Any other non-player owner is still never touched.
    bool gimmickOn = false;
    std::vector<uint32_t> gimmickKeys;
    int gimmickWindowMs = 1500;
    // Chain safety cap: slow motion that has been CONTINUOUS (gaps between slow-class events < chainGapMs) for maxChainMs real milliseconds is left at its
    // native values for the rest of that chain (until a gap >= chainGapMs). 0 = no cap.
    int maxChainMs = 6000;
    int chainGapMs = 1500;
    // Blinding Flash (skill key 10276): a CHAIN starts at the player's cast call and ends after bfGapMs without a chain event, at the first non-finale event
    // after a finale, or at the next cast. Inside a chain: finale = ctx key0 in bfFinaleKeys (any weapon), per-hit = every other event.
    // Mode 1: per-hit scale = bfChainSpeed (0.5..2, ABOVE 1 ONLY FOR THESE EVENTS), timings x0.5; mode 2: per-hit scale x bfHitScale, A and B x bfHitRamp
    // (subject to the chain cap); finale (both modes): scale x bfFinaleScale, A and B x bfFinaleRamp. No chain identified => the normal amplifier.
    int bfMode = 0;
    float bfChainSpeed = 1.3f, bfHitScale = 0.5f, bfHitRamp = 1.5f, bfFinaleScale = 0.25f, bfFinaleRamp = 2.5f;
    std::vector<uint32_t> bfFinaleKeys;
    int bfGapMs = 1500;
    std::vector<std::pair<float, float>> overrides;  // orig scale (+-0.005) -> replacement scale
    // --- skill-context filters (an event is amplified only if ALL pass; otherwise originals are written back) ---
    int contextWindowMs = 800;             // 50..5000: how recent a skill call on the same thread may be
    bool ctxAnyThread = true;              // fall back to the newest skill context of ANY thread
    bool requireCtx = false;               // no context => not amplified
    std::vector<uint32_t> allowKeys, denyKeys;  // SkillKeys (decimal); 0 = empty slot, ignored when matching
    int ownerMode = 0;                     // 0 none, 1 auto (= the current player), 2 fixed
    uint64_t ownerFixed = 0;               // a1 when ownerMode == 2
};

struct AmpLogRec {
    uint64_t idx = 0;
    long long t_us = 0;
    DWORD tid = 0;
    uintptr_t D = 0, ret = 0;
    float orig[3] = {0, 0, 0}, applied[3] = {0, 0, 0};  // scale, A, B
    bool haveCtx = false;
    uint64_t ctxA1 = 0;
    uint32_t ctxKeys[4] = {0, 0, 0, 0};
    long long ctxAgeUs = 0;
    bool viaGimmick = false;   // attributed to the player through the gimmick rule
    uint8_t bfKind = 0;        // Blinding Flash treatment: 0 none, 1 bf_hit (per-hit), 2 bf_finale
    int reason = 0;  // 0 amplified; 1 no ctx (RequireCtx), 2 DenyKeys, 3 AllowKeys miss, 4 owner mismatch, 5 owner not learned yet, 6 chain cap
};

struct AmpStats { uint64_t badType = 0, bfHit = 0, bfFinale = 0, bfChains = 0, bfNoFinale = 0, chainCap = 0, gimmick = 0, stopSeen = 0, stopModified = 0, modified = 0, events = 0, distinct = 0, rebased = 0, writeFail = 0, bad = 0, mapFull = 0, filtered = 0; };

// One chain event as seen by the Blinding Flash tracker.
struct BfEvent {
    uint32_t keys[4] = {0, 0, 0, 0};   // the ctx SkillKeys the event was attributed to
    uint8_t kind = 0;                  // 0 left alone (hit-stop blip, capped, ...), 1 bf_hit treated, 2 bf_finale treated, 3 finale key seen but not treatable (e.g. the hit-stop blip)
    long long tUs = 0;
    float orig = 0;
};
struct BfChain {
    int idx = 0, mode = 0;
    long long castUs = 0, startUs = 0, lastUs = 0;
    bool finaleSeen = false, noFinale = false;
    int hits = 0, finales = 0, capped = 0;
    std::string end;                   // finale | timeout | newcast | off
    std::vector<BfEvent> evs;          // ordered (at most 96)
};

// The logic shared by the real detour and the selftest (which drives a private instance with fake data).
class AmpEngine {
public:
    bool logNew = true;  // log the first sight of each D to CombatMod.log
    AmpEngine();
    void OnEvent(uint64_t a3, uintptr_t ret);  // never throws
    void SetParams(const AmpParams& p);
    AmpParams Params();
    void SetEnabled(bool on);                  // off => restore every remembered D
    void RestoreAll();
    void ResetMap();                           // restore, then forget every D
    AmpStats Stats();
    std::vector<AmpLogRec> Tail(size_t n);
    // Blinding Flash chains (newest last); a stale open chain is closed first
    std::vector<BfChain> BfChains(size_t n);
    std::string BfLogText(size_t n);        // `bf log`
    std::string BfChainsJson(size_t n);     // telemetry
    void BfClear();
    bool BfActive();
    // the newest time-event blocks seen (also the ones the layout guard rejected), newest first, distinct: {D, type matched gl::kTimeEventType}
    std::vector<std::pair<uintptr_t, bool>> RecentD(size_t n);
    void SetSkills(SkillEngine* s) { skills_ = s; }
    std::atomic<bool> feedTelemetry{false};   // only the process-wide engine feeds telemetry   // default (null) = the process-wide engine, Skills()

private:
    struct Entry { float orig[3]; float applied[3]; };
    void RestoreLocked();
    std::mutex mu_;
    AmpParams p_;
    std::unordered_map<uintptr_t, Entry> map_;
    std::vector<AmpLogRec> ring_;
    uint64_t seq_ = 0;
    long long chainStartUs_ = 0, chainLastUs_ = 0;   // the current slow-motion chain (guarded by mu_)
    BfChain bf_;                                     // the open Blinding Flash chain (bfActive_)
    bool bfActive_ = false;
    uintptr_t recentD_[8] = {}; bool recentOk_[8] = {}; unsigned recentN_ = 0;
    long long bfLastCast_ = 0;
    std::vector<BfChain> chains_;                    // finished chains, newest last (at most 50)
    int bfIdx_ = 0;
    int BfStep(SkillEngine* se, bool have, const SkillCtx& cx, bool slowClass, bool stopClass, float orig, long long now);   // 0 untouched, 1 per-hit, 2 finale
    void BfEnd(const char* why);
    void BfCloseStale(long long now);
    AmpStats st_;
    SkillEngine* skills_ = nullptr;
};

typedef __int64*(__fastcall* AmpFn)(__int64, uint32_t*, __int64, __int64, void**);
// What the detour does: amplify, then call the original with IDENTICAL arguments and return its value.
__int64* AmpCallThrough(AmpEngine& e, AmpFn fn, __int64 a1, uint32_t* a2, __int64 a3, __int64 a4, void** a5, uintptr_t ret);

void AmpInit();                                  // read [TimeAmp] from the ini; hook only if Enabled=1
void AmpShutdown();                              // restore originals, disable the hook (quit-bridge / unload)
bool AmpOverlaps(uintptr_t addr, size_t n);      // does [addr, addr+n) touch the hooked prologue?
const char* AmpReasonName(int reason);
std::string AmpSummary();                        // for `hooks`
bool AmpHooked();
void AmpFeedTelemetry(bool on);
AmpStats AmpStatsNow();                                // is the time_curve_start hook installed?
void AmpSyncFromSettings();                      // pull the SlowMo.* settings into the engine, install/enable/disable the hook
std::string CmdHitStop(const Args& t);              // hitstop status
std::string CmdAmp(const Args& t);               // amp status|on|off|set|log|reset_map
std::string CmdBf(const Args& t);                // bf status|log [n]|clear   Blinding Flash chains
std::vector<std::pair<uintptr_t, bool>> AmpRecentD(size_t n);   // for `layout check`
std::string AmpBfChainsJson(size_t n);           // the process-wide engine's last chains as JSON (telemetry)
