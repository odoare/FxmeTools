/*
  ------------------------------------------------------------------------------
    FxmeTools.cpp

    The module's single translation unit: the out-of-line definitions belonging
    to the JUCE-side headers, gathered into one compilation. The framework-free
    half compiles separately as the FxmeCore static library.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

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
