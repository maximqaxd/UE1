/*=============================================================================
	Model.cpp: DCUtil model processing functions
	
	This tool compresses LightBits in .unr map packages using RLE compression
	to reduce memory usage on Dreamcast. The compressed format stores:
	- 4-byte header with original uncompressed size
	- RLE-encoded byte sequences with run-length encoding
	
	Note: The engine's UModel::Serialize method needs to be modified to
	decompress LightBits when loading packages. See UnModel.cpp for the
	decompression implementation.
=============================================================================*/

#include "DCUtilPrivate.h"
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

/*-----------------------------------------------------------------------------
	RLE Compression for LightBits
-----------------------------------------------------------------------------*/

//
// Emit a run-length encoded byte sequence.
// Format: 
//   - If run length <= 63: single byte = (Code & 0xC0) | (RunLength & 0x3F)
//   - If run length > 63: two bytes = (Code & 0xC0) | 0x40 | (RunLength>>8), then RunLength & 0xFF
//   Code bits:
//     0x00 = normal run
//     0x80 = single byte exception (temporary change)
//
static void EmitRLEByteRun( TArray<BYTE>& Out, BYTE Code, INT RunLength, BYTE Value )
{
	guard(EmitRLEByteRun);
	
	if( RunLength <= 63 )
	{
		// Single byte encoding: Code[2 bits] + RunLength[6 bits] + Value[8 bits]
		Out.AddItem( (Code & 0xC0) | (RunLength & 0x3F) );
		Out.AddItem( Value );
	}
	else
	{
		// Two byte encoding: Code[2 bits] + 0x40 flag + RunLength high byte, then RunLength low byte + Value
		Out.AddItem( (Code & 0xC0) | 0x40 | ((RunLength >> 8) & 0x3F) );
		Out.AddItem( RunLength & 0xFF );
		Out.AddItem( Value );
	}
	
	unguard;
}

//
// Compress LightBits using RLE compression.
// Similar to UBitArray compression but for BYTE values instead of bits.
//
static void CompressLightBitsRLE( TArray<BYTE>& LightBits )
{
	guard(CompressLightBitsRLE);
	
	if( LightBits.Num() == 0 )
		return;
	
	// Create compressed output array
	TArray<BYTE> Compressed;
	Compressed.Empty();
	
	// First, write the original size (needed for decompression)
	INT OriginalSize = LightBits.Num();
	Compressed.AddItem( (OriginalSize >> 24) & 0xFF );
	Compressed.AddItem( (OriginalSize >> 16) & 0xFF );
	Compressed.AddItem( (OriginalSize >> 8) & 0xFF );
	Compressed.AddItem( OriginalSize & 0xFF );
	
	// RLE compression: find runs of identical bytes
	// Algorithm similar to UBitArray but for BYTE values
	BYTE CurrentValue = LightBits(0);
	INT RunLength = 1;
	
	for( INT i = 1; i < LightBits.Num(); i++ )
	{
		if( LightBits(i) == CurrentValue )
		{
			// Continue the run
			RunLength++;
			
			// Max run length is 16383 (14 bits)
			if( RunLength >= 16383 )
			{
				// Emit the run and start a new one
				EmitRLEByteRun( Compressed, 0x00, RunLength, CurrentValue );
				RunLength = 0;
			}
		}
		else
		{
			// Value changed - check if it's a temporary single-byte change
			if( i + 1 < LightBits.Num() && LightBits(i + 1) == CurrentValue )
			{
				// Temporary single-byte change - emit current run, then exception
				if( RunLength > 0 )
				{
					EmitRLEByteRun( Compressed, 0x00, RunLength, CurrentValue );
				}
				EmitRLEByteRun( Compressed, 0x80, 1, LightBits(i) );
				// Next iteration will process i+1 which equals CurrentValue
				RunLength = 0; // Will be set to 1 in next iteration when we see CurrentValue
			}
			else
			{
				// Permanent change - emit current run and start new one
				if( RunLength > 0 )
				{
					EmitRLEByteRun( Compressed, 0x00, RunLength, CurrentValue );
				}
				CurrentValue = LightBits(i);
				RunLength = 1;
			}
		}
	}
	
	// Emit final run
	if( RunLength > 0 )
	{
		EmitRLEByteRun( Compressed, 0x00, RunLength, CurrentValue );
	}
	
	// Replace original with compressed data
	LightBits = Compressed;
	
	unguard;
}

/*-----------------------------------------------------------------------------
	ConvertMapPkg: Process .unr map packages to compress LightBits
-----------------------------------------------------------------------------*/

