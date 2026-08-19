# Tests for the JUCE-free half of FxmeTools.
#
# These deliberately link ONLY FxmeCore. If a test ever needs JUCE to build,
# the code under test belongs on the other side of the split.

# ── 1. Behaviour of the core replacements for the JUCE helpers ──────────────
add_executable(FxmeCoreTests CoreUtilTests.cpp)
target_link_libraries(FxmeCoreTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreTests COMMAND FxmeCoreTests)

# ── 2. Every core header is self-contained without JUCE ────────────────────
add_executable(FxmeCoreHeaderTest CoreHeaderCompileTest.cpp)
target_link_libraries(FxmeCoreHeaderTest PRIVATE FxmeCore)
add_test(NAME FxmeCoreHeaderTest COMMAND FxmeCoreHeaderTest)

# SpectralFreeze.h sits in core but needs the WDL real FFT. Compile it into the
# header test only when the WDL submodule is actually checked out, so a fresh
# clone without submodules still builds and tests the rest.
if(TARGET FxmeCoreWdlFft)
    target_compile_definitions(FxmeCoreHeaderTest PRIVATE FXME_CORE_HAS_WDL=1)
    target_link_libraries(FxmeCoreHeaderTest PRIVATE FxmeCoreWdlFft)
else()
    message(STATUS "FxmeCore: SpectralFreeze.h excluded from the header test (no WDL)")
endif()

foreach(_t FxmeCoreTests FxmeCoreHeaderTest)
    target_compile_options(${_t} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
endforeach()
