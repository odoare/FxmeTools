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

namespace
{
    /** Householder reduction of the symmetric p x p matrix `z` (row-major,
        lower triangle, destroyed) to tridiagonal form: p - 2 reflections, each
        annihilating everything below the sub-diagonal of a column, applied on
        both sides so the matrix stays symmetric throughout.

        On return `e` holds the sub-diagonal in e[1..p-1], `z` the reflection
        vectors parked in the part of the matrix they have just emptied, and
        `d[i]` the norm h of reflection i, which accumulateReflections needs
        and then overwrites with the tridiagonal diagonal. The two halves are
        split so the transform can be transposed in between: see
        accumulateReflections for why that is worth a p^2 pass. */
    void reduceToTridiagonal (double* z, int p, double* d, double* e)
    {
        const auto at = [&] (int i, int j) -> double&
        {
            return z[(std::size_t) i * (std::size_t) p + (std::size_t) j];
        };

        for (int i = p - 1; i >= 1; --i)
        {
            const int l = i - 1;
            double h = 0.0, scale = 0.0;

            if (l > 0)
            {
                for (int k = 0; k <= l; ++k)
                    scale += std::abs (at (i, k));
            }

            if (l <= 0 || scale <= 0.0)
            {
                // Nothing to annihilate: the row is already tridiagonal.
                e[i] = at (i, l);
                d[i] = 0.0;
                continue;
            }

            // Scaling first, so that h is formed without overflow on a badly
            // conditioned row.
            for (int k = 0; k <= l; ++k)
            {
                at (i, k) /= scale;
                h += at (i, k) * at (i, k);
            }

            double f = at (i, l);
            double g = f >= 0.0 ? -std::sqrt (h) : std::sqrt (h);
            e[i] = scale * g;
            h -= f * g;
            at (i, l) = f - g;

            // u = the reflection vector (row i), and p_vec = A u / h, kept in
            // e[0..l] while it is needed.
            f = 0.0;
            for (int j = 0; j <= l; ++j)
            {
                at (j, i) = at (i, j) / h;   // park u/h in column i

                g = 0.0;
                for (int k = 0; k <= j; ++k)
                    g += at (j, k) * at (i, k);
                for (int k = j + 1; k <= l; ++k)
                    g += at (k, j) * at (i, k);

                e[j] = g / h;
                f += e[j] * at (i, j);
            }

            // The rank-two update A <- A - u q' - q u', with q the vector
            // above corrected by the u'p term.
            const double hh = f / (h + h);
            for (int j = 0; j <= l; ++j)
            {
                f = at (i, j);
                g = e[j] - hh * f;
                e[j] = g;
                for (int k = 0; k <= j; ++k)
                    at (j, k) -= f * e[k] + g * at (i, k);
            }

            d[i] = h;
        }

        d[0] = 0.0;
        e[0] = 0.0;
    }

    /** Unrolls the reflections left by reduceToTridiagonal into the orthogonal
        matrix that produced the tridiagonal form, and replaces `d` with that
        matrix's diagonal.

        Works on `w`, the **transpose** of the reduction's output, which is the
        whole reason the reduction stops where it does. Untransposed, both the
        dot product and the update below run down columns, a stride of p and a
        cache miss per element for two thirds of the reduction's arithmetic.
        Transposed they are contiguous, and the one vector that is not (row i,
        the reflection itself, which nothing in the loop writes to) is gathered
        once per reflection rather than re-read for every j. */
    void accumulateReflections (double* w, int p, double* d)
    {
        const auto row = [&] (int i) { return &w[(std::size_t) i * (std::size_t) p]; };
        std::vector<double> u ((std::size_t) p);

        for (int i = 0; i < p; ++i)
        {
            const int l = i - 1;

            if (d[i] > 0.0)
            {
                for (int k = 0; k <= l; ++k)
                    u[(std::size_t) k] = row (k)[i];

                const double* wi = row (i);
                for (int j = 0; j <= l; ++j)
                {
                    double* wj = row (j);
                    double g = 0.0;
                    for (int k = 0; k <= l; ++k)
                        g += u[(std::size_t) k] * wj[k];
                    for (int k = 0; k <= l; ++k)
                        wj[k] -= g * wi[k];
                }
            }

            d[i] = row (i)[i];
            row (i)[i] = 1.0;
            for (int j = 0; j <= l; ++j)
            {
                row (i)[j] = 0.0;
                row (j)[i] = 0.0;
            }
        }
    }

