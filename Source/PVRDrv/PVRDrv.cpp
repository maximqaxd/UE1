#include <kos.h>
#include <malloc.h>

#include "PVRDrvPrivate.h"

#define dcache_pref_block(a)	__builtin_prefetch(a)


// Global PVR DR state 
static pvr_dr_state_t GPVRDRState;

// Newer KOS made several poly-context fields strongly-typed enums/bools.
// CRITICAL: depth.write is the RAW hardware "Z-write disable" bit despite its
// "Enable depth writes" doc comment. pvr_prim.c does
//   FIELD_PREP(PVR_TA_PM1_DEPTHWRITE, depth.write)
// and defaults it to PVR_DEPTHWRITE_ENABLE(0). So 0 = writes ENABLED, 1 = writes
// DISABLED. Always assign PVR_DEPTHWRITE_ENABLE/DISABLE — NOT true/false.
static pvr_blend_mode_t    GPVRSrcBlend  = PVR_BLEND_ONE;
static pvr_blend_mode_t    GPVRDstBlend  = PVR_BLEND_ZERO;
static pvr_depthcmp_mode_t GPVRZFunction = PVR_DEPTHCMP_GEQUAL;
static int                 GPVRZWrite    = PVR_DEPTHWRITE_ENABLE;  // 0 = writes enabled
static int                 GPVRBlendEnabled = 0;
static pvr_cull_mode_t     GPVRCullMode  = PVR_CULLING_NONE;

struct FPVRRenderCallback
{
    virtual ~FPVRRenderCallback() {}
    virtual void Execute() = 0;
    virtual DWORD MemoryBytes() const = 0;
};

static TArray<FPVRRenderCallback*> GPVRPTCallbacks;
static TArray<FPVRRenderCallback*> GPVRTRCallbacks;
static UBOOL GPVROpaqueListOpen = 0;
static DWORD GPVROpaqueDirectBytes = 0;
static DWORD GPVROpaqueDirectCount = 0;

static void QueuePVRCallback( pvr_list_t List, FPVRRenderCallback* Callback )
{
	if( List == PVR_LIST_OP_POLY && GPVROpaqueListOpen )
	{
		GPVROpaqueDirectBytes += Callback->MemoryBytes();
		++GPVROpaqueDirectCount;
		Callback->Execute();
		delete Callback;
	}
	else if( List == PVR_LIST_PT_POLY )
	{
		GPVRPTCallbacks.AddItem( Callback );
	}
	else
	{
		GPVRTRCallbacks.AddItem( Callback );
	}
}

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

