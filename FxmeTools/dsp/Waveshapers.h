/*
  ------------------------------------------------------------------------------
    Waveshapers.h

    Pure, stateless saturation curves shared by the FX-Mechanics distortions
    (lifted from FxmeFX's Tube; FxmeFX is to be refactored to include this
    header instead of its private copies). Header-only, depends only on
    <cmath>. All functions are safe to call from the audio thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace fxme::shapers
{

/** Asymmetric soft saturator approximating a 12AX7 plate stage:
      - Softer compression on the negative side (extended dynamic range).
      - Harder corner on the positive side (grid-current conduction).
    Slope at x = 0 is 1 on both branches, so it composes cleanly with drive. */
inline float triodeCurve (float x)
{
    constexpr float kSoft = 1.2f; // negative side
    constexpr float kHard = 2.0f; // positive side (harder corner)
    if (x >= 0.0f)
        return (1.0f - std::exp (-kHard * x)) / kHard;
    return (std::exp (kSoft * x) - 1.0f) / kSoft;
}

/** One-sided saturator: ~0 below cutoff, tanh-saturated above. */
inline float halfWaveSat (float u)
{
    if (u <= 0.0f) return 0.0f;
    return std::tanh (u);
}

/** Push-pull (Class AB) — two oppositely biased half-wave saturators summed.
    overlap = 0  -> Class B (sharp crossover, odd harmonics dominate)
    overlap > 0  -> Class AB (smoother crossover, gradual approach to Class A) */
inline float classAbCurve (float x, float overlap)
{
    return halfWaveSat (x + overlap) - halfWaveSat (-x + overlap);
}

/** The "Standard" static tube curve: symmetric tanh with a drive gain and a
    DC bias that skews the operating point (even harmonics). The caller is
    expected to DC-block the output when bias != 0. */
inline float tanhDrive (float x, float drive, float bias)
{
    return std::tanh (x * drive + bias);
}

} // namespace fxme::shapers
