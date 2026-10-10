/*
  ------------------------------------------------------------------------------
    synth/WavetableCache.cpp

    See WavetableCache.h.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/synth/WavetableCache.h>
#include <FxmeTools/util/Math.h>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace fxme
{

void WavetableCache::prepare (int numSlots)
{
    numSlots = jmax (1, numSlots);
    slots.clear();
    slots.reserve ((std::size_t) numSlots);
    for (int i = 0; i < numSlots; ++i)
    {
        auto s = std::make_unique<Slot>();
        s->set.allocate();
        slots.push_back (std::move (s));
    }

    queueWrite.store (0);
    queueRead.store (0);
    lastQueued.fill ({});
    lastQueuedTick.fill (0);
    pending.clear();
    pending.reserve (maxStreams);
    pendingIndexForStream.assign ((std::size_t) maxStreams, -1);
    tick.store (0);
}

void WavetableCache::setSource (std::shared_ptr<WavetableSource> newSource)
{
    const std::lock_guard<std::mutex> lock (sourceLock);
    int g = (generation.load() + 1) & 0xffff;
    if (g == 0)
        g = 1;
    if (newSource != nullptr)
        newSource->generation = g;
    source = std::move (newSource);
    generation.store (g, std::memory_order_release);
}

std::shared_ptr<const WavetableSource> WavetableCache::getSource() const
{
    const std::lock_guard<std::mutex> lock (sourceLock);
    return source;
}

//==============================================================================
bool WavetableCache::tryHold (Slot& s, const Key& expected) noexcept
{
    const auto v1 = s.version.load (std::memory_order_acquire);
    if ((v1 & 1u) != 0)
        return false;
    if (s.keyA.load (std::memory_order_relaxed) != expected.a
        || s.keyB.load (std::memory_order_relaxed) != expected.b)
        return false;

    s.refs.fetch_add (1, std::memory_order_seq_cst);
    if (s.version.load (std::memory_order_seq_cst) != v1)
    {
        s.refs.fetch_sub (1, std::memory_order_seq_cst);
        return false;
    }
    s.lastUsed.store (tick.load (std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
}

int WavetableCache::findReady (const Key& key) const noexcept
{
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& s = *slots[(std::size_t) i];
        if (s.keyA.load (std::memory_order_relaxed) == key.a
            && s.keyB.load (std::memory_order_relaxed) == key.b
            && (s.version.load (std::memory_order_acquire) & 1u) == 0)
            return i;
    }
    return -1;
}

int WavetableCache::tryAcquire (const Key& key) noexcept
{
    if (! key.isValid())
        return -1;
    const int i = findReady (key);
    if (i >= 0 && tryHold (*slots[(std::size_t) i], key))
        return i;
    return -1;
}

int WavetableCache::acquireNearest (const Key& key) noexcept
{
    if (const int exact = tryAcquire (key); exact >= 0)
        return exact;

    int best = -1;
    double bestDistance = std::numeric_limits<double>::max();
    Key bestKey;

    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& s = *slots[(std::size_t) i];
        if ((s.version.load (std::memory_order_acquire) & 1u) != 0)
            continue;
        const Key k { s.keyA.load (std::memory_order_relaxed), s.keyB.load (std::memory_order_relaxed) };
        if (! k.isValid())
            continue;

        const double d = distance (k, key);

        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
            bestKey = k;
        }
    }

    if (best >= 0 && tryHold (*slots[(std::size_t) best], bestKey))
        return best;
    return -1;
}

double WavetableCache::distance (const Key& from, const Key& to) noexcept
{
    if (! from.isValid() || ! to.isValid())
        return std::numeric_limits<double>::max();
    // Position distance in widths, width distance in octaves; another
    // generation (an older source) only when nothing current is ready.
    const double width = (double) jmax (1, to.length());
    double d = std::abs ((double) (from.centre() - to.centre())) / width
             + std::abs (std::log2 ((double) jmax (1, from.length())) - std::log2 ((double) jmax (1, to.length())))
             + 0.01 * std::abs ((double) (from.overlapPermille() - to.overlapPermille())) / 10.0
             + (from.overlapShape() != to.overlapShape() || from.snap() != to.snap() ? 0.5 : 0.0);
    if (from.generation() != to.generation())
        d += 1.0e6;
    return d;
}

void WavetableCache::release (int slot) noexcept
{
    if (slot >= 0 && slot < (int) slots.size())
        slots[(std::size_t) slot]->refs.fetch_sub (1, std::memory_order_release);
}

void WavetableCache::request (const Key& key, int stream) noexcept
{
    if (! key.isValid())
        return;
    stream = jlimit (0, maxStreams - 1, stream);

    // Already queued recently from this stream: the worker has it (or had no
    // free slot for it, in which case it is asked again after a while).
    if (lastQueued[(std::size_t) stream] == key && tick.load (std::memory_order_relaxed) - lastQueuedTick[(std::size_t) stream] < 32)
        return;

    const int w = queueWrite.load (std::memory_order_relaxed);
    const int next = (w + 1) % queueSize;
    if (next == queueRead.load (std::memory_order_acquire))
        return;   // full: dropped, asked again later

    queue[(std::size_t) w] = { key, stream };
    queueWrite.store (next, std::memory_order_release);
    lastQueued[(std::size_t) stream] = key;
    lastQueuedTick[(std::size_t) stream] = tick.load (std::memory_order_relaxed);
}

//==============================================================================
int WavetableCache::chooseVictim (int currentGeneration) const noexcept
{
    int best = -1;
    std::int64_t bestScore = std::numeric_limits<std::int64_t>::max();

    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& s = *slots[(std::size_t) i];
        if (s.refs.load (std::memory_order_seq_cst) != 0)
            continue;

        const Key k { s.keyA.load(), s.keyB.load() };
        std::int64_t score;
        if (! k.isValid())
            score = std::numeric_limits<std::int64_t>::min();                 // empty: first
        else if (k.generation() != currentGeneration)
            score = std::numeric_limits<std::int64_t>::min() / 2 + s.lastUsed.load();   // stale source
        else
            score = s.lastUsed.load();                                       // least recently used

        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

bool WavetableCache::service (WavetableBuilder& builder)
{
    if (slots.empty())
        return false;

    // Drain the ring; per stream only the latest request survives.
    for (;;)
    {
        const int r = queueRead.load (std::memory_order_relaxed);
        if (r == queueWrite.load (std::memory_order_acquire))
            break;
        const Request req = queue[(std::size_t) r];
        queueRead.store ((r + 1) % queueSize, std::memory_order_release);

        int& index = pendingIndexForStream[(std::size_t) req.stream];
        if (index >= 0)
            pending[(std::size_t) index].key = req.key;
        else
        {
            index = (int) pending.size();
            pending.push_back (req);
        }
    }

    const auto src = getSource();
    const int currentGeneration = getGeneration();

    while (! pending.empty())
    {
        const Request req = pending.front();
        pending.erase (pending.begin());
        pendingIndexForStream[(std::size_t) req.stream] = -1;
        for (std::size_t i = 0; i < pending.size(); ++i)
            pendingIndexForStream[(std::size_t) pending[i].stream] = (int) i;

        if (req.key.generation() != currentGeneration || src == nullptr)
            continue;                 // asked for a source that is gone
        if (findReady (req.key) >= 0)
            continue;                 // already built (shared by another stream)

        // Claim a slot: odd version first, then check nobody holds it (see the
        // header for why this order makes the claim safe against tryHold).
        for (int attempt = 0; attempt < (int) slots.size(); ++attempt)
        {
            const int victim = chooseVictim (currentGeneration);
            if (victim < 0)
                return false;         // everything is held: try again later

            auto& s = *slots[(std::size_t) victim];
            const auto v = s.version.load();
            s.version.store (v + 1, std::memory_order_seq_cst);
            if (s.refs.load (std::memory_order_seq_cst) != 0)
            {
                s.version.store (v + 2, std::memory_order_seq_cst);   // somebody took it: leave it
                continue;
            }

            s.keyA.store (req.key.a, std::memory_order_relaxed);
            s.keyB.store (0, std::memory_order_relaxed);
            builder.build (*src, req.key.settings(), s.set);
            s.keyB.store (req.key.b, std::memory_order_relaxed);
            s.lastUsed.store (tick.load (std::memory_order_relaxed), std::memory_order_relaxed);
            s.version.store (v + 2, std::memory_order_release);
            return true;
        }
        return false;
    }
    return false;
}

//==============================================================================
struct WavetableWorker::Impl
{
    std::vector<WavetableCache*> caches;
    std::thread thread;
    std::atomic<bool> shouldStop { false };
};

WavetableWorker::WavetableWorker() = default;
WavetableWorker::~WavetableWorker() { stop(); }

void WavetableWorker::start (std::vector<WavetableCache*> cachesToService)
{
    stop();
    impl = std::make_unique<Impl>();
    impl->caches = std::move (cachesToService);
    running.store (true);

    impl->thread = std::thread ([this, state = impl.get()]
    {
        WavetableBuilder builder;
        while (! state->shouldStop.load())
        {
            bool didWork = false;
            for (auto* c : state->caches)
                didWork = c->service (builder) || didWork;
            if (! didWork)
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        running.store (false);
    });
}

void WavetableWorker::stop()
{
    if (impl == nullptr)
        return;
    impl->shouldStop.store (true);
    if (impl->thread.joinable())
        impl->thread.join();
    impl.reset();
    running.store (false);
}

} // namespace fxme
