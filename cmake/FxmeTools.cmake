# ──────────────────────────────────────────────────────────────────────────
# FxmeTools.cmake
#
# Registers the FxmeTools JUCE module and provides:
#
#   fxmetools_attach(<target>)
#       links the module and the JUCE-free FxmeCore static library, and
#       compiles + exposes the WDL convolution engine (needed by
#       <FxmeTools/dsp/FirFilter.h>).
#
#       A target that only needs headers — an offline test with no GUI, say —
#       can skip this and add both include roots by hand instead:
#           ${FXMETOOLS_ROOT} and ${FXMETOOLS_CORE_DIR}
#
#   fxmetools_attach_video(<target> [NO_CAMERA] [NO_FFMPEG])
#       enables the optional backends of FxmeTools/image (see its README):
#       webcams (juce_video on Windows/macOS, V4L2 on Linux) and video-file
#       decoding (FFmpeg via pkg-config). Both degrade gracefully when
#       unavailable, so calling it is safe on any machine.
#
#   fxmetools_add_clap(<target> ID <clap-id> FEATURES <feature>...)
#       adds a <target>_CLAP format target through clap-juce-extensions,
#       which the consumer pins as ClapJuceExtensions (CLAPJUCEEXTENSIONS_DIR).
#
# Usage from a consumer project (after add_subdirectory(JUCE)):
#     include(${FXMETOOLS_DIR}/cmake/FxmeTools.cmake)
#
# FXMETOOLS_DIR comes from the consumer's cmake/FxmeDeps.cmake (sibling clone
# or pinned commit). Including this file more than once — from a consumer and
# from a FxmeFX helper, say — is safe: only the first include registers targets.
#     ...
#     fxmetools_attach(MyPlugin)
#     fxmetools_attach_video(MyPlugin)     # only if the plugin uses image input
# ──────────────────────────────────────────────────────────────────────────

