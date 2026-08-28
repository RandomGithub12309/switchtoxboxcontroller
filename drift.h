//=============================================================================
//  drift.h - Automatic analog-stick drift detection and correction.
//  Pure logic, no Windows dependencies - unit-testable on any platform.
//
//  The Pro Controller's sticks report 12-bit values (0..4095) centered on
//  2048. Worn sticks drift in two ways, often at the same time:
//
//   1. CENTER-OFFSET DRIFT ("bias"): the mechanical center shifts, so the
//      stick reports e.g. 2110/2005 at rest instead of 2048/2048. Games see
//      that as constant input: walking cameras, drifting cars, menus that
//      scroll on their own.
//   2. JITTER DRIFT ("noise"): the sensor becomes noisy and the resting
//      value bounces around. Whenever a bounce exceeds the deadzone, random
//      motion leaks into the game.
//
//  StickDriftCorrector watches the raw stream and handles both, per stick:
//
//   - It detects when the stick is untouched (samples stay clustered in one
//     place instead of travelling), learns the true resting center, and
//     re-centers the output on 2048. Learning never stops while the stick
//     is genuinely at rest, so drift that slowly gets worse keeps being
//     cancelled automatically.
//   - It measures the resting noise floor and derives a recommended minimum
//     deadzone radius large enough to swallow the jitter.
//   - It classifies which type of drift is present (none / center offset /
//     jitter / both) so the UI can show what was detected and what fix was
//     applied.
//
//  Usage: create one instance per physical stick, call configure() when the
//  settings change, feed every HID report into process() - it returns the
//  corrected sample in the same 12-bit space. Statistics always update,
//  even while correction is disabled, so diagnostics stay live.
//
//  State machine:
//
//   BOOTSTRAP --(quiet window detected)--> TRACK
//   TRACK: at-rest samples adapt the center + noise estimate; motion stops
//   adaptation. A brand-new center must first be "confirmed" by ~1 s of real
//   rest; until then, a quieter cluster elsewhere replaces it (recovery from
//   a pad that was plugged in while its sticks were being held). A confirmed
//   center can only be nudged by at-rest learning, never jumped - so a
//   deflection the player holds still is never mistaken for the rest
//   position. startCalibration() returns to BOOTSTRAP on demand.
//=============================================================================
#pragma once

#include <cmath>

// Drift classification for one stick (bitmask-style values).
enum DriftType {
    DRIFT_NONE   = 0,   // no significant drift measured
    DRIFT_OFFSET = 1,   // resting position shifted away from 2048
    DRIFT_JITTER = 2,   // noisy resting position
    DRIFT_BOTH   = 3    // center offset and jitter
};

// Correction profile. 0 = gentle, 1 = balanced (default), 2 = aggressive.
//   gentle    - learns slowly, smallest auto deadzone; trusts the pad more
//   balanced  - middle of the road
//   aggressive- learns fast, largest safety margin, and additionally chases
//               sudden jumps of the resting position (worn sticks whose
//               offset intermittently snaps to one side, e.g. "pulls left")
struct DriftPreset {
    float learnAlpha;    // center adaptation rate per at-rest sample
    float noiseK;        // auto deadzone radius = noiseK * cluster sigma (2D)
    int   settleSamples; // quiet samples needed to (re)learn the center
    bool  chase;         // adopt long-persistent far-away resting clusters
};

inline DriftPreset driftPreset(int strength) {
    switch (strength) {
    case 0:  return { 0.02f,  3.0f,  90, false };
    case 2:  return { 0.10f,  4.5f,  40, true  };
    default: return { 0.05f,  3.75f, 60, false };
    }
}

class StickDriftCorrector {
public:
    StickDriftCorrector() { reset(); }

