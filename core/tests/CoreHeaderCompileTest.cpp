/*
  ------------------------------------------------------------------------------
    CoreHeaderCompileTest.cpp

    Includes every core header that currently exists, in one translation unit,
    with no JUCE on the include path. Its only job is to fail loudly the moment
    a core header stops being self-contained — because it started relying on
    something JuceHeader.h used to drag in for free (<cmath>, <vector>,
    juce::jlimit...).

    That is the main way a core/shell split rots in practice: the file has no
    juce:: symbol so the CheckNoJuce guard passes, but it only ever compiled
    because the JUCE umbrella header happened to be included first.

    Every include is __has_include-guarded, so this file does not need editing
    as headers migrate in: it simply covers more each time.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#define FXME_TRY_INCLUDE(path) \
    __has_include(path)

// -- util (always present) ------------------------------------------------
#include <FxmeTools/util/CoreVersion.h>
#include <FxmeTools/util/Math.h>
#include <FxmeTools/util/Random.h>
#include <FxmeTools/util/AudioBufferView.h>
#include <FxmeTools/util/ProcessSpec.h>
#include <FxmeTools/util/Fft.h>
#include <FxmeTools/util/AudioBuffer.h>
#include <FxmeTools/util/SmoothedValue.h>
#include <FxmeTools/util/StringRef.h>
#include <FxmeTools/util/StringUtils.h>
#include <FxmeTools/util/ArrayView.h>

// -- dsp ------------------------------------------------------------------
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/AllpassChain.h>)
 #include <FxmeTools/dsp/AllpassChain.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/Ambisonics.h>)
 #include <FxmeTools/dsp/Ambisonics.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/ArEnvelope.h>)
 #include <FxmeTools/dsp/ArEnvelope.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/Biquad.h>)
 #include <FxmeTools/dsp/Biquad.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/BitCrusher.h>)
 #include <FxmeTools/dsp/BitCrusher.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/CracksGenerator.h>)
 #include <FxmeTools/dsp/CracksGenerator.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/DelayLine.h>)
 #include <FxmeTools/dsp/DelayLine.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/DelayTimeResolver.h>)
 #include <FxmeTools/dsp/DelayTimeResolver.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/DeterministicRandom.h>)
 #include <FxmeTools/dsp/DeterministicRandom.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/Downsampler.h>)
 #include <FxmeTools/dsp/Downsampler.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/FormantFilter.h>)
 #include <FxmeTools/dsp/FormantFilter.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/GrainLooper.h>)
 #include <FxmeTools/dsp/GrainLooper.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/Lfo.h>)
 #include <FxmeTools/dsp/Lfo.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/ModDelayLine.h>)
 #include <FxmeTools/dsp/ModDelayLine.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/ModLfo.h>)
 #include <FxmeTools/dsp/ModLfo.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/PitchShifter.h>)
 #include <FxmeTools/dsp/PitchShifter.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/RmsMeter.h>)
 #include <FxmeTools/dsp/RmsMeter.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/RoomAcoustics.h>)
 #include <FxmeTools/dsp/RoomAcoustics.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/Saturator.h>)
 #include <FxmeTools/dsp/Saturator.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/SignalGenerator.h>)
 #include <FxmeTools/dsp/SignalGenerator.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/SpectralBandSplitter.h>)
 #include <FxmeTools/dsp/SpectralBandSplitter.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/SpectrumTap.h>)
 #include <FxmeTools/dsp/SpectrumTap.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/StereoCrossDelay.h>)
 #include <FxmeTools/dsp/StereoCrossDelay.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/UnisonSpread.h>)
 #include <FxmeTools/dsp/UnisonSpread.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/VuMeter.h>)
 #include <FxmeTools/dsp/VuMeter.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/WaveformTap.h>)
 #include <FxmeTools/dsp/WaveformTap.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/dsp/Waveshapers.h>)
 #include <FxmeTools/dsp/Waveshapers.h>
#endif

// SpectralFreeze needs the WDL real FFT; the target only defines this when the
// WDL submodule is actually checked out.
#if defined(FXME_CORE_HAS_WDL) && FXME_CORE_HAS_WDL \
    && FXME_TRY_INCLUDE(<FxmeTools/dsp/SpectralFreeze.h>)
 #include <FxmeTools/dsp/SpectralFreeze.h>
#endif

// Reverb wraps WDL's Freeverb-derived engine, so it is gated the same way.
#if defined(FXME_CORE_HAS_WDL) && FXME_CORE_HAS_WDL \
    && FXME_TRY_INCLUDE(<FxmeTools/dsp/Reverb.h>)
 #include <FxmeTools/dsp/Reverb.h>
#endif

// -- midi -----------------------------------------------------------------
#if FXME_TRY_INCLUDE(<FxmeTools/midi/ChordName.h>)
 #include <FxmeTools/midi/ChordName.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/GridTransform.h>)
 #include <FxmeTools/midi/GridTransform.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/NeoRiemannGrid.h>)
 #include <FxmeTools/midi/NeoRiemannGrid.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/MidiTools.h>)
 #include <FxmeTools/midi/MidiTools.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/NoteDuration.h>)
 #include <FxmeTools/midi/NoteDuration.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/Scale.h>)
 #include <FxmeTools/midi/Scale.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/SequencerEngine.h>)
 #include <FxmeTools/midi/SequencerEngine.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/midi/StringSequencer.h>)
 #include <FxmeTools/midi/StringSequencer.h>
#endif

// -- math -----------------------------------------------------------------
#if FXME_TRY_INCLUDE(<FxmeTools/math/ParallelFor.h>)
 #include <FxmeTools/math/ParallelFor.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/math/LinearOperator.h>)
 #include <FxmeTools/math/LinearOperator.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/math/DenseLinearAlgebra.h>)
 #include <FxmeTools/math/DenseLinearAlgebra.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/math/SparseMatrix.h>)
 #include <FxmeTools/math/SparseMatrix.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/math/BandwidthOrdering.h>)
 #include <FxmeTools/math/BandwidthOrdering.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/math/SkylineCholesky.h>)
 #include <FxmeTools/math/SkylineCholesky.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/math/SubspaceEigensolver.h>)
 #include <FxmeTools/math/SubspaceEigensolver.h>
#endif

// -- acoustics ------------------------------------------------------------
#if FXME_TRY_INCLUDE(<FxmeTools/acoustics/FemMesh.h>)
 #include <FxmeTools/acoustics/FemMesh.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/acoustics/PlateModes.h>)
 #include <FxmeTools/acoustics/PlateModes.h>
#endif

// -- image (geometry only; the JUCE-backed sources stay on the other side) --
#if FXME_TRY_INCLUDE(<FxmeTools/image/CameraPose.h>)
 #include <FxmeTools/image/CameraPose.h>
#endif
#if FXME_TRY_INCLUDE(<FxmeTools/image/Homography.h>)
 #include <FxmeTools/image/Homography.h>
#endif

#include <cstdio>

int main()
{
    std::printf ("FxmeCore %s — all present core headers are self-contained without JUCE\n",
                 fxme::coreVersionString());
    return 0;
}