    /** Implicit-shift QL on the tridiagonal (d, e), rotating the accumulated
        transform held **transposed** in `zt` (row j is eigenvector j). On
        return d holds the eigenvalues, unordered. False if a diagonal entry
        refused to split off within the iteration budget.

        Transposed because this is where the p^3 goes: each Givens rotation
        touches two columns of the transform across every row, which in
        row-major storage is a stride of p and a cache miss per element.
        Holding the transform by eigenvector makes the same loop contiguous,
        and the caller pays two p^2 transposes for it. */
    bool qlImplicitShift (double* d, double* e, double* zt, int p)
    {
        constexpr double eps = 2.220446049250313e-16;   // DBL_EPSILON
        constexpr int maxIterPerValue = 40;

        for (int i = 1; i < p; ++i)
            e[i - 1] = e[i];
        e[p - 1] = 0.0;

        for (int l = 0; l < p; ++l)
        {
            int iter = 0;

            while (true)
            {
                // Look for a negligible sub-diagonal entry: everything above
                // it has already split off into its own block.
                int m = l;
                for (; m < p - 1; ++m)
                {
                    const double dd = std::abs (d[m]) + std::abs (d[m + 1]);
                    if (std::abs (e[m]) <= eps * dd)
                        break;
                }

                if (m == l)
                    break;          // d[l] is an eigenvalue; move on

                if (++iter > maxIterPerValue)
                    return false;

                // Wilkinson shift, formed to avoid cancellation.
                double g = (d[l + 1] - d[l]) / (2.0 * e[l]);
                double r = std::hypot (g, 1.0);
                g = d[m] - d[l] + e[l] / (g + std::copysign (r, g));

                double s = 1.0, c = 1.0, shift = 0.0;
                bool underflowed = false;
                int i = m - 1;

                for (; i >= l; --i)
                {
                    double f = s * e[i];
                    const double b = c * e[i];
                    r = std::hypot (f, g);
                    e[i + 1] = r;

                    if (r <= 0.0)
                    {
                        // Underflow: recover by cancelling this entry and
                        // restarting the sweep.
                        d[i + 1] -= shift;
                        e[m] = 0.0;
                        underflowed = true;
                        break;
                    }

                    s = f / r;
                    c = g / r;
                    g = d[i + 1] - shift;
                    r = (d[i] - g) * s + 2.0 * c * b;
                    shift = s * r;
                    d[i + 1] = g + shift;
                    g = c * r - b;

                    double* zi  = &zt[(std::size_t) i * (std::size_t) p];
                    double* zi1 = &zt[(std::size_t) (i + 1) * (std::size_t) p];
                    for (int k = 0; k < p; ++k)
                    {
                        const double zf = zi1[k];
                        zi1[k] = s * zi[k] + c * zf;
                        zi[k]  = c * zi[k] - s * zf;
                    }
                }

                if (underflowed)
                    continue;

                d[l] -= shift;
                e[l] = g;
                e[m] = 0.0;
            }
        }

        return true;
    }

    void transposeSquare (const double* src, double* dst, int p)
    {
        for (int i = 0; i < p; ++i)
            for (int j = 0; j < p; ++j)
                dst[(std::size_t) j * (std::size_t) p + (std::size_t) i]
                    = src[(std::size_t) i * (std::size_t) p + (std::size_t) j];
    }
}

void symmetricEigenSolve (double* a, double* V, int p)
{
    if (p < 1)
        return;

    const std::size_t pp = (std::size_t) p * (std::size_t) p;

    if (p == 1)
    {
        V[0] = 1.0;
        return;
    }

    // The reduction runs on V, so `a` survives untouched and is still there
    // for the fallback below.
    std::copy (a, a + pp, V);

    std::vector<double> d ((std::size_t) p), e ((std::size_t) p);
    reduceToTridiagonal (V, p, d.data(), e.data());

    // Everything from here to the last line works on the transform held by
    // eigenvector rather than by component, which is what makes both cubic
    // loops contiguous.
    std::vector<double> zt (pp);
    transposeSquare (V, zt.data(), p);
    accumulateReflections (zt.data(), p, d.data());

    if (! qlImplicitShift (d.data(), e.data(), zt.data(), p))
    {
        jacobiEigenSymmetric (a, V, p);
        return;
    }

    transposeSquare (zt.data(), V, p);

    std::fill (a, a + pp, 0.0);
    for (int i = 0; i < p; ++i)
        a[(std::size_t) i * (std::size_t) p + (std::size_t) i] = d[(std::size_t) i];
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
