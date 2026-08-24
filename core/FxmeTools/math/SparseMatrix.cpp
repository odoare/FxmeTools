/*
  ------------------------------------------------------------------------------
    math/SparseMatrix.cpp — see SparseMatrix.h.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "SparseMatrix.h"

namespace fxme::math
{

std::shared_ptr<const SparsityPattern> SparsityBuilder::build()
{
    auto pattern = std::make_shared<SparsityPattern>();
    if (n < 1)
    {
        pattern->rowStart.assign (1, 0);
        entries.clear();
        return pattern;
    }

    pattern->n = n;
    pattern->rowStart.assign ((std::size_t) n + 1, 0);

    // One sort over packed row*n + col keys does the whole job: it groups the
    // rows and orders the columns inside them at the same time, which is
    // exactly the compressed-sparse-row invariant. Sorting a few hundred
    // thousand 64-bit integers costs a few milliseconds, well under the
    // element-matrix work this runs alongside.
    std::sort (entries.begin(), entries.end());
    entries.erase (std::unique (entries.begin(), entries.end()), entries.end());

    pattern->colIndex.resize (entries.size());
    for (std::size_t k = 0; k < entries.size(); ++k)
    {
        const std::int64_t key = entries[k];
        const int row = (int) (key / (std::int64_t) n);
        pattern->colIndex[k] = (int) (key - (std::int64_t) row * (std::int64_t) n);
        ++pattern->rowStart[(std::size_t) row + 1];
    }
    for (int i = 0; i < n; ++i)
        pattern->rowStart[(std::size_t) i + 1] += pattern->rowStart[(std::size_t) i];

    entries.clear();
    entries.shrink_to_fit();
    return pattern;
}

} // namespace fxme::math
