//=============================================================================
//  engine.h - Switch Pro Controller -> ViGEm/XInput engine.
//  All controller I/O and the ViGEm connection live here; the GUI talks to
//  it through these functions and receives events via the notifier.
//=============================================================================
#pragma once

#include "parse.h"

// Event codes passed to the notifier (called from engine threads).
#define ENGINE_EVT_DRIVER 1    // a = busConnected (0/1), b = dllLoaded (0/1)
#define ENGINE_EVT_SLOT   2    // a = slot index, b = connected (0/1), c = battery (0-8, -1 unknown)

typedef void (*EngineNotifier)(int evt, int a, int b, int c);

struct SlotInfo {
    bool connected;
    int  battery;    // 0-8, -1 unknown
};

struct EngineStatus {
    bool     dllLoaded;
    bool     busConnected;
    int      slotCount;
    SlotInfo slots[4];
};

void engine_set_notifier(EngineNotifier fn);
void engine_start();                                  // init, load ViGEmClient.dll
void engine_stop();                                   // idempotent full teardown
void engine_apply_config(const Config& cfg);          // live update / rebuild slots / rescan
bool engine_try_connect_bus();                        // connect to ViGEmBus; returns success
EngineStatus engine_get_status();
