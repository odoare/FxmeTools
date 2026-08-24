/*
  ------------------------------------------------------------------------------
    PlateModes.cpp — see PlateModes.h for the API and the physics.

    This file is the plate-specific half of the modal solve: boundary
    conditions, degree-of-freedom numbering, the Morley element matrices, and
    the modal quantities extracted at the end. Everything that is linear
    algebra rather than plate mechanics — matrix storage, Cholesky, the
    subspace eigensolver — lives in FxmeTools/math and knows nothing about
    plates.

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

    * Assembly runs in two passes over the triangles when the storage is
      sparse. The first gathers nothing but the six DOF indices per element and
      hands them to the sparsity builder; the second is the expensive one and
      scatters values into the pattern that pass produced. Both passes read the
      DOF indices through the same helper, so the pattern cannot fail to cover
      what the assembly scatters — and an element the value pass skips as
      degenerate costs a stored zero, nothing worse.

    * Eigensolver: dense Cholesky of (A + sigma*M) with A = K + T0*G, then
      shift-invert subspace iteration with Rayleigh-Ritz. The factorisation is
      still the dense one; the assembled operators are not.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "PlateModes.h"

#include <FxmeTools/math/DenseLinearAlgebra.h>
#include <FxmeTools/math/SparseMatrix.h>
#include <FxmeTools/math/SubspaceEigensolver.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace fxme::acoustics
{

namespace
{

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
    result.storageUsed = options.storage;
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
    const int nt = mesh.numTriangles();

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

    /** The 6 free-DOF indices of a triangle, -1 where a boundary condition
        removed one. Both assembly passes go through this, which is what makes
        the sparsity pattern a guaranteed superset of what is scattered. */
    const auto elementDofs = [&] (int ti, int dof[6])
    {
        const auto& tv = mesh.triangles[(size_t) ti];
        const auto& te = mesh.triEdges[(size_t) ti];
        for (int i = 0; i < 3; ++i)
        {
            dof[i]     = vertexDof[(size_t) tv[(size_t) i]];
            dof[3 + i] = edgeDof[(size_t) te[(size_t) i]];
        }
    };

    // ------------------------------------------------------------------
    // Storage for A = K + T0 G, plus G and M kept separately (G for the
    // per-mode tension coefficient, M for the eigenproblem). All three share
    // one sparsity pattern: same mesh, same DOF map, same couplings.
    // ------------------------------------------------------------------
    std::shared_ptr<const math::SparsityPattern> pattern;
    std::unique_ptr<math::AssemblableMatrix> A, G, M;

    if (options.storage == MatrixStorage::sparse)
    {
        math::SparsityBuilder builder (n);
        builder.reserveElements (nt, 6);
        for (int ti = 0; ti < nt; ++ti)
        {
            int dof[6];
            elementDofs (ti, dof);
            builder.addClique (dof, 6);
        }
        pattern = builder.build();

        A = std::make_unique<math::SparseSymmetricMatrix> (pattern);
        G = std::make_unique<math::SparseSymmetricMatrix> (pattern);
        M = std::make_unique<math::SparseSymmetricMatrix> (pattern);
    }
    else
    {
        A = std::make_unique<math::DenseSymmetricMatrix> (n);
        G = std::make_unique<math::DenseSymmetricMatrix> (n);
        M = std::make_unique<math::DenseSymmetricMatrix> (n);
    }
    report (0.05f);

    const double nu = options.poissonRatio;
    const double T0 = std::max (0.0, options.tension);

    // Degree-4 quadrature on the reference triangle (6 points, weights sum 1).
    static const double qw[6] = { 0.223381589678011, 0.223381589678011, 0.223381589678011,
                                  0.109951743655322, 0.109951743655322, 0.109951743655322 };
    static const double qa[6] = { 0.108103018168070, 0.445948490915965, 0.445948490915965,
                                  0.816847572980459, 0.091576213509771, 0.091576213509771 };
    static const double qb[6] = { 0.445948490915965, 0.108103018168070, 0.445948490915965,
                                  0.091576213509771, 0.816847572980459, 0.091576213509771 };

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
            const auto& edge = mesh.edges[(size_t) te[(size_t) s]];
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
        if (! math::invertMatrix<6> (C, S))
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
        elementDofs (ti, dof);

        for (int i = 0; i < 6; ++i)
        {
            if (dof[i] < 0)
                continue;
            for (int j = 0; j < 6; ++j)
            {
                if (dof[j] < 0)
                    continue;
                A->addEntry (dof[i], dof[j], Ke[i][j] + T0 * Ge[i][j]);
                G->addEntry (dof[i], dof[j], Ge[i][j]);
                M->addEntry (dof[i], dof[j], Me[i][j]);
            }
        }
    }

    // A dropped entry means the pattern and the assembly disagree about which
    // DOFs couple, which would quietly produce wrong modes. It cannot happen
    // while both go through elementDofs, so treat it as the bug it would be.
    if (A->droppedEntries() != 0 || G->droppedEntries() != 0 || M->droppedEntries() != 0)
        return result;
    report (0.1f);

    // ------------------------------------------------------------------
    // Shifted operator P = A + sigma M, Cholesky-factored once. The shift
    // keeps P positive definite when rigid-body modes make A singular.
    //
    // This is the one dense n x n allocation left on the sparse path, and the
    // reason its footprint is a few times smaller than the dense path's rather
    // than a hundred times.
    // ------------------------------------------------------------------
    const double traceA = A->trace();
    const double traceM = M->trace();
    const double sigma = std::max (1.0e-8, 1.0e-5 * traceA / std::max (traceM, 1.0e-30));

    const math::DenseCholesky shifted (math::denseShiftedSum (*A, sigma, *M), n);
    if (! shifted.ok())
        return result;
    report (0.25f);

    // ------------------------------------------------------------------
    // Subspace iteration with Rayleigh-Ritz on (A, M).
    // ------------------------------------------------------------------
    math::SubspaceOptions so;
    so.numModes = std::min (options.numModes, n);
    so.numThreads = options.numThreads;
    if (options.progress)
        so.progress = [&report] (float f) { report (0.25f + 0.70f * f); };

    const auto sub = math::subspaceEigenSolve (*A, *M, shifted, so);
    if (! sub.valid())
        return result;

    result.solverBytes = A->byteSize() + G->byteSize() + M->byteSize()
                         + (pattern != nullptr ? pattern->byteSize() : 0)
                         + shifted.byteSize()
                         + sub.blockBytes;

    // ------------------------------------------------------------------
    // Extract results: drop rigid-body/near-zero modes, mass-normalise,
    // sample shapes at the vertices, compute tension coefficients.
    // ------------------------------------------------------------------
    const int wanted = (int) sub.eigenvalues.size();
    const double dropBelow = std::max (1.0e-4, 1.0e-8 * std::abs (sub.eigenvalues[(size_t) wanted - 1]));

    std::vector<double> x ((size_t) n), Mx ((size_t) n), Gx ((size_t) n);
    for (int k = 0; k < wanted; ++k)
    {
        const double lambda = sub.eigenvalues[(size_t) k];
        if (! std::isfinite (lambda) || lambda < dropBelow)
            continue;

        std::copy (sub.vector (k), sub.vector (k) + n, x.begin());

        M->multiply (x.data(), Mx.data());
        double xMx = 0.0;
        for (int i = 0; i < n; ++i)
            xMx += x[(size_t) i] * Mx[(size_t) i];
        if (xMx <= 0.0)
            continue;
        const double scale = 1.0 / std::sqrt (xMx);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] *= scale;

        G->multiply (x.data(), Gx.data());
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
