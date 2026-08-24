/*
  ------------------------------------------------------------------------------
    math/SkylineCholesky.cpp — see SkylineCholesky.h.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "SkylineCholesky.h"

#include "BandwidthOrdering.h"

#include <algorithm>
#include <cmath>

namespace fxme::math
{

SkylineCholesky::SkylineCholesky (const SparseSymmetricMatrix& matrix)
{
    n_ = matrix.size();
    if (n_ < 1)
        return;

    const SparsityPattern& p = matrix.sparsity();

    // Envelope layout. Column indices are sorted within a row, so the leftmost
    // stored entry is simply the first one.
    first.resize ((std::size_t) n_);
    rowPtr.resize ((std::size_t) n_);

    std::size_t total = 0;
    for (int i = 0; i < n_; ++i)
    {
        first[(std::size_t) i] = firstStoredColumn (p, i);
        rowPtr[(std::size_t) i] = total;
        total += (std::size_t) (i - first[(std::size_t) i] + 1);
    }

    value.assign (total, 0.0);

    // Load the lower triangle. Entries above the diagonal are the mirror of
    // ones already stored, so they are skipped rather than added twice.
    const double* src = matrix.values();
    for (int i = 0; i < n_; ++i)
        for (int k = p.rowStart[(std::size_t) i], e = p.rowStart[(std::size_t) i + 1]; k < e; ++k)
        {
            const int j = p.colIndex[(std::size_t) k];
            if (j <= i)
                at (i, j) = src[(std::size_t) k];
        }

    // Envelope Cholesky, row by row. Both inner loops walk their two rows
    // forward in memory from the first column the rows have in common, which
    // is what makes this worth doing at all.
    for (int i = 0; i < n_; ++i)
    {
        const int fi = first[(std::size_t) i];
        double* rowI = &value[rowPtr[(std::size_t) i]];

        for (int j = fi; j < i; ++j)
        {
            const int fj = first[(std::size_t) j];
            const int from = std::max (fi, fj);
            const double* rowJ = &value[rowPtr[(std::size_t) j]];

            double s = rowI[j - fi];
            for (int k = from; k < j; ++k)
                s -= rowI[k - fi] * rowJ[k - fj];

            const double djj = rowJ[j - fj];
            rowI[j - fi] = s / djj;
        }

        double d = rowI[i - fi];
        for (int k = fi; k < i; ++k)
            d -= rowI[k - fi] * rowI[k - fi];

        if (! (d > 0.0))
            return;                     // not positive definite; ok_ stays false

        rowI[i - fi] = std::sqrt (d);
    }

    ok_ = true;
}

void SkylineCholesky::solveInPlace (double* b) const
{
    // Forward: L y = b. Row i is contiguous, so the dot product streams.
    for (int i = 0; i < n_; ++i)
    {
        const int fi = first[(std::size_t) i];
        const double* rowI = &value[rowPtr[(std::size_t) i]];

        double s = b[i];
        for (int k = fi; k < i; ++k)
            s -= rowI[k - fi] * b[k];
        b[i] = s / rowI[i - fi];
    }

    // Backward: L' x = y. Stored by rows rather than columns, so this runs as
    // a sequence of updates pushed backwards instead of dot products pulled
    // forwards -- same arithmetic, same contiguous access.
    for (int i = n_ - 1; i >= 0; --i)
    {
        const int fi = first[(std::size_t) i];
        const double* rowI = &value[rowPtr[(std::size_t) i]];

        const double xi = b[i] / rowI[i - fi];
        b[i] = xi;
        for (int k = fi; k < i; ++k)
            b[k] -= rowI[k - fi] * xi;
    }
}

} // namespace fxme::math
