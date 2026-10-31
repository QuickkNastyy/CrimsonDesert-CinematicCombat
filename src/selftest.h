#pragma once
#include "util.h"

// Plugin self-check that never touches game state: sigs, PE header read, write/undo on a private buffer,
// trace on a function inside this DLL, hardware watch on a variable inside this DLL (thread-restricted).
std::string CmdSelfTest();
