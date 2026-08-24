/*
  ------------------------------------------------------------------------------
    math/DenseLinearAlgebra.cpp — see DenseLinearAlgebra.h.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "DenseLinearAlgebra.h"

namespace fxme::math
{

bool choleskyFactorInPlace (double* a, int n)
{
    for (int j = 0; j < n; ++j)
    {
        double d = a[(std::size_t) j * (std::size_t) n + (std::size_t) j];
        for (int k = 0; k < j; ++k)
        {
            const double v = a[(std::size_t) j * (std::size_t) n + (std::size_t) k];
            d -= v * v;
        }
        if (d <= 0.0)
            return false;
        const double dj = std::sqrt (d);
        a[(std::size_t) j * (std::size_t) n + (std::size_t) j] = dj;

        for (int i = j + 1; i < n; ++i)
        {
            double s = a[(std::size_t) i * (std::size_t) n + (std::size_t) j];
            const double* ri = &a[(std::size_t) i * (std::size_t) n];
            const double* rj = &a[(std::size_t) j * (std::size_t) n];
            for (int k = 0; k < j; ++k)
                s -= ri[k] * rj[k];
            a[(std::size_t) i * (std::size_t) n + (std::size_t) j] = s / dj;
        }
    }
    return true;
}

void choleskySolveInPlace (const double* L, int n, double* b)
{
    for (int i = 0; i < n; ++i)
    {
        double s = b[i];
        const double* ri = &L[(std::size_t) i * (std::size_t) n];
        for (int k = 0; k < i; ++k)
            s -= ri[k] * b[k];
        b[i] = s / ri[i];
    }
    for (int i = n - 1; i >= 0; --i)
    {
        double s = b[i];
        for (int k = i + 1; k < n; ++k)
            s -= L[(std::size_t) k * (std::size_t) n + (std::size_t) i] * b[k];
        b[i] = s / L[(std::size_t) i * (std::size_t) n + (std::size_t) i];
    }
}

void symmetricMultiply (const double* A, int n, const double* x, double* y)
{
    for (int i = 0; i < n; ++i)
    {
        const double* row = &A[(std::size_t) i * (std::size_t) n];
        double s = 0.0;
        for (int j = 0; j < n; ++j)
            s += row[j] * x[j];
        y[i] = s;
    }
}

void jacobiEigenSymmetric (double* a, double* V, int p, int maxSweeps)
{
    const std::size_t pp = (std::size_t) p * (std::size_t) p;
    std::fill (V, V + pp, 0.0);
    for (int i = 0; i < p; ++i)
        V[(std::size_t) i * (std::size_t) p + (std::size_t) i] = 1.0;

    auto at = [&] (int i, int j) -> double& { return a[(std::size_t) i * (std::size_t) p + (std::size_t) j]; };
    auto vt = [&] (int i, int j) -> double& { return V[(std::size_t) i * (std::size_t) p + (std::size_t) j]; };

    for (int sweep = 0; sweep < maxSweeps; ++sweep)
    {
        // Relative convergence test: the entries scale with the eigenvalues
        // (an absolute threshold would never trigger and every call would
        // burn the full sweep budget).
        double off = 0.0, diag = 0.0;
        for (int i = 0; i < p; ++i)
        {
            diag += at (i, i) * at (i, i);
            for (int j = i + 1; j < p; ++j)
                off += at (i, j) * at (i, j);
        }
        if (off < 1.0e-22 * std::max (diag, 1.0e-300))
            break;

        // Skip rotations that cannot change the result at double precision.
        const double skip = 1.0e-28 * std::max (diag, 1.0e-300) / (double) (p * p);

        for (int i = 0; i < p - 1; ++i)
            for (int j = i + 1; j < p; ++j)
            {
                const double apq = at (i, j);
                if (apq * apq < skip)
                    continue;
                const double app = at (i, i), aqq = at (j, j);
                const double theta = 0.5 * (aqq - app) / apq;
                const double t = (theta >= 0.0 ? 1.0 : -1.0)
                                 / (std::abs (theta) + std::sqrt (theta * theta + 1.0));
                const double c = 1.0 / std::sqrt (t * t + 1.0);
                const double s = t * c;

                for (int k = 0; k < p; ++k)
                {
                    const double aik = at (k, i), ajk = at (k, j);
                    at (k, i) = c * aik - s * ajk;
                    at (k, j) = s * aik + c * ajk;
                }
                for (int k = 0; k < p; ++k)
                {
                    const double aik = at (i, k), ajk = at (j, k);
                    at (i, k) = c * aik - s * ajk;
                    at (j, k) = s * aik + c * ajk;
                }
                for (int k = 0; k < p; ++k)
                {
                    const double vik = vt (k, i), vjk = vt (k, j);
                    vt (k, i) = c * vik - s * vjk;
                    vt (k, j) = s * vik + c * vjk;
                }
            }
    }
}

std::vector<double> denseShiftedSum (const SymmetricOperator& A,
                                     double shift,
                                     const SymmetricOperator& M)
{
    const std::size_t n = (std::size_t) A.size();
    std::vector<double> out (n * n, 0.0);
    A.addToDense (out.data(), 1.0);
    M.addToDense (out.data(), shift);
    return out;
}

} // namespace fxme::math
