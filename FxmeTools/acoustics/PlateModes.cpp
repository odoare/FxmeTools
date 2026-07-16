/*
  ------------------------------------------------------------------------------
    PlateModes.cpp — see PlateModes.h for the API and the physics.

    Implementation notes
    --------------------
    * Morley element: on each triangle the deflection is the quadratic
      polynomial matching the 6 DOFs (3 vertex deflections, 3 mid-edge normal
      derivatives). Rather than hard-coding the classic closed-form shape
      functions, each element solves a 6x6 Vandermonde-like system in
      centroid-centred coordinates: robust, and it directly yields the
      polynomial coefficients from which curvatures (constant), gradients
      (linear) and values are evaluated.

    * Mid-edge normal DOFs use the *global* edge normal (edge stored v0 < v1,
      normal = left of v0->v1), so the DOF means the same thing in both
      adjacent elements.

    * Matrices: bending stiffness K (exact, curvatures constant), tension
      matrix G with the 3-midpoint rule (exact, gradients linear), mass M
      with a 6-point degree-4 rule (exact for the quartic integrand).

    * Eigensolver: dense Cholesky of (A + sigma*M) with A = K + T0*G, then
      subspace iteration with Rayleigh-Ritz. Suited to the few-thousand-DOF
      meshes this library targets; run it on a background thread.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "PlateModes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fxme::acoustics
{

namespace
{

// ---------------------------------------------------------------------------
// Small dense linear algebra helpers (double, row-major).
// ---------------------------------------------------------------------------

// Solves the 6x6 system C * X = I in place, i.e. inverts C. Gaussian
// elimination with partial pivoting. Returns false when singular.
bool invert6 (double C[6][6], double inv[6][6])
{
    double a[6][12];
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
        {
            a[i][j] = C[i][j];
            a[i][j + 6] = (i == j) ? 1.0 : 0.0;
        }

    for (int col = 0; col < 6; ++col)
    {
        int piv = col;
        for (int r = col + 1; r < 6; ++r)
            if (std::abs (a[r][col]) > std::abs (a[piv][col]))
                piv = r;
        if (std::abs (a[piv][col]) < 1.0e-14)
            return false;
        if (piv != col)
            for (int j = 0; j < 12; ++j)
                std::swap (a[piv][j], a[col][j]);

        const double d = a[col][col];
        for (int j = 0; j < 12; ++j)
            a[col][j] /= d;
        for (int r = 0; r < 6; ++r)
        {
            if (r == col)
                continue;
            const double f = a[r][col];
            if (f == 0.0)
                continue;
            for (int j = 0; j < 12; ++j)
                a[r][j] -= f * a[col][j];
        }
    }

    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            inv[i][j] = a[i][j + 6];
    return true;
}

/** In-place lower Cholesky of the n x n symmetric matrix `a` (row-major;
    upper triangle left stale). Returns false if not positive definite. */
bool choleskyFactor (std::vector<double>& a, int n)
{
    for (int j = 0; j < n; ++j)
    {
        double d = a[(size_t) j * (size_t) n + (size_t) j];
        for (int k = 0; k < j; ++k)
        {
            const double v = a[(size_t) j * (size_t) n + (size_t) k];
            d -= v * v;
        }
        if (d <= 0.0)
            return false;
        const double dj = std::sqrt (d);
        a[(size_t) j * (size_t) n + (size_t) j] = dj;

        for (int i = j + 1; i < n; ++i)
        {
            double s = a[(size_t) i * (size_t) n + (size_t) j];
            const double* ri = &a[(size_t) i * (size_t) n];
            const double* rj = &a[(size_t) j * (size_t) n];
            for (int k = 0; k < j; ++k)
                s -= ri[k] * rj[k];
            a[(size_t) i * (size_t) n + (size_t) j] = s / dj;
        }
    }
    return true;
}

/** Solves L L' x = b with the factor from choleskyFactor; b is overwritten. */
void choleskySolve (const std::vector<double>& L, int n, double* b)
{
    for (int i = 0; i < n; ++i)
    {
        double s = b[i];
        const double* ri = &L[(size_t) i * (size_t) n];
        for (int k = 0; k < i; ++k)
            s -= ri[k] * b[k];
        b[i] = s / ri[i];
    }
    for (int i = n - 1; i >= 0; --i)
    {
        double s = b[i];
        for (int k = i + 1; k < n; ++k)
            s -= L[(size_t) k * (size_t) n + (size_t) i] * b[k];
        b[i] = s / L[(size_t) i * (size_t) n + (size_t) i];
    }
}

