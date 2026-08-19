# FxmeTools

Shared C++ audio library behind the FX-Mechanics plugins (FxmeFX, SuperMoTo,
FxmeSampler). Consumed as a git submodule under `<plugin>/lib/FxmeTools`.

The repository is mid-way through a **core/shell split**: the JUCE-free half is
being moved under `core/`, leaving only genuinely JUCE-coupled code in the
`FxmeTools/` module directory. Read `core/README.md` before touching either
side — the rules below are the short version.

## The split contract

```
core/FxmeTools/     no JUCE. Builds with a bare C++17 toolchain.
FxmeTools/          the JUCE module: GUI, look-and-feel, file/state I/O.
```

Hard rules:

- **Nothing under `core/FxmeTools/` may reference JUCE** — not `juce::`, not
  `<JuceHeader.h>`, not a `<juce_*>` module header, **not even in a comment**.
  The `FxmeCoreGuard` build target enforces this and will fail the build. When
  a comment needs to name a JUCE type, write "JUCE's `AudioBuffer<float>`",
  never the qualified name.
- Core headers must be **self-contained**: include `<cmath>`, `<vector>` etc.
  explicitly. They used to get those free from `JuceHeader.h`; that is exactly
  the rot `FxmeCoreHeaderTest` exists to catch.
- The public include spelling is `<FxmeTools/dsp/Foo.h>` on **both** sides of
  the split. `core/` is a second include root, so moving a file between the two
  halves must never change how consumers include it. Do not "fix" consumer
  includes to point at `core/`.
- Core code stays real-time safe: no allocation, no locks, no exceptions in
  anything reachable from `processBlock`.

## Replacements for JUCE helpers

When promoting a file into core, use these. Argument order matches JUCE, so the
edits are textual — do not reorder operands.

| JUCE | core | header |
|---|---|---|
| `jlimit`, `jmax`, `jmin`, `jmap`, `roundToInt`, `nextPowerOfTwo`, `isPositiveAndBelow`, `degreesToRadians` | `fxme::` same name | `<FxmeTools/util/Math.h>` |
| `MathConstants<T>`, `Decibels::` | `fxme::MathConstants<T>`, `fxme::Decibels::` | `<FxmeTools/util/Math.h>` |
| `Random` | `fxme::Random` | `<FxmeTools/util/Random.h>` |
| `AudioBuffer<float>&` | `fxme::AudioBufferView` / `ConstAudioBufferView` | `<FxmeTools/util/AudioBufferView.h>` |
| `dsp::ProcessSpec` | `fxme::ProcessSpec` | `<FxmeTools/util/ProcessSpec.h>` |
| `int64`, `uint32`, `uint8` | `std::int64_t`, … | `<cstdint>` |

`AudioBufferView` and `ProcessSpec` convert **implicitly** from the JUCE types
(SFINAE on shape, no JUCE include). Changing a core signature to take them
therefore does **not** require editing JUCE-side call sites. Do not add
conversion helpers or adapter overloads — that is the whole point of the design.

## Build and verify

```sh
# core alone — no JUCE needed, seconds
cmake -S core -B build-core && cmake --build build-core
ctest --test-dir build-core --output-on-failure

# the real check: a consuming plugin still builds
cd ../..                       # e.g. the SuperMoTo repo root
cmake -S . -B build -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
cmake --build build -j
```

**A change is not verified until the JUCE plugin builds.** `build-core` going
green proves almost nothing about a signature change — it does not compile a
single call site in SuperMoTo or FxmeFX. Always run both.

LTO is on by default in the plugin builds and dominates link time on Linux;
pass `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` for iteration builds. Never
commit that as the default.

## Moving code into core

`scripts/split-core.py` does the mechanical part. Prefer it over hand-moving:

```sh
python3 scripts/split-core.py              # dry run: what moves, and why
python3 scripts/split-core.py --diff       # exactly what --apply will write
python3 scripts/split-core.py --apply      # git mv onto a new branch
python3 scripts/split-core.py --promotable # next batch + symbol mapping
```

It moves only files whose *code* has zero JUCE references, fixes the includes
the move invalidates in both directions, and drops moved `.cpp` files from the
module's unity TU (`FxmeTools/FxmeTools.cpp`).

Working discipline for the promotable batch:

- **One file per commit.** Both builds green before moving on. Do not batch
  several files into one commit — a mixed diff is miserable to bisect.
- **No opportunistic refactoring.** A `jlimit` → `fxme::jlimit` commit contains
  only that. Note anything else you spot; do not fix it in passing.
- Use `git mv` so history follows the file.

## Decisions that are NOT yours to make

Stop and ask rather than choosing:

- **`fxme::Random` produces a different sequence than JUCE's for a given seed.**
  Same LCG family, so the statistical character is unchanged, but any behaviour
  depending on a reproducible seed changes audibly. No test can settle this —
  it needs listening. Flag every site you touch in `SignalGenerator` and
  `CracksGenerator`.
- **`Lfo.h` returns `juce::StringArray` from `shapeChoices()` /
  `syncRateChoices()`.** Whether core returns `std::vector<std::string>` with a
  JUCE-side adapter, or those lists simply stay on the JUCE side, is an API
  design call. `dsp/ModLfo.h` is blocked on it.
- Anything needing `juce::File`, `juce::String`, `juce::ValueTree`,
  `juce::dsp::FFT`, `juce::Image` or `SmoothedValue` needs an **abstract
  interface in core with a JUCE implementation beside it** — a design step, not
  a substitution. Propose the interface before writing it.

## Licensing — in flux, do not mass-edit

Source headers currently carry `SPDX-License-Identifier: LGPL-3.0-or-later`,
which is being reconsidered (JUCE 8 is dual AGPLv3 / commercial, and the code
under `core/` is not a JUCE derivative at all, so the two halves may end up
under different licences). **Do not rewrite licence headers, add LICENSE files,
or change the `license:` field in `FxmeTools/FxmeTools.h`** unless explicitly
asked. Preserve the existing header block verbatim when moving a file.

## Style

Follow the surrounding code, which is JUCE house style:

- 4 spaces, Allman braces, space before the parameter list: `foo (a, b)`.
- `#pragma once`; everything in `namespace fxme`.
- Header-only by default; add a `.cpp` only for genuinely out-of-line code.
- Keep the existing banner comment (title, description, author, SPDX) at the
  top of every file, and the `//====` section separators.

## Repository facts

- `WDL/` is a submodule (zlib-licensed, Cockos). It provides the convolution
  engine for `dsp/FirFilter.h` and the real FFT used by `dsp/SpectralFreeze.h`.
  It is the JUCE-free FFT of choice for anything promoted into core.
- This repo is itself consumed as a submodule. Commits here need pushing to the
  FxmeTools remote **and** a submodule pointer bump in the consuming plugin.
- `~/src/FxmeTools` may be a stale clone. The authoritative checkout is
  whichever submodule you are working in — check `git log -1` before assuming.
