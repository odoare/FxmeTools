/*
  ------------------------------------------------------------------------------
    TankReverb.h

    The late reverberation of an algorithmic reverb: Jon Dattorro's
    figure-eight tank ("Effect Design, Part 1", JAES 1997), extended with a
    calibrated, frequency-dependent decay, modulation, freeze and shimmer.

    Structure (Dattorro's, delay lengths in samples at his 29761 Hz, scaled
    to the running rate and by Size):

      input   (L + R) / 2 -> band-limit (one-pole low-pass) -> four series
              allpasses (142, 107, 379, 277), coefficients 0.75, 0.75,
              0.625, 0.625 times Diffusion;
      tank    two branches, each fed the diffused input plus the other
              branch's output:
                modulated allpass (672 / 908, coefficient -decayDiffusion1)
                -> delay (4453 / 4217) -> absorption (below) -> allpass
                (1800 / 2656, decayDiffusion2) -> delay (3720 / 3163);
      output  Dattorro's seven signed taps per side across both branches.

    Calibrated decay. Each branch applies its loss once, as the gain a
    recirculation over its own total length L (samples) needs for the tail
    to fall 60 dB in RT60 seconds: g = 10^(-3 L / (RT60 fs)). Around it, two
    first-order shelves make that loss frequency dependent:
      low shelf   x + (kLow - 1) LP (x) at the crossover, kLow = gLow / g,
                  gLow for RT60 * bassMultiplier: the lows last longer
                  (> 1) or shorter (< 1);
      high shelf  x + (kHigh - 1) HP (x) at the damping frequency,
                  kHigh = gHigh / g, gHigh for RT60 * hfRatio: the highs
                  decay faster, as air absorption makes them.
    Each shelf's magnitude stays between 1 and its k, so the branch's gain
    never exceeds its largest target and the loop is stable for any setting.
    rt60At() gives the resulting decay at any frequency, for a display.

    Modulation. The two modulated allpasses read their lines at a delay
    swinging by up to 24 samples (at 29761 Hz, so the same in time at any
    rate) times Depth: one sine LFO in quadrature between the branches, plus
    a slow seeded random drift, so the tail chorus never repeats exactly.
    Reads are Catmull-Rom (fxme::ModDelayLine); every line is one, so Size
    can glide (smoothed over 0.2 s) rather than click.

    Freeze. Ramps (50 ms) every loss to unity (the decay gain and both
    shelves), the input to zero and the shimmer to zero: the allpasses are
    lossless, so the tail holds. A soft limit on each branch's output (the
    identity up to 1, then 1 + tanh (|x| - 1)) is the safety net should
    rounding or interpolation ever push the loop past unity.

    Shimmer. The tank's output (both branches' ends), high-passed at 400 Hz
    and through fxme::SplicePitchShifter (+12 semitones by default), is fed
    back into the tank input. Its gain is the Shimmer amount times
    shimmerHeadroom (1 - the loop's gain): a long decay leaves less room,
    so the pitched copies build up without running away. A second voice
    (shimmer2, its own interval, a second shifter on the same feed) climbs
    alongside, in a chord; the two share that headroom (their gains sum to
    at most one full voice's), so two voices are as stable as one.

    The low shelf is computed with the gain applied, g x + (gLow - g) LP (x),
    never through kLow: at short decays in long loops g is tiny and kLow
    huge. Freeze blends each band's gain (g, gLow, gHigh) to 1 on its own,
    so the loop never gains during the glide.

    Safety: the input is clamped (sanitiseSample, +-16); a branch whose
    absorption or end goes bad anyway (NaN, or beyond +-8) clears the tank.

    Threading: prepare() allocates (prepareToPlay); setSettings() and
    process() are realtime safe. setSettings() once per block is enough:
    Size and Freeze glide per sample, the rest is applied per block.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/dsp/ModDelayLine.h>
#include <FxmeTools/dsp/Sanitise.h>
#include <FxmeTools/dsp/SplicePitchShifter.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace fxme
{

class TankReverb
{
public:
    static constexpr double referenceRate = 29761.0;   // Dattorro's
    static constexpr float  maxSize = 2.5f;
    static constexpr float  minSize = 0.25f;

    /** The structure's coefficients, which a reverb mode chooses. */
    struct Voicing
    {
        float inputDiffusion1 = 0.75f, inputDiffusion2 = 0.625f;
        float decayDiffusion1 = 0.70f, decayDiffusion2 = 0.50f;
        float inputLowpassHz  = 12000.0f;
    };

    struct Settings
    {
        float rt60 = 2.5f;              // seconds, at mid frequencies
        float size = 1.0f;              // delay-length scale, minSize .. maxSize
        float diffusion = 1.0f;         // 0 .. 1, scales the input diffusers
        float dampHz = 5000.0f;         // where the highs start decaying faster
        float hfRatio = 0.5f;           // high RT60 / mid RT60, 0.05 .. 1
        float bassMultiplier = 1.0f;    // low RT60 / mid RT60, 0.25 .. 4
        float crossoverHz = 250.0f;
        float modRateHz = 0.8f;
        float modDepth = 0.5f;          // 0 .. 1
        bool  freeze = false;
        float shimmer = 0.0f;           // 0 .. 1
        float shimmerSemitones = 12.0f;
        float shimmer2 = 0.0f;          // a second voice, 0 .. 1 (0: off)
        float shimmerSemitones2 = 7.0f;
        Voicing voicing;
    };

    //==========================================================================
    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        rateScale = (float) (fs / referenceRate);

        const float longest = maxSize * rateScale;
        // The modulated allpass swings by up to 1.6 x the excursion (the sine
        // plus 0.6 of the drift), the excursion at most 24 x rateScale.
        const float maxSwing = 1.6f * 24.0f * rateScale;
        for (int i = 0; i < 4; ++i)
            diffusers[(size_t) i].prepare (fs, (diffuserLengths[i] * longest + 8.0f) / (float) fs);
        for (int b = 0; b < 2; ++b)
        {
            auto& br = branches[(size_t) b];
            br.modAp.prepare (fs, (tankLengths[b][0] * longest + maxSwing + 8.0f) / (float) fs);
            br.delay1.prepare (fs, (tankLengths[b][1] * longest + 8.0f) / (float) fs);
            br.ap2.prepare (fs, (tankLengths[b][2] * longest + 8.0f) / (float) fs);
            br.delay2.prepare (fs, (tankLengths[b][3] * longest + 8.0f) / (float) fs);
        }

        shifter.prepare (fs, 1, 40.0f);
        shifter2.prepare (fs, 1, 40.0f);
        sizeCoef = 1.0f - (float) std::exp (-1.0 / (0.2 * fs));
        // The longest line moves by at most maxReadSpeed samples a sample.
        maxSizeStep = maxReadSpeed / (4453.0f * rateScale);
        glideCoef = 1.0f - (float) std::exp (-1.0 / (0.03 * fs));
        freezeStep = (float) (1.0 / (0.05 * fs));
        driftCoef = 1.0f - (float) std::exp (-2.0 * 3.141592653589793 * 0.3 / fs);

        targetSize = std::clamp (settings.size, minSize, maxSize);
        updateCoefficients();
        reset();
    }

    /** Clears the tank, and ends every glide on its target: a tank cleared
        and given new settings starts as if prepared with them. */
    void reset()
    {
        currentSize = targetSize;
        freezeAmount = settings.freeze ? 1.0f : 0.0f;
        snapCoefficients();

        for (auto& d : diffusers) d.reset();
        for (auto& br : branches)
        {
            br.modAp.reset(); br.delay1.reset(); br.ap2.reset(); br.delay2.reset();
            br.lowState = br.highState = 0.0f;
            br.end = 0.0f;
            br.drift = 0.0f;
        }
        shifter.reset();
        shifter2.reset();
        bandState = shimmerHpState = shimmerFeed = 0.0f;
        lfoPhase = 0.0;
        rng = 0x5eed5eedu;
    }

    void setSettings (const Settings& s)
    {
        settings = s;
        targetSize = std::clamp (s.size, minSize, maxSize);
        shifter.setPitchSemitones (std::clamp (s.shimmerSemitones, -24.0f, 24.0f));
        shifter2.setPitchSemitones (std::clamp (s.shimmerSemitones2, -24.0f, 24.0f));
        updateCoefficients();
    }

    const Settings& getSettings() const noexcept { return settings; }

    /** One stereo sample in, the tank's stereo output out. */
    void process (float inL, float inR, float& outL, float& outR) noexcept
    {
        // Glides: Size per sample (the lines are read at fractional
        // positions), Freeze over 50 ms, the loop's gains and the rest of the
        // coefficients over about 30 ms (a step in a gain inside the loop
        // is a click).
        currentSize += std::clamp (sizeCoef * (targetSize - currentSize), -maxSizeStep, maxSizeStep);
        glideCoefficients();
        const float freezeTarget = settings.freeze ? 1.0f : 0.0f;
        if (freezeAmount != freezeTarget)
            freezeAmount = freezeAmount < freezeTarget ? std::min (freezeTarget, freezeAmount + freezeStep)
                                                       : std::max (freezeTarget, freezeAmount - freezeStep);
        const float f = freezeAmount;
        const float scale = currentSize * rateScale;

        // Input: band-limit, diffuse, plus the shimmer fed back.
        float x = sanitiseSample (0.5f * (inL + inR), 16.0f) * (1.0f - f);
        bandState += bandCoef * (x - bandState);
        x = bandState;
        for (int i = 0; i < 4; ++i)
            x = allpass (diffusers[(size_t) i], x, diffuserLengths[i] * scale, inputCoefs[(size_t) i].current);
        x += shimmerFeed * (1.0f - f);

        // Modulation: a sine in quadrature plus a slow drift per branch.
        lfoPhase += modIncrement;
        if (lfoPhase >= 1.0)
            lfoPhase -= 1.0;
        const float ang = (float) (2.0 * 3.141592653589793 * lfoPhase);
        const float lfo[2] { std::sin (ang), std::cos (ang) };

        const float crossIn[2] { branches[1].end, branches[0].end };
        for (int b = 0; b < 2; ++b)
        {
            auto& br = branches[(size_t) b];
            br.drift += driftCoef * (nextNoise() - br.drift);
            const float swing = excursion.current * (lfo[b] + 0.6f * br.drift);

            float v = allpass (br.modAp, x + crossIn[b], tankLengths[b][0] * scale + swing,
                               -settings.voicing.decayDiffusion1);
            v = delay (br.delay1, v, tankLengths[b][1] * scale);
            v = absorb (br, v, b, f);
            if (isOutOfBounds (v, 8.0f))   // caught before it fills the lines the output taps read
            {
                reset();
                outL = outR = 0.0f;
                return;
            }
            v = allpass (br.ap2, v, tankLengths[b][2] * scale, settings.voicing.decayDiffusion2);
            v = delay (br.delay2, v, tankLengths[b][3] * scale);

            if (isOutOfBounds (v, 8.0f))
            {
                reset();
                outL = outR = 0.0f;
                return;
            }
            br.end = softLimit (v);
        }

        // Shimmer: the tank's output, high-passed, pitched, fed back.
        const float sg1 = shimmerGain.current, sg2 = shimmerGain2.current;
        if ((sg1 > 1.0e-6f || sg2 > 1.0e-6f) && f < 1.0f)
        {
            const float s = 0.5f * (branches[0].end + branches[1].end);
            shimmerHpState += shimmerHpCoef * (s - shimmerHpState);
            const float hp = s - shimmerHpState;
            shimmerFeed = 0.0f;
            if (sg1 > 1.0e-6f) shimmerFeed += sg1 * shifter.processSample (hp);
            if (sg2 > 1.0e-6f) shimmerFeed += sg2 * shifter2.processSample (hp);
        }
        else
        {
            shimmerFeed = 0.0f;
        }

        // Output taps.
        float l = 0.0f, r = 0.0f;
        for (const auto& t : outputTaps[0]) l += t.sign * tapRead (t, scale);
        for (const auto& t : outputTaps[1]) r += t.sign * tapRead (t, scale);
        outL = 0.6f * l;
        outR = 0.6f * r;
    }

    /** The loop's mean energy, for a tail meter: the two branch ends squared. */
    float getEnergy() const noexcept
    {
        return 0.5f * (branches[0].end * branches[0].end + branches[1].end * branches[1].end);
    }

    /** The decay time at `hz` for these settings at `sampleRate` (as the tank
        realises it, from the same gains and shelves), in seconds. Infinite
        when frozen. */
    static double rt60At (const Settings& s, double hz, double sampleRate) noexcept
    {
        if (s.freeze)
            return std::numeric_limits<double>::infinity();
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double L = branchLength (0, std::clamp (s.size, minSize, maxSize) * (float) (sr / referenceRate));
        const auto g = gains (s, L, sr);
        const double low = shelfMagnitude (hz, s.crossoverHz, g.low / g.mid, true, sr);
        const double high = shelfMagnitude (hz, s.dampHz, g.high / g.mid, false, sr);
        const double perPass = g.mid * low * high;
        if (perPass >= 1.0)
            return std::numeric_limits<double>::infinity();
        return -3.0 * L / (sr * std::log10 (perPass));
    }

