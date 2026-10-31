#pragma once
#include "util.h"

// Guarded memory writes with an undo journal and optional timed auto-revert.
bool MemWrite(uintptr_t addr, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>* expect, bool patch,
              uint64_t revertMs, std::string& msg, int* idOut = nullptr);
std::string UndoLast(int n, bool& ok);   // restores the last n active entries, newest first
void RevertPending();                    // restore every entry that still has an auto-revert timer
int JournalRemoveId(int id);             // drop one entry (used by selftest); returns 1 if removed
bool JournalActive(int id);
std::string WritesSummary();             // active writes with a pending revert (for `hooks`)

std::string CmdWrite(Args t, char kind); // kind: 'b' write, 'p' patch, 'f' float, 'q' qword, 'd' dword
std::string CmdJournal();
std::string CmdUndo(const Args& t);
std::string CmdJournalClear(const Args& t);
