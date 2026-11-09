# Neural amp models in FxmeTools: plan

Status: decided, 2026-11-09; phase 0 (measurement) done the same day, nothing else written yet. The project (the FxmeFX
effect FxmeNeuralAmp, its phases, the decisions) is section 6 of FxmeFX's
`todo.md`;
this note is the shared library's side, for whoever picks it up from any
consumer (FxmeFX, Otomato, Camshaft, Mechanodd).

## Decisions (2026-11-09)

- Backend: NeuralAudio, NAM Core as its optional fallback.
- The FxmeFX effect is FxmeNeuralAmp: one model, no cabinet (FxmeCab is
  chained after it).
- Channels: Dual mono (one engine per side) and Mono (one engine on the
  sum): the engine below is mono; the effect owns one or two.
- Desktops first. The Raspberry Pi (Otomato) later: the small models,
  NeuralAudio's static builds and its quality scaling wait, but the
  interface already carries `setQuality`, so nothing has to change then.

## Phase 0 outcome (2026-11-09)

Measured in `neural-phase0/` (harness, raw results, analysis). What changed
in the design:

- **The engine always runs the model at its own rate (48 kHz), and
  `fxme::Resampler` converts around it**, whatever the host's rate. Left to
  itself, NeuralAudio does not resample: at an integer multiple it doubles
  the WaveNet's dilations (2 to 17 x the CPU, an approximation, A2 not
  loaded at all), and at other rates (44.1 kHz) or for LSTMs it does
  nothing, so the model runs at the wrong rate. The loader is always given
  `SetExternalSampleRate (model's rate)`.
- **WaveNets on NeuralAudio's internal engine** (identical to NAM Core, 5 to
  6 x faster for nano, 2 to 2.5 x for feather, level for standard and A2 on
  an AVX2 desktop), **LSTMs on NAM Core** (same speed, exact; NeuralAudio's
  LSTM differs by -48 to -57 dB).
- Loading from memory confirmed: `NeuralModelLoader::CreateFromStream`
  (also `CreateFromJson`). Loads take 0.4 to 58 ms: off the audio thread.
- A standard WaveNet is about 6 % of one core per mono instance at 48 kHz
  (i7-10810U); block size hardly matters.
- The test models NeuralAudio ships are CC BY-NC-ND: the golden test needs
  another model.

## Goal

Play `.nam` models (Neural Amp Modeler: WaveNet and LSTM, A1 and A2) and
Keras models in real time, in any FX-Mechanics project, with the same
behaviour everywhere: off-thread loading, click-free model changes, any host
sample rate, calibrated levels, the model kept in the session.

## Where it goes, and why

```
core/FxmeTools/dsp/Resampler.h     in FxmeCore: no dependency, reusable (any rate conversion)
neural/                            a new, optional, JUCE-free target: FxmeNeural
  CMakeLists.txt                   FxmeNeural -> FxmeCore + NeuralAudio (pinned)
  FxmeTools/neural/ModelData.h     a model in memory: bytes, metadata
  FxmeTools/neural/ModelLoader.h   parse and prepare a model, off the audio thread
  FxmeTools/neural/NeuralAmpEngine.h   the real-time side
  tests/                           CoreNeuralTests
cmake/FxmeTools.cmake              fxmetools_attach_neural (<target>)
```

- **Not in FxmeCore.** FxmeCore is plain C++17 with no dependencies, linked
  by every plugin, Pd external and test, and guarded (`FxmeCoreGuard`). The
  neural engine brings Eigen, a JSON parser and a third-party library with
  its own cadence. `FxmeNeural` is its own target, added only when a
  consumer calls `fxmetools_attach_neural()` (the pattern of WDL and
  `fxmetools_attach()`), so nothing changes for anyone else.
- **JUCE-free**, like core: Otomato's `Source/` never sees JUCE, and the
  engine can be tested in seconds. The same include spelling
  (`<FxmeTools/neural/...>`), `neural/` being one more include root.
- `FxmeCoreGuard`'s rule (no `juce::`) applies to `neural/` too.

## The backend

