/*=============================================================================
	PVRDrv.cpp: Unreal PowerVR (Dreamcast) render device.

	Submission model
	----------------
	The opaque list is opened in Lock() and written straight to the store
	queues.  Punch-through and translucent primitives are appended to two flat
	arenas of finished 32-byte TA payloads and replayed with a single
	pvr_prim() burst in Unlock().

	Two lists must be buffered because Unreal generates primitives in an order
	the hardware cannot consume directly:

	  * a lit world surface emits its base texture (opaque) and its lightmap
	    (modulate, hence translucent) from the same call, and
	  * the HUD is drawn after DrawWorld() has returned and popped the scene
	    arenas, so the world cannot be re-walked to pick up its later lists.

	Nothing is allocated per primitive and nothing is projected twice: vertex
	positions come from the screen coordinates Unreal already computed in
	FTransform::Project.
=============================================================================*/

#include <kos.h>
#include <malloc.h>
#include <sh4zam/shz_sh4zam.h>

#include "PVRDrvPrivate.h"
#include "UnDCFrameProfile.h"

#if defined(PLATFORM_DREAMCAST)
static void DCProfileTextureLoad( const void* Source, pvr_ptr_t Destination, size_t Bytes )
{
	DC_FRAME_SCOPE(DCFS_Upload);
	DCFrameUploadBytes(Bytes);
	pvr_txr_load(Source, Destination, Bytes);
}
#define pvr_txr_load DCProfileTextureLoad
#endif
#if defined(PLATFORM_DREAMCAST)
#include <zlib.h>
#endif

/*-----------------------------------------------------------------------------
	Tunables.
-----------------------------------------------------------------------------*/

// Camera-space depth at which a vertex is treated as behind the near plane.
// Unreal has already clipped against the four side planes, so straddling
// polygons are rare and the fast path below almost always wins.
#define PVR_NEAR_Z          1.0f

// Punch-through alpha test threshold (PVR_TA_PT_ALPHA_REF).
#define PVR_PT_ALPHA_THRESHOLD  64

// Screen-space overlays are placed above every world vertex in 1/w terms.
// A world vertex is nearest at the near plane, where 1/w == Frame->Proj.Z,
// so the band scales with field of view instead of being guessed -- see
// UpdateOverlayDepths(). The flash sits below the HUD so it tints the world
// without tinting the HUD, which reproduces Unreal's draw order even though
// the punch-through list is rendered before the translucent one.
#define PVR_FLASH_Z_SCALE   4.0f
#define PVR_UI_Z_SCALE      16.0f
#define PVR_UI_Z_SUBDIV     4096.0f

// Largest polygon Unreal can hand us, plus room for near-plane clipping.
#define PVR_MAX_POLY_VERTS  (FBspNode::MAX_FINAL_VERTICES + 8)

/*-----------------------------------------------------------------------------
	Math helpers.
-----------------------------------------------------------------------------*/

// fipr-backed dot product. FVector is three consecutive floats.
static inline FLOAT PVRDot( const FVector& A, const FVector& B )
{
	return shz_dot8f( A.X, A.Y, A.Z, 0.f, B.X, B.Y, B.Z, 0.f );
}

static inline DWORD PVRPackARGB( FLOAT A, FLOAT R, FLOAT G, FLOAT B )
{
	return ((DWORD)(shz_saturatef(A) * 255.f) << 24)
		|  ((DWORD)(shz_saturatef(R) * 255.f) << 16)
		|  ((DWORD)(shz_saturatef(G) * 255.f) <<  8)
		|  ((DWORD)(shz_saturatef(B) * 255.f));
}

static inline DWORD PVRPackLight( const FPlane& Light )
{
	return 0xFF000000u
		| ((DWORD)(shz_saturatef(Light.X) * 255.f) << 16)
		| ((DWORD)(shz_saturatef(Light.Y) * 255.f) <<  8)
		| ((DWORD)(shz_saturatef(Light.Z) * 255.f));
}

