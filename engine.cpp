//=============================================================================
//  engine.cpp - Switch Pro Controller -> ViGEm/XInput engine.
//  Reads the Pro Controller through the raw Windows HID stack and feeds a
//  virtual Xbox 360 pad through ViGEmBus (ViGEmClient.dll, MIT).
//=============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <initguid.h>

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <utility>

#include "engine.h"
#include "config.h"
#include "drift.h"

// {4D1E55B2-F16F-11CF-88CB-001111000030} - GUID_DEVINTERFACE_HID
DEFINE_GUID(GUID_DEVINTERFACE_HID,
    0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);

//=============================================================================
//  ViGEmClient.dll - manual declarations (official API, MIT). Loaded
//  dynamically at runtime so there is no import-lib dependency.
//=============================================================================

typedef void*   PVIGEM_CLIENT;
typedef void*   PVIGEM_TARGET;
typedef ULONG   VIGEM_ERROR;

#define VIGEM_ERROR_NONE                 0x20000000UL
#define VIGEM_ERROR_BUS_NOT_FOUND        0xE0000001UL
#define VIGEM_ERROR_NO_FREE_SLOT         0xE0000002UL
#define VIGEM_ERROR_BUS_ACCESS_FAILED    0xE0000009UL
#define VIGEM_ERROR_INVALID_PARAMETER    0xE0000015UL
#define VIGEM_SUCCESS(x)                 ((x) == VIGEM_ERROR_NONE)

#define EMU_VID  0x045E   // official Xbox 360 controller identity
#define EMU_PID  0x028E

typedef PVIGEM_CLIENT (__cdecl* FN_vigem_alloc)(void);
typedef void          (__cdecl* FN_vigem_free)(PVIGEM_CLIENT);
typedef VIGEM_ERROR   (__cdecl* FN_vigem_connect)(PVIGEM_CLIENT);
typedef void          (__cdecl* FN_vigem_disconnect)(PVIGEM_CLIENT);
typedef PVIGEM_TARGET (__cdecl* FN_vigem_target_x360_alloc)(void);
typedef void          (__cdecl* FN_vigem_target_free)(PVIGEM_TARGET);
typedef VIGEM_ERROR   (__cdecl* FN_vigem_target_add)(PVIGEM_CLIENT, PVIGEM_TARGET);
typedef VIGEM_ERROR   (__cdecl* FN_vigem_target_remove)(PVIGEM_CLIENT, PVIGEM_TARGET);
typedef void          (__cdecl* FN_vigem_target_set_vid)(PVIGEM_TARGET, USHORT);
typedef void          (__cdecl* FN_vigem_target_set_pid)(PVIGEM_TARGET, USHORT);
typedef void          (WINAPI* PFN_VIGEM_X360_NOTIFICATION)(PVIGEM_CLIENT, PVIGEM_TARGET,
                                                            UCHAR, UCHAR, UCHAR);
typedef VIGEM_ERROR   (__cdecl* FN_vigem_target_x360_register_notification)(PVIGEM_CLIENT, PVIGEM_TARGET,
                                                                            PFN_VIGEM_X360_NOTIFICATION);
typedef VIGEM_ERROR   (__cdecl* FN_vigem_target_x360_update)(PVIGEM_CLIENT, PVIGEM_TARGET, XUSB_REPORT);

struct ViGEmApi {
    HMODULE module = NULL;
    FN_vigem_alloc vigem_alloc = NULL;
    FN_vigem_free vigem_free = NULL;
    FN_vigem_connect vigem_connect = NULL;
    FN_vigem_disconnect vigem_disconnect = NULL;
    FN_vigem_target_x360_alloc vigem_target_x360_alloc = NULL;
    FN_vigem_target_free vigem_target_free = NULL;
    FN_vigem_target_add vigem_target_add = NULL;
    FN_vigem_target_remove vigem_target_remove = NULL;
    FN_vigem_target_set_vid vigem_target_set_vid = NULL;
    FN_vigem_target_set_pid vigem_target_set_pid = NULL;
    FN_vigem_target_x360_register_notification vigem_target_x360_register_notification = NULL;
    FN_vigem_target_x360_update vigem_target_x360_update = NULL;
};

