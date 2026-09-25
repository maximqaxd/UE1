#include "EnginePrivate.h"
#include <zlib.h>

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)

static const INT DCFrameTag = -0x44434631;
static const INT DCFrameTemporalTag = -0x44434632;
static const INT DCTopologyTag = -0x44435431;
static const INT DCTopologyClusterTag = -0x44435432;
static const INT DCTopologyNormalTag = -0x44435433;
static const INT DCTopologyNormalZTag = -0x44435434;
static const INT DCTopologyNormalQTag = -0x44435435;

#if defined(DC_RESOURCE_COOKER)
static _WORD DCPackMeshNormal( const FVector& Sum )
{
	const FLOAT Length2 = Sum.SizeSquared();
	if( Length2 <= 0.000001f )
		return 0xffff;
	const FVector N = Sum * (1.f / appSqrt(Length2));
	const FLOAT InvL1 = 1.f / (Abs(N.X) + Abs(N.Y) + Abs(N.Z));
	FLOAT X = N.X * InvL1, Y = N.Y * InvL1;
	if( N.Z < 0.f )
	{
		const FLOAT OldX = X;
		X = (1.f - Abs(Y)) * (OldX < 0.f ? -1.f : 1.f);
		Y = (1.f - Abs(OldX)) * (Y < 0.f ? -1.f : 1.f);
	}
	const INT BX = Clamp( appRound(X * 127.f) + 127, 0, 254 );
	const INT BY = Clamp( appRound(Y * 127.f) + 127, 0, 254 );
	return (_WORD)(BX | (BY << 8));
}
#endif

static FVector DCUnpackMeshNormal( _WORD Packed )
{
	if( Packed == 0xffff )
		return FVector(0,0,0);
	FLOAT X = (FLOAT)((Packed & 255) - 127) * (1.f / 127.f);
	FLOAT Y = (FLOAT)(((Packed >> 8) & 255) - 127) * (1.f / 127.f);
	FLOAT Z = 1.f - Abs(X) - Abs(Y);
	if( Z < 0.f )
	{
		const FLOAT OldX = X;
		X = (1.f - Abs(Y)) * (OldX < 0.f ? -1.f : 1.f);
		Y = (1.f - Abs(OldX)) * (Y < 0.f ? -1.f : 1.f);
	}
	return FVector(X,Y,Z);
}

#if defined(PLATFORM_DREAMCAST)
enum { DCNormalCacheFrames = 128, DCNormalMaxVertices = 512, DCNormalReadFrames = 16 };
struct FDCNormalFrame
{
	const UMesh* Mesh;
	INT Frame;
	DWORD Stamp;
	_WORD Words[DCNormalMaxVertices];
};
static FDCNormalFrame GDCNormalFrames[DCNormalCacheFrames];
static DWORD GDCNormalFrameStamp = 0;
static _WORD GDCNormalReadScratch[DCNormalMaxVertices * DCNormalReadFrames];
static BYTE GDCNormalPackedScratch[DCNormalMaxVertices * DCNormalReadFrames * 2 + 256];
static BYTE GDCNormalQuantScratch[DCNormalMaxVertices * DCNormalReadFrames * 2];

static _WORD DCExpandQuantizedNormal( const BYTE* Packed, INT Vertex )
{
	const INT Bit = Vertex * 10;
	const INT Byte = Bit >> 3;
	const INT Shift = Bit & 7;
	const DWORD Code = ((DWORD)Packed[Byte] | ((DWORD)Packed[Byte+1] << 8)
		| ((DWORD)Packed[Byte+2] << 16)) >> Shift;
	const INT X = ((Code & 31) * 254 + 15) / 31;
	const INT Y = (((Code >> 5) & 31) * 254 + 15) / 31;
	return (_WORD)(X | (Y << 8));
}

static const _WORD* DCGetNormalFrame( const UMesh& Mesh, INT Frame )
{
	if( Mesh.DCNormalWords.Num() )
		return &Mesh.DCNormalWords(Frame * Mesh.FrameVerts);
	if( (!Mesh.DCNormalStreamData.Size() && !Mesh.DCNormalCompressed.Num())
		|| Mesh.FrameVerts > DCNormalMaxVertices )
		return NULL;
	for( INT i = 0; i < DCNormalCacheFrames; ++i )
		if( GDCNormalFrames[i].Mesh == &Mesh && GDCNormalFrames[i].Frame == Frame )
		{
			GDCNormalFrames[i].Stamp = ++GDCNormalFrameStamp;
			return GDCNormalFrames[i].Words;
		}
	const INT First = Frame & ~(DCNormalReadFrames - 1);
	const INT Count = Min( (INT)DCNormalReadFrames, Mesh.AnimFrames - First );
	const INT BytesPerFrame = Mesh.FrameVerts * (INT)sizeof(_WORD);
	if( Mesh.DCNormalBlockOffsets.Num() )
	{
		const INT Block = First / DCNormalReadFrames;
		const INT Begin = Mesh.DCNormalBlockOffsets(Block);
		const INT PackedSize = Mesh.DCNormalBlockOffsets(Block+1) - Begin;
		if( PackedSize <= 0 || PackedSize > (INT)sizeof(GDCNormalPackedScratch) )
			appErrorf( "Invalid cooked mesh normal block" );
		if( Mesh.DCNormalStreamData.Size() )
			Mesh.DCNormalStreamData.ReadRange( Begin, GDCNormalPackedScratch, PackedSize );
		else
			appMemcpy( GDCNormalPackedScratch, &Mesh.DCNormalCompressed(Begin), PackedSize );
		const INT QuantFrameBytes = (Mesh.FrameVerts * 10 + 7) / 8;
		const INT StoredFrameBytes = Mesh.DCQuantizedNormals ? QuantFrameBytes : BytesPerFrame;
		BYTE* Output = Mesh.DCQuantizedNormals ? GDCNormalQuantScratch : (BYTE*)GDCNormalReadScratch;
		uLongf RawSize = Count * StoredFrameBytes;
		if( uncompress( Output, &RawSize,
			GDCNormalPackedScratch, PackedSize ) != Z_OK || RawSize != (uLongf)(Count * StoredFrameBytes) )
			appErrorf( "Cooked mesh normal block inflate failed" );
	}
	else
		Mesh.DCNormalStreamData.ReadRange( First * BytesPerFrame,
			GDCNormalReadScratch, Count * BytesPerFrame );
	if( Mesh.DCNormalBlockOffsets.Num() )
	{
		const INT StoredFrameBytes = Mesh.DCQuantizedNormals
			? (Mesh.FrameVerts * 10 + 7) / 8 : BytesPerFrame;
		BYTE* Bytes = Mesh.DCQuantizedNormals ? GDCNormalQuantScratch
			: (BYTE*)GDCNormalReadScratch;
		for( INT j = 1; j < Count; ++j )
			for( INT k = 0; k < StoredFrameBytes; ++k )
				Bytes[j * StoredFrameBytes + k] += Bytes[(j-1) * StoredFrameBytes + k];
		if( Mesh.DCQuantizedNormals )
			for( INT j = 0; j < Count; ++j )
				for( INT Vertex = 0; Vertex < Mesh.FrameVerts; ++Vertex )
					GDCNormalReadScratch[j * Mesh.FrameVerts + Vertex]
						= DCExpandQuantizedNormal( Bytes + j * StoredFrameBytes, Vertex );
	}
	const _WORD* Result = NULL;
	for( INT j = 0; j < Count; ++j )
	{
		FDCNormalFrame* Slot = NULL;
		for( INT i = 0; i < DCNormalCacheFrames; ++i )
			if( GDCNormalFrames[i].Mesh == &Mesh && GDCNormalFrames[i].Frame == First + j )
			{
				Slot = &GDCNormalFrames[i];
				break;
			}
		if( !Slot )
		{
			Slot = &GDCNormalFrames[0];
			for( INT i = 1; i < DCNormalCacheFrames; ++i )
				if( !GDCNormalFrames[i].Mesh || GDCNormalFrames[i].Stamp < Slot->Stamp )
					Slot = &GDCNormalFrames[i];
		}
		appMemcpy( Slot->Words, GDCNormalReadScratch + j * Mesh.FrameVerts, BytesPerFrame );
		Slot->Mesh = &Mesh;
		Slot->Frame = First + j;
		Slot->Stamp = ++GDCNormalFrameStamp;
		if( Slot->Frame == Frame )
			Result = Slot->Words;
	}
	return Result;
}
#endif

