#include "version.h"
#include "features.h"
#include "gamelayout.h"
#include "log.h"
#include "sigscan.h"
#include "telemetry.h"
#include "triggers.h"
#include <ctime>

static void ReadPe(unsigned long long& size, unsigned long long& stamp) {   // POD only (legal __try)
    HMODULE exe = GetModuleHandleW(nullptr);
    __try {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) { size = nt->OptionalHeader.SizeOfImage; stamp = nt->FileHeader.TimeDateStamp; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

std::string VersionLine() {
    unsigned long long size = 0, stamp = 0;
    ReadPe(size, stamp);
    std::string pe = "unknown";
    if (stamp) {
        time_t tt = static_cast<time_t>(stamp);
        tm t = {};
        if (!gmtime_s(&t, &tt)) pe = F("0x%llX = %04d-%02d-%02d %02d:%02d:%02d UTC", stamp, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    }
    unsigned long long fileSize = 0;
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (n && GetFileAttributesExW(std::wstring(p, n).c_str(), GetFileExInfoStandard, &fa)) fileSize = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    return F("CombatMod %s | layout target: game build %u | game exe: size_of_image=0x%llX file_size=%llu pe_time=%s", COMBATMOD_VERSION, gl::kBuild, size, fileSize, pe.c_str());
}

bool RequiredSigsResolved(std::string* missing) {
    auto sigs = SigsSnapshot();
    bool all = true;
    for (const char* name : gl::kRequiredSigs) {
        bool found = false;
        for (const Sig& s : sigs) if (s.name == name && s.st == SigStatus::Resolved && s.matches == 1) found = true;
        if (!found) { all = false; if (missing) *missing += std::string(missing->empty() ? "" : ", ") + name; }
    }
    return all;
}

std::string VersionInfo() {
    std::string miss;
    bool ok = RequiredSigsResolved(&miss);
    auto sigs = SigsSnapshot();
    size_t res = 0;
    for (const Sig& s : sigs) res += s.st == SigStatus::Resolved;
    return VersionLine() + F("\nsignatures: %zu/%zu resolved, required %s%s\n", res, sigs.size(), ok ? "all resolved" : "MISSING: ", ok ? "" : miss.c_str());
}

std::string CmdVersion(const Args& t) {
    if (t.size() > 1) return Err("usage: version");
    return Ok(VersionInfo());
}

static DWORD WINAPI RescanThread(LPVOID) {
    RegisterInternalThread();
    for (int i = 0; i < 100 && !g_stop; ++i) Sleep(100);   // ~10 s
    if (g_stop) return 0;
    std::string miss;
    if (RequiredSigsResolved(&miss)) return 0;             // something (a `rescan`) already fixed it
    Log("sigs: required signatures missing %d s after the start (%s): ONE automatic rescan now", 10, miss.c_str());
    int n = ResolveSigs(SigsPath());
    miss.clear();
    if (RequiredSigsResolved(&miss)) {
        Log("sigs: rescan resolved everything required (%d signatures): requesting the hooks of the enabled features now", n);
        FeaturesApply();
        TriggersSyncFromSettings();
        TelemetryRequestHooks();
    } else {
        Log("sigs: still missing after the rescan (%s): the mod stays inert (nothing is hooked without a unique match for every signature it needs)", miss.c_str());
    }
    return 0;
}

void SigRescanLater() {
    if (RequiredSigsResolved()) return;
    HANDLE h = CreateThread(nullptr, 0, RescanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
