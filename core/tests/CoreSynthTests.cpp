/*
  ------------------------------------------------------------------------------
    CoreSynthTests.cpp

    The synth building blocks in core/FxmeTools/synth and the DSP added with
    them (MultiModeFilter, LookaheadLimiter, MinMaxPyramid):

      1. WavetableBuilder: a range holding one cycle of a sine gives a sine
         table at the source's amplitude, with no DC; every mip level keeps
         only its harmonics; overlap makes a wrap continuous.
      2. Period detection finds a fractional period within a tenth of a
         sample; period snap makes a whole number of periods.
      3. WavetableCache: a request is built by service(), acquired, shared,
         never recycled while held, recycled when released.
      4. WavetableReader: every interpolation reads a sine table within a
         small error.
      5. CurveAdsr: segment times, sustain, release to idle, retrigger from
         the current level.
      6. BreakpointCurve: sustain loop stays in its region; release plays to
         the end; no release region keeps looping.
      7. VoiceAllocator: idle voices first, released stolen before held,
         mono note stack, legato glides.
      8. ModGraph: refuses a cycle, accepts feedback, orders nodes.
      9. MultiModeFilter: low-pass passes DC and attenuates far above cutoff;
         every type stays finite under modulation.
     10. LookaheadLimiter: output never above the ceiling.
     11. MinMaxPyramid: ranges agree with a brute-force scan.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/dsp/LookaheadLimiter.h>
#include <FxmeTools/dsp/MinMaxPyramid.h>
#include <FxmeTools/dsp/MultiModeFilter.h>
#include <FxmeTools/synth/BreakpointCurve.h>
#include <FxmeTools/synth/MipmappedBuffer.h>
#include <FxmeTools/synth/SourceAnalysis.h>
#include <FxmeTools/synth/CurveAdsr.h>
#include <FxmeTools/synth/ModGraph.h>
#include <FxmeTools/synth/ModulationLfo.h>
#include <FxmeTools/synth/VoiceAllocator.h>
#include <FxmeTools/synth/WavetableBuilder.h>
#include <FxmeTools/synth/WavetableCache.h>
#include <FxmeTools/synth/WavetableReader.h>
#include <FxmeTools/util/Random.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

namespace
{
    constexpr double pi = 3.141592653589793238;

    std::shared_ptr<fxme::WavetableSource> makeSource (int length, double period, int shape)
    {
        auto s = std::make_shared<fxme::WavetableSource>();
        s->channels.resize (1);
        s->channels[0].resize ((size_t) length);
        s->numSamples = length;
        for (int i = 0; i < length; ++i)
        {
            const double ph = std::fmod ((double) i / period, 1.0);
            float v = 0.0f;
            if (shape == 0)      v = (float) (0.5 * std::sin (2.0 * pi * ph));
            else if (shape == 1) v = (float) (0.5 * (2.0 * ph - 1.0));
            s->channels[0][(size_t) i] = v;
        }
        return s;
    }

    float maxAbs (const float* t, int n) { float m = 0; for (int i = 0; i < n; ++i) m = std::max (m, std::abs (t[i])); return m; }
    double mean (const float* t, int n) { double s = 0; for (int i = 0; i < n; ++i) s += t[i]; return s / n; }
}

static void testBuilder()
{
    std::printf ("WavetableBuilder\n");
    auto src = makeSource (48000, 400.0, 0);
    fxme::WavetableBuilder builder;
    fxme::WavetableSet set;
    set.allocate();

    fxme::WavetableBuildSettings s;
    s.centre = 10000.0 + 200.0;
    s.width = 400.0;
    builder.build (*src, s, set);

    const float* t0 = set.table (0, 0);
    check (std::abs (maxAbs (t0, 2048) - 0.5f) < 0.01f, "one sine cycle gives a sine table at the source amplitude");
    check (std::abs (mean (t0, 2048)) < 1.0e-4, "no DC");

    double err = 0.0;
    const double phase0 = std::atan2 ((double) t0[0], (double) t0[512]);   // sine at quarter turns
    for (int i = 0; i < 2048; ++i)
        err = std::max (err, std::abs ((double) t0[i] - 0.5 * std::sin (2.0 * pi * i / 2048.0 + phase0)));
    check (err < 0.01, "level 0 is a pure sine (one harmonic)");

    const float* t10 = set.table (0, 10);
    check (std::abs (maxAbs (t10, 2048) - 0.5f) < 0.01f, "level 10 (fundamental only) keeps the sine");
    check (t0[-1] == t0[2047] && t0[2048] == t0[0], "guard samples wrap");

    // A saw over 4 periods: harmonic content above level limits is removed.
    auto saw = makeSource (48000, 100.0, 1);
    s.centre = 5000.0;
    s.width = 1600.0;
    builder.build (*saw, s, set);
    check (maxAbs (set.table (0, 0), 2048) > 0.3f, "saw range builds");

    // Overlap: the wrap of a range that does not hold whole cycles is
    // continuous once overlapped.
    s.centre = 5000.0;
    s.width = 950.0;
    s.overlap = 0.2f;
    builder.build (*src, s, set);
    const float* t = set.table (0, 3);
    check (std::abs (t[2047] - t[0]) < 0.05f, "overlap closes the loop");
}

static void testAlignment()
{
    std::printf ("Phase alignment\n");
    // A saw of period 400: ranges one period wide, at centres 37 samples
    // apart, hold the same cycle rotated. Aligned, their tables match.
    auto saw = makeSource (48000, 400.0, 1);
    fxme::WavetableBuilder builder;
    fxme::WavetableSet a, b;
    a.allocate();
    b.allocate();
    fxme::WavetableBuildSettings s;
    s.width = 400.0;
    s.centre = 10000.0;
    builder.build (*saw, s, a);
    s.centre = 10037.0;
    builder.build (*saw, s, b);
    double err = 0.0, peak = 0.0;
    for (int i = 0; i < 2048; ++i)
    {
        err = std::max (err, (double) std::abs (a.table (0, 3)[i] - b.table (0, 3)[i]));
        peak = std::max (peak, (double) std::abs (a.table (0, 3)[i]));
    }
    check (err < 0.02 * peak, "rotated content gives the same aligned table");
    check (std::abs (std::fmod (a.alignment - b.alignment + 10.0, 1.0) - 37.0 / 400.0) < 0.002
               || std::abs (std::fmod (b.alignment - a.alignment + 10.0, 1.0) - (1.0 - 37.0 / 400.0)) < 0.002,
           "alignment records the rotation");

    const int g = 1;
    const auto target = fxme::WavetableCache::makeKey (g, 10000, 400, 0.0f, 1, 0);
    const auto near = fxme::WavetableCache::makeKey (g, 10010, 400, 0.0f, 1, 0);
    const auto far = fxme::WavetableCache::makeKey (g, 10200, 400, 0.0f, 1, 0);
    const auto old = fxme::WavetableCache::makeKey (g + 1, 10000, 400, 0.0f, 1, 0);
    check (fxme::WavetableCache::distance (near, target) < fxme::WavetableCache::distance (far, target)
           && fxme::WavetableCache::distance (far, target) < fxme::WavetableCache::distance (old, target),
           "key distance: nearer position first, another source last");
}

static void testPyramid2()
{
    std::printf ("MipmappedBuffer and RangeLoop\n");
    // White noise: reading it 8 times faster from the right level must not
    // put the energy of the folded-back band into the output. Compare the
    // level read at speed 8 with plain decimation-free reading at speed 8.
    const int n = 1 << 17;
    std::vector<float> noise ((size_t) n);
    fxme::Random rnd (11);
    for (auto& v : noise) v = rnd.nextBipolar();
    const float* ch[] = { noise.data() };
    fxme::MipmappedBuffer b;
    b.build (ch, 1, n);
    check (b.getNumLevels() >= 10, "levels down to a few dozen samples");

    using I = fxme::MipmappedBuffer::Interpolation;
    const double speed = 8.0;
    const float level = b.levelForSpeed (speed);
    check (level > 3.0f && level < 4.0f, "speed 8 reads between levels 3 and 4");
    double eFiltered = 0.0, eRaw = 0.0;
    for (int i = 0; i < 8000; ++i)
    {
        const double pos = 1000.0 + speed * i;
        const float f = b.readBlended (0, level, pos, I::cubic);
        const float r = b.read (0, 0, pos, I::cubic);
        eFiltered += f * f;
        eRaw += r * r;
    }
    // Noise is flat: the band that fits at speed 8 is about 1/8 to 1/16 of it.
    check (eFiltered < 0.2 * eRaw, "fast reading keeps only the band that fits (no aliasing)");

    // Level k sample m sits at level-0 position m * 2^k: a slow sine reads
    // the same from every level.
    std::vector<float> sine ((size_t) n);
    for (int i = 0; i < n; ++i) sine[(size_t) i] = (float) std::sin (2.0 * pi * i / 4096.0);
    const float* sch[] = { sine.data() };
    fxme::MipmappedBuffer sb;
    sb.build (sch, 1, n);
    double err = 0.0;
    for (int k = 0; k < 5; ++k)
        for (int i = 0; i < 200; ++i)
        {
            const double pos = 20000.0 + 37.3 * i;
            err = std::max (err, std::abs ((double) sb.read (0, k, pos, I::cubic) - std::sin (2.0 * pi * pos / 4096.0)));
        }
    check (err < 0.01, "levels are aligned with level 0");

    // RangeLoop: with an overlap the wrap is continuous.
    fxme::RangeLoop::Range r { 30000.0, 1500.0, 300.0, true };
    const float before = fxme::RangeLoop::read (sb, 0, 0.0f, r, 0.99999, I::cubic);
    const float after = fxme::RangeLoop::read (sb, 0, 0.0f, r, 0.0, I::cubic);
    check (std::abs (before - after) < 0.01f, "overlapped loop wraps without a step");
}

static void testAnalysis()
{
    std::printf ("SourceAnalysis\n");
    auto saw = makeSource (48000, 123.4, 1);
    const float* ch[] = { saw->channels[0].data() };
    fxme::SourceAnalysis a;
    a.build (ch, 1, saw->numSamples);
    check (std::abs (a.periodAt (20000.0) - 123.4) < 0.15, "period track finds the period");
    const double z = a.nearestRisingZeroCrossing (10000.0, 200.0);
    const int zi = (int) z;
    check (zi > 0 && saw->channels[0][(size_t) zi - 1] < 0.0f && saw->channels[0][(size_t) zi] >= 0.0f
           && std::abs (z - 10000.0) <= 62.0, "nearest rising zero crossing");
}

static void testPeriod()
{
    std::printf ("Period detection\n");
    auto src = makeSource (48000, 123.4, 1);
    fxme::WavetableBuilder builder;
    const double p = builder.detectPeriod (*src, 20000);
    check (std::abs (p - 123.4) < 0.1, "fractional period within a tenth of a sample");

    fxme::WavetableSet set;
    set.allocate();
    fxme::WavetableBuildSettings s;
    s.centre = 20000.0;
    s.width = 500.0;
    s.snap = fxme::WavetableBuildSettings::Snap::period;
    builder.build (*src, s, set);
    const double periods = builder.getLastLength() / 123.4;
    check (std::abs (periods - std::round (periods)) < 0.02, "period snap gives whole periods");
}

static void testCache()
{
    std::printf ("WavetableCache\n");
    fxme::WavetableCache cache;
    cache.prepare (2);
    cache.setSource (makeSource (48000, 400.0, 0));
    fxme::WavetableBuilder builder;

    const int g = cache.getGeneration();
    const auto k1 = fxme::WavetableCache::makeKey (g, 10000, 400, 0.0f, 1, 0);
    const auto k2 = fxme::WavetableCache::makeKey (g, 20000, 400, 0.0f, 1, 0);
    const auto k3 = fxme::WavetableCache::makeKey (g, 30000, 400, 0.0f, 1, 0);

    cache.beginBlock();
    check (cache.tryAcquire (k1) < 0, "nothing ready before a build");
    cache.request (k1, 0);
    check (cache.service (builder), "service builds the request");
    const int a = cache.tryAcquire (k1);
    check (a >= 0, "built key acquired");
    const int b = cache.tryAcquire (k1);
    check (b == a, "same key shared");
    cache.release (b);

    cache.request (k2, 1);
    cache.service (builder);
    const int c = cache.tryAcquire (k2);
    check (c >= 0 && c != a, "second key in the other slot");

    // Both slots held: a third key cannot evict anything.
    cache.request (k3, 2);
    check (! cache.service (builder), "no slot recycled while held");
    check (cache.tryAcquire (k1) == a, "held key still there");
    cache.release (a);
    cache.release (a);

    cache.beginBlock();
    cache.request (k3, 3);
    check (cache.service (builder), "released slot recycled");
    const int d = cache.tryAcquire (k3);
    check (d == a, "least recently used slot taken");
    const int n = cache.acquireNearest (fxme::WavetableCache::makeKey (g, 20100, 400, 0.0f, 1, 0));
    check (n == c, "nearest ready key found");
    cache.release (n);
    cache.release (c);
    cache.release (d);

    // Same stream asking twice: only the latest is built.
    cache.beginBlock();
    cache.request (fxme::WavetableCache::makeKey (g, 1000, 400, 0.0f, 1, 0), 7);
    cache.request (fxme::WavetableCache::makeKey (g, 2000, 400, 0.0f, 1, 0), 7);
    cache.service (builder);
    check (cache.tryAcquire (fxme::WavetableCache::makeKey (g, 1000, 400, 0.0f, 1, 0)) < 0
           && cache.tryAcquire (fxme::WavetableCache::makeKey (g, 2000, 400, 0.0f, 1, 0)) >= 0,
           "latest request per stream wins");
}

static void testReader()
{
    std::printf ("WavetableReader\n");
    fxme::WavetableSet set;
    set.allocate();
    for (int l = 0; l < fxme::WavetableSet::numLevels; ++l)
    {
        for (int i = 0; i < fxme::WavetableSet::tableSize; ++i)
            set.table (0, l)[i] = (float) std::sin (2.0 * pi * i / fxme::WavetableSet::tableSize);
        set.fillGuards (0, l);
    }
    using I = fxme::WavetableReader::Interpolation;
    for (auto mode : { I::linear, I::cubic, I::sinc })
    {
        double err = 0.0;
        for (int k = 0; k < 10000; ++k)
        {
            const double ph = (k + 0.37) / 10000.0;
            err = std::max (err, std::abs ((double) fxme::WavetableReader::read (set.table (0, 0), ph, mode)
                                           - std::sin (2.0 * pi * ph)));
        }
        check (err < 1.0e-4,
               mode == I::linear ? "linear read" : (mode == I::cubic ? "cubic read" : "sinc read"));
    }
    check ((int) fxme::WavetableSet::levelFor (20.0, 48000.0) == 0, "low notes read level 0");
    check (fxme::WavetableSet::levelFor (4186.0, 48000.0) >= 7.0f, "C8 reads level 7 or above");
}

static void testAdsr()
{
    std::printf ("CurveAdsr\n");
    fxme::CurveAdsr env;
    env.setSampleRate (1000.0);
    fxme::CurveAdsr::Parameters p;
    p.attack = 0.01f; p.decay = 0.02f; p.sustain = 0.5f; p.release = 0.01f;
    env.setParameters (p);
    env.noteOn();
    float v = 0.0f;
    for (int i = 0; i < 10; ++i) v = env.getNextSample();
    check (std::abs (v - 1.0f) < 1.0e-4f, "attack reaches 1 after its time");
    for (int i = 0; i < 25; ++i) v = env.getNextSample();
    check (std::abs (v - 0.5f) < 1.0e-4f && env.getState() == fxme::CurveAdsr::State::sustain, "decay to sustain");
    env.noteOff();
    for (int i = 0; i < 11; ++i) v = env.getNextSample();
    check (! env.isActive() && v == 0.0f, "release to idle");

    env.noteOn();
    env.advance (5);
    const float mid = env.getValue();
    env.noteOn();
    check (std::abs (env.getValue() - mid) < 1.0e-6f, "retrigger starts from the current level");
    env.advance (1000);
    check (std::abs (env.getValue() - 0.5f) < 1.0e-4f, "control-rate advance crosses segments");
}

static void testCurve()
{
    std::printf ("BreakpointCurve\n");
    fxme::CurveShape s;
    s.numPoints = 4;
    s.x = { 0.0f, 0.25f, 0.5f, 1.0f };
    s.y = { 0.0f, 1.0f, 0.5f, 0.0f };
    s.sustainEnabled = true;
    s.sustainStart = 1;
    s.sustainEnd = 2;
    s.sustainLoop = true;
    s.releaseEnabled = true;
    s.sanitise();

    fxme::SharedCurveShape shared;
    shared.write (s);
    fxme::CurveShape copy;
    std::uint32_t seen = 0;
    check (shared.readIfChanged (copy, seen) && copy.numPoints == 4 && copy.sustainEnd == 2, "shape handed over");
    check (! shared.readIfChanged (copy, seen), "no copy when unchanged");

    fxme::CurvePlayer pl;
    pl.noteOn();
    bool inRegion = true;
    for (int i = 0; i < 200; ++i)
    {
        pl.advance (copy, 0.013);
        if (i > 30 && (pl.getPosition() < 0.25 - 1e-9 || pl.getPosition() > 0.5 + 1e-9))
            inRegion = false;
    }
    check (inRegion, "sustain loop stays between its points");
    pl.noteOff (copy);
    check (pl.isReleasing(), "note-off starts the release");
    float v = 1.0f;
    for (int i = 0; i < 100; ++i)
        v = pl.advance (copy, 0.013);
    check (pl.isFinished() && std::abs (v) < 1.0e-6f, "release plays to the end");

    copy.releaseEnabled = false;
    pl.noteOn();
    pl.advance (copy, 0.3);
    pl.noteOff (copy);
    check (! pl.isReleasing(), "without release, note-off does nothing");
}

static void testAllocator()
{
    std::printf ("VoiceAllocator\n");
    using A = fxme::VoiceAllocator::Action;
    fxme::VoiceAllocator va;
    va.setPolyphony (2);
    A out[fxme::VoiceAllocator::maxActions];

    va.noteOn (60, 1.0f, out); const int v60 = out[0].voice;
    va.noteOn (62, 1.0f, out); const int v62 = out[0].voice;
    check (out[0].type == A::Type::start && v60 != v62, "idle voices first");
    va.noteOff (60, 0.0f, out);
    check (out[0].type == A::Type::release && out[0].voice == v60, "release");
    va.noteOn (64, 1.0f, out);
    check (out[0].type == A::Type::steal && out[0].voice == v60, "released voice stolen before a held one");
    va.noteOn (65, 1.0f, out);
    check (out[0].type == A::Type::steal && out[0].voice == v62, "then the oldest held");

    va.reset();
    va.setMode (fxme::VoiceAllocator::Mode::legato);
    va.noteOn (60, 1.0f, out);
    check (out[0].type == A::Type::start, "legato: first note starts");
    va.noteOn (64, 1.0f, out);
    check (out[0].type == A::Type::glide && out[0].legato, "legato: overlapping note glides");
    const int n = va.noteOff (64, 0.0f, out);
    check (n == 1 && out[0].type == A::Type::glide && out[0].note == 60, "back to the held note");
    va.noteOff (60, 0.0f, out);
    check (out[0].type == A::Type::release, "last note releases");

    va.reset();
    va.setMode (fxme::VoiceAllocator::Mode::mono);
    va.noteOn (60, 1.0f, out);
    va.noteOn (67, 1.0f, out);
    check (out[0].type == A::Type::retrigger, "mono retriggers");
}

static void testGraph()
{
    std::printf ("ModGraph\n");
    fxme::ModGraph g (4);
    g.connect (0, 1);
    g.connect (1, 2);
    check (! g.canConnect (2, 0), "a cycle is refused");
    check (g.canConnect (2, 2), "feedback is allowed");
    g.connect (2, 2);
    g.connect (3, 0);
    int order[4];
    check (g.topologicalOrder (order), "order exists");
    auto pos = [&] (int n) { for (int i = 0; i < 4; ++i) if (order[i] == n) return i; return -1; };
    check (pos (3) < pos (0) && pos (0) < pos (1) && pos (1) < pos (2), "sources before their targets");
}

static void testFilter()
{
    std::printf ("MultiModeFilter\n");
    fxme::MultiModeFilter f;
    f.prepare (48000.0);
    fxme::MultiModeFilter::Parameters p;
    p.type = fxme::MultiModeFilter::Type::lowPass24;
    p.cutoffHz = 500.0f;
    f.setParameters (p);
    float y = 0.0f;
    for (int i = 0; i < 48000; ++i) y = f.processSample (1.0f);
    check (std::abs (y - 1.0f) < 1.0e-3f, "low-pass passes DC");

    f.reset();
    float peak = 0.0f;
    for (int i = 0; i < 48000; ++i)
    {
        y = f.processSample ((float) std::sin (2.0 * pi * 10000.0 * i / 48000.0));
        if (i > 4800) peak = std::max (peak, std::abs (y));
    }
    check (peak < 0.001f, "24 dB/oct low-pass: 10 kHz well down at a 500 Hz cutoff");

    bool finite = true;
    fxme::Random rnd (42);
    for (int t = 0; t < fxme::MultiModeFilter::numTypes; ++t)
    {
        p.type = (fxme::MultiModeFilter::Type) t;
        f.prepare (48000.0);
        for (int blk = 0; blk < 300; ++blk)
        {
            p.cutoffHz = 20.0f + 19000.0f * rnd.nextFloat();
            p.resonance = rnd.nextFloat();
            p.gainDb = -24.0f + 48.0f * rnd.nextFloat();
            p.vowelPosition = rnd.nextFloat();
            f.setParameters (p);
            for (int i = 0; i < 32; ++i)
                if (! std::isfinite (f.processSample (rnd.nextBipolar())))
                    finite = false;
        }
    }
    check (finite, "every type stays finite under random modulation");
}

static void testLimiter()
{
    std::printf ("LookaheadLimiter\n");
    fxme::LookaheadLimiter lim;
    lim.prepare (48000.0, 1.0f, 50.0f);
    lim.setCeilingDb (-0.3f);
    const float ceiling = std::pow (10.0f, -0.3f / 20.0f);
    std::vector<float> l (4096), r (4096);
    fxme::Random rnd (7);
    float worst = 0.0f;
    for (int blk = 0; blk < 50; ++blk)
    {
        for (int i = 0; i < 4096; ++i)
        {
            const float burst = (blk % 3 == 0) ? 8.0f : 0.5f;
            l[(size_t) i] = burst * rnd.nextBipolar();
            r[(size_t) i] = burst * rnd.nextBipolar();
        }
        lim.process (l.data(), r.data(), 4096);
        worst = std::max ({ worst, maxAbs (l.data(), 4096), maxAbs (r.data(), 4096) });
    }
    check (worst <= ceiling + 1.0e-5f, "output never above the ceiling");
    check (lim.getLatencySamples() == 47, "1 ms at 48 kHz: 47 samples of latency");
}

static void testPyramid()
{
    std::printf ("MinMaxPyramid\n");
    std::vector<float> x (100000);
    fxme::Random rnd (3);
    for (auto& v : x) v = rnd.nextBipolar();
    fxme::MinMaxPyramid p;
    p.build (x.data(), (int) x.size());
    bool ok = true;
    for (int k = 0; k < 200; ++k)
    {
        const int a = rnd.nextInt (90000), b = a + 1 + rnd.nextInt (10000);
        float lo, hi;
        p.rangeAt (a, b, lo, hi);
        const auto mm = std::minmax_element (x.begin() + a, x.begin() + b);
        if (lo != *mm.first || hi != *mm.second)
            ok = false;
    }
    check (ok, "ranges match a brute-force scan");
}

static void testLfo()
{
    std::printf ("ModulationLfo\n");
    fxme::ModulationLfo lfo;
    lfo.setSampleRate (1000.0);
    fxme::ModulationLfo::Parameters p;
    p.rateHz = 1.0f;
    p.delaySeconds = 0.5f;
    p.fadeSeconds = 0.5f;
    lfo.setParameters (p);
    lfo.noteOn();
    check (lfo.advance (400) == 0.0f, "silent during the delay");
    lfo.advance (350);
    const float v = lfo.output();
    check (std::abs (v) <= 0.5f + 1e-4f, "fading in");
    p.shape = fxme::ModulationLfo::sampleAndHold;
    p.delaySeconds = 0.0f; p.fadeSeconds = 0.0f;
    lfo.setParameters (p);
    lfo.noteOn();
    const float a = lfo.advance (100), b = lfo.advance (100);
    check (a == b, "S&H holds within a cycle");

    // A note-on usually comes before that block's parameters: the start
    // phase given afterwards must still be the one used.
    fxme::ModulationLfo sine;
    sine.setSampleRate (1000.0);
    fxme::ModulationLfo::Parameters q;
    q.rateHz = 1.0f;
    q.startPhase = 0.0f;
    sine.setParameters (q);
    sine.noteOn();
    sine.advance (1);
    sine.noteOn();                 // next note, phase knob now at a quarter turn
    q.startPhase = 0.25f;
    sine.setParameters (q);
    const float first = sine.advance (10);
    check (std::abs (first - (float) std::sin (2.0 * pi * (0.25 + 0.01))) < 1.0e-4f,
           "start phase set after the note-on is the one used");

    // Phase after many control blocks: exactly rate x time.
    float last = 0.0f;
    for (int i = 0; i < 100; ++i)
        last = sine.advance (32);
    const double expected = std::sin (2.0 * pi * (0.25 + (10.0 + 3200.0) / 1000.0));
    check (std::abs (last - (float) expected) < 1.0e-3f, "phase advances at the rate, no drift");

    // Host lock: the phase is the host position over the cycle length.
    fxme::ModulationLfo synced;
    synced.setSampleRate (48000.0);
    fxme::ModulationLfo::Parameters r;
    r.synced = true;
    r.syncBeats = 1.0f;
    synced.setParameters (r);
    synced.setBpm (120.0);
    synced.noteOn();
    synced.syncToPpq (10.25);
    check (std::abs (synced.output() - 1.0f) < 1.0e-5f, "host-locked phase from the PPQ (a quarter beat in: peak)");
}

int main()
{
    testBuilder();
    testPeriod();
    testPyramid2();
    testAnalysis();
    testAlignment();
    testCache();
    testReader();
    testAdsr();
    testCurve();
    testAllocator();
    testGraph();
    testFilter();
    testLimiter();
    testPyramid();
    testLfo();

    std::printf (failures == 0 ? "All synth tests passed\n" : "%d synth test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
