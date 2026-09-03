# API changes and per-project migration

What this file is for: FxmeTools is shared by several plugins that are not all
worked on at once, so a change made while working on one of them has to be
findable months later while working on another. Every change to FxmeTools that a
consumer can observe goes here, newest section first, with what each project
still has to do about it.

The per-project checklist at the bottom is the part to read when returning to a
project after a break.

---

## `math/DenseLinearAlgebra.h` — `symmetricEigenSolve`, and the projected problem stops dominating

Additive: one new function, plus two loops rewritten inside
`subspaceEigenSolve`, which now calls it in place of `jacobiEigenSymmetric`.
**No consumer API change.** The same call returns the same modes, three times
faster on a fine mesh.

**Why.** Profiling the modal computation while fixing a progress bar turned up
something the architecture did not suggest. At Grid 32 (n = 2195, 256 modes,
block size p = 384) `jacobiEigenSymmetric` on the projected p-by-p problem was
**78% of the entire computation**, and the serial dense block containing it 88%.
Everything the finite-element side does (assembly, the profile factorisation,
the inverse-power solves, the sparse products) came to under 7% between them.
All the effort the sparse path spends was going into feeding a small dense
problem that then cost seven times as much as everything else put together.

Cyclic Jacobi costs `O(p^3)` per sweep and needs of the order of ten sweeps.
Householder tridiagonalisation followed by implicit-shift QL is a single pass of
that order.

| New function | What it is |
|---|---|
| `fxme::math::symmetricEigenSolve (double* a, double* V, int p)` | the same contract as `jacobiEigenSymmetric`: eigenvalues on the diagonal of `a`, eigenvectors as the columns of `V`, unordered |

`jacobiEigenSymmetric` is **not** deprecated and is not going anywhere. It is
the reference the new routine is tested against, and it is its fallback: the
reduction runs on the output buffer, so the input matrix is still intact, and a
QL that fails to converge falls back to Jacobi rather than returning an error
for the caller to handle. The new routine therefore always returns the
decomposition.

**What it measures.** Same machine, same meshes, the default ellipse:

| Grid | n | modes | before | after | |
|---:|---:|---:|---|---|---|
| 16 | 554 | 92 | 196 ms | 132 ms | 1.5x |
| 24 | 1243 | 207 | 3.26 s | 1.34 s | 2.4x |
| 32 | 2195 | 256 | 10.9 s | 3.21 s | 3.4x |

The routine on its own, against Jacobi on the same matrix: 5.9x at p = 120,
**19.6x at p = 384**. The end-to-end figure is smaller because the bottleneck
moves rather than disappearing.

**A third of that was cache, not arithmetic**, and it is the part worth knowing
before touching the file. Both cubic loops (unrolling the Householder
reflections into the accumulated transform, and the QL Givens rotations) walk
*columns* of the transform in their textbook form, which in row-major storage is
a stride of p and a cache miss per element. Holding the transform transposed, by
eigenvector rather than by component, makes both contiguous for two `p^2`
transposes. That alone was 158 ms to 113 ms at p = 384, and it is the reason the
reduction is split into `reduceToTridiagonal` and `accumulateReflections`: the
transpose happens between them, and neither half is useful without the other.

**Two triangular solves in `subspaceEigenSolve` went the same way**, once the
profile promoted them. Forming `B = L^-1 Ap L^-T` and back-substituting the
Ritz vectors both solved one column at a time, walking the Cholesky factor with
a stride of p; both now solve all p columns at once, a row at a time. The
division by the diagonal is kept as a division rather than a multiplication by a
reciprocal, which costs `p^2` divisions out of `p^3/2` flops and buys
**bit-identical output**, verified. Worth about 11%.

**One behavioural note.** A different algorithm rounds differently, so the modal
eigenvalues are no longer bit-identical to the Jacobi ones. They agree to
**4.8e-13** relative across all 141 modes of a Grid 20 solve, six orders below
the 1e-6 the iteration is asked to converge to, and `FxmeCoreMathTests` still
converges in the same 13 iterations to the same 8e-14 against the closed form.
Eigenvector signs are arbitrary in both routines and some do differ; that is a
gauge choice, and mode shapes enter a synthesis as products of pairs.

**Where the time goes now**, same solve, as a share of the sweep:

| | share of the sweep |
|---|---|
| before | Jacobi 78%, `B = L^-1 Ap L^-T` 7%, projection 5%, recombination 5%, rest 5% |
| after | eigensolve 39%, `B = L^-1 Ap L^-T` 20%, projection 16%, recombination 14%, rest 11% |

Balanced, where it used to be one function. The two serial blocks (the
eigensolve and the construction of `B`) are the obvious next target at 59%
between them, while the projection and the recombination are already parallel.
Cutting the block size `p` would reduce all four at once, since every one of
them is `O(p^3)` or `O(p^2 n)`; it is currently `wanted + max(8, wanted / 2)`,
which is generous.

**New test:** `testDenseEigensolvers` in `FxmeCoreMathTests` runs both routines
on the same unstructured matrix and checks that the spectra agree (2e-14), and,
independently of Jacobi, that the QL eigenpairs satisfy `A v = lambda v` (2e-15)
with unit norm (4e-15).

**Per project:** ModalDish is the only consumer of `math/` today and needs no
source change, only a rebuild of `FxmeCore`. Everyone else gains one function
they can ignore.

---

## `progress` callbacks report an estimate rather than an iteration count

Behavioural, no signature change. `ModalOptions::progress` and
`SubspaceOptions::progress` still take a `float` in 0..1 and are still called
from the worker thread. What the number means has changed, and a consumer
displaying it needs no edit.

**Why.** ModalDish's progress bar crept a little and then stood still until the
computation ended. It was reporting faithfully; the trouble was that it reported
two things that had nothing to do with elapsed time.

