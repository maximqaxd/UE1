# Unreal Engine 1 for Sega Dreamcast

An Unreal Engine 1 v200 port to Sega Dreamcast. Game assets are not included. To cook the complete single-player campaign, you need an original Unreal v200 retail installation. The v205 demo is not sufficient for the full campaign cook; other releases have not been tested.

## Dreamcast: cook assets and build a CDI

The retail `Unreal` folder must contain `System`, `Maps`, `Textures`, `Sounds`, and `Music`. Cooking writes to a separate directory; do not use an existing `gamedata` directory.

### Prebuilt release

Place `UE1-Dreamcast-2026-09-28.zip` beside the retail `Unreal` folder and extract it there. Its top-level `Unreal/` directory merges with the retail folder, adding `DCUtil.bin`, its `.so` libraries, `Profile/`, `Unreal.elf`, `IP.BIN`, and this guide directly under `Unreal/`. It does not replace the retail packages or `System/Unreal.ini`. The Dreamcast `Unreal.ini` stays in `Profile/` until the cooker copies it into the separate `gamedata` output.

From the retail folder, run:

```sh
cd Unreal
./DCUtil.bin --cook=everything --source=. --output=../gamedata
mkdcdisc -e Unreal.elf -D ../gamedata -p IP.BIN -N -o ../unreal.cdi
```

Wait for `DC cook complete: .../gamedata` before running `mkdcdisc`. A full cook can take roughly 20–25 minutes, depending on the machine. The output is `unreal.cdi` beside the retail folder. For another cook, choose a new output directory outside `Unreal` and pass that same directory to `mkdcdisc -D`.

### Build the tools and Dreamcast executable from source (Linux/WSL)

On Ubuntu/WSL, install the host build and cooking dependencies. The host cooker is a 32-bit x86 program, so it needs the i386 libraries even on a 64-bit Ubuntu installation:

```sh
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install build-essential cmake gcc-multilib g++-multilib \
  libc6-dev:i386 libsdl2-dev:i386 libgles2-mesa-dev:i386 zlib1g-dev:i386 \
  imagemagick ffmpeg unzip
ffmpeg -hide_banner -demuxers | grep libopenmpt
```

The last command must show the `libopenmpt` demuxer; a different FFmpeg build without it cannot cook the music. The cook also uses KallistiOS utilities `pvrtex` and `wav2adpcm` (normally under `/opt/toolchains/dc/kos/utils/`). For Dreamcast builds, install the SH-4 toolchain, KallistiOS, kos-ports zlib and libsh4zam, and `mkdcdisc`. KallistiOS provides `makeip` for the CDI target. Prebuilt releases still need the KallistiOS conversion utilities and `mkdcdisc`, but not the SH-4 compiler.

Before sourcing KallistiOS, check `/opt/toolchains/dc/kos/environ.sh` for the intended compiler settings:

```sh
export KOS_CFLAGS="${KOS_CFLAGS} -Os"
export KOS_SH4_PRECISION="-m4-single-only"
```

Then build the host cooker and Dreamcast executable:

```sh
source /opt/toolchains/dc/kos/environ.sh

cmake -S Source -B build_host_dcutil -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_DCUTIL=ON -DDC_RESOURCE_COOKER=ON \
  -DBUILD_UNREAL=OFF -DBUILD_NOPENALDRV=OFF
cmake --build build_host_dcutil -j"$(nproc)"

cmake -S Source -B build_dc -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$KOS_CMAKE_TOOLCHAIN" \
  -DPLATFORM_DREAMCAST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build_dc --target Unreal -j"$(nproc)"
```

This produces `build_host_dcutil/DCUtil/DCUtil.bin` and `build_dc/Unreal/Unreal.elf`. With the retail `Unreal` folder in the repository root, cook and build the disc image:

```sh
./build_host_dcutil/DCUtil/DCUtil.bin --cook=everything --source=Unreal --output=gamedata
cmake -S Source -B build_dc \
  -DDREAMCAST_BUILD_CDI=ON \
  -DDREAMCAST_GAME_DATA="$PWD/gamedata" \
  -DDREAMCAST_IP_BIN="$PWD/gamedata/IP.BIN" \
  -DDREAMCAST_CDI_OUTPUT="$PWD/unreal.cdi"
cmake --build build_dc --target cdi -j"$(nproc)"
```

The `cdi` target creates `gamedata/IP.BIN` from `Source/ip.txt` and calls `mkdcdisc` with the built ELF and cooked data. The cooker stages the Dreamcast `Unreal.ini` and other profile resources automatically; no manual configuration copy is needed.

## Note

Unreal Engine, Unreal, and related trademarks and copyrights are owned by Epic Games. This repository is not affiliated with or endorsed by Epic Games. It is based on the v200 source available elsewhere on the Internet, with game assets and third-party proprietary libraries removed. Do not use for commercial purposes.
