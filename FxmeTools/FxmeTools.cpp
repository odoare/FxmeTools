#include "FxmeTools.h"

// The GUI controls, look-and-feel and Biquad are header-only. The spectrum
// analyzer/display carry out-of-line definitions; each .cpp declares its own
// `namespace fxme { ... }`, so they are #included here at global scope.
#include "dsp/SpectrumAnalyzer.cpp"
#include "components/SpectrumDisplay.cpp"
#include "dsp/CracksGenerator.cpp"
#include "threading/BackgroundTaskRunner.cpp"
#include "presets/EmbeddedAudio.cpp"
#include "presets/PresetManager.cpp"
#include "components/PresetComponent.cpp"
#include "acoustics/FemMesh.cpp"
#include "acoustics/PlateModes.cpp"
