//=============================================================================
//  gui.cpp - SwitchProXInput GUI.
//  Custom-drawn dark Win32 UI with a standard window frame (minimize /
//  maximize / close / resize), DPI-aware scaling, a slim scrollbar with
//  mouse-wheel support, a system-tray icon, and "start with Windows".
//=============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

#include "parse.h"
#include "drift.h"
#include "config.h"
#include "engine.h"

//=============================================================================
//  Constants
//=============================================================================

#define IDI_ICON1              101

#define WM_APP_ENGINE_EVENT    (WM_APP + 1)
#define WM_APP_TRAY            (WM_APP + 2)
#define TRAY_UID               1

// Tray menu ids
#define TRAY_MENU_OPEN         2001
#define TRAY_MENU_HIDE         2002
#define TRAY_MENU_EXIT         2003

// Control ids
enum {
    IDC_BTN_LAYOUT = 100,
    IDC_CAPTURE,
    IDC_HOME,
    IDC_SWAP,
    IDC_LDZ,
    IDC_RDZ,
    IDC_LRANGE,
    IDC_RRANGE,
    IDC_INV_LX,
    IDC_INV_LY,
    IDC_INV_RX,
    IDC_INV_RY,
    IDC_RUMBLE,
    IDC_MAXPADS,
    IDC_DRIFT,
    IDC_DRIFT_DZ,
    IDC_DRIFT_STRENGTH,
    IDC_AUTOSTART,
    IDC_TRAYMIN,
    IDC_TRAYCLOSE,
    IDC_DEVICES = 120,
    IDC_BTN_DEFAULTS = 130,
    IDC_BTN_HIDE,
    IDC_BTN_RETRY = 140,
};

//=============================================================================
//  Colors
//=============================================================================

#define COL_BG        RGB(20, 21, 26)
#define COL_CARD      RGB(30, 32, 39)
#define COL_CARD2     RGB(38, 41, 50)
#define COL_INSET     RGB(36, 39, 48)
#define COL_BORDER    RGB(49, 52, 63)
#define COL_TEXT      RGB(232, 234, 240)
#define COL_DIM       RGB(139, 144, 160)
#define COL_ACCENT    RGB(88, 140, 255)
#define COL_ACCENT2   RGB(108, 156, 255)
#define COL_GREEN     RGB(52, 199, 123)
#define COL_RED       RGB(240, 71, 71)
#define COL_TRACK     RGB(43, 46, 56)
#define COL_KNOB      RGB(232, 234, 240)
#define COL_HOVITEM   RGB(58, 68, 88)
#define COL_WARN      RGB(255, 184, 54)

//=============================================================================
//  Globals
//=============================================================================

static HINSTANCE g_hInst;
static HWND      g_hwnd = NULL;
static HWND      g_editDevices = NULL;
static HWND      g_popup = NULL;
static int       g_popupCtrl = -1;
static int       g_popupHover = -1;

static HFONT g_fTitle, g_fSection, g_fText, g_fSmall, g_fEdit;
static HBRUSH g_brushEdit = NULL;

static float g_scale = 1.0f;

// content (virtual) size vs. viewport (client) size
static int g_winW = 0, g_winH = 0;        // natural content size
static int g_viewW = 0, g_viewH = 0;      // client area size
static int g_scrollY = 0;                 // vertical scroll offset
static int g_maxScroll = 0;               // g_winH - g_viewH (>= 0)
static int g_offX = 0;                    // horizontal centering offset

// scrollbar
static bool g_sbDragging = false;

static Config    g_cfg;
static AppPrefs  g_prefs;
static EngineStatus g_status;
static DriftView g_driftL, g_driftR;   // live drift status of the first pad
static bool      g_driftValid = false;

static bool g_trayAdded = false;
static bool g_hidden = false;
static bool g_balloonShown = false;
static NOTIFYICONDATAW g_nid = {};

static int  g_hotId = -1;       // hovered control id (or special ids below)
static bool g_tracking = false;

// special hot ids
#define HOT_DEFAULTS -4
#define HOT_HIDE     -5
#define HOT_RETRY    -6
#define HOT_CALIB    -7

//=============================================================================
//  Control model
//=============================================================================

enum class CtrlType { Toggle, MiniToggle, Slider, Combo, Stepper };

struct Ctrl {
    int      id;
    CtrlType type;
    RECT     rc;        // row rect (or pill rect for mini toggles, box for combos)
    RECT     rcLabel;   // label text area
    std::wstring label;
    bool     bval;
    int      ival, minv, maxv;
    std::vector<std::wstring> options;
    bool     dragging;
};

struct Section {
    RECT rc;
    std::wstring title;
};

static std::vector<Ctrl> g_ctrls;
static std::vector<Section> g_secs;

static RECT g_rcStatus, g_rcBtnRetry;
static RECT g_rcBottomBar, g_rcBtnDefaults, g_rcBtnHide;
static RECT g_rcEdit;
static RECT g_invLabelRect;   // label rect of the "Invert axes" row
static RECT g_rcBtnCalib, g_rcCalibHint;
static RECT g_rcDriftLineL, g_rcDriftLineR;

static const wchar_t* kMainClass = L"SwitchProXInputMainWnd";
static const wchar_t* kPopupClass = L"SwitchProXInputPopupWnd";

//=============================================================================
//  Forward declarations
//=============================================================================

static void quitApp();
static void doClose();
static void applyAutostartDecl(bool on);
static void g_cfgApplyFromSlider(Ctrl* c);
static void updateEditPos();
static void setScroll(int y);

//=============================================================================
//  Small helpers
//=============================================================================

static int S(int v) { return (int)(v * g_scale); }

static Ctrl* ctrl(int id) {
    for (auto& c : g_ctrls)
        if (c.id == id) return &c;
    return NULL;
}

static void fillRectC(HDC hdc, const RECT& r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FillRect(hdc, &r, br);
    DeleteObject(br);
}

static void fillRound(HDC hdc, const RECT& r, int rad, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void strokeRound(HDC hdc, const RECT& r, int rad, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ old = SelectObject(hdc, pen);
    HGDIOBJ br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, old);
    SelectObject(hdc, br);
    DeleteObject(pen);
}

static void drawTextC(HDC hdc, const RECT& r, const wchar_t* txt, HFONT f, COLORREF c, UINT fmt) {
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ of = SelectObject(hdc, f);
    RECT rc = r;
    DrawTextW(hdc, txt, -1, &rc, fmt);
    SelectObject(hdc, of);
}

