#include "uimodel.h"
#include "log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// ------------------------------------------------------------------ model
UiModel::UiModel() {
    int accent = 0;
    auto section = [&](const char* label, int acc, const char* tag = "") {
        UiRow r;
        r.kind = UiKind::Header; r.label = label; r.accent = acc; r.tag = tag;
        accent = acc;
        rows_.push_back(r);
    };
    // add(kind, label, title, help, tip, cfg, fmt, experimental)
    auto add = [&](UiKind k, const char* label, const char* title, const char* help, const char* tip, int cfg = -1, const char* fmt = "%g", bool exp = false) {
        UiRow r;
        r.kind = k; r.label = label; r.title = title; r.help = help; r.tip = tip; r.cfg = cfg; r.fmt = fmt; r.experimental = exp; r.accent = accent;
        rows_.push_back(r);
    };

    section("General", 0);
    add(UiKind::Master, "Mod ON / OFF", "Mod ON / OFF",
        "Switches the slow-motion and launch features on or off together. Each one also has its own checkbox below. Only your own attacks are ever affected: enemy attacks, explosions and your rolls/evades are never touched.",
        "Hit-stop and custom triggers have their own switches. Pick Vanilla in the presets to switch everything off at once.");
    add(UiKind::Check, "Pause game while open", "Pause game while open", "Freezes the game world while this panel is open.",
        "Turn it off if you want to watch the effect live while you adjust a slider.", UI_PauseGame);

    section("Slow motion", 1);
    add(UiKind::Check, "Enabled", "Slow motion: enabled",
        "Turns the slow-motion amplifier on. Off = the game's own slow motion is left exactly as it is. Only slow motion caused by your own attack skills is changed.",
        "Start with Scale 0.5 and a longer Ramp out for a smooth return.", SlowMo_Enabled);
    add(UiKind::Check, "Explosions and towers", "Slow motion: explosions and towers",
        "Also amplify the slow motion of explosions and towers (bombs, explosive arrows) that you set off. An explosion counts as yours when you used a skill or shot an arrow within about 1.5 seconds before it. Explosions that happen without a skill of yours just before are never touched.",
        "Switch it off if slow motion shows up on explosions you did not cause: an enemy's explosion right after your attack can be mistaken for yours.", SlowMo_Gimmicks);
    add(UiKind::Slider, "Scale", "Slow motion: Scale",
        "How strong the slow motion is. Multiplies the game's own slow-motion speed. 0.5 = twice as slow as normal; lower = slower.",
        "Around 0.3 looks dramatic; below 0.1 the action almost stops.", SlowMo_ScaleMul, "%.2fx");
    add(UiKind::Slider, "Ramp in", "Slow motion: Ramp in",
        "Stretch the two time segments each slow-motion event has: the first part (slowing into the effect and holding it) and the second part (easing back to normal speed). 2.0 = that part lasts twice as long, 0.5 = half as long. Longer ramp out = smoother return.",
        "This stretches the first part. Raise it to hold the slow motion longer.", SlowMo_RampInMul, "%.2fx");
    add(UiKind::Slider, "Ramp out", "Slow motion: Ramp out",
        "Stretch the two time segments each slow-motion event has: the first part (slowing into the effect and holding it) and the second part (easing back to normal speed). 2.0 = that part lasts twice as long, 0.5 = half as long. Longer ramp out = smoother return.",
        "This stretches the second part. Raise it for a gentler return to normal speed.", SlowMo_RampOutMul, "%.2fx");

    add(UiKind::Slider, "Slow-motion cap per chain", "Slow motion: cap per chain",
        "Safety limit for long fight chains. When slow motion has been going on continuously (hits less than 1.5 seconds apart) for this many real seconds, the following hits of the same chain play at normal speed until you stop hitting for 1.5 seconds. This only SHORTENS slow motion in long combos: it never makes slow motion longer, and the highest value (30 s) simply means almost no limit.",
        "6 seconds is a good start. Lower it if a long chain feels endless; raise it to allow more slow motion per chain.", SlowMo_MaxChainSeconds, "%.1f s");

    section("Blinding Flash", 7);
    {
        static const char* const kModeNames[3] = {"Off", "Fast", "Smooth"};
        add(UiKind::Slider, "Mode", "Blinding Flash: Mode",
            "Blinding Flash is a chain of dashes from enemy to enemy that ends with a throw-back finale. Off = the game's own slow motion and your normal slow-motion settings. Fast = the dashes between targets run at the Chain speed you choose and only the finale is slow motion. Smooth = every hit gets its own gentle slow motion and the finale is slower still. Works with every weapon.",
            "Fast gives the 'extremely fast from target to target' look. The sliders below belong to the chosen mode.", BlindingFlash_Mode, "%g");
        rows_.back().names = kModeNames; rows_.back().nNames = 3;
        add(UiKind::Slider, "Chain speed", "Blinding Flash: Chain speed",
            "Mode Fast: how fast the dashes between targets run (1.0 = normal speed, lower = slower). Speeds above 1.0 are locked: the game has ONE clock for everything, so a faster chain would speed up enemies and everything else as well.",
            "Leave it at 1.0 for the normal dash speed; the finale below is what makes this mode feel different.", BlindingFlash_ChainSpeed, "%.2fx");
        rows_.back().modeMask = 1 << 1;
        add(UiKind::Slider, "Per-hit slow motion", "Blinding Flash: Per-hit slow motion",
            "Mode Smooth: how strong the slow motion of each hit of the chain is. Multiplies the game's own per-hit slow motion: 0.5 = twice as slow as the game's; lower = slower.",
            "Move towards 1.0 for a lighter effect. Very long chains are limited by Longest chain above.", BlindingFlash_HitScale, "%.2fx");
        rows_.back().modeMask = 1 << 2;
        add(UiKind::Slider, "Per-hit ramp", "Blinding Flash: Per-hit ramp",
            "Mode Smooth: stretches the timing of each hit's slow motion, so it lasts longer and returns more softly. 1.5 = half again as long as the game's.",
            "Higher = smoother and longer. Lower = snappier.", BlindingFlash_HitRamp, "%.2fx");
        rows_.back().modeMask = 1 << 2;
        add(UiKind::Slider, "Finale slow motion", "Blinding Flash: Finale slow motion",
            "Fast and Smooth: the slow motion of the finale, the throw-back hit at the end of the chain. Multiplies the game's own slow motion for it: lower = slower. The tiny impact pause of the finale stays under the Hit-stop settings.",
            "0.25 is a strong, readable slow motion; 0.1 nearly stops. The finale is recognised by BlindingFlash.FinaleKeys in CombatMod.ini.", BlindingFlash_FinaleScale, "%.2fx");
        rows_.back().modeMask = (1 << 1) | (1 << 2);
        add(UiKind::Slider, "Finale ramp", "Blinding Flash: Finale ramp",
            "Fast and Smooth: stretches the timing of the finale, so its slow motion lasts longer. 2.5 = two and a half times as long as the game's.",
            "Raise it to savour the finale; lower it if it drags.", BlindingFlash_FinaleRamp, "%.2fx");
        rows_.back().modeMask = (1 << 1) | (1 << 2);
    }

    section("Hit-stop", 2, "experimental");
    add(UiKind::Check, "Enabled", "Hit-stop: enabled",
        "Experimental. On impacts the game briefly dips its speed to about 99% for a few hundredths of a second. This switch lets the two sliders below deepen and stretch that tiny pause so hits feel heavier.",
        "The Heavy hits preset switches this on with strength 0.15 and length 5.", HitStop_Enabled, "%g", true);
    add(UiKind::Slider, "Strength", "Hit-stop: Strength",
        "Experimental. How deep the impact pause is. Lower = deeper dip: 0.1 = almost frozen for the duration of the pause. 1.0 = unchanged.",
        "Lower is heavier. Combine with Length: a deep dip that lasts only a few hundredths of a second is hard to notice.", HitStop_ScaleMul, "%.2fx", true);
    add(UiKind::Slider, "Length", "Hit-stop: Length",
        "Experimental. How long each impact pause lasts. 1.0 = the game's own (about 0.07 s); 6.0 = six times longer. Strength and length together make hits feel heavy, e.g. strength 0.1 with length 6.0 is about 0.4 s of near-freeze.",
        "Start around 4-6 and lower the Strength until hits feel right.", HitStop_DurMul, "%.1fx", true);

    section("Launch", 3);
    add(UiKind::Check, "Enabled", "Launch: enabled",
        "Turns the launch (knockback) boost on. Off = enemies are launched exactly as in the vanilla game. Only launches caused by your own attacks are boosted; explosions or enemies throwing things at you never are.",
        "A launching skill takes effect the next time you use it.", Launch_Enabled);
    add(UiKind::Slider, "Strength", "Launch: Strength",
        "Multiplies how hard enemies are thrown by launching attacks (like the Force Palm finisher). 2.0 = about twice as far.",
        "2.0 to 3.0 is a good range for a visible but believable difference.", Launch_Mult, "%.1fx");
    add(UiKind::Slider, "Minimum launch", "Launch: Minimum launch",
        "Some attacks have little or no launch of their own, so multiplying it changes nothing. This raises the launch of such attacks to at least this value BEFORE the strength multiplier is applied. 0 = off. A HIGH minimum makes attacks that normally do not launch at all (many ordinary attacks) launch enemies. Explosive arrows and bombs are never raised.",
        "Try 20 to 30 for a noticeable but believable effect; 0 keeps the game's own launches. Needs Launch: Enabled.", Launch_MinValue, "%.0f");
    rows_.back().zeroText = "off";
    add(UiKind::Slider, "Angle [experimental]", "Launch: Angle",
        "Experimental. Multiplies the launch angle values stored with the launch (25 degrees on Force Palm). Higher = steeper/higher arc (unverified).",
        "If a launch looks wrong, set this back to 1.00x.", Launch_AngleMul, "%.2fx", true);

    {
        static const char* const kReact[4] = {"Native", "Knock", "Throw", "Ragdoll"};
        add(UiKind::Slider, "Finale throw", "Launch: Finale throw (Blinding Flash)",
            "Makes the finale of Blinding Flash (the throw-back hit) knock enemies back the same way every time. The game picks a different reaction per cast (push, knock-down or ground throw), so the result is inconsistent. Native = the game's own choice. Knock = knock-down. Throw = smashed into the ground, then thrown (the most reliable). Ragdoll = ragdoll fling. Only your own finale hits are changed, only while a Blinding Flash chain is running, and only for that single hit.",
            "Throw is a good start. Enemies that cannot be thrown (bosses, shielded enemies) are not affected.", Reaction_Finale, "%g");
        rows_.back().names = kReact; rows_.back().nNames = 4;
        add(UiKind::Slider, "Turning Slash throw", "Launch: Turning Slash throw",
            "Makes the hits of Turning Slash (and Tiger's Fury with fists) knock enemies back with the chosen reaction instead of a light push. Only your own hits that land about a second after you start Turning Slash are changed, one hit at a time; every other attack, and every enemy, is left alone.",
            "Native keeps the game's behaviour. Throw gives the strongest result.", Reaction_TurningSlash, "%g");
        rows_.back().names = kReact; rows_.back().nNames = 4;
    }

    section("Custom triggers", 4);
    add(UiKind::Check, "Tower and arrow explosions", "Custom triggers: Tower and arrow explosions",
        "Adds slow motion to tower and explosive-arrow explosions, which have none of their own in the game. When you shoot an explosive arrow, a short, deep slow motion starts. Only your own arrows are used, never enemies or the explosions of other things.",
        "The curve can be tuned in CombatMod.ini under [Advanced] (Triggers.TowerScale, TowerIn, TowerHold, TowerOut, TowerCooldownMs).", Triggers_TowerBlast);
    add(UiKind::Check, "Your own rules", "Custom triggers: your own rules",
        "Adds slow motion to moments where the game has none of its own. A trigger is a rule that watches for one of your own skills and then starts a short slow-motion curve. Triggers never react to enemies. Without rules this switch does nothing.",
        "The rules live in the [Triggers] section of CombatMod.ini (see the README). The Blinding Flash finisher already has slow motion of its own: the Slow motion section amplifies it.", Triggers_Enabled);
    section("Presets", 5);
    {
        UiRow presets;
        presets.kind = UiKind::ButtonRow;
        presets.title = "Presets";
        presets.accent = accent;
        presets.help = "Vanilla = everything off. Cinematic = slow motion x0.5 with 1.6x longer ramps and launch x2. Heavy hits = only the hit-stop (strength 0.15, length x5); slow motion and launch stay as they are. Extreme = slow motion x0.1 with 3x ramps and launch x10.";
        presets.tip = "Presets are not saved until you press Save & Close. Close without saving undoes them.";
        presets.buttons = {{"Vanilla", UiAct::PresetVanilla}, {"Cinematic", UiAct::PresetCinematic}, {"Heavy hits", UiAct::PresetHeavy}, {"Extreme", UiAct::PresetExtreme}};
        rows_.push_back(presets);
        UiRow acts;
        acts.kind = UiKind::ButtonRow;
        acts.title = "Save & Close / Close without saving";
        acts.accent = accent;
        acts.help = "Save & Close writes your settings to CombatMod.ini and resumes the game. Close without saving undoes every change made since the panel was opened.";
        acts.tip = "Keyboard: Esc closes without saving. Controller: B closes without saving, Start saves and closes.";
        acts.buttons = {{"Save & Close", UiAct::SaveClose}, {"Close without saving", UiAct::Discard}};
        rows_.push_back(acts);
    }
    section("Status", 6);
    savePath = g_dir + L"CombatMod.ini";
    for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i].Interactive()) { sel_ = static_cast<int>(i); break; }   // the Mod ON / OFF row
    session = CfgTake();
}

