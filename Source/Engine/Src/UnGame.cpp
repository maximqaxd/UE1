/*=============================================================================
	UnGame.cpp: Unreal game engine.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "EnginePrivate.h"
#include "UnDCFrameProfile.h"
#include "UnRender.h"
#include "UnNet.h"
#include "UnDCVMU.h"

/*-----------------------------------------------------------------------------
	Object class implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(UGameEngine);

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
extern CORE_API void appDCSetLinkerTablesReleased( UBOOL Released );

static UBOOL GDCGameLinkersReleased = 0;

// Drop the editor's CSG source brushes.
//
// A shipped level's geometry is already baked into Level->Model, so a brush
// that is not a mover is never consulted again: every runtime dereference of
// Actor->Brush is either gated on IsMovingBrush() or null-checked, and
// UnLevAct.cpp nulls the pointer itself when an actor is destroyed. Releasing
// the reference lets the collector take the whole UModel with it.
//
// The model is not the expensive part -- its UBspNodes is. That object carries
// Zones[64] and so costs 1592 bytes whether or not the brush has a single
// zone, and Chizra ships 199 of them on top of its movers.
//
// IsMovingBrush() reads Brush, so mover-ness has to be decided before anything
// is nulled; only non-movers are touched, and for those the predicate was
// already false, so it stays stable across the pass.
// With the model gone the ABrush itself is an inert shell -- its CSG is already
// baked into the level BSP, and no runtime path reads a static brush: every
// Level->Brush() caller is in Editor. So the actor slot goes too, which is
// another 536 bytes each, 199 of them on Chizra.
//
// Actors(0) is the LevelInfo and Actors(1) the builder brush; the engine reaches
// both by fixed index, so those two keep their slot and only lose the model.
static void DCDiscardEditorBrushes( ULevel* Level )
{
	// Clearing the Actors slot alone frees nothing: every FBspSurf keeps an
	// ABrush* back to the brush that generated it, that field is serialized
	// (UnObj.h), so the collector follows it and all 199 survive. The field is
	// write-only at runtime -- UnDynBsp sets it when a mover rebuilds, and the
	// only readers are in Editor -- so the static-brush ones can go.
	INT Unlinked = 0;
	if( Level->Model && Level->Model->Surfs )
	{
		for( INT SurfIndex = 0; SurfIndex < Level->Model->Surfs->Num(); ++SurfIndex )
		{
			FBspSurf& Surf = Level->Model->Surfs->Element(SurfIndex);
			if( Surf.Actor && !Surf.Actor->IsMovingBrush() )
			{
				Surf.Actor = NULL;
				++Unlinked;
			}
		}
	}

	INT Dropped = 0, Removed = 0;
	for( INT ActorIndex = 0; ActorIndex < Level->Num(); ++ActorIndex )
	{
		AActor* Actor = Level->Actors(ActorIndex);
		if( !Actor || !Actor->Brush || Actor->IsMovingBrush() )
			continue;
		// Not a mover and Brush is set, so IsStaticBrush() holds here -- but it
		// reads Brush, so the slot has to be dropped before the model is.
		UBOOL Removable = ( ActorIndex > 1 );
		Actor->Brush = NULL;
		++Dropped;
		if( Removable )
		{
			Level->Actors(ActorIndex) = NULL;
			++Removed;
		}
	}
	if( Dropped )
		debugf( "DCBRUSH discarded map=%s editor_brushes=%i actors_removed=%i surfs_unlinked=%i",
			Level->GetParent()->GetName(), Dropped, Removed, Unlinked );
}

static void DCDiscardNonMoverPolys( ULevel* Level )
{
	for( TObjectIterator<UModel> It; It; ++It )
	{
		if( !It->Polys || !It->Polys->Num() )
		{
			continue;
		}

		UBOOL UsedByMover = 0;
		for( INT ActorIndex = 0; ActorIndex < Level->Num(); ++ActorIndex )
		{
			AActor* Owner = Level->Actors(ActorIndex);
			if( Owner && Owner->IsMovingBrush() && Owner->Brush == *It )
			{
				UsedByMover = 1;
				break;
			}
		}
		if( !UsedByMover )
		{
			It->Polys->Empty();
			It->Polys = NULL;
		}
	}
}

static void DCReleaseGameLinkers()
{
	if( GDCGameLinkersReleased )
		return;

	for( FObjectIterator It; It; ++It )
	{
		if( It->GetFlags() & (RF_NeedLoad | RF_NeedPostLoad) )
			appErrorf( "DAT linker release found unfinished object: %s", It->GetFullName() );
	}
	GObj.ResetLoaders( NULL );
	appDCSetLinkerTablesReleased( 1 );
	GDCGameLinkersReleased = 1;
}
#endif

#if defined(PLATFORM_DREAMCAST)

enum
{
	DC_SESSION_URL_BYTES = 1024,
	DC_SESSION_ITEMS_BYTES = 16384
};

static UBOOL GDCSessionTravel = 0;
static char GDCSessionURL[DC_SESSION_URL_BYTES] = "";
static char GDCSessionItems[DC_SESSION_ITEMS_BYTES] = "";

static const char* GDCTerraniuxEvents[] =
{
	"lickmer", "fast", "sli", "goh", "sluty", "tremor", "tremor2", "noback"
};
static BYTE GDCTerraniuxPending[2][ARRAY_COUNT(GDCTerraniuxEvents)];
static BYTE GDCTerraniuxEver[ARRAY_COUNT(GDCTerraniuxEvents)];
static BYTE GDCTerraniuxOpen;
static const char GDCTerraniuxRelayPrefix[]="DCSplitTerraniuxRelay_";
static const char* GDCSkyTownEvents[] = { "Barndoors3", "StopAmbient" };
static BYTE GDCSkyTownPending[ARRAY_COUNT(GDCSkyTownEvents)];
static const char GDCSkyTownRelayPrefix[]="DCSplitSkyTownRelay_";

static INT DCTerraniuxPart( ULevel* Level )
{
	if( !Level ) return INDEX_NONE;
	const char* Name=Level->GetParent()->GetName();
	return !appStricmp(Name,"Terraniux1") ? 0
		: !appStricmp(Name,"Terraniux2") ? 1 : INDEX_NONE;
}

UBOOL DCTerraniuxHandleRelayEvent( AActor* Actor, UFunction* Function )
{
	const char* Name=Actor->GetName();
	if( Name && !appStrnicmp(Name,GDCSkyTownRelayPrefix,
		ARRAY_COUNT(GDCSkyTownRelayPrefix)-1) )
	{
		if( Function && !appStricmp(Function->GetName(),"Trigger") )
			for( INT i=0; i<ARRAY_COUNT(GDCSkyTownEvents); ++i )
				if( Actor->Tag==FName(GDCSkyTownEvents[i]) )
				{
					if( GDCSkyTownPending[i]<4 ) ++GDCSkyTownPending[i];
					debugf("DCSKYTOWN relay event=%s pending=%d",
						GDCSkyTownEvents[i],GDCSkyTownPending[i]);
					return 1;
				}
		if( Function && !appStricmp(Function->GetName(),"UnTrigger") ) return 1;
	}
	if( Name && !appStrnicmp(Name,GDCTerraniuxRelayPrefix,
		ARRAY_COUNT(GDCTerraniuxRelayPrefix)-1) )
	{
		if( Function && !appStricmp(Function->GetName(),"Trigger") )
		{
			const INT Part=DCTerraniuxPart(Actor->XLevel);
			if( Part!=INDEX_NONE )
				for( INT i=0; i<ARRAY_COUNT(GDCTerraniuxEvents); ++i )
					if( Actor->Tag==FName(GDCTerraniuxEvents[i]) )
					{
						BYTE& Count=GDCTerraniuxPending[1-Part][i];
						if( Count<4 ) ++Count;
						if( i==3 || i==4 ) GDCTerraniuxEver[i]=1;
						debugf("DCTERRA relay map=%s event=%s pending=%d",
							Actor->XLevel->GetParent()->GetName(),GDCTerraniuxEvents[i],Count);
						return 1;
					}
		}
		if( Function && !appStricmp(Function->GetName(),"UnTrigger") ) return 1;
	}
	return 0;
}

static void DCTerraniuxCapture( ULevel* Level )
{
	if( DCTerraniuxPart(Level)!=1 ) return;
	for( INT i=0; i<Level->Num(); ++i )
	{
		AMover* Mover=Cast<AMover>(Level->Actors(i));
		if( !Mover || Mover->KeyNum<=0 ) continue;
		if( Mover->Tag==FName("goh") ) GDCTerraniuxOpen|=1;
		if( Mover->Tag==FName("sluty") ) GDCTerraniuxOpen|=2;
	}
}

static void DCTerraniuxRestore( ULevel* Level )
{
	if( DCTerraniuxPart(Level)!=1 || !GDCTerraniuxOpen ) return;
	for( INT i=0; i<Level->Num(); ++i )
	{
		AMover* Mover=Cast<AMover>(Level->Actors(i));
		if( !Mover || Mover->bHidden ) continue;
		const INT Bit=Mover->Tag==FName("goh") ? 1
			: Mover->Tag==FName("sluty") ? 2 : 0;
		if( !(Bit & GDCTerraniuxOpen) ) continue;
		Mover->GotoState(NAME_None);
		Mover->Physics=PHYS_None;
		Mover->bInterpolating=0;
		Mover->bOpening=0;
		Mover->PhysAlpha=0;
		Mover->KeyNum=Mover->PrevKeyNum=1;
		Mover->Rotation=Mover->BaseRot+Mover->KeyRot[1];
		if( !Level->FarMoveActor(Mover,Mover->BasePos+Mover->KeyPos[1],0,1) )
			appErrorf("DCTERRA could not restore mover %s",Mover->GetName());
	}
}

static void DCTerraniuxReplay( ULevel* Level )
{
	const INT Part=DCTerraniuxPart(Level);
	if( Part==INDEX_NONE ) return;
	for( INT Event=0; Event<ARRAY_COUNT(GDCTerraniuxEvents); ++Event )
	{
		INT Count=GDCTerraniuxPending[Part][Event];
		if( (Event==3 || Event==4) && GDCTerraniuxEver[Event] && Part==1 && Count==0 )
			Count=1;
		GDCTerraniuxPending[Part][Event]=0;
		for( INT Pass=0; Pass<Count; ++Pass )
			for( INT i=0; i<Level->Num(); ++i )
			{
				AActor* Actor=Level->Actors(i);
				if( !Actor || Actor->bDeleteMe || Actor->Tag!=FName(GDCTerraniuxEvents[Event])
					|| !appStrnicmp(Actor->GetName(),GDCTerraniuxRelayPrefix,
						ARRAY_COUNT(GDCTerraniuxRelayPrefix)-1) ) continue;
				AMover* Mover=Cast<AMover>(Actor);
				if( Mover && Mover->bTriggerOnceOnly && Mover->KeyNum>0 ) continue;
				Actor->eventTrigger(NULL,NULL);
			}
		if( Count ) debugf("DCTERRA replay map=%s event=%s count=%d",
			Level->GetParent()->GetName(),GDCTerraniuxEvents[Event],Count);
	}
}

static void DCSkyTownReplay( ULevel* Level )
{
	if( appStricmp(Level->GetParent()->GetName(),"SkyTown1") ) return;
	for( INT Event=0; Event<ARRAY_COUNT(GDCSkyTownEvents); ++Event )
	{
		const INT Count=GDCSkyTownPending[Event];
		GDCSkyTownPending[Event]=0;
		for( INT Pass=0; Pass<Count; ++Pass )
			for( INT i=0; i<Level->Num(); ++i )
			{
				AActor* Actor=Level->Actors(i);
				if( Actor && !Actor->bDeleteMe && Actor->Tag==FName(GDCSkyTownEvents[Event]) )
					Actor->eventTrigger(NULL,NULL);
			}
		if( Count ) debugf("DCSKYTOWN replay event=%s count=%d",GDCSkyTownEvents[Event],Count);
	}
}

// Only semantic, pointer-free state survives a Dreamcast session restart.
// Keep this deliberately scoped to the experimental split maps: raw actor
// frames, object references and latent script actions cannot cross appExit.
enum { DC_CHIZRA_RECORDS = 160, DC_CHIZRA_NAME_BYTES = 48 };
enum { DC_CHIZRA_PICKUP = 1, DC_CHIZRA_PAWN = 2, DC_CHIZRA_TRIGGER = 3 };
struct FDCChizraRecord
{
	char Name[DC_CHIZRA_NAME_BYTES];
	BYTE Kind;
	BYTE Spent;
};
struct FDCChizraState
{
	UBOOL Initialized;
	INT Count;
	FDCChizraRecord Records[DC_CHIZRA_RECORDS];
};
static FDCChizraState GDCChizraStates[6];
static UBOOL GDCChizraBaptistryOpen = 0;

// Pointer-free campaign state accompanies the full serialized level package.
struct FDCVMUState
{
	DWORD Version;
	char Player[64];
	FDCChizraState Chizra[6];
	UBOOL Baptistry;
	BYTE TerraniuxPending[2][8], TerraniuxEver[8], TerraniuxOpen, SkyTownPending[2];
};
static char GDCVMUPlayer[64];
static char GDCVMUFile[128];
static INT GDCVMUReadySlot = -1;
static void DCVMUCaptureState(ULevel* Level, TArray<BYTE>& Bytes)
{
	FDCVMUState S = {};
	S.Version = 1;
	for (INT i = 0; i < Level->Num(); ++i)
		if (APlayerPawn* P = Cast<APlayerPawn>(Level->Actors(i)))
			if (P->Player)
			{
				appStrncpy(S.Player, P->GetName(), sizeof(S.Player));
				break;
			}
	appMemcpy(S.Chizra, GDCChizraStates, sizeof(S.Chizra));
	S.Baptistry = GDCChizraBaptistryOpen;
	appMemcpy(S.TerraniuxPending, GDCTerraniuxPending, sizeof(S.TerraniuxPending));
	appMemcpy(S.TerraniuxEver, GDCTerraniuxEver, sizeof(S.TerraniuxEver));
	S.TerraniuxOpen = GDCTerraniuxOpen;
	appMemcpy(S.SkyTownPending, GDCSkyTownPending, sizeof(S.SkyTownPending));
	Bytes.Empty();
	Bytes.Add(sizeof(S));
	appMemcpy(&Bytes(0), &S, sizeof(S));
}
static UBOOL DCVMURestoreState(const TArray<BYTE>& Bytes)
{
	if (Bytes.Num() != sizeof(FDCVMUState))
		return 0;
	const FDCVMUState& S = *(const FDCVMUState*)&Bytes(0);
	if (S.Version != 1 || !S.Player[0] || S.Player[63])
		return 0;
	for (INT i = 0; i < 6; ++i)
	{
		if (S.Chizra[i].Count < 0 || S.Chizra[i].Count > DC_CHIZRA_RECORDS)
			return 0;
		for (INT j = 0; j < S.Chizra[i].Count; ++j)
			if (S.Chizra[i].Records[j].Name[DC_CHIZRA_NAME_BYTES - 1])
				return 0;
	}
	appMemcpy(GDCChizraStates, S.Chizra, sizeof(S.Chizra));
	GDCChizraBaptistryOpen = S.Baptistry;
	appMemcpy(GDCTerraniuxPending, S.TerraniuxPending, sizeof(S.TerraniuxPending));
	appMemcpy(GDCTerraniuxEver, S.TerraniuxEver, sizeof(S.TerraniuxEver));
	GDCTerraniuxOpen = S.TerraniuxOpen;
	appMemcpy(GDCSkyTownPending, S.SkyTownPending, sizeof(S.SkyTownPending));
	appStrcpy(GDCVMUPlayer, S.Player);
	return 1;
}

static INT DCChizraPart( ULevel* Level )
{
	if( !Level ) return INDEX_NONE;
	const char* Name = Level->GetParent()->GetName();
	return !appStricmp(Name,"Chizra1") ? 0 : !appStricmp(Name,"Chizra2") ? 1
		: !appStricmp(Name,"IsvKran32A") ? 2 : !appStricmp(Name,"IsvKran32B") ? 3
		: !appStricmp(Name,"SkyTown1") ? 4 : !appStricmp(Name,"SkyTown2") ? 5
		: INDEX_NONE;
}

static AActor* DCChizraFindActor( ULevel* Level, const char* Name )
{
	for( INT i=0; i<Level->Num(); ++i )
	{
		AActor* Actor = Level->Actors(i);
		if( Actor && !appStricmp(Actor->GetName(),Name) ) return Actor;
	}
	return NULL;
}

static BYTE DCChizraRecordKind( AActor* Actor )
{
	if( Actor->IsA(AInventory::StaticClass) ) return DC_CHIZRA_PICKUP;
	if( Actor->IsA(APawn::StaticClass) && !Actor->IsA(APlayerPawn::StaticClass) )
		return DC_CHIZRA_PAWN;
	ATrigger* Trigger = Cast<ATrigger>(Actor);
	return Trigger && Trigger->bTriggerOnceOnly ? DC_CHIZRA_TRIGGER : 0;
}

static void DCChizraCapture( ULevel* Level )
{
	const INT Part = DCChizraPart(Level);
	if( Part==INDEX_NONE || !GDCChizraStates[Part].Initialized ) return;
	FDCChizraState& State = GDCChizraStates[Part];
	INT NewSpent=0;
	for( INT i=0; i<State.Count; ++i )
	{
		FDCChizraRecord& Record = State.Records[i];
		if( Record.Spent ) continue;
		AActor* Actor = DCChizraFindActor(Level,Record.Name);
		if( !Actor || Actor->bDeleteMe
			|| (Record.Kind==DC_CHIZRA_PICKUP
				&& (Cast<AInventory>(Actor)->bHeldItem || Actor->Owner))
			|| (Record.Kind==DC_CHIZRA_PAWN && Cast<APawn>(Actor)->Health<=0)
			|| (Record.Kind==DC_CHIZRA_TRIGGER && !Actor->bCollideActors) )
		{
			Record.Spent=1;
			++NewSpent;
		}
	}
	if( Part<2 )
		for( INT i=0; i<Level->Num(); ++i )
		{
			AMover* Mover = Cast<AMover>(Level->Actors(i));
			if( Mover && Mover->Tag==FName("baptistry") && Mover->KeyNum>0 )
				GDCChizraBaptistryOpen=1;
		}
	debugf("DCCHIZRA save map=%s records=%d new_spent=%d baptistry=%d",
		Level->GetParent()->GetName(),State.Count,NewSpent,GDCChizraBaptistryOpen);
}

static void DCChizraRestore( ULevel* Level )
{
	const INT Part = DCChizraPart(Level);
	if( Part==INDEX_NONE ) return;
	FDCChizraState& State = GDCChizraStates[Part];
	if( !State.Initialized )
	{
		for( INT i=0; i<Level->Num(); ++i )
		{
			AActor* Actor = Level->Actors(i);
			if( !Actor ) continue;
			const BYTE Kind = DCChizraRecordKind(Actor);
			if( !Kind ) continue;
			if( State.Count==DC_CHIZRA_RECORDS
				|| appStrlen(Actor->GetName())>=DC_CHIZRA_NAME_BYTES )
				appErrorf("DCCHIZRA actor ledger overflow in %s",Level->GetParent()->GetName());
			FDCChizraRecord& Record = State.Records[State.Count++];
			appStrcpy(Record.Name,Actor->GetName());
			Record.Kind=Kind;
			Record.Spent=0;
		}
		State.Initialized=1;
	}
	else
	{
		INT Restored=0;
		for( INT i=0; i<State.Count; ++i )
		{
			const FDCChizraRecord& Record = State.Records[i];
			if( !Record.Spent ) continue;
			AActor* Actor = DCChizraFindActor(Level,Record.Name);
			if( !Actor ) continue;
			if( Record.Kind==DC_CHIZRA_TRIGGER ) Actor->SetCollision(0,0,0);
			else if( !Level->DestroyActor(Actor) )
				appErrorf("DCCHIZRA could not restore spent actor %s",Record.Name);
			++Restored;
		}
		debugf("DCCHIZRA restore map=%s records=%d spent=%d",
			Level->GetParent()->GetName(),State.Count,Restored);
	}
	if( Part<2 && GDCChizraBaptistryOpen )
	{
		for( INT i=0; i<Level->Num(); ++i )
		{
			AMover* Mover = Cast<AMover>(Level->Actors(i));
			if( !Mover || Mover->Tag!=FName("baptistry") || Mover->bHidden ) continue;
			Mover->GotoState(NAME_None);
			Mover->Physics=PHYS_None;
			Mover->bInterpolating=0;
			Mover->bOpening=0;
			Mover->PhysAlpha=0;
			Mover->KeyNum=Mover->PrevKeyNum=1;
			Mover->Rotation=Mover->BaseRot+Mover->KeyRot[1];
			if( !Level->FarMoveActor(Mover,Mover->BasePos+Mover->KeyPos[1],0,1) )
				appErrorf("DCCHIZRA could not restore baptistry mover %s",Mover->GetName());
		}
	}
}

enum
{
	DC_MEMORY_SIM_MAGIC = 0x4443534d,
	DC_MEMORY_SIM_READY = 0x44534352,
	DC_MEMORY_SIM_COMPLETE = -1,
	DC_MEMORY_SIM_FRAMES_PER_MAP = 180
};

// The ready word lets Flycast wait for the executable instead of writing into
// its eventual address while the BIOS is still using that RAM. Real hardware
// never writes the opt-in magic, so normal play remains unaffected.
extern "C"
{
	DLL_EXPORT volatile DWORD GDCMemorySimMagic = 0;
	DLL_EXPORT volatile DWORD GDCMemorySimState = 0;
	DLL_EXPORT volatile DWORD GDCMemorySimReady = 0;
}

void UGameEngine::TickDCMemorySimulation()
{
	GDCMemorySimReady = DC_MEMORY_SIM_READY;
	if( GDCMemorySimMagic != DC_MEMORY_SIM_MAGIC || !GLevel )
	{
		return;
	}

	static const char* Route[] =
	{
		"Unreal",
		"Vortex2",
		"Nyleve",
		"Dig1",
		"Dig2",
		"Dug",
		"Passage",
		"Chizra1",
		"Chizra2",
		"Ceremony",
		"Dark",
		"Harobed",
		"TerraLift",
		"Terraniux1",
		"Terraniux2",
		"Noork",
		"Ruins1",
		"Ruins2",
		"Trench",
		"IsvKran4",
		"IsvKran32A",
		"IsvKran32B",
		"IsvKran32A",
		"IsvDeck1",
		"SpireVillage",
		"TheSunspire",
		"SkyCaves",
		"SkyTown1",
		"SkyTown2",
		"SkyTown1",
		"SkyBase",
		"VeloraEnd",
		"Bluff",
		"DasaPass",
		"DasaCellars1",
		"DasaCellars2",
		"NaliBoat",
		"NaliC",
		"NaliLord",
		"DCrater",
		"ExtremeBeg",
		"ExtremeLab",
		"ExtremeCore",
		"ExtremeGen",
		"ExtremeDGen",
		"ExtremeDark",
		"ExtremeEnd",
		"QueenEnd",
		"endgame"
	};
	static char ActiveMap[64] = "";
	static INT RouteIndex = INDEX_NONE;
	static INT Frames = 0;

	const char* Map = GLevel->GetParent()->GetName();
	if( appStricmp(ActiveMap, Map) )
	{
		appStrncpy( ActiveMap, Map, ARRAY_COUNT(ActiveMap) );
		const INT PreviousIndex = RouteIndex;
		RouteIndex = INDEX_NONE;
		for( INT i = PreviousIndex+1; i < ARRAY_COUNT(Route); ++i )
		{
			if( !appStricmp(Route[i], Map) )
			{
				RouteIndex = i;
				break;
			}
		}
		if( RouteIndex == INDEX_NONE )
			for( INT i = 0; i <= PreviousIndex; ++i )
				if( !appStricmp(Route[i], Map) ) { RouteIndex=i; break; }
		if( RouteIndex == INDEX_NONE )
		{
			appErrorf( "DCSIM unexpected map %s", Map );
		}
		Frames = 0;
		GDCMemorySimState = RouteIndex + 1;
		debugf( "DCSIM map_begin index=%d map=%s", RouteIndex, Map );
	}

	if( ++Frames != DC_MEMORY_SIM_FRAMES_PER_MAP )
	{
		return;
	}

	if( RouteIndex + 1 == ARRAY_COUNT(Route) )
	{
		debugf( "DCSIM complete maps=%d", ARRAY_COUNT(Route) );
		GDCMemorySimState = DC_MEMORY_SIM_COMPLETE;
		return;
	}

	ALevelInfo* Info = GLevel->GetLevelInfo();
	appStrncpy( Info->NextURL, Route[RouteIndex + 1], ARRAY_COUNT(Info->NextURL) );
	Info->NextSwitchCountdown = 0.0f;
	debugf( "DCSIM travel from=%s to=%s", Map, Route[RouteIndex + 1] );
}

UBOOL appDCHasSessionTravel()
{
	return GDCSessionTravel;
}

const char* appDCGetSessionTravelURL()
{
	return GDCSessionTravel ? GDCSessionURL : NULL;
}

static void DCSetSessionTravel( ULevel* Level, const FURL& URL, const char* TravelItems )
{
	DCChizraCapture(Level);
	DCTerraniuxCapture(Level);
	FString URLText;
	URL.String( URLText );
	const char* Items = TravelItems ? TravelItems : "";
	if( appStrlen(*URLText) >= DC_SESSION_URL_BYTES )
		appErrorf( "Dreamcast session URL is too long (%d bytes)", appStrlen(*URLText) );
	if( appStrlen(Items) >= DC_SESSION_ITEMS_BYTES )
		appErrorf( "Dreamcast travel inventory is too large (%d bytes)", appStrlen(Items) );
	appStrcpy( GDCSessionURL, *URLText );
	appStrcpy( GDCSessionItems, Items );
	GDCSessionTravel = 1;
	debugf( "DCSESSION travel_pending url=%s items=%d", GDCSessionURL, appStrlen(GDCSessionItems) );
}

static void DCConsumeSessionTravel()
{
	debugf( "DCSESSION travel_restored url=%s items=%d", GDCSessionURL, appStrlen(GDCSessionItems) );
	GDCSessionTravel = 0;
	GDCSessionURL[0] = 0;
	GDCSessionItems[0] = 0;
}

static UBOOL DCCanRestartTravel( const FURL& URL )
{
	return !ParseParam(appCmdLine(),"NODCSESSIONRESTART")
		&& URL.IsLocalInternal()
		&& !URL.HasOption("push")
		&& !URL.HasOption("pop")
		&& !URL.HasOption("load")
		&& !URL.GetOption("load=",NULL)
		&& !URL.HasOption("failed");
}
#endif

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
static UBOOL DCDirectSessionStartup()
{
#if defined(PLATFORM_DREAMCAST)
	return GDCSessionTravel;
#else
	return ParseParam( appCmdLine(), "DCDIRECTSESSION" );
#endif
}
#endif

/*-----------------------------------------------------------------------------
	Temporary.
-----------------------------------------------------------------------------*/

