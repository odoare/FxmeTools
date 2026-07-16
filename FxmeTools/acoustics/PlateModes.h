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

    Cost: dense linear algebra, O(n^2) memory / roughly O(n^2 * numModes)
    time for n free DOFs (n ~ vertices + edges). Fine up to a few thousand
    DOFs; call it from a background thread (it reports progress).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "FemMesh.h"

#include <functional>

namespace fxme::acoustics
{

enum class BoundaryCondition
{
    Free = 0,
    SimplySupported,
    Clamped,
    Sliding
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

struct ModalOptions
{
    int numModes = 32;              // how many modes to return (lowest first)
    double tension = 0.0;           // reference tension T0 (>= 0)
    double poissonRatio = 0.3;
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
