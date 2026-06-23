# ──────────────────────────────────────────────────────────────────────────
# FxmeTools.cmake
#
# Registers the FxmeTools JUCE module and provides fxmetools_attach(<target>)
# which links the module and compiles + exposes the WDL convolution engine
# (needed by <FxmeTools/dsp/FirFilter.h>).
#
# Usage from a consumer project (after add_subdirectory(JUCE)):
#     include(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/cmake/FxmeTools.cmake)
#     ...
#     fxmetools_attach(MyPlugin)
# ──────────────────────────────────────────────────────────────────────────

get_filename_component(FXMETOOLS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(FXMETOOLS_MODULE_DIR "${FXMETOOLS_ROOT}/FxmeTools")
set(FXMETOOLS_WDL_DIR    "${FXMETOOLS_ROOT}/WDL/WDL")

# Registers the module target `FxmeTools` (named after the module folder).
juce_add_module("${FXMETOOLS_MODULE_DIR}")

# Attach FxmeTools (module + WDL convolution engine) to a target.
function(fxmetools_attach target)
    target_link_libraries(${target} PRIVATE FxmeTools)

    # WDL convolution engine sources for <FxmeTools/dsp/FirFilter.h>.
    target_sources(${target} PRIVATE
        ${FXMETOOLS_WDL_DIR}/convoengine.cpp
        ${FXMETOOLS_WDL_DIR}/fft.c
        ${FXMETOOLS_WDL_DIR}/resample.cpp)
    target_include_directories(${target} PRIVATE ${FXMETOOLS_WDL_DIR})

    # WDL's convoengine.h pulls in <windows.h>; without NOMINMAX its min/max
    # macros clobber std::min/std::max in headers like FirFilter.h (MSVC C2589).
    target_compile_definitions(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>)
endfunction()
