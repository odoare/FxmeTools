# Tests for FxmeNeural.
#
# The models are NAM Core's example_models (MIT, Steven Atkinson), read in
# place from the NeuralAudio tree the build uses: nothing copied here.
# Optional: FXME_NAM_MODELS=<dir> in the environment adds every .nam file of
# that directory to the benchmark (local models, whatever their licence: they
# are only read).

add_executable(FxmeNeuralTests CoreNeuralTests.cpp)
target_link_libraries(FxmeNeuralTests PRIVATE FxmeNeural)
target_compile_definitions(FxmeNeuralTests PRIVATE
    FXME_NAM_EXAMPLES="${NEURALAUDIO_DIR}/deps/NeuralAmpModelerCore/example_models")
add_test(NAME FxmeNeuralTests COMMAND FxmeNeuralTests)
