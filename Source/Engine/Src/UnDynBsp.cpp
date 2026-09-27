/*=============================================================================
	UnDynBsp.cpp: Unreal dynamic Bsp object support
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	This code handles all moving brushes with a level.  These are objects which move
	once in a while and are added to and maintained within the level's Bsp whenever they 
	move.

Two forms of information are maintained about moving brushes during gameplay:

	1.	Permanent information.  Information that is allocated once for each moving
		brush: Bsp surfaces and vectors.  These records are updated as
		the brush moves, but they are not deleted/reallocated as the brush moves.  This info
		need only be allocated once because each brush requires a constant amount of this
		stuff, and the amount is known in advance.

	2.	Sporadic information.  Information that is deleted/reallocated for a brush each 
		time the brush moves: Bsp nodes, poly points, vertex pools.  This needs to be 
		reallocated dynamically because a brush requires a changing amount of it based on the 
		amount a moving brush is cut by the Bsp in its current position.

	Safely handles overflow conditions.

Implementation:

		Uses unlinked databases for all permanent and sporadic elements; any moving brush
		operation requires that each element of each database be traversed to see what needs
		to be updated.  This is fast because the databases and element sizes (WORDs) are small.
		New element allocation is performed via a roving feeler.

Major optimizations that could be performed:

	Store each brush as a brep with its own point and vector tables.  Then:
	- Preallocate space for all of the brep's points and vectors, so that only
	  points on clipped poly lines need to be sporadically maintained.
	- Point and vector usage will be minimized.
	- Duplicate points and vectors won't be transformed during rendering.
	- VertPools can be added with side linking.
	- Filtering work can be minimized by filtering the entire brep through the
	  bsp in one pass, instead of a per-poly pass.
	Store each brush as a Bsp and use tree merging.

This class uses the following members of other classes for purposes other than what
they are named/intended:

	* UModel::Color - First node index in the linked list of sporadic brush nodes.
	* FBspNode::iBound - Next node in the linked list of sporadic brush nodes.
	* FPoly::iLink - Bsp surface index corresponding to the FPoly
	* FPoly::iBrushPoly - Light mesh index that applies to the FPoly, or INDEX_NONE if none
	* FBspSurf::iActor - Index of actor whose moving brush owns the surface

Revision history:
	* Created by Tim Sweeney
=============================================================================*/

#include "EnginePrivate.h"

#define PRECOMPUTE_FILTER 0	/* Precompute sphere filter for optimization */
#define MallocArray(elements,type,descr) (type *)appMalloc((elements)*sizeof(type),descr)

/*---------------------------------------------------------------------------------------
	Brush class implementation.
---------------------------------------------------------------------------------------*/

void ABrush::InitPosRotScale()
{
	guard(ABrush::InitPosRotScale);
	check(Brush);
	
	MainScale = GMath.UnitScale;
	PostScale = GMath.UnitScale;
	Location  = FVector(0,0,0);
	Rotation  = FRotator(0,0,0);
	PrePivot  = FVector(0,0,0);

	unguard;
}
void ABrush::PostLoad()
{
	guard(ABrush::PostLoad);
	AActor::PostLoad();
	Location += PostPivot;//oldver
	PostPivot = FVector(0,0,0);
	unguard;
}
IMPLEMENT_CLASS(ABrush);

/*---------------------------------------------------------------------------------------
	Globals.
---------------------------------------------------------------------------------------*/

//
// Tracks moving brushes within a level.  One of these structures is kept for each 
// active level.  The structure is not saved with levels, but is rather rebuild at 
// load-time.
//
class FMovingBrushTracker : public FMovingBrushTrackerBase
{
public:

	///////////////////////////////////////
	// FMovingBrushTrackerBase interface //
	///////////////////////////////////////

	// Init/Exit.
	FMovingBrushTracker( ULevel *ThisLevel );
	~FMovingBrushTracker();

	// Lock/Unlock.
	void Lock();
	void Unlock();

	// Moving brush functions.
	void Flush( AActor *Actor );
	void Update( AActor *Actor );
	int  SurfIsDynamic( INT iSurf );

	///////////////////////////////////
	// FMovingBrushTracker interface //
	///////////////////////////////////

	// Operations that can be done while locked.
	void SetupAllBrushes();
	void RemoveAllBrushes();
	void UpdateBrushes(AActor **Actors,int Num);
	void Exit();

	// Private functions.
	inline int  NewPointIndex(AActor *Actor);
	inline int  NewVectorIndex(AActor *Actor);
	inline int  NewNodeIndex(AActor *Actor,INT iParent);
	inline int  NewSurfIndex(AActor *Actor);
	inline int  NewVertPoolIndex(AActor *Actor,int NumVerts);
	inline int	NewBrushMapIndex(AActor *Actor);

	inline void FreePointIndex(INT i);
	inline void FreeVectorIndex(INT i);
	inline void FreeNodeIndex(INT i);
	inline void FreeSurfIndex(INT i);
	inline void FreeVertPoolIndex(DWORD i,int NumVerts);
	inline void FreeBrushMapIndex(INT i);

	void SetupActorBrush(AActor *Actor);
	void RemoveActorBrush(AActor *Actor);

	void AddActorBrush(AActor *Actor);
	void FlushActorBrush(AActor *Actor,int Group);

	inline void ForceGroupFlush(INT iNode);

	void FilterFPoly( INT iNode, INT iCoplanarParent, FPoly *EdPoly, int Outside );
	void AddPolyFragment( INT iNode, INT iCoplanarParent, int IsFront, FPoly *EdPoly );

	///////////////////////////////////
	// FMovingBrushTracker internals //
	///////////////////////////////////

private:
#if defined(PLATFORM_LOW_MEMORY) || defined(DC_RESOURCE_COOKER)
	enum {MAX_MOVING_BRUSH_POLYS=2048};  // Maximum moving brush polys per level.
	enum {MAX_MOVING_BRUSH_ACTORS=256};  // Maximum moving brush actors per level.
	enum {MAX_TOUCHING_ACTORS=256};		 // Maximum actors touched by a moving brush during update.
#else
	enum {MAX_MOVING_BRUSH_POLYS=6000};  // Maximum moving brush polys per level.
	enum {MAX_MOVING_BRUSH_ACTORS=512};  // Maximum moving brush actors per level.
	enum {MAX_TOUCHING_ACTORS=512};		 // Maximum actors touched by a moving brush during update.
#endif

	ULevel*	Level;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	INT DCPeakNodes, DCPeakPoints, DCPeakVerts;
	void DCMeasure();
#endif
	FVector FPolyNormal;

	int iTopNode,iTopSurf,iTopPoint,iTopVector,iTopVertPool,iTopBrushMap;
	AActor *AddActor;
	INT iAddSurf;

	// Mechanism to pair actor indices with Bsp surfaces:
	AActor **BrushMapOwners;
	INT BrushMapCapacity;
	INT  *iBrushMapSurfs;
	INT  *iNodeParents;

	// Owner actor indices for things that are deleted and realllocated whenever a brush moves.
	AActor **NodeOwners;
	AActor **PointOwners;
	AActor **VertPoolOwners;

	// Owner actor indices for things that are only updated (not deleted/reallocated) whenever a brush moves.
	AActor **SurfOwners;
	AActor **VectorOwners;

	// List of actors to update in a multi-brush update.
	AMover* GroupActors[MAX_MOVING_BRUSH_ACTORS];
	int NumGroupActors;

	AActor* TouchActors[MAX_MOVING_BRUSH_ACTORS];
	int NumTouchActors;

	// Used by AddPolyFragment.
	INT* iActorNodePrevLink;
};

/*---------------------------------------------------------------------------------------
	Index management.
---------------------------------------------------------------------------------------*/

