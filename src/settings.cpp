#include "settings.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace {
const CfgDef kDefs[CFG_COUNT] = {
    {"SlowMo.Enabled", "Slow-motion amplifier on/off", CfgType::Bool, 0, 0, 1, 1, nullptr, false},
    {"SlowMo.ScaleMul", "Slow-motion strength: multiplies the native time scale (lower = slower)", CfgType::Float, 1.0f, 0.05f, 2.0f, 0.05f, nullptr, false},
    {"SlowMo.RampInMul", "Ramp-in length multiplier (event timing A)", CfgType::Float, 1.0f, 0.25f, 4.0f, 0.05f, nullptr, false},
    {"SlowMo.RampOutMul", "Ramp-out length multiplier (event timing B)", CfgType::Float, 1.0f, 0.25f, 4.0f, 0.05f, nullptr, false},
    {"SlowMo.DenyKeys", "Skill keys that never trigger the amplifier (roll / evade); comma list, empty = none", CfgType::List, 0, 0, 0, 0, "10010,10031,10072,10278,10279,10086", true},
    {"SlowMo.Gimmicks", "Also amplify the slow motion of explosions / towers (gimmick-owned) that you caused: one of your skills or arrows shortly before it", CfgType::Bool, 1, 0, 1, 1, nullptr, false},
    {"SlowMo.GimmickKeys", "SkillKeys of gimmick owners (bombs, towers) whose slow motion can count as yours; comma list, empty = none", CfgType::List, 0, 0, 0, 0, "60002", true},
    {"SlowMo.GimmickWindowMs", "How long after one of your skills / arrows (ms) a gimmick's slow motion still counts as caused by you", CfgType::Float, 1500, 200, 5000, 100, nullptr, true},
    {"SlowMo.MaxChainSeconds", "Slow-motion cap per chain: once slow motion has been continuous for this many real seconds (hits < 1.5 s apart), later hits of the chain stay at normal speed until a pause of 1.5 s (it only shortens)", CfgType::Float, 6, 1, 30, 0.5f, nullptr, false},
    {"BlindingFlash.Mode", "Blinding Flash mode: 0 = off (native slow motion / the normal rules), 1 = fast chain + slow finale, 2 = smooth slow motion per hit + slow finale", CfgType::Float, 0, 0, 2, 1, nullptr, false},
    {"BlindingFlash.ChainSpeed", "Mode 1: time scale of the per-hit events of a Blinding Flash chain (1.0 = normal; above 1.0 only with the [Advanced] unlock BlindingFlash.UnlockSpeedUp)", CfgType::Float, 1.0f, 0.5f, 2.0f, 0.05f, nullptr, false},
    {"BlindingFlash.HitScale", "Mode 2: multiplies the scale of each per-hit slow-motion event (lower = slower)", CfgType::Float, 0.5f, 0.2f, 1.0f, 0.05f, nullptr, false},
    {"BlindingFlash.HitRamp", "Mode 2: multiplies the timings (A and B) of each per-hit event", CfgType::Float, 1.5f, 0.5f, 3.0f, 0.05f, nullptr, false},
    {"BlindingFlash.FinaleScale", "Modes 1 and 2: multiplies the scale of the finale (the throw-back hit); lower = slower", CfgType::Float, 0.25f, 0.05f, 1.0f, 0.05f, nullptr, false},
    {"BlindingFlash.FinaleRamp", "Modes 1 and 2: multiplies the timings (A and B) of the finale", CfgType::Float, 2.5f, 0.5f, 4.0f, 0.05f, nullptr, false},
    {"BlindingFlash.FinaleKeys", "Skill key0 values that mark the finale of a Blinding Flash chain (any weapon); comma list. A chain without one is finale-less (bf_nofinale)", CfgType::List, 0, 0, 0, 0, "1023", true},
    {"BlindingFlash.UnlockSpeedUp", "Unlocks Chain speed above 1.0. WARNING: it speeds up EVERYONE (enemies and NPCs too), because the game has one global clock", CfgType::Bool, 0, 0, 1, 1, nullptr, true},
    {"Reaction.Finale", "Hit reaction of the Blinding Flash finale (the throw-back hit): 0 = the game's own, 1 = knock-down, 2 = ground throw, 3 = ragdoll fling; changed for that single hit only", CfgType::Float, 0, 0, 3, 1, nullptr, false},
    {"Reaction.TurningSlash", "Hit reaction of Turning Slash hits shortly after you start it: 0 = the game's own, 1 = knock-down, 2 = ground throw, 3 = ragdoll fling; changed for that single hit only", CfgType::Float, 0, 0, 3, 1, nullptr, false},
    {"Reaction.TurningSlashKeys", "SkillKey values whose use starts the Turning Slash window (10253; add the fist variant's key here); comma list", CfgType::List, 0, 0, 0, 0, "10253", true},
    {"Launch.Enabled", "Launch (knockback) boost on/off", CfgType::Bool, 0, 0, 1, 1, nullptr, false},
    {"Launch.Mult", "Launch strength multiplier", CfgType::Float, 1.0f, 0.5f, 20.0f, 0.1f, nullptr, false},
    {"Launch.AngleMul", "Launch angle multiplier (experimental: scales the two angle floats of the record)", CfgType::Float, 1.0f, 0.25f, 3.0f, 0.05f, nullptr, false},
    {"Launch.MinValue", "Minimum launch: an attack whose launch is below this (even 0) is raised to it BEFORE the strength multiplier (0 = off); a high value makes attacks that normally do not launch at all launch enemies", CfgType::Float, 0, 0, 60, 1, nullptr, false},
    {"Launch.DenyKeys", "Records that are never boosted or raised: entries 'key' or 'key:slot' (slot = the second key value), e.g. 1091:5 = the explosive arrow", CfgType::List, 0, 0, 0, 0, "1091:5,60002", true},
    {"HitStop.Enabled", "Hit-stop control on/off (the tiny near-1.0 impact pauses)", CfgType::Bool, 0, 0, 1, 1, nullptr, false},
    {"HitStop.ScaleMul", "Hit-stop strength: multiplies the scale of an impact pause (lower = deeper dip)", CfgType::Float, 1.0f, 0.05f, 1.0f, 0.05f, nullptr, false},
    {"HitStop.DurMul", "Hit-stop length: multiplies the duration (timings A and B) of an impact pause", CfgType::Float, 1.0f, 1.0f, 10.0f, 0.5f, nullptr, false},
    {"UI.PauseGame", "Freeze the game world while the control panel is open", CfgType::Bool, 1, 0, 1, 1, nullptr, false},
    {"Triggers.Enabled", "Custom slow-motion triggers on/off (rules in the [Triggers] ini section)", CfgType::Bool, 0, 0, 1, 1, nullptr, false},
    {"Triggers.Override", "Let a trigger start even while another time effect is running (rules may then replace a native slow motion)", CfgType::Bool, 0, 0, 1, 1, nullptr, true},
    {"Triggers.TowerBlast", "Slow motion for tower / arrow explosions (they have none of their own): a built-in trigger on the player's explosive-arrow record (key 1091:5)", CfgType::Bool, 0, 0, 1, 1, nullptr, false},
    {"Triggers.TowerScale", "TowerBlast curve: time scale (0.02..1)", CfgType::Float, 0.1f, 0.02f, 1.0f, 0.01f, nullptr, true},
    {"Triggers.TowerIn", "TowerBlast curve: ramp in, seconds (A = in + hold)", CfgType::Float, 0.03f, 0.0f, 5.0f, 0.01f, nullptr, true},
    {"Triggers.TowerHold", "TowerBlast curve: hold, seconds (A = in + hold)", CfgType::Float, 0.35f, 0.0f, 5.0f, 0.01f, nullptr, true},
    {"Triggers.TowerOut", "TowerBlast curve: ramp out, seconds (B = out)", CfgType::Float, 1.4f, 0.0f, 5.0f, 0.01f, nullptr, true},
    {"Triggers.TowerCooldownMs", "TowerBlast: minimum time between two slow motions (ms)", CfgType::Float, 3000, 0, 60000, 100, nullptr, true},
};

