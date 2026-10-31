// Skill-context capture: hooks the game's "apply attack skill" function (sig `apply_attack_skill_to_position`, sub_1421EF5A0)
//   __int64 __fastcall f(_QWORD* a1, __int64 a2)      a1 = owner object (rcx), S = *(u64*)(a2+gl::kApplyS) = skill data,
//   SkillKeys = 4 dwords at S+gl::kSKeys.., *(a2+gl::kApplyO160) = another object (attacker/owner?). All offsets: src/gamelayout.h.
// Every call is recorded (4096-entry lock-free ring, same QPC clock as the amp log) and remembered as the thread's
// "last skill context" so the slow-motion amplifier can attribute an event to the skill that caused it.
// The detour is cheap: no heap allocation, no locks, and nothing is held while the original runs.
#pragma once
#include "playertrack.h"
#include "util.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

struct SkillRec {
    uint64_t idx = 0;
    long long t_us = 0;
    DWORD tid = 0;
    uint64_t a1 = 0, a2 = 0, S = 0;
    uint32_t dw[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // 8 dwords from S+gl::kSKeys (the first 4 are the SkillKeys)
    uint64_t o160 = 0, q58 = 0, q60 = 0;          // a2+gl::kApplyO160, a2+gl::kApplyQ58, a2+gl::kApplyQ60
    uintptr_t ret = 0;
};

struct SkillCtx {
    long long t_us = 0;
    uint64_t a1 = 0;
    uint32_t keys[4] = {0, 0, 0, 0};
    DWORD tid = 0;
};

// Raw memory capture around each apply-skill call: 128 preallocated slots, each with 4 dumps BEFORE the original runs
// (and optionally 4 AFTER it returned). Dump order: 0 = a2, 1 = S (*(a2+0x70)), 2 = o160 (*(a2+0x160)), 3 = a1.
static const size_t kRawSlots = 128, kRawMax = 4096;
struct RawHdr {
    uint64_t idx = 0;
    long long t_us = 0;
    DWORD tid = 0;
    uint32_t key0 = 0;
    uint64_t base[4] = {0, 0, 0, 0};          // a2, S, o160, a1 (addresses that were dumped)
    uint32_t sz[4] = {0, 0, 0, 0};            // requested dump sizes
    uint32_t validB[4] = {0, 0, 0, 0}, validA[4] = {0, 0, 0, 0};   // bytes that were actually readable
    bool hasAfter = false;
};
const char* RawName(int which);               // which 0..7: a2,S,o160,a1,a2_after,S_after,o160_after,a1_after
bool RawParseName(const std::string& s, int& which);
std::wstring DevOutputRoot();                          // the project folder developer file output is confined to (with trailing backslash)
bool RawPathLexicalOk(const std::wstring& fullPath);   // must be under DevOutputRoot() (after normalisation)

// Runtime patch rules, applied inside the apply-skill detour BEFORE the original runs (never persisted).
// Regions: 0 = a2 (stack struct: left patched), 1 = S (*(a2+0x70)), 2 = o160 (*(a2+0x160)), 3 = a1 - the persistent ones
// (S, o160, a1) are restored right after the original returns.
static const size_t kMaxRules = 32, kPatchLog = 512;
struct LaunchCfg;
class LaunchEngine;
class TriggerEngine;
struct PatchRule {
    uint32_t id = 0;
    uint8_t active = 0;
    uint8_t keyMode = 0;     // 0 = every call, 1 = key0 in list, 2 = any of the 4 keys (S+gl::kSKeys..) in list
    uint8_t region = 0;      // 0 a2, 1 S, 2 o160, 3 a1
    uint8_t isFloat = 1;     // f32 / u32
    uint8_t op = 0;          // 0 mul, 1 set, 2 add
    uint8_t ownerMode = 0;   // 0 any, 1 player, 2 fixed
    uint16_t nkeys = 0;
    uint32_t keys[8] = {};
    uint32_t offset = 0;     // 0 .. 4092
    uint32_t val = 0;        // f32 bits or u32
    uint64_t owner = 0;
};
struct PatchUndo { uint64_t addr; uint32_t before, patched; uint8_t restore; };
struct PatchLogRec {
    uint64_t idx = 0;
    long long t_us = 0;
    DWORD tid = 0;
    uint32_t key0 = 0, ruleId = 0, offset = 0, before = 0, after = 0;
    uint8_t region = 0, isFloat = 0, op = 0;
};
bool ParsePatchRule(const Args& t, size_t from, PatchRule& r, std::string& err);   // <keys|*> <region> <offset_hex> <f32|u32> <mul|set|add> <value> [owner=..]
std::string PatchRuleStr(const PatchRule& r);
std::string PatchValueStr(bool isFloat, uint32_t bits);

// Hit-reaction override (key2 of the damage record = the crowd-control reaction id). Applied INSIDE the apply-skill detour for the duration of ONE call
// (patch -> original -> restore, through the same undo mechanism as the patch rules): shared records are never changed persistently, and only the
// player's own calls are touched. Finale = a call whose key0 is in finaleKeys while a Blinding Flash chain is open; Turning Slash = a damage call with
// key2 in {1104, 1105, 1107} within tsWindowUs after the player's own call with a key0 in tsKeys.
struct ReactCfg {
    uint32_t finale = 0;             // reaction id written into key2 of a finale call (0 = off; 1107 / 1109 / 1110)
    uint32_t turning = 0;            // same for Turning Slash damage calls
    uint32_t finaleKeys[16] = {};
    int nFinale = 0;
    uint32_t tsKeys[16] = {};
    int nTs = 0;
    long long tsWindowUs = 1200000;  // damage calls this long after the Turning Slash cast count
};
ReactCfg ReactCfgFromSettings();     // Reaction.* + BlindingFlash.FinaleKeys (1 -> 1107, 2 -> 1109, 3 -> 1110)

class SkillEngine {
public:
    std::atomic<bool> enabled{false};
    std::atomic<bool> feedTelemetry{false};   // the process-wide engine feeds the telemetry tables; test engines never do
    SkillEngine();
    void OnCall(uint64_t a1, uint64_t a2, uintptr_t ret) noexcept;       // lock-free, allocation-free
    bool LastCtx(DWORD tid, long long now_us, long long windowUs, SkillCtx& out);   // newest context of THIS thread
    bool LastAny(long long now_us, long long windowUs, SkillCtx& out);              // newest context of any thread
    std::vector<SkillRec> Tail(size_t n);
    std::vector<SkillRec> Since(long long cutoff_us);
    void Clear();                       // forget logged records (thread contexts are kept)
    uint64_t Total() const { return n_.load(); }
    uint64_t Since_clear() const { return n_.load() - clearBase_.load(); }
    size_t ThreadsSeen() const;
    PlayerTracker player;                  // rolling player identity (a1 with the most calls in the last 60 s)
    static const uint32_t kBlindingFlashKey = 10276;   // Skill_LightReflection: the player's apply-skill call that STARTS a Blinding Flash chain
    std::atomic<long long> bfGapUs{1500000};   // a Blinding Flash chain stays open while player calls arrive less than this apart
    std::atomic<long long> bfChainLastUs{0};   // the Blinding Flash chain as the SKILL engine sees it: opened by the cast, kept open by every player call < gap apart
    std::atomic<bool> bfChainOpen{false};
    std::atomic<long long> tsCastUs{0};        // the player's newest call with a Turning Slash key
    std::atomic<uint64_t> reactFinale{0}, reactTurning{0};   // calls whose key2 was overridden (for status / tests)
    static bool ThreadChainOpen();             // was a Blinding Flash chain open at this thread's newest OnCall?
    int ApplyReaction(uint64_t a1, uint64_t a2, const ReactCfg& cfg, long long now, PatchUndo* undo, int n, int maxUndo) noexcept;   // returns the new undo count
    std::atomic<long long> bfCastUs{0};     // time of the player's newest apply-skill call with key0 == kBlindingFlashKey (0 = none yet)
    long long BfCastUs() const { return bfCastUs.load(std::memory_order_relaxed); }
    std::atomic<long long> playerLastUs{0};   // time of the player's newest apply-skill call (0 = none yet)
    bool PlayerRecent(long long now_us, long long windowUs) const { long long t = playerLastUs.load(std::memory_order_relaxed); return t > 0 && now_us >= t && now_us - t <= windowUs; }
    bool Owner(uint64_t& owner) { owner = player.Current(); return owner != 0; }   // the player, if known
    LaunchEngine* launch = nullptr;        // null = the process-wide engine (tests use their own)
    const ReactCfg* reactCfg = nullptr;    // null = the user settings (tests override)
    TriggerEngine* trig = nullptr;         // custom slow-motion triggers: set only on the process-wide engine (tests leave it null)
    const LaunchCfg* launchCfg = nullptr;  // null = the user settings (tests override)