//
// Allocate an array of INDEX's for the elements in a database object from
// Num to Max.  These elements of level objects are reserved for use by moving 
// brush pieces.
//
INT* AllocDbIndex( UDatabase* Res, char* Descr )
{
	guard(FMovingBrushTracker::AllocDbIndex);
	if( Res->GetMax() == Res->GetNum() )
		return NULL;

	INT* Result = (INT *)MallocArray(Res->GetMax() - Res->GetNum(),INT,Descr);

	for( int i=Res->GetNum(); i<Res->GetMax(); i++ )
		Result[i - Res->GetNum()] = INDEX_NONE;

	return Result;

	unguard;
}
AActor** AllocDbActor( UDatabase* Res, char* Descr )
{
	guard(FMovingBrushTracker::AllocDbActor);
	if( Res->GetMax() == Res->GetNum() )
		return NULL;

	AActor** Result = (AActor **)MallocArray(Res->GetMax() - Res->GetNum(),AActor *,Descr);

	for( int i=Res->GetNum(); i<Res->GetMax(); i++ )
		Result[i - Res->GetNum()] = NULL;

	return Result;

	unguard;
}

//
// Shorten a database object's maximum element count to prevent
// moving brush data from trashing it as a sparse array.  Returns
// the number of active elements in the object.
//
static inline int ExpandDb( UDatabase* Res, int Slack=512 )
{
	guard(ExpandDb);

	Res->SetMax( Max( Res->GetMax(), 2*Res->GetNum() + Slack) );
	Res->Realloc();

	return Res->GetNum();

	unguard;
}

/*---------------------------------------------------------------------------------------
	Public functions.
---------------------------------------------------------------------------------------*/

//
// Flush an actor's brush.
//
void FMovingBrushTracker::Flush( AActor *Actor )
{
	guard(FMovingBrushTracker::Flush);
	FlushActorBrush( Actor, 0 );
	unguard;
}

//
// Update an actor's brush.
//
void FMovingBrushTracker::Update( AActor* Actor )
{
	guard(FMovingBrushTracker::Update);
	if( Actor->IsMovingBrush() )
		UpdateBrushes( &Actor, 1 );
	unguard;
}

//
// Return whether a surface belongs to a moving brush.
//
int FMovingBrushTracker::SurfIsDynamic( INT iSurf )
{
	guard(FMovingBrushTracker::SurfIsDynamic);
	
	if( iSurf<Level->Model->Surfs->Num() ) 
		return 0;
	return SurfOwners[iSurf - Level->Model->Surfs->Num()] != NULL;

	unguard;
}

/*---------------------------------------------------------------------------------------
	FMovingBrushTracker init & exit.
---------------------------------------------------------------------------------------*/

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
//
// Grow one model database to hold Extra more records, if it does not already.
//
static void DCGrowDb( UDatabase* Res, INT Extra )
{
	if( !Res )
		return;
	const INT Needed = Res->GetNum() + Extra;
	if( Res->GetMax() >= Needed )
		return;
	Res->SetMax( Needed );
	Res->Realloc();
}

//
// Reserve the dynamic-BSP headroom in the level model's arrays.
//
// Timing is the whole point. UDatabase::Realloc is an appRealloc over the
// entire array, not just the headroom, so growing Nodes by its mover budget
// transiently holds the old and the new copy at once -- around 2.4MB on
// Chizra. Done from the brush tracker that lands after the GameInfo wave and
// after the linker release has cut the free space into ~84 separate holes,
// and nothing is big enough. Done right after the map loads, the arrays are
// still the newest thing on the heap, where the allocator can extend in place.
//
// Idempotent, and the tracker still calls it, so a level that reaches the
// tracker without an early pass gets its headroom the old way.
//
ENGINE_API void DCReserveMoverBsp( ULevel* Level )
{
	guard(DCReserveMoverBsp);
	if( !Level || !Level->Model || !Level->Model->Nodes )
		return;

	INT NumMovers = 0, NumMoverPolys = 0;
	for( INT i = 0; i < Level->Num(); i++ )
	{
		AActor* Actor = Level->Actors(i);
		if( Actor && Actor->IsMovingBrush() )
		{
			++NumMovers;
			if( Actor->Brush && Actor->Brush->Polys )
				NumMoverPolys += Actor->Brush->Polys->Num();
		}
	}
	if( !NumMovers )
		return;

	INT Nodes = 0, Points = 0, Verts = 0;
	const char* Map = Level->GetParent()->GetName();
	const char* BudgetMap = !appStricmp(Map,"Dig1") || !appStricmp(Map,"Dig2") ? "Dig"
		: !appStricmp(Map,"Chizra1") || !appStricmp(Map,"Chizra2") ? "Chizra"
		: !appStricmp(Map,"DasaCellars1") || !appStricmp(Map,"DasaCellars2") ? "DasaCellars"
		: !appStricmp(Map,"Ruins1") || !appStricmp(Map,"Ruins2") ? "Ruins" : Map;
	// The two Terraniux halves were probed independently after collision-BSP
	// pruning. Keep the shared DCMover.ini unchanged so existing cooked streams
	// retain their resource fingerprint.
	const UBOOL Terra1=!appStricmp(Map,"Terraniux1");
	const UBOOL Terra2=!appStricmp(Map,"Terraniux2");
	const UBOOL Kran32A=!appStricmp(Map,"IsvKran32A");
	const UBOOL Kran32B=!appStricmp(Map,"IsvKran32B");
	const UBOOL SkyTown1=!appStricmp(Map,"SkyTown1");
	const UBOOL SkyTown2=!appStricmp(Map,"SkyTown2");
	if( Terra1 || Terra2 )
	{
		Nodes=Terra1 ? 1384 : 3148;
		Points=Terra1 ? 5948 : 13618;
		Verts=Terra1 ? 10500 : 24648;
	}
	if( Kran32A || Kran32B )
	{
		// Each pruned half was probed separately. The existing shared budget
		// file stays unchanged so other cooked streams keep their fingerprint.
		Nodes=Kran32A ? 3556 : 2802;
		Points=Kran32A ? 15568 : 11550;
		Verts=Kran32A ? 28088 : 21916;
	}
	if( SkyTown1 || SkyTown2 )
	{
		Nodes=SkyTown1 ? 4728 : 1048;
		Points=SkyTown1 ? 21216 : 4574;
		Verts=SkyTown1 ? 37492 : 8052;
	}
	if( (!(Terra1 || Terra2 || Kran32A || Kran32B || SkyTown1 || SkyTown2)
			&& (!GetConfigInt(BudgetMap, "Nodes", Nodes, "DCMover.ini")
				|| !GetConfigInt(BudgetMap, "Points", Points, "DCMover.ini")
				|| !GetConfigInt(BudgetMap, "Verts", Verts, "DCMover.ini")))
		|| Nodes <= 0 || Points <= NumMoverPolys || Verts <= 0
		|| Nodes > 1048576 || Points > 1048576 || Verts > 4194304 )
		appErrorf( "Missing/invalid measured mover budget for %s", Map );

	// SetupActorBrush allocates one surface and three vectors per mover poly.
	DCGrowDb( Level->Model->Nodes,   Nodes );
	DCGrowDb( Level->Model->Points,  Points );
	DCGrowDb( Level->Model->Verts,   Verts );
	DCGrowDb( Level->Model->Surfs,   NumMoverPolys + 32 );
	DCGrowDb( Level->Model->Vectors, 3*NumMoverPolys + 96 );
	unguard;
}
#endif