// Linear interpolation of two packed ARGB values, T in [0,1].
static inline DWORD PVRLerpARGB( DWORD C1, DWORD C2, FLOAT T )
{
	const INT Ti = Clamp<INT>( (INT)(T * 256.f), 0, 256 );
	const DWORD rb = ((((C2 & 0x00FF00FFu) - (C1 & 0x00FF00FFu)) * Ti) >> 8) + (C1 & 0x00FF00FFu);
	const DWORD g  = ((((C2 & 0x0000FF00u) - (C1 & 0x0000FF00u)) * Ti) >> 8) + (C1 & 0x0000FF00u);
	const DWORD a  = ((((C2 >> 24) - (C1 >> 24)) * Ti) >> 8) + (C1 >> 24);
	return (a << 24) | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

/*-----------------------------------------------------------------------------
	Command arenas.
-----------------------------------------------------------------------------*/

//
// A flat run of finished 32-byte TA payloads.  Replay is a single pvr_prim()
// store-queue burst, so nothing in here costs anything at submit time beyond
// the memory write that built it.
//
struct FPVRCmdArena
{
	BYTE*  Data;
	DWORD  Capacity;
	DWORD  Used;
	DWORD  Peak;
	DWORD  Dropped;

	void Reset() { Used = 0; Dropped = 0; }
	void Track() { Peak = Max( Peak, Used ); }
};

static FPVRCmdArena   GPVRArena[PVR_LIST_PT_POLY + 1];
static pvr_list_t     GPVRDirectList = (pvr_list_t)-1;
static pvr_dr_state_t GPVRDRState;

// Written to when an arena is full, so a dropped primitive costs a store
// rather than a branch in every emit.
static BYTE GPVRBitBucket[64] __attribute__((aligned(32)));

//
// Reserve room for one complete primitive group.  Returns false when the
// arena cannot hold it, in which case the caller must skip the whole draw:
// truncating a strip halfway would leave the TA without an end-of-list marker.
//
static inline UBOOL PVRBeginDraw( pvr_list_t List, DWORD MaxBytes )
{
	if( List == GPVRDirectList )
		return 1;
	FPVRCmdArena& A = GPVRArena[List];
	if( A.Used + MaxBytes > A.Capacity )
	{
		++A.Dropped;
		return 0;
	}
	return 1;
}

// Obtain a 32-byte destination for one TA payload. Only valid between a
// successful PVRBeginDraw and the matching PVRCommit32.
static inline void* PVRAlloc32( pvr_list_t List )
{
	if( List == GPVRDirectList )
		return pvr_dr_target( GPVRDRState );
	FPVRCmdArena& A = GPVRArena[List];
	if( A.Used + 32 > A.Capacity )
		return GPVRBitBucket;
	BYTE* P = A.Data + A.Used;
	A.Used += 32;
	return P;
}

static inline void PVRCommit32( pvr_list_t List, void* Dst )
{
	if( List == GPVRDirectList )
		pvr_dr_commit( Dst );
}

/*-----------------------------------------------------------------------------
	Header cache.
-----------------------------------------------------------------------------*/

//
// Recompiling a polygon context per primitive is pure waste: Unreal already
// sorts solid surfaces by texture, so consecutive draws usually share state.
// One cached key per list, since the three lists are built independently.
//
struct FPVRHeaderCache
{
	QWORD Key;
	UBOOL Valid;
};
static FPVRHeaderCache GPVRHeaderCache[PVR_LIST_PT_POLY + 1];

static inline void PVRInvalidateHeaders()
{
	for( INT i = 0; i <= PVR_LIST_PT_POLY; ++i )
		GPVRHeaderCache[i].Valid = 0;
}

/*-----------------------------------------------------------------------------
	Vertex emission.
-----------------------------------------------------------------------------*/

// A screen-space vertex ready for the TA.
struct FPVRVert
{
	FLOAT SX, SY, SZ;   // screen X/Y and 1/w
	FLOAT U, V;
	DWORD ARGB;
};

// Camera-space vertex, used only while near-plane clipping.
struct FPVRClipVert
{
	FLOAT X, Y, Z;
	FLOAT U, V;
	DWORD ARGB;
};

static inline void PVREmitVert( pvr_list_t List, const FPVRVert& V, DWORD Flags )
{
	pvr_vertex_t* Vtx = (pvr_vertex_t*)PVRAlloc32( List );
	Vtx->flags = Flags;
	Vtx->x     = V.SX;
	Vtx->y     = V.SY;
	Vtx->z     = V.SZ;
	Vtx->u     = V.U;
	Vtx->v     = V.V;
	Vtx->argb  = V.ARGB;
	Vtx->oargb = 0;
	PVRCommit32( List, Vtx );
}

//
// Submit a convex polygon as a single triangle strip.
//
// Zig-zag order (v0, v1, vN-1, v2, vN-2, ...) covers the same area as a fan
// but costs N vertices for N-2 triangles instead of 3*(N-2).
//
static void PVREmitConvexStrip( pvr_list_t List, const FPVRVert* V, INT Count )
{
	if( Count < 3 )
		return;

	// v0, v1, v[n-1], v2, v[n-2], ... -- the standard convex fan-to-strip
	// walk. Build the order first so the emit loop knows which vertex is last.
	INT Order[PVR_MAX_POLY_VERTS];
	INT N = 0;
	INT Lo = 0, Hi = Count - 1;
	Order[N++] = Lo++;
	Order[N++] = Lo++;
	UBOOL TakeHigh = 1;
	while( Lo <= Hi )
	{
		if( TakeHigh )
			Order[N++] = Hi--;
		else
			Order[N++] = Lo++;
		TakeHigh = !TakeHigh;
	}

	for( INT i = 0; i < N; ++i )
		PVREmitVert( List, V[Order[i]], ( i == N - 1 ) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX );
}

/*-----------------------------------------------------------------------------
	Projection and near-plane clipping.
-----------------------------------------------------------------------------*/

//
// Matches FTransform::Project exactly, so vertices produced here line up with
// the ones Unreal projected for us.
//
static inline void PVRProject( const FSceneNode* Frame, const FPVRClipVert& In, FPVRVert& Out )
{
	const FLOAT RZ = Frame->Proj.Z * shz_invf_fsrra( In.Z );
	Out.SX   = In.X * RZ + Frame->FX15;
	Out.SY   = In.Y * RZ + Frame->FY15;
	Out.SZ   = RZ;
	Out.U    = In.U;
	Out.V    = In.V;
	Out.ARGB = In.ARGB;
}

//
// Sutherland-Hodgman against Z = PVR_NEAR_Z in camera space.  Returns the
// clipped vertex count; Out must hold at least InCount + 1 vertices.
//
static INT PVRClipNear( const FPVRClipVert* In, INT InCount, FPVRClipVert* Out )
{
	INT OutCount = 0;
	FPVRClipVert S = In[InCount - 1];
	UBOOL SIn = ( S.Z >= PVR_NEAR_Z );
	for( INT i = 0; i < InCount; ++i )
	{
		const FPVRClipVert& E = In[i];
		const UBOOL EIn = ( E.Z >= PVR_NEAR_Z );
		if( SIn != EIn )
		{
			// Exact divide: the denominator straddles zero here, and an
			// FSRRA-based reciprocal warps geometry at the near plane.
			const FLOAT Denom = E.Z - S.Z;
			const FLOAT T = ( Denom != 0.f ) ? Clamp( (PVR_NEAR_Z - S.Z) / Denom, 0.f, 1.f ) : 0.f;
			FPVRClipVert& I = Out[OutCount++];
			I.X    = shz_lerpf( S.X, E.X, T );
			I.Y    = shz_lerpf( S.Y, E.Y, T );
			I.Z    = PVR_NEAR_Z;
			I.U    = shz_lerpf( S.U, E.U, T );
			I.V    = shz_lerpf( S.V, E.V, T );
			I.ARGB = PVRLerpARGB( S.ARGB, E.ARGB, T );
		}
		if( EIn )
			Out[OutCount++] = E;
		S = E;
		SIn = EIn;
	}
	return OutCount;
}

/*-----------------------------------------------------------------------------
	PVR initialization parameters.
-----------------------------------------------------------------------------*/

static pvr_init_params_t GPVRInitParams = {
	/* opb_sizes             */ { PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_8 },
	/* vertex_buf_size       */ 2536 * 256,
	/* dma_enabled           */ 0,
	/* fsaa_enabled          */ 0,
	/* autosort_disabled     */ 1,   // Unreal already emits translucent geometry back to front.
	/* opb_overflow_count    */ 2,
	/* vbuf_doublebuf_disabled */ 0
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
	new(Class, "NoFiltering",     RF_Public)UBoolProperty( CPP_PROPERTY(NoFiltering),     "Options", CPF_Config );
	new(Class, "UseTriStrips",    RF_Public)UBoolProperty( CPP_PROPERTY(UseTriStrips),    "Options", CPF_Config );
	new(Class, "DistanceFog",     RF_Public)UBoolProperty( CPP_PROPERTY(DistanceFog),     "Options", CPF_Config );
	new(Class, "Overbright",      RF_Public)UBoolProperty( CPP_PROPERTY(Overbright),      "Options", CPF_Config );
	new(Class, "VolumetricFog",   RF_Public)UBoolProperty( CPP_PROPERTY(VolumetricFog),   "Options", CPF_Config );
	new(Class, "FogDistanceDefault", RF_Public)UIntProperty( CPP_PROPERTY(FogDistanceDefault), "Options", CPF_Config );
	new(Class, "CommandBufferKB", RF_Public)UIntProperty ( CPP_PROPERTY(CommandBufferKB), "Options", CPF_Config );
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
	UseTriStrips = true;
	DistanceFog = false;
	Overbright = true;
	VolumetricFog = false;
	FogDistanceDefault = 0;
	CommandBufferKB = 256;
	OverlayZFlash = 1024.f;
	OverlayZUI = 4096.f;
	UIZStep = 1.f;
	UIZCursor = 0.f;
	FogActive = 0;
	Compose = NULL;
	ComposeSize = 0;
	VRAMUsed = 0;
	TextureFrame = 0;
	appMemset( PaletteBanks, 0, sizeof(PaletteBanks) );
	appMemset( &TextureCPUProfile, 0, sizeof(TextureCPUProfile) );
	appMemset( AtlasPages, 0, sizeof(AtlasPages) );
	AtlasPageCount = 0;
}

UBOOL UPVRRenderDevice::Init( UViewport* InViewport )
{
	guard(UPVRRenderDevice::Init)

#if defined(PLATFORM_DREAMCAST)
	struct mallinfo InitBefore = mallinfo();
	UBOOL ReusedPVR = GPVRSessionInitialized;
	if( !GPVRSessionInitialized )
	{
		if( pvr_init(&GPVRInitParams) < 0 )
			appErrorf("PVR initialization failed");
		GPVRSessionInitialized = 1;
	}
	struct mallinfo InitAfter = mallinfo();
	debugf( "DCPVRLIFETIME phase=init reused=%d heap_before=%u heap_after=%u delta=%d",
		ReusedPVR,
		(DWORD)InitBefore.uordblks, (DWORD)InitAfter.uordblks,
		(INT)InitAfter.uordblks - (INT)InitBefore.uordblks );
#else
	pvr_init(&GPVRInitParams);
#endif

	// Volumetric fog costs a third translucent pass per surface plus the
	// per-texel Volumetric() loop in FLightManager, so it is opt-in.
	SupportsFogMaps     = VolumetricFog ? 1 : 0;
	SupportsDistanceFog = false;
	NoVolumetricBlend   = true;
	SupportsTriStrips   = UseTriStrips ? 1 : 0;

	// Split the command budget between the two buffered lists. Translucent
	// carries a lightmap pass for every lit surface, so it needs the bulk.
	CommandBufferKB = Clamp( CommandBufferKB, 64, 1024 );
	const DWORD Budget = (DWORD)CommandBufferKB * 1024;
	const DWORD TRBytes = (Budget * 3 / 4) & ~31u;
	const DWORD PTBytes = (Budget - TRBytes) & ~31u;
	appMemset( GPVRArena, 0, sizeof(GPVRArena) );
	GPVRArena[PVR_LIST_TR_POLY].Data     = (BYTE*)memalign( 32, TRBytes );
	GPVRArena[PVR_LIST_TR_POLY].Capacity = TRBytes;
	GPVRArena[PVR_LIST_PT_POLY].Data     = (BYTE*)memalign( 32, PTBytes );
	GPVRArena[PVR_LIST_PT_POLY].Capacity = PTBytes;
	if( !GPVRArena[PVR_LIST_TR_POLY].Data || !GPVRArena[PVR_LIST_PT_POLY].Data )
		appErrorf( "PVR command buffer allocation failed (%u bytes)", Budget );
	debugf( NAME_Log, "PVR command buffers: PT=%u TR=%u bytes", PTBytes, TRBytes );
	debugf( NAME_Log, "PVR options: tristrips=%i distancefog=%i fogdefault=%i volumetricfog=%i"
		" shiny=%i volumetriclighting=%i coronas=%i filtering=%i overbright=%i",
		(INT)UseTriStrips, (INT)DistanceFog, FogDistanceDefault, (INT)VolumetricFog,
		(INT)ShinySurfaces, (INT)VolumetricLighting, (INT)Coronas, (INT)!NoFiltering,
		(INT)Overbright );

	Compose = NULL;
	ComposeSize = 0;
	pvr_set_pal_format( PVR_PAL_ARGB1555 );
	pvr_set_bg_color( 0.f, 0.f, 0.f );

	PrintMemStats();

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

	for( INT i = 0; i <= PVR_LIST_PT_POLY; ++i )
	{
		if( GPVRArena[i].Data )
			free( GPVRArena[i].Data );
		GPVRArena[i].Data = NULL;
		GPVRArena[i].Capacity = 0;
	}

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
	PVRInvalidateHeaders();

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
	LightAtlasFlush();
	appMemset( PaletteBanks, 0, sizeof(PaletteBanks) );

	unguard;
}

UBOOL UPVRRenderDevice::Exec( const char* Cmd, FOutputDevice* Out )
{
#if defined(PLATFORM_DREAMCAST)
	if( ParseCommand(&Cmd, "DCPDETAIL") )
	{
		GDCFrameProfileDetailed = !GDCFrameProfileDetailed;
		DCFrameProfileReset();
		Out->Logf("Detailed profiling %s; wait 30 ticks", GDCFrameProfileDetailed ? "on" : "off");
		return true;
	}
	if( ParseCommand(&Cmd, "DCPOVERLAY") )
	{
		GDCFrameProfileOverlay = !GDCFrameProfileOverlay;
		DCFrameProfileReset();
		Out->Logf("Profile overlay %s; wait 30 ticks", GDCFrameProfileOverlay ? "on" : "off");
		return true;
	}
	if( ParseCommand(&Cmd, "DCPPAGE") )
	{
		GDCFrameProfilePage = (GDCFrameProfilePage + 1) % 3;
		return true;
	}
	if( ParseCommand(&Cmd, "DCPDUMP") )
	{
		DCFrameProfileReport(Out);
		return true;
	}
	if( ParseCommand(&Cmd, "DCLEGACYTIMERS") )
	{
		GDCLegacyTimers = !GDCLegacyTimers;
		Out->Logf("Legacy timers %s", GDCLegacyTimers ? "on" : "off");
		return true;
	}
#endif
#if defined(PLATFORM_DREAMCAST)
	if( ParseCommand(&Cmd, "DCPROFILE") )
	{
		GDCFrameProfileEnabled = !GDCFrameProfileEnabled;
		Out->Logf("Frame profile %s", GDCFrameProfileEnabled ? "on" : "off");
		return true;
	}
#endif
	return false;
}

/*-----------------------------------------------------------------------------
	Frame.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
	guard(UPVRRenderDevice::Lock);

	// Texture uploads and eviction happen while building this frame's lists.
	// Wait here, before either can alter memory used by the previous scene.
	{
		DC_FRAME_SCOPE(DCFS_Wait);
		pvr_wait_ready();
	}
	++TextureFrame;
#if defined(PLATFORM_DREAMCAST)
	pvr_stats_t ProfileStats;
	if( pvr_get_stats(&ProfileStats) == 0 )
		DCFrameGPU(ProfileStats.frame_count, ProfileStats.rnd_last_time, ProfileStats.vtx_buffer_used);
#endif

	GPVRArena[PVR_LIST_PT_POLY].Reset();
	GPVRArena[PVR_LIST_TR_POLY].Reset();
	PVRInvalidateHeaders();
	UIZCursor = 0.f;

	pvr_set_bg_color( 0.f, 0.f, 0.f );
	pvr_scene_begin();

	// Opaque geometry is generated in an order the hardware accepts, so it
	// goes straight to the TA; the other two lists are buffered.
	pvr_list_begin( PVR_LIST_OP_POLY );
	GPVRDirectList = PVR_LIST_OP_POLY;

	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
		ColorMod = FPlane( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.f - Min( FlashScale.X * 2.f, 1.f ) );
	else
		ColorMod = FPlane( 0.f, 0.f, 0.f, 0.f );

	unguard;
}

void UPVRRenderDevice::Unlock( UBOOL Blit )
{
	guard(UPVRRenderDevice::Unlock);
	DC_FRAME_SCOPE(DCFS_Submit);

	// Close the opaque list; everything after this is replayed from RAM.
	pvr_list_finish();
	GPVRDirectList = (pvr_list_t)-1;

	FPVRCmdArena& PT = GPVRArena[PVR_LIST_PT_POLY];
	FPVRCmdArena& TR = GPVRArena[PVR_LIST_TR_POLY];
	PT.Track();
	TR.Track();

	// Submission order is free: the tile accelerator only bins, and the ISP
	// always renders opaque, then punch-through, then translucent.
	if( PT.Used )
	{
		PVR_SET(PVR_PT_ALPHA_REF, PVR_PT_ALPHA_THRESHOLD);
		pvr_list_begin( PVR_LIST_PT_POLY );
		pvr_prim( PT.Data, PT.Used );
		pvr_list_finish();
	}
	if( TR.Used )
	{
		pvr_list_begin( PVR_LIST_TR_POLY );
		pvr_prim( TR.Data, TR.Used );
		pvr_list_finish();
	}

	// Measure TA vertex-buffer usage BEFORE scene_finish (which resets POS).
	// If the buffer fills, the TA silently drops subsequent polys -> geometry
	// vanishes depending on view angle.
	{
		const size_t End  = PVR_GET(PVR_TA_VERTBUF_END);
		const size_t Pos  = PVR_GET(PVR_TA_VERTBUF_POS);
		const DWORD  Free = (End > Pos) ? (DWORD)(End - Pos) : 0;
		if( Free == 0 )
			debugf( "PVR: TA VERTEX BUFFER FULL (Pos=%u End=%u) - geometry being dropped!", (unsigned)Pos, (unsigned)End );
	}

	pvr_scene_finish();

	if( PT.Dropped || TR.Dropped )
	{
		static DWORD LastWarnFrame = 0;
		if( TextureFrame - LastWarnFrame >= 60 )
		{
			LastWarnFrame = TextureFrame;
			debugf( NAME_Warning, "PVR command buffer full: pt_dropped=%u tr_dropped=%u pt_peak=%u/%u tr_peak=%u/%u"
				" (raise PVRDrv.PVRRenderDevice.CommandBufferKB)",
				PT.Dropped, TR.Dropped, PT.Peak, PT.Capacity, TR.Peak, TR.Capacity );
		}
	}

	if( TextureFrame % 300 == 0 )
	{
		debugf( "DCPVRCMD pt_peak=%u/%u tr_peak=%u/%u", PT.Peak, PT.Capacity, TR.Peak, TR.Capacity );
		PrintTextureCPUProfile( 300 );
#if defined(PLATFORM_DREAMCAST)
		appDCDumpProceduralTextureProfile( 300 );
#endif
	}

	unguard;
}

/*-----------------------------------------------------------------------------
	Render state.
-----------------------------------------------------------------------------*/

//
// Apply Unreal's polygon flag precedence. The sky is never allowed to occlude.
//
DWORD UPVRRenderDevice::AdjustFlags( DWORD PolyFlags ) const
{
	if( !(PolyFlags & (PF_Translucent|PF_Modulated)) && !CurrentSceneNode.bIsSky )
		PolyFlags |= PF_Occlude;
	else if( PolyFlags & PF_Translucent )
		PolyFlags &= ~PF_Masked;
	return PolyFlags;
}

pvr_list_t UPVRRenderDevice::ListFor( DWORD PolyFlags ) const
{
	if( PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted) )
		return PVR_LIST_TR_POLY;
	if( PolyFlags & PF_Masked )
		return PVR_LIST_PT_POLY;
	return PVR_LIST_OP_POLY;
}

