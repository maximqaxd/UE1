/*=============================================================================
	UnMesh.cpp: Unreal mesh animation functions
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "EnginePrivate.h"
#include "UnRender.h"
#include "Amd3d.h"

#if defined(PLATFORM_DREAMCAST)
//
// (V - MeshOrigin).TransformPointBy(Coords) is three dot products after two
// subtractions. Both origins are constant for the whole mesh, so they fold
// into the translation column of a single 4x4 and each vertex becomes one
// FTRV. Row i is (Axis_i, -(MeshOrigin + Coords.Origin).Axis_i).
//
// XMTRX is a single global bank; it is loaded here and consumed immediately by
// the vertex loop below, with nothing in between that touches it.
//
static void DCLoadMeshCoords( const FCoords& Coords, const FVector& MeshOrigin )
{
	const FVector Base = MeshOrigin + Coords.Origin;
	shz_vec4_t R0, R1, R2, R3;
	R0.x = Coords.XAxis.X; R0.y = Coords.XAxis.Y; R0.z = Coords.XAxis.Z; R0.w = -(Base | Coords.XAxis);
	R1.x = Coords.YAxis.X; R1.y = Coords.YAxis.Y; R1.z = Coords.YAxis.Z; R1.w = -(Base | Coords.YAxis);
	R2.x = Coords.ZAxis.X; R2.y = Coords.ZAxis.Y; R2.z = Coords.ZAxis.Z; R2.w = -(Base | Coords.ZAxis);
	R3.x = 0.f;            R3.y = 0.f;            R3.z = 0.f;            R3.w = 1.f;
	shz_xmtrx_load_rows_4x4( &R0, &R1, &R2, &R3 );
}

static inline FVector DCTransformMeshVert( const FVector& V )
{
	shz_vec3_t In;
	In.x = V.X; In.y = V.Y; In.z = V.Z;
	const shz_vec3_t Out = shz_xmtrx_transform_point3( In );
	return FVector( Out.x, Out.y, Out.z );
}
#endif

/*-----------------------------------------------------------------------------
	UMesh object implementation.
-----------------------------------------------------------------------------*/

UMesh::UMesh()
{
	guard(UMesh::UMesh);

	// Scaling.
	Scale			= FVector(1,1,1);
	Origin			= FVector(0,0,0);
	RotOrigin		= FRotator(0,0,0);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	DCTemporalFrames = 0;
	DCClusteredRuns = 0;
#endif

	// Flags.
	AndFlags		= ~(DWORD)0;
	OrFlags			= 0;

	unguardobj;
}
void UMesh::Serialize( FArchive& Ar )
{
	guard(UMesh::Serialize);

	// Serialize parent.
	UPrimitive::Serialize(Ar);

	// Serialize this.
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	SerializeDCVerts( Ar );
	SerializeDCTopology( Ar );
	Ar << AnimSeqs;
#else
	Ar << Verts << Tris << AnimSeqs;
#endif
	Ar << Connects << BoundingBox << BoundingSphere << VertLinks << Textures;
	Ar << BoundingBoxes << BoundingSpheres;
	Ar << FrameVerts << AnimFrames;
	Ar << AndFlags << OrFlags;
	Ar << Scale << Origin << RotOrigin;
	Ar << CurPoly << CurVertex;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	if( Ar.IsLoading() )
	{
		ValidateDCMesh();
	}
#endif

	unguard;
}
IMPLEMENT_CLASS(UMesh);

/*-----------------------------------------------------------------------------
	UMesh collision interface.
-----------------------------------------------------------------------------*/

//
// Get the rendering bounding box for this primitive, as owned by Owner.
//
FBox UMesh::GetRenderBoundingBox( const AActor* Owner, UBOOL Exact ) const
{
	guard(UMesh::GetRenderBoundingBox);
	FBox Bound;

	// Get frame indices.
	INT iFrame1 = 0, iFrame2 = 0;
	const FMeshAnimSeq *Seq = GetAnimSeq( Owner->AnimSequence );
	if( Seq && Owner->AnimFrame>=0.0 )
	{
		// Animating, so use bound enclosing two frames' bounds.
		INT iFrame = appFloor((Owner->AnimFrame+1.0) * Seq->NumFrames);
		iFrame1    = Seq->StartFrame + ((iFrame + 0) % Seq->NumFrames);
		iFrame2    = Seq->StartFrame + ((iFrame + 1) % Seq->NumFrames);
		Bound      = BoundingBoxes(iFrame1) + BoundingBoxes(iFrame2);
	}
	else
	{
		// Interpolating, so be pessimistic and use entire-mesh bound.
		Bound = BoundingBox;
	}

	// Transform Bound by owner's scale and origin.
	FLOAT DrawScale = Owner->bParticles ? 1.5 : Owner->DrawScale;
	Bound = FBox( Scale*DrawScale*(Bound.Min - Origin), Scale*DrawScale*(Bound.Max - Origin) ).ExpandBy(1.0);
	FCoords Coords = GMath.UnitCoords / RotOrigin / Owner->Rotation;
	Coords.Origin  = Owner->Location + Owner->PrePivot;
	return Bound.TransformBy( Coords.Transpose() );
	unguardobj;
}