void UGameEngine::PaintProgress()
{
	guard(PaintProgress);

	FVector LoadFog(0,.1,.25);
	FVector LoadScale(.2,.2,.2);
	UViewport* Viewport=Client->Viewports(0);
	Exchange(Viewport->Actor->FlashFog,LoadFog);
	Exchange(Viewport->Actor->FlashScale,LoadScale);
	Draw( Viewport, NULL, NULL );
	Exchange(Viewport->Actor->FlashFog,LoadFog);
	Exchange(Viewport->Actor->FlashScale,LoadScale);

	unguard;
}

INT UGameEngine::ChallengeResponse( INT Challenge )
{
	guard(UGameEngine::ChallengeResponse);
	return (Challenge*237) ^ (0x93fe92Ce) ^ (Challenge>>16) ^ (Challenge<<16);
	unguard;
}

/*-----------------------------------------------------------------------------
	Game init and exit.
-----------------------------------------------------------------------------*/

//
// Construct the game engine.
//
UGameEngine::UGameEngine()
: LastURL("")
{}

//
// Class creator.
//
void UGameEngine::InternalClassInitializer( UClass* Class )
{
	guard(UGameEngine::InternalClassInitializer);
	if( appStricmp(Class->GetName(),"GameEngine")==0 )
	{
		(new(Class,"ServerActors",  RF_Public)UStringProperty( CPP_PROPERTY(ServerActors  ), "Settings", CPF_Config, 96 ))->ArrayDim=16;
		(new(Class,"ServerPackages",RF_Public)UStringProperty( CPP_PROPERTY(ServerPackages), "Settings", CPF_Config, 96 ))->ArrayDim=16;
	}
	unguard;
}

