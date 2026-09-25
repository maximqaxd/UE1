#include "EnginePrivate.h"
#include "UnDCFrameProfile.h"

#if defined(PLATFORM_DREAMCAST)
#include <arch/timer.h>

// Inclusive stage timers: recursion counts once, nested stages overlap.
ENGINE_API UBOOL GDCFrameProfileEnabled = 1;
ENGINE_API UBOOL GDCFrameProfileDetailed = 1;
ENGINE_API UBOOL GDCFrameProfileOverlay = 1;
ENGINE_API INT GDCFrameProfilePage = 0;
ENGINE_API INT GDCSpanMode = 1;
ENGINE_API INT GDCStationaryLightHz = 5;
ENGINE_API UBOOL GDCMeshOIXActive = 0;
// Use the integer KOS timer directly; single-only floating point loses
// microsecond precision when an absolute timestamp is converted to DOUBLE.
static struct FDCFrameProfile
{
	UBOOL Active;
	DWORD Begin, PreviousBegin, Interval;
	DWORD Start[DCFS_Count], Depth[DCFS_Count], Time[DCFS_Count];
	DWORD StageWorst[DCFS_Count];
	QWORD Sum[DCFS_Count], IntervalSum, WorkSum, BytesSum, HeadersSum;
	DWORD Frames, Worst, Bytes, Headers;
	FLOAT Display[DCFS_Count], FrameMS, WorkMS, WorstMS, UploadKB, HeaderCount;
	DWORD GPUFrame, GPUVertexBytes;
	FLOAT GPUMS;
	DWORD Counts[DCFC_Count];
	QWORD CountSum[DCFC_Count];
	FLOAT CountDisplay[DCFC_Count];
	DWORD TimerReads;
	QWORD TimerReadsSum;
	char Lines[36][128];
} GDCFrame;

ENGINE_API void DCFrameProfileReset()
{
	appMemset(&GDCFrame, 0, sizeof(GDCFrame));
}

ENGINE_API void DCFrameProfileReport( FOutputDevice* Out )
{
	Out->Logf("DCPROFILE detail=%d overlay=%d legacy=%d TA=SQ mesh_OIX=%d; completed 30-tick window",
		GDCFrameProfileDetailed, GDCFrameProfileOverlay, GDCLegacyTimers,
		GDCMeshOIXActive);
	for( INT i = 0; i < 36; ++i )
		if( GDCFrame.Lines[i][0] ) Out->Log(GDCFrame.Lines[i]);
}

static UBOOL DCStageEnabled( INT Stage )
{
	return GDCFrameProfileDetailed || Stage == DCFS_Game || Stage == DCFS_World
		|| Stage == DCFS_BSP || Stage == DCFS_Wait || Stage == DCFS_Submit
		|| Stage == DCFS_Overlay;
}

ENGINE_API void DCFrameBegin()
{
	if( !GDCFrameProfileEnabled )
	{
		appMemset(&GDCFrame, 0, sizeof(GDCFrame));
		return;
	}
	const DWORD Now = (DWORD)timer_us_gettime64();
	GDCFrame.Interval = GDCFrame.PreviousBegin ? Now - GDCFrame.PreviousBegin : 0;
	GDCFrame.PreviousBegin = Now;
	GDCFrame.Begin = Now;
	appMemset(GDCFrame.Time, 0, sizeof(GDCFrame.Time));
	appMemset(GDCFrame.Depth, 0, sizeof(GDCFrame.Depth));
	GDCFrame.Bytes = GDCFrame.Headers = 0;
	GDCFrame.TimerReads = 0;
	appMemset(GDCFrame.Counts, 0, sizeof(GDCFrame.Counts));
	GDCFrame.Active = 1;
}

ENGINE_API void DCFrameEnter( INT Stage )
{
	if( GDCFrame.Active && DCStageEnabled(Stage) && GDCFrame.Depth[Stage]++ == 0 )
	{
		++GDCFrame.TimerReads;
		GDCFrame.Start[Stage] = (DWORD)timer_us_gettime64();
	}
}

