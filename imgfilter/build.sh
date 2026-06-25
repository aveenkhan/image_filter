#!/usr/bin/env bash
# build.sh -- compiles the C++ pthreads backend.
# Usage: ./build.sh
set -e
cd "$(dirname "$0")/src"
g++ -O2 -Wall -pthread -o filter_engine filter_engine.cpp
echo "Built: src/filter_engine"