void FDCUtil::ConvertMapPkg( const FString& PkgPath, UPackage* Pkg )
{
	guard(ConvertMapPkg);
	
	printf( "Processing map package '%s'\n", Pkg->GetName() );
	
	// Record package size before processing for comparison
	INT OldSize = appFSize( *PkgPath );
	if( OldSize >= 0 && PackageSizeBefore.Find( Pkg ) == nullptr )
	{
		PackageSizeBefore.Add( Pkg, OldSize );
	}
	
	// Explicitly load the "MyLevel" ULevel object from the package
	// This ensures all level data is loaded, especially on Dreamcast where lazy loading is used
	ULevel* Level = LoadObject<ULevel>( Pkg, "MyLevel", nullptr, LOAD_NoFail | LOAD_KeepImports, nullptr );
	if( !Level )
	{
		printf( "  WARNING: Could not load MyLevel from package '%s'\n", Pkg->GetName() );
		return;
	}
	
	// Force load the Level's Model to ensure it's fully loaded
	// This is critical for proper saving later
	if( Level->Model )
	{
		// Access the Model to force it to load if it's lazy-loaded
		Level->Model->GetFullName(); // Force load by accessing
		// Ensure Model is marked as public so it gets saved
		if( !(Level->Model->GetFlags() & RF_Public) )
			Level->Model->SetFlags( RF_Public );
	}
	
	// Force load all objects in the package to ensure we process everything
	// On Dreamcast, LoadPackage doesn't eagerly load, so we need to explicitly load
	// Loading MyLevel should trigger loading of related objects, but we'll also
	// iterate through all models to make sure we catch everything
	
	UBOOL Changed = false;
	DWORD TotalOriginalSize = 0;
	DWORD TotalCompressedSize = 0;
	
	// Process the main level model first
	if( Level->Model && Level->Model->IsIn( Pkg ) )
	{
		UModel* Model = Level->Model;
		
		if( Model->LightBits.Num() > 0 )
		{
			const DWORD OriginalSize = Model->LightBits.Num();
			TotalOriginalSize += OriginalSize;
			
		TArray<BYTE> OriginalLightBits = Model->LightBits;
		CompressLightBitsRLE( Model->LightBits );
		const DWORD CompressedSize = Model->LightBits.Num();
		TotalCompressedSize += CompressedSize;
		
		if( CompressedSize < OriginalSize )
		{
			Changed = true;
			// Mark model as modified so it gets saved
			Model->Modify();
			printf( "  - Compressed LightBits in '%s': %u -> %u bytes (%.1f%% reduction)\n", 
				Model->GetName(), OriginalSize, CompressedSize, 
				100.0f * (1.0f - (FLOAT)CompressedSize / (FLOAT)OriginalSize) );
		}
		else
		{
			Model->LightBits = OriginalLightBits;
			printf( "  - LightBits in '%s': %u bytes (compression not beneficial)\n", 
				Model->GetName(), OriginalSize );
		}
		}
	}
	
	// Iterate through all other UModel objects in the package (brush models, etc.)
	for( TObjectIterator<UModel> It; It; ++It )
	{
		UModel* Model = *It;
		
		if( !Model->IsIn( Pkg ) )
			continue;
		
		// Skip the level model, we already processed it
		if( Model == Level->Model )
			continue;
		
		// Check if this model has LightBits to compress
		if( Model->LightBits.Num() == 0 )
			continue;
		
		const DWORD OriginalSize = Model->LightBits.Num();
		TotalOriginalSize += OriginalSize;
		
		// Make a copy of original for compression
		TArray<BYTE> OriginalLightBits = Model->LightBits;
		
		// Compress the LightBits
		CompressLightBitsRLE( Model->LightBits );
		
		const DWORD CompressedSize = Model->LightBits.Num();
		TotalCompressedSize += CompressedSize;
		
		if( CompressedSize < OriginalSize )
		{
			Changed = true;
			Model->Modify();
			printf( "  - Compressed LightBits in '%s': %u -> %u bytes (%.1f%% reduction)\n", 
				Model->GetName(), OriginalSize, CompressedSize, 
				100.0f * (1.0f - (FLOAT)CompressedSize / (FLOAT)OriginalSize) );
		}
		else
		{
			// Compression didn't help, restore original
			Model->LightBits = OriginalLightBits;
			printf( "  - LightBits in '%s': %u bytes (compression not beneficial)\n", 
				Model->GetName(), OriginalSize );
		}
	}
	
	if( Changed )
	{
		printf( "Total LightBits compression: %u -> %u bytes (%.1f%% reduction)\n",
			TotalOriginalSize, TotalCompressedSize,
			100.0f * (1.0f - (FLOAT)TotalCompressedSize / (FLOAT)TotalOriginalSize) );
		ChangedPackages.Add( PkgPath, Pkg );
	}
		else if( TotalOriginalSize > 0 )
	{
		printf( "No beneficial compression found for LightBits in '%s'\n", Pkg->GetName() );
	}
	
	unguard;
}

/*-----------------------------------------------------------------------------
	AnalyzeMapLimits: Full per-array analysis of every .unr map.
	For each BSP array, reports element count, current RAM cost,
	max index value, whether SWORD (16-bit signed, max 32767) or
	WORD (16-bit unsigned, max 65535) is sufficient, and projected
	savings from the proposed compressed struct layout.
-----------------------------------------------------------------------------*/

struct FArrayStats
{
	INT Count;
	INT MaxSlack;
	INT ElemSize;
	INT MaxIndex;
	const char* Name;

	DWORD CurrentBytes() const { return (DWORD)Count * ElemSize; }
	DWORD SlackBytes()   const { return (DWORD)MaxSlack * ElemSize; }
};

struct FFieldRange
{
	const char* Name;
	INT Max;
	INT Min;
	UBOOL FitsSWORD() const { return Max <= 32767 && Min >= -32768; }
	UBOOL FitsWORD()  const { return Max <= 65535 && Min >= 0; }
	UBOOL FitsBYTE()  const { return Max <= 255   && Min >= 0; }
};

struct FMapReport
{
	char MapName[128];

