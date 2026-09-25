/*=============================================================================
	UnEngine.cpp: Unreal engine main.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "EnginePrivate.h"
#include "UnRender.h"
#if defined(PLATFORM_DREAMCAST)
#include <malloc.h>
#endif

/*-----------------------------------------------------------------------------
	Object class implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(UEngine);
IMPLEMENT_CLASS(URenderBase);
IMPLEMENT_CLASS(URenderDevice);

/*-----------------------------------------------------------------------------
	Engine init and exit.
-----------------------------------------------------------------------------*/

//
// Construct the engine.
//
UEngine::UEngine()
{
	guard(UEngine::UEngine);
	unguard;
}

//
// Init class.
//
void UEngine::InternalClassInitializer( UClass* Class )
{
	guard(UEngine::InternalClassInitializer);
	if( appStricmp(Class->GetName(),"Engine")==0 )
	{
		new(Class,"CacheSizeMegs",      RF_Public)UIntProperty (CPP_PROPERTY(CacheSizeMegs      ), "Settings", CPF_Config );
		new(Class,"UseSound",           RF_Public)UBoolProperty(CPP_PROPERTY(UseSound           ), "Settings", CPF_Config );
	}
	unguard;
}

// Register names.
#define NAMES_ONLY
#define DECLARE_NAME(name) ENGINE_API FName ENGINE_##name;
#include "EngineClasses.h"
#undef DECLARE_NAME
#undef NAMES_ONLY

