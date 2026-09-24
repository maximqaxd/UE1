#include "EnginePrivate.h"
#include "UnDCFrameProfile.h"

#if defined(PLATFORM_DREAMCAST)
#include <malloc.h>
#include <arch/timer.h>

// Inclusive stage timers: recursion counts once, nested stages overlap.
ENGINE_API UBOOL GDCFrameProfileEnabled = 1;
ENGINE_API UBOOL GDCFrameProfileDetailed = 1;
ENGINE_API UBOOL GDCFrameProfileOverlay = 1;
ENGINE_API INT GDCFrameProfilePage = 0;
ENGINE_API INT GDCSpanMode = 1;
ENGINE_API INT GDCStationaryLightHz = 5;
ENGINE_API UBOOL GDCMeshOIXActive = 0;
// Use the integer KOS timer directly; single-only floating point loses
// microsecond precision when an absolute timestamp is converted to DOUBLE.
static struct FDCFrameProfile
{
	UBOOL Active;
	DWORD Begin, PreviousBegin, Interval;
	DWORD Start[DCFS_Count], Depth[DCFS_Count], Time[DCFS_Count];
	QWORD Sum[DCFS_Count], IntervalSum, WorkSum, BytesSum, HeadersSum;
	DWORD Frames, Worst, Bytes, Headers;
	FLOAT Display[DCFS_Count], FrameMS, WorkMS, WorstMS, UploadKB, HeaderCount;
	DWORD GPUFrame, GPUVertexBytes;
	FLOAT GPUMS;
	DWORD Counts[DCFC_Count];
	QWORD CountSum[DCFC_Count];
	FLOAT CountDisplay[DCFC_Count];
	DWORD TimerReads;
	QWORD TimerReadsSum;
	char Lines[36][128];
} GDCFrame;

ENGINE_API void DCFrameProfileReset()
{
	appMemset(&GDCFrame, 0, sizeof(GDCFrame));
}

ENGINE_API void DCFrameProfileReport( FOutputDevice* Out )
{
	Out->Logf("DCPROFILE detail=%d overlay=%d legacy=%d TA=SQ mesh_OIX=%d; completed 30-tick window",
		GDCFrameProfileDetailed, GDCFrameProfileOverlay, GDCLegacyTimers,
		GDCMeshOIXActive);
	for( INT i = 0; i < 36; ++i )
		if( GDCFrame.Lines[i][0] ) Out->Log(GDCFrame.Lines[i]);
}

static UBOOL DCStageEnabled( INT Stage )
{
	return GDCFrameProfileDetailed || Stage == DCFS_Game || Stage == DCFS_World
		|| Stage == DCFS_BSP || Stage == DCFS_Wait || Stage == DCFS_Submit
		|| Stage == DCFS_Overlay;
}

ENGINE_API void DCFrameBegin()
{
	if( !GDCFrameProfileEnabled )
	{
		appMemset(&GDCFrame, 0, sizeof(GDCFrame));
		return;
	}
	const DWORD Now = (DWORD)timer_us_gettime64();
	GDCFrame.Interval = GDCFrame.PreviousBegin ? Now - GDCFrame.PreviousBegin : 0;
	GDCFrame.PreviousBegin = Now;
	GDCFrame.Begin = Now;
	appMemset(GDCFrame.Time, 0, sizeof(GDCFrame.Time));
	appMemset(GDCFrame.Depth, 0, sizeof(GDCFrame.Depth));
	GDCFrame.Bytes = GDCFrame.Headers = 0;
	GDCFrame.TimerReads = 0;
	appMemset(GDCFrame.Counts, 0, sizeof(GDCFrame.Counts));
	GDCFrame.Active = 1;
}

ENGINE_API void DCFrameEnter( INT Stage )
{
	if( GDCFrame.Active && DCStageEnabled(Stage) && GDCFrame.Depth[Stage]++ == 0 )
	{
		++GDCFrame.TimerReads;
		GDCFrame.Start[Stage] = (DWORD)timer_us_gettime64();
	}
}

