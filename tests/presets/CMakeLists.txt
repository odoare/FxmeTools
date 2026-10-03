# FxmeToolsPresetTests: ModulePresetLibrary / ModulePresetTarget against a
# real APVTS (see doc/local-presets-plan.md, phase 3). No test framework to
# fetch: plain checks, exit code 0 when everything passes.
#
# Added from a consumer that has already registered JUCE, the FxmeTools
# module (target FxmeTools) and FxmeCore, on request only. Dede:
#     cmake -B build -DDEDE_BUILD_PRESET_TESTS=ON
#     cmake --build build --target FxmeToolsPresetTests
#     build/FxmeToolsPresetTests_artefacts/<config>/FxmeToolsPresetTests

juce_add_console_app(FxmeToolsPresetTests
    PRODUCT_NAME "FxmeTools Preset Tests"
)

target_sources(FxmeToolsPresetTests PRIVATE main.cpp)

target_compile_definitions(FxmeToolsPresetTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1      # runDispatchLoopUntil, for the async parts
)

target_link_libraries(FxmeToolsPresetTests
    PRIVATE
        FxmeTools
        FxmeCore
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

juce_generate_juce_header(FxmeToolsPresetTests)