UiHelp UiModel::HelpFor(int row) const {
    UiHelp h;
    if (row < 0 || row >= static_cast<int>(rows_.size())) return h;
    const UiRow& r = rows_[static_cast<size_t>(row)];
    h.title = r.title.empty() ? r.label : r.title;
    h.body = r.help;
    h.tip = r.tip;
    char b[64];
    if (r.kind == UiKind::Slider) {
        const CfgDef& d = CfgDefOf(r.cfg);
        std::string lo = FmtValue(r, d.min), hi = FmtValue(r, CfgMaxOf(r.cfg)), df = FmtValue(r, d.def);
        (void)b;
        h.facts.push_back({"Range", lo + " to " + hi});
        h.facts.push_back({"Default", df});
        h.facts.push_back({"Current", ValueText(row)});
    } else if (r.kind == UiKind::Check) {
        h.facts.push_back({"Default", CfgDefOf(r.cfg).def != 0.0f ? "On" : "Off"});
        h.facts.push_back({"Current", CfgBool(r.cfg) ? "On" : "Off"});
    } else if (r.kind == UiKind::Master) {
        h.facts.push_back({"Default", "Off"});
        h.facts.push_back({"Current", MasterOn() ? "ON" : "OFF"});
    }
    return h;
}

UiDock UiDockHelp(const UiRect& panel, const UiRect& work, int helpWidth) {
    UiDock d;
    if (panel.right + helpWidth <= work.right) { d.x = panel.right; d.onRight = true; }
    else { d.x = panel.left - helpWidth; d.onRight = false; }   // no room on the right: flush to the left edge instead
    return d;
}

