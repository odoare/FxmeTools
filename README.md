# FxmeTools

A C++17 audio library in two halves: **DSP, music theory and maths that depend
on nothing**, and **JUCE components built on top of them**.

The split is the point. Signal processing, tuning systems, finite-element plate
models and geometry have nothing to do with which framework happens to draw the
user interface, and they outlive any particular one. Keeping them physically
separate from the GUI code means they can be tested in a second without a plugin
host, reused from a command-line tool, a Pure Data external or an embedded
target, and carried across intact if the framework is ever swapped.

There is a licensing dimension too, and it is not incidental. JUCE 8 is dual
AGPLv3 / commercial, which constrains anything built on it. The code under
`core/` is not a JUCE derivative in any sense — it does not include a JUCE
header, link a JUCE library, or name a JUCE symbol — so it is free to carry
whatever licence its author chooses, independently of the framework half. That
freedom only exists because the separation is real and enforced rather than
merely intended.

```
core/FxmeTools/     no framework. Builds with a bare C++17 toolchain.
FxmeTools/          the JUCE module: GUI, look-and-feel, file and state I/O.
```

Both halves are reached by the same include spelling —
`<FxmeTools/dsp/Biquad.h>` — because `core/` is a second include root. Which
side a header lives on is therefore invisible to consumers, and a header can
move between them without anyone editing an include.

## The framework-free half

Builds and tests in seconds with no plugin host and no JUCE checkout:

```sh
cmake -S core -B build-core && cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

| directory | what is in it |
|---|---|
| `core/FxmeTools/util/` | the framework-free primitives: `Math`, `Random`, `Fft`/`RealFft`, `SmoothedValue`, `AudioBuffer` and its non-owning `AudioBufferView`, `ArrayView`, `StringRef` and its `StringUtils`, `ProcessSpec` |
| `core/FxmeTools/dsp/` | filters, delays, saturation, pitch and grain manipulation, spectral freeze, ambisonics, metering |
| `core/FxmeTools/midi/` | scales, chords, note and chord name parsing, neo-Riemannian harmony, sequencing, note durations, Euclidean rhythms |
| `core/FxmeTools/acoustics/` | triangular meshing and a Morley-element plate eigensolver |
| `core/FxmeTools/image/` | homography and camera-pose geometry |

Everything reachable from an audio callback is real-time safe: no allocation, no
locks, no exceptions.

### How the halves stay apart

Three mechanisms, because intent alone does not survive a deadline:

- **`FxmeCoreGuard`** greps `core/` on every build and fails it if a `juce::`
  symbol, a `<juce_*>` include, a `JUCE_*` macro or a `jassert` appears —
  including inside comments. Deliberately blunt: a rule you can trust is one
  that cannot be argued with.
- **`FxmeCoreHeaderTest`** includes every core header in one translation unit
  with no framework on the include path. This catches what the guard cannot: a
  header that has no framework symbol but only ever compiled because an umbrella
  header had already pulled in `<cmath>` or `<vector>` for it.
- **`FxmeCoreTests`** exercises the util layer against the semantics it
  promises — FFT scaling conventions and bin ordering, ramp arithmetic, the
  decibel floor, rounding behaviour, and the implicit conversions below.

### Interoperating without adapters

`AudioBufferView`, `ProcessSpec`, `StringRef` and `ArrayView` convert
*implicitly* from any framework type of the same shape, detected structurally
rather than by including anything. A core function can take a
`fxme::AudioBufferView` and still be called with a JUCE `AudioBuffer<float>`; it
can take a `fxme::StringRef` and be called with a `juce::String`, or a
`fxme::ArrayView<int>` and be called with a `juce::Array<int>` — no adapter, no
cast, no call-site change. The same trick will accept an iPlug2
`(sample**, channels, frames)` triple, which is what makes the framework half
replaceable rather than merely separable.

## The JUCE half

The `FxmeTools/` directory is a JUCE module: rotary controls and meters with a
shared look-and-feel, spectrum and waveform displays, a preset manager, embedded
audio and image assets, camera and video input, and background task running.
This is where anything touching files, images, value trees or windows belongs.

## Using it

As a submodule, from a consuming project's CMake:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/cmake/FxmeTools.cmake)
fxmetools_attach(MyPlugin)         # module + core + the WDL convolution engine
```

Registering the module directly instead of using `fxmetools_attach()` is fine —
several projects do, to avoid compiling WDL — but then `core/` has to be added
explicitly:

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/core FxmeCore)
juce_add_module(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/FxmeTools)
target_link_libraries(FxmeTools INTERFACE FxmeCore)
```

**A target that uses the DSP but not the GUI should link `FxmeCore` and nothing
else.** Headless test executables, offline render checks and Pure Data externals
all fall into this category, and for them core is not a dependency to work
around but the whole library they wanted.

## Repository layout

```
core/          the framework-free half, its build, its guard and its tests
FxmeTools/     the JUCE module
cmake/         FxmeTools.cmake — module registration and fxmetools_attach()
doc/           api-changes.md: consumer-visible changes and per-project migration
scripts/       split-core.py, which performs the mechanical half of a promotion
tests/         JUCE-side unit tests, added by a consuming project
WDL/           submodule (Cockos, zlib): convolution engine and reverb engine
```

`doc/api-changes.md` is the file to read when returning to a consuming project
after a break: it records every change observable from outside this repository,
sorted by how it fails rather than by when it happened.

## Direction

The DSP, music theory and acoustics are effectively done: the framework-free
half is the larger one and covers everything reachable from an audio callback.
What remains on the JUCE side is genuinely coupled — file and preset I/O, image
and video input, and the component tree. Those would move behind abstract
interfaces in core with framework implementations beside them, which is design
work rather than substitution, and worth doing only where the reuse is real.

## Licensing

The two halves carry different terms, which is the practical payoff of the
separation. Every file states its own in an SPDX header; `LICENSE.md` has the
full explanation.

| half | licence |
|---|---|
| `core/` | `LGPL-3.0-or-later` |
| `FxmeTools/` | `AGPL-3.0-or-later OR LicenseRef-FXME-Commercial` |

`core/` is framework-free and enforced to stay that way, so it is not a
derivative of JUCE and its licence is independent of the framework's. A tool
that links only `FxmeCore` — a headless test, an offline renderer, a Pure Data
external — takes on no JUCE obligation and needs only the LGPL.

`FxmeTools/` is a JUCE module and only builds against JUCE, so it mirrors JUCE's
own dual licence: use it under the AGPLv3, or under commercial terms from the
author. A commercial grant here covers this code only and is not a substitute
for a JUCE licence, which must be obtained separately from Raw Material
Software.

`WDL/` is a separate submodule from Cockos under the zlib licence.

## A note on how this was built

The separation of the two halves — moving the framework-free sources across
(`core/` now holds 60 of them), writing from scratch the replacements for the
framework primitives they had relied on, building the guard and the tests that
keep the halves apart, and migrating every consuming project onto it — was
carried out with [Claude](https://claude.ai), working from the existing codebase
and its conventions.

Author: Olivier Doaré — [github.com/odoare](https://github.com/odoare)
