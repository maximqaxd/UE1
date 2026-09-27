#pragma once

#ifndef DC_FRAME_PROFILE
#define DC_FRAME_PROFILE 0
#endif

#ifndef DC_PROFILE_OVERLAY
#define DC_PROFILE_OVERLAY 0
#endif

#if defined(PLATFORM_DREAMCAST)
enum EDCFrameStage
{
	DCFS_Game,
	DCFS_World,
	DCFS_BSP,
	DCFS_BSPBound,
	DCFS_BSPMergeSearch,
	DCFS_BSPDrawList,
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
	DCFS_SpanTest,
	DCFS_SpanUpdate,
	DCFS_SpanCopy,
	DCFS_SpanMerge,
	DCFS_Dynamics,
	DCFS_Read,
	DCFS_ReadDT,
	DCFS_ReadTextureOther,
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
	DCFS_MeshPrepareSetup,
	DCFS_MeshPrepareDecode,
	DCFS_MeshPrepareNormal,
	DCFS_MeshPrepareVisible,
	DCFS_MeshTextureInfo,
	DCFS_MeshLightSetup,
	DCFS_MeshVertexLight,
	DCFS_MeshVertexCollect,
	DCFS_MeshVertexNormal,
	DCFS_MeshVertexLightCall,
	DCFS_MeshVertexFog,
	DCFS_MeshVertexProject,
	DCFS_MeshDraw,
	DCFS_MeshDrawCache,
	DCFS_MeshDrawSetup,
	DCFS_MeshDrawEmit,
	DCFS_MeshDrawFallback,
	DCFS_MeshDrawAttachment,
	DCFS_MeshDrawCleanup,
	DCFS_Count
};
enum EDCFrameCount
{
	DCFC_Nodes, DCFC_Polys, DCFC_Points, DCFC_Spans,
	DCFC_Cold, DCFC_Reload, DCFC_Evict,
	DCFC_VRAMEvict,
	DCFC_ReadDTBytes, DCFC_ReadLightmapBytes,
	DCFC_ReadTextureOtherBytes,
	DCFC_ReadDTCold, DCFC_ReadDTReload, DCFC_ReadLMCold, DCFC_ReadLMReload,
	DCFC_ReadOtherCold, DCFC_ReadOtherReload,
	DCFC_SpanLinks, DCFC_SpanFragments, DCFC_SpanReject,
	DCFC_SpanOutputs, DCFC_SpanScreenSplits, DCFC_SpanBypassed,
	DCFC_ClipReject, DCFC_RasterReject, DCFC_MatrixLoads,
	DCFC_LightCooked, DCFC_LightVQDynamic, DCFC_LightStaticBuild, DCFC_LightStaticHit,
	DCFC_LightStaticMiss, DCFC_LightStaticInvalidated,
	DCFC_LightStaticCookedBypassed, DCFC_LightStaticMoverBuild, DCFC_LightStaticNoCooked,
	DCFC_LightDynamicMiss, DCFC_LightDynamicExpired, DCFC_LightDynamicHit,
	DCFC_LightDynamicPixels, DCFC_LightDynamicLights,
	DCFC_LightMerged,
	DCFC_LightCacheCreate,
	DCFC_UploadMissing, DCFC_UploadChanged, DCFC_UploadPalette,
	DCFC_UploadBank, DCFC_UploadLightmap,
	DCFC_HeaderUploadStable, DCFC_HeaderUploadAddress, DCFC_HeaderUploadState,
	DCFC_MeshActors, DCFC_MeshVerts, DCFC_MeshTris, DCFC_MeshVisible,
	DCFC_MeshStripTris, DCFC_MeshFallbackTris,
	DCFC_MeshPrepareTris, DCFC_MeshPrepareBlocks,
	DCFC_MeshVertexUnique,
	DCFC_MeshLightPairs, DCFC_MeshLightRadiusReject, DCFC_MeshLightEvaluated,
	DCFC_MeshCookedNormals,
	DCFC_MeshVisOutcodeReject, DCFC_MeshVisFacingTest, DCFC_MeshVisBackfaceReject,
	DCFC_MeshVisHardwareCull,
	DCFC_MeshPassSkipped, DCFC_MeshPreparedReuse, DCFC_MeshPoseReuse,
	DCFC_MeshletReject,
	DCFC_MeshLod,
	DCFC_MeshDirectSingles,
	DCFC_HeaderCacheHit, DCFC_HeaderCacheMiss, DCFC_WorldBatchPolys,
	DCFC_Count
};
ENGINE_API void DCFrameCount( INT Counter, DWORD Amount = 1 );
ENGINE_API void DCFrameMeshLightStats( DWORD Pairs, DWORD RadiusRejects, DWORD Evaluated );
extern ENGINE_API UBOOL GDCFrameProfileDetailed;
extern ENGINE_API UBOOL GDCFrameProfileOverlay;
extern ENGINE_API INT GDCFrameProfilePage;
// 0 = original; 1 = no output fragments; 2 = diagnostic opaque-span bypass.
extern ENGINE_API INT GDCSpanMode;
extern ENGINE_API INT GDCStationaryLightHz;
extern ENGINE_API UBOOL GDCMeshOptimize;
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

#if DC_FRAME_PROFILE
class FDCFrameScope
{
	INT Stage;
public:
	explicit FDCFrameScope( INT InStage ) : Stage(InStage) { DCFrameEnter(Stage); }
	~FDCFrameScope() { DCFrameLeave(Stage); }
};
#define DC_FRAME_SCOPE(Stage) FDCFrameScope DCFrameScope(Stage)
#define DC_FRAME_SCOPE_NAMED(Name, Stage) FDCFrameScope Name(Stage)
#define DC_FRAME_COUNT(Counter) DCFrameCount(Counter)
#else
// Discard arguments as well as calls: profiling must not evaluate hot-path work.
#define DCFrameBegin(...) ((void)0)
#define DCFrameEnd(...) ((void)0)
#define DCFrameEnter(...) ((void)0)
#define DCFrameLeave(...) ((void)0)
#define DCFrameCount(...) ((void)0)
#define DCFrameMeshLightStats(...) ((void)0)
#define DCFrameUploadBytes(...) ((void)0)
#define DCFrameHeader(...) ((void)0)
#define DCFrameGPU(...) ((void)0)
#define DCFrameDraw(...) ((void)0)
#define DCFrameProfileReset(...) ((void)0)
class FDCFrameScope { public: explicit FDCFrameScope(INT) {} };
#define DC_FRAME_SCOPE(Stage)
#define DC_FRAME_SCOPE_NAMED(Name, Stage)
#define DC_FRAME_COUNT(Counter)
#endif
#else
#define DC_FRAME_SCOPE(Stage)
#define DC_FRAME_SCOPE_NAMED(Name, Stage)
#define DC_FRAME_COUNT(Counter)
#endif