//=============================================================================
//  Globals
//=============================================================================

static EngineNotifier g_notifier = NULL;
static ViGEmApi       g_vigem;
static PVIGEM_CLIENT  g_client = NULL;

static volatile LONG  g_running = 0;      // engine running
static volatile LONG  g_rebuilding = 0;   // slots being rebuilt (watchdog pauses)

static CRITICAL_SECTION g_cfgLock;        // guards g_cfg
static CRITICAL_SECTION g_slotsLock;      // guards slot table vs. rumble callback
static Config            g_cfg;           // active config (defaults)

static HANDLE         g_watchdog = NULL;

const int MAX_SLOTS = 4;

// Diagnostics snapshot of one stick's drift correction, written by the
// reader thread on every report and read by the GUI.
struct DriftSnap {
    bool calibrating;
    bool calibrated;
    bool atRest;
    int  type;
    int  offX, offY;
    int  noise;
    int  autoDzPct;     // adaptive deadzone the engine actually applied (%)
};

struct Slot {
    CRITICAL_SECTION cs;              // lifetime: engine_start .. engine_stop
    PVIGEM_TARGET target;
    HANDLE        thread;
    HANDLE        device;             // guarded by cs
    std::wstring  path;               // only touched by watchdog / rebuild
    volatile LONG alive;
    volatile LONG stop;
    ULONGLONG     assignedAt;
    volatile LONG battery;
    int           index;
    DWORD         reportLen;
    DWORD         outputLen;
    DriftSnap     driftSnap[2];       // guarded by cs (left, right)
    volatile LONG calibReq;           // set by the GUI, consumed by the reader
};

static Slot g_slots[MAX_SLOTS];
static int  g_slotCount = 0;

static void notify(int evt, int a, int b, int c) {
    if (g_notifier) g_notifier(evt, a, b, c);
}

static Config configSnapshot() {
    Config c;
    EnterCriticalSection(&g_cfgLock);
    c = g_cfg;
    LeaveCriticalSection(&g_cfgLock);
    return c;
}

//=============================================================================
//  ViGEm loading
//=============================================================================

static const wchar_t* vigemErrorText(VIGEM_ERROR e) {
    switch (e) {
    case VIGEM_ERROR_BUS_NOT_FOUND:     return L"ViGEmBus driver not found";
    case VIGEM_ERROR_NO_FREE_SLOT:      return L"no free device slot on the bus";
    case VIGEM_ERROR_BUS_ACCESS_FAILED: return L"bus access failed";
    case VIGEM_ERROR_INVALID_PARAMETER: return L"invalid parameter";
    default:                            return L"unknown error";
    }
}

static bool loadViGEm(ViGEmApi& api, const std::wstring& exeDir) {
    api = ViGEmApi();
    SetDllDirectoryW(exeDir.c_str());
    api.module = LoadLibraryW(L"ViGEmClient.dll");
    if (!api.module) {
        api.module = LoadLibraryW((exeDir + L"ViGEmClient.dll").c_str());
    }
    if (!api.module) {
        api.module = LoadLibraryW(L"C:\\Program Files\\Nefarius Software Solutions\\ViGEm Bus Driver\\ViGEmClient.dll");
    }
    if (!api.module) {
        api.module = LoadLibraryW(L"C:\\Windows\\System32\\ViGEmClient.dll");
    }
    if (!api.module) return false;

#define LOAD(fn) do { FARPROC _p = GetProcAddress(api.module, #fn); \
        if (!_p) return false; \
        memcpy(&api.fn, &_p, sizeof(_p)); } while (0)
    LOAD(vigem_alloc);
    LOAD(vigem_free);
    LOAD(vigem_connect);
    LOAD(vigem_disconnect);
    LOAD(vigem_target_x360_alloc);
    LOAD(vigem_target_free);
    LOAD(vigem_target_add);
    LOAD(vigem_target_remove);
    LOAD(vigem_target_set_vid);
    LOAD(vigem_target_set_pid);
    LOAD(vigem_target_x360_register_notification);
    LOAD(vigem_target_x360_update);
