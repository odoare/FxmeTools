/*
  ------------------------------------------------------------------------------
    util/Math.h

    Framework-free replacements for the handful of JUCE helpers the FxmeTools
    DSP kernels relied on (JUCE's jlimit / jmax / jmin, MathConstants,
    Decibels, roundToInt, degreesToRadians).

    Semantics are deliberately identical to JUCE's, including argument order:
    fxme::jlimit (lower, upper, value) matches JUCE's jlimit exactly, so the
    migration is a pure textual substitution with no risk of silently swapping
    operands. fxme::clamp (value, lower, upper) is also provided for new code
    that prefers the std::clamp ordering.

    Header-only, no allocation, no exceptions — safe on the audio thread.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <algorithm>
#include <type_traits>

namespace fxme
{

//==============================================================================
// Constants. Mirrors JUCE's MathConstants<T>.

template <typename T>
struct MathConstants
{
    static constexpr T pi        = static_cast<T> (3.141592653589793238L);
    static constexpr T twoPi     = static_cast<T> (2.0L * 3.141592653589793238L);
    static constexpr T halfPi    = static_cast<T> (3.141592653589793238L / 2.0L);
    static constexpr T euler     = static_cast<T> (2.71828182845904523536L);
    static constexpr T sqrt2     = static_cast<T> (1.4142135623730950488L);
};

//==============================================================================
// Range helpers. Argument order matches JUCE so migration is mechanical.

/** Constrains value to [lowerLimit, upperLimit]. Same argument order as JUCE's jlimit. */
template <typename T>
constexpr T jlimit (T lowerLimit, T upperLimit, T valueToConstrain) noexcept
{
    return valueToConstrain < lowerLimit ? lowerLimit
         : (upperLimit < valueToConstrain ? upperLimit : valueToConstrain);
}

/** std::clamp argument order, for new code. */
template <typename T>
constexpr T clamp (T value, T lowerLimit, T upperLimit) noexcept
{
    return jlimit (lowerLimit, upperLimit, value);
}

template <typename T> constexpr T jmax (T a, T b) noexcept            { return a < b ? b : a; }
template <typename T> constexpr T jmax (T a, T b, T c) noexcept       { return jmax (jmax (a, b), c); }
template <typename T> constexpr T jmin (T a, T b) noexcept            { return b < a ? b : a; }
template <typename T> constexpr T jmin (T a, T b, T c) noexcept       { return jmin (jmin (a, b), c); }

/** Equivalent of JUCE's isPositiveAndBelow — true when 0 <= value < upperLimit. */
template <typename T>
constexpr bool isPositiveAndBelow (T value, T upperLimit) noexcept
{
    return T() <= value && value < upperLimit;
}

//==============================================================================
// Rounding. Matches JUCE's roundToInt round-half-away-from-zero behaviour.

template <typename T>
inline int roundToInt (T value) noexcept
{
    static_assert (std::is_floating_point<T>::value, "roundToInt expects a floating point value");
    return static_cast<int> (std::lround (value));
}

template <typename T>
inline long long roundToInt64 (T value) noexcept
{
    static_assert (std::is_floating_point<T>::value, "roundToInt64 expects a floating point value");
    return static_cast<long long> (std::llround (value));
}

//==============================================================================
// Angles.

template <typename T>
constexpr T degreesToRadians (T degrees) noexcept
{
    return degrees * (MathConstants<T>::pi / static_cast<T> (180));
}

template <typename T>
constexpr T radiansToDegrees (T radians) noexcept
{
    return radians * (static_cast<T> (180) / MathConstants<T>::pi);
}

//==============================================================================
// Decibels. Mirrors Decibels, including the -100 dB default floor and the
// clamping behaviour at that floor.

/** Equivalent of JUCE's Decibels::gainToDecibels. */
template <typename T>
inline T gainToDecibels (T gain, T minusInfinityDb = static_cast<T> (-100)) noexcept
{
    return gain > T() ? jmax (minusInfinityDb, static_cast<T> (std::log10 (gain)) * static_cast<T> (20))
                      : minusInfinityDb;
}

/** Equivalent of JUCE's Decibels::decibelsToGain. */
template <typename T>
inline T decibelsToGain (T decibels, T minusInfinityDb = static_cast<T> (-100)) noexcept
{
    return decibels > minusInfinityDb ? std::pow (static_cast<T> (10), decibels * static_cast<T> (0.05))
                                      : T();
}

/** Drop-in for call sites written as Decibels::gainToDecibels(...) in the JUCE namespace. */
struct Decibels
{
    template <typename T>
    static T gainToDecibels (T gain, T minusInfinityDb = static_cast<T> (-100)) noexcept
    {
        return fxme::gainToDecibels (gain, minusInfinityDb);
    }

    template <typename T>
    static T decibelsToGain (T decibels, T minusInfinityDb = static_cast<T> (-100)) noexcept
    {
        return fxme::decibelsToGain (decibels, minusInfinityDb);
    }
};

//==============================================================================
// Interpolation / mapping.

template <typename T>
constexpr T lerp (T a, T b, T t) noexcept { return a + (b - a) * t; }

/** Equivalent of JUCE's jmap (value, sourceMin, sourceMax, targetMin, targetMax). */
template <typename T>
constexpr T jmap (T value, T sourceMin, T sourceMax, T targetMin, T targetMax) noexcept
{
    return targetMin + ((targetMax - targetMin) * (value - sourceMin)) / (sourceMax - sourceMin);
}

/** Equivalent of JUCE's jmap (normalisedValue, targetMin, targetMax). */
template <typename T>
constexpr T jmap (T value0To1, T targetMin, T targetMax) noexcept
{
    return targetMin + value0To1 * (targetMax - targetMin);
}

//==============================================================================

/** Equivalent of JUCE's nextPowerOfTwo. */
constexpr int nextPowerOfTwo (int n) noexcept
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

} // namespace fxme
