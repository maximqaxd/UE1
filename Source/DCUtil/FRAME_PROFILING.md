# Dreamcast frame profiler

The overlay is enabled by default on Dreamcast. `DCPROFILE` toggles collection
and drawing through the console. Host builds do not collect these timings.

The overlay shows averages over 30 engine ticks. `frame` measures
tick-start to tick-start, including time outside the engine tick. `worst` is
the largest interval in that window. `tick` measures time inside the engine
tick. Loading/travel can contribute to those values; collect a fresh window
after loading when comparing scenes.

Stages are inclusive and overlap: world contains BSP/mesh/lighting work, and
texture preparation includes its uploads. Recursive calls to the same stage
are counted only once. Do not sum the displayed columns. Game is the level
tick (including scripts and simulation). Submit is renderer Unlock, including
PT/TR replay and scene finish; direct OP submission occurs during world work.
Wait is the pvr_wait_ready call before rendering. It is synchronization time,
not a measurement of GPU rendering alone.

Upload measures synchronous pvr_txr_load calls and bytes, including atlas
blocks. The separate pvr_txr_load_ex path is not included. Header count records
cache misses in EmitHeader, including overlay headers. Overlay timing includes
CPU text formatting and submission, but does not isolate its GPU pixel cost.
The GPU line is the latest completed PVR scene, not the 30-tick average; it
overlaps CPU execution. TA KB comes from the same KOS statistics snapshot.

Collection uses fixed static storage, no heap allocations, and integer KOS
microsecond timestamps. Timers have overhead, particularly for many tiny atlas
uploads. Compare the same scene with the profiler off before treating small
differences as improvements. This profiler does not attribute every DAT read,
individual script functions, mesh decode, or audio separately, and does not
provide percentiles or hardware cache-miss counters.

The expanded view separates polygon clipping, raster setup, span copying/merging,
and dynamic setup. These stages are inclusive across all callers: do not subtract
their sum from BSP as an exact traversal time. `nodes` counts front-pass node
visits, `polys` counts ClipBspSurf calls, `pts` counts its point-cache misses,
and `spans` counts requested raster rows passed to span-copy routines (not actual
span fragments or pixels). These workload counters are averages per tick.

Texture `read` covers streamed mip reads and cooked lightmap reads. The `DT`
and `LM` fields split those two sources by average milliseconds and KiB per
tick. `read` is inside `tex`, so those times must not be added. A high read
time with few KiB suggests seek/latency, whereas a large KiB count can indicate
transfer bandwidth or too many uploads. These fields do not
cover every engine DAT reader or the legacy package-file fallback. `inflate`
covers cooked-lightmap zlib decoding, `alloc` covers AllocateTexture (including
its eviction search), and `place` covers atlas placement (including new page
allocation). `twid+SQ` includes atlas block twiddling and its uploads. `cold`
means a new texture binding and `reload` means any upload for an existing binding,
including realtime updates. `evict` counts reclaimed atlas bindings only.
`vramEv` counts private texture allocations evicted when VRAM allocation
fails; a zero atlas count does not rule out private lightmap eviction.

`DCLEGACYTIMERS` toggles the old uclock/uunclock timers, off by default on DC.
The non-DC macros are unchanged. DC no longer applies the old hardcoded 34-unit
correction. Disabling these timers zeros/invalidates legacy timing displays,
not the new profiler. Wait a full reporting window after toggling. Formatting
of the overlay is cached every 30 ticks; text is still submitted every frame.

For a slow scene, hold the camera still, allow a complete reporting window,
and record the overlay. Repeat with the same map, camera, resolution, and
assets. A large wait needs correlation with the completed GPU time. A large
world/BSP/mesh time suggests CPU rendering work. Large texture and upload
times suggest resource preparation or transfer; investigate cold versus warm
frames before changing compression or residency.

## Detailed visibility and lighting investigation

`DCPPAGE` cycles four nine-line pages: overview, visibility, lighting, mesh.
`DCPDUMP` prints the last completed 30-tick window for all pages on demand.
No per-frame serial output is added. Counts are averages per engine tick.

