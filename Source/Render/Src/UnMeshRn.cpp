/*=============================================================================
	UnMeshRn.cpp: Unreal mesh rendering.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "RenderPrivate.h"
#include "UnDCFrameProfile.h"

/*------------------------------------------------------------------------------
	Globals.
------------------------------------------------------------------------------*/

UBOOL               HasSpecialCoords;
FCoords             SpecialCoords;
static FLOAT        UScale, VScale;
static UTexture*    Textures[16];
static FTextureInfo TextureInfo[16];
static FTextureInfo EnvironmentInfo;
static FVector      GUnlitColor;

/*------------------------------------------------------------------------------
	Environment mapping.
------------------------------------------------------------------------------*/

static void EnviroMap( FSceneNode* Frame, FTransTexture& P )
{
#if defined(PLATFORM_DREAMCAST)
	// UnsafeNormal + MirrorByVector + TransformVectorBy is a square root,
	// three divides and four dot products per vertex. FSRRA covers the first
	// two and FIPR the rest; only the two coordinates actually used are
	// transformed, so the third row of Uncoords is never touched.
	const FLOAT SizeSq = shz_dot8f( P.Point.X, P.Point.Y, P.Point.Z, 0.f,
	                                P.Point.X, P.Point.Y, P.Point.Z, 0.f );
	const FLOAT Scale  = shz_inv_sqrtf_fsrra( SizeSq );
	const FVector N( P.Point.X * Scale, P.Point.Y * Scale, P.Point.Z * Scale );

	const FLOAT Dot = shz_dot8f( N.X, N.Y, N.Z, 0.f,
	                             P.Normal.X, P.Normal.Y, P.Normal.Z, 0.f );
	const FLOAT TwoDot = 2.0f * Dot;
	const FVector M( N.X - P.Normal.X * TwoDot,
	                 N.Y - P.Normal.Y * TwoDot,
	                 N.Z - P.Normal.Z * TwoDot );

	const FLOAT TX = shz_dot8f( M.X, M.Y, M.Z, 0.f,
	                            Frame->Uncoords.XAxis.X, Frame->Uncoords.XAxis.Y, Frame->Uncoords.XAxis.Z, 0.f );
	const FLOAT TY = shz_dot8f( M.X, M.Y, M.Z, 0.f,
	                            Frame->Uncoords.YAxis.X, Frame->Uncoords.YAxis.Y, Frame->Uncoords.YAxis.Z, 0.f );
	P.U = (TX+1.0f) * 0.5f * 256.0f * UScale;
	P.V = (TY+1.0f) * 0.5f * 256.0f * VScale;
#else
	FVector T = P.Point.UnsafeNormal().MirrorByVector( P.Normal ).TransformVectorBy( Frame->Uncoords );
	P.U = (T.X+1.0) * 0.5 * 256.0 * UScale;
	P.V = (T.Y+1.0) * 0.5 * 256.0 * VScale;
#endif
}

/*--------------------------------------------------------------------------
	Clippers.
--------------------------------------------------------------------------*/

static FLOAT Dot[32];
static inline INT Clip( FSceneNode* Frame, FTransTexture** Dest, FTransTexture** Src, INT SrcNum )
{
	INT DestNum=0;
	for( INT i=0,j=SrcNum-1; i<SrcNum; j=i++ )
	{
		if( Dot[j]>=0.0 )
		{
			Dest[DestNum++] = Src[j];
		}
		if( Dot[j]*Dot[i]<0.0 )
		{
			FTransTexture* T = Dest[DestNum] = New<FTransTexture>(GMem);
			*T = FTransTexture( *Src[j] + (*Src[i]-*Src[j]) * (Dot[j]/(Dot[j]-Dot[i])) );
			T->Project( Frame );
			DestNum++;
		}
	}
	return DestNum;
}

/*------------------------------------------------------------------------------
	Subsurface rendering.
------------------------------------------------------------------------------*/

// Triangle subdivision table.
static const int CutTable[8][4][3] =
{
	{{0,1,2},{9,9,9},{9,9,9},{9,9,9}},
	{{0,3,2},{2,3,1},{9,9,9},{9,9,9}},
	{{0,1,4},{4,2,0},{9,9,9},{9,9,9}},
	{{0,3,2},{2,3,4},{4,3,1},{9,9,9}},
	{{0,1,5},{5,1,2},{9,9,9},{9,9,9}},
	{{0,3,5},{5,3,1},{1,2,5},{9,9,9}},
	{{0,1,4},{4,2,5},{5,0,4},{9,9,9}},
	{{0,3,5},{3,1,4},{5,4,2},{3,4,5}}
};