    // Forget everything learned (a different pad may have been connected).
    void reset() {
        mode_ = BOOTSTRAP;
        cx_ = 2048.0f; cy_ = 2048.0f;
        var_ = 0.0f;
        haveVar_ = false;
        bufN_ = 0;
        atRest_ = false;
        confirmed_ = false;
        approach_ = 0;
        chaseCount_ = 0;
        restSamples_ = 0;
        calibTotal_ = 0;
        manualCalib_ = false;
        hadTrack_ = false;
        prevValid_ = false;
        prevX_ = 0; prevY_ = 0;
        enabled_ = true;
        preset_ = driftPreset(1);
    }

    // Live configuration. enabled = apply corrections to the output;
    // strength selects the preset (0/1/2). Cheap enough to call per report.
    void configure(bool enabled, int strength) {
        enabled_ = enabled;
        preset_ = driftPreset(strength);
    }

    // Manual "calibrate now": re-learn the resting center from the next
    // quiet stretch. The UI should tell the user not to touch the sticks.
    void startCalibration() {
        hadTrack_ = (mode_ == TRACK);
        mode_ = BOOTSTRAP;
        manualCalib_ = true;
        calibTotal_ = 0;
        atRest_ = false;
        approach_ = 0;
        chaseCount_ = 0;
    }

    bool calibrating() const { return manualCalib_ && mode_ == BOOTSTRAP; }
    bool calibrated()  const { return mode_ == TRACK; }
    bool atRest()      const { return atRest_; }

    // Center correction currently applied, raw units. The stick's physical
    // resting position reads roughly 2048 + offsetX() / 2048 + offsetY().
    float offsetX() const { return cx_ - 2048.0f; }
    float offsetY() const { return cy_ - 2048.0f; }

    // Measured resting noise per axis in raw units; 0 until enough at-rest
    // data exists. A healthy Pro Controller sits around 1-4.
    float noiseSigma() const {
        if (!haveVar_ || restSamples_ < kMinNoiseSamples) return 0.0f;
        return sqrtf(var_ * 0.5f);   // var_ holds E[r^2] = 2 * per-axis var
    }

    // Which kind of drift this stick exhibits (vs. the factory center).
    DriftType classify() const {
        if (mode_ != TRACK) return DRIFT_NONE;
        bool off = fabsf(cx_ - 2048.0f) > kOffsetMin ||
                   fabsf(cy_ - 2048.0f) > kOffsetMin;
        bool jit = noiseSigma() > kJitterSigma;
        if (off && jit) return DRIFT_BOTH;
        if (off) return DRIFT_OFFSET;
        if (jit) return DRIFT_JITTER;
        return DRIFT_NONE;
    }

    // Recommended deadzone radius (raw units) that swallows the measured
    // noise floor; 0 while there is not enough data or nothing to swallow.
    int recommendedDeadzoneRaw() const {
        if (mode_ != TRACK || !haveVar_ || restSamples_ < kMinNoiseSamples)
            return 0;
        float sigma2d = sqrtf(var_);
        if (sigma2d < 1.0f) return 0;
        long dz = (long)ceilf(preset_.noiseK * sigma2d);
        if (dz < 4) return 0;                  // negligible vs. normal deadzones
        if (dz > kMaxAutoDz) dz = kMaxAutoDz;
        return (int)dz;
    }

    // Feed one raw 12-bit sample, receive the corrected sample.
    void process(int rawX, int rawY, int& outX, int& outY) {
        float x = (float)rawX, y = (float)rawY;

        float vel = 0.0f;
        if (prevValid_) vel = distf(x, y, (float)prevX_, (float)prevY_);
        prevValid_ = true;
        prevX_ = rawX; prevY_ = rawY;

        bufX_[bufN_ & (kBufSize - 1)] = x;
        bufY_[bufN_ & (kBufSize - 1)] = y;
        bufN_++;

        if (manualCalib_ && hadTrack_ && ++calibTotal_ > kCalibTimeout) {
            manualCalib_ = false;      // user kept moving: keep the old center
            mode_ = TRACK;
        } else if (mode_ == BOOTSTRAP) {
            bootstrap();
        } else {
            track(x, y, vel);
        }

        float ox = enabled_ ? x - (cx_ - 2048.0f) : x;
        float oy = enabled_ ? y - (cy_ - 2048.0f) : y;
        outX = clampRaw(ox);
        outY = clampRaw(oy);
    }

private:
    enum Mode { BOOTSTRAP, TRACK };