	INT NumNodes, NumSurfs, NumVerts, NumVectors, NumPoints;
	INT NumLightMaps, NumLightBits, NumBounds, NumLeafHulls, NumLeaves, NumLights;
	INT NumActors, NumTextures;

	DWORD CurrentTotalBytes;
	DWORD ProposedTotalBytes;
	DWORD SavingsBytes;

	INT NumFieldOverflows;
	char OverflowFields[2048];

	// Per-field max/min for this map
	INT NodeIVertPoolMax;
	INT SurfPBaseMax, SurfVNormalMax, SurfVTextureUMax, SurfVTextureVMax;
	INT SurfILightMapMax, SurfIBrushPolyMax;
	INT VertPVertexMax, VertISideMax;
	INT VertPVertexMin, VertISideMin;
};

static void AnalyzeLevelModel( UModel* Model, FMapReport& R )
{
	// --- Nodes ---
	if( Model->Nodes && Model->Nodes->Num() > 0 )
	{
		const INT N = Model->Nodes->Num();
		R.NumNodes = N;

		FFieldRange NodeFields[] = {
			{ "Node.iSurf",            0, 0 },
			{ "Node.iBack",            0, -1 },
			{ "Node.iFront",           0, -1 },
			{ "Node.iPlane",           0, -1 },
			{ "Node.iCollisionBound",  0, -1 },
			{ "Node.iRenderBound",     0, -1 },
			{ "Node.iVertPool",        0, 0 },
			{ "Node.iLeaf[0]",         0, -1 },
			{ "Node.iLeaf[1]",         0, -1 },
		};

		for( INT i = 0; i < N; i++ )
		{
			FBspNode& Node = Model->Nodes->Element(i);
			// On DC build these are already SWORD, but the .unr file stores INT.
			// We read via the desktop struct layout (INT fields).
			INT iSurf = Node.iSurf;
			INT iBack = Node.iBack;
			INT iFront = Node.iFront;
			INT iPlane = Node.iPlane;
			INT iColl = Node.iCollisionBound;
			INT iRend = Node.iRenderBound;
			INT iVP   = Node.iVertPool;
			INT iL0   = Node.iLeaf[0];
			INT iL1   = Node.iLeaf[1];

			if( iSurf > NodeFields[0].Max ) NodeFields[0].Max = iSurf;
			if( iSurf < NodeFields[0].Min ) NodeFields[0].Min = iSurf;
			if( iBack > NodeFields[1].Max ) NodeFields[1].Max = iBack;
			if( iBack < NodeFields[1].Min ) NodeFields[1].Min = iBack;
			if( iFront > NodeFields[2].Max ) NodeFields[2].Max = iFront;
			if( iFront < NodeFields[2].Min ) NodeFields[2].Min = iFront;
			if( iPlane > NodeFields[3].Max ) NodeFields[3].Max = iPlane;
			if( iPlane < NodeFields[3].Min ) NodeFields[3].Min = iPlane;
			if( iColl > NodeFields[4].Max ) NodeFields[4].Max = iColl;
			if( iColl < NodeFields[4].Min ) NodeFields[4].Min = iColl;
			if( iRend > NodeFields[5].Max ) NodeFields[5].Max = iRend;
			if( iRend < NodeFields[5].Min ) NodeFields[5].Min = iRend;
			if( iVP   > NodeFields[6].Max ) NodeFields[6].Max = iVP;
			if( iVP   < NodeFields[6].Min ) NodeFields[6].Min = iVP;
			if( iL0   > NodeFields[7].Max ) NodeFields[7].Max = iL0;
			if( iL0   < NodeFields[7].Min ) NodeFields[7].Min = iL0;
			if( iL1   > NodeFields[8].Max ) NodeFields[8].Max = iL1;
			if( iL1   < NodeFields[8].Min ) NodeFields[8].Min = iL1;
		}

		// Current: FBspNode = 48 bytes on DC (SWORD indices already), 64 on desktop
		// Proposed keeps the same 48-byte DC layout. iVertPool stays INT.
		// Check if iVertPool could also be SWORD (saves 2 more bytes per node).
		const DWORD CurNodeBytes = (DWORD)N * sizeof(FBspNode);
		R.CurrentTotalBytes += CurNodeBytes;
		R.ProposedTotalBytes += CurNodeBytes; // already compressed on DC

		for( INT f = 0; f < 9; f++ )
		{
			if( !NodeFields[f].FitsSWORD() )
			{
				R.NumFieldOverflows++;
				char Tmp[256];
				snprintf( Tmp, sizeof(Tmp), "  OVERFLOW: %s max=%d min=%d (SWORD limit +-32767)\n",
					NodeFields[f].Name, NodeFields[f].Max, NodeFields[f].Min );
				appStrcat( R.OverflowFields, Tmp );
			}
		}

		R.NodeIVertPoolMax = NodeFields[6].Max;

		// Check if iVertPool fits SWORD for additional savings
		if( NodeFields[6].FitsSWORD() )
		{
			DWORD Saved = (DWORD)N * 2; // INT->SWORD = 2 bytes saved
			R.ProposedTotalBytes -= Saved;
			R.SavingsBytes += Saved;
		}
		else
		{
			R.NumFieldOverflows++;
			char Tmp[256];
			snprintf( Tmp, sizeof(Tmp), "  NOTE: Node.iVertPool max=%d, stays INT (no extra saving)\n",
				NodeFields[6].Max );
			appStrcat( R.OverflowFields, Tmp );
		}
	}

	// --- Surfs ---
	if( Model->Surfs && Model->Surfs->Num() > 0 )
	{
		const INT N = Model->Surfs->Num();
		R.NumSurfs = N;

		FFieldRange SurfFields[] = {
			{ "Surf.pBase",      0, 0 },
			{ "Surf.vNormal",    0, 0 },
			{ "Surf.vTextureU",  0, 0 },
			{ "Surf.vTextureV",  0, 0 },
			{ "Surf.iLightMap",  0, -1 },
			{ "Surf.iBrushPoly", 0, -1 },
		};

		for( INT i = 0; i < N; i++ )
		{
			FBspSurf& S = Model->Surfs->Element(i);
			if( S.pBase     > SurfFields[0].Max ) SurfFields[0].Max = S.pBase;
			if( S.pBase     < SurfFields[0].Min ) SurfFields[0].Min = S.pBase;
			if( S.vNormal   > SurfFields[1].Max ) SurfFields[1].Max = S.vNormal;
			if( S.vNormal   < SurfFields[1].Min ) SurfFields[1].Min = S.vNormal;
			if( S.vTextureU > SurfFields[2].Max ) SurfFields[2].Max = S.vTextureU;
			if( S.vTextureU < SurfFields[2].Min ) SurfFields[2].Min = S.vTextureU;
			if( S.vTextureV > SurfFields[3].Max ) SurfFields[3].Max = S.vTextureV;
			if( S.vTextureV < SurfFields[3].Min ) SurfFields[3].Min = S.vTextureV;
			if( S.iLightMap > SurfFields[4].Max ) SurfFields[4].Max = S.iLightMap;
			if( S.iLightMap < SurfFields[4].Min ) SurfFields[4].Min = S.iLightMap;
			if( S.iBrushPoly> SurfFields[5].Max ) SurfFields[5].Max = S.iBrushPoly;
			if( S.iBrushPoly< SurfFields[5].Min ) SurfFields[5].Min = S.iBrushPoly;
		}

		// Current: FBspSurf = 40 bytes (6 INT index fields = 24 bytes of indices)
		// Proposed: 6 INT -> SWORD = saves 12 bytes per surf -> 28 bytes
		const DWORD CurBytes = (DWORD)N * 40;
		R.CurrentTotalBytes += CurBytes;

		R.SurfPBaseMax = SurfFields[0].Max;
		R.SurfVNormalMax = SurfFields[1].Max;
		R.SurfVTextureUMax = SurfFields[2].Max;
		R.SurfVTextureVMax = SurfFields[3].Max;
		R.SurfILightMapMax = SurfFields[4].Max;
		R.SurfIBrushPolyMax = SurfFields[5].Max;

		// Per-field savings: each field that fits SWORD saves 2 bytes per surf
		DWORD SurfSaved = 0;
		for( INT f = 0; f < 6; f++ )
		{
			if( SurfFields[f].FitsSWORD() )
			{
				SurfSaved += (DWORD)N * 2;
			}
			else
			{
				R.NumFieldOverflows++;
				char Tmp[256];
				snprintf( Tmp, sizeof(Tmp), "  OVERFLOW: %s max=%d min=%d (SWORD=+-32767, WORD=%s)\n",
					SurfFields[f].Name, SurfFields[f].Max, SurfFields[f].Min,
					SurfFields[f].FitsWORD() ? "fits" : "NO" );
				appStrcat( R.OverflowFields, Tmp );
			}
		}
		R.ProposedTotalBytes += CurBytes - SurfSaved;
		R.SavingsBytes += SurfSaved;
	}

	// --- Verts ---
	if( Model->Verts && Model->Verts->Num() > 0 )
	{
		const INT N = Model->Verts->Num();
		R.NumVerts = N;

		INT MaxPVertex = 0, MinPVertex = 0;
		INT MaxISide = 0, MinISide = -1;

		for( INT i = 0; i < N; i++ )
		{
			FVert& V = Model->Verts->Element(i);
			if( V.pVertex > MaxPVertex ) MaxPVertex = V.pVertex;
			if( V.pVertex < MinPVertex ) MinPVertex = V.pVertex;
			if( V.iSide   > MaxISide )   MaxISide = V.iSide;
			if( V.iSide   < MinISide )   MinISide = V.iSide;
		}

		// Current: FVert = 8 bytes (2x INT)
		// Proposed: 2x SWORD = 4 bytes -> 50% reduction
		const DWORD CurBytes = (DWORD)N * 8;
		R.CurrentTotalBytes += CurBytes;

		R.VertPVertexMax = MaxPVertex;
		R.VertPVertexMin = MinPVertex;
		R.VertISideMax = MaxISide;
		R.VertISideMin = MinISide;

		UBOOL PVertFits = (MaxPVertex <= 32767 && MinPVertex >= -32768);
		UBOOL ISideFits = (MaxISide <= 32767 && MinISide >= -32768);

		if( PVertFits && ISideFits )
		{
			DWORD Saved = (DWORD)N * 4;
			R.ProposedTotalBytes += CurBytes - Saved;
			R.SavingsBytes += Saved;
		}
		else
		{
			R.ProposedTotalBytes += CurBytes;
			if( !PVertFits )
			{
				R.NumFieldOverflows++;
				UBOOL PVertWord = (MaxPVertex <= 65535 && MinPVertex >= 0);
				char Tmp[256];
				snprintf( Tmp, sizeof(Tmp), "  OVERFLOW: Vert.pVertex max=%d min=%d (SWORD=NO, WORD=%s)\n",
					MaxPVertex, MinPVertex, PVertWord ? "fits" : "NO" );
				appStrcat( R.OverflowFields, Tmp );
			}
			if( !ISideFits )
			{
				R.NumFieldOverflows++;
				UBOOL ISideWord = (MaxISide <= 65535 && MinISide >= 0);
				char Tmp[256];
				snprintf( Tmp, sizeof(Tmp), "  OVERFLOW: Vert.iSide max=%d min=%d (SWORD=NO, WORD=%s)\n",
					MaxISide, MinISide, ISideWord ? "fits" : "NO" );
				appStrcat( R.OverflowFields, Tmp );
			}
		}
	}

	// --- Vectors & Points (FVector = 12 bytes, no index compression possible) ---
	if( Model->Vectors && Model->Vectors->Num() > 0 )
	{
		R.NumVectors = Model->Vectors->Num();
		const DWORD Bytes = (DWORD)R.NumVectors * 12;
		R.CurrentTotalBytes += Bytes;
		R.ProposedTotalBytes += Bytes;
	}
	if( Model->Points && Model->Points->Num() > 0 )
	{
		R.NumPoints = Model->Points->Num();
		const DWORD Bytes = (DWORD)R.NumPoints * 12;
		R.CurrentTotalBytes += Bytes;
		R.ProposedTotalBytes += Bytes;
	}

	// --- LightMap index array ---
	// FLightMapIndex = 40 bytes (INT DataOffset, INT iLightActors, FVector Pan, 2xFLOAT, 2xINT, 2xBYTE)
	R.NumLightMaps = Model->LightMap.Num();
	if( R.NumLightMaps > 0 )
	{
		const DWORD Bytes = (DWORD)R.NumLightMaps * 40;
		R.CurrentTotalBytes += Bytes;
		R.ProposedTotalBytes += Bytes;
	}

	// --- LightBits ---
	R.NumLightBits = Model->LightBits.Num();
	if( R.NumLightBits > 0 )
	{
		R.CurrentTotalBytes += (DWORD)R.NumLightBits;
		R.ProposedTotalBytes += (DWORD)R.NumLightBits;
	}

	// --- Bounds (FBox = 28 bytes: 2xFVector + BYTE flags + padding) ---
	R.NumBounds = Model->Bounds.Num();
	if( R.NumBounds > 0 )
	{
		const DWORD Bytes = (DWORD)R.NumBounds * sizeof(FBox);
		R.CurrentTotalBytes += Bytes;
		R.ProposedTotalBytes += Bytes;
	}

	// --- LeafHulls (TArray<INT>) ---
	R.NumLeafHulls = Model->LeafHulls.Num();
	if( R.NumLeafHulls > 0 )
	{
		const DWORD Bytes = (DWORD)R.NumLeafHulls * 4;
		R.CurrentTotalBytes += Bytes;
		R.ProposedTotalBytes += Bytes;
	}

	// --- Leaves (FLeaf = 20 bytes: 3xINT + QWORD) ---
	R.NumLeaves = Model->Leaves.Num();
	if( R.NumLeaves > 0 )
	{
		const DWORD Bytes = (DWORD)R.NumLeaves * 20;
		R.CurrentTotalBytes += Bytes;
		R.ProposedTotalBytes += Bytes;
	}

	// --- Lights ---
	R.NumLights = Model->Lights.Num();
}