`computePlateModes` gave the first quarter of its range to degree-of-freedom
numbering, assembly and the factorisation, which together take **0.04%** of a
Grid 32 run (8 ms of 18.5 s). And `subspaceEigenSolve` reported
`(iter + 1) / maxIterations` with `maxIterations` at 60, while solves converge
in six to eight sweeps, so the solver only ever walked an eighth of its own
range. Between them the bar arrived at a quarter in the first frame and then
crossed nine percentage points over the remaining eighteen seconds.

**What changed.** The pre-solve phases now carry 1, 2 and 4% of the range,
which is roughly what they cost. The eigensolver reports the fraction of the
walk to convergence it has made: subspace iteration converges linearly, so the
distance to tolerance falls by a roughly constant factor per sweep and its
logarithm approaches zero in close to a straight line, which self-calibrates to
whatever convergence rate the mesh in hand actually has. The iteration count
stays underneath as a floor for a solve whose distance refuses to fall, the
first sweep (which has no history to estimate from, and is a fifth of the
running time on a large mesh) uses a nominal sweep count, and the reported value
is clamped monotone. An estimate may be wrong; a progress report that goes
backwards is a bug in the eye of whoever is watching it.

The convergence test itself was rewritten as a maximum over the requested modes
rather than a loop short-circuiting on the first failure, because the estimate
needs the worst mode rather than the first bad one. It is the same predicate and
the eigenvalues are unchanged, bit for bit.

**What it measures.** Grid 32, the bar's reading against the fraction of the
run actually elapsed:

| | bar reading / fraction of the run elapsed |
|---|---|
| before | 25% at 0.04% elapsed, then 26% to 34% over the remaining 99.96% |
| after | 16/13, 28/26, 50/39, 68/51, 75/63, 83/76, 90/88, 97/99 |

