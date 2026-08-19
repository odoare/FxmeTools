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
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/core FxmeCore)
juce_add_module(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/FxmeTools)
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

### SuperMoTo — done

- [x] `fxmetools_attach()` carries FxmeCore; nothing to do in the plugin target.
- [x] `SuperMoToTests`, `SuperMoToMicCalTests` and `SuperMoToSweepTests` bypass
      `fxmetools_attach`, so they were given `${FXMETOOLS_ROOT}` and `FxmeCore`
      explicitly. `AnalysisTest.cpp` needs it — `Biquad.h` is core now.
- [x] Builds and runs. `Tests/SweepTest` passes against `fxme::Fft`.
- [ ] Uses `fxme::SignalGenerator` in `SplMeterEngine` for its stimulus, so its
      noise stream differs from before. Not a regression — it was never
      reproducible run to run — but worth one listen.

### Dede — done

- [x] Registers the module directly rather than via `fxmetools_attach()` (it
      compiles none of the WDL sources), so it needed the three CMake lines
      above adding by hand.
- [x] `Reverb.h`, which was an uncommitted file in Dede's own checkout, is now
      in core.
- [x] Compiles, links and runs.

### FxmeFX — NOT DONE, not yet checked out here

- [ ] Wire FxmeCore into its CMake (see above); which form depends on whether it
      uses `fxmetools_attach()`.
- [ ] Fix the `Lfo` choice-list call sites — the only source-breaking change.
      It is the only project that calls them.
- [ ] Check any default-constructed `fxme::Random`.
- [ ] Build and listen: it is the heaviest user of `dsp/`, so it exercises far
      more of the moved code than SuperMoTo or Dede do.
