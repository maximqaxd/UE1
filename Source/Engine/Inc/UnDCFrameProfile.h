#pragma once

#if defined(PLATFORM_DREAMCAST)
enum EDCFrameStage
{
	DCFS_Game,
	DCFS_World,
	DCFS_BSP,
	DCFS_Mesh,
	DCFS_Light,
	DCFS_Texture,
	DCFS_Upload,
	DCFS_Wait,
	DCFS_Submit,
	DCFS_Overlay,
	DCFS_Clip,
	DCFS_Raster,
	DCFS_Span,
	DCFS_Dynamics,
	DCFS_Read,
	DCFS_ReadDT,
	DCFS_ReadLightmap,
	DCFS_Inflate,
	DCFS_Allocate,
	DCFS_Place,
	DCFS_Twiddle,
	DCFS_Transform,
	DCFS_LightStaticBuild,
	DCFS_LightDynamicBuild,
	DCFS_LightCacheCreate,
	DCFS_HeaderCompile,
	DCFS_MeshFrame,
	DCFS_MeshOutcode,
	DCFS_MeshPrepare,
	DCFS_MeshTextureInfo,
	DCFS_MeshLightSetup,
	DCFS_MeshVertexLight,
	DCFS_MeshDraw,
	DCFS_Count
};
enum EDCFrameCount
{
	DCFC_Nodes, DCFC_Polys, DCFC_Points, DCFC_Spans,
	DCFC_Cold, DCFC_Reload, DCFC_Evict,
	DCFC_VRAMEvict,
	DCFC_ReadDTBytes, DCFC_ReadLightmapBytes,
	DCFC_SpanLinks, DCFC_SpanFragments, DCFC_SpanReject,
	DCFC_SpanOutputs, DCFC_SpanScreenSplits, DCFC_SpanBypassed,
	DCFC_ClipReject, DCFC_RasterReject, DCFC_MatrixLoads,
	DCFC_LightCooked, DCFC_LightStaticBuild, DCFC_LightStaticHit,
	DCFC_LightStaticMiss, DCFC_LightStaticInvalidated,
	DCFC_LightDynamicMiss, DCFC_LightDynamicExpired, DCFC_LightDynamicHit,
	DCFC_LightMerged,
	DCFC_LightCacheCreate,
	DCFC_UploadMissing, DCFC_UploadChanged, DCFC_UploadPalette,
	DCFC_UploadBank, DCFC_UploadLightmap,
	DCFC_HeaderUploadStable, DCFC_HeaderUploadAddress, DCFC_HeaderUploadState,
	DCFC_MeshActors, DCFC_MeshVerts, DCFC_MeshTris, DCFC_MeshVisible,
	DCFC_MeshStripTris, DCFC_MeshFallbackTris,
	DCFC_Count
};
ENGINE_API void DCFrameCount( INT Counter, DWORD Amount = 1 );
extern ENGINE_API UBOOL GDCFrameProfileDetailed;
extern ENGINE_API UBOOL GDCFrameProfileOverlay;
extern ENGINE_API INT GDCFrameProfilePage;
// 0 = original; 1 = no output fragments; 2 = diagnostic opaque-span bypass.
extern ENGINE_API INT GDCSpanMode;
extern ENGINE_API INT GDCStationaryLightHz;
// Set only while a mesh actually uses the on-chip OIX work area.
extern ENGINE_API UBOOL GDCMeshOIXActive;
ENGINE_API void DCFrameProfileReset();
ENGINE_API void DCFrameProfileReport( FOutputDevice* Out );

ENGINE_API void DCFrameBegin();
ENGINE_API void DCFrameEnd();
ENGINE_API void DCFrameEnter( INT Stage );
ENGINE_API void DCFrameLeave( INT Stage );
ENGINE_API void DCFrameDraw( UCanvas* Canvas );
ENGINE_API void DCFrameUploadBytes( DWORD Bytes );
ENGINE_API void DCFrameHeader();
ENGINE_API void DCFrameGPU( DWORD Frame, QWORD Nanoseconds, DWORD VertexBytes );
extern ENGINE_API UBOOL GDCFrameProfileEnabled;

class FDCFrameScope
{
	INT Stage;
public:
	explicit FDCFrameScope( INT InStage ) : Stage(InStage) { DCFrameEnter(Stage); }
	~FDCFrameScope() { DCFrameLeave(Stage); }
};
#define DC_FRAME_SCOPE(Stage) FDCFrameScope DCFrameScope(Stage)
#define DC_FRAME_COUNT(Counter) DCFrameCount(Counter)
#else
#define DC_FRAME_SCOPE(Stage)
#define DC_FRAME_COUNT(Counter)
#endif