    // --- constants (raw units; the stick range is 0..4095) ----------------
    static constexpr float kMoveTol        = 12.0f; // half-mean agreement for "stationary"
    static constexpr float kMaxSpread      = 120.0f; // max cluster sigma (2D) accepted as rest
    static constexpr int   kEnterRestStreak = 4;    // quiet samples to re-enter rest
    static constexpr float kNoiseBeta      = 0.04f; // noise EMA rate per at-rest sample
    static constexpr float kOffsetMin      = 25.0f; // resting offset => offset drift
    static constexpr float kJitterSigma    = 6.0f;  // per-axis noise => jitter drift
    static constexpr int   kMinNoiseSamples= 45;    // at-rest samples before noise is published
    static constexpr int   kConfirmSamples = 300;   // at-rest samples that confirm a center
    static constexpr int   kChaseSamples   = 120;   // persistent far-away rest => adopt (aggressive)
    static constexpr int   kMaxAutoDz      = 900;   // cap for the recommended deadzone
    static constexpr int   kCalibTimeout   = 1200;  // manual-calibration give-up (~20 s)
    static constexpr int   kBufSize        = 128;   // sliding window (power of two)

    static float distf(float x1, float y1, float x2, float y2) {
        float dx = x1 - x2, dy = y1 - y2;
        return sqrtf(dx * dx + dy * dy);
    }
    static int clampRaw(float v) {
        long r = lroundf(v);
        if (r < 0) r = 0;
        else if (r > 4095) r = 4095;
        return (int)r;
    }

    // Adaptive "quiet" velocity limit: healthy pads twitch only a few units
    // per report; noisy pads more, so the limit grows with measured noise.
    float velIdleMax() const {
        float lim = 8.0f;
        if (haveVar_) {
            float scaled = 2.0f * sqrtf(var_) + 4.0f;
            if (scaled > lim) lim = scaled;
        }
        return lim;
    }
    float enterRadius() const {
        float r = haveVar_ ? 3.0f * sqrtf(var_) + 10.0f : 0.0f;
        return r < 48.0f ? 48.0f : r;
    }
    float exitRadius() const {
        float r = haveVar_ ? 5.0f * sqrtf(var_) + 16.0f : 0.0f;
        return r < 120.0f ? 120.0f : r;
    }

    // Examine the last n buffered samples. Returns true when they form a
    // compact, stationary cluster - i.e. the stick is sitting untouched -
    // and reports the cluster's mean and 2D spread. Stationarity is judged
    // by comparing the means of both window halves (real motion, however
    // slow, keeps travelling; jitter just scatters around a fixed point),
    // so noisy pads are handled without a velocity threshold.
    bool stationaryWindow(int n, float& mx, float& my, float& spread2d) const {
        if (n > kBufSize) n = kBufSize;
        if (bufN_ < (unsigned)n) return false;

        float sx = 0, sy = 0, sxx = 0, syy = 0;
        float h1x = 0, h1y = 0, h2x = 0, h2y = 0;
        int half = n / 2;
        for (int i = 0; i < n; ++i) {
            unsigned idx = (bufN_ - (unsigned)n + (unsigned)i) & (kBufSize - 1);
            float px = bufX_[idx], py = bufY_[idx];
            sx += px;  sy += py;
            sxx += px * px;  syy += py * py;
            if (i < half) { h1x += px; h1y += py; }
            else          { h2x += px; h2y += py; }
        }
        float nf = (float)n, hf = (float)(n - half);
        mx = sx / nf; my = sy / nf;
        float vx = sxx / nf - mx * mx;  if (vx < 0) vx = 0;
        float vy = syy / nf - my * my;  if (vy < 0) vy = 0;
        spread2d = sqrtf(vx + vy);
        float dHalf = distf(h1x / (float)half, h1y / (float)half,
                            h2x / hf, h2y / hf);
        return dHalf < kMoveTol && spread2d < kMaxSpread;
    }

