#include "common.h"
#include "bridge.h"
#include "call.h"
#include "etrace.h"
#include "hookretry.h"
#include "pause.h"
#include "telemetry.h"
#include "features.h"
#include "launchboost.h"
#include "layoutcheck.h"
#include "settings.h"
#include "input.h"
#include "log.h"
#include "memwrite.h"
#include "selftest.h"
#include "sigscan.h"
#include "skillctx.h"
#include "timeamp.h"
#include "triggers.h"
#include "ui.h"
#include "timecurve.h"
#include "trace.h"
#include "util.h"
#include "version.h"
#include "watch.h"
#include <cctype>
#include <exception>
#include <functional>
#include <sstream>
#include <thread>

static std::atomic<bool> g_bstop{false};
static SOCKET g_listen = INVALID_SOCKET;

// ---------------------------------------------------------------- basic commands
static std::string HexDump(uintptr_t a, const std::vector<uint8_t>& d) {
    std::string o;
    for (size_t i = 0; i < d.size(); i += 16) {
        o += F("%016llX  ", U64(a + i));
        std::string asc;
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < d.size()) { o += F("%02X ", d[i + j]); asc += (d[i + j] >= 32 && d[i + j] < 127) ? static_cast<char>(d[i + j]) : '.'; }
            else o += "   ";
        }
        o += " |" + asc + "|\n";
    }
    return o;
}