//
// Initialize or reinitialize everything, and allocate all working tables.  Must be
// followed by a call to UpdateAllBrushes to actually add moving brushes to the world
// Bsp.  This function assumes that the Bsp is clean when it is called, i.e. it has no
// references to dynamic Bsp nodes in it.
//
FMovingBrushTracker::FMovingBrushTracker( ULevel* ThisLevel )
{
	guard(FMovingBrushTracker::FMovingBrushTracker);

	Level				= ThisLevel;
#if !defined(PLATFORM_DREAMCAST) && !defined(DC_RESOURCE_COOKER)
	iTopNode			= ExpandDb(Level->Model->Nodes);
	iTopSurf			= ExpandDb(Level->Model->Surfs);
	iTopPoint			= ExpandDb(Level->Model->Points,16384);
	iTopVector			= ExpandDb(Level->Model->Vectors,16384);
	iTopVertPool		= ExpandDb(Level->Model->Verts);
	iTopBrushMap		= 0;
#endif

	// Note that all actors are unassimilated and count all movers.
	INT i;
	INT NumMovers = 0;
	INT NumMoverPolys = 0;
	for( i=0; i<Level->Num(); i++ )
	{
		AActor* Actor = Level->Actors(i);
		if( Actor )
		{
			Actor->bAssimilated = 0;
			if( Actor->IsMovingBrush() )
			{
				++NumMovers;
				if( Actor->Brush && Actor->Brush->Polys )
					NumMoverPolys += Actor->Brush->Polys->Num();
			}
		}
	}

	debugf( NAME_Init, "%s has %d moving brushes with %d polys", Level->GetFullName(), NumMovers, NumMoverPolys );

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	DCPeakNodes = DCPeakPoints = DCPeakVerts = 0;
	if( NumMovers >= MAX_MOVING_BRUSH_ACTORS || NumMoverPolys > MAX_MOVING_BRUSH_POLYS )
		appErrorf( "Mover tracker limits exceeded: %s", Level->GetFullName() );
	UBOOL Probe = 0;
#if defined(DC_RESOURCE_COOKER)
	Probe = ParseParam(appCmdLine(), "DCPROBEMOVERS");
#endif
	if( Probe && NumMovers )
	{
		ExpandDb(Level->Model->Nodes);
		ExpandDb(Level->Model->Surfs);
		ExpandDb(Level->Model->Points,16384);
		ExpandDb(Level->Model->Vectors,16384);
		ExpandDb(Level->Model->Verts);
	}
	else
	{
		// Normally already done just after the map loaded, and then this is a
		// no-op; see DCReserveMoverBsp for why the timing matters.
		DCReserveMoverBsp( Level );
	}
	iTopNode = Level->Model->Nodes->Num();
	iTopSurf = Level->Model->Surfs->Num();
	iTopPoint = Level->Model->Points->Num();
	iTopVector = Level->Model->Vectors->Num();
	iTopVertPool = Level->Model->Verts->Num();
	iTopBrushMap = 0;
#endif

	BrushMapCapacity = MAX_MOVING_BRUSH_POLYS;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	BrushMapCapacity = NumMovers ? Min(NumMoverPolys + 32, (INT)MAX_MOVING_BRUSH_POLYS) : 0;
#endif
	BrushMapOwners = BrushMapCapacity
		? (AActor**)MallocArray(BrushMapCapacity, AActor*, "BrushMapOwners") : NULL;
	for( i=0; i<BrushMapCapacity; i++ )
		BrushMapOwners[i] = NULL;

	iBrushMapSurfs = BrushMapCapacity
		? (INT*)MallocArray(BrushMapCapacity, INT, "BrushMapSurfs") : NULL;

	VertPoolOwners = AllocDbActor(Level->Model->Verts, "VertPoolOwners");
	for( i=0; i<(Level->Model->Verts->Max() - Level->Model->Verts->Num()); i++ )
		VertPoolOwners[i] = NULL;

	NodeOwners			= AllocDbActor( Level->Model->Nodes,   "NodeOwners"   );
	iNodeParents		= AllocDbIndex( Level->Model->Nodes,   "NodeParents"  );
	SurfOwners			= AllocDbActor( Level->Model->Surfs,   "SurfOwners"   );
	PointOwners			= AllocDbActor( Level->Model->Points,  "PointOwners"  );
	VectorOwners		= AllocDbActor( Level->Model->Vectors, "VectorOwners" );

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	const INT ExtraNodes = Level->Model->Nodes->Max() - Level->Model->Nodes->Num();
	const INT ExtraPoints = Level->Model->Points->Max() - Level->Model->Points->Num();
	const INT ExtraVerts = Level->Model->Verts->Max() - Level->Model->Verts->Num();
	const INT ExtraSurfs = Level->Model->Surfs->Max() - Level->Model->Surfs->Num();
	const INT ExtraVectors = Level->Model->Vectors->Max() - Level->Model->Vectors->Num();
	debugf( "DCBSPRESERVE map=%s nodes=%d points=%d verts=%d array_bytes=%d owner_bytes=%d",
		Level->GetParent()->GetName(), ExtraNodes, ExtraPoints, ExtraVerts,
		ExtraNodes * sizeof(FBspNode) + ExtraPoints * sizeof(FVector)
			+ ExtraVerts * sizeof(FVert) + ExtraSurfs * sizeof(FBspSurf)
			+ ExtraVectors * sizeof(FVector),
		(ExtraNodes + ExtraPoints + ExtraVerts + ExtraSurfs + ExtraVectors) * sizeof(AActor*)
			+ ExtraNodes * sizeof(INT) + BrushMapCapacity * (sizeof(AActor*) + sizeof(INT)) );
#endif

	debugf( NAME_Init, "Initialized moving brush tracker for %s", Level->GetFullName() );

	// Now setup and update all brushes.
	UpdateBrushes(NULL,0);

#if defined(DC_RESOURCE_COOKER)
	if( Probe || ParseParam(appCmdLine(), "DCTESTMOVERS") )
	{
		// Probe each key interval at eighths, with the other brushes in their
		// startup poses. This is a measured sample, not a proof of every state.
		for( INT a = 0; a < Level->Num(); ++a )
		{
			AMover* Mover = Cast<AMover>(Level->Actors(a));
			if( !Mover || !Mover->IsMovingBrush() )
				continue;
			FVector SavedLocation = Mover->Location;
			FRotator SavedRotation = Mover->Rotation;
			for( INT key = 0; key < Min((INT)Mover->NumKeys, 8); ++key )
			{
				INT next = Min(key + 1, Min((INT)Mover->NumKeys, 8) - 1);
				for( INT step = 0; step <= 8; ++step )
				{
					FLOAT Alpha = step / 8.f;
					Mover->Location = Mover->BasePos + Mover->KeyPos[key]
						+ (Mover->KeyPos[next] - Mover->KeyPos[key]) * Alpha;
					Mover->Rotation = Mover->BaseRot + Mover->KeyRot[key]
						+ (Mover->KeyRot[next] - Mover->KeyRot[key]) * Alpha;
					Update(Mover);
				}
			}
			Mover->Location = SavedLocation;
			Mover->Rotation = SavedRotation;
			Update(Mover);
		}
		debugf( "DCMOVERPEAK map=%s nodes=%d points=%d verts=%d polys=%d",
			Level->GetParent()->GetName(), DCPeakNodes, DCPeakPoints, DCPeakVerts, NumMoverPolys );
	}
#endif

	// Drop polys from all brushes that aren't attached to a mover.
	for( TObjectIterator<UModel> It; It; ++It )
	{
		if( It->Polys && It->Polys->Num() )
		{
			UBOOL NotMoving = true;
			for( INT i = 0; i < Level->Num(); ++i )
			{
				AActor* Owner = Level->Actors(i);
				if( Owner && Owner->IsMovingBrush() && Owner->Brush == *It )
				{
					NotMoving = false;
					break;
				}
			}
			if( NotMoving )
			{
				It->Polys->Empty();
				It->Polys = nullptr;
			}
		}
	}

#if CHECK_ALL
	// Make sure that multiple actors don't share the same brush.
	for( i=0; i<Level->Num(); i++ )
	{
		AActor* Actor1 = Level->Actors(i);
		if( Actor1 && Actor1->IsMovingBrush() )
		{
			for( int j=0; j<i; j++ )
			{
				AActor* Actor2 = Level->Actors(j);
				if( Actor2 && Actor2->IsMovingBrush() )
				{
					if( Actor1->Brush == Actor2->Brush )
						appErrorf( "Shared brush %s", Actor1->Brush->GetName() );
				}
			}
		}
	}
#endif

	unguard;
}