//
// Initialize the engine.
//
void UEngine::Init()
{
	guard(UEngine::Init);

	#define NAMES_ONLY
	#define DECLARE_NAME(name) ENGINE_##name = FName(#name,FNAME_Intrinsic);
	#include "EngineClasses.h"
	#undef DECLARE_NAME
	#undef NAMES_ONLY

	// Subsystems.
	FURL::Init();

#ifdef PLATFORM_LOW_MEMORY
	GCache.Init( 1024 * 256, 2048 );
#else
	GCache.Init( 1024 * 1024 * Clamp(CacheSizeMegs,1,1024), 4096 );
#endif

	// Objects.
	Cylinder = new UPrimitive;

	// Add to root.
	GObj.AddToRoot( this );

	// Init audio.
	if
	(	UseSound
	&&	GIsClient
	&&	!ParseParam(appCmdLine(),"NOSOUND") )
	{
		UClass* AudioClass = GObj.LoadClass( UAudioSubsystem::StaticClass, NULL, "ini:Engine.Engine.AudioDevice", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
		Audio = ConstructClassObject<UAudioSubsystem>( AudioClass );
		if( !Audio->Init() )
		{
			debugf( NAME_Log, "Audio initialization failed" );
			delete Audio;
			Audio = NULL;
		}
	}

	debugf( NAME_Init, "Unreal engine initialized" );
	unguard;
}

//
// Exit the engine.
//
void UEngine::Destroy()
{
	guard(UEngine::Destroy);

	// Remove from root.
	GObj.RemoveFromRoot( this );

	// Shut down all subsystems.
	if( Audio )
	{
		delete Audio;
		Audio = NULL;
	}
	if( Render )
	{
		delete Render;
		Render = NULL;
	}
	if( Client )
	{
		delete Client;
		Client = NULL;
	}

	debugf( NAME_Exit, "Engine shut down" );

	USubsystem::Destroy();
	unguard;
}

//
// Flush all caches.
//
void UEngine::Flush()
{
	guard(UEngine::Flush);

	GCache.Flush();
	if( Client )
		Client->Flush();

	unguard;
}

//
// Tick rate.
//
INT UEngine::GetMaxTickRate()
{
	guard(UEngine::GetMaxTickRate);
	return 0;
	unguard;
}

//
// Progress indicator.
//
void UEngine::SetProgress( const char* Str1, const char* Str2, FLOAT Seconds )
{
	guard(UEngine::SetProgress);
	unguard;
}

//
// Serialize.
//
void UEngine::Serialize( FArchive& Ar )
{
	guard(UGameEngine::Serialize);

	USubsystem::Serialize( Ar );
	Ar << Cylinder << Client << Render << Audio;

	unguardobj;
}

#if defined (PLATFORM_DREAMCAST)
// ----------------------------------------------------------------------------
// Capacity-based Dreamcast memory audit.  Stream slices describe disc data,
// not resident buffers, so deliberately do not count their Length here.
// ----------------------------------------------------------------------------
extern "C" DWORD PVR_GetVRAMUsed();
ENGINE_API UBOOL GDCUseVQDynamicLightmaps = 0;
void DumpMemStatsDC( const char* Tag )
{
	guard(DumpMemStatsDC);
	DWORD BspUsed=0, BspCapacity=0, PolyCapacity=0, ModelAux=0, MeshArrays=0;
	DWORD TextureArrays=0, SoundArrays=0, ScriptArrays=0, DefaultArrays=0;
	DWORD ObjectBodies=0, LevelArrays=0;
	INT ObjectCount=0, MeshCount=0, TextureCount=0;
	for( FObjectIterator It; It; ++It )
	{
		UObject* Obj = (UObject*)*It;
		++ObjectCount;
		ObjectBodies += Obj->GetClass()->ClassRecordSize;
		if( Obj->IsA( UBspNodes::StaticClass ) )
		{
			UBspNodes* Db = (UBspNodes*)Obj;
			BspUsed += Db->Num() * sizeof(FBspNode);
			BspCapacity += Db->Max() * sizeof(FBspNode);
		}
		else if( Obj->IsA( UBspSurfs::StaticClass ) )
		{
			UBspSurfs* Db = (UBspSurfs*)Obj;
			BspUsed += Db->Num() * sizeof(FBspSurf);
			BspCapacity += Db->Max() * sizeof(FBspSurf);
		}
		else if( Obj->IsA( UVerts::StaticClass ) )
		{
			UVerts* Db = (UVerts*)Obj;
			BspUsed += Db->Num() * sizeof(FVert);
			BspCapacity += Db->Max() * sizeof(FVert);
		}
		else if( Obj->IsA( UVectors::StaticClass ) )
		{
			UVectors* Db = (UVectors*)Obj;
			BspUsed += Db->Num() * sizeof(FVector);
			BspCapacity += Db->Max() * sizeof(FVector);
		}
		if( Obj->IsA( UPolys::StaticClass ) )
			PolyCapacity += ((UPolys*)Obj)->Max() * sizeof(FPoly);
		if( Obj->IsA( UModel::StaticClass ) )
		{
			UModel* Model = (UModel*)Obj;
			ModelAux += Model->LightMap.ArrayMax * sizeof(FLightMapIndex)
				+ Model->LightBits.ArrayMax + Model->LightBlockOffsets.ArrayMax * sizeof(INT)
				+ Model->Bounds.ArrayMax * sizeof(FBox)
				+ Model->LeafHulls.ArrayMax * sizeof(INT)
				+ Model->Leaves.ArrayMax * sizeof(FLeaf)
				+ Model->Lights.ArrayMax * sizeof(AActor*);
		}
		if( Obj->IsA( UMesh::StaticClass ) )
		{
			UMesh* Mesh = (UMesh*)Obj;
			++MeshCount;
			MeshArrays += Mesh->Verts.ArrayMax * sizeof(FMeshVert)
				+ Mesh->Tris.ArrayMax * sizeof(FMeshTri)
				+ Mesh->AnimSeqs.ArrayMax * sizeof(FMeshAnimSeq)
				+ Mesh->Connects.ArrayMax * sizeof(FMeshVertConnect)
				+ Mesh->BoundingBoxes.ArrayMax * sizeof(FBox)
				+ Mesh->BoundingSpheres.ArrayMax * sizeof(FSphere)
				+ Mesh->VertLinks.ArrayMax * sizeof(INT)
				+ Mesh->Textures.ArrayMax * sizeof(UTexture*)
				+ Mesh->DCFrameWords.ArrayMax * sizeof(_WORD)
				+ Mesh->DCFrameOffsets.ArrayMax * sizeof(INT)
				+ Mesh->DCRuns.ArrayMax * sizeof(FDCMeshRun)
				+ Mesh->DCMaterials.ArrayMax * sizeof(FDCMeshMaterial)
				+ Mesh->DCIndices.ArrayMax * sizeof(_WORD)
				+ Mesh->DCUVs.ArrayMax * sizeof(_WORD)
				+ Mesh->DCNormalWords.ArrayMax * sizeof(_WORD)
				+ Mesh->DCNormalBlockOffsets.ArrayMax * sizeof(INT)
				+ Mesh->DCNormalCompressed.ArrayMax;
		}
		if( It->IsA( UTexture::StaticClass ) )
		{
			UTexture* T = (UTexture*)(UObject*)*It;
			TextureArrays += T->Mips.ArrayMax * sizeof(FMipmap);
			for( INT i=0; i<T->Mips.Num(); ++i )
				TextureArrays += T->Mips(i).DataArray.ArrayMax;
			++TextureCount;
		}
		if( Obj->IsA( USound::StaticClass ) )
			SoundArrays += ((USound*)Obj)->Data.ArrayMax;
		if( Obj->IsA( UStruct::StaticClass ) )
			ScriptArrays += ((UStruct*)Obj)->Script.ArrayMax;
		if( Obj->IsA( UClass::StaticClass ) )
			DefaultArrays += ((UClass*)Obj)->Defaults.ArrayMax;
		if( Obj->IsA( ULevel::StaticClass ) )
		{
			ULevel* L = (ULevel*)Obj;
			LevelArrays += L->Max() * sizeof(AActor*)
				+ L->ReachSpecs.ArrayMax * sizeof(FReachSpec)
				+ L->DCLightmaps.ArrayMax * sizeof(FDCLightmapEntry)
				+ L->DCLightmapPlacements.ArrayMax * sizeof(FDCLightmapPlacement)
				+ L->DCLightmapPages.ArrayMax * sizeof(FDCLightmapPage);
			LevelArrays += L->DCDynamicLightmaps.ArrayMax * sizeof(FDCDynamicLightmapEntry)
				+ L->DCDynamicLightmapPages.ArrayMax * sizeof(FDCLightmapPage)
				+ L->DCDynamicPageResident.ArrayMax;
		}
	}
	struct mallinfo Heap = mallinfo();
	INT ScratchAllocated=0, ScratchPooled=0, ScratchChunks=0;
	FMemStack::GetDCMemoryStats( ScratchAllocated, ScratchPooled, ScratchChunks );
	debugf( "DCMEM phase=%s heap=%d free=%d objects=%d bodies=%u bsp=%u/%u polys=%u model_aux=%u level=%u mesh=%d/%u decode=%u",
		Tag ? Tag : "manual", Heap.uordblks, Heap.fordblks, ObjectCount,
		(unsigned)ObjectBodies, (unsigned)BspUsed, (unsigned)BspCapacity,
		(unsigned)PolyCapacity, (unsigned)ModelAux, (unsigned)LevelArrays, MeshCount, (unsigned)MeshArrays,
		(unsigned)GetDCMeshDecodeCacheBytes() );
	debugf( "DCMEMDATA phase=%s textures=%d/%u sounds=%u scripts=%u defaults=%u cache=%d names=%d scratch=%d/%d/%d vram=%u",
		Tag ? Tag : "manual", TextureCount, (unsigned)TextureArrays,
		(unsigned)SoundArrays, (unsigned)ScriptArrays, (unsigned)DefaultArrays,
		GCache.GetDCAllocatedBytes(), FName::GetDCTableBytes(),
		ScratchAllocated, ScratchPooled, ScratchChunks, (unsigned)PVR_GetVRAMUsed() );

	unguard;
}
#endif
/*-----------------------------------------------------------------------------
	Input.
-----------------------------------------------------------------------------*/

//
// This always going to be the last exec handler in the chain. It
// handles passing the command to all other global handlers.
//
UBOOL UEngine::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UEngine::Exec);
	const char* Str = Cmd;
