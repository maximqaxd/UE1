/*------------------------------------------------------------------------------------
	Dependencies.
------------------------------------------------------------------------------------*/

#include <dc/pvr.h>
#include "RenderPrivate.h"

/*------------------------------------------------------------------------------------
	PVR rendering private definitions.
------------------------------------------------------------------------------------*/

//
// Fixed function PVR renderer for the Dreamcast.
//
// Submission model: the opaque list is opened in Lock() and written straight
// to the store queues.  Punch-through and translucent primitives are appended
// to two flat arenas of finished 32-byte TA payloads and replayed in Unlock(),
// which is what lets the HUD (drawn after the world) reach both lists without
// any per-primitive allocation.
//
class DLL_EXPORT UPVRRenderDevice : public URenderDevice
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT(UPVRRenderDevice, URenderDevice, CLASS_Config)

	static constexpr INT MaxMipLevel = 0;
	static constexpr INT MinTexSize = 8;

	// Options.
	UBOOL NoFiltering;
	UBOOL UseTriStrips;
	UBOOL DistanceFog;
	UBOOL VolumetricFog;
	INT   FogDistanceDefault;
	INT   CommandBufferKB;

    // All currently cached textures (CacheID -> VRAM ptr + last type).
	struct FTexBind
	{
        pvr_ptr_t Tex;
		BYTE LastType;
		INT  SizeBytes;
		DWORD DCFormat;
		INT DCWidth;
		INT DCHeight;
		UBOOL DCMipMapped;
		DWORD LastUsedFrame;
		INT PaletteBank;
		UBOOL PaletteMasked;
	};
	TMap<QWORD, FTexBind> BindMap;

	struct FPaletteBank
	{
		QWORD CacheID;
		UBOOL Masked;
		DWORD LastUsedFrame;
		DWORD LastUploadFrame;
	};
	FPaletteBank PaletteBanks[4];

	struct FTexInfo
	{
		QWORD CurrentCacheID;
		FTexBind* CurrentBind;
		FLOAT UMult;
		FLOAT VMult;
		FLOAT UPan;
		FLOAT VPan;
		UBOOL bIsTile;
	} TexInfo;

	// A snapshot of TexInfo, so a surface can hold on to its base texture
	// while the lightmap is bound for the same set of polygons.  Held by
	// value: TMap::Add grows a TArray, so an FTexBind* taken before the
	// second SetTexture can dangle.
	struct FTexState
	{
		pvr_ptr_t Tex;
		QWORD Key;              // texture CacheID; stable identity for the header cache
		DWORD Format;
		INT   Width, Height;
		UBOOL MipMapped;
		FLOAT UMult, VMult, UPan, VPan;
	};

	// Texture upload buffer;
	BYTE* Compose;
	DWORD ComposeSize;

	FLOAT RProjZ, Aspect;
	FLOAT RFX2, RFY2;
	FPlane ColorMod;

	// Screen-space overlays sort above every world vertex. The band is derived
	// from Frame->Proj.Z, which is the largest 1/w a world vertex can carry
	// (it is reached exactly at the near plane), so it tracks field of view.
	FLOAT OverlayZFlash;
	FLOAT OverlayZUI;
	FLOAT UIZStep;
	FLOAT UIZCursor;

	// Hardware fog state for the viewer's zone.
	UBOOL FogActive;
	DWORD VRAMUsed;
	DWORD TextureFrame;

	struct FTextureCPUProfile
	{
		DWORD PaletteCalls;
		DWORD PaletteCycles;
		DWORD P8TwiddleCalls;
		DWORD P8TwiddlePixels;
		DWORD P8TwiddleCycles;
		DWORD P8ConvertCalls;
		DWORD P8ConvertPixels;
		DWORD P8ConvertCycles;
		DWORD PaletteBankFallbacks;
		DWORD GenericP8Conversions;
		DWORD MinSizeExpansions;
		DWORD MinSizeSourcePixels;
		DWORD MinSizeOutputPixels;
		DWORD LightmapCalls;
		DWORD LightmapPixels;
		DWORD LightmapCycles;
		DWORD DynamicLightmapCalls;
		DWORD DynamicLightmapPixels;
		DWORD StaticLightmapCalls;
		DWORD StaticLightmapPixels;
		DWORD StaticLightmapCold;
		DWORD StaticLightmapReload;
		DWORD StaticLightmapRetype;
	} TextureCPUProfile;

	struct FCachedSceneNode
	{
		FLOAT FovAngle;
		FLOAT FX, FY;
		INT X, Y;
		INT XB, YB;
		INT SizeX, SizeY;
		UBOOL bIsSky;
	} CurrentSceneNode;

	// Constructors.
	UPVRRenderDevice();
	static void InternalClassInitializer( UClass* Class );

	// URenderDevice interface.
	virtual UBOOL Init( UViewport* InViewport ) override;
	virtual void Exit() override;
	virtual void Flush() override;
	virtual UBOOL Exec( const char* Cmd, FOutputDevice* Out ) override;
	virtual void Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize ) override;
	virtual void Unlock( UBOOL Blit ) override;
	virtual void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet ) override;
	virtual void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer ) override;
	virtual void DrawGouraudTriStrip( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer ) override;
	virtual void DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags ) override;
	virtual void EndFlash() override;
	virtual void GetStats( char* Result ) override;
	virtual void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 ) override;
	virtual void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 ) override;
	virtual void PushHit( const BYTE* Data, INT Count ) override;
	virtual void PopHit( INT Count, UBOOL bForce ) override;
	virtual void ReadPixels( FColor* Pixels ) override;
	virtual void ClearZ( FSceneNode* Frame ) override;

	// UPVRRenderDevice interface.
	void SetSceneNode( FSceneNode* Frame );
	void UpdateFog( FSceneNode* Frame );
	void SetTexture( FTextureInfo& Info, DWORD PolyFlags, FLOAT PanBias );
	void CaptureTexState( FTexState& Out ) const;
	void ResetTexture( );
	void UploadTexture( FTextureInfo& Info, UBOOL NewTexture, UBOOL Masked );
	INT AcquirePaletteBank( const FTextureInfo& Info, UBOOL Masked );
	void UploadPalette( INT Bank, const FTextureInfo& Info, UBOOL Masked );
	pvr_ptr_t AllocateTexture( INT Size );
	void EnsureComposeSize( const DWORD NewSize );
	void* TwiddleTextureMipP8( const FMipmap* Mip );
	void* ConvertTextureMipI8( const FMipmap* Mip, const FColor* Palette );
	void* ConvertTextureMipBGRA7777( const FMipmap* Mip );
	void* ConvertTextureMipBGRA7777Alpha( const FMipmap* Mip );
	void* VerticalUpscale( const INT USize, const INT VSize, const INT VTimes );
	void PrintTextureCPUProfile( INT Frames );
	void PrintMemStats() const;

	// Primitive emission.
	pvr_list_t ListFor( DWORD PolyFlags ) const;
	DWORD AdjustFlags( DWORD PolyFlags ) const;
	void EmitHeader( pvr_list_t List, DWORD PolyFlags, const FTexState* Tex, UBOOL NoDepth );

public:
	// Queryors
	DWORD GetVRAMUsed() const { return VRAMUsed; }
};