//
// Clear out the world and free all moving brush data.
//
void FMovingBrushTracker::Exit()
{
	guard(FMovingBrushTracker::Exit);

	// Remove all moving brushes.
	RemoveAllBrushes();

	unguard;
}
FMovingBrushTracker::~FMovingBrushTracker()
{
	guard(FMovingBrushTracker::~FMovingBrushTracker);

	// Free memory.
	if( BrushMapOwners )
		appFree(BrushMapOwners);
	if( iBrushMapSurfs )
		appFree(iBrushMapSurfs);
	if( NodeOwners )
		appFree(NodeOwners);
	if( iNodeParents )
		appFree(iNodeParents);
	if( SurfOwners )
		appFree(SurfOwners);
	if( PointOwners )
		appFree(PointOwners);
	if( VectorOwners )
		appFree(VectorOwners);
	if( VertPoolOwners )
		appFree(VertPoolOwners);

	debugf( NAME_Exit, "Shut down moving brush tracker for %s", Level->GetName() );

	unguard;
}

/*---------------------------------------------------------------------------------------
	Index functions.
---------------------------------------------------------------------------------------*/

//
// Get an index for a new actor.
//
inline int NewThingActor
(
	int&		TopThing, 
	const int&	NumThings,
	const int&	MaxThings,
	AActor**	ThingOwners,
	AActor*		Actor
)
{
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	if( MaxThings <= NumThings )
		appErrorf( "Mover reserve exhausted actor=%s capacity=0", Actor->GetFullName() );
	if( TopThing < NumThings || TopThing >= MaxThings )
		TopThing = NumThings;
#endif
#if CHECK_ALL
	if( (TopThing<NumThings) || (TopThing>=MaxThings) )
		appErrorf ("TopThing inconsistency %i<%i>%i",NumThings,TopThing,MaxThings);
#endif

	int   StartThing    = TopThing;
	AActor **ThingOwner = &ThingOwners[TopThing-NumThings];
	while( TopThing < MaxThings )
	{
		if( *ThingOwner == NULL )
		{
			*ThingOwner = Actor;
			return TopThing;
		}
		TopThing++;
		ThingOwner++;
	}
	TopThing    = NumThings;
	ThingOwner  = &ThingOwners[0];
	while( TopThing < StartThing )
	{
		if (*ThingOwner==NULL)
		{
			*ThingOwner = Actor;
			return TopThing;
		}
		TopThing++;
		ThingOwner++;
	}
#if CHECK_ALL || defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		appErrorf("Moving brush allocation exhausted: actor=%s first=%i max=%i",
			Actor->GetName(), NumThings, MaxThings);
#endif
	return INDEX_NONE;
}

/*---------------------------------------------------------------------------------------
	Routines to allocate new elements of particular types, for moving brush usage.
	These all call NewThingActor to do their work.
---------------------------------------------------------------------------------------*/

// Get a new Bsp node index.
inline int FMovingBrushTracker::NewNodeIndex( AActor* Actor, INT iParent )
{
	guardSlow(FMovingBrushTracker::NewNodeIndex);

	INT Result = NewThingActor( iTopNode, Level->Model->Nodes->Num(), Level->Model->Nodes->Max(), NodeOwners, Actor );

	if( Result!=INDEX_NONE )
	{
#if CHECK_ALL
		if( iNodeParents[Result - Level->Model->Nodes->Num()]!=INDEX_NONE )
			appError("Parent duplicate");
#endif
		iNodeParents[Result - Level->Model->Nodes->Num()] = iParent;
	}
	return Result;

	unguardSlow;
}

// Get a new Bsp surface index.
inline int FMovingBrushTracker::NewSurfIndex( AActor* Actor )
{
	guardSlow(FMovingBrushTracker::NewSurfIndex);
	return NewThingActor(iTopSurf,Level->Model->Surfs->Num(),Level->Model->Surfs->Max(),SurfOwners,Actor);
	unguardSlow;
}

// Get a new point index.
inline int FMovingBrushTracker::NewPointIndex( AActor* Actor )
{
	guardSlow(FMovingBrushTracker::NewPointIndex);
	return NewThingActor(iTopPoint,Level->Model->Points->Num(),Level->Model->Points->Max(),PointOwners,Actor);
	unguardSlow;
}

// Get a new vector index.
inline int FMovingBrushTracker::NewVectorIndex( AActor* Actor )
{
	guardSlow(FMovingBrushTracker::NewVectorIndex);
	return NewThingActor(iTopVector,Level->Model->Vectors->Num(),Level->Model->Vectors->Max(),VectorOwners,Actor);
	unguardSlow;
}

// Get a new brush-map index.
inline int FMovingBrushTracker::NewBrushMapIndex( AActor* Actor )
{
	guardSlow(FMovingBrushTracker::NewBrushMapIndex);
	return NewThingActor(iTopBrushMap,0,BrushMapCapacity,BrushMapOwners,Actor);
	unguardSlow;
}

// Get a new vertex pool index.
inline int FMovingBrushTracker::NewVertPoolIndex( AActor* Actor, int NumVerts )
{
	guardSlow(FMovingBrushTracker::NewVertPoolIndex);

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	const INT First = Level->Model->Verts->Num();
	const INT End = Level->Model->Verts->Max();
	if( NumVerts <= 0 || NumVerts > End - First )
		appErrorf( "Mover vertex reserve exhausted actor=%s request=%d capacity=%d",
			Actor->GetFullName(), NumVerts, End - First );
	const INT Start = Clamp(iTopVertPool, First, End);
	for( INT Pass = 0; Pass < 2; ++Pass )
	{
		INT Run = 0;
		for( INT Index = Pass ? First : Start; Index < End; ++Index )
		{
			Run = VertPoolOwners[Index - First] ? 0 : Run + 1;
			if( Run == NumVerts )
			{
				INT Result = Index + 1 - NumVerts;
				for( INT j = Result; j <= Index; ++j )
					VertPoolOwners[j - First] = Actor;
				iTopVertPool = Index + 1;
				return Result;
			}
		}
	}
	appErrorf( "Mover vertex reserve fragmented/full actor=%s request=%d capacity=%d",
		Actor->GetFullName(), NumVerts, End - First );
	return INDEX_NONE;
#else

#if CHECK_ALL
	if( (iTopVertPool<Level->Model->Verts->Num()) || (iTopVertPool>=Level->Model->Verts->Max()) )
		appError("TopThing inconsistency");
#endif

	int		iStart		= iTopVertPool;
	int		NumFree		= 0;
	AActor	**Owner		= &VertPoolOwners[iTopVertPool - Level->Model->Verts->Num()];

	while( iTopVertPool < (Level->Model->Verts->Max() - NumVerts) )
	{
		if( *Owner == NULL )
		{
			if( ++NumFree >= NumVerts )
			{
				while (NumFree-- > 0) *Owner-- = Actor;
				return iTopVertPool+1-NumVerts;
			}
		}
		else NumFree=0;

		iTopVertPool++;
		Owner++;
	}

	iTopVertPool	= Level->Model->Verts->Num();
	NumFree			= 0;
	Owner			= &VertPoolOwners[0];

	while( iTopVertPool < (iStart - NumVerts) )
	{
		if( *Owner == NULL )
		{
			if( ++NumFree >= NumVerts )
			{
				while (NumFree-- > 0) *Owner-- = Actor;
				return iTopVertPool+1-NumVerts;
			}
		}
		else NumFree=0;

		iTopVertPool++;
		Owner++;
	}
#if CHECK_ALL
		appError("NewVertPoolIndex overflow");
#endif
	return -1;
#endif
	unguardSlow;
}

