#include <kos.h>
#include <malloc.h>

#include "PVRDrvPrivate.h"

extern DLL_IMPORT const char* GStartupDbgDev;
static pvr_dr_state_t GPVRDRState;
static pvr_list_t GPVRCurrentList = (pvr_list_t)-1;
static unsigned GPVRBegunMask = 0; // bit0 OP, bit1 PT, bit2 TR
static INT GPVRDebugVertsLeft = 0; // per frame debug budget

// Software viewport matrix for PVR (row-major 4x4)
static FLOAT GPVRScreenView[4][4];

static inline void InitScreenViewMatrix()
{
    GPVRScreenView[0][0] = 1.0f; GPVRScreenView[0][1] = 0.0f; GPVRScreenView[0][2] = 0.0f; GPVRScreenView[0][3] = 0.0f;
    GPVRScreenView[1][0] = 0.0f; GPVRScreenView[1][1] = 1.0f; GPVRScreenView[1][2] = 0.0f; GPVRScreenView[1][3] = 0.0f;
    GPVRScreenView[2][0] = 0.0f; GPVRScreenView[2][1] = 0.0f; GPVRScreenView[2][2] = 1.0f; GPVRScreenView[2][3] = 0.0f;
    GPVRScreenView[3][0] = 0.0f; GPVRScreenView[3][1] = 0.0f; GPVRScreenView[3][2] = 0.0f; GPVRScreenView[3][3] = 1.0f;
}

static inline void SetViewportScreenView( FLOAT X, FLOAT Y, FLOAT Width, FLOAT Height )
{
    GPVRScreenView[0][0] = -Width * 0.5f;
    GPVRScreenView[1][1] =  Height * 0.5f;
    GPVRScreenView[2][2] =  1.0f;
    GPVRScreenView[3][0] = -GPVRScreenView[0][0] + X;
    GPVRScreenView[3][1] =  Height - (GPVRScreenView[1][1] + Y);
}

static inline void DebugDumpVec( const char* Label, FLOAT X, FLOAT Y, FLOAT Z )
{
    debugf( "%s = (%.4f, %.4f, %.4f)", Label, X, Y, Z );
}

static inline void DebugDumpCoords( const FCoords& C )
{
    DebugDumpVec( "Origin", C.Origin.X, C.Origin.Y, C.Origin.Z );
    DebugDumpVec( "XAxis", C.XAxis.X, C.XAxis.Y, C.XAxis.Z );
    DebugDumpVec( "YAxis", C.YAxis.X, C.YAxis.Y, C.YAxis.Z );
    DebugDumpVec( "ZAxis", C.ZAxis.X, C.ZAxis.Y, C.ZAxis.Z );
}

static inline void ProjectToScreenUE( const FSceneNode* Frame, FLOAT VX, FLOAT VY, FLOAT VZ, FLOAT& SX, FLOAT& SY, FLOAT& SZ )
{
    const FLOAT InvZ = (VZ != 0.0f) ? (1.0f / VZ) : 1.0f;
    const FLOAT RZ   = Frame->Proj.Z * InvZ;
    SX = VX * RZ + Frame->FX2;
    SY = VY * RZ + Frame->FY2;
    SZ = InvZ;
}

struct FPVRClipVert
{
    FLOAT X, Y, Z;
    FLOAT U, V;
    DWORD ARGB;
};
static inline INT ClipPolyNear( const FPVRClipVert* InVerts, INT InCount, FPVRClipVert* OutVerts )
{
    const FLOAT NearZ = 1.0f;
    if( InCount <= 0 )
        return 0;
    FPVRClipVert Temp[64];
    const FPVRClipVert* Src = InVerts;
    FPVRClipVert* Dst = Temp;
    INT SrcCount = InCount;
    INT DstCount = 0;

    FPVRClipVert S = Src[SrcCount - 1];
    UBOOL SInside = (S.Z > NearZ);
    for( INT i = 0; i < SrcCount; i++ )
    {
        const FPVRClipVert E = Src[i];
        const UBOOL EInside = (E.Z > NearZ);
        if( SInside && EInside )
        {
            Dst[DstCount++] = E;
        }
        else if( SInside && !EInside )
        {
            const FLOAT T = (NearZ - S.Z) / (E.Z - S.Z);
            FPVRClipVert I;
            I.X = S.X + T * (E.X - S.X);
            I.Y = S.Y + T * (E.Y - S.Y);
            I.Z = NearZ;
            I.U = S.U + T * (E.U - S.U);
            I.V = S.V + T * (E.V - S.V);
            I.ARGB = S.ARGB; // color lerp could be added if needed
            Dst[DstCount++] = I;
        }
        else if( !SInside && EInside )
        {
            const FLOAT T = (NearZ - S.Z) / (E.Z - S.Z);
            FPVRClipVert I;
            I.X = S.X + T * (E.X - S.X);
            I.Y = S.Y + T * (E.Y - S.Y);
            I.Z = NearZ;
            I.U = S.U + T * (E.U - S.U);
            I.V = S.V + T * (E.V - S.V);
            I.ARGB = E.ARGB;
            Dst[DstCount++] = I;
            Dst[DstCount++] = E;
        }
        S = E; SInside = EInside;
    }

    // Copy back to OutVerts
    for( INT i = 0; i < DstCount; i++ )
        OutVerts[i] = Dst[i];
    return DstCount;
}

static inline void PVRHeaderSubmit( const pvr_poly_hdr_t& Hdr )
{
    pvr_poly_hdr_t* Out = (pvr_poly_hdr_t*)pvr_dr_target( GPVRDRState );
    Out->cmd   = Hdr.cmd;
    Out->mode1 = Hdr.mode1;
    Out->mode2 = Hdr.mode2;
    Out->mode3 = Hdr.mode3;
    pvr_dr_commit( Out );
}

