/*
BEGIN_JUCE_MODULE_DECLARATION
  ID:               FxmeTools
  vendor:           odoare
  version:          0.0.1
  name:             FX-Mechanics shared C++ audio tools
  description:      Shared GUI controls, look-and-feel and DSP for FX-Mechanics
                    JUCE plugins. The WDL-backed FirFilter is provided as a
                    standalone header (include <FxmeTools/dsp/FirFilter.h>) so
                    consumers that do not need it are not forced to depend on WDL.
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
#include "components/FxmeSlider.h"
#include "components/FxmeButton.h"
#include "components/FxmeMeters.h"

// DSP (header-only, no external deps)
#include "dsp/Biquad.h"
#include "dsp/SpectrumTap.h"
#include "dsp/SpectrumAnalyzer.h"
#include "dsp/RmsMeter.h"
#include "dsp/VuMeter.h"
#include "dsp/SignalGenerator.h"

// Components (real-time analyzer display, SPL meter bar, level bar, help button)
#include "components/SpectrumDisplay.h"
#include "components/SplMeterComponent.h"
#include "components/VuMeterComponent.h"
#include "components/InfoButton.h"

// Note: dsp/FirFilter.h is intentionally NOT included here — it depends on WDL.
// Include it directly (<FxmeTools/dsp/FirFilter.h>) where needed.
