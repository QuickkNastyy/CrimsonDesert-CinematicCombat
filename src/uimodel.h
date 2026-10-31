// The logic of the in-game panel without any window code: the row model bound to the settings table, the hotkey parser /
// debounced edge detector, the controller navigation state machine and the XInput "swallow" transform. Everything here is pure
// (explicit time and input arguments) so it can be tested without a window or a pad.
#pragma once
#include "settings.h"
#include <cstdint>
#include <string>
#include <vector>

enum class UiKind { Master, Check, Slider, ButtonRow, Header };   // Header = a section title (not selectable)
enum class UiAct { None, PresetVanilla, PresetCinematic, PresetExtreme, PresetHeavy, SaveClose, Discard };
enum class UiResult { None, Close };

struct UiButton { std::string label; UiAct act; };
struct UiRow {
    UiKind kind = UiKind::Header;
    std::string label;
    int cfg = -1;                  // bound setting (Check / HeaderCheck / Slider)
    bool experimental = false;
    std::string title;             // name shown (bold) at the top of the help pane, e.g. "Slow motion: Scale"
    std::string help;              // plain-language explanation (help pane)
    std::string tip;               // one practical tip line (help pane)
    std::string tag;               // Header: small amber tag at the right of the title (e.g. "experimental")
    int accent = 0;                // Header: colour index of the section
    const char* fmt = "%g";        // slider value format
    const char* zeroText = nullptr; // slider: shown instead of the number when the value is 0 (e.g. "off")
    const char* const* names = nullptr; int nNames = 0;   // slider shown as a word: the value is rounded to an index into names
    int modeMask = 0;               // 0 = always visible; otherwise bit m set = visible while BlindingFlash.Mode == m
    std::vector<UiButton> buttons; // ButtonRow
    bool Interactive() const { return kind != UiKind::Header; }
};

// The content of the help pane for one row.
struct UiHelp {
    std::string title, body, tip;
    std::vector<std::pair<std::string, std::string>> facts;   // "Range" / "Default" / "Current" ...
};

// Where the help window goes: flush to the RIGHT edge of the main panel when it fits on the screen, else flush to its LEFT edge.
struct UiRect { int left, top, right, bottom; };
struct UiDock { int x; bool onRight; };
UiDock UiDockHelp(const UiRect& panel, const UiRect& work, int helpWidth);

class UiModel {
public:
    UiModel();
    const std::vector<UiRow>& Rows() const { return rows_; }
    int Selected() const { return Visible(sel_) ? sel_ : FirstVisible(); }   // a hidden row (its Blinding Flash mode is not active) falls back to the first control
    bool Visible(int row) const;               // rows that belong to another Blinding Flash mode are hidden
    int FirstVisible() const;
    int SubSel() const { return sub_; }
    void SetSub(int j) { sub_ = j < 0 ? 0 : j; }   // mouse click on a button of the selected row
    void Select(int row);                      // clamps to an interactive row
    void Move(int dy);                         // next / previous interactive row (wraps around)
    void Adjust(int steps);                    // slider: +-steps*step (snapped); check: >0 on, <0 off; button row: moves the sub selection
    bool SetFraction(int row, double frac);    // slider from 0..1 (mouse drag), snapped to the step grid
    double Fraction(int row) const;            // 0..1
    void SetValueBound(int row, bool toMin);   // Home / End
    void BeginSession();                       // remember every setting as it is NOW (called when the panel opens)
    void Discard();                            // "Close without saving": put every setting back to the BeginSession values (live)
    bool SaveAndClose();                       // "Save & Close": write CombatMod.ini; false (panel stays open, message says why) when it fails
    const std::string& Help(int row) const { return rows_[static_cast<size_t>(row)].help; }
    UiHelp HelpFor(int row) const;             // everything the help pane shows for a row (title, explanation, range/default/current, tip)
    UiResult Activate();                       // Enter / Space / pad A on the selected row
    UiResult ActivateButton(int row, int sub); // mouse click on a button
    bool CheckOn(int row) const;
    bool MasterOn() const;                     // SlowMo.Enabled && Launch.Enabled
    std::string ValueText(int row) const;
    static std::string FmtValue(const UiRow& r, float v);   // a slider value as shown (zeroText for 0)
    static double Snap(const CfgDef& d, double v, float maxv = -1.0f);   // round to the step grid, clamp to [min, max] (maxv < 0: the table's max)
    std::wstring savePath;                     // target of the Save button (default: CombatMod.ini next to the asi)
    std::string message;                       // result of the last Save / preset (shown in the status area)
    std::vector<std::pair<std::string, std::string>> status;   // key / value pairs of the Status block, filled by the window every 500 ms
    CfgSnapshot session;                       // values when the panel was opened

private:
    std::vector<UiRow> rows_;
    int sel_ = 0, sub_ = 0;
    UiResult Run(UiAct a);
};

// ---- hotkeys
struct UiHotkey { bool ctrl = false, shift = false, alt = false; int vk = 0; };
bool UiParseHotkey(const std::string& text, UiHotkey& out);   // "Ctrl+Enter", "Ctrl+Shift+F5", "Alt+Space" ...
std::string UiHotkeyStr(const UiHotkey& k);

// Rising-edge detector with debounce: one true per press, further edges inside the debounce interval are ignored.
class EdgeToggle {
public:
    explicit EdgeToggle(long long debounceMs = 300) : debounce_(debounceMs) {}
    bool Update(bool down, long long nowMs);
    void Reset() { prev_ = false; last_ = -1000000; }
private:
    long long debounce_;
    bool prev_ = false;
    long long last_ = -1000000;
};

// ---- controller
namespace pad {
const uint16_t Up = 0x0001, Down = 0x0002, Left = 0x0004, Right = 0x0008, Start = 0x0010, Back = 0x0020, LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000;
}
struct XState {   // layout of XINPUT_STATE
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
inline void XInputSwallow(XState& s) { s.buttons = 0; s.lt = s.rt = 0; s.lx = s.ly = s.rx = s.ry = 0; }   // the game sees an idle pad (packet number is kept)
inline bool PadOpenCombo(uint16_t buttons) { return (buttons & pad::LB) && (buttons & pad::Down); }       // LB + DPad-Down

struct PadEvents {
    int moveY = 0;        // -1 up, +1 down (row selection)
    int adjust = 0;       // signed number of slider steps (accelerates while held)
    bool activate = false;
    bool back = false;     // B: close without saving
    bool start = false;    // Start: save and close
};
class PadNav {
public:
    PadEvents Update(uint16_t buttons, int16_t lx, long long nowMs);
    void Reset();
    void Latch() { latched_ = true; }   // ignore everything until all navigation buttons are released (call when the panel opens)
private:
    struct Dir { long long since = -1, lastRepeat = -1; };
    Dir up_, down_, horiz_;
    int horizDir_ = 0;
    bool prevA_ = false, prevBack_ = false, prevStart_ = false;
    bool latched_ = false;   // LB (the combo modifier) was held: ignore everything until all navigation buttons are released
};
