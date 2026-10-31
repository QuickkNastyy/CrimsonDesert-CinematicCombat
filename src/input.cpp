#include "input.h"
#include "log.h"
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>

namespace {
struct Key {
    WORD vk = 0;
    int mouse = 0;  // 1=left 2=right 3=middle 4=X1 5=X2
    std::string name;
    int id() const { return mouse ? 1000 + mouse : vk; }
};

struct Step {
    enum Kind { Wait, Down, Up, Tap } kind = Wait;
    Key key;
    uint32_t ms = 0;
};

const uint32_t kMaxScriptMs = 60000, kMaxSteps = 600, kDefaultHold = 30, kMaxHold = 5000;

std::mutex g_hm;
std::map<int, Key> g_held;          // keys we pressed and have not released
std::atomic<bool> g_busy{false};    // one input command at a time
long long g_lastUs = 0;

bool ParseKey(const std::string& raw, Key& k) {
    std::string s = Lower(raw);
    k = Key();
    k.name = s;
    static const std::map<std::string, WORD> names = {
        {"shift", VK_SHIFT}, {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT}, {"ctrl", VK_CONTROL}, {"control", VK_CONTROL},
        {"lctrl", VK_LCONTROL}, {"rctrl", VK_RCONTROL}, {"alt", VK_MENU}, {"lalt", VK_LMENU}, {"ralt", VK_RMENU},
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
        {"tab", VK_TAB}, {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE}, {"insert", VK_INSERT},
        {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"up", VK_UP}, {"down", VK_DOWN},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"capslock", VK_CAPITAL}};
    if (s.compare(0, 5, "mouse") == 0 && s.size() == 6 && s[5] >= '1' && s[5] <= '5') { k.mouse = s[5] - '0'; return true; }
    auto it = names.find(s);
    if (it != names.end()) { k.vk = it->second; return true; }
    if (s.size() == 1 && isalpha(static_cast<unsigned char>(s[0]))) { k.vk = static_cast<WORD>(toupper(s[0])); return true; }
    if (s.size() == 1 && isdigit(static_cast<unsigned char>(s[0]))) { k.vk = static_cast<WORD>(s[0]); return true; }
    if (s.size() >= 2 && s.size() <= 3 && s[0] == 'f' && isdigit(static_cast<unsigned char>(s[1]))) {
        int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) { k.vk = static_cast<WORD>(VK_F1 + n - 1); return true; }
    }
    if (s.size() == 4 && s.compare(0, 3, "num") == 0 && isdigit(static_cast<unsigned char>(s[3]))) { k.vk = static_cast<WORD>(VK_NUMPAD0 + (s[3] - '0')); return true; }
    uint64_t v;
    bool hex = s.size() > 2 && s[0] == '0' && s[1] == 'x';
    bool dec = s.size() >= 2 && s.find_first_not_of("0123456789") == std::string::npos;
    if ((hex || dec) && ParseNum(s, v) && v > 0 && v < 255) { k.vk = static_cast<WORD>(v); return true; }
    return false;
}

bool IsExtended(WORD vk) {
    switch (vk) {
        case VK_RCONTROL: case VK_RMENU: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT: return true;
        default: return false;
    }
}

