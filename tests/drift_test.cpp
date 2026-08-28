//=============================================================================
//  drift_test.cpp - unit tests for drift.h (automatic stick drift fix).
//  No Windows needed:
//      g++ -std=c++17 tests/drift_test.cpp -I. -o drift_test && ./drift_test
//=============================================================================
#include "../drift.h"
#include "../parse.h"

#include <cstdio>
#include <cmath>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

// Deterministic xorshift32 PRNG so the tests are reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return (float)(next() & 0xFFFFFF) / 16777216.0f; }      // [0,1)
    float range(float a, float b) { return a + uni() * (b - a); }
    float gauss() {                                                       // ~ N(0,1)
        float v = 0;
        for (int i = 0; i < 6; ++i) v += uni();
        return (v - 3.0f) * 1.41421356f;
    }
};

// Feed n samples drawn from a resting position with small noise.
static void feedRest(StickDriftCorrector& d, Rng& rng, int cx, int cy,
                     float noise, int n, int& outX, int& outY) {
    for (int i = 0; i < n; ++i) {
        int x = cx + (int)lroundf(rng.range(-noise, noise));
        int y = cy + (int)lroundf(rng.range(-noise, noise));
        d.process(x, y, outX, outY);
    }
}

//---------------------------------------------------------------------------
static void test_healthy_pad() {
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(42);
    int outX = 0, outY = 0;
    feedRest(d, rng, 2048, 2048, 2.0f, 400, outX, outY);

    CHECK(d.calibrated());
    CHECK(!d.calibrating());
    CHECK(d.atRest());
    CHECK(d.classify() == DRIFT_NONE);
    CHECK(std::fabs(d.offsetX()) < 6.0f);
    CHECK(std::fabs(d.offsetY()) < 6.0f);
    CHECK(std::fabs(outX - 2048) <= 6);          // passthrough, nothing to fix
    CHECK(std::fabs(outY - 2048) <= 6);
    CHECK(d.recommendedDeadzoneRaw() < 40);      // negligible auto deadzone
}

//---------------------------------------------------------------------------
static void test_offset_drift_detected_and_removed() {
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(7);
    const int restX = 2152, restY = 1985;        // worn center: +104 / -63
    int outX = 0, outY = 0;

    feedRest(d, rng, restX, restY, 1.0f, 300, outX, outY);
    CHECK(d.calibrated());
    CHECK(d.classify() == DRIFT_OFFSET);
    CHECK(std::fabs(d.offsetX() - (restX - 2048)) < 8.0f);
    CHECK(std::fabs(d.offsetY() - (restY - 2048)) < 8.0f);
    CHECK(std::fabs(outX - 2048) <= 4);          // resting output re-centered
    CHECK(std::fabs(outY - 2048) <= 4);

    // Intentional motion must be preserved: push right quickly.
    int px = restX;
    for (int i = 0; i < 20; ++i) {
        px += 80;
        d.process(px, restY, outX, outY);
    }
    CHECK(outX >= 3600 && outX <= 3700);         // 3752 - 104 offset

    // Releasing the stick returns the output to center.
    feedRest(d, rng, restX, restY, 1.0f, 120, outX, outY);
    CHECK(d.atRest());
    CHECK(std::fabs(outX - 2048) <= 4);
}

//---------------------------------------------------------------------------
static void test_jitter_drift_detected_and_deadzoned() {
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(99);
    const float sigmaAxis = 18.0f;
    int outX = 0, outY = 0;

    for (int i = 0; i < 600; ++i) {
        int x = 2048 + (int)lroundf(rng.gauss() * sigmaAxis);
        int y = 2048 + (int)lroundf(rng.gauss() * sigmaAxis);
        d.process(x, y, outX, outY);
    }
    CHECK(d.calibrated());
    CHECK(d.classify() == DRIFT_JITTER);
    CHECK(std::fabs(d.offsetX()) < 15.0f);       // mean stays at the center
    float s = d.noiseSigma();
    CHECK(s > 10.0f && s < 30.0f);               // measured ~ sigmaAxis
    int dz = d.recommendedDeadzoneRaw();
    CHECK(dz >= 50 && dz <= 300);                // large enough to swallow noise

    // End to end: with the recommended deadzone, resting samples must not
    // leak any movement into the XInput report.
    int suppressed = 0;
    for (int i = 0; i < 200; ++i) {
        int x = 2048 + (int)lroundf(rng.gauss() * sigmaAxis);
        int y = 2048 + (int)lroundf(rng.gauss() * sigmaAxis);
        int16_t ox, oy;
        convertStick(x, y, dz, 1700, false, false, ox, oy);
        if (ox == 0 && oy == 0) suppressed++;
    }
    CHECK(suppressed >= 195);
}

