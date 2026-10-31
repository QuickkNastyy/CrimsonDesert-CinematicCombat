#include "layoutcheck.h"
#include "gamelayout.h"
#include "log.h"
#include "pause.h"
#include "sigscan.h"
#include "skillctx.h"
#include "timeamp.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
float FAt(const uint8_t* b, size_t off) { float v; memcpy(&v, b + off, 4); return v; }
uint32_t UAt(const uint8_t* b, size_t off) { uint32_t v; memcpy(&v, b + off, 4); return v; }
bool Fin(float f) { return std::isfinite(f); }

std::string DwordTable(const uint8_t* b, size_t from, size_t to, size_t mark0 = ~size_t(0)) {   // "+0x10: 0x3F800000 (float 1) ..." eight dwords per line
    std::string o;
    for (size_t off = from; off + 4 <= to; off += 4) {
        float f = FAt(b, off);
        o += F("    +0x%03zX: %08X  %s%g%s\n", off, UAt(b, off), Fin(f) && std::fabs(f) < 1e6f && std::fabs(f) > 1e-6f ? "float " : "", Fin(f) && std::fabs(f) < 1e6f && std::fabs(f) > 1e-6f ? static_cast<double>(f) : 0.0,
               off == mark0 ? "   <- expected marker block (kSMark0)" : "");
    }
    return o;
}
}  // namespace

const char* LayoutClassifyCtl(const float f[5]) {
    for (int i = 0; i < 5; ++i) if (!Fin(f[i])) return "unexpected";
    if (f[0] == -1.0f && f[1] == -1.0f && f[2] == 0.0f && f[3] == 0.0f && f[4] == 0.0f) return "idle";
    if (f[0] == 0.001f && f[2] == 3600.0f) return "pause";
    // a running curve: (scale, 1.0-ish, A, B, progress). Denormals / zeros (what an unrelated object usually holds) are NOT a scale or a factor.
    if (f[0] >= 0.0005f && f[0] <= 3.0f && f[1] > 0.0f && f[1] <= 3.0f && f[2] >= 0.0f && f[2] <= 3600.0f && f[3] >= 0.0f && f[3] <= 3600.0f && f[4] >= 0.0f && f[4] <= 4000.0f) return "curve";
    return "unexpected";
}

LayoutVerdict LayoutCheckEventBlock(const uint8_t* blk, size_t n) {
    LayoutVerdict v;
    if (n < 0x30) { v.ok = false; v.text = "  (block too short)\n"; return v; }
    uint16_t type;
    memcpy(&type, blk + gl::kEvType, 2);
    std::string bad;
    if (type != gl::kTimeEventType) bad += F("    - the u16 at D+0x%02X is %u (0x%X), expected %u (gl::kTimeEventType / gl::kEvType)\n", static_cast<unsigned>(gl::kEvType), static_cast<unsigned>(type), static_cast<unsigned>(type), gl::kTimeEventType);
    struct F_ { const char* name; uintptr_t off; float lo, hi; };
    const F_ fields[] = {{"scale (kEvScale)", gl::kEvScale, 0.0f, 3.0f}, {"second float (kEvScale2)", gl::kEvScale2, 0.0f, 3.0f}, {"A (kEvA)", gl::kEvA, 0.0f, 60.0f}, {"B (kEvB)", gl::kEvB, 0.0f, 60.0f}};
    for (const F_& f : fields) {
        float x = FAt(blk, f.off);
        if (!Fin(x) || x < f.lo || x > f.hi) bad += F("    - %s at D+0x%02zX = %g: not a plausible value (%g .. %g)\n", f.name, static_cast<size_t>(f.off), static_cast<double>(x), static_cast<double>(f.lo), static_cast<double>(f.hi));
    }
    v.ok = bad.empty();
    v.text = bad.empty() ? F("    OK: type %u, scale %g, second %g, A %g, B %g\n", static_cast<unsigned>(type), static_cast<double>(FAt(blk, gl::kEvScale)), static_cast<double>(FAt(blk, gl::kEvScale2)), static_cast<double>(FAt(blk, gl::kEvA)), static_cast<double>(FAt(blk, gl::kEvB)))
                           : "    MISMATCH:\n" + bad;
    if (!v.ok) {   // help locating the real fields: every plausible float and the first dwords
        v.text += "    plausible time-scale-like floats (0 < f <= 1.5) in the first 0x30 bytes: ";
        bool any = false;
        for (size_t off = 0; off + 4 <= 0x30; off += 4) { float x = FAt(blk, off); if (Fin(x) && x > 0.0f && x <= 1.5f && (UAt(blk, off) >> 23) != 0) { v.text += F("+0x%02zX=%g ", off, static_cast<double>(x)); any = true; } }
        v.text += any ? "\n" : "(none)\n";
        v.text += "    raw dwords:\n" + DwordTable(blk, 0, 0x30);
    }
    return v;
}