ENGINE_API void DCFrameLeave( INT Stage )
{
	if( GDCFrame.Active && GDCFrame.Depth[Stage] && --GDCFrame.Depth[Stage] == 0 )
	{
		++GDCFrame.TimerReads;
		GDCFrame.Time[Stage] += (DWORD)timer_us_gettime64() - GDCFrame.Start[Stage];
	}
}

ENGINE_API void DCFrameUploadBytes( DWORD Bytes ) { if( GDCFrame.Active ) GDCFrame.Bytes += Bytes; }
ENGINE_API void DCFrameHeader() { if( GDCFrame.Active ) ++GDCFrame.Headers; }
ENGINE_API void DCFrameCount( INT Counter, DWORD Amount )
{
	if( GDCFrame.Active && GDCFrameProfileDetailed ) GDCFrame.Counts[Counter] += Amount;
}
ENGINE_API void DCFrameMeshLightStats( DWORD Pairs, DWORD RadiusRejects, DWORD Evaluated )
{
	if( GDCFrame.Active && GDCFrameProfileDetailed )
	{
		GDCFrame.Counts[DCFC_MeshLightPairs] += Pairs;
		GDCFrame.Counts[DCFC_MeshLightRadiusReject] += RadiusRejects;
		GDCFrame.Counts[DCFC_MeshLightEvaluated] += Evaluated;
	}
}

ENGINE_API void DCFrameGPU( DWORD Frame, QWORD Nanoseconds, DWORD VertexBytes )
{
	if( Frame != GDCFrame.GPUFrame )
	{
		GDCFrame.GPUFrame = Frame;
		GDCFrame.GPUMS = Nanoseconds / 1000000.f;
		GDCFrame.GPUVertexBytes = VertexBytes;
	}
}

