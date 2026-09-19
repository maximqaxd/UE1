# PVR/AICA baseline

Branch: unreal-pvr-audio-baseline; integrated into local master
Base: 6d3ce1ad8fe96322569e6f492796e400f1f80abc
Worktree: C:/Dev/Dreamcast/UE1-baseline

The original dreamcast branch at 110e004, all 19 modified source files, and untracked data/builds remain at C:/Dev/Dreamcast/UE1. Its tracked diff was saved and verified byte-for-byte unchanged. No assets were recooked or overwritten.

## Review of every commit after the base

| Original | Decision | Reason / replay |
| --- | --- | --- |
| 22f961f | Exclude | Experimental BSP narrowing and dynamic BSP/allocation changes. |
| b44f5c8 | Exclude | Array-growth changes and broken database shrinking. |
| defdb10 | Exclude | Mesh/keyframe reduction, sound conversion and related cooker changes. |
| 0b287b9 | Exclude | LightBits RLE without runtime decoder. |
| fc41244 | Keep | PVR depth/font/sprite/list fixes. |
| 4981df5 | Keep | AICA implementation. |
| 87babf8 | Keep | Enable audio and WAV parsing fixes. Includes sample-release and quality behaviour in that audio commit. |
| 1349632 | Exclude | Fixed 30000-element renderer limits. |
| 5e540d0 | Keep | DreamShell ISO build support. |
| fcb7bf8 | Keep | Translucent blending fix. |
| aa1cd19 | Exclude | Analysis/cooker infrastructure tied to experimental optimisations, plus editor/mover changes. |
| b907664 | Keep | Current-KOS compatibility. |
| 216f02f | Exclude | Lossy lightmap reduction. |
| 73862a2 | Exclude | Repack/stage pipeline invokes rejected cook passes. |
| cdf3301 | Exclude | Later profiling, available for independent reintroduction. |
| ea106d2 | Exclude | Map verification/loading order change; keep base behaviour for comparison. |
| 110e004 | Exclude | Texture report depends on later cooker infrastructure. |

No uncommitted optimisation changes were replayed. Pre-6d3ce1 changes remain, including lazy mip payloads, LOW_MEMORY behaviour, VQ tooling and the temporary thread-stack fix. This is the requested historical baseline, not an untouched PC engine or proof all earlier optimisations are correct.

## Necessary compatibility follow-up

The original b907664 patch changed AudioEngine_Play's declaration to uint8_t/uint32_t but left its definition using uint8/uint32. Current KOS failed to link AudioEngine_Play. The definition now matches the declaration, a one-line change in Source/AICADrv/AudioEngine.cpp.

The six selected commits and the compatibility follow-up are recorded with single-line dc: subjects. Local master is created from the selected base and merged with this branch; origin/master is a separate upstream line and is unchanged.

## Verification

- All six selected commits replayed without conflict.
- Core, engine layouts, mesh/BSP/mover/collision implementation, renderer and DCUtil compare exactly to 6d3ce1.
- PVR, audio configuration and UnAudio.cpp match b907664. AICA differs only by the definition fix above.
- git diff --check passes.
- Fresh SH-4 RelWithDebInfo Unreal target compiled and linked successfully using WSL KOS and GCC 15.2.0. Existing warnings remain. No hardware/emulator runtime test was performed.
- Original checkout's tracked changes are byte-for-byte unchanged from the saved patch.
- KOS wrappers kos-cc, kos-c++, kos-ar, kos-ranlib and kos-objcopy lacked execute bits; owner-execute permissions were restored. No SDK source was edited.

Build from this worktree in WSL:

```sh
source /opt/toolchains/dc/kos/environ.sh
cmake -S Source -B build_dc -G 'Unix Makefiles' \
  -DCMAKE_TOOLCHAIN_FILE=/opt/toolchains/dc/kos/utils/cmake/kallistios.toolchain.cmake \
  -DPLATFORM_DREAMCAST=ON -DDREAMCAST_BUILD_CDI=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_dc --target Unreal -j8
```

Output: build_dc/Unreal/Unreal.elf.
Logs: baseline-configure.log and baseline-build.log.

## Asset baseline

No disc image or replacement data tree was made. Use verified original v200 assets in a separate staging directory for runtime validation. Existing cooked maps/UnrealI.u cannot recover lost animation keys or lightmap samples merely by reverting source. Unreal_orig is a candidate input, not independently verified pristine.

The DCUtil inherited from 6d3ce1 has sound/music removal commands: do not run CVTUAX/CVTUMX/CVTALL on the reference assets. The later CVTUMH/CVTUNR reducers and repack/stage/analyze/texreport shell subcommands are absent.

This baseline consumes more memory than the experimental build and may still fail on Dig. Future work should measure load peaks and correctness from this controlled source/asset starting point.