const size_t kMaxList = 32;
// removed settings: still recognised in an old ini (ignored with a log line) and by `cfg set` (explained)
const char* const kLegacy[] = {"slowmo.minscale", "slowmo.ignoreabove", "slowmo.playeronly", "launch.playeronly", "launch.maxvalue"};
std::atomic<float> g_v[CFG_COUNT];
const int kLists = 5;
std::atomic<uint32_t> g_lists[kLists][kMaxList];   // 0 = SlowMo.DenyKeys, 1 = SlowMo.GimmickKeys, 2 = Launch.DenyKeys (entries may be key:slot, see KeyEnc), 3 = BlindingFlash.FinaleKeys
std::atomic<int> g_listN[kLists] = {{0}, {0}, {0}, {0}, {0}};
int ListIdx(int id) { return id == SlowMo_DenyKeys ? 0 : id == SlowMo_GimmickKeys ? 1 : id == Launch_DenyKeys ? 2 : id == BlindingFlash_FinaleKeys ? 3 : id == Reaction_TurningSlashKeys ? 4 : -1; }
int ListId(int li) { return li == 0 ? SlowMo_DenyKeys : li == 1 ? SlowMo_GimmickKeys : li == 2 ? Launch_DenyKeys : li == 3 ? BlindingFlash_FinaleKeys : Reaction_TurningSlashKeys; }
std::atomic<bool> g_dirty{false};
std::mutex g_mu;                       // writers only
void (*g_handler)() = nullptr;
std::atomic<int> g_batch{0}, g_off{0};
std::atomic<bool> g_pending{false};
std::mutex g_noteMu;
std::string g_note;

