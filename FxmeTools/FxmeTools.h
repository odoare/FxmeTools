/*
BEGIN_JUCE_MODULE_DECLARATION
  ID:               FxmeTools
  vendor:           odoare
  version:          0.0.3
  name:             FX-Mechanics shared C++ audio tools
  description:      Shared GUI controls, look-and-feel, DSP and image/video
                    input for FX-Mechanics JUCE plugins. The WDL-backed
                    FirFilter is provided as a standalone header (include
                    <FxmeTools/dsp/FirFilter.h>) so consumers that do not need
                    it are not forced to depend on WDL. The image/ classes
                    compile everywhere; camera (juce_video) and video-file
                    (FFmpeg) backends are enabled per consumer by
                    fxmetools_attach_video().
  website:          http://www.github.com/odoare/FxmeTools
  license:          LGPL-3.0-or-later
  dependencies:     juce_audio_basics, juce_audio_processors, juce_audio_utils, juce_core, juce_data_structures, juce_dsp, juce_events, juce_graphics, juce_gui_basics
 END_JUCE_MODULE_DECLARATION
 */

#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Each header below declares its own `namespace fxme { ... }`, so they are
// included here at global scope (not wrapped). This keeps any third-party C/C++
// headers (e.g. WDL, pulled in by dsp/FirFilter.h) out of the fxme namespace.
//
// GUI controls + look-and-feel
#include "lookandfeels/FxmeLookAndFeel.h"
#include "lookandfeels/PanelBackground.h"
#include "components/FxmeSlider.h"
#include "components/FxmeButton.h"
#include "components/FxmeMeters.h"

// Threading: batch background jobs with progress + a message-thread
// completion callback (see threading/BackgroundTaskRunner.h for the pattern
// and a usage example).
#include "threading/BackgroundTaskRunner.h"

// MIDI / music-theory (header-only, no external deps)
#include <FxmeTools/midi/Scale.h>
#include <FxmeTools/midi/ChordName.h>
#include <FxmeTools/midi/NeoRiemannGrid.h>
#include <FxmeTools/midi/GridTransform.h>
#include <FxmeTools/midi/StringSequencer.h>
#include <FxmeTools/midi/SequencerEngine.h>
#include <FxmeTools/midi/NoteDuration.h>

// MIDI / music-theory (header-only, JUCE-based). MidiTools keeps its original
// namespace from CppMusicTools, nested inside fxme (fxme::MidiTools::Chord);
// the text-pattern arpeggiator engine is fxme::Arpeggiator.
#include "midi/MidiTools.h"
#include "midi/Arpeggiator.h"

// DSP (header-only, no external deps)
#include <FxmeTools/dsp/Ambisonics.h>
#include "dsp/AmbixToStereo.h"
#include <FxmeTools/dsp/RoomAcoustics.h>
#include <FxmeTools/dsp/Biquad.h>
#include "dsp/SpectrumTap.h"
#include "dsp/WaveformTap.h"
#include "dsp/SynchronizedSweep.h"
#include "dsp/SpectrumAnalyzer.h"
#include "dsp/SpectralBandSplitter.h"
#include "dsp/RmsMeter.h"
#include "dsp/VuMeter.h"
#include "dsp/SignalGenerator.h"
#include "dsp/Lfo.h"
#include "dsp/ModLfo.h"
#include <FxmeTools/dsp/UnisonSpread.h>
#include "dsp/CracksGenerator.h"
#include <FxmeTools/dsp/PitchShifter.h>
#include <FxmeTools/dsp/GrainLooper.h>
#include <FxmeTools/dsp/Waveshapers.h>
#include <FxmeTools/dsp/Saturator.h>
#include <FxmeTools/dsp/FormantFilter.h>
#include <FxmeTools/dsp/BitCrusher.h>
#include <FxmeTools/dsp/Downsampler.h>
#include <FxmeTools/dsp/DelayLine.h>
#include <FxmeTools/dsp/ModDelayLine.h>
#include <FxmeTools/dsp/AllpassChain.h>
#include <FxmeTools/dsp/StereoCrossDelay.h>
#include <FxmeTools/dsp/DelayTimeResolver.h>
#include <FxmeTools/dsp/DeterministicRandom.h>
#include <FxmeTools/dsp/ArEnvelope.h>

// DSP with out-of-line definitions (compiled from FxmeTools.cpp)
#include "dsp/MicCalibration.h"
#include "dsp/IemDecoder.h"

// Components (real-time analyzer display, SPL meter bar, level bar, help button)
#include "components/SpectrumDisplay.h"
#include "components/SpectrumRegionEditor.h"
#include "components/WaveformDisplay.h"
#include "components/ComponentSnapshot.h"
#include "components/SplMeterComponent.h"
#include "components/VuMeterComponent.h"
#include "components/InfoButton.h"
#include "components/TextEntryFocusFixer.h"
#include "components/SequencerRubber.h"
#include "components/TopBar.h"
#include "components/SplashOverlay.h"
#include "components/SphereView.h"
#include "components/AccentToggle.h"
#include "components/ScaleKeyboardComponent.h"
#include "components/TextEntryFocusFixer.h"

// Image / video input: still pictures, webcams (V4L2 on Linux,
// juce::CameraDevice elsewhere) and video files (FFmpeg), behind one
// FrameSource interface, plus colour adjustments and an analysis-friendly
// luminance grid. See image/README.md; enable the optional backends with
// fxmetools_attach_video() (cmake/FxmeTools.cmake).
#include <FxmeTools/image/CameraPose.h>
#include "image/ColorBlobTracker.h"
#include "image/FrameSource.h"
#include <FxmeTools/image/Homography.h>
#include "image/ImageAdjustments.h"
#include "image/LuminanceGrid.h"
#include "image/StillImageSource.h"
#include "image/V4l2CameraSource.h"
#include "image/JuceCameraSource.h"
#include "image/VideoFileSource.h"
#include "image/VideoEngine.h"

// Finite-element vibro-acoustics: 2D mesh generation, thin-plate modal
// analysis (Morley elements, mixed boundary conditions, tension term) and a
// grid / filled-contour display. The numerical core is JUCE-free — see
// acoustics/README.md.
#include <FxmeTools/acoustics/FemMesh.h>
#include <FxmeTools/acoustics/PlateModes.h>
#include "acoustics/FemViewComponent.h"

// Preset management (factory presets from BinaryData + user XML files) and
// its ready-made browser component + compact name/prev/next strip.
// EmbeddedAudio stores audio files (e.g. impulse responses) inside the state
// tree as FLAC+Base64 so presets and sessions are self-contained.
#include "presets/EmbeddedAudio.h"
#include "presets/EmbeddedImage.h"
#include "presets/PresetManager.h"
#include "components/PresetComponent.h"
#include "components/PresetBarComponent.h"

// Note: dsp/FirFilter.h is intentionally NOT included here — it depends on WDL.
// Include it directly (<FxmeTools/dsp/FirFilter.h>) where needed.
