/*
  ------------------------------------------------------------------------------
    ModelLoader.h

    Off the audio thread: from a model's bytes to a model ready to run at the
    host's rate.

      ModelLoader::readInfo  the metadata only (fast: no network built).
      ModelLoader::load      parse, build, prewarm: a PreparedModel, or an
                             error. Takes 1 to 60 ms (phase 0): never on the
                             audio thread.
      ModelLoader::makeDry   a PreparedModel without a network: the input,
                             delayed by the same latency (what an engine
                             plays without a model).

    A PreparedModel runs at the host's rate: the network at the rate it was
    trained at, with fxme::ResamplingContainer around it when the host's
    differs (never NeuralAudio's own "oversampling", which alters WaveNets
    and leaves LSTMs at the wrong rate, see doc/neural-phase0). Its latency
    is the same for every model at a given host rate (nominalLatency): a
    model trained at another rate than 48 kHz is padded to it, so a model
    change never changes what the host compensates. The exception: such a
    model whose own round trip is longer than the nominal one (a 44.1 kHz
    Keras capture in a 48 kHz host, where the nominal latency is 0) arrives
    late by the difference (its getLatencySamples() says by how much).

    Backends (decided in phase 0): WaveNets on NeuralAudio's internal engine
    (NAM Core's output, faster), LSTMs and non-standard A2 models on NAM
    Core, Keras models on RTNeural. The backend's types never appear here.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/neural/ModelData.h>

#include <memory>
#include <string>

namespace fxme::neural
{

/** A model, built and prewarmed for one host rate and maximum block. Made by
    ModelLoader, run by NeuralAmpEngine (one thread at a time). */
class PreparedModel
{
public:
    ~PreparedModel();
    PreparedModel (const PreparedModel&) = delete;
    PreparedModel& operator= (const PreparedModel&) = delete;

    const ModelInfo& getInfo() const noexcept;
    bool isDry() const noexcept;               // no network: makeDry()
    double getHostRate() const noexcept;
    int getMaxBlock() const noexcept;
    int getLatencySamples() const noexcept;    // host samples, = nominalLatency unless longer

    /** The gain that brings an interface whose full scale is `audioInputDbu`
        to the level the model was captured at (0 dB when the file does not
        say). */
    float getInputCalibrationDb (float audioInputDbu) const noexcept;
    /** The gain that brings the model's loudness to -18 dB (0 when unknown). */
    float getOutputNormalisationDb() const noexcept;

    bool hasQualityScaling() const noexcept;
    /** A2 slimmable models: 0..1. Only applied when the change is real-time
        safe for this model; returns whether it was. */
    bool setQuality (float scale) noexcept;

    /** Real time: `numSamples` (at most getMaxBlock()) host samples, in place. */
    void process (float* io, int numSamples) noexcept;

private:
    struct Impl;
    explicit PreparedModel (std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
    friend class ModelLoader;
};

struct LoadOptions
{
    /** Every architecture through NAM Core (the reference; slower for small
        WaveNets). For comparisons and as a fallback. */
    bool forceNamCore = false;
    /** The initial quality of A2 slimmable models, 0..1. */
    float quality = 1.0f;
};

struct LoadResult
{
    std::unique_ptr<PreparedModel> model;   // null on failure
    std::string error;                      // why, when it failed
    explicit operator bool() const noexcept { return model != nullptr; }
};

class ModelLoader
{
public:
    /** The metadata alone (the file is parsed, the network not built).
        Empty, with `error` set, when the bytes are not a model. */
    static std::optional<ModelInfo> readInfo (const ModelData&, std::string* error = nullptr);

    /** Not real time (allocates, parses, prewarms: up to tens of ms). */
    static LoadResult load (const ModelData&, double hostRate, int maxBlock, const LoadOptions& = {});

    /** The input delayed by nominalLatency (hostRate): an engine's "no model". */
    static std::unique_ptr<PreparedModel> makeDry (double hostRate, int maxBlock);

    /** The latency of every PreparedModel at this host rate (that of the
        round trip to 48 kHz; 0 at 48 kHz). */
    static int nominalLatency (double hostRate);

    /** False when this build's networks need instructions the CPU lacks
        (built for x86-64-v3, FXME_NEURAL_ARCH, on a CPU without AVX2 and
        FMA): load() then refuses every model with a message saying so,
        rather than stopping on an illegal instruction. */
    static bool isCpuSupported() noexcept;

    /** NAM models are trained at 48 kHz. */
    static constexpr double standardRate = 48000.0;
};

} // namespace fxme::neural
