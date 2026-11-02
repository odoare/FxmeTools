/*
  ------------------------------------------------------------------------------
    TelemetryRegistry.h

    The list of everything a plugin's editor reads from its processor that is
    not a parameter: meters, spectrum and waveform taps, atomics, computed
    values. FxmeNet runs the same plugin twice, as a "source" (on the server,
    processing audio) and a "mirror" (in the DAW, never processing, showing
    the editor). On the source this registry serialises those objects into a
    telemetry frame; on the mirror it writes the frame back into the very same
    objects, so the editor reads them as if the DSP ran locally and needs no
    change at all.

    Register everything once, in the processor's constructor (it allocates),
    each object under a name unique within the plugin:

        telemetry.addMeter ("gr", compressor.getGrMeter());
        telemetry.addTap   ("in", equalizer.getInputTap());

    Two kinds are for editors that call processor functions rather than read
    objects. Their getter, on the mirror, returns the received value:

        // constructor
        telemetry.addScalar ("gate1", [this] { return splitter.getGateOpenness (0); });
        // the function the editor calls
        float getGateOpenness (int) const
        {
            if (net.isMirror()) return net.telemetry().scalar ("gate1");
            ...
        }

    addPeak() is the same for read-and-reset functions ("the highest level
    since I last asked"): the source takes at each frame, the mirror keeps the
    highest value received until its own editor takes it.

    Threads. serialise() and setActive() run on one non-audio thread of the
    source (the server's telemetry pump), apply() on one non-audio thread of
    the mirror; they read and write the objects exactly the way an editor
    timer and the audio thread already do (atomics, single-writer taps).
    scalar() and takePeak() are for the mirror's message thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/net/TelemetryCodec.h>
#include <FxmeTools/dsp/SpectrumTap.h>
#include <FxmeTools/dsp/VuMeter.h>
#include <FxmeTools/dsp/WaveformTap.h>
#include <FxmeTools/util/StringRef.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fxme::net
{

class TelemetryRegistry
{
public:
    /** Most tap samples one frame carries per tap. At 30 frames/s a 48 kHz tap
        needs 1600; the rest is room to catch up after a late frame. */
    static constexpr int maxTapSamplesPerFrame = 8192;

    TelemetryRegistry() = default;

    //==============================================================================
    // Registration: constructor time only, before any other call.

    /** A level meter: RMS, peak and held max. */
    bool addMeter (StringRef name, VuMeter& meter)
    {
        auto* c = add (name, ChannelKind::meter);
        if (c != nullptr)
            c->meter = &meter;
        return c != nullptr;
    }

    /** An atomic the editor reads directly; the mirror stores into it. */
    bool addValue (StringRef name, std::atomic<float>& value)
    {
        auto* c = add (name, ChannelKind::value);
        if (c != nullptr)
            c->value = &value;
        return c != nullptr;
    }

    /** A computed value; read on the mirror with scalar(). */
    bool addScalar (StringRef name, std::function<float()> getter)
    {
        auto* c = add (name, ChannelKind::scalar);
        if (c != nullptr)
            c->function = std::move (getter);
        return c != nullptr;
    }

    /** A read-and-reset value; taken on the mirror with takePeak(). */
    bool addPeak (StringRef name, std::function<float()> take)
    {
        auto* c = add (name, ChannelKind::peak);
        if (c != nullptr)
            c->function = std::move (take);
        return c != nullptr;
    }

    /** A spectrum tap, streamed sample by sample while active. */
    bool addTap (StringRef name, SpectrumTap& tap)
    {
        auto* c = add (name, ChannelKind::tap);
        if (c != nullptr)
            c->spectrumTap = &tap;
        return c != nullptr;
    }

    /** A waveform tap, streamed sample by sample while active. */
    bool addTap (StringRef name, WaveformTap& tap)
    {
        auto* c = add (name, ChannelKind::tap);
        if (c != nullptr)
            c->waveformTap = &tap;
        return c != nullptr;
    }

    int getNumChannels() const noexcept    { return (int) channels.size(); }

    //==============================================================================
    // Source side.

    /** Telemetry is only worth producing while someone looks at the editor.
        Activating enables the taps and starts streaming them from now;
        deactivating disables them. Never call it on a plugin running
        normally in a DAW: it overrides what the plugin's own editor set. */
    void setActive (bool shouldBeActive)
    {
        active = shouldBeActive;
        for (auto& c : channels)
        {
            if (c.spectrumTap != nullptr)
            {
                c.spectrumTap->setEnabled (shouldBeActive);
                c.cursor = c.spectrumTap->getTotalPushed();
            }
            if (c.waveformTap != nullptr)
            {
                c.waveformTap->setEnabled (shouldBeActive);
                c.cursor = c.waveformTap->getTotalPushed();
            }
        }
    }

    bool isActive() const noexcept  { return active; }

    /** Writes one frame into `out` (cleared first). Takes the peaks and the
        meters' held max, and advances the taps: call it once per tick. */
    void serialise (std::vector<std::uint8_t>& out)
    {
        out.clear();
        ByteWriter w (out);
        w.u32 (frameMagic);
        w.u16 (codecVersion);
        w.u16 ((std::uint16_t) channels.size());

        for (auto& c : channels)
        {
            w.u32 (c.id);
            w.u8 ((std::uint8_t) c.kind);
            w.u8 (0);
            w.u16 (0);
            const auto sizeAt = w.position();
            w.u32 (0);
            const auto start = w.position();

            switch (c.kind)
            {
                case ChannelKind::meter:
                    w.f32 (c.meter->getRmsGain());
                    w.f32 (c.meter->getPeakGain());
                    w.f32 (c.meter->takeMaxGain());
                    break;

                case ChannelKind::value:
                    w.f32 (c.value->load());
                    break;

                case ChannelKind::scalar:
                case ChannelKind::peak:
                    w.f32 (c.function());
                    break;

                case ChannelKind::tap:
                {
                    int n = 0;
                    if (active)
                    {
                        if (c.spectrumTap != nullptr)
                            n = c.spectrumTap->readSince (c.cursor, scratch.data(), maxTapSamplesPerFrame);
                        else if (c.waveformTap != nullptr)
                            n = c.waveformTap->readSince (c.cursor, scratch.data(), maxTapSamplesPerFrame);
                    }
                    // readSince may have moved the cursor forward over samples
                    // the ring no longer holds: the first one sent is cursor - n.
                    w.u64 ((std::uint64_t) (c.cursor - n));
                    w.u32 ((std::uint32_t) n);
                    for (int i = 0; i < n; ++i)
                        w.f32 (scratch[(std::size_t) i]);
                    break;
                }
            }

            w.patchU32 (sizeAt, (std::uint32_t) (w.position() - start));
        }
    }

    /** Writes the schema (ids, kinds, names) into `out` (cleared first). */
    void writeSchema (std::vector<std::uint8_t>& out) const
    {
        out.clear();
        ByteWriter w (out);
        w.u32 (schemaMagic);
        w.u16 (codecVersion);
        w.u16 ((std::uint16_t) channels.size());
        for (const auto& c : channels)
        {
            w.u32 (c.id);
            w.u8 ((std::uint8_t) c.kind);
            const auto len = std::min<std::size_t> (c.name.size(), 255);
            w.u8 ((std::uint8_t) len);
            w.bytes (c.name.data(), len);
        }
    }

    //==============================================================================
    // Mirror side.

    /** Writes a frame into the registered objects. Channels it does not know
        are skipped. Returns false (having applied what came before) if the
        frame is malformed. */
    bool apply (const std::uint8_t* data, std::size_t size)
    {
        ByteReader r (data, size);
        if (r.u32() != frameMagic || r.u16() != codecVersion)
            return false;

        const int count = r.u16();
        for (int i = 0; i < count && r.ok(); ++i)
        {
            const auto id = r.u32();
            const auto kind = (ChannelKind) r.u8();
            r.skip (3);
            const auto bytes = r.u32();
            const auto* payload = r.take (bytes);
            if (payload == nullptr)
                return false;

            auto* c = find (id);
            if (c == nullptr || c->kind != kind)
                continue;

            ByteReader p (payload, bytes);
            switch (kind)
            {
                case ChannelKind::meter:
                {
                    const float rmsGain = p.f32();
                    const float peakGain = p.f32();
                    const float maxGain = p.f32();
                    if (p.ok())
                    {
                        c->meter->setLevels (rmsGain, peakGain);
                        c->meter->raiseMax (maxGain);
                    }
                    break;
                }

                case ChannelKind::value:
                {
                    const float v = p.f32();
                    if (p.ok())
                        c->value->store (v);
                    break;
                }

                case ChannelKind::scalar:
                {
                    const float v = p.f32();
                    if (p.ok())
                        c->received->store (v);
                    break;
                }

                case ChannelKind::peak:
                {
                    const float v = p.f32();
                    if (! p.ok())
                        break;
                    // The first value since the editor last took replaces the
                    // accumulated one; later ones only raise it.
                    if (! c->fresh.load())
                        c->received->store (v);
                    else if (v > c->received->load())
                        c->received->store (v);
                    c->fresh.store (true);
                    break;
                }

                case ChannelKind::tap:
                {
                    p.u64();   // first sample index: gaps are not filled, a skip only shows as a seam
                    const auto n = (int) p.u32();
                    const auto* samples = p.take ((std::size_t) n * 4);
                    if (samples == nullptr || n > maxTapSamplesPerFrame)
                        break;
                    ByteReader s (samples, (std::size_t) n * 4);
                    for (int k = 0; k < n; ++k)
                        scratch[(std::size_t) k] = s.f32();
                    if (c->spectrumTap != nullptr)
                        c->spectrumTap->push (scratch.data(), n);
                    else if (c->waveformTap != nullptr)
                        c->waveformTap->push (scratch.data(), n);
                    break;
                }
            }
        }
        return r.ok();
    }

    /** The last value received for an addScalar() channel (0 before any). */
    float scalar (StringRef name) const noexcept
    {
        const auto* c = find (idOf (name.data(), name.size()));
        return c != nullptr && c->received != nullptr ? c->received->load() : 0.0f;
    }

    /** The highest value received for an addPeak() channel since the last
        call. When no frame arrived in between, returns the previous result
        again rather than a floor, so an editor polling faster than the
        telemetry rate does not flicker. */
    float takePeak (StringRef name) noexcept
    {
        auto* c = find (idOf (name.data(), name.size()));
        if (c == nullptr || c->received == nullptr)
            return 0.0f;
        if (c->fresh.exchange (false))
            c->lastTaken = c->received->load();
        return c->lastTaken;
    }

