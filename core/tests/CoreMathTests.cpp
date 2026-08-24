/*
  ------------------------------------------------------------------------------
    CoreMathTests.cpp

    Unit tests for FxmeTools/math, with no JUCE and no acoustics in sight:

      1. Sparse and dense storage multiply identically, bit for bit.
      2. Reverse Cuthill-McKee produces a valid permutation and a much smaller
         factorisation profile.
      3. The profile Cholesky agrees with the dense one, in the natural order
         and under a renumbering.
      4. The subspace eigensolver reproduces analytically known eigenvalues.

    The test problem throughout is the 5-point Laplacian on a square grid: it
    is sparse the way a finite-element matrix is sparse, its graph is a genuine
    two-dimensional mesh graph (so the ordering test measures something real),
    and its eigenvalues are known in closed form.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/math/BandwidthOrdering.h>
#include <FxmeTools/math/DenseLinearAlgebra.h>
#include <FxmeTools/math/SkylineCholesky.h>
#include <FxmeTools/math/SparseMatrix.h>
#include <FxmeTools/math/SubspaceEigensolver.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace fxme::math;

static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

static void checkBelow (double got, double limit, const char* what)
{
    std::printf ("  [%s] %s: %.3e (limit %.1e)\n",
                 got <= limit ? "PASS" : "FAIL", what, got, limit);
    if (! (got <= limit))
        ++failures;
}

// ---------------------------------------------------------------------------
// The 5-point Laplacian on an m x m grid, natural (row-by-row) numbering.
// ---------------------------------------------------------------------------
struct GridLaplacian
{
    int m = 0;
    int n = 0;
    std::vector<int> index;         // grid position -> unknown number
    std::shared_ptr<const SparsityPattern> pattern;

    /** `scramble` numbers the unknowns in a fixed pseudo-random order instead
        of row by row. Row-by-row numbering of a square grid is already close
        to the best possible bandwidth, so an ordering measured against it
        looks unimpressive for reasons that have nothing to do with the
        ordering; an unstructured mesh arrives numbered by whatever the
        generator happened to do, which the scrambled case stands in for. */
    GridLaplacian (int side, bool scramble = false) : m (side), n (side * side)
    {
        index.resize ((std::size_t) n);
        for (int i = 0; i < n; ++i)
            index[(std::size_t) i] = i;

        if (scramble)
        {
            unsigned s = 987654321u;
            for (int i = n - 1; i > 0; --i)
            {
                s = s * 1664525u + 1013904223u;
                std::swap (index[(std::size_t) i], index[(std::size_t) (s % (unsigned) (i + 1))]);
            }
        }

        SparsityBuilder builder (n);
        forEachCoupling ([&] (int i, int j)
        {
            const int pair[2] = { i, j };
            builder.addClique (pair, 2);
        });
        pattern = builder.build();
    }

    /** Calls fn(i, j) once for the diagonal of each node and once for each
        grid edge, in unknown numbering. */
    template <class Fn>
    void forEachCoupling (Fn&& fn) const
    {
        for (int y = 0; y < m; ++y)
            for (int x = 0; x < m; ++x)
            {
                const int self = index[(std::size_t) (y * m + x)];
                fn (self, self);
                if (x + 1 < m) fn (self, index[(std::size_t) (y * m + x + 1)]);
                if (y + 1 < m) fn (self, index[(std::size_t) ((y + 1) * m + x)]);
            }
    }

    template <class Mat>
    void fill (Mat& a) const
    {
        forEachCoupling ([&a] (int i, int j)
        {
            if (i == j)
            {
                a.addEntry (i, i, 4.0);
            }
            else
            {
                a.addEntry (i, j, -1.0);
                a.addEntry (j, i, -1.0);
            }
        });
    }

    SparseSymmetricMatrix sparse() const
    {
        SparseSymmetricMatrix a (pattern);
        fill (a);
        return a;
    }

    DenseSymmetricMatrix dense() const
    {
        DenseSymmetricMatrix a (n);
        fill (a);
        return a;
    }
};

static std::vector<double> testVector (int n, int seed)
{
    std::vector<double> v ((std::size_t) n);
    unsigned s = 12345u + 7919u * (unsigned) seed;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525u + 1013904223u;
        v[(std::size_t) i] = (double) (s >> 8) / 8388608.0 - 1.0;
    }
    return v;
}

