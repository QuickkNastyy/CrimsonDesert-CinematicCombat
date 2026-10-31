// Fake xinput1_4.dll for the test host (NOT the game): XInputGetState / XInputGetStateEx (ordinal 100) return a scripted pad 0.
// FakePadSet(buttons, lx) is called by harness.exe, which reads the script from pad.txt.
#include <windows.h>
static volatile LONG g_buttons = 0, g_lx = 0, g_connected = 0, g_packet = 0;
struct XSTATE { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
extern "C" DWORD WINAPI XInputGetState(DWORD idx, XSTATE* st) {
    if (idx != 0 || !g_connected || !st) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    st->packet = (DWORD)InterlockedIncrement(&g_packet);
    st->buttons = (WORD)g_buttons;
    st->lt = st->rt = 0;
    st->lx = (SHORT)g_lx; st->ly = st->rx = st->ry = 0;
    return 0;
}
extern "C" DWORD WINAPI XInputGetStateEx(DWORD idx, XSTATE* st) { return XInputGetState(idx, st); }
extern "C" __declspec(dllexport) void WINAPI FakePadSet(LONG buttons, LONG lx, LONG connected) { g_buttons = buttons; g_lx = lx; g_connected = connected; }
