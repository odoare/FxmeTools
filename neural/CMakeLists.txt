# ──────────────────────────────────────────────────────────────────────────
# FxmeNeural: neural amp models (NAM's WaveNet and LSTM, Keras through
# RTNeural), JUCE-free, an optional target beside FxmeCore.
#
# Optional because it brings a third-party library (NeuralAudio, and through
# it NAM Core, RTNeural, Eigen, a JSON parser) that nothing else in
# FxmeTools needs. A consumer adds it with fxmetools_attach_neural (<target>)
# (cmake/FxmeTools.cmake); everyone else never configures this directory.
#
# Same include spelling as core: <FxmeTools/neural/...>, this directory being
# one more include root. The backend stays private: no NeuralAudio header
# reaches a consumer, so the backend can change without touching one.
#
# NeuralAudio comes from, in order:
#   NEURALAUDIO_DIR                  set by the top-level project's pin,
#                                    fxme_dependency (NeuralAudio ...), or -D
#   ../NeuralAudio                   a sibling clone (cloned with
#                                    --recurse-submodules)
#   the pin below, by FetchContent   standalone builds without a sibling
#
# Standalone (the tests, in seconds once NeuralAudio is built):
#     cmake -S neural -B build-neural -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-neural --target FxmeNeuralTests -j2
#     ctest --test-dir build-neural
# ──────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PROJECT_NAME)
    project(FxmeNeural VERSION 0.1.0 LANGUAGES C CXX)
    set(FXMENEURAL_STANDALONE TRUE)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release)   # a neural network at -O0 is useless to test
    endif()
endif()

if(TARGET FxmeNeural)
    return()
endif()

