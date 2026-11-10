/*
  ------------------------------------------------------------------------------
    CoreNeuralTests.cpp

    FxmeNeural: ModelLoader, PreparedModel, NeuralAmpEngine, on NAM Core's
    example models (wavenet.nam: a small WaveNet; lstm.nam: an LSTM;
    wavenet_a1_standard.nam: a standard A1 WaveNet; A2.nam: an A2 slimmable
    container).

      1. Metadata: read without building; garbage and empty data refused.
      2. Golden: our default backend (internal WaveNet) against NAM Core,
         at 48 kHz: identical within float rounding (-100 dB).
      3. Host rates: at 44.1 and 96 kHz the model's response (fundamental
         and third harmonic of a sine) is the 48 kHz one within 0.2 dB.
      4. Latency: nominal for every model; dry is the input delayed by it.
      5. Model swaps under a running sine: no step larger than the steady
         signal's own; many swaps in a row; a model for another rate refused.
      6. No allocation and no free in process(), swaps included.
      7. Not finite or huge input: finite output, then normal again.
      8. Calibration: output normalisation is the model's loudness to -18 dB.
      9. Cost and load time per model and host rate, reported (plus every
         .nam file in $FXME_NAM_MODELS, if set).

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/neural/NeuralAmpEngine.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <new>
#include <string>
#include <vector>

//==============================================================================
// Allocation counting: operator new everywhere; malloc and free too on glibc
// (Eigen allocates with malloc).
static std::atomic<bool> counting { false };
static std::atomic<long> allocations { 0 }, frees { 0 };

#if defined(__GLIBC__)
extern "C" void* __libc_malloc (size_t);
extern "C" void* __libc_calloc (size_t, size_t);
extern "C" void* __libc_realloc (void*, size_t);
extern "C" void* __libc_memalign (size_t, size_t);
extern "C" void __libc_free (void*);
extern "C" void* malloc (size_t n)
{
    if (counting.load (std::memory_order_relaxed)) ++allocations;
    return __libc_malloc (n);
}
extern "C" void* calloc (size_t a, size_t b)
{
    if (counting.load (std::memory_order_relaxed)) ++allocations;
    return __libc_calloc (a, b);
}
extern "C" void* realloc (void* p, size_t n)
{
    if (counting.load (std::memory_order_relaxed)) ++allocations;
    return __libc_realloc (p, n);
}
extern "C" int posix_memalign (void** p, size_t a, size_t n)
{
    if (counting.load (std::memory_order_relaxed)) ++allocations;
    *p = __libc_memalign (a, n);
    return *p != nullptr ? 0 : 12;
}
extern "C" void* aligned_alloc (size_t a, size_t n)
{
    if (counting.load (std::memory_order_relaxed)) ++allocations;
    return __libc_memalign (a, n);
}
extern "C" void free (void* p)
{
    if (p != nullptr && counting.load (std::memory_order_relaxed)) ++frees;
    __libc_free (p);
}
#else
void* operator new (std::size_t n)
{
    if (counting.load (std::memory_order_relaxed)) ++allocations;
    if (void* p = std::malloc (n)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept
{
    if (p != nullptr && counting.load (std::memory_order_relaxed)) ++frees;
    std::free (p);
}
void operator delete (void* p, std::size_t) noexcept { operator delete (p); }
#endif

//==============================================================================
static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

using namespace fxme::neural;

namespace
{
    constexpr double pi = 3.14159265358979323846;
    const std::string examples = FXME_NAM_EXAMPLES;

    ModelData example (const char* name) { return ModelData::fromFile (examples + "/" + name); }

    std::vector<float> sine (double hz, double rate, int n, double amp)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (amp * std::sin (2.0 * pi * hz * i / rate));
        return x;
    }

    /** A guitar-like test: decaying plucks of a few partials, a little noise. */
    std::vector<float> pluck (double rate, int n)
    {
        std::vector<float> x ((size_t) n);
        unsigned rng = 1;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / rate, local = std::fmod (t, 0.5);
            double s = 0.0;
            for (int h = 1; h <= 4; ++h)
                s += std::sin (2.0 * pi * 110.0 * h * t) / h;
            rng = rng * 1664525u + 1013904223u;
            x[(size_t) i] = (float) (0.3 * std::exp (-6.0 * local) * s + 0.002 * ((double) (rng >> 8) / 8388608.0 - 1.0));
        }
        return x;
    }

    template <typename Proc>
    std::vector<float> run (Proc&& proc, std::vector<float> x, int block)
    {
        for (size_t i = 0; i < x.size(); i += (size_t) block)
            proc (x.data() + i, (int) std::min<size_t> ((size_t) block, x.size() - i));
        return x;
    }

    std::vector<float> runModel (PreparedModel& m, const std::vector<float>& x, int block)
    {
        return run ([&m] (float* io, int n) { m.process (io, n); }, x, block);
    }

    double rms (const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i)
            s += (double) x[i] * x[i];
        return std::sqrt (s / (double) (to - from));
    }

    /** Amplitude of `hz` over the second half (one DFT bin). */
    double amplitude (const std::vector<float>& y, double hz, double rate)
    {
        double re = 0.0, im = 0.0;
        const size_t from = y.size() / 2;
        for (size_t i = from; i < y.size(); ++i)
        {
            re += y[i] * std::cos (2.0 * pi * hz * (double) i / rate);
            im -= y[i] * std::sin (2.0 * pi * hz * (double) i / rate);
        }
        return 2.0 * std::sqrt (re * re + im * im) / (double) (y.size() - from);
    }

    double maxStep (const std::vector<float>& y, size_t from, size_t to)
    {
        double m = 0.0;
        for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
            m = std::max (m, (double) std::abs (y[i] - y[i - 1]));
        return m;
    }

    bool allFinite (const std::vector<float>& y)
    {
        for (float v : y)
            if (! std::isfinite (v))
                return false;
        return true;
    }

    std::unique_ptr<PreparedModel> mustLoad (const ModelData& d, double rate, int block, const LoadOptions& o = {})
    {
        auto r = ModelLoader::load (d, rate, block, o);
        if (! r)
            std::printf ("     load of %s failed: %s\n", d.fileName.c_str(), r.error.c_str());
        return std::move (r.model);
    }
}

