// Rolling player identity: the player's a1 (owner object of the apply-skill call) is the a1 with the most apply-skill
// calls over a sliding window (default 60 s). Re-evaluated every few seconds; a challenger must clearly beat the current
// player (hysteresis) so the identity does not flap. Lock-free and allocation-free: usable inside the hook.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

class PlayerTracker {
public:
    struct Entry { uint64_t a1; uint64_t count; };
    PlayerTracker();
    // Tunables (tests shrink them to simulate minutes in milliseconds).
    long long bucketUs = 5000000;     // 12 buckets x 5 s = 60 s window
    long long evalUs = 3000000;       // re-evaluate at most this often
    double challengeRatio = 1.25;     // a challenger needs >= 1.25x the current player's calls ...
    uint64_t challengeMargin = 3;     // ... and at least this many more

    void Note(uint64_t a1, long long now_us) noexcept;       // one apply-skill call by a1
    uint64_t Current() const { return cur_.load(std::memory_order_relaxed); }   // 0 = no player known yet
    void Evaluate(long long now_us) noexcept;                // force a re-evaluation
    std::vector<Entry> Counts(long long now_us) const;       // per-a1 counts inside the window, busiest first
    void Reset();                                            // forget everything (player becomes unknown)
    uint64_t Switches() const { return switches_.load(); }
    long long SinceUs() const { return since_.load(); }      // when the current player was (re)selected

private:
    static const int kB = 12, kS = 16;
    struct Slot { std::atomic<uint64_t> a1{0}; std::atomic<uint64_t> cnt{0}; };
    struct Bucket { std::atomic<long long> epoch{-1}; Slot s[kS]; };
    Bucket b_[kB];
    std::atomic<uint64_t> cur_{0};
    std::atomic<long long> lastEval_{0}, since_{0};
    std::atomic<uint64_t> switches_{0};
    int Gather(long long now_us, Entry* out, int maxOut) const noexcept;
};
