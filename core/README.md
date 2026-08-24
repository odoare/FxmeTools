# FxmeCore — the JUCE-free half of FxmeTools

Everything under `core/` builds with a bare C++17 toolchain. No JUCE on the
include path, no `juce::` symbol in the sources, no plugin framework of any
kind. That is the entire contract, and it is enforced at build time.

Why bother: the DSP and music-theory code in FxmeTools is the part with lasting
value, and it is the part that has nothing to do with which plugin framework
you ship. Keeping it physically separate means it can be tested in a second
without a plugin host, reused in a command-line tool or a Pure Data external,
and — if JUCE is ever swapped for iPlug2 or anything else — carried across
untouched. The framework migration then becomes "rewrite the GUI", not
"rewrite everything".

## Layout

```
core/
├── CMakeLists.txt              FxmeCore target (+ optional FxmeCoreWdlFft)
├── cmake/CheckNoJuce.cmake     build-time guard: fails if JUCE leaks in
├── tests/                      builds and runs with no JUCE at all
└── FxmeTools/
    ├── util/                   the framework-free replacements (see below)
    ├── dsp/                    JUCE-free DSP kernels
    ├── math/                   linear algebra, sparse storage, eigensolver
    ├── midi/                   music theory, sequencing
    ├── acoustics/              FEM plate modes
    └── image/                  geometry (homography, camera pose)
```

The include spelling is deliberately unchanged: core headers are still reached
as `<FxmeTools/dsp/Ambisonics.h>`, because `core/` becomes a second include
root alongside the repository root. **No consumer include needs editing** — a
header moving between the halves is invisible from the outside.

## The math layer

`math/` holds the numerics that have nothing to do with acoustics: dense and
compressed-sparse-row matrix storage behind one small operator interface, a
dense and a profile Cholesky, a bandwidth-reducing renumbering, a Jacobi
eigensolver for small dense blocks, and a shift-invert subspace iteration for
the lowest eigenpairs of `A x = lambda M x`.

The split is worth stating explicitly because it is easy to lose. `acoustics/`
knows about plates — boundary conditions, Morley elements, mode shapes sampled
at mesh vertices. `math/` knows about matrices and nothing else: it has never
heard of a plate, and the eigensolver never sees a matrix at all, only
`SymmetricOperator::multiply` and `SpdSolver::solveInPlace`. That is what lets
the same iteration run on dense storage, on sparse storage, or later on a
matrix-free operator, chosen at the call site with no change to either half.

| Header | What it is |
|---|---|
| `math/LinearOperator.h` | `SymmetricOperator`, `AssemblableMatrix`, `SpdSolver` — the whole interface between an algorithm and a storage format |
| `math/DenseLinearAlgebra.h` | `DenseSymmetricMatrix`, `DenseCholesky`, Jacobi eigensolver, fixed-size inverse |
| `math/SparseMatrix.h` | `SparsityPattern`, `SparsityBuilder`, `SparseSymmetricMatrix` (compressed rows, both triangles, sorted columns) |
| `math/BandwidthOrdering.h` | reverse Cuthill-McKee renumbering, and the profile it produces |
| `math/SkylineCholesky.h` | profile (envelope) Cholesky, thread-safe solves |
| `math/SubspaceEigensolver.h` | lowest eigenpairs of `A x = lambda M x` by shift-invert subspace iteration |
| `math/ParallelFor.h` | small dynamic index-range splitter |

Two things are worth knowing before using any of it.

**Storage is exact, ordering is not free.** Compressed rows keep their column
indices sorted, so a sparse row walk visits non-zeros in the same ascending
order a dense row walk would, and the two matrix-vector products are the same
floating-point number — `CoreMathTests` asserts bit-for-bit equality, which is
sharp enough to catch a mis-set index rather than only a mis-set value. A
*factorisation* is different: changing the elimination order changes the
rounding, so a profile factorisation of a renumbered matrix agrees with a dense
one to round-off (measured at 1e-15), not to the bit.

**The renumbering is the caller's to apply.** `reverseCuthillMcKee` returns a
permutation and stops there; `SkylineCholesky` factorises whatever numbering it
is handed, and its envelope is only narrow if someone made it so. For a
finite-element assembly the right place to apply it is the degree-of-freedom
map, before anything is assembled — then every matrix, every eigenvector and
every exported nodal value is in the new numbering already and nothing is ever
permuted back. Applying it closer to the solver means threading it through each
of those steps, which is how one gets plausible-looking wrong eigenvectors.

## The util layer

Four headers replace the handful of JUCE helpers the DSP relied on:

