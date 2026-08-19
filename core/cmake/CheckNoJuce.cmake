# Fails if any source under FXMECORE_DIR references JUCE.
#
# This is the mechanism that keeps the core/juce split from silently rotting:
# without it, one `#include <JuceHeader.h>` added for convenience re-couples the
# whole library and nobody notices until a migration is attempted.
#
# Run standalone with:
#     cmake -DFXMECORE_DIR=core/FxmeTools -P core/cmake/CheckNoJuce.cmake

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED FXMECORE_DIR)
    message(FATAL_ERROR "CheckNoJuce.cmake: FXMECORE_DIR is not set")
endif()

file(GLOB_RECURSE _core_sources
     "${FXMECORE_DIR}/*.h"
     "${FXMECORE_DIR}/*.hpp"
     "${FXMECORE_DIR}/*.cpp"
     "${FXMECORE_DIR}/*.mm")

set(_offenders "")

foreach(_f IN LISTS _core_sources)
    file(READ "${_f}" _text)

    set(_hits "")

    # A juce:: qualified name, or an include of a JUCE header. Deliberately
    # also catches occurrences inside comments: a stale comment referring to
    # juce::Foo is worth cleaning up too, and keeping the rule dumb keeps it
    # trustworthy.
    if(_text MATCHES "juce::")
        list(APPEND _hits "juce:: symbol")
    endif()
    if(_text MATCHES "JuceHeader\\.h")
        list(APPEND _hits "<JuceHeader.h>")
    endif()
    if(_text MATCHES "#[ \t]*include[ \t]*<juce_")
        list(APPEND _hits "<juce_*> module header")
    endif()

    if(NOT _hits STREQUAL "")
        file(RELATIVE_PATH _rel "${FXMECORE_DIR}" "${_f}")
        string(REPLACE ";" ", " _hits_str "${_hits}")
        list(APPEND _offenders "  ${_rel}: ${_hits_str}")
    endif()
endforeach()

if(NOT _offenders STREQUAL "")
    string(REPLACE ";" "\n" _report "${_offenders}")
    message(FATAL_ERROR
        "FxmeCore is meant to be JUCE-free, but JUCE references were found:\n"
        "${_report}\n"
        "\n"
        "Either keep the file in the JUCE half of the library, or replace the\n"
        "JUCE dependency with its core equivalent:\n"
        "  juce::jlimit/jmax/jmin      -> fxme::jlimit/jmax/jmin      <FxmeTools/util/Math.h>\n"
        "  juce::MathConstants         -> fxme::MathConstants         <FxmeTools/util/Math.h>\n"
        "  juce::Decibels              -> fxme::Decibels              <FxmeTools/util/Math.h>\n"
        "  juce::roundToInt            -> fxme::roundToInt            <FxmeTools/util/Math.h>\n"
        "  juce::Random                -> fxme::Random                <FxmeTools/util/Random.h>\n"
        "  juce::AudioBuffer<float>&   -> fxme::AudioBufferView       <FxmeTools/util/AudioBufferView.h>\n"
        "  juce::dsp::ProcessSpec      -> fxme::ProcessSpec           <FxmeTools/util/ProcessSpec.h>\n")
endif()

list(LENGTH _core_sources _scanned)
message(STATUS "FxmeCore: JUCE-free check passed (${_scanned} sources scanned)")
