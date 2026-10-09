/*
  ------------------------------------------------------------------------------
    synth/WavetableCache.h

    A fixed pool of WavetableSets cut from one source, built on a worker
    thread and read by the audio thread without locks. It is what lets many
    voices each play their own range of the source (per-voice modulation of a
    range's position or width) while bounding the cost: positions are
    quantised by the caller into keys, a key is built once, and voices asking
    for the same key share its set.

    Audio thread, per voice (or once per block for a shared position):

        int slot = cache.tryAcquire (key);       // exact key, ready: hold it
        if (slot < 0)
        {
            cache.request (key, settings, stream);  // ask the worker for it
            slot = cache.acquireNearest (key);      // meanwhile, the closest ready one
        }
        ... read cache.getSet (slot) ...
        cache.release (slot);                    // when done with it

    A held slot is never rebuilt: the worker only recycles slots nobody holds,
    least recently used first (and sets cut from an older source before any).
    Requests carry a stream id (one per voice and range, say): when several
    requests from one stream are waiting, only the latest is built, so a
    position swept faster than the worker can follow costs one build per
    worker cycle, not one per block.

    Concurrency: a per-slot sequence counter (odd while the worker writes)
    plus a reference count, both sequentially consistent, so a reader either
    sees the slot stable and holds it, or backs off; the worker never writes a
    slot with a holder. The request queue is single producer (the audio
    thread) single consumer (the worker). setSource / prepare are for a
    thread that is not racing either of them (prepare: audio and worker
    stopped).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/synth/WavetableBuilder.h>
#include <FxmeTools/synth/WavetableSet.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace fxme
{

class WavetableCache
{
public:
    //==========================================================================
    /** A quantised build request, packed in two words so it can be published
        atomically per slot. Zero is "no key". */
    struct Key
    {
        std::uint64_t a = 0, b = 0;

        bool isValid() const noexcept { return (b >> 31) & 1u; }
        bool operator== (const Key& o) const noexcept { return a == o.a && b == o.b; }
        bool operator!= (const Key& o) const noexcept { return ! (*this == o); }

        int centre() const noexcept     { return (int) (std::uint32_t) (a & 0xffffffffu); }
        int length() const noexcept     { return (int) (std::uint32_t) (a >> 32); }
        int generation() const noexcept { return (int) (b & 0xffffu); }
        int overlapPermille() const noexcept { return (int) ((b >> 16) & 0x3ffu); }
        int overlapShape() const noexcept    { return (int) ((b >> 26) & 1u); }
        int snap() const noexcept            { return (int) ((b >> 27) & 3u); }

        /** The build settings this key stands for. */
        WavetableBuildSettings settings() const noexcept
        {
            WavetableBuildSettings s;
            s.centre = (double) centre();
            s.width = (double) length();
            s.overlap = (float) overlapPermille() * 0.001f;
            s.overlapShape = (WavetableBuildSettings::OverlapShape) overlapShape();
            s.snap = (WavetableBuildSettings::Snap) snap();
            return s;
        }
    };

    /** Packs a key. centre and length are in source samples (already
        quantised by the caller), overlap is a fraction 0 .. 0.5. */
    static Key makeKey (int generation, int centre, int length, float overlap,
                        int overlapShape, int snap) noexcept
    {
        Key k;
        k.a = (std::uint64_t) (std::uint32_t) (centre < 0 ? 0 : centre)
            | ((std::uint64_t) (std::uint32_t) (length < 1 ? 1 : length) << 32);
        const int permille = overlap <= 0.0f ? 0 : (overlap >= 0.5f ? 500 : (int) (overlap * 1000.0f + 0.5f));
        k.b = (std::uint64_t) (generation & 0xffff)
            | ((std::uint64_t) (permille & 0x3ff) << 16)
            | ((std::uint64_t) (overlapShape & 1) << 26)
            | ((std::uint64_t) (snap & 3) << 27)
            | ((std::uint64_t) 1 << 31);
        return k;
    }

    //==========================================================================
    WavetableCache() = default;

    /** Allocates numSlots sets (each about 180 kB) and empties the cache.
        Neither the audio thread nor the worker may be using it. */
    void prepare (int numSlots);

    int getNumSlots() const noexcept { return (int) slots.size(); }

    /** Replaces the source and bumps the generation (wrapping at 16 bits).
        Sets cut from the old source stay readable by their holders and are
        recycled first. Any thread but the audio thread. */
    void setSource (std::shared_ptr<WavetableSource> newSource);

    std::shared_ptr<const WavetableSource> getSource() const;

    /** The generation the next keys should carry. Any thread. */
    int getGeneration() const noexcept { return generation.load (std::memory_order_acquire); }

    //==========================================================================
    // Audio thread

    /** Advances the LRU clock; call once per audio block. */
    void beginBlock() noexcept { tick.fetch_add (1, std::memory_order_relaxed); }

    /** Holds the slot carrying exactly `key` if it is ready; -1 otherwise. */
    int tryAcquire (const Key& key) noexcept;

    /** Holds the ready slot closest to `key` (same generation preferred, then
        nearest position and width), or -1 when nothing is ready. */
    int acquireNearest (const Key& key) noexcept;

    /** Asks the worker to build `key`. Cheap and lock-free; asking again for
        a key already queued from the same stream is ignored. */
    void request (const Key& key, int stream) noexcept;

    /** Lets go of a slot from tryAcquire / acquireNearest. */
    void release (int slot) noexcept;

    const WavetableSet& getSet (int slot) const noexcept { return slots[(std::size_t) slot]->set; }

    /** The key a held slot carries. */
    Key getKey (int slot) const noexcept
    {
        const auto& s = *slots[(std::size_t) slot];
        return { s.keyA.load (std::memory_order_relaxed), s.keyB.load (std::memory_order_relaxed) };
    }

    //==========================================================================
    // Worker thread

    /** Builds at most one pending request with `builder`. Returns true when
        it built something (call again), false when there was nothing to do. */
    bool service (WavetableBuilder& builder);

    static constexpr int maxStreams = 512;

