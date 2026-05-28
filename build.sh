#!/usr/bin/env bash
set -euo pipefail

: "${KOS_BASE:?Please source your KOS environ.sh to set KOS_BASE}"

SRC_DIR="./Source"
BUILD_DIR="./build_dc"
BUILD_DIR_HOST="./build_host_dcutil"
GAMEDATA_DIR="./gamedata"
DEST_SYSTEM_DIR="$GAMEDATA_DIR/System"
UNREAL_DIR="./Unreal"

EXTRA_FLAGS=( -DDREAMCAST_BUILD_CDI=ON )

# ── Build DCUtil (host x86 tool) ──────────────────────────────────────────────

build_dcutil() {
  echo "=== Building DCUtil (host) ==="
  cmake -S "$SRC_DIR" -B "$BUILD_DIR_HOST" \
    -G "Unix Makefiles" \
    -DBUILD_DCUTIL=ON \
    -DBUILD_EDITOR=ON \
    -DBUILD_UNREAL=OFF \
    -DBUILD_WINDRV=OFF \
    -DBUILD_NOPENALDRV=OFF \
    -DBUILD_NULLSOUNDDRV=ON \
    -DUSE_SDL=ON \
    -DCMAKE_C_FLAGS=-m32 \
    -DCMAKE_CXX_FLAGS=-m32

  cmake --build "$BUILD_DIR_HOST" --target install -j"$(nproc)"

  mkdir -p "$DEST_SYSTEM_DIR"

  # Find where cmake installed the files (varies by config)
  HOST_OUTPUT=""
  for candidate in "$BUILD_DIR_HOST" "$BUILD_DIR_HOST/RelWithDebInfo" "$BUILD_DIR_HOST/Release" "$BUILD_DIR_HOST/Debug"; do
    if [[ -f "$candidate/DCUtil.bin" ]]; then
      HOST_OUTPUT="$candidate"
      break
    fi
  done

  if [[ -z "$HOST_OUTPUT" ]]; then
    echo "Error: DCUtil.bin not found in build output"
    exit 1
  fi

  cp "$HOST_OUTPUT"/DCUtil.bin "$DEST_SYSTEM_DIR/"
  shopt -s nullglob
  for so_file in "$HOST_OUTPUT"/*.so; do
    cp "$so_file" "$DEST_SYSTEM_DIR/"
  done
  shopt -u nullglob
  echo "DCUtil built: $DEST_SYSTEM_DIR/DCUtil.bin (from $HOST_OUTPUT)"
}

run_dcutil() {
  local ARGS="$1"
  local DCUTIL="$DEST_SYSTEM_DIR/DCUtil.bin"
  if [[ ! -f "$DCUTIL" ]]; then
    echo "DCUtil not found at $DCUTIL — building first..."
    build_dcutil
  fi
  # Copy DCUtil + .so libs into Unreal/System so ini Paths[] and .u packages resolve naturally
  cp "$DCUTIL" "$UNREAL_DIR/System/"
  for so_file in "$DEST_SYSTEM_DIR"/*.so; do
    cp "$so_file" "$UNREAL_DIR/System/"
  done
  echo "=== Running DCUtil: $ARGS ==="
  pushd "$UNREAL_DIR/System" > /dev/null
  LD_LIBRARY_PATH=. ./DCUtil.bin "$ARGS"
  popd > /dev/null
}

# ── Command dispatch ──────────────────────────────────────────────────────────

case "${1:-}" in
  analyze)
    build_dcutil
    run_dcutil "ANALYZE=../Maps/*.unr"
    exit 0
    ;;
  dcutil)
    build_dcutil
    exit 0
    ;;
  dcutil-run)
    shift
    run_dcutil "$*"
    exit 0
    ;;
  help)
    echo "Usage: ./build.sh [command]"
    echo ""
    echo "Commands:"
    echo "  (no args)   Build DCUtil + Dreamcast target + CDI/ISO"
    echo "  analyze     Build DCUtil, then analyze all maps for SWORD limits"
    echo "  dcutil      Build DCUtil only"
    echo "  dcutil-run  Build & run DCUtil with custom args, e.g.:"
    echo "              ./build.sh dcutil-run 'CVTUTX=../../Unreal/Textures/*.utx'"
    echo "  help        Show this help"
    exit 0
    ;;
esac

build_dcutil

# ── Build Dreamcast target ────────────────────────────────────────────────────
echo "=== Building Dreamcast target ==="
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$KOS_CMAKE_TOOLCHAIN" \
  -DPLATFORM_DREAMCAST=ON ${EXTRA_FLAGS[@]:-} ${EXTRA_CMAKE_FLAGS:-}

cmake --build "$BUILD_DIR" -j"$(nproc)"
cmake --install "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target cdi

# Create ISO target
echo "Creating ISO image..."

# Find the ELF file (check common locations)
ELF_FILE=""
if [[ -f "$BUILD_DIR/RelWithDebInfo/Unreal.elf" ]]; then
  ELF_FILE="$BUILD_DIR/RelWithDebInfo/Unreal.elf"
elif [[ -f "$BUILD_DIR/Unreal/Unreal.elf" ]]; then
  ELF_FILE="$BUILD_DIR/Unreal/Unreal.elf"
elif [[ -f "$BUILD_DIR/Release/Unreal.elf" ]]; then
  ELF_FILE="$BUILD_DIR/Release/Unreal.elf"
elif [[ -f "$BUILD_DIR/Debug/Unreal.elf" ]]; then
  ELF_FILE="$BUILD_DIR/Debug/Unreal.elf"
else
  # Try to find it
  ELF_FILE=$(find "$BUILD_DIR" -name "Unreal.elf" -type f | head -1)
fi

if [[ -z "$ELF_FILE" || ! -f "$ELF_FILE" ]]; then
  echo "Error: Could not find Unreal.elf in build directory"
  exit 1
fi

echo "Found ELF: $ELF_FILE"

# Create 1ST_READ.BIN from ELF
mkdir -p "$GAMEDATA_DIR"

echo "Creating 1ST_READ.BIN from $ELF_FILE..."
kos-objcopy -R .stack -O binary "$ELF_FILE" "$GAMEDATA_DIR/1ST_READ.BIN"

if [[ ! -f "$GAMEDATA_DIR/1ST_READ.BIN" ]]; then
  echo "Error: Failed to create 1ST_READ.BIN"
  exit 1
fi

echo "Created 1ST_READ.BIN ($(du -h "$GAMEDATA_DIR/1ST_READ.BIN" | cut -f1))"

# Create ISO
ISO_FILE="./Unreal.iso"
if [[ ! -f "$GAMEDATA_DIR/IP.BIN" ]]; then
  echo "Warning: IP.BIN not found in gamedata directory"
fi

echo "Creating ISO: $ISO_FILE..."
mkisofs -V Unreal -G "$GAMEDATA_DIR/IP.BIN" -r -J -l -o "$ISO_FILE" "$GAMEDATA_DIR"

if [[ -f "$ISO_FILE" ]]; then
  echo "Created ISO: $ISO_FILE ($(du -h "$ISO_FILE" | cut -f1))"
else
  echo "Error: Failed to create ISO"
  exit 1
fi

EMU_PATH="../flycast-x86_64.AppImage"
CDI_PATH="./unreal.cdi"

if [[ -x "$EMU_PATH" ]]; then
  "$EMU_PATH" "$CDI_PATH"
else
  echo "Flycast AppImage not found or not executable at $EMU_PATH (skipping launch)"
fi