**Per project:** ModalDish needs no source change (it stores the value in an
atomic and polls it from the editor's timer). Any other consumer of
`computePlateModes` or `subspaceEigenSolve` gets a better estimate for free.

---

## New `acoustics/FemView3DComponent.h` — the plate mesh as a deformed surface

Purely additive: one new component, one new header in the module umbrella.
**No consumer needs to do anything.**

**Why.** `FemViewComponent` draws a mesh and a nodal field flat, as banded
contours. That is the right picture for comparing mode shapes and for pointing
at a position on the plate, and the wrong one for seeing a plate *move*: from
directly above, a bump and a dip differ only in colour. The new component draws
the same mesh and the same field as a heightfield, orbited with click-and-drag.

| Type | What it is |
|---|---|
| `fxme::acoustics::HeightFieldProjection` | orthographic orbit camera over a heightfield — `setViewport`, `setOrientation`, `project(x, y, z, &depth)` |
| `fxme::acoustics::FemView3DComponent` | the component: `setMesh`, `setField`, `setFieldScale`, `setHeightScale`, `setColours`, `setSurfaceColour`, `setShowGrid`, `setOrientation`, `setZoom`, `paintOverlay` |

The setter names deliberately mirror `FemViewComponent`, so a caller already
animating one can feed the other from the same code — ModalDish switches between
them from a combo box and shares its whole field-refresh path.

**Two things worth knowing before using it.**

The mesh is stroked over each facet as it is filled (`setShowGrid`, on by
default). Stroking inside the depth-sorted loop rather than in a pass of its
own is what removes the wireframe's own hidden lines — a nearer facet drawn
later covers the edges behind it, where one pass over all edges at the end
would draw the whole mesh through the plate. Shared edges are stroked twice,
once per adjacent triangle, and that is the mechanism rather than waste: the
stroke that survives belongs to the nearer facet.

Drag orbits, the wheel zooms about the centre of the view. There is no pan:
zooming well in leaves the plate's edges off screen, and turning it is the
gesture that stands in for one. `setZoom` is public so a caller can offer its
own control.

It has **no `onPlateClick`**, and that is not an omission. The drag gesture is
the camera, and more importantly a screen point in a deformed view is not a
plate point: the surface folds over itself, so the mapping is neither
single-valued nor invertible. Anything that needs pointing at belongs in the
flat view. `projection()` is exposed for callers that want to draw *into* the
scene, which is the well-defined direction.

The projection is **orthographic on purpose**. A perspective divide makes the
near edge of a flat plate larger than the far one, which the eye reads as the
plate being wedge-shaped rather than as depth.

`setColours` takes the flat view's four colours so one call themes both, but
the undeflected surface is drawn in a neutral *derived* from the background
rather than in the background itself — a diverging map centred on the
background makes a flat or quiet plate invisible, leaving only its silhouette.
`setSurfaceColour` overrides that neutral.

**Cost** is one `fillPath` per triangle per repaint, software-rendered with a
painter's-algorithm depth sort — exact here, since a heightfield cannot
self-intersect, and it needs nothing but `juce::Graphics`. It scales with the
mesh rather than the window; a few thousand triangles at 30 Hz is comfortable,
and a very fine mesh is better viewed flat.

**Per project:** ModalDish adds two view-selector entries. Any other consumer
gets a new component and can ignore it.

---

## `math/BandwidthOrdering.h`, `math/SkylineCholesky.h` — the factorisation goes sparse too

Additive: two new headers, and `computePlateModes` now renumbers and
factorises inside an envelope on its sparse path. **No consumer API change** —
the same call returns the same modes, considerably faster and far smaller.

**Why.** The previous change made the assembled matrices sparse but left the
shifted operator `A + sigma M` factorised densely, which capped the win at
about 3x. That was not for want of a sparse Cholesky: Cholesky *fills in*, and
the factor of a sparse matrix numbered as a mesh generator happens to produce
it is a dense matrix. The fix is a better numbering, not a cleverer
factorisation.

| Header | Contents |
|---|---|
| `math/BandwidthOrdering.h` | `reverseCuthillMcKee`, `invertPermutation`, `profileSize`, `firstStoredColumn` |
| `math/SkylineCholesky.h` | `SkylineCholesky` — profile (envelope) Cholesky, an `SpdSolver` like the dense one |

Also on `SparseSymmetricMatrix`: `addScaled` (fast path when two matrices share
a pattern) and `values()`.

**Two things to know if you use these directly.**

`reverseCuthillMcKee` returns a permutation and stops. `SkylineCholesky`
factorises whatever numbering it is given and its envelope is only narrow if
someone made it so — handing it an unordered matrix is not wrong, merely
pointless. Where to apply the permutation is a real decision: for a
finite-element assembly it belongs in the degree-of-freedom map, before
anything is assembled, so that every matrix, every eigenvector and every
exported nodal value is in the new numbering already and nothing is permuted
back. `PlateModes.cpp` does exactly that, in four lines. Applying it closer to
the solver means threading it through each of those steps, which is how one
gets plausible-looking wrong eigenvectors.

`SkylineCholesky::solveInPlace` is `const` and uses no scratch, so any number
of threads may solve different right-hand sides against one factor at once —
which is what a block eigensolver wants, and why the permutation is not applied
there.

**What it measures.** Same machine, same meshes, dense against the current path:

| n | modes | dense | sparse + profile | |
|---:|---:|---|---|---|
| 2149 | 64 | 13.1 s / 125 MB | 0.45 s / 7 MB | 29x / 17x |
| 3841 | 128 | 114.8 s / 419 MB | 3.6 s / 25 MB | 32x / 17x |
| 6029 | 128 | 335.5 s / 1.0 GB | 5.5 s / 39 MB | 61x / 26x |
| 15381 | 256 | (7.6 GB, would not run) | 48.4 s / 196 MB | |
| 23865 | 256 | (18.2 GB, would not run) | 83.8 s / 309 MB | |

The gain is bigger than the `O(n^3/3)` factorisation alone explains, because
the factorisation was never the expensive part: the `p` triangular solves *per
iteration* were, and they drop from `O(n^2)` to `O(n^1.5)` with it. Nothing in
the solve is quadratic in `n` any longer, and the leading term is now the
iteration block, `4 p n` doubles — so cost is set by the number of modes asked
for, not by the mesh.

**One behavioural note.** Changing the elimination order changes the rounding,
so sparse and dense no longer agree bit for bit end to end. They agree to
whatever the iteration was asked to converge to, and the split is sharp: on a
24-mode solve the first twelve (held to 1e-6) agree to 1e-14 in the eigenvalue
and 4e-6 in shape, while the last few (entitled to 1e-4) differ by 1e-5. The
storage layer is still bit-identical — that claim was about `multiply`, and
still holds.

**New test target:** `FxmeCoreMathTests` in `core/tests/`, covering the math
layer against closed-form answers with no acoustics involved.

**Per project:** ModalDish needs no source change. Its Grid knob stops at 48 for
a reason that no longer exists (the dense solver needed 737 MB there), so that
cap is now a product decision rather than a numerical one.

---

## New `core/FxmeTools/math/` — linear algebra split out of the plate solver, and sparse matrix storage

Additive, with one behavioural default changed inside `computePlateModes`.
**No consumer API breaks**, but the plate solver now stores its matrices
sparsely by default, which changes its memory footprint (down) and its speed
(up) without changing a single returned number.

**Why.** `acoustics/PlateModes.cpp` had grown a private dense linear algebra
library inside its anonymous namespace: Cholesky, symmetric matrix-vector
product, Jacobi eigensolver, a 6x6 inverse, a thread splitter, and a subspace
iteration. None of that is about plates, all of it is generally useful, and
keeping it hidden meant it could not be tested, reused or replaced
independently of the physics.

It now lives in `core/FxmeTools/math/`:

| Header | Contents |
|---|---|
| `math/LinearOperator.h` | `SymmetricOperator`, `AssemblableMatrix`, `SpdSolver` |
| `math/DenseLinearAlgebra.h` | `DenseSymmetricMatrix`, `DenseCholesky`, `choleskyFactorInPlace`, `jacobiEigenSymmetric`, `invertMatrix<N>` |
| `math/SparseMatrix.h` | `SparsityPattern`, `SparsityBuilder`, `SparseSymmetricMatrix` |
| `math/SubspaceEigensolver.h` | `subspaceEigenSolve (A, M, shiftedSolver, options)` |
| `math/ParallelFor.h` | `parallelFor`, `defaultWorkerCount` |

The eigensolver never sees a matrix: it multiplies through
`SymmetricOperator` and solves through `SpdSolver`. That is what makes the
storage a call-site choice rather than a rewrite.

**What changed behaviourally.** A Morley plate element couples each degree of
freedom to about eleven others whatever the mesh density, so the assembled
matrices were mostly zeros in dense storage. `ModalOptions::storage` now
selects `MatrixStorage::sparse` (the default) or `MatrixStorage::dense`, and
`ModalResult` reports `solverBytes` (measured, not estimated) and
`storageUsed`.

The shifted operator `A + sigma M` is still factorised densely, so the working
set went from `4 n^2` doubles to `n^2` plus the iteration block, rather than
the two orders of magnitude a sparse factorisation would give. Measured back
to back, same mesh both ways:

| n | modes | dense | sparse |
|---:|---:|---|---|
| 2149 | 64 | 13.1 s / 125 MB | 7.3 s / 36 MB |
| 3841 | 128 | 114.8 s / 419 MB | 59.9 s / 122 MB |
| 6029 | 128 | 335.5 s / 1.0 GB | 171.0 s / 287 MB | Making the factorisation
sparse as well (bandwidth-reducing ordering plus a profile Cholesky) is the
next step, and it plugs in as another `SpdSolver` with nothing else moving.

Both paths produce **bit-identical** results, not merely equal-to-round-off
ones: compressed rows keep their column indices sorted, so a sparse row walk
sums the non-zeros in the same order a dense row walk does. ModalDish's
`FemTests` asserts this, which turns "the two storage paths agree" into a test
sharp enough to catch an indexing slip.

**Per project:** ModalDish is the only consumer of `acoustics/` so far and needs
no source change — the default simply got cheaper. A consumer wanting the old
behaviour exactly sets `options.storage = MatrixStorage::dense`. Anything that
needs a symmetric generalized eigenproblem solved can now use `math/` directly
without pulling in the plate code.

---

## `acoustics/FemViewComponent.h` — `setFieldScale()`, and a field that can animate

Purely additive: one new method, and an internal rewrite of the rasteriser.
The default behaviour is unchanged, so **no consumer needs to do anything.**

**Why.** ModalDish now draws the live plate displacement in the same contour
view it draws mode shapes in, refreshed from a 30 Hz timer. Two things in
`renderFieldImage()` made that impossible:

- it normalised every field on its own maximum, which is right for a still
  picture (a mode shape has no natural scale) and wrong for an animation:
  a decaying ring would look eternal and silence would come up as amplified
  noise. `setFieldScale (maxAbsValue)` lets the caller hold the reference
  across frames; `0`, the default, keeps the old self-normalisation.
- it cost a `std::round`, a `std::pow` and a `Colour` interpolation per
  *pixel*, plus a fresh `juce::Image` allocation per update. The colour map
  is quantised into `2*levels-1` bands, so those colours are now built once
  per render into a `PixelARGB` table and looked up, written straight through
  `BitmapData::getPixelPointer`, into an image that is reused whenever the
  size has not changed.

The visible output is identical band for band; only the cost and the
normalisation control changed.

**Per project:** ModalDish is the only consumer of `acoustics/` so far. Any
future one gets the speed-up for free and can ignore `setFieldScale()`.

---

## `components/FxmeNumberBox.h` — a new control, not a change

Purely additive: a new `fxme::FxmeNumberBox` component, registered in the
module manifest. No existing symbol touched, so **no consumer needs to do
anything** to keep building.

**Why.** Mango's denser effect panels (Grain, Gater, AuxSend and others
picked up several new knobs each in the recent std-deviation work) started
running into knobs too small to keep their name and value legible. A
`FxmeSlider` subclass — a bordered box with the parameter's name and value as
text and a thin fill strip along the bottom, dragged like a rotary knob
(`RotaryVerticalDrag`'s relative delta mapping, not a linear track's absolute
position, which would make a box this small unusably twitchy) and right-click
editable exactly like `FxmeSlider` already is. `paint()` is the only thing
it overrides; attachments, ranges, `setCentralValue()`, `setShowLabel()`
are all inherited.