void FDCUtil::AnalyzeMapLimits( const char* MapGlob )
{
	guard(AnalyzeMapLimits);

	printf( "=======================================================================\n" );
	printf( " Dreamcast BSP Compression Limit Analysis\n" );
	printf( " Proposed: FBspSurf INT->SWORD, FVert INT->SWORD\n" );
	printf( " SWORD range: -32768..32767  (INDEX_NONE = -1 fits)\n" );
	printf( "=======================================================================\n\n" );

	TArray<FString> Files = appFindFiles( MapGlob );
	if( Files.Num() == 0 )
	{
		printf( "No .unr files found matching '%s'\n", MapGlob );
		return;
	}

	printf( "Found %d map files\n\n", Files.Num() );

	// Extract directory from glob
	char Dir[2048];
	appStrcpy( Dir, MapGlob );
	char* Slash = strrchr( Dir, '/' );
	if( !Slash ) Slash = strrchr( Dir, '\\' );
	if( Slash ) *(Slash + 1) = 0;
	else Dir[0] = 0;

	TArray<FMapReport> Reports;
	INT TotalOverflowMaps = 0;

	for( INT i = 0; i < Files.Num(); i++ )
	{
		char FullPath[2048];
		snprintf( FullPath, sizeof(FullPath), "%s%s", Dir, *Files(i) );

		printf( "[%d/%d] %s: loading...", i + 1, Files.Num(), *Files(i) ); fflush( stdout );

		UPackage* Pkg = Cast<UPackage>( GObj.LoadPackage( nullptr, FullPath, LOAD_KeepImports ) );
		if( !Pkg )
		{
			printf( " FAILED\n" );
			continue;
		}
		printf( " pkg ok..."); fflush( stdout );

		ULevel* Level = LoadObject<ULevel>( Pkg, "MyLevel", nullptr, LOAD_KeepImports, nullptr );
		if( !Level || !Level->Model )
		{
			printf( " no level model, skip\n" );
			GObj.ResetLoaders( Pkg );
			continue;
		}
		printf( " level ok..."); fflush( stdout );

		FMapReport R;
		appMemset( &R, 0, sizeof(R) );
		appStrncpy( R.MapName, *Files(i), sizeof(R.MapName) - 1 );
		R.MapName[sizeof(R.MapName) - 1] = 0;
		R.OverflowFields[0] = 0;

		R.NumActors = Level->Num();
		printf( " actors=%d...", R.NumActors ); fflush( stdout );

		// Count textures in package
		for( TObjectIterator<UTexture> It; It; ++It )
			if( It->IsIn( Pkg ) )
				R.NumTextures++;
		printf( " tex=%d...", R.NumTextures ); fflush( stdout );

		AnalyzeLevelModel( Level->Model, R );
		printf( " analyzed..."); fflush( stdout );

		if( R.NumFieldOverflows > 0 )
			TotalOverflowMaps++;

		printf( " pre-add..."); fflush( stdout );
		Reports.AddItem( R );
		printf( " added..."); fflush( stdout );
		GObj.ResetLoaders( Pkg );
		printf( " done\n" ); fflush( stdout );
	}

	// --- Detailed per-map report ---
	for( INT i = 0; i < Reports.Num(); i++ )
	{
		FMapReport& R = Reports(i);
		printf( "-----------------------------------------------------------------------\n" );
		printf( " %s\n", R.MapName );
		printf( "-----------------------------------------------------------------------\n" );
		printf( "  Nodes:     %6d  (%7u KB)    Surfs:    %6d  (%7u KB)\n",
			R.NumNodes, (R.NumNodes * (DWORD)sizeof(FBspNode)) / 1024,
			R.NumSurfs, (R.NumSurfs * 40u) / 1024 );
		printf( "  Verts:     %6d  (%7u KB)    Vectors:  %6d  (%7u KB)\n",
			R.NumVerts, (R.NumVerts * 8u) / 1024,
			R.NumVectors, (R.NumVectors * 12u) / 1024 );
		printf( "  Points:    %6d  (%7u KB)    LightMap: %6d  (%7u KB)\n",
			R.NumPoints, (R.NumPoints * 12u) / 1024,
			R.NumLightMaps, (R.NumLightMaps * 40u) / 1024 );
		printf( "  LightBits: %6d  (%7u KB)    Bounds:   %6d  (%7u KB)\n",
			R.NumLightBits, (DWORD)R.NumLightBits / 1024,
			R.NumBounds, (R.NumBounds * (DWORD)sizeof(FBox)) / 1024 );
		printf( "  LeafHulls: %6d  (%7u KB)    Leaves:   %6d  (%7u KB)\n",
			R.NumLeafHulls, (R.NumLeafHulls * 4u) / 1024,
			R.NumLeaves, (R.NumLeaves * 20u) / 1024 );
		printf( "  Actors:    %6d                 Textures: %6d\n",
			R.NumActors, R.NumTextures );
		printf( "\n" );
		printf( "  Current BSP RAM:  %8u bytes (%u KB)\n", R.CurrentTotalBytes, R.CurrentTotalBytes / 1024 );
		printf( "  Proposed BSP RAM: %8u bytes (%u KB)\n", R.ProposedTotalBytes, R.ProposedTotalBytes / 1024 );
		printf( "  Savings:          %8u bytes (%u KB, %.1f%%)\n",
			R.SavingsBytes, R.SavingsBytes / 1024,
			R.CurrentTotalBytes > 0 ? (100.0f * R.SavingsBytes / R.CurrentTotalBytes) : 0.0f );

		if( R.NumFieldOverflows > 0 )
		{
			printf( "\n  *** %d FIELD(S) EXCEED SWORD LIMIT ***\n", R.NumFieldOverflows );
			printf( "%s", R.OverflowFields );
		}
		else
		{
			printf( "  All fields fit in SWORD range.\n" );
		}
		printf( "\n" );
	}

	// --- Summary table ---
	printf( "=======================================================================\n" );
	printf( " SUMMARY\n" );
	printf( "=======================================================================\n" );
	printf( "%-32s %7s %7s %7s %7s %6s %s\n",
		"Map", "Nodes", "Surfs", "Verts", "CurKB", "SaveKB", "Status" );
	printf( "-----------------------------------------------------------------------\n" );

	DWORD GrandCurrent = 0, GrandProposed = 0, GrandSavings = 0;
	for( INT i = 0; i < Reports.Num(); i++ )
	{
		FMapReport& R = Reports(i);
		GrandCurrent += R.CurrentTotalBytes;
		GrandProposed += R.ProposedTotalBytes;
		GrandSavings += R.SavingsBytes;

		printf( "%-32s %7d %7d %7d %7u %6u %s\n",
			R.MapName,
			R.NumNodes, R.NumSurfs, R.NumVerts,
			R.CurrentTotalBytes / 1024,
			R.SavingsBytes / 1024,
			R.NumFieldOverflows > 0 ? "OVERFLOW" : "OK" );
	}

	printf( "-----------------------------------------------------------------------\n" );
	printf( "%-32s %7s %7s %7s %7u %6u\n",
		"TOTAL", "", "", "",
		GrandCurrent / 1024,
		GrandSavings / 1024 );
	printf( "=======================================================================\n" );
	printf( "\n" );
	printf( "Maps analyzed: %d\n", Reports.Num() );
	printf( "Maps with SWORD overflows: %d\n", TotalOverflowMaps );
	printf( "Total current BSP RAM:  %u KB (%.1f MB)\n", GrandCurrent / 1024, GrandCurrent / (1024.0f * 1024.0f) );
	printf( "Total proposed BSP RAM: %u KB (%.1f MB)\n", GrandProposed / 1024, GrandProposed / (1024.0f * 1024.0f) );
	printf( "Total savings:          %u KB (%.1f MB, %.1f%%)\n",
		GrandSavings / 1024, GrandSavings / (1024.0f * 1024.0f),
		GrandCurrent > 0 ? (100.0f * GrandSavings / GrandCurrent) : 0.0f );

	if( TotalOverflowMaps > 0 )
		printf( "\nWARNING: %d map(s) have fields that exceed SWORD limits!\n", TotalOverflowMaps );
	else
		printf( "\nAll maps fit within SWORD limits. Compression is safe to apply.\n" );

	// --- Global worst-case per-field analysis ---
	printf( "\n=======================================================================\n" );
	printf( " GLOBAL WORST-CASE FIELD RANGES (across all %d maps)\n", Reports.Num() );
	printf( "=======================================================================\n" );

	INT GMaxIVertPool = 0;
	INT GMaxPBase = 0, GMaxVNormal = 0, GMaxVTexU = 0, GMaxVTexV = 0;
	INT GMaxILightMap = 0, GMaxIBrushPoly = 0;
	INT GMaxPVertex = 0, GMinPVertex = 0, GMaxISide = 0, GMinISide = 0;
	INT GMaxNodes = 0, GMaxSurfs = 0, GMaxVerts = 0, GMaxPoints = 0, GMaxVectors = 0;

	for( INT i = 0; i < Reports.Num(); i++ )
	{
		FMapReport& R = Reports(i);
		if( R.NodeIVertPoolMax > GMaxIVertPool ) GMaxIVertPool = R.NodeIVertPoolMax;
		if( R.SurfPBaseMax > GMaxPBase ) GMaxPBase = R.SurfPBaseMax;
		if( R.SurfVNormalMax > GMaxVNormal ) GMaxVNormal = R.SurfVNormalMax;
		if( R.SurfVTextureUMax > GMaxVTexU ) GMaxVTexU = R.SurfVTextureUMax;
		if( R.SurfVTextureVMax > GMaxVTexV ) GMaxVTexV = R.SurfVTextureVMax;
		if( R.SurfILightMapMax > GMaxILightMap ) GMaxILightMap = R.SurfILightMapMax;
		if( R.SurfIBrushPolyMax > GMaxIBrushPoly ) GMaxIBrushPoly = R.SurfIBrushPolyMax;
		if( R.VertPVertexMax > GMaxPVertex ) GMaxPVertex = R.VertPVertexMax;
		if( R.VertPVertexMin < GMinPVertex ) GMinPVertex = R.VertPVertexMin;
		if( R.VertISideMax > GMaxISide ) GMaxISide = R.VertISideMax;
		if( R.VertISideMin < GMinISide ) GMinISide = R.VertISideMin;
		if( R.NumNodes > GMaxNodes ) GMaxNodes = R.NumNodes;
		if( R.NumSurfs > GMaxSurfs ) GMaxSurfs = R.NumSurfs;
		if( R.NumVerts > GMaxVerts ) GMaxVerts = R.NumVerts;
		if( R.NumPoints > GMaxPoints ) GMaxPoints = R.NumPoints;
		if( R.NumVectors > GMaxVectors ) GMaxVectors = R.NumVectors;
	}

	printf( "\n  Array counts (worst case):\n" );
	printf( "    Nodes:   %6d    Surfs:   %6d    Verts:   %6d\n", GMaxNodes, GMaxSurfs, GMaxVerts );
	printf( "    Points:  %6d    Vectors: %6d\n", GMaxPoints, GMaxVectors );

	printf( "\n  %-28s %10s %10s  %s  %s  %s\n", "Field", "Max", "Min", "SWORD", "WORD", " Recommendation" );
	printf( "  -------------------------------------------------------------------------------------\n" );

	#define FIELD_ROW(name, mx, mn) \
		printf( "  %-28s %10d %10d  %s   %s   %s\n", name, mx, mn, \
			((mx) <= 32767 && (mn) >= -32768) ? " OK " : "FAIL", \
			((mx) <= 65535 && (mn) >= 0)      ? " OK " : "FAIL", \
			((mx) <= 32767 && (mn) >= -32768) ? "-> SWORD" : \
			((mx) <= 65535 && (mn) >= 0)      ? "-> WORD" : "keep INT" )

	FIELD_ROW( "Node.iVertPool", GMaxIVertPool, 0 );
	FIELD_ROW( "Surf.pBase", GMaxPBase, 0 );
	FIELD_ROW( "Surf.vNormal", GMaxVNormal, 0 );
	FIELD_ROW( "Surf.vTextureU", GMaxVTexU, 0 );
	FIELD_ROW( "Surf.vTextureV", GMaxVTexV, 0 );
	FIELD_ROW( "Surf.iLightMap", GMaxILightMap, -1 );
	FIELD_ROW( "Surf.iBrushPoly", GMaxIBrushPoly, -1 );
	FIELD_ROW( "Vert.pVertex", GMaxPVertex, GMinPVertex );
	FIELD_ROW( "Vert.iSide", GMaxISide, GMinISide );

	#undef FIELD_ROW

	// --- Dump full report to file ---
	{
		const char* ReportPath = "../../analyze_report.txt";
		FILE* F = fopen( ReportPath, "w" );
		if( F )
		{
			fprintf( F, "Dreamcast BSP Compression Limit Analysis\n" );
			fprintf( F, "=========================================\n\n" );

			for( INT i = 0; i < Reports.Num(); i++ )
			{
				FMapReport& R = Reports(i);
				fprintf( F, "-----------------------------------------------------------------------\n" );
				fprintf( F, " %s\n", R.MapName );
				fprintf( F, "-----------------------------------------------------------------------\n" );
				fprintf( F, "  Nodes:     %6d  (%7u KB)    Surfs:    %6d  (%7u KB)\n",
					R.NumNodes, (R.NumNodes * (DWORD)sizeof(FBspNode)) / 1024,
					R.NumSurfs, (R.NumSurfs * 40u) / 1024 );
				fprintf( F, "  Verts:     %6d  (%7u KB)    Vectors:  %6d  (%7u KB)\n",
					R.NumVerts, (R.NumVerts * 8u) / 1024,
					R.NumVectors, (R.NumVectors * 12u) / 1024 );
				fprintf( F, "  Points:    %6d  (%7u KB)    LightMap: %6d  (%7u KB)\n",
					R.NumPoints, (R.NumPoints * 12u) / 1024,
					R.NumLightMaps, (R.NumLightMaps * 40u) / 1024 );
				fprintf( F, "  LightBits: %6d  (%7u KB)    Bounds:   %6d  (%7u KB)\n",
					R.NumLightBits, (DWORD)R.NumLightBits / 1024,
					R.NumBounds, (R.NumBounds * (DWORD)sizeof(FBox)) / 1024 );
				fprintf( F, "  LeafHulls: %6d  (%7u KB)    Leaves:   %6d  (%7u KB)\n",
					R.NumLeafHulls, (R.NumLeafHulls * 4u) / 1024,
					R.NumLeaves, (R.NumLeaves * 20u) / 1024 );
				fprintf( F, "  Actors:    %6d                 Textures: %6d\n",
					R.NumActors, R.NumTextures );
				fprintf( F, "  Current BSP RAM:  %8u bytes (%u KB)\n", R.CurrentTotalBytes, R.CurrentTotalBytes / 1024 );
				fprintf( F, "  Proposed BSP RAM: %8u bytes (%u KB)\n", R.ProposedTotalBytes, R.ProposedTotalBytes / 1024 );
				fprintf( F, "  Savings:          %8u bytes (%u KB, %.1f%%)\n",
					R.SavingsBytes, R.SavingsBytes / 1024,
					R.CurrentTotalBytes > 0 ? (100.0f * R.SavingsBytes / R.CurrentTotalBytes) : 0.0f );
				fprintf( F, "  Surf field max: pBase=%d vNormal=%d vTexU=%d vTexV=%d iLightMap=%d iBrushPoly=%d\n",
					R.SurfPBaseMax, R.SurfVNormalMax, R.SurfVTextureUMax, R.SurfVTextureVMax,
					R.SurfILightMapMax, R.SurfIBrushPolyMax );
				fprintf( F, "  Vert field max: pVertex=%d (min=%d) iSide=%d (min=%d)\n",
					R.VertPVertexMax, R.VertPVertexMin, R.VertISideMax, R.VertISideMin );
				fprintf( F, "  Node.iVertPool max: %d\n", R.NodeIVertPoolMax );
				if( R.NumFieldOverflows > 0 )
					fprintf( F, "%s", R.OverflowFields );
				fprintf( F, "\n" );
			}

			fprintf( F, "\nGlobal worst-case: Node.iVertPool=%d Surf.pBase=%d vNormal=%d vTexU=%d vTexV=%d iLM=%d iBP=%d\n",
				GMaxIVertPool, GMaxPBase, GMaxVNormal, GMaxVTexU, GMaxVTexV, GMaxILightMap, GMaxIBrushPoly );
			fprintf( F, "Global worst-case: Vert.pVertex=%d..%d Vert.iSide=%d..%d\n",
				GMinPVertex, GMaxPVertex, GMinISide, GMaxISide );
			fprintf( F, "\nTotal current:  %u KB (%.1f MB)\n", GrandCurrent / 1024, GrandCurrent / (1024.0f * 1024.0f) );
			fprintf( F, "Total proposed: %u KB (%.1f MB)\n", GrandProposed / 1024, GrandProposed / (1024.0f * 1024.0f) );
			fprintf( F, "Total savings:  %u KB (%.1f MB, %.1f%%)\n",
				GrandSavings / 1024, GrandSavings / (1024.0f * 1024.0f),
				GrandCurrent > 0 ? (100.0f * GrandSavings / GrandCurrent) : 0.0f );

			fclose( F );
			printf( "\nFull report written to: %s\n", ReportPath );
		}
		else
		{
			printf( "\nWARNING: Could not write report file to %s\n", ReportPath );
		}
	}

	unguard;
}

