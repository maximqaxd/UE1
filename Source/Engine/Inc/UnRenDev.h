/*=============================================================================
	UnRenDev.h: 3D rendering device class.

	Copyright 1995 Epic MegaGames, Inc.
	Compiled with Visual C++ 4.0. Best viewed with Tabs=4.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

/*------------------------------------------------------------------------------------
	URenderDevice.
------------------------------------------------------------------------------------*/

// Flags for locking a rendering device.
enum ELockRenderFlags
{
	LOCKR_ClearScreen	= 1,
	LOCKR_LightDiminish = 2,
};

//
// A low-level 3D rendering device.
//
class ENGINE_API URenderDevice : public USubsystem
{
	DECLARE_ABSTRACT_CLASS(URenderDevice,USubsystem,CLASS_Config)

	// Variables.
	UViewport*	Viewport;
	UBOOL		SpanBased;
	UBOOL		FrameBuffered;
	UBOOL		SupportsFogMaps;
	UBOOL		SupportsDistanceFog;
	UBOOL		VolumetricLighting;
	UBOOL		ShinySurfaces;
	UBOOL		Coronas;
	UBOOL		HighDetailActors;
	UBOOL		NoVolumetricBlend;

	// Set by drivers that can consume a triangle strip directly. Meshes are
	// already strip-cooked offline (UMesh::DCRuns); when this is false the
	// renderer expands them back into individual triangles.
	UBOOL		SupportsTriStrips;

	// Constructors.
	static void InternalClassInitializer( UClass* Class );

	// URenderDevice low-level functions that drivers must implement.
	virtual UBOOL Init( UViewport* InViewport )=0;
	virtual void Exit()=0;
	virtual void Flush()=0;
	virtual UBOOL Exec( const char* Cmd, FOutputDevice* Out )=0;
	// Optional loading-screen warmup for frame-keyed cooked animations.
	virtual void PreloadCookedAnimationFrames() {}
	virtual void PreloadCookedStaticTextures() {}
#if defined(PLATFORM_DREAMCAST)
	virtual void PreloadCookedLightmaps( ULevel* Level ) {}
#endif
	virtual void Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize )=0;
	virtual void Unlock( UBOOL Blit )=0;
#if defined(PLATFORM_DREAMCAST)
	// A direct-list device renders one occluded world in OP, PT, TR order.
	virtual UBOOL UsesOrderedLists() const { return 0; }
	// True only when eligible one-sided mesh faces can be rejected by the TA.
	virtual UBOOL UsesHardwareMeshCulling() const { return 0; }
	virtual void BeginRenderPass( INT Pass ) {}
	virtual UBOOL WantsBspSurface( DWORD PolyFlags, INT Pass ) const { return 1; }
	virtual UBOOL WantsMeshFlags( DWORD PolyFlags ) const { return 1; }
#endif
	virtual void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )=0;
	virtual void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span )=0;

	// Draw NumPts vertices as one triangle strip. Pts[0..2] form the first
	// triangle in the order given; each further vertex closes one more
	// triangle with alternating winding, as in OpenGL's GL_TRIANGLE_STRIP.
	// The default implementation fans the strip out into DrawGouraudPolygon
	// calls, so drivers only override it if SupportsTriStrips is set.
	virtual void DrawGouraudTriStrip( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* Span );
#if defined(PLATFORM_DREAMCAST)
	// Optional cooked-mesh submission phase. Meshlet ids are local to one mesh.
	virtual void BeginCookedMesh() {}
	virtual void EndCookedMesh() {}
	virtual UBOOL DrawCookedMeshStrip( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* Span, INT MeshletId ) { return 0; }
	virtual UBOOL DrawIndexedMeshStrip( FSceneNode* Frame, FTextureInfo& Info, const FTransTexture* Samples, const _WORD* Indices, const _WORD* UVs, INT Count, UBOOL OddStart, DWORD PolyFlags ) { return 0; }
#endif
	virtual void DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags )=0;
	virtual void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )=0;
	virtual void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 )=0;
	virtual void ClearZ( FSceneNode* Frame )=0;
	virtual void PushHit( const BYTE* Data, INT Count )=0;
	virtual void PopHit( INT Count, UBOOL bForce )=0;
	virtual void GetStats( char* Result )=0;
	virtual void ReadPixels( FColor* Pixels )=0;
	virtual void EndFlash() {};
};

/*------------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------------*/