/** y = A x for symmetric A (full square storage). */
void symMatVec (const std::vector<double>& A, int n, const double* x, double* y)
{
    for (int i = 0; i < n; ++i)
    {
        const double* row = &A[(size_t) i * (size_t) n];
        double s = 0.0;
        for (int j = 0; j < n; ++j)
            s += row[j] * x[j];
        y[i] = s;
    }
}

/** Cyclic Jacobi eigensolver for a small symmetric p x p matrix (row-major).
    On return `a` holds ~diagonal eigenvalues and V the eigenvectors
    (columns). */
void jacobiEigen (std::vector<double>& a, std::vector<double>& V, int p)
{
    V.assign ((size_t) p * (size_t) p, 0.0);
    for (int i = 0; i < p; ++i)
        V[(size_t) i * (size_t) p + (size_t) i] = 1.0;

    auto at = [&] (int i, int j) -> double& { return a[(size_t) i * (size_t) p + (size_t) j]; };
    auto vt = [&] (int i, int j) -> double& { return V[(size_t) i * (size_t) p + (size_t) j]; };

    for (int sweep = 0; sweep < 60; ++sweep)
    {
        double off = 0.0;
        for (int i = 0; i < p; ++i)
            for (int j = i + 1; j < p; ++j)
                off += at (i, j) * at (i, j);
        if (off < 1.0e-24)
            break;

        for (int i = 0; i < p - 1; ++i)
            for (int j = i + 1; j < p; ++j)
            {
                const double apq = at (i, j);
                if (std::abs (apq) < 1.0e-300)
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

// Deterministic pseudo-random in [-1, 1] for the subspace start block.
double prand (uint64_t k)
{
    k += 0x9e3779b97f4a7c15ull;
    k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ull;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebull;
    k ^= k >> 31;
    return (double) (k & 0xffffffffull) / 2147483648.0 - 1.0;
}

constexpr int strength (BoundaryCondition bc) noexcept
{
    switch (bc)
    {
        case BoundaryCondition::Clamped:         return 3;
        case BoundaryCondition::SimplySupported: return 2;
        case BoundaryCondition::Sliding:         return 1;
        case BoundaryCondition::Free:            return 0;
    }
    return 0;
}

bool constrainsRotation (BoundaryCondition bc) noexcept
{
    return bc == BoundaryCondition::Clamped || bc == BoundaryCondition::Sliding;
}

} // namespace

ModalResult computePlateModes (const FemMesh& mesh,
                               const BoundarySpec& boundary,
                               const ModalOptions& options)
{
    ModalResult result;
    result.tensionRef = options.tension;
    if (mesh.empty() || options.numModes < 1)
        return result;

    const auto report = [&] (float p)
    {
        if (options.progress)
            options.progress (std::clamp (p, 0.0f, 1.0f));
    };
    report (0.0f);

    const int nv = mesh.numVertices();
    const int ne = mesh.numEdges();

    // ------------------------------------------------------------------
    // DOF numbering with boundary conditions eliminated. DOF layout:
    // vertex deflections first, then mid-edge normal derivatives.
    // ------------------------------------------------------------------
    std::vector<int> vertexDof ((size_t) nv, -1);
    std::vector<int> edgeDof ((size_t) ne, -1);

    // A boundary vertex takes the *strongest* condition of its adjacent
    // boundary edges (junction rule).
    std::vector<int> vertexBcStrength ((size_t) nv, -1);   // -1 = interior
    for (int e = 0; e < ne; ++e)
    {
        if (! mesh.isBoundaryEdge (e) || mesh.edgeParam[(size_t) e] < 0.0)
            continue;
        const auto bc = boundary.bcAt (mesh.edgeParam[(size_t) e]);
        const int s = strength (bc);
        for (int v : { mesh.edges[(size_t) e].v0, mesh.edges[(size_t) e].v1 })
            vertexBcStrength[(size_t) v] = std::max (vertexBcStrength[(size_t) v], s);
    }

    int n = 0;
    for (int v = 0; v < nv; ++v)
    {
        // Deflection constrained when the strongest adjacent segment is
        // simply supported (2) or clamped (3).
        if (vertexBcStrength[(size_t) v] < 2)
            vertexDof[(size_t) v] = n++;
    }
    for (int e = 0; e < ne; ++e)
    {
        bool constrained = false;
        if (mesh.isBoundaryEdge (e) && mesh.edgeParam[(size_t) e] >= 0.0)
            constrained = constrainsRotation (boundary.bcAt (mesh.edgeParam[(size_t) e]));
        if (! constrained)
            edgeDof[(size_t) e] = n++;
    }

    if (n < 1)
        return result;

    // ------------------------------------------------------------------
    // Assembly of A = K + T0 G, plus G and M kept separately (G for the
    // per-mode tension coefficient, M for the eigenproblem).
    // ------------------------------------------------------------------
    const size_t nn = (size_t) n * (size_t) n;
    std::vector<double> A (nn, 0.0), G (nn, 0.0), M (nn, 0.0);

    const double nu = options.poissonRatio;
    const double T0 = std::max (0.0, options.tension);

    // Degree-4 quadrature on the reference triangle (6 points, weights sum 1).
    static const double qw[6] = { 0.223381589678011, 0.223381589678011, 0.223381589678011,
                                  0.109951743655322, 0.109951743655322, 0.109951743655322 };
    static const double qa[6] = { 0.108103018168070, 0.445948490915965, 0.445948490915965,
                                  0.816847572980459, 0.091576213509771, 0.091576213509771 };
    static const double qb[6] = { 0.445948490915965, 0.108103018168070, 0.445948490915965,
                                  0.091576213509771, 0.816847572980459, 0.091576213509771 };

    const int nt = mesh.numTriangles();
    for (int ti = 0; ti < nt; ++ti)
    {
        const auto& tv = mesh.triangles[(size_t) ti];
        const auto& te = mesh.triEdges[(size_t) ti];
        const Point2 P[3] = { mesh.vertices[(size_t) tv[0]],
                              mesh.vertices[(size_t) tv[1]],
                              mesh.vertices[(size_t) tv[2]] };

        const double x0 = (P[0].x + P[1].x + P[2].x) / 3.0;
        const double y0 = (P[0].y + P[1].y + P[2].y) / 3.0;

        const double areaX2 = (P[1].x - P[0].x) * (P[2].y - P[0].y)
                            - (P[2].x - P[0].x) * (P[1].y - P[0].y);
        const double area = 0.5 * areaX2;
        if (area <= 0.0)
            continue;

        // 6x6 DOF-to-monomial matrix in centroid-centred coordinates.
        // Monomials: {1, X, Y, X^2, X*Y, Y^2}.
        double C[6][6];
        for (int i = 0; i < 3; ++i)
        {
            const double X = P[i].x - x0, Y = P[i].y - y0;
            C[i][0] = 1.0;  C[i][1] = X;      C[i][2] = Y;
            C[i][3] = X * X; C[i][4] = X * Y; C[i][5] = Y * Y;
        }
        for (int s = 0; s < 3; ++s)
        {
            const auto& edge = mesh.edges[(size_t) te[s]];
            const Point2& E0 = mesh.vertices[(size_t) edge.v0];
            const Point2& E1 = mesh.vertices[(size_t) edge.v1];
            const double ex = E1.x - E0.x, ey = E1.y - E0.y;
            const double el = std::hypot (ex, ey);
            const double nx = -ey / el, ny = ex / el;   // global normal (left of v0->v1)
            const double mx = 0.5 * (E0.x + E1.x) - x0;
            const double my = 0.5 * (E0.y + E1.y) - y0;
            // n . grad of the monomials at the edge midpoint:
            C[3 + s][0] = 0.0;
            C[3 + s][1] = nx;
            C[3 + s][2] = ny;
            C[3 + s][3] = 2.0 * mx * nx;
            C[3 + s][4] = my * nx + mx * ny;
            C[3 + s][5] = 2.0 * my * ny;
        }

        double S[6][6];   // S[m][j] = monomial coefficient m of shape function j
        if (! invert6 (C, S))
            continue;     // degenerate sliver: skip its contribution

        // Constant curvature vector (w_xx, w_yy, 2 w_xy) of each shape function.
        double kxx[6], kyy[6], kxy2[6];
        for (int j = 0; j < 6; ++j)
        {
            kxx[j]  = 2.0 * S[3][j];
            kyy[j]  = 2.0 * S[5][j];
            kxy2[j] = 2.0 * S[4][j];
        }

        double Ke[6][6], Ge[6][6], Me[6][6];
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j)
            {
                // Bending energy: k' D k with D = [[1,nu,0],[nu,1,0],[0,0,(1-nu)/2]].
                Ke[i][j] = area * (kxx[i] * kxx[j] + kyy[i] * kyy[j]
                                   + nu * (kxx[i] * kyy[j] + kyy[i] * kxx[j])
                                   + 0.5 * (1.0 - nu) * kxy2[i] * kxy2[j]);
                Ge[i][j] = 0.0;
                Me[i][j] = 0.0;
            }

        // Tension term: 3-midpoint rule (exact: gradients are linear).
        for (int q = 0; q < 3; ++q)
        {
            const int i0 = q, i1 = (q + 1) % 3;
            const double mx = 0.5 * (P[i0].x + P[i1].x) - x0;
            const double my = 0.5 * (P[i0].y + P[i1].y) - y0;
            double gx[6], gy[6];
            for (int j = 0; j < 6; ++j)
            {
                gx[j] = S[1][j] + 2.0 * S[3][j] * mx + S[4][j] * my;
                gy[j] = S[2][j] + S[4][j] * mx + 2.0 * S[5][j] * my;
            }
            const double w = area / 3.0;
            for (int i = 0; i < 6; ++i)
                for (int j = 0; j < 6; ++j)
                    Ge[i][j] += w * (gx[i] * gx[j] + gy[i] * gy[j]);
        }

        // Mass: 6-point degree-4 rule (shape products are quartic).
        for (int q = 0; q < 6; ++q)
        {
            const double l0 = qa[q], l1 = qb[q], l2 = 1.0 - qa[q] - qb[q];
            const double X = l0 * (P[0].x - x0) + l1 * (P[1].x - x0) + l2 * (P[2].x - x0);
            const double Y = l0 * (P[0].y - y0) + l1 * (P[1].y - y0) + l2 * (P[2].y - y0);
            double Nv[6];
            for (int j = 0; j < 6; ++j)
                Nv[j] = S[0][j] + S[1][j] * X + S[2][j] * Y
                      + S[3][j] * X * X + S[4][j] * X * Y + S[5][j] * Y * Y;
            const double w = area * qw[q];
            for (int i = 0; i < 6; ++i)
                for (int j = 0; j < 6; ++j)
                    Me[i][j] += w * Nv[i] * Nv[j];
        }

        // Scatter into the free-DOF global matrices.
        int dof[6];
        for (int i = 0; i < 3; ++i)
            dof[i] = vertexDof[(size_t) tv[(size_t) i]];
        for (int s = 0; s < 3; ++s)
            dof[3 + s] = edgeDof[(size_t) te[(size_t) s]];

        for (int i = 0; i < 6; ++i)
        {
            if (dof[i] < 0)
                continue;
            for (int j = 0; j < 6; ++j)
            {
                if (dof[j] < 0)
                    continue;
                const size_t idx = (size_t) dof[i] * (size_t) n + (size_t) dof[j];
                A[idx] += Ke[i][j] + T0 * Ge[i][j];
                G[idx] += Ge[i][j];
                M[idx] += Me[i][j];
            }
        }
    }
    report (0.1f);

    // ------------------------------------------------------------------
    // Shifted operator P = A + sigma M, Cholesky-factored once. The shift
    // keeps P positive definite when rigid-body modes make A singular.
    // ------------------------------------------------------------------
    double traceA = 0.0, traceM = 0.0;
    for (int i = 0; i < n; ++i)
    {
        traceA += A[(size_t) i * (size_t) n + (size_t) i];
        traceM += M[(size_t) i * (size_t) n + (size_t) i];
    }
    const double sigma = std::max (1.0e-8, 1.0e-5 * traceA / std::max (traceM, 1.0e-30));

    std::vector<double> P (nn);
    for (size_t i = 0; i < nn; ++i)
        P[i] = A[i] + sigma * M[i];
    if (! choleskyFactor (P, n))
        return result;
    report (0.25f);

    // ------------------------------------------------------------------
    // Subspace iteration with Rayleigh-Ritz on (A, M).
    // ------------------------------------------------------------------
    const int wanted = std::min (options.numModes, n);
    const int p = std::min (n, wanted + std::max (8, wanted / 2));

    std::vector<double> X ((size_t) n * (size_t) p);
    for (size_t i = 0; i < X.size(); ++i)
        X[i] = prand (i);

    std::vector<double> Z ((size_t) n * (size_t) p);
    std::vector<double> W ((size_t) n * (size_t) p);       // scratch: M*Z or A*Z columns
    std::vector<double> Ap ((size_t) p * (size_t) p), Mp ((size_t) p * (size_t) p);
    std::vector<double> theta ((size_t) p, 0.0), thetaPrev ((size_t) p, -1.0);
    std::vector<double> col ((size_t) n);

    const int maxIters = 60;
    int iter = 0;
    for (; iter < maxIters; ++iter)
    {
        // Z = P^-1 (M X): one inverse-power step towards the low modes.
        for (int j = 0; j < p; ++j)
        {
            for (int i = 0; i < n; ++i)
                col[(size_t) i] = X[(size_t) i * (size_t) p + (size_t) j];
            symMatVec (M, n, col.data(), &W[0]);
            choleskySolve (P, n, &W[0]);
            for (int i = 0; i < n; ++i)
                Z[(size_t) i * (size_t) p + (size_t) j] = W[(size_t) i];
        }

        // Normalise columns of Z (keeps the small projected problem sane).
        for (int j = 0; j < p; ++j)
        {
            double s = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double v = Z[(size_t) i * (size_t) p + (size_t) j];
                s += v * v;
            }
            const double inv = s > 0.0 ? 1.0 / std::sqrt (s) : 1.0;
            for (int i = 0; i < n; ++i)
                Z[(size_t) i * (size_t) p + (size_t) j] *= inv;
        }

        // Projected matrices Ap = Z'AZ, Mp = Z'MZ.
        auto project = [&] (const std::vector<double>& Mat, std::vector<double>& small)
        {
            // W = Mat * Z (n x p)
            for (int i = 0; i < n; ++i)
            {
                const double* row = &Mat[(size_t) i * (size_t) n];
                for (int j = 0; j < p; ++j)
                {
                    double s = 0.0;
                    for (int k = 0; k < n; ++k)
                        s += row[k] * Z[(size_t) k * (size_t) p + (size_t) j];
                    W[(size_t) i * (size_t) p + (size_t) j] = s;
                }
            }
            // small = Z' W (p x p)
            for (int a2 = 0; a2 < p; ++a2)
                for (int b2 = 0; b2 < p; ++b2)
                {
                    double s = 0.0;
                    for (int k = 0; k < n; ++k)
                        s += Z[(size_t) k * (size_t) p + (size_t) a2]
                           * W[(size_t) k * (size_t) p + (size_t) b2];
                    small[(size_t) a2 * (size_t) p + (size_t) b2] = s;
                }
        };
        project (A, Ap);
        project (M, Mp);

        // Small generalized problem Ap v = theta Mp v via Cholesky + Jacobi.
        std::vector<double> Lp = Mp;
        if (! choleskyFactor (Lp, p))
            break;   // subspace degenerated; keep previous iterate

        // B = L^-1 Ap L^-T
        std::vector<double> B ((size_t) p * (size_t) p);
        {
            // First solve L Y = Ap (column-wise on Ap columns) -> Y = L^-1 Ap
            std::vector<double> Y = Ap;
            for (int j = 0; j < p; ++j)
                for (int i = 0; i < p; ++i)
                {
                    double s = Y[(size_t) i * (size_t) p + (size_t) j];
                    for (int k = 0; k < i; ++k)
                        s -= Lp[(size_t) i * (size_t) p + (size_t) k]
                           * Y[(size_t) k * (size_t) p + (size_t) j];
                    Y[(size_t) i * (size_t) p + (size_t) j]
                        = s / Lp[(size_t) i * (size_t) p + (size_t) i];
                }
            // Then B = Y L^-T  <=>  B L' = Y  <=>  (row-wise forward solves)
            for (int i = 0; i < p; ++i)
                for (int j = 0; j < p; ++j)
                {
                    double s = Y[(size_t) i * (size_t) p + (size_t) j];
                    for (int k = 0; k < j; ++k)
                        s -= B[(size_t) i * (size_t) p + (size_t) k]
                           * Lp[(size_t) j * (size_t) p + (size_t) k];
                    B[(size_t) i * (size_t) p + (size_t) j]
                        = s / Lp[(size_t) j * (size_t) p + (size_t) j];
                }
        }

        std::vector<double> V;
        jacobiEigen (B, V, p);

        // Eigenvalues on B's diagonal; sort ascending.
        std::vector<int> order ((size_t) p);
        for (int i = 0; i < p; ++i)
        {
            order[(size_t) i] = i;
            theta[(size_t) i] = B[(size_t) i * (size_t) p + (size_t) i];
        }
        std::sort (order.begin(), order.end(),
                   [&] (int a2, int b2) { return theta[(size_t) a2] < theta[(size_t) b2]; });

        // Back-substitute the small eigenvectors: u = L^-T v, then Ritz
        // vectors X = Z * U (columns ordered ascending).
        std::vector<double> U ((size_t) p * (size_t) p);
        std::vector<double> uvec ((size_t) p);
        for (int jj = 0; jj < p; ++jj)
        {
            const int src = order[(size_t) jj];
            double* u = uvec.data();
            for (int i = 0; i < p; ++i)
                u[i] = V[(size_t) i * (size_t) p + (size_t) src];
            for (int i = p - 1; i >= 0; --i)
            {
                double s = u[i];
                for (int k = i + 1; k < p; ++k)
                    s -= Lp[(size_t) k * (size_t) p + (size_t) i] * u[k];
                u[i] = s / Lp[(size_t) i * (size_t) p + (size_t) i];
            }
            for (int i = 0; i < p; ++i)
                U[(size_t) i * (size_t) p + (size_t) jj] = u[i];
        }

        for (int i = 0; i < n; ++i)
        {
            const double* zrow = &Z[(size_t) i * (size_t) p];
            for (int j = 0; j < p; ++j)
            {
                double s = 0.0;
                for (int k = 0; k < p; ++k)
                    s += zrow[k] * U[(size_t) k * (size_t) p + (size_t) j];
                W[(size_t) i * (size_t) p + (size_t) j] = s;
            }
        }
        std::swap (X, W);

        std::sort (theta.begin(), theta.end());

        // Convergence on the wanted eigenvalues.
        double maxRel = 0.0;
        for (int k = 0; k < wanted; ++k)
        {
            const double denom = std::max (std::abs (theta[(size_t) k]), 1.0e-12);
            maxRel = std::max (maxRel, std::abs (theta[(size_t) k] - thetaPrev[(size_t) k]) / denom);
        }
        thetaPrev = theta;
        report (0.25f + 0.7f * (float) (iter + 1) / (float) maxIters);
        // 1e-7 relative on omega^2 is ~5e-8 on frequency: far below audibility.
        if (iter > 2 && maxRel < 1.0e-7)
            break;
    }

    // ------------------------------------------------------------------
    // Extract results: drop rigid-body/near-zero modes, mass-normalise,
    // sample shapes at the vertices, compute tension coefficients.
    // ------------------------------------------------------------------
    const double dropBelow = std::max (1.0e-4, 1.0e-8 * std::abs (theta[(size_t) wanted - 1]));

    std::vector<double> x ((size_t) n), Mx ((size_t) n), Gx ((size_t) n);
    for (int k = 0; k < wanted; ++k)
    {
        const double lambda = theta[(size_t) k];
        if (! std::isfinite (lambda) || lambda < dropBelow)
            continue;

        for (int i = 0; i < n; ++i)
            x[(size_t) i] = X[(size_t) i * (size_t) p + (size_t) k];

        symMatVec (M, n, x.data(), Mx.data());
        double xMx = 0.0;
        for (int i = 0; i < n; ++i)
            xMx += x[(size_t) i] * Mx[(size_t) i];
        if (xMx <= 0.0)
            continue;
        const double scale = 1.0 / std::sqrt (xMx);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] *= scale;

        symMatVec (G, n, x.data(), Gx.data());
        double g = 0.0;
        for (int i = 0; i < n; ++i)
            g += x[(size_t) i] * Gx[(size_t) i];

        std::vector<float> shape ((size_t) nv, 0.0f);
        for (int v = 0; v < nv; ++v)
            if (vertexDof[(size_t) v] >= 0)
                shape[(size_t) v] = (float) x[(size_t) vertexDof[(size_t) v]];

        result.lambda.push_back (lambda);
        result.tensionG.push_back (std::max (0.0, g));
        result.shapes.push_back (std::move (shape));
    }

    report (1.0f);
    return result;
}

} // namespace fxme::acoustics
