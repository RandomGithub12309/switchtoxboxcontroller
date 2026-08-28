//=============================================================================
//  config.cpp - INI configuration load/save for SwitchProXInput.
//=============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>
#include <string>
#include <vector>
#include <utility>
#include "config.h"

std::wstring exePathW() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return buf;
}

std::wstring exeDirW() {
    std::wstring p = exePathW();
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p;
}

static std::wstring iniPathW() {
    return exeDirW() + L"SwitchProXInput.ini";
}

static int iniInt(LPCWSTR file, LPCWSTR sec, LPCWSTR key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, file);
}

static bool parseHex16(const std::wstring& s, USHORT& out) {
    wchar_t* end = NULL;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (end == s.c_str() || v > 0xFFFF) return false;
    out = (USHORT)v;
    return true;
}

bool parseDevices(const std::wstring& text, std::vector<std::pair<USHORT, USHORT>>& out) {
    out.clear();
    size_t start = 0;
    while (start < text.size()) {
        size_t semi = text.find(L';', start);
        std::wstring tok = text.substr(start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
        start = (semi == std::wstring::npos) ? text.size() : semi + 1;

        // trim spaces
        while (!tok.empty() && (tok.front() == L' ' || tok.front() == L'\t')) tok.erase(0, 1);
        while (!tok.empty() && (tok.back() == L' ' || tok.back() == L'\t')) tok.pop_back();
        if (tok.empty()) continue;

        size_t colon = tok.find(L':');
        if (colon == std::wstring::npos) return false;
        USHORT vid = 0, pid = 0;
        if (!parseHex16(tok.substr(0, colon), vid) || !parseHex16(tok.substr(colon + 1), pid))
            return false;
        out.emplace_back(vid, pid);
    }
    return true;
}

std::wstring devicesToString(const std::vector<std::pair<USHORT, USHORT>>& devs) {
    std::wstring s;
    wchar_t buf[16];
    for (size_t i = 0; i < devs.size(); ++i) {
        if (i) s += L';';
        swprintf(buf, 16, L"%04X:%04X", devs[i].first, devs[i].second);
        s += buf;
    }
    return s;
}

Config loadConfig() {
    Config cfg;   // defaults
    std::wstring ini = iniPathW();

    cfg.maxControllers  = std::max(1, std::min(4, iniInt(ini.c_str(), L"General", L"MaxControllers", 4)));
    cfg.buttonLayout    = iniInt(ini.c_str(), L"Mapping", L"ButtonLayout", 0) != 0;
    cfg.captureButton   = iniInt(ini.c_str(), L"Mapping", L"CaptureButton", 0);
    cfg.homeButton      = iniInt(ini.c_str(), L"Mapping", L"HomeButton", 1);
    cfg.swapShoulders   = iniInt(ini.c_str(), L"Mapping", L"SwapShoulders", 0) != 0;
    cfg.leftDeadzone    = std::max(0, std::min(90, iniInt(ini.c_str(), L"Mapping", L"LeftDeadzone", 10)));
    cfg.rightDeadzone   = std::max(0, std::min(90, iniInt(ini.c_str(), L"Mapping", L"RightDeadzone", 10)));
    cfg.leftStickRange  = std::max(100, std::min(2048, iniInt(ini.c_str(), L"Mapping", L"LeftStickRange", 1700)));
    cfg.rightStickRange = std::max(100, std::min(2048, iniInt(ini.c_str(), L"Mapping", L"RightStickRange", 1700)));
    cfg.invertLX = iniInt(ini.c_str(), L"Mapping", L"InvertLX", 0) != 0;
    cfg.invertLY = iniInt(ini.c_str(), L"Mapping", L"InvertLY", 0) != 0;
    cfg.invertRX = iniInt(ini.c_str(), L"Mapping", L"InvertRX", 0) != 0;
    cfg.invertRY = iniInt(ini.c_str(), L"Mapping", L"InvertRY", 0) != 0;
    cfg.enableRumble = iniInt(ini.c_str(), L"Features", L"EnableRumble", 1) != 0;
    cfg.driftFix         = iniInt(ini.c_str(), L"DriftFix", L"Enable", 1) != 0;
    cfg.driftAutoDeadzone= iniInt(ini.c_str(), L"DriftFix", L"AutoDeadzone", 1) != 0;
    cfg.driftStrength    = std::max(0, std::min(2, iniInt(ini.c_str(), L"DriftFix", L"Strength", 1)));

    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(L"General", L"Devices", L"057E:2009", buf, 1024, ini.c_str());
    if (!parseDevices(buf, cfg.devices) || cfg.devices.empty())
        cfg.devices = { { 0x057E, 0x2009 } };

    return cfg;
}

void saveConfig(const Config& cfg) {
    std::wstring ini = iniPathW();
    wchar_t num[32];

    swprintf(num, 32, L"%d", cfg.maxControllers);
    WritePrivateProfileStringW(L"General", L"MaxControllers", num, ini.c_str());
    WritePrivateProfileStringW(L"General", L"Devices", devicesToString(cfg.devices).c_str(), ini.c_str());

    swprintf(num, 32, L"%d", cfg.buttonLayout ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"ButtonLayout", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.captureButton);
    WritePrivateProfileStringW(L"Mapping", L"CaptureButton", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.homeButton ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"HomeButton", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.swapShoulders ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"SwapShoulders", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.leftDeadzone);
    WritePrivateProfileStringW(L"Mapping", L"LeftDeadzone", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.rightDeadzone);
    WritePrivateProfileStringW(L"Mapping", L"RightDeadzone", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.leftStickRange);
    WritePrivateProfileStringW(L"Mapping", L"LeftStickRange", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.rightStickRange);
    WritePrivateProfileStringW(L"Mapping", L"RightStickRange", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.invertLX ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"InvertLX", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.invertLY ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"InvertLY", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.invertRX ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"InvertRX", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.invertRY ? 1 : 0);
    WritePrivateProfileStringW(L"Mapping", L"InvertRY", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.enableRumble ? 1 : 0);
    WritePrivateProfileStringW(L"Features", L"EnableRumble", num, ini.c_str());

    swprintf(num, 32, L"%d", cfg.driftFix ? 1 : 0);
    WritePrivateProfileStringW(L"DriftFix", L"Enable", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.driftAutoDeadzone ? 1 : 0);
    WritePrivateProfileStringW(L"DriftFix", L"AutoDeadzone", num, ini.c_str());
    swprintf(num, 32, L"%d", cfg.driftStrength);
    WritePrivateProfileStringW(L"DriftFix", L"Strength", num, ini.c_str());
}

AppPrefs loadPrefs() {
    AppPrefs p;
    std::wstring ini = iniPathW();
    p.autostart      = iniInt(ini.c_str(), L"System", L"Autostart", 0) != 0;
    p.minimizeToTray = iniInt(ini.c_str(), L"System", L"MinimizeToTray", 1) != 0;
    p.closeToTray    = iniInt(ini.c_str(), L"System", L"CloseToTray", 1) != 0;
    return p;
}

void savePrefs(const AppPrefs& p) {
    std::wstring ini = iniPathW();
    wchar_t num[8];
    swprintf(num, 8, L"%d", p.autostart ? 1 : 0);
    WritePrivateProfileStringW(L"System", L"Autostart", num, ini.c_str());
    swprintf(num, 8, L"%d", p.minimizeToTray ? 1 : 0);
    WritePrivateProfileStringW(L"System", L"MinimizeToTray", num, ini.c_str());
    swprintf(num, 8, L"%d", p.closeToTray ? 1 : 0);
    WritePrivateProfileStringW(L"System", L"CloseToTray", num, ini.c_str());
}