static std::string CmdStatus() {
    ModInfo mi = MainModule();
    auto sigs = SigsSnapshot();
    size_t ok = 0;
    for (auto& s : sigs) ok += s.st == SigStatus::Resolved;
    wchar_t p[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring ws(p, len);
    std::string name;
    for (wchar_t ch : ws.substr(ws.find_last_of(L"\\/") + 1)) name += ch < 128 ? static_cast<char>(ch) : '?';
    return Ok(F("module=%s base=0x%llX size=0x%llX\nsigs resolved=%zu total=%zu\nhooks=%s port=%d\nallow write=%d trace=%d watch=%d input=%d call=%d\n"
                "timecurve_sampler=%s\n",
                name.c_str(), U64(mi.base), U64(mi.size), ok, sigs.size(), g_cfg.enableHooks ? "enabled" : "disabled(read-only)", g_cfg.bridgePort,
                g_cfg.allowWrite, g_cfg.allowTrace, g_cfg.allowWatch, g_cfg.allowInput, g_cfg.allowCall, TimeCurveSampling() ? "on" : "off"));
}

static std::string CmdSigs() {
    std::string o;
    for (auto& s : SigsSnapshot()) {
        o += F("%s %s", s.name.c_str(), SigStatusName(s.st));
        if (s.st == SigStatus::Resolved) o += F(" 0x%llX", U64(s.addr));
        else o += F(" matches=%zu", s.matches);
        o += "\n";
    }
    return Ok(o);
}

static std::string CmdResolve(const Args& t) {
    Sig s;
    if (t.size() != 2) return Err("usage: resolve <name>");
    if (!FindSig(t[1], s)) return Err("unknown sig");
    if (s.st != SigStatus::Resolved) return Err(F("%s %s matches=%zu", s.name.c_str(), SigStatusName(s.st), s.matches));
    return Ok(F("%s RESOLVED 0x%llX (match 0x%llX)\n", s.name.c_str(), U64(s.addr), U64(s.match)));
}

static std::string CmdRead(const Args& t) {
    uintptr_t a;
    uint64_t n;
    std::string e;
    if (t.size() != 3 || !ParseNum(t[2], n)) return Err("usage: read <addr> <n>");
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (n == 0 || n > 4096) return Err("n must be 1..4096");
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (!SafeRead(a, buf.data(), buf.size())) return Err(F("unreadable memory at 0x%llX (+%llu)", U64(a), U64(n)));
    return Ok(HexDump(a, buf));
}

static std::string CmdReadF(const Args& t) {
    uintptr_t a;
    float f = 0;
    std::string e;
    if (t.size() != 2) return Err("usage: readf <addr>");
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (!Rd(a, f)) return Err(F("unreadable memory at 0x%llX", U64(a)));
    return Ok(F("0x%llX = %.9g\n", U64(a), static_cast<double>(f)));
}

static std::string CmdReadQ(const Args& t) {
    uintptr_t a;
    uint64_t v = 0;
    std::string e;
    if (t.size() != 2) return Err("usage: readq <addr>");
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (!Rd(a, v)) return Err(F("unreadable memory at 0x%llX", U64(a)));
    return Ok(F("0x%llX = 0x%llX (%lld)\n", U64(a), U64(v), static_cast<long long>(v)));
}

static std::string CmdChain(const Args& t) {  // v=*addr; then v=*(v+off) for each off (hex, may be negative)
    uintptr_t a;
    uint64_t v = 0;
    std::string e;
    if (t.size() < 2) return Err("usage: chain <addr> <off1> <off2> ...");
    if (!ResolveAddr(t[1], a, e)) return Err(e);
    if (!Rd(a, v)) return Err(F("unreadable memory at 0x%llX", U64(a)));
    std::string o = F("[0] *0x%llX = 0x%llX\n", U64(a), U64(v));
    for (size_t i = 2; i < t.size(); ++i) {
        int64_t off = 0;
        if (!ParseSHex(t[i], off)) return o + Err("bad offset '" + t[i] + "'");
        uint64_t addr = v + static_cast<uint64_t>(off);
        uint64_t nv = 0;
        if (!Rd(static_cast<uintptr_t>(addr), nv)) return o + Err(F("hop %zu: unreadable at 0x%llX", i - 1, U64(addr)));
        o += F("[%zu] *(0x%llX %c 0x%llX) = *0x%llX = 0x%llX\n", i - 1, U64(v), off < 0 ? '-' : '+', U64(off < 0 ? -off : off), U64(addr), U64(nv));
        v = nv;
    }
    return Ok(o);
}

static std::string CmdLog(const Args& t) {
    uint64_t n;
    if (t.size() != 2 || !ParseNum(t[1], n)) return Err("usage: log <n>");
    std::string o;
    for (auto& l : LogTail(static_cast<size_t>(n > 500 ? 500 : n))) o += l + "\n";
    return Ok(o);
}

static std::string CmdTimeCurve() {
    std::string o;
    if (TimeCurveReport(o)) return Ok(o);
    return Err(o);
}

static std::string CmdTimeCurveSample(const Args& t) {
    std::string m = t.size() == 2 ? Lower(t[1]) : "";
    if (m != "on" && m != "off") return Err("usage: timecurve_sample on|off");
    SetTimeCurveSampling(m == "on");
    return Ok(F("timecurve sampler %s -> CombatMod_timecurve.csv\n", m.c_str()));
}

static std::string CmdHooks() {
    std::string o = TracesSummary() + WatchesSummary() + WritesSummary() + InputSummary() + AmpSummary() + SkillsSummary() + EtSummaryLine() + HookRetrySummary() + TriggersSummary() + (PauseActive() ? std::string("pause: the game is currently HELD by the panel\n") : std::string());
    return Ok(o.empty() ? "(nothing active)\n" : o);
}

void ShutdownAll(bool full, bool inLoaderLock) {
    if (full) {
        TraceRemoveAll();
        WatchRemoveAll();
        AmpShutdown();
        SkillsShutdown();
        EtShutdown();
        UiShutdown(!inLoaderLock);   // the game gets its pad back first, then the panel thread stops
        PauseShutdown(!inLoaderLock);   // ... and the game is guaranteed to run again
        TriggersShutdown(!inLoaderLock);
        TelemetryShutdown(!inLoaderLock);   // final session file
        HookRetryShutdown(!inLoaderLock);
        FeaturesShutdown();
    }
    RevertPending();
    InputReleaseAll();
    Log("shutdown: traces/watches removed, pending reverts restored, keys released (full=%d)", full ? 1 : 0);
}

// ---------------------------------------------------------------- command table
enum Gate { G_NONE, G_WRITE, G_TRACE, G_WATCH, G_INPUT, G_CALL };
struct Cmd {
    const char* name;
    const char* usage;
    Gate gate;
    std::function<std::string(Args&)> fn;
};

static bool GateOpen(Gate g) {
    switch (g) {
        case G_WRITE: return g_cfg.allowWrite != 0;
        case G_TRACE: return g_cfg.allowTrace != 0;
        case G_WATCH: return g_cfg.allowWatch != 0;
        case G_INPUT: return g_cfg.allowInput != 0;
        case G_CALL: return g_cfg.allowCall != 0;
        default: return true;
    }
}

static const std::vector<Cmd>& Table() {
    static const std::vector<Cmd> t = {
        {"help", "", G_NONE, [](Args&) { return std::string(); }},  // handled specially (needs the table)
        {"ping", "", G_NONE, [](Args&) { return std::string("pong\nOK\n"); }},
        {"status", "", G_NONE, [](Args&) { return CmdStatus(); }},
        {"sigs", "", G_NONE, [](Args&) { return CmdSigs(); }},
        {"resolve", "<name>", G_NONE, [](Args& a) { return CmdResolve(a); }},
        {"rescan", "                 reload CombatMod.sigs and rescan", G_NONE, [](Args&) { return Ok(F("resolved=%d\n", ResolveSigs(SigsPath()))); }},
        {"read", "<addr> <n>          hex dump, n 1..4096 (addr = hex | signame | rva:HEX)", G_NONE, [](Args& a) { return CmdRead(a); }},
        {"readf", "<addr>", G_NONE, [](Args& a) { return CmdReadF(a); }},
        {"readq", "<addr>", G_NONE, [](Args& a) { return CmdReadQ(a); }},
        {"chain", "<addr> <off1> ...  follow pointers (offsets hex, may be negative)", G_NONE, [](Args& a) { return CmdChain(a); }},
        {"log", "<n>                 last n plugin log lines (max 500)", G_NONE, [](Args& a) { return CmdLog(a); }},
        {"timecurve", "", G_NONE, [](Args&) { return CmdTimeCurve(); }},
        {"timecurve_sample", "on|off     CSV sampler (CombatMod_timecurve.csv)", G_NONE, [](Args& a) { return CmdTimeCurveSample(a); }},
        {"write", "<addr> <hexbytes> [expect <hexbytes>] [revert_ms=N]   refuses executable memory", G_WRITE, [](Args& a) { return CmdWrite(a, 'b'); }},
        {"patch", "<addr> <hexbytes> [expect <hexbytes>] [revert_ms=N]   like write, allowed in code", G_WRITE, [](Args& a) { return CmdWrite(a, 'p'); }},
        {"writef", "<addr> <float> [expect <float>] [revert_ms=N]", G_WRITE, [](Args& a) { return CmdWrite(a, 'f'); }},
        {"writeq", "<addr> <int> [expect <int>] [revert_ms=N]   qword; decimal or 0x hex", G_WRITE, [](Args& a) { return CmdWrite(a, 'q'); }},
        {"writed", "<addr> <int> [expect <int>] [revert_ms=N]   dword; decimal or 0x hex", G_WRITE, [](Args& a) { return CmdWrite(a, 'd'); }},
        {"journal", "                list write/patch history", G_WRITE, [](Args&) { return CmdJournal(); }},
        {"undo", "[n]                restore the last n active writes (default 1)", G_WRITE, [](Args& a) { return CmdUndo(a); }},
        {"journal_clear", "[force]   drop reverted entries (force: drop all, cancel timers)", G_WRITE, [](Args& a) { return CmdJournalClear(a); }},
        {"trace", "<addr|signame> [regs]   log every call (tid, return RVA, rcx/rdx/r8/r9, xmm0-3 with regs)", G_TRACE, [](Args& a) { return CmdTrace(a); }},
        {"traces", "", G_TRACE, [](Args&) { return CmdTraces(); }},
        {"tracelog", "<addr|signame> [n]   last n logged calls (max 2000)", G_TRACE, [](Args& a) { return CmdTraceLog(a); }},
        {"untrace", "<addr|signame>", G_TRACE, [](Args& a) { return CmdUntrace(a); }},
        {"untrace_all", "", G_TRACE, [](Args&) { return CmdUntraceAll(); }},
        {"watch", "w|rw <addr> <size 1|2|4|8>   hardware data watchpoint on all threads (max 4)", G_WATCH, [](Args& a) { return CmdWatch(a); }},
        {"watches", "", G_WATCH, [](Args&) { return CmdWatches(); }},
        {"watchlog", "<addr> [n]         last n hits (max 500)", G_WATCH, [](Args& a) { return CmdWatchLog(a); }},
        {"unwatch", "<addr>", G_WATCH, [](Args& a) { return CmdUnwatch(a); }},
        {"key", "<vk> tap|down|up [hold_ms]   vk: W, 5, F5, LSHIFT, SPACE, MOUSE1.. or 0xNN", G_INPUT, [](Args& a) { return CmdKey(a); }},
        {"keys", "<script>           e.g. W:down;wait:300;J:tap;wait:50;W:up (max 60 s)", G_INPUT, [](Args& a) { return CmdKeys(a); }},
        {"keys_release", "", G_INPUT, [](Args&) { return CmdKeysRelease(); }},
        {"call", "<addr|signame> [a1..a4]   DANGEROUS: __fastcall call on a worker thread, 2 s timeout", G_CALL, [](Args& a) { return CmdCall(a); }},
        {"amp", "status|on|off|set K=V..|learn_owner|log [n]|reset_map   slow-mo amplifier; set: ScaleMul DurMul MinScale ScaleOverrides ContextWindowMs ContextAnyThread RequireCtx AllowKeys DenyKeys OnlyOwner", G_NONE, [](Args& a) { return CmdAmp(a); }},
        {"skills", "on|off|status|log [n]|summary [seconds]|clear   capture apply_attack_skill_to_position calls (owner a1, SkillKeys)", G_NONE, [](Args& a) { return CmdSkills(a); }},
        {"skills patch", "add <*|k0=a,b|any=a,b> <a2|S|o160|a1> <offset_hex> <f32|u32> <mul|set|add> <value> [owner=<hex a1>|player|any]   patch a field before the original runs (S/o160/a1 restored after; a2 stays)", G_WRITE, [](Args&) { return std::string(); }},
        {"skills patch", "list | del <id> | clear | on | off | log [n]", G_NONE, [](Args&) { return std::string(); }},
        {"skills raw", "on|off|both [bytes_a2 bytes_S bytes_o160 bytes_a1]   raw memory dumps around each call (defaults 1024 768 512 512, max 4096); `both` also dumps AFTER the call", G_TRACE, [](Args&) { return std::string(); }},
        {"skills rawlist", "[n]       list captures: index, t, a1, key0, sizes", G_NONE, [](Args&) { return std::string(); }},
        {"skills rawget", "<index|last> a2|S|o160|a1|a2_after|S_after|o160_after|a1_after [offset len]   hex dump, 32 bytes per line", G_NONE, [](Args&) { return std::string(); }},
        {"skills rawsave", "<path>    write all captures to a binary file (CMRAW001); path must be inside the CombatMod project folder", G_NONE, [](Args&) { return std::string(); }},
        {"mem", "scanf <addr> <len> <minabs> <maxabs>   find 4-byte-aligned float triples (vectors) with magnitude in [min,max]; first 200 hits", G_NONE, [](Args& a) { return CmdMemScanF(a); }},
        {"etrace", "on|off|status|log [n]|summary [seconds]|window <t_us> <before_ms> <after_ms>|clear|filter exclude=<types>   log every call of the central event dispatcher", G_NONE, [](Args& a) { return CmdEtrace(a); }},
        {"ui", "show|hide|toggle|status|help|pause on|off|status|snapshot <path> [row=N] [scale=X]   the in-game control panel (Ctrl+Enter or LB+DPad-Down) and its docked help window; snapshot writes a BMP of both", G_NONE, [](Args& a) { return CmdUi(a); }},
        {"trigger", "list|status|add <line>|del <n>|clear|enable [n]|disable [n]|fire <scale> <A> <B> [force]|log [n]|save   custom slow-motion triggers: start our own curve through the time controller when the player uses a skill (add/del/clear/fire/save need AllowWrite)", G_NONE, [](Args& a) { return CmdTrigger(a); }},
        {"layout", "check|show   does the live game still match src/gamelayout.h (build layout numbers)? event block type / floats, time-controller objects, skill-record keys and marker block; names the constants that are wrong", G_NONE, [](Args& a) { return CmdLayout(a); }},
        {"version", "                  plugin version, the game build it targets, the game exe (size / PE time) and the signature state", G_NONE, [](Args& a) { return CmdVersion(a); }},
        {"bf", "status|log [n]|clear   Blinding Flash (key 10276) chains: BlindingFlash.Mode 1 / 2; `bf log` shows the last chains with their ordered ctx key tuples per weapon (extend BlindingFlash.FinaleKeys live with `cfg set`)", G_NONE, [](Args& a) { return CmdBf(a); }},
        {"hooktest", "failnext <n>   developer: make the next n hook installs fail transiently to watch the 8x400 ms retry (see `hooks`)", G_TRACE, [](Args&) { return std::string(); }},
        {"hitstop", "status   hit-stop (impact pause) events seen / modified, per-minute telemetry with the context key", G_NONE, [](Args& a) { return CmdHitStop(a); }},
        {"telemetry", "status|dump|on|off   patch-diff database: one JSON per session in CombatMod_telemetry (event catalog, skill keys, launch records, slow-mo layouts, signatures); compare builds with tools/telemetry_diff.py", G_NONE, [](Args& a) { return CmdTelemetry(a); }},
        {"cfg", "list|get <key>|set <key> <value>|save|load|preset <vanilla|cinematic|extreme|heavy>   user settings (SlowMo.* / Launch.*), live; `save` writes the [Cinematic] section of CombatMod.ini", G_NONE, [](Args& a) { return CmdCfg(a); }},
        {"player", "status|reset   the learned player identity (a1 with the most apply-skill calls in the last 60 s)", G_NONE, [](Args& a) { return CmdPlayer(a); }},
        {"launch", "status|on|off|log [n]|restore|clear   launch (knockback) boost: records found by their FLT_MAX marker block (offsets: src/gamelayout.h), Launch.* settings; `launch scan` finds all records in memory", G_NONE, [](Args& a) { return CmdLaunch(a); }},
        {"hooks", "                  everything currently active (traces, watches, timed writes, held keys)", G_NONE, [](Args&) { return CmdHooks(); }},
        {"selftest", "               verify the plugin itself (no game state touched)", G_NONE, [](Args&) { return CmdSelfTest(); }},
        {"quit-bridge", "            remove all hooks, restore reverts, stop the bridge", G_NONE, [](Args&) { return std::string(); }},
    };
    return t;
}

static std::string Handle(const std::string& line, bool& quit) {
    std::istringstream is(line);
    Args t;
    std::string w;
    while (is >> w) t.push_back(w);
    if (t.empty()) return Err("empty command");
    t[0] = Lower(t[0]);
    const std::string c = t[0];
    if (c == "hooktest") {
        uint64_t n;
        if (!g_cfg.allowTrace) return Err("disabled");
        if (t.size() != 3 || Lower(t[1]) != "failnext" || !ParseNum(t[2], n) || n > 1000) return Err("usage: hooktest failnext <n>");
        HookTestFailNext(static_cast<int>(n));
        return Ok(F("the next %llu hook installs will fail transiently\n", U64(n)));
    }
    if (c == "help") {
        std::string o;
        for (const Cmd& k : Table()) {
            o += k.name;
            if (k.usage[0]) o += std::string(" ") + k.usage;
            if (k.gate != G_NONE) o += GateOpen(k.gate) ? "" : "   [disabled]";
            o += "\n";
        }
        return Ok(o);
    }
    if (c == "quit-bridge") {
        quit = true;
        ShutdownAll(true);
        return "bye\nOK\n";
    }
    for (const Cmd& k : Table())
        if (c == k.name) {
            if (!GateOpen(k.gate)) return Err("disabled");
            return k.fn(t);
        }
    return Err("unknown command '" + c + "' (try help)");
}

// ---------------------------------------------------------------- server
static void SendAll(SOCKET s, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) {
        int r = send(s, d.data() + off, static_cast<int>(d.size() - off), 0);
        if (r <= 0) return;
        off += static_cast<size_t>(r);
    }
}

