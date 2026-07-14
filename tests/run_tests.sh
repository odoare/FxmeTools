#!/usr/bin/env bash
# Build and run the FxmeTools unit tests through the host project.
# Assumes FxmeTools is checked out as <host>/Source/libs/FxmeTools and that
# the host project exposes a TEAR_BUILD_TESTS option adding this directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
BUILD_DIR="$HOST_ROOT/build_tests"

echo "==> Configuring (TEAR_BUILD_TESTS=ON)..."
cmake -B "$BUILD_DIR" \
      -S "$HOST_ROOT" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DTEAR_BUILD_TESTS=ON \
      "${@}"   # forward any extra cmake flags, e.g. -G Ninja

echo "==> Building..."
cmake --build "$BUILD_DIR" --target FxmeToolsTests --parallel

echo "==> Running tests..."
ctest --test-dir "$BUILD_DIR" --output-on-failure -R ArpeggiatorTests
