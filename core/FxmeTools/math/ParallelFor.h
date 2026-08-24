/*
  ------------------------------------------------------------------------------
    math/ParallelFor.h

    Minimal work-splitting helper for the numerical kernels in this directory:
    hand out indices 0..count-1 dynamically to a small team of threads.

    Deliberately not a thread pool. These kernels run once per solve on a
    background thread, the per-index work is large (a full matrix-vector
    product or triangular solve), and the team is short-lived, so thread
    creation is lost in the noise and a pool would only add state to reason
    about.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace fxme::math
{

/** Default team size for the kernels here: about half the hardware threads,
    never more than four. They are memory-bandwidth bound, so more threads buy
    very little, and this code runs behind an interactive application that
    should stay responsive while it works. */
inline int defaultWorkerCount() noexcept
{
    const unsigned hw = std::thread::hardware_concurrency();
    return std::min (4, std::max (1, (int) (hw > 1 ? hw / 2 : 1)));
}

/** Runs fn(0 .. count-1) on up to numThreads threads, the calling thread
    included. Indices are handed out dynamically, so unequal per-index cost
    still balances. numThreads <= 0 means defaultWorkerCount().

    fn instances must touch disjoint data: nothing here synchronises them. */
template <class Fn>
void parallelFor (int numThreads, int count, Fn&& fn)
{
    if (count <= 0)
        return;

    if (numThreads <= 0)
        numThreads = defaultWorkerCount();
    numThreads = std::min (numThreads, count);

    if (numThreads <= 1)
    {
        for (int i = 0; i < count; ++i)
            fn (i);
        return;
    }

    std::atomic<int> next { 0 };
    auto worker = [&next, count, &fn]
    {
        for (int i; (i = next.fetch_add (1)) < count;)
            fn (i);
    };

    std::vector<std::thread> team;
    team.reserve ((std::size_t) (numThreads - 1));
    for (int t = 0; t < numThreads - 1; ++t)
        team.emplace_back (worker);
    worker();
    for (auto& th : team)
        th.join();
}

} // namespace fxme::math
