/*
  ------------------------------------------------------------------------------
    math/LinearOperator.h

    The two abstractions the eigensolver in this directory works through, and
    the only thing standing between it and a storage format.

    An eigensolver does not need to see a matrix. It needs to multiply by one
    (SymmetricOperator) and to solve against a factorised shifted one
    (SpdSolver). Keeping the interface that narrow is what lets the same
    subspace iteration run on dense storage, on compressed-sparse-row storage,
    or later on a matrix-free operator, with no change and no template
    instantiation per combination.

    The virtual calls are per *vector*, not per element: one multiply() covers
    an entire matrix-vector product, so dispatch cost is unmeasurable. The one
    exception is AssemblableMatrix::addEntry, which is per element-block
    entry — a few hundred thousand calls for a whole assembly, still far below
    the cost of forming the element matrices themselves.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>

namespace fxme::math
{

/** A symmetric n x n linear operator. */
struct SymmetricOperator
{
    virtual ~SymmetricOperator() = default;

    virtual int size() const noexcept = 0;

    /** y = A x. Both buffers hold size() doubles and must not alias. */
    virtual void multiply (const double* x, double* y) const = 0;

    /** dst += scale * A, with dst an already-initialised size() x size()
        row-major buffer. This is how a sparse operator is expanded for a
        dense factorisation. */
    virtual void addToDense (double* dst, double scale) const = 0;

    /** Entry (i, i). Cheap for every storage format here. */
    virtual double diagonalEntry (int i) const noexcept = 0;

    /** Bytes held by this operator, for reporting a solver footprint. */
    virtual std::size_t byteSize() const noexcept = 0;

    double trace() const noexcept
    {
        double t = 0.0;
        for (int i = 0, e = size(); i < e; ++i)
            t += diagonalEntry (i);
        return t;
    }
};

/** A symmetric matrix under construction by scatter-add of element blocks. */
struct AssemblableMatrix : SymmetricOperator
{
    /** Accumulates v into entry (row, col). For sparse storage the entry has
        to exist in the pattern; if it does not, the value is dropped and
        droppedEntries() counts it rather than the matrix silently corrupting
        or the code paying for a reallocation mid-assembly. */
    virtual void addEntry (int row, int col, double v) = 0;

    /** Zeroes every stored value, keeping the allocation and the pattern. */
    virtual void setZero() = 0;

    /** Number of addEntry calls that found no home. Always zero for dense
        storage, and zero for a sparse pattern built from the same element
        cliques the assembly scatters into. A non-zero value means the pattern
        and the assembly have drifted apart, which is a bug worth failing on
        rather than approximating around. */
    virtual std::size_t droppedEntries() const noexcept = 0;
};

/** A factorised symmetric positive-definite operator. */
struct SpdSolver
{
    virtual ~SpdSolver() = default;

    virtual int size() const noexcept = 0;

    /** Solves P x = b, overwriting b with x. const and scratch-free, so
        several threads may solve different right-hand sides at once. */
    virtual void solveInPlace (double* b) const = 0;

    virtual std::size_t byteSize() const noexcept = 0;
};

} // namespace fxme::math