/*---------------------------------------------------------------------------------------
	Functions to free things.
---------------------------------------------------------------------------------------*/

// Free a point index.
inline void FMovingBrushTracker::FreePointIndex( INT i )
{
#if CHECK_ALL
		if( PointOwners[i-Level->Model->Points->Num()]==NULL )
			appError("FreePointIndex inconsistency");
#endif
	PointOwners[i-Level->Model->Points->Num()] = NULL;
}

// Free a vector index.
inline void FMovingBrushTracker::FreeVectorIndex( INT i )
{
#if CHECK_ALL
		if( VectorOwners[i-Level->Model->Vectors->Num()]==NULL )
			appError("FreeVectorIndex inconsistency");
#endif
	VectorOwners[i-Level->Model->Vectors->Num()] = NULL;
}

// Free a Bsp node index.
inline void FMovingBrushTracker::FreeNodeIndex( INT i )
{
#if CHECK_ALL
		if( NodeOwners[i-Level->Model->Nodes->Num()]==NULL )
			appError("FreeNodeIndex node inconsistency");
		
		if( iNodeParents[i-Level->Model->Nodes->Num()]==NULL )
			appError("FreeNodeIndex parent inconsistency");
#endif
	NodeOwners  [i-Level->Model->Nodes->Num()] = NULL;
	iNodeParents[i-Level->Model->Nodes->Num()] = INDEX_NONE;
}

// Free a Bsp surface index.
inline void FMovingBrushTracker::FreeSurfIndex( INT i )
{
#if CHECK_ALL
		if( SurfOwners[i-Level->Model->Surfs->Num()]==NULL )
			appError("FreeSurfIndex inconsistency");
#endif
	SurfOwners[i-Level->Model->Surfs->Num()] = NULL;
}

// Free a vertex pool index.
inline void FMovingBrushTracker::FreeVertPoolIndex( DWORD i, int NumVerts )
{
	AActor **VertPoolOwner = &VertPoolOwners[i - Level->Model->Verts->Num()];
	for( int j=0; j<NumVerts; j++ )
	{
#if CHECK_ALL
			if( *VertPoolOwner == NULL )
				appError("FreeVertPoolIndex inconsistency");
#endif
		*VertPoolOwner++ = NULL;
	}
}

// Free a brush-map index.
inline void FMovingBrushTracker::FreeBrushMapIndex( INT i )
{
#if CHECK_ALL
	if( BrushMapOwners[i]==NULL )
		appError("FreeBrushMapIndex inconsistency");
#endif
	BrushMapOwners[i]=NULL;
}

/*---------------------------------------------------------------------------------------
	Private, permanent per brush operations.
---------------------------------------------------------------------------------------*/

//
// Setup permanenent information (surfaces, vectors, base points) for a moving
// brush.
//
void FMovingBrushTracker::SetupActorBrush( AActor* Actor )
{
	guard(FMovingBrushTracker::SetupActorBrush);

	check(!Actor->bAssimilated);
	check(Actor->IsMovingBrush());

	UModel* Brush = Actor->Brush;

	Actor->bAssimilated = 1;
	*(INT*)&Brush->MoverLink = INDEX_NONE;

	// Create permanent maps for all moving brush FPolys.
	FBspSurf* Surf;
	INT       iSurf;
	for( INT i=0; i<Brush->Polys->Num(); i++ )
	{
		FPoly *Poly = &Brush->Polys->Element(i);

		// Create new surface elements.
		int iBrushMap = NewBrushMapIndex(Actor);
		if( iBrushMap==-1 )
			goto Over1; // Safe overflow.

		iSurf = NewSurfIndex(Actor);
		if( iSurf==INDEX_NONE )
			goto Over2;

		iBrushMapSurfs[iBrushMap] = iSurf;
		Surf = &Level->Model->Surfs->Element(iSurf);

		Surf->vNormal = NewVectorIndex(Actor);
		if( Surf->vNormal==INDEX_NONE )
			goto Over3;

		Surf->vTextureU = NewVectorIndex(Actor);
		if( Surf->vTextureU==INDEX_NONE )
			goto Over4;

		Surf->vTextureV = NewVectorIndex(Actor);
		if( Surf->vTextureV==INDEX_NONE )
			goto Over5;

		Surf->pBase = NewPointIndex( Actor );
		if( Surf->pBase==INDEX_NONE )
			goto Over6;

		Surf->iLightMap = Poly->iBrushPoly; // May be INDEX_NONE.

		// Set all other surface properties.
		Surf->Texture  		= Poly->Texture;
		Surf->PanU 		 	= Poly->PanU;
		Surf->PanV 		 	= Poly->PanV;
		Surf->iBrushPoly	= i;
		Surf->Actor			= (ABrush*)Actor;
		Surf->PolyFlags 	= Poly->PolyFlags & ~PF_NoAddToBSP;
		if( Actor->bSpecialLit )
			Surf->PolyFlags |= PF_SpecialLit;

		// Link FPoly to this Bsp surface.
		Poly->iLink         = iSurf;

		// Go to next FPoly.
		continue;

		// Overflow cleanup.
		Over6:	FreeVectorIndex(Surf->vTextureV);
		Over5:	FreeVectorIndex(Surf->vTextureU);
		Over4:	FreeVectorIndex(Surf->vNormal);
		Over3:	FreeSurfIndex(iSurf);
				Level->Model->Surfs->Element(iSurf).iLightMap = INDEX_NONE;
		Over2:	FreeBrushMapIndex(iBrushMap);
		Over1:	;
#if CHECK_ALL
			appErrorf("Overflowed",Actor->GetClass()->GetName());
#endif
		break;
	}
#if CHECK_ALL
	debugf("SetupActorBrush for %s",Actor->GetClass()->GetName());
#endif

	unguard;
}

//
// Remove all permanent information (surfaces, vectors, base points) for a moving brush from 
// the level.
//
void FMovingBrushTracker::RemoveActorBrush( AActor* Actor )
{
	guard(FMovingBrushTracker::RemoveActorBrush);
	check(Actor->bAssimilated);

	// Find all surfaces owned by this actor, and free them and their contents.
	AActor **BrushMapOwner = BrushMapOwners;
	for( int i=0; i<BrushMapCapacity; i++ )
	{
		if( *BrushMapOwner == Actor )
		{
			// Free all stuff owned by this surface.
			INT iSurf = iBrushMapSurfs[i];
			FBspSurf* Surf = &Level->Model->Surfs->Element(iSurf);
#if CHECK_ALL
				if( Surf->vNormal  ==INDEX_NONE ) appError("Bad vNormal");
				if( Surf->vTextureU==INDEX_NONE ) appError("Bad vTextureU");
				if( Surf->vTextureV==INDEX_NONE ) appError("Bad vTextureV");
				if( Surf->pBase    ==INDEX_NONE ) appError("Bad pBase");
#endif
			FreeSurfIndex		(iSurf);
			FreePointIndex		(Surf->pBase);
			FreeVectorIndex		(Surf->vTextureV);
			FreeVectorIndex		(Surf->vTextureU);
			FreeVectorIndex		(Surf->vNormal);
			FreeBrushMapIndex	(i);
		}
		BrushMapOwner++;
	}
	Actor->bAssimilated = 0;
	unguard;
}

/*---------------------------------------------------------------------------------------
	Polygon filtering.
---------------------------------------------------------------------------------------*/

