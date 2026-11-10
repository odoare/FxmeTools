# Neural amp models in FxmeTools: plan

Status: decided, 2026-11-09; phase 0 (measurement), phase 1 (`dsp/Resampler.h`) and phase 2 (`FxmeNeural`, `neural/`) done the same day; phase 3, the FxmeFX effect FxmeNeuralAmp, done 2026-11-10 (released plugins built for x86-64-v3); next, Otomato (phase 4). The project (the FxmeFX
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

## Phase 2 outcome (2026-11-09)

`neural/` as planned (`ModelData.h`, `ModelLoader.h/.cpp`,
`NeuralAmpEngine.h/.cpp`, `fxmetools_attach_neural`), tests in
`neural/tests/CoreNeuralTests.cpp`, all passing. What the implementation
found:

- **Test models: NAM Core's `example_models`** (MIT, in the NeuralAudio
  tree the build already has, read in place, nothing copied):
  `wavenet.nam` (small WaveNet), `lstm.nam`, `wavenet_a1_standard.nam`,
  `A2.nam`. The golden test compares our default backend with NAM Core
  itself (`LoadOptions::forceNamCore`): identical (-142 dB to bit-exact).
  Local models of any licence (the Boss set) only feed the bench, through
  `FXME_NAM_MODELS=<dir>`, never the repository. No need to train our own.
- **LSTMs to NAM Core** need the *WaveNet* load mode set to NAM Core: at
  this NeuralAudio commit the LSTM mode only chooses between its own LSTMs.
  The loader reads the architecture first and does so.
- **NAM Core's architectures register through static objects**, which a
  static-library link drops when nothing references their file
  ("No config parser registered for architecture: LSTM"). `NamRegistry.cpp`
  takes the address of each `create_config`, which pulls them in for every
  consumer.
- **NeuralAudio's WaveNets allocated on every block** (Eigen temporaries:
  a product inside a sum with the bias, and `const MatrixXf&` parameters
  given maps): 30 to 90 malloc/free pairs per block. Fixed in
  our fork's commit `2aeff04` (product, then bias;
  `Eigen::Ref` parameters); with it, no allocation or free at all in
  `process()`, swaps included, every model, every rate (the test counts
  malloc and free). In our fork, https://github.com/odoare/NeuralAudio
  (branch `fxme-no-audio-alloc`), which every consumer pins; a pull request
  to mikeoliphant/NeuralAudio once FxmeNeuralAmp's first version is done.
- **A new model starts from an empty history**: silent for its latency,
  then the signal at once. The engine runs it unheard for its latency +
  10 ms before the 20 ms crossfade (without: a step 3 x the signal's own).
- **Instruction set** (`FXME_NEURAL_ARCH`), CPU % of one core, one mono
  model at 48 kHz, i7-10810U, two runs:

  | Model | x86-64 (SSE2) | x86-64-v3 (AVX2, FMA) |
  |---|---:|---:|
  | WaveNet nano (Boss) | 0.9 to 1.0 | 0.6 to 0.7 |
  | WaveNet feather (Boss) | 3.0 to 3.2 | 1.9 to 2.3 |
  | WaveNet standard (Boss) | 9.8 to 11 | 5.5 to 6.1 |
  | WaveNet A2 (Boss) | 14.6 to 16.5 | 9.8 to 10.8 |
  | LSTM 1x16, 2x8 (Boss) | 1.0 to 1.3 | 0.55 to 0.75 |

  The phase 0 figures were `-march=native` (and `MULTIFRAME_8X8_CONVOLUTION`
  0, NeuralAudio's default with GCC 13; 8 makes the A2 models 2 to 3 x
  slower with GCC 13). At 44.1 and 96 kHz add the resampler (0.3 to 1 %).
  Load times 0.1 to 10 ms.
- **Latency**: the same for every model at a given host rate (the round
  trip to 48 kHz), and for "no model" (dry, delayed the same): 0 at
  48 kHz, 139 samples at 44.1 kHz, 275 at 96 kHz. A model trained at
  another rate is padded to it (or late by the difference when its own
  round trip is longer).

Decided 2026-11-10: the fix lives in our fork, pinned, and goes upstream
after FxmeNeuralAmp v1; the released plugins are built for x86-64-v3
(`fxmefx_attach_neural`), with a CPU check. Was to decide: `FXME_NEURAL_ARCH` for the released plugins
(x86-64-v3 halves the cost; a CPU older than 2013 could not load the plugin
at all, or the effect would have to check the CPU and refuse to run).

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

Pinned with `fxme_dependency (NeuralAudio https://github.com/odoare/NeuralAudio
<sha> SIBLING NeuralAudio)` in each top-level consumer, submodules
included: our fork, until its fixes are merged upstream. Behind our interface, so the
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

## `fxme::Resampler` (FxmeCore): done (phase 1)

`core/FxmeTools/dsp/Resampler.h`: `Resampler` (one direction, any ratio)
and `ResamplingContainer` (the round trip around a fixed-rate process:
`process (io, n, inner)`, exactly `n` host samples back, an exact integer
latency, a pass-through at equal rates). Polyphase windowed sinc (Kaiser,
64 zero crossings, cutoff 0.952 of the lower Nyquist, 256 phases
interpolated), vectorised without fast-math. Measured in
`CoreResamplerTests`: round trip error -100 dB, flat to 20 kHz at 44.1 kHz,
aliasing under -100 dB, any block sizes; 0.3 to 0.55 % of a core at
44.1 kHz, 1 to 2 % at 192 kHz; latency 139 samples at 44.1 kHz, 275 at
96 kHz. `NeuralAmpEngine` wraps its model in a `ResamplingContainer`; the
container's maximum inner block is what the loader's
`SetDefaultMaxAudioBufferSize` gets.

## Tests (CoreNeuralTests)

- Golden output: NAM Core's example models (MIT), our backend's output for
  a fixed input compared with NAM Core's (done, see Phase 2 outcome).
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
