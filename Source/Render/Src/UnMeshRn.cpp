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
#if defined(PLATFORM_DREAMCAST)
struct FDCMeshView
{
	const TArray<FDCMeshRun>* Runs;
	const TArray<_WORD>* Indices;
	const TArray<_WORD>* UVs;
	const TArray<FBox>* Bounds;
	UBOOL Lod;
	FDCMeshView(UMesh* M, UBOOL L) : Runs(L ? &M->DCLodRuns : &M->DCRuns),
		Indices(L ? &M->DCLodIndices : &M->DCIndices), UVs(L ? &M->DCLodUVs : &M->DCUVs),
		Bounds(L ? &M->DCLodBounds : &M->DCMeshletBounds), Lod(L) {}
};
#endif

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
// the frustum and needs clipping. Corner-local copies preserve UV seams without
// repeating transforms or lighting, and odd restarts retain their winding.
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
	FTransTexture**&		Pts,
	FTransTexture*&		Corners,
	INT&                Capacity,
	const TArray<_WORD>* Indices=NULL, const TArray<_WORD>* UVs=NULL
)
{
	guardSlow(EmitMeshStrip);

	// The PVR fast path consumes immutable samples and separate corner UVs.
	// Returning false leaves clipping and unusual materials on the old path.
	const _WORD* NativeIndices = Indices ? &(*Indices)(Run.First+FirstSlot) : &Mesh->DCIndices(Run.First+FirstSlot);
	const _WORD* NativeUVs = UVs ? &(*UVs)(Run.First+FirstSlot) : &Mesh->DCUVs(Run.First+FirstSlot);
	if( Frame->Viewport->RenDev->DrawIndexedMeshStrip(Frame,Info,Samples,NativeIndices,NativeUVs,
		LastSlot-FirstSlot+1,FirstSlot&1,PolyFlags) )
		return 1;

	// The direct indexed path needs no corner-copy storage. Allocate only on
	// its first fallback, growing for a longer range if necessary.
	const INT Needed=LastSlot-FirstSlot+1;
	if( Needed>Capacity )
	{
		Pts=New<FTransTexture*>(GMem,Needed+1);
		Corners=New<FTransTexture>(GMem,Needed);
		Capacity=Needed;
	}
	INT Num = 0;
	for( INT Slot=FirstSlot; Slot<=LastSlot; Slot++ )
	{
		const INT   Index = Run.First + Slot;
		const INT   iVert = Indices ? (*Indices)(Index) : Mesh->DCIndices(Index);
		const DWORD UV    = UVs ? (*UVs)(Index) : Mesh->DCUVs(Index);
		FTransTexture& V  = Corners[Slot-FirstSlot];
		V = Samples[iVert];

		// Anything with an outcode still needs RenderSubsurface's clipper.
		if( V.Flags )
			return 0;

		// Position/lighting are shared; texture coordinates belong to corners.
		V.U = (UV & 255) * UScale;
		V.V = (UV >> 8)  * VScale;

		if( PolyFlags & PF_Unlit )
			V.Light = GUnlitColor;

		// A restarted odd strip needs one degenerate vertex to retain winding.
		if( Slot == FirstSlot && (FirstSlot & 1) )
			Pts[Num++] = &V;
		Pts[Num++] = &V;
	}

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
#if defined(PLATFORM_DREAMCAST)
extern INT GFrameStamp;
extern INT GDCOrderedWorldPass;
struct FDCVisibleRange { INT Run, First, Last; };

static BYTE DCBoxOutcode(FSceneNode* Frame, const FBox& Box, const FCoords& Coords,
	const FVector& Origin, BYTE& Any)
{
	BYTE All=255; Any=0;
	for( INT c=0; c<8; ++c )
	{
		FTransform V;
		V.Point=(FVector(c&1 ? Box.Max.X : Box.Min.X,c&2 ? Box.Max.Y : Box.Min.Y,
			c&4 ? Box.Max.Z : Box.Min.Z)-Origin).TransformPointBy(Coords);
		V.ComputeOutcode(Frame);
		BYTE Code=V.Flags;
		if( Frame->NearClip.W!=0.f && Frame->NearClip.PlaneDot(V.Point)<0.f ) Code|=128;
		All&=Code; Any|=Code;
	}
	return All;
}

static BYTE* DCSelectMeshlets(FSceneNode* Frame, UMesh* Mesh, AActor* Owner,
	FCoords Coords, BYTE*& Meshlets, const FDCMeshView& View)
{
	if( !GDCMeshOptimize || !View.Bounds->Num() || Owner->bParticles
		|| Owner->AnimFrame<0.f || Owner->DrawScale<=0.f || Owner->Fatness!=128
		|| Mesh->FrameVerts>512 || Frame->Viewport->IsOrtho()
		|| Frame->Viewport->Actor->RendMap==REN_Wire ) return NULL;
	Coords=Coords*(Owner->Location+Owner->PrePivot)*Owner->Rotation*Mesh->RotOrigin
		*FScale(Mesh->Scale*Owner->DrawScale,0.f,SHEER_None);
	FBox All(0);
	for( INT m=0; m<View.Bounds->Num(); ++m ) All+=(*View.Bounds)(m);
	BYTE Any;
	DCBoxOutcode(Frame,All,Coords,Mesh->Origin,Any);
	if( !Any && !View.Lod ) return NULL;
	const UBOOL Inside=Any==0;
	Meshlets=New<BYTE>(GMem,View.Bounds->Num());
	BYTE* Vertices=New<BYTE>(GMem,Mesh->FrameVerts);
	appMemset(Vertices,0,Mesh->FrameVerts);
	INT Rejected=0;
	for( INT m=0; m<View.Bounds->Num(); ++m )
	{
		Meshlets[m]=Inside || DCBoxOutcode(Frame,(*View.Bounds)(m),Coords,Mesh->Origin,Any)==0;
		Rejected+=!Meshlets[m];
	}
	DCFrameCount(DCFC_MeshletReject,Rejected);
	if( !Rejected && !View.Lod ) { Meshlets=NULL; return NULL; }
	for( INT r=0; r<View.Runs->Num(); ++r )
		if( Meshlets[(*View.Runs)(r).Reserved] )
			for( INT v=0; v<(*View.Runs)(r).Count; ++v ) Vertices[(*View.Indices)((*View.Runs)(r).First+v)]=1;
	return Vertices;
}

static UBOOL DCSelectLOD(FSceneNode* Frame, UMesh* Mesh, AActor* Owner, FCoords Coords)
{
	if(!GDCMeshOptimize || !Mesh->DCLodRuns.Num() || Owner->bParticles || Owner->AnimFrame<0.f
		|| Owner->Fatness!=128 || Owner->DrawScale<=0.f || Owner->Owner==Frame->Viewport->Actor
		|| Frame->Recursion || Frame->Viewport->IsOrtho() || Frame->Viewport->Actor->RendMap!=REN_DynLight) return 0;
	struct FChoice { AActor* Owner; UMesh* Mesh; FSceneNode* Frame; INT Stamp; UBOOL Lod; };
	static FChoice Choices[128];
	INT Slot=0;
	for(INT i=0;i<128;++i)
	{
		if(Choices[i].Owner==Owner && Choices[i].Mesh==Mesh) { Slot=i; break; }
		if(Choices[i].Stamp<Choices[Slot].Stamp) Slot=i;
	}
	FChoice& C=Choices[Slot];
	if(C.Owner==Owner && C.Mesh==Mesh && C.Frame==Frame && C.Stamp==GFrameStamp) return C.Lod;
	const UBOOL Previous=C.Owner==Owner && C.Mesh==Mesh && C.Stamp==GFrameStamp-1 && C.Lod;
	Coords=Coords*(Owner->Location+Owner->PrePivot)*Owner->Rotation*Mesh->RotOrigin
		*FScale(Mesh->Scale*Owner->DrawScale,0.f,SHEER_None);
	FLOAT MinX=1.e20f,MinY=1.e20f,MaxX=-1.e20f,MaxY=-1.e20f;
	UBOOL Safe=1; const FBox& B=Mesh->BoundingBox;
	for(INT i=0;i<8;++i)
	{
		FTransform V;
		V.Point=(FVector(i&1 ? B.Max.X:B.Min.X,i&2 ? B.Max.Y:B.Min.Y,i&4 ? B.Max.Z:B.Min.Z)-Mesh->Origin).TransformPointBy(Coords);
		if(V.Point.Z<=1.f) { Safe=0; break; }
		V.Project(Frame); MinX=Min(MinX,V.ScreenX); MaxX=Max(MaxX,V.ScreenX);
		MinY=Min(MinY,V.ScreenY); MaxY=Max(MaxY,V.ScreenY);
	}
	C.Owner=Owner; C.Mesh=Mesh; C.Frame=Frame; C.Stamp=GFrameStamp;
	C.Lod=Safe && Max(MaxX-MinX,MaxY-MinY)<(Previous ? 80.f:64.f);
	return C.Lod;
}

static INT DCBuildVisibleRanges(FSceneNode* Frame, UMesh* Mesh, FTransTexture* Samples,
	DWORD ExtraFlags, FDCVisibleRange* Ranges, INT& NumRanges, const BYTE* Meshlets, const FDCMeshView& View)
{
	DC_FRAME_SCOPE(DCFS_MeshPrepareVisible);
	INT Visible=0;
	DWORD Out=0, Face=0, Back=0, HW=0;
	for( INT r=0; r<View.Runs->Num(); ++r )
	{
		const FDCMeshRun& Run=(*View.Runs)(r);
		if( Meshlets && !Meshlets[Run.Reserved] ) continue;
		const DWORD Flags=Mesh->DCMaterials(Run.Material).Flags | ExtraFlags;
		const _WORD* Indices=&(*View.Indices)(Run.First);
		for( INT v=2; v<Run.Count; ++v )
		{
			FTransTexture& A=Samples[Indices[v-2+((v&1)?1:0)]];
			FTransTexture& B=Samples[Indices[v-2+((v&1)?0:1)]];
			FTransTexture& C=Samples[Indices[v]];
			if( A.Flags & B.Flags & C.Flags ) { ++Out; continue; }
			if( (Flags & (PF_TwoSided|PF_Flat|PF_Invisible))==PF_Flat )
			{
				if( Frame->Viewport->RenDev->UsesHardwareMeshCulling() && Frame->Mirror==1.f
					&& Frame->NearClip.W==0.f && !(Flags & (PF_Environment|PF_Unlit))
					&& !(A.Flags|B.Flags|C.Flags) ) ++HW;
				else
				{
					++Face;
					if( Frame->Mirror * -(((A.Point-B.Point) ^ (C.Point-A.Point)) | A.Point)<=0.f )
					{ ++Back; continue; }
				}
			}
			if( NumRanges && Ranges[NumRanges-1].Run==r && Ranges[NumRanges-1].Last==v-1 )
				Ranges[NumRanges-1].Last=v;
			else { FDCVisibleRange& R=Ranges[NumRanges++]; R.Run=r; R.First=R.Last=v; }
			++Visible;
		}
	}
	DCFrameCount(DCFC_MeshVisOutcodeReject,Out); DCFrameCount(DCFC_MeshVisFacingTest,Face);
	DCFrameCount(DCFC_MeshVisBackfaceReject,Back); DCFrameCount(DCFC_MeshVisHardwareCull,HW);
	return Visible;
}

static void DCDrawVisibleRanges(FSceneNode* Frame, UMesh* Mesh, FTransTexture* Samples,
	FSpanBuffer* Span, DWORD ExtraFlags, FDCVisibleRange* Ranges, INT Count,
	FTransTexture**& StripPts, FTransTexture*& StripCorners, INT& StripCapacity,
	UBOOL UseStrips, const FDCMeshView& View, FDCMeshDrawCache* DrawCache)
{
	for( INT r=0; r<Count; ++r )
	{
		const FDCVisibleRange& Range=Ranges[r];
		const FDCMeshRun& Run=(*View.Runs)(Range.Run);
		const FDCMeshMaterial& Mat=Mesh->DCMaterials(Run.Material);
		const DWORD Flags=Mat.Flags | ExtraFlags;
		if( !(Flags & PF_Invisible) && !Frame->Viewport->RenDev->WantsMeshFlags(Flags) ) continue;
		FTextureInfo& Info=Textures[Mat.Texture] && !(Flags & PF_Environment) ? TextureInfo[Mat.Texture] : EnvironmentInfo;
		UScale=Info.UScale*Info.USize/256.0; VScale=Info.VScale*Info.VSize/256.0;
		if( UseStrips && DrawCache && (Flags & PF_Flat)
			&& !(Flags & (PF_Environment|PF_Unlit|PF_Invisible)) )
		{
			FDCMeshDrawRange Batch[32];
			INT BatchCount=0;
			{
			DC_FRAME_SCOPE(DCFS_MeshDrawSetup);
			// Contiguous ranges only: no material sorting of translucent draws,
			// attachment replay or exceptional materials across this boundary.
			for( INT b=r; b<Count && BatchCount<32; ++b )
			{
				const FDCVisibleRange& R=Ranges[b];
				const FDCMeshRun& Next=(*View.Runs)(R.Run);
				// A singleton is a valid three-vertex strip, including odd restarts.
				if( Next.Material!=Run.Material || R.Last<R.First ) break;
				FDCMeshDrawRange& B=Batch[BatchCount++];
				B.Indices=&(*View.Indices)(Next.First+R.First-2);
				B.UVs=&(*View.UVs)(Next.First+R.First-2);
				B.Count=R.Last-R.First+3; B.OddStart=(R.First-2)&1;
			}
			}
			if( BatchCount )
			{
				const INT Done=Frame->Viewport->RenDev->DrawIndexedMeshRanges(
					Frame,Info,Samples,*DrawCache,Batch,BatchCount,Flags);
				if( Done )
				{
					INT Tris=0, Singles=0;
					for( INT b=0; b<Done; ++b )
					{ Tris+=Batch[b].Count-2; Singles+=Batch[b].Count==3; }
					DCFrameCount(DCFC_MeshStripTris,Tris);
					DCFrameCount(DCFC_MeshDirectSingles,Singles);
					r+=Done-1;
					continue;
				}
			}
		}
		// Includes legacy strips, corner copies, clipping and fallback submission.
		// Attachment-only work is a measured component of OTHER.
		DC_FRAME_SCOPE_NAMED(FallbackScope, (Flags & PF_Invisible) ? DCFS_MeshDrawAttachment : DCFS_MeshDrawFallback);
		if( UseStrips && Range.Last>=Range.First && (Flags & PF_Flat)
			&& !(Flags & (PF_Environment|PF_Invisible))
			&& EmitMeshStrip(Frame,Mesh,Samples,Span,Info,Flags,Run,Range.First-2,Range.Last,StripPts,StripCorners,StripCapacity,View.Indices,View.UVs) )
		{
			DCFrameCount(DCFC_MeshStripTris,Range.Last-Range.First+1);
			continue;
		}
		// Only exceptional/clipped ranges are expanded, directly into corners.
		for( INT v=Range.First; v<=Range.Last; ++v )
		{
			FTransTexture Corners[3]; FTransTexture* Pts[6];
			for( INT c=0; c<3; ++c )
			{
				const INT Slot=Run.First+v-2+(c==2 ? 2 : (v&1) ? 1-c : c);
				Corners[c]=Samples[(*View.Indices)(Slot)];
				const DWORD UV=(*View.UVs)(Slot);
				Corners[c].U=(UV&255)*UScale; Corners[c].V=(UV>>8)*VScale; Pts[c]=&Corners[c];
			}
			if( Flags & PF_Invisible )
			{
				FVector Mid=(Corners[0].Point+Corners[2].Point)*0.5f;
				FCoords C; C.Origin=FVector(0,0,0);
				C.XAxis=(Corners[1].Point-Mid).SafeNormal();
				C.YAxis=(C.XAxis ^ (Corners[0].Point-Corners[2].Point)).SafeNormal(); C.ZAxis=C.YAxis ^ C.XAxis;
				SpecialCoords=GMath.UnitCoords * Mid * C; HasSpecialCoords=1;
				continue;
			}
			if( Frame->Mirror==-1 ) Exchange(Pts[0],Pts[2]);
			DCFrameCount(DCFC_MeshFallbackTris);
			if( (Flags & PF_Flat) && !(Flags & (PF_Environment|PF_Unlit|PF_TwoSided))
				&& !(Pts[0]->Flags|Pts[1]->Flags|Pts[2]->Flags) && Frame->NearClip.W==0.f )
				Frame->Viewport->RenDev->DrawGouraudPolygon(Frame,Info,Pts,3,Flags,Span);
			else RenderSubsurface(Frame,Info,Span,Pts,Flags,0);
		}
	}
}
struct FDCPreparedMesh
{
	FDCPreparedMesh* Next;
	FSceneNode* Frame;
	AActor* Owner;
	UMesh* Mesh;
	FCoords Coords;
	FLOAT Mirror;
	DWORD InputFlags, LitFlags;
	FTransTexture* Samples;
	FMeshTriSort* Triangles;
	INT Count;
	FDCVisibleRange* Ranges;
	INT NumRanges;
	FDCMeshDrawCache* DrawCache;
	UBOOL Lod;
};
static FDCPreparedMesh* GDCPreparedMeshes = NULL;
static INT GDCPreparedMeshStamp = -1;
static INT GDCPreparedMeshBytes = 0;
#endif
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
#if defined(PLATFORM_DREAMCAST)
	FDCMeshView View(Mesh,!Engine->Client->CurvedSurfaces && DCSelectLOD(Frame,Mesh,Owner,Coords));
	if(View.Lod) DCFrameCount(DCFC_MeshLod);
	if( GDCMeshOptimize && Owner->bParticles && Frame->Viewport->RenDev->UsesOrderedLists()
		&& !Frame->Viewport->RenDev->WantsMeshFlags(PF_Translucent)
		&& !Frame->Viewport->IsOrtho() && Frame->Viewport->Actor->RendMap!=REN_Wire )
	{
		HasSpecialCoords = 0;
		DCFrameCount(DCFC_MeshPassSkipped);
		STAT(uunclock(GStat.MeshTime));
		Mark.Pop();
		return;
	}
	// Reject a whole wrong-list draw before decoding or transforming its pose.
	// Invisible attachment triangles produce SpecialCoords for pawn weapons;
	// they must still run in every pass until that result is cached separately.
	if( GDCMeshOptimize && Mesh->DCRuns.Num() && !Owner->bParticles
		&& !Frame->Viewport->IsOrtho() && Frame->Viewport->Actor->RendMap!=REN_Wire )
	{
		UBOOL Needed = 0;
		for( INT m=0; m<Mesh->DCMaterials.Num(); ++m )
			if( (Mesh->DCMaterials(m).Flags & PF_Invisible)
				|| Frame->Viewport->RenDev->WantsMeshFlags(Mesh->DCMaterials(m).Flags | ExtraFlags) )
			{
				Needed = 1;
				break;
			}
		if( !Needed )
		{
			DCFrameCount(DCFC_MeshPassSkipped);
			HasSpecialCoords = 0;
			STAT(uunclock(GStat.MeshTime));
			Mark.Pop();
			return;
		}
	}
#endif
	INT TriangleCount = Mesh->Tris.Num();
	FMeshTri* Triangles = TriangleCount ? &Mesh->Tris(0) : NULL;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	if( Mesh->DCRuns.Num() )
	{
		TriangleCount = 0;
#if defined(PLATFORM_DREAMCAST)
		for( INT Run=0; Run<View.Runs->Num(); ++Run ) TriangleCount+=(*View.Runs)(Run).Count-2;
#else
		for( INT Run = 0; Run < Mesh->DCRuns.Num(); ++Run )
			TriangleCount += Mesh->DCRuns(Run).Count - 2;
#endif
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
#if defined(PLATFORM_DREAMCAST)
	if( GDCPreparedMeshStamp != GFrameStamp )
	{
		GDCPreparedMeshes = NULL;
		GDCPreparedMeshBytes = 0;
		GDCPreparedMeshStamp = GFrameStamp;
	}
	FDCPreparedMesh* Prepared = NULL;
	const DWORD CacheInputFlags = ExtraFlags;
	UBOOL CacheEligible = GDCMeshOptimize && Mesh->DCRuns.Num() && !Owner->bParticles
		&& (ExtraFlags & PF_Flat) && Frame->Viewport->RenDev->UsesOrderedLists()
		&& !Frame->Viewport->RenDev->SpanBased
		&& GDCOrderedWorldPass >= 0
		&& (Mesh->DCNormalWords.Num() || Mesh->DCNormalStreamData.Size() || Mesh->DCNormalCompressed.Num())
		&& !Frame->Viewport->IsOrtho() && Frame->Viewport->Actor->RendMap!=REN_Wire;
	DWORD MaterialLists = 0;
	UBOOL HasAttachment = 0;
	for( INT m=0; CacheEligible && m<Mesh->DCMaterials.Num(); ++m )
	{
		const DWORD Flags = ExtraFlags | Mesh->DCMaterials(m).Flags;
		MaterialLists |= Flags & (PF_Translucent|PF_Modulated|PF_Highlighted) ? 4 : Flags & PF_Masked ? 2 : 1;
		HasAttachment |= (Flags & PF_Invisible)!=0;
		// Native draws copy every strip/fallback corner before UV, winding or
		// clipping work. Two-sided masked faces therefore cannot mutate the
		// shared prepared samples. Keep special shading on its existing path.
		// Invisible attachment faces are replayed from those samples each pass.
		if( !(Flags & PF_Invisible) && (Flags & (PF_Environment|PF_Unlit)) )
			CacheEligible = 0;
	}
	CacheEligible = CacheEligible && (HasAttachment || (MaterialLists & (MaterialLists-1)));
	if( CacheEligible )
	{
		for( Prepared=GDCPreparedMeshes; Prepared; Prepared=Prepared->Next )
			if( Prepared->Frame==Frame && Prepared->Owner==Owner && Prepared->Mesh==Mesh
				&& Prepared->Mirror==Frame->Mirror
				&& Prepared->Lod==View.Lod
				&& Prepared->InputFlags==ExtraFlags && !appMemcmp(&Prepared->Coords,&Coords,sizeof(Coords)) )
				break;
	}
	const INT CacheBytes = sizeof(FDCPreparedMesh) + sizeof(FDCMeshDrawCache)
		+ Mesh->FrameVerts*(sizeof(FTransTexture)+sizeof(FDCMeshDrawVertex)+sizeof(BYTE))
		+ TriangleCount*sizeof(FDCVisibleRange);
	CacheEligible = CacheEligible && (Prepared || GDCPreparedMeshBytes+CacheBytes <= 128*1024);
	if( CacheEligible && !Prepared ) GDCPreparedMeshBytes += CacheBytes;
	FDCPreparedMesh* DrawCacheOwner=Prepared;
#endif

#if 0
	// For testing actor span clipping.
	if( SpanBuffer )
		for( INT i=SpanBuffer->StartY; i<SpanBuffer->EndY; i++ )
			for( FSpan* Span=SpanBuffer->Index[i-SpanBuffer->StartY]; Span; Span=Span->Next )
				appMemset( Frame->Screen(Span->Start,i), appRand(), (Span->End-Span->Start)*4 );
#endif

	// Get transformed verts.
	FTransTexture* Samples=NULL;
#if defined(PLATFORM_DREAMCAST)
	BYTE* ActiveMeshlets=NULL;
	BYTE* ActiveVertices=Prepared ? NULL : DCSelectMeshlets(Frame,Mesh,Owner,Coords,ActiveMeshlets,View);
	if(ActiveVertices)
	{
		UBOOL Any=0;
		for(INT v=0;v<Mesh->FrameVerts && !Any;++v) Any=ActiveVertices[v]!=0;
		if(!Any) { HasSpecialCoords=0; STAT(uunclock(GStat.MeshTime)); Mark.Pop(); return; }
	}
#endif
	UBOOL bWire=0;
	BYTE Outcode = FVF_OutReject;
#if defined(PLATFORM_DREAMCAST)
	if( Prepared )
	{
		DCFrameCount(DCFC_MeshPreparedReuse);
		Samples = Prepared->Samples;
		DCCookedNormals = 1;
		Outcode = 0;
	}
	else
#endif
	{
	guardSlow(Transform);
	STAT(uclock(GStat.MeshGetFrameTime));
#if defined(PLATFORM_DREAMCAST)
	DCFrameEnter( DCFS_MeshFrame );
#endif
#if defined(PLATFORM_DREAMCAST)
	Samples = New<FTransTexture>(CacheEligible ? GSceneMem : GMem,Mesh->FrameVerts);
#else
	Samples = New<FTransTexture>(GMem,Mesh->FrameVerts);
#endif
	bWire = Frame->Viewport->IsOrtho() || Frame->Viewport->Actor->RendMap==REN_Wire;
#if defined(PLATFORM_DREAMCAST)
	Mesh->GetFrame( &Samples->Point, sizeof(Samples[0]), bWire ? GMath.UnitCoords : Coords, Owner, ActiveVertices );
#else
	Mesh->GetFrame( &Samples->Point, sizeof(Samples[0]), bWire ? GMath.UnitCoords : Coords, Owner );
#endif
#if defined(PLATFORM_DREAMCAST)
	if( Mesh->DCNormalWords.Num() || Mesh->DCNormalStreamData.Size()
		|| Mesh->DCNormalCompressed.Num() )
	{
		DCVertexNormals = New<FVector>( GMem, Mesh->FrameVerts );
		DCCookedNormals = Mesh->GetDCCookedNormals( DCVertexNormals,
			bWire ? GMath.UnitCoords : Coords, Owner, ActiveVertices );
		if( DCCookedNormals )
			DCFrameCount( DCFC_MeshCookedNormals, Mesh->FrameVerts );
	}
	if(!DCCookedNormals && (ActiveVertices || View.Lod))
	{
		// A missing/unsupported normal pose must use the complete legacy input.
		View=FDCMeshView(Mesh,0); ActiveVertices=NULL; ActiveMeshlets=NULL;
		TriangleCount=0;
		for(INT r=0;r<Mesh->DCRuns.Num();++r) TriangleCount+=Mesh->DCRuns(r).Count-2;
		CacheEligible=0;
		Mesh->GetFrame(&Samples->Point,sizeof(Samples[0]),bWire ? GMath.UnitCoords : Coords,Owner);
	}
	DCFrameLeave( DCFS_MeshFrame );
#endif
	STAT(uunclock(GStat.MeshGetFrameTime));
	unguardSlow;

	// Compute outcodes.
	guardSlow(Outcode);
#if defined(PLATFORM_DREAMCAST)
	DCFrameEnter( DCFS_MeshOutcode );
#endif
	for( INT i=0; i<Mesh->FrameVerts; i++ )
	{
		Samples[i].Light.R = -1;
#if defined(PLATFORM_DREAMCAST)
		if( ActiveVertices && !ActiveVertices[i] ) { Samples[i].Flags=FVF_OutReject; continue; }
#endif
		Samples[i].ComputeOutcode( Frame );
		Outcode &= Samples[i].Flags;
	}
#if defined(PLATFORM_DREAMCAST)
	DCFrameLeave( DCFS_MeshOutcode );
#endif
	unguardSlow;
	}

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
#if defined(PLATFORM_DREAMCAST)
	FDCVisibleRange* NativeRanges=NULL;
	INT NumNativeRanges=0;
	if( Prepared )
	{
		TriPool = Prepared->Triangles;
		VisibleTriangles = Prepared->Count;
		NativeRanges=Prepared->Ranges; NumNativeRanges=Prepared->NumRanges;
	}
	else
#endif
	if( Outcode == 0 )
	{
		// Process triangles.
		guardSlow(Process);
#if defined(PLATFORM_DREAMCAST)
		DCFrameEnter( DCFS_MeshPrepare );
		DCFrameEnter( DCFS_MeshPrepareSetup );
		DCFrameCount( DCFC_MeshPrepareTris, TriangleCount );
#endif
#if defined(PLATFORM_DREAMCAST)
		if( GDCMeshOptimize && Mesh->DCRuns.Num() && DCCookedNormals && !Frame->Viewport->RenDev->SpanBased )
			NativeRanges=New<FDCVisibleRange>(CacheEligible ? GSceneMem : GMem,TriangleCount);
		else TriPool = New<FMeshTriSort>(CacheEligible ? GSceneMem : GMem,TriangleCount);
#else
		TriPool = New<FMeshTriSort>(GMem,TriangleCount);
#endif
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
		FMeshTriSort* TriTop = TriPool;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		FDCMeshTriangleCursor DCTriangleCursor( *Mesh );
#endif
#if defined(PLATFORM_DREAMCAST)
		DCFrameLeave( DCFS_MeshPrepareSetup );
		// Batch whole phases so timer granularity cannot be amplified by
		// extrapolating individual microsecond-long triangle samples.
		if( NativeRanges )
			VisibleTriangles=DCBuildVisibleRanges(Frame,Mesh,Samples,ExtraFlags,NativeRanges,NumNativeRanges,ActiveMeshlets,View);
		else if( Mesh->DCRuns.Num() && DCCookedNormals )
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
#if defined(PLATFORM_DREAMCAST)
		if( Prepared )
			ExtraFlags = Prepared->LitFlags;
		else
#endif
		{
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
		if( NativeRanges )
		{
			for( INT r=0; r<NumNativeRanges; ++r )
			{
				const FDCVisibleRange& Range=NativeRanges[r];
				const FDCMeshRun& Run=(*View.Runs)(Range.Run);
				for( INT v=Range.First-2; v<=Range.Last; ++v )
				{
					const INT Index=(*View.Indices)(Run.First+v);
					if( Samples[Index].Light.R==-1 )
					{ Samples[Index].Light.R=-2; VisibleVerts[UniqueVerts++]=Index; }
				}
			}
		}
		else
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
		GLightManager->LightBatch(Samples,VisibleVerts,UniqueVerts,ExtraFlags);
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
#if defined(PLATFORM_DREAMCAST)
		// Only the native corner-copy draw path may publish shared samples.
		if( CacheEligible && DCCookedNormals && NativeRanges )
		{
			FDCPreparedMesh* Entry = New<FDCPreparedMesh>(GSceneMem);
			Entry->Next=GDCPreparedMeshes; GDCPreparedMeshes=Entry;
			Entry->Frame=Frame; Entry->Owner=Owner; Entry->Mesh=Mesh; Entry->Coords=Coords;
			Entry->Mirror=Frame->Mirror;
			Entry->InputFlags=CacheInputFlags;
			Entry->LitFlags=ExtraFlags; Entry->Samples=Samples; Entry->Triangles=TriPool;
			Entry->Count=VisibleTriangles;
			Entry->Ranges=NativeRanges; Entry->NumRanges=NumNativeRanges;
			Entry->Lod=View.Lod;
			Entry->DrawCache=NULL;
			DrawCacheOwner=Entry;
		}
#endif
		}

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
		FTransTexture* StripCorners = NULL;
		INT StripCapacity=0;
		FDCMeshDrawCache* DrawCache=DrawCacheOwner ? DrawCacheOwner->DrawCache : NULL;
		if( UseStrips && NativeRanges && !DrawCache )
		{
			DC_FRAME_SCOPE(DCFS_MeshDrawCache);
			FMemStack& DrawMem=DrawCacheOwner ? GSceneMem : GMem;
			DrawCache=New<FDCMeshDrawCache>(DrawMem);
			DrawCache->Count=Mesh->FrameVerts;
			DrawCache->Vertices=New<FDCMeshDrawVertex>(DrawMem,Mesh->FrameVerts);
			DrawCache->State=New<BYTE>(DrawMem,Mesh->FrameVerts);
			appMemset(DrawCache->State,0,Mesh->FrameVerts);
			if( DrawCacheOwner ) DrawCacheOwner->DrawCache=DrawCache;
		}
		{
			DC_FRAME_SCOPE(DCFS_MeshDrawCleanup);
			Frame->Viewport->RenDev->BeginCookedMesh();
		}
		FDCMeshTriangleCursor DCDrawCursor( *Mesh );
#endif

#if defined(PLATFORM_DREAMCAST)
		if( NativeRanges )
			DCDrawVisibleRanges(Frame,Mesh,Samples,SpanBuffer,ExtraFlags,NativeRanges,NumNativeRanges,
				StripPts,StripCorners,StripCapacity,UseStrips,View,DrawCache);
		else
#endif
		for( INT i=0; i<VisibleTriangles; i++ )
		{
#if defined(PLATFORM_DREAMCAST)
			DC_FRAME_SCOPE(DCFS_MeshDrawFallback);
#endif
			// Set up the triangle.
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
			FMeshTri& Tri = TriPool[i].Tri;
#else
			FMeshTri& Tri = *TriPool[i].Tri;
#endif

#if defined(PLATFORM_DREAMCAST)
			// Mixed-list meshes share preparation but submit only this list.
			// Do not pay clipping/UV/strip-copy work for rejected submissions.
			if( GDCMeshOptimize && !(Tri.PolyFlags & PF_Invisible)
				&& !Frame->Viewport->RenDev->WantsMeshFlags(Tri.PolyFlags | ExtraFlags) )
				continue;
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

					if( End >= i )
					{
						INT Index = Tri.TextureIndex;
						FTextureInfo& StripInfo = Textures[Index] ? TextureInfo[Index] : EnvironmentInfo;
						UScale = StripInfo.UScale * StripInfo.USize / 256.0;
						VScale = StripInfo.VScale * StripInfo.VSize / 256.0;
						if( EmitMeshStrip( Frame, Mesh, Samples, SpanBuffer, StripInfo, StripFlags,
								Mesh->DCRuns(Run), TriPool[i].StripVert - 2, TriPool[End].StripVert,
								StripPts, StripCorners, StripCapacity ) )
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
		{
		DC_FRAME_SCOPE(DCFS_MeshDrawCleanup);
		Frame->Viewport->RenDev->EndCookedMesh();
#endif
#if defined(PLATFORM_DREAMCAST)
		if( !Prepared )
#endif
			GLightManager->FinishActor();
#if defined(PLATFORM_DREAMCAST)
		}
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
