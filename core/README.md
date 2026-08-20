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
    ├── midi/                   music theory, sequencing
    ├── acoustics/              FEM plate modes
    └── image/                  geometry (homography, camera pose)
```

The include spelling is deliberately unchanged: core headers are still reached
as `<FxmeTools/dsp/Ambisonics.h>`, because `core/` becomes a second include
root alongside the repository root. **No consumer include needs editing** — a
header moving between the halves is invisible from the outside.

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

That is the whole loop: seconds, no plugin host, no JUCE checkout needed. Two
tests run:

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
