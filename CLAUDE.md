# FxmeTools

Shared C++ audio library behind the FX-Mechanics plugins (FxmeFX, SuperMoTo,
FxmeSampler). Consumed as a git submodule under `<plugin>/lib/FxmeTools`.

The repository is mid-way through a **core/shell split**: the JUCE-free half is
being moved under `core/`, leaving only genuinely JUCE-coupled code in the
`FxmeTools/` module directory. Read `core/README.md` before touching either
side — the rules below are the short version.

**Every consumer-visible change lives in `doc/api-changes.md`**, with a
per-project checklist of what each plugin still has to do. Add to it whenever a
change can be observed from outside this repository, and read it when picking a
plugin back up.

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
| `const String&` (a parameter) | `fxme::StringRef` | `<FxmeTools/util/StringRef.h>` |
| `String` (a return) | `std::string` | `<string>` |
| `.trim()`, `.toLowerCase()`, `.endsWith()`, `.dropLastCharacters()`, `.substring()`, `.containsOnly()`, `.getIntValue()` | `fxme::trim`, `toLower`, `endsWith`, `dropLast`, `substring`, `containsOnly`, `toInt` | `<FxmeTools/util/StringUtils.h>` |
| `const Array<T>&` (parameter or return) | `fxme::ArrayView<T>` | `<FxmeTools/util/ArrayView.h>` |
| `Array<T>` (a member) | `std::vector<T>` | `<vector>` |
| `SortedSet<T>` | a sorted, de-duplicated `std::vector<T>` | `<vector>`, `<algorithm>` |
| `StringArray` (a fixed name list) | `static constexpr const char* const names[]` + `numNames` | — |
| `Random::getSystemRandom()` | `fxme::systemRandom()` | `<FxmeTools/util/Random.h>` |
| `int64`, `uint32`, `uint8` | `std::int64_t`, … | `<cstdint>` |

`AudioBufferView`, `ProcessSpec`, `StringRef` and `ArrayView` convert
**implicitly** from the JUCE types (SFINAE on shape, no JUCE include). Changing
a core signature to take them therefore does **not** require editing JUCE-side
call sites. Do not add conversion helpers or adapter overloads — that is the
whole point of the design.

`ArrayView` also deliberately keeps JUCE's *method* spellings — `isEmpty()`,
`size()` returning `int`, `indexOf()` returning -1 — so that returning one in
place of a `const Array<T>&` breaks nothing either. When you promote something
that returns a container, return an `ArrayView` over a `std::vector` member
rather than the vector itself; `.isEmpty()` at the call sites is the thing that
would otherwise force edits.

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

- ~~`fxme::Random` seeding~~ and ~~`Lfo.h`'s `StringArray` lists~~ — both
  **settled**; see `doc/api-changes.md`. The Random one was not the question it
  looked like: the sequence differing per seed mattered far less than the two
  default constructors disagreeing, JUCE's seeding randomly and core's from a
  fixed constant. Worth remembering as a pattern — when a substitution looks
  like it only changes values, check the constructors too.
- Anything needing `juce::File`, `juce::ValueTree` or `juce::Image` needs an
  **abstract interface in core with a JUCE implementation beside it** — a
  design step, not a substitution. Propose the interface before writing it.
  This is what still blocks `dsp/FirFilter.h`, `dsp/MicCalibration.*`,
  `dsp/IemDecoder.*`, `presets/` and `image/`.
- `juce::String` came off that list: `midi/MidiTools.h` showed it needed a
  value type, not an interface, and `fxme::StringRef` plus `StringUtils.h`
  covers it. `midi/Arpeggiator.h` is still blocked, but by `MidiBuffer`,
  `MidiMessage` and `AudioPlayHead::CurrentPositionInfo` — not by text.
- `juce::dsp::FFT` and `SmoothedValue` were on that list and came off it: both
  turned out to have no platform behaviour to abstract, so they are plain
  concrete replacements (`fxme::Fft`/`RealFft`, `fxme::SmoothedValue`) beside
  `Math.h`. The abstract-interface rule is for things with a system behind
  them, not for value types — but say which you think it is before writing.

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