void UiModel::BeginSession() {
    session = CfgTake();
    message.clear();
}

void UiModel::Discard() {
    CfgRestore(session);   // silent restore of the values and the dirty flag ...
    CfgNotifyNow();        // ... then one live re-application so the engines follow
    message = "Closed without saving: settings restored";
}

bool UiModel::SaveAndClose() {
    std::string err;
    if (!CfgSave(savePath, err)) { message = "Save failed: " + err; return false; }
    message = "Saved to CombatMod.ini";
    return true;
}

void UiModel::Select(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[static_cast<size_t>(row)].Interactive() || !Visible(row)) return;
    if (row != sel_) sub_ = 0;
    sel_ = row;
}

void UiModel::Move(int dy) {
    if (!dy) return;
    int n = static_cast<int>(rows_.size());
    int i = Selected();
    for (int k = 0; k < n; ++k) {
        i = (i + (dy > 0 ? 1 : n - 1)) % n;
        if (rows_[static_cast<size_t>(i)].Interactive() && Visible(i)) { sel_ = i; sub_ = 0; return; }
    }
}

double UiModel::Snap(const CfgDef& d, double v, float maxv) {
    double s = d.step > 0 ? d.step : 1.0;
    double r = std::round(v / s) * s;
    r = std::round(r * 1e6) / 1e6;   // kill float noise (0.1 steps)
    return std::min<double>(maxv < 0 ? d.max : maxv, std::max<double>(d.min, r));
}