void RenderSubsurface
(
	FSceneNode*		Frame,
	FTextureInfo&	Texture,
	FSpanBuffer*	Span,
	FTransTexture**	Pts,
	DWORD			PolyFlags,
	INT				SubCount
)
{
	guard(RenderSubsurface);

	// Handle effects.z
	if( PolyFlags & (PF_Environment | PF_Unlit) )
	{
		// Environment mapping.
		if( PolyFlags & PF_Environment )
			for( INT i=0; i<3; i++ )
				EnviroMap( Frame, *Pts[i] );

		// Handle unlit.
		if( PolyFlags & PF_Unlit )
			for( int j=0; j<3; j++ )
				Pts[j]->Light = GUnlitColor;
	}

	// Handle subdivision.
	if( SubCount<3 && !(PolyFlags & PF_Flat) )
	{
		// Compute side distances.
		INT CutSide[3], Cuts=0;
		FLOAT Alpha[3];
		STAT(uclock(GStat.MeshSubTime));
		for( INT i=0,j=2; i<3; j=i++ )
		{
			FLOAT Dist   = FDistSquared(Pts[j]->Point,Pts[i]->Point);
			FLOAT Curvy  = (Pts[j]->Normal ^ Pts[i]->Normal).SizeSquared();
			FLOAT Thresh = 50.0 * Frame->FX * SqrtApprox(Dist * Curvy) / Max(1.f, Pts[j]->Point.Z + Pts[i]->Point.Z);
			Alpha[j]     = Min( Thresh / Square(32.f) - 1.f, 1.f );
			CutSide[j]   = Alpha[j]>0.0;
			Cuts        += (CutSide[j]<<j);
		}
		STAT(uunclock(GStat.MeshSubTime));

		// See if it should be subdivided.
		if( Cuts )
		{
			STAT(uclock(GStat.MeshSubTime));
			FTransTexture Tmp[3];
			Pts[3]=Tmp+0;
			Pts[4]=Tmp+1;
			Pts[5]=Tmp+2;
			INT i;
			INT j=2;
			for( i=0; i<3; j=i++ ) if( CutSide[j] )
			{
				// Compute midpoint.
				FTransTexture& MidPt = *Pts[j+3];
				MidPt = (*Pts[j]+*Pts[i])*0.5;

				// Compute midpoint normal.
				MidPt.Normal = Pts[j]->Normal + Pts[i]->Normal;
				MidPt.Normal *= DivSqrtApprox( MidPt.Normal.SizeSquared() );

				// Enviro map it.
				if( PolyFlags & PF_Environment )
				{
					FLOAT U=MidPt.U, V=MidPt.V;
					EnviroMap( Frame, MidPt );
					MidPt.U = U + (MidPt.U - U)*Alpha[j];
					MidPt.V = V + (MidPt.V - V)*Alpha[j];
				}

				// Shade the midpoint.
				MidPt.Light += (GLightManager->Light( MidPt, PolyFlags ) - MidPt.Light) * Alpha[j];

				// Curve the midpoint.
				(FVector&)MidPt
				+=	0.15
				*	Alpha[j]
				*	(FVector&)MidPt.Normal
				*	SqrtApprox
					(
						(Pts[j]->Point  - Pts[i]->Point ).SizeSquared()
					*	(Pts[i]->Normal ^ Pts[j]->Normal).SizeSquared()
					);

				// Outcode and optionally transform midpoint.
				MidPt.ComputeOutcode( Frame );
				MidPt.Project( Frame );
			}
			FTransTexture* NewPts[6];
			for( i=0; i<4 && CutTable[Cuts][i][0]!=9; i++ )
			{
				for( INT j=0; j<3; j++ )
					NewPts[j] = Pts[CutTable[Cuts][i][j]];
				RenderSubsurface( Frame, Texture, Span, NewPts, PolyFlags, SubCount+1 );
			}
			STAT(uunclock(GStat.MeshSubTime));
			return;
		}
	}

	// If outcoded, skip it.
	if( Pts[0]->Flags & Pts[1]->Flags & Pts[2]->Flags )
		return;

	// Backface reject it.
	if( (PolyFlags & PF_TwoSided) && FTriple(Pts[0]->Point,Pts[1]->Point,Pts[2]->Point) <= 0.0 )
	{
		if( !(PolyFlags & PF_TwoSided) )
			return;
		Exchange( Pts[2], Pts[0] );
	}

	// Clip it.
	INT NumPts=3;
	BYTE AllCodes = Pts[0]->Flags | Pts[1]->Flags | Pts[2]->Flags;
	if( AllCodes )
	{
		if( AllCodes & FVF_OutXMin )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjXM * Pts[i]->Point.Z + Pts[i]->Point.X;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( AllCodes & FVF_OutXMax )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjXP * Pts[i]->Point.Z - Pts[i]->Point.X;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( AllCodes & FVF_OutYMin )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjYM * Pts[i]->Point.Z + Pts[i]->Point.Y;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( AllCodes & FVF_OutYMax )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjYP * Pts[i]->Point.Z - Pts[i]->Point.Y;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( Frame->NearClip.W != 0.0 )
		{
			UBOOL Clipped=0;
			for( INT i=0; i<NumPts; i++ )
			{
				Dot[i] = Frame->NearClip.PlaneDot(Pts[i]->Point);
				Clipped |= (Dot[i]<0.0);
			}
			if( Clipped )
			{
				static FTransTexture* LocalPts[8];
				NumPts = Clip( Frame, LocalPts, Pts, NumPts );
				if( NumPts==0 ) return;
				Pts = LocalPts;
			}
		}
	}

	for( INT i=0; i<NumPts; i++ )
	{
		//Pts[i]->ScreenX = Clamp(Pts[i]->ScreenX,0.f,Frame->FX);
		//Pts[i]->ScreenY = Clamp(Pts[i]->ScreenY,0.f,Frame->FY);
		ClipFloatFromZero(Pts[i]->ScreenX, Frame->FX);
		ClipFloatFromZero(Pts[i]->ScreenY, Frame->FY);
	}

	// Render it.
	STAT(uclock(GStat.MeshTmapTime));
	Frame->Viewport->RenDev->DrawGouraudPolygon( Frame, Texture, Pts, NumPts, PolyFlags, Span );
	STAT(uunclock(GStat.MeshTmapTime));
	STAT(GStat.MeshSubCount++);

	unguard;
}

/*------------------------------------------------------------------------------
	Cooked triangle strips.
------------------------------------------------------------------------------*/

#if defined(PLATFORM_DREAMCAST)
//
// Hand one contiguous span of a cooked strip run to the render device as a
// strip.  Returns 0 when the span cannot be stripped -- a vertex sits outside
// the frustum and needs clipping, or one mesh vertex would have to carry two
// different UVs within the same strip -- in which case the caller falls back
// to submitting the span's triangles individually.
//
static UBOOL EmitMeshStrip
(
	FSceneNode*			Frame,
	UMesh*				Mesh,
	FTransTexture*		Samples,
	FSpanBuffer*		SpanBuffer,
	FTextureInfo&		Info,
	DWORD				PolyFlags,
	const FDCMeshRun&	Run,
	INT					FirstSlot,
	INT					LastSlot,
	FTransTexture**		Pts,
	DWORD*				Stamp,
	DWORD				StripId
)
{
	guardSlow(EmitMeshStrip);

	INT Num = 0;
	for( INT Slot=FirstSlot; Slot<=LastSlot; Slot++ )
	{
		const INT   Index = Run.First + Slot;
		const INT   iVert = Mesh->DCIndices(Index);
		const DWORD UV    = Mesh->DCUVs(Index);
		FTransTexture& V  = Samples[iVert];

		// Anything with an outcode still needs RenderSubsurface's clipper.
		if( V.Flags )
			return 0;

		if( (Stamp[iVert] >> 16) == StripId )
		{
			// Already placed in this strip; it can only be reused if the
			// cooker gave it the same texture coordinates.
			if( (Stamp[iVert] & 0xFFFF) != UV )
				return 0;
		}
		else
		{
			Stamp[iVert] = (StripId << 16) | UV;
			V.U = (UV & 255) * UScale;
			V.V = (UV >> 8)  * VScale;
		}

		if( PolyFlags & PF_Unlit )
			V.Light = GUnlitColor;

		Pts[Num++] = &V;
	}

	if( !Frame->Viewport->RenDev->DrawCookedMeshStrip( Frame, Info, Pts, Num, PolyFlags, SpanBuffer, Run.Reserved ) )
		Frame->Viewport->RenDev->DrawGouraudTriStrip( Frame, Info, Pts, Num, PolyFlags, SpanBuffer );
	return 1;

	unguardSlow;
}
#endif