#undef LOAD
    return true;
}

//=============================================================================
//  Rumble passthrough (USB output report 0x10, Switch HD-rumble encoding)
//=============================================================================

static BYTE encodeRumbleAmp(float amp) {
    if (amp <= 0.0f) return 0;
    if (amp > 1.0f) amp = 1.0f;
    float v = (((log2f(amp * 1000.0f) * 32.0f) - 0x60) * 2.0f) - 0xf6;
    return (BYTE)lroundf(v);
}

static void buildRumblePacket(UCHAR largeMotor, UCHAR smallMotor, BYTE packet[4]) {
    float amp = (float)std::max(largeMotor, smallMotor) / 255.0f;
    if (amp <= 0.01f) {
        packet[0] = 0x00; packet[1] = 0x01; packet[2] = 0x40; packet[3] = 0x40;
        return;
    }
    USHORT hf = 0x0100;
    BYTE   lf = 0x40;
    BYTE hfAmp = encodeRumbleAmp(amp);
    hfAmp -= (BYTE)(hfAmp % 2);

    USHORT lfAmp = (USHORT)lroundf((float)encodeRumbleAmp(amp) * 0.5f);
    BYTE parity = (BYTE)(lfAmp % 2);
    if (parity) lfAmp--;
    lfAmp >>= 1;
    lfAmp += 0x40;
    if (parity) lfAmp |= 0x8000;

    packet[0] = (BYTE)(hf & 0xFF);
    packet[1] = (BYTE)(((hf >> 8) & 0xFF) + hfAmp);
    packet[2] = (BYTE)(((lfAmp >> 8) & 0xFF) + lf);
    packet[3] = (BYTE)(lfAmp & 0xFF);
}

static void WINAPI x360Notification(PVIGEM_CLIENT, PVIGEM_TARGET target,
                                    UCHAR largeMotor, UCHAR smallMotor, UCHAR) {
    bool enableRumble;
    EnterCriticalSection(&g_cfgLock);
    enableRumble = g_cfg.enableRumble;
    LeaveCriticalSection(&g_cfgLock);
    if (!enableRumble) return;

    Slot* s = NULL;
    EnterCriticalSection(&g_slotsLock);
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].target == target) { s = &g_slots[i]; break; }
    }
    if (!s) { LeaveCriticalSection(&g_slotsLock); return; }
    EnterCriticalSection(&s->cs);
    LeaveCriticalSection(&g_slotsLock);

    BYTE packet[4];
    buildRumblePacket(largeMotor, smallMotor, packet);

    BYTE out[64] = {};
    out[0] = 0x10;
    out[1] = 0x00;
    memcpy(out + 2, packet, 4);
    memcpy(out + 6, packet, 4);

    if (s->device != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(s->device, out, s->outputLen, &written, NULL);
    }
    LeaveCriticalSection(&s->cs);
}

//=============================================================================
//  HID device enumeration
//=============================================================================

static bool isWanted(const Config& cfg, USHORT vid, USHORT pid) {
    for (auto& d : cfg.devices)
        if (d.first == vid && d.second == pid) return true;
    return false;
}

static bool pathTaken(const std::wstring& path) {
    for (int i = 0; i < g_slotCount; ++i)
        if (!g_slots[i].path.empty() && g_slots[i].path == path) return true;
    return false;
}

