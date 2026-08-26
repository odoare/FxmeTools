# FxmeTools / acoustics

Finite-element vibro-acoustics tools: mesh an arbitrary 2D shape, compute the
bending modes of the corresponding thin plate (with optional membrane
tension and mixed boundary conditions), display grid and modal fields as
filled contours.

The numerical core (`FemMesh`, `PlateModes`) is **pure C++17 with no JUCE
dependency**, so it can be unit-tested from a console target and reused
outside JUCE projects. Only the two display components require JUCE.

| File | Contents | Deps |
| --- | --- | --- |
| `FemMesh.h/.cpp` | `Point2`, polygon utilities, `FemMesh`, Delaunay mesh generation, point location, nodal interpolation | std only |
| `PlateModes.h/.cpp` | `BoundaryCondition`, `BoundarySpec`, Morley-element assembly, dense subspace eigensolver, `ModalResult` | std only |
| `FemViewComponent.h` | mesh / filled-contour display component | JUCE |
| `FemView3DComponent.h` | the same mesh and field as an orbited deformed surface | JUCE |

## Physics

Scaled Kirchhoff plate (flexural rigidity = 1, surface density = 1):

```
d²w/dt² + Δ²w − T Δw = f(x, y, t)
```

`T ≥ 0` is the dimensionless tension-to-flexural-stiffness parameter
(`T = 0`: pure plate; large `T`: membrane-like). The eigenproblem
`(K + T G) φ = ω² M φ` is discretised with **Morley triangles** (3 vertex
deflections + 3 mid-edge normal derivatives), the classic minimal
non-conforming element for the biharmonic operator. Eigenvalues converge
from below at O(h²), which the consuming project's FEM tests check against
analytic references.

## Typical use

```cpp
using namespace fxme::acoustics;

// 1. Mesh a shape (closed polygon, any orientation, coordinates O(1)).
std::vector<Point2> outline = { ... };
FemMesh mesh = generateMesh (outline, 1.0 / 16.0);   // target edge length

// 2. Boundary conditions per outline segment (arc-length parameter in [0,1)).
BoundarySpec bc;
bc.segmentStart = { 0.0, 0.5 };                       // two segments
bc.segmentBc    = { BoundaryCondition::Clamped, BoundaryCondition::Free };

// 3. Solve (background thread recommended; progress callback available).
ModalOptions opt;
opt.numModes = 32;
opt.tension  = 0.0;                                   // reference tension T0
opt.progress = [] (float p) { ... };
ModalResult modes = computePlateModes (mesh, bc, opt);

// 4. Use the modes.
double omega1 = std::sqrt (modes.lambda[0]);          // scaled rad/s
float  phi    = evalNodalField (mesh, modes.shapes[0], 0.3, 0.6);
```

### Fast tension changes

The solver runs once at the reference tension `T0 = opt.tension` and
returns per-mode Rayleigh coefficients `g_k = φₖᵀ G φₖ`, giving the
first-order (exact at `T0`) law

```
ωₖ(T)² = λₖ + (T − T0) · gₖ
```

so a *tension knob* can retune the whole bank at audio rate without
re-solving; re-solve (e.g. on a "compute" button) when precision at a very
different tension matters.

### Boundary conditions

| Value | Constrains |
| --- | --- |
| `Free` | nothing (natural) |
| `SimplySupported` | w = 0 |
| `Clamped` | w = 0 and ∂w/∂n = 0 |
| `Sliding` | ∂w/∂n = 0 |

They map one-to-one onto the Morley DOFs (vertex deflections /
mid-edge normal derivatives). At a segment junction the stronger
condition wins.

### Sizing / cost

Dense linear algebra: memory O(n²), time roughly O(n² · numModes) for
n free DOFs (n ≈ vertices + edges ≈ 3× vertices). A `targetH` of 1/12 to
1/24 of the shape size (n ≈ 300–2000) solves in ~0.1–10 s on one core —
run it from a background thread (`fxme::BackgroundTaskRunner` fits well)
and keep meshes below a few thousand DOFs.

## Display

`FemView3DComponent` draws the same mesh and field as a heightfield, turned
by click-and-drag and zoomed with the wheel, with the field as relief, flat
Lambert shading on each facet and the mesh stroked over it. Use it to watch the plate *move*; use the flat view to compare mode
shapes or to point at a position, which the 3D one deliberately does not
support — a screen point there is not a plate point, since the surface folds
over itself. Its setters mirror the flat view's, so one refresh path can feed
either.

`FemViewComponent` fits the mesh into its bounds (plate y axis up),
draws the grid and/or a per-vertex field as banded filled contours
(diverging colour map, quantised into `setContourLevels()` bands), and
reports clicks/drags in plate coordinates (`onPlateClick`/`onPlateDrag`).
Custom decorations (markers, boundary colouring) go in `paintOverlay`.
