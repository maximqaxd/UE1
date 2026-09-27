/*=============================================================================
	PVRDrv.cpp: Unreal PowerVR (Dreamcast) render device.

	The engine replays one occluded scene in OP, PT, TR passes. Each pass writes
	its 32-byte headers and vertices directly to the active KOS TA list. The
	TR list stays open for the flash and HUD after DrawWorld returns.
=============================================================================*/

#include <kos.h>
#include <malloc.h>
#include <sh4zam/shz_sh4zam.h>

#include "PVRDrvPrivate.h"
#include "UnDCFrameProfile.h"

// KOS' texture heap is a general-purpose malloc heap. Its available-byte
// count is not the size of its largest hole, and repeated failed allocations
// are particularly costly during animation. Reserve its texture region once
// and suballocate it in 256-byte units, keeping large images 2 KiB aligned.
// This follows the layout idea of xash3d_dc's pvr_alloc without relocating
// live textures: TA headers may still refer to their current VRAM addresses.
namespace
{
	static const DWORD PVRPoolUnit = 256;
	static const INT PVRPoolMaxRanges = 2048;
	struct FPVRPoolRange { DWORD Start, Count; };
	struct FPVRPoolAllocation { pvr_ptr_t Ptr; DWORD Count; };
	static pvr_ptr_t GPVRPoolRaw = NULL;
	static BYTE* GPVRPoolBase = NULL;
	static DWORD GPVRPoolUnits = 0;
	static DWORD GPVRPoolFreeUnits = 0;
	static FPVRPoolRange GPVRPoolFree[PVRPoolMaxRanges];
	static FPVRPoolAllocation GPVRPoolAlloc[PVRPoolMaxRanges];
	static INT GPVRPoolFreeCount = 0;
	static INT GPVRPoolAllocCount = 0;

	static void PVRPoolInit()
	{
		if( GPVRPoolRaw )
			return;
		const size_t Available = pvr_mem_available();
		// Leave dlmalloc metadata and a small amount of VRAM outside the pool.
		if( Available <= 128 * 1024 )
			appErrorf( "PVR texture pool has only %u bytes", (unsigned)Available );
		const size_t Requested = Available - 64 * 1024;
		GPVRPoolRaw = pvr_mem_malloc( Requested );
		if( !GPVRPoolRaw )
			appErrorf( "PVR texture pool reservation failed: %u bytes", (unsigned)Requested );
		const uintptr_t Aligned = ((uintptr_t)GPVRPoolRaw + 2047u) & ~(uintptr_t)2047u;
		GPVRPoolBase = (BYTE*)Aligned;
		GPVRPoolUnits = (DWORD)((Requested - (Aligned - (uintptr_t)GPVRPoolRaw)) / PVRPoolUnit);
		GPVRPoolFreeUnits = GPVRPoolUnits;
		GPVRPoolFree[0] = { 0, GPVRPoolUnits };
		GPVRPoolFreeCount = 1;
		GPVRPoolAllocCount = 0;
		debugf( NAME_Log, "PVRPOOL init capacity=%uKB raw=%p base=%p", GPVRPoolUnits / 4,
			GPVRPoolRaw, GPVRPoolBase );
	}

	static void PVRPoolShutdown()
	{
		if( !GPVRPoolRaw )
			return;
		if( GPVRPoolAllocCount )
			debugf( NAME_Warning, "PVRPOOL shutdown with %i live allocations", GPVRPoolAllocCount );
		pvr_mem_free( GPVRPoolRaw );
		GPVRPoolRaw = NULL;
		GPVRPoolBase = NULL;
		GPVRPoolUnits = GPVRPoolFreeUnits = 0;
		GPVRPoolFreeCount = GPVRPoolAllocCount = 0;
	}

	static size_t PVRPoolAvailable() { return (size_t)GPVRPoolFreeUnits * PVRPoolUnit; }
	static size_t PVRPoolLargest()
	{
		DWORD Largest = 0;
		for( INT i=0; i<GPVRPoolFreeCount; ++i )
			Largest = Max( Largest, GPVRPoolFree[i].Count );
		return (size_t)Largest * PVRPoolUnit;
	}
	static void PVRPoolStats()
	{
		debugf( NAME_Log, "PVRPOOL capacity=%uKB free=%uKB largest=%uKB ranges=%i allocations=%i",
			GPVRPoolUnits / 4, GPVRPoolFreeUnits / 4,
			(unsigned)(PVRPoolLargest() / 1024), GPVRPoolFreeCount, GPVRPoolAllocCount );
	}

	static UBOOL PVRPoolFind( size_t Bytes, INT& FoundIndex, DWORD& FoundStart )
	{
		if( !Bytes || !GPVRPoolBase || GPVRPoolAllocCount == PVRPoolMaxRanges )
			return 0;
		const DWORD Units = (DWORD)((Bytes + PVRPoolUnit - 1) / PVRPoolUnit);
		const UBOOL Small = Bytes < 2048;
		// Preferred packing first; then permit any 256-byte-aligned hole. The
		// hardware does not require 2 KiB alignment for these texture pointers.
		for( INT Pass=0; Pass<2; ++Pass )
		for( INT Step=0; Step<GPVRPoolFreeCount; ++Step )
		{
			const INT i = Small ? GPVRPoolFreeCount-1-Step : Step;
			const FPVRPoolRange Range = GPVRPoolFree[i];
			DWORD Start;
			if( Small )
			{
				DWORD End = Range.Start + Range.Count;
				if( End < Units ) continue;
				Start = End - Units;
				if( Pass == 0 && Start / 8 != (End - 1) / 8 )
				{
					End &= ~7u;
					if( End < Units ) continue;
					Start = End - Units;
				}
			}
			else Start = Pass == 0 ? (Range.Start + 7) & ~7u : Range.Start;
			if( Start < Range.Start ) continue;
			const DWORD Prefix = Start - Range.Start;
			if( Prefix + Units > Range.Count )
				continue;
			const DWORD Suffix = Range.Count - Prefix - Units;
			if( Prefix && Suffix && GPVRPoolFreeCount == PVRPoolMaxRanges )
				continue;
			FoundIndex = i;
			FoundStart = Start;
			return 1;
		}
		return 0;
	}

	static UBOOL PVRPoolCanAlloc( size_t Bytes )
	{
		INT Index;
		DWORD Start;
		return PVRPoolFind( Bytes, Index, Start );
	}

	static pvr_ptr_t PVRPoolMalloc( size_t Bytes )
	{
		INT i;
		DWORD Start;
		if( !PVRPoolFind( Bytes, i, Start ) )
			return NULL;
		const DWORD Units = (DWORD)((Bytes + PVRPoolUnit - 1) / PVRPoolUnit);
		const FPVRPoolRange Range = GPVRPoolFree[i];
		const DWORD Prefix = Start - Range.Start;
		const DWORD Suffix = Range.Count - Prefix - Units;
		if( Prefix && Suffix )
		{
			for( INT j=GPVRPoolFreeCount; j>i+1; --j ) GPVRPoolFree[j] = GPVRPoolFree[j-1];
			GPVRPoolFree[i].Count = Prefix;
			GPVRPoolFree[i+1] = { Start + Units, Suffix };
			++GPVRPoolFreeCount;
		}
		else if( Prefix ) GPVRPoolFree[i].Count = Prefix;
		else if( Suffix ) { GPVRPoolFree[i].Start = Start + Units; GPVRPoolFree[i].Count = Suffix; }
		else
		{
			for( INT j=i; j+1<GPVRPoolFreeCount; ++j ) GPVRPoolFree[j] = GPVRPoolFree[j+1];
			--GPVRPoolFreeCount;
		}
		pvr_ptr_t Ptr = (pvr_ptr_t)(GPVRPoolBase + (size_t)Start * PVRPoolUnit);
		GPVRPoolAlloc[GPVRPoolAllocCount++] = { Ptr, Units };
		GPVRPoolFreeUnits -= Units;
		return Ptr;
	}

	static void PVRPoolFree( pvr_ptr_t Ptr )
	{
		if( !Ptr ) return;
		INT Allocation = 0;
		for( ; Allocation<GPVRPoolAllocCount; ++Allocation )
			if( GPVRPoolAlloc[Allocation].Ptr == Ptr ) break;
		if( Allocation == GPVRPoolAllocCount )
			appErrorf( "PVRPOOL free of unknown texture %p", Ptr );
		const DWORD Start = (DWORD)(((BYTE*)Ptr - GPVRPoolBase) / PVRPoolUnit);
		const DWORD Units = GPVRPoolAlloc[Allocation].Count;
		GPVRPoolAlloc[Allocation] = GPVRPoolAlloc[--GPVRPoolAllocCount];
		INT At = 0;
		while( At<GPVRPoolFreeCount && GPVRPoolFree[At].Start<Start ) ++At;
		const UBOOL JoinLeft = At>0 && GPVRPoolFree[At-1].Start + GPVRPoolFree[At-1].Count == Start;
		const UBOOL JoinRight = At<GPVRPoolFreeCount && Start + Units == GPVRPoolFree[At].Start;
		if( JoinLeft && JoinRight )
		{
			GPVRPoolFree[At-1].Count += Units + GPVRPoolFree[At].Count;
			for( INT j=At; j+1<GPVRPoolFreeCount; ++j ) GPVRPoolFree[j] = GPVRPoolFree[j+1];
			--GPVRPoolFreeCount;
		}
		else if( JoinLeft ) GPVRPoolFree[At-1].Count += Units;
		else if( JoinRight ) { GPVRPoolFree[At].Start = Start; GPVRPoolFree[At].Count += Units; }
		else
		{
			if( GPVRPoolFreeCount == PVRPoolMaxRanges ) appErrorf( "PVRPOOL free range table full" );
			for( INT j=GPVRPoolFreeCount; j>At; --j ) GPVRPoolFree[j] = GPVRPoolFree[j-1];
			GPVRPoolFree[At] = { Start, Units };
			++GPVRPoolFreeCount;
		}
		GPVRPoolFreeUnits += Units;
	}
}

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
	Direct TA submission.
-----------------------------------------------------------------------------*/

static pvr_list_t     GPVRDirectList = (pvr_list_t)-1;
static INT            GPVRCurrentPass = -1;
static pvr_dr_state_t GPVRDRState;

static void PVRStartList( pvr_list_t List )
{
	check( GPVRDirectList == (pvr_list_t)-1 );
	const INT Result = pvr_list_begin(List);
	check( Result == 0 );
	GPVRDirectList = List;
}

static void PVRFinishList()
{
	check( GPVRDirectList != (pvr_list_t)-1 );
	const INT Result = pvr_list_finish();
	check( Result == 0 );
	GPVRDirectList = (pvr_list_t)-1;
}

static inline UBOOL PVRBeginDraw( pvr_list_t List, DWORD MaxBytes )
{
	return List == GPVRDirectList;
}

static inline void PVRMirrorWrite(const void* Data)
{
	DWORD* Out=(DWORD*)pvr_dr_target(GPVRDRState);
	const DWORD* In=(const DWORD*)Data;
	for( INT i=0; i<8; ++i ) Out[i]=In[i];
	pvr_dr_commit(Out);
}

#include "PVRMirrorClip.h"

static inline void* PVRAlloc32( pvr_list_t List )
{
	check( List == GPVRDirectList );
	if( GPVRMirrorFrame ) return &GPVRMirrorScratch;
	return pvr_dr_target( GPVRDRState );
}