get_filename_component(FXMETOOLS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(FXMETOOLS_MODULE_DIR "${FXMETOOLS_ROOT}/FxmeTools")
set(FXMETOOLS_CORE_DIR   "${FXMETOOLS_ROOT}/core")
set(FXMETOOLS_WDL_DIR    "${FXMETOOLS_ROOT}/WDL/WDL")

# The paths above are set on every include, so a later include from another
# directory scope still sees them; targets and functions are global and are
# registered once.
include_guard(GLOBAL)

# The JUCE-free half of the library (see core/README.md). It is a second
# include root for the very same <FxmeTools/...> spelling, so whether a header
# lives here or in the module directory is invisible to consumers.
add_subdirectory("${FXMETOOLS_CORE_DIR}" FxmeCore)

# Registers the module target `FxmeTools` (named after the module folder).
juce_add_module("${FXMETOOLS_MODULE_DIR}")

# The module's own headers include core ones, and the module target is an
# INTERFACE library whose sources are compiled into the consumer — so core has
# to travel with it rather than being linked separately by every consumer.
target_link_libraries(FxmeTools INTERFACE FxmeCore)

# Attach FxmeTools (module + WDL convolution engine) to a target.
function(fxmetools_attach target)
    target_link_libraries(${target} PRIVATE FxmeTools FxmeCore)

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

# ──────────────────────────────────────────────────────────────────────────
# Optional image/video backends for FxmeTools/image.
#
# Defines FXME_HAS_JUCE_CAMERA and FXME_HAS_FFMPEG (always, 0 or 1) on the
# target — PUBLIC because the module's own sources are compiled into it.
#
#   fxmetools_attach_video(MyPlugin)             # everything available
#   fxmetools_attach_video(MyPlugin NO_FFMPEG)   # cameras only
#   fxmetools_attach_video(MyPlugin NO_CAMERA)   # video files only
#
# Linux webcams need nothing extra (V4L2 is a kernel ioctl API), so on Linux
# the camera path is always on. Windows/macOS link juce_video for
# juce::CameraDevice. FFmpeg is found with pkg-config; when it is missing the
# build still succeeds and VideoEngine::isVideoFileSupported() returns false.
# ──────────────────────────────────────────────────────────────────────────
function(fxmetools_attach_video target)
    cmake_parse_arguments(FXMEVID "NO_CAMERA;NO_FFMPEG" "" "" ${ARGN})

    set(_has_camera 0)
    set(_has_ffmpeg 0)

    if(NOT FXMEVID_NO_CAMERA)
        if(APPLE OR WIN32)
            target_link_libraries(${target} PRIVATE juce::juce_video)
            set(_has_camera 1)
            message(STATUS "FxmeTools/image: cameras via juce::CameraDevice (juce_video)")
        elseif(UNIX)
            # V4l2CameraSource: kernel headers only, nothing to link.
            message(STATUS "FxmeTools/image: cameras via V4L2")
        endif()
    endif()

    if(NOT FXMEVID_NO_FFMPEG)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            if(NOT TARGET PkgConfig::FXME_FFMPEG)
                pkg_check_modules(FXME_FFMPEG IMPORTED_TARGET
                    libavformat libavcodec libavutil libswscale)
            endif()
            if(TARGET PkgConfig::FXME_FFMPEG)
                target_link_libraries(${target} PRIVATE PkgConfig::FXME_FFMPEG)
                set(_has_ffmpeg 1)
                message(STATUS "FxmeTools/image: video files via FFmpeg")
            endif()
        endif()
        if(NOT _has_ffmpeg)
            message(STATUS "FxmeTools/image: FFmpeg not found — video-file sources disabled "
                           "(install libavformat/libavcodec/libavutil/libswscale dev packages)")
        endif()
    endif()

    target_compile_definitions(${target} PUBLIC
        FXME_HAS_JUCE_CAMERA=${_has_camera}
        FXME_HAS_FFMPEG=${_has_ffmpeg})
endfunction()

# ──────────────────────────────────────────────────────────────────────────
# CLAP format, through free-audio/clap-juce-extensions (JUCE 9 has no CLAP
# client of its own yet).
#
#   fxmetools_add_clap(MyPlugin
#       ID       "com.fx-mechanics.MyPlugin"
#       FEATURES audio-effect stereo)
#
# Creates MyPlugin_CLAP beside the VST3/AU/Standalone targets, built by
# MyPlugin_All too, into MyPlugin_artefacts/<config>/CLAP/MyPlugin.clap.
# Call it after juce_add_plugin(MyPlugin ...). Any other argument (CLAP_MANUAL_URL,
# CLAP_SUPPORT_URL, ...) goes to clap_juce_extensions_plugin() unchanged.
#
# ID is permanent once released: hosts store it in their sessions. The
# FX-Mechanics scheme is com.fx-mechanics.<PRODUCT_NAME>, whatever the
# target's BUNDLE_ID says. It is required, not derived from the product name,
# so that renaming a product can never change it silently.
#
# FEATURES by plugin type: effect → audio-effect (+ a category such as
# reverb, delay, compressor, ...), instrument → instrument synthesizer,
# MIDI effect → note-effect; add stereo or mono for the channel layout.
#
# This file does not fetch clap-juce-extensions: the top-level project pins
# it like every other dependency (cmake/Dependencies.cmake), e.g.
#     fxme_dependency(ClapJuceExtensions
#         https://github.com/free-audio/clap-juce-extensions <sha>
#         SIBLING clap-juce-extensions)
# which sets CLAPJUCEEXTENSIONS_DIR; FetchContent brings its clap and
# clap-helpers submodules along.
# ──────────────────────────────────────────────────────────────────────────
function(fxmetools_add_clap target)
    cmake_parse_arguments(FXMECLAP "" "ID" "FEATURES" ${ARGN})
    if(NOT FXMECLAP_ID)
        message(FATAL_ERROR "fxmetools_add_clap(${target}): ID is required")
    endif()
    if(NOT FXMECLAP_FEATURES)
        message(FATAL_ERROR "fxmetools_add_clap(${target}): FEATURES is required")
    endif()

    if(NOT TARGET clap_juce_extensions)
        if(NOT CLAPJUCEEXTENSIONS_DIR)
            message(FATAL_ERROR "fxmetools_add_clap(${target}): CLAPJUCEEXTENSIONS_DIR is not set; "
                                "pin ClapJuceExtensions with fxme_dependency() first")
        endif()
        # Added once, at the top of the build tree, whichever plugin asks first.
        add_subdirectory("${CLAPJUCEEXTENSIONS_DIR}"
                         "${CMAKE_BINARY_DIR}/clap-juce-extensions" EXCLUDE_FROM_ALL)
    endif()

    clap_juce_extensions_plugin(TARGET ${target}
        CLAP_ID "${FXMECLAP_ID}"
        CLAP_FEATURES ${FXMECLAP_FEATURES}
        ${FXMECLAP_UNPARSED_ARGUMENTS})

    # clap-juce-extensions does not hook into JUCE's <target>_All, which is
    # what "build every format of this plugin" means everywhere else.
    if(TARGET ${target}_All)
        add_dependencies(${target}_All ${target}_CLAP)
    endif()
endfunction()