/*------------------------------------------------------------------------------
	High level mesh rendering.
------------------------------------------------------------------------------*/

//
// Structure used by DrawMesh for sorting triangles.
//
struct FMeshTriSort
{
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	FMeshTri Tri;
	// Which cooked strip this triangle came from, so the draw loop can stitch
	// consecutive triangles back into a strip for the render device.
	INT StripRun;
	INT StripVert;
#else
	FMeshTri* Tri;
#endif
	INT Key;
};
INT Compare( const FMeshTriSort& A, const FMeshTriSort& B )
{
	return B.Key - A.Key;
}
INT Compare( const FTransform* A, const FTransform* B )
{
	return appRound(B->Point.Z - A->Point.Z);
}

// Draw a mesh map.
//
void URender::DrawMesh
(
	FSceneNode*		Frame,
	AActor*			Owner,
	FSpanBuffer*	SpanBuffer,
	AZoneInfo*		Zone,
	const FCoords&	Coords,
	FVolActorLink*	LeafLights,
	FActorLink*		Volumetrics,
	DWORD			ExtraFlags
)
{
	guard(URender::DrawMesh);
	DC_FRAME_SCOPE(DCFS_Mesh);
	STAT(uclock(GStat.MeshTime));
	FMemMark Mark(GMem);
	UMesh*  Mesh = Owner->Mesh;
	INT TriangleCount = Mesh->Tris.Num();
	FMeshTri* Triangles = TriangleCount ? &Mesh->Tris(0) : NULL;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	if( Mesh->DCRuns.Num() )
	{
		TriangleCount = 0;
		for( INT Run = 0; Run < Mesh->DCRuns.Num(); ++Run )
			TriangleCount += Mesh->DCRuns(Run).Count - 2;
		Triangles = NULL;
	}
	FVector* DCVertexNormals = NULL;
	UBOOL DCCookedNormals = 0;
#endif
	FVector Hack = FVector(0,-8,0);
#if defined(PLATFORM_DREAMCAST)
	DCFrameCount( DCFC_MeshActors );
	DCFrameCount( DCFC_MeshVerts, Mesh->FrameVerts );
	DCFrameCount( DCFC_MeshTris, TriangleCount );
#endif
	UBOOL NotWeaponHeuristic=(Owner->Owner!=Frame->Viewport->Actor);
	if( !Engine->Client->CurvedSurfaces )
		ExtraFlags |= PF_Flat;

#if 0
	// For testing actor span clipping.
	if( SpanBuffer )
		for( INT i=SpanBuffer->StartY; i<SpanBuffer->EndY; i++ )
			for( FSpan* Span=SpanBuffer->Index[i-SpanBuffer->StartY]; Span; Span=Span->Next )
				appMemset( Frame->Screen(Span->Start,i), appRand(), (Span->End-Span->Start)*4 );
#endif

	// Get transformed verts.
	FTransTexture* Samples=NULL;
	UBOOL bWire=0;
	guardSlow(Transform);
	STAT(uclock(GStat.MeshGetFrameTime));
#if defined(PLATFORM_DREAMCAST)
	DCFrameEnter( DCFS_MeshFrame );
#endif
	Samples = New<FTransTexture>(GMem,Mesh->FrameVerts);
	bWire = Frame->Viewport->IsOrtho() || Frame->Viewport->Actor->RendMap==REN_Wire;
	Mesh->GetFrame( &Samples->Point, sizeof(Samples[0]), bWire ? GMath.UnitCoords : Coords, Owner );
#if defined(PLATFORM_DREAMCAST)
	if( Mesh->DCNormalWords.Num() || Mesh->DCNormalStreamData.Size()
		|| Mesh->DCNormalCompressed.Num() )
	{
		DCVertexNormals = New<FVector>( GMem, Mesh->FrameVerts );
		DCCookedNormals = Mesh->GetDCCookedNormals( DCVertexNormals,
			bWire ? GMath.UnitCoords : Coords, Owner );
		if( DCCookedNormals )
			DCFrameCount( DCFC_MeshCookedNormals, Mesh->FrameVerts );
	}
	DCFrameLeave( DCFS_MeshFrame );
#endif
	STAT(uunclock(GStat.MeshGetFrameTime));
	unguardSlow;

	// Compute outcodes.
	BYTE Outcode = FVF_OutReject;
	guardSlow(Outcode);
#if defined(PLATFORM_DREAMCAST)
	DCFrameEnter( DCFS_MeshOutcode );
#endif
	for( INT i=0; i<Mesh->FrameVerts; i++ )
	{
		Samples[i].Light.R = -1;
		Samples[i].ComputeOutcode( Frame );
		Outcode &= Samples[i].Flags;
	}
#if defined(PLATFORM_DREAMCAST)
	DCFrameLeave( DCFS_MeshOutcode );
#endif
	unguardSlow;

	// Render a wireframe view or textured view.
	if( bWire )
	{
		// Render each wireframe triangle.
		guardSlow(RenderWire);
		FPlane Color = Owner->bSelected ? FPlane(.2,.8,.1,0) : FPlane(.6,.4,.1,0);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		FDCMeshTriangleCursor DCWireCursor( *Mesh );
#endif
		for( INT i=0; i<TriangleCount; i++ )
		{
			FMeshTri DecodedTri;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FMeshTri& Tri = Mesh->DCRuns.Num() ? DecodedTri : Triangles[i];
			if( Mesh->DCRuns.Num() && !DCWireCursor.Next(Tri) )
				appErrorf( "Cooked mesh strip ended early" );
#else
			FMeshTri& Tri = Triangles[i];
#endif
			FVector*  P1     = &Samples[Tri.iVertex[2]].Point;
			for( int j=0; j<3; j++ )
			{
				FVector* P2 = &Samples[Tri.iVertex[j]].Point;
				if( (Tri.PolyFlags & PF_TwoSided) || P1->X>=P2->X  )
					Draw3DLine( Frame, Color, LINE_DepthCued, *P1, *P2 );
				P1 = P2;
			}
		}
		STAT(uunclock(GStat.MeshTime));
		Mark.Pop();
		unguardSlow;
		return;
	}

	// Coloring.
	FLOAT Unlit  = Clamp( Owner->ScaleGlow*0.5f + Owner->AmbientGlow/256.f, 0.f, 1.f );
	GUnlitColor  = FVector( Unlit, Unlit, Unlit );
	if( GIsEditor && (ExtraFlags & PF_Selected) )
		GUnlitColor = GUnlitColor*0.4 + FVector(0.0,0.6,0.0);

	// Mesh based particle effects.
	if( Owner->bParticles )
	{
		guardSlow(Particles);
		check(Owner->Texture);
		FTransform** SortedPts = New<FTransform*>(GMem,Mesh->FrameVerts);
		INT Count=0;
		INT i;
		for( i=0; i<Mesh->FrameVerts; i++ )
		{
			if( !Samples[i].Flags && Samples[i].Point.Z>1.0 )
			{
				Samples[i].Project( Frame );
				SortedPts[Count++] = &Samples[i];
			}
		}
		if( Frame->Viewport->RenDev->SpanBased )
		{
			appSort( SortedPts, Count );
		}
		for( i=0; i<Count; i++ )
		{
			if( !SortedPts[i]->Flags )
			{
				FLOAT XSize = SortedPts[i]->RZ * Owner->Texture->USize * Owner->DrawScale;
				FLOAT YSize = SortedPts[i]->RZ * Owner->Texture->VSize * Owner->DrawScale;
				Frame->Viewport->Canvas->DrawIcon
				(
					Owner->Texture,
					SortedPts[i]->ScreenX - XSize/2,
					SortedPts[i]->ScreenY - XSize/2,
					XSize,
					YSize,
					SpanBuffer,
					Samples[i].Point.Z,
					GUnlitColor,
					FPlane(0,0,0,0),
					ExtraFlags | PF_TwoSided | Owner->Texture->PolyFlags
				);
			}
		}
		Mark.Pop();
		STAT(uunclock(GStat.MeshTime));
		unguardSlow;
		return;
	}

	// Set up triangles.
	INT VisibleTriangles = 0;
	HasSpecialCoords = 0;
	FMeshTriSort* TriPool=NULL;
	FVector* TriNormals=NULL;
	if( Outcode == 0 )
	{
		// Process triangles.
		guardSlow(Process);
#if defined(PLATFORM_DREAMCAST)
		DCFrameEnter( DCFS_MeshPrepare );
		DCFrameEnter( DCFS_MeshPrepareSetup );
		DCFrameCount( DCFC_MeshPrepareTris, TriangleCount );
#endif
		TriPool = New<FMeshTriSort>(GMem,TriangleCount);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		if( Mesh->DCRuns.Num() )
		{
			if( !DCVertexNormals )
				DCVertexNormals = New<FVector>( GMem, Mesh->FrameVerts );
			if( !DCCookedNormals )
				appMemset( DCVertexNormals, 0, Mesh->FrameVerts * sizeof(FVector) );
		}
		else
#endif
		{
			TriNormals = New<FVector>(GMem,TriangleCount);
		}

		// Set up list for triangle sorting, adding all possibly visible triangles.
		STAT(uclock(GStat.MeshProcessTime));
		FMeshTriSort* TriTop = &TriPool[0];
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		FDCMeshTriangleCursor DCTriangleCursor( *Mesh );
#endif
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshPrepareSetup );
		// Batch whole phases so timer granularity cannot be amplified by
		// extrapolating individual microsecond-long triangle samples.
		if( Mesh->DCRuns.Num() && DCCookedNormals )
		{
			// Cooked strips need only three indices for visibility. Do not
			// expand their UVs or copy a full FMeshTri unless drawing falls back.
			_WORD BlockIndices[64][3];
			INT BlockRun[64], BlockVertex[64];
			INT RunIndex = 0, RunVertex = 2;
			for( INT Base=0; Base<TriangleCount; Base+=64 )
			{
				const INT BlockCount = Min( 64, TriangleCount-Base );
				DCFrameCount( DCFC_MeshPrepareBlocks );
				DCFrameEnter( DCFS_MeshPrepareDecode );
				for( INT j=0; j<BlockCount; ++j )
				{
					while( RunIndex < Mesh->DCRuns.Num() && RunVertex >= Mesh->DCRuns(RunIndex).Count )
					{
						++RunIndex;
						RunVertex = 2;
					}
					if( RunIndex >= Mesh->DCRuns.Num() )
						appErrorf( "Cooked mesh strip ended early" );
					const FDCMeshRun& Run = Mesh->DCRuns(RunIndex);
					const _WORD* Indices = (_WORD*)Mesh->DCIndices.GetData() + Run.First;
					// One prefetch per index cache line, ahead of the sequential walk.
					if( (RunVertex & 15) == 2 && RunVertex + 14 < Run.Count )
						SHZ_PREFETCH( Indices + RunVertex + 14 );
					const INT First = RunVertex - 2;
					BlockIndices[j][0] = Indices[First + ((RunVertex & 1) ? 1 : 0)];
					BlockIndices[j][1] = Indices[First + ((RunVertex & 1) ? 0 : 1)];
					BlockIndices[j][2] = Indices[First + 2];
					BlockRun[j] = RunIndex;
					BlockVertex[j] = RunVertex++;
				}
				DCFrameLeave( DCFS_MeshPrepareDecode );

				DCFrameEnter( DCFS_MeshPrepareVisible );
				INT CachedRun = INDEX_NONE;
				DWORD CachedFlags = 0;
				DWORD CachedPolyFlags = 0;
				INT CachedTexture = 0;
				DWORD OutcodeRejects = 0, FacingTests = 0, BackfaceRejects = 0, HardwareCullFaces = 0;
				for( INT j=0; j<BlockCount; ++j )
				{
					FTransform& V1 = Samples[BlockIndices[j][0]];
					FTransform& V2 = Samples[BlockIndices[j][1]];
					FTransform& V3 = Samples[BlockIndices[j][2]];
					if( V1.Flags & V2.Flags & V3.Flags )
					{
						++OutcodeRejects;
						continue;
					}
					if( CachedRun != BlockRun[j] )
					{
						CachedRun = BlockRun[j];
						const FDCMeshMaterial& Material = Mesh->DCMaterials(Mesh->DCRuns(CachedRun).Material);
						CachedFlags = Material.Flags;
						CachedPolyFlags = ExtraFlags | CachedFlags;
						CachedTexture = Material.Texture;
					}
					if( (CachedPolyFlags & (PF_TwoSided|PF_Flat|PF_Invisible)) == PF_Flat )
					{
						const UBOOL HardwareCull = Frame->Viewport->RenDev->UsesHardwareMeshCulling()
							&& !Frame->Viewport->RenDev->SpanBased && Frame->Mirror == 1.f
							&& Frame->NearClip.W == 0.f
							&& !(CachedPolyFlags & (PF_Environment|PF_Unlit))
							&& !(V1.Flags | V2.Flags | V3.Flags);
						if( HardwareCull )
							++HardwareCullFaces;
						else
						{
							++FacingTests;
							const FVector FaceNormal = (V1.Point-V2.Point) ^ (V3.Point-V1.Point);
							if( !(Frame->Mirror * -(FaceNormal | V1.Point) > 0.0f) )
							{
								++BackfaceRejects;
								continue;
							}
						}
					}
					TriTop->Tri.iVertex[0] = BlockIndices[j][0];
					TriTop->Tri.iVertex[1] = BlockIndices[j][1];
					TriTop->Tri.iVertex[2] = BlockIndices[j][2];
					TriTop->Tri.PolyFlags = CachedFlags;
					TriTop->Tri.TextureIndex = CachedTexture;
					TriTop->StripRun = BlockRun[j];
					TriTop->StripVert = BlockVertex[j];
					if( Frame->Viewport->RenDev->SpanBased )
						TriTop->Key = NotWeaponHeuristic
							? appRound( V1.Point.Z + V2.Point.Z + V3.Point.Z )
							: appRound( FDistSquared(V1.Point,Hack)*FDistSquared(V2.Point,Hack)*FDistSquared(V3.Point,Hack) );
					++VisibleTriangles;
					++TriTop;
				}
				DCFrameCount( DCFC_MeshVisOutcodeReject, OutcodeRejects );
				DCFrameCount( DCFC_MeshVisFacingTest, FacingTests );
				DCFrameCount( DCFC_MeshVisBackfaceReject, BackfaceRejects );
				DCFrameCount( DCFC_MeshVisHardwareCull, HardwareCullFaces );
				DCFrameLeave( DCFS_MeshPrepareVisible );
			}
		}
		else
		{
		FMeshTri BlockTri[64];
		FLOAT BlockFacing[64];
		INT BlockRun[64], BlockVertex[64];
		for( INT Base=0; Base<TriangleCount; Base+=64 )
		{
			const INT BlockCount = Min( 64, TriangleCount-Base );
			DCFrameCount( DCFC_MeshPrepareBlocks );
			DCFrameEnter( DCFS_MeshPrepareDecode );
			for( INT j=0; j<BlockCount; ++j )
			{
				if( Mesh->DCRuns.Num() )
				{
					if( !DCTriangleCursor.Next(BlockTri[j], 0) )
						appErrorf( "Cooked mesh strip ended early" );
					BlockRun[j] = DCTriangleCursor.LastRun();
					BlockVertex[j] = DCTriangleCursor.LastVertex();
				}
				else
				{
					BlockTri[j] = Triangles[Base+j];
					BlockRun[j] = BlockVertex[j] = INDEX_NONE;
				}
			}
			DCFrameLeave( DCFS_MeshPrepareDecode );

			DCFrameEnter( DCFS_MeshPrepareNormal );
			for( INT j=0; j<BlockCount; ++j )
			{
				const FMeshTri& Tri = BlockTri[j];
				FTransform& V1 = Samples[Tri.iVertex[0]];
				FTransform& V2 = Samples[Tri.iVertex[1]];
				FTransform& V3 = Samples[Tri.iVertex[2]];
				FVector FaceNormal = (V1.Point-V2.Point) ^ (V3.Point-V1.Point);
				// Only flat one-sided faces need a facing value, and only after
				// the cheap triangle outcode has passed.
				if( !(V1.Flags & V2.Flags & V3.Flags)
					&& ((ExtraFlags | Tri.PolyFlags) & (PF_TwoSided|PF_Flat|PF_Invisible)) == PF_Flat )
					BlockFacing[j] = -(FaceNormal | V1.Point);
				if( !DCCookedNormals )
				{
					FaceNormal *= DivSqrtApprox(FaceNormal.SizeSquared()+0.001);
					if( DCVertexNormals )
					{
						for( INT Corner=0; Corner<3; ++Corner )
							DCVertexNormals[Tri.iVertex[Corner]] += FaceNormal;
					}
					else
						TriNormals[Base+j] = FaceNormal;
				}
			}
			DCFrameLeave( DCFS_MeshPrepareNormal );

			DCFrameEnter( DCFS_MeshPrepareVisible );
			for( INT j=0; j<BlockCount; ++j )
			{
				FMeshTri& Tri = BlockTri[j];
				FTransform& V1 = Samples[Tri.iVertex[0]];
				FTransform& V2 = Samples[Tri.iVertex[1]];
				FTransform& V3 = Samples[Tri.iVertex[2]];
				const DWORD PolyFlags = ExtraFlags | Tri.PolyFlags;
				const UBOOL HardwareCull = Frame->Viewport->RenDev->UsesHardwareMeshCulling()
					&& !Frame->Viewport->RenDev->SpanBased && Frame->Mirror == 1.f
					&& Frame->NearClip.W == 0.f
					&& !(PolyFlags & (PF_Environment|PF_Unlit))
					&& !(V1.Flags | V2.Flags | V3.Flags);
				if( !(V1.Flags & V2.Flags & V3.Flags)
					&& ((PolyFlags & (PF_TwoSided|PF_Flat|PF_Invisible)) != PF_Flat
						|| HardwareCull || Frame->Mirror*BlockFacing[j] > 0.0f) )
				{
					if( HardwareCull && (PolyFlags & (PF_TwoSided|PF_Flat|PF_Invisible)) == PF_Flat )
						DCFrameCount( DCFC_MeshVisHardwareCull );
					if( BlockRun[j] != INDEX_NONE )
						DCTriangleCursor.LoadUV( Tri, BlockRun[j], BlockVertex[j] );
					TriTop->Tri = Tri;
					TriTop->StripRun = BlockRun[j];
					TriTop->StripVert = BlockVertex[j];
					if( Frame->Viewport->RenDev->SpanBased )
						TriTop->Key = NotWeaponHeuristic
							? appRound( V1.Point.Z + V2.Point.Z + V3.Point.Z )
							: appRound( FDistSquared(V1.Point,Hack)*FDistSquared(V2.Point,Hack)*FDistSquared(V3.Point,Hack) );
					++VisibleTriangles;
					++TriTop;
				}
			}
			DCFrameLeave( DCFS_MeshPrepareVisible );
		}
		}
#else
		for( INT i=0; i<TriangleCount; i++ )
		{
			FMeshTri DecodedTri;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FMeshTri* Tri = Mesh->DCRuns.Num() ? &DecodedTri : &Triangles[i];
			if( Mesh->DCRuns.Num() && !DCTriangleCursor.Next(*Tri) )
				appErrorf( "Cooked mesh strip ended early" );
#else
			FMeshTri* Tri = &Triangles[i];
#endif
			FTransform& V1  = Samples[Tri->iVertex[0]];
			FTransform& V2  = Samples[Tri->iVertex[1]];
			FTransform& V3  = Samples[Tri->iVertex[2]];
			DWORD PolyFlags = ExtraFlags | Tri->PolyFlags;

			// Compute triangle normal.
			FVector FaceNormal = (V1.Point-V2.Point) ^ (V3.Point-V1.Point);
			// For this winding, FTriple(V1,V2,V3) is the negative dot of
			// V1 with the face normal. Reuse the cross product for culling
			// instead of evaluating a second three-vector determinant.
#if defined(PLATFORM_DREAMCAST)
			const FLOAT Facing = -(FaceNormal | V1.Point);
#endif
			FaceNormal *= DivSqrtApprox(FaceNormal.SizeSquared()+0.001);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			if( DCVertexNormals )
			{
				for( INT Corner = 0; Corner < 3; ++Corner )
				{
					DCVertexNormals[Tri->iVertex[Corner]] += FaceNormal;
				}
			}
			else
#endif
			{
				TriNormals[i] = FaceNormal;
			}

			// See if potentially visible.
			if( !(V1.Flags & V2.Flags & V3.Flags) )
			{
				if
				(	(PolyFlags & (PF_TwoSided|PF_Flat|PF_Invisible))!=(PF_Flat)
#if defined(PLATFORM_DREAMCAST)
				||	Frame->Mirror*Facing>0.0 )
#else
				||	Frame->Mirror*FTriple(V1.Point,V2.Point,V3.Point)>0.0 )
#endif
				{
					// This is visible.
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
					TriTop->Tri = *Tri;
					TriTop->StripRun  = Mesh->DCRuns.Num() ? DCTriangleCursor.LastRun()    : INDEX_NONE;
					TriTop->StripVert = Mesh->DCRuns.Num() ? DCTriangleCursor.LastVertex() : INDEX_NONE;
#else
					TriTop->Tri = Tri;
#endif

					// Set the sort key.
					TriTop->Key
					= NotWeaponHeuristic ? appRound( V1.Point.Z + V2.Point.Z + V3.Point.Z )
					: TriTop->Key=appRound( FDistSquared(V1.Point,Hack)*FDistSquared(V2.Point,Hack)*FDistSquared(V3.Point,Hack) );

					// Add to list.
					VisibleTriangles++;
					TriTop++;
				}
			}
		}
