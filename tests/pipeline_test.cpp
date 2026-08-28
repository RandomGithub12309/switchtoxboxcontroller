//=============================================================================
//  pipeline_test.cpp - end-to-end simulation of the engine's per-frame path
//  on a drifted pad: raw report bytes -> decode -> drift correction ->
//  adaptive deadzone -> parseReportCorrected -> XUSB output.
//  No Windows needed:
//      g++ -std=c++17 tests/pipeline_test.cpp -I. -o pipeline_test && ./pipeline_test
//=============================================================================
#include "../parse.h"
#include "../drift.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static void encodeStick(uint8_t* t, int x, int y) {
    t[0] = (uint8_t)(x & 0xFF);
    t[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    t[2] = (uint8_t)((y >> 4) & 0xFF);
}

int main() {
    Config cfg;                      // defaults: dz 10%, range 1700, drift on
    StickDriftCorrector L, R;

    uint8_t b[64] = {};
    b[0] = 0x30; b[2] = 0x40;
    const int restLX = 2300, restLY = 1900;   // left stick drifted +252/-148
    const int restRX = 2048, restRY = 2048;   // right stick healthy

    auto frame = [&](int lx, int ly, int rx, int ry, XUSB_REPORT& rep) {
        bool fixOn = cfg.driftFix != 0;
        L.configure(fixOn, cfg.driftStrength);
        R.configure(fixOn, cfg.driftStrength);
        encodeStick(b + 6, lx, ly);
        encodeStick(b + 9, rx, ry);
        int dlx = decodeStickX(b + 6), dly = decodeStickY(b + 6);
        int drx = decodeStickX(b + 9), dry = decodeStickY(b + 9);
        CHECK(dlx == lx && dly == ly && drx == rx && dry == ry);
        int olx, oly, orx, ory;
        L.process(dlx, dly, olx, oly);
        R.process(drx, dry, orx, ory);
        Config eff = cfg;
        if (fixOn && eff.driftAutoDeadzone) {
            int rec[2] = { L.recommendedDeadzoneRaw(), R.recommendedDeadzoneRaw() };
            int* dz[2] = { &eff.leftDeadzone, &eff.rightDeadzone };
            int range[2] = { eff.leftStickRange, eff.rightStickRange };
            for (int k = 0; k < 2; ++k)
                if (range[k] > 0 && rec[k] > dz[k][0] * range[k] / 100) {
                    int pct = (rec[k] * 100 + range[k] - 1) / range[k];
                    if (pct > 90) pct = 90;
                    dz[k][0] = pct;
                }
        }
        int battery = -1;
        parseReportCorrected(b, 64, eff, olx, oly, orx, ory, rep, battery);
    };

    XUSB_REPORT rep;
    // 5 seconds at rest
    for (int i = 0; i < 300; ++i) frame(restLX, restLY, restRX, restRY, rep);
    CHECK(L.calibrated() && R.calibrated());
    CHECK(L.classify() == DRIFT_OFFSET);
    CHECK(R.classify() == DRIFT_NONE);
    CHECK(rep.sThumbLX == 0 && rep.sThumbLY == 0);   // drift invisible to game

    // what the game WOULD have seen without the fix:
    {
        int battery = -1; XUSB_REPORT raw;
        parseReport(b, 64, cfg, raw, battery);
        CHECK(raw.sThumbLX > 1000);                  // the leak the fix removes
    }

    // gameplay: circle the left stick; movement must come through 1:1-ish
    int peakX = 0, peakY = 0;
    for (int i = 0; i < 240; ++i) {
        float a = i * (6.2831853f / 120.0f);
        int lx = restLX + (int)(1200.0f * cosf(a));
        int ly = restLY + (int)(1200.0f * sinf(a));
        frame(lx, ly, restRX, restRY, rep);
        if (rep.sThumbLX > peakX) peakX = rep.sThumbLX;
        if (rep.sThumbLY > peakY) peakY = rep.sThumbLY;
    }
    CHECK(peakX > 20000 && peakY > 20000);           // full motion preserved

    // back at rest: zero again
    for (int i = 0; i < 120; ++i) frame(restLX, restLY, restRX, restRY, rep);
    CHECK(rep.sThumbLX == 0 && rep.sThumbLY == 0);
    CHECK(L.atRest());

    std::printf("pipeline smoke: %s\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