ENGINE_API void DCFrameLeave( INT Stage )
{
	if( GDCFrame.Active && GDCFrame.Depth[Stage] && --GDCFrame.Depth[Stage] == 0 )
	{
		++GDCFrame.TimerReads;
		GDCFrame.Time[Stage] += (DWORD)timer_us_gettime64() - GDCFrame.Start[Stage];
	}
}

ENGINE_API void DCFrameUploadBytes( DWORD Bytes ) { if( GDCFrame.Active ) GDCFrame.Bytes += Bytes; }
ENGINE_API void DCFrameHeader() { if( GDCFrame.Active ) ++GDCFrame.Headers; }
ENGINE_API void DCFrameCount( INT Counter, DWORD Amount )
{
	if( GDCFrame.Active && GDCFrameProfileDetailed ) GDCFrame.Counts[Counter] += Amount;
}

ENGINE_API void DCFrameGPU( DWORD Frame, QWORD Nanoseconds, DWORD VertexBytes )
{
	if( Frame != GDCFrame.GPUFrame )
	{
		GDCFrame.GPUFrame = Frame;
		GDCFrame.GPUMS = Nanoseconds / 1000000.f;
		GDCFrame.GPUVertexBytes = VertexBytes;
	}
}

ENGINE_API void DCFrameEnd()
{
	if( !GDCFrame.Active ) return;
	GDCFrame.Active = 0;
	const DWORD Work = (DWORD)timer_us_gettime64() - GDCFrame.Begin;
	for( INT i = 0; i < DCFS_Count; ++i ) GDCFrame.Sum[i] += GDCFrame.Time[i];
	GDCFrame.WorkSum += Work;
	GDCFrame.IntervalSum += GDCFrame.Interval;
	GDCFrame.Worst = Max(GDCFrame.Worst, GDCFrame.Interval);
	GDCFrame.BytesSum += GDCFrame.Bytes;
	GDCFrame.HeadersSum += GDCFrame.Headers;
	GDCFrame.TimerReadsSum += GDCFrame.TimerReads;
	for( INT i = 0; i < DCFC_Count; ++i ) GDCFrame.CountSum[i] += GDCFrame.Counts[i];
	if( ++GDCFrame.Frames >= 30 )
	{
		const FLOAT Scale = 1.f / (1000.f * GDCFrame.Frames);
		for( INT i = 0; i < DCFS_Count; ++i )
		{
			GDCFrame.Display[i] = GDCFrame.Sum[i] * Scale;
			GDCFrame.Sum[i] = 0;
		}
		GDCFrame.FrameMS = GDCFrame.IntervalSum * Scale;
		GDCFrame.WorkMS = GDCFrame.WorkSum * Scale;
		GDCFrame.WorstMS = GDCFrame.Worst * 0.001f;
		GDCFrame.UploadKB = GDCFrame.BytesSum / (1024.f * GDCFrame.Frames);
		GDCFrame.HeaderCount = GDCFrame.HeadersSum / (FLOAT)GDCFrame.Frames;
		for( INT i = 0; i < DCFC_Count; ++i )
		{
			GDCFrame.CountDisplay[i] = GDCFrame.CountSum[i] / (FLOAT)GDCFrame.Frames;
			GDCFrame.CountSum[i] = 0;
		}
		const FLOAT* T = GDCFrame.Display;
		const FLOAT* C = GDCFrame.CountDisplay;
		appSprintf(GDCFrame.Lines[0], "DC %.1f FPS frame %.1f worst %.1f D%d TA:SQ M-OIX:%s",
			GDCFrame.FrameMS > 0 ? 1000.f/GDCFrame.FrameMS : 0.f,
			GDCFrame.FrameMS, GDCFrame.WorstMS, GDCFrameProfileDetailed,
			GDCMeshOIXActive ? "ON" : "OFF");
		appSprintf(GDCFrame.Lines[1], "tick %.1f game %.1f world %.1f wait %.1f", GDCFrame.WorkMS, T[DCFS_Game], T[DCFS_World], T[DCFS_Wait]);
		appSprintf(GDCFrame.Lines[2], "BSP %.1f clip %.1f raster %.1f span %.1f", T[DCFS_BSP], T[DCFS_Clip], T[DCFS_Raster], T[DCFS_Span]);
		appSprintf(GDCFrame.Lines[3], "dyn %.1f mesh %.1f light %.1f legacy %d", T[DCFS_Dynamics], T[DCFS_Mesh], T[DCFS_Light], GDCLegacyTimers);
		appSprintf(GDCFrame.Lines[4], "nodes %.0f polys %.0f pts %.0f rows %.0f", C[DCFC_Nodes], C[DCFC_Polys], C[DCFC_Points], C[DCFC_Spans]);
		appSprintf(GDCFrame.Lines[5], "tex %.1f read %.1f DT %.1f/%.1fK LM %.1f/%.1fK",
			T[DCFS_Texture], T[DCFS_Read],
			T[DCFS_ReadDT], C[DCFC_ReadDTBytes] / 1024.f,
			T[DCFS_ReadLightmap], C[DCFC_ReadLightmapBytes] / 1024.f);
		appSprintf(GDCFrame.Lines[6], "place %.1f twid+SQ %.1f SQ %.1f %.1fKB", T[DCFS_Place], T[DCFS_Twiddle], T[DCFS_Upload], GDCFrame.UploadKB);
		appSprintf(GDCFrame.Lines[7], "cold %.0f reload %.0f atlasEv %.0f vramEv %.0f hdr %.0f",
			C[DCFC_Cold], C[DCFC_Reload], C[DCFC_Evict], C[DCFC_VRAMEvict], GDCFrame.HeaderCount);
		appSprintf(GDCFrame.Lines[8], "submit %.1f overlay %.2f GPUlast %.1f", T[DCFS_Submit], T[DCFS_Overlay], GDCFrame.GPUMS);
		appSprintf(GDCFrame.Lines[9], "VIS frame %.1f detail %d spanmode %d", GDCFrame.FrameMS, GDCFrameProfileDetailed, GDCSpanMode);
		appSprintf(GDCFrame.Lines[10], "transform %.2f clip-only %.2f", T[DCFS_Transform], T[DCFS_Clip]);
		appSprintf(GDCFrame.Lines[11], "raster %.2f span %.2f", T[DCFS_Raster], T[DCFS_Span]);
		appSprintf(GDCFrame.Lines[12], "matrix loads %.0f transformed pts %.0f", C[DCFC_MatrixLoads], C[DCFC_Points]);
		appSprintf(GDCFrame.Lines[13], "span links %.0f out %.0f split %.0f bypass %.0f",
			C[DCFC_SpanLinks], C[DCFC_SpanOutputs], C[DCFC_SpanScreenSplits], C[DCFC_SpanBypassed]);
		appSprintf(GDCFrame.Lines[14], "reject clip %.0f raster %.0f span %.0f", C[DCFC_ClipReject], C[DCFC_RasterReject], C[DCFC_SpanReject]);
		appSprintf(GDCFrame.Lines[15], "scope timer reads/frame %.0f", GDCFrame.TimerReadsSum / (FLOAT)GDCFrame.Frames);
		appSprintf(GDCFrame.Lines[16], "hdr %.0f compile %.2fms stable %.0f addr %.0f state %.0f",
			GDCFrame.HeaderCount, T[DCFS_HeaderCompile],
			C[DCFC_HeaderUploadStable], C[DCFC_HeaderUploadAddress],
			C[DCFC_HeaderUploadState]);
		appSprintf(GDCFrame.Lines[17], "DCPDETAIL / DCPOVERLAY / DCPDUMP");
		appSprintf(GDCFrame.Lines[18], "LIGHT frame %.1f setup %.2f tex %.2f rate %dHz", GDCFrame.FrameMS, T[DCFS_Light], T[DCFS_Texture], GDCStationaryLightHz);
		appSprintf(GDCFrame.Lines[19], "cooked %.0f static build %.0f hit %.0f", C[DCFC_LightCooked], C[DCFC_LightStaticBuild], C[DCFC_LightStaticHit]);
		appSprintf(GDCFrame.Lines[20], "dynamic miss %.0f expired %.0f hit %.0f", C[DCFC_LightDynamicMiss], C[DCFC_LightDynamicExpired], C[DCFC_LightDynamicHit]);
		appSprintf(GDCFrame.Lines[21], "merged regen %.0f lightmap uploads %.0f", C[DCFC_LightMerged], C[DCFC_UploadLightmap]);
		appSprintf(GDCFrame.Lines[22], "upload cold %.0f missing %.0f", C[DCFC_Cold], C[DCFC_UploadMissing]);
		appSprintf(GDCFrame.Lines[23], "upload changed %.0f palette %.0f bank %.0f", C[DCFC_UploadChanged], C[DCFC_UploadPalette], C[DCFC_UploadBank]);
		appSprintf(GDCFrame.Lines[24], "reload %.0f atlasEv %.0f vramEv %.0f DT %.2f LM %.2f",
			C[DCFC_Reload], C[DCFC_Evict], C[DCFC_VRAMEvict],
			T[DCFS_ReadDT], T[DCFS_ReadLightmap]);
		appSprintf(GDCFrame.Lines[25], "build ms static %.2f dynamic %.2f", T[DCFS_LightStaticBuild], T[DCFS_LightDynamicBuild]);
		appSprintf(GDCFrame.Lines[26], "static miss %.0f invalid %.0f cache create %.0f/%.2fms",
			C[DCFC_LightStaticMiss], C[DCFC_LightStaticInvalidated],
			C[DCFC_LightCacheCreate], T[DCFS_LightCacheCreate]);
		appSprintf(GDCFrame.Lines[27], "MESH total %.2f frame %.2f outcode %.2f", T[DCFS_Mesh], T[DCFS_MeshFrame], T[DCFS_MeshOutcode]);
		appSprintf(GDCFrame.Lines[28], "prepare %.2f texture info %.2f", T[DCFS_MeshPrepare], T[DCFS_MeshTextureInfo]);
		appSprintf(GDCFrame.Lines[29], "light setup %.2f vertex %.2f", T[DCFS_MeshLightSetup], T[DCFS_MeshVertexLight]);
		appSprintf(GDCFrame.Lines[30], "draw+submit %.2f unmeasured %.2f", T[DCFS_MeshDraw],
			Max(0.f, T[DCFS_Mesh] - T[DCFS_MeshFrame] - T[DCFS_MeshOutcode]
				- T[DCFS_MeshPrepare] - T[DCFS_MeshTextureInfo]
				- T[DCFS_MeshLightSetup] - T[DCFS_MeshVertexLight] - T[DCFS_MeshDraw]));
		appSprintf(GDCFrame.Lines[31], "actors %.0f verts %.0f tris %.0f", C[DCFC_MeshActors], C[DCFC_MeshVerts], C[DCFC_MeshTris]);
		appSprintf(GDCFrame.Lines[32], "visible %.0f strip %.0f fallback %.0f", C[DCFC_MeshVisible], C[DCFC_MeshStripTris], C[DCFC_MeshFallbackTris]);
		appSprintf(GDCFrame.Lines[33], "strip coverage %.0f%% of drawn tris",
			100.f * C[DCFC_MeshStripTris] / Max(1.f, C[DCFC_MeshStripTris] + C[DCFC_MeshFallbackTris]));
		appSprintf(GDCFrame.Lines[34], "frame includes decode+interpolate+transform");
		appSprintf(GDCFrame.Lines[35], "draw includes clip+header+TA submission");
		GDCFrame.TimerReadsSum = 0;
		GDCFrame.IntervalSum = GDCFrame.WorkSum = GDCFrame.BytesSum = GDCFrame.HeadersSum = 0;
		GDCFrame.Frames = GDCFrame.Worst = 0;
	}
}

