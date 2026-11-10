/*
  ------------------------------------------------------------------------------
    NeuralAmpEngine.h

    The audio thread's side of a neural amp model: one mono channel (an effect
    in dual mono owns two).

      prepare (rate, maxBlock)   not real time: drops every model (they were
                                 built for the old rate) and starts dry.
      install (model)            any thread but the audio one: hands a
                                 PreparedModel over without a lock. The
                                 audio thread picks it up at its next block,
                                 runs it unheard for its latency + 10 ms (its
                                 history fills), then crossfades to it (20 ms
                                 by default), both models running: no click.
                                 A model installed before the previous one was
                                 picked up replaces it.
      unload()                   back to dry (the input delayed by the same
                                 latency), crossfaded too.
      process (io, n)            real time: no allocation, no lock, no free.
      collectGarbage()           any thread but the audio one: frees the
                                 models the audio thread has retired (it never
                                 frees one itself). install() calls it too; an
                                 owner calls it from a timer as well, so that a
                                 retired model's memory does not wait for the
                                 next install.

    Latency: ModelLoader::nominalLatency (rate), the same with or without a
    model, whichever model (the host never sees it change).

    Calibration, applied per model (it follows the model through a fade):
      input   the model's input_level_dbu against the interface's full-scale
              level in dBu (setInputLevelDbu, 12 dBu by default), when
              setCalibrateInput (true) and the file has it;
      output  the model's loudness brought to -18 dB, when
              setNormaliseOutput (true) and the file has it.
    Changes are ramped over a block.

    The input is sanitised (not finite -> 0, clamped to +-16, that is +24 dB
    over full scale), and so is the output: a model never gets or gives a NaN.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/neural/ModelLoader.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace fxme::neural
{

class NeuralAmpEngine
{
public:
    NeuralAmpEngine() = default;
    ~NeuralAmpEngine();
    NeuralAmpEngine (const NeuralAmpEngine&) = delete;
    NeuralAmpEngine& operator= (const NeuralAmpEngine&) = delete;

    /** Not real time, never during process(). Drops every model. */
    void prepare (double hostRate, int maxBlock);

    /** Not on the audio thread. False (and the model freed) when it was
        prepared for another host rate or a smaller maximum block. */
    bool install (std::unique_ptr<PreparedModel> model);
    /** Not on the audio thread: back to dry. */
    void unload();
    /** Not on the audio thread: frees the models the audio thread retired. */
    void collectGarbage();

    void setInputLevelDbu (float dbu) noexcept      { inputLevelDbu.store (dbu); }
    void setCalibrateInput (bool on) noexcept       { calibrateInput.store (on); }
    void setNormaliseOutput (bool on) noexcept      { normaliseOutput.store (on); }
    /** A2 slimmable models, 0..1: applied when real-time safe for the model,
        else at the next load (LoadOptions::quality). */
    void setQuality (float q) noexcept              { quality.store (q); }
    /** Not real time (sets the length of the next fades). */
    void setCrossfadeMs (float ms);

    /** Real time, in place (longer blocks than the prepared maximum are
        processed in pieces). */
    void process (float* io, int numSamples) noexcept;

    int getLatencySamples() const noexcept           { return latency; }
    double getHostRate() const noexcept              { return hostRate; }
    int getMaxBlock() const noexcept                 { return maxBlock; }
    /** Samples that were not finite or out of range (in or out), since prepare. */
    std::int64_t getSanitisedCount() const noexcept  { return sanitised.load (std::memory_order_relaxed); }

private:
    struct Slot
    {
        PreparedModel* model = nullptr;
        float inGain = 1.0f, outGain = 1.0f;   // the ramps' current values
    };

    void processChunk (float* io, int numSamples) noexcept;
    void runSlot (Slot&, float* io, int n, bool rampFromTarget) noexcept;
    float inputTarget (const PreparedModel&) const noexcept;
    float outputTarget (const PreparedModel&) const noexcept;
    bool retire (PreparedModel*) noexcept;
    void dropAll();

    double hostRate = 0.0;
    static constexpr float preRollMs = 10.0f;   // after the latency, before a fade
    int maxBlock = 0, latency = 0, fadeLength = 960, fadePos = 0, preRoll = 480;
    float fadeMs = 20.0f, appliedQuality = 1.0f;

    Slot current, fading;                  // audio thread's
    PreparedModel* toRetire = nullptr;     // waiting for room in the retire queue
    std::vector<float> dryCopy, other;

    std::atomic<PreparedModel*> pending { nullptr };

    // Single producer (audio thread), single consumer (collectGarbage).
    static constexpr std::size_t retireCapacity = 16;
    std::array<PreparedModel*, retireCapacity> retired {};
    std::atomic<std::size_t> retireHead { 0 }, retireTail { 0 };

    std::atomic<float> inputLevelDbu { 12.0f }, quality { 1.0f };
    std::atomic<bool> calibrateInput { true }, normaliseOutput { true };
    std::atomic<std::int64_t> sanitised { 0 };
};

} // namespace fxme::neural