std::vector<uint32_t> DefaultList(int id) {
    std::vector<uint32_t> v;
    const char* s = kDefs[id].defList;
    while (s && *s) {
        const char* comma = strchr(s, ',');
        std::string item = comma ? std::string(s, comma) : std::string(s);
        uint32_t enc = 0;
        std::string e;
        if (ParseKeyEnc(item, enc, e)) v.push_back(enc);
        s = comma ? comma + 1 : nullptr;
    }
    return v;
}

struct Init {
    Init() {
        for (int i = 0; i < CFG_COUNT; ++i) g_v[i].store(kDefs[i].def);
        for (int li = 0; li < kLists; ++li) {
            std::vector<uint32_t> d = DefaultList(ListId(li));
            for (size_t i = 0; i < d.size(); ++i) g_lists[li][i].store(d[i]);
            g_listN[li].store(static_cast<int>(d.size()));
        }
    }
} g_init;

void Notify() {
    if (g_off.load()) return;
    if (g_batch.load()) { g_pending = true; return; }
    if (g_handler) g_handler();
}

std::string ValueStrOf(int id) {
    if (kDefs[id].type == CfgType::Bool) return CfgGet(id) != 0.0f ? "1" : "0";
    if (kDefs[id].type == CfgType::Float) return F("%g", static_cast<double>(CfgGet(id)));
    std::vector<uint32_t> v = CfgList(id);
    std::string o;
    for (uint32_t k : v) o += (o.empty() ? "" : ",") + (id == Launch_DenyKeys ? KeyEncStr(k) : F("%u", k));
    return o.empty() ? "none" : o;
}

bool ParseBool(const std::string& s, bool& out) {
    std::string l = Lower(s);
    if (l == "1" || l == "true" || l == "on" || l == "yes") { out = true; return true; }
    if (l == "0" || l == "false" || l == "off" || l == "no") { out = false; return true; }
    return false;
}