static std::vector<std::wstring> findControllers(const Config& cfg) {
    std::vector<std::wstring> found;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return found;

    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(ifData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &need, NULL);
        if (need == 0) continue;

        std::vector<BYTE> buf(need);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, need, NULL, NULL))
            continue;

        std::wstring path = detail->DevicePath;
        if (pathTaken(path)) continue;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {};
        attrs.Size = sizeof(attrs);
        bool ok = HidD_GetAttributes(h, &attrs) &&
                  isWanted(cfg, attrs.VendorID, attrs.ProductID);
        CloseHandle(h);
        if (ok) found.push_back(path);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

//=============================================================================
//  Reader thread: one per connected controller
//=============================================================================

static DWORD WINAPI readerThread(void* param) {
    Slot& s = *(Slot*)param;
    std::wstring path = s.path;
    int idx = s.index;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&s.alive, 0);
        notify(ENGINE_EVT_SLOT, idx, 0, -1);
        return 1;
    }

    PHIDP_PREPARSED_DATA ppd = NULL;
    HIDP_CAPS caps = {};
    if (HidD_GetPreparsedData(h, &ppd)) {
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.InputReportByteLength >= 12 && caps.InputReportByteLength <= 1024)
                s.reportLen = caps.InputReportByteLength;
            if (caps.OutputReportByteLength >= 8 && caps.OutputReportByteLength <= 1024)
                s.outputLen = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(ppd);
    }
    HidD_SetNumInputBuffers(h, 32);

    {
        EnterCriticalSection(&s.cs);
        s.device = h;
        LeaveCriticalSection(&s.cs);
    }

    notify(ENGINE_EVT_SLOT, idx, 1, -1);

    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    std::vector<BYTE> buf(s.reportLen);
    XUSB_REPORT last = {};
    bool haveLast = false;
    int lastBattery = -1;
    bool disconnected = false;

    // Fresh drift state for every connection: a newly plugged pad may be a
    // different physical controller with its own wear.
    StickDriftCorrector drift[2];

    while (g_running && !s.stop) {
        ResetEvent(ev);
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        DWORD got = 0;

        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), NULL, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) { disconnected = true; break; }
            DWORD wr = WaitForSingleObject(ev, 1000);
            if (wr == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(h, &ov, &got, FALSE)) { disconnected = true; break; }
            } else if (wr == WAIT_TIMEOUT) {
                continue;
            } else {
                disconnected = true; break;
            }
        }

        if (got >= 12) {
            XUSB_REPORT rep = {};
            int battery = -1;
            Config cfgSnap = configSnapshot();

            // --- automatic stick-drift correction -------------------------
            if (InterlockedExchange(&s.calibReq, 0)) {
                drift[0].startCalibration();
                drift[1].startCalibration();
            }
            bool fixOn = cfgSnap.driftFix != 0;
            drift[0].configure(fixOn, cfgSnap.driftStrength);
            drift[1].configure(fixOn, cfgSnap.driftStrength);

            int lx = decodeStickX(buf.data() + 6), ly = decodeStickY(buf.data() + 6);
            int rx = decodeStickX(buf.data() + 9), ry = decodeStickY(buf.data() + 9);
            int olx, oly, orx, ory;
            drift[0].process(lx, ly, olx, oly);   // re-centered raw values
            drift[1].process(rx, ry, orx, ory);

            // Adaptive deadzone: never go below the measured noise floor.
            Config eff = cfgSnap;
            int autoDzPct[2] = { 0, 0 };
            if (fixOn && eff.driftAutoDeadzone) {
                int rec[2] = { drift[0].recommendedDeadzoneRaw(),
                               drift[1].recommendedDeadzoneRaw() };
                int* dz[2] = { &eff.leftDeadzone, &eff.rightDeadzone };
                int range[2] = { eff.leftStickRange, eff.rightStickRange };
                for (int k = 0; k < 2; ++k) {
                    if (range[k] > 0 && rec[k] > dz[k][0] * range[k] / 100) {
                        int pct = (rec[k] * 100 + range[k] - 1) / range[k];
                        if (pct > 90) pct = 90;
                        autoDzPct[k] = pct;
                        dz[k][0] = pct;
                    }
                }
            }

            parseReportCorrected(buf.data(), got, eff, olx, oly, orx, ory,
                                 rep, battery);

            // Diagnostics snapshot for the GUI.
            EnterCriticalSection(&s.cs);
            for (int k = 0; k < 2; ++k) {
                DriftSnap& sn = s.driftSnap[k];
                sn.calibrating = drift[k].calibrating();
                sn.calibrated  = drift[k].calibrated();
                sn.atRest      = drift[k].atRest();
                sn.type        = (int)drift[k].classify();
                sn.offX        = lroundf(drift[k].offsetX());
                sn.offY        = lroundf(drift[k].offsetY());
                sn.noise       = lroundf(drift[k].noiseSigma());
                sn.autoDzPct   = autoDzPct[k];
            }
            LeaveCriticalSection(&s.cs);

            if (battery != lastBattery && battery >= 0 && battery <= 8) {
                lastBattery = battery;
                InterlockedExchange(&s.battery, battery);
                notify(ENGINE_EVT_SLOT, idx, 1, battery);
            }
            if (!haveLast || memcmp(&rep, &last, sizeof(rep)) != 0) {
                if (g_client && s.target)
                    g_vigem.vigem_target_x360_update(g_client, s.target, rep);
                last = rep;
                haveLast = true;
            }
        }
    }

    // Neutralize the virtual pad so games see the controller go quiet.
    XUSB_REPORT neutral = {};
    if (g_client && s.target)
        g_vigem.vigem_target_x360_update(g_client, s.target, neutral);

    {
        EnterCriticalSection(&s.cs);
        if (s.device == h) s.device = INVALID_HANDLE_VALUE;
        CloseHandle(h);
        LeaveCriticalSection(&s.cs);
    }

    if (disconnected || s.stop)
        notify(ENGINE_EVT_SLOT, idx, 0, -1);
    CloseHandle(ev);
    InterlockedExchange(&s.alive, 0);
    return 0;
}

