/*
  ------------------------------------------------------------------------------
    PlateModes.h

    Finite-element modal analysis of a thin (Kirchhoff) plate of arbitrary
    shape, with optional in-plane tension (membrane term) and mixed boundary
    conditions per boundary segment.

    Pure C++17, no JUCE dependency. Namespace fxme::acoustics.

    Physics
    -------
    The plate equation is used in its scaled form (flexural rigidity = 1,
    surface mass density = 1):

        d2w/dt2 + Laplacian^2 w - T Laplacian w = f(x, y, t)

    where T >= 0 is the dimensionless tension-to-flexural-stiffness parameter.
    The generalized eigenproblem  (K + T G) phi = omega^2 M phi  is discretised
    with Morley triangles (the classic non-conforming plate element: 3 vertex
    deflections + 3 mid-edge normal derivatives per element) on a FemMesh.

    The solver is run once at a reference tension T0 (ModalOptions::tension).
    For fast, audio-rate tension changes the result carries the per-mode
    Rayleigh coefficient  g_k = phi_k' G phi_k, giving the first-order
    (exact-at-T0) frequency law

        omega_k(T)^2  =  lambda_k  +  (T - T0) * g_k .

    Mode shapes are mass-normalised (phi' M phi = 1); the returned per-vertex
    values can be interpolated anywhere with evalNodalField (FemMesh.h).

    Boundary conditions
    -------------------
    The outline is split into segments by sorted arc-length parameters
    (BoundarySpec::segmentStart, in [0,1), same parameterisation as
    FemMesh::vertexParam). Each segment carries one of:

        Free             nothing constrained (natural)
        SimplySupported  w = 0
        Clamped          w = 0 and dw/dn = 0
        Sliding          dw/dn = 0 (w free)

    which map directly onto the Morley DOFs: deflections at boundary
    vertices, normal derivatives at boundary-edge midpoints. At a junction
    between two segments the stronger (more constrained) condition wins.

    Storage and cost
    ----------------
    The linear algebra lives in FxmeTools/math: compressed-sparse-row and dense
    storage behind a common operator interface, a bandwidth-reducing ordering,
    a profile factorisation and a dense one, and one shift-invert subspace
    iteration that runs on any of it. ModalOptions::storage picks the path.

    A Morley plate couples each degree of freedom to about eleven others
    whatever the mesh density, so the assembled matrices are far emptier than
    dense storage assumes: at n = 5000 free DOFs the three of them together are
    5 MB sparse against 590 MB dense. The factorisation of A + sigma M would be
    dense whatever the matrices look like -- Cholesky fills in -- were the
    unknowns not first renumbered (reverse Cuthill-McKee) so that the fill is
    trapped in a narrow envelope around the diagonal. Nothing is left quadratic
    after that, and the working set becomes

        sparse   ~1.1 n^1.5 doubles (the factor) + O(n) + the iteration block
        dense    4 n^2 doubles

    which at n = 6029 measured 39 MB against 1040 MB, and 5.5 s against 336 s
    for 128 modes. The dominant term is now the iteration block itself, 4 p n
    doubles for a block of p = m + max(8, m/2) vectors, so what a solve costs
    is set by how many modes are asked for rather than by how fine the mesh is.

    Call it from a background thread (it reports progress).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "FemMesh.h"

#include <cstddef>
#include <functional>

namespace fxme::acoustics
{

/** Ordered from the stiffest edge to the least stiff, which is the order
    `strength()` scores them in and the order the UI presents and cycles
    them in. Clamped constrains deflection and rotation, simple support
    only deflection, sliding only rotation, free neither.

    The numeric values are serialised by consumers, so a reordering is a
    file-format change: ModalDish carries a `bcOrder` migration for state
    written before this order. */
enum class BoundaryCondition
{
    Clamped = 0,
    SimplySupported,
    Sliding,
    Free
};

/** Per-segment boundary conditions over the outline arc parameter.
    segmentStart must be sorted ascending in [0,1); segment i spans
    [segmentStart[i], segmentStart[i+1]) (the last one wraps around to
    segmentStart[0]). Empty spec = fully clamped. */
struct BoundarySpec
{
    std::vector<double> segmentStart;
    std::vector<BoundaryCondition> segmentBc;

    BoundaryCondition bcAt (double t) const noexcept
    {
        const size_t n = std::min (segmentStart.size(), segmentBc.size());
        if (n == 0)
            return BoundaryCondition::Clamped;
        // Last segment whose start is <= t; before the first start we are on
        // the wrapped-around last segment.
        size_t seg = n - 1;
        for (size_t i = 0; i < n; ++i)
        {
            if (segmentStart[i] > t)
                break;
            seg = i;
        }
        if (t < segmentStart[0])
            seg = n - 1;
        return segmentBc[seg];
    }
};

/** How the assembled global matrices are stored. Both paths run the same
    eigensolver over the same element matrices, and agree bit for bit rather
    than merely to round-off: compressed rows keep their column indices sorted,
    so a sparse row walk sums the non-zeros in the same order a dense row walk
    does. `dense` is kept as the reference the sparse path is validated
    against, and as a fallback. */
enum class MatrixStorage
{
    sparse = 0,
    dense
};

struct ModalOptions
{
    int numModes = 32;              // how many modes to return (lowest first)
    double tension = 0.0;           // reference tension T0 (>= 0)
    double poissonRatio = 0.3;
    int numThreads = 0;             // eigensolver worker threads; 0 = auto
                                    // (about half the cores, at most 4)
    MatrixStorage storage = MatrixStorage::sparse;
    std::function<void (float)> progress;   // optional, called with 0..1
};

struct ModalResult
{
    /** lambda_k: eigenvalue at the reference tension = omega_k^2 (scaled
        units), ascending. Rigid-body / near-zero modes are dropped. */
    std::vector<double> lambda;

    /** g_k = phi_k' G phi_k, the tension sensitivity of mode k:
        omega_k(T)^2 = lambda_k + (T - tensionRef) * g_k. */
    std::vector<double> tensionG;

    /** Mass-normalised mode shapes sampled at the mesh vertices
        (shapes[k][v]; the mid-edge rotation DOFs are internal only). */
    std::vector<std::vector<float>> shapes;

    double tensionRef = 0.0;        // the T0 the eigenproblem was solved at

    /** Peak working set of the solve, in bytes: global matrices, the shifted
        factor and the iteration block. Measured rather than estimated, so it
        is the honest number to report or to compare storage paths with. */
    std::size_t solverBytes = 0;

    /** Which storage the solve actually used. */
    MatrixStorage storageUsed = MatrixStorage::sparse;

    int numModes() const noexcept { return (int) lambda.size(); }
    bool valid() const noexcept   { return ! lambda.empty(); }
};

/** Computes the lowest modes of the plate discretised on `mesh` with the
    given boundary conditions. Returns an empty result when the mesh is
    empty or over-constrained. Deterministic. */
ModalResult computePlateModes (const FemMesh& mesh,
                               const BoundarySpec& boundary,
                               const ModalOptions& options);

} // namespace fxme::acoustics
