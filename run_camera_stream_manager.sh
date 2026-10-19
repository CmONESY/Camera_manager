#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BINARY="${BUILD_DIR}/camera_stream_manager"
SRC="${SCRIPT_DIR}/camera_stream_manager.cpp"

mkdir -p "${BUILD_DIR}"
g++ -std=c++17 -O2 -Wall -Wextra -pedantic "${SRC}" -o "${BINARY}"
exec "${BINARY}" "$@"
