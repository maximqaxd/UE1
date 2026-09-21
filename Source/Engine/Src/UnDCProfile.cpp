#include "EnginePrivate.h"

#if defined(PLATFORM_DREAMCAST)
#include <malloc.h>

extern "C" DWORD PVR_GetVRAMUsed();
extern "C" DWORD AICA_GetSampleBytes();
extern CORE_API void appDCProfileObjects( const char* Phase, UBOOL Detailed );

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
		(DWORD)(sizeof(INT) * 2 + sizeof(DWORD) * 2),
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

	// Payload counters exclude TArray slack and object headers. Heap includes
	// allocator overhead. Largest free block is deliberately not inferred.
	debugf( "DCPROFILE phase=%s heap=%u sampled_peak=%u arena_free=%u vram=%u aica_samples=%u"
		" bsp=%u model_aux=%u lighting=%u lighting_streamed=%u mesh=%u mesh_streamed=%u"
		" texture_cpu=%u sound_cpu=%u light_calls=%u light_bytes=%u bsp_capacity=%u",
		Phase, (DWORD)Heap.uordblks, SampledHeapPeak, (DWORD)Heap.fordblks,
		PVR_GetVRAMUsed(), AICA_GetSampleBytes(), Bsp, Models, Lighting,
		LightingStreamed, Meshes, MeshStreamed, Textures, Sounds,
		DecodeCalls, DecodeBytes, BspCapacity );
	appDCProfileObjects( Phase, 0 );
}
#endif