//
// Compile and emit a polygon header, skipping it when the previous primitive
// in this list already established the same state.
//
void UPVRRenderDevice::EmitHeader( pvr_list_t List, DWORD PolyFlags, const FTexState* Tex, UBOOL NoDepth )
{
	// State that actually reaches the hardware, hashed into one key. The
	// texture is identified by its cache ID rather than its VRAM address:
	// re-uploads move the address, and those already invalidate the cache.
	const DWORD StateBits =
		  ( PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted|PF_Invisible|PF_Occlude|PF_Masked) )
		| ( NoDepth ? 0x1u : 0u );
	const QWORD Key = ( Tex && Tex->Tex ) ? ( (Tex->Key << 8) ^ StateBits ) : StateBits;

	FPVRHeaderCache& Cache = GPVRHeaderCache[List];
	if( Cache.Valid && Cache.Key == Key )
		return;
	Cache.Valid = 1;
	Cache.Key = Key;
#if defined(PLATFORM_DREAMCAST)
	DCFrameHeader();
#endif

	pvr_poly_cxt_t Cxt;
	if( Tex && Tex->Tex )
	{
		pvr_poly_cxt_txr( &Cxt, List, Tex->Format, Tex->Width, Tex->Height,
			Tex->Tex, NoFiltering ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR );
		Cxt.txr.mipmap = Tex->MipMapped;
	}
	else
	{
		pvr_poly_cxt_col( &Cxt, List );
	}

	if( NoDepth )
	{
		// Screen-space overlays: always pass, and claim the depth slot so
		// translucent geometry submitted later is rejected behind them.
		Cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
		Cxt.depth.write      = (List == PVR_LIST_TR_POLY) ? PVR_DEPTHWRITE_DISABLE : PVR_DEPTHWRITE_ENABLE;
	}
	else
	{
		Cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
		// Only surfaces Unreal marked as occluders may write depth.
		// AdjustFlags withholds PF_Occlude from the sky on purpose: the sky is
		// a child scene node rendered from a different origin, so its 1/w
		// values are meaningless next to the world's. On a PC the engine
		// clears Z between the two; a tile-based deferred renderer cannot, so
		// the sky instead relies on being submitted first and leaving the
		// depth buffer alone, letting the world overwrite it.
		//
		// depth.write is the raw hardware "Z-write disable" bit despite its
		// "Enable depth writes" doc comment: pvr_prim.c does
		//   FIELD_PREP(PVR_TA_PM1_DEPTHWRITE, depth.write)
		// so 0 = writes ENABLED, 1 = writes DISABLED. Always assign
		// PVR_DEPTHWRITE_ENABLE/DISABLE, never true/false.
		Cxt.depth.write = ( List == PVR_LIST_TR_POLY || !(PolyFlags & PF_Occlude) )
			? PVR_DEPTHWRITE_DISABLE : PVR_DEPTHWRITE_ENABLE;
	}

	// Unreal's BSP and mesh code do their own backface rejection, and mirrored
	// scene nodes invert winding, so leave the hardware culler off.
	Cxt.gen.culling = PVR_CULLING_NONE;

	if( PolyFlags & PF_Invisible )
	{
		Cxt.blend.src = PVR_BLEND_ZERO;
		Cxt.blend.dst = PVR_BLEND_ZERO;
	}
	else if( List == PVR_LIST_TR_POLY )
	{
		if( PolyFlags & PF_Modulated )
		{
			Cxt.blend.src = PVR_BLEND_DESTCOLOR;
			Cxt.blend.dst = Overbright ? PVR_BLEND_DESTCOLOR : PVR_BLEND_ZERO;
		}
		else if( PolyFlags & PF_Translucent )
		{
			// Approximates GL_ONE / GL_ONE_MINUS_SRC_COLOR.
			Cxt.blend.src = PVR_BLEND_ONE;
			Cxt.blend.dst = PVR_BLEND_ONE;
		}
		else if( PolyFlags & PF_Highlighted )
		{
			// GL_ONE / GL_ONE_MINUS_SRC_ALPHA.
			Cxt.blend.src = PVR_BLEND_ONE;
			Cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
		}
		else
		{
			// A screen overlay carrying no blend flag of its own still has to
			// honour its texture's alpha, which is what Unreal's HUD relies on.
			Cxt.blend.src = PVR_BLEND_SRCALPHA;
			Cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
		}
	}
	else
	{
		Cxt.blend.src = PVR_BLEND_ONE;
		Cxt.blend.dst = PVR_BLEND_ZERO;
	}

	Cxt.txr.alpha    = PVR_TXRALPHA_ENABLE;
	// The opaque list discards alpha anyway; both other lists need the vertex
	// alpha folded in as well as the texture's.
	Cxt.txr.env      = (List == PVR_LIST_OP_POLY) ? PVR_TXRENV_MODULATE : PVR_TXRENV_MODULATEALPHA;

	// Fog is applied per pixel before the blend unit, so it can only go on the
	// pass that establishes base colour. Putting it on the translucent list
	// would fog the lightmap itself, which a modulate blend then multiplies
	// into the framebuffer -- the result is not fog. Screen-space overlays
	// never fog.
	Cxt.gen.fog_type = ( FogActive && !NoDepth && List != PVR_LIST_TR_POLY )
		? PVR_FOG_TABLE : PVR_FOG_DISABLE;

	pvr_poly_hdr_t Hdr;
	pvr_poly_compile( &Hdr, &Cxt );

	pvr_poly_hdr_t* Out = (pvr_poly_hdr_t*)PVRAlloc32( List );
	*Out = Hdr;
	PVRCommit32( List, Out );
}