ENGINE_API void DCFrameEnd()
{
	if( !GDCFrame.Active ) return;
	GDCFrame.Active = 0;
	const DWORD Work = (DWORD)timer_us_gettime64() - GDCFrame.Begin;
	for( INT i = 0; i < DCFS_Count; ++i )
	{
		GDCFrame.Sum[i] += GDCFrame.Time[i];
		GDCFrame.StageWorst[i] = Max(GDCFrame.StageWorst[i], GDCFrame.Time[i]);
	}
	GDCFrame.WorkSum += Work;
	GDCFrame.IntervalSum += GDCFrame.Interval;
	GDCFrame.Worst = Max(GDCFrame.Worst, GDCFrame.Interval);
	GDCFrame.BytesSum += GDCFrame.Bytes;
	GDCFrame.HeadersSum += GDCFrame.Headers;
	GDCFrame.TimerReadsSum += GDCFrame.TimerReads;
	for( INT i = 0; i < DCFC_Count; ++i ) GDCFrame.CountSum[i] += GDCFrame.Counts[i];
	if( ++GDCFrame.Frames >= 30 )
	{
		const FLOAT Scale = 1.f / (1000.f * GDCFrame.Frames);
		for( INT i = 0; i < DCFS_Count; ++i )
		{
			GDCFrame.Display[i] = GDCFrame.Sum[i] * Scale;
			GDCFrame.Sum[i] = 0;
		}
		GDCFrame.FrameMS = GDCFrame.IntervalSum * Scale;
		GDCFrame.WorkMS = GDCFrame.WorkSum * Scale;
		GDCFrame.WorstMS = GDCFrame.Worst * 0.001f;
		GDCFrame.UploadKB = GDCFrame.BytesSum / (1024.f * GDCFrame.Frames);
		GDCFrame.HeaderCount = GDCFrame.HeadersSum / (FLOAT)GDCFrame.Frames;
		for( INT i = 0; i < DCFC_Count; ++i )
		{
			GDCFrame.CountDisplay[i] = GDCFrame.CountSum[i] / (FLOAT)GDCFrame.Frames;
			GDCFrame.CountSum[i] = 0;
		}
		const FLOAT* T = GDCFrame.Display;
		const FLOAT* C = GDCFrame.CountDisplay;
		appSprintf(GDCFrame.Lines[0], "DC %.1f FPS frame %.1f worst %.1f D%d TA:SQ M-OIX:%s",
			GDCFrame.FrameMS > 0 ? 1000.f/GDCFrame.FrameMS : 0.f,
			GDCFrame.FrameMS, GDCFrame.WorstMS, GDCFrameProfileDetailed,
			GDCMeshOIXActive ? "ON" : "OFF");
		appSprintf(GDCFrame.Lines[1], "tick %.1f game %.1f world %.1f wait %.1f", GDCFrame.WorkMS, T[DCFS_Game], T[DCFS_World], T[DCFS_Wait]);
		appSprintf(GDCFrame.Lines[2], "BSP %.1f clip %.1f raster %.1f span %.1f", T[DCFS_BSP], T[DCFS_Clip], T[DCFS_Raster], T[DCFS_Span]);
		appSprintf(GDCFrame.Lines[3], "dyn %.1f mesh %.1f light %.1f legacy %d", T[DCFS_Dynamics], T[DCFS_Mesh], T[DCFS_Light], GDCLegacyTimers);
		appSprintf(GDCFrame.Lines[4], "nodes %.0f polys %.0f pts %.0f rows %.0f", C[DCFC_Nodes], C[DCFC_Polys], C[DCFC_Points], C[DCFC_Spans]);
		appSprintf(GDCFrame.Lines[5], "tex %.1f read %.1f DT %.1f/%.1fK LM %.1f/%.1fK O %.1f/%.1fK",
			T[DCFS_Texture], T[DCFS_Read],
			T[DCFS_ReadDT], C[DCFC_ReadDTBytes] / 1024.f,
			T[DCFS_ReadLightmap], C[DCFC_ReadLightmapBytes] / 1024.f,
			T[DCFS_ReadTextureOther], C[DCFC_ReadTextureOtherBytes] / 1024.f);
		appSprintf(GDCFrame.Lines[6], "place %.1f twid+SQ %.1f SQ %.1f %.1fKB", T[DCFS_Place], T[DCFS_Twiddle], T[DCFS_Upload], GDCFrame.UploadKB);
		appSprintf(GDCFrame.Lines[7], "cold %.0f reload %.0f atlasEv %.0f vramEv %.0f hdr %.0f",
			C[DCFC_Cold], C[DCFC_Reload], C[DCFC_Evict], C[DCFC_VRAMEvict], GDCFrame.HeaderCount);
		appSprintf(GDCFrame.Lines[8], "submit %.1f overlay %.2f GPUlast %.1f", T[DCFS_Submit], T[DCFS_Overlay], GDCFrame.GPUMS);
		appSprintf(GDCFrame.Lines[9], "VIS frame %.1f detail %d spanmode %d BSP other~ %.1f",
			GDCFrame.FrameMS, GDCFrameProfileDetailed, GDCSpanMode,
			Max(0.f, T[DCFS_BSP] - T[DCFS_Clip] - T[DCFS_Raster] - T[DCFS_Span]));
		appSprintf(GDCFrame.Lines[10], "transform %.2f clip-only %.2f", T[DCFS_Transform], T[DCFS_Clip]);
		appSprintf(GDCFrame.Lines[11], "span test %.2f update %.2f copy %.2f merge %.2f",
			T[DCFS_SpanTest], T[DCFS_SpanUpdate], T[DCFS_SpanCopy], T[DCFS_SpanMerge]);
		appSprintf(GDCFrame.Lines[12], "matrix loads %.0f transformed pts %.0f", C[DCFC_MatrixLoads], C[DCFC_Points]);
		appSprintf(GDCFrame.Lines[13], "span links %.0f out %.0f split %.0f bypass %.0f",
			C[DCFC_SpanLinks], C[DCFC_SpanOutputs], C[DCFC_SpanScreenSplits], C[DCFC_SpanBypassed]);
		appSprintf(GDCFrame.Lines[14], "reject clip %.0f raster %.0f span %.0f", C[DCFC_ClipReject], C[DCFC_RasterReject], C[DCFC_SpanReject]);
		appSprintf(GDCFrame.Lines[15], "BSP bound %.2f merge %.2f drawlist %.2f timer %.0f",
			T[DCFS_BSPBound], T[DCFS_BSPMergeSearch], T[DCFS_BSPDrawList],
			GDCFrame.TimerReadsSum / (FLOAT)GDCFrame.Frames);
		appSprintf(GDCFrame.Lines[16], "hdr %.0f compile %.2fms stable %.0f addr %.0f state %.0f",
			GDCFrame.HeaderCount, T[DCFS_HeaderCompile],
			C[DCFC_HeaderUploadStable], C[DCFC_HeaderUploadAddress],
			C[DCFC_HeaderUploadState]);
		appSprintf(GDCFrame.Lines[17], "READ peak DT %.2f LM %.2f other %.2f ms",
			GDCFrame.StageWorst[DCFS_ReadDT] * 0.001f,
			GDCFrame.StageWorst[DCFS_ReadLightmap] * 0.001f,
			GDCFrame.StageWorst[DCFS_ReadTextureOther] * 0.001f);
		appSprintf(GDCFrame.Lines[18], "LIGHT frame %.1f setup %.2f tex %.2f rate %dHz", GDCFrame.FrameMS, T[DCFS_Light], T[DCFS_Texture], GDCStationaryLightHz);
		appSprintf(GDCFrame.Lines[19], "cooked %.0f vq-dyn %.0f static build %.0f hit %.0f bypass %.0f mover %.0f no %.0f",
			C[DCFC_LightCooked], C[DCFC_LightVQDynamic], C[DCFC_LightStaticBuild], C[DCFC_LightStaticHit],
			C[DCFC_LightStaticCookedBypassed], C[DCFC_LightStaticMoverBuild],
			C[DCFC_LightStaticNoCooked]);
		appSprintf(GDCFrame.Lines[20], "dynamic miss %.0f expired %.0f hit %.0f", C[DCFC_LightDynamicMiss], C[DCFC_LightDynamicExpired], C[DCFC_LightDynamicHit]);
		appSprintf(GDCFrame.Lines[21], "merged regen %.0f lightmap uploads %.0f", C[DCFC_LightMerged], C[DCFC_UploadLightmap]);
		appSprintf(GDCFrame.Lines[22], "upload cold %.0f missing %.0f", C[DCFC_Cold], C[DCFC_UploadMissing]);
		appSprintf(GDCFrame.Lines[23], "upload changed %.0f palette %.0f bank %.0f", C[DCFC_UploadChanged], C[DCFC_UploadPalette], C[DCFC_UploadBank]);
		appSprintf(GDCFrame.Lines[24], "read DT %.0f/%.0f LM %.0f/%.0f other %.0f/%.0f ev %.0f",
			C[DCFC_ReadDTCold], C[DCFC_ReadDTReload],
			C[DCFC_ReadLMCold], C[DCFC_ReadLMReload],
			C[DCFC_ReadOtherCold], C[DCFC_ReadOtherReload], C[DCFC_VRAMEvict]);
		appSprintf(GDCFrame.Lines[25], "build static %.2f/%.1f dyn %.2f/%.1f pix %.0f lights %.0f",
			T[DCFS_LightStaticBuild], GDCFrame.StageWorst[DCFS_LightStaticBuild] * 0.001f,
			T[DCFS_LightDynamicBuild], GDCFrame.StageWorst[DCFS_LightDynamicBuild] * 0.001f,
			C[DCFC_LightDynamicPixels], C[DCFC_LightDynamicLights]);
		appSprintf(GDCFrame.Lines[26], "static miss %.0f invalid %.0f cache create %.0f/%.2fms",
			C[DCFC_LightStaticMiss], C[DCFC_LightStaticInvalidated],
			C[DCFC_LightCacheCreate], T[DCFS_LightCacheCreate]);
		appSprintf(GDCFrame.Lines[27], "MESH total %.2f frame %.2f outcode %.2f", T[DCFS_Mesh], T[DCFS_MeshFrame], T[DCFS_MeshOutcode]);
		appSprintf(GDCFrame.Lines[28], "prepare %.2f texture info %.2f", T[DCFS_MeshPrepare], T[DCFS_MeshTextureInfo]);
		appSprintf(GDCFrame.Lines[29], "light setup %.2f vertex %.2f", T[DCFS_MeshLightSetup], T[DCFS_MeshVertexLight]);
		appSprintf(GDCFrame.Lines[30], "draw+submit %.2f unmeasured %.2f", T[DCFS_MeshDraw],
			Max(0.f, T[DCFS_Mesh] - T[DCFS_MeshFrame] - T[DCFS_MeshOutcode]
				- T[DCFS_MeshPrepare] - T[DCFS_MeshTextureInfo]
				- T[DCFS_MeshLightSetup] - T[DCFS_MeshVertexLight] - T[DCFS_MeshDraw]));
		appSprintf(GDCFrame.Lines[31], "actors %.0f verts %.0f tris %.0f", C[DCFC_MeshActors], C[DCFC_MeshVerts], C[DCFC_MeshTris]);
		appSprintf(GDCFrame.Lines[32], "visible %.0f strip %.0f fallback %.0f cover %.0f%%",
			C[DCFC_MeshVisible], C[DCFC_MeshStripTris], C[DCFC_MeshFallbackTris],
			100.f * C[DCFC_MeshStripTris] / Max(1.f, C[DCFC_MeshStripTris] + C[DCFC_MeshFallbackTris]));
		appSprintf(GDCFrame.Lines[33], "prep %.2f dec %.2f norm %.2f vis %.2f out %.0f face %.0f back %.0f hw %.0f",
			T[DCFS_MeshPrepareSetup], T[DCFS_MeshPrepareDecode],
			T[DCFS_MeshPrepareNormal], T[DCFS_MeshPrepareVisible],
			C[DCFC_MeshVisOutcodeReject], C[DCFC_MeshVisFacingTest],
			C[DCFC_MeshVisBackfaceReject], C[DCFC_MeshVisHardwareCull]);
		appSprintf(GDCFrame.Lines[34], "vert gather %.2f normal %.2f light %.2f fog %.2f proj %.2f",
			T[DCFS_MeshVertexCollect], T[DCFS_MeshVertexNormal],
			T[DCFS_MeshVertexLightCall], T[DCFS_MeshVertexFog], T[DCFS_MeshVertexProject]);
		appSprintf(GDCFrame.Lines[35], "unique %.0f light pairs %.0f reject %.0f eval %.0f cooked %.0f",
			C[DCFC_MeshVertexUnique], C[DCFC_MeshLightPairs],
			C[DCFC_MeshLightRadiusReject], C[DCFC_MeshLightEvaluated],
			C[DCFC_MeshCookedNormals]);
		appMemset(GDCFrame.StageWorst, 0, sizeof(GDCFrame.StageWorst));
		GDCFrame.TimerReadsSum = 0;
		GDCFrame.IntervalSum = GDCFrame.WorkSum = GDCFrame.BytesSum = GDCFrame.HeadersSum = 0;
		GDCFrame.Frames = GDCFrame.Worst = 0;
	}
}

ENGINE_API void DCFrameDraw( UCanvas* Canvas )
{
	if( !GDCFrameProfileEnabled || !GDCFrameProfileOverlay || !Canvas || !Canvas->SmallFont ) return;
	DC_FRAME_SCOPE(DCFS_Overlay);
	for( INT i = 0; i < 9; ++i )
		Canvas->Printf(Canvas->SmallFont, 4, 24 + i * 10, "%s", GDCFrame.Lines[GDCFrameProfilePage * 9 + i]);
}

#endif