void FMovingBrushTracker::AddPolyFragment
(
	INT		iParent,
	INT		iCoplanarParent,
	int		IsFront,
	FPoly*	EdPoly
)
{
	guard(FMovingBrushTracker::AddPolyFragment);

	FBspNode	*Node,*Parent,*ZoneParent,*FirstParent;
	FVert		*VertPool;
	INT			iNode;
	int			i,iVertPool;

	int ZoneFront = IsFront;
	Parent = ZoneParent = FirstParent = &Level->Model->Nodes->Element(iParent);

	// If this node is meant to be added as a coplanar, handle it now.
	int Different=0;
	if( iCoplanarParent != INDEX_NONE )
	{
		Different = iParent!=iCoplanarParent;
		iParent = iCoplanarParent;
		IsFront = 2;
	}

	// Find parent.
	Parent = &Level->Model->Nodes->Element(iParent);
	FirstParent = Parent;
	if( IsFront == 2 ) // Coplanar
	{
		while( Parent->iPlane != INDEX_NONE )
		{
			iParent = Parent->iPlane;
			Parent = &Level->Model->Nodes->Element(iParent);
		}
	}

	// Create a new sporadic node.
	iNode = NewNodeIndex( AddActor, iParent );
	if( iNode == INDEX_NONE )
		goto Over1;
	Node = &Level->Model->Nodes->Element(iNode);

	// Set node's info.
	Node->iSurf       	  = iAddSurf;
	Node->iCollisionBound = INDEX_NONE;
	Node->iRenderBound    = INDEX_NONE;
	Node->ZoneMask		  = Parent->ZoneMask;
	Node->NumVertices	  = EdPoly->NumVertices;

	// Set node flags.
	Node->NodeFlags   	  = NF_IsNew;
	if( EdPoly->PolyFlags & PF_NotSolid              ) Node->NodeFlags |= NF_NotCsg;
	if( EdPoly->PolyFlags & (PF_Invisible|PF_Portal) ) Node->NodeFlags |= NF_NotVisBlocking;
	if( EdPoly->PolyFlags & PF_Masked                ) Node->NodeFlags |= NF_ShootThrough;

	if( iCoplanarParent==INDEX_NONE || Different )
	{
		Node->iZone[0] = Node->iZone[1]	= ZoneParent->iZone[ZoneFront];
		Node->iLeaf[0] = Node->iLeaf[1]	= ZoneParent->iLeaf[ZoneFront];
	}
	else
	{
		INT IsFlipped  = (Node->Plane|Parent->Plane)<0.0;
		Node->iZone[0] = Parent->iZone[IsFlipped  ]; Node->iLeaf[0] = Parent->iLeaf[IsFlipped  ];
		Node->iZone[1] = Parent->iZone[1-IsFlipped]; Node->iLeaf[1] = Parent->iLeaf[1-IsFlipped];
	}
	FirstParent->ZoneMask |= ((QWORD)1 << Node->iZone[0]) | ((QWORD)1 << Node->iZone[1]);

	Node->iFront		= INDEX_NONE;
	Node->iBack			= INDEX_NONE;
	Node->iPlane		= INDEX_NONE;

	// Make the node's plane from the base and normal.
	Node->Plane = FPlane( EdPoly->Base, EdPoly->Normal );

	// Allocate this node's vertex pool and vertices.
	iVertPool = NewVertPoolIndex(AddActor,EdPoly->NumVertices);
	if (iVertPool==MAXDWORD) goto Over2;

	Node->iVertPool = iVertPool;
	VertPool        = &Level->Model->Verts->Element(iVertPool);

	for( i=0; i<EdPoly->NumVertices; i++ )
	{
		INT pVertex = NewPointIndex(AddActor);
		if( pVertex==INDEX_NONE )
			goto Over3;

		VertPool->iSide		          = INDEX_NONE;
		VertPool->pVertex	          = pVertex;
		Level->Model->Points->Element(pVertex) = EdPoly->Vertex[i];

		VertPool++;
	}

	// Success; link this node to its parent and return.
	// (Can't fail past this point).

#if CHECK_ALL
	if		((IsFront==2)&&(Parent->iPlane!=INDEX_NONE)) appError("iPlane exists");
	else if ((IsFront==1)&&(Parent->iFront!=INDEX_NONE)) appError("iFront exists");
	else if ((IsFront==0)&&(Parent->iBack !=INDEX_NONE)) appError("iBack exists");
#endif

	if		(IsFront==2) Parent->iPlane = iNode;
	else if (IsFront==1) Parent->iFront = iNode;
	else				 Parent->iBack  = iNode;

	*iActorNodePrevLink = iNode;
	iActorNodePrevLink  = &Node->iRenderBound;

	return;

	// Overflow handlers.
	Over3:	while(--i >= 0) FreePointIndex((--VertPool)->pVertex);
	Over2:	FreeNodeIndex(iNode);
	Over1:	;
#if CHECK_ALL
		appError("Overflowed");
#endif
	unguard;
}

void FMovingBrushTracker::FilterFPoly
(
	INT		iNode, 
	INT		iCoplanarParent,
	FPoly*	EdPoly, 
	int		Outside
)
{
	FPoly		*TempFrontEdPoly, *TempBackEdPoly;
	FBspNode	*Node;
	FBspSurf	*Surf;
	int			SplitResult;

	TempFrontEdPoly	= new(GMem)FPoly;
	TempBackEdPoly	= new(GMem)FPoly;

	FilterLoop:
	Node  = &Level->Model->Nodes->Element(iNode);
	// A cooked split keeps opposite-half CSG decision planes for collision,
	// but removes their drawable surfaces. Movers still filter against those
	// planes, so never dereference iSurf when it is INDEX_NONE.
	Surf  = Node->iSurf != INDEX_NONE
		? &Level->Model->Surfs->Element(Node->iSurf) : NULL;

	if( EdPoly->NumVertices >= FPoly::VERTEX_THRESHOLD )
	{
		// Must split to avoid vertex overflow.
		TempFrontEdPoly = new(GMem)FPoly;
		EdPoly->SplitInHalf( TempFrontEdPoly );
		FilterFPoly( iNode, iCoplanarParent, TempFrontEdPoly, Outside );
	}
#if PRECOMPUTE_FILTER && !CHECK_ALL
	if( Node->NodeFlags & NF_IsFront )
		goto Front;
	else if( Node->NodeFlags & NF_IsBack )
		goto Back;
#endif
	SplitResult = EdPoly->SplitWithPlaneFast
	(
		Surf ? FPlane( Level->Model->Points->Element(Surf->pBase),
			Level->Model->Vectors->Element(Surf->vNormal) ) : Node->Plane,
		TempFrontEdPoly,
		TempBackEdPoly
	);
	if( SplitResult == SP_Front )
	{
#if CHECK_ALL && PRECOMPUTE_FILTER
		if( Node->NodeFlags & NF_IsBack )
			appError("Precompute error 1");
#endif
		Front:
		Outside = Outside || Node->IsCsg();
		if( Node->iFront != INDEX_NONE )
		{
			iNode = Node->iFront;
			goto FilterLoop;
		}
		else if( Outside )
			AddPolyFragment(iNode,iCoplanarParent,1,EdPoly);
	}
	else if( SplitResult == SP_Back )
	{
#if CHECK_ALL && PRECOMPUTE_FILTER
		if( Node->NodeFlags & NF_IsFront )
			appError("Precompute error 2");
#endif
		Back:
		Outside = Outside && !Node->IsCsg();
		if( Node->iBack != INDEX_NONE )
		{
			iNode = Node->iBack;
			goto FilterLoop;
		}
		else if( Outside )
			AddPolyFragment(iNode,iCoplanarParent,0,EdPoly);
	}
	else if( SplitResult == SP_Coplanar )
	{
#if CHECK_ALL && PRECOMPUTE_FILTER
		if( Node->NodeFlags & (NF_IsFront | NF_IsBack) )
			appError("Precompute error 3");
#endif
		const FVector PlaneNormal = Surf
			? Level->Model->Vectors->Element(Surf->vNormal)
			: FVector(Node->Plane.X,Node->Plane.Y,Node->Plane.Z);
		if( (PlaneNormal | FPolyNormal) >= 0.0 )
			iCoplanarParent = iNode;
		goto Front;
	}
	else if( SplitResult == SP_Split )
	{
#if CHECK_ALL && PRECOMPUTE_FILTER
		if( Node->NodeFlags & (NF_IsFront | NF_IsBack) )
			appError("Precompute error 4");
#endif

		// Handle front fragment.
		if( Node->iFront != INDEX_NONE )
			FilterFPoly( Node->iFront, iCoplanarParent, TempFrontEdPoly, Outside || Node->IsCsg() );
		else if( Outside || Node->IsCsg() )
			AddPolyFragment( iNode, iCoplanarParent, 1, TempFrontEdPoly );

		// Handle back fragment.
		EdPoly			= TempBackEdPoly;
		TempBackEdPoly	= new(GMem)FPoly;
		goto Back;
	}
}