bool ParseKeys(const std::string& s, std::vector<uint32_t>& out, std::string& err, bool pairs = false) {
    out.clear();
    std::string t = Lower(s);
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    if (t.empty() || t == "none") return true;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(',', pos);
        std::string item = t.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? t.size() + 1 : comma + 1;
        uint64_t v;
        if (pairs) {   // "key" or "key:slot"
            uint32_t enc;
            if (!ParseKeyEnc(item, enc, err)) return false;
            v = enc;
        } else if (!ParseNum(item, v) || v == 0 || v > 0xFFFFFFFFull) { err = "bad key '" + item + "' (decimal 1..4294967295)"; return false; }
        if (std::find(out.begin(), out.end(), static_cast<uint32_t>(v)) == out.end()) out.push_back(static_cast<uint32_t>(v));
        if (out.size() > kMaxList) { err = "at most 32 keys"; return false; }
    }
    return true;
}

// internal setters without notification
bool SetValueNoNotify(int id, float v, std::string& err) {
    const CfgDef& d = kDefs[id];
    if (d.type == CfgType::List) { err = "not a numeric setting"; return false; }
    if (!std::isfinite(v)) { err = "value must be finite"; return false; }
    if (d.type == CfgType::Bool) v = v != 0.0f ? 1.0f : 0.0f;
    else if (v < d.min || v > CfgMaxOf(id)) {
        err = F("%s must be within %g .. %g", d.key, static_cast<double>(d.min), static_cast<double>(CfgMaxOf(id)));
        if (id == BlindingFlash_ChainSpeed && v <= d.max) err += " (above 1.0 needs [Advanced] BlindingFlash.UnlockSpeedUp=1: it speeds up EVERYONE, the game clock is global)";
        return false;
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (g_v[id].load() != v) { g_v[id].store(v); g_dirty = true; }
    if (id == BlindingFlash_UnlockSpeedUp && v == 0.0f && g_v[BlindingFlash_ChainSpeed].load() > 1.0f) { g_v[BlindingFlash_ChainSpeed].store(1.0f); g_dirty = true; }   // locking again clamps the speed
    return true;
}

bool SetListNoNotify(int id, const std::vector<uint32_t>& keys, std::string& err) {
    if (kDefs[id].type != CfgType::List) { err = "not a list setting"; return false; }
    if (keys.size() > kMaxList) { err = "at most 32 keys"; return false; }
    std::lock_guard<std::mutex> l(g_mu);
    int li = ListIdx(id);
    g_listN[li].store(0);   // readers see "empty" while it is rewritten rather than a torn list
    for (size_t i = 0; i < keys.size(); ++i) g_lists[li][i].store(keys[i]);
    g_listN[li].store(static_cast<int>(keys.size()));
    g_dirty = true;
    return true;
}

bool SetStrNoNotify(int id, const std::string& value, std::string& err) {
    const CfgDef& d = kDefs[id];
    if (d.type == CfgType::List) {
        std::vector<uint32_t> v;
        return ParseKeys(value, v, err, id == Launch_DenyKeys) && SetListNoNotify(id, v, err);
    }
    if (d.type == CfgType::Bool) {
        bool b;
        if (!ParseBool(value, b)) { err = std::string(d.key) + " is a switch: use 1/0, on/off or true/false"; return false; }
        return SetValueNoNotify(id, b ? 1.0f : 0.0f, err);
    }
    float f;
    if (!ParseFloat(value, f)) { err = std::string(d.key) + " needs a number"; return false; }
    return SetValueNoNotify(id, f, err);
}
}  // namespace

const CfgDef& CfgDefOf(int id) { return kDefs[id]; }
float CfgMaxOf(int id) { return id == BlindingFlash_ChainSpeed && g_v[BlindingFlash_UnlockSpeedUp].load() == 0.0f ? 1.0f : kDefs[id].max; }

