/*
  ------------------------------------------------------------------------------
    ModelLoader.cpp

    The only file that sees the backend (NeuralAudio).

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/neural/ModelLoader.h>
#include <FxmeTools/dsp/Resampler.h>

#include <NeuralAudio/NeuralModel.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace fxme::neural
{

namespace detail { bool namArchitecturesLinked() noexcept; }   // NamRegistry.cpp

struct PreparedModel::Impl
{
    ModelInfo info;
    double hostRate = ModelLoader::standardRate;
    int maxBlock = 0, latency = 0;

    std::unique_ptr<NeuralAudio::NeuralModel> net;   // null: dry
    ResamplingContainer container;

    // Pads the latency up to the nominal one (models not at 48 kHz, and dry).
    std::vector<float> delay;
    int delayPos = 0;
};

PreparedModel::PreparedModel (std::unique_ptr<Impl> i) : impl (std::move (i)) {}
PreparedModel::~PreparedModel() = default;

const ModelInfo& PreparedModel::getInfo() const noexcept { return impl->info; }
bool PreparedModel::isDry() const noexcept               { return impl->net == nullptr; }
double PreparedModel::getHostRate() const noexcept       { return impl->hostRate; }
int PreparedModel::getMaxBlock() const noexcept          { return impl->maxBlock; }
int PreparedModel::getLatencySamples() const noexcept    { return impl->latency; }

float PreparedModel::getInputCalibrationDb (float audioInputDbu) const noexcept
{
    return impl->info.inputLevelDbu ? audioInputDbu - *impl->info.inputLevelDbu : 0.0f;
}

float PreparedModel::getOutputNormalisationDb() const noexcept
{
    return impl->info.loudnessDb ? -18.0f - *impl->info.loudnessDb : 0.0f;
}

bool PreparedModel::hasQualityScaling() const noexcept
{
    return impl->net != nullptr && impl->net->HasQualityScaling();
}

bool PreparedModel::setQuality (float scale) noexcept
{
    if (! hasQualityScaling())
        return false;
    scale = std::clamp (scale, 0.0f, 1.0f);
    if (scale == impl->net->GetQualityScaleFactor())
        return true;
    if (! impl->net->IsQualityChangeRealtimeSafe (scale))
        return false;
    impl->net->SetQualityScaleFactor (scale);
    return true;
}

void PreparedModel::process (float* io, int numSamples) noexcept
{
    auto& d = *impl;
    if (d.net != nullptr)
    {
        auto* net = d.net.get();
        d.container.process (io, numSamples, [net] (const float* in, float* out, int m)
        {
            net->Process (in, out, (size_t) m);
        });
    }

    if (! d.delay.empty())
    {
        const int size = (int) d.delay.size();
        for (int i = 0; i < numSamples; ++i)
        {
            const float y = d.delay[(size_t) d.delayPos];
            d.delay[(size_t) d.delayPos] = io[i];
            io[i] = y;
            if (++d.delayPos == size)
                d.delayPos = 0;
        }
    }
}

//==============================================================================
namespace
{
    std::string stringField (const nlohmann::json& j, const char* key)
    {
        return j.contains (key) && j.at (key).is_string() ? j.at (key).get<std::string>() : std::string();
    }

    std::optional<float> numberField (const nlohmann::json& j, const char* key)
    {
        if (j.contains (key) && j.at (key).is_number())
            return j.at (key).get<float>();
        return std::nullopt;
    }

    nlohmann::json parse (const ModelData& data)
    {
        return nlohmann::json::parse (data.bytes.begin(), data.bytes.end());
    }

    /** NAM's architecture names, from the config: A1 WaveNets by the first
        layer array's channels (16 Standard, 12 Lite, 8 Feather, 4 Nano), A2
        by its single layer array or its slimmable container, LSTMs by their
        shape. */
    std::string sizeClassOf (const nlohmann::json& j, const std::string& architecture)
    {
        if (architecture == "SlimmableContainer")
            return "A2";
        if (! j.contains ("config") || ! j.at ("config").is_object())
            return {};
        const auto& c = j.at ("config");
        if (architecture == "LSTM" && c.contains ("num_layers") && c.contains ("hidden_size")
            && c.at ("num_layers").is_number() && c.at ("hidden_size").is_number())
            return "LSTM " + std::to_string (c.at ("num_layers").get<int>()) + "x"
                   + std::to_string (c.at ("hidden_size").get<int>());
        if (architecture == "WaveNet" && c.contains ("layers") && c.at ("layers").is_array()
            && ! c.at ("layers").empty())
        {
            const auto& layers = c.at ("layers");
            if (layers.size() == 1)
                return "A2";
            const auto& first = layers.at (0);
            if (! first.contains ("channels") || ! first.at ("channels").is_number())
                return {};
            switch (first.at ("channels").get<int>())
            {
                case 16: return "Standard";
                case 12: return "Lite";
                case 8:  return "Feather";
                case 4:  return "Nano";
                default: return "WaveNet " + std::to_string (first.at ("channels").get<int>());
            }
        }
        return {};
    }

    ModelInfo infoFrom (const nlohmann::json& j, const std::string& extension)
    {
        ModelInfo info;
        info.architecture = stringField (j, "architecture");
        if (info.architecture.empty() && extension != ".nam")
            info.architecture = "Keras";
        info.version = stringField (j, "version");
        if (j.contains ("sample_rate") && j.at ("sample_rate").is_number())
        {
            const double r = j.at ("sample_rate").get<double>();
            if (r > 0.0)
                info.sampleRate = r;
        }
        info.sizeClass = sizeClassOf (j, info.architecture);
        if (j.contains ("metadata") && j.at ("metadata").is_object())
        {
            const auto& m = j.at ("metadata");
            info.name = stringField (m, "name");
            info.modeledBy = stringField (m, "modeled_by");
            info.gearMake = stringField (m, "gear_make");
            info.gearModel = stringField (m, "gear_model");
            info.gearType = stringField (m, "gear_type");
            info.toneType = stringField (m, "tone_type");
            info.loudnessDb = numberField (m, "loudness");
            info.inputLevelDbu = numberField (m, "input_level_dbu");
            info.outputLevelDbu = numberField (m, "output_level_dbu");
        }
        return info;
    }

    const char* backendName (NeuralAudio::EModelLoadMode m)
    {
        switch (m)
        {
            case NeuralAudio::EModelLoadMode::Internal: return "Internal";
            case NeuralAudio::EModelLoadMode::RTNeural: return "RTNeural";
            case NeuralAudio::EModelLoadMode::NAMCore:  return "NAM Core";
        }
        return "";
    }
}

