#pragma once
#include <string>

// Read-only observer of the game's slow-motion "time curve" structs.
// Chain: G = &qword (sig "time_mgr_global"); M = *G; P = *M; obj1 = *(P+gl::kCtlObj[0]); obj2 = *(P+gl::kCtlObj[1]) (src/gamelayout.h).
// Each obj: floats f0..f4 at +0x00..+0x10 (idle: -1,-1,0,0,0).
// true: `out` holds report lines (each ending in '\n'); false: `out` holds an error message.
bool TimeCurveReport(std::string& out);

// CSV sampler (every ~8 ms, writes only on change to CombatMod_timecurve.csv). The thread is started once and
// can be switched on/off at runtime. The first enable in a session truncates the csv, later ones append.
void StartTimeCurveSampler();          // = enable
void SetTimeCurveSampling(bool on);
bool TimeCurveSampling();
