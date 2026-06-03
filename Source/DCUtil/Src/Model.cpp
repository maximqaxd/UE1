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
	ReduceLightmapResolution: Halve lightmap resolution to save RAM.

	Each surface's shadow data in LightBits is a bitmask grid of UClamp x VClamp
	texels, packed 8 bits per byte (row-major, ceil(UClamp/8) bytes per row).
	For each light affecting the surface, there's one such grid, stored
	sequentially at DataOffset.

	We downsample each 2x2 block of shadow texels into 1 texel using OR
	(if any of the 4 source texels is lit, the destination is lit — preserves
	light visibility, errs on the side of too much light rather than too dark).
-----------------------------------------------------------------------------*/

static UBOOL GetShadowBit( const BYTE* Bits, INT UClamp, INT U, INT V )
{
	// Shadow bits are packed: byte = row V, bit position = U within that byte
	// Byte index = V * ceil(UClamp/8) + (U >> 3)
	// Bit index  = U & 7
	const INT BytesPerRow = (UClamp + 7) >> 3;
	return ( Bits[ V * BytesPerRow + (U >> 3) ] >> (U & 7) ) & 1;
}

static void SetShadowBit( BYTE* Bits, INT UClamp, INT U, INT V )
{
	const INT BytesPerRow = (UClamp + 7) >> 3;
	Bits[ V * BytesPerRow + (U >> 3) ] |= (1 << (U & 7));
}

// 4-byte sentinel prepended to LightBits after reduction so a map can never be
// reduced twice (which would corrupt the data). The engine reads shadow bits via
// FLightMapIndex.DataOffset, so these 4 unreferenced prefix bytes are harmless at
// runtime. All new DataOffsets are >= LIGHTBITS_MAGIC_SIZE.
static const BYTE  LIGHTBITS_MAGIC[4]   = { 'D', 'C', 'L', 'M' };
static const INT   LIGHTBITS_MAGIC_SIZE = 4;

static UBOOL IsLightmapAlreadyReduced( UModel* Model )
{
	if( !Model || Model->LightBits.Num() < LIGHTBITS_MAGIC_SIZE )
		return false;
	for( INT i = 0; i < LIGHTBITS_MAGIC_SIZE; ++i )
		if( Model->LightBits(i) != LIGHTBITS_MAGIC[i] )
			return false;
	return true;
}