Controller: hold **right trigger** and tap D-pad:
right = next page, up = dump to log, down = overlay toggle, left = detail toggle.
Hold **both triggers** and tap D-pad right to cycle BSP span modes. Mode 0 is
the original path. Mode 1 is the default: it updates occlusion without allocating discarded
output fragments for ordinary hardware polygons. Mode 2 additionally bypasses
opaque BSP span tests and updates for a diagnostic A/B measurement; it can
render portals or hidden geometry incorrectly and must not be used as a
production visibility mode. The visibility page shows the current mode and
output/split/bypass counts. `DCSPANMODE 0`, `1`, or `2` also selects a mode via
the console. Switching modes clears the profiling window.
Hold **both triggers** and tap D-pad up to cycle stationary dynamic-lightmap
rates through 5, 10 and 15 Hz; 5 Hz is the default. Moving lights still
update every frame. `DCLIGHTRATE 5`, `10`, or `15` selects a rate explicitly.
The lighting page displays the selected rate. The visibility page reports
polygon-header compilation time and upload counts for unchanged versus
changed header state. In-place atlas updates should leave headers valid.
Works in gameplay and menus. Commands are press-edge driven, without repeat.
Gameplay/menu input and stick look are suppressed during the chord. Release all
buttons and both triggers to resume normal input. Right trigger alone still
fires: press trigger and D-pad together to avoid firing before the chord is
recognized. Once recognized, releasing D-pad first cannot resume firing.
Start retains its normal menu behavior outside the chord.

- Transform includes matrix setup, point-cache lookup/allocation, transformation,
  projection and outcodes in ClipBspSurf. Clip-only starts after that loop.
  Clip rejects include trivial outcode and plane-clipping rejection.
- Matrix loads count all DCLoadPipeCoords calls, including the separate point
  lookup path; transformed-point counts still cover ClipBspSurf cache misses only.
- Rows are requested scanlines, not pixels. Span links count non-null list nodes
  reached through row-head or Next reads in CopyFromRaster/CopyFromRasterUpdate.
  Output and split counters separate the two allocation types. The older
  aggregate fragment count includes both in the original routines.
  They exclude MergeWith allocations and other span-buffer construction. Work
  counts include non-BSP callers. Span rejects, however, are counted at the BSP
  call site when a polygon returns no visible coverage (polygon attempts, not
  unique surfaces; child views may visit a surface again).
- Raster rejects count SetupRaster bounding-box visibility rejection.
- Lighting reports cooked hits, static builds/hits, dynamic cache misses,
  expired timestamps, reused timestamps, and uncached merged regeneration.
  Static and dynamic counts can both increase for one surface setup.
  Static misses mean the base map was absent from the 256 KiB cache;
  invalidations mean it was present but a mover or changed light forced a rebuild.
  Cache-create time and count cover static and dynamic lightmap allocations.
  Static build timing covers regeneration including its cache allocation;
  dynamic build timing starts after its cache decision/allocation and covers
  copying the static base, illumination and merging, including merged surfaces.
- For a lighting-rate comparison, hold the camera still for two 30-tick
  windows after each `DCLIGHTRATE` change. Compare dynamic expired/hit,
  changed uploads, header compile time, and total frame time. Moving-light
  surfaces deliberately remain uncapped.
- Existing-binding upload reasons are mutually exclusive, in priority order:
  missing residency, realtime changed, palette changed, invalid palette bank.
  Cold bindings are counted separately. Lightmap uploads are a cross-cutting
  count of BGRA lightmaps and streamed cooked RGB565 uploads, not another reason.

### Measuring observer overhead

Hold the same camera and allow two complete windows after changing modes:

1. Detailed + visible overlay (default); capture `DCPDUMP`.
2. `DCPOVERLAY` hides only drawing; collection continues. Capture `DCPDUMP`.
3. `DCPDETAIL` disables detailed stage timestamps and workload counters while
   retaining game/world/BSP/wait/submit/overlay timing; capture `DCPDUMP`.
4. Repeat in reverse order to check scene variation and warming effects.

Compare frame intervals, not the sums of nested timers. Detailed versus coarse
with overlay hidden estimates the extra instrumentation cost. Coarse still has
scope calls/branches, aggregation and formatting: it is not an uninstrumented
baseline. Dumping itself is outside the saved window; let its disturbance pass
before another sample. Keep legacy timers off and console closed during capture.
Scope timer reads/frame are reported as a workload indicator, not converted into
an invented overhead estimate. Mode switches clear mixed windows. The cached
text and counter arrays are included in profiler_static memory accounting.

## Atlas implementation

The renderer's cooker first writes independent RGB565 lightmap entries with
lightmap/zone keys, dimensions, offsets, and optional zlib compression. The
`cook_lightmaps.py` post-pass packs these entries into 256x256 static pages and
encodes each page as RGB565 VQ with `pvrtex`. Version 4 `.dlm` sidecars retain
the individual entries as fallback data and add page/slot placements and full
`.DT` page images. At level load, the PVR driver uploads each static page once;
dynamic and mover lightmaps use separate writable RGB565 pages. Version 3
sidecars retain the former runtime placement and twiddled tile upload path.
