/*
  ------------------------------------------------------------------------------
    synth/ModGraph.h

    A small directed graph of up to 16 nodes, for audio-rate connections
    between oscillators (FM, ring, AM) or any processing that must run in
    dependency order within a sample: which node feeds which, whether a new
    connection would close a loop, and the order to process the nodes in.

    Self-connections (a node modulating itself: feedback FM) are allowed and
    ignored by the cycle test and the ordering; a self-loop reads the node's
    previous output, which is the only meaningful feedback anyway. Any other
    cycle is refused: canConnect() says no, so a graph built through it is
    always acyclic and topologicalOrder() always succeeds.

    Edges are bit masks: inputsOf[n] has bit m set when m feeds n. Header-only,
    no allocation, realtime safe (the order can be recomputed on the audio
    thread when the graph changes).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <array>
#include <cstdint>

namespace fxme
{

class ModGraph
{
public:
    static constexpr int maxNodes = 16;

    explicit ModGraph (int numberOfNodes = 4) noexcept : numNodes (numberOfNodes) {}

    int getNumNodes() const noexcept { return numNodes; }

    void clear() noexcept { inputsOf.fill (0); }

    bool isConnected (int from, int to) const noexcept { return (inputsOf[(std::size_t) to] >> from) & 1u; }

    /** Connects regardless (use canConnect first). */
    void connect (int from, int to) noexcept    { inputsOf[(std::size_t) to] |= (std::uint16_t) (1u << from); }
    void disconnect (int from, int to) noexcept { inputsOf[(std::size_t) to] &= (std::uint16_t) ~(1u << from); }

    /** False when from -> to would close a loop through other nodes. */
    bool canConnect (int from, int to) const noexcept
    {
        if (from == to)
            return true;            // feedback: allowed
        return ! reaches (to, from);
    }

    /** True when `to` can be reached from `from` along the edges (ignoring
        self-loops). */
    bool reaches (int from, int to) const noexcept
    {
        std::uint32_t visited = 0, frontier = 1u << from;
        while (frontier != 0)
        {
            visited |= frontier;
            std::uint32_t next = 0;
            for (int n = 0; n < numNodes; ++n)
                if ((inputsOf[(std::size_t) n] & frontier & ~(1u << n)) != 0)
                    next |= 1u << n;
            if ((next >> to) & 1u)
                return true;
            frontier = next & ~visited;
        }
        return false;
    }

    /** Writes the nodes in an order where every node comes after the nodes
        feeding it. Returns false (and a partial order) only if the graph has
        a cycle, which canConnect() prevents. */
    bool topologicalOrder (int* order) const noexcept
    {
        std::uint32_t done = 0;
        int count = 0;
        while (count < numNodes)
        {
            bool progressed = false;
            for (int n = 0; n < numNodes; ++n)
            {
                if ((done >> n) & 1u)
                    continue;
                const std::uint32_t deps = inputsOf[(std::size_t) n] & ~(1u << n);
                if ((deps & ~done) == 0)
                {
                    order[count++] = n;
                    done |= 1u << n;
                    progressed = true;
                }
            }
            if (! progressed)
            {
                for (int n = 0; n < numNodes; ++n)   // break the cycle: append the rest
                    if (! ((done >> n) & 1u))
                        order[count++] = n;
                return false;
            }
        }
        return true;
    }

    std::uint16_t getInputs (int node) const noexcept { return inputsOf[(std::size_t) node]; }

private:
    int numNodes;
    std::array<std::uint16_t, maxNodes> inputsOf {};
};

} // namespace fxme
