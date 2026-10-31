// `launch scan`: finds EVERY skill component record that currently exists in process memory, not only the ones the game happened to use since the
// capture was switched on. A record is recognised byte-wise (records are ODD-aligned, e.g. 0x43A90F59391, so nothing here assumes 4-byte alignment) by
//   gl::kSMark0..2 = FLT_MAX (0x7F7FFFFF) x3    (12 bytes -> the scan pattern, S = hit - gl::kSMark0)
//   gl::kSNeg in [-1, 0)  (-0.4 / -0.5 on real records), the 4 SkillKeys at gl::kSKeys readable,
//   gl::kSStr0 / kSStr1 (launch strength) finite and in [0, 500) - 0/0 is a valid "no launch" record.
// gl::kSAng0 / kSAng1 (the angles) are reported (angle_ok = both finite and in (0, pi)) but not required.   (src/gamelayout.h has the offsets)
// Only committed, private / image, plain read-write regions are scanned (no write-combined, guard, no-access or file-mapped regions). The scan is
// read-only, SEH-guarded, runs in 1 MB chunks on the CALLER's thread (the bridge thread, never a game thread) under a time budget, and can be resumed.
#pragma once
#include "util.h"
#include <vector>

struct LaunchScanRec {
    uint64_t S = 0;
    uint32_t keys[4] = {0, 0, 0, 0};
    float launch[2] = {0, 0};        // gl::kSStr0, kSStr1
    float angles[2] = {0, 0};        // gl::kSAng0, kSAng1
    float f138 = 0;                  // gl::kSNeg
    bool angleOk = false;
    bool Zero() const { return launch[0] <= 0.0f && launch[1] <= 0.0f; }
};

struct LaunchScanStats {
    uint64_t bytes = 0, regions = 0, regionsSkipped = 0, hits = 0, accepted = 0, rejected = 0, faults = 0;
    long long elapsedUs = 0;
    bool done = false;
    uintptr_t cursor = 0;
};

bool LaunchScanValidate(uint64_t S, LaunchScanRec& out);   // does a launch record start at S? (reads memory, SEH-guarded)

class LaunchScan {
public:
    explicit LaunchScan(uintptr_t lo = 0, uintptr_t hi = 0);   // [lo, hi); 0, 0 = the whole user address space
    void Exclude(uintptr_t lo, uintptr_t hi);                   // never scan inside this range (our own probe copies); up to 8
    bool Step(long long budgetUs);                              // scan for at most budgetUs; true once the whole range is done
    bool Done() const { return st_.done; }
    LaunchScanStats Stats() const { return st_; }
    std::vector<LaunchScanRec> Records() const;                 // deduplicated by S, sorted by keys then S

private:
    bool NextRegion();
    void ScanSegment(uintptr_t a, uintptr_t b);
    uintptr_t lo_, hi_, scanPos_, cur_ = 0, regEnd_ = 0;
    struct Range { uintptr_t lo, hi; };
    std::vector<Range> excl_;
    std::vector<LaunchScanRec> recs_;
    std::vector<uint32_t> hitBuf_;
    LaunchScanStats st_;
    unsigned chunks_ = 0;
};

std::string LaunchScanJson(const std::vector<LaunchScanRec>& recs, const LaunchScanStats& st, const std::string& when, size_t cap);   // "null"-free, valid JSON
std::string LaunchScanTable(const std::vector<LaunchScanRec>& recs, int mode, size_t cap);   // mode 0 = grouped by SkillKeys, 1 = every record, 2 = only 0/0 records
std::string LaunchScanLastJson(size_t cap);        // the newest scan as JSON for the telemetry session file ("null" if none)
std::string CmdLaunchScan(const Args& t);          // launch scan [zero|all|more|show [zero|all]]
