# FxmeNeuralAmp, phase 0: NeuralAudio against NAM Core

Measured 2026-11-09, for the plan in `../neural-plan.md` (and section 6 of
FxmeFX's `todo.md`).

## Setup

- NeuralAudio `b98dfbd` (2026-10-09), its submodule NeuralAmpModelerCore
  `896c5f1`, RTNeural `5909c44`; cloned as the sibling `~/src/NeuralAudio`
  (`--recurse-submodules`).
- Built Release (`-O3 -DNDEBUG`) with `-march=native` for both engines (the
  same code generation, AVX2 and FMA), NeuralAudio's defaults otherwise:
  NAM Core on, static internal WaveNet (A1 and A2) on, static LSTM off,
  `MULTIFRAME_8X8_CONVOLUTION` 8 (256-bit SIMD). The build: 1 min 17 s at
  `-j2`.
- Intel Core i7-10810U (6 cores, up to 4.9 GHz, a laptop part), Linux 7.0,
  GCC 13.3; the benchmark pinned to one core (`taskset -c 2`).
- `bench.cpp` (here): each model loaded from memory (`CreateFromStream`, as
  a plugin will from its state), per backend, host rate and block size;
  10 s of a guitar-like test signal after a second of warm-up; the CPU is
  the time spent over the audio's duration, one mono instance, one core.
  Then both backends rendered on the same input and compared.
- Models: NeuralAudio's test set (`Utils/Models`), one Boss pedal captured
  as WaveNet nano, feather, standard and A2, and LSTM 1x16 and 2x8. **Under
  CC BY-NC-ND 4.0: fine for a local benchmark, not as test data in our
  repositories** (no derivatives, non-commercial; FX-Mechanics projects are
  dual-licensed). The golden test of phase 2 needs a model we may
  redistribute (NAM Core's `example_models`, licence to check, or our own).

To run it again (another machine, a Raspberry Pi): build NeuralAudio with
`-DBUILD_UTILS=ON`, then compile `bench.cpp` with ModelTest's flags and the
same objects (see `ModelTest`'s `flags.make` and `link.txt`), and run
`bench ~/src/NeuralAudio/Utils/Models`. Raw results: `results-*.md`.

## Results (CPU % of one core, one mono instance)

At 48 kHz (the models' rate), block 64 / 256:

| Model | NeuralAudio internal | NAM Core | Internal faster by |
|---|---:|---:|---:|
| WaveNet nano | 0.56 / 0.54 | 3.31 / 2.57 | 5 to 6 x |
| WaveNet feather | 1.67 / 1.68 | 4.29 / 3.48 | 2 to 2.5 x |
| WaveNet standard | 5.65 / 6.47 | 6.06 / 5.38 | about the same |
| WaveNet A2 | 9.80 / 9.86 | 10.31 / 8.87 | about the same |
| LSTM 1x16 | 0.54 / 0.53 | 0.56 / 0.56 | the same |
| LSTM 2x8 | 0.77 / 0.76 | 0.71 / 0.71 | the same |

At 96 kHz (block 64 / 256):

| Model | NeuralAudio internal | NAM Core |
|---|---:|---:|
| WaveNet nano | 9.47 / 9.42 | 6.71 / 5.20 |
| WaveNet feather | 9.65 / 11.15 | 8.60 / 6.89 |
| WaveNet standard | 14.30 / 13.53 | 12.31 / 10.47 |
| WaveNet A2 | not loaded | 22.02 / 18.43 |
| LSTM 1x16, 2x8 | twice the 48 kHz figure | twice the 48 kHz figure |

Load time (parse, build, prewarm): 0.4 to 7 ms with the internal engine, 2.6
to 58 ms with NAM Core (A2 at 96 kHz the slowest).

Equivalence at 48 kHz, the same input through both:

| Model | Difference, dB under the signal |
|---|---:|
| WaveNet nano, feather, standard, A2 | -132 to -138 (float rounding: identical) |
| LSTM 1x16 | -48 |
| LSTM 2x8 | -57 |

Calibration reported by these models: recommended input adjustment 0 dB
(no input level in their metadata), output -6 to -9 dB. Receptive field
4092 samples (A1), 6346 (A2), none for the LSTMs.

## What it means

1. **NeuralAudio's internal engine is worth it for the small WaveNets**
   (nano 5 to 6 x, feather 2 to 2.5 x faster than NAM Core) and produces
   NAM Core's output exactly (-132 dB). For standard and A2 the two are
   level on this machine (NAM Core's own fast paths and Eigen do well with
   AVX2). The small models are the ones that matter for many instances and
   for a Raspberry Pi later, so the backend decision stands.

2. **LSTMs: load them through NAM Core.** Same speed, and NeuralAudio's own
   LSTM (dynamic here: its static LSTMs are off by default) differs by -48
   to -57 dB, probably its fast activation approximations: not identical,
   for no gain. `SetLSTMLoadMode (NAMCore)`; the WaveNets stay internal.

3. **The model always runs at its own rate; our resampler converts.** The
   decisive finding. With a host at an integer multiple of 48 kHz,
   NeuralAudio does not resample: it doubles the WaveNet's dilations and
   runs the network at 96 kHz (`OversampleNAMConfig`). That costs 2 x (NAM
   Core) to 17 x (nano: the internal engine falls back from its static code
   to its dynamic one, 0.55 % to 9.5 %), A2 does not load at all, and the
   result is an approximation of the 48 kHz model. LSTMs and rates that are
   not a multiple of 48 kHz (44.1, 88.2 kHz) are not adjusted at all: the
   model then runs at the wrong rate. So the loader is always told the
   model's rate (`SetExternalSampleRate (48000)`), whatever the host's,
   and `fxme::Resampler` converts around the engine: the static fast path,
   the exact model, every host rate, and a 96 kHz session costs what a
   48 kHz one does plus the resampler.

4. **Loading off the audio thread is required** (up to 58 ms), as planned.
   NAM Core's loads are 4 to 10 times slower than the internal engine's.

5. **Block size hardly matters** (64 against 256): no reason to buffer
   internally beyond what the host gives, nor to impose a block size.

6. **Budget.** A standard WaveNet is about 6 % of one core per mono
   instance at 48 kHz on this laptop CPU; dual mono about 12 %; a nano or a
   feather 1 to 3.5 % in dual mono. Comfortable on a desktop; the CPU hint
   in the GUI (from the architecture) is worth having.

## Consequences for phases 1 and 2

- Phase 1, `fxme::Resampler`: no longer a convenience but the way every
  model runs. Requirements: 44.1, 88.2, 96, 176.4, 192 kHz to 48 kHz and
  back, a fixed latency reported to the host, flat to 20 kHz, cheap enough
  that a nano's 0.55 % is not dominated by it (two conversions per block).
- Phase 2, `FxmeNeural`: the loader set to the model's rate and to the
  host's maximum block (converted to the model's rate), WaveNets internal,
  LSTMs NAM Core, models from memory (`CreateFromStream`, confirmed), loading
  and prewarm on a worker thread. The golden test needs a redistributable
  model (see Setup).