bool UiModel::CheckOn(int row) const {
    const UiRow& r = rows_[static_cast<size_t>(row)];
    return r.cfg >= 0 && CfgBool(r.cfg);
}

bool UiModel::MasterOn() const { return CfgBool(SlowMo_Enabled) && CfgBool(Launch_Enabled); }

double UiModel::Fraction(int row) const {
    const UiRow& r = rows_[static_cast<size_t>(row)];
    if (r.kind != UiKind::Slider) return 0.0;
    const CfgDef& d = CfgDefOf(r.cfg);
    double span = static_cast<double>(CfgMaxOf(r.cfg)) - d.min;
    return span > 0 ? std::min(1.0, std::max(0.0, (static_cast<double>(CfgGet(r.cfg)) - d.min) / span)) : 0.0;
}

bool UiModel::SetFraction(int row, double frac) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const UiRow& r = rows_[static_cast<size_t>(row)];
    if (r.kind != UiKind::Slider) return false;
    const CfgDef& d = CfgDefOf(r.cfg);
    frac = std::min(1.0, std::max(0.0, frac));
    std::string err;
    return CfgSet(r.cfg, static_cast<float>(Snap(d, d.min + frac * (static_cast<double>(CfgMaxOf(r.cfg)) - d.min), CfgMaxOf(r.cfg))), err);
}

