/*
  ------------------------------------------------------------------------------
    math/SubspaceEigensolver.h

    Lowest eigenpairs of the generalized symmetric problem

        A x = lambda M x,     A symmetric, M symmetric positive definite

    by shift-invert subspace iteration with Rayleigh-Ritz projection.

    The caller supplies three things and no matrix: the operators A and M, and
    a solver for the shifted matrix P = A + sigma M. That is the whole
    interface (see LinearOperator.h), so the same iteration runs on dense
    storage, on compressed sparse rows, or on anything else able to multiply
    and to solve.

    Method
    ------
    A block of p > numModes vectors is driven towards the low end of the
    spectrum by one inverse-power step per iteration, X <- P^-1 M X, then
    re-extracted by solving the p x p projected problem Z'AZ u = theta Z'MZ u.
    The block converges from the bottom up, which is why p is taken larger
    than the number of modes wanted: the surplus vectors absorb the slow
    convergence at the top of the requested range.

    Two inverse-power steps between projections would halve the projection
    cost, and were tried: without re-orthonormalisation in between, the block
    collapses onto the lowest mode and the projected mass matrix goes
    numerically singular. One step per projection it is.

    Cost, per iteration, for n degrees of freedom and a block of p:
        p triangular solves against P   (the storage format decides this)
        2p operator multiplies          (likewise)
        O(n p^2) for the projections and the Ritz recombination
    The last term is dense whatever the matrices look like, so it is what a
    sparse storage format eventually leaves behind as the bottleneck. It grows
    with the number of modes asked for, not with the size of the mesh.

    Pure C++17, no framework dependency. Namespace fxme::math.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "LinearOperator.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace fxme::math
{

struct SubspaceOptions
{
    int numModes = 32;              // eigenpairs wanted, lowest first
    int blockSize = 0;              // 0 = numModes + max(8, numModes / 2)
    int maxIterations = 60;
    int numThreads = 0;             // 0 = fxme::math::defaultWorkerCount()

    /** Relative tolerance on the eigenvalues, applied to the lower half of the
        requested range and to the upper half respectively. The top of a
        subspace block is always the last to settle, and it is also where the
        discretisation error of the underlying model is largest, so holding it
        to the same tolerance as the bottom buys accuracy that is not there to
        begin with and costs a large share of the iterations. */
    double toleranceLow = 1.0e-6;
    double toleranceHigh = 1.0e-4;

    std::function<void (float)> progress;   // optional, called with 0..1
};

struct SubspaceResult
{
    int n = 0;
    std::vector<double> eigenvalues;    // ascending, numModes of them
    std::vector<double> vectors;        // numModes rows of n, row-major
    int iterations = 0;
    bool converged = false;
    std::size_t blockBytes = 0;         // working set of the iteration itself

    bool valid() const noexcept { return ! eigenvalues.empty(); }

    const double* vector (int k) const noexcept
    {
        return vectors.data() + (std::size_t) k * (std::size_t) n;
    }
};

/** Solves A x = lambda M x for the lowest options.numModes eigenpairs.
    `shiftedSolver` must factorise A + sigma M for a shift sigma chosen by the
    caller (large enough to make it positive definite, small enough not to
    blunt the convergence). Returns an empty result when the block degenerates
    on the very first iteration. Deterministic: the starting block comes from
    a fixed integer hash, not from a random source. */
SubspaceResult subspaceEigenSolve (const SymmetricOperator& A,
                                   const SymmetricOperator& M,
                                   const SpdSolver& shiftedSolver,
                                   const SubspaceOptions& options);

} // namespace fxme::math
