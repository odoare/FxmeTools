/*
    Unit tests for the modulation-effect kernels:
    dsp/ModDelayLine.h, dsp/AllpassChain.h, dsp/ModLfo.h (and the sync-division
    table Lfo.h grew for them).
*/

#include <JuceHeader.h>
#include <doctest/doctest.h>

#include <FxmeTools/dsp/AllpassChain.h>
#include <FxmeTools/dsp/ModDelayLine.h>
#include <FxmeTools/dsp/ModLfo.h>

#include <cmath>

TEST_CASE ("ModDelayLine reads integer delays exactly")
{
    fxme::ModDelayLine line;
    line.prepare (48000.0, 0.05f);   // 2400 samples

    // A ramp, so every sample is identifiable by its value.
    for (int i = 0; i < 1000; ++i)
        line.write ((float) i);

    // 1000 samples written, so the write pointer sits at 1000: a delay of d
    // must return the sample written d steps ago. Two samples back is the
    // shortest the interpolator can serve.
    CHECK (line.read (2.0f)   == doctest::Approx (998.0f));
    CHECK (line.read (10.0f)  == doctest::Approx (990.0f));
    CHECK (line.read (500.0f) == doctest::Approx (500.0f));
}

TEST_CASE ("ModDelayLine interpolates between samples and stays in range")
{
    fxme::ModDelayLine line;
    line.prepare (48000.0, 0.01f);

    for (int i = 0; i < 200; ++i)
        line.write ((float) i);

    // Catmull-Rom is exact on a straight line.
    CHECK (line.read (10.5f) == doctest::Approx (189.5f));

    // Out-of-range requests clamp rather than read past the buffer.
    CHECK (std::isfinite (line.read (0.0f)));
    CHECK (std::isfinite (line.read (-5.0f)));
    CHECK (std::isfinite (line.read (1.0e6f)));
}

TEST_CASE ("AllpassChain passes DC at unit gain whatever the stage count")
{
    fxme::AllpassChain chain;
    chain.prepare (48000.0);
    chain.setFrequency (800.0f);

    for (int stages : { 1, 2, 6, 12 })
    {
        chain.setNumStages (stages);
        chain.reset();

        float y = 0.0f;
        for (int i = 0; i < 20000; ++i)
            y = chain.process (1.0f);

        CHECK (chain.getNumStages() == stages);
        CHECK (y == doctest::Approx (1.0f).epsilon (0.001));
    }
}

TEST_CASE ("AllpassChain clamps its stage count and its frequency")
{
    fxme::AllpassChain chain;
    chain.prepare (48000.0);

    chain.setNumStages (0);
    CHECK (chain.getNumStages() == 1);

    chain.setNumStages (999);
    CHECK (chain.getNumStages() == fxme::AllpassChain::maxStages);

    // Absurd break frequencies must not produce a NaN coefficient.
    for (float hz : { -100.0f, 0.0f, 1.0e6f })
    {
        chain.setFrequency (hz);
        chain.reset();
        CHECK (std::isfinite (chain.process (0.5f)));
    }
}

TEST_CASE ("ModLfo free-running phase completes one cycle per period")
{
    fxme::ModLfo lfo;
    lfo.prepare (48000.0);
    lfo.setFrequency (1.0f);
    lfo.reset (0.0f);

    for (int i = 0; i < 48000; ++i)
        lfo.advance();

    // Back at the top of the cycle — either just short of 1 or just past 0,
    // depending on where the accumulated float rounding lands.
    const float p = lfo.phase();
    CHECK ((p < 0.01f || p > 0.99f));
}

TEST_CASE ("ModLfo derives its rate from the tempo when synced")
{
    fxme::ModLfo lfo;
    lfo.prepare (48000.0);
    lfo.setFrequency (3.0f);
    lfo.setBpm (120.0);

    CHECK (lfo.frequencyHz() == doctest::Approx (3.0f));   // free: the Hz rate

    lfo.setSynced (true);
    lfo.setSyncBeats (4.0f);                                // a whole note
    // 120 bpm = 2 beats/s, so 4 beats take 2 s.
    CHECK (lfo.frequencyHz() == doctest::Approx (0.5f));

    lfo.setSyncBeats (1.0f);                                // a quarter note
    CHECK (lfo.frequencyHz() == doctest::Approx (2.0f));
}

TEST_CASE ("ModLfo locks its phase to the host timeline")
{
    fxme::ModLfo lfo;
    lfo.prepare (48000.0);
    lfo.setSynced (true);
    lfo.setSyncBeats (4.0f);

    lfo.setPhaseFromPpq (0.0);
    CHECK (lfo.phase() == doctest::Approx (0.0f));

    lfo.setPhaseFromPpq (2.0);      // half of a four-beat cycle
    CHECK (lfo.phase() == doctest::Approx (0.5f));

    lfo.setPhaseFromPpq (12.0);     // three whole cycles in
    CHECK (lfo.phase() == doctest::Approx (0.0f));
}

TEST_CASE ("ModLfo reads offset phases for the other channel")
{
    fxme::ModLfo lfo;
    lfo.prepare (48000.0);
    lfo.setShape (fxme::Lfo::sine);
    lfo.reset (0.0f);

    CHECK (lfo.valueAt (0.0f)  == doctest::Approx (0.0f).epsilon (0.001));
    CHECK (lfo.valueAt (0.25f) == doctest::Approx (1.0f).epsilon (0.001));

    // Offsets wrap, so a half-cycle offset is the same wherever it is measured.
    CHECK (lfo.valueAt (1.5f) == doctest::Approx (lfo.valueAt (0.5f)).epsilon (0.001));
}

TEST_CASE ("Lfo sync-division table matches its choice list")
{
    const auto names = fxme::Lfo::syncDivisionChoices();
    CHECK (names.size() == 17);

    CHECK (names[fxme::Lfo::defaultSyncDivision] == "1/1");
    CHECK (fxme::Lfo::syncDivisionBeats (fxme::Lfo::defaultSyncDivision)
           == doctest::Approx (4.0f));

    CHECK (fxme::Lfo::syncDivisionBeats (0)  == doctest::Approx (32.0f));       // 8/1
    CHECK (fxme::Lfo::syncDivisionBeats (5)  == doctest::Approx (1.0f));        // 1/4
    CHECK (fxme::Lfo::syncDivisionBeats (10) == doctest::Approx (2.0f / 3.0f)); // 1/4T
    CHECK (fxme::Lfo::syncDivisionBeats (14) == doctest::Approx (1.5f));        // 1/4.

    // Out-of-range indices clamp instead of reading past the table.
    CHECK (fxme::Lfo::syncDivisionBeats (-1)   == doctest::Approx (32.0f));
    CHECK (fxme::Lfo::syncDivisionBeats (9999) == doctest::Approx (0.375f));
}
