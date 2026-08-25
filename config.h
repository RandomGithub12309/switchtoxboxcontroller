//=============================================================================
//  config.h - INI configuration load/save for SwitchProXInput.
//=============================================================================
#pragma once

#include <string>
#include <vector>
#include <utility>
#include "parse.h"

// App-level preferences (kept in the [System] INI section).
struct AppPrefs {
    bool autostart      = false;   // run at Windows logon (registry Run key)
    bool minimizeToTray = true;    // minimize button hides to tray
    bool closeToTray    = true;    // X button hides to tray instead of quitting
};

std::wstring exePathW();   // full path of the running exe
std::wstring exeDirW();    // directory of the running exe (with trailing backslash)

Config    loadConfig();
void      saveConfig(const Config& cfg);
AppPrefs  loadPrefs();
void      savePrefs(const AppPrefs& p);

// Parse/format the Devices list ("057E:2009;057E:200E").
bool         parseDevices(const std::wstring& text, std::vector<std::pair<USHORT, USHORT>>& out);
std::wstring devicesToString(const std::vector<std::pair<USHORT, USHORT>>& devs);