static INT DCFrameWordCount( const UMesh& Mesh )
{
	return Mesh.DCFrameStreamData.Size() ? Mesh.DCFrameStreamData.Size() / sizeof(_WORD)
		: Mesh.DCFrameWords.Num();
}

static INT SignedBits( DWORD Value, INT Bits )
{
	INT Mask = 1 << (Bits - 1);
	return (INT)(Value & ((1 << Bits) - 1)) - ((Value & Mask) ? (1 << Bits) : 0);
}

#if defined(PLATFORM_DREAMCAST)
struct FDCDecodedMeshFrame
{
	const UMesh* Mesh;
	INT Frame;
	DWORD Stamp;
	TArray<FMeshVert> Verts;

	FDCDecodedMeshFrame()
		: Mesh(NULL), Frame(INDEX_NONE), Stamp(0)
	{}
};

static FDCDecodedMeshFrame GDCDecodedMeshFrames[2];
static TArray<_WORD> GDCMeshFrameScratch;
static DWORD GDCMeshFrameStamp = 0;

enum
{
	DCMeshPageSize = 8 * 1024,
	DCMeshPageCount = 4
};

struct FDCMeshPage
{
	const UMesh* Mesh;
	INT Start;
	INT Count;
	DWORD Stamp;
	BYTE Data[DCMeshPageSize];

	FDCMeshPage()
		: Mesh(NULL), Start(0), Count(0), Stamp(0)
	{}
};

static FDCMeshPage GDCMeshPages[DCMeshPageCount];

DWORD GetDCMeshDecodeCacheBytes()
{
	return sizeof(GDCDecodedMeshFrames) + sizeof(GDCMeshFrameScratch) + sizeof(GDCMeshPages)
		+ GDCDecodedMeshFrames[0].Verts.ArrayMax * sizeof(FMeshVert)
		+ GDCDecodedMeshFrames[1].Verts.ArrayMax * sizeof(FMeshVert)
		+ GDCMeshFrameScratch.ArrayMax * sizeof(_WORD);
}

static void DCReadStreamedMeshBytes( const UMesh& Mesh, INT Offset, void* Destination, INT Count )
{
	BYTE* Output = (BYTE*)Destination;
	while( Count )
	{
		INT PageStart = Offset & ~(DCMeshPageSize - 1);
		FDCMeshPage* Page = NULL;
		for( INT i = 0; i < DCMeshPageCount; ++i )
		{
			if( GDCMeshPages[i].Mesh == &Mesh && GDCMeshPages[i].Start == PageStart )
			{
				Page = &GDCMeshPages[i];
				break;
			}
		}
		if( !Page )
		{
			Page = &GDCMeshPages[0];
			for( INT i = 1; i < DCMeshPageCount; ++i )
				if( GDCMeshPages[i].Stamp < Page->Stamp )
					Page = &GDCMeshPages[i];
			Page->Mesh = NULL;
			Page->Start = PageStart;
			Page->Count = Min( (INT)DCMeshPageSize, Mesh.DCFrameStreamData.Size() - PageStart );
			if( Page->Count <= 0 )
				appErrorf( "Invalid streamed mesh page: %s", Mesh.GetPathName() );
			Mesh.DCFrameStreamData.ReadRange( PageStart, Page->Data, Page->Count );
			Page->Mesh = &Mesh;
		}
		Page->Stamp = ++GDCMeshFrameStamp;
		INT Skip = Offset - PageStart;
		INT Bytes = Min( Count, Page->Count - Skip );
		if( Skip < 0 || Bytes <= 0 )
			appErrorf( "Invalid streamed mesh page range: %s", Mesh.GetPathName() );
		appMemcpy( Output, Page->Data + Skip, Bytes );
		Output += Bytes;
		Offset += Bytes;
		Count -= Bytes;
	}
}

static void DCReadMeshFrameWords( const UMesh& Mesh, INT Frame )
{
	INT WordCount = DCFrameWordCount( Mesh );
	INT Start = Mesh.DCFrameOffsets(Frame);
	INT End = Frame + 1 < Mesh.DCFrameOffsets.Num()
		? Mesh.DCFrameOffsets(Frame + 1) : WordCount;
	if( Start < 0 || End < Start || End > WordCount || End - Start < Mesh.FrameVerts
		|| End - Start > Mesh.FrameVerts * 2 )
	{
		appErrorf( "Invalid cooked mesh frame range: %s", Mesh.GetPathName() );
	}

	GDCMeshFrameScratch.SetNum( End - Start );
	if( Mesh.DCFrameStreamData.Size() )
	{
		DCReadStreamedMeshBytes( Mesh, Start * sizeof(_WORD),
			&GDCMeshFrameScratch(0), (End - Start) * sizeof(_WORD) );
	}
	else
	{
		appMemcpy( &GDCMeshFrameScratch(0), &Mesh.DCFrameWords(Start),
			(End - Start) * sizeof(_WORD) );
	}
}

static void DCApplyMeshFrame( const UMesh& Mesh, INT Frame, UBOOL KeyFrame,
	TArray<FMeshVert>& Result )
{
	DCReadMeshFrameWords( Mesh, Frame );
	INT Position = 0;
	INT X = 0;
	INT Y = 0;
	INT Z = 0;
	for( INT Vertex = 0; Vertex < Mesh.FrameVerts; ++Vertex )
	{
		if( !KeyFrame )
		{
			X = Result(Vertex).X;
			Y = Result(Vertex).Y;
			Z = Result(Vertex).Z;
		}
		if( Position >= GDCMeshFrameScratch.Num() )
			appErrorf( "Truncated cooked mesh frame: %s", Mesh.GetPathName() );
		DWORD Word = GDCMeshFrameScratch(Position++);
		if( Word & 0x8000 )
		{
			X += SignedBits( Word >> 10, 5 );
			Y += SignedBits( Word >> 5, 5 );
			Z += SignedBits( Word, 5 );
		}
		else
		{
			if( Position >= GDCMeshFrameScratch.Num() )
				appErrorf( "Truncated absolute mesh vertex: %s", Mesh.GetPathName() );
			DWORD Packed = (Word << 16) | GDCMeshFrameScratch(Position++);
			X = SignedBits( Packed >> 21, 10 ) * 2;
			Y = SignedBits( Packed >> 10, 11 );
			Z = SignedBits( Packed, 10 );
		}
		Result(Vertex) = FMeshVert( FVector(X, Y, Z) );
	}
	if( Position != GDCMeshFrameScratch.Num() )
		appErrorf( "Cooked frame has trailing words: %s", Mesh.GetPathName() );
}