    // ---- raw capture (mode 0 off, 1 before only, 2 before+after) ----
    std::atomic<int> rawMode{0};
    bool RawPrepare();                                            // allocate the 128 slots (~4 MB); call before enabling
    void RawConfigure(int mode, const uint32_t sizes[4]);
    uint64_t RawBegin(uint64_t a1, uint64_t a2) noexcept;         // dump before the call; returns the slot sequence number (0 = none)
    void RawEnd(uint64_t seq) noexcept;                           // dump after the call
    std::vector<RawHdr> RawList(size_t n);
    bool RawGet(uint64_t seq, int which, RawHdr& h, std::vector<uint8_t>& data, std::string& err);
    bool RawSerialize(std::vector<uint8_t>& out);                 // file image for `skills rawsave`
    uint64_t RawCount() const { return rawN_.load(); }

    // ---- patch rules ----
    std::atomic<bool> patchOn{true};                              // global kill switch (rules exist independently)
    bool PatchAdd(const PatchRule& r, uint32_t& id, std::string& err);
    bool PatchDel(uint32_t id);
    void PatchClear();
    std::vector<std::pair<PatchRule, uint64_t>> PatchList();      // rule + times applied
    int PatchRuleCount() const { return ruleCount_.load(); }
    int ApplyPatches(uint64_t a1, uint64_t a2, PatchUndo* undo, int maxUndo) noexcept;   // lock-free; returns #undo entries
    void RestorePatches(const PatchUndo* undo, int n) noexcept;   // newest first; only if the game did not change the field meanwhile
    std::vector<PatchLogRec> PatchTail(size_t n);
    uint64_t PatchNotRestored() const { return notRestored_.load(); }
    uint64_t PlayerA1() { return player.Current(); }              // used by patch rules with owner=player
    void RawSizes(uint32_t sz[4]) const { for (int i = 0; i < 4; ++i) sz[i] = rawSz_[i].load(); }

private:
    struct TSlot {
        std::atomic<DWORD> tid{0};
        std::atomic<uint32_t> seq{0};
        std::atomic<long long> t{0};
        std::atomic<uint64_t> a1{0}, k01{0}, k23{0};
    };
    static const size_t kRing = 4096, kTS = 64;
    TSlot* Find(DWORD tid, bool create);
    static void Publish(TSlot& s, DWORD tid, long long t, uint64_t a1, const uint32_t* keys);
    static bool ReadSlot(const TSlot& s, SkillCtx& out);

