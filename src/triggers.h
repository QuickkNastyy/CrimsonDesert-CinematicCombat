// Custom slow-motion triggers: the game has NO native slow motion for some moments (the Blinding Flash finisher, explosive-arrow
// tower hits, other explosions). A trigger starts OUR OWN curve through the engine's own time controller, exactly like the
// panel pause does (pause.cpp): (scale, 1.0, A, B, 0) is written into both controller objects; the engine advances the progress in
// real time and ends / resets the curve by itself after A+B seconds. Nothing is held or re-asserted by us.
//
//   rule = "when the PLAYER's apply-skill call carries one of these SkillKeys, start curve C"
//   Trigger<n> = key=<SkillKey list>|owner=player|scale=0.15|in=0.10|hold=0.5|out=0.45|cooldownMs=1500|firstOnly=1|delayMs=0|name=<text>
//
// The curve mapping is PROVISIONAL: the engine only knows (scale, A, B) and decides the shape itself, so A = in + hold and B = out.
// The detour side (OnSkill) is lock-free and allocation-free: it only matches and queues a request; a worker thread does the timing
// (delay, "last hit of a chain") and the controller access. All decisions take explicit times and an abstract controller, so the whole
// engine is testable with a fake controller.
#pragma once
#include "util.h"
#include <atomic>
#include <mutex>
#include <vector>

static const int kTrigMax = 16, kTrigQueue = 32, kTrigLog = 256;
static const int kTrigBuiltin = kTrigMax + 1;   // slot NUMBER of the built-in rule (TowerArrow); user rules are 1..kTrigMax

struct TriggerRule {                      // trivially copyable (read under a seqlock)
    char name[24] = {0};
    uint32_t keys[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // KeyEnc entries: "key" (any of the 4 SkillKeys) or "key:slot" (keys[0] == key && keys[1] == slot)
    uint8_t nkeys = 0;
    uint8_t enabled = 1;
    uint8_t firstOnly = 1;                // fire on the first matching call of a burst
    uint8_t lastOnly = 0;                 // fire on the last call of a burst (idleMs without a further matching call)
    uint8_t builtin = 0;                  // 1 = the built-in rule driven by Triggers.TowerBlast (not saved to the ini)
    uint8_t reserved[3] = {0, 0, 0};
    float scale = 0.15f, in = 0.10f, hold = 0.5f, out = 0.45f;
    int32_t cooldownMs = 1500, delayMs = 0, idleMs = 250;
};
bool TriggerParse(const std::string& text, TriggerRule& r, std::string& err);   // "key=..|scale=..|..." (an optional "TriggerN =" prefix is accepted)
std::string TriggerFormat(const TriggerRule& r);                                // round-trips through TriggerParse

// The time controller as the engine sees it (the real one is pause.cpp's chain; tests use a fake).
class TrigCtl {
public:
    virtual ~TrigCtl() {}
    virtual bool Read(float f[2][5]) = 0;          // both controller objects; false = chain unreadable
    virtual bool Write(const float v[5]) = 0;      // one curve into both objects
};
bool TrigCtlIdle(const float f[2][5]);             // both objects idle (f0 < 0)?

struct TrigLogRec {
    long long t_us = 0;
    int rule = -1;                                 // slot number (1-based), 0 = manual `trigger fire`
    char name[24] = {0};
    uint32_t key = 0;
    uint64_t a1 = 0;
    float scale = 0, A = 0, B = 0;
    long long ageUs = 0;                           // skill call -> controller write
    char outcome[56] = {0};
    bool fired = false;
};
struct TrigStats {
    uint64_t matches = 0, fired = 0, burstSuppressed = 0, cooldownSuppressed = 0, skippedPause = 0, skippedBusy = 0, skippedUnreadable = 0, writeFailed = 0;
};

class TriggerEngine {
public:
    TriggerEngine();
    std::atomic<bool> enabled{false};              // master: user rules or the built-in rule are on
    std::atomic<bool> userOn{true};                // Triggers.Enabled: the rules from the ini / `trigger add`
    std::atomic<bool> towerOn{true};               // Triggers.TowerBlast: the built-in rule
    std::atomic<bool> allowOverride{false};        // Triggers.Override: fire even while another time effect runs
    HANDLE wake = nullptr;                         // set by the worker thread; null in tests