private:
    struct Slot
    {
        WavetableSet set;
        std::atomic<std::uint32_t> version { 0 };   // odd while being written
        std::atomic<int> refs { 0 };
        std::atomic<std::uint64_t> keyA { 0 }, keyB { 0 };
        std::atomic<std::int64_t> lastUsed { 0 };
    };

    struct Request
    {
        Key key;
        int stream = 0;
    };

    bool tryHold (Slot& s, const Key& expected) noexcept;
    int findReady (const Key& key) const noexcept;
    int chooseVictim (int currentGeneration) const noexcept;

    std::vector<std::unique_ptr<Slot>> slots;
    std::atomic<std::int64_t> tick { 0 };   // written by the audio thread, read by the worker

    // SPSC request ring (audio -> worker).
    static constexpr int queueSize = 1024;
    std::array<Request, queueSize> queue {};
    std::atomic<int> queueWrite { 0 }, queueRead { 0 };

    // Audio side: the last key queued per stream, to avoid flooding the ring.
    std::array<Key, maxStreams> lastQueued {};
    std::array<std::int64_t, maxStreams> lastQueuedTick {};

    // Worker side: latest pending request per stream, in arrival order.
    std::vector<Request> pending;
    std::vector<int> pendingIndexForStream;

    mutable std::mutex sourceLock;            // never taken by the audio thread
    std::shared_ptr<WavetableSource> source;
    std::atomic<int> generation { 1 };
};

//==============================================================================
/** A background thread servicing a set of caches: builds whatever they have
    pending, sleeps a couple of milliseconds when idle. Not used by the audio
    thread at all; start it once the caches are prepared, stop it before
    preparing them again. */
class WavetableWorker
{
public:
    WavetableWorker();
    ~WavetableWorker();

    void start (std::vector<WavetableCache*> cachesToService);
    void stop();
    bool isRunning() const noexcept { return running.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> running { false };
};

} // namespace fxme