#ifndef FXME_NEURAL_REQUIRES_AVX2
 #define FXME_NEURAL_REQUIRES_AVX2 0
#endif

#if FXME_NEURAL_REQUIRES_AVX2 && (defined (__x86_64__) || defined (_M_X64) || defined (__i386__) || defined (_M_IX86))
 #define FXME_NEURAL_CHECK_CPU 1
 #if defined (_MSC_VER)
  #include <intrin.h>
 #endif
#else
 #define FXME_NEURAL_CHECK_CPU 0
#endif

bool ModelLoader::isCpuSupported() noexcept
{
#if FXME_NEURAL_CHECK_CPU
 #if defined (_MSC_VER)
    // AVX2 (leaf 7, EBX bit 5), FMA (leaf 1, ECX bit 12), and the OS saving
    // the AVX registers (OSXSAVE, ECX bit 27, then XCR0 bits 1 and 2).
    int r[4] {};
    __cpuid (r, 0);
    if (r[0] < 7)
        return false;
    __cpuid (r, 1);
    const bool fma = (r[2] & (1 << 12)) != 0, osxsave = (r[2] & (1 << 27)) != 0;
    if (! fma || ! osxsave || (_xgetbv (0) & 6) != 6)
        return false;
    __cpuidex (r, 7, 0);
    return (r[1] & (1 << 5)) != 0;
 #else
    static const bool ok = __builtin_cpu_supports ("avx2") && __builtin_cpu_supports ("fma");
    return ok;
 #endif
#else
    return true;
#endif
}