static void drawDot(HDC hdc, int cx, int cy, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// Per-type sub-rects (single source of truth for painting AND hit-testing)

static RECT pillRect(const Ctrl& c) {
    RECT r = c.rc;
    r.left = r.right - S(44);
    r.top += S(5);
    r.bottom -= S(5);
    return r;
}

static RECT trackRect(const Ctrl& c) {
    RECT r = c.rc;
    r.top += S(24);
    r.bottom = r.top + S(4);
    r.right -= S(56);
    return r;
}

static RECT stepperMinusRect(const Ctrl& c) {
    RECT r = c.rc;
    int cy = (r.top + r.bottom) / 2;
    return { r.right - S(88), cy - S(11), r.right - S(66), cy + S(11) };
}

static RECT stepperValueRect(const Ctrl& c) {
    RECT r = c.rc;
    int cy = (r.top + r.bottom) / 2;
    return { r.right - S(66), cy - S(11), r.right - S(24), cy + S(11) };
}

static RECT stepperPlusRect(const Ctrl& c) {
    RECT r = c.rc;
    int cy = (r.top + r.bottom) / 2;
    return { r.right - S(24), cy - S(11), r.right - S(2), cy + S(11) };
}

//=============================================================================
//  Layout (natural content size; the window may be smaller - then it scrolls)
//=============================================================================

static void relayout() {
    const int W = S(780);
    const int margin = S(14);
    const int gap = S(10);
    const int statusY = S(8);
    const int statusH = S(64);
    const int colW = (W - 2 * margin - gap) / 2;
    const int xL = margin;
    const int xR = margin + colW + gap;
    const int pad = S(10);
    const int headH = S(18);
    const int rowH = S(30);
    const int sliderH = S(34);
    const int innerW = colW - 2 * pad;
    const int labelW = (int)(innerW * 0.46f);

    g_ctrls.clear();
    g_secs.clear();

    g_rcStatus = { margin, statusY, W - margin, statusY + statusH };
    g_rcBtnRetry = { g_rcStatus.right - S(76), g_rcStatus.top + S(8),
                     g_rcStatus.right - S(10), g_rcStatus.top + S(32) };

    int y = statusY + statusH + S(8);
    int colTop = y;   // both columns start at the same height

    auto startCard = [&](int x, const wchar_t* title, int h) {
        RECT r = { x, y, x + colW, y + h };
        g_secs.push_back({ r, title });
        y += h + gap;
        return r;
    };

    auto addCombo = [&](RECT card, int& cy, int id, const wchar_t* label,
                        std::vector<std::wstring> opts, int labelW) {
        RECT r = { card.left + pad, cy, card.right - pad, cy + rowH };
        cy += rowH;
        Ctrl c = { id, CtrlType::Combo, r, { r.left, r.top, r.left + labelW, r.bottom },
                   label, false, 0, 0, 0, opts, false };
        c.rc = { r.left + labelW + S(8), r.top + S(2), r.right, r.bottom - S(2) };
        g_ctrls.push_back(c);
    };

    auto addToggle = [&](RECT card, int& cy, int id, const wchar_t* label) {
        RECT r = { card.left + pad, cy, card.right - pad, cy + rowH };
        cy += rowH;
        g_ctrls.push_back({ id, CtrlType::Toggle, r,
                            { r.left, r.top, r.right - S(52), r.bottom },
                            label, false, 0, 0, 0, {}, false });
    };

    // ------- left column -------
    RECT c1 = startCard(xL, L"CONTROLLER", pad + headH + 4 * rowH + pad);
    int cy = c1.top + pad + headH;
    addCombo(c1, cy, IDC_BTN_LAYOUT, L"Face buttons",
             { L"Xbox positions (match prompts)", L"Same labels (A=A, B=B)" }, labelW);
    addCombo(c1, cy, IDC_CAPTURE, L"Capture button",
             { L"Unmapped", L"Back", L"Start", L"Guide" }, labelW);
    addCombo(c1, cy, IDC_HOME, L"Home button",
             { L"Unmapped", L"Guide (Xbox button)" }, labelW);
    addToggle(c1, cy, IDC_SWAP, L"Swap L/ZL and R/ZR");

    RECT c2 = startCard(xL, L"FEATURES", pad + headH + 2 * rowH + pad);
    cy = c2.top + pad + headH;
    addToggle(c2, cy, IDC_RUMBLE, L"HD rumble");
    {
        RECT r = { c2.left + pad, cy, c2.right - pad, cy + rowH };
        cy += rowH;
        g_ctrls.push_back({ IDC_MAXPADS, CtrlType::Stepper, r,
                            { r.left, r.top, r.left + labelW, r.bottom },
                            L"Max controllers", false, 4, 1, 4, {}, false });
    }

    RECT c3 = startCard(xL, L"CONTROLLER IDS (VID:PID)", pad + headH + S(24) + S(14) + pad);
    cy = c3.top + pad + headH;
    g_rcEdit = { c3.left + pad, cy, c3.right - pad, cy + S(22) };

    // ------- right column (same top as the left column) -------
    y = colTop;
    RECT s1 = startCard(xR, L"STICKS", pad + headH + 4 * sliderH + rowH + pad);
    cy = s1.top + pad + headH;
    {
        struct { int id; const wchar_t* label; int minv, maxv, def; } defs[4] = {
            { IDC_LDZ,    L"Left deadzone",    0, 90,   10 },
            { IDC_RDZ,    L"Right deadzone",   0, 90,   10 },
            { IDC_LRANGE, L"Left stick range", 100, 2048, 1700 },
            { IDC_RRANGE, L"Right stick range",100, 2048, 1700 },
        };
        for (int i = 0; i < 4; ++i) {
            RECT r = { s1.left + pad, cy, s1.right - pad, cy + sliderH };
            cy += sliderH;
            g_ctrls.push_back({ defs[i].id, CtrlType::Slider, r,
                                { r.left, r.top, r.left + labelW, r.bottom },
                                defs[i].label, false, defs[i].def, defs[i].minv, defs[i].maxv, {}, false });
        }
    }
    {
        RECT r = { s1.left + pad, cy, s1.right - pad, cy + rowH };
        cy += rowH;
        int gy = r.top + S(7);
        int gx = s1.left + pad + labelW + S(8);
        int gw = (s1.right - pad - gx) / 4;
        int ids[4] = { IDC_INV_LX, IDC_INV_LY, IDC_INV_RX, IDC_INV_RY };
        const wchar_t* labs[4] = { L"LX", L"LY", L"RX", L"RY" };
        for (int i = 0; i < 4; ++i) {
            RECT pill = { gx + i * gw + S(16), gy, gx + i * gw + S(16) + S(26), gy + S(14) };
            g_ctrls.push_back({ ids[i], CtrlType::MiniToggle, pill,
                                { gx + i * gw, gy, gx + i * gw + S(16), gy + S(14) },
                                labs[i], false, 0, 0, 0, {}, false });
        }
        g_invLabelRect = { r.left, r.top, gx - S(6), r.bottom };
    }

    RECT sd = startCard(xR, L"ANTI-DRIFT", pad + headH + 4 * rowH + S(2) + 2 * S(20) + pad);
    cy = sd.top + pad + headH;
    addToggle(sd, cy, IDC_DRIFT, L"Auto drift correction");
    addCombo(sd, cy, IDC_DRIFT_STRENGTH, L"Correction strength",
             { L"Gentle", L"Balanced", L"Aggressive" }, labelW);
    addToggle(sd, cy, IDC_DRIFT_DZ, L"Auto deadzone from noise");
    {
        RECT r = { sd.left + pad, cy, sd.right - pad, cy + rowH };
        cy += rowH;
        g_rcBtnCalib = { r.right - S(132), r.top + S(4), r.right, r.bottom - S(4) };
        g_rcCalibHint = { r.left, r.top, g_rcBtnCalib.left - S(8), r.bottom };
        g_rcDriftLineL = { r.left, cy + S(2), sd.right - pad, cy + S(2) + S(20) };
        g_rcDriftLineR = { r.left, cy + S(2) + S(20), sd.right - pad, cy + S(2) + 2 * S(20) };
    }

    RECT s2 = startCard(xR, L"SYSTEM", pad + headH + 3 * rowH + pad);
    cy = s2.top + pad + headH;
    addToggle(s2, cy, IDC_AUTOSTART, L"Start with Windows");
    addToggle(s2, cy, IDC_TRAYMIN, L"Minimize to tray");
    addToggle(s2, cy, IDC_TRAYCLOSE, L"Close to tray");

    // bottom bar
    int bottomY = y + S(2);
    g_rcBottomBar = { margin, bottomY, W - margin, bottomY + S(36) };
    g_rcBtnDefaults = { margin, bottomY + S(4), margin + S(150), bottomY + S(32) };
    g_rcBtnHide = { W - margin - S(140), bottomY + S(4), W - margin, bottomY + S(32) };

    g_winW = W;
    g_winH = bottomY + S(36) + S(8);
}

//=============================================================================
//  Fonts
//=============================================================================

static HFONT makeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void makeFonts() {
    if (g_fTitle) DeleteObject(g_fTitle);
    if (g_fSection) DeleteObject(g_fSection);
    if (g_fText) DeleteObject(g_fText);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fEdit) DeleteObject(g_fEdit);
    g_fTitle   = makeFont(S(15), FW_SEMIBOLD);
    g_fSection = makeFont(S(11), FW_SEMIBOLD);
    g_fText    = makeFont(S(14), FW_NORMAL);
    g_fSmall   = makeFont(S(12), FW_NORMAL);
    g_fEdit    = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
}

