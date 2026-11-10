#!/usr/bin/env bash
# latch build. Deps (Fedora): cmake gcc-c++.  (Ubuntu: cmake g++.)
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j"$(nproc)"
echo ""
echo "Built: ./build/liblatch.a  ./build/latch_selftest   Install: ./install.sh"