static inline void PVRVertexSubmit( FLOAT X, FLOAT Y, FLOAT Z, FLOAT U, FLOAT V, DWORD ARGB, unsigned Flags )
{
    pvr_vertex_t* Vtx = (pvr_vertex_t*)pvr_dr_target( GPVRDRState );
    Vtx->flags = Flags;
    Vtx->x     = X;
    Vtx->y     = Y;
    Vtx->z     = Z;
    Vtx->u     = U;
    Vtx->v     = V;
    Vtx->argb  = ARGB;
    Vtx->oargb = 0;
    pvr_dr_commit( Vtx );
}

static inline void SubmitTriangleFan( const FSceneNode* Frame, const FPVRClipVert* V, INT Count, UBOOL SubmitUV )
{
    if( Count < 3 )
        return;
    for( INT j = 1; j < Count - 1; ++j )
    {
        FLOAT sx, sy, sz;
        ProjectToScreenUE( Frame, V[0].X, V[0].Y, V[0].Z, sx, sy, sz );
        if( SubmitUV ) PVRVertexSubmit( sx, sy, sz, V[0].U, V[0].V, V[0].ARGB, PVR_CMD_VERTEX );
        else           PVRVertexSubmit( sx, sy, sz, 0.f,   0.f,   V[0].ARGB, PVR_CMD_VERTEX );

        ProjectToScreenUE( Frame, V[j].X, V[j].Y, V[j].Z, sx, sy, sz );
        if( SubmitUV ) PVRVertexSubmit( sx, sy, sz, V[j].U, V[j].V, V[j].ARGB, PVR_CMD_VERTEX );
        else           PVRVertexSubmit( sx, sy, sz, 0.f,   0.f,   V[j].ARGB, PVR_CMD_VERTEX );

        ProjectToScreenUE( Frame, V[j+1].X, V[j+1].Y, V[j+1].Z, sx, sy, sz );
        if( SubmitUV ) PVRVertexSubmit( sx, sy, sz, V[j+1].U, V[j+1].V, V[j+1].ARGB, PVR_CMD_VERTEX_EOL );
        else           PVRVertexSubmit( sx, sy, sz, 0.f,     0.f,     V[j+1].ARGB, PVR_CMD_VERTEX_EOL );
    }
}


static inline void EnsurePVRList( pvr_list_t List )
{
    // If already on desired list, nothing to do
    if( GPVRCurrentList == List )
        return;

    // If desired list was already begun earlier this scene, we cannot reopen; keep current
    const unsigned Bit = (List == PVR_LIST_OP_POLY) ? 1u : (List == PVR_LIST_PT_POLY) ? 2u : 4u;
    if( GPVRBegunMask & Bit )
        return;

    // Close currently open list if any
    if( GPVRCurrentList != (pvr_list_t)-1 )
        pvr_list_finish();

    // Open desired list
    pvr_list_begin( List );
    pvr_dr_init( &GPVRDRState );
    GPVRCurrentList = List;
    GPVRBegunMask |= Bit;
}

static inline void BuildPolyHeader( UPVRRenderDevice* RD, DWORD PolyFlags, const FTextureInfo* Info, pvr_poly_hdr_t& OutHdr, pvr_list_t& OutList, UBOOL ForceNoDepthTest = 0 )
{
    const UBOOL IsMasked = (PolyFlags & PF_Masked) != 0;
    const UBOOL IsTrans  = (PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted)) != 0;
    OutList = IsMasked ? PVR_LIST_PT_POLY : (IsTrans ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);

    pvr_poly_cxt_t Cxt;
    if( Info && RD->TexInfo.CurrentBind && RD->TexInfo.CurrentBind->Tex )
    {
        const INT USize = Max( UPVRRenderDevice::MinTexSize, Info->USize );
        const INT VSize = Max( UPVRRenderDevice::MinTexSize, Info->VSize );
        int PvrFmt = (Info->Palette ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_RGB565);
        if( Info->Format == TEXF_EXT_ARGB1555_VQ )
            PvrFmt |= PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE; // VQ data is pre-twiddled
        else
            PvrFmt |= PVR_TXRFMT_NONTWIDDLED; // we upload linear data
        pvr_poly_cxt_txr( &Cxt, OutList, PvrFmt, USize, VSize, RD->TexInfo.CurrentBind->Tex, RD->NoFiltering ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR );
    }
    else
    {
        pvr_poly_cxt_col( &Cxt, OutList );
    }

    // Depth, culling and blend defaults
    if( ForceNoDepthTest )
    {
        Cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        Cxt.depth.write      = PVR_DEPTHWRITE_DISABLE;
    }
    else
    {
        // With z = 1/w, nearer has larger z; use GEQUAL and write on OP and PT
        Cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        Cxt.depth.write      = (OutList != PVR_LIST_TR_POLY);
    }
    Cxt.gen.culling      = PVR_CULLING_NONE; // GL path didn't use backface culling
    if( OutList == PVR_LIST_TR_POLY )
    {
        if( PolyFlags & PF_Translucent )
        {
            Cxt.blend.src = PVR_BLEND_SRCALPHA;
            Cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        }
        else if( PolyFlags & PF_Modulated )
        {
            // Multiply dest by src (lightmaps): dst = dst * src
            Cxt.blend.src = PVR_BLEND_DESTCOLOR;
            Cxt.blend.dst = PVR_BLEND_ZERO;
        }
        else if( PolyFlags & PF_Highlighted )
        {
            Cxt.blend.src = PVR_BLEND_ONE;
            Cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        }
        else
        {
            Cxt.blend.src = PVR_BLEND_ONE;
            Cxt.blend.dst = PVR_BLEND_ZERO;
        }
    }
    Cxt.gen.fog_type = PVR_FOG_DISABLE;

    pvr_poly_compile( &OutHdr, &Cxt );
}
pvr_init_params_t params = {
	{ PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8 },
	2536 * 256,    /* vertex buffer */
	0,             /* dma disabled for TA */
	0,             /* fsaa off */
	0,             /* keep PVR translucent autosort ON (or tune per need) */
	2            /* OPB count: start with 8; only consider 2 after profiling */
};