static DWORD ReduceLightmapResolution( UModel* Model, INT ScaleFactor )
{
	guard(ReduceLightmapResolution);

	if( !Model || Model->LightMap.Num() == 0 || Model->LightBits.Num() == 0 )
		return 0;

	// Idempotency guard: never reduce twice.
	if( IsLightmapAlreadyReduced( Model ) )
		return 0;

	const INT NumSurfs = Model->LightMap.Num();
	const INT OldBitsSize = Model->LightBits.Num();

	// Step 1: Determine how many bytes each surface owns in LightBits.
	// Sort surfaces by DataOffset to find contiguous blocks, then
	// NumLights = OwnedBytes / MaskSpace.

	// Build sorted index array — only include surfaces that actually have lights
	struct FSurfSort { INT iSurf; INT DataOffset; };
	TArray<FSurfSort> Sorted;
	for( INT i = 0; i < NumSurfs; i++ )
	{
		FLightMapIndex& Index = Model->LightMap(i);
		if( Index.DataOffset != INDEX_NONE && Index.DataOffset >= 0
			&& Index.UClamp > 0 && Index.VClamp > 0
			&& Index.iLightActors != INDEX_NONE )
		{
			FSurfSort S;
			S.iSurf = i;
			S.DataOffset = Index.DataOffset;
			Sorted.AddItem( S );
		}
	}

	if( Sorted.Num() == 0 )
		return 0;

	// Simple insertion sort by DataOffset (stable, small N)
	for( INT i = 1; i < Sorted.Num(); i++ )
	{
		FSurfSort Key = Sorted(i);
		INT j = i - 1;
		while( j >= 0 && Sorted(j).DataOffset > Key.DataOffset )
		{
			Sorted(j + 1) = Sorted(j);
			j--;
		}
		Sorted(j + 1) = Key;
	}

	// Compute owned bytes per surface
	TArray<INT> OwnedBytes;
	OwnedBytes.AddZeroed( NumSurfs );
	for( INT i = 0; i < Sorted.Num(); i++ )
	{
		INT NextOffset = (i + 1 < Sorted.Num()) ? Sorted(i + 1).DataOffset : OldBitsSize;
		OwnedBytes(Sorted(i).iSurf) = NextOffset - Sorted(i).DataOffset;
	}

	// Step 2: Rebuild LightBits with downsampled data.
	// Start with the 4-byte sentinel so the rebuilt array is tagged as reduced
	// and all surface DataOffsets land at >= LIGHTBITS_MAGIC_SIZE.
	const BYTE* OldBits = &Model->LightBits(0);
	TArray<BYTE> NewBits;
	NewBits.Empty();
	for( INT m = 0; m < LIGHTBITS_MAGIC_SIZE; ++m )
		NewBits.AddItem( LIGHTBITS_MAGIC[m] );

	DWORD TotalOldBytes = 0;
	DWORD TotalNewBytes = 0;
	INT NumReduced = 0;

	for( INT iSurf = 0; iSurf < NumSurfs; iSurf++ )
	{
		FLightMapIndex& Index = Model->LightMap(iSurf);

		if( Index.DataOffset == INDEX_NONE || Index.DataOffset < 0 || Index.UClamp <= 0 || Index.VClamp <= 0
			|| Index.iLightActors == INDEX_NONE )
			continue;

		const INT OldUClamp = Index.UClamp;
		const INT OldVClamp = Index.VClamp;
		const INT OldBytesPerRow = (OldUClamp + 7) >> 3;
		const INT OldMaskSpace = OldBytesPerRow * OldVClamp;

		// Derive light count from gap between consecutive DataOffsets.
		// This is correct because the editor appends each surface's data sequentially.
		const INT SurfOwnedBytes = OwnedBytes(iSurf);
		INT NumLights = (OldMaskSpace > 0) ? (SurfOwnedBytes / OldMaskSpace) : 0;

		// Cap to a sane maximum (no surface has >64 lights in practice)
		if( NumLights > 64 ) NumLights = 64;
		if( NumLights <= 0 ) NumLights = 1;

		// Also clamp to what's actually in the array
		INT MaxLights = (OldBitsSize - Index.DataOffset) / Max(OldMaskSpace, 1);
		if( MaxLights > 64 ) MaxLights = 64;
		if( NumLights > MaxLights ) NumLights = MaxLights;
		if( NumLights <= 0 )
		{
			Index.DataOffset = INDEX_NONE;
			continue;
		}

		// Don't reduce surfaces that are already tiny
		if( OldUClamp <= 2 && OldVClamp <= 2 )
		{
			const INT CopySize = OldMaskSpace * NumLights;
			const INT NewOffset = NewBits.Num();
			for( INT b = 0; b < CopySize; b++ )
				NewBits.AddItem( OldBits[Index.DataOffset + b] );
			TotalOldBytes += CopySize;
			TotalNewBytes += CopySize;
			Index.DataOffset = NewOffset;
			continue;
		}

		// Calculate new resolution
		INT NewUClamp = Max( 2, (OldUClamp + ScaleFactor - 1) / ScaleFactor );
		INT NewVClamp = Max( 2, (OldVClamp + ScaleFactor - 1) / ScaleFactor );
		const INT NewBytesPerRow = (NewUClamp + 7) >> 3;
		const INT NewMaskSpace = NewBytesPerRow * NewVClamp;

		const INT NewOffset = NewBits.Num();

		// Downsample each light's shadow mask
		for( INT iLight = 0; iLight < NumLights; iLight++ )
		{
			const BYTE* SrcLight = OldBits + Index.DataOffset + (OldMaskSpace * iLight);

			// Allocate zeroed destination
			const INT DstStart = NewBits.Num();
			NewBits.AddZeroed( NewMaskSpace );
			BYTE* DstLight = &NewBits(DstStart);

			// Downsample: for each new texel, OR the ScaleFactor x ScaleFactor source texels
			for( INT NewV = 0; NewV < NewVClamp; NewV++ )
			{
				for( INT NewU = 0; NewU < NewUClamp; NewU++ )
				{
					UBOOL Lit = 0;
					for( INT dv = 0; dv < ScaleFactor && !Lit; dv++ )
					{
						const INT SrcV = NewV * ScaleFactor + dv;
						if( SrcV >= OldVClamp ) break;
						for( INT du = 0; du < ScaleFactor && !Lit; du++ )
						{
							const INT SrcU = NewU * ScaleFactor + du;
							if( SrcU >= OldUClamp ) break;
							Lit = GetShadowBit( SrcLight, OldUClamp, SrcU, SrcV );
						}
					}
					if( Lit )
						SetShadowBit( DstLight, NewUClamp, NewU, NewV );
				}
			}
		}

		TotalOldBytes += OldMaskSpace * NumLights;
		TotalNewBytes += NewMaskSpace * NumLights;

		// Update the index
		Index.DataOffset = NewOffset;
		Index.UScale *= ScaleFactor;
		Index.VScale *= ScaleFactor;
		Index.UClamp = NewUClamp;
		Index.VClamp = NewVClamp;
		Index.UBits = 0; { INT v = NewUClamp; while( v > 1 ) { v >>= 1; Index.UBits++; } }
		Index.VBits = 0; { INT v = NewVClamp; while( v > 1 ) { v >>= 1; Index.VBits++; } }

		NumReduced++;
	}

	// Replace LightBits (always tagged with the magic prefix now).
	const INT OldTotal = Model->LightBits.Num();
	const INT NewTotal = NewBits.Num();
	Model->LightBits = NewBits;

	// Report net savings for accounting; clamp at 0 (tiny models may not shrink
	// once the 4-byte sentinel is added, but they're still correctly tagged).
	return (NewTotal < OldTotal) ? (DWORD)(OldTotal - NewTotal) : 0;

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

		// Idempotency guard: if the level model's LightBits already carries the
		// reduction sentinel, this map was already processed — skip it entirely
		// so we never double-reduce (which would corrupt the data).
		if( IsLightmapAlreadyReduced( Level->Model ) )
		{
			printf( "  Already processed (LightBits tagged) — skipping '%s'\n", Pkg->GetName() );
			return;
		}
	}
	
	// Force load all objects in the package to ensure we process everything
	// On Dreamcast, LoadPackage doesn't eagerly load, so we need to explicitly load
	// Loading MyLevel should trigger loading of related objects, but we'll also
	// iterate through all models to make sure we catch everything
	
	UBOOL Changed = false;
	DWORD TotalSaved = 0;

	// Reduce lightmap resolution (quarter each dimension = up to 16x fewer texels).
	// NOTE: No RLE compression — the engine has no LightBits RLE decoder
	// (UModel::Serialize does a plain Ar << LightBits), and RLE would not reduce
	// runtime RAM anyway (it decompresses to full size on load). Only the
	// resolution downsample reduces RAM, and the data stays raw/loadable.
	for( TObjectIterator<UModel> It; It; ++It )
	{
		UModel* Model = *It;
		if( !Model->IsIn( Pkg ) || Model->LightBits.Num() == 0 || Model->LightMap.Num() == 0 )
			continue;

		const DWORD Before = Model->LightBits.Num();
		DWORD Saved = ReduceLightmapResolution( Model, 4 ); // 4x = quarter each dimension
		if( Saved > 0 )
		{
			Changed = true;
			TotalSaved += Saved;
			Model->Modify();
			printf( "  %s: LightBits %u -> %u bytes\n", Model->GetName(), Before, Model->LightBits.Num() );
		}
	}

	if( Changed )
	{
		printf( "  Total LightBits saved: %u bytes (%.1f KB)\n", TotalSaved, TotalSaved / 1024.0f );
		ChangedPackages.Add( PkgPath, Pkg );
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

/*-----------------------------------------------------------------------------
	ReportTextures: report-only scan of texture dimensions (no modification).

	Finds textures below the PVR 8x8 minimum (which currently hit the runtime
	I8 expand+upscale path) and splits them into static vs parametric/realtime,
	since static ones can be padded offline but parametric/realtime ones must be
	bumped to 8x8 at load (they are regenerated each frame).
-----------------------------------------------------------------------------*/

void FDCUtil::ReportTextures( const char* Glob )
{
	guard(ReportTextures);

	printf( "=======================================================================\n" );
	printf( " Texture min-size report (read-only)\n" );
	printf( " PVR minimum texture size is 8x8; anything smaller hits runtime padding.\n" );
	printf( "=======================================================================\n\n" );

	TArray<FString> Files = appFindFiles( Glob );
	if( Files.Num() == 0 )
	{
		printf( "No files found matching '%s'\n", Glob );
		return;
	}

	// Directory portion of the glob.
	char Dir[2048];
	appStrcpy( Dir, Glob );
	char* Slash = strrchr( Dir, '/' );
	if( !Slash ) Slash = strrchr( Dir, '\\' );
	if( Slash ) *(Slash + 1) = 0;
	else Dir[0] = 0;

	// Histogram buckets for min(USize,VSize): 1,2,4,8,16,32,64,128,>=256
	INT Hist[9] = {0};
	auto BucketOf = []( INT D ) -> INT
	{
		if( D <= 1 ) return 0;
		if( D <= 2 ) return 1;
		if( D <= 4 ) return 2;
		if( D <= 8 ) return 3;
		if( D <= 16 ) return 4;
		if( D <= 32 ) return 5;
		if( D <= 64 ) return 6;
		if( D <= 128 ) return 7;
		return 8;
	};

	INT TotalTex = 0;
	INT Sub8Static = 0;
	INT Sub8Dynamic = 0;   // parametric/realtime/realtimepalette
	// Category totals (any size) to confirm flag detection works.
	INT TotParam = 0, TotRealtime = 0, TotRTPal = 0;
	// Distinct palettes (HW PVR palette feasibility: only 1024 entries = 4x256).
	TArray<UPalette*> AllPals;        // every distinct palette
	TArray<UPalette*> DynPals;        // palettes used by realtime/parametric textures

	// Record packages already resident (Core/Engine/Editor/etc.) so we never
	// ResetLoaders on them — doing so unloads the engine's own packages and crashes.
	TArray<UPackage*> PreLoaded;
	for( TObjectIterator<UPackage> It; It; ++It )
		PreLoaded.AddItem( *It );

	for( INT i = 0; i < Files.Num(); i++ )
	{
		char FullPath[2048];
		snprintf( FullPath, sizeof(FullPath), "%s%s", Dir, *Files(i) );

		UPackage* Pkg = Cast<UPackage>( GObj.LoadPackage( nullptr, FullPath, LOAD_KeepImports ) );
		if( !Pkg )
		{
			printf( "FAILED to load: %s\n", *Files(i) );
			continue;
		}

		for( TObjectIterator<UTexture> It; It; ++It )
		{
			if( !It->IsIn( Pkg ) )
				continue;

			UTexture* T = *It;
			const INT U = T->USize, V = T->VSize;
			const INT MinDim = Min( U, V );
			const UBOOL bDynamic = ( T->TextureFlags & (TF_Realtime|TF_RealtimePalette|TF_Parametric) ) != 0;

			TotalTex++;
			Hist[ BucketOf( MinDim ) ]++;
			if( T->TextureFlags & TF_Parametric )      TotParam++;
			if( T->TextureFlags & TF_Realtime )        TotRealtime++;
			if( T->TextureFlags & TF_RealtimePalette ) TotRTPal++;

			if( T->Palette )
			{
				AllPals.AddUniqueItem( T->Palette );
				if( bDynamic )
					DynPals.AddUniqueItem( T->Palette );
			}

			if( MinDim < 8 )
			{
				if( bDynamic ) Sub8Dynamic++; else Sub8Static++;
				const char* Kind =
					( T->TextureFlags & TF_Parametric )      ? "PARAM" :
					( T->TextureFlags & TF_RealtimePalette ) ? "RTPAL" :
					( T->TextureFlags & TF_Realtime )        ? "RT"    : "static";
				printf( "  SUB-8: %-28s %3dx%-3d fmt=%d %s\n",
					T->GetPathName(), U, V, T->Format, Kind );
			}
		}

		// Only unload packages we loaded fresh; never the engine's resident ones.
		UBOOL WasPreloaded = false;
		for( INT k = 0; k < PreLoaded.Num(); k++ )
			if( PreLoaded(k) == Pkg ) { WasPreloaded = true; break; }
		if( !WasPreloaded )
			GObj.ResetLoaders( Pkg );
	}

	printf( "\n" );
	printf( "min(USize,VSize) histogram:\n" );
	printf( "  ==1:%d  ==2:%d  <=4:%d  ==8:%d  <=16:%d  <=32:%d  <=64:%d  <=128:%d  >=256:%d\n",
		Hist[0], Hist[1], Hist[2], Hist[3], Hist[4], Hist[5], Hist[6], Hist[7], Hist[8] );
	printf( "\n" );
	printf( "Total textures:            %d\n", TotalTex );
	printf( "  of which Parametric=%d, Realtime=%d, RealtimePalette=%d (any size)\n",
		TotParam, TotRealtime, TotRTPal );
	printf( "Sub-8x8 static (pad offline in DCUtil):        %d\n", Sub8Static );
	printf( "Sub-8x8 parametric/realtime (bump at load):    %d\n", Sub8Dynamic );
	printf( "Distinct palettes: %d total, %d used by realtime/parametric\n",
		AllPals.Num(), DynPals.Num() );
	printf( "  (PVR HW palette holds 1024 entries = 4x256-color banks)\n" );
	printf( "=======================================================================\n" );

	unguard;
}

