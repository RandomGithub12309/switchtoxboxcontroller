//=============================================================================
//  parse_test.cpp - unit tests for parse.h (report parsing + stick math).
//  No Windows needed:
//      g++ -std=c++17 tests/parse_test.cpp -I. -o parse_test && ./parse_test
//=============================================================================
#include "../parse.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

// --- helpers ---------------------------------------------------------------

static void encodeStick(uint8_t* t, int x, int y) {
    t[0] = (uint8_t)(x & 0xFF);
    t[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    t[2] = (uint8_t)((y >> 4) & 0xFF);
}

static void makeReport(uint8_t* b, uint8_t id, uint8_t b3, uint8_t b4, uint8_t b5,
                       int lx, int ly, int rx, int ry, int battery = 4) {
    std::memset(b, 0, 64);
    b[0] = id;
    b[2] = (uint8_t)(battery << 4);
    b[3] = b3; b[4] = b4; b[5] = b5;
    encodeStick(b + 6, lx, ly);
    encodeStick(b + 9, rx, ry);
}

// --- tests -----------------------------------------------------------------

static void test_stick_decode_roundtrip() {
    const int vals[][2] = { {0,0}, {2048,2048}, {4095,4095}, {1,4094}, {0xABC,0x123} };
    for (auto& v : vals) {
        uint8_t t[3];
        encodeStick(t, v[0], v[1]);
        CHECK(decodeStickX(t) == v[0]);
        CHECK(decodeStickY(t) == v[1]);
    }
}

static void test_convert_stick() {
    int16_t x, y;

    convertStick(2048, 2048, 170, 1700, false, false, x, y);   // dead center
    CHECK(x == 0 && y == 0);

    convertStick(2048 + 100, 2048, 170, 1700, false, false, x, y); // inside dz
    CHECK(x == 0 && y == 0);

    convertStick(2048 + 1700, 2048, 170, 1700, false, false, x, y); // full right
    CHECK(x == 32767);

    convertStick(2048, 2048 + 1700, 170, 1700, false, false, x, y); // full up
    CHECK(y == 32767);

    // beyond full deflection clamps
    convertStick(2048 + 2048, 2048, 170, 1700, false, false, x, y);
    CHECK(x == 32767);

    // inversion flags flip the sign
    convertStick(2048 + 1700, 2048, 170, 1700, true, false, x, y);
    CHECK(x == -32767);
    convertStick(2048, 2048 + 1700, 170, 1700, false, true, x, y);
    CHECK(y == -32767);

    // radial scaling: 45 degrees keeps both axes equal
    convertStick(2048 + 600, 2048 + 600, 0, 1700, false, false, x, y);
    CHECK(x == y);
    CHECK(x > 0);
}

static void test_buttons_position_mapping() {
    uint8_t b[64];
    XUSB_REPORT r;
    int battery = -1;
    Config cfg;   // buttonLayout = 0: Xbox positions

    makeReport(b, 0x30, 0x08, 0, 0, 2048, 2048, 2048, 2048);   // Switch A
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_B) != 0);   // Switch A -> Xbox B slot
    CHECK((r.wButtons & XUSB_GAMEPAD_A) == 0);

    cfg.buttonLayout = 1;                          // same labels
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_A) != 0);
    CHECK((r.wButtons & XUSB_GAMEPAD_B) == 0);

    cfg.buttonLayout = 0;
    makeReport(b, 0x30, 0x04, 0, 0, 2048, 2048, 2048, 2048);   // Switch B
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_A) != 0);   // Switch B -> Xbox A slot

    makeReport(b, 0x30, 0x01, 0, 0, 2048, 2048, 2048, 2048);   // Switch Y
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_X) != 0);   // Switch Y -> Xbox X slot
    CHECK((r.wButtons & XUSB_GAMEPAD_Y) == 0);   // (both sit on the left)
}

static void test_dpad_shoulders_triggers() {
    uint8_t b[64];
    XUSB_REPORT r;
    int battery = -1;
    Config cfg;

    makeReport(b, 0x30, 0, 0, 0x0F, 2048, 2048, 2048, 2048);   // all dpad
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & 0x000F) == 0x000F);

    makeReport(b, 0x30, 0, 0, 0x40, 2048, 2048, 2048, 2048);   // L
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_LEFT_SHOULDER) != 0);
    CHECK(r.bLeftTrigger == 0 && r.bRightTrigger == 0);

    makeReport(b, 0x30, 0, 0, 0x80, 2048, 2048, 2048, 2048);   // ZL
    parseReport(b, 64, cfg, r, battery);
    CHECK(r.bLeftTrigger == 0xFF);               // digital -> full analog

    cfg.swapShoulders = 1;
    parseReport(b, 64, cfg, r, battery);         // ZL now behaves like L
    CHECK((r.wButtons & XUSB_GAMEPAD_LEFT_SHOULDER) != 0);
    CHECK(r.bLeftTrigger == 0);

    makeReport(b, 0x30, 0x40, 0, 0, 2048, 2048, 2048, 2048);   // R only
    cfg.swapShoulders = 1;
    parseReport(b, 64, cfg, r, battery);         // R swaps to the trigger
    CHECK((r.wButtons & XUSB_GAMEPAD_RIGHT_SHOULDER) == 0);
    CHECK(r.bRightTrigger == 0xFF);
    CHECK(r.bLeftTrigger == 0);
    cfg.swapShoulders = 0;
    parseReport(b, 64, cfg, r, battery);         // R is the bumper
    CHECK((r.wButtons & XUSB_GAMEPAD_RIGHT_SHOULDER) != 0);
    CHECK(r.bRightTrigger == 0);
}

