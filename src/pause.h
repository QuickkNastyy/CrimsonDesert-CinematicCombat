// Pauses the game world while the control panel is open by holding the game's OWN time controller.
// Chain (sig time_mgr_global = address of a qword): M = *G, P = *M, obj1 = *(P+gl::kCtlObj[0]), obj2 = *(P+gl::kCtlObj[1]) (src/gamelayout.h); each obj holds f0..f4 at +0x00..+0x10
// = (scale, 1.0, timingA, timingB, progress); idle is (-1, -1, 0, 0, 0). Pause = (0.001, 1.0, 3600, 1.0, 0) written into both objects and
// re-asserted every ~8 ms by a keeper thread (the engine advances the progress f4 in real time); resume = the idle values (instant).
// All writes are SEH-guarded, the chain is validated on every tick, nothing is done when it is unreadable, and the idle values are restored on
// close / unload / quit-bridge / UI-thread death, plus by a watchdog that clears our pause signature when nobody asked for it for > 1 s.
#pragma once
#include "util.h"

void PauseInit();                  // starts the keeper thread (idempotent)
void PauseShutdown(bool wait);     // restore idle, stop the keeper (wait=false inside DllMain)
void PauseSetOpen(bool open);      // the panel is open / closed (paused only if UI.PauseGame is on)
void PauseRestoreNow();            // synchronous: if we are holding the game, write the idle values right now
bool PauseActive();                // are we currently holding the game?
std::string CmdPause(const Args& t);   // ui pause on|off|status
std::string PauseGuardStatus();                // "ok ..." or "REFUSING WRITES: <why>": the controller objects failed the layout classifier
std::string PauseCtlReport(bool& ok);        // `layout check`: the chain G -> M -> P -> objects, what the objects hold, and which pointers in P look like controller objects
bool PauseCtlRead(float out[2][5]);        // both controller objects (f0..f4 each); false if the chain is unreadable
bool PauseCtlWrite(const float v[5]);         // one curve (scale, 1.0, A, B, progress) into both objects (SEH-guarded)

// Pure helpers (selftest)
struct PauseVals { float f[5]; };
bool PauseIsOurs(const PauseVals& v);          // does an object hold our pause signature?
PauseVals PauseSignature();                    // (0.001, 1, 3600, 1, 0)
PauseVals PauseIdle();                         // (-1, -1, 0, 0, 0)
// One keeper decision for one object (no memory access): what to write, if anything.
enum class PauseAction { None, WriteAll, WriteProgress };
PauseAction PauseDecide(const PauseVals& cur);