#endif
		STAT(uunclock(GStat.MeshProcessTime));
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshPrepare );
#endif
		unguardSlow;
	}

#if defined(PLATFORM_DREAMCAST)
	DCFrameCount( DCFC_MeshVisible, VisibleTriangles );
#endif

	// Render triangles.
	if( VisibleTriangles>0 )
	{
		guardSlow(Render);

		// Fatness.
		UBOOL Fatten = Owner->Fatness!=128;
		FLOAT Fatness = (Owner->Fatness/16.0)-8.0;

		// Sort by depth.
		if( Frame->Viewport->RenDev->SpanBased )
			appSort( TriPool, VisibleTriangles );

		// Lock the textures.
		UTexture* EnvironmentMap = NULL;
		guardSlow(Lock);
#if defined(PLATFORM_DREAMCAST)
		DCFrameEnter( DCFS_MeshTextureInfo );
#endif
		check(Mesh->Textures.Num()<=ARRAY_COUNT(TextureInfo));
		for( INT i=0; i<Mesh->Textures.Num(); i++ )
		{
			Textures[i] = Mesh->GetTexture( i, Owner );
			if( Textures[i] )
			{
				Textures[i]->GetInfo( TextureInfo[i], Frame->Viewport->CurrentTime );
				EnvironmentMap = Textures[i];
			}
		}
		if( Owner->Texture )
			EnvironmentMap = Owner->Texture;
		else if( Owner->Region.Zone && Owner->Region.Zone->EnvironmentMap )
			EnvironmentMap = Owner->Region.Zone->EnvironmentMap;
		else if( Owner->Level->EnvironmentMap )
			EnvironmentMap = Owner->Level->EnvironmentMap;
		check(EnvironmentMap);
		EnvironmentMap->GetInfo( EnvironmentInfo, Frame->Viewport->CurrentTime );
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshTextureInfo );
#endif
		unguardSlow;

		// Build list of all incident lights on the mesh.
		STAT(uclock(GStat.MeshLightSetupTime));