void UiModel::SetValueBound(int row, bool toMin) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<size_t>(row)].kind != UiKind::Slider) return;
    const CfgDef& d = CfgDefOf(rows_[static_cast<size_t>(row)].cfg);
    std::string err;
    CfgSet(rows_[static_cast<size_t>(row)].cfg, toMin ? d.min : CfgMaxOf(rows_[static_cast<size_t>(row)].cfg), err);
}

void UiModel::Adjust(int steps) {
    if (!steps) return;
    UiRow& r = rows_[static_cast<size_t>(Selected())];
    std::string err;
    if (r.kind == UiKind::Slider) {
        const CfgDef& d = CfgDefOf(r.cfg);
        double step = d.step > 0 ? d.step : 1.0;
        double k = static_cast<double>(CfgGet(r.cfg)) / step;
        double base = std::fabs(k - std::round(k)) < 1e-4 ? std::round(k) : (steps > 0 ? std::floor(k) : std::ceil(k));
        CfgSet(r.cfg, static_cast<float>(Snap(d, (base + steps) * step, CfgMaxOf(r.cfg))), err);
    } else if (r.kind == UiKind::Check) {
        CfgSet(r.cfg, steps > 0 ? 1.0f : 0.0f, err);
    } else if (r.kind == UiKind::Master) {
        CfgBatch b;
        CfgSet(SlowMo_Enabled, steps > 0 ? 1.0f : 0.0f, err);
        CfgSet(Launch_Enabled, steps > 0 ? 1.0f : 0.0f, err);
    } else if (r.kind == UiKind::ButtonRow) {
        int n = static_cast<int>(r.buttons.size());
        sub_ = std::min(n - 1, std::max(0, sub_ + (steps > 0 ? 1 : -1)));
    }
}

