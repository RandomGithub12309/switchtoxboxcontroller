//=============================================================================
//  parse.h - Nintendo Switch Pro Controller input report parsing.
//  Pure logic, no Windows dependencies - unit-testable on any platform.
//
//  Report layout of the standard full input report (IDs 0x21 / 0x30 / 0x31..)
//  as streamed by the Pro Controller over USB (64-byte reports):
//    byte 0        report ID (0x30)
//    byte 1        timer
//    byte 2        hi nibble: battery (0-8), lo nibble: connection info
//    byte 3        Y X B A SR SL R ZR        (bits 0..7)
//    byte 4        Minus Plus RStick LStick Home Capture - -
//    byte 5        Down Up Right Left SR SL L ZL
//    bytes 6-8     left stick   (12-bit, 3 bytes packed)
//    bytes 9-11    right stick  (12-bit, 3 bytes packed)
//
//  Stick packing (confirmed against SDL's hidapi switch driver and the Linux
//  hid-nintendo kernel driver):
//      X = byte0 | ((byte1 & 0x0F) << 8)
//      Y = (byte1 >> 4) | (byte2 << 4)
//  Center is 0x800 (2048). Right = higher raw X. Y is inverted: pushing UP
//  LOWERS the raw Y value (same convention as the Joy-Cons), which is why
//  both reference drivers negate Y before reporting it.
//=============================================================================
#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <utility>

// --- XInput (Xbox 360) report format ---------------------------------------
typedef struct XUSB_REPORT {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
} XUSB_REPORT;

#define XUSB_GAMEPAD_DPAD_UP           0x0001
#define XUSB_GAMEPAD_DPAD_DOWN         0x0002
#define XUSB_GAMEPAD_DPAD_LEFT         0x0004
#define XUSB_GAMEPAD_DPAD_RIGHT        0x0008
#define XUSB_GAMEPAD_START             0x0010
#define XUSB_GAMEPAD_BACK              0x0020
#define XUSB_GAMEPAD_LEFT_THUMB        0x0040
#define XUSB_GAMEPAD_RIGHT_THUMB       0x0080
#define XUSB_GAMEPAD_LEFT_SHOULDER     0x0100
#define XUSB_GAMEPAD_RIGHT_SHOULDER    0x0200
#define XUSB_GAMEPAD_GUIDE             0x0400
#define XUSB_GAMEPAD_A                 0x1000
#define XUSB_GAMEPAD_B                 0x2000
#define XUSB_GAMEPAD_X                 0x4000
#define XUSB_GAMEPAD_Y                 0x8000

// --- Configuration used by the parser --------------------------------------
struct Config {
    std::vector<std::pair<uint16_t, uint16_t>> devices;  // accepted VID:PID
    int  maxControllers  = 4;      // virtual pads to create (1-4)
    int  buttonLayout    = 0;      // 0 = Xbox positions, 1 = same labels
    int  captureButton   = 0;      // 0 none, 1 back, 2 start, 3 guide
    int  homeButton      = 1;      // 0 none, 1 guide
    int  swapShoulders   = 0;      // L<->ZL, R<->ZR
    int  leftDeadzone    = 10;     // percent of range
    int  rightDeadzone   = 10;
    int  leftStickRange  = 1700;   // raw units (out of 2048) for full deflection
    int  rightStickRange = 1700;
    // Y axes are NOT inverted by default (0): the official Pro Controller over
    // USB reports Y the standard way (raw value rises when the stick is
    // pushed up). The inverted-Y convention only applies to Bluetooth
    // Joy-Con / full-state reports. Set the Y flags to 1 only for pads that
    // actually report it the other way around.
    int  invertLX = 0, invertLY = 0, invertRX = 0, invertRY = 0;
    int  enableRumble    = 1;
    // Automatic stick-drift correction (see drift.h).
    int  driftFix        = 1;   // master switch: apply the learned corrections
    int  driftAutoDeadzone = 1; // raise the deadzone to the measured noise floor
    int  driftStrength   = 1;   // 0 gentle, 1 balanced, 2 aggressive
};

// Radial-deadzone stick conversion. rawX/rawY are 12-bit values centered on
// 2048. Output is signed 16-bit XInput axes (up/right = positive).
inline void convertStick(int rawX, int rawY, int dzRaw, int range,
                         bool invX, bool invY, int16_t& outX, int16_t& outY) {
    float dx = (float)(rawX - 2048); if (invX) dx = -dx;
    float dy = (float)(rawY - 2048); if (invY) dy = -dy;
    float r = sqrtf(dx * dx + dy * dy);
    if (r <= (float)dzRaw) { outX = 0; outY = 0; return; }
    float k = (r - (float)dzRaw) / (float)(range - dzRaw);
    if (k > 1.0f) k = 1.0f;
    float s = k / r * 32767.0f;
    outX = (int16_t)(dx * s);
    outY = (int16_t)(dy * s);
}

// 12-bit packed stick decoding: X = b0 | ((b1 & 0x0F) << 8),
//                               Y = (b1 >> 4) | (b2 << 4).
inline int decodeStickX(const uint8_t* t) { return t[0] | ((t[1] & 0x0F) << 8); }
inline int decodeStickY(const uint8_t* t) { return (t[1] >> 4) | (t[2] << 4); }