bool CfgIsLegacyKey(const std::string& key) {
    std::string k = Lower(key);
    for (const char* l : kLegacy) if (k == l) return true;
    return false;
}

int CfgFind(const std::string& key) {
    std::string k = Lower(key);
    for (int i = 0; i < CFG_COUNT; ++i) if (Lower(kDefs[i].key) == k) return i;
    return -1;
}

float CfgGet(int id) { return id >= 0 && id < CFG_COUNT ? g_v[id].load(std::memory_order_relaxed) : 0.0f; }

std::vector<uint32_t> CfgList(int id) {
    std::vector<uint32_t> v;
    int li = ListIdx(id);
    if (li < 0) return v;
    int n = g_listN[li].load();
    for (int i = 0; i < n && i < static_cast<int>(kMaxList); ++i) v.push_back(g_lists[li][i].load());
    return v;
}

bool CfgKeysMatch(int id, const uint32_t keys[4]) {
    int li = ListIdx(id);
    if (li < 0) return false;
    int n = g_listN[li].load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < static_cast<int>(kMaxList); ++i) if (KeyEncMatch(g_lists[li][i].load(std::memory_order_relaxed), keys)) return true;
    return false;
}

int CfgListCopy(int id, uint32_t* out, int maxN) {
    int li = ListIdx(id);
    if (li < 0) return 0;
    int n = g_listN[li].load(std::memory_order_relaxed), k = 0;
    for (int i = 0; i < n && i < static_cast<int>(kMaxList) && k < maxN; ++i) out[k++] = g_lists[li][i].load(std::memory_order_relaxed);
    return k;
}

bool CfgListHas(int id, uint32_t key) {
    int li = ListIdx(id);
    if (li < 0 || !key) return false;
    int n = g_listN[li].load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < static_cast<int>(kMaxList); ++i) if (g_lists[li][i].load(std::memory_order_relaxed) == key) return true;
    return false;
}

std::string CfgValueStr(int id) { return ValueStrOf(id); }

bool CfgSet(int id, float v, std::string& err) {
    if (id < 0 || id >= CFG_COUNT) { err = "unknown setting"; return false; }
    if (!SetValueNoNotify(id, v, err)) return false;
    Notify();
    return true;
}

bool CfgSetList(int id, const std::vector<uint32_t>& keys, std::string& err) {
    if (!SetListNoNotify(id, keys, err)) return false;
    Notify();
    return true;
}

bool CfgSetStr(const std::string& key, const std::string& value, std::string& err) {
    int id = CfgFind(key);
    if (id < 0 && CfgIsLegacyKey(key)) { err = key + " was removed: it is a fixed internal value now (scale floor 0.02, ignore-above 0.9, player-only always on, launch cap 1000)"; return false; }
    if (id < 0) { err = "unknown setting '" + key + "' (see `cfg list`)"; return false; }
    if (!SetStrNoNotify(id, value, err)) return false;
    Notify();
    return true;
}

void CfgResetAll() {
    CfgBatch b;
    std::string e;
    for (int i = 0; i < CFG_COUNT; ++i) {
        if (kDefs[i].type == CfgType::List) SetListNoNotify(i, DefaultList(i), e);
        else SetValueNoNotify(i, kDefs[i].def, e);
    }
    Notify();
}

void CfgNotifyNow() { Notify(); }

std::string CfgPresetNames() { return "vanilla, cinematic, extreme, heavy"; }