//=============================================================================
//  Watchdog thread: enumerates controllers and manages readers
//=============================================================================

static DWORD WINAPI watchdogThread(void*) {
    while (g_running) {
        if (!g_rebuilding && g_client) {
            ULONGLONG now = GetTickCount64();

            // Reap dead reader threads (after a short backoff).
            for (int i = 0; i < g_slotCount; ++i) {
                Slot& s = g_slots[i];
                if (!s.alive && !s.path.empty()) {
                    if (now - s.assignedAt > 3000) {
                        if (s.thread) { CloseHandle(s.thread); s.thread = NULL; }
                        s.path.clear();
                    }
                }
            }

            // Find free controllers and start a reader for each.
            std::vector<std::wstring> devices = findControllers(configSnapshot());
            for (auto& path : devices) {
                Slot* freeSlot = NULL;
                for (int i = 0; i < g_slotCount; ++i) {
                    if (g_slots[i].target && g_slots[i].path.empty()) { freeSlot = &g_slots[i]; break; }
                }
                if (!freeSlot) break;
                freeSlot->path = path;
                freeSlot->assignedAt = GetTickCount64();
                freeSlot->battery = -1;
                InterlockedExchange(&freeSlot->stop, 0);
                InterlockedExchange(&freeSlot->alive, 1);
                freeSlot->thread = CreateThread(NULL, 0, readerThread, freeSlot, 0, NULL);
                if (!freeSlot->thread) {
                    InterlockedExchange(&freeSlot->alive, 0);
                    freeSlot->path.clear();
                }
            }
        }
        Sleep(750);
    }
    return 0;
}

//=============================================================================
//  Slot lifecycle
//=============================================================================

static void resetSlot(Slot& s, int index) {
    s.target = NULL;
    s.thread = NULL;
    s.device = INVALID_HANDLE_VALUE;
    s.path.clear();
    s.alive = 0;
    s.stop = 0;
    s.assignedAt = 0;
    s.battery = -1;
    s.index = index;
    s.reportLen = 64;
    s.outputLen = 64;
    s.driftSnap[0] = DriftSnap();
    s.driftSnap[1] = DriftSnap();
    s.calibReq = 0;
}

