/*
  ------------------------------------------------------------------------------
    ArEnvelope.h

    Trapezoid attack / hold / release envelope, sample-counter based: linear
    0->1 over the attack, 1 during the hold, linear 1->0 over the release,
    then 0 until the next trigger(). Made for gate windows (e.g. a rhythmic
    gater's open phase) but generally reusable wherever a click-free
    rectangular envelope is needed.

    Header-only, no dependencies; realtime-safe.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

namespace fxme
{

class ArEnvelope
{
public:
    /** (Re)start the envelope. Negative counts are treated as zero; a zero
        attack starts at full level immediately. */
    void trigger (int attackSamples, int holdSamples, int releaseSamples)
    {
        attack  = attackSamples  > 0 ? attackSamples  : 0;
        hold    = holdSamples    > 0 ? holdSamples    : 0;
        release = releaseSamples > 0 ? releaseSamples : 0;
        pos = 0;
    }

    /** The envelope value for the current sample; advances one sample. */
    float nextSample()
    {
        const int p = pos;
        if (p >= attack + hold + release)
            return 0.0f;
        ++pos;

        if (p < attack)
            return (float) (p + 1) / (float) attack;
        if (p < attack + hold)
            return 1.0f;
        return (float) (attack + hold + release - p - 1) / (float) release;
    }

    bool isActive() const { return pos < attack + hold + release; }

    void reset() { pos = attack + hold + release; }

private:
    int attack = 0, hold = 0, release = 0;
    int pos = 0;
};

} // namespace fxme
