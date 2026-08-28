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

// Live view of the automatic stick-drift correction for one stick
// (see drift.h). Filled by engine_get_drift().
struct DriftView {
    bool connected;      // pad present
    bool enabled;        // correction is being applied (driftFix on)
    bool autoDzOn;       // adaptive noise-floor deadzone enabled
    bool calibrating;    // manual "calibrate now" in progress
    bool calibrated;     // resting center learned at least once
    bool atRest;         // stick currently detected as untouched
    int  type;           // DriftType: 0 none, 1 center offset, 2 jitter, 3 both
    int  offX, offY;     // detected resting offset in raw units (signed)
    int  noise;          // measured noise floor in raw units (per axis)
    int  autoDzPct;      // adaptive deadzone currently in force (% of range)
};

void engine_set_notifier(EngineNotifier fn);
void engine_start();                                  // init, load ViGEmClient.dll
void engine_stop();                                   // idempotent full teardown
void engine_apply_config(const Config& cfg);          // live update / rebuild slots / rescan
bool engine_try_connect_bus();                        // connect to ViGEmBus; returns success
EngineStatus engine_get_status();

// Stick-drift correction -----------------------------------------------------
// Ask every connected pad to re-learn its resting center; the user should
// not touch the sticks for ~1-2 s afterwards.
void engine_calibrate_sticks();
// Drift status of one slot (left/right stick). Returns false when the slot
// has no connected pad.
bool engine_get_drift(int slot, DriftView& left, DriftView& right);