static inline void PVRCommit32( pvr_list_t List, void* Dst )
{
	check( List == GPVRDirectList );
	if( GPVRMirrorFrame )
	{
		const DWORD Command=*(const DWORD*)Dst;
		if( (Command & 0xE0000000u)==PVR_CMD_VERTEX )
			PVRMirrorVertex(*(const pvr_vertex_t*)Dst);
		else PVRMirrorWrite(Dst);
		return;
	}
	pvr_dr_commit( Dst );
}

/*-----------------------------------------------------------------------------
	Header cache.
-----------------------------------------------------------------------------*/

//
// Keep the last hardware state for each list. Texture identity alone is not
// enough: an upload may change its address, format or dimensions, while an
// in-place atlas update changes none of those header fields.
//
struct FPVRHeaderCache
{
	UBOOL Valid;
	DWORD PolyFlags;
	UBOOL NoDepth;
	UBOOL FilteringDisabled;
	UBOOL FogEnabled;
	UBOOL OverbrightEnabled;
	pvr_cull_mode_t Cull;
	QWORD TextureKey;
	pvr_ptr_t Texture;
	DWORD TextureFormat;
	INT TextureWidth;
	INT TextureHeight;
	UBOOL TextureMipMapped;
};
static FPVRHeaderCache GPVRHeaderCache[PVR_LIST_PT_POLY + 1];
struct FPVRCompiledHeader
{
	FPVRHeaderCache State;
	pvr_list_t List;
	pvr_poly_hdr_t Header;
};
static FPVRCompiledHeader GPVRCompiledHeaders[64];

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
	new(Class, "UseVQDynamicLightmaps", RF_Public)UBoolProperty( CPP_PROPERTY(UseVQDynamicLightmaps), "Options", CPF_Config );
	new(Class, "DistanceFog",     RF_Public)UBoolProperty( CPP_PROPERTY(DistanceFog),     "Options", CPF_Config );
	new(Class, "Overbright",      RF_Public)UBoolProperty( CPP_PROPERTY(Overbright),      "Options", CPF_Config );
	new(Class, "VolumetricFog",   RF_Public)UBoolProperty( CPP_PROPERTY(VolumetricFog),   "Options", CPF_Config );
	new(Class, "FogDistanceDefault", RF_Public)UIntProperty( CPP_PROPERTY(FogDistanceDefault), "Options", CPF_Config );
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
	ShinySurfaces = false;
	UseVQDynamicLightmaps = true;
	// CPU culling avoids lighting and submitting hidden faces. The PVR's
	// backface test occurs only after those costs have already been paid.
	UseHardwareMeshCull = false;
	MeshDrawScope = false;
	DistanceFog = false;
	Overbright = true;
	VolumetricFog = false;
	FogDistanceDefault = 0;
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
	AtlasDynamicFirstPage = 0;
	AtlasPreloadActive = false;
	TexturePreloadActive = false;
	PreloadedLightmapLevel = NULL;
	appMemset( &PreloadedLightmapBind, 0, sizeof(PreloadedLightmapBind) );
}

