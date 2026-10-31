#pragma once
#include <string>
#include <vector>

void LogOpen(const std::wstring& path);        // truncates; fresh log per game session
void Log(const char* fmt, ...);                // printf-style, thread-safe, flushed per line
std::vector<std::string> LogTail(size_t n);    // last n lines (ring buffer holds 500)