private:
    struct Channel
    {
        std::uint32_t id = 0;
        ChannelKind kind = ChannelKind::value;
        std::string name;

        VuMeter* meter = nullptr;
        std::atomic<float>* value = nullptr;
        std::function<float()> function;
        SpectrumTap* spectrumTap = nullptr;
        WaveformTap* waveformTap = nullptr;

        std::int64_t cursor = 0;                          // taps, source side
        std::unique_ptr<std::atomic<float>> received;     // scalar and peak, mirror side
        std::atomic<bool> fresh { false };                // peak, mirror side
        float lastTaken = 0.0f;                           // peak, mirror message thread

        Channel() = default;
        Channel (Channel&& other) noexcept
            : id (other.id), kind (other.kind), name (std::move (other.name)),
              meter (other.meter), value (other.value), function (std::move (other.function)),
              spectrumTap (other.spectrumTap), waveformTap (other.waveformTap),
              cursor (other.cursor), received (std::move (other.received)),
              fresh (other.fresh.load()), lastTaken (other.lastTaken) {}
    };

    /** Returns nullptr when the name (or its hash) is already taken. */
    Channel* add (StringRef name, ChannelKind kind)
    {
        const auto id = idOf (name.data(), name.size());
        if (find (id) != nullptr)
            return nullptr;

        Channel c;
        c.id = id;
        c.kind = kind;
        c.name = name.str();
        if (kind == ChannelKind::scalar || kind == ChannelKind::peak)
            c.received = std::make_unique<std::atomic<float>> (0.0f);
        if (kind == ChannelKind::tap && scratch.empty())
            scratch.assign ((std::size_t) maxTapSamplesPerFrame, 0.0f);

        channels.push_back (std::move (c));
        return &channels.back();
    }

    Channel* find (std::uint32_t id) noexcept
    {
        for (auto& c : channels)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    const Channel* find (std::uint32_t id) const noexcept
    {
        for (const auto& c : channels)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    std::vector<Channel> channels;
    std::vector<float> scratch;   // one tap's samples, either side
    bool active = false;

    TelemetryRegistry (const TelemetryRegistry&) = delete;
    TelemetryRegistry& operator= (const TelemetryRegistry&) = delete;
};

} // namespace fxme::net