static double worstRelative (const std::vector<double>& a, const std::vector<double>& b)
{
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        num = std::max (num, std::abs (a[i] - b[i]));
        den = std::max (den, std::abs (a[i]));
    }
    return num / std::max (den, 1e-300);
}

// ---------------------------------------------------------------------------
// 1. Storage formats multiply identically
// ---------------------------------------------------------------------------
// Compressed rows keep their column indices sorted, so a sparse row walk adds
// the non-zeros in exactly the order a dense row walk does, and the two sums
// are the same floating-point number. Asserting *exact* equality rather than
// closeness is deliberate: it is what makes this able to catch a mis-set index
// rather than only a mis-set value.
static void testStorageMultiply()
{
    std::printf ("\n== Sparse and dense storage multiply identically ==\n");

    const GridLaplacian g (24);
    const auto sp = g.sparse();
    const auto dn = g.dense();

    std::printf ("  %d x %d grid: n = %d, %d non-zeros (%.1f per row)\n",
                 g.m, g.m, g.n, g.pattern->numNonZeros(),
                 (double) g.pattern->numNonZeros() / (double) g.n);

    bool identical = true;
    for (int trial = 0; trial < 4; ++trial)
    {
        const auto x = testVector (g.n, trial);
        std::vector<double> ys ((std::size_t) g.n), yd ((std::size_t) g.n);
        sp.multiply (x.data(), ys.data());
        dn.multiply (x.data(), yd.data());
        for (int i = 0; i < g.n; ++i)
            if (! (ys[(std::size_t) i] == yd[(std::size_t) i]))
                identical = false;
    }
    check (identical, "sparse and dense matrix-vector products are bit-identical");

    check ((int) sp.byteSize() * 20 < (int) dn.byteSize(), "sparse storage is far smaller");
    check (sp.droppedEntries() == 0, "no assembled entry fell outside the pattern");
}

// ---------------------------------------------------------------------------
// 2. Reverse Cuthill-McKee
// ---------------------------------------------------------------------------
static void testOrdering()
{
    std::printf ("\n== Reverse Cuthill-McKee ==\n");

    const GridLaplacian g (40, /* scramble */ true);
    const auto perm = reverseCuthillMcKee (*g.pattern);

    check ((int) perm.size() == g.n, "permutation covers every unknown");

    std::vector<char> seen ((std::size_t) g.n, 0);
    bool bijective = true;
    for (int v : perm)
    {
        if (v < 0 || v >= g.n || seen[(std::size_t) v])
            bijective = false;
        else
            seen[(std::size_t) v] = 1;
    }
    check (bijective, "permutation is a bijection");

    const auto inv = invertPermutation (perm);
    bool inverseOk = true;
    for (int i = 0; i < g.n; ++i)
        if (inv[(std::size_t) perm[(std::size_t) i]] != i)
            inverseOk = false;
    check (inverseOk, "invertPermutation inverts it");

    // Profile before and after. Rebuilding the pattern through the inverse
    // permutation is exactly what a finite-element assembly does when it
    // renumbers its degree-of-freedom map.
    SparsityBuilder builder (g.n);
    for (int i = 0; i < g.n; ++i)
        for (int k = g.pattern->rowStart[(std::size_t) i]; k < g.pattern->rowStart[(std::size_t) i + 1]; ++k)
        {
            const int pair[2] = { inv[(std::size_t) i],
                                  inv[(std::size_t) g.pattern->colIndex[(std::size_t) k]] };
            builder.addClique (pair, 2);
        }
    const auto reordered = builder.build();

    const double before = (double) profileSize (*g.pattern) / (double) g.n;
    const double after  = (double) profileSize (*reordered) / (double) g.n;
    std::printf ("  mean row bandwidth: %.1f natural -> %.1f reordered (%.1fx)\n",
                 before, after, before / after);
    check (after < before, "reordering shrinks the profile");
    check (reordered->numNonZeros() == g.pattern->numNonZeros(),
           "reordering preserves the non-zero count");
}

