#include "common.h"
#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <share.h>

static std::mutex g_mu;
static FILE* g_file = nullptr;
static std::deque<std::string> g_ring;
static const size_t kRing = 500;

void LogOpen(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);  // others may read while we run
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);  // truncates and NUL-terminates
    va_end(ap);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%5lu] %s", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    g_ring.emplace_back(line);
    if (g_ring.size() > kRing) g_ring.pop_front();
}

std::vector<std::string> LogTail(size_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t c = n < g_ring.size() ? n : g_ring.size();
    return std::vector<std::string>(g_ring.end() - static_cast<std::ptrdiff_t>(c), g_ring.end());
}
