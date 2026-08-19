# ──────────────────────────────────────────────────────────────────────────
# FxmeCore — the JUCE-free half of FxmeTools.
#
# Everything under core/ builds with a bare C++17 toolchain: no JUCE on the
# include path, no JUCE symbols in the sources. That constraint is the whole
# point of this target, and it is enforced two ways:
#
#   * the `FxmeCoreGuard` target greps the tree and fails the build if a JUCE
#     include or a juce:: symbol reappears;
#   * `FxmeCoreTests` compiles and links without JUCE, so a leak breaks CI even
#     if someone disables the guard.
#
# Include convention is unchanged from before the split: core headers are still
# reached as <FxmeTools/dsp/Foo.h>, because this directory (not the module
# directory) is the include root for them. Consumers need no edits.
#
# Standalone use (fast iteration on the DSP with no JUCE in sight):
#     cmake -S core -B build-core && cmake --build build-core && ctest --test-dir build-core
#
# From the JUCE side this is pulled in by cmake/FxmeTools.cmake.
# ──────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PROJECT_NAME)
    project(FxmeCore VERSION 0.1.0 LANGUAGES CXX)
    set(FXMECORE_STANDALONE TRUE)
endif()

if(TARGET FxmeCore)
    return()   # already added by another consumer in the same build tree
endif()

set(FXMECORE_ROOT "${CMAKE_CURRENT_LIST_DIR}")

# Out-of-line definitions belonging to core headers. Globbed on purpose: files
# arrive here in batches as scripts/split-core.py promotes them, and a glob
# means that migration never also needs a CMake edit. CONFIGURE_DEPENDS makes
# the build system re-glob when the directory changes.
#
# Header-only components (most of dsp/ and midi/) need no entry at all — they
# reach consumers through the include directory below.
file(GLOB_RECURSE FXMECORE_SOURCES CONFIGURE_DEPENDS
     "${FXMECORE_ROOT}/FxmeTools/*.cpp")

add_library(FxmeCore STATIC ${FXMECORE_SOURCES})
add_library(Fxme::Core ALIAS FxmeCore)

target_include_directories(FxmeCore PUBLIC ${FXMECORE_ROOT})
target_compile_features(FxmeCore PUBLIC cxx_std_17)

set_target_properties(FxmeCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON     # linked into shared plugin bundles
    CXX_EXTENSIONS OFF)

target_compile_options(FxmeCore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

# ──────────────────────────────────────────────────────────────────────────
# Optional: the WDL real FFT, compiled without JUCE.
#
# A few core headers (SpectralFreeze, and later SpectrumAnalyzer once it stops
# using juce::dsp::FFT) need an FFT. WDL is already a submodule of this repo and
# is zlib-licensed, so it is the natural JUCE-free choice. The target only
# appears when the submodule is checked out, so `git clone` without
# --recurse-submodules still configures and builds the rest.
# ──────────────────────────────────────────────────────────────────────────
set(FXMECORE_WDL_DIR "${FXMECORE_ROOT}/../WDL/WDL")

if(EXISTS "${FXMECORE_WDL_DIR}/fft.c")
    enable_language(C)
    add_library(FxmeCoreWdlFft STATIC "${FXMECORE_WDL_DIR}/fft.c")
    target_include_directories(FxmeCoreWdlFft PUBLIC "${FXMECORE_WDL_DIR}")
    set_target_properties(FxmeCoreWdlFft PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # WDL's headers pull in <windows.h> on MSVC; its min/max macros clobber
    # std::min/std::max in anything that includes them afterwards.
    target_compile_definitions(FxmeCoreWdlFft PUBLIC $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>)
else()
    message(STATUS "FxmeCore: WDL submodule not checked out — FFT-backed core components disabled")
endif()

# ──────────────────────────────────────────────────────────────────────────
# Guard: fail the build if JUCE leaks back into core/.
# ──────────────────────────────────────────────────────────────────────────
add_custom_target(FxmeCoreGuard ALL
    COMMAND ${CMAKE_COMMAND}
            -DFXMECORE_DIR=${FXMECORE_ROOT}/FxmeTools
            -P ${FXMECORE_ROOT}/cmake/CheckNoJuce.cmake
    COMMENT "FxmeCore: checking that core/ stays JUCE-free"
    VERBATIM)

add_dependencies(FxmeCore FxmeCoreGuard)

# ──────────────────────────────────────────────────────────────────────────
# Tests. Enabled when core/ is the top-level project, or when the parent
# project has already called enable_testing().
# ──────────────────────────────────────────────────────────────────────────
if(FXMECORE_STANDALONE)
    enable_testing()
endif()

if(BUILD_TESTING OR FXMECORE_STANDALONE)
    add_subdirectory(tests)
endif()