static void test_system_buttons() {
    uint8_t b[64];
    XUSB_REPORT r;
    int battery = -1;
    Config cfg;

    makeReport(b, 0x30, 0, 0x10, 0, 2048, 2048, 2048, 2048);   // Home
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_GUIDE) != 0);

    cfg.homeButton = 0;
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_GUIDE) == 0);

    makeReport(b, 0x30, 0, 0x22, 0, 2048, 2048, 2048, 2048);   // Capture + Plus
    cfg.captureButton = 2;
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_START) != 0);   // Plus -> Start
    // Capture -> Start too (both map to Start here): fine, still Start.

    makeReport(b, 0x30, 0, 0x21, 0, 2048, 2048, 2048, 2048);   // Capture + Minus
    cfg.captureButton = 1;
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_BACK) != 0);

    makeReport(b, 0x30, 0, 0x0C, 0, 2048, 2048, 2048, 2048);   // stick clicks
    parseReport(b, 64, cfg, r, battery);
    CHECK((r.wButtons & XUSB_GAMEPAD_LEFT_THUMB) != 0);
    CHECK((r.wButtons & XUSB_GAMEPAD_RIGHT_THUMB) != 0);
}

static void test_battery_and_validation() {
    uint8_t b[64];
    XUSB_REPORT r;
    Config cfg;

    makeReport(b, 0x30, 0, 0, 0, 2048, 2048, 2048, 2048, 8);
    int battery = -1;
    parseReport(b, 64, cfg, r, battery);
    CHECK(battery == 8);

    battery = -1;
    parseReport(b, 8, cfg, r, battery);          // too short
    CHECK(battery == -1);
    CHECK(r.wButtons == 0);

    battery = -1;
    b[0] = 0x3F;                                  // unknown report id
    parseReport(b, 64, cfg, r, battery);
    CHECK(battery == -1);
}

static void test_report_stick_decoding() {
    uint8_t b[64];
    XUSB_REPORT r;
    int battery = -1;
    Config cfg;
    cfg.leftDeadzone = 0;
    cfg.leftStickRange = 2048;
    cfg.rightDeadzone = 0;
    cfg.rightStickRange = 2048;

    makeReport(b, 0x30, 0, 0, 0, 2500, 1500, 100, 4000);
    parseReport(b, 64, cfg, r, battery);
    // Left: dx = 452, dy = -548, r < range => out = d * 32767 / 2048.
    CHECK(std::abs(r.sThumbLX - (int16_t)(452 * 32767 / 2048)) <= 2);
    CHECK(std::abs(r.sThumbLY - (int16_t)(-548 * 32767 / 2048)) <= 2);
    // Right: dx = -1948, dy = 1952, r > range => k clamps to 1 and the
    // output is the direction times full scale.
    float rr = std::sqrt(1948.0f * 1948.0f + 1952.0f * 1952.0f);
    CHECK(std::abs(r.sThumbRX - (int16_t)lroundf(-1948.0f * 32767.0f / rr)) <= 2);
    CHECK(std::abs(r.sThumbRY - (int16_t)lroundf(1952.0f * 32767.0f / rr)) <= 2);
    CHECK(r.sThumbRY > 0);                        // 4000 -> up is positive
}

static void test_corrected_sticks_override_bytes() {
    uint8_t b[64];
    XUSB_REPORT r1, r2;
    int battery = -1;
    Config cfg;                                    // default dz 10% of 1700 = 170

    // A stick resting at 2348/2048 has drifted 300 units right: without a
    // fix it leaks past the deadzone.
    makeReport(b, 0x30, 0, 0, 0, 2348, 2048, 2048, 2048);
    parseReport(b, 64, cfg, r1, battery);
    // ~9% of full output leaks into the game - enough to walk on its own.
    CHECK(r1.sThumbLX >= 2700 && r1.sThumbLX <= 2900);

    // The drift corrector would hand back the re-centered value 2048/2048:
    parseReportCorrected(b, 64, cfg, 2048, 2048, 2048, 2048, r2, battery);
    CHECK(r2.sThumbLX == 0 && r2.sThumbLY == 0);
    CHECK(r2.sThumbRX == 0 && r2.sThumbRY == 0);

    // Buttons keep working in both paths.
    makeReport(b, 0x30, 0x08, 0, 0, 2348, 2048, 2048, 2048);
    parseReportCorrected(b, 64, cfg, 2048, 2048, 2048, 2048, r2, battery);
    CHECK((r2.wButtons & XUSB_GAMEPAD_B) != 0);
}

//===========================================================================
int main() {
    test_stick_decode_roundtrip();
    test_convert_stick();
    test_buttons_position_mapping();
    test_dpad_shoulders_triggers();
    test_system_buttons();
    test_battery_and_validation();
    test_report_stick_decoding();
    test_corrected_sticks_override_bytes();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("parse_test: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