#if defined(PLATFORM_DREAMCAST)
		DCFrameEnter( DCFS_MeshLightSetup );
#endif
		ExtraFlags |= GLightManager->SetupForActor( Frame, Owner, LeafLights, Volumetrics );
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshLightSetup );
#endif
		STAT(uunclock(GStat.MeshLightSetupTime));

		// Perform all vertex lighting.
		guardSlow(Light);
#if defined(PLATFORM_DREAMCAST)
		DCFrameEnter( DCFS_MeshVertexLight );
		// Gather each visible mesh vertex once. Whole-pass timings keep the
		// light/fog/projection breakdown out of the microsecond sampling trap.
		DCFrameEnter( DCFS_MeshVertexCollect );
		INT* VisibleVerts = New<INT>( GMem, Mesh->FrameVerts );
		INT UniqueVerts = 0;
		for( INT i=0; i<VisibleTriangles; ++i )
		{
			const FMeshTri& Tri = TriPool[i].Tri;
			for( INT j=0; j<3; ++j )
			{
				const INT iVert = Tri.iVertex[j];
				if( Samples[iVert].Light.R == -1 )
				{
					Samples[iVert].Light.R = -2;
					VisibleVerts[UniqueVerts++] = iVert;
				}
			}
		}
		DCFrameCount( DCFC_MeshVertexUnique, UniqueVerts );
		DCFrameLeave( DCFS_MeshVertexCollect );

		DCFrameEnter( DCFS_MeshVertexNormal );
		for( INT i=0; i<UniqueVerts; ++i )
		{
			const INT iVert = VisibleVerts[i];
			FTransSample& Vert = Samples[iVert];
			FVector Norm(0,0,0);
			if( DCVertexNormals )
				Norm = DCVertexNormals[iVert];
			else
			{
				FMeshVertConnect& Connect = Mesh->Connects(iVert);
				for( INT k=0; k<Connect.NumVertTriangles; ++k )
					Norm += TriNormals[Mesh->VertLinks(Connect.TriangleListOffset + k)];
			}
			Vert.Normal = FPlane( Vert.Point, Norm * DivSqrtApprox(Norm.SizeSquared()) );
			if( Fatten )
			{
				Vert.Point += Vert.Normal * Fatness;
				Vert.ComputeOutcode( Frame );
			}
		}
		DCFrameLeave( DCFS_MeshVertexNormal );

		DCFrameEnter( DCFS_MeshVertexLightCall );
		for( INT i=0; i<UniqueVerts; ++i )
		{
			FTransSample& Vert = Samples[VisibleVerts[i]];
			Vert.Light = GLightManager->Light( Vert, ExtraFlags );
		}
		DCFrameLeave( DCFS_MeshVertexLightCall );

		DCFrameEnter( DCFS_MeshVertexFog );
		for( INT i=0; i<UniqueVerts; ++i )
		{
			FTransSample& Vert = Samples[VisibleVerts[i]];
			Vert.Fog = GLightManager->Fog( Vert, ExtraFlags );
		}
		DCFrameLeave( DCFS_MeshVertexFog );

		DCFrameEnter( DCFS_MeshVertexProject );
		for( INT i=0; i<UniqueVerts; ++i )
		{
			FTransSample& Vert = Samples[VisibleVerts[i]];
			if( !Vert.Flags )
				Vert.Project( Frame );
		}
		DCFrameLeave( DCFS_MeshVertexProject );