    std::unique_ptr<SkillRec[]> ring_;
    std::unique_ptr<std::atomic<uint64_t>[]> stamp_;
    std::atomic<uint64_t> n_{0}, clearBase_{0};
    TSlot ts_[kTS], any_;

    struct RawSlot;
    std::unique_ptr<RawSlot[]> raw_;
    std::atomic<uint64_t> rawN_{0};
    std::atomic<uint32_t> rawSz_[4];

    struct RuleSlot {
        std::atomic<uint32_t> seq{0};      // odd = being written; the detour skips such a rule for that call
        PatchRule r;
        std::atomic<uint64_t> hits{0};
    };
    RuleSlot rules_[kMaxRules];
    std::atomic<int> ruleCount_{0};
    std::atomic<uint32_t> nextRuleId_{1};
    std::mutex ruleMu_;                    // serialises bridge-thread updates only; the detour never takes it
    std::atomic<uint64_t> notRestored_{0};
    std::unique_ptr<PatchLogRec[]> plog_;
    std::unique_ptr<std::atomic<uint64_t>[]> pstamp_;
    std::atomic<uint64_t> plogN_{0};
public:
    ~SkillEngine();
};

typedef __int64(__fastcall* SkillFn)(uint64_t*, __int64);
// What the detour does: capture (if enabled), then call the original with identical args and return its value.
__int64 SkillCallThrough(SkillEngine& e, SkillFn fn, uint64_t* a1, __int64 a2, uintptr_t ret);

SkillEngine& Skills();                           // the process-wide engine used by the real hook
bool SkillsEnsureOn(std::string& msg);           // synchronous (developer commands); a transient failure schedules background retries
void SkillsRequestOn();                          // asynchronous: install in the background with retries, then start capturing           // install (if needed) + enable capture; needs a UNIQUE signature
void SkillsOff();                                // capture off, hook stays as a pass-through
void SkillsInit();                               // [Skills] Enabled=1 in the ini starts capture at load
void SkillsShutdown();                           // disable the hook (quit-bridge / unload)
bool SkillsOverlaps(uintptr_t addr, size_t n);
bool SkillsHooked();                             // is the apply-skill hook installed?
bool ResolveSavePath(const std::string& in, std::wstring& full, std::string& err);   // developer file output: absolute path inside the CombatMod project folder
std::string SkillsSummary();                     // for `hooks`
std::string CmdSkills(const Args& t);            // skills on|off|status|log|summary|clear|raw|rawlist|rawget|rawsave
std::string CmdPlayer(const Args& t);            // player status|reset
std::string CmdMemScanF(const Args& t);          // mem scanf <addr> <len> <minabs> <maxabs>