//---------------------------------------------------------------------------
static void test_offset_plus_jitter() {
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(1234);
    int outX = 0, outY = 0;
    for (int i = 0; i < 600; ++i) {
        int x = 2180 + (int)lroundf(rng.gauss() * 12.0f);
        int y = 2048 + (int)lroundf(rng.gauss() * 12.0f);
        d.process(x, y, outX, outY);
    }
    CHECK(d.classify() == DRIFT_BOTH);
    CHECK(std::fabs(outX - 2048) <= 15);         // still re-centered
}

//---------------------------------------------------------------------------
static void test_motion_is_never_learned() {
    // Spinning the stick in circles must never be captured as "rest".
    StickDriftCorrector d;
    d.configure(true, 1);
    int outX = 0, outY = 0;
    for (int i = 0; i < 900; ++i) {
        float a = (float)i * (6.2831853f / 60.0f);       // 1 rev / second
        int x = 2048 + (int)(600.0f * cosf(a));
        int y = 2048 + (int)(600.0f * sinf(a));
        d.process(x, y, outX, outY);
    }
    CHECK(!d.calibrated());
    CHECK(d.classify() == DRIFT_NONE);
    CHECK(std::fabs(d.offsetX()) < 25.0f);
}

//---------------------------------------------------------------------------
static void test_held_deflection_self_heals() {
    // Plug the pad in while holding the stick right: the first capture
    // learns the held position. Once the stick is released and left alone,
    // the unconfirmed center must be replaced by the true rest position.
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(2024);
    int outX = 0, outY = 0;

    int x = 2048;
    for (int i = 0; i < 200; ++i) {              // ramp out and hold at 2548
        if (x < 2548) x += 3;
        d.process(x, 2048, outX, outY);
    }
    for (int i = 0; i < 200; ++i)                // held still for a while
        d.process(2548, 2048, outX, outY);
    CHECK(d.calibrated());
    CHECK(std::fabs(d.offsetX() - 500.0f) < 20.0f);   // wrong capture...

    feedRest(d, rng, 2048, 2048, 2.0f, 300, outX, outY);  // ...then released
    CHECK(std::fabs(d.offsetX()) < 15.0f);       // healed to the true center
    CHECK(std::fabs(outX - 2048) <= 6);
}

//---------------------------------------------------------------------------
static void test_confirmed_center_ignores_held_deflection() {
    // Once a center is confirmed by real rest, holding the stick somewhere
    // else (walking in a game) must not move it.
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(555);
    int outX = 0, outY = 0;

    feedRest(d, rng, 2048, 2048, 2.0f, 400, outX, outY);   // confirmed
    for (int i = 0; i < 400; ++i)                // held hard right while playing
        d.process(2900, 2048, outX, outY);
    CHECK(std::fabs(d.offsetX()) < 25.0f);       // center did not move

    feedRest(d, rng, 2048, 2048, 2.0f, 200, outX, outY);   // released
    CHECK(d.atRest());
    CHECK(std::fabs(outX - 2048) <= 6);
}

//---------------------------------------------------------------------------
static void test_manual_recalibration() {
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(31337);
    int outX = 0, outY = 0;

    feedRest(d, rng, 2300, 2048, 1.0f, 300, outX, outY);
    CHECK(std::fabs(d.offsetX() - 252.0f) < 10.0f);

    // The drift gets worse; the user presses "Calibrate now".
    d.startCalibration();
    CHECK(d.calibrating());
    feedRest(d, rng, 2360, 2048, 1.0f, 200, outX, outY);
    CHECK(!d.calibrating());
    CHECK(d.calibrated());
    CHECK(std::fabs(d.offsetX() - 312.0f) < 12.0f);
    CHECK(std::fabs(outX - 2048) <= 4);
}

//---------------------------------------------------------------------------
static void test_disabled_passes_through_but_still_detects() {
    StickDriftCorrector d;
    d.configure(false, 1);                       // correction off
    Rng rng(77);
    int outX = 0, outY = 0;
    int failsBefore = g_failures;
    for (int i = 0; i < 300; ++i) {
        int x = 2150, y = 1990;
        d.process(x, y, outX, outY);
        CHECK(outX == x && outY == y);           // exact passthrough
        if (g_failures > failsBefore) break;
    }
    CHECK(d.calibrated());
    CHECK(d.classify() == DRIFT_OFFSET);         // detection stays live
}

//---------------------------------------------------------------------------
static void test_no_premature_recommendations() {
    StickDriftCorrector d;
    d.configure(true, 1);
    int outX, outY;
    for (int i = 0; i < 10; ++i) d.process(2048, 2048, outX, outY);
    CHECK(!d.calibrated());
    CHECK(d.recommendedDeadzoneRaw() == 0);
    CHECK(d.noiseSigma() == 0.0f);
    CHECK(d.classify() == DRIFT_NONE);
}

