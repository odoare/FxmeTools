# API changes and per-project migration

What this file is for: FxmeTools is shared by several plugins that are not all
worked on at once, so a change made while working on one of them has to be
findable months later while working on another. Every change to FxmeTools that a
consumer can observe goes here, newest section first, with what each project
still has to do about it.

The per-project checklist at the bottom is the part to read when returning to a
project after a break.

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
| FemPlate | `lib/FxmeTools` | done | 2026-08 |
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