bool GameFocused() {
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (!h) return false;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

void Pace() {  // rate limit: at least 4 ms between injected events
    long long now = NowUs();
    if (g_lastUs && now - g_lastUs < 4000) Sleep(static_cast<DWORD>((4000 - (now - g_lastUs)) / 1000 + 1));
    g_lastUs = NowUs();
}

bool Inject(const Key& k, bool down) {
    Pace();
    INPUT in = {};
    if (k.mouse) {
        in.type = INPUT_MOUSE;
        switch (k.mouse) {
            case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
            case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
            case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
            default: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = k.mouse == 4 ? XBUTTON1 : XBUTTON2; break;
        }
    } else {
        in.type = INPUT_KEYBOARD;
        UINT sc = MapVirtualKeyW(k.vk, MAPVK_VK_TO_VSC);
        if (sc) { in.ki.wScan = static_cast<WORD>(sc); in.ki.dwFlags = KEYEVENTF_SCANCODE; }  // scancodes work with raw-input/DirectInput games
        else in.ki.wVk = k.vk;
        if (IsExtended(k.vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    bool ok = SendInput(1, &in, sizeof in) == 1;
    std::lock_guard<std::mutex> l(g_hm);
    if (ok && down) g_held[k.id()] = k;
    if (ok && !down) g_held.erase(k.id());
    return ok;
}

void SleepMs(uint32_t ms) {
    while (ms && !g_stop) { uint32_t c = ms > 20 ? 20 : ms; Sleep(c); ms -= c; }
}

struct BusyGuard {
    bool ok;
    BusyGuard() { bool e = false; ok = g_busy.compare_exchange_strong(e, true); }
    ~BusyGuard() { if (ok) g_busy = false; }
};

// Executes steps; keys the script pressed and left down are released at the end. Returns "" on success or an error.
std::string Run(const std::vector<Step>& steps, std::string& info) {
    std::set<int> mine;
    std::vector<Key> keys;
    long long t0 = NowUs();
    std::string err;
    size_t done = 0;
    for (const Step& s : steps) {
        if (g_stop) { err = "plugin shutting down"; break; }
        if (s.kind == Step::Wait) { SleepMs(s.ms); ++done; continue; }
        if (s.kind != Step::Up && !GameFocused()) { err = F("game not focused (aborted before step %zu)", done + 1); break; }
        // releases are always sent (even if focus moved) so no key stays stuck down in the game
        if (s.kind == Step::Down || s.kind == Step::Tap) {
            if (!Inject(s.key, true)) { err = "SendInput failed"; break; }
            mine.insert(s.key.id());
            keys.push_back(s.key);
        }
        if (s.kind == Step::Tap) { SleepMs(s.ms); Inject(s.key, false); mine.erase(s.key.id()); }
        if (s.kind == Step::Up) { Inject(s.key, false); mine.erase(s.key.id()); }
        ++done;
    }
    std::string left;
    for (const Key& k : keys)
        if (mine.count(k.id())) {
            Inject(k, false);
            mine.erase(k.id());
            left += (left.empty() ? "" : ",") + k.name;
        }
    info = F("steps=%zu/%zu elapsed_ms=%lld", done, steps.size(), (NowUs() - t0) / 1000);
    if (!left.empty()) info += " released_at_end=" + left;
    return err;
}

std::string Trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool ParseScript(const std::string& script, std::vector<Step>& out, std::string& err) {
    uint64_t total = 0;
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t semi = script.find_first_of(";\n", pos);
        std::string tok = Trimmed(script.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        pos = semi == std::string::npos ? script.size() + 1 : semi + 1;
        if (tok.empty()) continue;
        std::vector<std::string> p;
        size_t q = 0, c;
        while ((c = tok.find(':', q)) != std::string::npos) { p.push_back(Trimmed(tok.substr(q, c - q))); q = c + 1; }
        p.push_back(Trimmed(tok.substr(q)));
        Step s;
        uint64_t ms = 0;
        if (Lower(p[0]) == "wait") {
            if (p.size() != 2 || !ParseNum(p[1], ms) || ms > kMaxScriptMs) { err = "bad step '" + tok + "' (wait:<ms>, 0..60000)"; return false; }
            s.kind = Step::Wait; s.ms = static_cast<uint32_t>(ms);
        } else {
            if (p.size() < 2 || p.size() > 3 || !ParseKey(p[0], s.key)) { err = "bad step '" + tok + "' (KEY:down|up|tap[:hold_ms])"; return false; }
            std::string a = Lower(p[1]);
            s.kind = a == "down" ? Step::Down : a == "up" ? Step::Up : a == "tap" ? Step::Tap : Step::Wait;
            if (a != "down" && a != "up" && a != "tap") { err = "bad action in '" + tok + "'"; return false; }
            if (p.size() == 3) {
                if (s.kind != Step::Tap || !ParseNum(p[2], ms) || ms > kMaxHold) { err = "hold_ms only valid for tap, max 5000: '" + tok + "'"; return false; }
                s.ms = static_cast<uint32_t>(ms);
            } else if (s.kind == Step::Tap) s.ms = kDefaultHold;
        }
        total += s.ms;
        if (total > kMaxScriptMs) { err = "script longer than 60 s"; return false; }
        out.push_back(s);
        if (out.size() > kMaxSteps) { err = "more than 600 steps"; return false; }
    }
    if (out.empty()) { err = "empty script"; return false; }
    return true;
}
}  // namespace

void InputReleaseAll() {
    std::vector<Key> ks;
    {
        std::lock_guard<std::mutex> l(g_hm);
        for (auto& kv : g_held) ks.push_back(kv.second);
    }
    for (const Key& k : ks) Inject(k, false);
}

std::string InputSummary() {
    std::lock_guard<std::mutex> l(g_hm);
    std::string o;
    for (auto& kv : g_held) o += F("key held: %s\n", kv.second.name.c_str());
    return o;
}

std::string CmdKey(const Args& t) {
    if (t.size() < 3 || t.size() > 4) return Err("usage: key <vk> tap|down|up [hold_ms]");
    Step s;
    if (!ParseKey(t[1], s.key)) return Err("unknown key: " + t[1]);
    std::string a = Lower(t[2]);
    if (a != "tap" && a != "down" && a != "up") return Err("action must be tap, down or up");
    s.kind = a == "tap" ? Step::Tap : a == "down" ? Step::Down : Step::Up;
    s.ms = kDefaultHold;
    if (t.size() == 4) {
        uint64_t ms;
        if (s.kind != Step::Tap) return Err("hold_ms is only valid with tap");
        if (!ParseNum(t[3], ms) || ms > kMaxHold) return Err("hold_ms must be 0..5000");
        s.ms = static_cast<uint32_t>(ms);
    }
    BusyGuard g;
    if (!g.ok) return Err("input busy (another key/keys command is running)");
    if (s.kind != Step::Up && !GameFocused()) return Err("game not focused");
    std::string info, e;
    if (s.kind == Step::Down) {  // intentionally leave it down; Run() would release it at the end
        if (!Inject(s.key, true)) return Err("SendInput failed");
        return Ok("pressed " + s.key.name + " (still down until `key " + s.key.name + " up`)\n");
    }
    e = Run({s}, info);
    return e.empty() ? Ok(info + "\n") : Err(e);
}

std::string CmdKeys(const Args& t) {
    if (t.size() < 2) return Err("usage: keys <script>   e.g. W:down;wait:300;J:tap;wait:50;W:up");
    std::string script;
    for (size_t i = 1; i < t.size(); ++i) script += (i > 1 ? " " : "") + t[i];
    std::vector<Step> steps;
    std::string e;
    if (!ParseScript(script, steps, e)) return Err(e);
    BusyGuard g;
    if (!g.ok) return Err("input busy (another key/keys command is running)");
    bool needsFocus = false;
    for (const Step& s : steps) if (s.kind == Step::Down || s.kind == Step::Tap) needsFocus = true;
    if (needsFocus && !GameFocused()) return Err("game not focused");
    std::string info;
    e = Run(steps, info);
    Log("keys script: %s%s%s", info.c_str(), e.empty() ? "" : " ERROR: ", e.c_str());
    return e.empty() ? Ok(info + "\n") : Err(e + " (" + info + ")");
}

std::string CmdKeysRelease() {
    InputReleaseAll();
    return Ok("released all keys held by the bridge\n");
}