SkillRecCheck LayoutCheckSkillRecord(const uint8_t* blk, size_t n) {
    SkillRecCheck c;
    if (n < gl::kSAng1 + 4) return c;
    c.readable = true;
    for (int i = 0; i < 4; ++i) c.keys[i] = UAt(blk, gl::kSKeys + 4 * static_cast<size_t>(i));
    c.keysPlausible = c.keys[0] >= 1 && c.keys[0] < 1000000 && c.keys[1] < 1000000 && c.keys[2] < 1000000 && c.keys[3] < 1000000;
    c.markers = UAt(blk, gl::kSMark0) == gl::kFltMaxBits && UAt(blk, gl::kSMark1) == gl::kFltMaxBits && UAt(blk, gl::kSMark2) == gl::kFltMaxBits;
    float neg = FAt(blk, gl::kSNeg), s0 = FAt(blk, gl::kSStr0), s1 = FAt(blk, gl::kSStr1);
    c.negOk = Fin(neg) && neg >= -1.0f && neg < 0.0f;
    c.strengthOk = Fin(s0) && s0 >= 0.0f && s0 < 500.0f && Fin(s1) && s1 >= 0.0f && s1 < 500.0f;
    for (size_t off = 0; off + 12 <= n; off += 4)
        if (UAt(blk, off) == gl::kFltMaxBits && UAt(blk, off + 4) == gl::kFltMaxBits && UAt(blk, off + 8) == gl::kFltMaxBits) { c.markerAt = static_cast<int>(off); break; }
    return c;
}

std::string LayoutJson() {
    return F("{\"build\":%u,\"time_event_type\":%u,\"ev_type\":%u,\"ev_scale\":%u,\"ev_scale2\":%u,\"ev_a\":%u,\"ev_b\":%u,\"ev_data_in_args\":%u,\"ev_table_base\":%u,\"ev_table_stride\":%u,\"handler_fn_in_vtable\":%u,"
             "\"ctl_obj\":[%u,%u],\"apply_s\":%u,\"apply_o160\":%u,\"s_keys\":%u,\"s_half\":%u,\"s_neg\":%u,\"s_marker\":[%u,%u,%u],\"s_strength\":[%u,%u],\"s_angle\":[%u,%u]}",
             gl::kBuild, gl::kTimeEventType, static_cast<unsigned>(gl::kEvType), static_cast<unsigned>(gl::kEvScale), static_cast<unsigned>(gl::kEvScale2), static_cast<unsigned>(gl::kEvA), static_cast<unsigned>(gl::kEvB),
             static_cast<unsigned>(gl::kEvDataInArgs), static_cast<unsigned>(gl::kEvTableBase), static_cast<unsigned>(gl::kEvTableStride), static_cast<unsigned>(gl::kHandlerFnInVtable), static_cast<unsigned>(gl::kCtlObj[0]),
             static_cast<unsigned>(gl::kCtlObj[1]), static_cast<unsigned>(gl::kApplyS), static_cast<unsigned>(gl::kApplyO160), static_cast<unsigned>(gl::kSKeys), static_cast<unsigned>(gl::kSHalf), static_cast<unsigned>(gl::kSNeg),
             static_cast<unsigned>(gl::kSMark0), static_cast<unsigned>(gl::kSMark1), static_cast<unsigned>(gl::kSMark2), static_cast<unsigned>(gl::kSStr0), static_cast<unsigned>(gl::kSStr1), static_cast<unsigned>(gl::kSAng0),
             static_cast<unsigned>(gl::kSAng1));
}

