// In-game control panel ("Cinematic Combat"): a plain Win32 window on its OWN thread, custom drawn, bound live to the settings table.
// Hotkey Ctrl+Enter (configurable) or LB + DPad-Down toggles it; while it is open the game's XInput polling is fed an idle pad.
// No third-party code; user32/gdi32 only (plus dynamic xinput1_4 / dwmapi). Never touches game code or game threads.
#pragma once
#include "util.h"

void UiInit();                       // reads [UI] from the ini; starts the UI thread when Enabled=1 (the window itself is created on first open)
void UiShutdown(bool wait);          // close + destroy the panel, remove the XInput hooks, stop the thread (wait=false inside DllMain)
bool UiIsOpen();
std::string CmdUi(const Args& t);    // ui show|hide|toggle|status|snapshot <path>