std::optional<ModelInfo> ModelLoader::readInfo (const ModelData& data, std::string* error)
{
    try
    {
        return infoFrom (parse (data), data.extension());
    }
    catch (const std::exception& e)
    {
        if (error != nullptr)
            *error = e.what();
    }
    catch (...)
    {
        if (error != nullptr)
            *error = "unreadable model";
    }
    return std::nullopt;
}

int ModelLoader::nominalLatency (double hostRate)
{
    ResamplingContainer c;
    c.prepare (hostRate, standardRate, 1);
    return c.getLatencySamples();
}

LoadResult ModelLoader::load (const ModelData& data, double hostRate, int maxBlock, const LoadOptions& options)
{
    LoadResult result;
    if (data.isEmpty())
    {
        result.error = "no model";
        return result;
    }
    if (hostRate <= 0.0 || maxBlock <= 0)
    {
        result.error = "not prepared";
        return result;
    }
    if (! isCpuSupported())
    {
        result.error = "this processor lacks AVX2 / FMA, which this build needs to run models";
        return result;
    }

    try
    {
        auto json = parse (data);
        const auto extension = data.extension();
        auto impl = std::make_unique<PreparedModel::Impl>();
        impl->info = infoFrom (json, extension);
        impl->hostRate = hostRate;
        impl->maxBlock = maxBlock;

        // The network runs at its own rate, inside the round trip.
        impl->container.prepare (hostRate, impl->info.sampleRate, maxBlock);
        const int maxInner = impl->container.getMaxInnerBlock();

        NeuralAudio::NeuralModelLoader loader;
        loader.SetExternalSampleRate ((int) std::lround (impl->info.sampleRate));   // never resampled by the backend
        loader.SetDefaultMaxAudioBufferSize (maxInner);
        loader.SetDefaultQualityScaleFactor (std::clamp (options.quality, 0.0f, 1.0f));
        // NeuralAudio sends a .nam file to NAM Core when its *WaveNet* mode
        // says so, whatever the architecture (its LSTM mode only chooses
        // between its own LSTMs): LSTMs go to NAM Core that way (phase 0:
        // its own LSTM differs by -50 dB, for no gain in speed).
        if (options.forceNamCore || impl->info.architecture == "LSTM")
        {
            if (! detail::namArchitecturesLinked())
                throw std::runtime_error ("NAM Core not linked");
            loader.SetWaveNetLoadMode (NeuralAudio::EModelLoadMode::NAMCore);
        }

        impl->net.reset (loader.CreateFromJson (json, extension, true));
        if (impl->net == nullptr)
        {
            result.error = "unsupported model (" + (impl->info.architecture.empty() ? extension : impl->info.architecture) + ")";
            return result;
        }
        impl->net->SetMaxAudioBufferSize (maxInner);

        impl->info.backend = backendName (impl->net->GetLoadMode());
        impl->info.isStatic = impl->net->IsStatic();
        impl->info.receptiveField = impl->net->GetReceptiveFieldSize();

        // Every model at this host rate has the same latency.
        const int own = impl->container.getLatencySamples();
        const int pad = std::max (0, nominalLatency (hostRate) - own);
        impl->delay.assign ((size_t) pad, 0.0f);
        impl->latency = own + pad;

        result.model.reset (new PreparedModel (std::move (impl)));
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
    }
    catch (...)
    {
        result.error = "unreadable model";
    }
    return result;
}

std::unique_ptr<PreparedModel> ModelLoader::makeDry (double hostRate, int maxBlock)
{
    auto impl = std::make_unique<PreparedModel::Impl>();
    impl->info.name = "Dry";
    impl->hostRate = hostRate;
    impl->maxBlock = maxBlock;
    impl->latency = nominalLatency (hostRate);
    impl->delay.assign ((size_t) impl->latency, 0.0f);
    return std::unique_ptr<PreparedModel> (new PreparedModel (std::move (impl)));
}

} // namespace fxme::neural