// ---------------------------------------------------------------------------
// 3. Profile Cholesky against the dense one
// ---------------------------------------------------------------------------
// Both are backward-stable factorisations of the same positive-definite
// matrix, so their solutions differ only by rounding — a far sharper statement
// than "both look plausible", and the one that catches an envelope indexing
// slip, which would otherwise show up as a quietly wrong answer.
static void testSkylineCholesky()
{
    std::printf ("\n== Profile Cholesky vs dense Cholesky ==\n");

    const GridLaplacian g (30);

    const SkylineCholesky skyline (g.sparse());
    check (skyline.ok(), "profile factorisation succeeded");

    auto denseCopy = g.dense();
    std::vector<double> raw (denseCopy.data(), denseCopy.data() + (std::size_t) g.n * (std::size_t) g.n);
    const DenseCholesky dense (std::move (raw), g.n);
    check (dense.ok(), "dense factorisation succeeded");

    if (! (skyline.ok() && dense.ok()))
        return;

    std::printf ("  profile %zu doubles (%.1f per row) vs dense %d\n",
                 skyline.profile(), (double) skyline.profile() / (double) g.n, g.n * g.n);

    double worstSolve = 0.0, worstResidual = 0.0;
    for (int trial = 0; trial < 4; ++trial)
    {
        const auto b = testVector (g.n, 100 + trial);

        auto xs = b, xd = b;
        skyline.solveInPlace (xs.data());
        dense.solveInPlace (xd.data());
        worstSolve = std::max (worstSolve, worstRelative (xd, xs));

        // Independent check that it really solved the stated system.
        std::vector<double> r ((std::size_t) g.n);
        g.sparse().multiply (xs.data(), r.data());
        for (int i = 0; i < g.n; ++i)
            r[(std::size_t) i] -= b[(std::size_t) i];
        double num = 0.0, den = 0.0;
        for (int i = 0; i < g.n; ++i)
        {
            num = std::max (num, std::abs (r[(std::size_t) i]));
            den = std::max (den, std::abs (b[(std::size_t) i]));
        }
        worstResidual = std::max (worstResidual, num / den);
    }
    checkBelow (worstSolve, 1e-10, "profile and dense solutions agree");
    checkBelow (worstResidual, 1e-12, "residual of the profile solve");

    // The same again on a renumbered matrix, which is how it is actually used:
    // permute the system, solve, permute back, and require the same answer.
    // An off-by-one in a renumbering survives every test that does not do this
    // round trip, because the factor of a permuted matrix is perfectly valid —
    // just of the wrong matrix.
    const auto perm = reverseCuthillMcKee (*g.pattern);
    const auto inv = invertPermutation (perm);

    SparsityBuilder builder (g.n);
    for (int i = 0; i < g.n; ++i)
        for (int k = g.pattern->rowStart[(std::size_t) i]; k < g.pattern->rowStart[(std::size_t) i + 1]; ++k)
        {
            const int pair[2] = { inv[(std::size_t) i],
                                  inv[(std::size_t) g.pattern->colIndex[(std::size_t) k]] };
            builder.addClique (pair, 2);
        }
    const auto reordered = builder.build();

    SparseSymmetricMatrix permuted (reordered);
    const auto natural = g.sparse();
    for (int i = 0; i < g.n; ++i)
        for (int k = g.pattern->rowStart[(std::size_t) i]; k < g.pattern->rowStart[(std::size_t) i + 1]; ++k)
            permuted.addEntry (inv[(std::size_t) i],
                               inv[(std::size_t) g.pattern->colIndex[(std::size_t) k]],
                               natural.values()[(std::size_t) k]);
    check (permuted.droppedEntries() == 0, "renumbered assembly stayed inside its pattern");

    const SkylineCholesky reorderedFactor (permuted);
    check (reorderedFactor.ok(), "renumbered profile factorisation succeeded");
    std::printf ("  renumbered profile %zu doubles (%.1f per row)\n",
                 reorderedFactor.profile(), (double) reorderedFactor.profile() / (double) g.n);
    check (reorderedFactor.profile() < skyline.profile(),
           "renumbering shrinks the factor");

    if (! reorderedFactor.ok())
        return;

    double worstRoundTrip = 0.0;
    for (int trial = 0; trial < 4; ++trial)
    {
        const auto b = testVector (g.n, 200 + trial);

        auto xd = b;
        dense.solveInPlace (xd.data());

        std::vector<double> pb ((std::size_t) g.n);
        for (int i = 0; i < g.n; ++i)
            pb[(std::size_t) inv[(std::size_t) i]] = b[(std::size_t) i];
        reorderedFactor.solveInPlace (pb.data());
        std::vector<double> xs ((std::size_t) g.n);
        for (int i = 0; i < g.n; ++i)
            xs[(std::size_t) i] = pb[(std::size_t) inv[(std::size_t) i]];

        worstRoundTrip = std::max (worstRoundTrip, worstRelative (xd, xs));
    }
    checkBelow (worstRoundTrip, 1e-10, "renumbered solve matches the dense one");
}

