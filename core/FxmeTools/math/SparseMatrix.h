/*
  ------------------------------------------------------------------------------
    math/SparseMatrix.h

    Compressed-sparse-row storage for the symmetric matrices a finite-element
    assembly produces, plus the two-pass builder that discovers their pattern.

    Why this exists
    ---------------
    A finite-element operator couples a degree of freedom only to the ones
    sharing an element with it. For the Morley plate triangle that is about
    eleven entries per row, and — this is the useful part — eleven regardless
    of how fine the mesh gets. Dense storage of the same matrix is n^2, so the
    fraction wasted grows without bound: at n = 5000 it is already 24 million
    doubles standing in for 55 thousand real numbers.

    Storage layout
    --------------
    Both triangles are stored, not just the lower one. That doubles nothing
    worth counting (11 entries per row against 6) and buys two things that
    matter: multiply() is a plain independent row walk, so it parallelises
    with no reduction and no atomics, and column indices stay sorted within a
    row, so a row walk visits entries in exactly the ascending column order a
    dense row walk would. The second point is what makes the sparse and dense
    paths agree to the last few bits rather than merely to round-off.

    A pattern is separate from the values because an assembly usually produces
    several matrices over the same mesh — here stiffness, tension and mass —
    and they share a pattern exactly. Building it once and sharing it saves
    both the discovery pass and two thirds of the index memory.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "LinearOperator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fxme::math
{

/** The non-zero structure of a square matrix, in compressed-sparse-row form.
    Column indices are sorted ascending within each row. */
struct SparsityPattern
{
    int n = 0;
    std::vector<int> rowStart;    // n + 1 entries
    std::vector<int> colIndex;    // rowStart[n] entries, sorted within a row

    int numNonZeros() const noexcept { return rowStart.empty() ? 0 : rowStart.back(); }

    /** Index into a value array for entry (row, col), or -1 when the entry is
        not part of the pattern. Binary search over a row of about a dozen. */
    int find (int row, int col) const noexcept
    {
        const int lo = rowStart[(std::size_t) row];
        const int hi = rowStart[(std::size_t) row + 1];
        const auto b = colIndex.begin();
        const auto it = std::lower_bound (b + lo, b + hi, col);
        if (it == b + hi || *it != col)
            return -1;
        return (int) (it - b);
    }

    std::size_t byteSize() const noexcept
    {
        return rowStart.size() * sizeof (int) + colIndex.size() * sizeof (int);
    }
};

/** Collects the (row, col) pairs an assembly will touch and turns them into a
    SparsityPattern.

    Use it exactly as the value pass will be written: for each element, hand it
    the same degree-of-freedom list the scatter step uses. Any pair the
    assembly can produce is then in the pattern by construction, and the
    pattern being a superset (elements skipped later as degenerate, say) costs
    only a stored zero. */
class SparsityBuilder
{
public:
    explicit SparsityBuilder (int numDofs) : n (numDofs) {}

    /** Registers every ordered pair drawn from `dofs`, which is how an element
        block scatters. Negative entries (constrained degrees of freedom) are
        skipped, matching the assembly. */
    void addClique (const int* dofs, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if (dofs[i] < 0)
                continue;
            for (int j = 0; j < count; ++j)
            {
                if (dofs[j] < 0)
                    continue;
                entries.push_back ((std::int64_t) dofs[i] * (std::int64_t) n
                                   + (std::int64_t) dofs[j]);
            }
        }
    }

    /** Reserves room for `numElements` blocks of `dofsPerElement`, to keep the
        collection pass from repeatedly reallocating. Optional. */
    void reserveElements (int numElements, int dofsPerElement)
    {
        entries.reserve ((std::size_t) numElements
                         * (std::size_t) dofsPerElement * (std::size_t) dofsPerElement);
    }

    /** Sorts, de-duplicates and emits the pattern. The builder is left empty. */
    std::shared_ptr<const SparsityPattern> build();

private:
    int n = 0;
    std::vector<std::int64_t> entries;
};

/** Symmetric matrix over a shared SparsityPattern. */
class SparseSymmetricMatrix final : public AssemblableMatrix
{
public:
    SparseSymmetricMatrix() = default;

    explicit SparseSymmetricMatrix (std::shared_ptr<const SparsityPattern> p)
    {
        reset (std::move (p));
    }

    void reset (std::shared_ptr<const SparsityPattern> p)
    {
        pattern = std::move (p);
        value.assign ((std::size_t) (pattern ? pattern->numNonZeros() : 0), 0.0);
        dropped = 0;
    }

    int size() const noexcept override { return pattern ? pattern->n : 0; }

    void addEntry (int row, int col, double v) override
    {
        const int k = pattern->find (row, col);
        if (k < 0)
        {
            ++dropped;
            return;
        }
        value[(std::size_t) k] += v;
    }

    void setZero() override
    {
        std::fill (value.begin(), value.end(), 0.0);
        dropped = 0;
    }

    std::size_t droppedEntries() const noexcept override { return dropped; }

    void multiply (const double* x, double* y) const override
    {
        const int n = pattern->n;
        const int* rs = pattern->rowStart.data();
        const int* ci = pattern->colIndex.data();
        const double* va = value.data();

        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;
            for (int k = rs[i], e = rs[i + 1]; k < e; ++k)
                s += va[k] * x[ci[k]];
            y[i] = s;
        }
    }

    void addToDense (double* dst, double scale) const override
    {
        const int n = pattern->n;
        const int* rs = pattern->rowStart.data();
        const int* ci = pattern->colIndex.data();

        for (int i = 0; i < n; ++i)
        {
            double* row = dst + (std::size_t) i * (std::size_t) n;
            for (int k = rs[i], e = rs[i + 1]; k < e; ++k)
                row[ci[k]] += scale * value[(std::size_t) k];
        }
    }

    double diagonalEntry (int i) const noexcept override
    {
        const int k = pattern->find (i, i);
        return k < 0 ? 0.0 : value[(std::size_t) k];
    }

    /** this += scale * other. The fast path needs the two to share a pattern,
        which is the usual case here (one assembly, several matrices); anything
        else goes through addEntry and so requires this matrix's pattern to
        cover the other's. */
    void addScaled (const SparseSymmetricMatrix& other, double scale)
    {
        if (other.pattern == pattern)
        {
            for (std::size_t k = 0; k < value.size(); ++k)
                value[k] += scale * other.value[k];
            return;
        }

        const SparsityPattern& q = *other.pattern;
        for (int i = 0; i < q.n; ++i)
            for (int k = q.rowStart[(std::size_t) i], e = q.rowStart[(std::size_t) i + 1]; k < e; ++k)
                addEntry (i, q.colIndex[(std::size_t) k], scale * other.value[(std::size_t) k]);
    }

    /** Values only: the pattern is shared, so counting it here would count it
        once per matrix. Add pattern->byteSize() separately. */
    std::size_t byteSize() const noexcept override { return value.size() * sizeof (double); }

    const SparsityPattern& sparsity() const noexcept { return *pattern; }

    /** Raw values, indexed exactly as the pattern's colIndex is. */
    const double* values() const noexcept { return value.data(); }

private:
    std::shared_ptr<const SparsityPattern> pattern;
    std::vector<double> value;
    std::size_t dropped = 0;
};

} // namespace fxme::math
