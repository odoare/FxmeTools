/*
  ------------------------------------------------------------------------------
    CoreUtilTests.cpp

    Builds and runs with a bare C++17 toolchain — no JUCE on the include path.
    That is the point: if this target ever stops compiling, JUCE has leaked back
    into FxmeTools/core.

    Also asserts that the JUCE-shaped implicit conversions still work, using
    local stand-ins that mimic juce::AudioBuffer and juce::dsp::ProcessSpec
    without including or linking JUCE.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/util/Math.h>
#include <FxmeTools/util/Random.h>
#include <FxmeTools/util/AudioBufferView.h>
#include <FxmeTools/util/ProcessSpec.h>

#include <FxmeTools/dsp/AmbixToStereo.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

//==============================================================================
namespace
{
    int failures = 0;
    int checks   = 0;

    void check (bool condition, const char* what, int line)
    {
        ++checks;
        if (! condition)
        {
            ++failures;
            std::printf ("  FAIL (line %d): %s\n", line, what);
        }
    }

    void checkClose (double a, double b, double tol, const char* what, int line)
    {
        check (std::fabs (a - b) <= tol, what, line);
        if (std::fabs (a - b) > tol)
            std::printf ("        got %.10g, expected %.10g\n", a, b);
    }

    #define CHECK(cond)              check ((cond), #cond, __LINE__)
    #define CHECK_CLOSE(a, b, tol)   checkClose ((a), (b), (tol), #a " ~= " #b, __LINE__)

    //--------------------------------------------------------------------------
    // Stand-ins mimicking the JUCE types' shape, to prove the implicit
    // conversions bind without JUCE being present.

    struct FakeJuceAudioBuffer
    {
        FakeJuceAudioBuffer (int numCh, int numSamp)
            : storage (static_cast<std::size_t> (numCh),
                       std::vector<float> (static_cast<std::size_t> (numSamp), 0.0f)),
              nCh (numCh), nS (numSamp)
        {
            for (auto& v : storage) writePtrs.push_back (v.data());
            for (auto& v : storage) readPtrs.push_back (v.data());
        }

        float* const*       getArrayOfWritePointers()       { return writePtrs.data(); }
        const float* const* getArrayOfReadPointers() const   { return readPtrs.data(); }
        int getNumChannels() const                           { return nCh; }
        int getNumSamples()  const                           { return nS; }

        std::vector<std::vector<float>> storage;
        std::vector<float*>             writePtrs;
        std::vector<const float*>       readPtrs;
        int nCh, nS;
    };

    struct FakeJuceProcessSpec
    {
        double        sampleRate;
        std::uint32_t maximumBlockSize;
        std::uint32_t numChannels;
    };

    // Core-style signatures: these must accept the JUCE-shaped types unchanged.
    float sumBuffer (fxme::ConstAudioBufferView v)
    {
        float total = 0.0f;
        for (int c = 0; c < v.getNumChannels(); ++c)
            for (int i = 0; i < v.getNumSamples(); ++i)
                total += v.getSample (c, i);
        return total;
    }

    double rateOf (fxme::ProcessSpec s) { return s.sampleRate; }
}

//==============================================================================
int main()
{
    std::printf ("FxmeTools core util tests\n");

    // ---- Math: jlimit argument order must match juce::jlimit -----------------
    CHECK (fxme::jlimit (0, 10, 15) == 10);
    CHECK (fxme::jlimit (0, 10, -5) == 0);
    CHECK (fxme::jlimit (0, 10, 7)  == 7);
    CHECK (fxme::jlimit (2.0f, 3.0f, 2.5f) == 2.5f);
    CHECK (fxme::clamp (15, 0, 10) == 10);

    CHECK (fxme::jmax (3, 7) == 7);
    CHECK (fxme::jmin (3, 7) == 3);
    CHECK (fxme::jmax (1, 9, 4) == 9);
    CHECK (fxme::jmin (1, 9, 4) == 1);

    CHECK (fxme::isPositiveAndBelow (0, 4));
    CHECK (fxme::isPositiveAndBelow (3, 4));
    CHECK (! fxme::isPositiveAndBelow (4, 4));
    CHECK (! fxme::isPositiveAndBelow (-1, 4));

    CHECK (fxme::roundToInt (2.5)  == 3);
    CHECK (fxme::roundToInt (-2.5) == -3);   // away from zero, like juce::roundToInt
    CHECK (fxme::roundToInt (2.4)  == 2);

    CHECK (fxme::nextPowerOfTwo (1000) == 1024);
    CHECK (fxme::nextPowerOfTwo (1024) == 1024);

    // ---- Math: constants -----------------------------------------------------
    CHECK_CLOSE (fxme::MathConstants<double>::pi,    3.14159265358979312, 1e-15);
    CHECK_CLOSE (fxme::MathConstants<double>::twoPi, 6.28318530717958623, 1e-15);
    CHECK_CLOSE (fxme::MathConstants<float>::twoPi,  6.2831853f,          1e-6);
    CHECK_CLOSE (fxme::degreesToRadians (180.0), fxme::MathConstants<double>::pi, 1e-15);
    CHECK_CLOSE (fxme::radiansToDegrees (fxme::MathConstants<double>::pi), 180.0, 1e-12);

    // ---- Math: decibels, matching juce::Decibels semantics -------------------
    CHECK_CLOSE (fxme::gainToDecibels (1.0),   0.0,   1e-12);
    CHECK_CLOSE (fxme::gainToDecibels (0.5),  -6.0206, 1e-4);
    CHECK_CLOSE (fxme::gainToDecibels (2.0),   6.0206, 1e-4);
    CHECK_CLOSE (fxme::gainToDecibels (0.0), -100.0,  1e-12);   // floor, not -inf
    CHECK_CLOSE (fxme::gainToDecibels (-1.0), -100.0, 1e-12);   // negative -> floor
    CHECK_CLOSE (fxme::gainToDecibels (1.0e-9), -100.0, 1e-12); // clamped at floor
    CHECK_CLOSE (fxme::gainToDecibels (0.0f, -120.0f), -120.0, 1e-5);

    CHECK_CLOSE (fxme::decibelsToGain (0.0),    1.0,  1e-12);
    CHECK_CLOSE (fxme::decibelsToGain (-6.0206), 0.5, 1e-5);
    CHECK_CLOSE (fxme::decibelsToGain (-100.0), 0.0,  1e-12);   // at floor -> exactly 0
    CHECK_CLOSE (fxme::decibelsToGain (-200.0), 0.0,  1e-12);

    // round trip
    for (double g : { 0.001, 0.01, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0 })
        CHECK_CLOSE (fxme::decibelsToGain (fxme::gainToDecibels (g)), g, 1e-9);

    // struct-style call sites (drop-in for juce::Decibels::)
    CHECK_CLOSE (fxme::Decibels::gainToDecibels (0.5), -6.0206, 1e-4);
    CHECK_CLOSE (fxme::Decibels::decibelsToGain (0.0), 1.0, 1e-12);

    // ---- Math: mapping -------------------------------------------------------
    CHECK_CLOSE (fxme::jmap (0.5, 0.0, 1.0, 10.0, 20.0), 15.0, 1e-12);
    CHECK_CLOSE (fxme::jmap (0.25, 10.0, 20.0), 12.5, 1e-12);
    CHECK_CLOSE (fxme::lerp (0.0, 10.0, 0.3), 3.0, 1e-12);

    // ---- Random --------------------------------------------------------------
    {
        fxme::Random r (12345);

        bool inRange = true, sawLow = false, sawHigh = false;
        double sum = 0.0;

        for (int i = 0; i < 200000; ++i)
        {
            const float v = r.nextFloat();
            if (v < 0.0f || v >= 1.0f) inRange = false;
            if (v < 0.05f) sawLow = true;
            if (v > 0.95f) sawHigh = true;
            sum += v;
        }

        CHECK (inRange);
        CHECK (sawLow && sawHigh);
        CHECK_CLOSE (sum / 200000.0, 0.5, 0.01);          // uniform mean

        bool boundedOk = true;
        for (int i = 0; i < 100000; ++i)
        {
            const int v = r.nextInt (7);
            if (v < 0 || v >= 7) boundedOk = false;
        }
        CHECK (boundedOk);
        CHECK (r.nextInt (0) == 0);
        CHECK (r.nextInt (-3) == 0);

        bool rangeOk = true;
        for (int i = 0; i < 10000; ++i)
        {
            const int v = r.nextInt (5, 9);
            if (v < 5 || v >= 9) rangeOk = false;
        }
        CHECK (rangeOk);

        bool bipolarOk = true;
        for (int i = 0; i < 10000; ++i)
        {
            const float v = r.nextBipolar();
            if (v < -1.0f || v >= 1.0f) bipolarOk = false;
        }
        CHECK (bipolarOk);

        // deterministic for a given seed
        fxme::Random a (999), b (999);
        bool same = true;
        for (int i = 0; i < 1000; ++i)
            if (a.nextInt() != b.nextInt()) same = false;
        CHECK (same);
    }

    // ---- AudioBufferView -----------------------------------------------------
    {
        std::vector<float> l (8, 1.0f), rr (8, 2.0f);
        float* chans[2] = { l.data(), rr.data() };

        fxme::AudioBufferView v (chans, 2, 8);
        CHECK (v.getNumChannels() == 2);
        CHECK (v.getNumSamples() == 8);
        CHECK (! v.isEmpty());
        CHECK (v.getSample (0, 0) == 1.0f);
        CHECK (v[1][3] == 2.0f);

        v.applyGain (0.5f);
        CHECK (v.getSample (0, 0) == 0.5f);
        CHECK (v.getSample (1, 7) == 1.0f);

        v.applyGain (1, 0.0f);
        CHECK (v.getSample (1, 0) == 0.0f);
        CHECK (v.getSample (0, 0) == 0.5f);

        v.setSample (0, 2, 9.0f);
        CHECK (v.getSample (0, 2) == 9.0f);
        v.addSample (0, 2, 1.0f);
        CHECK (v.getSample (0, 2) == 10.0f);

        v.clearChannel (0);
        CHECK (v.getSample (0, 2) == 0.0f);

        v.clear();
        CHECK (v.getSample (0, 0) == 0.0f && v.getSample (1, 7) == 0.0f);

        CHECK (fxme::AudioBufferView().isEmpty());
    }

    // ---- Implicit conversion from JUCE-shaped types --------------------------
    {
        FakeJuceAudioBuffer jb (2, 4);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4; ++i)
                jb.storage[static_cast<std::size_t> (c)][static_cast<std::size_t> (i)] = 1.0f;

        // Passing a JUCE-shaped buffer straight into a core signature.
        CHECK_CLOSE (sumBuffer (jb), 8.0, 1e-9);

        fxme::AudioBufferView v (jb);
        CHECK (v.getNumChannels() == 2 && v.getNumSamples() == 4);
        v.applyGain (2.0f);
        CHECK (jb.storage[0][0] == 2.0f);

        FakeJuceProcessSpec js { 48000.0, 512, 2 };
        CHECK_CLOSE (rateOf (js), 48000.0, 1e-9);

        fxme::ProcessSpec cs (js);
        CHECK (cs.maximumBlockSize == 512u && cs.numChannels == 2u);

        // Copy construction must NOT go through the templated converter.
        fxme::ProcessSpec copy (cs);
        CHECK (copy.sampleRate == cs.sampleRate);

        fxme::ProcessSpec direct (44100.0, 256, 1);
        CHECK (direct.sampleRate == 44100.0 && direct.maximumBlockSize == 256u);
    }

    //--------------------------------------------------------------------------
    // ---- A real core API taking the views, fed a JUCE-shaped buffer ---------
    //
    // AmbixToStereo::process used to take AudioBuffer<float> references. The
    // claim that swapping those for views costs consumers nothing rests
    // entirely on the implicit conversion, and no consumer's call sites were
    // edited on the strength of it — so it is asserted here, at the one
    // boundary where the substitution actually changed a signature.
    {
        FakeJuceAudioBuffer ambi (4, 8);      // W = 1, Y = Z = X = 0
        for (int i = 0; i < 8; ++i)
            ambi.storage[0][static_cast<std::size_t> (i)] = 1.0f;

        FakeJuceAudioBuffer stereo (2, 8);

        fxme::AmbixToStereo decoder;
        decoder.process (ambi, stereo);       // no adapter, no cast

        // A figure-of-eight has no omni component, so a W-only field puts
        // nothing in the side signal: both channels get the cardioid's
        // alpha = 0.5 of W, and they are equal.
        CHECK_CLOSE (stereo.storage[0][0], 0.5, 1e-6);
        CHECK_CLOSE (stereo.storage[1][0], 0.5, 1e-6);
        CHECK (stereo.storage[0][7] == stereo.storage[1][7]);

        // process() adds rather than replaces — running it twice doubles.
        decoder.process (ambi, stereo);
        CHECK_CLOSE (stereo.storage[0][0], 1.0, 1e-6);
    }

    //--------------------------------------------------------------------------
    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
