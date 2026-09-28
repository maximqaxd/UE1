# DCUtil

## Dreamcast resource pipeline

The host cooker runs under Linux/WSL. Build with `DC_RESOURCE_COOKER` enabled.
It uses `pvrtex`, `wav2adpcm`, `ffmpeg` with libopenmpt, and ImageMagick `convert`;
Python is not used by the native pipeline.

```sh
./DCUtil.bin --cook=everything --source=/games/Unreal --output=/build/gamedata
```

The command splits the seven large campaign maps, converts package resources,
cooks meshes and BSP data, packs static/dynamic lightmaps, records and verifies
streams and save baselines, renders music, and stages the single-player resources.
Replaced original maps and multiplayer streams are not published.
The console names each input and output, reports resource and atlas counts, and
prints elapsed time while a worker or encoder runs without a progress marker.

Output must be a new directory. Intermediate packages and logs stay in
`<output>.cook`; the final directory is published only after all checks pass.
`--work=PATH` overrides the intermediate directory. `--resume` verifies the input,
tool and completed-output fingerprints before reusing work. Changed inputs require
a new work directory. A resumed run keeps its preserved engine worker executable;
use a fresh directory when changing engine cooking behavior. Concurrent writers to
one work directory are rejected.

`--resume --reimport` repeats package assembly using the work tree's existing
encoded media, then rebuilds and verifies maps. It does not accept changed source
assets or encoder settings; those require a fresh work tree.

`--plan` validates inputs and prints the stages without cooking. `--map=NAME`
limits map processing to one campaign map plus Entry for testing; it is not a
complete campaign build. `--until=splits|resources|maps|music` stops before
publication. `--boot=1ST_READ.BIN` optionally copies the separately built runtime;
DCUtil does not compile the engine or create the CDI.

The default profile is copied beside the executable by CMake. `--profile=PATH`
selects another profile; `--build=PATH` selects the host build containing native
libraries. Encoder paths can be overridden with `--pvrtex`, `--wav2adpcm`,
`--ffmpeg` and `--convert` (all use `--option=value`).

## Stream packing

```text
DCUtil --pack-stream session.raw Map.dcs /path/to/cooked/System
```

Packs an existing cook-session trace into a DCS3 stream without Python. The
trace must have matching `.manifest.tsv` and `.indices.tsv` sidecars and refer
to packages in the cooked tree. Output must not already exist.

This lower-level command only assembles an existing trace; the full pipeline
records and verifies traces itself.

The standalone packer test can be built without engine libraries:

```sh
c++ -std=c++17 -O2 test_cook_stream.cpp -o test_cook_stream
./test_cook_stream
```