static const FMeshVert* DCDecodeMeshFrame( const UMesh& Mesh, INT Frame )
{
	for( INT i = 0; i < 2; ++i )
	{
		if( GDCDecodedMeshFrames[i].Mesh == &Mesh && GDCDecodedMeshFrames[i].Frame == Frame )
		{
			GDCDecodedMeshFrames[i].Stamp = ++GDCMeshFrameStamp;
			return &GDCDecodedMeshFrames[i].Verts(0);
		}
	}

	INT Slot = GDCDecodedMeshFrames[0].Stamp <= GDCDecodedMeshFrames[1].Stamp ? 0 : 1;
	FDCDecodedMeshFrame& Cache = GDCDecodedMeshFrames[Slot];
	Cache.Mesh = NULL;
	Cache.Frame = INDEX_NONE;
	Cache.Verts.SetNum( Mesh.FrameVerts );
	INT KeyFrame = Mesh.DCTemporalFrames ? Frame & ~7 : Frame;
	DCApplyMeshFrame( Mesh, KeyFrame, 1, Cache.Verts );
	for( INT SourceFrame = KeyFrame + 1; SourceFrame <= Frame; ++SourceFrame )
		DCApplyMeshFrame( Mesh, SourceFrame, 0, Cache.Verts );
	Cache.Mesh = &Mesh;
	Cache.Frame = Frame;
	Cache.Stamp = ++GDCMeshFrameStamp;
	return &Cache.Verts(0);
}
#endif

FDCMeshFrameCursor::FDCMeshFrameCursor( const UMesh& InMesh, INT Frame )
	: Mesh(InMesh), X(0), Y(0), Z(0), TargetFrame(Frame), KeyFrame(Frame)
#if defined(PLATFORM_DREAMCAST)
	, Decoded(NULL)
#endif
{
	if( Frame < 0 || Frame >= Mesh.AnimFrames )
	{
		appErrorf( "Invalid mesh frame %i: %s", Frame, Mesh.GetPathName() );
	}
	if( Mesh.DCFrameOffsets.Num() )
	{
#if defined(PLATFORM_DREAMCAST)
		Decoded = DCDecodeMeshFrame( Mesh, Frame );
		Positions[0] = 0;
		Ends[0] = Mesh.FrameVerts;
		return;
#else
		KeyFrame = Mesh.DCTemporalFrames ? Frame & ~7 : Frame;
		INT WordCount = DCFrameWordCount( Mesh );
		for( INT i = 0; i < 8; ++i )
		{
			INT SourceFrame = KeyFrame + i;
			Positions[i] = SourceFrame <= Frame ? Mesh.DCFrameOffsets(SourceFrame) : 0;
			Ends[i] = SourceFrame <= Frame
				? (SourceFrame + 1 < Mesh.DCFrameOffsets.Num()
					? Mesh.DCFrameOffsets(SourceFrame + 1) : WordCount) : 0;
			if( SourceFrame <= Frame && (Positions[i] < 0 || Ends[i] < Positions[i]
				|| Ends[i] > WordCount) )
			{
				appErrorf( "Invalid cooked mesh frame range: %s", Mesh.GetPathName() );
			}
		}
#endif
	}
	else
	{
		Positions[0] = Frame * Mesh.FrameVerts;
		Ends[0] = Positions[0] + Mesh.FrameVerts;
	}
}

FVector FDCMeshFrameCursor::Next()
{
	if( Positions[0] >= Ends[0] )
	{
		appErrorf( "Truncated mesh frame: %s", Mesh.GetPathName() );
	}
	if( !Mesh.DCFrameOffsets.Num() )
	{
		return Mesh.Verts(Positions[0]++).Vector();
	}
#if defined(PLATFORM_DREAMCAST)
	if( Decoded )
		return Decoded[Positions[0]++].Vector();
#endif
	DWORD Word = Mesh.DCFrameWords(Positions[0]++);
	if( Word & 0x8000 )
	{
		X += SignedBits( Word >> 10, 5 );
		Y += SignedBits( Word >> 5, 5 );
		Z += SignedBits( Word, 5 );
	}
	else
	{
		if( Positions[0] >= Ends[0] )
		{
			appErrorf( "Truncated absolute mesh vertex" );
		}
		DWORD Packed = (Word << 16) | Mesh.DCFrameWords(Positions[0]++);
		X = SignedBits( Packed >> 21, 10 ) * 2;
		Y = SignedBits( Packed >> 10, 11 );
		Z = SignedBits( Packed, 10 );
	}
	for( INT Frame = KeyFrame + 1; Frame <= TargetFrame; ++Frame )
	{
		INT Slot = Frame - KeyFrame;
		if( Positions[Slot] >= Ends[Slot] )
			appErrorf( "Truncated temporal mesh frame: %s", Mesh.GetPathName() );
		DWORD Delta = Mesh.DCFrameWords(Positions[Slot]++);
		if( Delta & 0x8000 )
		{
			X += SignedBits( Delta >> 10, 5 );
			Y += SignedBits( Delta >> 5, 5 );
			Z += SignedBits( Delta, 5 );
		}
		else
		{
			if( Positions[Slot] >= Ends[Slot] )
				appErrorf( "Truncated temporal absolute vertex" );
			DWORD Packed = (Delta << 16) | Mesh.DCFrameWords(Positions[Slot]++);
			X = SignedBits( Packed >> 21, 10 ) * 2;
			Y = SignedBits( Packed >> 10, 11 );
			Z = SignedBits( Packed, 10 );
		}
	}
	return FVector( X, Y, Z );
}

UBOOL FDCMeshFrameCursor::AtEnd() const
{
#if defined(PLATFORM_DREAMCAST)
	if( Decoded )
		return Positions[0] == Ends[0];
#endif
	for( INT i = 0; i <= TargetFrame - KeyFrame; ++i )
		if( Positions[i] != Ends[i] )
			return 0;
	return 1;
}

