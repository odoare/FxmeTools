// Run 2026-11-09 against NeuralAudio b98dfbd (NeuralAmpModelerCore 896c5f1); results in results-*.md, analysis in README.md.
// Phase 0 benchmark for FxmeNeuralAmp: NeuralAudio's internal engine against
// NAM Core (through NeuralAudio), per model, sample rate and block size.
// Usage: bench <models dir>
#include "NeuralModel.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace NeuralAudio;
using Clock = std::chrono::steady_clock;

static std::vector<float> testSignal (size_t n, double rate)
{
    // A guitar-like test: decaying plucks of a few partials, plus a little noise.
    std::vector<float> x (n);
    unsigned rng = 1;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / rate, local = std::fmod (t, 0.5);
        const double env = std::exp (-6.0 * local);
        double s = 0.0;
        for (int h = 1; h <= 4; ++h)
            s += std::sin (2.0 * M_PI * 110.0 * h * t) / h;
        rng = rng * 1664525u + 1013904223u;
        x[i] = (float) (0.3 * env * s + 0.002 * ((double) (rng >> 8) / 8388608.0 - 1.0));
    }
    return x;
}

static std::unique_ptr<NeuralModel> load (const std::filesystem::path& file, EModelLoadMode mode, int rate, int block, double& loadMs)
{
    NeuralModelLoader loader;
    loader.SetExternalSampleRate (rate);
    loader.SetDefaultMaxAudioBufferSize (block);
    if (! loader.SetWaveNetLoadMode (mode) || ! loader.SetLSTMLoadMode (mode))
        return nullptr;
    // From memory, as a plugin will (a model embedded in the session).
    std::ifstream f (file, std::ios::binary);
    std::stringstream bytes;
    bytes << f.rdbuf();
    const auto t0 = Clock::now();
    std::unique_ptr<NeuralModel> m (loader.CreateFromStream (bytes, file.extension(), true));
    loadMs = std::chrono::duration<double, std::milli> (Clock::now() - t0).count();
    if (m == nullptr || m->GetLoadMode() != mode)
        return nullptr;
    return m;
}

static double cpuPercent (NeuralModel& m, int rate, int block, double seconds)
{
    const auto x = testSignal ((size_t) (rate * seconds), rate);
    std::vector<float> y ((size_t) block);
    // Warm up one second, then time.
    for (size_t i = 0; i + (size_t) block <= (size_t) rate; i += (size_t) block)
        m.Process (x.data() + i, y.data(), (size_t) block);
    const auto t0 = Clock::now();
    for (size_t i = 0; i + (size_t) block <= x.size(); i += (size_t) block)
        m.Process (x.data() + i, y.data(), (size_t) block);
    const double t = std::chrono::duration<double> (Clock::now() - t0).count();
    return 100.0 * t / seconds;
}

static std::vector<float> render (NeuralModel& m, int rate, int block, double seconds)
{
    const auto x = testSignal ((size_t) (rate * seconds), rate);
    std::vector<float> y (x.size());
    for (size_t i = 0; i + (size_t) block <= x.size(); i += (size_t) block)
        m.Process (x.data() + i, y.data() + i, (size_t) block);
    return y;
}

int main (int argc, char** argv)
{
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    const char* models[] { "BossWN-nano.nam", "BossWN-feather.nam", "BossWN-standard.nam", "BossWN-a2.nam",
                           "BossLSTM-1x16.nam", "BossLSTM-2x8.nam" };
    const char* modeName[] { "Internal", "RTNeural", "NAMCore" };

    printf ("| Model | Backend | Rate | Block | Runs at | Static | Load ms | CPU %% (one core, one mono instance) |\n");
    printf ("|---|---|---:|---:|---:|---|---:|---:|\n");
    for (const char* name : models)
        for (EModelLoadMode mode : { EModelLoadMode::Internal, EModelLoadMode::NAMCore })
            for (int rate : { 48000, 96000 })
                for (int block : { 64, 256 })
                {
                    double loadMs = 0.0;
                    auto m = load (dir / name, mode, rate, block, loadMs);
                    if (m == nullptr)
                    {
                        printf ("| %s | %s | %d | %d | - | - | - | not available |\n", name, modeName[mode], rate, block);
                        continue;
                    }
                    const double cpu = cpuPercent (*m, rate, block, 10.0);
                    printf ("| %s | %s | %d | %d | %.0f | %s | %.1f | %.2f |\n", name, modeName[mode], rate, block,
                            m->GetSampleRate(), m->IsStatic() ? "yes" : "no", loadMs, cpu);
                    fflush (stdout);
                }

    printf ("\nEquivalence at 48 kHz (2 s, block 256): internal against NAM Core\n\n");
    printf ("| Model | Output RMS | RMS of the difference | Difference, dB under the signal | Recommended in / out dB | Receptive field |\n|---|---:|---:|---:|---|---:|\n");
    for (const char* name : models)
    {
        double ms = 0.0;
        auto a = load (dir / name, EModelLoadMode::Internal, 48000, 256, ms);
        auto b = load (dir / name, EModelLoadMode::NAMCore, 48000, 256, ms);
        if (! a || ! b)
            continue;
        const auto ya = render (*a, 48000, 256, 2.0), yb = render (*b, 48000, 256, 2.0);
        double sig = 0.0, diff = 0.0;
        for (size_t i = 0; i < ya.size(); ++i)
        {
            sig += (double) yb[i] * yb[i];
            diff += (double) (ya[i] - yb[i]) * (ya[i] - yb[i]);
        }
        sig = std::sqrt (sig / ya.size());
        diff = std::sqrt (diff / ya.size());
        printf ("| %s | %.4f | %.2e | %.1f | %.1f / %.1f | %d |\n", name, sig, diff, 20.0 * std::log10 (diff / sig + 1e-30),
                a->GetRecommendedInputDBAdjustment(), a->GetRecommendedOutputDBAdjustment(), a->GetReceptiveFieldSize());
    }
    return 0;
}
