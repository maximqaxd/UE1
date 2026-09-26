#include "EnginePrivate.h"
#include <zlib.h>

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)

static const INT DCFrameTag = -0x44434631;
static const INT DCFrameTemporalTag = -0x44434632;
// Temporal v3: an empty non-keyframe range repeats the preceding pose.
// Keep all virtual frame numbers so sequence rates/notifies need no remapping.
static const INT DCFrameRepeatTag = -0x44434633;
static const INT DCFrameSparseTag = -0x44434634;
static const INT DCTopologyTag = -0x44435431;
static const INT DCTopologyClusterTag = -0x44435432;
static const INT DCTopologyNormalTag = -0x44435433;
static const INT DCTopologyNormalZTag = -0x44435434;
static const INT DCTopologyNormalQTag = -0x44435435;
static const INT DCTopologyBoundsTag = -0x44435436;
static const INT DCTopologyLodTag = -0x44435437;

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

// Also exercised by the cooker's sequential-versus-block roundtrip.
static void DCApplyIndependentBlock(const _WORD* Words, INT Count, FMeshVert* Verts, INT Num, UBOOL Key)
{
	INT p=0,X=0,Y=0,Z=0;
	for(INT v=0;v<Num;++v)
	{
		if(!Key) {X=Verts[v].X; Y=Verts[v].Y; Z=Verts[v].Z;}
		if(p>=Count) appErrorf("Truncated mesh block");
		DWORD W=Words[p++];
		if(W&0x8000)
		{
			if(Key && v==0) appErrorf("Dependent mesh block anchor");
			X+=SignedBits(W>>10,5); Y+=SignedBits(W>>5,5); Z+=SignedBits(W,5);
		}
		else
		{
			if(p>=Count) appErrorf("Truncated absolute mesh block");
			DWORD P=(W<<16)|Words[p++];
			X=SignedBits(P>>21,10)*2; Y=SignedBits(P>>10,11); Z=SignedBits(P,10);
		}
		Verts[v]=FMeshVert(FVector(X,Y,Z));
	}
	if(p!=Count) appErrorf("Trailing mesh block data");
}

