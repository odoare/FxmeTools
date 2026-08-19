# API changes and per-project migration

What this file is for: FxmeTools is shared by several plugins that are not all
worked on at once, so a change made while working on one of them has to be
findable months later while working on another. Every change to FxmeTools that a
consumer can observe goes here, newest section first, with what each project
still has to do about it.

The per-project checklist at the bottom is the part to read when returning to a
project after a break.

---

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

### Source-breaking: `Lfo` choice lists

The only source-breaking change in the split. Three functions returning a JUCE
string container became string literals plus a count, so the header could stop
depending on JUCE:

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
`fxme::AudioBuffer` (owning counterpart to `AudioBufferView`), and the existing
`Math.h` / `Random.h` / `ProcessSpec.h` helpers.

---

## Per-project checklist

Eleven projects on this machine embed FxmeTools. **Nothing breaks in any of them
until its submodule pointer moves forward** — the split is only visible once a
project bumps. So the breakage arrives one project at a time, months apart,
which is what this file is for.

| project | submodule path | CMake exposure | pinned at (2026-08-19) |
|---|---|---|---|
| AmbiProbe | `lib/FxmeTools` | safe | `4b22e3c` |
| AmbiRR2 | `lib/FxmeTools` | safe | `e55cedf1` |
| Bloom | `lib/FxmeTools` | safe | `4b22e3c` |
| Dede | `lib/FxmeTools` | wired by hand | **tip** |
| FemPlate | `lib/FxmeTools` | **breaks** | `58a31f3` |
| FxmeFX | `lib/FxmeTools` | safe (Pd externals needed core) | tracking branch |
| Localizer | `lib/FxmeTools` | safe | `7a66389` |
| Mango | `lib/FxmeTools` | **breaks** | `bb324dd` |
| Neorix | `lib/FxmeTools` | **breaks** | `58a31f3` |
| SuperMoTo | `lib/FxmeTools` | done | **tip** |
| TeAr | `Source/libs/FxmeTools` | safe | `0de002d` |

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
of its Pure Data externals (fixed). Mango has two. FemPlate has one, and its
case is worse than a missing header — see below.

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

**FemPlate** — the worst-affected. Needs the CMake wiring, *and* its `FemTests`
target compiles two FxmeTools sources by path:

```cmake
lib/FxmeTools/FxmeTools/acoustics/FemMesh.cpp
lib/FxmeTools/FxmeTools/acoustics/PlateModes.cpp
```

Both moved to `core/FxmeTools/acoustics/`. This fails as a missing *file* at
configure time, not as a missing header — a louder failure than the rest, at
least. Fix by linking `FxmeCore`, which already compiles both, and deleting the
two lines rather than repointing them.

**Mango** — needs the CMake wiring. Its `MangoTests` and `MangoRenderTest` set
`lib/FxmeTools` as an include root by hand and need `lib/FxmeTools/core` as well.
Its seed-determinism test is **not** affected by the `fxme::Random` change: Mango
draws through `fxme::detrand::u01 (seed, lane, block, draw)`, a pure function of
its arguments, which moved to core untouched in the first batch.

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