static void Client(SOCKET s) {
    RegisterInternalThread();
    std::string buf;
    char tmp[1024];
    bool quit = false;
    while (!quit && !g_bstop && !g_stop) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        size_t p;
        while (!quit && (p = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string reply;
            try { reply = Handle(line, quit); }
            catch (const std::exception& e) { reply = std::string("ERR exception: ") + e.what() + "\n"; }
            catch (...) { reply = "ERR unknown exception\n"; }
            SendAll(s, reply);
        }
        if (buf.size() > 8192) break;  // runaway line
    }
    closesocket(s);
    if (quit) {
        g_bstop = true;
        Log("bridge: quit-bridge received");
    }
    UnregisterInternalThread();
}

static void Server(int port) {
    RegisterInternalThread();
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { Log("bridge: WSAStartup failed"); return; }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { Log("bridge: socket() failed %d", WSAGetLastError()); return; }
    BOOL excl = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<u_short>(port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only
    if (bind(g_listen, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(g_listen, 4) != 0) {
        Log("bridge: bind/listen on 127.0.0.1:%d failed (%d); bridge disabled", port, WSAGetLastError());
        closesocket(g_listen);
        return;
    }
    Log("bridge: listening on 127.0.0.1:%d", port);
    while (!g_bstop && !g_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_listen, &fds);
        timeval tv = {0, 300000};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        try { std::thread(Client, c).detach(); } catch (...) { closesocket(c); }
    }
    closesocket(g_listen);
    Log("bridge: stopped");
}

void StartBridge(int port) {
    try { std::thread(Server, port).detach(); }
    catch (...) { Log("bridge: could not start thread"); }
}
