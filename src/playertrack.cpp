#include "playertrack.h"
#include <algorithm>

PlayerTracker::PlayerTracker() {}

void PlayerTracker::Note(uint64_t a1, long long now) noexcept {
    if (!a1) return;
    long long epoch = now / bucketUs;
    Bucket& b = b_[epoch % kB];
    long long e = b.epoch.load();
    if (e != epoch) {  // this bucket still holds an old time slice: recycle it (first thread to notice wins)
        if (b.epoch.compare_exchange_strong(e, epoch)) {
            for (Slot& s : b.s) { s.a1.store(0); s.cnt.store(0); }
        }
    }
    for (int i = 0; i < kS; ++i) {
        uint64_t cur = b.s[i].a1.load();
        if (cur == 0) {
            uint64_t z = 0;
            if (b.s[i].a1.compare_exchange_strong(z, a1)) cur = a1; else cur = b.s[i].a1.load();
        }
        if (cur == a1) { b.s[i].cnt.fetch_add(1); break; }
    }
    long long last = lastEval_.load();
    if (cur_.load() == 0 || now - last >= evalUs) Evaluate(now);
}

// Sums the counts of all buckets inside the window; returns the number of distinct a1 (<= maxOut).
int PlayerTracker::Gather(long long now, Entry* out, int maxOut) const noexcept {
    long long epoch = now / bucketUs;
    int n = 0;
    for (int bi = 0; bi < kB; ++bi) {
        const Bucket& b = b_[bi];
        long long e = b.epoch.load();
        if (e < 0 || e > epoch || e <= epoch - kB) continue;   // outside the sliding window
        for (int i = 0; i < kS; ++i) {
            uint64_t a1 = b.s[i].a1.load(), c = b.s[i].cnt.load();
            if (!a1 || !c) continue;
            int k = 0;
            while (k < n && out[k].a1 != a1) ++k;
            if (k == n) { if (n >= maxOut) continue; out[n++] = Entry{a1, 0}; }
            out[k].count += c;
        }
    }
    return n;
}

void PlayerTracker::Evaluate(long long now) noexcept {
    lastEval_.store(now);
    Entry tot[kB * kS];
    int n = Gather(now, tot, kB * kS);
    if (!n) {
        // nothing inside the window any more: the old player has gone quiet
        if (cur_.load()) { cur_.store(0); since_.store(now); switches_.fetch_add(1); }
        return;
    }
    int bi = 0;
    for (int i = 1; i < n; ++i) if (tot[i].count > tot[bi].count) bi = i;
    uint64_t cur = cur_.load(), curCount = 0;
    for (int i = 0; i < n; ++i) if (tot[i].a1 == cur) curCount = tot[i].count;
    uint64_t best = tot[bi].a1, bestN = tot[bi].count;
    bool change = false;
    if (!cur || curCount == 0) change = true;
    else if (best != cur && static_cast<double>(bestN) >= static_cast<double>(curCount) * challengeRatio && bestN >= curCount + challengeMargin) change = true;
    if (change && best != cur) {
        cur_.store(best);
        since_.store(now);
        switches_.fetch_add(1);
    }
}

std::vector<PlayerTracker::Entry> PlayerTracker::Counts(long long now) const {
    Entry tot[kB * kS];
    int n = Gather(now, tot, kB * kS);
    std::vector<Entry> v(tot, tot + n);
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });
    return v;
}

void PlayerTracker::Reset() {
    cur_.store(0);
    since_.store(0);
    lastEval_.store(0);
    for (Bucket& b : b_) {
        b.epoch.store(-1);
        for (Slot& s : b.s) { s.a1.store(0); s.cnt.store(0); }
    }
}