#if defined(PLATFORM_DREAMCAST)
	if( ParseCommand(&Str,"DCMEM") )
	{
		DumpMemStatsDC("manual");
		return 1;
	}
#endif

	// See if any other subsystems claim the command.
	if( GObj.Exec					(Cmd,Out) ) return 1;
	if( GCache.Exec					(Cmd,Out) ) return 1;
	if( GExecHook && GExecHook->Exec(Cmd,Out) ) return 1;
	if( GSystem && GSystem->Exec	(Cmd,Out) ) return 1;
	if( Client  && Client->Exec		(Cmd,Out) ) return 1;
	if( Render  && Render->Exec		(Cmd,Out) ) return 1;
	if( Audio   && Audio->Exec		(Cmd,Out) ) return 1;

	// Handle engine command line.
	if( ParseCommand(&Str,"FLUSH") )
	{
		Flush();
		Out->Log( "Flushed engine caches" );
		return 1;
	}
	else return 0;
	unguard;
}

//
// Key handler.
//
UBOOL UEngine::Key( UViewport* Viewport, EInputKey Key )
{
	guard(UEngine::Key);
	return Viewport->Console && Viewport->Console->eventKeyType( Key );
	unguard;
}

//
// Input event handler.
//
int	UEngine::InputEvent( UViewport* Viewport, EInputKey iKey, EInputAction State, FLOAT Delta )
{
	guard(UEngine::InputEvent);
	if( Viewport->Console && Viewport->Console->eventKeyEvent( iKey, State, Delta ) )
	{
		// Player console handled it.
		return 1;
	}
	else if( Viewport->Input->Process( Viewport->Console ? (FOutputDevice&)*Viewport->Console : (FOutputDevice&)*GSystem, iKey, State, Delta ) )
	{
		// Input system handled it.
		return 1;
	}
	else
	{
		// Nobody handled it.
		return 0;
	}
	unguard;
}

INT UEngine::ChallengeResponse( INT Challenge )
{
	guard(UEngine::ChallengeResponse);
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