static std::string Show() {
    return F("build target %u (src/gamelayout.h)\n"
             "time event D = *(a3+0x%zX): type u16 at +0x%zX == %u | scale +0x%zX, second float +0x%zX, A +0x%zX, B +0x%zX\n"
             "dispatcher: H = *(a1 + 0x%zX + 0x%zX*type), fn = *(*H + 0x%zX) [unverified]\n"
             "time controller: P = **G, objects at P+0x%zX and P+0x%zX (f0..f4, idle = -1,-1,0,0,0)\n"
             "apply-skill: S = *(a2+0x%zX), a2+0x%zX = object, a2+0x%zX/0x%zX = qwords\n"
             "skill record S: keys +0x%zX..+0x%zX | 0.5 at +0x%zX | neg float +0x%zX | FLT_MAX x3 at +0x%zX +0x%zX +0x%zX | strengths +0x%zX +0x%zX | angles +0x%zX +0x%zX\n",
             gl::kBuild, static_cast<size_t>(gl::kEvDataInArgs), static_cast<size_t>(gl::kEvType), gl::kTimeEventType, static_cast<size_t>(gl::kEvScale), static_cast<size_t>(gl::kEvScale2), static_cast<size_t>(gl::kEvA), static_cast<size_t>(gl::kEvB),
             static_cast<size_t>(gl::kEvTableBase), static_cast<size_t>(gl::kEvTableStride), static_cast<size_t>(gl::kHandlerFnInVtable), static_cast<size_t>(gl::kCtlObj[0]), static_cast<size_t>(gl::kCtlObj[1]), static_cast<size_t>(gl::kApplyS),
             static_cast<size_t>(gl::kApplyO160), static_cast<size_t>(gl::kApplyQ58), static_cast<size_t>(gl::kApplyQ60), static_cast<size_t>(gl::kSKeys), static_cast<size_t>(gl::kSKeys + 12), static_cast<size_t>(gl::kSHalf),
             static_cast<size_t>(gl::kSNeg), static_cast<size_t>(gl::kSMark0), static_cast<size_t>(gl::kSMark1), static_cast<size_t>(gl::kSMark2), static_cast<size_t>(gl::kSStr0), static_cast<size_t>(gl::kSStr1), static_cast<size_t>(gl::kSAng0),
             static_cast<size_t>(gl::kSAng1));
}

static std::string CheckEvents(bool& allOk) {
    std::string o = "[1] time-event blocks (the hook time_curve_start)\n";
    auto recent = AmpRecentD(4);
    if (recent.empty()) {
        allOk = false;
        return o + "    no time event seen yet: the amplifier must be running (telemetry on, or SlowMo / HitStop / Blinding Flash on) and the game must have played a slow motion or hit-stop. Try again after a fight.\n";
    }
    int good = 0;
    for (auto& d : recent) {
        uint8_t blk[0x40];
        o += F("  D = 0x%llX (the layout guard %s it)\n", U64(d.first), d.second ? "accepted" : "REJECTED");
        if (!SafeRead(d.first, blk, sizeof blk)) { o += "    unreadable now\n"; continue; }
        LayoutVerdict v = LayoutCheckEventBlock(blk, sizeof blk);
        o += v.text;
        if (v.ok) ++good;
    }
    if (!good) allOk = false;
    o += good ? F("  => event layout OK for %d of %zu blocks\n", good, recent.size()) : "  => event layout WRONG: fix gl::kTimeEventType / kEvType / kEvScale / kEvScale2 / kEvA / kEvB (the amplifier leaves rejected blocks alone)\n";
    return o;
}

