/*
  ------------------------------------------------------------------------------
    NeuralAmpEngine.cpp

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/neural/NeuralAmpEngine.h>

#include <algorithm>
#include <cmath>

namespace fxme::neural
{

namespace
{
    constexpr float inputLimit = 16.0f;

    float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }

    /** Multiplies by a gain ramped from `g` to `target` over the block. */
    void ramp (float* x, int n, float& g, float target) noexcept
    {
        if (g == target)
        {
            if (g != 1.0f)
                for (int i = 0; i < n; ++i)
                    x[i] *= g;
            return;
        }
        const float step = (target - g) / (float) n;
        for (int i = 0; i < n; ++i)
            x[i] *= g + step * (float) (i + 1);
        g = target;
    }

    /** Not finite -> 0, clamped; returns how many samples it changed. */
    int sanitise (float* x, int n, float limit) noexcept
    {
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            const float v = x[i];
            if (! std::isfinite (v))
            {
                x[i] = 0.0f;
                ++count;
            }
            else if (v > limit || v < -limit)
            {
                x[i] = v > 0.0f ? limit : -limit;
                ++count;
            }
        }
        return count;
    }
}

NeuralAmpEngine::~NeuralAmpEngine()
{
    dropAll();
}

void NeuralAmpEngine::dropAll()
{
    collectGarbage();
    delete pending.exchange (nullptr);
    delete current.model;
    delete fading.model;
    delete toRetire;
    current = {};
    fading = {};
    toRetire = nullptr;
}

void NeuralAmpEngine::prepare (double rate, int block)
{
    dropAll();
    hostRate = rate;
    maxBlock = std::max (1, block);
    latency = ModelLoader::nominalLatency (rate);
    dryCopy.assign ((size_t) maxBlock, 0.0f);
    other.assign ((size_t) maxBlock, 0.0f);
    setCrossfadeMs (fadeMs);
    fadePos = 0;
    appliedQuality = quality.load();
    sanitised.store (0);
    current.model = ModelLoader::makeDry (rate, maxBlock).release();
}

void NeuralAmpEngine::setCrossfadeMs (float ms)
{
    fadeMs = std::max (0.0f, ms);
    const double rate = hostRate > 0.0 ? hostRate : 48000.0;
    fadeLength = std::max (1, (int) std::lround (fadeMs * 0.001 * rate));
    preRoll = (int) std::lround (preRollMs * 0.001 * rate);
}

bool NeuralAmpEngine::install (std::unique_ptr<PreparedModel> model)
{
    collectGarbage();
    if (model == nullptr || hostRate <= 0.0 || std::abs (model->getHostRate() - hostRate) > 1.0e-6
        || model->getMaxBlock() < maxBlock)
        return false;
    // The model this one replaces, if the audio thread had not taken it yet.
    delete pending.exchange (model.release(), std::memory_order_acq_rel);
    return true;
}

void NeuralAmpEngine::unload()
{
    if (hostRate > 0.0)
        install (ModelLoader::makeDry (hostRate, maxBlock));
}

void NeuralAmpEngine::collectGarbage()
{
    auto tail = retireTail.load (std::memory_order_relaxed);
    const auto head = retireHead.load (std::memory_order_acquire);
    while (tail != head)
    {
        delete retired[tail % retireCapacity];
        retired[tail % retireCapacity] = nullptr;
        ++tail;
    }
    retireTail.store (tail, std::memory_order_release);
}

bool NeuralAmpEngine::retire (PreparedModel* m) noexcept
{
    const auto head = retireHead.load (std::memory_order_relaxed);
    if (head - retireTail.load (std::memory_order_acquire) >= retireCapacity)
        return false;
    retired[head % retireCapacity] = m;
    retireHead.store (head + 1, std::memory_order_release);
    return true;
}

float NeuralAmpEngine::inputTarget (const PreparedModel& m) const noexcept
{
    return calibrateInput.load (std::memory_order_relaxed)
               ? dbToGain (m.getInputCalibrationDb (inputLevelDbu.load (std::memory_order_relaxed)))
               : 1.0f;
}

float NeuralAmpEngine::outputTarget (const PreparedModel& m) const noexcept
{
    return normaliseOutput.load (std::memory_order_relaxed) ? dbToGain (m.getOutputNormalisationDb()) : 1.0f;
}

void NeuralAmpEngine::runSlot (Slot& s, float* io, int n, bool startAtTarget) noexcept
{
    const float in = inputTarget (*s.model), out = outputTarget (*s.model);
    if (startAtTarget)
    {
        s.inGain = in;
        s.outGain = out;
    }
    ramp (io, n, s.inGain, in);
    s.model->process (io, n);
    ramp (io, n, s.outGain, out);
}

void NeuralAmpEngine::process (float* io, int numSamples) noexcept
{
    if (current.model == nullptr)
        return;
    for (int done = 0; done < numSamples; done += maxBlock)
        processChunk (io + done, std::min (maxBlock, numSamples - done));
}

void NeuralAmpEngine::processChunk (float* io, int numSamples) noexcept
{

    int fixes = sanitise (io, numSamples, inputLimit);

    // A model whose fade ended while the retire queue was full.
    if (toRetire != nullptr && retire (toRetire))
        toRetire = nullptr;

    // A new model: crossfade to it (one fade at a time; the next waits).
    bool fresh = false;
    if (fading.model == nullptr && toRetire == nullptr)
        if (auto* next = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            fading = current;
            current = Slot { next, 1.0f, 1.0f };
            // Pre-roll: the new model starts from an empty history, silent
            // for its latency, then the signal arrives at once (a step the
            // fade would let through). It runs unheard until its output is
            // its own, then the fade begins.
            fadePos = -(next->getLatencySamples() + preRoll);
            fresh = true;
        }

    const float q = quality.load (std::memory_order_relaxed);
    if (q != appliedQuality)
    {
        current.model->setQuality (q);
        if (fading.model != nullptr)
            fading.model->setQuality (q);
        appliedQuality = q;
    }

    if (fading.model != nullptr)
        std::copy (io, io + numSamples, dryCopy.data());

    runSlot (current, io, numSamples, fresh);

    if (fading.model != nullptr)
    {
        std::copy (dryCopy.data(), dryCopy.data() + numSamples, other.data());
        runSlot (fading, other.data(), numSamples, false);
        const float inv = 1.0f / (float) fadeLength;
        int i = 0;
        for (; i < numSamples && fadePos < fadeLength; ++i, ++fadePos)
        {
            const float w = fadePos < 0 ? 0.0f : (float) (fadePos + 1) * inv;
            io[i] = io[i] * w + other[(size_t) i] * (1.0f - w);
        }
        if (fadePos >= fadeLength)
        {
            if (! retire (fading.model))
                toRetire = fading.model;
            fading = {};
        }
    }

    fixes += sanitise (io, numSamples, inputLimit);
    if (fixes != 0)
        sanitised.fetch_add (fixes, std::memory_order_relaxed);
}

} // namespace fxme::neural