ENGINE_API void DCFrameDraw( UCanvas* Canvas )
{
	if( !GDCFrameProfileEnabled || !GDCFrameProfileOverlay || !Canvas || !Canvas->SmallFont ) return;
	DC_FRAME_SCOPE(DCFS_Overlay);
	for( INT i = 0; i < 9; ++i )
		Canvas->Printf(Canvas->SmallFont, 4, 24 + i * 10, "%s", GDCFrame.Lines[GDCFrameProfilePage * 9 + i]);
}

extern "C" DWORD PVR_GetVRAMUsed();
extern "C" DWORD AICA_GetSampleBytes();
extern CORE_API void appDCProfileObjects( const char* Phase, UBOOL Detailed );
extern CORE_API void appDCVRAMStats( DWORD& OutUsed, DWORD& OutPeak, INT& OutBlocks );

static DWORD DCSampledArrayRounding = 0;
static DWORD DCSampledArrayBlocks = 0;

template<class T> static void DCArrayBytes( TArray<T>& Array, DWORD& Used, DWORD& Capacity )
{
	Used += Array.Num() * sizeof(T);
	Capacity += Array.ArrayMax * sizeof(T);
	if( Array.GetData() )
	{
		DWORD Requested = Array.ArrayMax * sizeof(T);
		DWORD Usable = malloc_usable_size(Array.GetData());
		if( Usable >= Requested )
			DCSampledArrayRounding += Usable - Requested;
		++DCSampledArrayBlocks;
	}
}

