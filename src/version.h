// Plugin version and the game-build identity, for the log's first lines, the `version` bridge command and `ui status` (bug reports must say which release and
// which game build they come from). Also the one-shot signature rescan after a cold start.
#pragma once
#include "util.h"

#define COMBATMOD_VERSION "0.9.0-beta"

std::string VersionLine();                                // "CombatMod 0.9.0-beta | layout target build 26308950 | game exe: size_of_image=0x.., pe_time=.., file_size=.."
std::string VersionInfo();                                // the same plus signature state: "sigs: 4/4 required resolved"
bool RequiredSigsResolved(std::string* missing = nullptr);   // all gl::kRequiredSigs resolved exactly once?
void SigRescanLater();                                    // if a required signature is missing at start: ONE automatic rescan ~10 s later (never hooks on a partial match)
std::string CmdVersion(const Args& t);                    // version