**Drop-in for an existing knob.** It reads the same `Slider` ColourIds a
knob already has themed — `rotarySliderFillColourId` (body),
`rotarySliderOutlineColourId` (border), `trackColourId` (fill strip),
`thumbColourId` (value text) — so swapping `fxme::FxmeSlider` for
`fxme::FxmeNumberBox` in a consumer is a type change, not a re-theming; any
existing per-control accent-colouring helper keeps working unchanged.

**Per project:** Mango is the first (only, so far) consumer. Nothing else
uses it yet.

---

## `GrainLooper::triggerForPeriod()` — exact-period loops

Purely additive: `trigger()` and `setCrossfade()` are unchanged, so **no
consumer needs to do anything.** New method only, on `dsp/GrainLooper.h`
(core).

**Why.** `trigger()`'s instance *period* is the recorded length minus the
crossfade (see the class comment), not the recorded length itself — by
design, for the overlap that keeps the grain train at unity gain through the
seams. A caller that wants the loop to land exactly on a beat, or exactly on
a pitch, gets one fade's worth of drift instead. This was found auditing
Mango's block-duration sync, and confirmed worse than expected in Bloom:
Bloom's MIDI-tracking grain mode (`grainStep[k] = 1/hz`, `BloomEngine.cpp`)
sets the recorded length to a note's exact period with no crossfade
compensation (it runs on the class's 30 ms default). For any note above
~16.7 Hz — i.e. the entire practical range, including the bottom of an
88-key keyboard — `setCrossfade`'s own half-grain clamp collapses the fade to
exactly half the recorded length, so the loop period is exactly half the
requested one and the effect plays **one octave sharp** of the note held, not
a few cents off.

**`triggerForPeriod (periodSeconds, fadeSeconds)`** records
`periodSeconds + fadeSeconds` and sets the crossfade to match, cancelling the
offset; the fade is capped to `periodSeconds` first so a disproportionately
long fade against a short period shortens the fade rather than reintroducing
a wrong period. Callers that want the current "close, minus one fade"
behaviour keep calling `trigger()` — this is a new option, not a replacement.

**Per project:**

- **Mango** — `GrainDupEffect` switches to it (see the per-project table).
- **Bloom** — **not touched.** Its MIDI-tracking mode is a full octave sharp
  today, confirmed as an existing bug, but its output is still musically "in
  tune" (an octave is a consonant interval), and the fix wasn't applied
  because it's a live, presumably-in-use parameter and this needed thinking
  through first (Bloom's own multi-take staggered scheduling, keyed off the
  *current* period-shortening behaviour in ways not fully understood from
  outside the project, made a blind fix risky). Revisit deliberately, as its
  own piece of work, when picking Bloom back up — don't fix it as a side
  effect of something else.

---

## Licensing split (2026-08-22)

**The most consumer-visible change in this file, and the only one with legal
rather than technical consequences.** The two halves now carry different terms:

| half | SPDX |
|---|---|
| `core/` | `LGPL-3.0-or-later` (unchanged) |
| `FxmeTools/` | `AGPL-3.0-or-later OR LicenseRef-FXME-Commercial` (was LGPL) |

`core/` is framework-free and enforced to stay that way, so it is not a JUCE
derivative and its licence is independent of the framework's. `FxmeTools/` only
builds against JUCE, so it mirrors JUCE 8's own dual licence rather than
fighting it. Full reasoning in `LICENSE.md` at the repository root.

**Nothing breaks at compile time, and no code changed.** What changed is what a
consuming plugin may be distributed under:

- A plugin links the JUCE half, so distributing it means either releasing it
  under the AGPLv3 with sources, or holding both a commercial JUCE licence and
  commercial terms for this module.
- **The plugins in this family currently declare LGPL-3.0 in their own LICENSE
  files and READMEs.** That is now inconsistent with the module they link, and
  each project needs its own decision. This repository cannot make it for them,
  and none of them was edited.
- A target that links **only** `FxmeCore` takes on no JUCE obligation at all and
  needs only the LGPL. Headless tests, offline renderers, Pure Data externals
  and console tools are all in that category. This is the case the split was
  built to make possible, and it is the reason the auxiliary targets were worth
  wiring to `FxmeCore` rather than to the whole module.

A file promoted from `FxmeTools/` into `core/` is re-licensed from
AGPL/commercial to LGPL as part of the move; the header must be changed to
match. See the submodule's `CLAUDE.md`.

## The core/shell split (branch `fxme-core-split`)

FxmeTools is now two halves: `core/`, which has no JUCE at all, and
`FxmeTools/`, the JUCE module. See `core/README.md` for the design and the
repository `CLAUDE.md` for the working rules.

### What did NOT change

**The include spelling.** `<FxmeTools/dsp/Biquad.h>` still resolves whether the
header ended up in core or stayed module-side, because `core/` is a second
include root. No consumer include was edited, and none should be — do not
"fix" an include to point at `core/`.

### Required in every consumer: link FxmeCore

Core is a real static library, so it has to be added and linked. Consumers
going through `fxmetools_attach()` get this for free — it is already wired into
`cmake/FxmeTools.cmake`. Consumers that register the module directly need:

```cmake
# <FXME> is wherever the submodule sits: lib/FxmeTools in most projects,
# Source/libs/FxmeTools in TeAr.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/<FXME>/core FxmeCore)
juce_add_module(${CMAKE_CURRENT_SOURCE_DIR}/<FXME>/FxmeTools)
target_link_libraries(FxmeTools INTERFACE FxmeCore)   # module sources include core headers
```

and `FxmeCore` in the target's own `target_link_libraries`.

**The trap:** any target that reaches FxmeTools headers *without* going through
`fxmetools_attach()` — a headless test executable, a console tool — has its own
include roots and will stop finding whatever moved into core. SuperMoTo had
three such targets. Give them `${FXMETOOLS_ROOT}` and link `FxmeCore`.

### Source-breaking: `MidiTools` moves into core

`midi/MidiTools.h` is now a core header. It had been the largest remaining
piece of pure music theory stuck on the JUCE side, blocked by nothing but its
text and container types — no file, no image, no `ValueTree` — so it needed
substitutions rather than an abstract interface.

Three new core primitives carry it, and they are worth knowing about because
everything promoted after this will use them too:

| type | header | what it is for |
|---|---|---|
| `fxme::StringRef` | `<FxmeTools/util/StringRef.h>` | a string *parameter*. Converts implicitly from `juce::String`, `std::string` or a literal, so call sites do not change |
| `fxme::ArrayView<T>` | `<FxmeTools/util/ArrayView.h>` | a read-only sequence, in or out. Converts implicitly from `juce::Array<T>` and `std::vector<T>`, and answers to `isEmpty()`, `size()`, `indexOf()` and `[]` the way `juce::Array` does |
| `fxme::trim`, `toLower`, `endsWith`, … | `<FxmeTools/util/StringUtils.h>` | the parsing operations, on `StringRef`, returning `std::string` |

`StringRef` and `ArrayView` use the same structural-conversion trick as
`AudioBufferView`: they detect the JUCE type by its shape (`toRawUTF8()` /
`data()` + `size()`) without including anything from JUCE. That is why almost
nothing had to be edited.

**What actually breaks.** Only the return types, and only where the result is
used as a JUCE type rather than passed straight on:

| was | is now | fix |
|---|---|---|
| `Scale::getScaleTypeNames()` → `juce::StringArray` | `Scale::scaleTypeNames`, `Scale::numScaleTypes` | `juce::StringArray (Scale::scaleTypeNames, Scale::numScaleTypes)` — same as `Lfo` above |
| `Chord::getSortedSet()` → `juce::SortedSet<int>` | `std::vector<int>`, sorted and duplicate-free | assign to `auto` |
| `Chord::getName()` → `const juce::String&` | `const std::string&` | `==` against a literal still works; `.isEmpty()` becomes `.empty()` |
| `getNoteName`, `getFrenchChordName`, the four `getRandom*` → `juce::String` | `std::string` | `juce::String` constructs from it implicitly, so only `.isEmpty()`-style calls on the result need an edit |
| `euclidianRythm()` → `juce::Array<bool>` | `std::vector<char>` | `for (bool b : …)` is unchanged; `std::vector<bool>` was avoided because its proxy references behave unlike every other vector here |
| `isChordEqual (Collection, …)` | `isChordEqual (ArrayView<int>, …)` | it had no callers; a `juce::Array` still binds |

`Scale::getNotes()`, `Chord::getDegrees()` and `Chord::getRawNotes()` return
`ArrayView<int>` instead of `const juce::Array<int>&`, which is deliberately
*not* in that table: every call site kept compiling, including
`for (int n : …)`, `.isEmpty()`, `.size()`, `.indexOf()` and `const auto&`
binding. Binding `const auto&` to the returned view is safe — it extends the
temporary's lifetime, and the view points into a member that outlives it. What
does not work is storing one past the owner's lifetime; use it, do not keep it.

Two JUCE-side signatures moved with it, both inside this repository:

- `ScaleKeyboardComponent::update()` takes `fxme::ArrayView<int>` for its scale
  and input notes, so a core `Scale` binds to it directly. A `juce::Array`
  still converts, so no caller changed.
- `Arpeggiator` needed one edit, at the `getSortedSet()` call.

Also new: `fxme::systemRandom()` in `<FxmeTools/util/Random.h>`, a thread-local
shared generator for free functions with no instance to hold one. Anything
running per block should still own a `Random` member.

Only TeAr uses any of this, and it needed exactly one line changed — the
`scaleType` parameter's name list.

### Source-breaking: `Lfo` choice lists

Three functions returning a JUCE string container became string literals plus a
count, so the header could stop depending on JUCE. `MidiTools` above did the
same thing to `Scale::getScaleTypeNames()` later, for the same reason:

| was | is now |
|---|---|
| `Lfo::shapeChoices()` | `Lfo::shapeNames`, `Lfo::numShapes` |
| `Lfo::syncRateChoices()` | `Lfo::syncRateNames`, `Lfo::numSyncRates` |
| `Lfo::syncDivisionChoices()` | `Lfo::syncDivisionNames`, `Lfo::numSyncDivisions` |

One line per call site, because `StringArray` already has a matching
constructor:

```cpp
// before
auto shapes = fxme::Lfo::shapeChoices();
// after
juce::StringArray shapes (fxme::Lfo::shapeNames, fxme::Lfo::numShapes);
```

The names stayed in core rather than moving JUCE-side because they index into
`syncRateBeats()` and `syncDivisionBeats()`, which are core. Each beats table
now carries a `static_assert` tying its length to the matching name list.

### Behavioural: `fxme::Random` default seeding

Not a compile error — read this one even if everything builds.

`fxme::Random()` originally seeded from a fixed constant, so every instance in a
process and every run produced the identical stream. JUCE's default constructor
seeds randomly. Two noise sources sharing a sequence are not independent noise:
they sum coherently, +6 dB instead of +3, and collapse to the centre of the
image instead of spreading.

`fxme::Random()` now seeds from a process-wide counter, the object's address and
the clock, as JUCE's does. `fxme::Random(seed)` is unchanged and remains exactly
reproducible.

Two consequences:

- **Anything that default-constructed and relied on a repeatable stream no
  longer gets one.** Nothing currently does; use `Random(seed)` if you need it.
- **`Random(seed)` produces a different sequence than JUCE's `Random(seed)` for
  the same seed** — same LCG family, so identical statistical character, but a
  different stream. Only matters where a specific rendering was being matched.

The default constructor reads a clock, so construct at prepare time, not on the
audio thread. The seeded constructor stays audio-thread safe.

### Silent, but worth knowing

- **`AmbixToStereo::process`** now takes `ConstAudioBufferView` / `AudioBufferView`
  instead of `AudioBuffer<float>` references, and **`CracksGenerator::prepare`**
  takes `fxme::ProcessSpec`. Both convert implicitly from the JUCE types, so no
  call site changes. This is by design — do not add adapters or overloads.
- **Leak detectors were removed** from `RmsMeter`, `SignalGenerator`,
  `SpectralBandSplitter`, `SpectrumAnalyzer`, `SynchronizedSweep` and
  `WaveformTap`, which have no core equivalent. The classes are still
  non-copyable, via deleted copy operations. Debug-time leak reporting for these
  is gone.
- **`SpectrumAnalyzer` and `SpectralBandSplitter` now share `fxme::RealFft`.**
  Their levels match because both use the same transform with the same scaling
  — the reason the splitter originally used JUCE's FFT. That coupling is real
  and would break silently: if you change the FFT under one, check the other.
- **`WaveformTap::getTotalPushed()`** returns `std::int64_t` rather than the JUCE
  alias. Same type; the spelling changed.

### New in core, available to everyone

`fxme::Fft` / `fxme::RealFft` (radix-2, JUCE's exact semantics — natural bin
order, forward unscaled, inverse scaled by 1/N; no size ceiling, unlike WDL's
32768), `fxme::SmoothedValue` (linear, JUCE's ramp arithmetic step for step),
`fxme::AudioBuffer` (owning counterpart to `AudioBufferView`), `fxme::StringRef`
+ `StringUtils.h` and `fxme::ArrayView` (see the `MidiTools` section above),
`fxme::systemRandom()`, and the existing `Math.h` / `Random.h` /
`ProcessSpec.h` helpers.

---

## Per-project checklist

**Fourteen** projects on this machine embed FxmeTools: eleven directly, and
three that reach it *through FxmeFX*. **Nothing breaks in any of them until its
submodule pointer moves forward** — the split is only visible once a project
bumps. So the breakage arrives one project at a time, months apart, which is
what this file is for.

As of 2026-08-20, ten are through plus this repository's own test suite. The
four outstanding ones are all direct consumers that wire the module by hand
(AmbiProbe, AmbiRR2, Bloom, Localizer); they are also the only ones left that
need real CMake work rather than a pointer bump.

### Direct consumers

Their own `.gitmodules` names FxmeTools.

| project | submodule path | state | last built against the split |
|---|---|---|---|
| AmbiProbe | `lib/FxmeTools` | CMake safe, **never built** | — |
| AmbiRR2 | `lib/FxmeTools` | CMake safe, **never built** | — |
| Bloom | `lib/FxmeTools` | CMake safe, **never built** | — |
| Dede | `lib/FxmeTools` | done (wired by hand) | 2026-08 |
| ModalDish (was FemPlate) | `lib/FxmeTools` | done | 2026-08 |
| FxmeFX | `lib/FxmeTools` | done (Pd externals needed core) | 2026-08 |
| Localizer | `lib/FxmeTools` | CMake safe, **never built** | — |
| Mango | `lib/FxmeTools` | done | 2026-08 |
| Neorix | `lib/FxmeTools` | done | 2026-08 |
| SuperMoTo | `lib/FxmeTools` | done | 2026-08-20 |
| TeAr | `Source/libs/FxmeTools` | done | 2026-08-20 |

### Transitive consumers, through FxmeFX

**These do not appear in any search for a FxmeTools submodule.** They embed
FxmeFX and use the FxmeTools nested inside it, reaching it as
`<FxmeFX path>/lib/FxmeTools`. They also compile FxmeFX's effect sources
straight into their own target by path, so they inherit its source changes too.

| project | FxmeFX at | reaches FxmeTools as | state | last built against the split |
|---|---|---|---|---|
| FlowSynth | `lib/FxmeFX` | `lib/FxmeFX/lib/FxmeTools` | done | 2026-08-20 |
| FxmeSampler | `FxmeFX` | `FxmeFX/lib/FxmeTools` | done | 2026-08-20 |
| Mechanodd | `lib/FxmeFX` | `lib/FxmeFX/lib/FxmeTools` | done | 2026-08-20 |

All three are through. What each actually cost, because the spread is the
interesting part:

- **FxmeSampler**: one submodule pointer. No source edit, no CMake edit. All
  four of its targets (three kits plus the `FxmeSamplerDev` host) already called
  `fxmetools_attach()`, so `FxmeCore` arrived on its own.
- **Mechanodd**: one pointer plus two lines. Its `ModEngine` wrapped
  `Lfo::shapeChoices()` and `syncRateChoices()` behind its own accessors, so
  fixing the two forwarders fixed all five call sites.
- **FlowSynth**: one pointer, four source edits and a CMake change. Its
  `ModEngine` has the same two forwarders, but two further call sites bypass the
  wrapper and reach `fxme::Lfo::syncRateChoices()` directly
  (`Analysis/AnalysisBank.cpp`, `UI/AnalysisPanelComponent.cpp`). Its
  `FlowSynthVideoTest` console target also compiles a FxmeTools source by path
  with only the module include root, so it gained `${FXMETOOLS_CORE_DIR}` and
  `FxmeCore` — preventative, since nothing it compiles reaches core today.

**Two traps specific to the transitive layout.** A grep for `shapeChoices` in
FlowSynth returns sixteen hits, but eleven belong to its own
`params::shapeChoices()`, which names terrain shapes and has nothing to do with
the LFO: match on `fxme::Lfo::` rather than the bare name. And because these
projects compile FxmeFX's effect sources by path, a bump brings effect
*behaviour* changes as well as library ones (IR loading moved off the audio
thread, the shared biquad, the look-and-feel retrofit), which no compiler
checks. Load one in a DAW after bumping.

Two things follow that do not apply to the direct consumers:

- **The bump is two levels deep, and it is ordered.** They pin FxmeFX, which
  pins FxmeTools. FxmeFX has to commit and push its own FxmeTools pointer
  before any of them can pick the split up at all — so FxmeFX is a hard
  prerequisite, not merely "one of the fourteen".
- **Their wiring is already right.** All three
  `include(.../lib/FxmeTools/cmake/FxmeTools.cmake)` and call
  `fxmetools_attach()`, so `FxmeCore` arrives with no CMake edit. That makes
  them *less* work than AmbiProbe and friends, not more — what remains is the
  renamed-API sweep and a build.

To find every consumer, search `.gitmodules` for FxmeTools **and** FxmeFX. A
search for FxmeTools alone finds eleven of the fourteen, which is how these
three were missed on the first pass.

Not consumers, despite using the `fxme::` namespace: MoTo, Vibe, MapSynth,
Mercator, PictOSC, VanDerPol, 3body-problem and Gloubiboulga. Those are
Projucer-era projects on the predecessor library FxmeJuceTools, and are
untouched by any of this.

**"CMake safe" is a claim about wiring, not about sources.** It means the
project includes `cmake/FxmeTools.cmake` and so gets core with no edit. It says
nothing about whether a renamed API breaks a call site, and that is the failure
mode that actually bites: the `Lfo` rename went through five consumers cleanly
and then broke this repository's own test suite, which only TeAr builds. Treat
the four never-built projects (AmbiProbe, AmbiRR2, Bloom, Localizer) as
unverified.

"Safe" means the project does `include(.../cmake/FxmeTools.cmake)`, which adds
the core subdirectory and hangs it off the module, so linking `FxmeTools` pulls
core in with no edit. Calling `fxmetools_attach()` is not required for this —
merely including the helper is enough.

"Breaks" means the project calls `juce_add_module()` directly and never includes
the helper, so it never gets core at all. Those three need the three lines above.

### The auxiliary-target trap

Worth checking in any project, because it is invisible until it bites: a target
that reaches FxmeTools headers **without linking the module** — a console test,
an offline render check — has its own include roots and stops finding whatever
moved. Linking `FxmeCore` fixes it, or add the second root by hand.

Known instances: SuperMoTo had three (fixed). FxmeFX had thirteen — every one
of its Pure Data externals (fixed). Mango has two. ModalDish had one, whose
case was worse than a missing header (fixed) — see below.

The FxmeFX case is the one worth internalising, because the target was *right*
to avoid the JUCE module and still broke. A Pd external is headless DSP: it
deliberately omits `FxmeTools` and reached the header-only helpers through a
bare `lib/FxmeTools` include dir. That dir covers the module half only, so every
migrated header disappeared from it at once. The fix is not to add a second
include path but to link `FxmeCore` — DSP without a framework is exactly what
core is, so a Pd external is its ideal consumer. One line in the shared helper
covered all thirteen.

**Rule of thumb:** if a target uses FxmeTools DSP but not FxmeTools GUI, it
should link `FxmeCore`, not add include paths.

### Per project

**SuperMoTo** — done. Three offline test targets were given `${FXMETOOLS_ROOT}`
and `FxmeCore` explicitly. Builds and runs; `Tests/SweepTest` passes against
`fxme::Fft`. One open item: it uses `fxme::SignalGenerator` in `SplMeterEngine`,
so its stimulus noise differs from before — not a regression, but worth a listen.

**Dede** — done. Registers the module directly, so it was wired by hand.
`Reverb.h`, previously an uncommitted file in Dede's own checkout, is now in
core. Compiles, links and runs.

**FxmeFX** — migrated and building; **not yet committed or listened to**.

Two changes were needed. Twelve `Lfo` call sites across six files (Chorus,
Flanger, Phaser — each a processor and a component), the split's only
source-breaking change. And `Source/PdCommon/PdExternal.cmake` now links
`FxmeCore`, which fixed all thirteen Pure Data externals at once — see the
auxiliary-target trap above; `<FxmeTools/dsp/VuMeter.h>` was what broke first.

The plugin CMake itself needed nothing: including `cmake/FxmeTools.cmake` was
already enough, even though FxmeFX never calls `fxmetools_attach()`.

Result: 13 plugins and 13 Pd externals build clean. As the heaviest user of
`dsp/` it exercises far more of the moved code than SuperMoTo or Dede, which
makes it the strongest evidence the split is sound — but it is a compile, not a
listen. Chorus, Flanger and Phaser drive `ModLfo`/`Lfo`, and Freeze drives
`SpectralFreeze`; those are worth hearing before the branch merges.

**ModalDish** (renamed from FemPlate in 2026-08) — done. It was the
worst-affected: it needed the CMake wiring, *and* its `FemTests` target
compiled two FxmeTools sources by path:

```cmake
lib/FxmeTools/FxmeTools/acoustics/FemMesh.cpp
lib/FxmeTools/FxmeTools/acoustics/PlateModes.cpp
```

Both had moved to `core/FxmeTools/acoustics/`, which failed as a missing *file*
at configure time rather than as a missing header — a louder failure than the
rest, at least. Fixed by linking `FxmeCore`, which already compiles both, and
deleting the two lines rather than repointing them. All four of its test
targets now link `FxmeCore` on their own line.

It is still the only consumer of `acoustics/`, and it has since contributed
back to it: `simplifyPolygon` / `simplifyPolygonTo` in `FemMesh` (closed-polygon
Douglas-Peucker, used to take a 128-point spline outline down to something a
person can drag by hand) came out of its shape editor, with
`tests/CoreGeometryTests.cpp` alongside.

**Mango** — done. Plugin, Standalone and VST3 all link (0 undefined symbols,
core archive members verifiably pulled into the bundle), and both test suites
pass: `ctest` 2/2, 26,769 checks. Its CMakeLists and test fixes were left
uncommitted for the author to land.

The most core-dependent consumer so far: *every* FxmeTools header it uses — the
dsp/ kernels, DeterministicRandom, the midi/ sequencing, SpectralFreeze — has
moved to core. It needed the three CMake lines, plus `FxmeCore` on `MangoTests`.
`MangoRenderTest` needed nothing: it links the module and gets core transitively.

`MangoTests` is worth noting as the third distinct shape of the auxiliary-target
trap. SuperMoTo's were JUCE console apps; FxmeFX's were Pd externals; Mango's is
a plain `add_executable` with **no JUCE linked at all** — "unit tests for the
JUCE-free pieces". That is core's ideal consumer, and linking `FxmeCore` is not
a workaround there but the natural expression of what the target already was.

Its seed-determinism test is **not** affected by the `fxme::Random` change, now
confirmed by running it: Mango draws through `fxme::detrand::u01 (seed, lane,
block, draw)`, a pure function of its arguments, which moved to core untouched.

Two `MangoTests` failures surfaced, **both pre-existing and unrelated to the
split** — proven by running the identical test source, flags and `fft.o` against
Mango's pre-sync FxmeTools (`bb324dd`), where they failed on the same two lines.
Both are now resolved, but in opposite ways, and the difference is the point:

- `testDownsampler` was a **real bug, since fixed**. `Downsampler.h` latched on
  the *last* sample of each group instead of holding from the first
  (0,0,0,**3**,3,3,3,**7**… where the test wants 0,0,0,0,**4**,4,4,4…): with
  `phase` starting at 1 so the first input is taken immediately, incrementing
  before the test left `phase` at `inc` rather than `0`, costing the opening
  group a sample. Fixed by latching before advancing. **This changes how the
  decimator sounds** — one sample per group — so presets leaning on the old
  timing shift slightly.
- `testSpectralFreezeMulti`: `rmsRatio = 0.3909` against a required 0.7–1.4.
  Read that number carefully — it is **not** an 8 dB level error, which is what
  it looks like at first glance. The test compares the *first* wash block at
  width 0 against the *second* wash block at width 1, and the wash level swings
  by a factor of ~2.7 from block to block at a fixed width (randomised-phase
  resynthesis, plus the capture->wash crossfade making block 1 low). Measured
  like for like, block 1 vs block 1, width 0 sits **1.8 dB** below width 1 —
  consistent at block 2 vs block 2 as well.

  And even the 1.8 dB was measurement noise. Averaged over 40 identities and 8
  wash blocks the blend is flat to within 0.09 dB from width 0 to width 1 — the
  `a² + b² = 1` normalisation in `setWidth` is correct and was never touched.
  **The only defect here was in the test**, which has been fixed to capture two
  instances, advance them in lockstep, skip the crossfade block and average
  before comparing; the tolerance is now 0.9-1.1 rather than a 0.7-1.4 that only
  ever accommodated an untrustworthy measurement.

  Worth remembering as a method, not just an outcome: a single block of a
  randomised-phase wash says nothing about level, because it varies 2.7x on its
  own. Two such measurements agreeing says almost nothing either. This was read
  as an 8 dB bug, then a 1.8 dB bug, before averaging showed there was none.

Neither is fixed here: both are audible behaviour in shipped effects, so
changing them changes how existing presets sound. `Downsampler.h` is in core
now, so a fix lands in this repository.

Note how they stayed hidden: `MangoTests` is opt-in (`MANGO_BUILD_TESTS=OFF`)
and its `main()` returned on the first failure, so the Downsampler bug masked
every test after it — the three SpectralFreeze cases included. That harness now
runs all 19 cases and reports them together, verified by injecting failures into
an early and a late case and confirming both are reported.

**Neorix** — needs the CMake wiring. Nothing else; no `Lfo` calls, no `Random`.

**TeAr** — CMake-safe, but special in one way: it is the **only** project that
builds FxmeTools' own `tests/` directory (`add_subdirectory(.../tests)`, opt-in
via `TEAR_BUILD_TESTS`). That `CMakeLists.txt` needed the core include root
adding, because `ModDelayLine.h` and `AllpassChain.h` moved while `ModLfo.h` did
not — the change is already made, but TeAr is the only place it is ever
exercised. Also the only project on the older `Source/libs/` layout.

**AmbiProbe, AmbiRR2, Bloom, Localizer** — CMake-safe, no `Lfo` calls, nothing
special found. AmbiRR2 has one `Random` use worth a glance when it bumps. None
have been built against the split; they are pinned well behind it.
