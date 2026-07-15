cmake_minimum_required(VERSION 3.22)

# Unit tests for the FxmeTools midi/Arpeggiator engine.
# This directory is meant to be added from a consumer project's CMakeLists
# (after add_subdirectory(JUCE)), e.g.:
#     add_subdirectory(Source/libs/FxmeTools/tests)

# doctest — header-only test framework, fetched at configure time
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(doctest)

juce_add_console_app(FxmeToolsTests
    PRODUCT_NAME "FxmeTools Tests"
)

target_sources(FxmeToolsTests
    PRIVATE
        main.cpp
        test_arpeggiator.cpp
)

# The FxmeTools module folder lives one directory up from here, so
# <FxmeTools/midi/Arpeggiator.h> resolves against the repo root.
target_include_directories(FxmeToolsTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_definitions(FxmeToolsTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(FxmeToolsTests
    PRIVATE
        doctest::doctest
        juce_audio_basics
        juce_core
        juce_data_structures
        juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Must be called after target_link_libraries so the generated JuceHeader.h
# picks up all linked modules. The tests include <JuceHeader.h>, which
# resolves to this generated file via the include path JUCE adds automatically.
juce_generate_juce_header(FxmeToolsTests)

add_test(NAME ArpeggiatorTests COMMAND FxmeToolsTests)
