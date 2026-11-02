/*
  ------------------------------------------------------------------------------
    CoreNetTelemetryTests.cpp

    FxmeNet telemetry (net/TelemetryCodec.h, net/TelemetryRegistry.h) and the
    remote-metering additions to VuMeter, SpectrumTap and WaveformTap.

    Each test builds a "source" registry bound to objects fed as the audio
    thread would feed them, and a "mirror" registry bound to twin objects
    that are never fed, then checks that serialise() on one and apply() on
    the other leaves the twins reading what the originals read.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/net/NetAbi.h>
#include <FxmeTools/net/TelemetryCodec.h>
#include <FxmeTools/net/TelemetryRegistry.h>

#include <atomic>
#include <cmath>
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

    using namespace fxme::net;
    using Bytes = std::vector<std::uint8_t>;

    /** source.serialise() -> mirror.apply(), as the network would. */
    bool transfer (TelemetryRegistry& source, TelemetryRegistry& mirror)
    {
        Bytes frame;
        source.serialise (frame);
        return mirror.apply (frame.data(), frame.size());
    }

    void feed (fxme::VuMeter& meter, float level, int numSamples)
    {
        std::vector<float> block ((std::size_t) numSamples, level);
        meter.process (block.data(), numSamples);
    }

    std::vector<float> ramp (int start, int count)
    {
        std::vector<float> v ((std::size_t) count);
        for (int i = 0; i < count; ++i)
            v[(std::size_t) i] = (float) (start + i);
        return v;
    }

    /** True when the last `count` samples of both taps are the same. */
    template <typename Tap>
    bool sameTail (const Tap& a, const Tap& b, int count)
    {
        std::vector<float> x ((std::size_t) count), y ((std::size_t) count);
        a.snapshot (x.data(), count);
        b.snapshot (y.data(), count);
        return x == y;
    }
}