/*-----------------------------------------------------------------------------
	World surfaces.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guard(UPVRRenderDevice::DrawComplexSurface);

	check(Surface.Texture);

	SetSceneNode( Frame );

	// @HACK: Don't draw translucent and masked parts of the sky. The PVR has
	// no mid-scene depth clear, so portals and sky share one depth range.
	if( CurrentSceneNode.bIsSky && ( Surface.PolyFlags & (PF_Translucent|PF_Masked) ) )
		return;

	const DWORD BaseFlags = AdjustFlags( Surface.PolyFlags );
	const pvr_list_t BaseList = ListFor( BaseFlags );

	// Bind both textures up front so the per-polygon loop can emit the base
	// and lightmap passes from one set of screen coordinates.
	SetTexture( *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
	FTexState Base;
	CaptureTexState( Base );

	const UBOOL HasLightMap = ( Surface.LightMap != NULL ) && !CurrentSceneNode.bIsSky;
	FTexState Light;
	DWORD LightFlags = 0;
	if( HasLightMap )
	{
		SetTexture( *Surface.LightMap, 0, -0.5f );
		CaptureTexState( Light );
		LightFlags = PF_Modulated | (Surface.PolyFlags & PF_Masked);
	}

	// Volumetric light shafts, when the device advertises fog maps.
	const UBOOL HasFogMap = ( Surface.FogMap != NULL ) && !CurrentSceneNode.bIsSky;
	FTexState Fog;
	DWORD FogFlags = 0;
	if( HasFogMap )
	{
		SetTexture( *Surface.FogMap, ( Surface.PolyFlags & PF_Masked ), -0.5f );
		CaptureTexState( Fog );
		FogFlags = PF_Highlighted | (Surface.PolyFlags & PF_Masked);
	}

	const FVector& MapU = Facet.MapCoords.XAxis;
	const FVector& MapV = Facet.MapCoords.YAxis;
	const FLOAT UDot = PVRDot( MapU, Facet.MapCoords.Origin );
	const FLOAT VDot = PVRDot( MapV, Facet.MapCoords.Origin );

	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		const INT NumPts = Poly->NumPts;
		if( NumPts < 3 || NumPts > FBspNode::MAX_FINAL_VERTICES )
			continue;

		// Unreal has already clipped against the four side planes and filled
		// in ScreenX/ScreenY/RZ, so the common case needs no projection work
		// at all -- only the raw map-coordinate dots for U and V.
		FLOAT Dots[PVR_MAX_POLY_VERTS][2];
		UBOOL NeedsClip = 0;
		INT i;
		for( i = 0; i < NumPts; ++i )
		{
			const FVector& P = Poly->Pts[i]->Point;
			Dots[i][0] = PVRDot( MapU, P ) - UDot;
			Dots[i][1] = PVRDot( MapV, P ) - VDot;
			NeedsClip |= ( P.Z < PVR_NEAR_Z );
		}

		FPVRVert Verts[PVR_MAX_POLY_VERTS];
		INT Count = NumPts;
		if( !NeedsClip )
		{
			for( i = 0; i < NumPts; ++i )
			{
				const FTransform& T = *Poly->Pts[i];
				Verts[i].SX   = T.ScreenX;
				Verts[i].SY   = T.ScreenY;
				Verts[i].SZ   = T.RZ;
				Verts[i].ARGB = 0xFFFFFFFFu;
			}
		}
		else
		{
			// Rebuild in camera space, clip, and project ourselves. U and V
			// are carried as unscaled dots and scaled per pass below.
			FPVRClipVert In[PVR_MAX_POLY_VERTS], Clipped[PVR_MAX_POLY_VERTS];
			for( i = 0; i < NumPts; ++i )
			{
				const FVector& P = Poly->Pts[i]->Point;
				In[i].X = P.X; In[i].Y = P.Y; In[i].Z = P.Z;
				In[i].U = Dots[i][0];
				In[i].V = Dots[i][1];
				In[i].ARGB = 0xFFFFFFFFu;
			}
			Count = PVRClipNear( In, NumPts, Clipped );
			if( Count < 3 )
				continue;
			for( i = 0; i < Count; ++i )
			{
				PVRProject( Frame, Clipped[i], Verts[i] );
				Dots[i][0] = Clipped[i].U;
				Dots[i][1] = Clipped[i].V;
			}
		}

		const DWORD MaxBytes = 32 + Count * 32;

		// Base texture pass.
		if( PVRBeginDraw( BaseList, MaxBytes ) )
		{
			EmitHeader( BaseList, BaseFlags, &Base, 0 );
			for( i = 0; i < Count; ++i )
			{
				Verts[i].U = ( Dots[i][0] - Base.UPan ) * Base.UMult;
				Verts[i].V = ( Dots[i][1] - Base.VPan ) * Base.VMult;
			}
			PVREmitConvexStrip( BaseList, Verts, Count );
		}

		// Lightmap modulate pass.
		if( HasLightMap && PVRBeginDraw( PVR_LIST_TR_POLY, MaxBytes ) )
		{
			EmitHeader( PVR_LIST_TR_POLY, LightFlags, &Light, 0 );
			for( i = 0; i < Count; ++i )
			{
				Verts[i].U = ( Dots[i][0] - Light.UPan ) * Light.UMult;
				Verts[i].V = ( Dots[i][1] - Light.VPan ) * Light.VMult;
			}
			PVREmitConvexStrip( PVR_LIST_TR_POLY, Verts, Count );
		}

		// Volumetric fog pass.
		if( HasFogMap && PVRBeginDraw( PVR_LIST_TR_POLY, MaxBytes ) )
		{
			EmitHeader( PVR_LIST_TR_POLY, FogFlags, &Fog, 0 );
			for( i = 0; i < Count; ++i )
			{
				Verts[i].U = ( Dots[i][0] - Fog.UPan ) * Fog.UMult;
				Verts[i].V = ( Dots[i][1] - Fog.VPan ) * Fog.VMult;
			}
			PVREmitConvexStrip( PVR_LIST_TR_POLY, Verts, Count );
		}
	}

	unguard;
}

/*-----------------------------------------------------------------------------
	Gouraud geometry.
-----------------------------------------------------------------------------*/

//
// Pack one transformed mesh vertex. Unreal already projected it for us.
//
static inline void PVRBuildGouraudVert( const FTransTexture& P, FLOAT UMult, FLOAT VMult, UBOOL Modulated, FPVRVert& Out )
{
	Out.SX   = P.ScreenX;
	Out.SY   = P.ScreenY;
	Out.SZ   = P.RZ;
	Out.U    = P.U * UMult;
	Out.V    = P.V * VMult;
	Out.ARGB = Modulated ? 0xFFFFFFFFu : PVRPackLight( P.Light );
}

static inline void PVRBuildGouraudClipVert( const FTransTexture& P, FLOAT UMult, FLOAT VMult, UBOOL Modulated, FPVRClipVert& Out )
{
	Out.X    = P.Point.X;
	Out.Y    = P.Point.Y;
	Out.Z    = P.Point.Z;
	Out.U    = P.U * UMult;
	Out.V    = P.V * VMult;
	Out.ARGB = Modulated ? 0xFFFFFFFFu : PVRPackLight( P.Light );
}

//
// Clip one triangle against the near plane and emit it as a 3- or 4-vertex
// strip. Used only when a triangle actually straddles the plane.
//
static void PVREmitClippedTriangle( pvr_list_t List, const FSceneNode* Frame, const FPVRClipVert* Tri )
{
	FPVRClipVert Clipped[8];
	const INT Count = PVRClipNear( Tri, 3, Clipped );
	if( Count < 3 )
		return;
	FPVRVert Verts[8];
	for( INT i = 0; i < Count; ++i )
		PVRProject( Frame, Clipped[i], Verts[i] );
	PVREmitConvexStrip( List, Verts, Count );
}

void UPVRRenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer )
{
	guard(UPVRRenderDevice::DrawGouraudPolygon);

	if( NumPts < 3 || NumPts > FBspNode::MAX_FINAL_VERTICES )
		return;

	SetSceneNode( Frame );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );

	const DWORD Flags = AdjustFlags( PolyFlags );
	const pvr_list_t List = ListFor( Flags );
	const UBOOL Modulated = ( PolyFlags & PF_Modulated ) != 0;

	UBOOL NeedsClip = 0;
	INT i;
	for( i = 0; i < NumPts; ++i )
		NeedsClip |= ( Pts[i]->Point.Z < PVR_NEAR_Z );

	FTexState Tex;
	CaptureTexState( Tex );

	if( !NeedsClip )
	{
		if( !PVRBeginDraw( List, 32 + NumPts * 32 ) )
			return;
		EmitHeader( List, Flags, &Tex, 0 );
		FPVRVert Verts[PVR_MAX_POLY_VERTS];
		for( i = 0; i < NumPts; ++i )
			PVRBuildGouraudVert( *Pts[i], TexInfo.UMult, TexInfo.VMult, Modulated, Verts[i] );
		// This is a convex polygon (a fan), not a strip -- Unreal's near
		// clipping turns a triangle into a 4- or 5-gon and the two orderings
		// cover different areas.
		PVREmitConvexStrip( List, Verts, NumPts );
		return;
	}

	// Fan the polygon into triangles and clip each one.
	if( !PVRBeginDraw( List, 32 + (NumPts - 2) * 4 * 32 ) )
		return;
	EmitHeader( List, Flags, &Tex, 0 );
	FPVRClipVert V0;
	PVRBuildGouraudClipVert( *Pts[0], TexInfo.UMult, TexInfo.VMult, Modulated, V0 );
	for( i = 1; i < NumPts - 1; ++i )
	{
		FPVRClipVert Tri[3];
		Tri[0] = V0;
		PVRBuildGouraudClipVert( *Pts[i],   TexInfo.UMult, TexInfo.VMult, Modulated, Tri[1] );
		PVRBuildGouraudClipVert( *Pts[i+1], TexInfo.UMult, TexInfo.VMult, Modulated, Tri[2] );
		PVREmitClippedTriangle( List, Frame, Tri );
	}

	unguard;
}

void UPVRRenderDevice::DrawGouraudTriStrip( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer )
{
	guard(UPVRRenderDevice::DrawGouraudTriStrip);

	if( NumPts < 3 )
		return;

	SetSceneNode( Frame );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );

	const DWORD Flags = AdjustFlags( PolyFlags );
	const pvr_list_t List = ListFor( Flags );
	const UBOOL Modulated = ( PolyFlags & PF_Modulated ) != 0;

	UBOOL NeedsClip = 0;
	INT i;
	for( i = 0; i < NumPts; ++i )
		NeedsClip |= ( Pts[i]->Point.Z < PVR_NEAR_Z );

	FTexState Tex;
	CaptureTexState( Tex );

	if( !NeedsClip )
	{
		// The whole strip goes to the TA as a strip: N vertices for N-2
		// triangles, which is the entire point of cooking meshes this way.
		if( !PVRBeginDraw( List, 32 + NumPts * 32 ) )
			return;
		EmitHeader( List, Flags, &Tex, 0 );
		for( i = 0; i < NumPts; ++i )
		{
			FPVRVert V;
			PVRBuildGouraudVert( *Pts[i], TexInfo.UMult, TexInfo.VMult, Modulated, V );
			PVREmitVert( List, V, ( i == NumPts - 1 ) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX );
		}
		return;
	}

	// Something straddles the near plane; fall back to clipping each triangle
	// of the strip independently, preserving the strip's winding alternation.
	if( !PVRBeginDraw( List, 32 + (NumPts - 2) * 4 * 32 ) )
		return;
	EmitHeader( List, Flags, &Tex, 0 );
	for( i = 2; i < NumPts; ++i )
	{
		FPVRClipVert Tri[3];
		const INT A = ( i & 1 ) ? i - 1 : i - 2;
		const INT B = ( i & 1 ) ? i - 2 : i - 1;
		PVRBuildGouraudClipVert( *Pts[A], TexInfo.UMult, TexInfo.VMult, Modulated, Tri[0] );
		PVRBuildGouraudClipVert( *Pts[B], TexInfo.UMult, TexInfo.VMult, Modulated, Tri[1] );
		PVRBuildGouraudClipVert( *Pts[i], TexInfo.UMult, TexInfo.VMult, Modulated, Tri[2] );
		PVREmitClippedTriangle( List, Frame, Tri );
	}

	unguard;
}