UBOOL UPVRRenderDevice::Init( UViewport* InViewport )
{
	guard(UPVRRenderDevice::Init)
	WorldLightCount=WorldLightVertexCount=0; WorldLightFrame=NULL;
	appMemset(GPVRCompiledHeaders,0,sizeof(GPVRCompiledHeaders));

#if defined(PLATFORM_DREAMCAST)
	if( !GPVRSessionInitialized )
	{
		if( pvr_init(&GPVRInitParams) < 0 )
			appErrorf("PVR initialization failed");
		GPVRSessionInitialized = 1;
	}
	PVRPoolInit();
#else
	pvr_init(&GPVRInitParams);
	PVRPoolInit();
#endif

	// Reserve a far reciprocal-depth range for sky BSP. KOS defaults to a
	// 0.0001 background depth, which would reject the scaled sky vertices.
	pvr_set_zclip(1.0e-36f);

	// Volumetric fog costs a third translucent pass per surface plus the
	// per-texel Volumetric() loop in FLightManager, so it is opt-in.
	SupportsFogMaps     = VolumetricFog ? 1 : 0;
	SupportsDistanceFog = false;
	NoVolumetricBlend   = true;
	SupportsTriStrips   = UseTriStrips ? 1 : 0;
	GDCUseVQDynamicLightmaps = UseVQDynamicLightmaps;

	debugf( NAME_Log, "PVR command submission: direct OP/PT/TR" );
	debugf( NAME_Log, "PVR mesh hardware culling: %s", UseHardwareMeshCull ? "enabled" : "disabled" );
	debugf( NAME_Log, "PVR options: tristrips=%i vqdyn=%i distancefog=%i fogdefault=%i volumetricfog=%i"
		" shiny=%i volumetriclighting=%i coronas=%i filtering=%i overbright=%i",
		(INT)UseTriStrips, (INT)UseVQDynamicLightmaps, (INT)DistanceFog, FogDistanceDefault, (INT)VolumetricFog,
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

	GPVRDeviceInstance = NULL;
#if defined(PLATFORM_DREAMCAST)
	// Level travel rebuilds the UObject graph, but the KOS PVR subsystem owns
	// process-wide command buffers and interrupt state. Keep it alive across
	// sessions; Flush above has released all renderer-owned textures.
	if( !appDCHasSessionTravel() )
	{
		PVRPoolShutdown();
		pvr_shutdown();
		GPVRSessionInitialized = 0;
	}
#endif

	unguard;
}

void UPVRRenderDevice::Flush()
{
	guard(UPVRRenderDevice::Flush);
	WorldLightCount=WorldLightVertexCount=0; WorldLightFrame=NULL;
	appMemset(GPVRCompiledHeaders,0,sizeof(GPVRCompiledHeaders));

	ResetTexture();
	PVRInvalidateHeaders();
	PreloadedLightmapLevel = NULL;
	PreloadedLightmaps.Empty();
	AtlasPreloadActive = false;
	TexturePreloadActive = false;

	if( BindMap.Size() )
	{
		debugf( NAME_Log, "Flushing %d textures", BindMap.Size() );
		for( INT i = 0; i < BindMap.Size(); ++i )
		{
			if( BindMap[i].Tex )
			{
				PVRPoolFree( BindMap[i].Tex );
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
#if DC_FRAME_PROFILE
		GDCFrameProfileDetailed = !GDCFrameProfileDetailed;
		DCFrameProfileReset();
		Out->Logf("Detailed profiling %s; wait 30 ticks", GDCFrameProfileDetailed ? "on" : "off");
#else
		DCFrameProfileReport(Out);
#endif
		return true;
	}
	if( ParseCommand(&Cmd, "DCPOVERLAY") )
	{
#if DC_FRAME_PROFILE && DC_PROFILE_OVERLAY
		GDCFrameProfileOverlay = !GDCFrameProfileOverlay;
		DCFrameProfileReset();
		Out->Logf("Profile overlay %s; wait 30 ticks", GDCFrameProfileOverlay ? "on" : "off");
#else
		Out->Logf("Profile overlay compiled out (DREAMCAST_PROFILE_OVERLAY=OFF)");
#endif
		return true;
	}
	if( ParseCommand(&Cmd, "DCPPAGE") )
	{
		GDCFrameProfilePage = (GDCFrameProfilePage + 1) % 4;
		return true;
	}
	if( ParseCommand(&Cmd, "DCPDUMP") )
	{
		DCFrameProfileReport(Out);
		return true;
	}
	if( ParseCommand(&Cmd, "DCSPANMODE") )
	{
		if( *Cmd >= '0' && *Cmd <= '2' )
			GDCSpanMode = *Cmd - '0';
		else
			GDCSpanMode = (GDCSpanMode + 1) % 3;
		DCFrameProfileReset();
		Out->Logf( "BSP span mode %d: %s", GDCSpanMode,
			GDCSpanMode == 0 ? "original" : GDCSpanMode == 1
				? "update visibility without output fragments"
				: "DIAGNOSTIC opaque occlusion bypass; portals may be wrong" );
		return true;
	}
	if( ParseCommand(&Cmd, "DCMESHHCULL") )
	{
		UseHardwareMeshCull = !UseHardwareMeshCull;
		PVRInvalidateHeaders();
		DCFrameProfileReset();
		Out->Logf( "Mesh PVR backface culling %s", UseHardwareMeshCull ? "on" : "off" );
		return true;
	}
	if( ParseCommand(&Cmd, "DCMESHOPT") )
	{
		GDCMeshOptimize = (*Cmd=='0' || *Cmd=='1') ? *Cmd=='1' : !GDCMeshOptimize;
		DCFrameProfileReset();
		Out->Logf("Mesh pass/pose reuse %s", GDCMeshOptimize ? "on" : "off");
		return true;
	}
	if( ParseCommand(&Cmd, "DCLIGHTRATE") )
	{
		const INT Requested = appAtoi(Cmd);
		if( Requested == 5 || Requested == 10 || Requested == 15 )
			GDCStationaryLightHz = Requested;
		else
			GDCStationaryLightHz = GDCStationaryLightHz == 5 ? 10
				: GDCStationaryLightHz == 10 ? 15 : 5;
		DCFrameProfileReset();
		Out->Logf("Stationary dynamic lightmaps: %d Hz; moving lights: every frame",
			GDCStationaryLightHz);
		return true;
	}
	if( ParseCommand(&Cmd, "DCLEGACYTIMERS") )
	{
#if DC_FRAME_PROFILE
		GDCLegacyTimers = !GDCLegacyTimers;
		Out->Logf("Legacy timers %s", GDCLegacyTimers ? "on" : "off");
#else
		DCFrameProfileReport(Out);
#endif
		return true;
	}
#endif
#if defined(PLATFORM_DREAMCAST)
	if( ParseCommand(&Cmd, "DCPROFILE") )
	{
#if DC_FRAME_PROFILE
		GDCFrameProfileEnabled = !GDCFrameProfileEnabled;
		Out->Logf("Frame profile %s", GDCFrameProfileEnabled ? "on" : "off");
#else
		DCFrameProfileReport(Out);
#endif
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
	PVRMirrorSetFrame(NULL);
	MeshDrawScope = false;

	++TextureFrame;
#if defined(PLATFORM_DREAMCAST)
#if DC_FRAME_PROFILE
	pvr_stats_t ProfileStats;
	if( pvr_get_stats(&ProfileStats) == 0 )
		DCFrameGPU(ProfileStats.frame_count, ProfileStats.rnd_last_time, ProfileStats.vtx_buffer_used);
#endif
#endif

	PVRInvalidateHeaders();
	UIZCursor = 0.f;

	pvr_set_bg_color( 0.f, 0.f, 0.f );
	if( TextureFrame <= 2 ) debugf("DCFIRST scene begin frame=%u", (unsigned)TextureFrame);
	pvr_scene_begin();

	PVRStartList( PVR_LIST_OP_POLY );
	GPVRCurrentPass = 0;

	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
		ColorMod = FPlane( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.f - Min( FlashScale.X * 2.f, 1.f ) );
	else
		ColorMod = FPlane( 0.f, 0.f, 0.f, 0.f );

	unguard;
}

void UPVRRenderDevice::BeginRenderPass( INT Pass )
{
	FlushWorldLightmaps();
	check( Pass >= 0 && Pass <= 2 );
	check( GPVRCurrentPass >= 0 && Pass >= GPVRCurrentPass );
	while( GPVRCurrentPass < Pass )
	{
		PVRFinishList();
		++GPVRCurrentPass;
		const pvr_list_t List = GPVRCurrentPass == 1 ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY;
		if( List == PVR_LIST_PT_POLY )
			PVR_SET(PVR_PT_ALPHA_REF, PVR_PT_ALPHA_THRESHOLD);
		PVRStartList( List );
	}
}

UBOOL UPVRRenderDevice::WantsBspSurface( DWORD PolyFlags, INT Pass ) const
{
	// TR also carries lightmaps and fog for OP/PT base surfaces.
	if( Pass == 2 )
		return 1;
	if( PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted) )
		return 0;
	return Pass == ((PolyFlags & PF_Masked) ? 1 : 0);
}

UBOOL UPVRRenderDevice::WantsMeshFlags( DWORD PolyFlags ) const
{
	return GPVRCurrentPass < 0 || ListFor(PolyFlags) == GPVRDirectList;
}

void UPVRRenderDevice::Unlock( UBOOL Blit )
{
	guard(UPVRRenderDevice::Unlock);
	FlushWorldLightmaps();
	PVRMirrorSetFrame(NULL); // Flash/HUD are never reflected view geometry.
	DC_FRAME_SCOPE(DCFS_Submit);

	// KOS may close each list only once. DrawWorld and the HUD have already
	// submitted directly to OP, PT and TR in that order.
	PVRFinishList();
	GPVRCurrentPass = -1;

	// Measure TA vertex-buffer usage BEFORE scene_finish (which resets POS).
	// If the buffer fills, the TA silently drops subsequent polys -> geometry
	// vanishes depending on view angle.
	{
		static DWORD TAPeakUsed = 0;
		static DWORD TAPeakCapacity = 0;
		const size_t Start = PVR_GET(PVR_TA_VERTBUF_START);
		const size_t End  = PVR_GET(PVR_TA_VERTBUF_END);
		const size_t Pos  = PVR_GET(PVR_TA_VERTBUF_POS);
		const DWORD  Free = (End > Pos) ? (DWORD)(End - Pos) : 0;
		if( End > Start && Pos >= Start )
		{
			TAPeakUsed = Max( TAPeakUsed, (DWORD)(Min(Pos, End) - Start) );
			TAPeakCapacity = (DWORD)(End - Start);
		}
		if( TextureFrame % 300 == 0 )
		{
			debugf( NAME_Log, "PVRTA 300-frame peak=%u/%uKB spare=%uKB",
				(unsigned)(TAPeakUsed / 1024), (unsigned)(TAPeakCapacity / 1024),
				(unsigned)((TAPeakCapacity - TAPeakUsed) / 1024) );
			TAPeakUsed = 0;
		}
		if( Free == 0 )
			debugf( "PVR: TA VERTEX BUFFER FULL (Pos=%u End=%u) - geometry being dropped!", (unsigned)Pos, (unsigned)End );
	}

	if( TextureFrame <= 2 ) debugf("DCFIRST scene finish frame=%u", (unsigned)TextureFrame);
	pvr_scene_finish();
	if( TextureFrame <= 2 ) debugf("DCFIRST scene finished frame=%u", (unsigned)TextureFrame);

	if( TextureFrame % 300 == 0 )
		PrintTextureCPUProfile( 300 );

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
	if( CurrentSceneNode.bIsSky ) PolyFlags &= ~PF_Occlude;
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
SHZ_NO_INLINE __attribute__((noclone))
void UPVRRenderDevice::EmitHeader( pvr_list_t List, DWORD PolyFlags, const FTexState* Tex, UBOOL NoDepth, pvr_cull_mode_t Cull )
{
	const DWORD StateBits =
		PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted|PF_Invisible|PF_Occlude|PF_Masked);
	const UBOOL Textured = Tex && Tex->Tex;
	const QWORD TextureKey = Textured ? Tex->Key : 0;
	const pvr_ptr_t Texture = Textured ? Tex->Tex : NULL;
	const DWORD TextureFormat = Textured ? Tex->Format : 0;
	const INT TextureWidth = Textured ? Tex->Width : 0;
	const INT TextureHeight = Textured ? Tex->Height : 0;
	const UBOOL TextureMipMapped = Textured ? Tex->MipMapped : 0;

	FPVRHeaderCache& Cache = GPVRHeaderCache[List];
	if( Cache.Valid
		&& Cache.PolyFlags == StateBits
		&& Cache.NoDepth == NoDepth
		&& Cache.FilteringDisabled == NoFiltering
		&& Cache.FogEnabled == FogActive
		&& Cache.OverbrightEnabled == Overbright
		&& Cache.Cull == Cull
		&& Cache.TextureKey == TextureKey
		&& Cache.Texture == Texture
		&& Cache.TextureFormat == TextureFormat
		&& Cache.TextureWidth == TextureWidth
		&& Cache.TextureHeight == TextureHeight
		&& Cache.TextureMipMapped == TextureMipMapped )
		return;
	Cache.Valid = 1;
	Cache.PolyFlags = StateBits;
	Cache.NoDepth = NoDepth;
	Cache.FilteringDisabled = NoFiltering;
	Cache.FogEnabled = FogActive;
	Cache.OverbrightEnabled = Overbright;
	Cache.Cull = Cull;
	Cache.TextureKey = TextureKey;
	Cache.Texture = Texture;
	Cache.TextureFormat = TextureFormat;
	Cache.TextureWidth = TextureWidth;
	Cache.TextureHeight = TextureHeight;
	Cache.TextureMipMapped = TextureMipMapped;
#if defined(PLATFORM_DREAMCAST)
	DCFrameHeader();
#endif
	// Both state objects have static zero initialization (including padding).
	// Full equality, not the hash alone, protects all hardware state changes.
	const DWORD Hash=((uintptr_t)Texture>>5) ^ ((uintptr_t)Texture>>13)
		^ ((uintptr_t)Texture>>19) ^ TextureFormat ^ StateBits
		^ (DWORD)List*17 ^ (DWORD)Cull*7 ^ (NoDepth ? 31 : 0);
	FPVRCompiledHeader& Compiled=GPVRCompiledHeaders[Hash & 63];
	if( Compiled.State.Valid && Compiled.List==List
		&& !appMemcmp(&Compiled.State,&Cache,sizeof(Cache)) )
	{
		DCFrameCount(DCFC_HeaderCacheHit);
		pvr_poly_hdr_t* Out=(pvr_poly_hdr_t*)PVRAlloc32(List);
		*Out=Compiled.Header;
		PVRCommit32(List,Out);
		return;
	}
	DCFrameCount(DCFC_HeaderCacheMiss);

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
		// "Enable depth writes" doc comment: KOS header compilation does
		//   FIELD_PREP(PVR_TA_PM1_DEPTHWRITE, depth.write)
		// so 0 = writes ENABLED, 1 = writes DISABLED. Always assign
		// PVR_DEPTHWRITE_ENABLE/DISABLE, never true/false.
		Cxt.depth.write = ( List == PVR_LIST_TR_POLY || !(PolyFlags & PF_Occlude) )
			? PVR_DEPTHWRITE_DISABLE : PVR_DEPTHWRITE_ENABLE;
	}

	// BSP, UI, two-sided and special mesh paths retain their original state.
	// Eligible mesh fronts project clockwise in screen coordinates under the
	// engine's Frame->Mirror == 1 convention, so reject counterclockwise faces.
	Cxt.gen.culling = Cull;

	if( PolyFlags & PF_Invisible )
	{
		Cxt.blend.src = PVR_BLEND_ZERO;
		Cxt.blend.dst = PVR_BLEND_ZERO;
	}
	else if( List == PVR_LIST_PT_POLY )
	{
		// Keep KOS' punch-through blend state: transparent cutout texels must
		// preserve the background rather than overwrite it with their RGB.
		Cxt.blend.src = PVR_BLEND_SRCALPHA;
		Cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
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
	{
		DC_FRAME_SCOPE(DCFS_HeaderCompile);
		pvr_poly_compile( &Hdr, &Cxt );
	}
	Compiled.State=Cache; Compiled.List=List; Compiled.Header=Hdr;

	pvr_poly_hdr_t* Out = (pvr_poly_hdr_t*)PVRAlloc32( List );
	*Out = Hdr;
	PVRCommit32( List, Out );
}

/*-----------------------------------------------------------------------------
	World surfaces.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::FlushWorldLightmaps()
{
	if( !WorldLightCount ) return;
	check(GPVRDirectList==PVR_LIST_TR_POLY);
	// Stable insertion sort of a tiny, screen-disjoint batch only.
	for( INT i=1; i<WorldLightCount; ++i )
	{
		FWorldLightDraw Item=WorldLightDraws[i]; INT j=i;
		while( j && WorldLightDraws[j-1].Tex.Key>Item.Tex.Key )
		{ WorldLightDraws[j]=WorldLightDraws[j-1]; --j; }
		WorldLightDraws[j]=Item;
	}
	if( WorldLightCount>1 ) DCFrameCount(DCFC_WorldBatchPolys,WorldLightCount);
	for( INT i=0; i<WorldLightCount; ++i )
	{
		const FWorldLightDraw& D=WorldLightDraws[i];
		EmitHeader(PVR_LIST_TR_POLY,D.Flags,&D.Tex,0);
		for( INT v=0; v<D.Count; ++v )
		{
			pvr_vertex_t* Out=(pvr_vertex_t*)PVRAlloc32(PVR_LIST_TR_POLY);
			*Out=WorldLightVertices[D.First+v];
			PVRCommit32(PVR_LIST_TR_POLY,Out);
		}
	}
	WorldLightCount=WorldLightVertexCount=0;
	WorldLightFrame=NULL;
}

void UPVRRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guard(UPVRRenderDevice::DrawComplexSurface);
	const UBOOL BatchCandidate=GPVRDirectList==PVR_LIST_TR_POLY
		&& Surface.LightMap && !Surface.FogMap
		&& !(Surface.PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted|PF_Masked|PF_Invisible|PF_Portal));
	if( !BatchCandidate || (WorldLightCount && WorldLightFrame!=Frame) ) FlushWorldLightmaps();

	check(Surface.Texture);

	SetSceneNode( Frame );

	// Sky layers are emitted normally; their depth is placed behind the
	// world below, without flattening reciprocal W (which distorts UVs).
	// Invisible mirror masks used to paint black and overwrite the reflected
	// view's depth. Reflection geometry is now clipped to its opening instead.
	if( (Surface.PolyFlags & (PF_Mirrored|PF_Invisible))==(PF_Mirrored|PF_Invisible) )
		return;

	const DWORD BaseFlags = AdjustFlags( Surface.PolyFlags );
	const pvr_list_t BaseList = ListFor( BaseFlags );
	const UBOOL DrawBase = BaseList == GPVRDirectList;
	const UBOOL DrawLight = GPVRDirectList == PVR_LIST_TR_POLY
		&& Surface.LightMap != NULL && !CurrentSceneNode.bIsSky;
	const UBOOL DrawFog = GPVRDirectList == PVR_LIST_TR_POLY
		&& Surface.FogMap != NULL && !CurrentSceneNode.bIsSky;
	if( !DrawBase && !DrawLight && !DrawFog )
		return;

	// Only bind textures needed by the active TA list.
	FTexState Base;
	if( DrawBase )
	{
		SetTexture( *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
		CaptureTexState( Base );
	}

	FTexState Light;
	DWORD LightFlags = 0;
	if( DrawLight )
	{
		SetTexture( *Surface.LightMap, 0, -0.5f );
		CaptureTexState( Light );
		LightFlags = PF_Modulated | (Surface.PolyFlags & PF_Masked);
	}
	// Only immutable preloaded VQ pages can survive deferred binding safely.
	const UBOOL BatchLight=BatchCandidate && !DrawBase && !CurrentSceneNode.bIsSky
		&& (Light.Key & ((QWORD)1<<63)) && (Light.Format & PVR_TXRFMT_VQ_ENABLE);
	if( !BatchLight ) FlushWorldLightmaps();

	// Volumetric light shafts, when the device advertises fog maps.
	FTexState Fog;
	DWORD FogFlags = 0;
	if( DrawFog )
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
		if( DrawBase && PVRBeginDraw( BaseList, MaxBytes ) )
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
		if( DrawLight && PVRBeginDraw( PVR_LIST_TR_POLY, MaxBytes ) )
		{
			for( i = 0; i < Count; ++i )
			{
				Verts[i].U = ( Dots[i][0] - Light.UPan ) * Light.UMult;
				Verts[i].V = ( Dots[i][1] - Light.VPan ) * Light.VMult;
			}
			if( BatchLight && Count<=256 )
			{
				FLOAT MinX=Verts[0].SX, MaxX=MinX, MinY=Verts[0].SY, MaxY=MinY;
				for( i=1; i<Count; ++i )
				{ MinX=Min(MinX,Verts[i].SX); MaxX=Max(MaxX,Verts[i].SX);
				  MinY=Min(MinY,Verts[i].SY); MaxY=Max(MaxY,Verts[i].SY); }
				UBOOL Overlap=0;
				for( i=0; i<WorldLightCount; ++i )
				{
					const FWorldLightDraw& D=WorldLightDraws[i];
					// One-pixel guard also preserves shared-edge rasterization order.
					Overlap |= !(MaxX+1<D.MinX || MinX>D.MaxX+1 || MaxY+1<D.MinY || MinY>D.MaxY+1);
				}
				if( Overlap || WorldLightCount==16 || WorldLightVertexCount+Count>256 ) FlushWorldLightmaps();
				WorldLightFrame=Frame;
				FWorldLightDraw& D=WorldLightDraws[WorldLightCount++];
				D.Tex=Light; D.Flags=LightFlags; D.First=WorldLightVertexCount; D.Count=Count;
				D.MinX=MinX; D.MaxX=MaxX; D.MinY=MinY; D.MaxY=MaxY;
				INT Lo=2, Hi=Count-1;
				for( i=0; i<Count; ++i )
				{
					const INT Index=i<2 ? i : (i&1) ? Lo++ : Hi--;
					const FPVRVert& V=Verts[Index];
					pvr_vertex_t& Out=WorldLightVertices[WorldLightVertexCount++];
					Out.flags=i==Count-1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
					Out.x=V.SX; Out.y=V.SY; Out.z=V.SZ; Out.u=V.U; Out.v=V.V;
					Out.argb=V.ARGB; Out.oargb=0;
				}
			}
			else
			{
				FlushWorldLightmaps();
				EmitHeader(PVR_LIST_TR_POLY,LightFlags,&Light,0);
				PVREmitConvexStrip(PVR_LIST_TR_POLY,Verts,Count);
			}
		}

		// Volumetric fog pass.
		if( DrawFog && PVRBeginDraw( PVR_LIST_TR_POLY, MaxBytes ) )
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

// The common, unclipped mesh strip writes its final 32-byte TA vertex in
// place.  In particular, do not construct an FPVRVert on the stack and copy
// its fields into the store queue for every vertex.
static inline void PVREmitGouraudVert( pvr_list_t List, const FTransTexture& P,
	FLOAT UMult, FLOAT VMult, UBOOL Modulated, DWORD Flags )
{
	pvr_vertex_t* Vtx = (pvr_vertex_t*)PVRAlloc32( List );
	Vtx->flags = Flags;
	Vtx->x = P.ScreenX;
	Vtx->y = P.ScreenY;
	Vtx->z = P.RZ;
	Vtx->u = P.U * UMult;
	Vtx->v = P.V * VMult;
	Vtx->argb = Modulated ? 0xFFFFFFFFu : PVRPackLight( P.Light );
	Vtx->oargb = 0;
	PVRCommit32( List, Vtx );
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
	FlushWorldLightmaps();

	if( NumPts < 3 || NumPts > FBspNode::MAX_FINAL_VERTICES )
		return;
	const DWORD Flags = AdjustFlags( PolyFlags );
	const pvr_list_t List = ListFor( Flags );
	if( List != GPVRDirectList )
		return;

	SetSceneNode( Frame );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );

	const UBOOL Modulated = ( PolyFlags & PF_Modulated ) != 0;

	UBOOL NeedsClip = 0;
	INT i;
	for( i = 0; i < NumPts; ++i )
		NeedsClip |= ( Pts[i]->Point.Z < PVR_NEAR_Z );

	FTexState Tex;
	CaptureTexState( Tex );
	const pvr_cull_mode_t Cull = MeshDrawScope && UseHardwareMeshCull
		&& Frame->Mirror == 1.f && Frame->NearClip.W == 0.f
		&& NumPts == 3 && !NeedsClip && !(Pts[0]->Flags | Pts[1]->Flags | Pts[2]->Flags)
		&& (Flags & (PF_TwoSided|PF_Flat|PF_Invisible)) == PF_Flat
		&& !(Flags & (PF_Environment|PF_Unlit))
		? PVR_CULLING_CCW : PVR_CULLING_NONE;

	if( !NeedsClip )
	{
		if( !PVRBeginDraw( List, 32 + NumPts * 32 ) )
			return;
		EmitHeader( List, Flags, &Tex, 0, Cull );
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
	EmitHeader( List, Flags, &Tex, 0, Cull );
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
	FlushWorldLightmaps();

	if( NumPts < 3 )
		return;
	const DWORD Flags = AdjustFlags( PolyFlags );
	const pvr_list_t List = ListFor( Flags );
	if( List != GPVRDirectList )
		return;

	SetSceneNode( Frame );
	SetTexture( Texture, ( PolyFlags & PF_Masked ), 0.f );

	const UBOOL Modulated = ( PolyFlags & PF_Modulated ) != 0;

	UBOOL NeedsClip = 0;
	INT i;
	for( i = 0; i < NumPts; ++i )
		NeedsClip |= ( Pts[i]->Point.Z < PVR_NEAR_Z );

	FTexState Tex;
	CaptureTexState( Tex );
	const pvr_cull_mode_t Cull = MeshDrawScope && UseHardwareMeshCull
		&& Frame->Mirror == 1.f && Frame->NearClip.W == 0.f && !NeedsClip
		&& (Flags & (PF_TwoSided|PF_Flat|PF_Invisible)) == PF_Flat
		&& !(Flags & (PF_Environment|PF_Unlit))
		? PVR_CULLING_CCW : PVR_CULLING_NONE;

	if( !NeedsClip )
	{
		// The whole strip goes to the TA as a strip: N vertices for N-2
		// triangles, which is the entire point of cooking meshes this way.
		if( !PVRBeginDraw( List, 32 + NumPts * 32 ) )
			return;
		EmitHeader( List, Flags, &Tex, 0, Cull );
		for( i = 0; i < NumPts; ++i )
			PVREmitGouraudVert( List, *Pts[i], TexInfo.UMult, TexInfo.VMult,
				Modulated, ( i == NumPts - 1 ) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX );
		return;
	}

	// Something straddles the near plane; fall back to clipping each triangle
	// of the strip independently, preserving the strip's winding alternation.
	if( !PVRBeginDraw( List, 32 + (NumPts - 2) * 4 * 32 ) )
		return;
	EmitHeader( List, Flags, &Tex, 0, Cull );
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

void UPVRRenderDevice::BeginCookedMesh()
{
	FlushWorldLightmaps();
	MeshBatchFrame = NULL;
	MeshBatchTexture = NULL;
	MeshDrawScope = true;
}

void UPVRRenderDevice::EndCookedMesh()
{
	MeshBatchFrame = NULL;
	MeshBatchTexture = NULL;
	MeshDrawScope = false;
}

INT UPVRRenderDevice::DrawIndexedMeshRanges( FSceneNode* Frame, FTextureInfo& Texture,
	const FTransTexture* Samples, FDCMeshDrawCache& Cache,
	const FDCMeshDrawRange* Ranges, INT Count, DWORD PolyFlags )
{
	if( !MeshDrawScope
		|| (PolyFlags & (PF_Environment|PF_Unlit|PF_Invisible)) ) return 0;
	const DWORD Flags=AdjustFlags(PolyFlags);
	const pvr_list_t List=ListFor(Flags);
	if( List!=GPVRDirectList ) return Count;
	INT Ready=0;
	{
	DC_FRAME_SCOPE(DCFS_MeshDrawCache);
	// Validate/cache the accepted prefix as a block: no timers per vertex or
	// strip, and no TA writes until we know exactly which ranges can proceed.
	for( ; Ready<Count; ++Ready )
	{
		const FDCMeshDrawRange& Range=Ranges[Ready];
		if( Range.Count<3 ) goto CacheDone;
		// Validate the whole strip before its header/vertices. Only immutable
		// native samples enter this cache; fallback operates on corner copies.
		for( INT i=0; i<Range.Count; ++i )
		{
			const INT Index=Range.Indices[i];
			if( Index>=Cache.Count ) goto CacheDone;
			if( !Cache.State[Index] )
			{
				const FTransTexture& P=Samples[Index];
				if( P.Flags || P.Point.Z<PVR_NEAR_Z ) Cache.State[Index]=2;
				else
				{
					FDCMeshDrawVertex& V=Cache.Vertices[Index];
					V.X=P.ScreenX; V.Y=P.ScreenY; V.Z=P.RZ;
					V.ARGB=PVRPackLight(P.Light);
					Cache.State[Index]=1;
				}
			}
			if( Cache.State[Index]!=1 ) goto CacheDone;
		}
	}
	}
CacheDone:
	if( !Ready ) return 0;
	FLOAT UScale, VScale;
	{
		DC_FRAME_SCOPE(DCFS_MeshDrawSetup);
		UScale=Texture.UScale*Texture.USize/256.0;
		VScale=Texture.VScale*Texture.VSize/256.0;
		{
			if( MeshBatchFrame!=Frame || MeshBatchTexture!=&Texture || MeshBatchFlags!=PolyFlags )
			{
				SetSceneNode(Frame); SetTexture(Texture,PolyFlags & PF_Masked,0.f);
				MeshBatchFrame=Frame; MeshBatchTexture=&Texture; MeshBatchFlags=PolyFlags;
			}
			FTexState Tex; CaptureTexState(Tex);
			const pvr_cull_mode_t Cull=UseHardwareMeshCull && Frame->Mirror==1.f
				&& Frame->NearClip.W==0.f && (Flags & (PF_TwoSided|PF_Flat))==PF_Flat
				? PVR_CULLING_CCW : PVR_CULLING_NONE;
			EmitHeader(List,Flags,&Tex,0,Cull);
		}
	}
	{
	DC_FRAME_SCOPE(DCFS_MeshDrawEmit);
	for( INT r=0; r<Ready; ++r )
	{
		const FDCMeshDrawRange& Range=Ranges[r];
		for( INT i=-INT(Range.OddStart); i<Range.Count; ++i )
		{
			const INT Corner=Max(i,0);
			const FDCMeshDrawVertex& P=Cache.Vertices[Range.Indices[Corner]];
			const DWORD UV=Range.UVs[Corner];
			pvr_vertex_t* V=(pvr_vertex_t*)PVRAlloc32(List);
			V->flags=i==Range.Count-1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
			V->x=P.X; V->y=P.Y; V->z=P.Z;
			V->u=((UV&255)*UScale)*TexInfo.UMult;
			V->v=((UV>>8)*VScale)*TexInfo.VMult;
			V->argb=(PolyFlags & PF_Modulated) ? 0xffffffffu : P.ARGB;
			V->oargb=0;
			PVRCommit32(List,V);
		}
	}
	}
	return Ready;
}

UBOOL UPVRRenderDevice::DrawIndexedMeshStrip( FSceneNode* Frame, FTextureInfo& Texture,
	const FTransTexture* Samples, const _WORD* Indices, const _WORD* UVs,
	INT Count, UBOOL OddStart, DWORD PolyFlags )
{
	if( !MeshDrawScope || Count < 3
		|| (PolyFlags & (PF_Environment|PF_Unlit|PF_Invisible)) )
		return 0;
	const DWORD Flags = AdjustFlags(PolyFlags);
	const pvr_list_t List = ListFor(Flags);
	if( List != GPVRDirectList ) return 1;
	// Validate the complete range before emitting anything; fallback is atomic.
	for( INT i=0; i<Count; ++i )
		if( Samples[Indices[i]].Flags || Samples[Indices[i]].Point.Z < PVR_NEAR_Z ) return 0;
	if( MeshBatchFrame != Frame || MeshBatchTexture != &Texture || MeshBatchFlags != PolyFlags )
	{
		SetSceneNode(Frame);
		SetTexture(Texture,PolyFlags & PF_Masked,0.f);
		MeshBatchFrame=Frame; MeshBatchTexture=&Texture; MeshBatchFlags=PolyFlags;
	}
	FTexState Tex;
	CaptureTexState(Tex);
	const pvr_cull_mode_t Cull = UseHardwareMeshCull && Frame->Mirror==1.f
		&& Frame->NearClip.W==0.f && (Flags & (PF_TwoSided|PF_Flat))==PF_Flat
		? PVR_CULLING_CCW : PVR_CULLING_NONE;
	if( !PVRBeginDraw(List,32+(Count+OddStart)*32) ) return 1;
	EmitHeader(List,Flags,&Tex,0,Cull);
	// Match the legacy two-stage UV scale without changing seam ownership.
	const FLOAT UScale=Texture.UScale*Texture.USize/256.0;
	const FLOAT VScale=Texture.VScale*Texture.VSize/256.0;
	for( INT i=-INT(OddStart); i<Count; ++i )
	{
		const INT Corner=Max(i,0);
		const FTransTexture& P=Samples[Indices[Corner]];
		const DWORD UV=UVs[Corner];
		pvr_vertex_t* V=(pvr_vertex_t*)PVRAlloc32(List);
		V->flags=i==Count-1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
		V->x=P.ScreenX; V->y=P.ScreenY; V->z=P.RZ;
		V->u=((UV&255)*UScale)*TexInfo.UMult;
		V->v=((UV>>8)*VScale)*TexInfo.VMult;
		V->argb=(PolyFlags & PF_Modulated) ? 0xffffffffu : PVRPackLight(P.Light);
		V->oargb=0;
		PVRCommit32(List,V);
	}
	return 1;
}

/*-----------------------------------------------------------------------------
	Tiles.
-----------------------------------------------------------------------------*/

void UPVRRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags )
{
	guard(UPVRRenderDevice::DrawTile);
	FlushWorldLightmaps();
	if( GPVRDirectList != PVR_LIST_TR_POLY )
		return;

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
	const FLOAT TileZ = GPVRMirrorFrame ? Frame->Proj.Z / Max(Z,1.f) : OverlayZUI;

	EmitHeader( List, PolyFlags, &Tex, /*NoDepth=*/GPVRMirrorFrame ? 0 : 1 );

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
	if( WorldLightCount && WorldLightFrame!=Frame ) FlushWorldLightmaps();
	PVRMirrorSetFrame(Frame);
	MeshBatchFrame = NULL;
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
	// Scale sky depth once at final submission, including sprites and meshes.
	if( CurrentSceneNode.bIsSky )
	{
		// Only genuine ancestor mirrors restrict the sky opening.
		GPVRMirrorFrame=Frame;
		GPVRMirrorDepthScale=1.f/16777216.f;
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

void UPVRRenderDevice::PreloadCookedLightmapPages( ULevel* Level )
{
	guard(UPVRRenderDevice::PreloadCookedLightmapPages);
#if defined(PLATFORM_DREAMCAST)
	AtlasPreloadActive = true;
	PreloadedLightmapLevel = Level;
	const INT Count = Level->DCLightmaps.Num();
	PreloadedLightmaps.SetNum( Count );
	for( INT i=0; i<Count; ++i )
		PreloadedLightmaps(i).Page = -1;

	const INT PageCount = Level->DCLightmapPages.Num();
	if( PageCount >= AtlasPageMax )
		appErrorf( "Too many cooked lightmap atlas pages: %i", PageCount );
	const INT DynamicPageCount = UseVQDynamicLightmaps ? Level->DCDynamicLightmapPages.Num() : 0;
	if( PageCount + DynamicPageCount >= AtlasPageMax )
		appErrorf( "Too many combined cooked lightmap pages: %i+%i",
			PageCount, DynamicPageCount );
	const size_t FreeReserve = 3 * 1024 * 1024;
	INT LoadedPages = 0;
	DWORD LoadedBytes = 0;
	for( INT i=0; i<PageCount; ++i )
	{
		const FDCLightmapPage& Entry = Level->DCLightmapPages(i);
		if( Entry.Size < 32 || Entry.Size > 64 * 1024 )
			appErrorf( "Invalid cooked VQ lightmap page size: %u", Entry.Size );
		BYTE* Data = (BYTE*)appMalloc( Entry.Size, "CookedLightmapAtlas" );
		Level->DCLightmapAtlasData.ReadRange( Entry.Offset, Data, Entry.Size );
		DWORD Header[8];
		appMemcpy( Header, Data, sizeof(Header) );
		const INT HeaderSize = (Data[9] + 1) * 32;
		const DWORD Mode = Header[4];
		const INT Width = 8 << ((Mode >> 3) & 7);
		const INT Height = 8 << (Mode & 7);
		if( Header[0] != 0x78546344 || Header[1] != Entry.Size
			|| HeaderSize >= (INT)Entry.Size || Data[10] != 255
			|| Width != AtlasPageDim || Height != AtlasPageDim
			|| (Mode & (PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE))
				!= (PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE)
			|| (Mode & 0x80000000) )
			appErrorf( "Invalid cooked VQ lightmap page %i", i );
		const INT PayloadSize = Entry.Size - HeaderSize;
		FLightAtlasPage& Page = AtlasPages[i];
		if( PVRPoolAvailable() >= FreeReserve + (size_t)PayloadSize )
			Page.Tex = PVRPoolMalloc( PayloadSize );
		if( Page.Tex )
		{
			pvr_txr_load( Data + HeaderSize, Page.Tex, PayloadSize );
			Page.SizeBytes = PayloadSize;
			Page.Format = Mode & 0x7e000000;
			VRAMUsed += PayloadSize;
			LoadedBytes += PayloadSize;
			++LoadedPages;
		}
		appFree( Data );
	}
	INT DynamicLoadedPages = 0;
	DWORD DynamicLoadedBytes = 0;
	for( INT i=0; i<DynamicPageCount; ++i )
	{
		const FDCLightmapPage& Entry = Level->DCDynamicLightmapPages(i);
		if( Entry.Size < 32 || Entry.Size > 64*1024 )
			appErrorf( "Invalid dynamic VQ lightmap page size: %u", Entry.Size );
		BYTE* Data = (BYTE*)appMalloc(Entry.Size,"CookedDynamicLightmapAtlas");
		Level->DCDynamicLightmapAtlasData.ReadRange(Entry.Offset,Data,Entry.Size);
		DWORD Header[8];
		appMemcpy(Header,Data,sizeof(Header));
		const INT HeaderSize = (Data[9]+1)*32;
		const DWORD Mode = Header[4];
		const INT Width = 8 << ((Mode >> 3) & 7);
		const INT Height = 8 << (Mode & 7);
		if( Header[0] != 0x78546344 || Header[1] != Entry.Size
			|| HeaderSize >= (INT)Entry.Size || Data[10] != 255
			|| Width != AtlasPageDim || Height != AtlasPageDim
			|| (Mode & (PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE))
				!= (PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE) )
			appErrorf( "Invalid dynamic VQ lightmap page %i", i );
		const INT PayloadSize = Entry.Size - HeaderSize;
		FLightAtlasPage& Page = AtlasPages[PageCount+i];
		if( PVRPoolAvailable() >= FreeReserve + (size_t)PayloadSize )
			Page.Tex = PVRPoolMalloc(PayloadSize);
		if( Page.Tex )
		{
			pvr_txr_load(Data+HeaderSize,Page.Tex,PayloadSize);
			Page.SizeBytes = PayloadSize;
			Page.Format = Mode & 0x7e000000;
			VRAMUsed += PayloadSize;
			DynamicLoadedBytes += PayloadSize;
			Level->DCDynamicPageResident(i) = 1;
			++DynamicLoadedPages;
		}
		appFree(Data);
	}
	AtlasPageCount = PageCount + DynamicPageCount;
	AtlasDynamicFirstPage = AtlasPageCount;
	INT LoadedTiles = 0;
	for( INT i=0; i<Count; ++i )
	{
		const FDCLightmapPlacement& Placement = Level->DCLightmapPlacements(i);
		if( Placement.Page >= PageCount || !AtlasPages[Placement.Page].Tex )
			continue;
		FPreloadedLightmap& Resident = PreloadedLightmaps(i);
		Resident.Page = Placement.Page;
		Resident.X = Placement.X;
		Resident.Y = Placement.Y;
		++LoadedTiles;
	}
	// The mutable page remains RGB565. It is never shared with immutable VQ
	// pages, so dynamic/moving lights retain the exact existing upload path.
	if( AtlasPageCount < AtlasPageMax
		&& PVRPoolAvailable() >= FreeReserve + AtlasPageDim * AtlasPageDim * sizeof(_WORD) )
	{
		FLightAtlasPage& Runtime = AtlasPages[AtlasPageCount];
		Runtime.Tex = PVRPoolMalloc( AtlasPageDim * AtlasPageDim * sizeof(_WORD) );
		if( Runtime.Tex )
		{
			Runtime.SizeBytes = AtlasPageDim * AtlasPageDim * sizeof(_WORD);
			Runtime.Format = PVR_TXRFMT_RGB565;
			VRAMUsed += Runtime.SizeBytes;
			debugf( NAME_Log, "PVR: lightmap atlas page %i allocated phase=preload-dynamic-reserve (128 KB)", AtlasPageCount );
			++AtlasPageCount;
		}
	}
	AtlasPreloadActive = false;
	debugf( NAME_Log, "DCLIGHTMAP VQ preload map=%s tiles=%i/%i pages=%i/%i bytes=%uKB free=%uKB",
		Level->GetParent()->GetName(), LoadedTiles, Count, LoadedPages, PageCount,
		LoadedBytes / 1024, (unsigned)(PVRPoolAvailable() / 1024) );
	debugf( NAME_Log, "DCDYNLIGHT VQ preload map=%s variants=%i pages=%i/%i bytes=%uKB enabled=%d",
		Level->GetParent()->GetName(), Level->DCDynamicLightmaps.Num(), DynamicLoadedPages,
		DynamicPageCount, DynamicLoadedBytes/1024, (INT)UseVQDynamicLightmaps );
#endif
	unguard;
}

void UPVRRenderDevice::PreloadCookedLightmaps( ULevel* Level )
{
	guard(UPVRRenderDevice::PreloadCookedLightmaps);
#if defined(PLATFORM_DREAMCAST)
	if( !Level || !Level->Model || !Level->DCLightmaps.Num() || !Level->DCLightmapData.Size() )
	{
		debugf( NAME_Log, "DCLIGHTMAP preload skipped level=%s model=%i entries=%i bytes=%u",
			Level ? Level->GetParent()->GetName() : "None", Level && Level->Model ? 1 : 0,
			Level ? Level->DCLightmaps.Num() : 0,
			Level ? (unsigned)Level->DCLightmapData.Size() : 0 );
		return;
	}
	if( Level->DCLightmapPages.Num() )
	{
		PreloadCookedLightmapPages( Level );
		return;
	}
	AtlasPreloadActive = true;

	const INT Count = Level->DCLightmaps.Num();
	PreloadedLightmaps.SetNum( Count );
	for( INT i = 0; i < Count; ++i )
		PreloadedLightmaps(i).Page = -1;
	PreloadedLightmapLevel = Level;

	// Vortex2's complete compressed payload is only about 409KB. Bulk-read it
	// once to avoid a seek for every tile. Larger maps use bounded scratch RAM.
	const DWORD PackedBytes = Level->DCLightmapData.Size();
	BYTE* Bulk = NULL;
	BYTE* PackedScratch = NULL;
	DWORD MaxPacked = 0;
	for( INT i = 0; i < Count; ++i )
	{
		const FDCLightmapEntry& Entry = Level->DCLightmaps(i);
		if( Entry.USize <= AtlasMaxTile && Entry.VSize <= AtlasMaxTile )
			MaxPacked = Max( MaxPacked, Entry.PackedSize );
	}
	if( PackedBytes <= 512 * 1024 )
	{
		Bulk = (BYTE*)malloc( PackedBytes );
		if( Bulk )
			Level->DCLightmapData.Read( Bulk );
	}
	if( !Bulk && MaxPacked )
	{
		PackedScratch = (BYTE*)appMalloc( MaxPacked, "CookedLightmapScratch" );
	}
	_WORD* Pixels = (_WORD*)memalign( 32, AtlasMaxTile * AtlasMaxTile * sizeof(_WORD) );
	if( !Pixels )
		appErrorf( "Cooked lightmap preload scratch allocation failed" );

	INT Loaded = 0, Oversize = 0, NoRoom = 0;
	DWORD LoadedPixels = 0;
	const size_t FreeReserve = 3 * 1024 * 1024;
	for( INT i = 0; i < Count; ++i )
	{
		const FDCLightmapEntry& Entry = Level->DCLightmaps(i);
		if( Entry.USize > AtlasMaxTile || Entry.VSize > AtlasMaxTile )
		{
			++Oversize;
			continue;
		}
		if( PVRPoolAvailable() < FreeReserve + AtlasPageDim * AtlasPageDim * sizeof(_WORD) )
		{
			++NoRoom;
			continue;
		}

		FTexBind Slot = { NULL, 0, 0, PVR_TXRFMT_RGB565,
			Entry.USize, Entry.VSize, 0, 0, INDEX_NONE, 0, -1, 0, 0 };
		// The final page remains available for moving/dynamic lightmaps. Pinned
		// cooked slots never enter BindMap and therefore cannot be reclaimed.
		if( !LightAtlasPlace( &Slot, Entry.USize, Entry.VSize, AtlasPageMax - 1, 0 ) )
		{
			++NoRoom;
			continue;
		}

		const BYTE* Packed = Bulk ? Bulk + Entry.Offset : PackedScratch;
		if( !Bulk )
			Level->DCLightmapData.ReadRange( Entry.Offset, PackedScratch, Entry.PackedSize );
		if( Entry.Codec == 0 )
			appMemcpy( Pixels, Packed, Entry.Size() );
		else
		{
			uLongf OutputSize = Entry.Size();
			if( uncompress( (BYTE*)Pixels, &OutputSize, Packed, Entry.PackedSize ) != Z_OK
				|| OutputSize != (uLongf)Entry.Size() )
				appErrorf( "Invalid preloaded cooked lightmap %i", i );
		}
		LightAtlasStore( &Slot, Entry.USize, Entry.VSize, Pixels );
		FPreloadedLightmap& Resident = PreloadedLightmaps(i);
		Resident.Page = Slot.AtlasPage;
		Resident.X = Slot.AtlasX;
		Resident.Y = Slot.AtlasY;
		++Loaded;
		LoadedPixels += Entry.USize * Entry.VSize;
	}

	free( Pixels );
	if( PackedScratch ) appFree( PackedScratch );
	if( Bulk ) free( Bulk );
	debugf( NAME_Log, "DCLIGHTMAP preload map=%s tiles=%i/%i oversize=%i no_room=%i pages=%i pixels=%u free=%uKB bulk=%i",
		Level->GetParent()->GetName(), Loaded, Count, Oversize, NoRoom,
		AtlasPageCount, LoadedPixels, (unsigned)(PVRPoolAvailable() / 1024), Bulk != NULL );
	// Static slots are pinned for this level. Dynamic tiles must never spend
	// frame time scanning those full pages or displace their reservations.
	AtlasDynamicFirstPage = AtlasPageCount;
	// Keep one ready page for dynamic/mover lightmaps. Additional pages grow
	// only if the runtime working set needs them; three idle pages cost 384 KB
	// that can instead hold cooked textures during level preload.
	while( AtlasPageCount < Min( AtlasPageMax, AtlasDynamicFirstPage + 1 )
		&& PVRPoolAvailable() >= FreeReserve + AtlasPageDim * AtlasPageDim * sizeof(_WORD) )
	{
		const INT Page = AtlasPageCount;
		FLightAtlasPage& Runtime = AtlasPages[Page];
		Runtime.Tex = PVRPoolMalloc( AtlasPageDim * AtlasPageDim * sizeof(_WORD) );
		if( Runtime.Tex )
		{
			Runtime.SizeBytes = AtlasPageDim * AtlasPageDim * sizeof(_WORD);
			Runtime.Format = PVR_TXRFMT_RGB565;
			appMemset( Runtime.Rows, 0, sizeof(Runtime.Rows) );
			Runtime.SlotsUsed = 0;
			VRAMUsed += AtlasPageDim * AtlasPageDim * sizeof(_WORD);
			AtlasPageCount = Page + 1;
			debugf( NAME_Log, "PVR: lightmap atlas page %i allocated phase=preload-dynamic-reserve (128 KB)", Page );
		}
	}
	AtlasPreloadActive = false;
#endif
	unguard;
}

void UPVRRenderDevice::PreloadCookedAnimationFrames()
{
	guard(UPVRRenderDevice::PreloadCookedAnimationFrames);
#if defined(PLATFORM_DREAMCAST)
	TexturePreloadActive = true;
	// The frame-specific cache keys already retain each uploaded image. Warm
	// them while the loading screen is up. Keep a small VRAM floor for textures
	// discovered during play; AllocateTexture can evict cold binds if needed.
	const size_t FreeReserve = 256 * 1024;
	// Attempt every loaded animation frame. Available VRAM, rather than a
	// fixed 1-2MB quota, decides when the warm-up must fall back to streaming.
	const DWORD MaxPreloadBytes = PVRPoolAvailable() > FreeReserve
		? (DWORD)(PVRPoolAvailable() - FreeReserve) : 0;
	DWORD PreloadedBytes = 0;
	INT Animations = 0, Frames = 0, AlreadyResident = 0, Skipped = 0;
	ResetTexture();
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		UTexture* Texture = *It;
		if( Texture->Format != TEXF_EXT_DCANIM || Texture->Mips.Num() < 2 )
			continue;
		++Animations;
		const BYTE SavedFrame = Texture->PrimeCurrent;
		const DWORD SavedFlags = Texture->TextureFlags;
		for( INT Frame = 0; Frame < Texture->Mips.Num(); ++Frame )
		{
			FMipmap& Mip = Texture->Mips(Frame);
			const INT SourceBytes = Mip.DataArray.Num()
				? Mip.DataArray.Num() : Mip.StreamData.Size();
			if( SourceBytes < 32 )
			{
				++Skipped;
				continue;
			}
			Texture->PrimeCurrent = Frame;
			FTextureInfo Info;
			Texture->GetInfo( Info, 0.0 );
			FTexBind* Existing = BindMap.Find( Info.CacheID );
			if( Existing && Existing->Tex )
			{
				++AlreadyResident;
				continue;
			}
			// SourceBytes includes the DT header, so this is conservative relative
			// to the payload actually allocated in VRAM by UploadTexture.
			if( SourceBytes > (INT)(MaxPreloadBytes - PreloadedBytes)
				|| PVRPoolAvailable() <= FreeReserve + (size_t)SourceBytes
				|| !PVRPoolCanAlloc( SourceBytes ) )
			{
				++Skipped;
				continue;
			}
			// SetTexture owns validation, upload and the frame-keyed binding.
			// Reset between calls because BindMap::Add may relocate its entries.
			ResetTexture();
			SetTexture( Info, 0, 0.f );
			FTexBind* Loaded = BindMap.Find( Info.CacheID );
			if( Loaded && Loaded->Tex )
			{
				PreloadedBytes += Loaded->SizeBytes;
				++Frames;
			}
		}
		Texture->PrimeCurrent = SavedFrame;
		Texture->TextureFlags = SavedFlags;
	}
	ResetTexture();
	debugf( NAME_Log, "DCANIM preload animations=%i frames=%i resident=%i skipped=%i vram=%u/%uKB free=%uKB reserve=256KB",
		Animations, Frames, AlreadyResident, Skipped, PreloadedBytes / 1024,
		MaxPreloadBytes / 1024, (unsigned)(PVRPoolAvailable() / 1024) );
	TexturePreloadActive = false;
#endif
	unguard;
}

void UPVRRenderDevice::PreloadCookedStaticTextures()
{
	guard(UPVRRenderDevice::PreloadCookedStaticTextures);
#if defined(PLATFORM_DREAMCAST)
	TexturePreloadActive = true;
	// Warm every loaded, non-animated cooked DT once. This moves its disc read
	// and VRAM upload under the loading screen; the normal cache path remains
	// the fallback when VRAM cannot hold all textures.
	const size_t FreeReserve = 64 * 1024;
	INT Candidates = 0, LoadedCount = 0, AlreadyResident = 0, Skipped = 0;
	INT PriorityLoaded = 0;
	DWORD LoadedBytes = 0;
	ResetTexture();
	// These independently cooked effect frames appear late in object order,
	// yet a single explosion can request the whole sequence in successive
	// frames. Give them the first claim on VRAM, then visit all other textures.
	for( INT Pass = 0; Pass < 2; ++Pass )
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		UTexture* Texture = *It;
		if( Texture->Format != TEXF_EXT_DCTEX || Texture->Mips.Num() < 1 )
			continue;
		const UBOOL Priority = Texture->GetParent()
			&& !appStricmp( Texture->GetParent()->GetName(), "Maineffect" );
		if( Priority != (Pass == 0) )
			continue;
		++Candidates;
		FMipmap& Mip = Texture->Mips(0);
		const INT SourceBytes = Mip.DataArray.Num()
			? Mip.DataArray.Num() : ( Mip.StreamData.Size() ? Mip.StreamData.Size() : Mip.DCDataSize );
		if( SourceBytes < 32 )
		{
			++Skipped;
			continue;
		}
		FTextureInfo Info;
		Texture->GetInfo( Info, 0.0 );
		FTexBind* Existing = BindMap.Find( Info.CacheID );
		if( Existing && Existing->Tex )
		{
			++AlreadyResident;
			continue;
		}
		if( PVRPoolAvailable() <= FreeReserve + (size_t)SourceBytes
			|| !PVRPoolCanAlloc( SourceBytes ) )
		{
			++Skipped;
			continue;
		}
		ResetTexture();
		SetTexture( Info, 0, 0.f );
		FTexBind* Loaded = BindMap.Find( Info.CacheID );
		if( Loaded && Loaded->Tex )
		{
			LoadedBytes += Loaded->SizeBytes;
			++LoadedCount;
			if( Priority )
				++PriorityLoaded;
		}
	}
	ResetTexture();
	debugf( NAME_Log, "DCTEX preload candidates=%i loaded=%i priority=%i resident=%i skipped=%i bytes=%uKB free=%uKB reserve=64KB",
		Candidates, LoadedCount, PriorityLoaded, AlreadyResident, Skipped, LoadedBytes / 1024,
		(unsigned)(PVRPoolAvailable() / 1024) );
	TexturePreloadActive = false;
#endif
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
	// A reduced .DT VQ codebook is stored at the end of the hardware's fixed
	// 2 KiB codebook address range. Keep Bind->Tex unadjusted for upload/free;
	// only the address compiled into the polygon header is biased.
	if( Out.Tex && Bind->DCCodebookBytes )
		Out.Tex = (pvr_ptr_t)((BYTE*)Out.Tex - 2048 + Bind->DCCodebookBytes);
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
	MeshBatchTexture = NULL;
	guard(UPVRRenderDevice::SetTexture);

	// Set panning.
	FTexInfo& Tex = TexInfo;
	Tex.UPan      = Info.Pan.X + PanBias*Info.UScale;
	Tex.VPan      = Info.Pan.Y + PanBias*Info.VScale;

	// Account for all the impact on scale normalization.
	Tex.UMult = 1.f / (Info.UScale * static_cast<FLOAT>(Info.USize));
	Tex.VMult = 1.f / (Info.VScale * static_cast<FLOAT>(Info.VSize));

	// The cooked atlas directory is sorted, so this lookup stays logarithmic.
	// Keeping thousands of static tiles out of BindMap is important: UE1's
	// TMap searches that map linearly on every ordinary texture bind.
	if( PreloadedLightmapLevel && Info.Format == TEXF_RGB565
		&& (Info.CacheID & 0xff) == CID_StaticMap && Info.Mips[0]
		&& Info.Mips[0]->DCExternalStream == &PreloadedLightmapLevel->DCLightmapData
		&& (DWORD)(Info.CacheID >> 32) == (DWORD)PreloadedLightmapLevel->Model->GetIndex() )
	{
		const INT LightMap = (INT)((Info.CacheID >> 16) & 0xffff);
		const INT Zone = (INT)((Info.CacheID >> 8) & 0xff);
		const FDCLightmapEntry* Entry = PreloadedLightmapLevel->FindDCLightmap( LightMap, Zone );
		if( Entry )
		{
			const INT Index = Entry - &PreloadedLightmapLevel->DCLightmaps(0);
			const FPreloadedLightmap& Resident = PreloadedLightmaps(Index);
			if( Resident.Page >= 0 )
			{
				PreloadedLightmapBind = { NULL, CID_StaticMap, 0, AtlasPages[Resident.Page].Format,
					Entry->USize, Entry->VSize, 0, TextureFrame, INDEX_NONE, 0,
					Resident.Page, Resident.X, Resident.Y };
				Tex.CurrentCacheID = Info.CacheID;
				Tex.CurrentBind = &PreloadedLightmapBind;
				ApplyAtlasTransform( Tex.CurrentBind );
				return;
			}
		}
	}
	if( PreloadedLightmapLevel && Info.Format == TEXF_RGB565
		&& (Info.CacheID & 0xff) == CID_StaticMap && Info.Mips[0]
		&& Info.Mips[0]->DCExternalStream == &PreloadedLightmapLevel->DCDynamicLightmapAtlasData
		&& (DWORD)(Info.CacheID >> 32) == (DWORD)PreloadedLightmapLevel->Model->GetIndex() )
	{
		const INT LightMap = (INT)((Info.CacheID >> 16) & 0xffff);
		const INT Zone = (INT)((Info.CacheID >> 8) & 0xff);
		const INT Variant = Info.Mips[0]->DCExternalOffset;
		const FDCDynamicLightmapEntry* E = PreloadedLightmapLevel->FindDCDynamicLightmap(LightMap,Zone,Variant);
		const FDCLightmapEntry* Base = PreloadedLightmapLevel->FindDCLightmap(LightMap,Zone);
		if( E && Base && E->Page < PreloadedLightmapLevel->DCDynamicPageResident.Num()
			&& PreloadedLightmapLevel->DCDynamicPageResident(E->Page) )
		{
			const INT PageIndex = PreloadedLightmapLevel->DCLightmapPages.Num() + E->Page;
			if( PageIndex < AtlasPageCount && AtlasPages[PageIndex].Tex )
			{
				PreloadedLightmapBind = { NULL, CID_StaticMap, 0, AtlasPages[PageIndex].Format,
					Base->USize, Base->VSize, 0, TextureFrame, INDEX_NONE, 0,
					PageIndex, E->X, E->Y };
				Tex.CurrentCacheID = Info.CacheID;
				Tex.CurrentBind = &PreloadedLightmapBind;
				ApplyAtlasTransform(Tex.CurrentBind);
				return;
			}
		}
		appErrorf( "Cooked dynamic lightmap lost residency: %i:%i:%i", LightMap, Zone, Variant );
	}

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
	Bind->IsAnimation = Info.Texture && Info.Texture->Format == TEXF_EXT_DCANIM;

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
		FTexState HeaderBefore;
		CaptureTexState( HeaderBefore );
		Bind->LastType = NewType;
		Info.TextureFlags &= ~TF_RealtimeChanged;
		UploadTexture( Info, NewTexture, Masked );

		FTexState HeaderAfter;
		CaptureTexState( HeaderAfter );
		const UBOOL AddressChanged = HeaderBefore.Tex != HeaderAfter.Tex;
		const UBOOL OtherStateChanged = HeaderBefore.Key != HeaderAfter.Key
			|| HeaderBefore.Format != HeaderAfter.Format
			|| HeaderBefore.Width != HeaderAfter.Width
			|| HeaderBefore.Height != HeaderAfter.Height
			|| HeaderBefore.MipMapped != HeaderAfter.MipMapped;
		if( AddressChanged || OtherStateChanged )
		{
			DC_FRAME_COUNT(AddressChanged
				? DCFC_HeaderUploadAddress : DCFC_HeaderUploadState);
			PVRInvalidateHeaders();
		}
		else
		{
			// Rewriting the same VRAM tile changes pixels, not PVR context.
			DC_FRAME_COUNT(DCFC_HeaderUploadStable);
		}
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
void* UPVRRenderDevice::ConvertTextureMipBGRA7777Alpha( const FMipmap* Mip, INT UClamp, INT VClamp )
{
	const INT USize = Max(MinTexSize, Mip->USize);
	const INT VSize = Max(MinTexSize, Mip->VSize);
	const FColor* Pixels = (const FColor*)Mip->DataPtr;
	const DWORD Count = USize * VSize;
	const DWORD StartCycles = appCycles();

	EnsureComposeSize( Count * sizeof(_WORD) );
	_WORD* Dst = (_WORD*)Compose;

	// Source components are 7-bit (BGRA7777); shift down to 4.
	for( INT y = 0; y < VSize; ++y )
	for( INT x = 0; x < USize; ++x )
	{
		// The light manager only fills the clamped rectangle, not POT padding.
		const FColor* Src = Pixels + Min(y * Mip->VSize / VSize, VClamp-1) * Mip->USize
			+ Min(x * Mip->USize / USize, UClamp-1);
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
	pvr_ptr_t Result = PVRPoolMalloc( Size );
	if( Result )
		return Result;
	INT Evicted = 0;
	UBOOL EvictedAnimation = 0;

	// Evict least-recently-used static binds first. Animation frame preloading
	// buys nothing if a later static texture can immediately evict those frames.
	// The pool tests contiguous space, not just the total free-byte count.
	for( INT Tier = 0; Tier < 2 && !Result; ++Tier )
	{
		for( ;; )
		{
			INT Oldest = INDEX_NONE;
			for( INT i = 0; i < BindMap.Size(); ++i )
			{
				const FTexBind& Candidate = BindMap[i];
				if( !Candidate.Tex || &Candidate == TexInfo.CurrentBind
					|| Candidate.LastUsedFrame == TextureFrame
					|| (Candidate.IsAnimation ? 1 : 0) != Tier )
					continue;
				if( Oldest == INDEX_NONE || Candidate.LastUsedFrame < BindMap[Oldest].LastUsedFrame )
					Oldest = i;
			}
			if( Oldest == INDEX_NONE ) break;
			FTexBind& Candidate = BindMap[Oldest];
			PVRPoolFree( Candidate.Tex );
			++Evicted;
			EvictedAnimation |= Candidate.IsAnimation;
			DCFrameCount(DCFC_VRAMEvict);
			if( Candidate.SizeBytes > 0 && VRAMUsed >= (DWORD)Candidate.SizeBytes )
				VRAMUsed -= (DWORD)Candidate.SizeBytes;
			Candidate.Tex = NULL;
			Candidate.SizeBytes = 0;
			Result = PVRPoolMalloc( Size );
			if( Result ) break;
		}
	}

	if( !Result )
		appErrorf( "PVR texture VRAM exhausted: phase=%s request=%i resident=%u free=%u largest=%u",
			TexturePreloadActive ? "preload" : "gameplay", Size, VRAMUsed,
			(unsigned)PVRPoolAvailable(), (unsigned)PVRPoolLargest() );
	if( EvictedAnimation )
		debugf( NAME_Warning, "PVRPOOL evicted animation frame request=%i count=%i free=%uKB largest=%uKB",
			Size, Evicted, (unsigned)(PVRPoolAvailable()/1024), (unsigned)(PVRPoolLargest()/1024) );

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
UBOOL UPVRRenderDevice::LightAtlasPlace( FTexBind* Bind, INT USize, INT VSize, INT MaxPages, UBOOL Reclaim )
{
	DC_FRAME_SCOPE(DCFS_Place);
	// The expanded array also holds immutable VQ variant pages. Preserve the
	// old one-page mutable budget so a busy scene cannot allocate the rest of
	// that array as 128-KiB runtime pages.
	if( Reclaim )
		MaxPages = Min(MaxPages, AtlasDynamicFirstPage + 1);
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
	for( INT Attempt = 0; Attempt < (Reclaim ? 2 : 1); ++Attempt )
	{
		if( Attempt && !LightAtlasReclaim( Bind ) )
			break;

		for( INT Page = Reclaim ? AtlasDynamicFirstPage : 0; Page < MaxPages; ++Page )
		{
			FLightAtlasPage& P = AtlasPages[Page];
			if( !P.Tex )
			{
				// Grow the pool lazily: most maps never need the second page.
				P.Tex = PVRPoolMalloc( AtlasPageDim * AtlasPageDim * sizeof(_WORD) );
				if( !P.Tex )
					break;
				P.SizeBytes = AtlasPageDim * AtlasPageDim * sizeof(_WORD);
				P.Format = PVR_TXRFMT_RGB565;
				appMemset( P.Rows, 0, sizeof(P.Rows) );
				P.SlotsUsed = 0;
				VRAMUsed += AtlasPageDim * AtlasPageDim * sizeof(_WORD);
				AtlasPageCount = Max( AtlasPageCount, Page + 1 );
				debugf( NAME_Log, "PVR: lightmap atlas page %i allocated phase=%s (%i KB)",
					Page, AtlasPreloadActive ? "preload" : "gameplay",
					(INT)(AtlasPageDim * AtlasPageDim * sizeof(_WORD) / 1024) );
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
	check( P.Format == PVR_TXRFMT_RGB565 );
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
			PVRPoolFree( AtlasPages[i].Tex );
			const DWORD Bytes = AtlasPages[i].SizeBytes;
			if( VRAMUsed >= Bytes )
				VRAMUsed -= Bytes;
		}
		AtlasPages[i].Tex = NULL;
		AtlasPages[i].SizeBytes = 0;
		AtlasPages[i].Format = 0;
		appMemset( AtlasPages[i].Rows, 0, sizeof(AtlasPages[i].Rows) );
		AtlasPages[i].SlotsUsed = 0;
	}
	AtlasPageCount = 0;
	AtlasDynamicFirstPage = 0;
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
	const UBOOL IsDTRead = Info.Format == TEXF_EXT_DCTEX;
	const char* DTReadSource = "resident";
	INT DTReadBytes = 0;
	DWORD DTReadCycles = 0;
	const INT ReadStage = IsDTRead ? DCFS_ReadDT : DCFS_ReadTextureOther;
	const INT ReadColdCounter = IsDTRead ? DCFC_ReadDTCold : DCFC_ReadOtherCold;
	const INT ReadReloadCounter = IsDTRead ? DCFC_ReadDTReload : DCFC_ReadOtherReload;
	const INT ReadBytesCounter = IsDTRead ? DCFC_ReadDTBytes : DCFC_ReadTextureOtherBytes;
	if( !Mip0->DataPtr )
	{
		if( Mip0->StreamData.Size() )
		{
			const DWORD ReadStart = IsDTRead && !TexturePreloadActive ? appCycles() : 0;
			DCLoaded = (BYTE*)appMalloc( Mip0->StreamData.Size(), "DatTexStream" );
			{
				DC_FRAME_SCOPE(DCFS_Read);
				FDCFrameScope ReadTextureScope(ReadStage);
				DCFrameCount(NewTexture ? ReadColdCounter : ReadReloadCounter);
				DCFrameCount(ReadBytesCounter, Mip0->StreamData.Size());
				Mip0->StreamData.Read( DCLoaded );
			}
			Mip0->DataPtr = DCLoaded;
			if( ReadStart )
			{
				DTReadSource = "stream";
				DTReadBytes = Mip0->StreamData.Size();
				DTReadCycles = appCycles() - ReadStart;
			}
		}
		else if( Info.Texture && Info.Texture->GetLinker() )
		{
			ULinkerLoad* L = Info.Texture->GetLinker();
			FArchiveFileLoad* FL = (FArchiveFileLoad*)L;
			if( appDCStreamActive() && Mip0->DCDataSize > 0 )
			{
				const DWORD ReadStart = IsDTRead && !TexturePreloadActive ? appCycles() : 0;
				DCLoaded = (BYTE*)appMalloc( Mip0->DCDataSize, "DatTexFirstUse" );
				{
					DC_FRAME_SCOPE(DCFS_Read);
					FDCFrameScope ReadTextureScope(ReadStage);
					DCFrameCount(NewTexture ? ReadColdCounter : ReadReloadCounter);
					DCFrameCount(ReadBytesCounter, Mip0->DCDataSize);
					Mip0->ReadDCData( *FL, DCLoaded );
				}
				Mip0->DataPtr = DCLoaded;
				if( ReadStart )
				{
					DTReadSource = "session";
					DTReadBytes = Mip0->DCDataSize;
					DTReadCycles = appCycles() - ReadStart;
				}
			}
			else if( Mip0->DCDataSize > 0 && Mip0->DCDataOffset > 0 )
			{
				const DWORD ReadStart = IsDTRead && !TexturePreloadActive ? appCycles() : 0;
				DC_FRAME_SCOPE(DCFS_Read);
				FDCFrameScope ReadTextureScope(ReadStage);
				DCFrameCount(NewTexture ? ReadColdCounter : ReadReloadCounter);
				DCFrameCount(ReadBytesCounter, Mip0->DCDataSize);
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
							if( ReadStart )
							{
								DTReadSource = "file";
								DTReadBytes = Mip0->DCDataSize;
								DTReadCycles = appCycles() - ReadStart;
							}
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
			|| HeaderSize >= Size
			|| ((Mode & 0x40000000) && Size - HeaderSize < (Data[10] + 1) * 8)
			|| ((Mode & 0x80000000) && Width != Height) )
		{
			appErrorf( "Unsupported or invalid DT texture header" );
		}

		const INT PayloadSize = Size - HeaderSize;
		if( Bind->Tex && Bind->SizeBytes != PayloadSize )
		{
			PVRPoolFree( Bind->Tex );
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
		#if defined(PLATFORM_DREAMCAST)
		const DWORD UploadStart = !TexturePreloadActive ? appCycles() : 0;
		#endif
		pvr_txr_load( Data + HeaderSize, Bind->Tex, Bind->SizeBytes );
		#if defined(PLATFORM_DREAMCAST)
		if( !TexturePreloadActive )
		{
			const DWORD UploadCycles = appCycles() - UploadStart;
			debugf( NAME_Log,
				"DCDT runtime tex=%s kind=%s animframe=%i cache=%08x:%08x bind=%s read=%s/%i/%.2fms upload=%i/%.2fms vram=%p free=%uKB largest=%uKB",
				Info.Texture ? Info.Texture->GetPathName() : "None",
				Info.Texture && Info.Texture->Format == TEXF_EXT_DCANIM ? "anim" : "static",
				Info.Texture ? (INT)Info.Texture->PrimeCurrent : -1,
				(DWORD)(Info.CacheID >> 32), (DWORD)Info.CacheID,
				NewTexture ? "cold" : "reload", DTReadSource, DTReadBytes,
				DTReadCycles * GSecondsPerCycle * 1000.0,
				Bind->SizeBytes, UploadCycles * GSecondsPerCycle * 1000.0,
				Bind->Tex, (unsigned)(PVRPoolAvailable() / 1024),
				(unsigned)(PVRPoolLargest() / 1024) );
		}
		#endif
		Bind->DCFormat = Mode & 0x7e000000;
		Bind->DCWidth = Width;
		Bind->DCHeight = Height;
		Bind->DCMipMapped = (Mode & 0x80000000) != 0;
		Bind->DCCodebookBytes = (Mode & 0x40000000) && Data[10] != 255
			? (Data[10] + 1) * 8 : 0;
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
				FDCFrameScope ReadLightmapScope(DCFS_ReadLightmap);
				DCFrameCount(NewTexture ? DCFC_ReadLMCold : DCFC_ReadLMReload);
				DCFrameCount(DCFC_ReadLightmapBytes, SizeBytes);
				Mip0->DCExternalStream->ReadRange(Mip0->DCExternalOffset, Pixels, SizeBytes);
			}
		}
		else if( Mip0->DCExternalCodec == 1 )
		{
			const INT PackedSize = Mip0->DCExternalPackedSize;
			BYTE* Packed = (BYTE*)appMalloc( PackedSize, "DCLightmapPacked" );
			{
				DC_FRAME_SCOPE(DCFS_Read);
				FDCFrameScope ReadLightmapScope(DCFS_ReadLightmap);
				DCFrameCount(NewTexture ? DCFC_ReadLMCold : DCFC_ReadLMReload);
				DCFrameCount(DCFC_ReadLightmapBytes, PackedSize);
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
		// A reload of the same tile can update its current atlas address. Only
		// release the reservation when its dimensions really change.
		if( IsAtlased( Bind )
			&& ( Bind->DCWidth != Mip0->USize || Bind->DCHeight != Mip0->VSize ) )
			LightAtlasRelease( Bind );
		const UBOOL KeptSlot = IsAtlased( Bind );

		Bind->DCWidth = Mip0->USize;
		Bind->DCHeight = Mip0->VSize;
		Bind->DCMipMapped = 0;
		Bind->PaletteBank = INDEX_NONE;
		Bind->PaletteMasked = 0;

		// Prefer a shared page. Every tile that lands in one is a texture
		// header the translucent pass no longer has to emit.
		if( KeptSlot || LightAtlasPlace( Bind, Mip0->USize, Mip0->VSize ) )
		{
			if( Bind->Tex )
			{
				PVRPoolFree( Bind->Tex );
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
				PVRPoolFree( Bind->Tex );
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
			PVRPoolFree( Bind->Tex );
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
				PVRPoolFree( Bind->Tex );
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
				PVRPoolFree( Bind->Tex );
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
			PVRPoolFree( Bind->Tex );
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
	else if( (Info.CacheID & 0xFF) == CID_RenderFogMap )
	{
		// Volumetric fog map: ARGB4444 so the coverage alpha survives.
		void* Lin = ConvertTextureMipBGRA7777Alpha( Mip0,
			Clamp(Info.UClamp,1,Mip0->USize), Clamp(Info.VClamp,1,Mip0->VSize) );
		const INT USize = Max(MinTexSize, Mip0->USize);
		const INT VSize = Max(MinTexSize, Mip0->VSize);
		const INT SizeBytes = USize * VSize * 2;
		if( Bind->Tex && Bind->SizeBytes != SizeBytes )
		{
			PVRPoolFree( Bind->Tex );
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
		Bind->DCWidth = USize;
		Bind->DCHeight = VSize;
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
				PVRPoolFree( Bind->Tex );
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
			if( Bind->Tex && Bind->SizeBytes != SizeBytes )
			{
				PVRPoolFree( Bind->Tex );
				if( Bind->SizeBytes > 0 && VRAMUsed >= (DWORD)Bind->SizeBytes )
					VRAMUsed -= (DWORD)Bind->SizeBytes;
				Bind->Tex = NULL;
				Bind->SizeBytes = 0;
			}
			if( !Bind->Tex )
			{
				Bind->Tex = AllocateTexture( SizeBytes );
				if( Bind->Tex )
				{
					Bind->SizeBytes = SizeBytes;
					VRAMUsed += SizeBytes;
				}
			}
			if( Bind->Tex )
				pvr_txr_load( Lin, Bind->Tex, SizeBytes );
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
	(void)Frames;
	appMemset( &TextureCPUProfile, 0, sizeof(TextureCPUProfile) );
}

void UPVRRenderDevice::PrintMemStats() const
{
	// The TA buffer and texture pool occupy different reserved VRAM regions.
	const size_t End = PVR_GET(PVR_TA_VERTBUF_END);
	const size_t Pos = PVR_GET(PVR_TA_VERTBUF_POS);
	const size_t FreeTA = (End > Pos) ? (End - Pos) : 0;
	debugf( "Free TA buffer = %u", (unsigned)FreeTA );
	PVRPoolStats();
	malloc_stats();
}

extern "C" DLL_EXPORT DWORD PVR_GetVRAMUsed()
{
	return GPVRDeviceInstance ? GPVRDeviceInstance->GetVRAMUsed() : 0;
}