#else
		for( INT i=0; i<VisibleTriangles; i++ )
		{
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FMeshTri& Tri = TriPool[i].Tri;
#else
			FMeshTri& Tri = *TriPool[i].Tri;
#endif
			for( INT j=0; j<3; j++ )
			{
				INT iVert = Tri.iVertex[j];
				FTransSample& Vert = Samples[iVert];
				if( Vert.Light.R == -1 )
				{
					// Compute vertex normal.
					FVector Norm(0,0,0);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
					if( DCVertexNormals )
					{
						Norm = DCVertexNormals[iVert];
					}
					else
#endif
					{
					FMeshVertConnect& Connect = Mesh->Connects(iVert);
					for( INT k=0; k<Connect.NumVertTriangles; k++ )
						Norm += TriNormals[Mesh->VertLinks(Connect.TriangleListOffset + k)];
					}
					Vert.Normal = FPlane( Vert.Point, Norm * DivSqrtApprox(Norm.SizeSquared()) );

					// Fatten it if desired.
					if( Fatten )
					{
						Vert.Point += Vert.Normal * Fatness;
						Vert.ComputeOutcode( Frame );
					}

					// Compute effect of each lightsource on this vertex.
					Vert.Light = GLightManager->Light( Vert, ExtraFlags );
					Vert.Fog   = GLightManager->Fog  ( Vert, ExtraFlags );

					// Project it.
					if( !Vert.Flags )
						Vert.Project( Frame );
				}
			}
		}