#if defined(PLATFORM_DREAMCAST)
struct FDCDecodedMeshFrame
{
	const UMesh* Mesh;
	INT Frame;
	DWORD Stamp;
	TArray<FMeshVert> Verts;
	TArray<BYTE> Blocks;

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

static const FMeshVert* DCDecodeMeshFrame( const UMesh& Mesh, INT Frame, const BYTE* Active )
{
	if( Mesh.DCTemporalFrames==3 && Frame+1<Mesh.AnimFrames
		&& Mesh.DCFrameOffsets(Frame)==Mesh.DCFrameOffsets(Frame+1) )
		appErrorf("Sparse mesh frame must be resolved to retained anchors: %s",Mesh.GetPathName());
	INT Hit=INDEX_NONE;
	for( INT i = 0; i < 2; ++i )
	{
		if( GDCDecodedMeshFrames[i].Mesh == &Mesh && GDCDecodedMeshFrames[i].Frame == Frame )
		{
			Hit=i;
			break;
		}
	}

	INT Slot = Hit!=INDEX_NONE ? Hit : GDCDecodedMeshFrames[0].Stamp <= GDCDecodedMeshFrames[1].Stamp ? 0 : 1;
	FDCDecodedMeshFrame& Cache = GDCDecodedMeshFrames[Slot];
	const INT Blocks=(Mesh.FrameVerts+31)/32;
	if( Hit==INDEX_NONE )
	{
		Cache.Blocks.SetNum(Blocks); appMemset(Cache.Blocks.GetData(),0,Blocks);
		Cache.Verts.SetNum(Mesh.FrameVerts); appMemset(Cache.Verts.GetData(),0,Mesh.FrameVerts*sizeof(FMeshVert));
	}
	if( Active && Mesh.DCFrameBlockOffsets.Num()==Mesh.AnimFrames*(Blocks+1) )
	{
		for( INT b=0; b<Blocks; ++b )
		{
			if( Cache.Blocks(b) ) continue;
			const INT First=b*32, Last=Min(First+32,Mesh.FrameVerts);
			UBOOL Needed=!Active;
			for( INT v=First; !Needed && v<Last; ++v ) Needed=Active[v]!=0;
			if( !Needed ) continue;
			const INT Key=Mesh.DCTemporalFrames ? Frame&~7 : Frame;
			for( INT f=Key; f<=Frame; ++f )
			{
				const INT Begin=Mesh.DCFrameBlockOffsets(f*(Blocks+1)+b);
				const INT End=Mesh.DCFrameBlockOffsets(f*(Blocks+1)+b+1);
				if( Begin==End && f!=Key ) continue;
				if( End-Begin<Last-First || End-Begin>2*(Last-First) ) appErrorf("Invalid mesh block range");
				_WORD Words[64];
				const INT Offset=Mesh.DCFrameOffsets(f)+Begin;
				if( Mesh.DCFrameStreamData.Size() ) DCReadStreamedMeshBytes(Mesh,Offset*2,Words,(End-Begin)*2);
				else appMemcpy(Words,&Mesh.DCFrameWords(Offset),(End-Begin)*2);
				DCApplyIndependentBlock(Words,End-Begin,&Cache.Verts(First),Last-First,f==Key);
			}
			Cache.Blocks(b)=1;
		}
		Cache.Mesh=&Mesh; Cache.Frame=Frame; Cache.Stamp=++GDCMeshFrameStamp;
		return &Cache.Verts(0);
	}
	UBOOL Complete=Hit!=INDEX_NONE;
	for(INT b=0; Complete && b<Blocks; ++b) Complete=Cache.Blocks(b)!=0;
	if( Complete ) { Cache.Stamp=++GDCMeshFrameStamp; return &Cache.Verts(0); }
	Cache.Mesh = NULL;
	Cache.Frame = INDEX_NONE;
	Cache.Verts.SetNum( Mesh.FrameVerts );
	INT KeyFrame = Mesh.DCTemporalFrames ? Frame & ~7 : Frame;
	DCApplyMeshFrame( Mesh, KeyFrame, 1, Cache.Verts );
	for( INT SourceFrame = KeyFrame + 1; SourceFrame <= Frame; ++SourceFrame )
	{
		const INT End = SourceFrame + 1 < Mesh.AnimFrames
			? Mesh.DCFrameOffsets(SourceFrame + 1) : DCFrameWordCount(Mesh);
		if( Mesh.DCTemporalFrames >= 2 && End == Mesh.DCFrameOffsets(SourceFrame) )
			continue;
		DCApplyMeshFrame( Mesh, SourceFrame, 0, Cache.Verts );
	}
	Cache.Mesh = &Mesh;
	Cache.Frame = Frame;
	appMemset(Cache.Blocks.GetData(),1,Blocks);
	Cache.Stamp = ++GDCMeshFrameStamp;
	return &Cache.Verts(0);
}
#endif

void UMesh::ResolveDCFrameSample( INT& First, INT& Second, FLOAT& Alpha ) const
{
	if( DCTemporalFrames != 3 ) return;
	const INT OldFirst=First, OldSecond=Second;
	const FLOAT Time=First+Alpha;
	while( First>0 && First+1<AnimFrames && DCFrameOffsets(First)==DCFrameOffsets(First+1) ) --First;
	while( Second+1<AnimFrames && DCFrameOffsets(Second)==DCFrameOffsets(Second+1) ) ++Second;
	// Sequence loop edges are pinned; never interpolate across a sequence seam.
	if( OldSecond<OldFirst || First==Second ) return;
	Alpha=(Time-First)/(Second-First);
}

FDCMeshFrameCursor::FDCMeshFrameCursor( const UMesh& InMesh, INT Frame, const BYTE* Active )
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
#if !defined(PLATFORM_DREAMCAST)
		if( Mesh.DCTemporalFrames==3 && Frame+1<Mesh.AnimFrames
			&& Mesh.DCFrameOffsets(Frame)==Mesh.DCFrameOffsets(Frame+1) )
		{
			INT First=Frame, Second=Frame; FLOAT Alpha=0.f;
			Mesh.ResolveDCFrameSample(First,Second,Alpha);
			FDCMeshFrameCursor A(Mesh,First), B(Mesh,Second);
			SparseVerts.SetNum(Mesh.FrameVerts); SparsePosition=0;
			for( INT i=0; i<Mesh.FrameVerts; ++i )
			{
				const FVector V=A.Next(); SparseVerts(i)=V+(B.Next()-V)*Alpha;
			}
			return;
		}
#endif
#if defined(PLATFORM_DREAMCAST)
		Decoded = DCDecodeMeshFrame( Mesh, Frame, Active );
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
#if !defined(PLATFORM_DREAMCAST)
	if( SparseVerts.Num() ) return SparseVerts(SparsePosition++);
#endif
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
		if( Mesh.DCTemporalFrames >= 2 && Positions[Slot] == Ends[Slot] )
			continue;
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
#if !defined(PLATFORM_DREAMCAST)
	if( SparseVerts.Num() ) return SparsePosition==SparseVerts.Num();
#endif
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
	INT Count = DCFrameOffsets.Num() ? (DCTemporalFrames == 3 ? DCFrameSparseTag : DCTemporalFrames == 2 ? DCFrameRepeatTag
		: DCTemporalFrames ? DCFrameTemporalTag : DCFrameTag) : Verts.Num();
	Ar << AR_INDEX(Count);
	if( Count == DCFrameTag || Count == DCFrameTemporalTag || Count == DCFrameRepeatTag || Count == DCFrameSparseTag )
	{
		if( Ar.IsLoading() )
		{
			DCTemporalFrames = Count == DCFrameSparseTag ? 3 : Count == DCFrameRepeatTag ? 2 : Count == DCFrameTemporalTag;
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
			<< DCNormalWords << DCNormalBlockOffsets << DCNormalCompressed
			<< DCMeshletBounds << DCFrameBlockOffsets << DCLodRuns << DCLodIndices << DCLodUVs << DCLodBounds;
		return;
	}
	INT Count = DCRuns.Num() ? (DCNormalBlockOffsets.Num()
		? (DCQuantizedNormals ? DCTopologyNormalQTag : DCTopologyNormalZTag)
		: DCNormalWords.Num() ? DCTopologyNormalTag
		: (DCClusteredRuns ? DCTopologyClusterTag : DCTopologyTag)) : Tris.Num();
	if( DCMeshletBounds.Num() ) Count=DCTopologyBoundsTag;
	if( DCLodRuns.Num() ) Count=DCTopologyLodTag;
	Ar << AR_INDEX(Count);
	const UBOOL HasLod=Count==DCTopologyLodTag;
	const UBOOL HasBounds=Count==DCTopologyBoundsTag || HasLod;
	if( HasBounds ) Count=DCTopologyNormalQTag;
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
		if( HasBounds ) Ar << DCMeshletBounds << DCFrameBlockOffsets;
		else if( Ar.IsLoading() ) { DCMeshletBounds.Empty(); DCFrameBlockOffsets.Empty(); }
		if( HasLod ) Ar << DCLodRuns << DCLodIndices << DCLodUVs << DCLodBounds << DCLodVerts;
		else if( Ar.IsLoading() ) { DCLodRuns.Empty(); DCLodIndices.Empty(); DCLodUVs.Empty(); DCLodBounds.Empty(); DCLodVerts=0; }
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
	if( DCFrameBlockOffsets.Num() )
	{
		const INT Blocks=(FrameVerts+31)/32;
		if(DCFrameOffsets.Num()!=AnimFrames || DCFrameBlockOffsets.Num()!=AnimFrames*(Blocks+1))
			appErrorf("Invalid mesh independent-block directory");
		for(INT f=0;f<AnimFrames;++f)
		{
			const INT Length=(f+1<AnimFrames ? DCFrameOffsets(f+1) : DCFrameWordCount(*this))-DCFrameOffsets(f);
			if(DCFrameBlockOffsets(f*(Blocks+1))!=0 || DCFrameBlockOffsets(f*(Blocks+1)+Blocks)!=Length)
				appErrorf("Invalid mesh block extent");
			for(INT b=0;b<Blocks;++b)
			{
				INT N=DCFrameBlockOffsets(f*(Blocks+1)+b+1)-DCFrameBlockOffsets(f*(Blocks+1)+b);
				INT V=Min(32,FrameVerts-b*32);
				if(Length && (N<V || N>V*2)) appErrorf("Invalid mesh block length");
			}
		}
	}
	for(INT mode=0;mode<2;++mode)
	{
		const TArray<FDCMeshRun>& Runs=mode ? DCLodRuns : DCRuns;
		const TArray<_WORD>& Indices=mode ? DCLodIndices : DCIndices;
		const TArray<_WORD>& UVs=mode ? DCLodUVs : DCUVs;
		const TArray<FBox>& Bounds=mode ? DCLodBounds : DCMeshletBounds;
		INT End=0;
		if(mode && Runs.Num() && (DCLodVerts<=0 || DCLodVerts>FrameVerts || !Bounds.Num())) appErrorf("Invalid mesh LOD");
		for(INT r=0;r<Runs.Num();++r)
		{
			const FDCMeshRun& R=Runs(r);
			if(R.First!=End || R.Count<3 || R.Count>Indices.Num()-End || R.Material>=DCMaterials.Num()
				|| (Bounds.Num() && R.Reserved>=Bounds.Num())) appErrorf("Invalid bounded mesh run");
			End+=R.Count;
		}
		if(End!=Indices.Num() || Indices.Num()!=UVs.Num()) appErrorf("Invalid bounded mesh indices");
		for(INT i=0;i<Indices.Num();++i) if(Indices(i)>=(mode ? DCLodVerts : FrameVerts)) appErrorf("Invalid bounded mesh vertex");
	}
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
				|| (End - DCFrameOffsets(Frame) < FrameVerts
					&& !((DCTemporalFrames==2 || (DCTemporalFrames==3 && Frame+1<AnimFrames))
						&& (Frame & 7) != 0 && End == DCFrameOffsets(Frame)))
				|| End - DCFrameOffsets(Frame) > FrameVerts * 2 )
			{
				appErrorf( "Invalid cooked mesh frame directory" );
			}
		}
		if( DCTemporalFrames==3 )
			for( INT i=0; i<AnimSeqs.Num(); ++i )
			{
				const FMeshAnimSeq& Seq=AnimSeqs(i);
				if( Seq.NumFrames<=0 || Seq.StartFrame<0 || Seq.StartFrame+Seq.NumFrames>AnimFrames ) continue;
				const INT First=Seq.StartFrame, Last=First+Seq.NumFrames-1;
				if( (First+1<AnimFrames && DCFrameOffsets(First)==DCFrameOffsets(First+1))
					|| (Last+1<AnimFrames && DCFrameOffsets(Last)==DCFrameOffsets(Last+1)) )
					appErrorf("Sparse mesh sequence endpoint is missing: %s",GetPathName());
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
UBOOL UMesh::GetDCCookedNormals( FVector* Result, FCoords Coords, AActor* Owner, const BYTE* Active ) const
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
		if( Active && !Active[i] ) { Result[i]=FVector(0,0,0); continue; }
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
		if( (i&31) && DX >= -16 && DX <= 15 && DY >= -16 && DY <= 15 && DZ >= -16 && DZ <= 15 )
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

// A UE1 mesh index may be duplicated for a UV seam, but UVs live on triangle
// corners in the cooked format. Merge only identical animation trajectories
// whose lighting remains identical after the topology is remapped.
static void DCCalculateRawMeshNormals( const TArray<FMeshVert>& Verts,
	const TArray<FMeshTri>& Tris, INT FrameVerts, INT AnimFrames,
	const FVector& Scale, TArray<_WORD>& Result, TArray<FVector>& Directions )
{
	TArray<FVector> Points, Sums;
	Points.SetNum( FrameVerts );
	Sums.SetNum( FrameVerts );
	Result.SetNum( FrameVerts * AnimFrames );
	Directions.SetNum( FrameVerts * AnimFrames );
	for( INT Frame=0; Frame<AnimFrames; ++Frame )
	{
		for( INT i=0; i<FrameVerts; ++i )
			Points(i) = Verts(Frame * FrameVerts + i).Vector() * Scale;
		appMemset( Sums.GetData(), 0, FrameVerts * sizeof(FVector) );
		for( INT i=0; i<Tris.Num(); ++i )
		{
			const FMeshTri& Tri=Tris(i);
			const FVector Face=(Points(Tri.iVertex[0])-Points(Tri.iVertex[1]))
				^ (Points(Tri.iVertex[2])-Points(Tri.iVertex[0]));
			const FVector Unit=Face * (1.f / appSqrt(Face.SizeSquared()+0.001f));
			for( INT Corner=0; Corner<3; ++Corner )
				Sums(Tri.iVertex[Corner]) += Unit;
		}
		for( INT i=0; i<FrameVerts; ++i )
		{
			const FVector& Sum=Sums(i);
			const FLOAT Length2=Sum.SizeSquared();
			Directions(Frame * FrameVerts + i)=Length2>0.000001f
				? Sum * (1.f / appSqrt(Length2)) : FVector(0,0,0);
			Result(Frame * FrameVerts + i)=DCPackMeshNormal(Sum);
		}
	}
}

static INT DCDeduplicateMeshVertices( UMesh& Mesh )
{
	const INT OldCount=Mesh.FrameVerts;
	if( Mesh.Tris.Num()==0 ) return 0;
	// Particle rendering consumes all vertices, not just triangle indices.
	for( TObjectIterator<UClass> It; It; ++It )
		if( It->IsChildOf(AActor::StaticClass) )
		{
			const AActor* Default=It->GetDefaultActor();
			if( Default && Default->bParticles && Default->Mesh==&Mesh ) return 0;
		}
	for( TObjectIterator<AActor> It; It; ++It )
		if( It->bParticles && It->Mesh==&Mesh ) return 0;
	TArray<BYTE> Used;
	Used.SetNum(OldCount);
	appMemset(Used.GetData(),0,OldCount);
	for( INT i=0; i<Mesh.Tris.Num(); ++i )
		for( INT Corner=0; Corner<3; ++Corner )
		{
			const INT Vertex=Mesh.Tris(i).iVertex[Corner];
			if( Vertex<0 || Vertex>=OldCount )
				appErrorf("Mesh triangle index out of range: %s",Mesh.GetPathName());
			Used(Vertex)=1;
		}
	TArray<INT> Canonical;
	Canonical.SetNum(OldCount);
	INT PositionCandidates=0;
	INT Unused=0;
	for( INT i=0; i<OldCount; ++i )
	{
		Canonical(i)=i;
		if( !Used(i) ) { ++Unused; continue; }
		for( INT j=0; j<i; ++j )
		{
			if( !Used(j) || Mesh.Verts(i).D!=Mesh.Verts(j).D )
				continue;
			if( Canonical(j)!=j ) continue;
			INT Frame=1;
			for( ; Frame<Mesh.AnimFrames; ++Frame )
				if( Mesh.Verts(Frame*OldCount+i).D!=Mesh.Verts(Frame*OldCount+j).D )
					break;
			if( Frame==Mesh.AnimFrames )
			{
				Canonical(i)=j;
				++PositionCandidates;
				break;
			}
		}
	}
	if( !PositionCandidates && !Unused ) return 0;
	TArray<_WORD> OriginalNormals;
	TArray<FVector> OriginalDirections;
	DCCalculateRawMeshNormals(Mesh.Verts,Mesh.Tris,OldCount,Mesh.AnimFrames,
		Mesh.Scale,OriginalNormals,OriginalDirections);
	for( INT i=0; i<OldCount; ++i )
		if( Canonical(i)!=i )
			for( INT Frame=0; Frame<Mesh.AnimFrames; ++Frame )
				if( OriginalNormals(Frame*OldCount+i)
					!=OriginalNormals(Frame*OldCount+Canonical(i)) )
				{
					Canonical(i)=i;
					break;
				}
	TArray<INT> Remap;
	Remap.SetNum(OldCount);
	INT NewCount=0;
	for( INT i=0; i<OldCount; ++i )
		Remap(i)=Used(i) && Canonical(i)==i ? NewCount++ : INDEX_NONE;
	for( INT i=0; i<OldCount; ++i )
		if( Used(i) && Canonical(i)!=i ) Remap(i)=Remap(Canonical(i));
	if( NewCount==OldCount ) return 0;
	TArray<FMeshVert> NewVerts;
	NewVerts.SetNum(NewCount*Mesh.AnimFrames);
	for( INT Frame=0; Frame<Mesh.AnimFrames; ++Frame )
		for( INT i=0; i<OldCount; ++i )
			if( Used(i) && Canonical(i)==i )
				NewVerts(Frame*NewCount+Remap(i))=Mesh.Verts(Frame*OldCount+i);
	TArray<FMeshTri> NewTris=Mesh.Tris;
	for( INT i=0; i<NewTris.Num(); ++i )
		for( INT Corner=0; Corner<3; ++Corner )
			NewTris(i).iVertex[Corner]=Remap(NewTris(i).iVertex[Corner]);
	TArray<_WORD> NewNormals;
	TArray<FVector> NewDirections;
	DCCalculateRawMeshNormals(NewVerts,NewTris,NewCount,Mesh.AnimFrames,
		Mesh.Scale,NewNormals,NewDirections);
	for( INT Frame=0; Frame<Mesh.AnimFrames; ++Frame )
		for( INT i=0; i<OldCount; ++i )
		{
			if( !Used(i) ) continue;
			const FVector& A=OriginalDirections(Frame*OldCount+i);
			const FVector& B=NewDirections(Frame*NewCount+Remap(i));
			if( OriginalNormals(Frame*OldCount+i)!=NewNormals(Frame*NewCount+Remap(i))
				|| Abs(A.X-B.X)>0.00001f || Abs(A.Y-B.Y)>0.00001f
				|| Abs(A.Z-B.Z)>0.00001f )
			{
				printf("DCMESH dedup skipped %s: merged normals differ\n",Mesh.GetPathName());
				return 0;
			}
		}
	Mesh.Verts=NewVerts;
	Mesh.Tris=NewTris;
	Mesh.FrameVerts=NewCount;
	if( Mesh.CurVertex>=0 && Mesh.CurVertex<OldCount )
		Mesh.CurVertex=Remap(Mesh.CurVertex)==INDEX_NONE ? 0 : Remap(Mesh.CurVertex);
	printf("DCMESH dedup %s vertices=%d->%d candidates=%d unused=%d\n",
		Mesh.GetPathName(),OldCount,NewCount,PositionCandidates,Unused);
	return OldCount-NewCount;
}

// Position identities are independent of normals, UV corners and materials.
// Hash decoded retained anchors, then verify full trajectories on hash matches.
static void DCReportPositionIdentities( const UMesh& Mesh )
{
	TArray<DWORD> Hashes;
	TArray<FMeshVert> Anchors;
	Hashes.SetNum(Mesh.FrameVerts);
	for( INT v=0; v<Mesh.FrameVerts; ++v ) Hashes(v)=2166136261U;
	INT Frames=0;
	for( INT f=0; f<Mesh.AnimFrames; ++f )
	{
		if( Mesh.DCTemporalFrames==3 && f+1<Mesh.AnimFrames
			&& Mesh.DCFrameOffsets(f)==Mesh.DCFrameOffsets(f+1) ) continue;
		FDCMeshFrameCursor Cursor(Mesh,f);
		const INT Base=Anchors.Add(Mesh.FrameVerts);
		for( INT v=0; v<Mesh.FrameVerts; ++v )
		{
			Anchors(Base+v)=FMeshVert(Cursor.Next());
			Hashes(v)=(Hashes(v)^Anchors(Base+v).D)*16777619U;
		}
		if( !Cursor.AtEnd() ) appErrorf("Position identity scan left trailing frame words");
		++Frames;
	}
	TArray<INT> Canonical;
	Canonical.SetNum(Mesh.FrameVerts);
	INT Unique=0, Collisions=0;
	for( INT v=0; v<Mesh.FrameVerts; ++v )
	{
		Canonical(v)=v;
		for( INT j=0; j<v; ++j )
		{
			if( Canonical(j)!=j || Hashes(j)!=Hashes(v) ) continue;
			INT f=0;
			for( ; f<Frames; ++f )
				if( Anchors(f*Mesh.FrameVerts+j).D!=Anchors(f*Mesh.FrameVerts+v).D ) break;
			if( f==Frames ) { Canonical(v)=j; break; }
			++Collisions;
		}
		Unique+=Canonical(v)==v;
	}
	printf("DCMESH position_ids %s shading=%d positions=%d shared=%d anchors=%d collisions=%d\n",
		Mesh.GetPathName(),Mesh.FrameVerts,Unique,Mesh.FrameVerts-Unique,Frames,Collisions);
}

static void DCSelectReducedKeyframes( const UMesh& Mesh, TArray<BYTE>& Removed )
{
	Removed.SetNum(Mesh.AnimFrames);
	appMemset(Removed.GetData(),0,Mesh.AnimFrames);
	if( Mesh.AnimFrames<4 || Mesh.Tris.Num()==0 ) return;
	TArray<BYTE> Pinned;
	Pinned.SetNum(Mesh.AnimFrames);
	appMemset(Pinned.GetData(),0,Mesh.AnimFrames);
	Pinned(0)=Pinned(Mesh.AnimFrames-1)=1;
	FLOAT MinRate=10000.f, MaxRate=0.f;
	for( INT i=0; i<Mesh.AnimSeqs.Num(); ++i )
	{
		const FMeshAnimSeq& Seq=Mesh.AnimSeqs(i);
		MinRate=Min(MinRate,Seq.Rate);
		MaxRate=Max(MaxRate,Seq.Rate);
		if( Seq.StartFrame<0 || Seq.StartFrame+Seq.NumFrames>Mesh.AnimFrames ) continue;
		Pinned(Seq.StartFrame)=Pinned(Seq.StartFrame+Seq.NumFrames-1)=1;
		for( INT j=0; j<Seq.Notifys.Num(); ++j )
		{
			const INT Frame=Clamp(Seq.StartFrame+appRound(Seq.Notifys(j).Time*Seq.NumFrames),
				Seq.StartFrame,Seq.StartFrame+Seq.NumFrames-1);
			Pinned(Frame)=1;
		}
	}
	TArray<_WORD> NormalWords; TArray<FVector> Normals;
	DCCalculateRawMeshNormals(Mesh.Verts,Mesh.Tris,Mesh.FrameVerts,Mesh.AnimFrames,
		Mesh.Scale,NormalWords,Normals);
	TArray<FVector> Points,Sums;
	Points.SetNum(Mesh.FrameVerts); Sums.SetNum(Mesh.FrameVerts);
	INT Reduced=0;
	FLOAT Worst=0.f;
	for( INT Frame=1; Frame+1<Mesh.AnimFrames; Frame+=2 )
	{
		if( Pinned(Frame) ) continue;
		FLOAT Maximum=0.f;
		for( INT i=0; i<Mesh.FrameVerts; ++i )
		{
			const FVector A=Mesh.Verts((Frame-1)*Mesh.FrameVerts+i).Vector()*Mesh.Scale;
			const FVector B=Mesh.Verts(Frame*Mesh.FrameVerts+i).Vector()*Mesh.Scale;
			const FVector C=Mesh.Verts((Frame+1)*Mesh.FrameVerts+i).Vector()*Mesh.Scale;
			Maximum=Max(Maximum,(B-(A+C)*0.5f).Size());
		}
		if( Maximum>1.f ) continue;
		for( INT i=0; i<Mesh.FrameVerts; ++i )
			Points(i)=(Mesh.Verts((Frame-1)*Mesh.FrameVerts+i).Vector()
				+Mesh.Verts((Frame+1)*Mesh.FrameVerts+i).Vector())*0.5f*Mesh.Scale;
		appMemset(Sums.GetData(),0,Mesh.FrameVerts*sizeof(FVector));
		UBOOL Safe=1;
		for( INT i=0; i<Mesh.Tris.Num(); ++i )
		{
			const FMeshTri& T=Mesh.Tris(i);
			const FVector Face=(Points(T.iVertex[0])-Points(T.iVertex[1]))
				^ (Points(T.iVertex[2])-Points(T.iVertex[0]));
			const FVector OldA=Mesh.Verts(Frame*Mesh.FrameVerts+T.iVertex[0]).Vector()*Mesh.Scale;
			const FVector OldB=Mesh.Verts(Frame*Mesh.FrameVerts+T.iVertex[1]).Vector()*Mesh.Scale;
			const FVector OldC=Mesh.Verts(Frame*Mesh.FrameVerts+T.iVertex[2]).Vector()*Mesh.Scale;
			const FVector OldFace=(OldA-OldB)^(OldC-OldA);
			if( OldFace.SizeSquared()>0.000001f && (OldFace|Face)<=0.f ) { Safe=0; break; }
			const FVector Unit=Face*(1.f/appSqrt(Face.SizeSquared()+0.001f));
			for( INT c=0; c<3; ++c ) Sums(T.iVertex[c])+=Unit;
		}
		for( INT i=0; i<Mesh.FrameVerts && Safe; ++i )
		{
			const FLOAT Size2=Sums(i).SizeSquared();
			const FVector N=Size2>0.000001f ? Sums(i)*(1.f/appSqrt(Size2)) : FVector(0,0,1);
			Safe=(N|Normals(Frame*Mesh.FrameVerts+i))>=0.995f;
		}
		if( Safe ) { Removed(Frame)=1; ++Reduced; Worst=Max(Worst,Maximum); }
	}
	if( Reduced ) printf("DCMESH sparse %s removed=%d/%d max_error=%.3f normal_dot=0.995 rate=%.1f..%.1f\n",
		Mesh.GetPathName(),Reduced,Mesh.AnimFrames,Worst,MinRate,MaxRate);
}

static FLOAT DCMinFace(const FVector* A, const FVector* B, const FVector& Ref)
{
	const FVector E=A[1]-A[0], F=A[2]-A[0];
	const FVector DE=(B[1]-B[0])-E, DF=(B[2]-B[0])-F;
	const FLOAT C0=(E^F)|Ref, C1=((DE^F)+(E^DF))|Ref, C2=(DE^DF)|Ref;
	FLOAT Result=Min(C0,C0+C1+C2);
	if( C2>0.f ) { const FLOAT t=-C1/(2.f*C2); if(t>0.f && t<1.f) Result=Min(Result,C0+t*(C1+t*C2)); }
	return Result;
}

static void DCBuildAnimationLOD(UMesh& Mesh, TArray<FMeshTri>& Lod)
{
	Mesh.DCLodVerts=0;
	const INT N=Mesh.FrameVerts;
	if( N<224 || N>512 || Mesh.Tris.Num()<128 ) return;
	for( TObjectIterator<UClass> It; It; ++It )
		if( It->IsChildOf(AActor::StaticClass) && It->GetDefaultActor()->bParticles && It->GetDefaultActor()->Mesh==&Mesh ) return;
	for( TObjectIterator<AActor> It; It; ++It ) if(It->bParticles && It->Mesh==&Mesh) return;
	TArray<FVector> P(Mesh.Verts.Num());
	FBox Box(0);
	for( INT i=0; i<P.Num(); ++i )
	{
		FMeshVert V=Mesh.Verts(i); V.X &= ~1; P(i)=V.Vector()*Mesh.Scale; Box+=P(i);
	}
	const FVector Extent=Box.Max-Box.Min;
	const FLOAT Limit=Max(Extent.X,Max(Extent.Y,Extent.Z))*0.02f;
	if( Limit<=0.f ) return;
	TArray<BYTE> Protected(N), Rejected(N*N);
	TArray<INT> UV(N), Mat(N), Root(N), Edges(N*N);
	TArray<DWORD> MaterialFlags(N); appMemset(MaterialFlags.GetData(),255,N*sizeof(DWORD));
	TArray<FLOAT> Error(N);
	appMemset(Protected.GetData(),0,N); appMemset(Edges.GetData(),0,N*N*sizeof(INT));
	appMemset(Rejected.GetData(),0,N*N); appMemset(Error.GetData(),0,N*sizeof(FLOAT));
	for(INT i=0;i<N;++i) { UV(i)=Mat(i)=INDEX_NONE; Root(i)=i; }
	for(INT t=0;t<Mesh.Tris.Num();++t)
	{
		const FMeshTri& T=Mesh.Tris(t);
		for(INT c=0;c<3;++c)
		{
			const INT v=T.iVertex[c], u=PackedUV(T.Tex[c]), m=T.TextureIndex;
			if( (UV(v)!=INDEX_NONE && UV(v)!=u) || (Mat(v)!=INDEX_NONE && Mat(v)!=m)
				|| (MaterialFlags(v)!=0xffffffffu && MaterialFlags(v)!=T.PolyFlags)
				|| (T.PolyFlags & (PF_Invisible|PF_Translucent|PF_Modulated|PF_Environment|PF_TwoSided)) ) Protected(v)=1;
			UV(v)=u; Mat(v)=m; MaterialFlags(v)=T.PolyFlags;
			const INT b=T.iVertex[(c+1)%3]; ++Edges(Min(v,b)*N+Max(v,b));
		}
	}
	for(INT a=0;a<N;++a) for(INT b=a+1;b<N;++b)
		if( Edges(a*N+b) && Edges(a*N+b)!=2 ) Protected(a)=Protected(b)=1;
	Lod=Mesh.Tris;
	INT Collapses=0;
	while( Lod.Num()>Mesh.Tris.Num()*3/5 )
	{
		INT A=INDEX_NONE,B=INDEX_NONE; FLOAT Best=Limit;
		for(INT t=0;t<Lod.Num();++t) for(INT c=0;c<3;++c)
		{
			INT a=Lod(t).iVertex[c], b=Lod(t).iVertex[(c+1)%3];
			if(a<b) Exchange(a,b);
			if(a==b || Protected(a) || Protected(b) || Rejected(a*N+b)) continue;
			FLOAT Distance2=0.f;
			const FLOAT Remaining=Best-Error(a);
			if(Remaining<=0.f) continue;
			for(INT f=0;f<Mesh.AnimFrames;++f)
			{
				Distance2=Max(Distance2,(P(f*N+a)-P(f*N+b)).SizeSquared());
				if(Distance2>=Remaining*Remaining) break;
			}
			const FLOAT Cost=Max(Error(b),Error(a)+(FLOAT)appSqrt(Distance2));
			if(Cost<Best) { Best=Cost; A=a; B=b; }
		}
		if(A==INDEX_NONE) break;
		BYTE NA[512]={0}, NB[512]={0}, Opp[512]={0}; INT Faces=0;
		for(INT t=0;t<Lod.Num();++t)
		{
			UBOOL HA=0,HB=0;
			for(INT c=0;c<3;++c) { HA|=Lod(t).iVertex[c]==A; HB|=Lod(t).iVertex[c]==B; }
			for(INT c=0;c<3;++c)
			{
				const INT v=Lod(t).iVertex[c]; if(v==A || v==B) continue;
				if(HA) NA[v]=1; if(HB) NB[v]=1; if(HA && HB) Opp[v]=1;
			}
			Faces+=HA && HB;
		}
		UBOOL Safe=Faces==2;
		for(INT v=0;v<N;++v) if((NA[v] && NB[v])!=Opp[v]) Safe=0;
		for(INT t=0;Safe && t<Lod.Num();++t)
		{
			INT Old[3], New[3]; UBOOL Changed=0,HasB=0;
			for(INT c=0;c<3;++c) { Old[c]=Lod(t).iVertex[c]; New[c]=Old[c]==A ? B : Old[c]; Changed|=Old[c]==A; HasB|=Old[c]==B; }
			if(!Changed || HasB) continue;
			// Check the entire linear interval, including sequence wrap edges.
			for(INT s=-1;Safe && s<Mesh.AnimSeqs.Num();++s)
			{
				const INT Start=s<0 ? 0 : Mesh.AnimSeqs(s).StartFrame;
				const INT Count=s<0 ? Mesh.AnimFrames : Mesh.AnimSeqs(s).NumFrames;
				if(Count<=0 || Start<0 || Start+Count>Mesh.AnimFrames) { Safe=0; break; }
				for(INT f=0;Safe && f<Count;++f)
				{
					const INT f0=Start+f, f1=s<0 ? f0 : Start+(f+1)%Count;
					FVector O0[3],O1[3],Q0[3],Q1[3];
					for(INT c=0;c<3;++c) { O0[c]=P(f0*N+Old[c]); O1[c]=P(f1*N+Old[c]); Q0[c]=P(f0*N+New[c]); Q1[c]=P(f1*N+New[c]); }
					const FVector Ref=(O0[1]-O0[0])^(O0[2]-O0[0]);
					const FLOAT Epsilon=Ref.SizeSquared()*0.001f+0.0000001f;
					if(DCMinFace(O0,O1,Ref)<=Epsilon || DCMinFace(Q0,Q1,Ref)<=Epsilon) Safe=0;
				}
			}
		}
		if(!Safe) { Rejected(A*N+B)=1; continue; }
		for(INT v=0;v<N;++v) if(Root(v)==A) Root(v)=B;
		Error(B)=Best; ++Collapses;
		for(INT t=Lod.Num()-1;t>=0;--t)
		{
			for(INT c=0;c<3;++c) if(Lod(t).iVertex[c]==A) Lod(t).iVertex[c]=B;
			if(Lod(t).iVertex[0]==Lod(t).iVertex[1] || Lod(t).iVertex[1]==Lod(t).iVertex[2] || Lod(t).iVertex[0]==Lod(t).iVertex[2]) Lod.Remove(t);
		}
		appMemset(Rejected.GetData(),0,N*N);
	}
	if( Collapses<4 ) { Lod.Empty(); return; }
	// Pack active LOD vertices first: rejected tail blocks need no read/decode.
	TArray<BYTE> Active(N); appMemset(Active.GetData(),0,N);
	for(INT t=0;t<Lod.Num();++t) for(INT c=0;c<3;++c) Active(Lod(t).iVertex[c])=1;
	TArray<INT> Remap(N); INT Next=0;
	for(INT v=0;v<N;++v) if(Active(v)) Remap(v)=Next++;
	Mesh.DCLodVerts=Next;
	for(INT v=0;v<N;++v) if(!Active(v)) Remap(v)=Next++;
	TArray<FMeshVert> Reordered(Mesh.Verts.Num());
	for(INT f=0;f<Mesh.AnimFrames;++f) for(INT v=0;v<N;++v)
	{
		FMeshVert V=Mesh.Verts(f*N+v); V.X &= ~1; Reordered(f*N+Remap(v))=V;
		Mesh.BoundingBoxes(f)+=V.Vector(); Mesh.BoundingBox+=V.Vector();
	}
	Mesh.Verts=Reordered;
	for(INT t=0;t<Mesh.Tris.Num();++t) for(INT c=0;c<3;++c) Mesh.Tris(t).iVertex[c]=Remap(Mesh.Tris(t).iVertex[c]);
	for(INT t=0;t<Lod.Num();++t) for(INT c=0;c<3;++c) Lod(t).iVertex[c]=Remap(Lod(t).iVertex[c]);
	printf("DCMESH lod %s vertices=%d/%d triangles=%d/%d bound=%.3f collapses=%d\n",Mesh.GetPathName(),Mesh.DCLodVerts,N,Lod.Num(),Mesh.Tris.Num(),Limit,Collapses);
}

static void DCCookStrips(const char* Name, const TArray<FMeshTri>& Tris,
	TArray<FDCMeshRun>& DCRuns, TArray<FDCMeshMaterial>& DCMaterials,
	TArray<_WORD>& DCIndices, TArray<_WORD>& DCUVs)
{
	const TArray<FDCMeshMaterial> InitialMaterials=DCMaterials;
	TArray<BYTE> Used( Tris.Num() );
	TArray<FDCMeshRun> BaselineRuns;
	TArray<FDCMeshMaterial> BaselineMaterials;
	TArray<_WORD> BaselineIndices, BaselineUVs;
	for( INT Strategy=0; Strategy<2; ++Strategy )
	{
	const UBOOL Optimize = Strategy!=0;
	DCRuns.Empty(); DCMaterials=InitialMaterials; DCIndices.Empty(); DCUVs.Empty();
	appMemset( Used.GetData(), 0, Used.Num() );
	// Offline directed-edge adjacency. Each corner includes its UV so seams
	// cannot be welded accidentally. Hash collisions are checked explicitly.
	TArray<INT> EdgeHeads(4096), EdgeNext(Tris.Num()*3), Degree(Tris.Num());
	for( INT i=0; i<EdgeHeads.Num(); ++i ) EdgeHeads(i)=INDEX_NONE;
	appMemset(Degree.GetData(),0,Degree.Num()*sizeof(INT));
	for( INT t=0; t<Tris.Num(); ++t )
		for( INT c=0; c<3; ++c )
		{
			const INT n=(c+1)%3;
			const DWORD A=Tris(t).iVertex[c] | (DWORD(PackedUV(Tris(t).Tex[c]))<<16);
			const DWORD B=Tris(t).iVertex[n] | (DWORD(PackedUV(Tris(t).Tex[n]))<<16);
			const INT H=((A*1664525u) ^ (B*1013904223u)) & 4095;
			EdgeNext(t*3+c)=EdgeHeads(H); EdgeHeads(H)=t*3+c;
		}
	for( INT t=0; t<Tris.Num(); ++t )
		for( INT c=0; c<3; ++c )
		{
			const INT n=(c+1)%3;
			const DWORD A=Tris(t).iVertex[n] | (DWORD(PackedUV(Tris(t).Tex[n]))<<16);
			const DWORD B=Tris(t).iVertex[c] | (DWORD(PackedUV(Tris(t).Tex[c]))<<16);
			const INT H=((A*1664525u) ^ (B*1013904223u)) & 4095;
			for( INT e=EdgeHeads(H); e!=INDEX_NONE; e=EdgeNext(e) )
			{
				const FMeshTri& T=Tris(e/3); const INT k=e%3, j=(k+1)%3;
				if( e/3!=t && T.PolyFlags==Tris(t).PolyFlags && T.TextureIndex==Tris(t).TextureIndex
					&& (T.iVertex[k] | (DWORD(PackedUV(T.Tex[k]))<<16))==A
					&& (T.iVertex[j] | (DWORD(PackedUV(T.Tex[j]))<<16))==B ) ++Degree(t);
			}
		}
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
		const UBOOL OrderSensitive = (First.PolyFlags & (PF_Translucent|PF_Modulated|PF_Highlighted|PF_Invisible))!=0;
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
		TArray<INT> Seeds;
		Seeds.AddItem(Start);
		if( Optimize && !OrderSensitive )
			for( INT s=0; s<5; ++s )
			{
				INT Pick=INDEX_NONE;
				for( INT t=Start+1; t<Tris.Num(); ++t )
				{
					if( Used(t) || Tris(t).PolyFlags!=First.PolyFlags || Tris(t).TextureIndex!=First.TextureIndex ) continue;
					UBOOL Seen=0;
					for( INT k=0; k<Seeds.Num(); ++k ) if( Seeds(k)==t ) Seen=1;
					if( !Seen && (Pick==INDEX_NONE || Degree(t)<Degree(Pick)) ) Pick=t;
				}
				if( Pick==INDEX_NONE ) break;
				Seeds.AddItem(Pick);
			}
		for( INT Trial=0; Trial<Seeds.Num()*3; ++Trial )
		{
			const INT Seed=Seeds(Trial/3), Rotation=Trial%3;
			TArray<_WORD> CandidateIndices;
			TArray<_WORD> CandidateUVs;
			TArray<INT> CandidateTriangles;
			for( INT Corner = 0; Corner < 3; ++Corner )
			{
				const INT SourceCorner = (Corner + Rotation) % 3;
				CandidateIndices.AddItem( Tris(Seed).iVertex[SourceCorner] );
				CandidateUVs.AddItem( PackedUV(Tris(Seed).Tex[SourceCorner]) );
			}
			CandidateTriangles.AddItem( Seed );
			++TrialId;
			TrialMarks(Seed) = TrialId;
		while( CandidateIndices.Num() < 128 )
			{
				INT A = CandidateIndices.Num() - 2;
				INT B = CandidateIndices.Num() - 1;
				if( CandidateIndices.Num() & 1 )
					Exchange( A, B );
				INT Match = INDEX_NONE;
				INT MatchCorner = 0;
				const DWORD KA=CandidateIndices(A) | (DWORD(CandidateUVs(A))<<16);
				const DWORD KB=CandidateIndices(B) | (DWORD(CandidateUVs(B))<<16);
				const INT H=((KA*1664525u) ^ (KB*1013904223u)) & 4095;
				for( INT Edge=EdgeHeads(H); Edge!=INDEX_NONE; Edge=EdgeNext(Edge) )
				{
					const INT i=Edge/3;
					const FMeshTri& Tri = Tris(i);
					if( Used(i) || TrialMarks(i) == TrialId
						|| Tri.PolyFlags != First.PolyFlags
						|| Tri.TextureIndex != First.TextureIndex )
						continue;
					const INT Corner=Edge%3;
					{
						const INT Next = (Corner + 1) % 3;
						if( Tri.iVertex[Corner] == CandidateIndices(A)
							&& PackedUV(Tri.Tex[Corner]) == CandidateUVs(A)
							&& Tri.iVertex[Next] == CandidateIndices(B)
							&& PackedUV(Tri.Tex[Next]) == CandidateUVs(B) )
						{
							if( Match==INDEX_NONE || ((OrderSensitive || !Optimize) ? i<Match
								: Degree(i)<Degree(Match) || (Degree(i)==Degree(Match) && i<Match)) )
							{
								Match = i;
								MatchCorner = (Corner + 2) % 3;
							}
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
		// Another seed may have won: do not lose the original unused face.
		if( !Used(Start) ) --Start;
	}
	if( !Optimize )
	{
		BaselineRuns=DCRuns; BaselineMaterials=DCMaterials;
		BaselineIndices=DCIndices; BaselineUVs=DCUVs;
	}
	else
	{
		const INT CandidateCount=DCIndices.Num();
		if( CandidateCount>=BaselineIndices.Num() )
		{
			DCRuns=BaselineRuns; DCMaterials=BaselineMaterials;
			DCIndices=BaselineIndices; DCUVs=BaselineUVs;
		}
		printf("DCMESH strip_search %s baseline=%d candidate=%d selected=%d\n",
			Name,BaselineIndices.Num(),CandidateCount,DCIndices.Num());
	}
	}
}

static void DCVerifyStripTopology(const TArray<FMeshTri>& Tris, const TArray<FDCMeshRun>& Runs,
	const TArray<FDCMeshMaterial>& Materials, const TArray<_WORD>& Indices, const TArray<_WORD>& UVs)
{
	TArray<BYTE> Used(Tris.Num()); appMemset(Used.GetData(),0,Used.Num());
	INT Count=0;
	for(INT r=0;r<Runs.Num();++r) for(INT v=2;v<Runs(r).Count;++v)
	{
		const FDCMeshRun& R=Runs(r); const FDCMeshMaterial& M=Materials(R.Material);
		UBOOL Found=0;
		for(INT t=0;t<Tris.Num() && !Found;++t)
		{
			if(Used(t) || Tris(t).TextureIndex!=M.Texture || Tris(t).PolyFlags!=M.Flags) continue;
			for(INT rotation=0;rotation<3 && !Found;++rotation)
			{
				Found=1;
				for(INT c=0;c<3;++c)
				{
					INT slot=R.First+v-2+(c==2 ? 2 : (v&1) ? 1-c : c), src=(c+rotation)%3;
					Found=Found && Indices(slot)==Tris(t).iVertex[src] && UVs(slot)==PackedUV(Tris(t).Tex[src]);
				}
			}
			if(Found) Used(t)=1;
		}
		if(!Found) appErrorf("LOD strip topology mismatch");
		++Count;
	}
	if(Count!=Tris.Num()) appErrorf("LOD strip triangle count mismatch");
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
	const UBOOL CookNormals = FrameVerts >= 224 && Tris.Num() != 0;
	DCDeduplicateMeshVertices(*this);
	TArray<FMeshTri> LodTriangles;
	if( CookNormals ) DCBuildAnimationLOD(*this,LodTriangles);
	TArray<BYTE> Removed;
	if( LodTriangles.Num() )
	{
		// LOD validation covers the original piecewise-linear animation. Do not
		// subsequently change its interpolation intervals with sparse sampling.
		Removed.SetNum(AnimFrames); appMemset(Removed.GetData(),0,AnimFrames);
	}
	else DCSelectReducedKeyframes(*this,Removed);

	TArray<INT> SpatialOffsets;
	TArray<_WORD> SpatialWords;
	TArray<INT> TemporalOffsets;
	TArray<_WORD> TemporalWords;
	TArray<FMeshVert> Reconstructed;
	Reconstructed.Add(FrameVerts);
	INT RepeatedFrames = 0;
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
		{
			if( Removed(Frame) ) ++RepeatedFrames;
			else DCEncodeTemporalFrame( Reconstructed, Current, FrameVerts, TemporalWords );
		}
	}
	INT RawBytes = Verts.Num() * sizeof(FMeshVert);
	INT SpatialBytes = SpatialWords.Num() * sizeof(_WORD) + SpatialOffsets.Num() * sizeof(INT);
	INT TemporalBytes = TemporalWords.Num() * sizeof(_WORD) + TemporalOffsets.Num() * sizeof(INT);
	if( TemporalBytes < SpatialBytes && TemporalBytes < RawBytes )
	{
		DCFrameOffsets = TemporalOffsets;
		DCFrameWords = TemporalWords;
		DCTemporalFrames = RepeatedFrames ? 3 : 1;
		if( RepeatedFrames )
			printf("DCMESH sparse_encoded %s removed=%d/%d minimum_saved=%d bytes\n",
				GetPathName(),RepeatedFrames,AnimFrames,RepeatedFrames*FrameVerts*2);
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
				if( DCTemporalFrames==3 && Removed(Frame)
					? ((Decoded-Original)*Scale).Size()>1.f+Abs(Scale.X)+0.0001f
					: (Abs( Decoded.X - Original.X ) > 1
					|| Decoded.Y != Original.Y || Decoded.Z != Original.Z) )
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
	if( DCTemporalFrames==3 )
	{
		// A virtual frame's original bound may not contain its replacement.
		// Use the enclosing anchor bounds, also conservative between samples.
		for( INT Frame=1; Frame+1<AnimFrames; ++Frame )
			if( Removed(Frame) )
				BoundingBoxes(Frame)=BoundingBoxes(Frame)+BoundingBoxes(Frame-1)+BoundingBoxes(Frame+1);
		INT Tested=0;
		for( INT s=0; s<AnimSeqs.Num(); ++s )
		{
			const FMeshAnimSeq& Seq=AnimSeqs(s);
			if( Seq.NumFrames<=0 || Seq.StartFrame<0 || Seq.StartFrame+Seq.NumFrames>AnimFrames ) continue;
			for( INT f=0; f<Seq.NumFrames; ++f )
			{
				const INT OriginalFirst=Seq.StartFrame+f;
				const INT OriginalSecond=Seq.StartFrame+(f+1)%Seq.NumFrames;
				if( !Removed(OriginalFirst) && !Removed(OriginalSecond) ) continue;
				for( INT q=0; q<4; ++q )
				{
					INT First=OriginalFirst,Second=OriginalSecond; FLOAT Alpha=q*0.25f;
					ResolveDCFrameSample(First,Second,Alpha);
					if( Removed(First) || Removed(Second) ) appErrorf("Sparse sample resolved to missing anchor");
					FDCMeshFrameCursor A(*this,First), B(*this,Second);
					for( INT v=0; v<FrameVerts; ++v )
					{
						const FVector P=A.Next(), Sample=P+(B.Next()-P)*Alpha;
						const FVector OldA=Verts(OriginalFirst*FrameVerts+v).Vector();
						const FVector OldB=Verts(OriginalSecond*FrameVerts+v).Vector();
						if( ((Sample-(OldA+(OldB-OldA)*(q*0.25f)))*Scale).Size()>1.f+Abs(Scale.X)+0.0001f )
							appErrorf("Sparse animation sample exceeded error bound: %s",GetPathName());
					}
					++Tested;
				}
			}
		}
		printf("DCMESH sparse_verified %s samples=%d timeline_unchanged=1\n",GetPathName(),Tested);
	}
	DCReportPositionIdentities(*this);
	// The position frames stay in the DAT at runtime. Cook one compact normal
	// per animation vertex now, while the original oriented triangles exist.
	// Runtime keeps only a bounded frame cache rather than all animation normals.
	if( CookNormals )
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

	DCCookStrips(GetPathName(),Tris,DCRuns,DCMaterials,DCIndices,DCUVs);
	if( LodTriangles.Num() )
	{
		DCCookStrips(GetPathName(),LodTriangles,DCLodRuns,DCMaterials,DCLodIndices,DCLodUVs);
		DCVerifyStripTopology(LodTriangles,DCLodRuns,DCMaterials,DCLodIndices,DCLodUVs);
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

	INT LodMeshlets=0;
	if( DCLodRuns.Num() )
	{
		TArray<BYTE> Seen(FrameVerts); appMemset(Seen.GetData(),0,FrameVerts);
		INT Unique=0;
		for(INT r=0;r<DCLodRuns.Num();++r)
		{
			INT Added=0; TArray<BYTE> Trial=Seen;
			for(INT v=0;v<DCLodRuns(r).Count;++v) { INT i=DCLodIndices(DCLodRuns(r).First+v); if(!Trial(i)) {Trial(i)=1; ++Added;} }
			if(Unique+Added>128) { ++LodMeshlets; Unique=0; appMemset(Seen.GetData(),0,FrameVerts); }
			DCLodRuns(r).Reserved=LodMeshlets;
			for(INT v=0;v<DCLodRuns(r).Count;++v) { INT i=DCLodIndices(DCLodRuns(r).First+v); if(!Seen(i)) {Seen(i)=1; ++Unique;} }
		}
		++LodMeshlets;
	}
	if( CookNormals && DCQuantizedNormals && DCFrameOffsets.Num() && FrameVerts<16384 )
	{
		const INT Blocks=(FrameVerts+31)/32;
		DCMeshletBounds.SetNum(MeshletCount);
		for( INT b=0; b<MeshletCount; ++b ) DCMeshletBounds(b)=FBox(0);
		DCLodBounds.SetNum(LodMeshlets);
		for(INT b=0;b<LodMeshlets;++b) DCLodBounds(b)=FBox(0);
		TArray<FVector> Pose(FrameVerts);
		for( INT f=0; f<AnimFrames; ++f )
		{
			FDCMeshFrameCursor Cursor(*this,f);
			for( INT v=0; v<FrameVerts; ++v ) Pose(v)=Cursor.Next();
			for( INT r=0; r<DCRuns.Num(); ++r )
				for( INT v=0; v<DCRuns(r).Count; ++v )
					DCMeshletBounds(DCRuns(r).Reserved)+=Pose(DCIndices(DCRuns(r).First+v));
			for(INT r=0;r<DCLodRuns.Num();++r) for(INT v=0;v<DCLodRuns(r).Count;++v)
				DCLodBounds(DCLodRuns(r).Reserved)+=Pose(DCLodIndices(DCLodRuns(r).First+v));
			const INT Start=DCFrameOffsets(f), End=f+1<AnimFrames ? DCFrameOffsets(f+1) : DCFrameWords.Num();
			INT p=Start;
			for( INT v=0; v<FrameVerts; ++v )
			{
				if( !(v&31) ) DCFrameBlockOffsets.AddItem((_WORD)(p-Start));
				if( Start==End ) continue;
				if( p>=End ) appErrorf("Cooked block directory truncated");
				const _WORD W=DCFrameWords(p++);
				if( !(v&31) && (!DCTemporalFrames || !(f&7)) && (W&0x8000) )
					appErrorf("Mesh block does not start independently");
				if( !(W&0x8000) ) ++p;
			}
			DCFrameBlockOffsets.AddItem((_WORD)(p-Start));
			if( p!=End ) appErrorf("Mesh block directory trailing words");
		}
		if( DCFrameBlockOffsets.Num()!=AnimFrames*(Blocks+1) ) appErrorf("Mesh block directory count");
		for( INT b=0; b<MeshletCount; ++b ) DCMeshletBounds(b)=DCMeshletBounds(b).ExpandBy(0.01f);
		for(INT b=0;b<LodMeshlets;++b) DCLodBounds(b)=DCLodBounds(b).ExpandBy(0.01f);
		for(INT f=0;f<AnimFrames;++f)
		{
			if(DCTemporalFrames==3 && f+1<AnimFrames && DCFrameOffsets(f)==DCFrameOffsets(f+1)) continue;
			FDCMeshFrameCursor Full(*this,f);
			for(INT b=0;b<Blocks;++b)
			{
				FMeshVert Decoded[32]; const INT N=Min(32,FrameVerts-b*32), Key=DCTemporalFrames ? f&~7 : f;
				for(INT k=Key;k<=f;++k)
				{
					const INT Begin=DCFrameBlockOffsets(k*(Blocks+1)+b), End=DCFrameBlockOffsets(k*(Blocks+1)+b+1);
					if(Begin!=End) DCApplyIndependentBlock(&DCFrameWords(DCFrameOffsets(k)+Begin),End-Begin,Decoded,N,k==Key);
				}
				for(INT v=0;v<N;++v) if(Decoded[v].Vector()!=Full.Next()) appErrorf("Independent mesh block mismatch: %s",GetPathName());
			}
		}
	}
	else { DCLodRuns.Empty(); DCLodIndices.Empty(); DCLodUVs.Empty(); DCLodBounds.Empty(); DCLodVerts=0; }

	// Reconstruct and match oriented triangles, UV seams and materials before
	// Both base and LOD strip searches retain their material/corner identities.
	// removing the legacy topology. Cyclic corner rotation preserves winding.
	FMemMark Mark( GMem );
	INT Count = 0;
	FMeshTri* Decoded = GetDCTriangles( Count );
	if( Count != Tris.Num() )
	{
		appErrorf( "Mesh triangle count mismatch" );
	}
	TArray<BYTE> Used(Tris.Num());
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
		+ DCNormalBlockOffsets.Num() * 4 + DCNormalCompressed.Num()
		+ DCFrameBlockOffsets.Num()*2 + (DCMeshletBounds.Num()+DCLodBounds.Num())*sizeof(FBox)
		+ DCLodRuns.Num()*sizeof(FDCMeshRun) + DCLodIndices.Num()*4;
	printf( "DCMESH %s frames=%i vertices=%i strips=%i indices=%i meshlets=%i normals=%i raw=%i cooked=%i\n",
		GetPathName(), AnimFrames, FrameVerts, DCRuns.Num(), DCIndices.Num(), MeshletCount,
		DCNormalCompressed.Num(), Before, After );
	unguard;
}
#endif
#endif