bool CfgPreset(const std::string& name, std::string& err) {
    std::string n = Lower(name);
    struct P { int id; float v; };
    std::vector<P> set;
    bool reset = true;   // presets start from the defaults, except "heavy hits" which only sets the hit-stop values
    if (n == "vanilla") {}
    else if (n == "cinematic") set = {{SlowMo_Enabled, 1}, {SlowMo_ScaleMul, 0.5f}, {SlowMo_RampInMul, 1.6f}, {SlowMo_RampOutMul, 1.6f}, {Launch_Enabled, 1}, {Launch_Mult, 2.0f}};
    else if (n == "extreme") set = {{SlowMo_Enabled, 1}, {SlowMo_ScaleMul, 0.1f}, {SlowMo_RampInMul, 3.0f}, {SlowMo_RampOutMul, 3.0f}, {Launch_Enabled, 1}, {Launch_Mult, 10.0f}};
    else if (n == "heavy" || n == "heavy-hits" || n == "heavy_hits" || n == "heavyhits") { set = {{HitStop_Enabled, 1}, {HitStop_ScaleMul, 0.15f}, {HitStop_DurMul, 5.0f}}; reset = false; }
    else { err = "unknown preset '" + name + "' (" + CfgPresetNames() + ")"; return false; }
    CfgBatch b;
    float keepPause = CfgGet(UI_PauseGame);   // presets describe the mod, not the panel: they never touch UI.PauseGame
    float keepTrig[2] = {CfgGet(Triggers_Enabled), CfgGet(Triggers_TowerBlast)};
    static const int kBf[6] = {BlindingFlash_Mode, BlindingFlash_ChainSpeed, BlindingFlash_HitScale, BlindingFlash_HitRamp, BlindingFlash_FinaleScale, BlindingFlash_FinaleRamp};
    float keepBf[6];
    for (int k = 0; k < 6; ++k) keepBf[k] = CfgGet(kBf[k]);
    float keepMin = CfgGet(Launch_MinValue), keepRe[2] = {CfgGet(Reaction_Finale), CfgGet(Reaction_TurningSlash)};   // personal choices: only Vanilla (everything off) resets them   // the Blinding Flash choice is its own setup: only Vanilla (everything off) resets it
    std::vector<std::pair<int, float>> advV;               // [Advanced] settings are tuning, not looks: a preset never touches them
    std::vector<std::pair<int, std::vector<uint32_t>>> advL;
    for (int i = 0; i < CFG_COUNT; ++i) {
        if (!kDefs[i].advanced) continue;
        if (kDefs[i].type == CfgType::List) advL.emplace_back(i, CfgList(i)); else advV.emplace_back(i, CfgGet(i));
    }
    if (reset) CfgResetAll();
    SetValueNoNotify(UI_PauseGame, keepPause, err);
    if (n != "vanilla") {   // vanilla = everything off (custom triggers too); the other presets leave the triggers as they are
        SetValueNoNotify(Triggers_Enabled, keepTrig[0], err);
        SetValueNoNotify(Triggers_TowerBlast, keepTrig[1], err);
        for (int k = 0; k < 6; ++k) SetValueNoNotify(kBf[k], keepBf[k], err);
        SetValueNoNotify(Launch_MinValue, keepMin, err);
        SetValueNoNotify(Reaction_Finale, keepRe[0], err);
        SetValueNoNotify(Reaction_TurningSlash, keepRe[1], err);
    }
    for (auto& v : advV) SetValueNoNotify(v.first, v.second, err);
    for (auto& l : advL) SetListNoNotify(l.first, l.second, err);
    for (const P& p : set) if (!SetValueNoNotify(p.id, p.v, err)) return false;
    Notify();
    return true;
}

bool CfgDirty() { return g_dirty.load(); }

std::string CfgListText() {
    std::string o;
    for (int i = 0; i < CFG_COUNT; ++i) {
        const CfgDef& d = kDefs[i];
        std::string range = d.type == CfgType::Bool ? "0/1" : d.type == CfgType::Float ? F("%g..%g step %g", static_cast<double>(d.min), static_cast<double>(CfgMaxOf(i)), static_cast<double>(d.step)) : "list of SkillKeys, max 32";
        std::string def = d.type == CfgType::List ? d.defList : d.type == CfgType::Bool ? (d.def != 0 ? "1" : "0") : F("%g", static_cast<double>(d.def));
        o += F("%s = %s   [%s; default %s]  %s%s\n", d.key, ValueStrOf(i).c_str(), range.c_str(), def.c_str(), d.label, d.advanced ? "  (ini [Advanced] only, not in the panel)" : "");
    }
    o += F("dirty=%d (unsaved changes)\n", g_dirty.load() ? 1 : 0);
    return o;
}

