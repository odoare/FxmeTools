#include "FxmeTools.h"

// The GUI controls, look-and-feel and Biquad are header-only. The spectrum
// analyzer/display carry out-of-line definitions; each .cpp declares its own
// `namespace fxme { ... }`, so they are #included here at global scope.
#include "components/SpectrumDisplay.cpp"
#include "components/SpectrumRegionEditor.cpp"
#include "threading/BackgroundTaskRunner.cpp"
#include "presets/EmbeddedAudio.cpp"
#include "presets/EmbeddedImage.cpp"
#include "presets/PresetManager.cpp"
#include "components/PresetComponent.cpp"
#include "image/V4l2CameraSource.cpp"
#include "image/VideoFileSource.cpp"
#include "image/VideoEngine.cpp"
#include "dsp/MicCalibration.cpp"
#include "dsp/IemDecoder.cpp"