static std::string CheckSkills(bool& allOk) {
    std::string o = "[3] skill records (the hook apply_attack_skill_to_position; capture must be on)\n";
    if (!SkillsHooked() || !Skills().enabled.load()) { allOk = false; return o + "    skill capture is off / the hook is not installed: `skills on` (or switch a feature on) and use a few skills, then check again\n"; }
    std::vector<SkillRec> tail = Skills().Tail(200);
    std::vector<uint64_t> seen;
    int n = 0, sReadable = 0, keysOk = 0, markersOk = 0, markersElsewhere = 0, negOk = 0, strOk = 0;
    std::vector<int> elsewhere;
    uint64_t firstBad = 0;
    for (auto it = tail.rbegin(); it != tail.rend() && n < 40; ++it) {
        if (!it->S || std::find(seen.begin(), seen.end(), it->S) != seen.end()) continue;
        seen.push_back(it->S);
        ++n;
        std::vector<uint8_t> b(gl::kSCheckSpan);
        size_t got = gl::kSCheckSpan;
        while (got >= gl::kSAng1 + 4 && !SafeRead(static_cast<uintptr_t>(it->S), b.data(), got)) got -= 0x40;
        if (got < gl::kSAng1 + 4) continue;
        ++sReadable;
        SkillRecCheck c = LayoutCheckSkillRecord(b.data(), got);
        keysOk += c.keysPlausible;
        if (c.markers) { ++markersOk; negOk += c.negOk; strOk += c.strengthOk; }
        else if (c.markerAt >= 0) { ++markersElsewhere; if (std::find(elsewhere.begin(), elsewhere.end(), c.markerAt) == elsewhere.end()) elsewhere.push_back(c.markerAt); }
        if (!firstBad && !c.markers && !c.keysPlausible) firstBad = it->S;
    }
    o += F("    records examined: %d distinct S (S pointer readable for %d)\n", n, sReadable);
    o += F("    SkillKeys at S+0x%zX..: plausible (key0 1..999999, others < 1e6) for %d of %d\n", static_cast<size_t>(gl::kSKeys), keysOk, sReadable);
    o += F("    FLT_MAX marker block at the expected S+0x%zX/0x%zX/0x%zX: %d records (of those: neg float in [-1,0) at S+0x%zX: %d, strengths at S+0x%zX/0x%zX in [0,500): %d)\n", static_cast<size_t>(gl::kSMark0), static_cast<size_t>(gl::kSMark1), static_cast<size_t>(gl::kSMark2),
           markersOk, static_cast<size_t>(gl::kSNeg), negOk, static_cast<size_t>(gl::kSStr0), static_cast<size_t>(gl::kSStr1), strOk);
    if (markersElsewhere) {
        o += F("    marker triple found at ANOTHER offset in %d records: ", markersElsewhere);
        for (int e : elsewhere) o += F("S+0x%X (expected 0x%zX, shift %+d) ", e, static_cast<size_t>(gl::kSMark0), e - static_cast<int>(gl::kSMark0));
        o += "\n";
    }
    bool layoutOk = sReadable > 0 && keysOk * 2 >= sReadable && markersOk > 0 && negOk == markersOk && strOk == markersOk;
    if (!layoutOk) allOk = false;
    if (sReadable == 0) o += "  => no readable skill record: gl::kApplyS (S = *(a2+kApplyS)) is probably wrong, or no skill was used yet\n";
    else if (keysOk * 2 < sReadable) o += "  => SkillKeys look wrong: fix gl::kSKeys (the capture prints the keys it read; the table below shows the first suspicious record)\n";
    else if (markersOk == 0 && markersElsewhere) o += "  => the marker block moved: shift gl::kSNeg / kSMark0..2 / kSStr0..1 / kSAng0..1 / kSHalf by the amount reported above (plain attack records may legitimately have no marker block, but one launch record must)\n";
    else if (markersOk == 0) o += "  => no record has the FLT_MAX marker block anywhere: use a launching skill (push / force palm) and check again; only launch records carry it\n";
    else if (negOk != markersOk || strOk != markersOk) o += "  => marker block found, but the neighbouring floats (kSNeg / kSStr0 / kSStr1) look wrong for some records\n";
    else o += F("  => skill-record layout OK (%d launch records carry the marker block at the expected offsets)\n", markersOk);
    if (firstBad && !layoutOk) {
        std::vector<uint8_t> b(0x180);
        if (SafeRead(static_cast<uintptr_t>(firstBad), b.data(), b.size())) o += F("    dwords of a suspicious record S=0x%llX (+0x100 .. +0x17F):\n", U64(firstBad)) + DwordTable(b.data(), 0x100, 0x180, gl::kSMark0);
    }
    return o;
}

std::string CmdLayout(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "check";
    if (sub == "show" && t.size() <= 2) return Ok(Show());
    if (sub != "check" || t.size() > 2) return Err("usage: layout check | layout show");
    bool allOk = true;
    ModInfo mi = MainModule();
    std::string o = F("layout check for build target %u (src/gamelayout.h) - module base 0x%llX size 0x%llX\n", gl::kBuild, U64(mi.base), U64(mi.size));
    o += CheckEvents(allOk);
    bool ctlOk = true;
    o += "[2] time controller (sig time_mgr_global)\n" + PauseCtlReport(ctlOk);
    if (!ctlOk) allOk = false;
    o += CheckSkills(allOk);
    {   // the dispatcher table is only reported (telemetry enumerates it): a wrong base shows up as an empty `etrace` / event catalog
        o += "[4] dispatcher table (event_dispatcher; layout unverified): telemetry status / `etrace on` + a few events show whether handlers are found; not checked here\n";
    }
    o += allOk ? "RESULT: every checked part of the layout matches\n" : "RESULT: SOME PARTS DO NOT MATCH (see the => lines)\n";
    Log("layout check: %s", allOk ? "all matched" : "mismatches found");
    return Ok(o);
}