// ------------------------------------------------------------------ notification plumbing
CfgHandler CfgSetChangeHandler(CfgHandler fn) { CfgHandler old = g_handler; g_handler = fn; return old; }
CfgBatch::CfgBatch() { g_batch.fetch_add(1); }
CfgBatch::~CfgBatch() {
    if (g_batch.fetch_sub(1) == 1 && g_pending.exchange(false)) Notify();
}
CfgNotifyOff::CfgNotifyOff() { g_off.fetch_add(1); }
CfgNotifyOff::~CfgNotifyOff() { g_off.fetch_sub(1); }

CfgSnapshot CfgTake() {
    CfgSnapshot s;
    for (int i = 0; i < CFG_COUNT; ++i) s.v[i] = g_v[i].load();
    for (int li = 0; li < kLists; ++li) s.lists[li] = CfgList(ListId(li));
    s.dirty = g_dirty.load();
    return s;
}
void CfgRestore(const CfgSnapshot& s) {
    std::lock_guard<std::mutex> l(g_mu);
    for (int i = 0; i < CFG_COUNT; ++i) g_v[i].store(s.v[i]);
    for (int li = 0; li < kLists; ++li) {
        const std::vector<uint32_t>& src = s.lists[li];
        g_listN[li].store(0);
        for (size_t i = 0; i < src.size(); ++i) g_lists[li][i].store(src[i]);
        g_listN[li].store(static_cast<int>(src.size()));
    }
    g_dirty = s.dirty;
}
void CfgNoteSet(const std::string& note) { std::lock_guard<std::mutex> l(g_noteMu); g_note += note; }
std::string CfgTakeNote() { std::lock_guard<std::mutex> l(g_noteMu); std::string n; n.swap(g_note); return n; }