/*-----------------------------------------------------------------------------
	Tiles.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags )
{
	guard(UPVRRenderDevice::DrawTile);

	// Mark as UI tile so UploadTexture keeps SH4-side data for reloads.
	TexInfo.bIsTile = true;
	SetSceneNode( Frame );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );
	TexInfo.bIsTile = false;

	FTexState Tex;
	CaptureTexState( Tex );

	// Every tile goes to the translucent list, and it has to.
	//
	// The hardware renders punch-through before translucent no matter what
	// order the lists were submitted in, so splitting the HUD across the two
	// destroys its painter ordering: a panel routed to TR lands on top of the
	// text routed to PT, whatever order Unreal drew them in. Punch-through is
	// also one-bit alpha only, so a partially transparent tile comes out
	// opaque there. Keeping the HUD in one list preserves both.
	//
	// Masked world geometry still uses punch-through -- it is depth tested, so
	// the depth buffer resolves it rather than submission order.
	const pvr_list_t List = PVR_LIST_TR_POLY;

	// One header plus a four-vertex strip.
	if( !PVRBeginDraw( List, 32 + 4 * 32 ) )
		return;

	// Screen-space overlay depth. Ordering between tiles comes from submission
	// order, so every tile can share one depth in front of the world.
	const FLOAT TileZ = OverlayZUI;

	EmitHeader( List, PolyFlags, &Tex, /*NoDepth=*/1 );

	const DWORD ARGB = ( PolyFlags & PF_Modulated ) ? 0xFFFFFFFFu : PVRPackLight( Light );

	const FLOAT U0 =  U       * TexInfo.UMult;
	const FLOAT V0 =  V       * TexInfo.VMult;
	const FLOAT U1 = (U + UL) * TexInfo.UMult;
	const FLOAT V1 = (V + VL) * TexInfo.VMult;

	// Strip order for a quad: top-left, top-right, bottom-left, bottom-right.
	FPVRVert Q[4];
	Q[0].SX = X;      Q[0].SY = Y;      Q[0].U = U0; Q[0].V = V0;
	Q[1].SX = X + XL; Q[1].SY = Y;      Q[1].U = U1; Q[1].V = V0;
	Q[2].SX = X;      Q[2].SY = Y + YL; Q[2].U = U0; Q[2].V = V1;
	Q[3].SX = X + XL; Q[3].SY = Y + YL; Q[3].U = U1; Q[3].V = V1;
	for( INT i = 0; i < 4; ++i )
	{
		Q[i].SZ   = TileZ;
		Q[i].ARGB = ARGB;
		PVREmitVert( List, Q[i], ( i == 3 ) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX );
	}

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

	// The flash tints the world but must not tint the HUD, which is drawn
	// afterwards into the punch-through list at a nearer overlay depth.
	const pvr_list_t List = PVR_LIST_TR_POLY;
	if( !PVRBeginDraw( List, 32 + 64 ) )
		return;

	pvr_sprite_cxt_t Cxt;
	pvr_sprite_cxt_col( &Cxt, List );
	Cxt.gen.culling      = PVR_CULLING_NONE;
	Cxt.gen.fog_type     = PVR_FOG_DISABLE;
	Cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
	Cxt.depth.write      = PVR_DEPTHWRITE_DISABLE;
	Cxt.blend.src        = PVR_BLEND_ONE;
	Cxt.blend.dst        = PVR_BLEND_INVSRCALPHA;

	pvr_sprite_hdr_t Hdr;
	pvr_sprite_compile( &Hdr, &Cxt );
	Hdr.argb  = PVRPackARGB( ColorMod.W, ColorMod.X, ColorMod.Y, ColorMod.Z );
	Hdr.oargb = 0;
	GPVRHeaderCache[List].Valid = 0;

	pvr_sprite_hdr_t* HdrOut = (pvr_sprite_hdr_t*)PVRAlloc32( List );
	*HdrOut = Hdr;
	PVRCommit32( List, HdrOut );

	const FLOAT W = (FLOAT)Viewport->SizeX;
	const FLOAT H = (FLOAT)Viewport->SizeY;

	pvr_sprite_col_t Sprite;
	Sprite.flags = PVR_CMD_VERTEX_EOL;
	Sprite.ax = 0.f; Sprite.ay = H;   Sprite.az = OverlayZFlash;
	Sprite.bx = 0.f; Sprite.by = 0.f; Sprite.bz = OverlayZFlash;
	Sprite.cx = W;   Sprite.cy = 0.f; Sprite.cz = OverlayZFlash;
	Sprite.dx = W;   Sprite.dy = H;
	Sprite.d1 = Sprite.d2 = Sprite.d3 = Sprite.d4 = 0;

	const BYTE* Src = (const BYTE*)&Sprite;
	for( INT Half = 0; Half < 2; ++Half )
	{
		BYTE* Dst = (BYTE*)PVRAlloc32( List );
		appMemcpy( Dst, Src + Half * 32, 32 );
		PVRCommit32( List, Dst );
	}

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
	// A tile-based deferred renderer has no mid-scene depth clear.
}

void UPVRRenderDevice::SetSceneNode( FSceneNode* Frame )
{
	guard(UPVRRenderDevice::SetSceneNode);

	check(Viewport);

	if( !Frame )
	{
		// Invalidate current saved data.
		CurrentSceneNode.X = -1;
		CurrentSceneNode.FX = -1.f;
		CurrentSceneNode.SizeX = -1;
		return;
	}

	if( Frame->X != CurrentSceneNode.X || Frame->Y != CurrentSceneNode.Y ||
		Frame->XB != CurrentSceneNode.XB || Frame->YB != CurrentSceneNode.YB ||
		Viewport->SizeX != CurrentSceneNode.SizeX || Viewport->SizeY != CurrentSceneNode.SizeY )
	{
		CurrentSceneNode.X = Frame->X;
		CurrentSceneNode.Y = Frame->Y;
		CurrentSceneNode.XB = Frame->XB;
		CurrentSceneNode.YB = Frame->YB;
		CurrentSceneNode.SizeX = Viewport->SizeX;
		CurrentSceneNode.SizeY = Viewport->SizeY;
	}

	if( Frame->Level )
	{
		AActor* ZoneActor = Frame->Level->Model->Nodes->Zones[Frame->ZoneNumber].ZoneActor;
		CurrentSceneNode.bIsSky = ( Cast<ASkyZoneInfo>( ZoneActor ) != NULL );
	}

	if( Frame->FX != CurrentSceneNode.FX || Frame->FY != CurrentSceneNode.FY ||
		Viewport->Actor->FovAngle != CurrentSceneNode.FovAngle )
	{
		RProjZ = shz_tanf( Viewport->Actor->FovAngle * (FLOAT)PI / 360.0f );
		Aspect = Frame->FY / Frame->FX;
		RFX2 = 2.0f * RProjZ / Frame->FX;
		RFY2 = 2.0f * RProjZ * Aspect / Frame->FY;
		CurrentSceneNode.FX = Frame->FX;
		CurrentSceneNode.FY = Frame->FY;
		CurrentSceneNode.FovAngle = Viewport->Actor->FovAngle;
	}

	// A world vertex is nearest at the near plane, where 1/w reaches Proj.Z.
	// Overlays are placed above that, so zooming (which raises Proj.Z) cannot
	// push geometry through the screen flash or the HUD.
	const FLOAT WorldMaxZ = Max( Frame->Proj.Z / PVR_NEAR_Z, 1.f );
	OverlayZFlash = WorldMaxZ * PVR_FLASH_Z_SCALE;
	OverlayZUI    = WorldMaxZ * PVR_UI_Z_SCALE;
	UIZStep       = OverlayZUI / PVR_UI_Z_SUBDIV;

	UpdateFog( Frame );

	unguard;
}

//
// Program the hardware fog unit from the viewer's zone.
//
// The PVR indexes its fog table by w, and the z we submit is Proj.Z/Z_camera,
// so a world distance D corresponds to w = D/Proj.Z.  There is a single fog
// table for the whole scene, so the viewer's zone wins; Unreal can have a
// different fog per zone but the hardware cannot.
//
void UPVRRenderDevice::UpdateFog( FSceneNode* Frame )
{
	guard(UPVRRenderDevice::UpdateFog);

	FogActive = 0;
	if( !DistanceFog || !Frame || !Viewport->Actor )
		return;

	AZoneInfo* Zone = Viewport->Actor->Region.Zone;
	if( !Zone || Frame->Proj.Z <= 0.f )
		return;

	// Nothing in this engine reads AZoneInfo::FogDistance, so level authors
	// had no reason to set it and most zones leave it at zero. Fall back to
	// the configured distance for any zone flagged as foggy.
	FLOAT Distance = Zone->FogDistance;
	if( Distance <= 0.f && Zone->bFogZone )
		Distance = (FLOAT)FogDistanceDefault;
	if( Distance <= 0.f )
		return;

	const FLOAT Far = Distance / Frame->Proj.Z;
	pvr_fog_table_color( 1.f,
		Zone->FogColor.R / 255.f,
		Zone->FogColor.G / 255.f,
		Zone->FogColor.B / 255.f );
	pvr_fog_table_linear( 0.f, Far );
	FogActive = 1;

	unguard;
}