#endif
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshVertexLight );
#endif
		unguardSlow;

		// Draw the triangles.
		guardSlow(DrawVisible);
#if defined(PLATFORM_DREAMCAST)
		DCFrameEnter( DCFS_MeshDraw );
#endif
		STAT(GStat.MeshPolyCount+=VisibleTriangles);

#if defined(PLATFORM_DREAMCAST)
		// Meshes are strip-cooked offline. When the render device can take a
		// strip directly, stitch runs of consecutive visible triangles back
		// together: N vertices for N-2 triangles instead of 3*(N-2).
		// Mirrored scene nodes are excluded because they invert winding, which
		// a strip cannot express by swapping two corners.
		UBOOL UseStrips = Frame->Viewport->RenDev->SupportsTriStrips
			&& Mesh->DCRuns.Num()
			&& !Frame->Viewport->RenDev->SpanBased
			&& Frame->Mirror != -1;
		FTransTexture** StripPts = NULL;
		DWORD* StripStamp = NULL;
		DWORD StripId = 0;
		if( UseStrips )
		{
			INT MaxRun = 0;
			for( INT r=0; r<Mesh->DCRuns.Num(); r++ )
				MaxRun = Max<INT>( MaxRun, Mesh->DCRuns(r).Count );
			if( MaxRun >= 3 && Mesh->FrameVerts > 0 )
			{
				StripPts   = New<FTransTexture*>( GMem, MaxRun );
				StripStamp = New<DWORD>( GMem, Mesh->FrameVerts );
				appMemset( StripStamp, 0, Mesh->FrameVerts * sizeof(DWORD) );
			}
			else
			{
				UseStrips = 0;
			}
		}
		Frame->Viewport->RenDev->BeginCookedMesh();
		FDCMeshTriangleCursor DCDrawCursor( *Mesh );