//==============================================================================
int main()
{
    //--------------------------------------------------------------------------
    std::printf ("Codec\n");
    {
        CHECK (idOf ("a") == 0xe40c292cu);   // the published FNV-1a 32 test vector
        CHECK (idOf ("") == 2166136261u);
        CHECK (idOf ("gr") == idOf ("gr", 2));

        Bytes b;
        ByteWriter w (b);
        w.u8 (0x12);
        w.u16 (0x3456);
        w.u32 (0x789abcdeu);
        w.u64 (0x0123456789abcdefull);
        w.f32 (-1.5f);
        CHECK (b.size() == 1 + 2 + 4 + 8 + 4);
        CHECK (b[1] == 0x56 && b[2] == 0x34);   // little-endian whatever the host

        ByteReader r (b.data(), b.size());
        CHECK (r.u8() == 0x12);
        CHECK (r.u16() == 0x3456);
        CHECK (r.u32() == 0x789abcdeu);
        CHECK (r.u64() == 0x0123456789abcdefull);
        CHECK (r.f32() == -1.5f);
        CHECK (r.ok() && r.remaining() == 0);

        CHECK (r.u32() == 0);
        CHECK (! r.ok());   // sticky after a read past the end
    }

    //--------------------------------------------------------------------------
    std::printf ("Meter\n");
    {
        fxme::VuMeter meter, twin;
        meter.prepare (48000.0);
        twin.prepare (48000.0);

        TelemetryRegistry source, mirror;
        CHECK (source.addMeter ("level", meter));
        CHECK (mirror.addMeter ("level", twin));

        feed (meter, 0.5f, 4800);   // one full 100 ms window
        CHECK (transfer (source, mirror));
        CHECK_CLOSE (twin.getRMS(), meter.getRMS(), 1.0e-4);
        CHECK_CLOSE (twin.getPeak(), meter.getPeak(), 1.0e-6);
        CHECK_CLOSE (twin.getMax(), fxme::Decibels::gainToDecibels (0.5f), 1.0e-5);

        // The source's held max was taken by the frame; the mirror keeps the
        // highest one until its own editor resets it.
        CHECK (meter.takeMaxGain() == 0.0f);
        feed (meter, 0.25f, 4800);
        CHECK (transfer (source, mirror));
        CHECK_CLOSE (twin.getMax(), fxme::Decibels::gainToDecibels (0.5f), 1.0e-5);

        twin.resetMax();
        feed (meter, 0.25f, 4800);
        CHECK (transfer (source, mirror));
        CHECK_CLOSE (twin.getMax(), fxme::Decibels::gainToDecibels (0.25f), 1.0e-5);
        CHECK_CLOSE (twin.getRmsGain(), 0.25, 1.0e-4);
    }

    //--------------------------------------------------------------------------
    std::printf ("Value, scalar, peak\n");
    {
        std::atomic<float> value { 0.0f }, twinValue { 0.0f };
        float computed = 0.0f;
        float peakSinceTake = 0.0f;
        int takes = 0;

        TelemetryRegistry source, mirror;
        source.addValue ("bpm", value);
        source.addScalar ("gate", [&] { return computed; });
        source.addPeak ("band", [&] { ++takes; const float p = peakSinceTake; peakSinceTake = -200.0f; return p; });

        mirror.addValue ("bpm", twinValue);
        mirror.addScalar ("gate", [] { return 0.0f; });
        mirror.addPeak ("band", [] { return 0.0f; });

        value = 128.0f;
        computed = 0.75f;
        peakSinceTake = -6.0f;
        CHECK (transfer (source, mirror));
        CHECK (twinValue.load() == 128.0f);
        CHECK (mirror.scalar ("gate") == 0.75f);
        CHECK (takes == 1);

        // Two frames before the mirror's editor takes: it gets the higher.
        peakSinceTake = -12.0f;
        CHECK (transfer (source, mirror));
        CHECK (mirror.takePeak ("band") == -6.0f);

        // No frame since: the previous result again, not a floor.
        CHECK (mirror.takePeak ("band") == -6.0f);

        // A new frame replaces it, even when lower.
        peakSinceTake = -20.0f;
        CHECK (transfer (source, mirror));
        CHECK (mirror.takePeak ("band") == -20.0f);

        CHECK (mirror.scalar ("unknown") == 0.0f);
        CHECK (mirror.takePeak ("unknown") == 0.0f);
    }

    //--------------------------------------------------------------------------
    std::printf ("SpectrumTap\n");
    {
        fxme::SpectrumTap tap, twin;
        twin.setEnabled (true);   // the mirror's editor does this

        TelemetryRegistry source, mirror;
        source.addTap ("in", tap);
        mirror.addTap ("in", twin);

        // Inactive: the tap stays disabled and nothing is streamed.
        auto block = ramp (0, 512);
        tap.push (block.data(), 512);
        CHECK (tap.getTotalPushed() == 0);

        source.setActive (true);
        CHECK (tap.isEnabled());

        int next = 0;
        for (int b = 0; b < 10; ++b)
        {
            block = ramp (next, 480);
            tap.push (block.data(), 480);
            next += 480;
        }
        CHECK (transfer (source, mirror));
        CHECK (twin.getTotalPushed() == 4800);
        CHECK (sameTail (tap, twin, 4800));

        // More than one frame's worth: streamed over several frames, in order.
        block = ramp (next, 20000);
        tap.push (block.data(), 20000);
        CHECK (transfer (source, mirror));
        CHECK (twin.getTotalPushed() == 4800 + TelemetryRegistry::maxTapSamplesPerFrame);
        CHECK (transfer (source, mirror));
        CHECK (transfer (source, mirror));
        CHECK (twin.getTotalPushed() == 24800);
        CHECK (sameTail (tap, twin, 16384));

        source.setActive (false);
        CHECK (! tap.isEnabled());
    }

    //--------------------------------------------------------------------------
    std::printf ("readSince skips what the ring no longer holds\n");
    {
        fxme::SpectrumTap tap;
        tap.setEnabled (true);
        std::int64_t from = 0;
        const int half = fxme::spectrumMaxFftSize;   // half the ring

        auto block = ramp (0, 3 * half);
        tap.push (block.data(), 3 * half);

        std::vector<float> out (16);
        const int n = tap.readSince (from, out.data(), 16);
        CHECK (n == 16);
        CHECK (from == 2 * half + 16);
        CHECK (out[0] == (float) (2 * half));
    }

    //--------------------------------------------------------------------------
    std::printf ("WaveformTap\n");
    {
        fxme::WaveformTap tap, twin;
        tap.prepare (48000.0, 1.0);
        twin.prepare (48000.0, 1.0);
        twin.setEnabled (true);

        TelemetryRegistry source, mirror;
        source.addTap ("scope", tap);
        mirror.addTap ("scope", twin);
        source.setActive (true);

        auto block = ramp (0, 3000);
        tap.push (block.data(), 3000);
        CHECK (transfer (source, mirror));
        CHECK (twin.getTotalPushed() == 3000);
        CHECK (sameTail (tap, twin, 3000));

        fxme::WaveformTap unprepared;
        std::int64_t from = 0;
        float sink[4];
        CHECK (unprepared.readSince (from, sink, 4) == 0);
    }

    //--------------------------------------------------------------------------
    std::printf ("Version skew, bad frames, schema\n");
    {
        std::atomic<float> a { 1.0f }, b { 2.0f }, twinB { 0.0f };
        fxme::VuMeter meter;

        TelemetryRegistry source, mirror;
        source.addValue ("added-later", a);        // unknown to the mirror
        source.addValue ("b", b);
        source.addValue ("kind-changed", a);
        mirror.addValue ("b", twinB);
        mirror.addMeter ("kind-changed", meter);   // same name, other kind

        CHECK (transfer (source, mirror));
        CHECK (twinB.load() == 2.0f);
        CHECK (meter.getRmsGain() == 0.0f);

        CHECK (! source.addValue ("b", a));        // names are unique

        Bytes frame;
        source.serialise (frame);
        CHECK (! mirror.apply (frame.data(), frame.size() - 3));   // truncated
        CHECK (! mirror.apply (frame.data(), 2));
        frame[0] ^= 0xff;
        CHECK (! mirror.apply (frame.data(), frame.size()));       // bad magic

        Bytes schema;
        source.writeSchema (schema);
        ByteReader r (schema.data(), schema.size());
        CHECK (r.u32() == schemaMagic);
        CHECK (r.u16() == codecVersion);
        CHECK (r.u16() == 3);
        CHECK (r.u32() == idOf ("added-later"));
        CHECK (r.u8() == (std::uint8_t) ChannelKind::value);
        const auto len = r.u8();
        const auto* name = r.take (len);
        CHECK (name != nullptr && std::string ((const char*) name, len) == "added-later");
    }

    //--------------------------------------------------------------------------
    std::printf ("ABI\n");
    {
        CHECK (abiVersion == 1);
        CHECK (sizeof (netPluginIid) == 16);
        CHECK ((int) Role::local == 0 && (int) Role::source == 1 && (int) Role::mirror == 2);
    }

    //--------------------------------------------------------------------------
    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