private:
    //==========================================================================
    static constexpr float diffuserLengths[4] { 142.0f, 107.0f, 379.0f, 277.0f };
    static constexpr float tankLengths[2][4] { { 672.0f, 4453.0f, 1800.0f, 3720.0f },
                                               { 908.0f, 4217.0f, 2656.0f, 3163.0f } };

    struct Branch
    {
        ModDelayLine modAp, delay1, ap2, delay2;
        float lowState = 0.0f, highState = 0.0f;
        // Per-pass gains, each <= 1, gliding to their targets (a convex
        // mix of values <= 1 stays <= 1: stable all along the glide).
        float gMid = 0.9f, gLow = 0.9f, gHigh = 0.9f;
        float kHigh = 1.0f;                              // gHigh / gMid, <= 1
        float gMidT = 0.9f, gLowT = 0.9f, gHighT = 0.9f, kHighT = 1.0f;
        float end = 0.0f, drift = 0.0f;
    };

    // Dattorro's output taps: which line (branch, element), where, and sign.
    struct Tap { int branch, element; float position; float sign; };
    static constexpr Tap outputTaps[2][7] {
        { { 1, 1,  266.0f, +1.0f }, { 1, 1, 2974.0f, +1.0f }, { 1, 2, 1913.0f, -1.0f }, { 1, 3, 1996.0f, +1.0f },
          { 0, 1, 1990.0f, -1.0f }, { 0, 2,  187.0f, -1.0f }, { 0, 3, 1066.0f, -1.0f } },
        { { 0, 1,  353.0f, +1.0f }, { 0, 1, 3627.0f, +1.0f }, { 0, 2, 1228.0f, -1.0f }, { 0, 3, 2673.0f, +1.0f },
          { 1, 1, 2111.0f, -1.0f }, { 1, 2,  335.0f, -1.0f }, { 1, 3,  121.0f, -1.0f } } };

    /** Per-pass gains at mid, low and high frequencies (double: at short
        decays and long loops they are tiny, and their ratios huge). */
    struct Gains { double mid, low, high; };

    static double branchLength (int b, float scale) noexcept
    {
        double sum = 0.0;
        for (float len : tankLengths[b])
            sum += (double) len;
        return sum * (double) scale;
    }

    static Gains gains (const Settings& s, double L, double sr) noexcept
    {
        const double rt = std::max (0.05, (double) s.rt60);
        auto gainFor = [L, sr] (double seconds) { return std::pow (10.0, -3.0 * L / (seconds * sr)); };
        const double mid  = gainFor (rt);
        const double low  = gainFor (rt * std::clamp ((double) s.bassMultiplier, 0.25, 4.0));
        const double high = gainFor (rt * std::clamp ((double) s.hfRatio, 0.05, 1.0));
        return { mid, low, high };
    }

    /** |x + (k - 1) LP (x)| (low) or |x + (k - 1) HP (x)| (high), one-pole
        filters prewarped as the TPT ones are. */
    static double shelfMagnitude (double hz, double fcHz, double k, bool low, double sr) noexcept
    {
        const double pi = 3.141592653589793;
        const double w = std::tan (pi * std::clamp (hz, 1.0, 0.499 * sr) / sr)
                       / std::tan (pi * std::clamp (fcHz, 1.0, 0.45 * sr) / sr);
        const std::complex<double> jw (0.0, w);
        const std::complex<double> part = low ? 1.0 / (1.0 + jw) : jw / (1.0 + jw);
        return std::abs (1.0 + (k - 1.0) * part);
    }

    void updateCoefficients() noexcept
    {
        const double pi = 3.141592653589793;
        const auto& v = settings.voicing;
        const float d = std::clamp (settings.diffusion, 0.0f, 1.0f);
        const float coefs[4] { v.inputDiffusion1 * d, v.inputDiffusion1 * d, v.inputDiffusion2 * d, v.inputDiffusion2 * d };
        for (size_t i = 0; i < 4; ++i)
            inputCoefs[i].target = coefs[i];

        bandCoef = 1.0f - (float) std::exp (-2.0 * pi * std::clamp ((double) v.inputLowpassHz, 100.0, 0.45 * fs) / fs);
        lowG  = (float) std::tan (pi * std::clamp ((double) settings.crossoverHz, 10.0, 0.45 * fs) / fs);
        highG = (float) std::tan (pi * std::clamp ((double) settings.dampHz, 100.0, 0.45 * fs) / fs);

        double loopGain = 1.0;
        for (int b = 0; b < 2; ++b)
        {
            const auto g = gains (settings, branchLength (b, targetSize * rateScale), fs);
            auto& br = branches[(size_t) b];
            br.gMidT = (float) g.mid;
            br.gLowT = (float) g.low;
            br.gHighT = (float) g.high;
            br.kHighT = g.mid > 0.0 ? (float) std::min (1.0, g.high / g.mid) : 0.0f;
            loopGain *= g.mid;
        }

        modIncrement = std::clamp ((double) settings.modRateHz, 0.01, 20.0) / fs;
        excursion.target = 24.0f * rateScale * std::clamp (settings.modDepth, 0.0f, 1.0f);

        shimmerHpCoef = 1.0f - (float) std::exp (-2.0 * pi * 400.0 / fs);
        // The two voices share the loop's headroom: their gains sum to at
        // most what one voice at full amount gets.
        const float a1 = std::clamp (settings.shimmer, 0.0f, 1.0f), a2 = std::clamp (settings.shimmer2, 0.0f, 1.0f);
        const float budget = shimmerHeadroom * (float) (1.0 - loopGain) / std::max (1.0f, a1 + a2);
        // Gliding with the loop's gains at the same rate: the budget is a
        // concave function of the gain, so the glide stays under it.
        shimmerGain.target = a1 * budget;
        shimmerGain2.target = a2 * budget;
    }

    void glideCoefficients() noexcept
    {
        const float c = glideCoef;
        for (auto& br : branches)
        {
            br.gMid  += c * (br.gMidT  - br.gMid);
            br.gLow  += c * (br.gLowT  - br.gLow);
            br.gHigh += c * (br.gHighT - br.gHigh);
            br.kHigh += c * (br.kHighT - br.kHigh);
        }
        for (auto& k : inputCoefs)
            k.next (c);
        excursion.next (c);
        shimmerGain.next (c);
        shimmerGain2.next (c);
    }

    void snapCoefficients() noexcept
    {
        for (auto& br : branches)
        {
            br.gMid = br.gMidT;
            br.gLow = br.gLowT;
            br.gHigh = br.gHighT;
            br.kHigh = br.kHighT;
        }
        for (auto& k : inputCoefs)
            k.snap();
        excursion.snap();
        shimmerGain.snap();
        shimmerGain2.snap();
    }

    float absorb (Branch& br, float x, int /*branch*/, float freeze) noexcept
    {
        // Frozen: each band's per-pass gain blends to unity on its own, so
        // none passes 1 during the glide. (Blending the mid gain and the
        // shelves' ratios instead multiplied them: with a short decay and
        // Bass above 1, gMid x kLow reached thousands halfway.)
        float gMid = br.gMid, gLow = br.gLow, kHigh = br.kHigh;
        if (freeze > 0.0f)
        {
            gMid = br.gMid + freeze * (1.0f - br.gMid);
            gLow = br.gLow + freeze * (1.0f - br.gLow);
            const float gHigh = br.gHigh + freeze * (1.0f - br.gHigh);
            kHigh = gMid > 0.0f ? std::min (1.0f, gHigh / gMid) : 0.0f;
        }

        // One-pole TPT low-pass at the crossover: the low shelf, with the
        // mid gain applied, gMid at high frequencies and gLow at DC. (Not
        // as gMid x (x + (gLow / gMid - 1) LP): that ratio overflows.)
        const float lowCoef = lowG / (1.0f + lowG);
        const float vLow = (x - br.lowState) * lowCoef;
        const float lp = vLow + br.lowState;
        br.lowState = lp + vLow;
        float y = gMid * x + (gLow - gMid) * lp;

        // One-pole TPT low-pass at the damping frequency; its complement is
        // the high-pass of the high shelf (kHigh <= 1).
        const float highCoef = highG / (1.0f + highG);
        const float vHigh = (y - br.highState) * highCoef;
        const float lp2 = vHigh + br.highState;
        br.highState = lp2 + vHigh;
        return y + (kHigh - 1.0f) * (y - lp2);
    }

    static float allpass (ModDelayLine& line, float x, float delaySamples, float g) noexcept
    {
        const float z = line.read (delaySamples);
        const float w = x + g * z;
        line.write (w);
        return z - g * w;
    }

    static float delay (ModDelayLine& line, float x, float delaySamples) noexcept
    {
        const float y = line.read (delaySamples);
        line.write (x);
        return y;
    }

    float tapRead (const Tap& t, float scale) const noexcept
    {
        const auto& br = branches[(size_t) t.branch];
        const ModDelayLine& line = t.element == 1 ? br.delay1 : (t.element == 2 ? br.ap2 : br.delay2);
        return line.read (t.position * scale);
    }

    static float softLimit (float x) noexcept
    {
        const float a = std::abs (x);
        return a <= 1.0f ? x : std::copysign (1.0f + std::tanh (a - 1.0f), x);
    }

    float nextNoise() noexcept
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) (rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    static constexpr float shimmerHeadroom = 0.9f;

    double fs = 48000.0;
    float rateScale = 1.0f;
    Settings settings;

    std::array<ModDelayLine, 4> diffusers;
    std::array<Branch, 2> branches;
    SplicePitchShifter shifter, shifter2;

    /** A coefficient gliding to its target (one pole, glideCoef). */
    struct Glide
    {
        float current = 0.0f, target = 0.0f;
        void next (float c) noexcept { current += c * (target - current); }
        void snap() noexcept         { current = target; }
    };

    std::array<Glide, 4> inputCoefs {};
    float bandCoef = 0.5f, bandState = 0.0f;
    float lowG = 0.03f, highG = 0.3f;
    /** A Size glide moves a line's read point by at most this many samples
        per sample: a pitch bend of about 4 semitones at most, not a chirp. */
    static constexpr float maxReadSpeed = 0.25f;

    float currentSize = 1.0f, targetSize = 1.0f, sizeCoef = 0.001f, maxSizeStep = 1.0e-5f;
    float freezeAmount = 0.0f, freezeStep = 0.001f;
    double lfoPhase = 0.0, modIncrement = 0.0;
    Glide excursion;
    float driftCoef = 0.001f, glideCoef = 0.001f;
    Glide shimmerGain, shimmerGain2;
    float shimmerHpCoef = 0.05f, shimmerHpState = 0.0f, shimmerFeed = 0.0f;
    std::uint32_t rng = 0x5eed5eedu;
};

} // namespace fxme
