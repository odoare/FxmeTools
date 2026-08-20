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
#include <FxmeTools/util/AudioBuffer.h>
#include <FxmeTools/util/Fft.h>
#include <FxmeTools/util/SmoothedValue.h>
#include <FxmeTools/util/StringRef.h>
#include <FxmeTools/util/StringUtils.h>
#include <FxmeTools/util/ArrayView.h>
#include <FxmeTools/midi/MidiTools.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
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

    /** Mimics the shape of the framework's string: UTF-8 out, and a byte
        count that excludes the terminator. Nothing here includes it. */
    struct FakeJuceString
    {
        std::string s;
        const char* toRawUTF8()        const { return s.c_str(); }
        std::size_t getNumBytesAsUTF8() const { return s.size(); }
    };

    /** A narrower one that cannot report its own length. */
    struct FakeUnsizedString
    {
        std::string s;
        const char* toRawUTF8() const { return s.c_str(); }
    };

    /** Mimics the framework's array: contiguous, with an int size(). */
    struct FakeJuceArray
    {
        std::vector<int> v;
        const int* data() const { return v.data(); }
        int        size() const { return static_cast<int> (v.size()); }
    };

    struct FakeJuceProcessSpec
    {
        double        sampleRate;
        std::uint32_t maximumBlockSize;
        std::uint32_t numChannels;
    };

    // O(N^2) reference transform. Slow on purpose: it is the definition of the
    // DFT written out, so agreeing with it is evidence the butterflies and the
    // bit-reversal are right, in a way a forward/inverse round-trip is not —
    // a round-trip passes even when both directions share the same error.
    void naiveDft (const std::complex<float>* in, std::complex<float>* out,
                   int n, bool inverse)
    {
        const double pi   = 3.141592653589793238;
        const double sign = inverse ? 2.0 : -2.0;

        for (int k = 0; k < n; ++k)
        {
            double re = 0.0, im = 0.0;

            for (int i = 0; i < n; ++i)
            {
                const double a = sign * pi * static_cast<double> (k)
                                          * static_cast<double> (i) / static_cast<double> (n);
                const double c = std::cos (a), s = std::sin (a);
                re += static_cast<double> (in[i].real()) * c - static_cast<double> (in[i].imag()) * s;
                im += static_cast<double> (in[i].real()) * s + static_cast<double> (in[i].imag()) * c;
            }

            if (inverse)
            {
                re /= static_cast<double> (n);
                im /= static_cast<double> (n);
            }

            out[k] = { static_cast<float> (re), static_cast<float> (im) };
        }
    }

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
    // ---- Fft / RealFft: JUCE's dsp::FFT semantics, without JUCE -------------
    //
    // Three DSP files are migrated on the promise that these match JUCE bin for
    // bin — natural order, forward unscaled, inverse scaled by 1/N. Nothing in
    // a JUCE build would catch a mismatch: the code would compile and simply
    // sound or measure wrong, so the conventions are pinned here.
    {
        // --- forward agrees with the definition of the DFT -------------------
        for (int order = 1; order <= 6; ++order)
        {
            const int n = 1 << order;
            fxme::Random rng (12345);
            std::vector<std::complex<float>> in ((std::size_t) n), got ((std::size_t) n), want ((std::size_t) n);

            for (int i = 0; i < n; ++i)
                in[(std::size_t) i] = { rng.nextBipolar(), rng.nextBipolar() };

            fxme::Fft fft (order);
            fft.perform (in.data(), got.data(), false);
            naiveDft (in.data(), want.data(), n, false);

            double worst = 0.0;
            for (int k = 0; k < n; ++k)
                worst = std::fmax (worst, (double) std::abs (got[(std::size_t) k] - want[(std::size_t) k]));

            CHECK (worst < 1.0e-4);

            // --- and so does the inverse, including the 1/N scaling ----------
            fft.perform (in.data(), got.data(), true);
            naiveDft (in.data(), want.data(), n, true);

            worst = 0.0;
            for (int k = 0; k < n; ++k)
                worst = std::fmax (worst, (double) std::abs (got[(std::size_t) k] - want[(std::size_t) k]));

            CHECK (worst < 1.0e-4);
        }

        // --- scaling, stated as the two facts call sites depend on -----------
        {
            const int order = 4, n = 1 << order;
            std::vector<std::complex<float>> in ((std::size_t) n, { 2.0f, 0.0f }), out ((std::size_t) n);
            fxme::Fft fft (order);

            // Forward is UNSCALED: a constant 2 puts 2*N in the DC bin.
            fft.perform (in.data(), out.data(), false);
            CHECK_CLOSE (out[0].real(), 2.0 * n, 1e-3);
            CHECK_CLOSE (std::abs (out[1]), 0.0, 1e-3);

            // Inverse is scaled by 1/N: the same input gives back 2 at bin 0.
            fft.perform (in.data(), out.data(), true);
            CHECK_CLOSE (out[0].real(), 2.0, 1e-5);
        }

        // --- bin k really is frequency k (the permutation trap) --------------
        {
            const int order = 8, n = 1 << order;
            const int bin = 5;
            const double twoPi = 2.0 * 3.141592653589793238;
            std::vector<std::complex<float>> in ((std::size_t) n), out ((std::size_t) n);

            for (int i = 0; i < n; ++i)
                in[(std::size_t) i] = { (float) std::cos (twoPi * bin * i / (double) n), 0.0f };

            fxme::Fft (order).perform (in.data(), out.data(), false);

            // A real cosine at bin k: N/2 at k and at N-k, nothing anywhere else.
            CHECK_CLOSE (std::abs (out[(std::size_t) bin]), n / 2.0, 1e-2);
            CHECK_CLOSE (std::abs (out[(std::size_t) (n - bin)]), n / 2.0, 1e-2);

            double leak = 0.0;
            for (int k = 0; k < n; ++k)
                if (k != bin && k != n - bin)
                    leak = std::fmax (leak, (double) std::abs (out[(std::size_t) k]));

            CHECK (leak < 1.0e-2);
        }

        // --- RealFft forward == complex forward of the same samples ----------
        {
            const int order = 7, n = 1 << order;
            fxme::Random rng (999);
            std::vector<float> real ((std::size_t) (2 * n), 0.0f);
            std::vector<std::complex<float>> in ((std::size_t) n), want ((std::size_t) n);

            for (int i = 0; i < n; ++i)
            {
                const float s = rng.nextBipolar();
                real[(std::size_t) i] = s;
                in[(std::size_t) i]   = { s, 0.0f };
            }

            fxme::RealFft (order).performRealOnlyForwardTransform (real.data());
            fxme::Fft (order).perform (in.data(), want.data(), false);

            const auto* got = reinterpret_cast<const std::complex<float>*> (real.data());
            double worst = 0.0;
            for (int k = 0; k < n; ++k)                     // all N bins, not just N/2+1
                worst = std::fmax (worst, (double) std::abs (got[k] - want[(std::size_t) k]));

            CHECK (worst < 1.0e-3);
        }

        // --- RealFft round-trip returns the samples in the first N floats ----
        {
            const int order = 9, n = 1 << order;
            fxme::Random rng (4242);
            std::vector<float> buf ((std::size_t) (2 * n), 0.0f), original ((std::size_t) n);

            for (int i = 0; i < n; ++i)
                original[(std::size_t) i] = buf[(std::size_t) i] = rng.nextBipolar();

            fxme::RealFft fft (order);
            fft.performRealOnlyForwardTransform (buf.data());
            fft.performRealOnlyInverseTransform (buf.data());

            double worst = 0.0;
            for (int i = 0; i < n; ++i)
                worst = std::fmax (worst, std::fabs ((double) (buf[(std::size_t) i]
                                                               - original[(std::size_t) i])));

            CHECK (worst < 1.0e-4);
        }

        // --- magnitude spectrum, and how far it zeroes behind itself ---------
        {
            const int order = 6, n = 1 << order;
            const int bin = 3;
            const double twoPi = 2.0 * 3.141592653589793238;

            for (int pass = 0; pass < 2; ++pass)
            {
                const bool nonNegOnly = (pass == 1);
                std::vector<float> buf ((std::size_t) (2 * n), 0.0f);

                for (int i = 0; i < n; ++i)
                    buf[(std::size_t) i] = (float) std::cos (twoPi * bin * i / (double) n);

                fxme::RealFft (order).performFrequencyOnlyForwardTransform (buf.data(), nonNegOnly);

                CHECK_CLOSE (buf[(std::size_t) bin], n / 2.0, 1e-2);

                const int limit = nonNegOnly ? (n / 2) + 1 : n;
                bool tailZeroed = true;
                for (int i = limit; i < 2 * n; ++i)
                    if (buf[(std::size_t) i] != 0.0f)
                        tailZeroed = false;

                CHECK (tailZeroed);
            }
        }

        // --- degenerate order 0, which JUCE special-cases everywhere ---------
        {
            fxme::Fft one (0);
            CHECK (one.getSize() == 1);

            const std::complex<float> in { 3.0f, -1.0f };
            std::complex<float> out { 0.0f, 0.0f };
            one.perform (&in, &out, false);
            CHECK (out.real() == 3.0f && out.imag() == -1.0f);

            fxme::RealFft realOne (0);
            float buf[2] = { 7.0f, 0.0f };
            realOne.performRealOnlyForwardTransform (buf);
            CHECK (buf[0] == 7.0f);              // left alone, as JUCE leaves it
        }
    }

    //--------------------------------------------------------------------------
    // ---- SmoothedValue: JUCE's ramp, step for step --------------------------
    //
    // A ramp that lands a sample early or approaches its target by accumulated
    // addition instead of snapping onto it is audible and compiles fine, so the
    // arithmetic is pinned rather than assumed.
    {
        // An un-reset instance has no ramp length: setTargetValue must jump,
        // not freeze at the initial value.
        {
            fxme::SmoothedValue<float> sv (0.0f);
            sv.setTargetValue (1.0f);
            CHECK (! sv.isSmoothing());
            CHECK (sv.getNextValue() == 1.0f);
        }

        // reset() floors the step count and snaps to the current target.
        {
            fxme::SmoothedValue<float> sv (0.0f);
            sv.reset (1000.0, 0.0049);          // 4.9 steps -> 4
            sv.setTargetValue (1.0f);

            CHECK (sv.isSmoothing());
            CHECK_CLOSE (sv.getNextValue(), 0.25, 1e-6);
            CHECK_CLOSE (sv.getNextValue(), 0.50, 1e-6);
            CHECK_CLOSE (sv.getNextValue(), 0.75, 1e-6);

            // Exactly on the last step, and exactly the target.
            CHECK (sv.getNextValue() == 1.0f);
            CHECK (! sv.isSmoothing());

            // Past the end it holds, it does not overshoot.
            CHECK (sv.getNextValue() == 1.0f);
        }

        // reset() mid-ramp abandons it rather than leaving it part way through.
        {
            fxme::SmoothedValue<float> sv (0.0f);
            sv.reset (48000.0, 1.0);
            sv.setTargetValue (1.0f);
            sv.getNextValue();
            CHECK (sv.isSmoothing());

            sv.reset (48000.0, 1.0);
            CHECK (! sv.isSmoothing());
            CHECK (sv.getCurrentValue() == 1.0f);   // snapped onto the target
        }

        // setCurrentAndTargetValue cancels a ramp outright.
        {
            fxme::SmoothedValue<float> sv (0.0f);
            sv.reset (48000.0, 1.0);
            sv.setTargetValue (1.0f);
            sv.setCurrentAndTargetValue (-3.0f);
            CHECK (! sv.isSmoothing());
            CHECK (sv.getNextValue() == -3.0f);
        }

        // skip() lands where the equivalent getNextValue calls would.
        {
            fxme::SmoothedValue<float> a (0.0f), b (0.0f);
            a.reset (1000.0, 0.008);            // 8 steps
            b.reset (1000.0, 0.008);
            a.setTargetValue (1.0f);
            b.setTargetValue (1.0f);

            for (int i = 0; i < 3; ++i)
                a.getNextValue();

            CHECK_CLOSE (b.skip (3), a.getCurrentValue(), 1e-6);

            // Skipping past the end settles exactly on the target.
            CHECK (b.skip (100) == 1.0f);
            CHECK (! b.isSmoothing());
        }
    }

    //--------------------------------------------------------------------------
    // ---- AudioBuffer: owning storage that is still view-compatible ----------
    {
        fxme::AudioBuffer buf;
        buf.setSize (3, 16);

        CHECK (buf.getNumChannels() == 3 && buf.getNumSamples() == 16);

        // setSize zeroes.
        bool allZero = true;
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 16; ++i)
                if (buf.getSample (c, i) != 0.0f)
                    allZero = false;
        CHECK (allZero);

        // Channels are independent regions, not aliases of one another.
        for (int i = 0; i < 16; ++i)
            buf.setSample (1, i, 2.0f);

        CHECK (buf.getSample (0, 0) == 0.0f);
        CHECK (buf.getSample (1, 0) == 2.0f);
        CHECK (buf.getSample (2, 0) == 0.0f);
        CHECK (buf.getReadPointer (1)[15] == 2.0f);

        // The whole point: it satisfies the view's shape test, so it converts
        // implicitly at a core API boundary with no adapter.
        CHECK_CLOSE (sumBuffer (buf), 32.0, 1e-6);

        fxme::AudioBufferView v (buf);
        CHECK (v.getNumChannels() == 3 && v.getNumSamples() == 16);
        v.applyGain (1, 0.5f);
        CHECK (buf.getSample (1, 0) == 1.0f);       // the view wrote through

        buf.clearChannel (1);
        CHECK (buf.getSample (1, 0) == 0.0f);

        for (int i = 0; i < 16; ++i)
            buf.setSample (0, i, 1.0f);
        buf.clear();
        CHECK_CLOSE (sumBuffer (buf), 0.0, 1e-9);

        // Resizing re-points the channel array rather than dangling.
        buf.setSize (2, 64);
        CHECK (buf.getNumChannels() == 2 && buf.getNumSamples() == 64);
        buf.setSample (1, 63, 7.0f);
        CHECK (buf.getReadPointer (1)[63] == 7.0f);

        // Degenerate sizes must not blow up.
        fxme::AudioBuffer empty;
        CHECK (empty.getNumChannels() == 0 && empty.getNumSamples() == 0);
        empty.clear();
    }

    //--------------------------------------------------------------------------
    // ---- Random's two constructors are meant to behave differently ----------
    //
    // The default one must decorrelate instances: two noise sources sharing a
    // sequence sum coherently instead of spreading, which is the whole reason
    // this is not just a fixed seed. The explicit one must stay reproducible.
    {
        // Independent by default — checked over a run of values, since any one
        // draw could collide by chance.
        {
            fxme::Random a, b;
            int identical = 0;
            for (int i = 0; i < 32; ++i)
                if (a.nextInt() == b.nextInt())
                    ++identical;

            CHECK (identical < 32);
            CHECK (a.getSeed() != b.getSeed());
        }

        // Even built back to back in a tight loop, where the clock may not have
        // ticked at all: the process-wide counter has to carry it.
        {
            std::int64_t seeds[8];
            for (int i = 0; i < 8; ++i)
                seeds[i] = fxme::Random().getSeed();

            bool allDistinct = true;
            for (int i = 0; i < 8; ++i)
                for (int j = i + 1; j < 8; ++j)
                    if (seeds[i] == seeds[j])
                        allDistinct = false;

            CHECK (allDistinct);
        }

        // And an explicit seed is still exactly reproducible.
        {
            fxme::Random a (2024), b (2024);
            bool same = true;
            for (int i = 0; i < 64; ++i)
                if (a.nextFloat() != b.nextFloat())
                    same = false;

            CHECK (same);
            CHECK (fxme::Random (7).getSeed() == 7);
        }

        // setSeedRandomly moves a deterministic instance off its seed.
        {
            fxme::Random r (12345);
            r.setSeedRandomly();
            CHECK (r.getSeed() != 12345);
        }
    }

    // ---- StringRef: implicit conversion, the whole point of the type ---------
    {
        auto lengthOf = [] (fxme::StringRef s) { return s.length(); };
        auto textOf   = [] (fxme::StringRef s) { return std::string (s.data(), s.length()); };

        // A framework-shaped string binds with no conversion at the call site.
        FakeJuceString framework { "C#maj7" };
        CHECK (lengthOf (framework) == 6);
        CHECK (textOf (framework) == "C#maj7");

        // So does one that cannot report its own length.
        FakeUnsizedString narrow { "Bb" };
        CHECK (lengthOf (narrow) == 2);

        // And so do the two standard spellings.
        CHECK (lengthOf ("Am7") == 3);
        CHECK (lengthOf (std::string ("Am7")) == 3);

        // data() is always null-terminated, so it can go to a C interface.
        fxme::StringRef r (framework);
        CHECK (r.data()[r.length()] == '\0');
        CHECK (std::string (r.data()) == "C#maj7");

        CHECK (fxme::StringRef().isEmpty());
        CHECK (fxme::StringRef ("").isEmpty());
        CHECK (fxme::StringRef ("x").isNotEmpty());
        CHECK (fxme::StringRef (nullptr).isEmpty());   // must not crash

        CHECK (fxme::StringRef ("abc") == fxme::StringRef ("abc"));
        CHECK (fxme::StringRef ("abc") != fxme::StringRef ("abd"));
        CHECK (fxme::StringRef ("ab")  != fxme::StringRef ("abc"));
        CHECK (fxme::StringRef ("ab")  <  fxme::StringRef ("abc"));
        CHECK (fxme::StringRef ("abc") <  fxme::StringRef ("abd"));
        CHECK (! (fxme::StringRef ("abc") < fxme::StringRef ("abc")));
        CHECK (fxme::StringRef ("abc").str() == "abc");
    }

    // ---- StringUtils: same answers the framework's own methods gave ----------
    {
        CHECK (fxme::trim ("  hello  ") == "hello");
        CHECK (fxme::trim ("\t\r\n x \n") == "x");
        CHECK (fxme::trim ("   ") == "");
        CHECK (fxme::trim ("") == "");
        CHECK (fxme::trim ("no-padding") == "no-padding");

        CHECK (fxme::toLower ("C#M7") == "c#m7");
        CHECK (fxme::toUpper ("c#m7") == "C#M7");
        CHECK (fxme::toLower ("") == "");

        CHECK (fxme::endsWith ("CM7", "M7"));
        CHECK (! fxme::endsWith ("CM7", "m7"));
        CHECK (fxme::endsWith ("CM7", ""));
        CHECK (! fxme::endsWith ("7", "M7"));       // suffix longer than subject
        CHECK (fxme::startsWith ("CM7", "C"));
        CHECK (! fxme::startsWith ("CM7", "D"));

        CHECK (fxme::containsOnly ("-123", "-0123456789"));
        CHECK (! fxme::containsOnly ("1a3", "-0123456789"));
        CHECK (fxme::containsOnly ("", "abc"));      // vacuously, as before

        CHECK (fxme::dropLast ("CM7", 2) == "C");
        CHECK (fxme::dropLast ("CM7", 0) == "CM7");
        CHECK (fxme::dropLast ("CM7", 3) == "");
        CHECK (fxme::dropLast ("CM7", 9) == "");     // over-drop clamps

        CHECK (fxme::substring ("c#4", 0, 2) == "c#");
        CHECK (fxme::substring ("c#4", 2)    == "4");
        CHECK (fxme::substring ("c#4", 0, 9) == "c#4");   // end clamps
        CHECK (fxme::substring ("c#4", 9)    == "");
        CHECK (fxme::substring ("c#4", 2, 1) == "");      // inverted range

        // getIntValue semantics: leading signed digits, junk ignored, 0 if none.
        CHECK (fxme::toInt ("4")    == 4);
        CHECK (fxme::toInt ("-1")   == -1);
        CHECK (fxme::toInt ("+3")   == 3);
        CHECK (fxme::toInt ("12ab") == 12);
        CHECK (fxme::toInt ("ab")   == 0);
        CHECK (fxme::toInt ("")     == 0);
    }

    // ---- ArrayView: converts from both container shapes ----------------------
    {
        auto sum = [] (fxme::ArrayView<int> v)
        {
            int t = 0;
            for (int x : v) t += x;
            return t;
        };

        std::vector<int> vec { 1, 2, 3, 4 };
        FakeJuceArray arr { { 5, 6, 7 } };

        CHECK (sum (vec) == 10);
        CHECK (sum (arr) == 18);

        fxme::ArrayView<int> v (vec);
        CHECK (v.size() == 4);
        CHECK (! v.isEmpty());
        CHECK (v[0] == 1 && v[3] == 4);
        CHECK (v.getFirst() == 1);
        CHECK (v.getLast() == 4);
        CHECK (v.getUnchecked (2) == 3);
        CHECK (v.contains (3));
        CHECK (! v.contains (9));
        CHECK (v.indexOf (3) == 2);
        CHECK (v.indexOf (9) == -1);   // -1, as the framework array returned

        std::vector<int> none;
        CHECK (fxme::ArrayView<int> (none).isEmpty());
        CHECK (fxme::ArrayView<int>().isEmpty());

        // Copying a view must not go through the container conversion.
        fxme::ArrayView<int> copy (v);
        CHECK (copy.size() == 4 && copy.data() == v.data());
    }

    // ---- MidiTools: the behaviour the framework version had -------------------
    {
        using namespace fxme::MidiTools;

        CHECK (getNoteName (60) == "C4");
        CHECK (getNoteName (61) == "C#4");
        CHECK (getNoteName (0)  == "C-1");
        CHECK (getNoteName (127) == "G9");
        CHECK (getNoteName (-1) == "Invalid");
        CHECK (getNoteName (128) == "Invalid");

        CHECK (getNoteNumber ("C4")  == 60);
        CHECK (getNoteNumber ("c4")  == 60);
        CHECK (getNoteNumber (" C4 ") == 60);
        CHECK (getNoteNumber ("C#4") == 61);
        CHECK (getNoteNumber ("Db4") == 61);
        CHECK (getNoteNumber ("C-1") == 0);
        CHECK (getNoteNumber ("G9")  == 127);
        CHECK (getNoteNumber ("G#9") == -1);    // out of MIDI range
        CHECK (getNoteNumber ("H4")  == -1);    // not a note
        CHECK (getNoteNumber ("C")   == -1);    // no octave
        CHECK (getNoteNumber ("Cx")  == -1);    // junk octave
        CHECK (getNoteNumber ("")    == -1);

        CHECK (isNoteEqual (60, "C"));
        CHECK (isNoteEqual (72, "c"));
        CHECK (isNoteEqual (61, "Db"));
        CHECK (! isNoteEqual (60, "D"));
        CHECK (! isNoteEqual (60, "H"));
        CHECK (! isNoteEqual (-1, "C"));

        CHECK (getRootNoteFromChord ("CM7") == 0);
        CHECK (getRootNoteFromChord ("Am")  == 9);
        CHECK (getRootNoteFromChord ("F#7") == 6);
        CHECK (getRootNoteFromChord ("Eb5") == 3);
        CHECK (getRootNoteFromChord ("")    == 0);
        CHECK (getRootNoteFromChord ("???") == 0);   // falls back to C

        // Chord parsing: the seven suffix branches, by their degrees.
        {
            Chord cM7 ("CM7");
            CHECK (cM7.getName() == "CM7");
            CHECK (cM7.getDegrees().size() == 7);
            CHECK (cM7.getDegree (0) == 0);
            CHECK (cM7.getDegree (1) == 4);
            CHECK (cM7.getDegree (2) == 7);
            CHECK (cM7.getDegree (3) == 11);
            CHECK (cM7.getDegree (4) == -1);
            CHECK (cM7.getDegree (99) == -1);   // out of range

            CHECK (Chord ("Cm7").getDegree (1) == 3);
            CHECK (Chord ("Cm7").getDegree (3) == 10);
            CHECK (Chord ("C7").getDegree (1)  == 4);
            CHECK (Chord ("C7").getDegree (3)  == 10);
            CHECK (Chord ("C5").getDegree (1)  == -1);
            CHECK (Chord ("C5").getDegree (2)  == 7);
            CHECK (Chord ("Cm").getDegree (1)  == 3);
            CHECK (Chord ("CM").getDegree (1)  == 4);
            CHECK (Chord ("C").getDegree (0)   == 0);
            CHECK (Chord ("C").getDegree (1)   == -1);

            // Unparseable names leave every degree absent.
            CHECK (Chord ("").getDegree (0) == -1);
            CHECK (Chord ("Hm7").getDegree (0) == -1);

            // A framework-shaped string still constructs one, unchanged.
            FakeJuceString name { "F#m7" };
            CHECK (Chord (name).getDegree (0) == 6);
        }

        // getSortedSet: sorted, duplicate-free, absent degrees dropped.
        {
            Chord c ("CM7");
            auto set = c.getSortedSet();
            CHECK (set.size() == 4);
            CHECK (set[0] == 0 && set[1] == 4 && set[2] == 7 && set[3] == 11);
        }

        // setDegreesByArray takes either container shape and normalises octaves.
        {
            Chord c ("");
            std::vector<int> notes { 60, 64, 67 };      // C E G
            c.setDegreesByArray (notes);
            CHECK (c.getName() == "Custom");
            CHECK (c.getDegree (0) == 0);
            CHECK (c.getDegree (1) == 4);
            CHECK (c.getDegree (2) == 7);
            CHECK (c.getDegree (3) == -1);

            FakeJuceArray arr { { 67, 60, 64 } };       // same chord, out of order
            Chord d ("");
            d.setDegreesByArray (arr);
            CHECK (d.getDegree (0) == 0 && d.getDegree (1) == 4 && d.getDegree (2) == 7);

            // Duplicates collapse, as the sorted set did.
            std::vector<int> dupes { 60, 72, 64 };
            Chord e ("");
            e.setDegreesByArray (dupes);
            CHECK (e.getDegree (0) == 0 && e.getDegree (1) == 4 && e.getDegree (2) == -1);

            Chord f ("CM7");
            f.setDegreesByArray (std::vector<int>{});
            CHECK (f.getDegree (0) == -1);
        }

        // setNotesByArray keeps the octave and sorts.
        {
            Chord c ("");
            std::vector<int> notes { 67, 60, 64 };
            c.setNotesByArray (notes);
            auto raw = c.getRawNotes();
            CHECK (raw.size() == 3);
            CHECK (raw[0] == 60 && raw[1] == 64 && raw[2] == 67);
        }

        // isChordEqual: octave and inversion do not matter.
        {
            FakeJuceArray held { { 60, 64, 67 } };
            CHECK (isChordEqual (held, "CM"));
            CHECK (! isChordEqual (held, "Cm"));
            // A bare name is a single note, not a triad — the parser's
            // long-standing behaviour, kept deliberately.
            CHECK (! isChordEqual (held, "C"));

            FakeJuceArray inverted { { 64, 67, 72 } };
            CHECK (isChordEqual (inverted, "CM"));

            FakeJuceArray empty { {} };
            CHECK (! isChordEqual (empty, "CM"));
            CHECK (! isChordEqual (held, ""));
            CHECK (! isChordEqual (held, "  "));
        }

        // Scale: the intervals, and the name list matching the enum.
        {
            Scale major ("C", Scale::Type::Major);
            auto n = major.getNotes();
            CHECK (n.size() == 7);
            CHECK (n[0] == 0 && n[1] == 2 && n[2] == 4 && n[3] == 5);
            CHECK (n[4] == 7 && n[5] == 9 && n[6] == 11);
            CHECK (major.getRootNote() == 0);

            Scale aMinor ("A", Scale::Type::Aeolian);
            CHECK (aMinor.getNotes()[0] == 9);
            CHECK (aMinor.getRootNote() == 9);

            Scale byNumber (69, Scale::Type::Aeolian);   // A4, octave ignored
            CHECK (byNumber.getNotes()[0] == 9);

            // An unknown root name falls back to C rather than failing.
            CHECK (Scale ("H", Scale::Type::Major).getRootNote() == 0);

            // Non-7-note scales keep their own length.
            CHECK (Scale (0, Scale::Type::MajorPentatonic).getNotes().size() == 5);
            CHECK (Scale (0, Scale::Type::Blues).getNotes().size() == 6);
            CHECK (Scale (0, Scale::Type::OctatonicHalfWhole).getNotes().size() == 8);

            // reset() reuses the storage and gives the same answer.
            Scale s (0, Scale::Type::Major);
            s.reset (2, Scale::Type::Dorian);
            CHECK (s.getRootNote() == 2);
            CHECK (s.getNotes().size() == 7);
            CHECK (s.getNotes()[0] == 2);

            // The name list is the framework string list's replacement.
            CHECK (Scale::numScaleTypes == 32);
            CHECK (std::string (Scale::scaleTypeNames[0]) == "Major (Ionian)");
            CHECK (std::string (Scale::scaleTypeNames[Scale::numScaleTypes - 1])
                     == "Octatonic (Whole-Half)");
            CHECK (std::string (Scale::scaleTypeNames[(int) Scale::Type::Aeolian]) == "Aeolian");
        }

        // Chord from a scale degree.
        {
            Scale cMajor (0, Scale::Type::Major);
            auto triad = Chord::fromScaleAndDegree (cMajor, 0, true);
            CHECK (triad.getDegree (0) == 0);
            CHECK (triad.getDegree (1) == 4);
            CHECK (triad.getDegree (2) == 7);
            CHECK (triad.getDegree (3) == 11);

            // Non-chord mode gives the scale itself, voiced above the root.
            auto run = Chord::fromScaleAndDegree (cMajor, 1, false);
            CHECK (run.getDegrees().size() == 7);
            CHECK (run.getDegree (0) == 2);

            // The in-place form must agree with the returning one.
            Chord inPlace ("");
            inPlace.ensureCapacity (8, 16);
            inPlace.setFromScaleAndDegree (cMajor, 0, true);
            CHECK (inPlace.getName() == "Diatonic");
            for (int i = 0; i < 7; ++i)
                CHECK (inPlace.getDegree (i) == triad.getDegree (i));

            inPlace.reset();
            CHECK (inPlace.getDegrees().size() == 7);
            CHECK (inPlace.getDegree (0) == -1);
            CHECK (inPlace.getName().empty());
        }

        // French names.
        {
            CHECK (getFrenchNoteName ("C")  == "Do");
            CHECK (getFrenchNoteName ("c#") == "Do#");
            CHECK (getFrenchNoteName ("A")  == "La");
            CHECK (getFrenchNoteName ("Db") == "Do#");
            CHECK (getFrenchNoteName ("H").empty());

            CHECK (getFrenchChordName ("CM7") == "DoM7");
            CHECK (getFrenchChordName ("Am7") == "Lam7");
            CHECK (getFrenchChordName ("G7")  == "Sol7");
            CHECK (getFrenchChordName ("G5")  == "Sol5");
            CHECK (getFrenchChordName ("Am")  == "Lam");
            CHECK (getFrenchChordName ("CM")  == "DoM");
            CHECK (getFrenchChordName ("C")   == "Do");
            CHECK (getFrenchChordName ("")    == "");
            CHECK (getFrenchChordName ("Hm")  == "Hm");   // unparseable: unchanged

            // The French map is still keyed as it was.
            CHECK (getFrenchNoteNameOffsetMap().at ("sol") == 7);
            CHECK (getFrenchNoteNameOffsetMap().at ("re")  == 2);
        }

        // Euclidean rhythms: the classic distributions.
        {
            auto pattern = [] (int hits, int steps, int rot)
            {
                std::string s;
                for (char b : euclidianRythm (hits, steps, rot))
                    s += (b ? '1' : '.');
                return s;
            };

            CHECK (pattern (4, 16, 0).size() == 16);
            CHECK (pattern (0, 8, 0)  == "........");
            CHECK (pattern (8, 8, 0)  == "11111111");
            CHECK (pattern (99, 8, 0) == "11111111");   // hits clamp to steps
            CHECK (pattern (4, 8, 0)  == "1.1.1.1.");
            CHECK (pattern (3, 8, 0)  == "1..1..1.");

            // Rotation shifts the same pattern, keeping the hit count.
            auto count = [] (int hits, int steps, int rot)
            {
                int n = 0;
                for (char b : euclidianRythm (hits, steps, rot))
                    n += (b ? 1 : 0);
                return n;
            };
            CHECK (count (3, 8, 2) == 3);
            CHECK (count (3, 8, -2) == 3);
            CHECK (pattern (3, 8, 1) == ".1..1..1");

            CHECK (euclidianRythm (4, 0, 0).empty());
            CHECK (euclidianRythm (4, -1, 0).empty());
        }

        // The random generators must produce names the parsers accept.
        {
            bool allValid = true;
            for (int i = 0; i < 200; ++i)
            {
                if (Chord (getRandomChordName()).getDegree (0) == -1)      allValid = false;
                if (Chord (getRandomSeventhChord()).getDegree (3) == -1)   allValid = false;
                if (Chord (getRandomSingleNoteName()).getDegree (0) == -1) allValid = false;
                if (Chord (getRandomFifthInterval()).getDegree (2) == -1)  allValid = false;
            }
            CHECK (allValid);
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
