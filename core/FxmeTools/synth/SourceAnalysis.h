/*
  ------------------------------------------------------------------------------
    synth/SourceAnalysis.h

    What a range oscillator needs to snap a range to the sound, computed once
    when a recording is loaded so the audio thread only looks it up:

      - every rising zero crossing of the mono mix (sorted positions), for
        snapping range edges: nearestRisingZeroCrossing() is a binary search;
      - a period track: the period detected (normalised autocorrelation, by
        FFT) on 4096-sample windows every `hop` samples, 0 where the sound has
        no clear period; periodAt() interpolates it.

    build() allocates and runs FFTs (a second or so for a minute of audio):
    a non-audio thread. Read-only afterwards, realtime safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <vector>

namespace fxme
{

class SourceAnalysis
{
public:
    static constexpr int hop = 2048;
    static constexpr int minLag = 16;
    static constexpr int maxLag = 2048;

    /** Analyses `numSamples` of `numChannels` channels (mixed to mono). */
    void build (const float* const* channels, int numChannels, int numSamples);

    /** The rising zero crossing nearest to `position`, within `radius`
        samples; `position` itself when there is none. */
    double nearestRisingZeroCrossing (double position, double radius) const noexcept;

    /** The period around `position`, in samples; 0 when there is none. */
    double periodAt (double position) const noexcept;

    int getNumZeroCrossings() const noexcept { return (int) zeroCrossings.size(); }

private:
    std::vector<int> zeroCrossings;
    std::vector<float> periods;   // one per hop, centred on frame * hop
};

} // namespace fxme
