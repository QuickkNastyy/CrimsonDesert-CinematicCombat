#pragma once
#include "util.h"

// Keyboard/mouse injection via SendInput, only while a window of this process is in the foreground.
void InputReleaseAll();           // release every key/button this module left pressed
std::string InputSummary();       // keys currently held (for `hooks`)

std::string CmdKey(const Args& t);
std::string CmdKeys(const Args& t);
std::string CmdKeysRelease();