/*-----------------------------------------------------------------------------
	Textures.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::ResetTexture( )
{
	guard(UPVRRenderDevice::ResetTexture);
	TexInfo.CurrentCacheID = 0;
	TexInfo.CurrentBind = nullptr;
	unguard;
}

void UPVRRenderDevice::CaptureTexState( FTexState& Out ) const
{
	const FTexBind* Bind = TexInfo.CurrentBind;
	if( IsAtlased( Bind ) )
	{
		const FLightAtlasPage& P = AtlasPages[Bind->AtlasPage];
		Out.Tex       = P.Tex;
		Out.Key       = ( (QWORD)1 << 63 ) | (QWORD)Bind->AtlasPage;
		Out.Format    = Bind->DCFormat;
		Out.Width     = AtlasPageDim;
		Out.Height    = AtlasPageDim;
		Out.MipMapped = 0;
		Out.UMult     = TexInfo.UMult;
		Out.VMult     = TexInfo.VMult;
		Out.UPan      = TexInfo.UPan;
		Out.VPan      = TexInfo.VPan;
		return;
	}
	Out.Tex       = ( Bind && Bind->Tex ) ? Bind->Tex : NULL;
	Out.Key       = TexInfo.CurrentCacheID;
	Out.Format    = Bind ? Bind->DCFormat : 0;
	Out.Width     = Bind ? Bind->DCWidth : 0;
	Out.Height    = Bind ? Bind->DCHeight : 0;
	Out.MipMapped = Bind ? Bind->DCMipMapped : 0;
	Out.UMult     = TexInfo.UMult;
	Out.VMult     = TexInfo.VMult;
	Out.UPan      = TexInfo.UPan;
	Out.VPan      = TexInfo.VPan;
}

void UPVRRenderDevice::SetTexture( FTextureInfo& Info, DWORD PolyFlags, FLOAT PanBias )
{
	guard(UPVRRenderDevice::SetTexture);

	// Set panning.
	FTexInfo& Tex = TexInfo;
	Tex.UPan      = Info.Pan.X + PanBias*Info.UScale;
	Tex.VPan      = Info.Pan.Y + PanBias*Info.VScale;

	// Account for all the impact on scale normalization.
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
		if( Tex.CurrentBind && ( Tex.CurrentBind->Tex || IsAtlased(Tex.CurrentBind) ) )
		{
			Tex.CurrentBind->LastUsedFrame = TextureFrame;
			// UMult/UPan were just recomputed from Info, so an atlased bind
			// still needs its sub-rect folded back in.
			ApplyAtlasTransform( Tex.CurrentBind );
			return;
		}
	}

	// The low byte is the cache namespace (static lightmap, dynamic lightmap,
	// texture, fog, ...), not a replaceable texture type. Collapsing it made a
	// static lightmap and its dynamic variant evict/re-upload one another every
	// time a surface changed namespace.
	const QWORD LookupID = NewCacheID;
	const BYTE NewType = NewCacheID & 0xFF;
	FTexBind* Bind = BindMap.Find( LookupID );
	const UBOOL NewTexture = !Bind;
	if( NewTexture )
	{
		// Create new binding entry; VRAM is allocated in UploadTexture.
		Bind = BindMap.Add( LookupID, { 0, NewType, 0, 0, 0, 0, 0, 0, -1, 0, -1, 0, 0 } );
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

	if( NewTexture || ( !Bind->Tex && !IsAtlased(Bind) )
		|| RealtimeChanged || PaletteChanged || !BindPaletteValid )
	{
#if defined(PLATFORM_DREAMCAST)
		// Exclusive primary reason. New bindings are counted by UploadTexture.
		if( !NewTexture )
		{
			if( !Bind->Tex && !IsAtlased(Bind) ) DCFrameCount(DCFC_UploadMissing);
			else if( RealtimeChanged ) DCFrameCount(DCFC_UploadChanged);
			else if( PaletteChanged ) DCFrameCount(DCFC_UploadPalette);
			else if( !BindPaletteValid ) DCFrameCount(DCFC_UploadBank);
		}
		if( Info.Format == TEXF_BGRA8_LM
			|| (Info.Format == TEXF_RGB565 && Info.Mips[0] && Info.Mips[0]->DCExternalStream) )
			DCFrameCount(DCFC_UploadLightmap);
#endif
		if( Info.Format == TEXF_BGRA8_LM && Info.Mips[0] )
		{
			const DWORD Pixels = Info.Mips[0]->USize * Info.Mips[0]->VSize;
			if( RealtimeChanged )
			{
				TextureCPUProfile.DynamicLightmapCalls++;
				TextureCPUProfile.DynamicLightmapPixels += Pixels;
			}
			else
			{
				TextureCPUProfile.StaticLightmapCalls++;
				TextureCPUProfile.StaticLightmapPixels += Pixels;
				if( NewTexture )
					TextureCPUProfile.StaticLightmapCold++;
				else if( !Bind->Tex )
					TextureCPUProfile.StaticLightmapReload++;
			}
		}

		// New texture or it has changed, upload it to VRAM.
		Bind->LastType = NewType;
		Info.TextureFlags &= ~TF_RealtimeChanged;
		UploadTexture( Info, NewTexture, Masked );

		// A bind whose VRAM address just moved invalidates any header that
		// referenced it.
		PVRInvalidateHeaders();
	}

	ApplyAtlasTransform( Bind );

	unguard;
}

// Retarget the UV scale/pan from the tile onto its sub-rect of the atlas page.
//
// The per-vertex maths downstream is U = (Dot - UPan) * UMult, and we want
// U' = U*S + O for the tile's scale S and origin O within the page. Folding
// gives UMult' = UMult*S and UPan' = UPan - O/UMult', so the inner loop in
// DrawComplexSurface is untouched -- atlasing costs nothing per vertex.
void UPVRRenderDevice::ApplyAtlasTransform( const FTexBind* Bind )
{
	if( !IsAtlased( Bind ) )
		return;

	const FLOAT Inv = 1.f / (FLOAT)AtlasPageDim;
	const FLOAT US = Bind->DCWidth  * Inv;
	const FLOAT VS = Bind->DCHeight * Inv;
	const FLOAT UO = Bind->AtlasX * AtlasSlotDim * Inv;
	const FLOAT VO = Bind->AtlasY * AtlasSlotDim * Inv;

	TexInfo.UMult *= US;
	TexInfo.VMult *= VS;
	TexInfo.UPan  -= UO / TexInfo.UMult;
	TexInfo.VPan  -= VO / TexInfo.VMult;
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

	const DWORD StartCycles = appCycles();
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
	TextureCPUProfile.PaletteCalls++;
	TextureCPUProfile.PaletteCycles += appCycles() - StartCycles;
}

void UPVRRenderDevice::EnsureComposeSize( const DWORD NewSize )
{
	if( NewSize > ComposeSize )
	{
		if( Compose )
			free( Compose );
		Compose = (BYTE*)memalign( 32, NewSize );
		verify( Compose );
		debugf( "PVR: Compose size increased %d -> %d", ComposeSize, NewSize );
		ComposeSize = NewSize;
	}
}

void* UPVRRenderDevice::TwiddleTextureMipP8( const FMipmap* Mip )
{
	check(Mip);
	check(Mip->DataPtr);
	check(Mip->USize >= MinTexSize && Mip->VSize >= MinTexSize);

	const DWORD SizeBytes = Mip->USize * Mip->VSize;
	EnsureComposeSize( SizeBytes );

	// KOS' extended loader performs the required PAL8 twiddle with ordinary
	// CPU stores. Target cached system RAM first, then use pvr_txr_load() for
	// one sequential Store Queue transfer instead of scattered VRAM writes.
	const DWORD StartCycles = appCycles();
	pvr_txr_load_ex(
		Mip->DataPtr,
		(pvr_ptr_t)Compose,
		Mip->USize,
		Mip->VSize,
		PVR_TXRLOAD_8BPP );
	TextureCPUProfile.P8TwiddleCalls++;
	TextureCPUProfile.P8TwiddlePixels += SizeBytes;
	TextureCPUProfile.P8TwiddleCycles += appCycles() - StartCycles;

	return Compose;
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
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				[[fallthrough]];
			case 4:
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				[[fallthrough]];
			case 2:
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
				[[fallthrough]];
			default:
				appMemcpy( Dst, Src, SrcLine ); Dst += USize;
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
	const DWORD StartCycles = appCycles();
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
	void* Result = Compose;
	if( VTimes > 1 )
		Result = VerticalUpscale( USize, VSize, VTimes );

	TextureCPUProfile.P8ConvertCalls++;
	TextureCPUProfile.P8ConvertPixels += SrcCount;
	TextureCPUProfile.P8ConvertCycles += appCycles() - StartCycles;
	if( Mip->USize < MinTexSize || Mip->VSize < MinTexSize )
	{
		TextureCPUProfile.MinSizeExpansions++;
		TextureCPUProfile.MinSizeSourcePixels += SrcCount;
		TextureCPUProfile.MinSizeOutputPixels += Max(MinTexSize, Mip->USize)
			* Max(MinTexSize, Mip->VSize);
	}

	return Result;
}

void* UPVRRenderDevice::ConvertTextureMipBGRA7777( const FMipmap* Mip )
{
	// BGRA8888. This is actually a BGRA7777 lightmap, so we need to scale it.
	DWORD i;
	INT USize = Mip->USize;
	INT VSize = Mip->VSize;
	const FColor* Src = (const FColor*)Mip->DataPtr;
	const DWORD Count = USize * VSize;
	const DWORD StartCycles = appCycles();

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
	void* Result = Compose;
	if( VTimes > 1 )
		Result = VerticalUpscale( USize, VSize, VTimes );

	TextureCPUProfile.LightmapCalls++;
	TextureCPUProfile.LightmapPixels += Count;
	TextureCPUProfile.LightmapCycles += appCycles() - StartCycles;
	if( Mip->USize < MinTexSize || Mip->VSize < MinTexSize )
	{
		TextureCPUProfile.MinSizeExpansions++;
		TextureCPUProfile.MinSizeSourcePixels += Count;
		TextureCPUProfile.MinSizeOutputPixels += Max(MinTexSize, Mip->USize)
			* Max(MinTexSize, Mip->VSize);
	}

	return Result;
}

//
// Fog maps carry their coverage in the alpha channel, so unlike a lightmap
// they cannot go through the RGB565 path above. Four bits of alpha is ample
// for a volumetric gradient.
//
void* UPVRRenderDevice::ConvertTextureMipBGRA7777Alpha( const FMipmap* Mip )
{
	const INT USize = Mip->USize;
	const INT VSize = Mip->VSize;
	const FColor* Src = (const FColor*)Mip->DataPtr;
	const DWORD Count = USize * VSize;
	const DWORD StartCycles = appCycles();

	EnsureComposeSize( Count * sizeof(_WORD) );
	_WORD* Dst = (_WORD*)Compose;

	// Source components are 7-bit (BGRA7777); shift down to 4.
	for( DWORD i = 0; i < Count; ++i, ++Src )
	{
		*Dst++ = (_WORD)( ((Src->A & 0x78) << 9)
		                | ((Src->R & 0x78) << 5)
		                | ((Src->G & 0x78) << 1)
		                | ((Src->B & 0x78) >> 3) );
	}

	TextureCPUProfile.LightmapCalls++;
	TextureCPUProfile.LightmapPixels += Count;
	TextureCPUProfile.LightmapCycles += appCycles() - StartCycles;
	return Compose;
}

pvr_ptr_t UPVRRenderDevice::AllocateTexture( INT Size )
{
	DC_FRAME_SCOPE(DCFS_Allocate);
	pvr_ptr_t Result = pvr_mem_malloc( Size );
	if( Result )
		return Result;

	// Evict in a single sweep. The previous implementation rescanned the whole
	// bind map after every free, which is quadratic exactly when VRAM is under
	// pressure and the frame can least afford it.
	for( INT i = 0; i < BindMap.Size() && !Result; ++i )
	{
		FTexBind& Candidate = BindMap[i];
		if( !Candidate.Tex || &Candidate == TexInfo.CurrentBind
			|| Candidate.LastUsedFrame == TextureFrame )
			continue;
		pvr_mem_free( Candidate.Tex );
		if( Candidate.SizeBytes > 0 && VRAMUsed >= (DWORD)Candidate.SizeBytes )
			VRAMUsed -= (DWORD)Candidate.SizeBytes;
		Candidate.Tex = NULL;
		Candidate.SizeBytes = 0;
		Result = pvr_mem_malloc( Size );
	}

	if( !Result )
		appErrorf( "PVR frame working set exceeds VRAM: request=%i resident=%u", Size, VRAMUsed );

	// Freed VRAM may have been reused, so no cached header is trustworthy.
	PVRInvalidateHeaders();
	return Result;
}

/*------------------------------------------------------------------------------------
	Lightmap atlas.
------------------------------------------------------------------------------------*/