// ------------------------------------------------------------------ ini load / save ([Cinematic] section only)
static std::string TrimS(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool CfgLoad(const std::wstring& path, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot open the ini file"; return false; }
    CfgBatch batch;
    std::string line, section;
    int applied = 0;
    std::string problems;
    std::vector<std::pair<int, std::string>> later;   // everything but the unlock key: it must be applied first (it widens the range of BlindingFlash.ChainSpeed), whatever the order in the file
    while (std::getline(f, line)) {
        line = TrimS(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t e = line.find(']');
            section = e == std::string::npos ? "" : Lower(TrimS(line.substr(1, e - 1)));
            continue;
        }
        if (section != "cinematic" && section != "advanced") continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = TrimS(line.substr(0, eq)), v = TrimS(line.substr(eq + 1));
        int id = CfgFind(k);
        if (id < 0 && CfgIsLegacyKey(k)) { Log("settings: ignored legacy key %s (removed setting)", k.c_str()); continue; }
        if (id < 0) { problems += " unknown key " + k + ";"; continue; }
        std::string e;
        if (id != BlindingFlash_UnlockSpeedUp) { later.emplace_back(id, v); continue; }
        if (SetStrNoNotify(id, v, e)) ++applied; else problems += " " + e + ";";
    }
    for (auto& pr : later) {
        std::string e;
        if (SetStrNoNotify(pr.first, pr.second, e)) ++applied; else problems += " " + e + ";";
    }
    g_dirty = false;
    Notify();
    if (!problems.empty()) Log("settings: load: ignored:%s", problems.c_str());
    err = F("%d settings loaded", applied) + (problems.empty() ? "" : " (some ignored:" + problems + ")");
    return true;
}

bool CfgSave(const std::wstring& path, std::string& err) {
    std::string keep;
    {
        std::ifstream f(path);
        std::string line, section;
        bool skipping = false;
        while (f && std::getline(f, line)) {
            std::string t = TrimS(line);
            if (!t.empty() && t[0] == '[') {
                size_t e = t.find(']');
                std::string nm = e != std::string::npos ? Lower(TrimS(t.substr(1, e - 1))) : "";
                skipping = nm == "cinematic" || nm == "advanced";
            }
            if (!skipping) keep += line + "\n";
        }
    }
    while (keep.size() >= 2 && keep[keep.size() - 1] == '\n' && keep[keep.size() - 2] == '\n') keep.pop_back();   // no blank-line growth
    std::string blk = "\n[Cinematic]\n; Written by CombatMod (`cfg save` / the in-game panel). Switch settings are 1/0.\n";
    for (int i = 0; i < CFG_COUNT; ++i) {
        const CfgDef& d = kDefs[i];
        if (d.advanced) continue;
        blk += F("; %s%s\n", d.label, d.type == CfgType::Float ? F(" (%g..%g)", static_cast<double>(d.min), static_cast<double>(d.max)).c_str() : "");
        blk += F("%s=%s\n", d.key, ValueStrOf(i) == "none" ? "" : ValueStrOf(i).c_str());
    }
    blk += "\n[Advanced]\n; Not shown in the panel.\n";
    for (int i = 0; i < CFG_COUNT; ++i) {
        const CfgDef& d = kDefs[i];
        if (!d.advanced) continue;
        blk += F("; %s\n", d.label);
        blk += F("%s=%s\n", d.key, ValueStrOf(i) == "none" ? "" : ValueStrOf(i).c_str());
    }
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write the ini file"; return false; }
        o << keep << blk;
        if (!o) { err = "write failed"; return false; }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        err = F("cannot replace the ini file (error %lu)", GetLastError());
        return false;
    }
    g_dirty = false;
    err = "saved";
    return true;
}

// ------------------------------------------------------------------ bridge commands
std::string CmdCfg(const Args& t) {
    std::string sub = t.size() > 1 ? Lower(t[1]) : "list";
    if (sub == "list" && t.size() <= 2) return Ok(CfgListText());
    if (sub == "get" && t.size() == 3) {
        int id = CfgFind(t[2]);
        if (id < 0) return Err("unknown setting '" + t[2] + "' (see `cfg list`)");
        return Ok(F("%s = %s\n", kDefs[id].key, ValueStrOf(id).c_str()));
    }
    if (sub == "set" && t.size() >= 4) {
        std::string value;
        for (size_t i = 3; i < t.size(); ++i) value += (i > 3 ? "," : "") + t[i];   // allow "10010, 10031"
        std::string e;
        CfgTakeNote();
        if (!CfgSetStr(t[2], value, e)) return Err(e);
        int id = CfgFind(t[2]);
        return Ok(F("%s = %s\n", kDefs[id].key, ValueStrOf(id).c_str()) + CfgTakeNote());
    }
    if (sub == "save" && t.size() == 2) {
        std::string e;
        if (!CfgSave(g_dir + L"CombatMod.ini", e)) return Err(e);
        Log("settings: saved to CombatMod.ini");
        return Ok("saved the [Cinematic] section of CombatMod.ini\n");
    }
    if (sub == "load" && t.size() == 2) {
        std::string e;
        CfgTakeNote();
        if (!CfgLoad(g_dir + L"CombatMod.ini", e)) return Err(e);
        return Ok(e + "\n" + CfgTakeNote());
    }
    if (sub == "preset" && t.size() == 3) {
        std::string e;
        CfgTakeNote();
        if (!CfgPreset(t[2], e)) return Err(e);
        return Ok("preset " + Lower(t[2]) + " applied (not saved: `cfg save`)\n" + CfgListText() + CfgTakeNote());
    }
    return Err("usage: cfg list | get <key> | set <key> <value> | save | load | preset <" + CfgPresetNames() + ">");
}
