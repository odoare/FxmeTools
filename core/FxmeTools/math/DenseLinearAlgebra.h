/*
  ------------------------------------------------------------------------------
    math/DenseLinearAlgebra.h

    Dense symmetric linear algebra: Cholesky factorisation and solve, symmetric
    matrix-vector product, two symmetric eigensolvers, and a fixed-size inverse
    for element-level work.

    All storage is row-major double. Nothing here is a general BLAS: these are
    the handful of kernels the finite-element eigensolver needs, kept small
    enough to read in one sitting and free of any external dependency.

    Sizes: the dense path costs n^2 doubles per matrix and n^3/3 flops to
    factorise, which is fine to a few thousand degrees of freedom and hopeless
    beyond. See SparseMatrix.h for the storage that lifts that limit; this file
    stays because it is the reference the sparse path is checked against, and
    because the small projected problems inside the subspace iteration are
    genuinely dense.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "LinearOperator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fxme::math
{

// ---------------------------------------------------------------------------
// Raw kernels on row-major buffers.
// ---------------------------------------------------------------------------

/** In-place lower Cholesky of the n x n symmetric matrix `a` (row-major; the
    upper triangle is left stale). Returns false when not positive definite. */
bool choleskyFactorInPlace (double* a, int n);

/** Solves L L' x = b using the factor from choleskyFactorInPlace; b is
    overwritten with x. */
void choleskySolveInPlace (const double* L, int n, double* b);

/** y = A x for a symmetric A held in full square storage. */
void symmetricMultiply (const double* A, int n, const double* x, double* y);

/** Cyclic Jacobi eigensolver for a small symmetric p x p matrix. On return
    `a` is ~diagonal with the eigenvalues on it and V (p x p, row-major) holds
    the eigenvectors as columns. Both buffers hold p * p doubles.

    Kept as the reference the routine below is checked against, and as its
    fallback. Prefer symmetricEigenSolve for anything of a size worth timing. */
void jacobiEigenSymmetric (double* a, double* V, int p, int maxSweeps = 60);

/** The same problem and the same contract, by Householder tridiagonalisation
    followed by implicit-shift QL: on return `a` holds the eigenvalues on its
    diagonal and zeros elsewhere, and V holds the eigenvectors as columns,
    unordered, as Jacobi leaves them.

    Jacobi costs O(p^3) per sweep and needs of the order of ten sweeps to
    converge; this is a single pass of that order. At p = 384, the block size
    the subspace iteration reaches on a plate mesh, that is a factor of twenty
    on this routine, and the small dense problem stops being four fifths of
    the modal computation that contains it.

    Falls back to Jacobi in the case QL fails to converge, so the result is
    always the eigen-decomposition and never an error to handle. */
void symmetricEigenSolve (double* a, double* V, int p);

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

/** Symmetric matrix in full n x n row-major storage. Both triangles are
    stored, so multiply() is a plain row walk. */
class DenseSymmetricMatrix final : public AssemblableMatrix
{
public:
    DenseSymmetricMatrix() = default;
    explicit DenseSymmetricMatrix (int n) { resize (n); }

    void resize (int n)
    {
        n_ = n;
        a.assign ((std::size_t) n * (std::size_t) n, 0.0);
    }

    int size() const noexcept override { return n_; }

    void addEntry (int row, int col, double v) override
    {
        a[(std::size_t) row * (std::size_t) n_ + (std::size_t) col] += v;
    }

    void setZero() override { std::fill (a.begin(), a.end(), 0.0); }

    std::size_t droppedEntries() const noexcept override { return 0; }

    void multiply (const double* x, double* y) const override
    {
        symmetricMultiply (a.data(), n_, x, y);
    }

    void addToDense (double* dst, double scale) const override
    {
        for (std::size_t i = 0; i < a.size(); ++i)
            dst[i] += scale * a[i];
    }

    double diagonalEntry (int i) const noexcept override
    {
        return a[(std::size_t) i * (std::size_t) n_ + (std::size_t) i];
    }

    std::size_t byteSize() const noexcept override { return a.size() * sizeof (double); }

    const double* data() const noexcept { return a.data(); }
    double* data() noexcept             { return a.data(); }

private:
    int n_ = 0;
    std::vector<double> a;
};

/** Dense Cholesky factor of a symmetric positive-definite matrix. */
class DenseCholesky final : public SpdSolver
{
public:
    /** Factorises `matrix` (n x n row-major symmetric), which is moved in and
        overwritten by its own factor. Check ok() before using: a matrix that
        is not positive definite leaves the factor unusable. */
    DenseCholesky (std::vector<double> matrix, int n)
        : n_ (n), L (std::move (matrix))
    {
        ok_ = choleskyFactorInPlace (L.data(), n_);
    }

    bool ok() const noexcept { return ok_; }

    int size() const noexcept override { return n_; }

    void solveInPlace (double* b) const override
    {
        choleskySolveInPlace (L.data(), n_, b);
    }

    std::size_t byteSize() const noexcept override { return L.size() * sizeof (double); }

private:
    int n_ = 0;
    bool ok_ = false;
    std::vector<double> L;
};

/** Builds the dense row-major matrix A + shift * M from any two operators of
    the same size. This is where a sparse assembly meets a dense factorisation,
    and it is the one n^2 allocation the sparse path still makes. May throw
    std::bad_alloc for a large n; callers that offer the user a size knob are
    expected to catch it. */
std::vector<double> denseShiftedSum (const SymmetricOperator& A,
                                     double shift,
                                     const SymmetricOperator& M);

// ---------------------------------------------------------------------------
// Fixed-size dense inverse, for element-level algebra.
// ---------------------------------------------------------------------------

/** Inverts the N x N matrix `in` into `out` by Gauss-Jordan elimination with
    partial pivoting. Returns false when singular to working precision. */
template <int N>
bool invertMatrix (const double in[N][N], double out[N][N])
{
    double a[N][2 * N];
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
        {
            a[i][j] = in[i][j];
            a[i][j + N] = (i == j) ? 1.0 : 0.0;
        }

    for (int col = 0; col < N; ++col)
    {
        int piv = col;
        for (int r = col + 1; r < N; ++r)
            if (std::abs (a[r][col]) > std::abs (a[piv][col]))
                piv = r;
        if (std::abs (a[piv][col]) < 1.0e-14)
            return false;
        if (piv != col)
            for (int j = 0; j < 2 * N; ++j)
                std::swap (a[piv][j], a[col][j]);

        const double d = a[col][col];
        for (int j = 0; j < 2 * N; ++j)
            a[col][j] /= d;
        for (int r = 0; r < N; ++r)
        {
            if (r == col)
                continue;
            const double f = a[r][col];
            if (f == 0.0)
                continue;
            for (int j = 0; j < 2 * N; ++j)
                a[r][j] -= f * a[col][j];
        }
    }

    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            out[i][j] = a[i][j + N];
    return true;
}

} // namespace fxme::math