UiResult UiModel::Run(UiAct a) {
    std::string err;
    switch (a) {
        case UiAct::PresetVanilla: message = CfgPreset("vanilla", err) ? "Preset: vanilla (everything off)" : err; break;
        case UiAct::PresetCinematic: message = CfgPreset("cinematic", err) ? "Preset: cinematic" : err; break;
        case UiAct::PresetExtreme: message = CfgPreset("extreme", err) ? "Preset: extreme" : err; break;
        case UiAct::PresetHeavy: message = CfgPreset("heavy", err) ? "Preset: heavy hits (hit-stop only)" : err; break;
        case UiAct::SaveClose: return SaveAndClose() ? UiResult::Close : UiResult::None;
        case UiAct::Discard: Discard(); return UiResult::Close;
        default: break;
    }
    return UiResult::None;
}

UiResult UiModel::ActivateButton(int row, int sub) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return UiResult::None;
    const UiRow& r = rows_[static_cast<size_t>(row)];
    if (r.kind != UiKind::ButtonRow || sub < 0 || sub >= static_cast<int>(r.buttons.size())) return UiResult::None;
    return Run(r.buttons[static_cast<size_t>(sub)].act);
}

UiResult UiModel::Activate() {
    const UiRow& r = rows_[static_cast<size_t>(Selected())];
    std::string err;
    switch (r.kind) {
        case UiKind::Master: {
            bool on = !MasterOn();
            CfgBatch b;
            CfgSet(SlowMo_Enabled, on ? 1.0f : 0.0f, err);
            CfgSet(Launch_Enabled, on ? 1.0f : 0.0f, err);
            break;
        }
        case UiKind::Check: CfgSet(r.cfg, CfgBool(r.cfg) ? 0.0f : 1.0f, err); break;
        case UiKind::ButtonRow: return ActivateButton(Selected(), sub_);
        default: break;
    }
    return UiResult::None;
}

bool UiModel::Visible(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    int mask = rows_[static_cast<size_t>(row)].modeMask;
    if (!mask) return true;
    int mode = static_cast<int>(std::lround(CfgGet(BlindingFlash_Mode)));
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    return ((mask >> mode) & 1) != 0;
}

int UiModel::FirstVisible() const {
    for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i].Interactive() && Visible(static_cast<int>(i))) return static_cast<int>(i);
    return 0;
}

std::string UiModel::FmtValue(const UiRow& r, float v) {
    if (r.names && r.nNames > 0) { int i = static_cast<int>(std::lround(v)); return r.names[i < 0 ? 0 : i >= r.nNames ? r.nNames - 1 : i]; }
    if (r.zeroText && v == 0.0f) return r.zeroText;
    char b[32];
    snprintf(b, sizeof b, r.fmt, static_cast<double>(v));
    return b;
}

std::string UiModel::ValueText(int row) const {
    const UiRow& r = rows_[static_cast<size_t>(row)];
    if (r.kind == UiKind::Master) return MasterOn() ? "ON" : "OFF";
    if (r.kind != UiKind::Slider) return "";
    return FmtValue(r, CfgGet(r.cfg));
}

// ------------------------------------------------------------------ hotkeys
bool UiParseHotkey(const std::string& text, UiHotkey& out) {
    out = UiHotkey();
    std::string t;
    for (char c : text) if (c != ' ') t += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (t.empty()) return false;
    size_t pos = 0;
    bool haveKey = false;
    while (pos <= t.size()) {
        size_t plus = t.find('+', pos);
        std::string tok = t.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
        pos = plus == std::string::npos ? t.size() + 1 : plus + 1;
        if (tok.empty()) return false;
        if (tok == "ctrl" || tok == "control") { out.ctrl = true; continue; }
        if (tok == "shift") { out.shift = true; continue; }
        if (tok == "alt") { out.alt = true; continue; }
        if (haveKey) return false;   // exactly one non-modifier key
        haveKey = true;
        static const struct { const char* n; int vk; } names[] = {
            {"enter", 0x0D}, {"return", 0x0D}, {"space", 0x20}, {"tab", 0x09}, {"esc", 0x1B}, {"escape", 0x1B}, {"backspace", 0x08},
            {"insert", 0x2D}, {"delete", 0x2E}, {"del", 0x2E}, {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22},
            {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27}, {"backquote", 0xC0}, {"tilde", 0xC0}};
        int vk = 0;
        for (auto& n : names) if (tok == n.n) vk = n.vk;
        if (!vk && tok.size() == 1 && isalpha(static_cast<unsigned char>(tok[0]))) vk = toupper(tok[0]);
        if (!vk && tok.size() == 1 && isdigit(static_cast<unsigned char>(tok[0]))) vk = tok[0];
        if (!vk && tok.size() >= 2 && tok.size() <= 3 && tok[0] == 'f' && isdigit(static_cast<unsigned char>(tok[1]))) {
            int n = atoi(tok.c_str() + 1);
            if (n >= 1 && n <= 24) vk = 0x70 + n - 1;
        }
        if (!vk) return false;
        out.vk = vk;
    }
    return haveKey;
}

