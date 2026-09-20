#!/usr/bin/env bash
# Build and run the Mibot Voice Agent host tests from WSL.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"
rm -rf build
cmake -S . -B build
cmake --build build
cd build
ctest --output-on-failure