//---------------------------------------------------------------------------
static void test_presets() {
    DriftPreset g = driftPreset(0), b = driftPreset(1), a = driftPreset(2);
    CHECK(g.learnAlpha < b.learnAlpha && b.learnAlpha < a.learnAlpha);
    CHECK(g.noiseK     < b.noiseK     && b.noiseK     < a.noiseK);
    CHECK(g.settleSamples > b.settleSamples && b.settleSamples > a.settleSamples);
    CHECK(!g.chase && !b.chase && a.chase);             // chase = aggressive only
    CHECK(driftPreset(5).learnAlpha == b.learnAlpha);   // clamped to balanced
}

//---------------------------------------------------------------------------
static void test_aggressive_chases_sudden_rest_jump() {
    // A worn stick whose resting spot intermittently snaps to one side
    // ("pulls left"): aggressive mode adopts the new resting cluster after
    // it persists, so the pull keeps getting cancelled.
    StickDriftCorrector d;
    d.configure(true, 2);
    Rng rng(9);
    int outX = 0, outY = 0;

    feedRest(d, rng, 2048, 2048, 1.0f, 400, outX, outY);   // confirmed center
    CHECK(std::fabs(d.offsetX()) < 10.0f);

    feedRest(d, rng, 1648, 2048, 1.0f, 400, outX, outY);   // rest jumps 400 left
    CHECK(std::fabs(d.offsetX() + 400.0f) < 20.0f);        // adopted
    CHECK(std::fabs(outX - 2048) <= 6);                    // pull cancelled

    // If the adoption was wrong, releasing the stick heals it.
    feedRest(d, rng, 2048, 2048, 1.0f, 300, outX, outY);
    CHECK(std::fabs(d.offsetX()) < 15.0f);
    CHECK(std::fabs(outX - 2048) <= 6);
}

//---------------------------------------------------------------------------
static void test_balanced_does_not_chase() {
    // Same scenario on balanced: a far stationary cluster is left alone
    // (it could be input the player is deliberately holding).
    StickDriftCorrector d;
    d.configure(true, 1);
    Rng rng(9);
    int outX = 0, outY = 0;

    feedRest(d, rng, 2048, 2048, 1.0f, 400, outX, outY);
    feedRest(d, rng, 1648, 2048, 1.0f, 400, outX, outY);
    CHECK(std::fabs(d.offsetX()) < 25.0f);                 // center untouched
    CHECK(outX >= 1600);                                   // signal passes through
}

//---------------------------------------------------------------------------
static void test_short_holds_are_never_chased() {
    // Even aggressive must not adopt clusters held for less than the chase
    // persistence - that is normal gameplay (aiming, walking).
    StickDriftCorrector d;
    d.configure(true, 2);
    Rng rng(9);
    int outX = 0, outY = 0;

    feedRest(d, rng, 2048, 2048, 1.0f, 400, outX, outY);
    for (int rep = 0; rep < 5; ++rep) {
        feedRest(d, rng, 1500, 2600, 1.0f, 80, outX, outY); // held < threshold
        feedRest(d, rng, 2048, 2048, 1.0f, 60, outX, outY); // released
    }
    CHECK(std::fabs(d.offsetX()) < 25.0f);
    CHECK(std::fabs(d.offsetY()) < 25.0f);
}

//---------------------------------------------------------------------------
static void test_slow_progressive_drift_is_followed() {
    // Drift that worsens little by little while the stick sits at rest must
    // be tracked continuously, not just at calibration time.
    StickDriftCorrector d;
    d.configure(true, 2);                        // aggressive learning
    Rng rng(4242);
    int outX = 0, outY = 0;

    feedRest(d, rng, 2048, 2048, 1.0f, 200, outX, outY);
    float rest = 2048.0f;
    for (int step = 0; step < 30; ++step) {
        rest += 2.0f;                            // 60 raw units of slow drift
        feedRest(d, rng, (int)rest, 2048, 1.0f, 30, outX, outY);
    }
    CHECK(std::fabs(d.offsetX() - 60.0f) < 12.0f);
    CHECK(std::fabs(outX - 2048) <= 6);          // output still centered
}

//===========================================================================
int main() {
    test_healthy_pad();
    test_offset_drift_detected_and_removed();
    test_jitter_drift_detected_and_deadzoned();
    test_offset_plus_jitter();
    test_motion_is_never_learned();
    test_held_deflection_self_heals();
    test_confirmed_center_ignores_held_deflection();
    test_manual_recalibration();
    test_disabled_passes_through_but_still_detects();
    test_no_premature_recommendations();
    test_presets();
    test_slow_progressive_drift_is_followed();
    test_aggressive_chases_sudden_rest_jump();
    test_balanced_does_not_chase();
    test_short_holds_are_never_chased();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("drift_test: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