//=============================================================================
//  "Start with Windows" (HKCU Run key)
//=============================================================================

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunVal = L"SwitchProXInput";

static bool readAutostart() {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    bool on = RegQueryValueExW(k, kRunVal, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void applyAutostartDecl(bool on) {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + exePathW() + L"\" --minimized";
        RegSetValueExW(k, kRunVal, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunVal);
    }
    RegCloseKey(k);
}

//=============================================================================
//  Settings application
//=============================================================================

static void applySetting(int id) {
    Ctrl* c = ctrl(id);
    if (!c) return;
    switch (id) {
    case IDC_BTN_LAYOUT: g_cfg.buttonLayout    = c->ival; break;
    case IDC_CAPTURE:    g_cfg.captureButton   = c->ival; break;
    case IDC_HOME:       g_cfg.homeButton      = c->ival; break;
    case IDC_SWAP:       g_cfg.swapShoulders   = c->bval; break;
    case IDC_LDZ:        g_cfg.leftDeadzone    = c->ival; break;
    case IDC_RDZ:        g_cfg.rightDeadzone   = c->ival; break;
    case IDC_LRANGE:     g_cfg.leftStickRange  = c->ival; break;
    case IDC_RRANGE:     g_cfg.rightStickRange = c->ival; break;
    case IDC_INV_LX:     g_cfg.invertLX        = c->bval; break;
    case IDC_INV_LY:     g_cfg.invertLY        = c->bval; break;
    case IDC_INV_RX:     g_cfg.invertRX        = c->bval; break;
    case IDC_INV_RY:     g_cfg.invertRY        = c->bval; break;
    case IDC_RUMBLE:     g_cfg.enableRumble    = c->bval; break;
    case IDC_MAXPADS:    g_cfg.maxControllers  = c->ival; break;
    case IDC_DRIFT:          g_cfg.driftFix          = c->bval; break;
    case IDC_DRIFT_DZ:       g_cfg.driftAutoDeadzone = c->bval; break;
    case IDC_DRIFT_STRENGTH: g_cfg.driftStrength     = c->ival; break;
    case IDC_AUTOSTART:
        g_prefs.autostart = c->bval;
        applyAutostartDecl(c->bval);
        savePrefs(g_prefs);
        return;
    case IDC_TRAYMIN:
        g_prefs.minimizeToTray = c->bval;
        savePrefs(g_prefs);
        return;
    case IDC_TRAYCLOSE:
        g_prefs.closeToTray = c->bval;
        savePrefs(g_prefs);
        return;
    default: return;
    }
    engine_apply_config(g_cfg);
    saveConfig(g_cfg);
}

// Live-apply slider changes to the engine without touching the INI.
static void g_cfgApplyFromSlider(Ctrl* c) {
    switch (c->id) {
    case IDC_LDZ: g_cfg.leftDeadzone = c->ival; break;
    case IDC_RDZ: g_cfg.rightDeadzone = c->ival; break;
    case IDC_LRANGE: g_cfg.leftStickRange = c->ival; break;
    case IDC_RRANGE: g_cfg.rightStickRange = c->ival; break;
    default: return;
    }
    engine_apply_config(g_cfg);
}

// Refresh engine + drift status. Called on engine events and by a timer,
// because drift statistics evolve while the pad sits idle.
static void refreshStatus() {
    g_status = engine_get_status();
    g_driftValid = false;
    for (int i = 0; i < g_status.slotCount; ++i) {
        if (g_status.slots[i].connected &&
            engine_get_drift(i, g_driftL, g_driftR)) {
            g_driftValid = true;
            break;
        }
    }
}

static void commitDevices() {
    int len = GetWindowTextLengthW(g_editDevices);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(g_editDevices, &text[0], len + 1);
    text.resize(len);

    std::vector<std::pair<USHORT, USHORT>> devs;
    if (parseDevices(text, devs) && !devs.empty()) {
        g_cfg.devices = devs;
        engine_apply_config(g_cfg);
        saveConfig(g_cfg);
    } else {
        SetWindowTextW(g_editDevices, devicesToString(g_cfg.devices).c_str());
    }
}

static void syncUI() {
    Ctrl* c;
    if ((c = ctrl(IDC_BTN_LAYOUT))) c->ival = g_cfg.buttonLayout;
    if ((c = ctrl(IDC_CAPTURE)))    c->ival = g_cfg.captureButton;
    if ((c = ctrl(IDC_HOME)))       c->ival = g_cfg.homeButton;
    if ((c = ctrl(IDC_SWAP)))       c->bval = g_cfg.swapShoulders != 0;
    if ((c = ctrl(IDC_LDZ)))        c->ival = g_cfg.leftDeadzone;
    if ((c = ctrl(IDC_RDZ)))        c->ival = g_cfg.rightDeadzone;
    if ((c = ctrl(IDC_LRANGE)))     c->ival = g_cfg.leftStickRange;
    if ((c = ctrl(IDC_RRANGE)))     c->ival = g_cfg.rightStickRange;
    if ((c = ctrl(IDC_INV_LX)))     c->bval = g_cfg.invertLX != 0;
    if ((c = ctrl(IDC_INV_LY)))     c->bval = g_cfg.invertLY != 0;
    if ((c = ctrl(IDC_INV_RX)))     c->bval = g_cfg.invertRX != 0;
    if ((c = ctrl(IDC_INV_RY)))     c->bval = g_cfg.invertRY != 0;
    if ((c = ctrl(IDC_RUMBLE)))     c->bval = g_cfg.enableRumble != 0;
    if ((c = ctrl(IDC_MAXPADS)))    c->ival = g_cfg.maxControllers;
    if ((c = ctrl(IDC_DRIFT)))          c->bval = g_cfg.driftFix != 0;
    if ((c = ctrl(IDC_DRIFT_DZ)))       c->bval = g_cfg.driftAutoDeadzone != 0;
    if ((c = ctrl(IDC_DRIFT_STRENGTH))) c->ival = std::max(0, std::min(2, g_cfg.driftStrength));
    if ((c = ctrl(IDC_AUTOSTART)))  c->bval = g_prefs.autostart;
    if ((c = ctrl(IDC_TRAYMIN)))    c->bval = g_prefs.minimizeToTray;
    if ((c = ctrl(IDC_TRAYCLOSE)))  c->bval = g_prefs.closeToTray;
    if (g_editDevices)
        SetWindowTextW(g_editDevices, devicesToString(g_cfg.devices).c_str());
    if (g_hwnd)
        InvalidateRect(g_hwnd, NULL, TRUE);
}

static void restoreDefaults() {
    g_cfg = Config();
    g_prefs = AppPrefs();
    engine_apply_config(g_cfg);
    saveConfig(g_cfg);
    savePrefs(g_prefs);
    applyAutostartDecl(false);
    syncUI();
}

//=============================================================================
//  Scroll & viewport
//=============================================================================

static RECT scrollTrackRect() {
    return { g_viewW - S(10) - S(2), 0, g_viewW - S(2), g_viewH };
}

static RECT scrollThumbRect() {
    RECT t = scrollTrackRect();
    int trackH = t.bottom - t.top;
    int thumbH = std::max(S(28), (int)((long long)g_viewH * g_viewH / g_winH));
    if (g_maxScroll <= 0) return { t.left, t.top, t.right, t.top + trackH };
    int y = t.top + (int)((long long)g_scrollY * (trackH - thumbH) / g_maxScroll);
    return { t.left + S(1), y, t.right - S(1), y + thumbH };
}

static void updateScrollExtent() {
    g_maxScroll = std::max(0, g_winH - g_viewH);
    if (g_scrollY > g_maxScroll) g_scrollY = g_maxScroll;
    if (g_scrollY < 0) g_scrollY = 0;
    g_offX = std::max(0, (g_viewW - g_winW) / 2);
}

static void setScroll(int y) {
    if (y < 0) y = 0;
    if (y > g_maxScroll) y = g_maxScroll;
    if (y == g_scrollY) return;
    g_scrollY = y;
    updateEditPos();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void updateEditPos() {
    if (!g_editDevices) return;
    int x = g_rcEdit.left + g_offX;
    int y = g_rcEdit.top - g_scrollY;
    int w = g_rcEdit.right - g_rcEdit.left;
    int h = g_rcEdit.bottom - g_rcEdit.top;
    bool visible = (y + h > 0) && (y < g_viewH) && (x + w > 0) && (x < g_viewW);
    if (visible) {
        ShowWindow(g_editDevices, SW_SHOW);
        SetWindowPos(g_editDevices, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        ShowWindow(g_editDevices, SW_HIDE);
    }
}

//=============================================================================
//  Tray
//=============================================================================

static void addTrayIcon() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    static const GUID trayGuid = { 0x9c1a8d3f, 0x5e44, 0x4f7a, { 0x9b, 0x2c, 0xd1, 0xe4, 0x77, 0x82, 0x13, 0xab } };
    g_nid.guidItem = trayGuid;
    wcscpy(g_nid.szTip, L"SwitchProXInput");
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (g_trayAdded)
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void hideToTray() {
    if (!g_trayAdded) return;          // nowhere to hide to
    ShowWindow(g_hwnd, SW_HIDE);
    g_hidden = true;
    if (!g_balloonShown) {
        g_nid.uFlags = NIF_INFO;
        wcscpy(g_nid.szInfoTitle, L"SwitchProXInput");
        wcscpy(g_nid.szInfo, L"Still running in the system tray. Click the icon to reopen.");
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
        g_balloonShown = true;
    }
}

static void showFromTray() {
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    g_hidden = false;
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void showTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, TRAY_MENU_OPEN, L"Open SwitchProXInput");
    AppendMenuW(m, MF_STRING, TRAY_MENU_HIDE, L"Hide to tray");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, TRAY_MENU_EXIT, L"Exit");
    SetForegroundWindow(g_hwnd);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

//=============================================================================
//  Combo popup
//=============================================================================

static void closePopup() {
    if (g_popup) {
        DestroyWindow(g_popup);
        g_popup = NULL;
        g_popupCtrl = -1;
        g_popupHover = -1;
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
}

static void openCombo(int id) {
    if (g_popup && g_popupCtrl == id) { closePopup(); return; }
    if (g_popup) closePopup();

    Ctrl* c = ctrl(id);
    if (!c) return;
    int n = (int)c->options.size();
    int w = c->rc.right - c->rc.left;
    int ih = S(24);
    int h = n * ih + S(8);

    // account for scroll / centering offsets
    POINT pt = { c->rc.left + g_offX, c->rc.bottom - g_scrollY + S(2) };
    ClientToScreen(g_hwnd, &pt);

    g_popupCtrl = id;
    g_popupHover = -1;
    g_popup = CreateWindowExW(0, kPopupClass, L"", WS_POPUP | WS_BORDER,
                              pt.x, pt.y, w, h, g_hwnd, NULL, g_hInst, NULL);
    SetWindowPos(g_popup, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_popup, SW_SHOWNOACTIVATE);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static LRESULT CALLBACK popupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        fillRectC(hdc, rc, COL_CARD2);
        Ctrl* c = ctrl(g_popupCtrl);
        int n = c ? (int)c->options.size() : 0;
        int ih = S(24);
        for (int i = 0; i < n; ++i) {
            RECT r = { S(4), S(4) + i * ih, rc.right - S(4), S(4) + (i + 1) * ih };
            if (g_popupHover == i) fillRectC(hdc, r, COL_HOVITEM);
            drawTextC(hdc, { r.left + S(8), r.top, r.right - S(8), r.bottom },
                      c->options[i].c_str(), g_fText, COL_TEXT,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (c->ival == i)
                drawDot(hdc, r.left + S(11), (r.top + r.bottom) / 2, S(3), COL_ACCENT);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int ih = S(24);
        int idx = (pt.y - S(4)) / ih;
        if (idx < 0) idx = 0;
        Ctrl* c = ctrl(g_popupCtrl);
        int n = c ? (int)c->options.size() : 0;
        if (n == 0) return 0;
        if (idx >= n) idx = n - 1;
        if (idx != g_popupHover) {
            g_popupHover = idx;
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        Ctrl* c = ctrl(g_popupCtrl);
        if (c && g_popupHover >= 0 && g_popupHover < (int)c->options.size()) {
            c->ival = g_popupHover;
            applySetting(c->id);
        }
        closePopup();
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) closePopup();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

//=============================================================================
//  Painting (content is painted at its natural size; the window blits the
//  visible part and overlays the scrollbar)
//=============================================================================

static void paintStatus(HDC hdc) {
    fillRound(hdc, g_rcStatus, S(10), COL_CARD);

    bool bus = g_status.busConnected;
    bool dll = g_status.dllLoaded;
    COLORREF dotCol = bus ? COL_GREEN : COL_RED;
    int dotY = (g_rcStatus.top + S(6) + g_rcStatus.top + S(26)) / 2;
    drawDot(hdc, g_rcStatus.left + S(16), dotY, S(4), dotCol);

    const wchar_t* state;
    if (bus) state = L"Connected";
    else if (dll) state = L"Driver not connected";
    else state = L"ViGEmClient.dll not found";
    wchar_t line1[128];
    swprintf(line1, 128, L"ViGEmBus driver: %ls", state);
    drawTextC(hdc, { g_rcStatus.left + S(30), g_rcStatus.top + S(6),
                     g_rcStatus.right - S(90), g_rcStatus.top + S(26) },
              line1, g_fText, bus ? COL_GREEN : COL_RED,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (!bus) {
        bool hot = g_hotId == HOT_RETRY;
        fillRound(hdc, g_rcBtnRetry, S(12), hot ? COL_ACCENT2 : COL_ACCENT);
        drawTextC(hdc, g_rcBtnRetry, L"Retry", g_fText, RGB(255,255,255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT l2 = { g_rcStatus.left + S(30), g_rcStatus.top + S(32),
                g_rcStatus.right - S(16), g_rcStatus.bottom - S(8) };
    if (!bus) {
        drawTextC(hdc, l2, L"Install ViGEmBus (github.com/nefarius/ViGEmBus/releases), then press Retry.",
                  g_fSmall, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    int connected = 0;
    for (int i = 0; i < g_status.slotCount; ++i)
        if (g_status.slots[i].connected) connected++;
    if (connected == 0) {
        drawTextC(hdc, l2, L"No controllers connected \u2014 plug in your Pro Controller via USB.",
                  g_fSmall, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int x = l2.left;
    wchar_t buf[64];
    for (int i = 0; i < g_status.slotCount; ++i) {
        if (!g_status.slots[i].connected) continue;
        int bat = g_status.slots[i].battery;
        if (bat >= 0 && bat <= 8) swprintf(buf, 64, L"Pad %d \u00B7 battery %d/8   ", i + 1, bat);
        else swprintf(buf, 64, L"Pad %d \u00B7 battery \u2014   ", i + 1);
        int w = S(160);
        drawTextC(hdc, { x, l2.top, x + w, l2.bottom }, buf, g_fSmall, COL_TEXT,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += w;
        if (x > l2.right - S(60)) break;
    }
}

static void paintToggle(HDC hdc, const Ctrl& c) {
    bool hot = g_hotId == c.id;
    drawTextC(hdc, c.rcLabel, c.label.c_str(), g_fText, COL_TEXT,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT pill = pillRect(c);
    fillRound(hdc, pill, S(10), c.bval ? COL_ACCENT : COL_TRACK);
    if (hot) strokeRound(hdc, pill, S(10), COL_BORDER);
    int d = pill.bottom - pill.top - S(4);
    int cy = (pill.top + pill.bottom) / 2;
    int cx = c.bval ? pill.right - S(3) - d / 2 : pill.left + S(3) + d / 2;
    fillRound(hdc, { cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2 }, d, COL_KNOB);
}

static void paintMiniToggle(HDC hdc, const Ctrl& c) {
    bool hot = g_hotId == c.id;
    drawTextC(hdc, c.rcLabel, c.label.c_str(), g_fSmall, COL_DIM,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT pill = c.rc;
    fillRound(hdc, pill, S(7), c.bval ? COL_ACCENT : COL_TRACK);
    if (hot) strokeRound(hdc, pill, S(7), COL_BORDER);
    int d = pill.bottom - pill.top - S(3);
    int cy = (pill.top + pill.bottom) / 2;
    int cx = c.bval ? pill.right - S(2) - d / 2 : pill.left + S(2) + d / 2;
    fillRound(hdc, { cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2 }, d, COL_KNOB);
}

static void paintSlider(HDC hdc, const Ctrl& c) {
    drawTextC(hdc, { c.rcLabel.left, c.rc.top, c.rcLabel.right, c.rc.top + S(18) },
              c.label.c_str(), g_fText, COL_TEXT,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t val[16];
    if (c.id == IDC_LDZ || c.id == IDC_RDZ) swprintf(val, 16, L"%d%%", c.ival);
    else swprintf(val, 16, L"%d", c.ival);
    RECT vr = { c.rc.right - S(56), c.rc.top, c.rc.right, c.rc.top + S(18) };
    drawTextC(hdc, vr, val, g_fSmall, c.dragging ? COL_ACCENT2 : COL_DIM,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT tr = trackRect(c);
    fillRound(hdc, tr, S(2), COL_TRACK);
    float t = (c.maxv > c.minv) ? (float)(c.ival - c.minv) / (float)(c.maxv - c.minv) : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int fx = tr.left + (int)(t * (tr.right - tr.left));
    if (fx > tr.left + S(2))
        fillRound(hdc, { tr.left, tr.top, fx, tr.bottom }, S(2), COL_ACCENT);
    int cy = (tr.top + tr.bottom) / 2;
    int r = S(6);
    fillRound(hdc, { fx - r, cy - r, fx + r, cy + r }, r * 2, COL_KNOB);
}

static void paintCombo(HDC hdc, const Ctrl& c) {
    drawTextC(hdc, c.rcLabel, c.label.c_str(), g_fText, COL_TEXT,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    bool open = g_popupCtrl == c.id;
    bool hot = g_hotId == c.id && !open;
    fillRound(hdc, c.rc, S(6), open ? COL_CARD2 : COL_INSET);
    if (open || hot) strokeRound(hdc, c.rc, S(6), open ? COL_ACCENT : COL_BORDER);

    const wchar_t* val = L"";
    if (c.ival >= 0 && c.ival < (int)c.options.size()) val = c.options[c.ival].c_str();
    drawTextC(hdc, { c.rc.left + S(8), c.rc.top, c.rc.right - S(22), c.rc.bottom },
              val, g_fText, COL_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int cy = (c.rc.top + c.rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, S(1), COL_DIM);
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, c.rc.right - S(16), cy - S(3), NULL);
    LineTo(hdc, c.rc.right - S(10), cy + S(3));
    LineTo(hdc, c.rc.right - S(4), cy - S(3));
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void paintStepper(HDC hdc, const Ctrl& c) {
    drawTextC(hdc, c.rcLabel, c.label.c_str(), g_fText, COL_TEXT,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT m = stepperMinusRect(c), p = stepperPlusRect(c), v = stepperValueRect(c);
    fillRound(hdc, m, S(6), g_hotId == c.id ? COL_CARD2 : COL_INSET);
    fillRound(hdc, p, S(6), g_hotId == c.id ? COL_CARD2 : COL_INSET);
    int cy = (m.top + m.bottom) / 2;
    fillRectC(hdc, { m.left + S(7), cy - S(1), m.right - S(7), cy + S(1) }, COL_TEXT);
    cy = (p.top + p.bottom) / 2;
    fillRectC(hdc, { p.left + S(7), cy - S(1), p.right - S(7), cy + S(1) }, COL_TEXT);
    fillRectC(hdc, { p.left + S(11), p.top + S(4), p.right - S(11), p.bottom - S(4) }, COL_TEXT);
    wchar_t buf[8];
    swprintf(buf, 8, L"%d", c.ival);
    drawTextC(hdc, v, buf, g_fText, COL_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Draw a small-font line made of two differently colored parts.
static void drawTwoPart(HDC hdc, const RECT& r, const wchar_t* p1, COLORREF c1,
                        const wchar_t* p2, COLORREF c2) {
    RECT rc1 = r;
    drawTextC(hdc, rc1, p1, g_fSmall, c1,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
    drawTextC(hdc, r, p1, g_fSmall, c1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (p2[0]) {
        RECT r2 = { rc1.right + S(4), r.top, r.right, r.bottom };
        drawTextC(hdc, r2, p2, g_fSmall, c2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// Human-readable pull direction from the resting offset (raw space:
// +X = right, +Y = up). Returns false when no axis is clearly biased.
static bool driftDirection(int offX, int offY, wchar_t* dir, size_t n) {
    const int T = 25;                     // matches the detection threshold
    bool west = offX < -T, east = offX > T;
    bool north = offY > T, south = offY < -T;
    const wchar_t* ns = north ? L"up" : south ? L"down" : L"";
    const wchar_t* ew = east ? L"right" : west ? L"left" : L"";
    if (!ns[0] && !ew[0]) return false;
    if (ns[0] && ew[0]) swprintf(dir, n, L"%ls-%ls", ns, ew);
    else if (ew[0])     swprintf(dir, n, L"%ls", ew);
    else                swprintf(dir, n, L"%ls", ns);
    return true;
}

// Compose the status line for one stick: "<label>: <what was detected>"
// plus "<what the fix is doing about it>".
static void driftLineParts(const wchar_t* label, const DriftView& v,
                           wchar_t* prefix, size_t pn, wchar_t* suffix, size_t sn) {
    prefix[0] = 0;
    suffix[0] = 0;

    if (v.calibrating) {
        swprintf(prefix, pn, L"%ls: calibrating \u2014 don't touch the sticks\u2026", label);
        return;
    }
    if (!v.calibrated) {
        swprintf(prefix, pn, L"%ls: learning rest position \u2014 release the sticks\u2026", label);
        return;
    }

    wchar_t dir[16];
    switch (v.type) {
    case DRIFT_OFFSET:
        if (driftDirection(v.offX, v.offY, dir, 16))
            swprintf(prefix, pn, L"%ls: drifts %ls (X %+d, Y %+d)", label, dir, v.offX, v.offY);
        else
            swprintf(prefix, pn, L"%ls: center offset (X %+d, Y %+d)", label, v.offX, v.offY);
        break;
    case DRIFT_JITTER:
        swprintf(prefix, pn, L"%ls: noisy stick (\u00B1%d)", label, v.noise);
        break;
    case DRIFT_BOTH:
        if (driftDirection(v.offX, v.offY, dir, 16))
            swprintf(prefix, pn, L"%ls: drifts %ls (X %+d, Y %+d) + noise (\u00B1%d)",
                     label, dir, v.offX, v.offY, v.noise);
        else
            swprintf(prefix, pn, L"%ls: offset (X %+d, Y %+d) + noise (\u00B1%d)",
                     label, v.offX, v.offY, v.noise);
        break;
    default:
        swprintf(prefix, pn, L"%ls: clean \u2014 no drift detected", label);
        break;
    }

    if (!v.enabled) {
        if (v.type != DRIFT_NONE) swprintf(suffix, sn, L"\u00B7 correction off");
        return;
    }
    switch (v.type) {
    case DRIFT_OFFSET:
        swprintf(suffix, sn, L"\u00B7 auto-centered");
        break;
    case DRIFT_JITTER:
        if (v.autoDzPct > 0) swprintf(suffix, sn, L"\u00B7 auto deadzone %d%%", v.autoDzPct);
        else swprintf(suffix, sn, L"\u00B7 within deadzone");
        break;
    case DRIFT_BOTH:
        if (v.autoDzPct > 0) swprintf(suffix, sn, L"\u00B7 auto-centered, auto-DZ %d%%", v.autoDzPct);
        else swprintf(suffix, sn, L"\u00B7 auto-centered");
        break;
    default:
        swprintf(suffix, sn, L"\u00B7 no fix needed");
        break;
    }
}

static void paintDriftCard(HDC hdc) {
    // Calibrate button
    bool calib = g_driftValid && (g_driftL.calibrating || g_driftR.calibrating);
    bool hot = g_hotId == HOT_CALIB && !calib;
    fillRound(hdc, g_rcBtnCalib, S(12),
              calib ? COL_CARD2 : (hot ? COL_ACCENT2 : COL_ACCENT));
    drawTextC(hdc, g_rcBtnCalib, calib ? L"Calibrating\u2026" : L"Calibrate now",
              g_fSmall, calib ? COL_DIM : RGB(255,255,255),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawTextC(hdc, g_rcCalibHint, L"Best with the sticks untouched",
              g_fSmall, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Status lines
    if (!g_driftValid) {
        drawTextC(hdc, g_rcDriftLineL, L"Plug in a controller \u2014 drift status appears here.",
                  g_fSmall, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    wchar_t p1[96], p2[64];

    if (g_driftL.calibrating || g_driftR.calibrating ||
        (!g_driftL.calibrated && !g_driftR.calibrated)) {
        // Both sticks share one message while (re)calibrating.
        driftLineParts(L"L", g_driftL, p1, 96, p2, 64);
        drawTextC(hdc, g_rcDriftLineL, p1, g_fSmall, COL_ACCENT2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    driftLineParts(L"L", g_driftL, p1, 96, p2, 64);
    drawTwoPart(hdc, g_rcDriftLineL, p1,
                g_driftL.type == DRIFT_NONE ? COL_GREEN : COL_WARN,
                p2, g_driftL.enabled || g_driftL.type == DRIFT_NONE ? COL_GREEN : COL_WARN);
    driftLineParts(L"R", g_driftR, p1, 96, p2, 64);
    drawTwoPart(hdc, g_rcDriftLineR, p1,
                g_driftR.type == DRIFT_NONE ? COL_GREEN : COL_WARN,
                p2, g_driftR.enabled || g_driftR.type == DRIFT_NONE ? COL_GREEN : COL_WARN);
}

static void paintBottomBar(HDC hdc) {
    bool hotD = g_hotId == HOT_DEFAULTS;
    fillRound(hdc, g_rcBtnDefaults, S(14), hotD ? COL_CARD2 : COL_CARD);
    strokeRound(hdc, g_rcBtnDefaults, S(14), COL_BORDER);
    drawTextC(hdc, g_rcBtnDefaults, L"Restore defaults", g_fText, hotD ? COL_TEXT : COL_DIM,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    bool hotH = g_hotId == HOT_HIDE;
    fillRound(hdc, g_rcBtnHide, S(14), hotH ? COL_ACCENT2 : COL_ACCENT);
    drawTextC(hdc, g_rcBtnHide, L"Hide to tray", g_fText, RGB(255,255,255),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    drawTextC(hdc, { g_rcBtnDefaults.right + S(12), g_rcBtnDefaults.top,
                     g_rcBtnHide.left - S(12), g_rcBtnDefaults.bottom },
              L"v1.2.1", g_fSmall, COL_DIM,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paintAll(HDC hdc) {
    fillRectC(hdc, { 0, 0, g_winW, g_winH }, COL_BG);
    paintStatus(hdc);

    for (auto& s : g_secs) {
        fillRound(hdc, s.rc, S(10), COL_CARD);
        drawTextC(hdc, { s.rc.left + S(10), s.rc.top + S(8), s.rc.right - S(10), s.rc.top + S(26) },
                  s.title.c_str(), g_fSection, COL_DIM,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (g_invLabelRect.top > 0)
        drawTextC(hdc, g_invLabelRect, L"Invert axes", g_fText, COL_TEXT,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (auto& c : g_ctrls) {
        switch (c.type) {
        case CtrlType::Toggle:     paintToggle(hdc, c); break;
        case CtrlType::MiniToggle: paintMiniToggle(hdc, c); break;
        case CtrlType::Slider:     paintSlider(hdc, c); break;
        case CtrlType::Combo:      paintCombo(hdc, c); break;
        case CtrlType::Stepper:    paintStepper(hdc, c); break;
        }
    }

    if (g_rcEdit.top > 0) {
        RECT hint = { g_rcEdit.left, g_rcEdit.bottom + S(3), g_rcEdit.right, g_rcEdit.bottom + S(15) };
        drawTextC(hdc, hint, L"VID:PID hex pairs separated by ';'  \u2014  e.g. 057E:2009;057E:200E",
                  g_fSmall, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    paintDriftCard(hdc);
    paintBottomBar(hdc);
}

static void paintScrollbar(HDC hdc) {
    if (g_maxScroll <= 0) return;
    RECT t = scrollTrackRect();
    RECT th = scrollThumbRect();
    fillRectC(hdc, t, COL_TRACK);
    fillRound(hdc, th, S(4), g_sbDragging ? COL_TEXT : COL_DIM);
}

//=============================================================================
//  Hit-testing & input
//=============================================================================

static int hitTest(POINT pt) {
    if (PtInRect(&g_rcBtnDefaults, pt)) return HOT_DEFAULTS;
    if (PtInRect(&g_rcBtnHide, pt)) return HOT_HIDE;
    if (PtInRect(&g_rcBtnCalib, pt)) return HOT_CALIB;
    if (!g_status.busConnected && PtInRect(&g_rcBtnRetry, pt)) return HOT_RETRY;
    for (auto it = g_ctrls.rbegin(); it != g_ctrls.rend(); ++it) {
        const Ctrl& c = *it;
        switch (c.type) {
        case CtrlType::Toggle:
        case CtrlType::Stepper:
        case CtrlType::Slider:
        case CtrlType::MiniToggle:
        case CtrlType::Combo:
            if (PtInRect(&c.rc, pt)) return c.id;
            break;
        }
    }
    return -1;
}

static void toggle(int id) {
    Ctrl* c = ctrl(id);
    if (!c) return;
    c->bval = !c->bval;
    applySetting(id);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void sliderFromX(Ctrl& c, int x) {
    RECT tr = trackRect(c);
    float t = (float)(x - tr.left) / (float)(tr.right - tr.left);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    c.ival = c.minv + (int)(t * (c.maxv - c.minv) + 0.5f);
}

static void scrollbarFromY(int y) {
    RECT t = scrollTrackRect();
    int trackH = t.bottom - t.top;
    int thumbH = scrollThumbRect().bottom - scrollThumbRect().top;
    if (trackH <= thumbH || g_maxScroll <= 0) { setScroll(0); return; }
    int ty = y - t.top - thumbH / 2;
    if (ty < 0) ty = 0;
    if (ty > trackH - thumbH) ty = trackH - thumbH;
    setScroll((int)((long long)ty * g_maxScroll / (trackH - thumbH)));
}

static void onLButtonDown(POINT pt) {
    // scrollbar first (viewport coords)
    RECT sbt = scrollTrackRect();
    if (g_maxScroll > 0 && PtInRect(&sbt, pt)) {
        g_sbDragging = true;
        SetCapture(g_hwnd);
        scrollbarFromY(pt.y);
        return;
    }

    POINT cpt = { pt.x - g_offX, pt.y + g_scrollY };   // content coords
    int id = hitTest(cpt);

    if (g_popup) {
        int owning = g_popupCtrl;
        closePopup();
        if (id == owning) return;
    }

    if (id == HOT_DEFAULTS) { restoreDefaults(); return; }
    if (id == HOT_HIDE) { hideToTray(); return; }
    if (id == HOT_CALIB) {
        engine_calibrate_sticks();
        refreshStatus();
        InvalidateRect(g_hwnd, NULL, TRUE);
        return;
    }
    if (id == HOT_RETRY) {
        engine_try_connect_bus();
        refreshStatus();
        InvalidateRect(g_hwnd, NULL, TRUE);
        return;
    }

    Ctrl* c = ctrl(id);
    if (!c) return;
    switch (c->type) {
    case CtrlType::Toggle: {
        RECT pill = pillRect(*c);
        if (PtInRect(&pill, cpt)) toggle(c->id);
        break;
    }
    case CtrlType::MiniToggle:
        toggle(c->id);
        break;
    case CtrlType::Combo:
        openCombo(c->id);
        break;
    case CtrlType::Stepper: {
        RECT m = stepperMinusRect(*c), p = stepperPlusRect(*c);
        if (PtInRect(&m, cpt) && c->ival > c->minv) c->ival--;
        else if (PtInRect(&p, cpt) && c->ival < c->maxv) c->ival++;
        else return;
        applySetting(c->id);
        InvalidateRect(g_hwnd, NULL, TRUE);
        break;
    }
    case CtrlType::Slider:
        c->dragging = true;
        SetCapture(g_hwnd);
        sliderFromX(*c, cpt.x);
        g_cfgApplyFromSlider(c);
        InvalidateRect(g_hwnd, NULL, TRUE);
        break;
    }
}

static void onLButtonUp() {
    if (g_sbDragging) {
        g_sbDragging = false;
        ReleaseCapture();
        InvalidateRect(g_hwnd, NULL, TRUE);
        return;
    }
    for (auto& c : g_ctrls) {
        if (c.dragging) {
            c.dragging = false;
            ReleaseCapture();
            saveConfig(g_cfg);
            InvalidateRect(g_hwnd, NULL, TRUE);
            return;
        }
    }
}

static void onMouseMove(POINT pt) {
    if (g_sbDragging) {
        scrollbarFromY(pt.y);
        return;
    }
    for (auto& c : g_ctrls) {
        if (c.dragging) {
            sliderFromX(c, pt.x - g_offX);
            g_cfgApplyFromSlider(&c);
            InvalidateRect(g_hwnd, NULL, TRUE);
            return;
        }
    }
    int id = hitTest({ pt.x - g_offX, pt.y + g_scrollY });
    if (id != g_hotId) {
        g_hotId = id;
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
}

//=============================================================================
//  Engine notifier bridge (called on engine threads)
//=============================================================================

void gui_notify_bridge(int evt, int a, int b, int c) {
    (void)c;
    if (!g_hwnd) return;
    PostMessageW(g_hwnd, WM_APP_ENGINE_EVENT, (WPARAM)evt, MAKELPARAM(a, b));
}

//=============================================================================
//  Dark title bar (DwmSetWindowAttribute)
//=============================================================================

static void enableDarkTitleBar(HWND h) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI* pfnDwmSet)(HWND, DWORD, LPCVOID, DWORD);
    pfnDwmSet fn = (pfnDwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        BOOL dark = TRUE;
        fn(h, 20, &dark, sizeof(dark));   // DWMWA_USE_IMMERSIVE_DARK_MODE (20H1+)
        fn(h, 19, &dark, sizeof(dark));   // 1809/1903
    }
    FreeLibrary(dwm);
}

//=============================================================================
//  Window procedures
//=============================================================================

static WNDPROC g_editOldProc = NULL;

static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        SetFocus(g_hwnd);
        return 0;
    }
    if (m == WM_CHAR && w == 0x0D) return 0;   // swallow enter beep
    return CallWindowProcW(g_editOldProc, h, m, w, l);
}

static void quitApp() {
    if (!g_hwnd) return;
    engine_stop();
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
    HWND h = g_hwnd;
    g_hwnd = NULL;
    DestroyWindow(h);
}

static void doClose() {
    if (g_prefs.closeToTray && g_trayAdded) hideToTray();
    else quitApp();
}

static LRESULT CALLBACK mainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        // background
        fillRectC(hdc, { 0, 0, g_viewW, g_viewH }, COL_BG);
        // content (drawn at natural size, blitted with scroll offset)
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, g_winW, g_winH);
        HGDIOBJ ob = SelectObject(mem, bmp);
        paintAll(mem);
        BitBlt(hdc, g_offX, -g_scrollY, g_winW, g_winH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        // scrollbar overlay
        paintScrollbar(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (w != SIZE_MINIMIZED) {
            g_viewW = LOWORD(l);
            g_viewH = HIWORD(l);
            updateScrollExtent();
            updateEditPos();
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        setScroll(g_scrollY - delta * S(48) / WHEEL_DELTA);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        int waW = mi.rcWork.right - mi.rcWork.left;
        mmi->ptMinTrackSize.x = std::min(g_winW, (int)waW);
        mmi->ptMinTrackSize.y = S(240);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        onLButtonDown(pt);
        return 0;
    }
    case WM_LBUTTONUP: {
        onLButtonUp();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_sbDragging) {
            g_sbDragging = false;
            InvalidateRect(h, NULL, TRUE);
        }
        for (auto& c : g_ctrls) c.dragging = false;
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        onMouseMove(pt);
        if (!g_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            g_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hotId = -1;
        InvalidateRect(h, NULL, TRUE);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) closePopup();
        return 0;

    case WM_SYSCOMMAND:
        if (w == SC_MINIMIZE && g_prefs.minimizeToTray && g_trayAdded) {
            hideToTray();
            return 0;
        }
        break;

    case WM_COMMAND: {
        if (l == 0 && HIWORD(w) == 0) {
            switch (LOWORD(w)) {
            case TRAY_MENU_OPEN: showFromTray(); break;
            case TRAY_MENU_HIDE: hideToTray(); break;
            case TRAY_MENU_EXIT: quitApp(); break;
            }
            return 0;
        }
        if (LOWORD(w) == IDC_DEVICES && HIWORD(w) == EN_KILLFOCUS) {
            commitDevices();
            return 0;
        }
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, COL_TEXT);
        SetBkColor(dc, COL_INSET);
        if (!g_brushEdit) g_brushEdit = CreateSolidBrush(COL_INSET);
        return (LRESULT)g_brushEdit;
    }

    case WM_DPICHANGED: {
        RECT* suggest = (RECT*)l;
        g_scale = (float)HIWORD(w) / 96.0f;
        if (g_scale < 0.5f) g_scale = 0.5f;
        if (g_scale > 4.0f) g_scale = 4.0f;
        makeFonts();
        relayout();
        g_viewW = suggest->right - suggest->left;
        g_viewH = suggest->bottom - suggest->top;
        SetWindowPos(h, NULL, suggest->left, suggest->top,
                     g_viewW, g_viewH, SWP_NOZORDER | SWP_NOACTIVATE);
        updateScrollExtent();
        updateEditPos();
        if (g_editDevices)
            SendMessageW(g_editDevices, WM_SETFONT, (WPARAM)g_fEdit, TRUE);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }

    case WM_APP_ENGINE_EVENT:
        refreshStatus();
        InvalidateRect(h, NULL, TRUE);
        return 0;

    case WM_TIMER:                          // drift statistics evolve over time
        if (w == 1) {
            refreshStatus();
            if (!g_hidden) InvalidateRect(h, NULL, TRUE);
        }
        return 0;

    case WM_APP_TRAY:
        if (LOWORD(l) == WM_LBUTTONDBLCLK || LOWORD(l) == WM_LBUTTONUP) {
            showFromTray();
        } else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            showTrayMenu();
        }
        return 0;

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) SendMessageW(h, WM_CLOSE, 0, 0);
        return 0;

    case WM_CLOSE:
        doClose();
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        if (g_brushEdit) { DeleteObject(g_brushEdit); g_brushEdit = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

//=============================================================================
//  Entry
//=============================================================================

const wchar_t* gui_class_name() { return kMainClass; }

int gui_run(HINSTANCE hInst, bool startHidden) {
    g_hInst = hInst;

    typedef UINT (WINAPI* pfnGetDpiForWindow)(HWND);
    pfnGetDpiForWindow pGetDpi = (pfnGetDpiForWindow)
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");

    // config
    g_cfg = loadConfig();
    g_prefs = loadPrefs();
    bool regOn = readAutostart();
    if (regOn != g_prefs.autostart) {
        g_prefs.autostart = regOn;
        savePrefs(g_prefs);
    }
    g_status = engine_get_status();

    makeFonts();
    relayout();

    // register classes
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = popupProc;
    wc.hIcon = NULL;
    wc.hIconSm = NULL;
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);

    // Standard window frame: minimize / maximize / close / resize.
    // Client size = content size, clamped to the monitor's work area.
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT desired = { 0, 0, g_winW, g_winH };
    AdjustWindowRectEx(&desired, style, FALSE, 0);
    int winW = desired.right - desired.left;
    int winH = desired.bottom - desired.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    winW = std::min(winW, (int)(wa.right - wa.left));
    winH = std::min(winH, (int)(wa.bottom - wa.top));
    int posX = wa.left + std::max(0, (int)((wa.right - wa.left) - winW) / 2);
    int posY = wa.top + std::max(0, (int)((wa.bottom - wa.top) - winH) / 2);

    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kMainClass, L"SwitchProXInput v1.2.1",
                             style, posX, posY, winW, winH,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    // viewport = client area
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    g_viewW = cr.right;
    g_viewH = cr.bottom;

    // apply real DPI if available
    if (pGetDpi) {
        UINT dpi = pGetDpi(g_hwnd);
        if (dpi != 96) {
            g_scale = (float)dpi / 96.0f;
            makeFonts();
            relayout();
            // re-clamp window to the monitor's work area at the new scale
            MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            RECT d2 = { 0, 0, g_winW, g_winH };
            AdjustWindowRectEx(&d2, style, FALSE, 0);
            int w2 = std::min(d2.right - d2.left, mi.rcWork.right - mi.rcWork.left);
            int h2 = std::min(d2.bottom - d2.top, mi.rcWork.bottom - mi.rcWork.top);
            SetWindowPos(g_hwnd, NULL, 0, 0, w2, h2, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetClientRect(g_hwnd, &cr);
            g_viewW = cr.right;
            g_viewH = cr.bottom;
        }
    }

    updateScrollExtent();

    // dark title bar (Win10/11)
    enableDarkTitleBar(g_hwnd);

    // devices edit control
    g_editDevices = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                    0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_DEVICES, hInst, NULL);
    if (g_editDevices) {
        SendMessageW(g_editDevices, WM_SETFONT, (WPARAM)g_fEdit, TRUE);
        SetWindowTextW(g_editDevices, devicesToString(g_cfg.devices).c_str());
        g_editOldProc = (WNDPROC)SetWindowLongPtrW(g_editDevices, GWLP_WNDPROC, (LONG_PTR)editProc);
        updateEditPos();
    }

    syncUI();
    addTrayIcon();
    refreshStatus();
    SetTimer(g_hwnd, 1, 750, NULL);      // keep the drift status line fresh

    if (!startHidden) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_fTitle) DeleteObject(g_fTitle);
    if (g_fSection) DeleteObject(g_fSection);
    if (g_fText) DeleteObject(g_fText);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fEdit) DeleteObject(g_fEdit);
    return 0;
}