//
// Initialize the game engine.
//
void UGameEngine::Init()
{
	guard(UGameEngine::Init);
	check(sizeof(*this)==GetClass()->GetPropertiesSize());

	// Call base.
	UEngine::Init();

	// Init variables.
	GLevel = NULL;
#if defined(PLATFORM_DREAMCAST)
	if( !GDCSessionTravel )
	{
		appMemset(GDCChizraStates,0,sizeof(GDCChizraStates));
		GDCChizraBaptistryOpen=0;
		appMemset(GDCTerraniuxPending,0,sizeof(GDCTerraniuxPending));
		appMemset(GDCSkyTownPending,0,sizeof(GDCSkyTownPending));
		appMemset(GDCTerraniuxEver,0,sizeof(GDCTerraniuxEver));
		GDCTerraniuxOpen=0;
	}
#endif
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	GDCGameLinkersReleased = 0;
	appDCSetLinkerTablesReleased( 0 );
#endif

	// Delete temporary files in cache.
	appCleanFileCache();

	// If not a dedicated server.
	if( GIsClient )
	{	
		// Init client.
		UClass* ClientClass = GObj.LoadClass( UClient::StaticClass, NULL, "ini:Engine.Engine.ViewportManager", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
		Client = ConstructClassObject<UClient>( ClientClass );
		Client->Init( this );

		// Init rendering.
		UClass* RenderClass = GObj.LoadClass( URenderBase::StaticClass, NULL, "ini:Engine.Engine.Render", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
		Render = ConstructClassObject<URenderBase>( RenderClass );
		Render->Init( this );
	}

	// Load the entry level.
	char Error256[256];
	if( Client
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		&& !DCDirectSessionStartup()
#endif
	)
	{
		if( !LoadMap( FURL("Entry"), NULL, Error256 ) )
			appErrorf( LocalizeError("LoadEntry"), Error256 );
		Exchange( GLevel, GEntry );
#if defined(PLATFORM_LOW_MEMORY) || defined(DC_RESOURCE_COOKER)
		// Purge unused objects and flush caches.
		Flush();
		GObj.CollectGarbage( GSystem, RF_Intrinsic );
#endif
	}

	// Create default URL.
	FURL DefaultURL;
	DefaultURL.GetConfigOptions( "DefaultPlayer" );

	// Enter initial world.
	char AutoURL[1024]="";
	ETravelType InitialTravel = TRAVEL_Partial;
#if defined(PLATFORM_DREAMCAST)
	if( GDCSessionTravel )
	{
		appStrncpy( AutoURL, GDCSessionURL, ARRAY_COUNT(AutoURL) );
		InitialTravel = TRAVEL_Absolute;
	}
	else
#endif
	{
		const char* Tmp = appCmdLine();
		if
		(	!ParseToken( Tmp, AutoURL, ARRAY_COUNT(AutoURL), 0 )
		||	AutoURL[0]=='-' )
			appStrcpy( AutoURL, *FURL::DefaultLocalMap );
	}
	FURL URL( &DefaultURL, AutoURL, InitialTravel );
	if( !URL.Valid )
		appErrorf( LocalizeError("InvalidUrl"), AutoURL );
#if defined(PLATFORM_DREAMCAST)
	if( GDCSessionTravel )
	{
		LastURL = URL;
		for( INT i = LastURL.Op.Num() - 1; i >= 0; --i )
			if( appStricmp( *LastURL.Op(i), "restart" ) == 0 )
				LastURL.Op.Remove( i );
	}
#endif
	UBOOL Success = Browse(
#if defined(PLATFORM_DREAMCAST)
		GDCSessionTravel ? URL :
#endif
		FURL(&LastURL,AutoURL,TRAVEL_Partial), Error256 );

	// If waiting for a network connection, go into the starting level.
	if( !Success && !Error256[0] && appStricmp( AutoURL, *FURL::DefaultLocalMap )!=0 )
		Success = Browse( FURL(&LastURL,*FURL::DefaultLocalMap,TRAVEL_Partial), Error256 );

	// Handle failure.
	if( !Success )
		appErrorf( LocalizeError("FailedBrowse"), AutoURL, Error256 );

	// Open initial Viewport.
	if( Client )
	{
		UViewport* Viewport = Client->NewViewport( GLevel, NAME_None );
		const char* InitialItems =
#if defined(PLATFORM_DREAMCAST)
			GDCSessionTravel ? GDCSessionItems :
#endif
			"";
#if defined(PLATFORM_DREAMCAST)
		APlayerPawn* SavedPlayer = NULL;
		if (GDCVMUReadySlot >= 0)
			for (INT i = 0; i < GLevel->Num(); ++i)
				if (GLevel->Actors(i) && !appStricmp(GLevel->Actors(i)->GetName(), GDCVMUPlayer))
					SavedPlayer = Cast<APlayerPawn>(GLevel->Actors(i));
		if (GDCVMUReadySlot >= 0 && !SavedPlayer)
			appErrorf("Saved player is absent from VMU snapshot");
		if (SavedPlayer)
		{
			SavedPlayer->SetPlayer(Viewport);
			GDCVMUReadySlot = -1;
		}
		else
#endif
			if (!GLevel->SpawnPlayActor(Viewport, ROLE_SimulatedProxy, URL, InitialItems, Error256))
			appErrorf(Error256);
#if defined(PLATFORM_DREAMCAST)
		if( GDCSessionTravel )
			DCConsumeSessionTravel();
#endif
		Viewport->Input->Init( Viewport, GSystem );
		UBOOL OpenRuntimeWindow = 1;
#if defined(DC_RESOURCE_COOKER)
		OpenRuntimeWindow = !ParseParam(appCmdLine(),"COOKSESSION")
			&& !ParseParam(appCmdLine(),"VERIFYSESSION")
			&& !ParseParam(appCmdLine(),"BAKEDCLIGHTMAPS");
		if( !OpenRuntimeWindow )
			debugf( "DCSESSION host_window_skipped" );
#endif
		if( OpenRuntimeWindow )
			Viewport->OpenWindow( NULL, 0, Client->ViewportX, Client->ViewportY, INDEX_NONE, INDEX_NONE );
#if defined(PLATFORM_DREAMCAST)
		// The initial Browse/LoadMap runs before this first viewport exists, so
		// its level-load hook cannot preload anything. Warm the actual game map
		// here, before the stream closes and before the first gameplay frame.
		if( Viewport->RenDev )
		{
			Viewport->RenDev->PreloadCookedLightmaps( GLevel );
			Viewport->RenDev->PreloadCookedAnimationFrames();
			Viewport->RenDev->PreloadCookedStaticTextures();
		}
#endif
		if( Audio )
			Audio->SetViewport( Viewport );
		if( GPendingLevel )
		{
			// Reprint connecting message.
			char Msg1[256], Msg2[256];
			appSprintf( Msg1, "Connecting (F10 Cancels):" );
			appSprintf( Msg2, "unreal://%s/%s", *URL.Host, *URL.Map );
			SetProgress( Msg1, Msg2, 60.0 );
		}
	}
#if defined(PLATFORM_DREAMCAST)
	else if( GDCSessionTravel )
	{
		// Dedicated sessions have no local viewport/player to receive inventory.
		DCConsumeSessionTravel();
	}
	if( appDCStreamActive() )
	{
		appDCStreamFinish();
		appDCStreamClose();
	}
	DCVMURefreshMenus();
	if( (!GLevel || !GLevel->NetDriver) && !GDCGameLinkersReleased )
	{
		DCReleaseGameLinkers();
	}
#elif defined(DC_RESOURCE_COOKER)
	if( DCDirectSessionStartup()
		&& (!GLevel || !GLevel->NetDriver)
		&& !GDCGameLinkersReleased )
	{
		DCReleaseGameLinkers();
	}
#endif
	debugf( NAME_Init, "Game engine initialized" );
	unguard;
}

//
// Game exit.
//
void UGameEngine::Destroy()
{
	guard(UGameEngine::Destroy);

	// Game exit.
	if( GPendingLevel )
		CancelPending();
	GLevel = NULL;
	debugf( NAME_Exit, "Game engine shut down" );

	UEngine::Destroy();
	unguard;
}

//
// Progress text.
//
void UGameEngine::SetProgress( const char* Str1, const char* Str2, FLOAT Seconds )
{
	guard(UGameEngine::SetProgress);
	if( Client && Client->Viewports.Num() )
	{
		APlayerPawn* Actor = Client->Viewports(0)->Actor;
		if( Seconds==-1.0 )
		{
			// Upgrade message.
			Actor->eventShowUpgradeMenu();
		}
		appStrncpy( Actor->ProgressMessage, Str1, ARRAY_COUNT(Actor->ProgressMessage) );
		appStrncpy( Actor->ProgressMessageTwo, Str2, ARRAY_COUNT(Actor->ProgressMessageTwo) );
		Actor->ProgressTimeOut = Actor->Level->TimeSeconds + Seconds;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Command line executor.
-----------------------------------------------------------------------------*/

//
// This always going to be the last exec handler in the chain. It
// handles passing the command to all other global handlers.
//
UBOOL UGameEngine::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UGameEngine::Exec);
	const char *Str = Cmd;
	if( ParseCommand( &Str, "OPEN" ) )
	{
		char Error256[256];
		if( !Browse( FURL(&LastURL,Str,TRAVEL_Partial), Error256 ) && Error256[0] )
			Out->Logf( "Open failed: %s", Error256 );
		return 1;
	}
	else if( ParseCommand( &Str, "START" ) )
	{
		char Error256[256];
		if( !Browse( FURL(&LastURL,Str,TRAVEL_Absolute), Error256 ) && Error256[0] )
			Out->Logf( "Start failed: %s", Error256 );
		return 1;
	}
#if defined(PLATFORM_DREAMCAST)
	else if (ParseCommand(&Str, "DCVMULOAD"))
	{
		char Target[64], Error[256];
		appSprintf(Target, "?load=%d", appAtoi(Str));
		Browse(FURL(&LastURL, Target, TRAVEL_Partial), Error);
		return 1;
	}
#endif
	else if (ParseCommand(&Str, "SAVEGAME"))
	{
		if (!GIsEditor && appIsDigit(Str[0]) && Str[1] == 0)
			SaveGame(appAtoi(Str));
		return 1;
	}
	else if( ParseCommand( &Cmd, "CANCEL" ) )
	{
		if( GPendingLevel )
			SetProgress( "Cancelled Connect Attempt", "", 2.0 );
		else
			SetProgress( "", "", 0.0 );
		CancelPending();
		return 1;
	}
	else if( GLevel && GLevel->Exec( Cmd, Out ) )
	{
		return 1;
	}
	else if( UEngine::Exec( Cmd, Out ) )
	{
		return 1;
	}
	else return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Serialization.
-----------------------------------------------------------------------------*/

//
// Serializer.
//
void UGameEngine::Serialize( FArchive& Ar )
{
	guard(UGameEngine::Serialize);
	UEngine::Serialize(Ar);

	Ar << GLevel << GEntry << GPendingLevel;

	unguardobj;
}

/*-----------------------------------------------------------------------------
	Game entering.
-----------------------------------------------------------------------------*/

//
// Cancel pending level.
//
void UGameEngine::CancelPending()
{
	guard(UGameEngine::CancelPending);
	if( GPendingLevel )
	{
		delete GPendingLevel;
		GPendingLevel = NULL;
	}
	unguard;
}

//
// Match Viewports to actors.
//
static void MatchViewportsToActors( UClient* Client, ULevel* Level, const FURL& URL )
{
	guard(MatchViewportsToActors);
	for( INT i=0; i<Client->Viewports.Num(); i++ )
	{
		char Error256[256]="";
		UViewport* Viewport = Client->Viewports(i);
		debugf( NAME_Log, "Spawning new actor for Viewport %s", Viewport->GetName() );
		if( !Level->SpawnPlayActor( Viewport, ROLE_SimulatedProxy, URL, Viewport->TravelItems, Error256 ) )
			appErrorf( Error256 );
		Viewport->TravelItems = "";
	}
	unguard;
}

//
// Browse to a specified URL, relative to the current one.
//
UBOOL UGameEngine::Browse( FURL URL, char* Error256 )
{
	guard(UGameEngine::Browse);
	check(Error256);
	Error256[0]=0;
	const char* Option;
#if defined(PLATFORM_DREAMCAST)
	// Existing campaign teleporters retain their legacy map names. Keep each
	// portal suffix while routing to the first half of a split map.
	const char* ResolvedMap=appDCResolveCampaignMap(*URL.Map);
	if( ResolvedMap!=*URL.Map ) URL.Map=ResolvedMap;
#endif

	// Crack the URL.
	FString UrlStr;
	const char* StringURL=NULL;
	guard(Message);
	URL.String(UrlStr);
	StringURL = *UrlStr;
	debugf( "Browse: %s", StringURL );
	unguard;
	if( !URL.Valid )
	{
		// Unknown URL.
		guard(UnknownURL);
		appSprintf( Error256, LocalizeError("InvalidUrl"), StringURL );
		unguard;
		return 0;
	}
	else if( URL.HasOption("failed") )
	{
		// Handle failure URL.
		guard(FailedURL);
		debugf( NAME_Log, LocalizeError("AbortToEntry") );
		GLevel = GEntry;
		GLevel->GetLevelInfo()->LevelAction = LEVACT_None;
		check(Client && Client->Viewports.Num());
		MatchViewportsToActors( Client, GLevel, URL );
		if( Audio && Client->Viewports.Num() )
			Audio->SetViewport( Client->Viewports(0) );
		GObj.CollectGarbage( GSystem, RF_Intrinsic );
		unguard;
		return 1;
	}
	else if( URL.HasOption("pop") )
	{
		// Pop the hub.
		guard(PopURL);
		if( GLevel && GLevel->GetLevelInfo()->HubStackLevel>0 )
		{
			char Filename[256], SavedPortal[256];
			appSprintf( Filename, "%s\\Game%i.usa", GSys->SavePath, GLevel->GetLevelInfo()->HubStackLevel-1 );
			appStrcpy( SavedPortal, *URL.Portal );
			URL = FURL( &URL, Filename, TRAVEL_Partial );
			URL.Portal = SavedPortal;
		}
		else return 0;
		unguard;
	}
	else if( URL.HasOption("restart") )
	{
		// Handle restarting.
		guard(RestartURL);
		URL = LastURL;
		unguard;
	}
	else if ((Option = URL.GetOption("load=", NULL)) != NULL)
	{
		// Handle restarting.
		guard(LoadURL);
		char Temp[256], Error256[256];
#if defined(PLATFORM_DREAMCAST)
		if (!GDCSessionTravel)
		{
			char Map[64], File[128], Failure[256];
			TArray<BYTE> State;
			if (!DCVMULoad(appAtoi(Option), Map, File, State, Failure))
			{
				SetProgress("VMU load failed", Failure, 8.f);
				debugf("DCVMU %s", Failure);
				return 0;
			}
			if (State.Num() != sizeof(FDCVMUState))
			{
				SetProgress("VMU load failed", "Incompatible campaign state", 8.f);
				return 0;
			}
			char Target[128];
			appSprintf(Target, "%s?load=%d", Map, appAtoi(Option));
			DCSetSessionTravel(GLevel, FURL(NULL, Target, TRAVEL_Absolute), "");
			if (!DCVMURestoreState(State))
			{
				GDCSessionTravel = 0;
				SetProgress("VMU load failed", "Invalid campaign state", 8.f);
				return 0;
			}
			appStrcpy(GDCVMUFile, File);
			GDCVMUReadySlot = appAtoi(Option);
			GIsRunning = 0;
			return 1;
		}
		if (GDCVMUReadySlot != appAtoi(Option))
			return 0;
		appSprintf(Temp, "%s?load", GDCVMUFile);
#else
		appSprintf(Temp, "%s\\Save%i.usa?load", GSys->SavePath, appAtoi(Option));
#endif
		if (LoadMap(FURL(&LastURL, Temp, TRAVEL_Partial), NULL, Error256))
		{
			// Copy the hub stack.
			INT i;
			for (i = 0; i < GLevel->GetLevelInfo()->HubStackLevel; i++)
			{
				char Src[256], Dest[256];
				appSprintf(Src, "%s\\Save%i%i.usa", GSys->SavePath, appAtoi(Option), i);
				appSprintf(Dest, "%s\\Game%i.usa", GSys->SavePath, i);
				appCopyFile(Src, Dest);
			}
			while (1)
			{
				appSprintf(Temp, "%s\\Game%i.usa", GSys->SavePath, i++);
				if (appFSize(Temp) <= 0)
					break;
				appUnlink(Temp);
			}
			LastURL = GLevel->URL;
			return 1;
		}
		else
		{
#if defined(PLATFORM_DREAMCAST)
			GDCVMUReadySlot = -1;
			GDCVMUPlayer[0] = 0;
			debugf("DCVMU snapshot load failed: %s", Error256);
#endif
			return 0;
		}
		unguard;
	}

	// Handle normal URL's.
	if( URL.IsLocalInternal() )
	{
		// Local map file.
		guard(LocalMapURL);
		return LoadMap( URL, NULL, Error256 )!=NULL;
		unguard;
	}
	else if( URL.IsInternal() && GIsClient && (!GLevel || !GLevel->NetDriver || GLevel->NetDriver->ServerConnection) )
	{
		// Network URL.
		guard(NetworkURL);
		if( GPendingLevel )
			CancelPending();
		char Msg1[256], Msg2[256];
		appSprintf( Msg1, "Connecting (F10 Cancels):" );
		appSprintf( Msg2, "unreal://%s/%s", *URL.Host, *URL.Map );
		SetProgress( Msg1, Msg2, 60.0 );
		GPendingLevel = new UPendingLevel( this, URL );
		if( !GPendingLevel->NetDriver )
		{
			SetProgress( "Networking Failed", GPendingLevel->Error256, 6.0 );
			delete GPendingLevel;
			GPendingLevel = NULL;
		}
		return 0;
		unguard;
	}
	else if( URL.IsInternal() )
	{
		// Invalid.
		guard(InvalidURL);
		appSprintf( Error256, LocalizeError("ServerOpen") );
		unguard;
		return 0;
	}
	else
	{
		// External URL.
		guard(ExternalURL);
		appLaunchURL( StringURL, "", Error256 );
		unguard;
		return 0;
	}
	unguard;
}

//
// Load a map.
//
ULevel* UGameEngine::LoadMap( const FURL& URL, UPendingLevel* Pending, char* Error256 )
{
	guard(UGameEngine::LoadMap);
	check(!GIsEditor);
	Error256[0]=0;
	FString Str;
	URL.String(Str);
	debugf( NAME_Log, "LoadMap: %s", *Str );
#if defined(PLATFORM_DREAMCAST)
	char DatPath[256];
	char MapName[128];
	appStrncpy( MapName, *URL.Map, ARRAY_COUNT(MapName) );
	char* Extension = appStrchr( MapName, '.' );
	if( Extension )
	{
		*Extension = 0;
	}
	snprintf( DatPath, sizeof(DatPath), "../Maps/%s.dat", MapName );
	if( !appDCStreamActive() )
		appDCOpenDat( DatPath );
#endif

	// Remember current level's stack level.
	INT SavedHubStackLevel = GLevel ? GLevel->GetLevelInfo()->HubStackLevel : 0;

	// Display loading screen.
	guard(LoadingScreen);
	if( Client && Client->Viewports.Num() && GLevel )
	{
		GLevel->GetLevelInfo()->LevelAction = LEVACT_Loading;
		PaintProgress();
		if( Audio )
			Audio->SetViewport( Client->Viewports(0) );
		GLevel->GetLevelInfo()->LevelAction = LEVACT_None;
	}
	unguard;

	// Verify that we can load all packages we need.
	FGuid* Guid = NULL;
	UObject* MapParent = NULL;
	guard(VerifyPackages);
	try
	{
		if( Pending )
		{
			UNetConnection* Connection = Pending->NetDriver->ServerConnection;
			for( INT i=0; i<Connection->Driver->Map.Num(); i++ )
				GObj.GetPackageLinker( Connection->Driver->Map(i).Parent, NULL, LOAD_Verify | LOAD_Throw | LOAD_KeepImports | LOAD_NoWarn, NULL, &Connection->Driver->Map(i).Guid );
			if( Connection->Driver->Map.Num() )
			{
				MapParent = Connection->Driver->Map(0).Parent;
				Guid = &Connection->Driver->Map(0).Guid;
			}
		}
		LoadObject<ULevel>( MapParent, "MyLevel", *URL.Map, LOAD_Verify | LOAD_Throw | LOAD_KeepImports | LOAD_NoWarn, NULL );
	}
	catch( char* Error )
	{
		// Safely failed loading.
		appStrcpy( Error256, Error );
		SetProgress( "Failed To Load Map", Error, 6.0 );
		return NULL;
	}
	unguard;

	// Dissociate Viewport actors.
	guard(DissociateViewports);
	if( Client )
	{
		for( INT i=0; i<Client->Viewports.Num(); i++ )
		{
			APlayerPawn* Actor          = Client->Viewports(i)->Actor;
			ULevel*      Level          = Actor->XLevel;
			Actor->Player               = NULL;
			Client->Viewports(i)->Actor = NULL;
			Level->DestroyActor( Actor );
		}
	}
	unguard;

	// Clean up game state.
	guard(ExitLevel);
	if( GLevel )
	{
		// Shut down.
		GObj.ResetLoaders( GLevel->GetParent() );
		if( GLevel->BrushTracker )
		{
			GLevel->BrushTracker->Exit();
			delete GLevel->BrushTracker;
			GLevel->BrushTracker = NULL;
		}
		if( GLevel->NetDriver )
		{
			delete GLevel->NetDriver;
			GLevel->NetDriver = NULL;
		}
		if( URL.HasOption("push") )
		{
			// Save the current level sans players actors.
			GLevel->CleanupDestroyed( 1 );
			char Filename[256];
			appSprintf( Filename, "%s\\Game%i.usa", GSys->SavePath, SavedHubStackLevel );
			GObj.SavePackage( GLevel->GetParent(), GLevel, 0, Filename );
		}
		GLevel = NULL;
		// Purge unused objects and flush caches.
		guard(CleanupAfterExit);
		Flush();
		GObj.CollectGarbage( GSystem, RF_Intrinsic );
		unguard;
	}
	unguard;

	// Load all packages we need.
	guard(LoadLevel);
	if( MapParent && Guid )
		GObj.GetPackageLinker( MapParent, NULL, LOAD_Verify | LOAD_Throw | LOAD_KeepImports | LOAD_NoWarn, NULL, Guid );
	GLevel = LoadObject<ULevel>( MapParent, "MyLevel", *URL.Map, LOAD_KeepImports | LOAD_NoFail, NULL );
#if defined(PLATFORM_DREAMCAST)
	appDCDatStats();
#endif
	check(!GLevel->NetDriver);
	unguard;

	// Setup network package info.
	if( Pending )
	{
		if( Pending->LonePlayer )
		{
			Pending = NULL;
		}
		else
		{
			Pending->NetDriver->ServerConnection->Driver->Map.Compute();
		}
	}

	// Verify classes.
	guard(VerifyClasses);
	VERIFY_CLASS_OFFSET( A, Actor,       Owner         );
	VERIFY_CLASS_OFFSET( A, Actor,       TimerCounter  );
	VERIFY_CLASS_OFFSET( A, PlayerPawn,  Player        );
	VERIFY_CLASS_OFFSET( A, PlayerPawn,  MaxStepHeight );
	unguard;

	// Get LevelInfo.
	check(GLevel);
	ALevelInfo* Info = GLevel->GetLevelInfo();
	appStrcpy( Info->ComputerName, GComputerName );

	// Handle pushing.
	guard(ProcessHubStack);
	Info->HubStackLevel
	=	URL.HasOption("load") ? Info->HubStackLevel
	:	URL.HasOption("push") ? SavedHubStackLevel+1
	:	URL.HasOption("pop" ) ? Max(SavedHubStackLevel-1,0)
	:	URL.HasOption("peer") ? SavedHubStackLevel
	:	                        0;
	unguard;

	// Handle pending level.
	guard(ActivatePending);
	if( Pending )
	{
		check(Pending==GPendingLevel);

		// Hook network driver up to level.
		GLevel->NetDriver         = Pending->NetDriver;
		GLevel->NetDriver->Notify = GLevel;

		// Setup level.
		GLevel->GetLevelInfo()->NetMode    = NM_Client;
		GLevel->GetLevelInfo()->bInternet  = GLevel->NetDriver->IsInternet();
	}
	else check(!GLevel->NetDriver);
	unguard;

	// Set level info.
	guard(InitLevel);
	if( !URL.GetOption("load",NULL) )
		GLevel->URL = URL;
	appStrncpy( Info->EngineVersion, "1.0", ARRAY_COUNT(Info->EngineVersion) );
	GLevel->Engine = this;
	unguard;

	// Purge unused objects and flush caches.
	guard(Cleanup);
	Flush();
	GObj.CollectGarbage( GSystem, RF_Intrinsic );
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	// Dynamic BSP needs source polygons only for moving brushes. The brush
	// tracker discarded all other polygon databases later, after GameInfo had
	// already caused a second export wave. Discard them before that peak.
	//
	// Release the editor brushes first: that orphans their models, so the
	// poly sweep below and the collect that follows reclaim the models and
	// their Zones-carrying UBspNodes as well, not just the polygons.
	DCDiscardEditorBrushes( GLevel );
	DCDiscardNonMoverPolys( GLevel );
	GObj.CollectGarbage( GSystem, RF_Intrinsic );
#endif
	unguard;

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	// The final session map and all mover source polygons are resident. Detach
	// package tables before GameInfo dependencies and BeginPlay allocations.
	if( GEntry
		&& !Pending
		&& !GLevel->NetDriver
		&& appDCStreamActive() )
		DCReleaseGameLinkers();
#endif
#if defined(PLATFORM_DREAMCAST)
	DumpMemStatsDC("post-linkers");
	// Reserve while the level model is still near the newest heap allocations.
	// The tracker makes the same call later, but at that point GameInfo and
	// BeginPlay have already fragmented the heap on large maps such as Dig.
	DCReserveMoverBsp( GLevel );
	DumpMemStatsDC("post-bsp-reserve");
#endif

	// Init collision.
	GLevel->SetActorCollision( 1 );

	// Setup zone distance table for sound damping.
	guard(SetupZoneTable);
	QWORD OldConvConn[64];
	QWORD ConvConn[64];
	INT i, j;
	for( i=0; i<64; i++ )
	{
		for ( INT j=0; j<64; j++ )
		{
			OldConvConn[i] = GLevel->Model->Nodes->Zones[i].Connectivity;
			if( i == j )
				GLevel->ZoneDist[i][j] = 0;
			else
				GLevel->ZoneDist[i][j] = 255;
		}
	}
	for( i=1; i<64; i++ )
	{
		for( j=0; j<64; j++ )
			for( INT k=0; k<64; k++ )
				if( (GLevel->ZoneDist[j][k] > i) && ((OldConvConn[j] & ((QWORD)1 << k)) != 0) )
					GLevel->ZoneDist[j][k] = i;
		for( j=0; j<64; j++ )
			ConvConn[j] = 0;
		for( j=0; j<64; j++ )
			for( INT k=0; k<64; k++ )
				if( (OldConvConn[j] & ((QWORD)1 << k)) != 0 )
					ConvConn[j] = ConvConn[j] | OldConvConn[k];
		for( j=0; j<64; j++ )
			OldConvConn[j] = ConvConn[j];
	}
	unguard;

	// Init the game info.
	char Options[1024]="";
	char Error256[256]="";
	char GameClassName[256]="";
	guard(InitGameInfo);
	for( INT i=0; i<URL.Op.Num(); i++ )
	{
		appStrcat( Options, "?" );
		appStrcat( Options, *URL.Op(i) );
		Parse( *URL.Op(i), "GAME=", GameClassName, ARRAY_COUNT(GameClassName) );
	}
	if( GLevel->IsServer() && !Info->Game )
	{
		// Get the GameInfo class.
		UClass* GameClass=NULL;
		if( !GameClassName[0] )
		{
			GameClass=Info->DefaultGameType;
			if( !GameClass )
				GameClass = GObj.LoadClass( AGameInfo::StaticClass, NULL, Client ? "ini:Engine.Engine.DefaultGame" : "ini:Engine.Engine.DefaultServerGame", NULL, LOAD_NoFail | LOAD_KeepImports, GLevel->GetSandbox() );
		}
		else GameClass = GObj.LoadClass( AGameInfo::StaticClass, NULL, GameClassName, NULL, LOAD_NoFail | LOAD_KeepImports, GLevel->GetSandbox() );

		// Spawn the GameInfo.
		debugf( NAME_Log, "Game class is '%s'", GameClass->GetName() );
		Info->Game = (AGameInfo*)GLevel->SpawnActor( GameClass );
		check(Info->Game!=NULL);
	}
	unguard;
	// Listen for clients.
	guard(Listen);
	if( !Client || URL.HasOption("Listen") )
	{
		char Error256[256];
		if( !GLevel->Listen( Error256 ) )
			appErrorf( LocalizeError("ServerListen"), Error256 );
	}
	unguard;

	// Init detail.
	Info->bHighDetailMode = 1;
	if
	(	Client
	&&	Client->Viewports.Num()
	&&	Client->Viewports(0)->RenDev
	&&	!Client->Viewports(0)->RenDev->HighDetailActors )
		Info->bHighDetailMode = 0;

	// Init level gameplay info.
	guard(BeginPlay);
	GLevel->iFirstDynamicActor = 0;
	if( !Info->bBegunPlay )
	{
		// Lock the level.
		debugf( NAME_Log, "Bringing %s up for play...", GLevel->GetFullName() );

		// Init touching actors.
		INT i;
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				for( INT j=0; j<ARRAY_COUNT(GLevel->Actors(i)->Touching); j++ )
					GLevel->Actors(i)->Touching[j] = NULL;

		// Handle network issues.
		if( !GLevel->IsServer() )
		{
			// Kill off actors that aren't interesting to the client.
			for( INT i=0; i<GLevel->Num(); i++ )
			{
				AActor* Actor = GLevel->Actors(i);
				if( Actor )
				{
					if( Actor->bStatic || Actor->bNoDelete )
						Exchange( Actor->Role, Actor->RemoteRole );
					else
						GLevel->DestroyActor( Actor );
				}
			}
		}

		// Init scripting.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->InitExecution();

		// Enable actor script calls.
		Info->bBegunPlay = 1;
		Info->bStartup = 1;

		// Init the game.
		if( Info->Game )
			Info->Game->eventInitGame( Options, Error256 );

		// Send PreBeginPlay.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventPreBeginPlay();

		// Set BeginPlay.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventBeginPlay();

		// Set zones.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->SetActorZone( GLevel->Actors(i), 1, 1 );

		// Post begin play.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventPostBeginPlay();

		// Begin scripting.
		for( i=0; i<GLevel->Num(); i++ )
			if( GLevel->Actors(i) )
				GLevel->Actors(i)->eventSetInitialState();

		// Find bases
		for( i=0; i<GLevel->Num(); i++ )
		{
			if( GLevel->Actors(i) && !GLevel->Actors(i)->Base && GLevel->Actors(i)->bCollideWorld 
				 && (GLevel->Actors(i)->IsA(ADecoration::StaticClass) || GLevel->Actors(i)->IsA(AInventory::StaticClass) || GLevel->Actors(i)->IsA(APawn::StaticClass)) 
				 &&	((GLevel->Actors(i)->Physics == PHYS_None) || (GLevel->Actors(i)->Physics == PHYS_Rotating)) )
			{
				 GLevel->Actors(i)->FindBase();
				 if ( GLevel->Actors(i)->Base == Info )
					 GLevel->Actors(i)->SetBase(NULL, 0);
			}
		}
		Info->bStartup = 0;
	}
	unguard;

	// Rearrange actors: static first, then others.
	guard(Rearrange);
	TArray<AActor*> Actors;
	Actors.AddItem(GLevel->Element(0));
	Actors.AddItem(GLevel->Element(1));
	INT i;
	for( i=2; i<GLevel->Num(); i++ )
		if( GLevel->Element(i) && GLevel->Element(i)->bStatic )
			Actors.AddItem( GLevel->Element(i) );
	GLevel->iFirstDynamicActor=Actors.Num();
	for( i=2; i<GLevel->Num(); i++ )
		if( GLevel->Element(i) && !GLevel->Element(i)->bStatic )
			Actors.AddItem( GLevel->Element(i) );
	GLevel->Empty();
	GLevel->Add( Actors.Num() );
	for( i=0; i<Actors.Num(); i++ )
		GLevel->Element(i) = Actors(i);
	Actors.Empty();
	unguard;

#if defined(PLATFORM_DREAMCAST)
	// Scripts have initialized their movers and pickups, but the moving-brush
	// tracker and arriving player are not yet present. Restore semantic state
	// here so the tracker builds collision from the final keyframe.
	if(!URL.HasOption("load"))
	{
		DCChizraRestore(GLevel);
		DCTerraniuxRestore(GLevel);
	}
#endif

	// Cleanup profiling.
#if DO_SLOW_GUARD
	guard(CleanupProfiling);
	for( TObjectIterator<UFunction> It; It; ++It )
		It->Calls = It->Cycles=0;
	GTicks=1;
	unguard;
#endif

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	if( ( GEntry || DCDirectSessionStartup() )
		&& !Pending && !GLevel->NetDriver && appDCStreamActive() )
		DCReleaseGameLinkers();
#endif

	// Client init.
	guard(ClientInit);
	if( Client )
	{
		// Match Viewports to actors.
		MatchViewportsToActors( Client, GLevel->IsServer() ? GLevel : GEntry, URL );

		// Reset input.
		for( INT i=0; i<Client->Viewports.Num(); i++ )
			Client->Viewports(i)->Input->ResetInput();

		// Init brush tracker.
#if defined(PLATFORM_DREAMCAST)
		DumpMemStatsDC("pre-movers");
#endif
		GLevel->BrushTracker = GNewBrushTracker( GLevel );
#if defined(PLATFORM_DREAMCAST)
		DumpMemStatsDC("post-movers");
#endif

		// Set up audio.
		if( Audio && Client->Viewports.Num()>0 )
			Audio->SetViewport( Client->Viewports(0) );
	}
	unguard;

	// Init detail.
	GLevel->DetailChange( Info->bHighDetailMode );

#if defined(PLATFORM_DREAMCAST)
	// PostLoad has captured the cooked frame slices and the final renderer
	// flush is over. Warm their VRAM bindings before gameplay can tick them.
	if( Client )
		for( INT i=0; i<Client->Viewports.Num(); ++i )
			if( Client->Viewports(i)->RenDev )
			{
				Client->Viewports(i)->RenDev->PreloadCookedLightmaps( GLevel );
				Client->Viewports(i)->RenDev->PreloadCookedAnimationFrames();
				Client->Viewports(i)->RenDev->PreloadCookedStaticTextures();
			}
#endif

	// Remember the URL.
	guard(RememberURL);
	LastURL = URL;
	unguard;

	// Successfully started local level.
#if defined(PLATFORM_DREAMCAST)
	if(!URL.HasOption("load"))
	{
		DCTerraniuxReplay(GLevel);
		DCSkyTownReplay(GLevel);
	}
#endif
	return GLevel;
	unguard;
}

/*-----------------------------------------------------------------------------
	Game Viewport functions.
-----------------------------------------------------------------------------*/

//
// Draw a global view.
//
void UGameEngine::Draw( UViewport* Viewport, BYTE* HitData, INT* HitSize )
{
	guard(UGameEngine::Draw);

	// Get view location.
	AActor*      ViewActor    = Viewport->Actor;
	FVector      ViewLocation = ViewActor->Location;
	FRotator     ViewRotation = ViewActor->Rotation;
	Viewport->Actor->eventPlayerCalcView( ViewActor, ViewLocation, ViewRotation );
	check(ViewActor);

	// See if viewer is inside world.
	DWORD LockFlags=0;
	FCheckResult Hit;
	if( !GLevel->Model->PointCheck(Hit,NULL,ViewLocation,FVector(0,0,0),0) )
		LockFlags |= LOCKR_ClearScreen;

	// Lock the Viewport.
	check(Render);
	FPlane FlashScale = Client->ScreenFlashes ? 0.5*Viewport->Actor->FlashScale : FVector(0.5,0.5,0.5);
	FPlane FlashFog   = Client->ScreenFlashes ? Viewport->Actor->FlashFog : FVector(0,0,0);
	FlashScale.X = Clamp( FlashScale.X, 0.f, 1.f );
	FlashScale.Y = Clamp( FlashScale.Y, 0.f, 1.f );
	FlashScale.Z = Clamp( FlashScale.Z, 0.f, 1.f );
	FlashFog.X   = Clamp( FlashFog.X  , 0.f, 1.f );
	FlashFog.Y   = Clamp( FlashFog.Y  , 0.f, 1.f );
	FlashFog.Z   = Clamp( FlashFog.Z  , 0.f, 1.f );
	if( !Viewport->Lock(FlashScale,FlashFog,FPlane(0,0,0,0),LockFlags,HitData,HitSize) )
	{
		debugf( NAME_Warning, "Couldn't lock Viewport for drawing" );
		return;
	}

	// Setup rendering coords.
	FMemMark SceneMark(GSceneMem);
	FSceneNode* Frame = Render->CreateMasterFrame( Viewport, ViewLocation, ViewRotation, NULL );

	// Update level audio.
	if( Audio )
	{
		uclock(GLevel->AudioTickCycles);
		Audio->Update( ViewActor->Region, Frame->Coords );
		uunclock(GLevel->AudioTickCycles);
	}
	FMemMark MemMark(GMem);
	FMemMark DynMark(GDynMem);

	// Render.
	Render->PreRender( Frame );
	if( Viewport->Console )
		Viewport->Console->PreRender( Frame );
	Viewport->Canvas->Update( Frame );
	Viewport->Actor->eventPreRender( Viewport->Canvas );
	if( Frame->X>0 && Frame->Y>0 )
		Render->DrawWorld( Frame );
	#if defined(PLATFORM_DREAMCAST)
	// The HUD and flash are TR. Even an empty viewport must advance past OP/PT.
	if( Viewport->RenDev->UsesOrderedLists() )
		Viewport->RenDev->BeginRenderPass( 2 );
	#endif
	Viewport->RenDev->EndFlash();
	Viewport->Actor->eventPostRender( Viewport->Canvas );
	if( Viewport->Console )
		Viewport->Console->PostRender( Frame );
	Render->PostRender( Frame );

	// Done.
	Viewport->Unlock( 1 );
	MemMark.Pop();
	DynMark.Pop();
	SceneMark.Pop();

	unguard;
}

void ExportTravel( FOutputDevice& Out, AActor* Actor )
{
	guard(ExportTravel);
	check(Actor);
	if( !Actor->bTravel )
		return;
	Out.Logf( "Class=%s Name=%s\r\n{\r\n", Actor->GetClass()->GetPathName(), Actor->GetName() );
	for( TFieldIterator<UProperty> It(Actor->GetClass()); It; ++It )
	{
		for( INT Index=0; Index<It->ArrayDim; Index++ )
		{
			char Value[1024];
			if
			(	(It->PropertyFlags & CPF_Travel)
			&&	It->ExportText( Index, Value, (BYTE*)Actor, &Actor->GetClass()->Defaults(0), 0 ) )
			{
				Out.Log( It->GetName() );
				if( It->ArrayDim!=1 )
					Out.Logf( "[%i]", Index );
				Out.Log( "=" );
				UObjectProperty* Ref = Cast<UObjectProperty>( *It );
				if( Ref && Ref->PropertyClass->IsChildOf(AActor::StaticClass) )
				{
					UObject* Obj = *(UObject**)( (BYTE*)Actor + It->Offset + Index*It->GetElementSize() );
					Out.Logf( "%s\r\n", Obj ? Obj->GetName() : "None" );
				}
				Out.Logf( "%s\r\n", Value );
			}
		}
	}
	Out.Logf( "}\r\n" );
	unguard;
}

//
// Jumping viewport.
//
void UGameEngine::SetClientTravel( UPlayer* Player, const char* NextURL, UBOOL bURL, UBOOL bItems, ETravelType TravelType )
{
	guard(UGameEngine::SetClientTravel);
	if( !Player && Client && Client->Viewports.Num() )
	{
		Player = Client->Viewports(0);
	}
	if( Player )
	{
		if( NextURL && appStricmp(NextURL,"?RESTART")==0 && Player->Actor && Player->Actor->CarryInfo )
		{
			// Automatically carry items the player had at start.
			Player->TravelItems = Player->Actor->CarryInfo->Text;
		}
		else if( bItems )
		{
			// Export items and self.
			FStringOut CarryInfo;
			ExportTravel( CarryInfo, Player->Actor );
			for( AActor* Inv=Player->Actor->Inventory; Inv; Inv=Inv->Inventory )
				ExportTravel( CarryInfo, Inv );
			Player->TravelItems = CarryInfo;
		}
		if( bURL && Cast<UViewport>(Player) )
		{
			// Set next URL.
			Cast<UViewport>(Player)->TravelURL = NextURL;
			Cast<UViewport>(Player)->TravelType = TravelType;
		}
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Tick.
-----------------------------------------------------------------------------*/

//
// Get tick rate limitor.
//
INT UGameEngine::GetMaxTickRate()
{
	guard(UEngine::GetMaxTickRate);
	if( GLevel && GLevel->NetDriver && !GLevel->NetDriver->ServerConnection )
		return GLevel->NetDriver->MaxTicksPerSecond;
	else
		return 0;
	unguard;
}

//
// Update everything.
//
void UGameEngine::Tick( FLOAT DeltaSeconds )
{
	guard(UGameEngine::Tick);
#if defined(PLATFORM_DREAMCAST)
	DCFrameBegin();
#endif
#if defined(PLATFORM_DREAMCAST)
	TickDCMemorySimulation();
#endif
	INT LocalTickCycles=0;
	uclock(LocalTickCycles);

	// If all viewports closed, time to exit.
	if( Client && Client->Viewports.Num()==0 )
	{
		debugf("All Windows Closed");
		appRequestExit();
		return;
	}

	// If game is paused, release the cursor.
	static UBOOL WasPaused=1;
	if( Client && Client->CaptureMouse && Client->Viewports.Num()==1 && GLevel && !Client->FullscreenViewport )
	{
		UBOOL IsPaused = (GLevel->GetLevelInfo()->Pauser[0]!=0) || (Client->Viewports(0)->Actor->bShowMenu);
		if( IsPaused && !WasPaused )
			Client->Viewports(0)->SetMouseCapture( 0, 0 );
		else if( WasPaused && !IsPaused )
			Client->Viewports(0)->SetMouseCapture( 1, 1, 1 );
		WasPaused = IsPaused;
	}
	else WasPaused=0;

	// Update subsystems.
	GObj.Tick();				
	GCache.Tick();

	// Update the level.
	guard(TickLevel);
	DC_FRAME_SCOPE(DCFS_Game);
	GameCycles=0;
	uclock(GameCycles);
	if( GLevel )
		GLevel->Tick( LEVELTICK_All, DeltaSeconds );
	if( Client && Client->Viewports.Num() && Client->Viewports(0)->Actor->XLevel!=GLevel )
		Client->Viewports(0)->Actor->XLevel->Tick( LEVELTICK_All, DeltaSeconds );
	uunclock(GameCycles);
	unguard;

	// Handle server travelling.
	guard(ServerTravel);
	if( GLevel && *GLevel->GetLevelInfo()->NextURL )
	{
		if( (GLevel->GetLevelInfo()->NextSwitchCountdown-=DeltaSeconds) <= 0.0 )
		{
			// Travel to new level, and exit.
			TArray<FString> TravelNames;
			TArray<FString> TravelItems;
			for( INT i=0; i<GLevel->Num(); i++ )
			{
				APlayerPawn* P = Cast<APlayerPawn>( GLevel->Element(i) );
				if( P && P->Player )
				{
					P->Player->TravelItems="";
					if( Cast<UNetConnection>(P->Player) )
						SetClientTravel( P->Player, GLevel->GetLevelInfo()->NextURL, 1, GLevel->GetLevelInfo()->bNextItems, TRAVEL_Relative );
					if( Cast<UViewport>(P->Player) )
					{
#if defined(PLATFORM_DREAMCAST)
						if( GLevel->GetLevelInfo()->bNextItems )
							SetClientTravel( P->Player, NULL, 0, 1, TRAVEL_Relative );
#endif
						Cast<UViewport>( P->Player )->TravelURL = "";
					}
					new(TravelNames)FString(P->PlayerName);
					new(TravelItems)FString(P->Player->TravelItems);
				}
			}
			debugf( "Server switch level: %s", GLevel->GetLevelInfo()->NextURL );
#if defined(PLATFORM_DREAMCAST)
			FURL SessionURL( &LastURL, GLevel->GetLevelInfo()->NextURL, TRAVEL_Relative );
			if( DCCanRestartTravel(SessionURL) )
			{
				const char* SessionItems = "";
				if( Client && Client->Viewports.Num() )
					SessionItems = *Client->Viewports(0)->TravelItems;
				DCSetSessionTravel( GLevel, SessionURL, SessionItems );
				*GLevel->GetLevelInfo()->NextURL = 0;
				GIsRunning = 0;
				return;
			}
#endif
			char Error256[256];
			Browse( FURL(&LastURL,GLevel->GetLevelInfo()->NextURL,TRAVEL_Relative), Error256 );
			*GLevel->GetLevelInfo()->NextURL = 0;
			GLevel->TravelNames = TravelNames;
			GLevel->TravelItems = TravelItems;
			return;
		}
	}
	unguard;

	// Handle client travelling.
	guard(ClientTravel);
	if( Client && Client->Viewports.Num() && Client->Viewports(0)->TravelURL!="" )
	{
		// Travel to new level, and exit.
		FString NextURL = Client->Viewports(0)->TravelURL;
		ETravelType TravelType = Client->Viewports(0)->TravelType;
		Client->Viewports(0)->TravelURL="";
#if defined(PLATFORM_DREAMCAST)
		FURL SessionURL( &LastURL, *NextURL, TravelType );
		if( DCCanRestartTravel(SessionURL) )
		{
			DCSetSessionTravel( GLevel, SessionURL, *Client->Viewports(0)->TravelItems );
			GIsRunning = 0;
			return;
		}
#endif
		char Error256[256];
		Browse( FURL(&LastURL,*NextURL,TravelType), Error256 );
		return;
	}
	unguard;

	// Update the pending level.
	guard(TickPending);
	if( GPendingLevel )
	{
		GPendingLevel->Tick( DeltaSeconds );
		if( GPendingLevel && GPendingLevel->Error256[0] )
		{
			// Pending connect failed.
			guard(PendingFailed);
			FString Str;
			GPendingLevel->URL.String( Str );
			debugf( NAME_Log, LocalizeError("Pending"), *Str, GPendingLevel->Error256 );
			delete GPendingLevel;
			GPendingLevel = NULL;
			//!!should convey this failure to the user by an in-game message.
			unguard;
		}
		else if( GPendingLevel->Success && !GPendingLevel->FilesNeeded && !GPendingLevel->SentJoin )
		{
			// Attempt to load the map.
			char Error256[256];
			guard(AttemptLoadPending);
			LoadMap( GPendingLevel->URL, GPendingLevel, Error256 );
			if( Error256[0] )
			{
				//!!report the error.
			}
			else if( !GPendingLevel->LonePlayer )
			{
				GPendingLevel->SentJoin = 1;
				GPendingLevel->NetDriver->ServerConnection->Logf( "JOIN" );
				GPendingLevel->NetDriver->ServerConnection->FlushNet();
				GPendingLevel->NetDriver = NULL;
				GLevel->GetLevelInfo()->LevelAction = LEVACT_Connecting;
				GEntry->GetLevelInfo()->LevelAction = LEVACT_Connecting;
			}
			unguard;

			// Kill the pending level.
			guard(KillPending);
			delete GPendingLevel;
			GPendingLevel = NULL;
			unguard;
		}
	}
	unguard;

	// Render everything.
	guard(ClientTick);
	INT LocalClientCycles=0;
	if( Client )
	{
		uclock(LocalClientCycles);
		Client->Tick();
		uunclock(LocalClientCycles);
	}
	ClientCycles=LocalClientCycles;
	unguard;

	uunclock(LocalTickCycles);
	TickCycles=LocalTickCycles;
	GTicks++;
#if defined(PLATFORM_DREAMCAST)
	DCFrameEnd();
#endif
	unguard;
}

/*-----------------------------------------------------------------------------
	Saving the game.
-----------------------------------------------------------------------------*/

//
// Save the current game state to a file.
//
void UGameEngine::SaveGame(INT Position)
{
	guard(UGameEngine::SaveGame);
	char Filename[256];
#if defined(PLATFORM_DREAMCAST)
	if (Position < 0 || Position > 9 || GLevel->NetDriver || GLevel->GetLevelInfo()->HubStackLevel)
	{
		SetProgress("VMU save failed", "Requires single-player, slot 0-9 and no active hub stack",
					8.f);
		return;
	}
	char VMUError[256];
	if (!DCVMUCanSave(VMUError))
	{
		SetProgress("VMU save failed", VMUError, 8.f);
		return;
	}
	appStrcpy(GSys->SavePath, "/ram");
	appSprintf(Filename, "/ram/%s.usa", GLevel->GetParent()->GetName());
#else
	appMkdir(GSys->SavePath);
	appSprintf(Filename, "%s\\Save%i.usa", GSys->SavePath, Position);
#endif
	GLevel->GetLevelInfo()->LevelAction = LEVACT_Saving;
	PaintProgress();
	GSystem->BeginSlowTask(LocalizeProgress("Saving"), 1, 0);
	if (GLevel->BrushTracker)
	{
		GLevel->BrushTracker->Exit();
		delete GLevel->BrushTracker;
	}
	GLevel->CleanupDestroyed(1);
	if (GObj.SavePackage(GLevel->GetParent(), GLevel, 0, Filename))
	{
#if defined(PLATFORM_DREAMCAST)
		TArray<BYTE> State;
		DCChizraCapture(GLevel);
		DCTerraniuxCapture(GLevel);
		DCVMUCaptureState(GLevel, State);
		char Error[256];
		if (DCVMUSave(Position, Filename, GLevel->GetParent()->GetName(), State, Error))
			SetProgress("VMU save complete", "Full world saved", 5.f);
		else
		{
			debugf("DCVMU save failed: %s", Error);
			SetProgress("VMU save failed", Error, 8.f);
		}
		DCVMURefreshMenus();
		// A newly-created snapshot is only staging. Keep a loaded snapshot
		// while the current level may still have file-backed readers.
		if (appStricmp(Filename, GDCVMUFile))
			appUnlink(Filename);
#else
		// Copy the hub stack.
		INT i;
		for( i=0; i<GLevel->GetLevelInfo()->HubStackLevel; i++ )
		{
			char Src[256], Dest[256];
			appSprintf( Src, "%s\\Game%i.usa", GSys->SavePath, i );
			appSprintf( Dest, "%s\\Save%i%i.usa", GSys->SavePath, Position, i );
			appCopyFile( Src, Dest );
		}
		while( 1 )
		{
			appSprintf( Filename, "%s\\Save%i%i.usa", GSys->SavePath, Position, i++ );
			if( appFSize(Filename)<=0 )
				break;
			appUnlink( Filename );
		}
#endif
	}
#if defined(PLATFORM_DREAMCAST)
	else
	{
		SetProgress("VMU save failed","Could not finalize RAM snapshot; previous VMU save kept",8.f);
		DCVMURefreshMenus();
	}
#endif
	for( INT i=0; i<GLevel->Num(); i++ )
		if( Cast<AMover>(GLevel->Actors(i)) )
			Cast<AMover>(GLevel->Actors(i))->SavedPos = FVector(-1,-1,-1);
	GLevel->BrushTracker = GNewBrushTracker( GLevel );
	GSystem->EndSlowTask();
	GLevel->GetLevelInfo()->LevelAction=LEVACT_None;

	unguard;
}

/*-----------------------------------------------------------------------------
	Mouse feedback.
-----------------------------------------------------------------------------*/

//
// Mouse delta while dragging.
//
void UGameEngine::MouseDelta( UViewport* Viewport, DWORD ClickFlags, FLOAT DX, FLOAT DY )
{
	guard(UGameEngine::MouseDelta);
	if( (ClickFlags & MOUSE_FirstHit) && Client && Client->Viewports.Num()==1 && GLevel && !Client->FullscreenViewport && GLevel->GetLevelInfo()->Pauser[0]==0 && !Viewport->Actor->bShowMenu )
	{
		Viewport->SetMouseCapture( 1, 1, 1 );
	}
	else if( (ClickFlags & MOUSE_LastRelease) && !Client->CaptureMouse )
	{
		Viewport->SetMouseCapture( 0, 0 );
	}
	unguard;
}

//
// Absolute mouse position.
//
void UGameEngine::MousePosition( UViewport* Viewport, DWORD ClickFlags, FLOAT X, FLOAT Y )
{
	guard(UGameEngine::MousePosition);
	unguard;
}

//
// Mouse clicking.
//
void UGameEngine::Click( UViewport* Viewport, DWORD ClickFlags, FLOAT X, FLOAT Y )
{
	guard(UGameEngine::Click);
	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