// KOS' twiddle interleave: bit i of the input lands at bit 2i.
static inline DWORD PVRTwiddleBits( DWORD x )
{
	x = (x | (x << 8)) & 0x00FF00FFu;
	x = (x | (x << 4)) & 0x0F0F0F0Fu;
	x = (x | (x << 2)) & 0x33333333u;
	x = (x | (x << 1)) & 0x55555555u;
	return x;
}

// Matches TWIDOUT(x,y) in KOS pvr_texture.c: y on even bits, x on odd.
static inline DWORD PVRTwiddleIndex( DWORD x, DWORD y )
{
	return PVRTwiddleBits( y ) | ( PVRTwiddleBits( x ) << 1 );
}

// Reserve a SlotW x SlotH run of slots for this tile. Tiles are placed on a
// slot grid rather than packed tightly: at 8x8 granularity the waste is at
// most a few texels and it keeps every write 128-byte aligned.
UBOOL UPVRRenderDevice::LightAtlasPlace( FTexBind* Bind, INT USize, INT VSize )
{
	DC_FRAME_SCOPE(DCFS_Place);
	if( USize > AtlasMaxTile || VSize > AtlasMaxTile )
	{
		TextureCPUProfile.AtlasOversize++;
		return 0;
	}

	const INT SlotW = ( USize + AtlasSlotDim - 1 ) / AtlasSlotDim;
	const INT SlotH = ( VSize + AtlasSlotDim - 1 ) / AtlasSlotDim;
	const DWORD Mask = ( SlotW >= 32 ) ? 0xFFFFFFFFu : ( ( 1u << SlotW ) - 1u );

	// First pass takes free space; if there is none, reclaim the slots held by
	// tiles we have not drawn this frame and try once more.
	for( INT Attempt = 0; Attempt < 2; ++Attempt )
	{
		if( Attempt && !LightAtlasReclaim( Bind ) )
			break;

		for( INT Page = 0; Page < AtlasPageMax; ++Page )
		{
			FLightAtlasPage& P = AtlasPages[Page];
			if( !P.Tex )
			{
				// Grow the pool lazily: most maps never need the second page.
				P.Tex = pvr_mem_malloc( AtlasPageDim * AtlasPageDim * sizeof(_WORD) );
				if( !P.Tex )
					break;
				appMemset( P.Rows, 0, sizeof(P.Rows) );
				P.SlotsUsed = 0;
				VRAMUsed += AtlasPageDim * AtlasPageDim * sizeof(_WORD);
				AtlasPageCount = Max( AtlasPageCount, Page + 1 );
				debugf( NAME_Log, "PVR: lightmap atlas page %i allocated (%i KB)",
					Page, (INT)(AtlasPageDim * AtlasPageDim * sizeof(_WORD) / 1024) );
			}

			// Tiles must sit on a multiple of their own size for the twiddled
			// run to stay contiguous, so step the search by SlotW/SlotH.
			for( INT Y = 0; Y + SlotH <= AtlasSlots; Y += SlotH )
			{
				for( INT X = 0; X + SlotW <= AtlasSlots; X += SlotW )
				{
					INT Row = 0;
					for( ; Row < SlotH; ++Row )
						if( P.Rows[Y + Row] & ( Mask << X ) )
							break;
					if( Row != SlotH )
						continue;
					for( Row = 0; Row < SlotH; ++Row )
						P.Rows[Y + Row] |= ( Mask << X );
					P.SlotsUsed += SlotW * SlotH;
					Bind->AtlasPage = Page;
					Bind->AtlasX = X;
					Bind->AtlasY = Y;
					TextureCPUProfile.AtlasInserts++;
					return 1;
				}
			}
		}
	}

	TextureCPUProfile.AtlasNoSpace++;
	return 0;
}

// Drop every atlas tile that has not been drawn this frame. Releasing a bind
// is all it takes to make SetTexture upload it again when it is next visible,
// so this is safe to do at any point. Without it the pages fill once and the
// atlas silently stops accepting tiles for the rest of the level.
INT UPVRRenderDevice::LightAtlasReclaim( const FTexBind* Keep )
{
	INT Reclaimed = 0;
	for( INT i = 0; i < BindMap.Size(); ++i )
	{
		FTexBind& Candidate = BindMap[i];
		if( !IsAtlased( &Candidate ) || &Candidate == Keep
			|| &Candidate == TexInfo.CurrentBind
			|| Candidate.LastUsedFrame == TextureFrame )
			continue;
		LightAtlasRelease( &Candidate );
		++Reclaimed;
	}
	TextureCPUProfile.AtlasEvictions += Reclaimed;
#if defined(PLATFORM_DREAMCAST)
	DCFrameCount(DCFC_Evict, Reclaimed);
#endif
	return Reclaimed;
}

// Scatter a linear RGB565 tile into the page, one 8x8 twiddled block at a
// time. Each block is a contiguous 128 bytes in the page, so this is a
// sequence of small store-queue transfers with no read-modify-write.
void UPVRRenderDevice::LightAtlasStore(
	const FTexBind* Bind, INT USize, INT VSize, const _WORD* Pixels )
{
	check( Bind && Bind->AtlasPage >= 0 );
	DC_FRAME_SCOPE(DCFS_Twiddle);
	const FLightAtlasPage& P = AtlasPages[Bind->AtlasPage];
	const INT BlocksX = ( USize + AtlasSlotDim - 1 ) / AtlasSlotDim;
	const INT BlocksY = ( VSize + AtlasSlotDim - 1 ) / AtlasSlotDim;

	// pvr_txr_load reinterprets the source as uint32_t*, so a bare _WORD array
	// is under-aligned; match the store queue's granularity while we are here.
	alignas(32) _WORD Block[AtlasSlotDim * AtlasSlotDim];
	for( INT By = 0; By < BlocksY; ++By )
	{
		for( INT Bx = 0; Bx < BlocksX; ++Bx )
		{
			// Twiddle this block in cached RAM first.
			for( INT dy = 0; dy < AtlasSlotDim; ++dy )
			{
				const INT SrcY = By * AtlasSlotDim + dy;
				const _WORD* Src = Pixels + SrcY * USize + Bx * AtlasSlotDim;
				for( INT dx = 0; dx < AtlasSlotDim; ++dx )
					Block[PVRTwiddleIndex( dx, dy )] = Src[dx];
			}
			const DWORD Slot = PVRTwiddleIndex(
				Bind->AtlasX + Bx, Bind->AtlasY + By );
			pvr_txr_load( Block,
				(pvr_ptr_t)( (BYTE*)P.Tex + Slot * sizeof(Block) ),
				sizeof(Block) );
			TextureCPUProfile.AtlasBlocks++;
		}
	}
}

void UPVRRenderDevice::LightAtlasRelease( FTexBind* Bind )
{
	if( !IsAtlased( Bind ) )
		return;
	FLightAtlasPage& P = AtlasPages[Bind->AtlasPage];
	const INT SlotW = ( Bind->DCWidth  + AtlasSlotDim - 1 ) / AtlasSlotDim;
	const INT SlotH = ( Bind->DCHeight + AtlasSlotDim - 1 ) / AtlasSlotDim;
	const DWORD Mask = ( SlotW >= 32 ) ? 0xFFFFFFFFu : ( ( 1u << SlotW ) - 1u );
	for( INT Row = 0; Row < SlotH; ++Row )
		P.Rows[Bind->AtlasY + Row] &= ~( Mask << Bind->AtlasX );
	P.SlotsUsed -= SlotW * SlotH;
	Bind->AtlasPage = -1;
}

void UPVRRenderDevice::LightAtlasFlush()
{
	for( INT i = 0; i < AtlasPageMax; ++i )
	{
		if( AtlasPages[i].Tex )
		{
			pvr_mem_free( AtlasPages[i].Tex );
			const DWORD Bytes = AtlasPageDim * AtlasPageDim * sizeof(_WORD);
			if( VRAMUsed >= Bytes )
				VRAMUsed -= Bytes;
		}
		AtlasPages[i].Tex = NULL;
		appMemset( AtlasPages[i].Rows, 0, sizeof(AtlasPages[i].Rows) );
		AtlasPages[i].SlotsUsed = 0;
	}
	AtlasPageCount = 0;
}