/*-----------------------------------------------------------------------------
	Global implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_PACKAGE(PVRDrv);
IMPLEMENT_CLASS(UPVRRenderDevice);

/*-----------------------------------------------------------------------------
	UPVRRenderDevice implementation.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::InternalClassInitializer( UClass* Class )
{
	guardSlow(UPVRRenderDevice::InternalClassInitializer);
	new(Class, "NoFiltering",  RF_Public)UBoolProperty( CPP_PROPERTY(NoFiltering),  "Options", CPF_Config );
	unguardSlow;
}

static UPVRRenderDevice* GPVRDeviceInstance = NULL;

UPVRRenderDevice::UPVRRenderDevice()
{
	NoFiltering = false;
	VRAMUsed = 0;
}

UBOOL UPVRRenderDevice::Init( UViewport* InViewport )
{
	guard(UPVRRenderDevice::Init)

	// if we were using fb dbgio, disable it before initializing PVR
	const char* DbgDev = dbgio_dev_get();
	if( DbgDev && !appStrcmp( DbgDev, "fb" ) )
	{
		// try to drop back to whatever we had at startup first
		if( !GStartupDbgDev || dbgio_dev_select( GStartupDbgDev ) < 0 )
			dbgio_dev_select( "scif" );
	}

    pvr_init(&params);
    InitScreenViewMatrix();
	SupportsFogMaps = false; // true;
	SupportsDistanceFog = false; // true;
	NoVolumetricBlend = true;

	ComposeSize = 0;
	EnsureComposeSize( 256 * 256 * 2 );

    // PVR: no fixed function matrices; we keep a software viewport matrix.
    // Initialized in InitScreenViewMatrix() and updated from SetSceneNode/viewport.

    // Set default background color; per-frame clear is done in Lock via pvr_set_bg_color.
    pvr_set_bg_color( 0.f, 0.f, 0.f );

	PrintMemStats();

	CurrentPolyFlags = PF_Occlude;
	Viewport = InViewport;
	GPVRDeviceInstance = this;

	return true;
	unguard;
}

void UPVRRenderDevice::Exit()
{
	guard(UPVRRenderDevice::Exit);

	debugf( NAME_Log, "Shutting down OpenGL renderer" );

	Flush();

	if( Compose )
	{
		appFree( Compose );
		Compose = NULL;
	}
	ComposeSize = 0;
	GPVRDeviceInstance = NULL;

	unguard;
}

void UPVRRenderDevice::Flush()
{
	guard(UPVRRenderDevice::Flush);

	ResetTexture();

	if( BindMap.Size() )
	{
		debugf( NAME_Log, "Flushing %d textures", BindMap.Size() );
		for( INT i = 0; i < BindMap.Size(); ++i )
		{
			if( BindMap[i].Tex )
			{
				pvr_mem_free( BindMap[i].Tex );
				if( BindMap[i].SizeBytes > 0 && VRAMUsed >= (DWORD)BindMap[i].SizeBytes )
					VRAMUsed -= (DWORD)BindMap[i].SizeBytes;
				BindMap[i].Tex = NULL;
				BindMap[i].SizeBytes = 0;
			}
		}
		BindMap.Empty();
	}

	unguard;
}

UBOOL UPVRRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
	return false;
}

void UPVRRenderDevice::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
	guard(UPVRRenderDevice::Lock);

	// Begin a new PVR scene and the per-list streams. Background clear via bg color.
    pvr_wait_ready();
    pvr_set_bg_color( ScreenClear.X, ScreenClear.Y, ScreenClear.Z );
    pvr_scene_begin();
    // Defer list begin until first submission; reset tracking
    GPVRCurrentList = (pvr_list_t)-1;
    GPVRBegunMask = 0;
    GPVRDebugVertsLeft = 0; // set to e.g. 8 to dump

	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
		ColorMod = FPlane( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.f - Min( FlashScale.X * 2.f, 1.f ) );
	else
		ColorMod = FPlane( 0.f, 0.f, 0.f, 0.f );

	unguard;
}

// Forward decl for engine mem dump
ENGINE_API void DumpMemStatsDC( const char* Tag );

void UPVRRenderDevice::Unlock( UBOOL Blit )
{
	guard(UPVRRenderDevice::Unlock);

	static DWORD Frame = 0;

    // Finish any open list, then finish scene.
    if( GPVRCurrentList != (pvr_list_t)-1 )
        pvr_list_finish();
	pvr_scene_finish();

	++Frame;
	// One-shot after first scene
	if( Frame == 1 )
	{
		DumpMemStatsDC( "after first scene" );
	}
	// Periodic
	if( ( Frame & 0xff ) == 0 )
	{
		debugf( "Frame %d", Frame );
		PrintMemStats();
		// After ~256 frames
		DumpMemStatsDC( "after several scenes" );
	}

	unguard;
}

void UPVRRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guard(UPVRRenderDevice::DrawComplexSurface);

	check(Surface.Texture);

    SetSceneNode( Frame );

	// @HACK: Don't draw translucent and masked parts of the sky. Don't know how to do that yet.
	if( CurrentSceneNode.bIsSky && ( Surface.PolyFlags & (PF_Translucent|PF_Masked) ))
		return;

	FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;

    // Draw base texture pass.
    SetBlend( Surface.PolyFlags );
    SetTexture( *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
    {
        pvr_poly_hdr_t Hdr; pvr_list_t List;
        BuildPolyHeader( this, Surface.PolyFlags, Surface.Texture, Hdr, List );
        EnsurePVRList( List );

        const DWORD White = 0xFFFFFFFFu;
        for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
        {
            FPVRClipVert In[64]; FPVRClipVert Clipped[64];
            INT N = 0;
            for( INT i = 0; i < Poly->NumPts && N < 64; i++ )
            {
                const FVector& V = Poly->Pts[i]->Point; // already in view space
                In[N].X = V.X; In[N].Y = V.Y; In[N].Z = V.Z;
                In[N].U = ( (Facet.MapCoords.XAxis | V) - UDot - TexInfo.UPan ) * TexInfo.UMult;
                In[N].V = ( (Facet.MapCoords.YAxis | V) - VDot - TexInfo.VPan ) * TexInfo.VMult;
                In[N].ARGB = White;
                N++;
            }
            if( GPVRDebugVertsLeft > 0 )
            {
                debugf( "---- Frame/Coords ----" );
                DebugDumpCoords( Frame->Coords );
                debugf( "RProjZ=%.4f Aspect=%.4f FX=%.4f FY=%.4f", RProjZ, Aspect, (FLOAT)Frame->FX, (FLOAT)Frame->FY );
                for( INT di=0; di<Min(N,3); ++di )
                {
                    debugf( "Pw[%d]", di ); DebugDumpVec( "view", In[di].X, In[di].Y, In[di].Z );
                    FLOAT sx, sy, sz; ProjectToScreenUE( Frame, In[di].X, In[di].Y, In[di].Z, sx, sy, sz );
                    debugf( "screen = (%.2f, %.2f, z=%.5f)", sx, sy, sz );
                }
                GPVRDebugVertsLeft--;
            }
            const INT C = ClipPolyNear( In, N, Clipped );
            if( C < 3 )
                continue;
            PVRHeaderSubmit( Hdr );
            SubmitTriangleFan( Frame, Clipped, C, 1 );
        }
    }

	// Draw lightmap.
	// @HACK: Unless this is the sky. See above.
    if( Surface.LightMap && !CurrentSceneNode.bIsSky )
    {
        SetBlend( PF_Modulated );
        SetTexture( *Surface.LightMap, 0, -0.5f );
        pvr_poly_hdr_t Hdr; pvr_list_t List;
        BuildPolyHeader( this, PF_Modulated | (Surface.PolyFlags & PF_Masked), Surface.LightMap, Hdr, List );
        EnsurePVRList( List );

        const DWORD White = 0xFFFFFFFFu;
        for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
        {
            FPVRClipVert In[64]; FPVRClipVert Clipped[64];
            INT N = 0;
            for( INT i = 0; i < Poly->NumPts && N < 64; i++ )
            {
                const FVector& V = Poly->Pts[i]->Point;
                In[N].X = V.X; In[N].Y = V.Y; In[N].Z = V.Z;
                In[N].U = ( (Facet.MapCoords.XAxis | V) - UDot - TexInfo.UPan ) * TexInfo.UMult;
                In[N].V = ( (Facet.MapCoords.YAxis | V) - VDot - TexInfo.VPan ) * TexInfo.VMult;
                In[N].ARGB = White;
                N++;
            }
            const INT C = ClipPolyNear( In, N, Clipped );
            if( C < 3 )
                continue;
            PVRHeaderSubmit( Hdr );
            SubmitTriangleFan( Frame, Clipped, C, 1 );
        }
    }

	// Draw fog.
	/*
	if( Surface.FogMap )
	{
		SetBlend( PF_Highlighted );
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_EQUAL );
		SetTexture( *Surface.FogMap, 0, -0.5f );
		for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
		{
			glBegin( GL_TRIANGLE_FAN );
			for( INT i = 0; i < Poly->NumPts; i++ )
			{
				FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
				FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
				glTexCoord2f( (U-UDot-TexInfo.UPan)*TexInfo.UMult, (V-VDot-TexInfo.VPan)*TexInfo.VMult );
				glVertex3f( Poly->Pts[i]->Point.X, Poly->Pts[i]->Point.Y, Poly->Pts[i]->Point.Z );
			}
			glEnd();
		}
		if( Surface.PolyFlags & PF_Masked )
			glDepthFunc( GL_LEQUAL );
	}
	*/

	unguard;
}

void UPVRRenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer )
{
	guard(UPVRRenderDevice::DrawGouraudPolygon);

	SetSceneNode( Frame );
	SetBlend( PolyFlags );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0 );

	const UBOOL IsModulated = ( PolyFlags & PF_Modulated );

	// Build header for textured gouraud fan
    pvr_poly_hdr_t Hdr; pvr_list_t List;
    BuildPolyHeader( this, PolyFlags, &Texture, Hdr, List );
	EnsurePVRList( List );
	PVRHeaderSubmit( Hdr );

    {
        FPVRClipVert In[64]; FPVRClipVert Clipped[64];
        INT N = 0;
        for( INT i = 0; i < NumPts && N < 64; i++ )
        {
            FTransTexture* P = Pts[i];
            In[N].X = P->Point.X; In[N].Y = P->Point.Y; In[N].Z = P->Point.Z;
            In[N].U = P->U * TexInfo.UMult;
            In[N].V = P->V * TexInfo.VMult;
            if( IsModulated ) In[N].ARGB = 0xFFFFFFFFu; else {
                const BYTE R = (BYTE)Clamp<INT>(appRound(P->Light.X * 255.f), 0, 255);
                const BYTE G = (BYTE)Clamp<INT>(appRound(P->Light.Y * 255.f), 0, 255);
                const BYTE B = (BYTE)Clamp<INT>(appRound(P->Light.Z * 255.f), 0, 255);
                In[N].ARGB = (255u<<24) | (R<<16) | (G<<8) | (B);
            }
            N++;
        }
        const INT C = ClipPolyNear( In, N, Clipped );
        if( C >= 3 )
        {
            for( INT i = 0; i < C; i++ )
            {
                const FPVRClipVert& Vtx = Clipped[i];
                FLOAT SX, SY, SZ;
                ProjectToScreenUE( Frame, Vtx.X, Vtx.Y, Vtx.Z, SX, SY, SZ );
                const unsigned Flags = (i == C - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                PVRVertexSubmit( SX, SY, SZ, Vtx.U, Vtx.V, Vtx.ARGB, Flags );
            }
        }
    }

	// Optional fog-only pass
	if( (PolyFlags & (PF_RenderFog|PF_Translucent|PF_Modulated)) == PF_RenderFog )
	{
        pvr_poly_hdr_t FogHdr; pvr_list_t FogList;
        BuildPolyHeader( this, PF_Highlighted, nullptr, FogHdr, FogList );
        EnsurePVRList( FogList );
        PVRHeaderSubmit( FogHdr );
        FPVRClipVert InF[64]; FPVRClipVert ClipF[64];
        INT NF = 0;
        for( INT i = 0; i < NumPts && NF < 64; i++ )
        {
            FTransTexture* P = Pts[i];
            InF[NF].X = P->Point.X; InF[NF].Y = P->Point.Y; InF[NF].Z = P->Point.Z;
            const BYTE A = (BYTE)Clamp<INT>(appRound(P->Fog.W * 255.f), 0, 255);
            const BYTE R = (BYTE)Clamp<INT>(appRound(P->Fog.X * 255.f), 0, 255);
            const BYTE G = (BYTE)Clamp<INT>(appRound(P->Fog.Y * 255.f), 0, 255);
            const BYTE B = (BYTE)Clamp<INT>(appRound(P->Fog.Z * 255.f), 0, 255);
            InF[NF].U = 0.f; InF[NF].V = 0.f; InF[NF].ARGB = (A<<24)|(R<<16)|(G<<8)|B;
            NF++;
        }
        const INT CF = ClipPolyNear( InF, NF, ClipF );
        for( INT i = 0; i < CF; i++ )
        {
            const FPVRClipVert& Vtx = ClipF[i];
            FLOAT SX, SY, SZ;
            ProjectToScreenUE( Frame, Vtx.X, Vtx.Y, Vtx.Z, SX, SY, SZ );
            const unsigned Flags = (i == CF - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            PVRVertexSubmit( SX, SY, SZ, 0.f, 0.f, Vtx.ARGB, Flags );
        }
	}

	unguard;
}

void UPVRRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags )
{
	guard(UPVRRenderDevice::DrawTile);

	// Mark as UI tile so we keep SH4-side data for reloads
	TexInfo.bIsTile = true;

	SetSceneNode( Frame );
	SetBlend( PolyFlags );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );

    pvr_poly_hdr_t Hdr; pvr_list_t List;
    // UI tiles should ignore depth to avoid fighting with world and among tiles
    BuildPolyHeader( this, PolyFlags, &Texture, Hdr, List, /*ForceNoDepthTest=*/1 );
	EnsurePVRList( List );
	PVRHeaderSubmit( Hdr );

	const DWORD ARGB = (PolyFlags & PF_Modulated)
		? 0xFFFFFFFFu
		: ((255u<<24)
			| (BYTE)Clamp<INT>(appRound(Light.X * 255.f),0,255) << 16
			| (BYTE)Clamp<INT>(appRound(Light.Y * 255.f),0,255) << 8
			| (BYTE)Clamp<INT>(appRound(Light.Z * 255.f),0,255));

	const FLOAT U0 = (U   ) * TexInfo.UMult;
	const FLOAT V0 = (V   ) * TexInfo.VMult;
	const FLOAT U1 = (U+UL) * TexInfo.UMult;
	const FLOAT V1 = (V+VL) * TexInfo.VMult;

    // Submit as two independent triangles (no strips in DR): A,B,C and A,C,D
    const FLOAT Ax = X,        Ay = Y;        const FLOAT Au = U0, Av = V0;
    const FLOAT Bx = X + XL,   By = Y;        const FLOAT Bu = U1, Bv = V0;
    const FLOAT Cx = X + XL,   Cy = Y + YL;   const FLOAT Cu = U1, Cv = V1;
    const FLOAT Dx = X,        Dy = Y + YL;   const FLOAT Du = U0, Dv = V1;

    PVRVertexSubmit( Ax, Ay, Z, Au, Av, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( Bx, By, Z, Bu, Bv, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( Cx, Cy, Z, Cu, Cv, ARGB, PVR_CMD_VERTEX_EOL );

    PVRVertexSubmit( Ax, Ay, Z, Au, Av, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( Cx, Cy, Z, Cu, Cv, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( Dx, Dy, Z, Du, Dv, ARGB, PVR_CMD_VERTEX_EOL );

	TexInfo.bIsTile = false;

	unguard;
}

void UPVRRenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )
{

}

void UPVRRenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 )
{

}

void UPVRRenderDevice::EndFlash( )
{
	guard(UPVRRenderDevice::EndFlash);

	if( ColorMod == FPlane( 0.f, 0.f, 0.f, 0.f ) )
		return;

	ResetTexture();
	SetBlend( PF_Highlighted );

    pvr_poly_hdr_t Hdr; pvr_list_t List;
    // Fullscreen flash should ignore depth
    BuildPolyHeader( this, PF_Highlighted, nullptr, Hdr, List, /*ForceNoDepthTest=*/1 );
	EnsurePVRList( List );
	PVRHeaderSubmit( Hdr );

	const BYTE A = (BYTE)Clamp<INT>(appRound(ColorMod.W * 255.f), 0, 255);
	const BYTE R = (BYTE)Clamp<INT>(appRound(ColorMod.X * 255.f), 0, 255);
	const BYTE G = (BYTE)Clamp<INT>(appRound(ColorMod.Y * 255.f), 0, 255);
	const BYTE B = (BYTE)Clamp<INT>(appRound(ColorMod.Z * 255.f), 0, 255);
	const DWORD ARGB = (A<<24) | (R<<16) | (G<<8) | (B);

	const FLOAT W = (FLOAT)Viewport->SizeX;
	const FLOAT H = (FLOAT)Viewport->SizeY;
	const FLOAT Z = 1.f;

    // Two independent triangles for fullscreen quad
    PVRVertexSubmit( 0.f, 0.f, Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( W,   0.f, Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( W,   H,   Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX_EOL );

    PVRVertexSubmit( 0.f, 0.f, Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( W,   H,   Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
    PVRVertexSubmit( 0.f, H,   Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX_EOL );

	unguard;
}

void UPVRRenderDevice::PushHit( const BYTE* Data, INT Count )
{

}

void UPVRRenderDevice::PopHit( INT Count, UBOOL bForce )
{

}

void UPVRRenderDevice::GetStats( char* Result )
{
	guard(UPVRRenderDevice::GetStats)

	if( Result ) *Result = '\0';

	unguard;
}

void UPVRRenderDevice::ReadPixels( FColor* Pixels )
{
	guard(UPVRRenderDevice::ReadPixels);

	unguard;
}

void UPVRRenderDevice::ClearZ( FSceneNode* Frame )
{
	guard(UPVRRenderDevice::ClearZ);

	SetBlend( PF_Occlude );

	unguard;
}

void UPVRRenderDevice::SetSceneNode( FSceneNode* Frame )
{
	guard(UPVRRenderDevice::SetSceneNode);

	check(Viewport);

	if( !Frame )
	{
		// invalidate current saved data
		CurrentSceneNode.X = -1;
		CurrentSceneNode.FX = -1.f;
		CurrentSceneNode.SizeX = -1;
		return;
	}

    if( Frame->X != CurrentSceneNode.X || Frame->Y != CurrentSceneNode.Y ||
            Frame->XB != CurrentSceneNode.XB || Frame->YB != CurrentSceneNode.YB ||
            Viewport->SizeX != CurrentSceneNode.SizeX || Viewport->SizeY != CurrentSceneNode.SizeY )
    {
        SetViewportScreenView( Frame->XB, Frame->YB, Frame->X, Frame->Y );
        CurrentSceneNode.X = Frame->X;
        CurrentSceneNode.Y = Frame->Y;
        CurrentSceneNode.XB = Frame->XB;
        CurrentSceneNode.YB = Frame->YB;
        CurrentSceneNode.SizeX = Viewport->SizeX;
        CurrentSceneNode.SizeY = Viewport->SizeY;
    }

	if( Frame->Level )
	{
		AZoneInfo* ZoneInfo = (AZoneInfo*)Frame->Level->GetZoneActor( Frame->ZoneNumber );
		const UBOOL bIsSky = ( ZoneInfo && ZoneInfo->SkyZone && ZoneInfo->IsA( ASkyZoneInfo::StaticClass ) );
		CurrentSceneNode.bIsSky = bIsSky;
	}

	if( Frame->FX != CurrentSceneNode.FX || Frame->FY != CurrentSceneNode.FY ||
			Viewport->Actor->FovAngle != CurrentSceneNode.FovAngle )
	{
		RProjZ = ftan( Viewport->Actor->FovAngle * (FLOAT)PI / 360.0f );
		Aspect = Frame->FY / Frame->FX;
		RFX2 = 2.0f * RProjZ / Frame->FX;
		RFY2 = 2.0f * RProjZ * Aspect / Frame->FY;
        // Projection is handled in software;
		CurrentSceneNode.FX = Frame->FX;
		CurrentSceneNode.FY = Frame->FY;
		CurrentSceneNode.FovAngle = Viewport->Actor->FovAngle;
	}

	unguard;
}

void UPVRRenderDevice::SetBlend( DWORD PolyFlags, UBOOL InverseOrder )
{
	guard(UPVRRenderDevice::SetBlend);

	// Adjust PolyFlags according to Unreal's precedence rules.
	// @HACK: Unless this is the sky, in which case we want it to never occlude.
	if( !(PolyFlags & (PF_Translucent|PF_Modulated)) && !CurrentSceneNode.bIsSky )
		PolyFlags |= PF_Occlude;
	else if( PolyFlags & PF_Translucent )
		PolyFlags &= ~PF_Masked;

	// Record current flags; BuildPolyHeader will translate them into PVR header state.
	CurrentPolyFlags = PolyFlags;

	unguard;
}

void UPVRRenderDevice::ResetTexture( )
{
	guard(UPVRRenderDevice::ResetTexture);

	TexInfo.CurrentCacheID = 0;
	TexInfo.CurrentBind = nullptr;

	unguard;
}

void UPVRRenderDevice::SetTexture( FTextureInfo& Info, DWORD PolyFlags, FLOAT PanBias )
{
	guard(UPVRRenderDevice::SetTexture);

	// Set panning.
	FTexInfo& Tex = TexInfo;
	Tex.UPan      = Info.Pan.X + PanBias*Info.UScale;
	Tex.VPan      = Info.Pan.Y + PanBias*Info.VScale;

	// Account for all the impact on scale normalization.
	//const INT USize = Max( MinTexSize, Info.USize );
	//const INT VSize = Max( MinTexSize, Info.VSize );
	Tex.UMult = 1.f / (Info.UScale * static_cast<FLOAT>(Info.USize));
	Tex.VMult = 1.f / (Info.VScale * static_cast<FLOAT>(Info.VSize));

	// Find in cache.
	const QWORD NewCacheID = Info.CacheID;
	const UBOOL RealtimeChanged = ( Info.TextureFlags & TF_RealtimeChanged ) != 0;
	if( !RealtimeChanged && NewCacheID == Tex.CurrentCacheID )
		return;

	const QWORD LookupID = NewCacheID & ~0xFFULL;
	const BYTE NewType = NewCacheID & 0xFF;
	FTexBind* Bind = BindMap.Find( LookupID );
	const UBOOL NewTexture = !Bind;
	if( NewTexture )
	{
		// Create new binding entry; VRAM is allocated in UploadTexture.
		Bind = BindMap.Add( LookupID, { 0, NewType, 0 } );
	}

	// Make current.
	Tex.CurrentCacheID = NewCacheID;
	Tex.CurrentBind = Bind;

	if( NewTexture || RealtimeChanged || Bind->LastType != NewType )
	{
		// New texture or it has changed, upload it to VRAM.
		Bind->LastType = NewType;
		Info.TextureFlags &= ~TF_RealtimeChanged;
		UploadTexture( Info, NewTexture );
	}

	unguard;
}

void UPVRRenderDevice::EnsureComposeSize( const DWORD NewSize )
{
	if( NewSize > ComposeSize )
	{
		if( Compose )
			free( Compose );
		Compose = (BYTE*)memalign( 32, NewSize );
		verify( Compose );
		debugf( "GL: Compose size increased %d -> %d", ComposeSize, NewSize );
		ComposeSize = NewSize;
	}
}

void* UPVRRenderDevice::VerticalUpscale( const INT USize, const INT VSize, const INT VTimes )
{
	DWORD i;
	const INT SrcLine = USize << 1;
	const _WORD* Src = (_WORD*)Compose;
	_WORD* NewBase = (_WORD*)Compose + USize * VSize;
	_WORD* Dst = NewBase;

	// at this point U is already at least 8, so we can freely use memcpy4
	for( i = 0; i < VSize; ++i, Src += USize )
	{
		switch( VTimes )
		{
			case 8:
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				[[fallthrough]];
			case 4:
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				[[fallthrough]];
			case 2:
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				[[fallthrough]];
			default:
				memcpy4( Dst, Src, SrcLine ); Dst += USize;
				break;
		}
	}

	return NewBase;
}

void* UPVRRenderDevice::ConvertTextureMipI8( const FMipmap* Mip, const FColor* Palette )
{
	// 8-bit indexed. We have to fix the alpha component since it's mostly garbage.
	DWORD i;
	_WORD* Dst = (_WORD*)Compose;
	const BYTE* Src = (const BYTE*)Mip->DataPtr;
	const DWORD SrcCount = Mip->USize * Mip->VSize;
	INT USize = Mip->USize;
	INT VSize = Mip->VSize;

	EnsureComposeSize( SrcCount * 2 );

	// convert palette; if texture is masked, make first entry transparent
	_WORD DstPal[NUM_PAL_COLORS];
	DstPal[0] = Palette[0].RGB888ToARGB1555() & ~0x8000U;
	for( i = 1; i < ARRAY_COUNT( DstPal ); ++i )
		DstPal[i] = Palette[i].RGB888ToARGB1555();

	// convert and upscale texture horizontally to width = 8 if needed
	const INT UTimes = MinTexSize / USize;
	for( i = 0; i < SrcCount; ++i, ++Src )
	{
		const _WORD C = DstPal[*Src];
		switch( UTimes )
		{
			case 8:
				*Dst++ = C;
				*Dst++ = C;
				*Dst++ = C;
				*Dst++ = C;
				[[fallthrough]];
			case 4:
				*Dst++ = C;
				*Dst++ = C;
				[[fallthrough]];
			case 2:
				*Dst++ = C;
				[[fallthrough]];
			default:
				*Dst++ = C;
				break;
		}
	}
	if( UTimes > 1 )
		USize = MinTexSize;

	// upscale texture vertically to height = 8 if needed
	const INT VTimes = MinTexSize / VSize;
	if( VTimes > 1 )
		return VerticalUpscale( USize, VSize, VTimes );

	return Compose;
}

void* UPVRRenderDevice::ConvertTextureMipBGRA7777( const FMipmap* Mip )
{
	// BGRA8888. This is actually a BGRA7777 lightmap, so we need to scale it.
	DWORD i;
	INT USize = Mip->USize;
	INT VSize = Mip->VSize;
	_WORD* Dst = (_WORD*)Compose;
	const FColor* Src = (const FColor*)Mip->DataPtr;
	const DWORD Count = USize * VSize;

	EnsureComposeSize( Count * 2 );

	// convert and upscale texture horizontally to width = 8 if needed
	const INT UTimes = MinTexSize / USize;
	for( i = 0; i < Count; ++i, ++Src )
	{
		const _WORD C = Src->BGRA7777ToRGB565();
		switch( UTimes )
		{
			case 8:
				*Dst++ = C;
				*Dst++ = C;
				*Dst++ = C;
				*Dst++ = C;
				[[fallthrough]];
			case 4:
				*Dst++ = C;
				*Dst++ = C;
				[[fallthrough]];
			case 2:
				*Dst++ = C;
				[[fallthrough]];
			default:
				*Dst++ = C;
				break;
		}
	}
	if( UTimes > 1 )
		USize = MinTexSize;

	// upscale texture vertically to height = 8 if needed
	const INT VTimes = MinTexSize / VSize;
	if( VTimes > 1 )
		return VerticalUpscale( USize, VSize, VTimes );

	return Compose;
}

void UPVRRenderDevice::UploadTexture( FTextureInfo& Info, const UBOOL NewTexture )
{
	guard(UPVRRenderDevice::UploadTexture);

	if( !Info.Mips[0] )
	{
		debugf( NAME_Warning, "Encountered texture with invalid mips!" );
		return;
	}

	FTexBind* Bind = TexInfo.CurrentBind;
	check(Bind);

	// We currently upload a single base level. VQ data is already twiddled.
	FMipmap* Mip0 = Info.Mips[0];
	// Dreamcast lazy streaming: load mip0 bytes on demand if not resident
	BYTE* DCLoaded = nullptr;
#if defined(PLATFORM_DREAMCAST)
	if( !Mip0->DataPtr )
	{
		if( Info.Texture && Info.Texture->GetLinker() )
		{
			ULinkerLoad* L = Info.Texture->GetLinker();
			FArchiveFileLoad* FL = (FArchiveFileLoad*)L; 
			if( Mip0->DCDataSize > 0 && Mip0->DCDataOffset > 0 )
			{
				FILE* F = appFopen( FL->Filename, "rb" );
				if( F )
				{
					appFseek( F, Mip0->DCDataOffset, USEEK_SET );
					DCLoaded = (BYTE*)appMalloc( Mip0->DCDataSize, "TexStream" );
					if( DCLoaded )
					{
						const INT Ok = appFread( DCLoaded, Mip0->DCDataSize, 1, F );
						if( Ok == 1 )
						{
							Mip0->DataPtr = DCLoaded;
						}
						else
						{
							appFree( DCLoaded );
							DCLoaded = nullptr;
						}
					}
					appFclose( F );
				}
			}
		}
	}
#endif
	if( Info.Format == TEXF_EXT_ARGB1555_VQ )
	{
		const INT SizeBytes =
			(Mip0->DataArray.Num() > 0) ? Mip0->DataArray.Num() :
#if defined(PLATFORM_DREAMCAST)
			Mip0->DCDataSize;
#else
			0;
#endif
		if( Bind->Tex )
		{
			pvr_mem_free( Bind->Tex );
			if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
				VRAMUsed -= (DWORD)Bind->SizeBytes;
			Bind->Tex = NULL;
			Bind->SizeBytes = 0;
		}
		Bind->Tex = pvr_mem_malloc( SizeBytes );
		if( Bind->Tex )
		{
			pvr_txr_load( Mip0->DataPtr ? Mip0->DataPtr : (Mip0->DataArray.Num() ? &Mip0->DataArray(0) : nullptr), Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
	}
	else if( Info.Palette )
	{
		// Convert to ARGB1555 (Compose) then twiddle/copy: placeholder copies linear; twiddle to be added.
		void* Lin = ConvertTextureMipI8( Mip0, Info.Palette );
		const INT USize = Max( MinTexSize, Mip0->USize );
		const INT VSize = Max( MinTexSize, Mip0->VSize );
		const INT SizeBytes = USize * VSize * 2;
		if( Bind->Tex )
		{
			pvr_mem_free( Bind->Tex );
			if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
				VRAMUsed -= (DWORD)Bind->SizeBytes;
			Bind->Tex = NULL;
			Bind->SizeBytes = 0;
		}
		Bind->Tex = pvr_mem_malloc( SizeBytes );
		if( Bind->Tex )
		{
			pvr_txr_load( Lin, Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
	}
	else
	{
		// Lightmaps: convert to RGB565 (Compose) then upload; twiddle to be added.
		void* Lin = ConvertTextureMipBGRA7777( Mip0 );
		const INT USize = Max( MinTexSize, Mip0->USize );
		const INT VSize = Max( MinTexSize, Mip0->VSize );
		const INT SizeBytes = USize * VSize * 2;
		if( Bind->Tex )
		{
			pvr_mem_free( Bind->Tex );
			if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
				VRAMUsed -= (DWORD)Bind->SizeBytes;
			Bind->Tex = NULL;
			Bind->SizeBytes = 0;
		}
		Bind->Tex = pvr_mem_malloc( SizeBytes );
		if( Bind->Tex )
		{
			pvr_txr_load( Lin, Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
	}

	// Free temporary streamed buffer immediately
#if defined(PLATFORM_DREAMCAST)
	if( DCLoaded )
	{
		appFree( DCLoaded );
		Mip0->DataPtr = nullptr;
	}
#endif
	// If this wasn't a lightmap, UI texture or realtime texture, free SH4-side data.
	if( !TexInfo.bIsTile && Info.Format != TEXF_BGRA8_LM && !( Info.TextureFlags & (TF_Realtime|TF_RealtimePalette|TF_Parametric) ) )
	{
		for( INT i = 0; i < Info.NumMips; ++i )
		{
			Info.Mips[i]->DataArray.Empty();
			Info.Mips[i]->DataPtr = nullptr;
		}
	}

	unguard;
}

void UPVRRenderDevice::PrintMemStats() const
{
	// Report TA vertex buffer free as an approximate metric; detailed VRAM stats via pvr_mem_stats().
	const size_t End = PVR_GET(PVR_TA_VERTBUF_END);
	const size_t Pos = PVR_GET(PVR_TA_VERTBUF_POS);
	const size_t FreeTA = (End > Pos) ? (End - Pos) : 0;
	debugf( "Free TA buffer = %u", (unsigned)FreeTA );
	pvr_mem_stats(); 
	malloc_stats();
}

extern "C" DLL_EXPORT DWORD PVR_GetVRAMUsed()
{
	return GPVRDeviceInstance ? GPVRDeviceInstance->GetVRAMUsed() : 0;
}
 