set(FXMENEURAL_ROOT "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(_fxmeneural_tools_root "${FXMENEURAL_ROOT}/.." ABSOLUTE)

if(NOT TARGET FxmeCore)
    add_subdirectory("${_fxmeneural_tools_root}/core" "${CMAKE_BINARY_DIR}/FxmeCore")
endif()

# ── NeuralAudio ──────────────────────────────────────────────────────────
# The pin for standalone builds; consumers pin it themselves (the same
# commit, normally) in their cmake/Dependencies.cmake. Our fork
# (odoare/NeuralAudio) until its fixes are merged upstream
# (mikeoliphant/NeuralAudio): the WaveNets' allocations on the audio thread
# (branch fxme-no-audio-alloc).
set(FXMENEURAL_NEURALAUDIO_PIN 2aeff04543bb678546c5128b3e4aa0dc54278e57)

if(NOT NEURALAUDIO_DIR)
    get_filename_component(_sibling "${_fxmeneural_tools_root}/../NeuralAudio" ABSOLUTE)
    if(EXISTS "${_sibling}/NeuralAudio/NeuralModel.h")
        set(NEURALAUDIO_DIR "${_sibling}")
        message(STATUS "FxmeNeural: NeuralAudio from the sibling clone ${NEURALAUDIO_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(NeuralAudio
            GIT_REPOSITORY https://github.com/odoare/NeuralAudio
            GIT_TAG        ${FXMENEURAL_NEURALAUDIO_PIN}
            SOURCE_SUBDIR  fxme-populate-only)
        FetchContent_MakeAvailable(NeuralAudio)
        set(NEURALAUDIO_DIR "${neuralaudio_SOURCE_DIR}")
        message(STATUS "FxmeNeural: NeuralAudio pinned ${FXMENEURAL_NEURALAUDIO_PIN}")
    endif()
endif()

if(NOT EXISTS "${NEURALAUDIO_DIR}/deps/NeuralAmpModelerCore/NAM/dsp.h")
    message(FATAL_ERROR "FxmeNeural: ${NEURALAUDIO_DIR} has no submodules; "
                        "run git submodule update --init --recursive there")
endif()

# NeuralAudio's options (its own defaults otherwise). NAM Core stays on: the
# LSTMs load through it (phase 0: NeuralAudio's own LSTM differs by -50 dB).
set(BUILD_NAMCORE ON CACHE BOOL "" FORCE)

# The instruction set of the networks (NeuralAudio, RTNeural only; the rest
# of the plugin is unaffected). Measured (doc/neural-phase0): AVX2 and FMA
# halve the cost of the WaveNets against the x86-64 baseline.
#   ""            the compiler's default (x86-64: SSE2), runs everywhere
#   "x86-64-v3"   AVX2, FMA, BMI2: Intel Haswell (2013) / AMD Excavator (2015)
#                 and later; an older CPU stops on an illegal instruction
#   "native"      this machine (local benchmarks only)
# Not for ARM (the Pi's flags differ): leave empty there.
# MULTIFRAME_8X8_CONVOLUTION stays NeuralAudio's choice per compiler (0 for
# GCC < 16: 8 made the A2 models 2 to 3 x slower with GCC 13).
set(FXME_NEURAL_ARCH "" CACHE STRING "-march for the neural networks: empty, x86-64-v3 or native")

# Only the x86 builds get it: on a macOS universal build, the Intel slice
# alone (-Xarch_x86_64); ARM builds ignore it. FXME_NEURAL_REQUIRES_AVX2 then
# tells the loader to check the CPU, so that a machine without AVX2 / FMA
# gets an error message instead of an illegal instruction.
set(_fxmeneural_arch_flags "")
set(_fxmeneural_requires_avx2 0)
if(FXME_NEURAL_ARCH)
    if(MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86|x86")
            set(_fxmeneural_arch_flags /arch:AVX2)
            set(_fxmeneural_requires_avx2 1)
        endif()
    elseif(APPLE)
        if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES
           OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"))
            set(_fxmeneural_arch_flags "SHELL:-Xarch_x86_64 -march=${FXME_NEURAL_ARCH}")
            set(_fxmeneural_requires_avx2 1)
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
        set(_fxmeneural_arch_flags -march=${FXME_NEURAL_ARCH})
        set(_fxmeneural_requires_avx2 1)
    endif()
    if(_fxmeneural_arch_flags)
        message(STATUS "FxmeNeural: networks built for ${FXME_NEURAL_ARCH} (${_fxmeneural_arch_flags})")
    else()
        message(STATUS "FxmeNeural: FXME_NEURAL_ARCH ignored on ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()
if(FXME_NEURAL_ARCH STREQUAL "native")
    set(_fxmeneural_requires_avx2 0)   # a local build: this machine has what it was built for
endif()

# NeuralAudio's API uses std::filesystem: macOS 10.15 at least, whatever
# the consumer's deployment target (FxmeFX's is 10.13). Public: the plugin
# that links it is then a 10.15 plugin too.
set(_fxmeneural_macos_min "")
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 10.15)
    set(_fxmeneural_macos_min -mmacosx-version-min=10.15)
endif()

add_subdirectory("${NEURALAUDIO_DIR}/NeuralAudio" "${CMAKE_BINARY_DIR}/NeuralAudio" EXCLUDE_FROM_ALL)

# NeuralAudio sets no optimisation of its own. A Debug build of the plugin
# keeps the networks at -O2: at -O0 a standard WaveNet does not run in real
# time, and nobody debugs inside Eigen.
foreach(_t NeuralAudio RTNeural)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(NOT MSVC)
            target_compile_options(${_t} PRIVATE $<$<CONFIG:Debug>:-O2>)
        endif()
        if(_fxmeneural_arch_flags)
            target_compile_options(${_t} PRIVATE ${_fxmeneural_arch_flags})
        endif()
        if(_fxmeneural_macos_min)
            target_compile_options(${_t} PRIVATE ${_fxmeneural_macos_min})
        endif()
    endif()
endforeach()

# ── The target ───────────────────────────────────────────────────────────
file(GLOB_RECURSE FXMENEURAL_SOURCES CONFIGURE_DEPENDS "${FXMENEURAL_ROOT}/FxmeTools/*.cpp")

add_library(FxmeNeural STATIC ${FXMENEURAL_SOURCES})
add_library(Fxme::Neural ALIAS FxmeNeural)

target_include_directories(FxmeNeural PUBLIC "${FXMENEURAL_ROOT}")
target_link_libraries(FxmeNeural PUBLIC FxmeCore PRIVATE NeuralAudio)
target_compile_features(FxmeNeural PUBLIC cxx_std_17)
set_target_properties(FxmeNeural PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)
target_compile_definitions(FxmeNeural PRIVATE NAM_SAMPLE_FLOAT DSP_SAMPLE_FLOAT
    FXME_NEURAL_REQUIRES_AVX2=${_fxmeneural_requires_avx2})
if(_fxmeneural_macos_min)
    target_compile_options(FxmeNeural PUBLIC ${_fxmeneural_macos_min})
    target_link_options(FxmeNeural PUBLIC ${_fxmeneural_macos_min})
endif()
# NamRegistry.cpp sees NAM Core's headers (and so Eigen's).
target_include_directories(FxmeNeural SYSTEM PRIVATE
    "${NEURALAUDIO_DIR}/deps/NeuralAmpModelerCore"
    "${NEURALAUDIO_DIR}/deps/RTNeural/modules/Eigen")
target_compile_options(FxmeNeural PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /bigobj>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)

# The same guard as core: no JUCE here either.
add_custom_target(FxmeNeuralGuard ALL
    COMMAND ${CMAKE_COMMAND}
            -DFXMECORE_DIR=${FXMENEURAL_ROOT}/FxmeTools
            -P ${_fxmeneural_tools_root}/core/cmake/CheckNoJuce.cmake
    COMMENT "FxmeNeural: checking that neural/ stays JUCE-free"
    VERBATIM)
add_dependencies(FxmeNeural FxmeNeuralGuard)

set(FXMENEURAL_NEURALAUDIO_DIR "${NEURALAUDIO_DIR}" CACHE INTERNAL "")

if(FXMENEURAL_STANDALONE)
    enable_testing()
endif()
if(FXMENEURAL_STANDALONE OR FXME_NEURAL_TESTS)
    add_subdirectory(tests)
endif()