#endif

		for( INT i=0; i<VisibleTriangles; i++ )
		{
			// Set up the triangle.
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FMeshTri& Tri = TriPool[i].Tri;
#else
			FMeshTri& Tri = *TriPool[i].Tri;
#endif

#if defined(PLATFORM_DREAMCAST)
			if( UseStrips && !(Tri.PolyFlags & PF_Invisible) && TriPool[i].StripRun != INDEX_NONE )
			{
				const DWORD StripFlags = Tri.PolyFlags | ExtraFlags;
				// Curved-surface subdivision and environment mapping both need
				// RenderSubsurface's per-vertex work, so they stay off the
				// strip path.
				if( (StripFlags & PF_Flat) && !(StripFlags & PF_Environment) )
				{
					const INT Run = TriPool[i].StripRun;
					INT End = i;
					while( End+1 < VisibleTriangles
						&& TriPool[End+1].StripRun == Run
						&& TriPool[End+1].StripVert == TriPool[End].StripVert + 1
						&& !(TriPool[End+1].Tri.PolyFlags & PF_Invisible)
						&& TriPool[End+1].Tri.PolyFlags == Tri.PolyFlags
						&& TriPool[End+1].Tri.TextureIndex == Tri.TextureIndex )
						End++;

					if( End > i )
					{
						INT Index = Tri.TextureIndex;
						FTextureInfo& StripInfo = Textures[Index] ? TextureInfo[Index] : EnvironmentInfo;
						UScale = StripInfo.UScale * StripInfo.USize / 256.0;
						VScale = StripInfo.VScale * StripInfo.VSize / 256.0;
						if( EmitMeshStrip( Frame, Mesh, Samples, SpanBuffer, StripInfo, StripFlags,
								Mesh->DCRuns(Run), TriPool[i].StripVert - 2, TriPool[End].StripVert,
								StripPts, StripStamp, ++StripId ) )
						{
							STAT(GStat.MeshSubCount += End - i + 1);
							DCFrameCount( DCFC_MeshStripTris, End - i + 1 );
							i = End;
							continue;
						}
					}
				}
			}
#endif

			if( !(Tri.PolyFlags & PF_Invisible) )
			{
#if defined(PLATFORM_DREAMCAST)
				DCFrameCount( DCFC_MeshFallbackTris );
				if( Mesh->DCRuns.Num() && DCCookedNormals && TriPool[i].StripRun != INDEX_NONE )
					DCDrawCursor.LoadUV( Tri, TriPool[i].StripRun, TriPool[i].StripVert );
#endif
				// Get texture.
				DWORD PolyFlags = Tri.PolyFlags | ExtraFlags;
				INT Index = Tri.TextureIndex;
				FTextureInfo& Info = (Textures[Index] && !(PolyFlags & PF_Environment)) ? TextureInfo[Index] : EnvironmentInfo;
				UScale = Info.UScale * Info.USize / 256.0;
				VScale = Info.VScale * Info.VSize / 256.0;

				// Set up texture coords.
				FTransTexture* Pts[6];
				for( INT j=0; j<3; j++ )
				{
					Pts[j]    = &Samples[Tri.iVertex[j]];
					Pts[j]->U = Tri.Tex[j].U * UScale;
					Pts[j]->V = Tri.Tex[j].V * VScale;
				}
				if( Frame->Mirror == -1 )
					Exchange( Pts[2], Pts[0] );
#if defined(PLATFORM_DREAMCAST)
				// A flat triangle entirely inside the view requires neither curved
				// subdivision nor the general mesh clipper. Keep special shading,
				// two-sided winding, and near-plane cases on the original path.
				if( (PolyFlags & PF_Flat)
					&& !(PolyFlags & (PF_Environment | PF_Unlit | PF_TwoSided))
					&& !(Pts[0]->Flags | Pts[1]->Flags | Pts[2]->Flags)
					&& Frame->NearClip.W == 0.0f )
				{
					Frame->Viewport->RenDev->DrawGouraudPolygon(
						Frame, Info, Pts, 3, PolyFlags, SpanBuffer );
				}
				else
#endif
				{
					RenderSubsurface( Frame, Info, SpanBuffer, Pts, PolyFlags, 0 );
				}
			}
			else
			{
				// Remember coordinate system.
				FVector Mid = 0.5*(Samples[Tri.iVertex[0]].Point + Samples[Tri.iVertex[2]].Point);

				FCoords C;
				C.Origin = FVector(0,0,0);
				C.XAxis	 = (Samples[Tri.iVertex[1]].Point - Mid).SafeNormal();
				C.YAxis	 = (C.XAxis ^ (Samples[Tri.iVertex[0]].Point - Samples[Tri.iVertex[2]].Point)).SafeNormal();
				C.ZAxis	 = C.YAxis ^ C.XAxis;

				SpecialCoords = GMath.UnitCoords * Mid * C;
				HasSpecialCoords = 1;
			}
		}
#if defined(PLATFORM_DREAMCAST)
		Frame->Viewport->RenDev->EndCookedMesh();
#endif
		GLightManager->FinishActor();
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshDraw );
#endif
		unguardSlow;
		unguardSlow;
	}

	STAT(GStat.MeshCount++);
	STAT(uunclock(GStat.MeshTime));
	Mark.Pop();
	unguardf(( "(%s)", Owner->Mesh->GetName() ));
}

/*------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------*/