    void commitCenter(float mx, float my, float spread2d) {
        cx_ = mx; cy_ = my;
        var_ = spread2d * spread2d;    // E[r^2] of the resting cluster
        haveVar_ = true;
        restSamples_ = preset_.settleSamples;
        confirmed_ = false;
        mode_ = TRACK;
        manualCalib_ = false;
        atRest_ = true;
        approach_ = 0;
        chaseCount_ = 0;
    }

    // Learn the initial resting center from the sliding window.
    void bootstrap() {
        float mx, my, spread;
        if (stationaryWindow(preset_.settleSamples, mx, my, spread))
            commitCenter(mx, my, spread);
    }

    void track(float x, float y, float vel) {
        float r = distf(x, y, cx_, cy_);
        if (!atRest_) {
            if (r < enterRadius() && vel < velIdleMax()) {
                if (++approach_ >= kEnterRestStreak) atRest_ = true;
                chaseCount_ = 0;
            } else {
                approach_ = 0;
                // A compact stationary cluster away from the learned center.
                float mx, my, spread;
                bool farRest =
                    stationaryWindow(preset_.settleSamples, mx, my, spread) &&
                    distf(mx, my, cx_, cy_) > exitRadius();
                if (!confirmed_ && farRest) {
                    // Center never verified (e.g. the pad was plugged in
                    // while held): trust the new quiet cluster.
                    commitCenter(mx, my, spread);
                } else if (preset_.chase && farRest) {
                    // Aggressive mode - drift chase: worn sticks can make
                    // their resting spot suddenly jump to one side ("pulls
                    // left") and sit there. A cluster that persists this
                    // long without any motion is treated as the new rest
                    // position. It is adopted unconfirmed, so if it was a
                    // mistake, releasing the stick heals it.
                    if (++chaseCount_ >= kChaseSamples)
                        commitCenter(mx, my, spread);
                } else {
                    chaseCount_ = 0;
                }
            }
            return;
        }
        float velExit = 60.0f;
        float scaled = 3.0f * velIdleMax();
        if (scaled > velExit) velExit = scaled;
        if (r > exitRadius() || vel > velExit) {
            atRest_ = false;           // the user grabbed the stick
            approach_ = 0;
            return;
        }
        // Still untouched: follow slow center drift, refresh the noise floor.
        cx_ += preset_.learnAlpha * (x - cx_);
        cy_ += preset_.learnAlpha * (y - cy_);
        float dx = x - cx_, dy = y - cy_;
        var_ = var_ * (1.0f - kNoiseBeta) + (dx * dx + dy * dy) * kNoiseBeta;
        if (restSamples_ < 1000000) restSamples_++;
        if (!confirmed_ && restSamples_ >= kConfirmSamples) confirmed_ = true;
    }

    // --- state --------------------------------------------------------------
    Mode   mode_;
    float  cx_, cy_;           // learned resting center (raw units)
    float  var_;               // EMA of E[r^2] around the center
    bool   haveVar_;
    float  bufX_[kBufSize], bufY_[kBufSize];   // sliding window
    unsigned bufN_;
    bool   atRest_;
    bool   confirmed_;         // center verified by enough real rest
    int    approach_;          // consecutive quiet samples while entering rest
    int    chaseCount_;        // persistent far-away stationary samples seen
    int    restSamples_;       // at-rest samples seen (saturating)
    int    calibTotal_;
    bool   manualCalib_;
    bool   hadTrack_;
    bool   prevValid_;
    int    prevX_, prevY_;
    bool   enabled_;
    DriftPreset preset_;
};