std::string UiHotkeyStr(const UiHotkey& k) {
    std::string s;
    if (k.ctrl) s += "Ctrl+";
    if (k.shift) s += "Shift+";
    if (k.alt) s += "Alt+";
    char b[16];
    snprintf(b, sizeof b, "vk0x%02X", k.vk);
    return s + b;
}

bool EdgeToggle::Update(bool down, long long nowMs) {
    bool fire = down && !prev_ && nowMs - last_ >= debounce_;
    if (down && !prev_) { /* a rising edge inside the debounce window is swallowed but still counts as seen */ }
    if (fire) last_ = nowMs;
    prev_ = down;
    return fire;
}

// ------------------------------------------------------------------ controller navigation
void PadNav::Reset() {
    up_ = down_ = horiz_ = Dir();
    horizDir_ = 0;
    prevA_ = prevBack_ = prevStart_ = false;
    latched_ = false;
}

PadEvents PadNav::Update(uint16_t b, int16_t lx, long long now) {
    PadEvents ev;
    bool aNow = (b & pad::A) != 0, backNow = (b & pad::B) != 0, startNow = (b & pad::Start) != 0;
    if (b & pad::LB) latched_ = true;   // LB is the "combo" modifier (LB + DPad-Down toggles the panel)
    if (latched_) {
        // no navigation while it is held, nor after it is released until the buttons that were part of the combo are let go too
        // (otherwise the DPad-Down of the combo would also move the selection of the freshly opened panel)
        bool anyNav = (b & (pad::Up | pad::Down | pad::Left | pad::Right | pad::A | pad::B | pad::Start | pad::LB)) || lx < -9000 || lx > 9000;
        up_ = down_ = horiz_ = Dir();
        horizDir_ = 0;
        prevA_ = aNow; prevBack_ = backNow; prevStart_ = startNow;
        if (!anyNav) latched_ = false;
        return ev;
    }
    if (aNow && !prevA_) ev.activate = true;
    if (backNow && !prevBack_) ev.back = true;
    if (startNow && !prevStart_) ev.start = true;
    prevA_ = aNow; prevBack_ = backNow; prevStart_ = startNow;

    const long long kFirst = 350, kVert = 140, kHoriz = 90;
    auto rep = [&](Dir& d, bool pressed, long long interval) -> bool {
        if (!pressed) { d = Dir(); return false; }
        if (d.since < 0) { d.since = now; d.lastRepeat = now; return true; }   // first press
        if (now - d.since >= kFirst && now - d.lastRepeat >= interval) { d.lastRepeat = now; return true; }
        return false;
    };
    bool up = (b & pad::Up) != 0, down = (b & pad::Down) != 0;
    if (up && !down && rep(up_, true, kVert)) ev.moveY = -1;
    else if (!up) up_ = Dir();
    if (down && !up && rep(down_, true, kVert)) ev.moveY = 1;
    else if (!down) down_ = Dir();

    int dir = 0;
    if ((b & pad::Left) || lx < -9000) dir = -1;
    else if ((b & pad::Right) || lx > 9000) dir = 1;
    if (dir != horizDir_) { horiz_ = Dir(); horizDir_ = dir; }
    if (dir && rep(horiz_, true, kHoriz)) {
        long long held = now - horiz_.since;
        int mult = held < 800 ? 1 : held < 1600 ? 3 : held < 2800 ? 8 : 20;   // auto-repeat accelerates the longer it is held
        ev.adjust = dir * mult;
    } else if (!dir) {
        horiz_ = Dir();
    }
    return ev;
}