// ---------------------------------------------------------------------------
// 4. Subspace eigensolver against closed-form eigenvalues
// ---------------------------------------------------------------------------
// The 1D Laplacian tridiag(-1, 2, -1) of order n has eigenvalues
// 4 sin^2(k pi / 2(n+1)), k = 1..n, with M the identity.
static void testSubspaceEigensolver()
{
    std::printf ("\n== Subspace eigensolver vs closed form ==\n");

    const int n = 300;

    SparsityBuilder builder (n);
    for (int i = 0; i < n; ++i)
    {
        int pair[2] = { i, i };
        builder.addClique (pair, 2);
        if (i + 1 < n) { pair[1] = i + 1; builder.addClique (pair, 2); }
    }
    const auto pattern = builder.build();

    SparseSymmetricMatrix A (pattern), M (pattern);
    for (int i = 0; i < n; ++i)
    {
        A.addEntry (i, i, 2.0);
        M.addEntry (i, i, 1.0);
        if (i + 1 < n)
        {
            A.addEntry (i, i + 1, -1.0);
            A.addEntry (i + 1, i, -1.0);
        }
    }

    SparseSymmetricMatrix P = A;
    const double sigma = 1.0e-5 * A.trace() / M.trace();
    P.addScaled (M, sigma);

    const SkylineCholesky factor (P);
    check (factor.ok(), "shifted operator factorised");
    if (! factor.ok())
        return;

    // The solver stops on a relative change in the eigenvalues, so its
    // accuracy is whatever was asked of it: at the default tolerances the
    // error lands at 5e-6, at 1e-12 it lands at 8e-14, tracking the request
    // over eight orders of magnitude. Asking for a tight one here tests the
    // iteration rather than its stopping rule.
    SubspaceOptions opt;
    opt.numModes = 12;
    opt.numThreads = 1;
    opt.toleranceLow = 1e-12;
    opt.toleranceHigh = 1e-12;
    const auto result = subspaceEigenSolve (A, M, factor, opt);

    check (result.valid(), "eigensolver returned modes");
    check (result.converged, "eigensolver converged");
    if (! result.valid())
        return;

    double worst = 0.0;
    for (int k = 0; k < (int) result.eigenvalues.size(); ++k)
    {
        const double s = std::sin ((double) (k + 1) * M_PI / (2.0 * (n + 1)));
        const double expected = 4.0 * s * s;
        worst = std::max (worst, std::abs (result.eigenvalues[(std::size_t) k] - expected) / expected);
    }
    std::printf ("  %d eigenvalues in %d iterations\n",
                 (int) result.eigenvalues.size(), result.iterations);
    checkBelow (worst, 1e-11, "worst relative eigenvalue error");

    // Eigenvectors: check the residual of the pencil directly, which needs no
    // reference values and no sign or scale convention.
    double worstResidual = 0.0;
    std::vector<double> ax ((std::size_t) n), mx ((std::size_t) n);
    for (int k = 0; k < (int) result.eigenvalues.size(); ++k)
    {
        const double* x = result.vector (k);
        A.multiply (x, ax.data());
        M.multiply (x, mx.data());
        const double lambda = result.eigenvalues[(std::size_t) k];
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; ++i)
        {
            num = std::max (num, std::abs (ax[(std::size_t) i] - lambda * mx[(std::size_t) i]));
            den = std::max (den, std::abs (lambda * mx[(std::size_t) i]));
        }
        worstResidual = std::max (worstResidual, num / std::max (den, 1e-300));
    }
    // Eigenvectors converge as the square root of the eigenvalues for a
    // symmetric pencil, so this limit is the square root of the one above.
    checkBelow (worstResidual, 1e-5, "worst eigenvector residual |Ax - lambda Mx|");
}

int main()
{
    std::printf ("FxmeTools/math unit tests\n");
    testStorageMultiply();
    testOrdering();
    testSkylineCholesky();
    testSubspaceEigensolver();

    std::printf ("\n%s (%d failure%s)\n",
                 failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
