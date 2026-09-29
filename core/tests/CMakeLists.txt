# Tests for the JUCE-free half of FxmeTools.
#
# These deliberately link ONLY FxmeCore. If a test ever needs JUCE to build,
# the code under test belongs on the other side of the split.

# ── 1. Behaviour of the core replacements for the JUCE helpers ──────────────
add_executable(FxmeCoreTests CoreUtilTests.cpp)
target_link_libraries(FxmeCoreTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreTests COMMAND FxmeCoreTests)

# ── 2. The math layer: storage, ordering, factorisation, eigensolver ───────
add_executable(FxmeCoreMathTests CoreMathTests.cpp)
target_link_libraries(FxmeCoreMathTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreMathTests COMMAND FxmeCoreMathTests)

# ── 3. Polygon helpers in acoustics/ ───────────────────────────────────────
add_executable(FxmeCoreGeometryTests CoreGeometryTests.cpp)
target_link_libraries(FxmeCoreGeometryTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreGeometryTests COMMAND FxmeCoreGeometryTests)

# ── 4. The spectral band splitter's level gating (gate, ceiling, knee) ─────
add_executable(FxmeCoreSplitterTests CoreSplitterTests.cpp)
target_link_libraries(FxmeCoreSplitterTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreSplitterTests COMMAND FxmeCoreSplitterTests)

# ── 5. Pitch, freeze and blur inside the splitter (SpectralBandEffects) ────
add_executable(FxmeCoreSpectralEffectsTests CoreSpectralEffectsTests.cpp)
target_link_libraries(FxmeCoreSpectralEffectsTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreSpectralEffectsTests COMMAND FxmeCoreSpectralEffectsTests)

# ── 6. A band-pass following moving edges (EdgeBandPass) ───────────────────
add_executable(FxmeCoreEdgeBandPassTests CoreEdgeBandPassTests.cpp)
target_link_libraries(FxmeCoreEdgeBandPassTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreEdgeBandPassTests COMMAND FxmeCoreEdgeBandPassTests)

# ── 6b. Automatic gain compensation (LevelMatcher, behind Saturator) ──────
add_executable(FxmeCoreLevelMatcherTests CoreLevelMatcherTests.cpp)
target_link_libraries(FxmeCoreLevelMatcherTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreLevelMatcherTests COMMAND FxmeCoreLevelMatcherTests)

# ── 6c. The live engine: filter bank and band gate ────────────────────────
add_executable(FxmeCoreFilterBankTests CoreFilterBankTests.cpp)
target_link_libraries(FxmeCoreFilterBankTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreFilterBankTests COMMAND FxmeCoreFilterBankTests)

# ── 6d. Inserts in a delay loop, and the pitch shifter for them ──────────
add_executable(FxmeCoreDelayInsertTests CoreDelayInsertTests.cpp)
target_link_libraries(FxmeCoreDelayInsertTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreDelayInsertTests COMMAND FxmeCoreDelayInsertTests)

# ── 6e. Stereo input through the splitters and the spectral effects ───────
add_executable(FxmeCoreStereoBandsTests CoreStereoBandsTests.cpp)
target_link_libraries(FxmeCoreStereoBandsTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreStereoBandsTests COMMAND FxmeCoreStereoBandsTests)

# ── 6f. Band dynamics: expander / compressor curves, both splitters ─────────
add_executable(FxmeCoreDynamicsTests CoreDynamicsTests.cpp)
target_link_libraries(FxmeCoreDynamicsTests PRIVATE FxmeCore)
add_test(NAME FxmeCoreDynamicsTests COMMAND FxmeCoreDynamicsTests)

# ── 7. Every core header is self-contained without JUCE ────────────────────
add_executable(FxmeCoreHeaderTest CoreHeaderCompileTest.cpp)
target_link_libraries(FxmeCoreHeaderTest PRIVATE FxmeCore)
add_test(NAME FxmeCoreHeaderTest COMMAND FxmeCoreHeaderTest)

# The test's includes are __has_include-guarded, so a header migrating into
# core/ changes what this TU compiles without changing the TU itself — and the
# compiler's depfile only lists headers that were included *last* time. Left
# alone, the first build after a migration relinks a stale object and reports
# success without ever having compiled the new header. Depending explicitly on
# every core header closes that hole; CONFIGURE_DEPENDS re-globs when the
# directory changes, so a migration still needs no CMake edit.
file(GLOB_RECURSE FXMECORE_HEADERS CONFIGURE_DEPENDS
     "${FXMECORE_ROOT}/FxmeTools/*.h")
set_property(SOURCE CoreHeaderCompileTest.cpp
             PROPERTY OBJECT_DEPENDS "${FXMECORE_HEADERS}")

# SpectralFreeze.h sits in core but needs the WDL real FFT. Compile it into the
# header test only when the WDL submodule is actually checked out, so a fresh
# clone without submodules still builds and tests the rest.
if(TARGET FxmeCoreWdlFft)
    target_compile_definitions(FxmeCoreHeaderTest PRIVATE FXME_CORE_HAS_WDL=1)
    target_link_libraries(FxmeCoreHeaderTest PRIVATE FxmeCoreWdlFft)

    # Lets the RealFft tests insist the WDL backend is really in use, rather
    # than passing on the scalar fallback without anyone noticing.
    target_compile_definitions(FxmeCoreTests PRIVATE FXME_CORE_HAS_WDL=1)
else()
    message(STATUS "FxmeCore: SpectralFreeze.h excluded from the header test (no WDL)")
endif()

foreach(_t FxmeCoreTests FxmeCoreMathTests FxmeCoreGeometryTests FxmeCoreSplitterTests
           FxmeCoreSpectralEffectsTests FxmeCoreEdgeBandPassTests FxmeCoreLevelMatcherTests FxmeCoreFilterBankTests FxmeCoreDelayInsertTests FxmeCoreStereoBandsTests FxmeCoreDynamicsTests FxmeCoreHeaderTest)
    target_compile_options(${_t} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
endforeach()