void UMesh::SerializeDCVerts( FArchive& Ar )
{
	if( Ar.IsLoading() )
	{
		DCFrameStreamData = FDCStreamSlice();
#if defined(PLATFORM_DREAMCAST)
		for( INT i = 0; i < 2; ++i )
		{
			if( GDCDecodedMeshFrames[i].Mesh == this )
			{
				GDCDecodedMeshFrames[i].Mesh = NULL;
				GDCDecodedMeshFrames[i].Frame = INDEX_NONE;
			}
		}
		for( INT i = 0; i < DCMeshPageCount; ++i )
		{
			if( GDCMeshPages[i].Mesh == this )
				GDCMeshPages[i].Mesh = NULL;
		}
#endif
	}
	if( !Ar.IsLoading() && !Ar.IsSaving() )
	{
		Ar << Verts << DCFrameOffsets << DCFrameWords;
		return;
	}
	INT Count = DCFrameOffsets.Num() ? (DCTemporalFrames ? DCFrameTemporalTag : DCFrameTag) : Verts.Num();
	Ar << AR_INDEX(Count);
	if( Count == DCFrameTag || Count == DCFrameTemporalTag )
	{
		if( Ar.IsLoading() )
		{
			DCTemporalFrames = Count == DCFrameTemporalTag;
		}
		Ar << DCFrameOffsets;
		if( Ar.IsLoading() && appDCStreamActive() )
		{
			INT WordCount = 0;
			Ar << AR_INDEX(WordCount);
			FArchiveFileLoad& File = (FArchiveFileLoad&)Ar;
			if( WordCount <= 0 || WordCount > 32 * 1024 * 1024
				|| File.Tell() < 0 || File.Tell() > File.Eof
				|| WordCount > (File.Eof - File.Tell()) / (INT)sizeof(_WORD) )
			{
				appErrorf( "Invalid deferred DAT mesh frame length: %d", WordCount );
			}
			INT ByteCount = WordCount * sizeof(_WORD);
			appDCStreamCapture( File.Filename, File.Tell(), ByteCount, DCFrameStreamData );
#if defined(DC_RESOURCE_COOKER)
			DCFrameWords.Empty();
			DCFrameWords.SetNum( WordCount );
			File.Serialize( &DCFrameWords(0), ByteCount );
#else
			DCFrameWords.Empty();
			BYTE Scratch[2048];
			for( INT Remaining = ByteCount; Remaining; )
			{
				INT Bytes = Min( Remaining, (INT)sizeof(Scratch) );
				File.Serialize( Scratch, Bytes );
				Remaining -= Bytes;
			}
#endif
		}
		else
		{
			Ar << DCFrameWords;
		}
	}
	else
	{
		if( Count < 0 || Count > 16 * 1024 * 1024 )
		{
			appErrorf( "Invalid mesh vertex count" );
		}
		if( Ar.IsLoading() )
		{
			Verts.SetNum( Count );
		}
		for( INT i = 0; i < Count; ++i )
		{
			Ar << Verts(i);
		}
	}
}

void UMesh::SerializeDCTopology( FArchive& Ar )
{
	if( Ar.IsLoading() )
	{
		DCNormalStreamData = FDCStreamSlice();
#if defined(PLATFORM_DREAMCAST)
		for( INT i = 0; i < DCNormalCacheFrames; ++i )
			if( GDCNormalFrames[i].Mesh == this )
				GDCNormalFrames[i].Mesh = NULL;
#endif
	}
	if( !Ar.IsLoading() && !Ar.IsSaving() )
	{
		Ar << Tris << DCRuns << DCMaterials << DCIndices << DCUVs
			<< DCNormalWords << DCNormalBlockOffsets << DCNormalCompressed;
		return;
	}
	INT Count = DCRuns.Num() ? (DCNormalBlockOffsets.Num()
		? (DCQuantizedNormals ? DCTopologyNormalQTag : DCTopologyNormalZTag)
		: DCNormalWords.Num() ? DCTopologyNormalTag
		: (DCClusteredRuns ? DCTopologyClusterTag : DCTopologyTag)) : Tris.Num();
	Ar << AR_INDEX(Count);
	if( Count == DCTopologyTag || Count == DCTopologyClusterTag
		|| Count == DCTopologyNormalTag || Count == DCTopologyNormalZTag
		|| Count == DCTopologyNormalQTag )
	{
		if( Ar.IsLoading() )
		{
			DCClusteredRuns = Count == DCTopologyClusterTag
				|| Count == DCTopologyNormalTag || Count == DCTopologyNormalZTag
				|| Count == DCTopologyNormalQTag;
			DCQuantizedNormals = Count == DCTopologyNormalQTag;
		}
		Ar << DCRuns << DCMaterials << DCIndices << DCUVs;
		if( Count == DCTopologyNormalTag || Count == DCTopologyNormalZTag
			|| Count == DCTopologyNormalQTag )
		{
			if( Count == DCTopologyNormalZTag || Count == DCTopologyNormalQTag )
				Ar << DCNormalBlockOffsets;
#if defined(PLATFORM_DREAMCAST)
			if( Ar.IsLoading() && appDCStreamActive() )
			{
				INT ItemCount = 0;
				Ar << AR_INDEX(ItemCount);
				FArchiveFileLoad& File = (FArchiveFileLoad&)Ar;
				const INT ItemSize = Count == DCTopologyNormalTag ? (INT)sizeof(_WORD) : 1;
				if( ItemCount <= 0 || ItemCount > 16 * 1024 * 1024
					|| File.Tell() < 0 || File.Tell() > File.Eof
					|| ItemCount > (File.Eof - File.Tell()) / ItemSize )
					appErrorf( "Invalid deferred DAT mesh normal length" );
				const INT ByteCount = ItemCount * ItemSize;
				appDCStreamCapture( File.Filename, File.Tell(), ByteCount, DCNormalStreamData );
				DCNormalWords.Empty();
				DCNormalCompressed.Empty();
				BYTE Scratch[2048];
				for( INT Remaining = ByteCount; Remaining; )
				{
					const INT Bytes = Min( Remaining, (INT)sizeof(Scratch) );
					File.Serialize( Scratch, Bytes );
					Remaining -= Bytes;
				}
			}
			else
#endif
			{
				if( Count == DCTopologyNormalZTag || Count == DCTopologyNormalQTag )
					Ar << DCNormalCompressed;
				else
					Ar << DCNormalWords;
			}
		}
		else if( Ar.IsLoading() )
		{
			DCNormalWords.Empty();
			DCNormalBlockOffsets.Empty();
			DCNormalCompressed.Empty();
		}
	}
	else
	{
		if( Count < 0 || Count > 1024 * 1024 )
		{
			appErrorf( "Invalid mesh triangle count" );
		}
		if( Ar.IsLoading() )
		{
			Tris.SetNum( Count );
		}
		for( INT i = 0; i < Count; ++i )
		{
			Ar << Tris(i);
		}
	}
}

