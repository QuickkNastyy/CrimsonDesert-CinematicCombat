#pragma once
#include "util.h"

// DANGEROUS: calls a game function with up to 4 integer args on a worker thread (not the game's own threads).
std::string CmdCall(const Args& t);
