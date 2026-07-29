# ──────────────────────────────────────────────────────────────────────────
# FxmeTools.cmake
#
# Registers the FxmeTools JUCE module and provides:
#
#   fxmetools_attach(<target>)
#       links the module and compiles + exposes the WDL convolution engine
#       (needed by <FxmeTools/dsp/FirFilter.h>).
#
#   fxmetools_attach_video(<target> [NO_CAMERA] [NO_FFMPEG])
#       enables the optional backends of FxmeTools/image (see its README):
#       webcams (juce_video on Windows/macOS, V4L2 on Linux) and video-file
#       decoding (FFmpeg via pkg-config). Both degrade gracefully when
#       unavailable, so calling it is safe on any machine.
#
# Usage from a consumer project (after add_subdirectory(JUCE)):
#     include(${CMAKE_CURRENT_SOURCE_DIR}/lib/FxmeTools/cmake/FxmeTools.cmake)
#     ...
#     fxmetools_attach(MyPlugin)
#     fxmetools_attach_video(MyPlugin)     # only if the plugin uses image input
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