static void stopReaders() {
    for (int i = 0; i < g_slotCount; ++i)
        InterlockedExchange(&g_slots[i].stop, 1);
    for (int i = 0; i < g_slotCount; ++i) {
        Slot& s = g_slots[i];
        if (s.thread) {
            WaitForSingleObject(s.thread, 3000);
            CloseHandle(s.thread);
            s.thread = NULL;
        }
    }
}

static void buildSlots() {
    EnterCriticalSection(&g_slotsLock);
    Config cfg = configSnapshot();
    g_slotCount = std::max(1, std::min(4, cfg.maxControllers));
    for (int i = 0; i < g_slotCount; ++i) {
        Slot& s = g_slots[i];
        resetSlot(s, i);
        s.target = g_vigem.vigem_target_x360_alloc();
        if (!s.target) continue;
        g_vigem.vigem_target_set_vid(s.target, EMU_VID);
        g_vigem.vigem_target_set_pid(s.target, EMU_PID);
        VIGEM_ERROR err = g_vigem.vigem_target_add(g_client, s.target);
        if (!VIGEM_SUCCESS(err)) {
            g_vigem.vigem_target_free(s.target);
            s.target = NULL;
            continue;
        }
        g_vigem.vigem_target_x360_register_notification(g_client, s.target, x360Notification);
    }
    LeaveCriticalSection(&g_slotsLock);
}

static void teardownSlots() {
    stopReaders();
    EnterCriticalSection(&g_slotsLock);
    for (int i = 0; i < g_slotCount; ++i) {
        Slot& s = g_slots[i];
        if (s.target) {
            g_vigem.vigem_target_remove(g_client, s.target);
            g_vigem.vigem_target_free(s.target);
        }
        s.target = NULL;
        s.path.clear();
        s.alive = 0;
        s.battery = -1;
    }
    g_slotCount = 0;
    LeaveCriticalSection(&g_slotsLock);
}

//=============================================================================
//  Public API
//=============================================================================

void engine_set_notifier(EngineNotifier fn) {
    g_notifier = fn;
}

void engine_start() {
    InitializeCriticalSection(&g_cfgLock);
    InitializeCriticalSection(&g_slotsLock);
    for (int i = 0; i < MAX_SLOTS; ++i)
        InitializeCriticalSection(&g_slots[i].cs);

    g_cfg = Config();

    bool dll = loadViGEm(g_vigem, exeDirW());
    notify(ENGINE_EVT_DRIVER, 0, dll ? 1 : 0, 0);

    InterlockedExchange(&g_running, 1);
    g_watchdog = CreateThread(NULL, 0, watchdogThread, NULL, 0, NULL);
}

bool engine_try_connect_bus() {
    if (g_client) return true;
    if (!g_vigem.module) loadViGEm(g_vigem, exeDirW());
    if (!g_vigem.module) {
        notify(ENGINE_EVT_DRIVER, 0, 0, 0);
        return false;
    }

    PVIGEM_CLIENT client = g_vigem.vigem_alloc();
    if (!client) return false;
    VIGEM_ERROR err = g_vigem.vigem_connect(client);
    if (!VIGEM_SUCCESS(err)) {
        wchar_t dbg[256];
        swprintf(dbg, 256, L"ViGEmBus connect failed: %ls (0x%08lX)\n", vigemErrorText(err), err);
        OutputDebugStringW(dbg);
        g_vigem.vigem_free(client);
        notify(ENGINE_EVT_DRIVER, 0, 1, 0);
        return false;
    }

    g_client = client;
    buildSlots();
    notify(ENGINE_EVT_DRIVER, 1, 1, 0);
    return true;
}