static inline void SubmitTriangleFan( const FSceneNode* Frame, const FPVRClipVert* V, INT Count, UBOOL SubmitUV, UBOOL ReverseWinding = false )
{
    if( Count < 3 )
        return;
    dcache_pref_block(V);
    if( ReverseWinding )
    {
        for( INT j = 1; j < Count - 1; ++j )
        {
            dcache_pref_block(&V[j + 1]);
            FLOAT sx, sy, sz;
            ProjectToScreenUE( Frame, V[0].X, V[0].Y, V[0].Z, sx, sy, sz );
            if( SubmitUV ) PVRVertexSubmit( sx, sy, sz, V[0].U, V[0].V, V[0].ARGB, PVR_CMD_VERTEX );
            else           PVRVertexSubmit( sx, sy, sz, 0.f,   0.f,   V[0].ARGB, PVR_CMD_VERTEX );

            ProjectToScreenUE( Frame, V[j+1].X, V[j+1].Y, V[j+1].Z, sx, sy, sz );
            if( SubmitUV ) PVRVertexSubmit( sx, sy, sz, V[j+1].U, V[j+1].V, V[j+1].ARGB, PVR_CMD_VERTEX );
            else           PVRVertexSubmit( sx, sy, sz, 0.f,   0.f,   V[j+1].ARGB, PVR_CMD_VERTEX );

            ProjectToScreenUE( Frame, V[j].X, V[j].Y, V[j].Z, sx, sy, sz );
            if( SubmitUV ) PVRVertexSubmit( sx, sy, sz, V[j].U, V[j].V, V[j].ARGB, PVR_CMD_VERTEX_EOL );
            else           PVRVertexSubmit( sx, sy, sz, 0.f,     0.f,     V[j].ARGB, PVR_CMD_VERTEX_EOL );
        }
    }
    else
    {
        for( INT j = 1; j < Count - 1; ++j )
        {
            dcache_pref_block(&V[j + 1]);
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
}



// Build polygon header using global render state 
static inline void BuildPolyHeader( UPVRRenderDevice* RD, DWORD PolyFlags, const FTextureInfo* Info, pvr_poly_hdr_t& OutHdr, pvr_list_t& OutList, UBOOL ForceNoDepthTest = 0 )
{
    DWORD AdjustedFlags = PolyFlags;
    if( !(AdjustedFlags & (PF_Translucent|PF_Modulated)) && !RD->CurrentSceneNode.bIsSky )
        AdjustedFlags |= PF_Occlude;
    else if( AdjustedFlags & PF_Translucent )
        AdjustedFlags &= ~PF_Masked;
    
    const UBOOL IsMasked = (AdjustedFlags & PF_Masked) != 0;
    const UBOOL IsTrans  = (AdjustedFlags & (PF_Translucent|PF_Modulated|PF_Highlighted)) != 0;
    OutList = IsMasked ? PVR_LIST_PT_POLY : (IsTrans ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);

    pvr_poly_cxt_t Cxt;
	if( Info && RD->TexInfo.CurrentBind && RD->TexInfo.CurrentBind->Tex )
	{
		const UPVRRenderDevice::FTexBind& Bind = *RD->TexInfo.CurrentBind;
		pvr_poly_cxt_txr(
			&Cxt,
			OutList,
			Bind.DCFormat,
			Bind.DCWidth,
			Bind.DCHeight,
			Bind.Tex,
			RD->NoFiltering ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR );
		Cxt.txr.mipmap = Bind.DCMipMapped;
    }
    else
    {
        pvr_poly_cxt_col( &Cxt, OutList );
    }

    if( ForceNoDepthTest )
    {
        Cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        Cxt.depth.write      = PVR_DEPTHWRITE_DISABLE;
    }
    else
    {
        Cxt.depth.comparison = GPVRZFunction;
        Cxt.depth.write      = (OutList != PVR_LIST_TR_POLY) ? GPVRZWrite : PVR_DEPTHWRITE_DISABLE;
    }
    Cxt.gen.culling = GPVRCullMode;
    
    if( AdjustedFlags & PF_Invisible )
    {
        Cxt.blend.src = PVR_BLEND_ZERO;
        Cxt.blend.dst = PVR_BLEND_ZERO;
    }
    else if( OutList == PVR_LIST_TR_POLY )
    {
        if( GPVRBlendEnabled )
        {
            Cxt.blend.src = GPVRSrcBlend;
            Cxt.blend.dst = GPVRDstBlend;
        }
        else
        {
            Cxt.blend.src = PVR_BLEND_ONE;
            Cxt.blend.dst = PVR_BLEND_ZERO;
        }
    }
    else
    {
        Cxt.blend.src = PVR_BLEND_ONE;
        Cxt.blend.dst = PVR_BLEND_ZERO;
    }
    
    // Texture alpha and environment settings
    if( OutList == PVR_LIST_TR_POLY )
    {
        Cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        Cxt.txr.env  = PVR_TXRENV_MODULATEALPHA;
    }
    else
    {
        Cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        Cxt.txr.env  = PVR_TXRENV_MODULATE;
    }
    
    Cxt.gen.fog_type = PVR_FOG_DISABLE;

    pvr_poly_compile( &OutHdr, &Cxt );
}
pvr_init_params_t params = {
	{ PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8 },
	2536 * 256,    /* vertex buffer */
	0,             /* dma disabled for TA */
	0,             /* fsaa off */
	0,             /* keep PVR translucent autosort OFF  */
	2            /* OPB count: start with 2*/
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
#if defined(PLATFORM_DREAMCAST)
static UBOOL GPVRSessionInitialized = 0;
extern ENGINE_API UBOOL appDCHasSessionTravel();
#endif

UPVRRenderDevice::UPVRRenderDevice()
{
	NoFiltering = false;
	Compose = NULL;
	ComposeSize = 0;
	VRAMUsed = 0;
	TextureFrame = 0;
	appMemset( PaletteBanks, 0, sizeof(PaletteBanks) );
}

UBOOL UPVRRenderDevice::Init( UViewport* InViewport )
{
	guard(UPVRRenderDevice::Init)

#if defined(PLATFORM_DREAMCAST)
	struct mallinfo InitBefore = mallinfo();
	UBOOL ReusedPVR = GPVRSessionInitialized;
	if( !GPVRSessionInitialized )
	{
		if( pvr_init(&params) < 0 )
			appErrorf("PVR initialization failed");
		GPVRSessionInitialized = 1;
	}
	struct mallinfo InitAfter = mallinfo();
	debugf( "DCPVRLIFETIME phase=init reused=%d heap_before=%u heap_after=%u delta=%d",
		ReusedPVR,
		(DWORD)InitBefore.uordblks, (DWORD)InitAfter.uordblks,
		(INT)InitAfter.uordblks - (INT)InitBefore.uordblks );
#else
    pvr_init(&params);
#endif
    InitScreenViewMatrix();
	SupportsFogMaps = false; // true;
	SupportsDistanceFog = false; // true;
	NoVolumetricBlend = true;

	Compose = NULL;
	ComposeSize = 0;
	pvr_set_pal_format( PVR_PAL_ARGB1555 );

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

	debugf( NAME_Log, "Shutting down PVR renderer" );

	// Texture and command-list memory may still be referenced by the previous
	// scene. Session teardown must not reclaim either until TA/PVR is idle.
	pvr_wait_ready();

	Flush();

	if( Compose )
	{
		appFree( Compose );
		Compose = NULL;
	}
	ComposeSize = 0;
	GPVRDeviceInstance = NULL;
#if defined(PLATFORM_DREAMCAST)
	// Level travel rebuilds the UObject graph, but the KOS PVR subsystem owns
	// process-wide command buffers and interrupt state. Keep it alive across
	// sessions; Flush above has released all renderer-owned textures.
	if( !appDCHasSessionTravel() )
	{
		struct mallinfo ShutdownBefore = mallinfo();
		pvr_shutdown();
		GPVRSessionInitialized = 0;
		struct mallinfo ShutdownAfter = mallinfo();
		debugf( "DCPVRLIFETIME phase=shutdown heap_before=%u heap_after=%u delta=%d",
			(DWORD)ShutdownBefore.uordblks, (DWORD)ShutdownAfter.uordblks,
			(INT)ShutdownAfter.uordblks - (INT)ShutdownBefore.uordblks );
	}
	else
	{
		struct mallinfo TravelHeap = mallinfo();
		debugf( "DCPVRLIFETIME phase=travel_keepalive heap=%u", (DWORD)TravelHeap.uordblks );
	}
#endif

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
	appMemset( PaletteBanks, 0, sizeof(PaletteBanks) );

	unguard;
}

UBOOL UPVRRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
	return false;
}

void UPVRRenderDevice::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
	guard(UPVRRenderDevice::Lock);
	// Texture uploads and eviction happen while building this frame's lists.
	// Wait here, before either can alter memory used by the previous scene.
	pvr_wait_ready();
	++TextureFrame;

	GPVRPTCallbacks.Empty();
	GPVRTRCallbacks.Empty();
	GPVROpaqueDirectBytes = 0;
	GPVROpaqueDirectCount = 0;

	// Opaque geometry is the dominant callback allocation. It is already
	// generated in opaque order, so send it straight to the TA and retain only
	// the later translucent and punch-through lists.
	pvr_set_bg_color( 0.f, 0.f, 0.f );
	pvr_scene_begin();
	pvr_dr_init( &GPVRDRState );
	pvr_list_begin( PVR_LIST_OP_POLY );
	GPVROpaqueListOpen = 1;

	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
		ColorMod = FPlane( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.f - Min( FlashScale.X * 2.f, 1.f ) );
	else
		ColorMod = FPlane( 0.f, 0.f, 0.f, 0.f );

	unguard;
}

void UPVRRenderDevice::Unlock( UBOOL Blit )
{
	guard(UPVRRenderDevice::Unlock);

	DWORD CallbackBytes = 0;
	for( INT i = 0; i < GPVRPTCallbacks.Num(); ++i )
		CallbackBytes += GPVRPTCallbacks(i)->MemoryBytes();
	for( INT i = 0; i < GPVRTRCallbacks.Num(); ++i )
		CallbackBytes += GPVRTRCallbacks(i)->MemoryBytes();
	DWORD QueueBytes = (GPVRPTCallbacks.ArrayMax + GPVRTRCallbacks.ArrayMax)
		* sizeof(FPVRRenderCallback*);
	static DWORD CallbackPeak = 0;
	if( TextureFrame == 1 )
		CallbackPeak = 0;
	CallbackPeak = Max(CallbackPeak, CallbackBytes + QueueBytes);
	if( TextureFrame <= 3 )
	{
		INT Pal8Count = 0;
		DWORD Pal8VRAM = 0;
		for( INT Index = 0; Index < BindMap.Size(); ++Index )
		{
			const FTexBind& Bind = BindMap[Index];
			if( Bind.Tex && (Bind.DCFormat & (7 << 27)) == PVR_TXRFMT_PAL8BPP )
			{
				++Pal8Count;
				Pal8VRAM += Bind.SizeBytes;
			}
		}
		struct mallinfo Heap = mallinfo();
		debugf(
			"DCPVRMEM phase=before_submit heap=%u callbacks=%u opaque_direct=%u "
			"queue_capacity=%u queue_peak=%u compose=%u pal8_count=%d pal8_vram=%u "
			"op=%u pt=%d tr=%d",
			(DWORD)Heap.uordblks, CallbackBytes, GPVROpaqueDirectBytes, QueueBytes,
			CallbackPeak, ComposeSize, Pal8Count, Pal8VRAM, GPVROpaqueDirectCount,
			GPVRPTCallbacks.Num(), GPVRTRCallbacks.Num());
	}

	// The opaque list has been populated directly since Lock().
	pvr_list_finish();
	GPVROpaqueListOpen = 0;

	// Render TR_POLY list.
	if( GPVRTRCallbacks.Num() > 0 )
	{
		pvr_dr_init( &GPVRDRState );
		pvr_list_begin( PVR_LIST_TR_POLY );
		for( INT i = 0; i < GPVRTRCallbacks.Num(); i++ )
		{
			GPVRTRCallbacks(i)->Execute();
			delete GPVRTRCallbacks(i);
		}
		pvr_list_finish();
	}

	// Render PT_POLY list.
	if( GPVRPTCallbacks.Num() > 0 )
	{
		PVR_SET(0x11C, 64); // PT Alpha test value
		pvr_dr_init( &GPVRDRState );
		pvr_list_begin( PVR_LIST_PT_POLY );
		for( INT i = 0; i < GPVRPTCallbacks.Num(); i++ )
		{
			GPVRPTCallbacks(i)->Execute();
			delete GPVRPTCallbacks(i);
		}
		pvr_list_finish();
	}

	// Measure TA vertex-buffer usage BEFORE scene_finish (which resets POS).
	// If the buffer fills, the TA silently drops subsequent polys -> geometry
	// vanishes depending on view angle. Track peak usage / detect overflow.
	{
		const size_t End  = PVR_GET(PVR_TA_VERTBUF_END);
		const size_t Pos  = PVR_GET(PVR_TA_VERTBUF_POS);
		const DWORD  Free = (End > Pos) ? (DWORD)(End - Pos) : 0;
		if( Free == 0 )
			debugf( "PVR: TA VERTEX BUFFER FULL (Pos=%u End=%u) - geometry being dropped!", (unsigned)Pos, (unsigned)End );
	}

	pvr_scene_finish();

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

	struct FDrawSurfaceCallback : public FPVRRenderCallback
	{
		pvr_poly_hdr_t Hdr;
		FSceneNode* Frame;
		TArray<FPVRClipVert> ClippedVerts;
		UBOOL ReverseWinding;
		virtual DWORD MemoryBytes() const override
		{
			return sizeof(*this) + ClippedVerts.ArrayMax * sizeof(FPVRClipVert);
		}
		
		virtual void Execute() override
		{
			PVRHeaderSubmit( Hdr );
			if( ClippedVerts.Num() >= 3 )
			{
				dcache_pref_block(&ClippedVerts(0));
				if( ReverseWinding )
				{
					for( INT j = 1; j < ClippedVerts.Num() - 1; ++j )
					{
						dcache_pref_block(&ClippedVerts(j + 1));
						FLOAT sx, sy, sz;
						ProjectToScreenUE( Frame, ClippedVerts(0).X, ClippedVerts(0).Y, ClippedVerts(0).Z, sx, sy, sz );
						PVRVertexSubmit( sx, sy, sz, ClippedVerts(0).U, ClippedVerts(0).V, ClippedVerts(0).ARGB, PVR_CMD_VERTEX );
						ProjectToScreenUE( Frame, ClippedVerts(j+1).X, ClippedVerts(j+1).Y, ClippedVerts(j+1).Z, sx, sy, sz );
						PVRVertexSubmit( sx, sy, sz, ClippedVerts(j+1).U, ClippedVerts(j+1).V, ClippedVerts(j+1).ARGB, PVR_CMD_VERTEX );
						ProjectToScreenUE( Frame, ClippedVerts(j).X, ClippedVerts(j).Y, ClippedVerts(j).Z, sx, sy, sz );
						PVRVertexSubmit( sx, sy, sz, ClippedVerts(j).U, ClippedVerts(j).V, ClippedVerts(j).ARGB, PVR_CMD_VERTEX_EOL );
					}
				}
				else
				{
					SubmitTriangleFan( Frame, &ClippedVerts(0), ClippedVerts.Num(), 1 );
				}
			}
		}
	};

    // Draw base texture pass.
    SetBlend( Surface.PolyFlags );
    SetTexture( *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
    {
        pvr_poly_hdr_t Hdr; pvr_list_t List;
        BuildPolyHeader( this, Surface.PolyFlags, Surface.Texture, Hdr, List );

        const DWORD White = 0xFFFFFFFFu;
        const UBOOL ReverseWinding = (Frame->Mirror < 0.0f);
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
            
            FDrawSurfaceCallback* CB = new FDrawSurfaceCallback;
            CB->Hdr = Hdr;
            CB->Frame = Frame;
            CB->ReverseWinding = ReverseWinding;
            CB->ClippedVerts.Empty();
            for( INT i = 0; i < C; i++ )
                CB->ClippedVerts.AddItem( Clipped[i] );
            
            QueuePVRCallback( List, CB );
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

        const DWORD White = 0xFFFFFFFFu;
        const UBOOL ReverseWinding = (Frame->Mirror < 0.0f);
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
            
            FDrawSurfaceCallback* CB = new FDrawSurfaceCallback;
            CB->Hdr = Hdr;
            CB->Frame = Frame;
            CB->ReverseWinding = ReverseWinding;
            CB->ClippedVerts.Empty();
            for( INT i = 0; i < C; i++ )
                CB->ClippedVerts.AddItem( Clipped[i] );
            
            QueuePVRCallback( List, CB );
        }
    }

	// Draw fog.
	if( Surface.FogMap )
	{
		const DWORD FogFlags = PF_Highlighted | (Surface.PolyFlags & PF_Masked);
		SetBlend( FogFlags );
		SetTexture( *Surface.FogMap, ( Surface.PolyFlags & PF_Masked ), -0.5f );

		pvr_poly_hdr_t Hdr; pvr_list_t List;
		BuildPolyHeader( this, FogFlags, Surface.FogMap, Hdr, List );

		const DWORD White = 0xFFFFFFFFu;
		const UBOOL ReverseWinding = (Frame->Mirror < 0.0f);
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
			
			FDrawSurfaceCallback* CB = new FDrawSurfaceCallback;
			CB->Hdr = Hdr;
			CB->Frame = Frame;
			CB->ReverseWinding = ReverseWinding;
			CB->ClippedVerts.Empty();
			for( INT i = 0; i < C; i++ )
				CB->ClippedVerts.AddItem( Clipped[i] );
			
			QueuePVRCallback( List, CB );
		}
	}

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

	struct FDrawGouraudCallback : public FPVRRenderCallback
	{
		pvr_poly_hdr_t Hdr;
		FSceneNode* Frame;
		TArray<FPVRClipVert> ClippedVerts;
		virtual DWORD MemoryBytes() const override
		{
			return sizeof(*this) + ClippedVerts.ArrayMax * sizeof(FPVRClipVert);
		}
		
		virtual void Execute() override
		{
			PVRHeaderSubmit( Hdr );
			if( ClippedVerts.Num() >= 3 )
			{
				for( INT i = 0; i < ClippedVerts.Num(); i++ )
				{
					const FPVRClipVert& Vtx = ClippedVerts(i);
					FLOAT SX, SY, SZ;
					ProjectToScreenUE( Frame, Vtx.X, Vtx.Y, Vtx.Z, SX, SY, SZ );
					const unsigned Flags = (i == ClippedVerts.Num() - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
					PVRVertexSubmit( SX, SY, SZ, Vtx.U, Vtx.V, Vtx.ARGB, Flags );
				}
			}
		}
	};

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
            FDrawGouraudCallback* CB = new FDrawGouraudCallback;
            CB->Hdr = Hdr;
            CB->Frame = Frame;
            CB->ClippedVerts.Empty();
            for( INT i = 0; i < C; i++ )
                CB->ClippedVerts.AddItem( Clipped[i] );
            
            QueuePVRCallback( List, CB );
        }
    }

	// Optional fog-only pass
	if( (PolyFlags & (PF_RenderFog|PF_Translucent|PF_Modulated)) == PF_RenderFog )
	{
        pvr_poly_hdr_t FogHdr; pvr_list_t FogList;
        BuildPolyHeader( this, PF_Highlighted, nullptr, FogHdr, FogList );
        
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
        if( CF >= 3 )
        {
            FDrawGouraudCallback* FogCB = new FDrawGouraudCallback;
            FogCB->Hdr = FogHdr;
            FogCB->Frame = Frame;
            FogCB->ClippedVerts.Empty();
            for( INT i = 0; i < CF; i++ )
                FogCB->ClippedVerts.AddItem( ClipF[i] );
            
            QueuePVRCallback( FogList, FogCB );
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
	
	const UBOOL IsUITile = (Span == NULL);
	DWORD TileFlags = PolyFlags;
	if( IsUITile && !(TileFlags & (PF_Translucent|PF_Modulated|PF_Highlighted)) )
	{
		TileFlags |= PF_Translucent;
		TileFlags &= ~PF_Occlude;
	}
	
	SetBlend( TileFlags );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );

	pvr_poly_hdr_t Hdr;
	pvr_list_t List;
	BuildPolyHeader( this, TileFlags, &Texture, Hdr, List, /*ForceNoDepthTest=*/1 );
	
	if( IsUITile && (TileFlags & PF_Translucent) )
	{
		// Modify the header blend mode directly for UI tiles
		// This is a hack but necessary since BuildPolyHeader uses global state
		// We need to recompile with proper alpha blend mode
		pvr_poly_cxt_t Cxt;
		if( TexInfo.CurrentBind && TexInfo.CurrentBind->Tex )
		{
			const FTexBind& Bind = *TexInfo.CurrentBind;
			pvr_poly_cxt_txr(
				&Cxt,
				PVR_LIST_TR_POLY,
				Bind.DCFormat,
				Bind.DCWidth,
				Bind.DCHeight,
				Bind.Tex,
				NoFiltering ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR );
			Cxt.txr.mipmap = Bind.DCMipMapped;
		}
		else
		{
			pvr_poly_cxt_col( &Cxt, PVR_LIST_TR_POLY );
		}
		Cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
		Cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
		Cxt.gen.culling = GPVRCullMode;
		Cxt.blend.src = PVR_BLEND_SRCALPHA;
		Cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
		Cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
		Cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
		Cxt.gen.fog_type = PVR_FOG_DISABLE;
		pvr_poly_compile( &Hdr, &Cxt );
		List = PVR_LIST_TR_POLY;
	}

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

	struct FDrawTileCallback : public FPVRRenderCallback
	{
		virtual DWORD MemoryBytes() const override { return sizeof(*this); }
		pvr_poly_hdr_t Hdr;
		FSceneNode* Frame;
		FLOAT Ax, Ay, Bx, By, Cx, Cy, Dx, Dy;
		FLOAT Au, Av, Bu, Bv, Cu, Cv, Du, Dv;
		FLOAT Z;
		DWORD ARGB;
		
		virtual void Execute() override
		{
			PVRHeaderSubmit( Hdr );
			PVRVertexSubmit( Ax, Ay, Z, Au, Av, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( Bx, By, Z, Bu, Bv, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( Cx, Cy, Z, Cu, Cv, ARGB, PVR_CMD_VERTEX_EOL );
			PVRVertexSubmit( Ax, Ay, Z, Au, Av, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( Cx, Cy, Z, Cu, Cv, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( Dx, Dy, Z, Du, Dv, ARGB, PVR_CMD_VERTEX_EOL );
		}
	};

	FDrawTileCallback* CB = new FDrawTileCallback;
	CB->Hdr = Hdr;
	CB->Frame = Frame;
	CB->Ax = X;        CB->Ay = Y;        CB->Au = U0; CB->Av = V0;
	CB->Bx = X + XL;   CB->By = Y;        CB->Bu = U1; CB->Bv = V0;
	CB->Cx = X + XL;   CB->Cy = Y + YL;   CB->Cu = U1; CB->Cv = V1;
	CB->Dx = X;        CB->Dy = Y + YL;   CB->Du = U0; CB->Dv = V1;
	CB->Z = Z;
	CB->ARGB = ARGB;

	GPVRTRCallbacks.AddItem( CB );

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

	const BYTE A = (BYTE)Clamp<INT>(appRound(ColorMod.W * 255.f), 0, 255);
	const BYTE R = (BYTE)Clamp<INT>(appRound(ColorMod.X * 255.f), 0, 255);
	const BYTE G = (BYTE)Clamp<INT>(appRound(ColorMod.Y * 255.f), 0, 255);
	const BYTE B = (BYTE)Clamp<INT>(appRound(ColorMod.Z * 255.f), 0, 255);
	const DWORD ARGB = (A<<24) | (R<<16) | (G<<8) | (B);

	const FLOAT W = (FLOAT)Viewport->SizeX;
	const FLOAT H = (FLOAT)Viewport->SizeY;
	const FLOAT Z = 1.f;

	// Create callback for flash (always TR_POLY for 2D)
	struct FEndFlashCallback : public FPVRRenderCallback
	{
		virtual DWORD MemoryBytes() const override { return sizeof(*this); }
		pvr_poly_hdr_t Hdr;
		FLOAT W, H, Z;
		DWORD ARGB;
		
		virtual void Execute() override
		{
			PVRHeaderSubmit( Hdr );
			PVRVertexSubmit( 0.f, 0.f, Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( W,   0.f, Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( W,   H,   Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX_EOL );
			PVRVertexSubmit( 0.f, 0.f, Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( W,   H,   Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX );
			PVRVertexSubmit( 0.f, H,   Z, 0.f, 0.f, ARGB, PVR_CMD_VERTEX_EOL );
		}
	};

	FEndFlashCallback* CB = new FEndFlashCallback;
	CB->Hdr = Hdr;
	CB->W = W;
	CB->H = H;
	CB->Z = Z;
	CB->ARGB = ARGB;
	GPVRTRCallbacks.AddItem( CB );

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

	// Update global render state 
	if( PolyFlags & PF_Translucent )
	{
		GPVRBlendEnabled = 1;
		GPVRSrcBlend = PVR_BLEND_ONE;
		GPVRDstBlend = PVR_BLEND_ONE; // Approximate GL_ONE_MINUS_SRC_COLOR
	}
	else if( PolyFlags & PF_Modulated )
	{
		GPVRBlendEnabled = 1;
		GPVRSrcBlend = PVR_BLEND_DESTCOLOR;
		GPVRDstBlend = PVR_BLEND_DESTCOLOR;
	}
	else if( PolyFlags & PF_Highlighted )
	{
		GPVRBlendEnabled = 1;
		GPVRSrcBlend = PVR_BLEND_ONE;
		GPVRDstBlend = PVR_BLEND_INVSRCALPHA;
	}
	else
	{
		GPVRBlendEnabled = 0;
		GPVRSrcBlend = PVR_BLEND_ONE;
		GPVRDstBlend = PVR_BLEND_ZERO;
	}

	// depth.write is the raw "Z-write disable" bit: ENABLE(0) writes Z, DISABLE(1) doesn't.
	// Opaque world (PF_Occlude) must write Z so the z-buffer works; sky and translucent
	// surfaces must NOT write Z.
	if( PolyFlags & PF_Occlude )
		GPVRZWrite = PVR_DEPTHWRITE_ENABLE;   // 0 -> writes enabled
	else
		GPVRZWrite = PVR_DEPTHWRITE_DISABLE;  // 1 -> writes disabled

	// Record current flags
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
	const UBOOL PaletteChanged = ( Info.TextureFlags & TF_RealtimePalette ) != 0;
	const UBOOL Masked = (PolyFlags & PF_Masked) != 0;
	UBOOL CurrentPaletteValid = 1;
	if( Tex.CurrentBind && Tex.CurrentBind->PaletteBank != INDEX_NONE )
	{
		const INT Bank = Tex.CurrentBind->PaletteBank;
		CurrentPaletteValid = PaletteBanks[Bank].CacheID == Info.PaletteCacheID
			&& PaletteBanks[Bank].Masked == Masked;
		if( CurrentPaletteValid )
		{
			PaletteBanks[Bank].LastUsedFrame = TextureFrame;
		}
	}
	if( !RealtimeChanged && !PaletteChanged && CurrentPaletteValid && NewCacheID == Tex.CurrentCacheID )
	{
		if( Tex.CurrentBind && Tex.CurrentBind->Tex )
		{
			Tex.CurrentBind->LastUsedFrame = TextureFrame;
			return;
		}
	}

	const QWORD LookupID = NewCacheID & ~0xFFULL;
	const BYTE NewType = NewCacheID & 0xFF;
	FTexBind* Bind = BindMap.Find( LookupID );
	const UBOOL NewTexture = !Bind;
	if( NewTexture )
	{
		// Create new binding entry; VRAM is allocated in UploadTexture.
		Bind = BindMap.Add( LookupID, { 0, NewType, 0, 0, 0, 0, 0, 0, -1, 0 } );
	}
	UBOOL BindPaletteValid = 1;
	if( Bind->PaletteBank != INDEX_NONE )
	{
		const INT Bank = Bind->PaletteBank;
		BindPaletteValid = PaletteBanks[Bank].CacheID == Info.PaletteCacheID
			&& PaletteBanks[Bank].Masked == Masked;
		if( BindPaletteValid )
		{
			PaletteBanks[Bank].LastUsedFrame = TextureFrame;
		}
	}

	// Make current.
	Tex.CurrentCacheID = NewCacheID;
	Tex.CurrentBind = Bind;
	Bind->LastUsedFrame = TextureFrame;

	if( NewTexture || !Bind->Tex || RealtimeChanged || PaletteChanged || !BindPaletteValid || Bind->LastType != NewType )
	{
		// New texture or it has changed, upload it to VRAM.
		Bind->LastType = NewType;
		Info.TextureFlags &= ~TF_RealtimeChanged;
		UploadTexture( Info, NewTexture, Masked );
	}

	unguard;
}

INT UPVRRenderDevice::AcquirePaletteBank( const FTextureInfo& Info, UBOOL Masked )
{
	for( INT Bank = 0; Bank < ARRAY_COUNT(PaletteBanks); ++Bank )
	{
		if( PaletteBanks[Bank].CacheID == Info.PaletteCacheID
			&& PaletteBanks[Bank].Masked == Masked )
		{
			PaletteBanks[Bank].LastUsedFrame = TextureFrame;
			return Bank;
		}
	}

	for( INT Bank = 0; Bank < ARRAY_COUNT(PaletteBanks); ++Bank )
	{
		if( PaletteBanks[Bank].LastUsedFrame != TextureFrame )
		{
			PaletteBanks[Bank].CacheID = Info.PaletteCacheID;
			PaletteBanks[Bank].Masked = Masked;
			PaletteBanks[Bank].LastUsedFrame = TextureFrame;
			PaletteBanks[Bank].LastUploadFrame = 0;
			return Bank;
		}
	}

	return INDEX_NONE;
}

void UPVRRenderDevice::UploadPalette( INT Bank, const FTextureInfo& Info, UBOOL Masked )
{
	check(Bank >= 0 && Bank < ARRAY_COUNT(PaletteBanks));
	check(Info.Palette);

	FPaletteBank& State = PaletteBanks[Bank];
	if( State.LastUploadFrame == TextureFrame )
	{
		return;
	}

	const DWORD Base = Bank * NUM_PAL_COLORS;
	for( INT Index = 0; Index < NUM_PAL_COLORS; ++Index )
	{
		_WORD Color = Info.Palette[Index].RGB888ToARGB1555();
		if( Masked && Index == 0 )
		{
			Color &= ~0x8000U;
		}
		pvr_set_pal_entry( Base + Index, Color );
	}
	State.LastUploadFrame = TextureFrame;
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
	const BYTE* Src = (const BYTE*)Mip->DataPtr;
	const DWORD SrcCount = Mip->USize * Mip->VSize;
	INT USize = Mip->USize;
	INT VSize = Mip->VSize;

	// Horizontal expansion writes the first image; VerticalUpscale appends
	// its output after that image. Acquire the pointer only after allocation.
	const DWORD ConvertedPixels = Max(MinTexSize, USize) * VSize;
	const DWORD UpscaledPixels = VSize < MinTexSize
		? Max(MinTexSize, USize) * MinTexSize : 0;
	EnsureComposeSize( (ConvertedPixels + UpscaledPixels) * sizeof(_WORD) );
	_WORD* Dst = (_WORD*)Compose;

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
	const FColor* Src = (const FColor*)Mip->DataPtr;
	const DWORD Count = USize * VSize;

	const DWORD ConvertedPixels = Max(MinTexSize, USize) * VSize;
	const DWORD UpscaledPixels = VSize < MinTexSize
		? Max(MinTexSize, USize) * MinTexSize : 0;
	EnsureComposeSize( (ConvertedPixels + UpscaledPixels) * sizeof(_WORD) );
	_WORD* Dst = (_WORD*)Compose;

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

pvr_ptr_t UPVRRenderDevice::AllocateTexture( INT Size )
{
	pvr_ptr_t Result = pvr_mem_malloc( Size );
	while( !Result )
	{
		FTexBind* Oldest = NULL;
		for( INT i = 0; i < BindMap.Size(); ++i )
		{
			FTexBind& Candidate = BindMap[i];
			if( Candidate.Tex && &Candidate != TexInfo.CurrentBind
				&& Candidate.LastUsedFrame != TextureFrame
				&& (!Oldest || Candidate.LastUsedFrame < Oldest->LastUsedFrame) )
			{
				Oldest = &Candidate;
			}
		}
		if( !Oldest )
		{
			appErrorf( "PVR frame working set exceeds VRAM: request=%i resident=%u", Size, VRAMUsed );
		}
		pvr_mem_free( Oldest->Tex );
		VRAMUsed -= Oldest->SizeBytes;
		Oldest->Tex = NULL;
		Oldest->SizeBytes = 0;
		Result = pvr_mem_malloc( Size );
	}
	return Result;
}

void UPVRRenderDevice::UploadTexture( FTextureInfo& Info, UBOOL NewTexture, UBOOL Masked )
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
		if( Mip0->StreamData.Size() )
		{
			DCLoaded = (BYTE*)appMalloc( Mip0->StreamData.Size(), "DatTexStream" );
			Mip0->StreamData.Read( DCLoaded );
			Mip0->DataPtr = DCLoaded;
		}
		else if( Info.Texture && Info.Texture->GetLinker() )
		{
			ULinkerLoad* L = Info.Texture->GetLinker();
			FArchiveFileLoad* FL = (FArchiveFileLoad*)L; 
			if( appDCStreamActive() && Mip0->DCDataSize > 0 )
			{
				DCLoaded = (BYTE*)appMalloc( Mip0->DCDataSize, "DatTexFirstUse" );
				Mip0->ReadDCData( *FL, DCLoaded );
				Mip0->DataPtr = DCLoaded;
			}
			else if( Mip0->DCDataSize > 0 && Mip0->DCDataOffset > 0 )
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
	if( Info.Format == TEXF_EXT_DCTEX )
	{
		const BYTE* Data = Mip0->DataPtr;
		INT Size = Mip0->DataArray.Num() ? Mip0->DataArray.Num() : Mip0->DCDataSize;
		DWORD Header[8];
		if( !Data || Size < 32 )
		{
			appErrorf( "Missing DT texture data" );
		}
		appMemcpy( Header, Data, sizeof(Header) );
		INT HeaderSize = (Data[9] + 1) * 32;
		DWORD Mode = Header[4];
		INT Width = 8 << ((Mode >> 3) & 7);
		INT Height = 8 << (Mode & 7);
		if( Header[0] != 0x78546344 || Header[1] != (DWORD)Size || Data[8] != 0
			|| HeaderSize >= Size || ((Mode & 0x40000000) && Data[10] != 255)
			|| ((Mode & 0x80000000) && Width != Height) )
		{
			appErrorf( "Unsupported or invalid DT texture header" );
		}

		if( Bind->Tex )
		{
			pvr_mem_free( Bind->Tex );
			VRAMUsed -= Bind->SizeBytes;
		}
		Bind->SizeBytes = Size - HeaderSize;
		Bind->Tex = AllocateTexture( Bind->SizeBytes );
		if( !Bind->Tex )
		{
			appErrorf( "DT VRAM allocation failed: %i bytes", Bind->SizeBytes );
		}
		pvr_txr_load( Data + HeaderSize, Bind->Tex, Bind->SizeBytes );
		VRAMUsed += Bind->SizeBytes;
		Bind->DCFormat = Mode & 0x7e000000;
		Bind->DCWidth = Width;
		Bind->DCHeight = Height;
		Bind->DCMipMapped = (Mode & 0x80000000) != 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;
	}
	else if( Info.Format == TEXF_EXT_ARGB1555_VQ )
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
		Bind->Tex = AllocateTexture( SizeBytes );
		if( Bind->Tex )
		{
			pvr_txr_load( Mip0->DataPtr ? Mip0->DataPtr : (Mip0->DataArray.Num() ? &Mip0->DataArray(0) : nullptr), Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
		Bind->DCFormat = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_VQ_ENABLE;
		Bind->DCWidth = Max( MinTexSize, Mip0->USize );
		Bind->DCHeight = Max( MinTexSize, Mip0->VSize );
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;
	}
	else if( Info.Format == TEXF_P8
		&& Info.Palette
		&& (Info.TextureFlags & (TF_Realtime | TF_RealtimePalette | TF_Parametric))
		&& Mip0->USize >= MinTexSize
		&& Mip0->VSize >= MinTexSize
		&& Mip0->USize <= 256
		&& Mip0->VSize <= 256 )
	{
		const INT Bank = AcquirePaletteBank( Info, Masked );
		if( Bank != INDEX_NONE )
		{
			const INT SizeBytes = Mip0->USize * Mip0->VSize;
			const DWORD Format = PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(Bank);
			const UBOOL UploadPixels = !Bind->Tex
				|| Bind->SizeBytes != SizeBytes
				|| (Bind->DCFormat & (7 << 27)) != PVR_TXRFMT_PAL8BPP
				|| (Info.TextureFlags & TF_RealtimeChanged);

			if( Bind->Tex && (Bind->SizeBytes != SizeBytes
				|| (Bind->DCFormat & (7 << 27)) != PVR_TXRFMT_PAL8BPP) )
			{
				pvr_mem_free( Bind->Tex );
				VRAMUsed -= Bind->SizeBytes;
				Bind->Tex = NULL;
				Bind->SizeBytes = 0;
			}
			if( !Bind->Tex )
			{
				Bind->Tex = AllocateTexture( SizeBytes );
				Bind->SizeBytes = SizeBytes;
				VRAMUsed += SizeBytes;
			}
			if( UploadPixels )
			{
				check(Mip0->DataPtr);
				pvr_txr_load_ex(
					Mip0->DataPtr,
					Bind->Tex,
					Mip0->USize,
					Mip0->VSize,
					PVR_TXRLOAD_8BPP );
			}

			UploadPalette( Bank, Info, Masked );
			Bind->DCFormat = Format;
			Bind->DCWidth = Mip0->USize;
			Bind->DCHeight = Mip0->VSize;
			Bind->DCMipMapped = 0;
			Bind->PaletteBank = Bank;
			Bind->PaletteMasked = Masked;
		}
		else
		{
			// All four hardware PAL8 banks are referenced by this scene. Preserve
			// correctness by expanding this texture through the 16-bit fallback.
			void* Lin = ConvertTextureMipI8( Mip0, Info.Palette );
			const INT USize = Max( MinTexSize, Mip0->USize );
			const INT VSize = Max( MinTexSize, Mip0->VSize );
			const INT SizeBytes = USize * VSize * 2;
			if( Bind->Tex )
			{
				pvr_mem_free( Bind->Tex );
				VRAMUsed -= Bind->SizeBytes;
			}
			Bind->Tex = AllocateTexture( SizeBytes );
			pvr_txr_load( Lin, Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
			Bind->DCFormat = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED;
			Bind->DCWidth = USize;
			Bind->DCHeight = VSize;
			Bind->DCMipMapped = 0;
			Bind->PaletteBank = INDEX_NONE;
			Bind->PaletteMasked = 0;
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
		Bind->Tex = AllocateTexture( SizeBytes );
		if( Bind->Tex )
		{
			pvr_txr_load( Lin, Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
		Bind->DCFormat = PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED;
		Bind->DCWidth = USize;
		Bind->DCHeight = VSize;
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;
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
		Bind->Tex = AllocateTexture( SizeBytes );
		if( Bind->Tex )
		{
			pvr_txr_load( Lin, Bind->Tex, SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
		Bind->DCFormat = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED;
		Bind->DCWidth = USize;
		Bind->DCHeight = VSize;
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;
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
	if( !TexInfo.bIsTile && Info.Format != TEXF_BGRA8_LM && Info.Format != TEXF_P8
		&& !( Info.TextureFlags & (TF_Realtime|TF_RealtimePalette|TF_Parametric) ) )
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
