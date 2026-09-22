#include "EnginePrivate.h"

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)

static const INT DCFrameTag = -0x44434631;
static const INT DCFrameTemporalTag = -0x44434632;
static const INT DCTopologyTag = -0x44435431;

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
	if( !Ar.IsLoading() && !Ar.IsSaving() )
	{
		Ar << Tris << DCRuns << DCMaterials << DCIndices << DCUVs;
		return;
	}
	INT Count = DCRuns.Num() ? DCTopologyTag : Tris.Num();
	Ar << AR_INDEX(Count);
	if( Count == DCTopologyTag )
	{
		Ar << DCRuns << DCMaterials << DCIndices << DCUVs;
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
		if( Run.Reserved || Run.Count < 3 || Run.First != Next
			|| Run.Count > DCIndices.Num() - Next || Run.Material >= DCMaterials.Num() )
		{
			appErrorf( "Invalid cooked mesh strip" );
		}
		Next += Run.Count;
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

UBOOL FDCMeshTriangleCursor::Next( FMeshTri& Triangle )
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
		Triangle.Tex[Corner].U = Mesh.DCUVs(Index) & 255;
		Triangle.Tex[Corner].V = Mesh.DCUVs(Index) >> 8;
	}
	++RunVertex;
	return 1;
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
	if( DCFrameOffsets.Num() )
	{
		Verts.Empty();
	}

	TArray<BYTE> Used( Tris.Num() );
	appMemset( Used.GetData(), 0, Used.Num() );
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
		FDCMeshRun Run = { (_WORD)Material, 0, (_WORD)DCIndices.Num(), 3 };
		for( INT i = 0; i < 3; ++i )
		{
			DCIndices.AddItem( First.iVertex[i] );
			DCUVs.AddItem( PackedUV(First.Tex[i]) );
		}
		Used(Start) = 1;
		while( true )
		{
			INT A = DCIndices.Num() - 2;
			INT B = DCIndices.Num() - 1;
			if( Run.Count & 1 )
			{
				Exchange( A, B );
			}
			INT Match = INDEX_NONE;
			INT Corner = 0;
			for( INT i = 0; i < Tris.Num() && Match == INDEX_NONE; ++i )
			{
				const FMeshTri& Tri = Tris(i);
				if( Used(i) || Tri.PolyFlags != First.PolyFlags || Tri.TextureIndex != First.TextureIndex )
				{
					continue;
				}
				for( INT k = 0; k < 3; ++k )
				{
					INT Next = (k + 1) % 3;
					if( Tri.iVertex[k] == DCIndices(A) && PackedUV(Tri.Tex[k]) == DCUVs(A)
						&& Tri.iVertex[Next] == DCIndices(B) && PackedUV(Tri.Tex[Next]) == DCUVs(B) )
					{
						Match = i;
						Corner = (k + 2) % 3;
						break;
					}
				}
			}
			if( Match == INDEX_NONE )
			{
				break;
			}
			DCIndices.AddItem( Tris(Match).iVertex[Corner] );
			DCUVs.AddItem( PackedUV(Tris(Match).Tex[Corner]) );
			Used(Match) = 1;
			++Run.Count;
		}
		DCRuns.AddItem( Run );
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
		+ DCRuns.Num() * 8 + DCMaterials.Num() * 8 + DCIndices.Num() * 4;
	printf( "DCMESH %s frames=%i vertices=%i strips=%i indices=%i raw=%i cooked=%i\n",
		GetPathName(), AnimFrames, FrameVerts, DCRuns.Num(), DCIndices.Num(), Before, After );
	unguard;
}
#endif
#endif