| Header | Replaces | Notes |
|---|---|---|
| `util/Math.h` | `jlimit`, `jmax`, `jmin`, `jmap`, `roundToInt`, `MathConstants`, `Decibels`, `degreesToRadians`, `isPositiveAndBelow`, `nextPowerOfTwo` | Same argument order as JUCE, including `jlimit (lower, upper, value)` — migration is a pure textual substitution |
| `util/Random.h` | JUCE's `Random` | Same LCG family, so noise character is unchanged. **The sequence for a given seed differs**; anything relying on a reproducible seed needs a listen |
| `util/AudioBufferView.h` | `AudioBuffer<float>&` at API boundaries | Converts *implicitly* from any JUCE-shaped buffer, so call sites do not change |
| `util/ProcessSpec.h` | `dsp::ProcessSpec` | Same trick — implicit conversion, call sites unchanged |

The implicit-conversion trick is worth understanding, because it is what makes
the split cheap. `AudioBufferView` accepts anything exposing
`getArrayOfWritePointers() / getNumChannels() / getNumSamples()`, detected by
SFINAE rather than by including JUCE. So a core function can take a
`fxme::AudioBufferView` and still be called with a `juce::AudioBuffer<float>`
with no adapter and no edit. The same will hold for an iPlug2
`(sample**, nChans, nFrames)` triple with a small constructor addition — which
means core signatures should not need touching again if the framework changes.

## Building and testing, without JUCE

```sh
cmake -S core -B build-core
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

That is the whole loop: seconds, no plugin host, no JUCE checkout needed. Three
tests run:

- **`FxmeCoreMathTests`** — the math layer against closed-form answers: storage
  formats multiplying identically, the renumbering being a valid permutation
  that shrinks the profile, the profile factorisation agreeing with the dense
  one under a renumbering round trip, and the eigensolver reproducing the
  known spectrum of a 1D Laplacian.
- **`FxmeCoreTests`** — behaviour of the util layer against JUCE's documented
  semantics (the −100 dB `Decibels` floor, `roundToInt`'s round-half-away-from-zero,
  `jlimit`'s argument order, `Random`'s ranges, the implicit conversions).
- **`FxmeCoreHeaderTest`** — includes every core header in one TU. This catches
  the failure mode the `juce::` guard cannot: a header that has no JUCE symbol
  but only ever compiled because `JuceHeader.h` had already pulled in `<cmath>`
  or `<vector>` for it.

The `FxmeCoreGuard` target greps `core/` on every build and fails with a
migration table if a `juce::` symbol, `<JuceHeader.h>` or a `<juce_*>` include
reappears. It is deliberately dumb — it also flags JUCE mentions in comments,
which is why core files say "JUCE's AudioBuffer" rather than the qualified
name. A dumb rule is a rule you can trust.

## Wiring it into the JUCE build

In `cmake/FxmeTools.cmake`, three additions:

```cmake
# 1. before juce_add_module(): bring in the JUCE-free half
add_subdirectory("${FXMETOOLS_ROOT}/core" FxmeCore)

# 2. after juce_add_module(): the module's own sources need core's headers
target_link_libraries(FxmeTools INTERFACE FxmeCore)

# 3. inside fxmetools_attach(): consumers link the static library
function(fxmetools_attach target)
    target_link_libraries(${target} PRIVATE FxmeTools FxmeCore)
    ...
endfunction()
```

`FxmeCore` is built `POSITION_INDEPENDENT_CODE ON`, so it links into shared
plugin bundles without complaint.

## Moving more code in

`scripts/split-core.py` does the mechanical part:

```sh
python3 scripts/split-core.py                # dry run — what would move, and why
python3 scripts/split-core.py --diff         # exactly what would be written
python3 scripts/split-core.py --apply        # git mv onto a new branch
python3 scripts/split-core.py --promotable   # the next batch, with the mapping
```

It only moves files whose *code* has no JUCE reference at all, fixes every
include the move invalidates in both directions, and drops moved `.cpp` files
from the module's unity TU. It will not touch a file that needs a judgement
call.

`--promotable` lists the files whose only JUCE contact has a drop-in
equivalent above — those are one-line-per-call-site edits, done by hand, and
the `[!]` markers flag the two that change behaviour rather than just spelling.

Everything else — `juce::File`, `juce::String`, `juce::ValueTree`,
`juce::dsp::FFT`, `juce::Image`, and the whole `components/` tree — needs a
design decision (an abstract interface in core with a JUCE implementation
beside it), not a substitution. Those are the interesting ones, and they are
where the next real work is.