int main()
{
    const auto wavenet = example ("wavenet.nam"), lstm = example ("lstm.nam"),
               standard = example ("wavenet_a1_standard.nam"), a2 = example ("A2.nam");
    if (wavenet.isEmpty() || lstm.isEmpty())
    {
        std::printf ("NAM Core's example models not found in %s\n", examples.c_str());
        return 1;
    }

    std::printf ("1. Metadata\n");
    {
        const auto info = ModelLoader::readInfo (wavenet);
        const auto linfo = ModelLoader::readInfo (lstm);
        check (info && info->architecture == "WaveNet" && info->name == "Test Model" && info->sampleRate == 48000.0
                   && info->loudnessDb && std::abs (*info->loudnessDb + 20.02f) < 0.01f,
               "wavenet.nam: WaveNet, \"Test Model\", 48 kHz, loudness -20.02 dB");
        check (linfo && linfo->architecture == "LSTM" && linfo->modeledBy == "Steve", "lstm.nam: LSTM, modelled by Steve");
        const auto sinfo = ModelLoader::readInfo (standard), ainfo = ModelLoader::readInfo (a2);
        std::printf ("     size classes: %s, %s, %s, %s\n", info ? info->sizeClass.c_str() : "?",
                     linfo ? linfo->sizeClass.c_str() : "?", sinfo ? sinfo->sizeClass.c_str() : "?",
                     ainfo ? ainfo->sizeClass.c_str() : "?");
        check (sinfo && sinfo->sizeClass == "Standard" && sinfo->cpuClass() == 2 && ainfo && ainfo->sizeClass == "A2"
                   && linfo && linfo->sizeClass.rfind ("LSTM ", 0) == 0 && linfo->cpuClass() == 0,
               "size classes: Standard, A2, LSTM (CPU classes heavy, heavy, light)");

        ModelData garbage { "broken.nam", { 'n', 'o', 't', ' ', 'j', 's', 'o', 'n' } };
        std::string error;
        const bool refused = ! ModelLoader::readInfo (garbage, &error) && ! error.empty();
        auto r = ModelLoader::load (garbage, 48000.0, 256);
        auto e = ModelLoader::load (ModelData {}, 48000.0, 256);
        ModelData notAModel { "x.nam", { '{', '}' } };
        auto n = ModelLoader::load (notAModel, 48000.0, 256);
        check (refused && ! r && ! r.error.empty() && ! e && ! n && ! n.error.empty(),
               "garbage, empty and non-model data refused with a reason");
    }

    std::printf ("2. Golden: our backend against NAM Core, 48 kHz\n");
    {
        bool ok = true;
        for (const auto* d : { &wavenet, &lstm, &standard })
        {
            auto ours = mustLoad (*d, 48000.0, 256);
            LoadOptions ref;
            ref.forceNamCore = true;
            auto nam = mustLoad (*d, 48000.0, 256, ref);
            if (! ours || ! nam)
            {
                ok = false;
                continue;
            }
            const auto x = pluck (48000.0, 96000);
            const auto a = runModel (*ours, x, 256), b = runModel (*nam, x, 256);
            double diff = 0.0, sig = 0.0, change = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                diff += (double) (a[i] - b[i]) * (a[i] - b[i]);
                sig += (double) b[i] * b[i];
                change += (double) (b[i] - x[i]) * (b[i] - x[i]);
            }
            const double db = 10.0 * std::log10 (diff / sig + 1.0e-30);
            std::printf ("     %-24s %-9s against NAM Core: %.1f dB (output rms %.4f)\n", d->fileName.c_str(),
                         ours->getInfo().backend.c_str(), db, std::sqrt (sig / (double) a.size()));
            ok = ok && db < -100.0 && sig > 1.0e-3 * (double) a.size() * 1.0e-3 && change > 0.0;
        }
        check (ok, "identical within float rounding, and the model does something");
    }

    std::printf ("3. Host rates: the 48 kHz response at 44.1 and 96 kHz\n");
    {
        bool ok = true;
        for (const auto* d : { &wavenet, &lstm })
        {
            double ref1 = 0.0, ref3 = 0.0;
            for (double rate : { 48000.0, 44100.0, 96000.0 })
            {
                auto m = mustLoad (*d, rate, 512);
                if (! m)
                {
                    ok = false;
                    continue;
                }
                const auto y = runModel (*m, sine (500.0, rate, (int) rate, 0.3), 512);
                const double a1 = amplitude (y, 500.0, rate), a3 = amplitude (y, 1500.0, rate);
                if (rate == 48000.0)
                {
                    ref1 = a1;
                    ref3 = a3;
                    continue;
                }
                const double d1 = 20.0 * std::log10 (a1 / ref1), d3 = 20.0 * std::log10 (a3 / ref3);
                std::printf ("     %-12s %6.0f Hz: 500 Hz %+.3f dB, 1500 Hz %+.3f dB (latency %d)\n", d->fileName.c_str(),
                             rate, d1, d3, m->getLatencySamples());
                ok = ok && std::abs (d1) < 0.2 && std::abs (d3) < 0.2;
            }
        }
        check (ok, "fundamental and third harmonic within 0.2 dB");
    }

    std::printf ("4. Latency\n");
    {
        bool ok = true;
        for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            auto m = mustLoad (wavenet, rate, 256);
            auto dry = ModelLoader::makeDry (rate, 256);
            const int nominal = ModelLoader::nominalLatency (rate);
            const auto x = pluck (rate, 20000);
            const auto y = runModel (*dry, x, 100);
            bool exact = true;
            for (size_t i = (size_t) nominal; i < y.size(); ++i)
                exact = exact && y[i] == x[i - (size_t) nominal];
            std::printf ("     %6.0f Hz: nominal %d, model %d, dry delayed exactly: %s\n", rate, nominal,
                         m ? m->getLatencySamples() : -1, exact ? "yes" : "no");
            ok = ok && m && m->getLatencySamples() == nominal && exact && (rate != 48000.0 || nominal == 0);
        }
        check (ok, "every model at the nominal latency (0 at 48 kHz); dry = input delayed by it");
    }

    std::printf ("5. Model swaps\n");
    {
        const double rate = 44100.0;
        const int block = 128;
        NeuralAmpEngine engine;
        engine.prepare (rate, block);
        auto mw = mustLoad (wavenet, rate, block), ml = mustLoad (lstm, rate, block);
        const auto x = sine (220.0, rate, (int) (2.4 * rate), 0.3);
        std::vector<float> y = x;
        const size_t seg = (size_t) (0.6 * rate) / block * block;
        engine.install (std::move (mw));
        for (size_t i = 0; i < y.size(); i += block)
        {
            if (i == seg)
                engine.install (std::move (ml));
            if (i == 2 * seg)
                engine.unload();
            engine.process (y.data() + i, (int) std::min<size_t> (block, y.size() - i));
        }
        // The steady parts' own largest step, against the largest anywhere.
        const size_t settle = (size_t) (0.1 * rate);
        double steady = 0.0;
        for (size_t s : { (size_t) 0, seg, 2 * seg, 3 * seg })
            steady = std::max (steady, maxStep (y, s + settle, std::min (y.size(), s + seg)));
        const double worst = maxStep (y, 1, y.size());
        size_t where = 1;
        for (size_t i = 1; i < y.size(); ++i)
            if (std::abs (y[i] - y[i - 1]) == (float) worst)
                where = i;
        std::printf ("     largest step %.4f at %.3f s (swaps at 0, %.3f, %.3f s), steady signals' own %.4f\n", worst,
                     (double) where / rate, (double) seg / rate, 2.0 * (double) seg / rate, steady);
        check (worst <= 1.05 * steady && allFinite (y), "no step larger than the signals' own");

        auto other = mustLoad (wavenet, 48000.0, block);
        auto small = mustLoad (wavenet, rate, block / 2);
        check (! engine.install (std::move (other)) && ! engine.install (std::move (small)),
               "a model for another rate or a smaller block refused");

        std::vector<float> buf ((size_t) block, 0.1f);
        bool fine = true;
        for (int k = 0; k < 60; ++k)
        {
            engine.install (mustLoad (k % 2 ? wavenet : lstm, rate, block));
            for (int b = 0; b < (k % 3) * 4; ++b)
                engine.process (buf.data(), block);
            fine = fine && std::isfinite (buf[0]);
        }
        engine.collectGarbage();
        check (fine, "60 installs, some before the previous fade ended");
    }

    std::printf ("6. No allocation and no free on the audio thread\n");
    {
        const int block = 256;
        auto count = [&] (const ModelData* d, double rate, long& a, long& f)
        {
            NeuralAmpEngine engine;
            engine.prepare (rate, block);
            if (d != nullptr)
                engine.install (mustLoad (*d, rate, block));
            auto x = pluck (rate, (int) rate);
            engine.process (x.data(), block);   // picks the model up, outside the count
            allocations = 0;
            frees = 0;
            counting = true;
            for (size_t i = (size_t) block; i + (size_t) block <= x.size(); i += (size_t) block)
                engine.process (x.data() + i, block);
            counting = false;
            a = allocations;
            f = frees;
        };
        bool ok = true;
        for (double rate : { 48000.0, 96000.0 })
            for (const ModelData* d : { (const ModelData*) nullptr, &wavenet, &lstm, &standard, &a2 })
            {
                long a = 0, f = 0;
                count (d, rate, a, f);
                std::printf ("     %6.0f Hz %-24s %ld allocations, %ld frees in 1 s\n", rate,
                             d ? d->fileName.c_str() : "(dry)", a, f);
                ok = ok && a == 0 && f == 0;
            }

        // Through a swap: the pick-up, the fade, the retirement.
        NeuralAmpEngine engine;
        engine.prepare (96000.0, block);
        engine.install (mustLoad (standard, 96000.0, block));
        auto x = pluck (96000.0, 96000);
        for (int k = 0; k < 10; ++k)
            engine.process (x.data() + (size_t) (k * block), block);
        engine.install (mustLoad (lstm, 96000.0, block));
        allocations = 0;
        frees = 0;
        counting = true;
        for (int k = 10; k < 60; ++k)
            engine.process (x.data() + (size_t) (k * block), block);
        counting = false;
        std::printf ("     through a swap: %ld allocations, %ld frees\n", allocations.load(), frees.load());
        check (ok && allocations == 0 && frees == 0, "none, per model and through a swap");
    }

    std::printf ("7. Not finite input\n");
    {
        NeuralAmpEngine engine;
        engine.prepare (48000.0, 64);
        engine.install (mustLoad (wavenet, 48000.0, 64));
        std::vector<float> y (64);
        for (int b = 0; b < 30; ++b)
        {
            for (int i = 0; i < 64; ++i)
                y[(size_t) i] = b == 10 ? (i % 3 == 0 ? NAN : i % 3 == 1 ? INFINITY : 1.0e30f) : 0.2f * std::sin ((float) i);
            engine.process (y.data(), 64);
        }
        check (allFinite (y) && engine.getSanitisedCount() >= 64, "finite output after NaN, infinity and 1e30");
    }

    std::printf ("8. Calibration\n");
    {
        const double rate = 48000.0;
        auto level = [&] (bool normalise)
        {
            NeuralAmpEngine engine;
            engine.prepare (rate, 256);
            engine.setNormaliseOutput (normalise);
            engine.install (mustLoad (wavenet, rate, 256));
            auto y = run ([&engine] (float* io, int n) { engine.process (io, n); }, pluck (rate, 48000), 256);
            return rms (y, 12000, y.size());
        };
        const double db = 20.0 * std::log10 (level (true) / level (false));
        std::printf ("     normalised against raw: %+.3f dB (expected %+.3f)\n", db, -18.0 + 20.02);
        check (std::abs (db - (-18.0 + 20.0207)) < 0.01, "the model's loudness brought to -18 dB");
    }

    std::printf ("9. Cost (one mono model, one core) and load time\n");
    {
        std::vector<ModelData> models { wavenet, lstm, standard, a2 };
        if (const char* dir = std::getenv ("FXME_NAM_MODELS"))
            if (DIR* d = opendir (dir))
            {
                while (const dirent* e = readdir (d))
                {
                    const std::string n = e->d_name;
                    if (n.size() > 4 && n.substr (n.size() - 4) == ".nam")
                        models.push_back (ModelData::fromFile (std::string (dir) + "/" + n));
                }
                closedir (d);
            }
        std::printf ("     %-26s %-9s %-7s %8s %8s %8s\n", "model", "backend", "static", "44.1k", "48k", "96k");
        for (const auto& d : models)
        {
            if (d.isEmpty())
                continue;
            std::string backend, isStatic;
            double cost[3] {}, loadMs = 0.0;
            int k = 0;
            for (double rate : { 44100.0, 48000.0, 96000.0 })
            {
                const auto t0 = std::chrono::steady_clock::now();
                auto m = mustLoad (d, rate, 256);
                const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
                if (! m)
                    break;
                if (rate == 48000.0)
                    loadMs = ms;
                backend = m->getInfo().backend;
                isStatic = m->getInfo().isStatic ? "yes" : "no";
                const double seconds = 4.0;
                auto x = pluck (rate, (int) (seconds * rate));
                runModel (*m, pluck (rate, (int) (0.5 * rate)), 256);   // warm up
                const auto t1 = std::chrono::steady_clock::now();
                runModel (*m, x, 256);
                cost[k++] = 100.0 * std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count() / seconds;
            }
            std::printf ("     %-26s %-9s %-7s %7.2f%% %7.2f%% %7.2f%%   load %.1f ms\n", d.fileName.c_str(), backend.c_str(),
                         isStatic.c_str(), cost[0], cost[1], cost[2], loadMs);
        }
    }

    std::printf (failures == 0 ? "All neural tests passed\n" : "%d neural test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