void UMesh::ValidateDCMesh()
{
	if( DCNormalBlockOffsets.Num() )
	{
		const INT Blocks = (AnimFrames + 15) / 16;
		const INT PackedSize = DCNormalStreamData.Size() ? DCNormalStreamData.Size()
			: DCNormalCompressed.Num();
		if( FrameVerts <= 0 || FrameVerts > 512 || AnimFrames <= 0
			|| DCNormalBlockOffsets.Num() != Blocks + 1
			|| DCNormalBlockOffsets(0) != 0
			|| DCNormalBlockOffsets(Blocks) != PackedSize )
			appErrorf( "Invalid cooked mesh normal block directory" );
		for( INT i = 0; i < Blocks; ++i )
			if( DCNormalBlockOffsets(i+1) <= DCNormalBlockOffsets(i) )
				appErrorf( "Invalid cooked mesh normal block range" );
	}
	else if( (DCNormalWords.Num() || DCNormalStreamData.Size())
		&& (FrameVerts <= 0 || AnimFrames <= 0
		|| (QWORD)(DCNormalWords.Num() ? DCNormalWords.Num()
			: DCNormalStreamData.Size() / (INT)sizeof(_WORD)) != (QWORD)FrameVerts * AnimFrames) )
		appErrorf( "Invalid cooked mesh normal table: %s", GetPathName() );
	if( DCFrameOffsets.Num() )
	{
		INT WordCount = DCFrameWordCount( *this );
		if( FrameVerts <= 0 || AnimFrames != DCFrameOffsets.Num() || DCFrameOffsets(0) != 0
			|| WordCount <= 0 )
		{
			appErrorf( "Invalid cooked mesh frame directory" );
		}
		for( INT Frame = 0; Frame < AnimFrames; ++Frame )
		{
			INT End = Frame + 1 < AnimFrames ? DCFrameOffsets(Frame + 1) : WordCount;
			if( DCFrameOffsets(Frame) < 0 || End < DCFrameOffsets(Frame) || End > WordCount
				|| End - DCFrameOffsets(Frame) < FrameVerts
				|| End - DCFrameOffsets(Frame) > FrameVerts * 2 )
			{
				appErrorf( "Invalid cooked mesh frame directory" );
			}
		}
#if !defined(PLATFORM_DREAMCAST)
		for( INT Frame = 0; Frame < AnimFrames; ++Frame )
		{
			FDCMeshFrameCursor Cursor( *this, Frame );
			for( INT Vertex = 0; Vertex < FrameVerts; ++Vertex )
			{
				Cursor.Next();
			}
			if( !Cursor.AtEnd() )
			{
				appErrorf( "Cooked frame has trailing words" );
			}
		}
#endif
	}
	INT Next = 0;
	for( INT i = 0; i < DCRuns.Num(); ++i )
	{
		FDCMeshRun& Run = DCRuns(i);
		if( (!DCClusteredRuns && Run.Reserved) || Run.Count < 3 || Run.First != Next
			|| Run.Count > DCIndices.Num() - Next || Run.Material >= DCMaterials.Num() )
		{
			appErrorf( "Invalid cooked mesh strip" );
		}
		Next += Run.Count;
	}
	if( DCClusteredRuns && DCRuns.Num() )
	{
		TArray<BYTE> Seen( FrameVerts );
		appMemset( Seen.GetData(), 0, Seen.Num() );
		INT Group = 0;
		INT GroupVertices = 0;
		for( INT RunIndex = 0; RunIndex < DCRuns.Num(); ++RunIndex )
		{
			const FDCMeshRun& Run = DCRuns(RunIndex);
			if( Run.Reserved != Group )
			{
				if( Run.Reserved != Group + 1 )
					appErrorf( "Invalid cooked meshlet order" );
				Group = Run.Reserved;
				GroupVertices = 0;
				appMemset( Seen.GetData(), 0, Seen.Num() );
			}
			for( INT Slot = 0; Slot < Run.Count; ++Slot )
			{
				const INT Vertex = DCIndices(Run.First + Slot);
				if( Vertex >= FrameVerts )
					appErrorf( "Cooked meshlet vertex out of range" );
				if( !Seen(Vertex) )
				{
					Seen(Vertex) = 1;
					if( ++GroupVertices > 128 )
						appErrorf( "Cooked meshlet exceeds 128 vertices" );
				}
			}
		}
	}
	if( Next != DCIndices.Num() || DCIndices.Num() != DCUVs.Num() )
	{
		appErrorf( "Invalid cooked mesh index/UV count" );
	}
	for( INT i = 0; i < DCIndices.Num(); ++i )
	{
		if( DCIndices(i) >= FrameVerts )
		{
			appErrorf( "Cooked mesh vertex index out of range" );
		}
	}
}

#if defined(PLATFORM_DREAMCAST)
UBOOL UMesh::GetDCCookedNormals( FVector* Result, FCoords Coords, AActor* Owner ) const
{
	if( (!DCNormalWords.Num() && !DCNormalStreamData.Size()
		&& !DCNormalCompressed.Num()) || !Owner
		|| Owner->AnimFrame < 0.f || FrameVerts <= 0 || AnimFrames <= 0
		|| FrameVerts > DCNormalMaxVertices )
		return 0;
	// The mesh's fixed, possibly nonuniform Scale is already included in the
	// cooked normals. A positive DrawScale only changes length. Tweened poses
	// depend on the previous actor pose, so use live face normals there.
	const FLOAT DrawScale = Owner->bParticles ? 1.f : Owner->DrawScale;
	if( DrawScale <= 0.f )
		return 0;
	FLOAT Alpha = 0.f;
	INT Frame1 = 0, Frame2 = 0;
	const FMeshAnimSeq* Seq = GetAnimSeq( Owner->AnimSequence );
	if( Seq && Seq->NumFrames > 0 )
	{
		const FLOAT Frame = Max(Owner->AnimFrame,0.f) * Seq->NumFrames;
		const INT Index = appFloor(Frame);
		Alpha = Frame - Index;
		Frame1 = Seq->StartFrame + Index % Seq->NumFrames;
		Frame2 = Seq->StartFrame + (Index + 1) % Seq->NumFrames;
	}
	if( Frame1 < 0 || Frame2 < 0 || Frame1 >= AnimFrames || Frame2 >= AnimFrames )
		return 0;
	const _WORD* N1 = DCGetNormalFrame( *this, Frame1 );
	const _WORD* N2 = Frame1 == Frame2 ? N1 : DCGetNormalFrame( *this, Frame2 );
	if( !N1 || !N2 )
		return 0;
	Coords = Coords * (Owner->Location + Owner->PrePivot)
		* Owner->Rotation * RotOrigin;
	for( INT i = 0; i < FrameVerts; ++i )
	{
		const FVector A = DCUnpackMeshNormal(N1[i]);
		const FVector B = DCUnpackMeshNormal(N2[i]);
		Result[i] = (A + (B-A)*Alpha).TransformVectorBy(Coords);
	}
	return 1;
}
#endif

FMeshTri* UMesh::GetDCTriangles( INT& Count )
{
	Count = Tris.Num();
	if( !DCRuns.Num() )
	{
		return Count ? &Tris(0) : NULL;
	}
	Count = 0;
	for( INT i = 0; i < DCRuns.Num(); ++i )
	{
		Count += DCRuns(i).Count - 2;
	}
	FMeshTri* Result = New<FMeshTri>( GMem, Count );
	INT Triangle = 0;
	for( INT i = 0; i < DCRuns.Num(); ++i )
	{
		const FDCMeshRun& Run = DCRuns(i);
		for( INT j = 2; j < Run.Count; ++j )
		{
			FMeshTri& Tri = Result[Triangle++];
			Tri.PolyFlags = DCMaterials(Run.Material).Flags;
			Tri.TextureIndex = DCMaterials(Run.Material).Texture;
			INT Order[3] = { j - 2, j - 1, j };
			if( j & 1 )
			{
				Exchange( Order[0], Order[1] );
			}
			for( INT k = 0; k < 3; ++k )
			{
				INT Index = Run.First + Order[k];
				Tri.iVertex[k] = DCIndices(Index);
				Tri.Tex[k].U = DCUVs(Index) & 255;
				Tri.Tex[k].V = DCUVs(Index) >> 8;
			}
		}
	}
	return Result;
}

