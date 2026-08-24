/*
  ------------------------------------------------------------------------------
    math/BandwidthOrdering.h

    Reverse Cuthill-McKee renumbering of a symmetric sparsity pattern.

    Why it matters. A Cholesky factorisation fills in: L is dense wherever the
    original matrix has a non-zero anywhere to its left in the same row. For a
    finite-element matrix in the order the mesh generator happened to produce,
    that means almost everywhere, and the factor is effectively dense. Renumber
    the unknowns so that coupled ones sit close together and the fill is
    trapped inside a narrow envelope around the diagonal instead — which is
    what makes a profile factorisation (SkylineCholesky.h) worth having.

    Measured on the Morley plate meshes this library produces, the mean row
    bandwidth falls from about n/1.5 to about 1.1 * sqrt(n): at n = 4913, from
    3681 to 197, and the factor from 737 MB to 3 MB.

    Cuthill-McKee is a breadth-first walk that visits each frontier in order of
    increasing degree, started from a pseudo-peripheral node (George-Liu: walk
    to the far end of the graph, twice). Reversing the resulting order leaves
    the bandwidth unchanged but strictly shrinks the profile, which is the
    quantity a profile factorisation pays for -- so the reversal is free and
    always worth doing.

    Nothing here decides what to do with the permutation. Applying it is the
    caller's business, and for a finite-element assembly the natural place is
    the degree-of-freedom map itself, before anything is assembled: then every
    matrix, every eigenvector and every exported nodal value is in the new
    numbering already and nothing needs permuting back.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "SparseMatrix.h"

#include <cstddef>
#include <vector>

namespace fxme::math
{

/** Reverse Cuthill-McKee ordering of `pattern`, which must be structurally
    symmetric. Returns `perm` with `perm[newIndex] = oldIndex`; the inverse,
    `oldIndex -> newIndex`, is what a degree-of-freedom map needs, so see
    invertPermutation. Disconnected components are handled one after another.
    Returns an empty vector for an empty pattern. */
std::vector<int> reverseCuthillMcKee (const SparsityPattern& pattern);

/** invPerm[perm[i]] = i. */
std::vector<int> invertPermutation (const std::vector<int>& perm);

/** Leftmost column stored in row `row`, clamped to the diagonal. This is the
    left edge of the envelope that a profile factorisation fills, and an empty
    row is treated as holding only its (implicit) diagonal. */
inline int firstStoredColumn (const SparsityPattern& pattern, int row) noexcept
{
    const int begin = pattern.rowStart[(std::size_t) row];
    const int end   = pattern.rowStart[(std::size_t) row + 1];
    if (begin >= end)
        return row;
    const int c = pattern.colIndex[(std::size_t) begin];
    return c < row ? c : row;
}

/** Number of doubles a profile factorisation of `pattern` would store: the
    sum over rows of (i - firstColumn(i) + 1). Divided by n it is the mean row
    bandwidth, which is the useful figure to compare orderings with. */
std::size_t profileSize (const SparsityPattern& pattern);

} // namespace fxme::math
