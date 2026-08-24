/*
  ------------------------------------------------------------------------------
    math/BandwidthOrdering.cpp — see BandwidthOrdering.h.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "BandwidthOrdering.h"

#include <algorithm>

namespace fxme::math
{

namespace
{

/** Breadth-first walk from `root` over its connected component, appending the
    visit order to `order` and recording each node's level in `levelOf` (-1 for
    nodes outside the component). The unvisited neighbours of each node are
    appended in order of increasing degree: that is the Cuthill-McKee rule, and
    it is what keeps the frontier narrow. Returns the number of levels. */
int breadthFirst (const SparsityPattern& p,
                  const std::vector<int>& degree,
                  int root,
                  std::vector<int>& order,
                  std::vector<int>& levelOf)
{
    std::fill (levelOf.begin(), levelOf.end(), -1);
    order.clear();
    order.push_back (root);
    levelOf[(std::size_t) root] = 0;

    std::vector<int> neighbours;
    int depth = 0;

    for (std::size_t head = 0; head < order.size(); ++head)
    {
        const int v = order[head];
        const int lv = levelOf[(std::size_t) v];

        neighbours.clear();
        for (int k = p.rowStart[(std::size_t) v], e = p.rowStart[(std::size_t) v + 1]; k < e; ++k)
        {
            const int w = p.colIndex[(std::size_t) k];
            if (w == v || levelOf[(std::size_t) w] >= 0)
                continue;
            levelOf[(std::size_t) w] = lv + 1;
            neighbours.push_back (w);
        }

        std::sort (neighbours.begin(), neighbours.end(),
                   [&degree] (int a, int b) { return degree[(std::size_t) a] < degree[(std::size_t) b]; });
        order.insert (order.end(), neighbours.begin(), neighbours.end());
        depth = std::max (depth, lv + (neighbours.empty() ? 0 : 1));
    }

    return depth + 1;
}

/** George-Liu: a node at (or near) one end of the component's longest path.
    Starting the walk there is what makes the level structure deep and narrow
    rather than shallow and wide. */
int pseudoPeripheral (const SparsityPattern& p,
                      const std::vector<int>& degree,
                      int start,
                      std::vector<int>& order,
                      std::vector<int>& levelOf)
{
    int root = start;
    int depth = breadthFirst (p, degree, root, order, levelOf);

    // Each round can only deepen the level structure, so this terminates on
    // its own; the guard is against a pathological graph, not a normal one.
    for (int round = 0; round < 10; ++round)
    {
        int candidate = -1;
        for (int v : order)
            if (levelOf[(std::size_t) v] == depth - 1
                && (candidate < 0 || degree[(std::size_t) v] < degree[(std::size_t) candidate]))
                candidate = v;

        if (candidate < 0 || candidate == root)
            break;

        const int deeper = breadthFirst (p, degree, candidate, order, levelOf);
        if (deeper <= depth)
            break;

        root = candidate;
        depth = deeper;
    }

    return root;
}

} // namespace

std::vector<int> reverseCuthillMcKee (const SparsityPattern& pattern)
{
    std::vector<int> perm;
    const int n = pattern.n;
    if (n < 1)
        return perm;

    perm.reserve ((std::size_t) n);

    std::vector<int> degree ((std::size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int d = pattern.rowStart[(std::size_t) i + 1] - pattern.rowStart[(std::size_t) i];
        if (pattern.find (i, i) >= 0)
            --d;                        // the diagonal is not an edge
        degree[(std::size_t) i] = d;
    }

    std::vector<char> placed ((std::size_t) n, 0);
    std::vector<int> order, levelOf ((std::size_t) n, -1);

    for (int seed = 0; seed < n; ++seed)
    {
        if (placed[(std::size_t) seed])
            continue;

        // A breadth-first walk reaches exactly one component, and components
        // are consumed whole, so anything reachable from an unplaced seed is
        // itself unplaced.
        const int root = pseudoPeripheral (pattern, degree, seed, order, levelOf);
        breadthFirst (pattern, degree, root, order, levelOf);

        for (int v : order)
        {
            perm.push_back (v);
            placed[(std::size_t) v] = 1;
        }
    }

    // Reversal: same bandwidth, strictly smaller profile.
    std::reverse (perm.begin(), perm.end());
    return perm;
}

std::vector<int> invertPermutation (const std::vector<int>& perm)
{
    std::vector<int> inv (perm.size());
    for (std::size_t i = 0; i < perm.size(); ++i)
        inv[(std::size_t) perm[i]] = (int) i;
    return inv;
}

std::size_t profileSize (const SparsityPattern& pattern)
{
    std::size_t total = 0;
    for (int i = 0; i < pattern.n; ++i)
        total += (std::size_t) (i - firstStoredColumn (pattern, i) + 1);
    return total;
}

} // namespace fxme::math