ENGINE_API void DCProfileMemory( const char* Phase )
{
	static DWORD SampledHeapPeak = 0;
	struct mallinfo Heap = mallinfo();
	SampledHeapPeak = Max( SampledHeapPeak, (DWORD)Heap.uordblks );

	DWORD Models = 0;
	DWORD Bsp = 0;
	DWORD BspCapacity = 0;
	DWORD Lighting = 0;
	DWORD LightingStreamed = 0;
	DWORD Meshes = 0;
	DWORD MeshStreamed = 0;
	DWORD Textures = 0;
	DWORD Sounds = 0;
	DWORD DecodeCalls = 0;
	DWORD DecodeBytes = 0;
	DCSampledArrayRounding = DCSampledArrayBlocks = 0;
	DWORD AuxUsed = 0, AuxCapacity = 0;
	DWORD TextureUsed = 0, TextureCapacity = 0;
	DWORD SoundUsed = 0, SoundCapacity = 0;
	DWORD MeshCapacity = 0;
	DWORD MeshFullUsed = 0;
	DWORD VfHashBytes = 0;
	INT VfHashCount = 0;
	for( TObjectIterator<UState> It; It; ++It )
	{
		if( It->VfHash )
		{
			VfHashBytes += UField::HASH_COUNT * sizeof(UField*);
			++VfHashCount;
		}
	}

	for( TObjectIterator<UModel> It; It; ++It )
	{
		Bsp += It->Nodes ? It->Nodes->Num() * sizeof(FBspNode) : 0;
		Bsp += It->Surfs ? It->Surfs->Num() * sizeof(FBspSurf) : 0;
		Bsp += It->Verts ? It->Verts->Num() * sizeof(FVert) : 0;
		Bsp += It->Points ? It->Points->Num() * sizeof(FVector) : 0;
		Bsp += It->Vectors ? It->Vectors->Num() * sizeof(FVector) : 0;
		BspCapacity += It->Nodes ? It->Nodes->Max() * sizeof(FBspNode) : 0;
		BspCapacity += It->Surfs ? It->Surfs->Max() * sizeof(FBspSurf) : 0;
		BspCapacity += It->Verts ? It->Verts->Max() * sizeof(FVert) : 0;
		BspCapacity += It->Points ? It->Points->Max() * sizeof(FVector) : 0;
		BspCapacity += It->Vectors ? It->Vectors->Max() * sizeof(FVector) : 0;
		Lighting += It->LightBits.Num() + It->LightBlockOffsets.Num() * sizeof(INT);
		LightingStreamed += It->LightStreamData.Size();
		Models += It->LightMap.Num() * sizeof(FLightMapIndex);
		Models += It->Bounds.Num() * sizeof(FBox);
		Models += It->LeafHulls.Num() * sizeof(INT);
		Models += It->Leaves.Num() * sizeof(FLeaf);
		Models += It->Lights.Num() * sizeof(AActor*);
		DecodeCalls += It->LightDecodeCalls;
		DecodeBytes += It->LightDecodeBytes;
		DCArrayBytes(It->LightMap, AuxUsed, AuxCapacity);
		DCArrayBytes(It->Bounds, AuxUsed, AuxCapacity);
		DCArrayBytes(It->LeafHulls, AuxUsed, AuxCapacity);
		DCArrayBytes(It->Leaves, AuxUsed, AuxCapacity);
		DCArrayBytes(It->Lights, AuxUsed, AuxCapacity);
		DCArrayBytes(It->LightBits, AuxUsed, AuxCapacity);
		DCArrayBytes(It->LightBlockOffsets, AuxUsed, AuxCapacity);
	}

	for( TObjectIterator<UMesh> It; It; ++It )
	{
		Meshes += It->Verts.Num() * sizeof(FMeshVert);
		Meshes += It->Tris.Num() * sizeof(FMeshTri);
		Meshes += It->Connects.Num() * sizeof(FMeshVertConnect);
		Meshes += It->VertLinks.Num() * sizeof(INT);
		Meshes += It->DCFrameWords.Num() * 2 + It->DCFrameOffsets.Num() * 4;
		MeshStreamed += It->DCFrameStreamData.Size();
		Meshes += It->DCRuns.Num() * 8 + It->DCMaterials.Num() * 8 + It->DCIndices.Num() * 4;
		DWORD Anim = 0, AnimCap = 0, Topology = 0, TopologyCap = 0;
		DWORD Bounds = 0, BoundsCap = 0, Meta = 0, MetaCap = 0;
		DCArrayBytes(It->Verts, Anim, AnimCap);
		DCArrayBytes(It->DCFrameWords, Anim, AnimCap);
		DCArrayBytes(It->DCFrameOffsets, Anim, AnimCap);
		DCArrayBytes(It->Tris, Topology, TopologyCap);
		DCArrayBytes(It->Connects, Topology, TopologyCap);
		DCArrayBytes(It->VertLinks, Topology, TopologyCap);
		DCArrayBytes(It->DCRuns, Topology, TopologyCap);
		DCArrayBytes(It->DCMaterials, Topology, TopologyCap);
		DCArrayBytes(It->DCIndices, Topology, TopologyCap);
		DCArrayBytes(It->DCUVs, Topology, TopologyCap);
		DCArrayBytes(It->BoundingBoxes, Bounds, BoundsCap);
		DCArrayBytes(It->BoundingSpheres, Bounds, BoundsCap);
		DCArrayBytes(It->AnimSeqs, Meta, MetaCap);
		DCArrayBytes(It->Textures, Meta, MetaCap);
		for( INT i = 0; i < It->AnimSeqs.Num(); ++i )
			DCArrayBytes(It->AnimSeqs(i).Notifys, Meta, MetaCap);
		MeshFullUsed += Anim + Topology + Bounds + Meta;
		MeshCapacity += AnimCap + TopologyCap + BoundsCap + MetaCap;
	}
	INT Scratch = 0, Pooled = 0, Chunks = 0;
	FMemStack::GetDCMemoryStats(Scratch, Pooled, Chunks);
	DWORD NameBytes = FName::GetDCTableBytes();
	for( INT i = 0; i < FName::GetMaxNames(); ++i )
	{
		FNameEntry* Entry = FName::GetEntry(i);
		if( Entry && !(Entry->Flags & RF_Intrinsic) )
			NameBytes += sizeof(FNameEntry) - NAME_SIZE + appStrlen(Entry->Name) + 1;
	}
	debugf( "DCMEMDETAIL phase=%s mesh_used=%u mesh_capacity=%u mesh_decode_cache=%u"
		" scratch_allocated=%d scratch_pooled=%d scratch_chunks=%d cache_allocated=%d"
		" names_requested=%u vfhash_bytes=%u vfhash_count=%d profiler_static=%u"
		" profiler_heap=0 allocator_free_blocks=%d allocator_arena=%u",
		Phase, MeshFullUsed, MeshCapacity, GetDCMeshDecodeCacheBytes(),
		Scratch, Pooled, Chunks,
		GCache.GetDCAllocatedBytes(), NameBytes, VfHashBytes, VfHashCount,
		(DWORD)(sizeof(GDCFrame) + sizeof(INT) * 2 + sizeof(DWORD) * 2),
		Heap.ordblks, (DWORD)Heap.arena);

	for( TObjectIterator<UTexture> It; It; ++It )
	{
		for( INT Mip = 0; Mip < It->Mips.Num(); ++Mip )
		{
			Textures += It->Mips(Mip).DataArray.Num();
			DCArrayBytes(It->Mips(Mip).DataArray, TextureUsed, TextureCapacity);
		}
	}

	for( TObjectIterator<USound> It; It; ++It )
	{
		Sounds += It->Data.Num();
		DCArrayBytes(It->Data, SoundUsed, SoundCapacity);
	}
	debugf( "DCARRAYMEM phase=%s model_aux_light=%u/%u textures=%u/%u sounds=%u/%u"
		" sampled_blocks=%u sampled_rounding=%u allocator_headers=unmeasured",
		Phase, AuxUsed, AuxCapacity, TextureUsed, TextureCapacity, SoundUsed, SoundCapacity,
		DCSampledArrayBlocks, DCSampledArrayRounding);

	{
		DWORD VRAMUsed = 0, VRAMPeak = 0;
		INT VRAMBlocks = 0;
		appDCVRAMStats( VRAMUsed, VRAMPeak, VRAMBlocks );
		debugf( "DCVRAM phase=%s used=%u peak=%u blocks=%d",
			Phase, VRAMUsed, VRAMPeak, VRAMBlocks );
	}

	// Payload counters exclude TArray slack and object headers. Heap includes
	// allocator overhead. Largest free block is deliberately not inferred.
	debugf( "DCPROFILE phase=%s heap=%u sampled_peak=%u arena_free=%u vram=%u aica_samples=%u"
		" bsp=%u model_aux=%u lighting=%u lighting_streamed=%u mesh=%u mesh_streamed=%u"
		" texture_cpu=%u sound_cpu=%u light_calls=%u light_bytes=%u bsp_capacity=%u",
		Phase, (DWORD)Heap.uordblks, SampledHeapPeak, (DWORD)Heap.fordblks,
		PVR_GetVRAMUsed(), AICA_GetSampleBytes(), Bsp, Models, Lighting,
		LightingStreamed, Meshes, MeshStreamed, Textures, Sounds,
		DecodeCalls, DecodeBytes, BspCapacity );
	// Per-package attribution is O(packages * objects) with a parent walk each
	// time, so it is worth it only where we actually need to know who owns the
	// resident object graph: once the level is up, and once it is running.
	appDCProfileObjects( Phase,
		appStricmp( Phase, "level_ready" ) == 0
		|| appStricmp( Phase, "play" ) == 0
		|| appStricmp( Phase, "gameinfo_pre" ) == 0
		|| appStricmp( Phase, "gameinfo_post" ) == 0 );
}
#endif
