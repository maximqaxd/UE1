#!/usr/bin/env bash
set -euo pipefail

: "${KOS_BASE:?Please source your KOS environ.sh to set KOS_BASE}"

SRC_DIR="./Source"
BUILD_DIR="./build_dc"

EXTRA_FLAGS=( -DDREAMCAST_BUILD_CDI=ON )

# Now build Dreamcast target
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$KOS_CMAKE_TOOLCHAIN" \
  -DPLATFORM_DREAMCAST=ON ${EXTRA_FLAGS[@]:-} ${EXTRA_CMAKE_FLAGS:-}

cmake --build "$BUILD_DIR" -j"$(nproc)"
cmake --install "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target cdi

EMU_PATH="../flycast-x86_64.AppImage"
CDI_PATH="./unreal.cdi"

if [[ -x "$EMU_PATH" ]]; then
  "$EMU_PATH" "$CDI_PATH"
else
  echo "Flycast AppImage not found or not executable at $EMU_PATH (skipping launch)"
fi