//
// Get the rendering bounding sphere for this primitive, as owned by Owner.
//
FSphere UMesh::GetRenderBoundingSphere( const AActor* Owner, UBOOL Exact ) const
{
	guard(UMesh::GetRenderBoundingSphere);
	return FSphere(0);
	unguardobj;
}

//
// Primitive box line check.
//
UBOOL UMesh::LineCheck
(
	FCheckResult	&Result,
	AActor			*Owner,
	FVector			End,
	FVector			Start,
	FVector			Extent,
	DWORD           ExtraNodeFlags
)
{
	guard(UMesh::LineCheck);
	if( Extent != FVector(0,0,0) )
	{
		// Use cylinder.
		return UPrimitive::LineCheck( Result, Owner, End, Start, Extent, ExtraNodeFlags );
	}
	else
	{
		// Could use exact mesh collision.
		// 1. Reject with local bound.
		// 2. x-wise intersection test with all polygons.
		return UPrimitive::LineCheck( Result, Owner, End, Start, FVector(0,0,0), ExtraNodeFlags );
	}
	unguardobj;
}

/*-----------------------------------------------------------------------------
	UMesh animation interface.
-----------------------------------------------------------------------------*/

//
// Get the transformed point set corresponding to the animation frame 
// of this primitive owned by Owner. Returns the total outcode of the points.
//
void UMesh::GetFrame
(
	FVector*	ResultVerts,
	INT			Size,
	FCoords		Coords,
	AActor*		Owner
)
{
	guard(UMesh::GetFrame);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	if( FrameVerts <= 0 )
	{
		return;
	}
#endif

	// Create or get cache memory.
	FCacheItem* Item;
	UBOOL WasCached = 1;
	QWORD CacheID   = MakeCacheID( CID_TweenAnim, Owner, NULL );
	BYTE* Mem       = GCache.Get( CacheID, Item );
	if( Mem==NULL || *(UMesh**)Mem!=this )
	{
		if( Mem != NULL )
		{
			// Actor's mesh changed.
			Item->Unlock();
			GCache.Flush( CacheID );
		}
		Mem = GCache.Create( CacheID, Item, sizeof(UMesh*) + sizeof(FLOAT) + sizeof(FName) + FrameVerts * sizeof(FVector) );
		WasCached = 0;
	}
	UMesh*& CachedMesh  = *(UMesh**)Mem; Mem += sizeof(UMesh*);
	FLOAT&  CachedFrame = *(FLOAT *)Mem; Mem += sizeof(FLOAT );
	FName&  CachedSeq   = *(FName *)Mem; Mem += sizeof(FName);
	if( !WasCached )
	{
		CachedMesh  = this;
		CachedSeq   = NAME_None;
		CachedFrame = 0.0;
	}

	// Get stuff.
	FLOAT    DrawScale      = Owner->bParticles ? 1.0 : Owner->DrawScale;
	FVector* CachedVerts    = (FVector*)Mem;
	Coords                  = Coords * (Owner->Location + Owner->PrePivot) * Owner->Rotation * RotOrigin * FScale(Scale * DrawScale,0.0,SHEER_None);
	const FMeshAnimSeq* Seq = GetAnimSeq( Owner->AnimSequence );

	// Transform all points into screenspace.
	if( Owner->AnimFrame>=0.0 || !WasCached )
	{
		// Compute interpolation numbers.
		FLOAT Alpha=0.0;
		INT iFrameOffset1=0, iFrameOffset2=0;
		if( Seq )
		{
			FLOAT Frame   = ::Max(Owner->AnimFrame,0.f) * Seq->NumFrames;
			INT iFrame    = appFloor(Frame);
			Alpha         = Frame - iFrame;
			iFrameOffset1 = (Seq->StartFrame + ((iFrame + 0) % Seq->NumFrames)) * FrameVerts;
			iFrameOffset2 = (Seq->StartFrame + ((iFrame + 1) % Seq->NumFrames)) * FrameVerts;
		}

		// Interpolate two frames.
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		FDCMeshFrameCursor Frame1( *this, iFrameOffset1 / FrameVerts );
		FDCMeshFrameCursor Frame2( *this, iFrameOffset2 / FrameVerts );
#else
		FMeshVert* MeshVertex1 = &Verts( iFrameOffset1 );
		FMeshVert* MeshVertex2 = &Verts( iFrameOffset2 );
#endif
#if defined(PLATFORM_DREAMCAST)
		// Loaded here, not earlier: the cursors above are constructed first
		// and XMTRX shares the back FP bank with 8-byte moves.
		DCLoadMeshCoords( Coords, Origin );
#endif
		for( INT i=0; i<FrameVerts; i++ )
		{
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FVector V1 = Frame1.Next();
			FVector V2 = Frame2.Next();
#else
			FVector V1( MeshVertex1[i].X, MeshVertex1[i].Y, MeshVertex1[i].Z );
			FVector V2( MeshVertex2[i].X, MeshVertex2[i].Y, MeshVertex2[i].Z );
#endif
			CachedVerts[i] = V1 + (V2-V1)*Alpha;
#if defined(PLATFORM_DREAMCAST)
			*ResultVerts = DCTransformMeshVert( CachedVerts[i] );
#else
			*ResultVerts = (CachedVerts[i] - Origin).TransformPointBy(Coords);
#endif
			*(BYTE**)&ResultVerts += Size;
		}
	}
	else
	{
		// Compute tweening numbers.
		FLOAT StartFrame = Seq ? (-1.0 / Seq->NumFrames) : 0.0;
		INT iFrameOffset = Seq ? Seq->StartFrame * FrameVerts : 0;
		FLOAT Alpha = 1.0 - Owner->AnimFrame / CachedFrame;
		if( CachedSeq!=Owner->AnimSequence || Alpha<0.0 || Alpha>1.0)
		{
			CachedSeq   = Owner->AnimSequence;
			CachedFrame = StartFrame;
			Alpha       = 0.0;
		}

		// Tween all points.
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		FDCMeshFrameCursor Frame( *this, iFrameOffset / FrameVerts );
#else
		FMeshVert* MeshVertex = &Verts( iFrameOffset );
#endif
#if defined(PLATFORM_DREAMCAST)
		DCLoadMeshCoords( Coords, Origin );
#endif
		for( INT i=0; i<FrameVerts; i++ )
		{
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FVector V2 = Frame.Next();
#else
			FVector V2( MeshVertex[i].X, MeshVertex[i].Y, MeshVertex[i].Z );
#endif
			CachedVerts[i] += (V2 - CachedVerts[i]) * Alpha;
#if defined(PLATFORM_DREAMCAST)
			*ResultVerts = DCTransformMeshVert( CachedVerts[i] );
#else
			*ResultVerts = (CachedVerts[i] - Origin).TransformPointBy(Coords);
#endif
			*(BYTE**)&ResultVerts += Size;
		}

		// Update cached frame.
		CachedFrame = Owner->AnimFrame;
	}
	Item->Unlock();
	unguardobj;
}

/*-----------------------------------------------------------------------------
	UMesh constructor.
-----------------------------------------------------------------------------*/

//
// UMesh constructor.
//
UMesh::UMesh( INT NumPolys, INT NumVerts, INT NumFrames )
{
	guard(UMesh::UMesh);

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	DCTemporalFrames = 0;
	DCClusteredRuns = 0;
#endif

	// Set counts.
	FrameVerts	= NumVerts;
	AnimFrames	= NumFrames;

	// Allocate all stuff.
	Tris			.Add(NumPolys);
	Verts			.Add(NumVerts * NumFrames);
	Connects		.Add(NumVerts);
	BoundingBoxes	.Add(NumFrames);
	BoundingSpheres	.Add(NumFrames);

	// Init textures.
	for( int i=0; i<Textures.Num(); i++ )
		Textures(i) = NULL;

	unguardobj;
}

/*-----------------------------------------------------------------------------
	The end.
-----------------------------------------------------------------------------*/
