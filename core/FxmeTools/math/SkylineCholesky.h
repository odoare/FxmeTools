/*
  ------------------------------------------------------------------------------
    math/SkylineCholesky.h

    Cholesky factorisation of a symmetric positive-definite matrix in profile
    (skyline, variable-band) storage.

    The idea. Cholesky fill-in obeys one simple rule: L can only be non-zero
    where the original matrix has a non-zero somewhere to its left in the same
    row. Everything strictly left of a row's leftmost entry therefore stays
    exactly zero, in exact arithmetic and in floating point alike -- so a
    factorisation that stores only the envelope between each row's leftmost
    entry and its diagonal is not an approximation of anything. It is the
    complete factor, with the provably-zero part left out.

    Why this rather than a general sparse Cholesky. A supernodal factorisation
    with a minimum-degree ordering has better asymptotics: O(n^1.5) against the
    envelope's effectively O(n^2) in two dimensions. It also needs a symbolic
    analysis phase, an elimination tree, and supernode detection, which is the
    bulk of the difficulty and most of the code. At the sizes this library
    works at -- a few thousand unknowns, half a Gflop at the very top -- that
    asymptotic advantage buys nothing measurable, while the envelope needs no
    symbolic phase at all: the storage is known the moment the pattern is.
    Simplicity wins on this problem, and it is worth being explicit that it was
    a choice rather than an oversight.

    Ordering is NOT this class's business. It factorises the matrix it is
    given, in the numbering it arrives in, and the envelope is only narrow if
    the caller made it so -- see BandwidthOrdering.h. Handing it an unordered
    finite-element matrix is not wrong, merely pointless: the envelope will be
    most of the matrix and the factor most of a dense one.

    Thread safety. solveInPlace is const and uses no scratch of any kind, so
    any number of threads may solve different right-hand sides against the same
    factor at once, which is exactly what a block eigensolver wants.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "LinearOperator.h"
#include "SparseMatrix.h"

#include <cstddef>
#include <vector>

namespace fxme::math
{

class SkylineCholesky final : public SpdSolver
{
public:
    /** Factorises `matrix`, which must be symmetric positive definite. Check
        ok() before use: a matrix that is not positive definite leaves the
        factor unusable, exactly as for the dense factorisation. */
    explicit SkylineCholesky (const SparseSymmetricMatrix& matrix);

    bool ok() const noexcept { return ok_; }

    int size() const noexcept override { return n_; }

    void solveInPlace (double* b) const override;

    std::size_t byteSize() const noexcept override
    {
        return value.size() * sizeof (double)
             + first.size() * sizeof (int)
             + rowPtr.size() * sizeof (std::size_t);
    }

    /** Doubles stored, zeros inside the envelope included. Divided by size()
        it is the mean row bandwidth, the figure that says whether the ordering
        did its job. */
    std::size_t profile() const noexcept { return value.size(); }

private:
    double& at (int row, int col) noexcept
    {
        return value[rowPtr[(std::size_t) row] + (std::size_t) (col - first[(std::size_t) row])];
    }

    int n_ = 0;
    bool ok_ = false;
    std::vector<int> first;             // leftmost column of each row
    std::vector<std::size_t> rowPtr;    // offset of that column in `value`
    std::vector<double> value;          // row-major within the envelope
};

} // namespace fxme::math