    // ---- rules (bridge / ini thread)
    int Add(const TriggerRule& r, std::string& err);        // slot number 1..kTrigMax, or 0
    bool SetBuiltin(const TriggerRule& r);                  // (re)writes the built-in slot; keeps its runtime state when the rule is unchanged
    bool Del(int n);
    void Clear();
    bool SetRuleEnabled(int n, bool on);
    std::vector<std::pair<int, TriggerRule>> List() const;
    int ActiveCount() const;                                // rules that could fire right now (enabled)
    int RuleCount() const;
    uint64_t RuleMatches(int n) const;
    uint64_t RuleFired(int n) const;

    // ---- detour side: lock-free, allocation-free
    void OnSkill(const uint32_t keys[4], uint64_t a1, uint64_t player, long long nowUs) noexcept;

    // ---- worker side (explicit time)
    int Pump(long long nowUs, TrigCtl& ctl, bool paused);            // runs everything that is due; returns how many curves were started
    long long NextDueUs(long long nowUs);                            // microseconds until something is due (0 = now), -1 = nothing pending
    bool FireNow(float scale, float A, float B, TrigCtl& ctl, bool paused, bool force, long long nowUs, std::string& outcome);   // `trigger fire`

    TrigStats Stats() const;
    std::vector<TrigLogRec> LogTail(size_t n) const;                  // newest first
    void ClearLog();

private:
    static constexpr long long kNever = -(1LL << 60);
    struct Req {
        std::atomic<uint64_t> ready{0};            // sequence number + 1 once the fields are valid
        int rule = 0;
        long long due = 0, call = 0;
        uint32_t key = 0;
        uint64_t a1 = 0;
    };
    struct Slot {
        std::atomic<uint32_t> seq{0};              // odd while being rewritten
        std::atomic<uint8_t> used{0};
        TriggerRule r;
        std::atomic<long long> lastMatch{kNever}, lastAccept{kNever};
        std::atomic<bool> lastPending{false};
        std::atomic<uint32_t> lastKey{0};
        std::atomic<uint64_t> lastA1{0}, matches{0}, fired{0};
    };
    struct Sched { int rule; long long due, call; uint32_t key; uint64_t a1; };
    static bool ReadRule(const Slot& s, TriggerRule& out);
    void Push(int slot, long long due, long long call, uint32_t key, uint64_t a1) noexcept;
    void Wake() noexcept;
    bool Attempt(int slotNumber, const char* name, uint32_t key, uint64_t a1, float scale, float A, float B, long long callUs, long long nowUs, TrigCtl& ctl, bool paused, bool force, std::string* outcome);
    void Record(const TrigLogRec& r);

    bool OwnCurveExpired(const float f[2][5], long long nowUs);   // a leftover of OUR last curve that the engine never reset
    Slot s_[kTrigMax + 1];                         // the last slot is the built-in rule
    std::mutex ruleMu_;
    Req q_[kTrigQueue];
    std::atomic<uint64_t> qhead_{0};
    uint64_t qtail_ = 0;
    std::vector<Sched> sched_;                     // worker only
    std::mutex fireMu_;
    float lastScale_ = 0, lastA_ = 0, lastB_ = 0;
    long long lastAt_ = kNever;
    mutable std::mutex logMu_;
    std::vector<TrigLogRec> log_;
    std::atomic<uint64_t> st_[9];
};

TriggerRule TriggerTowerRule(float scale, float in, float hold, float out, int cooldownMs);   // the built-in TowerArrow rule (key 1091:5, player only, firstOnly)
TriggerEngine& Triggers();                         // the process-wide engine used by the real hook
void TriggersInit();                               // [Triggers] rules from the ini, worker thread, hooks the engine into the skill detour
void TriggersShutdown(bool wait);
void TriggersSyncFromSettings();                   // Triggers.* settings -> engine (+ requests the skill hook when something could fire)
std::string TriggersSummary();                     // for `hooks`
std::string TriggerStatusText();                   // one-line live status for the panel
std::string CmdTrigger(const Args& t);             // trigger list|status|add|del|clear|enable|disable|fire|log|save