/*---------------------------------------------------------------------------------------
	Private, sporadic per brush operations.
---------------------------------------------------------------------------------------*/

//
// Add all sporadic information (nodes, poly points) for a moving brush to the
// level.  Assumes that no Bsp nodes coming from this actor's brush already exist in the 
// level (those Bsp nodes, if any, must have already been cleaned out by FlushActorBrush).
//
void FMovingBrushTracker::AddActorBrush( AActor* Actor )
{
	guard(FMovingBrushTracker::AddActorBrush);
	check(Actor->bAssimilated);
	check(Actor->IsMovingBrush());

	FMemMark Mark(GMem);

	iActorNodePrevLink = (INT *)(&Actor->Brush->MoverLink);
	AddActor           = Actor;
	UModel *Brush      = Actor->Brush;

#if CHECK_ALL
	{
		// Verify that no Bsp nodes coming from this actor's brush already exist in the level.
		for( int i=Level->Model->Nodes->Num(); i<Level->Model->Nodes->Max(); i++ )
		{
			if( NodeOwners[i-Level->Model->Nodes->Num()]==Actor )
				appError( "Brush nodes already exist" );
		}

		// Verify that that each FPoly in the brush is referenced once and only once by BrushMapOwners.
		for( i=0; i<Brush->Polys->Num(); i++ )
		{
			int Found=0;
			for( int j=0; j<BrushMapCapacity; j++ )
			{
				if( BrushMapOwners[j]==Actor )
				{
					FBspSurf *Surf = &Level->Model->Surfs(iBrushMapSurfs[j]);
					if( Surf->iBrushPoly == i )
						Found++;
				}
			}
			if( Found != 1 )
				appErrorf("Surf/FPoly dissociation %i",Found);
		}
	}
#endif

	// Build coordinate system to transform Brush's original polygons into the world according
	// to the Brush's current rotation:
	FModelCoords Coords;
	FLOAT Orientation = ((ABrush*)Actor)->BuildCoords(&Coords,NULL);

	int Total=0;
	int i;
	for( i=0; i<Brush->Polys->Num(); i++ )
		Total += Brush->Polys->Element(i).NumVertices;

	FPoly	*TransformedPolys = new( GMem, Brush->Polys->Num() )FPoly;
	FVector *Points           = new( GMem, Total )FVector;

	int Count=0;
	for( i=0; i<Brush->Polys->Num(); i++ )
	{
		TransformedPolys[i] = Brush->Polys->Element(i);
		TransformedPolys[i].Transform( Coords, ((ABrush*)Actor)->PrePivot, Actor->Location, Orientation );
#if PRECOMPUTE_FILTER
		for( int j=0; j<TransformedPolys[i].NumVertices; j++ )
			Points[Count++] = TransformedPolys[i].Vertex[j];
#endif
	}
#if PRECOMPUTE_FILTER
	debug(Count==Total);
	FBox Bound( Total, Points );
	//Level->Model->PrecomputeSphereFilter( Bound.Sphere );
#endif

	// Go through list of all brush FPolys and update their corresponding Bsp surfaces.
	FPoly *Poly = &TransformedPolys[0];
	for( i=0; i<Brush->Polys->Num(); i++ )
	{
		INT      iSurf  = Poly->iLink;
		FBspSurf *Surf  = &Level->Model->Surfs->Element(iSurf);

#if CHECK_ALL
		if( SurfOwners[iSurf-Level->Model->Surfs->Num()] != Actor )
			appError( "iSurfOwner mismatch" );
#endif

		FPolyNormal = Poly->Normal;
		Level->Model->Points ->Element(Surf->pBase    ) = Poly->Base;
		Level->Model->Vectors->Element(Surf->vNormal  ) = Poly->Normal;
		Level->Model->Vectors->Element(Surf->vTextureU) = Poly->TextureU;
		Level->Model->Vectors->Element(Surf->vTextureV) = Poly->TextureV;

		// Filter the brush's FPoly through the Bsp, creating new sporadic Bsp nodes (and their 
		// corresponding VertPools and points) for all outside leaves the FPoly fragments fall into:
		guard(Filtering);
		{
			iAddSurf = iSurf;
			if( Level->Model->Nodes->Num() > 0 )
				FilterFPoly( 0, INDEX_NONE, Poly, 1 );

		}
		unguard;
		Poly++;
	}
	*iActorNodePrevLink = INDEX_NONE;

	// Tag all newly-added nodes as non-new.
	INT iNode = *(INT*)&Brush->MoverLink;
	while( iNode != INDEX_NONE )
	{
		FBspNode *Node   = &Level->Model->Nodes->Element(iNode);

#if CHECK_ALL
		if( NodeOwners[iNode-Level->Model->Nodes->Num()]!=Actor )
			appError( "Add node inconsistency" );
#endif

		Node->NodeFlags &= ~NF_IsNew;
		iNode = Node->iRenderBound;
	}

	Mark.Pop();
	unguard;
}

//
// Force the brush that owns a specified Bsp node to be flushed and later
// updated as part of a group flushing operation.
//
void FMovingBrushTracker::ForceGroupFlush( INT iNode )
{
	guard(FMovingBrushTracker::ForceGroupFlush);

	AActor* OwnerActor = NodeOwners[iNode - Level->Model->Nodes->Num()];
	if( OwnerActor == NULL )
		return;
	for( int i=0; i<NumGroupActors; i++ )
		if( GroupActors[i]==OwnerActor )
			return;
	if( NumGroupActors < MAX_MOVING_BRUSH_ACTORS )
		GroupActors[NumGroupActors++] = Cast<AMover>(OwnerActor);

	unguard;
}