FDCMeshTriangleCursor::FDCMeshTriangleCursor( const UMesh& InMesh )
	: Mesh(InMesh), RunIndex(0), RunVertex(2), LastRunIndex(INDEX_NONE), LastRunVertex(INDEX_NONE)
{}

UBOOL FDCMeshTriangleCursor::Next( FMeshTri& Triangle, UBOOL IncludeUV )
{
	while( RunIndex < Mesh.DCRuns.Num() && RunVertex >= Mesh.DCRuns(RunIndex).Count )
	{
		++RunIndex;
		RunVertex = 2;
	}
	if( RunIndex >= Mesh.DCRuns.Num() )
	{
		return 0;
	}

	const FDCMeshRun& Run = Mesh.DCRuns(RunIndex);
	LastRunIndex = RunIndex;
	LastRunVertex = RunVertex;
	const FDCMeshMaterial& Material = Mesh.DCMaterials(Run.Material);
	Triangle.PolyFlags = Material.Flags;
	Triangle.TextureIndex = Material.Texture;
	INT Order[3] = { RunVertex - 2, RunVertex - 1, RunVertex };
	if( RunVertex & 1 )
	{
		Exchange( Order[0], Order[1] );
	}
	for( INT Corner = 0; Corner < 3; ++Corner )
	{
		INT Index = Run.First + Order[Corner];
		Triangle.iVertex[Corner] = Mesh.DCIndices(Index);
	}
	if( IncludeUV )
		LoadUV( Triangle, RunIndex, RunVertex );
	++RunVertex;
	return 1;
}

void FDCMeshTriangleCursor::LoadUV( FMeshTri& Triangle, INT RunIndex, INT Vertex ) const
{
	const FDCMeshRun& Run = Mesh.DCRuns(RunIndex);
	INT Order[3] = { Vertex - 2, Vertex - 1, Vertex };
	if( Vertex & 1 )
		Exchange( Order[0], Order[1] );
	for( INT Corner=0; Corner<3; ++Corner )
	{
		const _WORD UV = Mesh.DCUVs(Run.First + Order[Corner]);
		Triangle.Tex[Corner].U = UV & 255;
		Triangle.Tex[Corner].V = UV >> 8;
	}
}

#if defined(DC_RESOURCE_COOKER)
static _WORD PackedUV( const FMeshUV& UV )
{
	return UV.U | (UV.V << 8);
}

static void DCAppendAbsoluteVertex( TArray<_WORD>& Words, const FMeshVert& Vertex )
{
	INT X = Vertex.X & ~1;
	DWORD Packed = (((X / 2) & 1023) << 21) | ((Vertex.Y & 2047) << 10) | (Vertex.Z & 1023);
	Words.AddItem( Packed >> 16 );
	Words.AddItem( Packed & 65535 );
}

static void DCEncodeSpatialFrame( const FMeshVert* Verts, INT Count, TArray<_WORD>& Words )
{
	INT X = 0, Y = 0, Z = 0;
	for( INT i = 0; i < Count; ++i )
	{
		INT DX = Verts[i].X - X;
		INT DY = Verts[i].Y - Y;
		INT DZ = Verts[i].Z - Z;
		if( DX >= -16 && DX <= 15 && DY >= -16 && DY <= 15 && DZ >= -16 && DZ <= 15 )
		{
			Words.AddItem( 0x8000 | ((DX & 31) << 10) | ((DY & 31) << 5) | (DZ & 31) );
			X = Verts[i].X;
		}
		else
		{
			DCAppendAbsoluteVertex( Words, Verts[i] );
			X = Verts[i].X & ~1;
		}
		Y = Verts[i].Y;
		Z = Verts[i].Z;
	}
}

static void DCEncodeTemporalFrame( TArray<FMeshVert>& Reconstructed, const FMeshVert* Current,
	INT Count, TArray<_WORD>& Words )
{
	for( INT i = 0; i < Count; ++i )
	{
		INT DX = Current[i].X - Reconstructed(i).X;
		INT DY = Current[i].Y - Reconstructed(i).Y;
		INT DZ = Current[i].Z - Reconstructed(i).Z;
		if( DX >= -16 && DX <= 15 && DY >= -16 && DY <= 15 && DZ >= -16 && DZ <= 15 )
		{
			Words.AddItem( 0x8000 | ((DX & 31) << 10) | ((DY & 31) << 5) | (DZ & 31) );
			Reconstructed(i) = Current[i];
		}
		else
		{
			DCAppendAbsoluteVertex( Words, Current[i] );
			Reconstructed(i) = Current[i];
			Reconstructed(i).X &= ~1;
		}
	}
}

