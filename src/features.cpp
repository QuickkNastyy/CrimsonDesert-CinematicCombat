#include "features.h"
#include "launchboost.h"
#include "log.h"
#include "settings.h"
#include "skillctx.h"
#include "timeamp.h"
#include "triggers.h"

void FeaturesApply() {
    try {
        AmpSyncFromSettings();
        LaunchSyncFromSettings();
        TriggersSyncFromSettings();
    } catch (...) {
        Log("features: apply failed with an exception (ignored)");
    }
}

void FeaturesInit() {
    Launch();   // build the launch table now: the hook must never allocate it lazily
    std::string e;
    if (CfgLoad(g_dir + L"CombatMod.ini", e)) Log("settings: %s", e.c_str());
    else Log("settings: %s (defaults in use)", e.c_str());
    AmpInit();      // amp-only dev keys + legacy [TimeAmp] mapping onto the settings
    SkillsInit();   // [Skills] Enabled=1
    CfgSetChangeHandler(&FeaturesApply);
    FeaturesApply();
    Log("features: SlowMo.Enabled=%d Launch.Enabled=%d (hooks are installed only for enabled features)", CfgBool(SlowMo_Enabled) ? 1 : 0, CfgBool(Launch_Enabled) ? 1 : 0);
}

void FeaturesShutdown() {
    Launch().RestoreAll();
}