// Convert (possibly drift-corrected) raw stick values into the report.
inline void applySticks(XUSB_REPORT& out, int lx, int ly, int rx, int ry,
                        const Config& cfg) {
    convertStick(lx, ly, cfg.leftDeadzone  * cfg.leftStickRange  / 100, cfg.leftStickRange,
                 cfg.invertLX != 0, cfg.invertLY != 0, out.sThumbLX, out.sThumbLY);
    convertStick(rx, ry, cfg.rightDeadzone * cfg.rightStickRange / 100, cfg.rightStickRange,
                 cfg.invertRX != 0, cfg.invertRY != 0, out.sThumbRX, out.sThumbRY);
}

// Parse buttons / triggers / battery from a valid full report. The sticks
// are left untouched; feed them through applySticks() afterwards.
inline void parseButtons(const uint8_t* b, const Config& cfg, XUSB_REPORT& out,
                         int& battery) {
    const uint8_t* b3 = b + 3;   // buttons byte 0
    const uint8_t* b4 = b + 4;   // buttons byte 1
    const uint8_t* b5 = b + 5;   // buttons byte 2

    battery = (b[2] >> 4) & 0x0F;

    bool swY = (b3[0] & 0x01) != 0, swX = (b3[0] & 0x02) != 0;
    bool swB = (b3[0] & 0x04) != 0, swA = (b3[0] & 0x08) != 0;
    bool swR = (b3[0] & 0x40) != 0, swZR = (b3[0] & 0x80) != 0;
    bool swMinus = (b4[0] & 0x01) != 0, swPlus = (b4[0] & 0x02) != 0;
    bool swRStick = (b4[0] & 0x04) != 0, swLStick = (b4[0] & 0x08) != 0;
    bool swHome = (b4[0] & 0x10) != 0, swCapture = (b4[0] & 0x20) != 0;
    bool swDown = (b5[0] & 0x01) != 0, swUp = (b5[0] & 0x02) != 0;
    bool swRight = (b5[0] & 0x04) != 0, swLeft = (b5[0] & 0x08) != 0;
    bool swL = (b5[0] & 0x40) != 0, swZL = (b5[0] & 0x80) != 0;

    // --- Face buttons -----------------------------------------------------
    bool xbA, xbB, xbX, xbY;
    if (cfg.buttonLayout == 0) {          // Xbox physical positions (default)
        xbA = swB; xbB = swA; xbX = swY; xbY = swX;
    } else {                              // same labels
        xbA = swA; xbB = swB; xbX = swX; xbY = swY;
    }
    if (xbA) out.wButtons |= XUSB_GAMEPAD_A;
    if (xbB) out.wButtons |= XUSB_GAMEPAD_B;
    if (xbX) out.wButtons |= XUSB_GAMEPAD_X;
    if (xbY) out.wButtons |= XUSB_GAMEPAD_Y;

    // --- D-pad ------------------------------------------------------------
    if (swUp)    out.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (swDown)  out.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (swLeft)  out.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (swRight) out.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

    // --- Start / Back -----------------------------------------------------
    if (swPlus)  out.wButtons |= XUSB_GAMEPAD_START;
    if (swMinus) out.wButtons |= XUSB_GAMEPAD_BACK;

    // --- Stick clicks -----------------------------------------------------
    if (swLStick) out.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (swRStick) out.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;

    // --- Guide / Home / Capture ------------------------------------------
    if (cfg.homeButton == 1 && swHome) out.wButtons |= XUSB_GAMEPAD_GUIDE;
    if (swCapture) {
        if (cfg.captureButton == 1) out.wButtons |= XUSB_GAMEPAD_BACK;
        else if (cfg.captureButton == 2) out.wButtons |= XUSB_GAMEPAD_START;
        else if (cfg.captureButton == 3) out.wButtons |= XUSB_GAMEPAD_GUIDE;
    }

    // --- Bumpers / triggers ------------------------------------------------
    // ZL/ZR are digital; map them to full-scale analog triggers.
    bool lb = cfg.swapShoulders ? swZL : swL;
    bool rb = cfg.swapShoulders ? swZR : swR;
    bool lt = cfg.swapShoulders ? swL  : swZL;
    bool rt = cfg.swapShoulders ? swR  : swZR;
    if (lb) out.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (rb) out.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (lt) out.bLeftTrigger  = 0xFF;
    if (rt) out.bRightTrigger = 0xFF;
}

// Returns false when the buffer is not a usable full input report.
inline bool isFullReport(const uint8_t* b, uint32_t len) {
    if (len < 12) return false;
    uint8_t id = b[0];
    return id == 0x30 || id == 0x21 || id == 0x31 || id == 0x32 || id == 0x33;
}

// Parse a full report exactly as it came off the wire (sticks decoded from
// the report bytes, no drift correction).
inline void parseReport(const uint8_t* b, uint32_t len, const Config& cfg,
                        XUSB_REPORT& out, int& battery) {
    out = XUSB_REPORT{};
    if (!isFullReport(b, len)) return;
    parseButtons(b, cfg, out, battery);
    applySticks(out, decodeStickX(b + 6), decodeStickY(b + 6),
                     decodeStickX(b + 9), decodeStickY(b + 9), cfg);
}

// Parse a full report using pre-decoded raw stick values. The engine runs
// the decoded values through the drift correctors (drift.h) first, so the
// sticks passed in here are already re-centered / de-noised as needed.
inline void parseReportCorrected(const uint8_t* b, uint32_t len, const Config& cfg,
                                 int rawLX, int rawLY, int rawRX, int rawRY,
                                 XUSB_REPORT& out, int& battery) {
    out = XUSB_REPORT{};
    if (!isFullReport(b, len)) return;
    parseButtons(b, cfg, out, battery);
    applySticks(out, rawLX, rawLY, rawRX, rawRY, cfg);
}