void UMesh::CookDCMesh()
{
	guard(UMesh::CookDCMesh);

	if( DCFrameOffsets.Num() || DCRuns.Num() )
	{
		return;
	}
	if( FrameVerts <= 0 || AnimFrames <= 0 || Verts.Num() / FrameVerts != AnimFrames
		|| Verts.Num() % FrameVerts || FrameVerts > 65535 || Tris.Num() > 20000 )
	{
		appErrorf( "Mesh cannot be cooked: %s", GetPathName() );
	}
	INT Before = Verts.Num() * 4 + Tris.Num() * 20 + Connects.Num() * 8 + VertLinks.Num() * 4;

	TArray<INT> SpatialOffsets;
	TArray<_WORD> SpatialWords;
	TArray<INT> TemporalOffsets;
	TArray<_WORD> TemporalWords;
	TArray<FMeshVert> Reconstructed;
	Reconstructed.Add(FrameVerts);
	for( INT Frame = 0; Frame < AnimFrames; ++Frame )
	{
		const FMeshVert* Current = &Verts(Frame * FrameVerts);
		SpatialOffsets.AddItem( SpatialWords.Num() );
		DCEncodeSpatialFrame( Current, FrameVerts, SpatialWords );
		TemporalOffsets.AddItem( TemporalWords.Num() );
		if( (Frame & 7) == 0 )
		{
			for( INT i = 0; i < FrameVerts; ++i )
			{
				DCAppendAbsoluteVertex( TemporalWords, Current[i] );
				Reconstructed(i) = Current[i];
				Reconstructed(i).X &= ~1;
			}
		}
		else
			DCEncodeTemporalFrame( Reconstructed, Current, FrameVerts, TemporalWords );
	}
	INT RawBytes = Verts.Num() * sizeof(FMeshVert);
	INT SpatialBytes = SpatialWords.Num() * sizeof(_WORD) + SpatialOffsets.Num() * sizeof(INT);
	INT TemporalBytes = TemporalWords.Num() * sizeof(_WORD) + TemporalOffsets.Num() * sizeof(INT);
	if( TemporalBytes < SpatialBytes && TemporalBytes < RawBytes )
	{
		DCFrameOffsets = TemporalOffsets;
		DCFrameWords = TemporalWords;
		DCTemporalFrames = 1;
	}
	else if( SpatialBytes < RawBytes )
	{
		DCFrameOffsets = SpatialOffsets;
		DCFrameWords = SpatialWords;
		DCTemporalFrames = 0;
	}
	if( DCFrameOffsets.Num() )
	{
		for( INT Frame = 0; Frame < AnimFrames; ++Frame )
		{
			FDCMeshFrameCursor Cursor( *this, Frame );
			for( INT i = 0; i < FrameVerts; ++i )
			{
				FVector Decoded = Cursor.Next();
				FVector Original = Verts(Frame * FrameVerts + i).Vector();
				if( Abs( Decoded.X - Original.X ) > 1
					|| Decoded.Y != Original.Y || Decoded.Z != Original.Z )
				{
					appErrorf( "Mesh frame roundtrip failed" );
				}
			}
			if( !Cursor.AtEnd() )
			{
				appErrorf( "Mesh frame codec left trailing words" );
			}
		}
	}
	// The position frames stay in the DAT at runtime. Cook one compact normal
	// per animation vertex now, while the original oriented triangles exist.
	// Runtime keeps only a bounded frame cache rather than all animation normals.
	if( FrameVerts >= 224 && Tris.Num() )
	{
		TArray<FVector> Points;
		TArray<FVector> NormalSums;
		Points.Add( FrameVerts );
		NormalSums.Add( FrameVerts );
		for( INT Frame = 0; Frame < AnimFrames; ++Frame )
		{
			FDCMeshFrameCursor Cursor( *this, Frame );
			for( INT Vertex = 0; Vertex < FrameVerts; ++Vertex )
				Points(Vertex) = Cursor.Next() * Scale;
			appMemset( NormalSums.GetData(), 0, FrameVerts * sizeof(FVector) );
			for( INT Triangle = 0; Triangle < Tris.Num(); ++Triangle )
			{
				const FMeshTri& Tri = Tris(Triangle);
				const FVector Face = (Points(Tri.iVertex[0]) - Points(Tri.iVertex[1]))
					^ (Points(Tri.iVertex[2]) - Points(Tri.iVertex[0]));
				const FVector Unit = Face * (1.f / appSqrt(Face.SizeSquared() + 0.001f));
				for( INT Corner = 0; Corner < 3; ++Corner )
					NormalSums(Tri.iVertex[Corner]) += Unit;
			}
			for( INT Vertex = 0; Vertex < FrameVerts; ++Vertex )
				DCNormalWords.AddItem( DCPackMeshNormal(NormalSums(Vertex)) );
		}
		for( INT First = 0; First < AnimFrames; First += 16 )
		{
			const INT Frames = Min( 16, AnimFrames - First );
			const INT FrameBytes = (FrameVerts * 10 + 7) / 8;
			const INT RawBytes = Frames * FrameBytes;
			TArray<BYTE> Quantized;
			Quantized.SetNum( RawBytes );
			appMemset( &Quantized(0), 0, RawBytes );
			for( INT j = 0; j < Frames; ++j )
			{
				BYTE* Dest = &Quantized(j * FrameBytes);
				for( INT Vertex = 0; Vertex < FrameVerts; ++Vertex )
				{
					const _WORD Normal = DCNormalWords((First+j) * FrameVerts + Vertex);
					const INT X = Min(31, ((Normal & 255) * 31 + 127) / 254);
					const INT Y = Min(31, (((Normal >> 8) & 255) * 31 + 127) / 254);
					const DWORD Code = X | (Y << 5);
					const INT Bit = Vertex * 10;
					const INT Byte = Bit >> 3;
					const DWORD Shifted = Code << (Bit & 7);
					Dest[Byte] |= Shifted & 255;
					if( Byte + 1 < FrameBytes ) Dest[Byte+1] |= (Shifted >> 8) & 255;
					if( Byte + 2 < FrameBytes ) Dest[Byte+2] |= (Shifted >> 16) & 255;
				}
			}
			TArray<BYTE> Delta;
			Delta.SetNum( RawBytes );
			appMemcpy( &Delta(0), &Quantized(0), RawBytes );
			for( INT j = Frames-1; j > 0; --j )
				for( INT k = 0; k < FrameBytes; ++k )
					Delta(j * FrameBytes + k) -= Delta((j-1) * FrameBytes + k);
			uLongf PackedBytes = compressBound( RawBytes );
			TArray<BYTE> Packed;
			Packed.SetNum( PackedBytes );
			if( compress2( &Packed(0), &PackedBytes, &Delta(0), RawBytes, Z_BEST_COMPRESSION ) != Z_OK )
				appErrorf( "Mesh normal compression failed" );
			TArray<BYTE> Verify;
			Verify.SetNum( RawBytes );
			uLongf VerifyBytes = RawBytes;
			if( uncompress( &Verify(0), &VerifyBytes, &Packed(0), PackedBytes ) != Z_OK
				|| VerifyBytes != (uLongf)RawBytes )
				appErrorf( "Mesh normal compression roundtrip failed" );
			for( INT j = 1; j < Frames; ++j )
				for( INT k = 0; k < FrameBytes; ++k )
					Verify(j * FrameBytes + k) += Verify((j-1) * FrameBytes + k);
			if( appMemcmp( &Verify(0), &Quantized(0), RawBytes ) )
				appErrorf( "Mesh normal delta roundtrip failed" );
			DCNormalBlockOffsets.AddItem( DCNormalCompressed.Num() );
			const INT Start = DCNormalCompressed.Add( PackedBytes );
			appMemcpy( &DCNormalCompressed(Start), &Packed(0), PackedBytes );
		}
		DCNormalBlockOffsets.AddItem( DCNormalCompressed.Num() );
		DCQuantizedNormals = 1;
		DCNormalWords.Empty();
	}
	if( DCFrameOffsets.Num() )
	{
		Verts.Empty();
	}

	TArray<BYTE> Used( Tris.Num() );
	appMemset( Used.GetData(), 0, Used.Num() );
	TArray<INT> TrialMarks( Tris.Num() );
	appMemset( TrialMarks.GetData(), 0, TrialMarks.Num() * sizeof(INT) );
	INT TrialId = 0;
	for( INT Start = 0; Start < Tris.Num(); ++Start )
	{
		if( Used(Start) )
		{
			continue;
		}
		const FMeshTri& First = Tris(Start);
		INT Material = 0;
		while( Material < DCMaterials.Num() && (DCMaterials(Material).Flags != First.PolyFlags
			|| DCMaterials(Material).Texture != First.TextureIndex) )
		{
			++Material;
		}
		if( Material == DCMaterials.Num() )
		{
			FDCMeshMaterial Entry = { First.PolyFlags, First.TextureIndex };
			DCMaterials.AddItem( Entry );
		}
		// The first corner of a strip is arbitrary, but it decides which edge
		// can be extended. Try all three cyclic (winding-preserving) starts and
		// keep the longest strip. Every triangle, UV seam and material is still
		// checked by the full topology roundtrip below.
		TArray<_WORD> BestIndices;
		TArray<_WORD> BestUVs;
		TArray<INT> BestTriangles;
		for( INT Rotation = 0; Rotation < 3; ++Rotation )
		{
			TArray<_WORD> CandidateIndices;
			TArray<_WORD> CandidateUVs;
			TArray<INT> CandidateTriangles;
			for( INT Corner = 0; Corner < 3; ++Corner )
			{
				const INT SourceCorner = (Corner + Rotation) % 3;
				CandidateIndices.AddItem( First.iVertex[SourceCorner] );
				CandidateUVs.AddItem( PackedUV(First.Tex[SourceCorner]) );
			}
			CandidateTriangles.AddItem( Start );
			++TrialId;
			TrialMarks(Start) = TrialId;
		while( CandidateIndices.Num() < 128 )
			{
				INT A = CandidateIndices.Num() - 2;
				INT B = CandidateIndices.Num() - 1;
				if( CandidateIndices.Num() & 1 )
					Exchange( A, B );
				INT Match = INDEX_NONE;
				INT MatchCorner = 0;
				for( INT i = 0; i < Tris.Num() && Match == INDEX_NONE; ++i )
				{
					const FMeshTri& Tri = Tris(i);
					if( Used(i) || TrialMarks(i) == TrialId
						|| Tri.PolyFlags != First.PolyFlags
						|| Tri.TextureIndex != First.TextureIndex )
						continue;
					for( INT Corner = 0; Corner < 3; ++Corner )
					{
						const INT Next = (Corner + 1) % 3;
						if( Tri.iVertex[Corner] == CandidateIndices(A)
							&& PackedUV(Tri.Tex[Corner]) == CandidateUVs(A)
							&& Tri.iVertex[Next] == CandidateIndices(B)
							&& PackedUV(Tri.Tex[Next]) == CandidateUVs(B) )
						{
							Match = i;
							MatchCorner = (Corner + 2) % 3;
							break;
						}
					}
				}
				if( Match == INDEX_NONE )
					break;
				CandidateIndices.AddItem( Tris(Match).iVertex[MatchCorner] );
				CandidateUVs.AddItem( PackedUV(Tris(Match).Tex[MatchCorner]) );
				CandidateTriangles.AddItem( Match );
				TrialMarks(Match) = TrialId;
			}
			if( CandidateTriangles.Num() > BestTriangles.Num() )
			{
				BestIndices = CandidateIndices;
				BestUVs = CandidateUVs;
				BestTriangles = CandidateTriangles;
			}
		}
		FDCMeshRun Run = { (_WORD)Material, 0, (_WORD)DCIndices.Num(), (_WORD)BestIndices.Num() };
		for( INT i = 0; i < BestIndices.Num(); ++i )
		{
			DCIndices.AddItem( BestIndices(i) );
			DCUVs.AddItem( BestUVs(i) );
		}
		for( INT i = 0; i < BestTriangles.Num(); ++i )
			Used(BestTriangles(i)) = 1;
		DCRuns.AddItem( Run );
	}

	// Store a meshlet ID in the run's formerly reserved word. Meshlets keep
	// input order (important for translucent materials) and contain at most
	// 128 distinct animated vertices, matching one 8 KiB/64-byte TA workset.
	INT MeshletCount = 0;
	if( DCRuns.Num() )
	{
		TArray<BYTE> Seen( FrameVerts );
		TArray<INT> TrialSeen( FrameVerts );
		appMemset( Seen.GetData(), 0, Seen.Num() );
		appMemset( TrialSeen.GetData(), 0, TrialSeen.Num() * sizeof(INT) );
		INT Stamp = 0;
		INT Unique = 0;
		for( INT RunIndex = 0; RunIndex < DCRuns.Num(); ++RunIndex )
		{
			FDCMeshRun& Run = DCRuns(RunIndex);
			INT Added = 0;
			++Stamp;
			for( INT Slot = 0; Slot < Run.Count; ++Slot )
			{
				const INT Vertex = DCIndices(Run.First + Slot);
				if( !Seen(Vertex) && TrialSeen(Vertex) != Stamp )
				{
					TrialSeen(Vertex) = Stamp;
					++Added;
				}
			}
			if( RunIndex && Unique + Added > 128 )
			{
				++MeshletCount;
				Unique = 0;
				appMemset( Seen.GetData(), 0, Seen.Num() );
			}
			Run.Reserved = MeshletCount;
			for( INT Slot = 0; Slot < Run.Count; ++Slot )
			{
				const INT Vertex = DCIndices(Run.First + Slot);
				if( !Seen(Vertex) )
				{
					Seen(Vertex) = 1;
					++Unique;
				}
			}
			if( Unique > 128 )
				appErrorf( "Meshlet vertex budget exceeded" );
		}
		++MeshletCount;
		DCClusteredRuns = 1;
	}

	// Reconstruct and match oriented triangles, UV seams and materials before
	// removing the legacy topology. Cyclic corner rotation preserves winding.
	FMemMark Mark( GMem );
	INT Count = 0;
	FMeshTri* Decoded = GetDCTriangles( Count );
	if( Count != Tris.Num() )
	{
		appErrorf( "Mesh triangle count mismatch" );
	}
	appMemset( Used.GetData(), 0, Used.Num() );
	for( INT i = 0; i < Count; ++i )
	{
		UBOOL Found = false;
		for( INT j = 0; j < Tris.Num() && !Found; ++j )
		{
			if( Used(j) || Decoded[i].PolyFlags != Tris(j).PolyFlags || Decoded[i].TextureIndex != Tris(j).TextureIndex )
			{
				continue;
			}
			for( INT Rotation = 0; Rotation < 3 && !Found; ++Rotation )
			{
				Found = true;
				for( INT k = 0; k < 3; ++k )
				{
					INT Source = (k + Rotation) % 3;
					Found = Found && Decoded[i].iVertex[k] == Tris(j).iVertex[Source]
						&& PackedUV(Decoded[i].Tex[k]) == PackedUV(Tris(j).Tex[Source]);
				}
			}
			if( Found )
			{
				Used(j) = 1;
			}
		}
		if( !Found )
		{
			appErrorf( "Mesh strip roundtrip failed" );
		}
	}
	Mark.Pop();
	ValidateDCMesh();
	if( DCRuns.Num() )
	{
		Tris.Empty();
		Connects.Empty();
		VertLinks.Empty();
	}
	INT After = Verts.Num() * 4 + DCFrameWords.Num() * 2 + DCFrameOffsets.Num() * 4
		+ DCRuns.Num() * 8 + DCMaterials.Num() * 8 + DCIndices.Num() * 4
		+ DCNormalBlockOffsets.Num() * 4 + DCNormalCompressed.Num();
	printf( "DCMESH %s frames=%i vertices=%i strips=%i indices=%i meshlets=%i normals=%i raw=%i cooked=%i\n",
		GetPathName(), AnimFrames, FrameVerts, DCRuns.Num(), DCIndices.Num(), MeshletCount,
		DCNormalCompressed.Num(), Before, After );
	unguard;
}
#endif
#endif