**NeuralAudio** (Mike Oliphant, MIT,
https://github.com/mikeoliphant/NeuralAudio), with **NAM Core** (Steven
Atkinson, MIT, https://github.com/sdatkinson/NeuralAmpModelerCore) as its
optional fallback (`-DBUILD_NAMCORE=ON`). NeuralAudio:

- implements NAM's WaveNet and LSTM itself, designed to give exactly NAM
  Core's output with better speed and memory; falls back to NAM Core for
  non-standard A2 models and to RTNeural (BSD-3) for Keras models;
- API: `NeuralModelLoader` (`SetExternalSampleRate`, default 48 kHz;
  `SetDefaultMaxAudioBufferSize`, default 128), `CreateFromFile`,
  `NeuralModel::Process (float* in, float* out, int n)` (mono),
  `GetRecommendedInputDBAdjustment` / `GetRecommendedOutputDBAdjustment`,
  `SetAudioInputLevelDBu`, `GetReceptiveFieldSize`, quality scaling for A2
  slimmable models;
- caveats from its README: changing the maximum buffer size is not real-time
  safe; WaveNet models are altered on load for even multiples of 48 kHz, so
  a rate change means a reload (and other rates need resampling: hence the
  resampler); no optimisation flags are set by its CMake (we must pass
  `-O3` and a target, and its `MULTIFRAME_8X8_CONVOLUTION` setting per
  platform: 4 for 128-bit SIMD (Pi 4), 8 for 256-bit (Pi 5, most x64)).
  Loading from memory is not shown in its README: check its sources (else
  write the embedded model to a temporary file, or load through NAM Core's
  JSON entry point).

Pinned with `fxme_dependency (NeuralAudio ... SIBLING NeuralAudio)` in each
top-level consumer, submodules included. Behind our interface, so the
backend can change without touching a consumer.

## The interface (first proposal)

```cpp
namespace fxme::neural
{
    struct ModelInfo                 // from the file's metadata, when present
    {
        std::string name, architecture, author;
        double trainedRate = 48000.0;
        float loudnessDb, recommendedInputDb, recommendedOutputDb;
        int receptiveField;          // samples, -1 when unknown
    };

    /** A model as bytes, kept in a session or a preset as it is. */
    struct ModelData { std::vector<char> bytes; std::string fileName; ModelInfo info; };

    /** Off the audio thread: parse, build, prewarm. Returns a ready model or an error. */
    class ModelLoader { ... std::unique_ptr<PreparedModel> prepare (const ModelData&, double hostRate, int maxBlock); };

    /** The audio thread's side. */
    class NeuralAmpEngine
    {
    public:
        void prepare (double sampleRate, int maxBlock);     // allocates
        void install (std::unique_ptr<PreparedModel>);       // any thread: queued, swapped at the next block
        void setInputLevelDbu (float), setOutputTrimDb (float), setQuality (float);
        void process (float* mono, int numSamples) noexcept; // real time, no allocation
        int  getLatencySamples() const noexcept;             // the resampler's, if any
        // the retired model goes back to the loader's thread to be freed
    };
}
```

- **Swap without a click:** the new model starts on a short crossfade (about
  20 ms) from the old one, both running during it; the old one is handed
  back to a non-real-time thread to be freed (never deleted on the audio
  thread).
- **Rates:** the engine always runs the model at its trained rate (never
  the backend's own "oversampling", see Phase 0 outcome) and converts with
  `fxme::Resampler` when the host differs; latency reported.
- **Calibration:** input in dBu as the model expects, the recommended
  adjustments applied, so models of different loudness come out comparable.

## `fxme::Resampler` (FxmeCore)

JUCE-free, polyphase windowed-sinc (or Lanczos, as NAM's own plugin does
through AudioDSPTools' `ResamplingContainer`), arbitrary ratio, fixed
latency, no allocation after `prepare()`. Tested: flat to 20 kHz at 44.1 to
48 kHz, aliasing rejection, latency exact. Useful beyond neural models (any
fixed-rate kernel in a host at another rate).

## Tests (CoreNeuralTests)

- Golden output: a model we may redistribute (trained by us, small), its
  output for a fixed input compared with NAM Core's to a tolerance.
- A model swap under a running sine: no step larger than the signal's own.
- 44.1, 48 and 96 kHz: same response within the resampler's tolerance.
- No allocation in `process()` (a counting allocator in the test).
- NaN and infinity at the input: finite output.
- A CPU bench per architecture (Nano, Feather, Lite, Standard), results kept
  in `doc/`.

## Licences

NeuralAudio and NAM Core: MIT; RTNeural: BSD-3; Eigen: MPL-2. All compatible
with FxmeCore's LGPL and the plugins' AGPL; their notices go with the
binaries (a `CREDITS` file in each shipping project). Models belong to their
authors: none is shipped unless trained by us.

## Consumers

- FxmeFX: the effect `Source/NeuralAmp/` and its plugin FxmeNeuralAmp; `fxmefx_attach_neural()`
  for the hosts that compile FxmeFX effects from source.
- Otomato: one more entry in `Agents/Fxme`'s effects.
- Camshaft, Mechanodd: the same, if wanted.
- Pure Data: later (loading a model needs a message the Pd host lacks).
