/*
  ------------------------------------------------------------------------------
    math/SubspaceEigensolver.cpp — see SubspaceEigensolver.h.

    The iteration block is stored transposed (one vector per contiguous row) so
    that every O(n p) and O(n p^2) inner loop walks memory in order.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "SubspaceEigensolver.h"

#include "DenseLinearAlgebra.h"
#include "ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fxme::math
{

namespace
{

/** Deterministic pseudo-random in [-1, 1] for the starting block. A fixed hash
    rather than a random engine, so a given problem always produces the same
    modes: users compare presets across sessions. */
double prand (std::uint64_t k)
{
    k += 0x9e3779b97f4a7c15ull;
    k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ull;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebull;
    k ^= k >> 31;
    return (double) (k & 0xffffffffull) / 2147483648.0 - 1.0;
}

} // namespace

SubspaceResult subspaceEigenSolve (const SymmetricOperator& A,
                                   const SymmetricOperator& M,
                                   const SpdSolver& shiftedSolver,
                                   const SubspaceOptions& options)
{
    SubspaceResult result;

    const int n = A.size();
    if (n < 1 || M.size() != n || shiftedSolver.size() != n || options.numModes < 1)
        return result;

    const auto report = [&] (float f)
    {
        if (options.progress)
            options.progress (std::clamp (f, 0.0f, 1.0f));
    };

    const int wanted = std::min (options.numModes, n);
    const int p = options.blockSize > 0
                    ? std::min (n, std::max (options.blockSize, wanted))
                    : std::min (n, wanted + std::max (8, wanted / 2));

    const int nThreads = options.numThreads > 0 ? options.numThreads : defaultWorkerCount();

    const std::size_t block = (std::size_t) p * (std::size_t) n;
    std::vector<double> Xt (block);        // p rows of length n
    for (std::size_t i = 0; i < Xt.size(); ++i)
        Xt[i] = prand (i);

    std::vector<double> Zt (block), AZt (block), MZt (block);
    std::vector<double> Ap ((std::size_t) p * (std::size_t) p);
    std::vector<double> Mp ((std::size_t) p * (std::size_t) p);
    std::vector<double> theta ((std::size_t) p, 0.0), thetaPrev ((std::size_t) p, -1.0);

    result.blockBytes = 4 * block * sizeof (double)
                        + 2 * (std::size_t) p * (std::size_t) p * sizeof (double);

    const int maxIters = std::max (1, options.maxIterations);
    int iter = 0;
    bool converged = false;

    // Progress. Subspace iteration converges linearly, so the distance to the
    // tolerance falls by a roughly constant factor per sweep and its logarithm
    // walks to zero in something close to a straight line. Reporting the
    // fraction of that walk which has been made is what makes the number an
    // estimate of the work rather than a count of the worst case: a solve
    // typically stops after seven of the sixty sweeps it is allowed, so
    // (iter + 1) / maxIters never reached an eighth of its range, and a caller
    // showing it stood still through nearly all of the running time.
    //
    // The iteration count stays underneath as a floor, for the case where the
    // distance refuses to fall, and the value is held monotone: an estimate
    // may be wrong, but a progress report that goes backwards is a bug in the
    // eye of whoever is watching it.
    // What a solve is expected to cost, in sweeps, used for the first one
    // only: until two iterates have been compared there is no convergence
    // history to estimate from, and the first sweep is a fifth of the running
    // time on a large mesh. Six to eight is the measured range. Guessing high
    // costs a pause, never a jump backwards, because the report is clamped
    // monotone below.
    constexpr int nominalIters = 8;

    double logStart = 0.0;
    float reported = 0.0f;

    const auto progressFor = [&] (int i, double distance)
    {
        float f = (float) (i + 1) / (float) maxIters;
        if (i == 0)
            f = std::max (f, 1.0f / (float) nominalIters);

        if (distance > 0.0)
        {
            const double lg = std::log (distance);

            // Set the scale from the first real sweep-to-sweep change: the one
            // before it is measured against the seed, which is random.
            if (logStart <= 0.0)
                logStart = lg;

            if (logStart > 0.0)
                f = std::max (f, (float) std::clamp ((logStart - lg) / logStart, 0.0, 1.0));
        }

        reported = std::max (reported, std::min (f, 0.99f));
        return reported;
    };

    for (; iter < maxIters; ++iter)
    {
        // Inverse-power step X -> Z towards the low modes, normalising each
        // vector (the MZt rows double as per-vector scratch).
        parallelFor (nThreads, p, [&] (int j)
        {
            const double* s = &Xt[(std::size_t) j * (std::size_t) n];
            double* scratch = &MZt[(std::size_t) j * (std::size_t) n];
            M.multiply (s, scratch);
            shiftedSolver.solveInPlace (scratch);

            double norm = 0.0;
            for (int i = 0; i < n; ++i)
                norm += scratch[i] * scratch[i];
            const double inv = norm > 0.0 ? 1.0 / std::sqrt (norm) : 1.0;
            double* z = &Zt[(std::size_t) j * (std::size_t) n];
            for (int i = 0; i < n; ++i)
                z[i] = scratch[i] * inv;
        });

        // AZ = A Z and MZ = M Z, one contiguous row per vector.
        parallelFor (nThreads, p, [&] (int j)
        {
            const double* z = &Zt[(std::size_t) j * (std::size_t) n];
            A.multiply (z, &AZt[(std::size_t) j * (std::size_t) n]);
            M.multiply (z, &MZt[(std::size_t) j * (std::size_t) n]);
        });

        // Projected matrices Ap = Z'AZ, Mp = Z'MZ (symmetric, contiguous dots).
        parallelFor (nThreads, p, [&] (int a2)
        {
            const double* za = &Zt[(std::size_t) a2 * (std::size_t) n];
            for (int b2 = a2; b2 < p; ++b2)
            {
                const double* ab = &AZt[(std::size_t) b2 * (std::size_t) n];
                const double* mb = &MZt[(std::size_t) b2 * (std::size_t) n];
                double sa = 0.0, sm = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    sa += za[i] * ab[i];
                    sm += za[i] * mb[i];
                }
                Ap[(std::size_t) a2 * (std::size_t) p + (std::size_t) b2] = sa;
                Ap[(std::size_t) b2 * (std::size_t) p + (std::size_t) a2] = sa;
                Mp[(std::size_t) a2 * (std::size_t) p + (std::size_t) b2] = sm;
                Mp[(std::size_t) b2 * (std::size_t) p + (std::size_t) a2] = sm;
            }
        });

        // Small generalized problem Ap v = theta Mp v via Cholesky + Jacobi.
        std::vector<double> Lp = Mp;
        if (! choleskyFactorInPlace (Lp.data(), p))
            break;   // subspace degenerated; keep the previous iterate

        // B = L^-1 Ap L^-T
        std::vector<double> B ((std::size_t) p * (std::size_t) p);
        {
            // First L Y = Ap column-wise, giving Y = L^-1 Ap.
            std::vector<double> Y = Ap;
            for (int j = 0; j < p; ++j)
                for (int i = 0; i < p; ++i)
                {
                    double s = Y[(std::size_t) i * (std::size_t) p + (std::size_t) j];
                    for (int k = 0; k < i; ++k)
                        s -= Lp[(std::size_t) i * (std::size_t) p + (std::size_t) k]
                           * Y[(std::size_t) k * (std::size_t) p + (std::size_t) j];
                    Y[(std::size_t) i * (std::size_t) p + (std::size_t) j]
                        = s / Lp[(std::size_t) i * (std::size_t) p + (std::size_t) i];
                }
            // Then B L' = Y, row-wise forward solves.
            for (int i = 0; i < p; ++i)
                for (int j = 0; j < p; ++j)
                {
                    double s = Y[(std::size_t) i * (std::size_t) p + (std::size_t) j];
                    for (int k = 0; k < j; ++k)
                        s -= B[(std::size_t) i * (std::size_t) p + (std::size_t) k]
                           * Lp[(std::size_t) j * (std::size_t) p + (std::size_t) k];
                    B[(std::size_t) i * (std::size_t) p + (std::size_t) j]
                        = s / Lp[(std::size_t) j * (std::size_t) p + (std::size_t) j];
                }
        }

        std::vector<double> V ((std::size_t) p * (std::size_t) p);
        jacobiEigenSymmetric (B.data(), V.data(), p);

        // Eigenvalues on B's diagonal; sort ascending.
        std::vector<int> order ((std::size_t) p);
        for (int i = 0; i < p; ++i)
        {
            order[(std::size_t) i] = i;
            theta[(std::size_t) i] = B[(std::size_t) i * (std::size_t) p + (std::size_t) i];
        }
        std::sort (order.begin(), order.end(),
                   [&] (int a2, int b2) { return theta[(std::size_t) a2] < theta[(std::size_t) b2]; });

        // Back-substitute the small eigenvectors: u = L^-T v, then Ritz
        // vectors X = Z U (columns ordered ascending).
        std::vector<double> U ((std::size_t) p * (std::size_t) p);
        std::vector<double> uvec ((std::size_t) p);
        for (int jj = 0; jj < p; ++jj)
        {
            const int src = order[(std::size_t) jj];
            double* u = uvec.data();
            for (int i = 0; i < p; ++i)
                u[i] = V[(std::size_t) i * (std::size_t) p + (std::size_t) src];
            for (int i = p - 1; i >= 0; --i)
            {
                double s = u[i];
                for (int k = i + 1; k < p; ++k)
                    s -= Lp[(std::size_t) k * (std::size_t) p + (std::size_t) i] * u[k];
                u[i] = s / Lp[(std::size_t) i * (std::size_t) p + (std::size_t) i];
            }
            for (int i = 0; i < p; ++i)
                U[(std::size_t) i * (std::size_t) p + (std::size_t) jj] = u[i];
        }

        // Row j of the new X is sum_k U[k][j] * Z_k.
        parallelFor (nThreads, p, [&] (int j)
        {
            double* x = &Xt[(std::size_t) j * (std::size_t) n];
            for (int i = 0; i < n; ++i)
                x[i] = 0.0;
            for (int k = 0; k < p; ++k)
            {
                const double u = U[(std::size_t) k * (std::size_t) p + (std::size_t) j];
                if (u == 0.0)
                    continue;
                const double* z = &Zt[(std::size_t) k * (std::size_t) n];
                for (int i = 0; i < n; ++i)
                    x[i] += u * z[i];
            }
        });

        std::sort (theta.begin(), theta.end());

        // How far the block still is from its tolerance, as the largest
        // shortfall over the requested modes: the distance crosses 1 exactly
        // when the last of them settles. Every mode is measured, where the
        // test used to stop at the first failure, because the estimate above
        // needs the worst one rather than the first bad one. That is `wanted`
        // subtractions against a sweep that has just solved p systems of order
        // n, so it costs nothing worth counting.
        double distance = 0.0;
        for (int k = 0; k < wanted; ++k)
        {
            const double denom = std::max (std::abs (theta[(std::size_t) k]), 1.0e-12);
            const double rel = std::abs (theta[(std::size_t) k] - thetaPrev[(std::size_t) k]) / denom;
            const double tol = k < wanted / 2 ? options.toleranceLow : options.toleranceHigh;
            distance = std::max (distance, rel / std::max (tol, 1.0e-300));
        }

        converged = iter > 2 && distance < 1.0;
        thetaPrev = theta;
        report (progressFor (iter, distance));
        if (converged)
            break;
    }

    result.n = n;
    result.iterations = iter;
    result.converged = converged;
    result.eigenvalues.assign (theta.begin(), theta.begin() + wanted);
    result.vectors.assign (Xt.begin(), Xt.begin() + (std::ptrdiff_t) ((std::size_t) wanted * (std::size_t) n));

    report (1.0f);
    return result;
}

} // namespace fxme::math