void engine_apply_config(const Config& cfg) {
    bool countChanged = false;
    bool devicesChanged = false;

    EnterCriticalSection(&g_cfgLock);
    countChanged = cfg.maxControllers != g_slotCount;
    devicesChanged = cfg.devices != g_cfg.devices;
    g_cfg = cfg;
    LeaveCriticalSection(&g_cfgLock);

    if (!g_client) return;                       // slots don't exist yet; config stored
    if (countChanged) {
        InterlockedExchange(&g_rebuilding, 1);
        teardownSlots();
        buildSlots();
        InterlockedExchange(&g_rebuilding, 0);
    } else if (devicesChanged) {
        // Re-grab: stop all readers; the watchdog re-enumerates immediately.
        for (int i = 0; i < g_slotCount; ++i) {
            InterlockedExchange(&g_slots[i].stop, 1);
            g_slots[i].assignedAt = 0;           // let the watchdog reap instantly
        }
    }
}

EngineStatus engine_get_status() {
    EngineStatus st = {};
    st.dllLoaded = g_vigem.module != NULL;
    st.busConnected = g_client != NULL;
    EnterCriticalSection(&g_slotsLock);
    st.slotCount = g_slotCount;
    for (int i = 0; i < g_slotCount && i < 4; ++i) {
        st.slots[i].connected = g_slots[i].alive != 0;
        st.slots[i].battery = (int)g_slots[i].battery;
    }
    LeaveCriticalSection(&g_slotsLock);
    return st;
}

void engine_calibrate_sticks() {
    EnterCriticalSection(&g_slotsLock);
    for (int i = 0; i < g_slotCount; ++i)
        InterlockedExchange(&g_slots[i].calibReq, 1);
    LeaveCriticalSection(&g_slotsLock);
}

bool engine_get_drift(int slot, DriftView& left, DriftView& right) {
    left = DriftView();
    right = DriftView();
    Config cfg = configSnapshot();
    bool ok = false;

    EnterCriticalSection(&g_slotsLock);
    if (slot >= 0 && slot < g_slotCount && g_slots[slot].alive) {
        Slot& s = g_slots[slot];
        EnterCriticalSection(&s.cs);
        DriftView* out[2] = { &left, &right };
        for (int k = 0; k < 2; ++k) {
            const DriftSnap& sn = s.driftSnap[k];
            out[k]->connected   = true;
            out[k]->enabled     = cfg.driftFix != 0;
            out[k]->autoDzOn    = cfg.driftAutoDeadzone != 0;
            out[k]->calibrating = sn.calibrating;
            out[k]->calibrated  = sn.calibrated;
            out[k]->atRest      = sn.atRest;
            out[k]->type        = sn.type;
            out[k]->offX        = sn.offX;
            out[k]->offY        = sn.offY;
            out[k]->noise       = sn.noise;
            out[k]->autoDzPct   = sn.autoDzPct;
        }
        LeaveCriticalSection(&s.cs);
        ok = true;
    }
    LeaveCriticalSection(&g_slotsLock);
    return ok;
}

void engine_stop() {
    InterlockedExchange(&g_running, 0);

    if (g_watchdog) {
        WaitForSingleObject(g_watchdog, 4000);
        CloseHandle(g_watchdog);
        g_watchdog = NULL;
    }

    if (g_client) {
        stopReaders();
        EnterCriticalSection(&g_slotsLock);
        for (int i = 0; i < g_slotCount; ++i) {
            Slot& s = g_slots[i];
            if (s.target) {
                g_vigem.vigem_target_remove(g_client, s.target);
                g_vigem.vigem_target_free(s.target);
                s.target = NULL;
            }
        }
        g_slotCount = 0;
        LeaveCriticalSection(&g_slotsLock);

        g_vigem.vigem_disconnect(g_client);
        g_vigem.vigem_free(g_client);
        g_client = NULL;
    }

    if (g_vigem.module) {
        FreeLibrary(g_vigem.module);
        g_vigem = ViGEmApi();
    }
}