void UPVRRenderDevice::UploadTexture( FTextureInfo& Info, UBOOL NewTexture, UBOOL Masked )
{
	guard(UPVRRenderDevice::UploadTexture);
	DC_FRAME_SCOPE(DCFS_Texture);
#if defined(PLATFORM_DREAMCAST)
	DCFrameCount(NewTexture ? DCFC_Cold : DCFC_Reload);
#endif

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
			{
				DC_FRAME_SCOPE(DCFS_Read);
				Mip0->StreamData.Read( DCLoaded );
			}
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

		const INT PayloadSize = Size - HeaderSize;
		if( Bind->Tex && Bind->SizeBytes != PayloadSize )
		{
			pvr_mem_free( Bind->Tex );
			VRAMUsed -= Bind->SizeBytes;
			Bind->Tex = NULL;
			Bind->SizeBytes = 0;
		}
		if( !Bind->Tex )
		{
			Bind->SizeBytes = PayloadSize;
			Bind->Tex = AllocateTexture( Bind->SizeBytes );
			VRAMUsed += Bind->SizeBytes;
		}
		if( !Bind->Tex )
		{
			appErrorf( "DT VRAM allocation failed: %i bytes", Bind->SizeBytes );
		}
		pvr_txr_load( Data + HeaderSize, Bind->Tex, Bind->SizeBytes );
		Bind->DCFormat = Mode & 0x7e000000;
		Bind->DCWidth = Width;
		Bind->DCHeight = Height;
		Bind->DCMipMapped = (Mode & 0x80000000) != 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;
	}
	else if( Info.Format == TEXF_RGB565 && Mip0->DCExternalStream )
	{
		const DWORD StartCycles = appCycles();
		const INT SizeBytes = Mip0->DCExternalSize;
		if( SizeBytes != Mip0->USize * Mip0->VSize * 2 )
		{
			appErrorf( "Invalid cooked lightmap size: %i", SizeBytes );
		}
		if( NewTexture )
			TextureCPUProfile.CookedLightmapCold++;
		else
			TextureCPUProfile.CookedLightmapReload++;
		TextureCPUProfile.CookedLightmapBytes += SizeBytes;
		BYTE* Pixels = (BYTE*)appMalloc( SizeBytes, "DCLightmapUpload" );
		if( Mip0->DCExternalCodec == 0 )
		{
			if( Mip0->DCExternalPackedSize != SizeBytes )
				appErrorf( "Invalid raw cooked lightmap size" );
			{
				DC_FRAME_SCOPE(DCFS_Read);
				Mip0->DCExternalStream->ReadRange(Mip0->DCExternalOffset, Pixels, SizeBytes);
			}
		}
		else if( Mip0->DCExternalCodec == 1 )
		{
			const INT PackedSize = Mip0->DCExternalPackedSize;
			BYTE* Packed = (BYTE*)appMalloc( PackedSize, "DCLightmapPacked" );
			{
				DC_FRAME_SCOPE(DCFS_Read);
				Mip0->DCExternalStream->ReadRange(Mip0->DCExternalOffset, Packed, PackedSize);
			}
			uLongf OutputSize = SizeBytes;
			INT Result;
			{
				DC_FRAME_SCOPE(DCFS_Inflate);
				Result = uncompress( Pixels, &OutputSize, Packed, PackedSize );
			}
			appFree( Packed );
			if( Result != Z_OK || OutputSize != (uLongf)SizeBytes )
				appErrorf( "Invalid compressed cooked lightmap" );
		}
		else
		{
			appErrorf( "Unsupported cooked lightmap codec" );
		}
		// Release against the dimensions the slots were reserved with, before
		// the new ones overwrite them.
		LightAtlasRelease( Bind );

		Bind->DCWidth = Mip0->USize;
		Bind->DCHeight = Mip0->VSize;
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;

		// Prefer a shared page. Every tile that lands in one is a texture
		// header the translucent pass no longer has to emit.
		if( LightAtlasPlace( Bind, Mip0->USize, Mip0->VSize ) )
		{
			if( Bind->Tex )
			{
				pvr_mem_free( Bind->Tex );
				if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
					VRAMUsed -= (DWORD)Bind->SizeBytes;
				Bind->Tex = NULL;
				Bind->SizeBytes = 0;
			}
			LightAtlasStore( Bind, Mip0->USize, Mip0->VSize, (const _WORD*)Pixels );
			Bind->DCFormat = PVR_TXRFMT_RGB565;
		}
		else
		{
			if( Bind->Tex && Bind->SizeBytes != SizeBytes )
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
			pvr_txr_load( Pixels, Bind->Tex, SizeBytes );
			Bind->DCFormat = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED;
		}
		appFree( Pixels );
		TextureCPUProfile.CookedLightmapCycles += appCycles() - StartCycles;
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
				void* Twiddled = TwiddleTextureMipP8( Mip0 );
				pvr_txr_load( Twiddled, Bind->Tex, SizeBytes );
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
			TextureCPUProfile.PaletteBankFallbacks++;
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
		TextureCPUProfile.GenericP8Conversions++;
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
	else if( (Info.CacheID & 0xFF) == CID_RenderFogMap
		&& Mip0->USize >= MinTexSize && Mip0->VSize >= MinTexSize )
	{
		// Volumetric fog map: ARGB4444 so the coverage alpha survives.
		void* Lin = ConvertTextureMipBGRA7777Alpha( Mip0 );
		const INT SizeBytes = Mip0->USize * Mip0->VSize * 2;
		if( Bind->Tex && Bind->SizeBytes != SizeBytes )
		{
			pvr_mem_free( Bind->Tex );
			if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
				VRAMUsed -= (DWORD)Bind->SizeBytes;
			Bind->Tex = NULL;
			Bind->SizeBytes = 0;
		}
		if( !Bind->Tex )
		{
			Bind->Tex = AllocateTexture( SizeBytes );
			Bind->SizeBytes = SizeBytes;
			VRAMUsed += SizeBytes;
		}
		pvr_txr_load( Lin, Bind->Tex, SizeBytes );
		Bind->DCFormat = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
		Bind->DCWidth = Mip0->USize;
		Bind->DCHeight = Mip0->VSize;
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;
	}
	else
	{
		// Dynamic lightmaps: convert to RGB565 (Compose) then upload.
		void* Lin = ConvertTextureMipBGRA7777( Mip0 );
		const INT USize = Max( MinTexSize, Mip0->USize );
		const INT VSize = Max( MinTexSize, Mip0->VSize );
		const INT SizeBytes = USize * VSize * 2;

		if( IsAtlased( Bind )
			&& ( Bind->DCWidth != USize || Bind->DCHeight != VSize ) )
		{
			LightAtlasRelease( Bind );
		}
		const UBOOL KeptSlot = IsAtlased( Bind );

		Bind->DCWidth = USize;
		Bind->DCHeight = VSize;
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;

		if( KeptSlot || LightAtlasPlace( Bind, USize, VSize ) )
		{
			if( Bind->Tex )
			{
				pvr_mem_free( Bind->Tex );
				if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
					VRAMUsed -= (DWORD)Bind->SizeBytes;
				Bind->Tex = NULL;
				Bind->SizeBytes = 0;
			}
			LightAtlasStore( Bind, USize, VSize, (const _WORD*)Lin );
			if( KeptSlot )
				TextureCPUProfile.AtlasUpdates++;
			Bind->DCFormat = PVR_TXRFMT_RGB565;
		}
		else
		{
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

void UPVRRenderDevice::PrintTextureCPUProfile( INT Frames )
{
	const DOUBLE ToMilliseconds = GSecondsPerCycle * 1000.0;
	const DOUBLE TotalMilliseconds =
		(TextureCPUProfile.PaletteCycles
		+ TextureCPUProfile.P8TwiddleCycles
		+ TextureCPUProfile.P8ConvertCycles
		+ TextureCPUProfile.LightmapCycles) * ToMilliseconds;
	debugf(
		"DCTEXCPU frames=%d frame_ms=%.3f "
		"palette=%u/%.3fms twiddle=%u/%u/%.3fms "
		"p8_expand=%u/%u/%.3fms bank_fallback=%u generic_p8=%u "
		"min8=%u/%u/%u lightmap=%u/%u/%.3fms dynamic=%u/%u "
		"static=%u/%u/%u/%u/%u",
		Frames, Frames ? TotalMilliseconds / Frames : 0.0,
		TextureCPUProfile.PaletteCalls,
		TextureCPUProfile.PaletteCycles * ToMilliseconds,
		TextureCPUProfile.P8TwiddleCalls, TextureCPUProfile.P8TwiddlePixels,
		TextureCPUProfile.P8TwiddleCycles * ToMilliseconds,
		TextureCPUProfile.P8ConvertCalls, TextureCPUProfile.P8ConvertPixels,
		TextureCPUProfile.P8ConvertCycles * ToMilliseconds,
		TextureCPUProfile.PaletteBankFallbacks,
		TextureCPUProfile.GenericP8Conversions,
		TextureCPUProfile.MinSizeExpansions,
		TextureCPUProfile.MinSizeSourcePixels,
		TextureCPUProfile.MinSizeOutputPixels,
		TextureCPUProfile.LightmapCalls, TextureCPUProfile.LightmapPixels,
		TextureCPUProfile.LightmapCycles * ToMilliseconds,
		TextureCPUProfile.DynamicLightmapCalls,
		TextureCPUProfile.DynamicLightmapPixels,
		TextureCPUProfile.StaticLightmapCalls,
		TextureCPUProfile.StaticLightmapPixels,
		TextureCPUProfile.StaticLightmapCold,
		TextureCPUProfile.StaticLightmapReload,
		TextureCPUProfile.StaticLightmapRetype );
	debugf(
		"DCLMATLAS cooked=%u/%u/%uKB/%.3fms atlas=%u/%u/%u pages=%i/%i "
		"evict=%u reject=%u/%u",
		TextureCPUProfile.CookedLightmapCold,
		TextureCPUProfile.CookedLightmapReload,
		TextureCPUProfile.CookedLightmapBytes / 1024,
		TextureCPUProfile.CookedLightmapCycles * ToMilliseconds,
		TextureCPUProfile.AtlasInserts,
		TextureCPUProfile.AtlasUpdates,
		TextureCPUProfile.AtlasBlocks,
		AtlasPageCount,
		AtlasPageCount ? AtlasPages[0].SlotsUsed : 0,
		TextureCPUProfile.AtlasEvictions,
		TextureCPUProfile.AtlasOversize,
		TextureCPUProfile.AtlasNoSpace );
	appMemset( &TextureCPUProfile, 0, sizeof(TextureCPUProfile) );
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