//
// Flush all sporadic information (nodes, poly points) for a moving brush from 
// the level.  If Group is true, expects that NumGroupActors and GroupActors 
// are valid.
//
void FMovingBrushTracker::FlushActorBrush( AActor* Actor, INT Group )
{
	guard(FMovingBrushTracker::FlushActorBrush);
	check(Actor->bAssimilated);

	// Go through all sporadic nodes in the Bsp and find ones owned by this actor.
	INT iNode = Actor->Brush->MoverLink;
	while( iNode != INDEX_NONE )
	{
		FBspNode* Node   = &Level->Model->Nodes->Element(iNode);
		INT iParent      = iNodeParents[iNode-Level->Model->Nodes->Num()];
		FBspNode* Parent = &Level->Model->Nodes->Element(iParent);

#if CHECK_ALL
		if( NodeOwners[iNode-Level->Model->Nodes->Num()]!=Actor )
			appError( "Flush node inconsistency" );
#endif

		// Remove references to this node from its parents.
		if	   ( Parent->iFront==iNode ) Parent->iFront=INDEX_NONE;
		else if( Parent->iBack ==iNode ) Parent->iBack =INDEX_NONE;
		else if( Parent->iPlane==iNode ) Parent->iPlane=INDEX_NONE;
#if CHECK_ALL
		else appError( "Parent mismatch" );
#endif
		// If those node has any children belonging to moving brushes that
		// aren't in our group list, tag them for subsequent flushing. This
		// prevents them from being orphaned.
		if( Group )
		{
			if( Node->iFront!=INDEX_NONE )
				ForceGroupFlush( Node->iFront );
			if( Node->iBack !=INDEX_NONE )
				ForceGroupFlush( Node->iBack );
			if( Node->iPlane!=INDEX_NONE )
				ForceGroupFlush( Node->iPlane );
		}

		// Free all sporadic data.
		FVert* VertPool = &Level->Model->Verts->Element(Node->iVertPool);
		for( DWORD j=0; j<Node->NumVertices; j++ )
		{
			FreePointIndex( VertPool->pVertex );
			VertPool++;
		}
		FreeVertPoolIndex( Node->iVertPool, Node->NumVertices );
		FreeNodeIndex( iNode );
		iNode = Node->iRenderBound;
	}
	Actor->Brush->MoverLink = INDEX_NONE;
	unguard;
}

/*---------------------------------------------------------------------------------------
	Public operations.
---------------------------------------------------------------------------------------*/


//
// Setup all brushes in the level.  Cleans out all permanent and sporadic data, then
// sets up permanent data (surfaces, vectors) for all moving brushes.  Sporadic data
// isn't added until the next call to UpdateAllBrushes.
//
void FMovingBrushTracker::SetupAllBrushes()
{
	guard(FMovingBrushTracker::SetupAllBrushes);

	RemoveAllBrushes();

	for( int i=0; i<Level->Num(); i++ )
	{
		AActor *Actor = Level->Actors(i);
		if( Actor && Actor->IsMovingBrush() )
			SetupActorBrush( Actor );
	}
	unguard;
}

//
// Remove all permanent and sporadic moving brush data.
// Note that this must be called before a level is saved, or else Bsp nodes that
// should be leaves will reference nodes in the range from Num-Max that don't exist 
// in the saved file resulting.
//
void FMovingBrushTracker::RemoveAllBrushes()
{
	guard(FMovingBrushTracker::RemoveAllBrushes);

	// Flush all sporadic data.
	guard(FlushSporadics);
	for( INT i=0; i<Level->Num(); i++ )
	{
		AActor* Actor = Level->Actors(i);
		if( Actor && Actor->IsMovingBrush() && Actor->bAssimilated )
			FlushActorBrush( Actor, 0 );
	}
	unguard;

	// Verify that all dynamic nodes were actually removed.
#if CHECK_ALL
	guard(CheckSporadics);
	INT n = Level->Model->Nodes->Num();
	for( INT i=0; i<n; i++ )
	{
		FBspNode* Node = &Level->Model->Nodes(i);
		if( Node->iFront!=INDEX_NONE && Node->iFront>=n )
			appErrorf( "Bad iFront %i",i );
		if( Node->iBack !=INDEX_NONE && Node->iBack>=n )
			appErrorf( "Bad iBack %i", i );
		if( Node->iPlane!=INDEX_NONE && Node->iPlane>=n )
			appErrorf( "Bad iPlane %i",i );
	}
	unguard;
#endif

	// Remove all permanent data.
	guard(RemoveSporadics);
	for( INT i=0; i<Level->Num(); i++ )
	{
		AActor *Actor = Level->Actors(i);
		if( Actor && Actor->IsMovingBrush() && Actor->bAssimilated )
			RemoveActorBrush(Actor);
	}
	unguard;
	unguard;
}

//
// Update certain moving brushes in the level.  Call with Actors=NULL to update all moving
// brushes, or with a list of specific brushes to be updated in lock-step.  All of the
// specified moving brushes (and brushes interwoved with their Bsp's) are cleaned out of the 
// Bsp, and then re-added.
//
void FMovingBrushTracker::UpdateBrushes( AActor** Actors, int Num )
{
	guard(FMovingBrushTracker::UpdateBrushes);
	INT Group;

	if( Actors == NULL )
	{
		// Update all actor brushes.
		Group = 0;
		NumGroupActors = 0;
		for( INT iActor=0; iActor<Level->Num(); iActor++ )
		{
			AActor* Actor = Level->Actors(iActor);
			if( Actor && Actor->IsMovingBrush() )
			{
				GroupActors[NumGroupActors++] = Cast<AMover>(Actor);
				if( NumGroupActors >= MAX_MOVING_BRUSH_ACTORS )
					break;
			}
		}
	}
	else
	{
		// Update specified actor brushes.
		Group = 1;
		NumGroupActors = Min(Num,(int)MAX_MOVING_BRUSH_ACTORS);
		appMemcpy( GroupActors, Actors, NumGroupActors * sizeof(AActor *) );
	}

	// Init touch actors so that they can be added back later.
	NumTouchActors = 0;

	// Eliminate actors that haven't moved.
	INT i;
	for( i=0; i<NumGroupActors; i++ )
	{
		AMover* Actor = GroupActors[i];
		// Saved transforms survive serialization, but a new tracker owns no brush
		// geometry yet. Only an already-assimilated brush can skip insertion.
		if( Actor->bAssimilated && Actor->SavedPos==Actor->Location && Actor->SavedRot==Actor->Rotation )
		{
			GroupActors[i] = NULL;
		}
		else
		{
			Actor->SavedPos = Actor->Location;
			Actor->SavedRot = Actor->Rotation;
		}
	}

	// Flush all actor brushes.  Calls to FlushActorBrush with Group set to true
	// cause GroupActors to be expanded to include all brushes interwoven with the
	// specified brushes.
	for( i=0; i<NumGroupActors; i++ )
	{
		AActor* Actor = GroupActors[i];
		if( Actor && Actor->IsMovingBrush() && Actor->bAssimilated )
			FlushActorBrush( Actor, Group );
	}

	// Add all moving brush sporadic data to the world.
	for( i=0; i<NumGroupActors; i++ )
	{
		AActor* Actor = GroupActors[i];
		if( Actor && Actor->IsMovingBrush() )
		{
			if( !Actor->bAssimilated )
				SetupActorBrush( Actor );
			AddActorBrush( Actor );
		}
	}
#if defined(DC_RESOURCE_COOKER)
	if( ParseParam(appCmdLine(), "DCPROBEMOVERS") || ParseParam(appCmdLine(), "DCTESTMOVERS") )
		DCMeasure();
#endif
	unguard;
}

/*---------------------------------------------------------------------------------------
	Instantiation.
---------------------------------------------------------------------------------------*/

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
void FMovingBrushTracker::DCMeasure()
{
	INT Nodes = 0, Points = 0, Verts = 0;
	for( INT i = 0; i < Level->Model->Nodes->Max() - Level->Model->Nodes->Num(); ++i )
		Nodes += NodeOwners[i] != NULL;
	for( INT i = 0; i < Level->Model->Points->Max() - Level->Model->Points->Num(); ++i )
		Points += PointOwners[i] != NULL;
	for( INT i = 0; i < Level->Model->Verts->Max() - Level->Model->Verts->Num(); ++i )
		Verts += VertPoolOwners[i] != NULL;
	DCPeakNodes = Max(DCPeakNodes, Nodes);
	DCPeakPoints = Max(DCPeakPoints, Points);
	DCPeakVerts = Max(DCPeakVerts, Verts);
}
#endif

ENGINE_API FMovingBrushTrackerBase* GNewBrushTracker( ULevel* InLevel )
{
	guard(GNewBrushTracker);
	return new FMovingBrushTracker( InLevel );
	unguard;
}

/*---------------------------------------------------------------------------------------
	The End.
---------------------------------------------------------------------------------------*/
