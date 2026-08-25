//=============================================================================
//  main.cpp - SwitchProXInput entry point.
//=============================================================================
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>

#include "config.h"
#include "engine.h"

extern int gui_run(HINSTANCE hInst, bool startHidden);
extern const wchar_t* gui_class_name();
extern void gui_notify_bridge(int evt, int a, int b, int c);

typedef BOOL (WINAPI* pfnSetProcessDpiAwarenessContext)(HANDLE);

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Per-monitor DPI awareness (Win10 1703+); fall back to system DPI.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        pfnSetProcessDpiAwarenessContext fn =
            (pfnSetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else {
            typedef BOOL (WINAPI* pfnSetProcessDPIAware)(void);
            pfnSetProcessDPIAware fn2 = (pfnSetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
            if (fn2) fn2();
        }
    }

    // Command line: --minimized (used by the "start with Windows" entry).
    bool startHidden = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i)
            if (lstrcmpiW(argv[i], L"--minimized") == 0) startHidden = true;
        LocalFree(argv);
    }

    // Single instance: a second launch just shows the existing window.
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\SwitchProXInput.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(gui_class_name(), NULL);
        if (other) {
            ShowWindow(other, SW_SHOW);
            SetForegroundWindow(other);
        }
        CloseHandle(mutex);
        return 0;
    }

    engine_set_notifier(gui_notify_bridge);
    engine_start();
    engine_apply_config(loadConfig());
    engine_try_connect_bus();

    int rc = gui_run(hInst, startHidden);

    engine_stop();
    if (mutex) CloseHandle(mutex);
    return rc;
}
