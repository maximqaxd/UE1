#include "EnginePrivate.h"

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)

// Negative count identifies this port's versioned extension to v61.
// Layout: raw size, block offsets (including end), payload byte array.
// Each 4096-byte block starts with mode 0 (raw), 1 (run/literal packets),
// or 2 (LZ tokens).  Mode 2 uses LZ4-style tokens with a 16-bit distance.
static const INT DCLightTag = -0x44434c31;
static const INT DCLightBlock = 4096;

// Rendering is single-threaded. Keep the last LZ block decoded so adjacent
// shadow masks do not rebuild the same 4 KiB history, and keep that buffer off
// the Dreamcast main thread's 32 KiB stack.
static UModel* GDCLightCacheModel = NULL;
static INT GDCLightCacheBlock = INDEX_NONE;
static BYTE GDCLightCache[DCLightBlock];
static UModel* GDCLightPackedModel = NULL;
static INT GDCLightPackedBlock = INDEX_NONE;
static INT GDCLightPackedCount = 0;
static BYTE GDCLightPacked[DCLightBlock + 1];

void UModel::SerializeLightBits( FArchive& Ar )
{
	guard(UModel::SerializeLightBits);

	if( Ar.IsStateArchive() )
	{
		// State archives are not linker files. Preserve the DAT-backed payload
		// without casting the archive to FArchiveFileLoad or retaining a copy.
		Ar << LightRawSize << LightBlockOffsets;
		INT Count = LightStreamData.Size() ? LightStreamData.Size() : LightBits.Num();
		Ar << Count;
		if( Count < 0 || Count > 16*1024*1024 ) appErrorf("Invalid state lighting size");
		if( Ar.IsLoading() && (!LightStreamData.Size() || Count != LightStreamData.Size()) )
		{
			LightStreamData = FDCStreamSlice();
			LightBits.SetNum(Count);
		}
		BYTE Buffer[1024], Original[1024];
		for( INT Offset=0; Offset<Count; )
		{
			INT Bytes = Min(Count-Offset, (INT)sizeof(Buffer));
			if( Ar.IsSaving() )
			{
				if( LightStreamData.Size() ) LightStreamData.ReadRange(Offset, Buffer, Bytes);
				else appMemcpy(Buffer, &LightBits(Offset), Bytes);
			}
			Ar.Serialize(Buffer, Bytes);
			if( Ar.IsLoading() )
			{
				if( LightStreamData.Size() )
				{
					LightStreamData.ReadRange(Offset, Original, Bytes);
					if( appMemcmp(Buffer, Original, Bytes) )
					{
						LightBits.SetNum(Count);
						LightStreamData.Read(&LightBits(0));
						LightStreamData = FDCStreamSlice();
					}
				}
				if( !LightStreamData.Size() ) appMemcpy(&LightBits(Offset), Buffer, Bytes);
			}
			Offset += Bytes;
		}
		if( Ar.IsLoading() )
		{
			GDCLightCacheModel = NULL;
			GDCLightPackedModel = NULL;
		}
		return;
	}

	if( !Ar.IsLoading() && !Ar.IsSaving() )
	{
		Ar << LightBits << LightBlockOffsets;
		return;
	}

	INT Count = LightBlockOffsets.Num() ? DCLightTag : LightBits.Num();
	Ar << AR_INDEX(Count);
	if( Ar.IsLoading() )
	{
		LightStreamData = FDCStreamSlice();
	}

	if( Count == DCLightTag )
	{
		Ar << LightRawSize << LightBlockOffsets;

		if( Ar.IsLoading() && appDCStreamActive() )
		{
			INT PackedCount = 0;
			Ar << AR_INDEX(PackedCount);
			FArchiveFileLoad& File = (FArchiveFileLoad&)Ar;
			if( PackedCount <= 0 || File.Tell() < 0 || File.Tell() > File.Eof
				|| PackedCount > File.Eof - File.Tell() )
			{
				appErrorf( "Invalid deferred DAT lighting length: %d", PackedCount );
			}

			appDCStreamCapture( File.Filename, File.Tell(), PackedCount, LightStreamData );
#if defined(DC_RESOURCE_COOKER)
			// Retain reference bytes on the host so the captured DAT slice can be
			// CRC-checked after the forward-only load stream is closed.
			LightBits.SetNum( PackedCount );
			File.Serialize( &LightBits(0), PackedCount );
#else
			// Consume the strict dependency record without retaining its body.
			LightBits.Empty();
			BYTE Scratch[2048];
			for( INT Remaining = PackedCount; Remaining; )
			{
				INT Bytes = Min( Remaining, (INT)sizeof(Scratch) );
				File.Serialize( Scratch, Bytes );
				Remaining -= Bytes;
			}
#endif
		}
		else
		{
			Ar << LightBits;
		}

		if( LightRawSize <= 0 || LightRawSize > 64 * 1024 * 1024
			|| LightBlockOffsets.Num() != (LightRawSize + DCLightBlock - 1) / DCLightBlock + 1
			|| LightBlockOffsets(0) != 0
			|| LightBlockOffsets(LightBlockOffsets.Num() - 1)
				!= (LightStreamData.Size() ? LightStreamData.Size() : LightBits.Num()) )
		{
			appErrorf( "Invalid DC lighting directory: %s", GetPathName() );
		}

		for( INT i = 1; i < LightBlockOffsets.Num(); ++i )
		{
			INT PackedSize = LightStreamData.Size() ? LightStreamData.Size() : LightBits.Num();
			if( LightBlockOffsets(i) <= LightBlockOffsets(i - 1) || LightBlockOffsets(i) > PackedSize )
			{
				appErrorf( "Invalid DC lighting block: %s", GetPathName() );
			}
		}
	}
	else
	{
		if( Count < 0 || Count > 64 * 1024 * 1024 )
		{
			appErrorf( "Invalid LightBits size %i", Count );
		}

		if( Ar.IsLoading() )
		{
			LightBlockOffsets.Empty();
			LightRawSize = Count;
			LightBits.SetNum( Count );
		}

		if( Count )
		{
			Ar.Serialize( &LightBits(0), Count );
		}
	}

	if( Ar.IsLoading() )
	{
		if( GDCLightCacheModel == this )
		{
			GDCLightCacheModel = NULL;
			GDCLightCacheBlock = INDEX_NONE;
		}
		if( GDCLightPackedModel == this )
		{
			GDCLightPackedModel = NULL;
			GDCLightPackedBlock = INDEX_NONE;
			GDCLightPackedCount = 0;
		}
		LightDecodeCalls = 0;
		LightDecodeBytes = 0;
	}

	unguard;
}

#if defined(DC_RESOURCE_COOKER)
static void DCMarkLightList( UModel* Model, INT Root, TArray<BYTE>& Used )
{
	if( Root == INDEX_NONE )
		return;
	if( Root < 0 || Root >= Model->Lights.Num() )
		appErrorf( "Invalid light-list root %i/%i in %s", Root, Model->Lights.Num(), Model->GetPathName() );

	for( INT Index = Root; Index < Model->Lights.Num(); ++Index )
	{
		Used(Index) = 1;
		if( !Model->Lights(Index) )
			return;
	}
	appErrorf( "Unterminated light list at %i in %s", Root, Model->GetPathName() );
}

static void DCRemapLightList( INT& Root, const TArray<INT>& OldToNew )
{
	if( Root != INDEX_NONE )
	{
		if( Root < 0 || Root >= OldToNew.Num() || OldToNew(Root) == INDEX_NONE )
			appErrorf( "Failed to remap light-list root %i", Root );
		Root = OldToNew(Root);
	}
}

void UModel::CompactLightLists()
{
	guard(UModel::CompactLightLists);

	if( !Lights.Num() )
		return;

	TArray<BYTE> Used;
	Used.AddZeroed( Lights.Num() );
	for( INT i = 0; i < LightMap.Num(); ++i )
		DCMarkLightList( this, LightMap(i).iLightActors, Used );
	for( INT i = 0; i < Leaves.Num(); ++i )
	{
		DCMarkLightList( this, Leaves(i).iPermeating, Used );
		DCMarkLightList( this, Leaves(i).iVolumetric, Used );
	}

	TArray<INT> OldToNew;
	OldToNew.Add( Lights.Num() );
	TArray<AActor*> Compact;
	for( INT i = 0; i < Lights.Num(); ++i )
	{
		OldToNew(i) = INDEX_NONE;
		if( Used(i) )
			OldToNew(i) = Compact.AddItem( Lights(i) );
	}

	for( INT i = 0; i < LightMap.Num(); ++i )
		DCRemapLightList( LightMap(i).iLightActors, OldToNew );
	for( INT i = 0; i < Leaves.Num(); ++i )
	{
		DCRemapLightList( Leaves(i).iPermeating, OldToNew );
		DCRemapLightList( Leaves(i).iVolumetric, OldToNew );
	}

	Lights = Compact;
	Lights.Shrink();

	unguard;
}

static void DCLightWriteLength( TArray<BYTE>& Dest, INT Length )
{
	while( Length >= 255 )
	{
		Dest.AddItem( 255 );
		Length -= 255;
	}
	Dest.AddItem( Length );
}

static DWORD DCLightHash( const TArray<BYTE>& Data, INT Pos )
{
	DWORD Value = Data(Pos)
		| ((DWORD)Data(Pos + 1) << 8)
		| ((DWORD)Data(Pos + 2) << 16)
		| ((DWORD)Data(Pos + 3) << 24);
	return (Value * 2654435761u) >> 20;
}

void UModel::CompressLightBits()
{
	guard(UModel::CompressLightBits);

	if( LightBlockOffsets.Num() || !LightBits.Num() )
	{
		return;
	}

	TArray<BYTE> Packed;
	TArray<INT> Offsets;

	for( INT Base = 0; Base < LightBits.Num(); Base += DCLightBlock )
	{
		INT End = Min( Base + DCLightBlock, LightBits.Num() );
		TArray<BYTE> RleBlock;
		RleBlock.AddItem( 1 );

		for( INT Pos = Base; Pos < End; )
		{
			INT Run = 1;
			while( Pos + Run < End && Run < 128 && LightBits(Pos + Run) == LightBits(Pos) )
			{
				++Run;
			}

			if( Run >= 3 )
			{
				RleBlock.AddItem( 0x80 | (Run - 1) );
				RleBlock.AddItem( LightBits(Pos) );
				Pos += Run;
			}
			else
			{
				INT Start = Pos;
				do
				{
					++Pos;
				}
				while( Pos < End && Pos - Start < 128
					&& !(Pos + 2 < End && LightBits(Pos) == LightBits(Pos + 1) && LightBits(Pos) == LightBits(Pos + 2)) );

				RleBlock.AddItem( Pos - Start - 1 );
				for( INT j = Start; j < Pos; ++j )
				{
					RleBlock.AddItem( LightBits(j) );
				}
			}
		}

		TArray<BYTE> LzBlock;
		LzBlock.AddItem( 2 );
		TArray<INT> Hash;
		Hash.Add( 4096 );
		for( INT i = 0; i < Hash.Num(); ++i )
		{
			Hash(i) = INDEX_NONE;
		}

		INT Anchor = Base;
		INT Pos = Base;
		while( Pos + 4 <= End )
		{
			DWORD Slot = DCLightHash( LightBits, Pos );
			INT Match = Hash(Slot);
			Hash(Slot) = Pos;

			if( Match < Base || Pos - Match > 65535
				|| LightBits(Match) != LightBits(Pos)
				|| LightBits(Match + 1) != LightBits(Pos + 1)
				|| LightBits(Match + 2) != LightBits(Pos + 2)
				|| LightBits(Match + 3) != LightBits(Pos + 3) )
			{
				++Pos;
				continue;
			}

			INT MatchLength = 4;
			while( Pos + MatchLength < End
				&& LightBits(Match + MatchLength) == LightBits(Pos + MatchLength) )
			{
				++MatchLength;
			}

			INT LiteralLength = Pos - Anchor;
			INT TokenIndex = LzBlock.Add();
			LzBlock(TokenIndex) = (Min( LiteralLength, 15 ) << 4)
				| Min( MatchLength - 4, 15 );
			if( LiteralLength >= 15 )
			{
				DCLightWriteLength( LzBlock, LiteralLength - 15 );
			}
			for( INT j = Anchor; j < Pos; ++j )
			{
				LzBlock.AddItem( LightBits(j) );
			}

			INT Distance = Pos - Match;
			LzBlock.AddItem( Distance & 255 );
			LzBlock.AddItem( Distance >> 8 );
			if( MatchLength - 4 >= 15 )
			{
				DCLightWriteLength( LzBlock, MatchLength - 19 );
			}

			Pos += MatchLength;
			Anchor = Pos;
		}

		if( Anchor < End )
		{
			INT LiteralLength = End - Anchor;
			INT TokenIndex = LzBlock.Add();
			LzBlock(TokenIndex) = Min( LiteralLength, 15 ) << 4;
			if( LiteralLength >= 15 )
			{
				DCLightWriteLength( LzBlock, LiteralLength - 15 );
			}
			for( INT j = Anchor; j < End; ++j )
			{
				LzBlock.AddItem( LightBits(j) );
			}
		}

		Offsets.AddItem( Packed.Num() );
		TArray<BYTE>* Best = RleBlock.Num() <= LzBlock.Num() ? &RleBlock : &LzBlock;
		if( Best->Num() >= End - Base + 1 )
		{
			Packed.AddItem( 0 );
			for( INT j = Base; j < End; ++j )
			{
				Packed.AddItem( LightBits(j) );
			}
		}
		else
		{
			for( INT j = 0; j < Best->Num(); ++j )
			{
				Packed.AddItem( (*Best)(j) );
			}
		}
	}

	Offsets.AddItem( Packed.Num() );
	if( Packed.Num() + Offsets.Num() * 4 + 16 >= LightBits.Num() )
	{
		return;
	}

	// Verify using the runtime decoder before discarding any source bytes.
	TArray<BYTE> Original = LightBits;
	LightRawSize = Original.Num();
	LightBits = Packed;
	LightBlockOffsets = Offsets;

	BYTE Check[DCLightBlock];
	for( INT Base = 0; Base < Original.Num(); Base += DCLightBlock )
	{
		INT Count = Min( DCLightBlock, Original.Num() - Base );
		ReadLightBits( Base, Check, Count );

		if( appMemcmp( Check, &Original(Base), Count ) )
		{
			appErrorf( "DC lighting roundtrip failed" );
		}
	}

	LightBits.Shrink();
	LightBlockOffsets.Shrink();

	unguard;
}
#endif

void UModel::ReadLightBits( INT Offset, BYTE* Dest, INT Count )
{
	guard(UModel::ReadLightBits);

	INT Size = LightBlockOffsets.Num() ? LightRawSize : LightBits.Num();
	if( Offset < 0 || Count < 0 || Offset > Size || Count > Size - Offset )
	{
		appErrorf( "Shadow mask range %i+%i/%i in %s", Offset, Count, Size, GetPathName() );
	}

	if( !Count )
	{
		return;
	}

	if( !LightBlockOffsets.Num() )
	{
		appMemcpy( Dest, &LightBits(Offset), Count );
		return;
	}

	++LightDecodeCalls;
	LightDecodeBytes += Count;

	while( Count )
	{
		INT Block = Offset / DCLightBlock;
		INT Skip = Offset % DCLightBlock;
		INT Raw = Min( DCLightBlock, LightRawSize - Block * DCLightBlock );
		INT Take = Min( Count, Raw - Skip );
		INT PackedStart = LightBlockOffsets(Block);
		INT PackedCount = LightBlockOffsets(Block + 1) - PackedStart;
		const BYTE* Packed;
		if( LightStreamData.Size() )
		{
			if( PackedCount > (INT)sizeof(GDCLightPacked) )
				appErrorf( "Oversized streamed lighting block" );
			if( GDCLightPackedModel != this || GDCLightPackedBlock != Block
				|| GDCLightPackedCount != PackedCount )
			{
				LightStreamData.ReadRange( PackedStart, GDCLightPacked, PackedCount );
				GDCLightPackedModel = this;
				GDCLightPackedBlock = Block;
				GDCLightPackedCount = PackedCount;
			}
			Packed = GDCLightPacked;
		}
		else
		{
			Packed = &LightBits(PackedStart);
		}
		INT Pos = 1;
		INT End = PackedCount;
		BYTE Mode = Packed[0];

		if( Mode == 0 )
		{
			if( End - Pos != Raw )
			{
				appErrorf( "Invalid raw lighting block" );
			}

			appMemcpy( Dest, Packed + Pos + Skip, Take );
		}
		else if( Mode == 1 )
		{
			INT Out = 0;
			while( Pos < End && Out < Raw )
			{
				BYTE Control = Packed[Pos++];
				INT Run = (Control & 127) + 1;
				INT Bytes = (Control & 128) ? 1 : Run;

				if( Run > Raw - Out || Bytes > End - Pos )
				{
					appErrorf( "Invalid lighting packet" );
				}

				INT CopyStart = Max( Out, Skip );
				INT CopyEnd = Min( Out + Run, Skip + Take );
				if( CopyEnd > CopyStart )
				{
					if( Control & 128 )
					{
						appMemset( Dest + CopyStart - Skip, Packed[Pos], CopyEnd - CopyStart );
					}
					else
					{
						appMemcpy( Dest + CopyStart - Skip, Packed + Pos + CopyStart - Out, CopyEnd - CopyStart );
					}
				}

				Pos += Bytes;
				Out += Run;
			}

			if( Out != Raw || Pos != End )
			{
				appErrorf( "Invalid lighting block end" );
			}
		}
		else if( Mode == 2 )
		{
			if( GDCLightCacheModel != this || GDCLightCacheBlock != Block )
			{
				INT Out = 0;
				while( Pos < End )
				{
					BYTE Token = Packed[Pos++];
					INT LiteralLength = Token >> 4;
					if( LiteralLength == 15 )
					{
						BYTE Extension;
						do
						{
							if( Pos >= End )
								appErrorf( "Invalid LZ lighting literal length" );
							Extension = Packed[Pos++];
							LiteralLength += Extension;
						}
						while( Extension == 255 );
					}

					if( LiteralLength > Raw - Out || LiteralLength > End - Pos )
						appErrorf( "Invalid LZ lighting literals" );
					appMemcpy( GDCLightCache + Out, Packed + Pos, LiteralLength );
					Out += LiteralLength;
					Pos += LiteralLength;
					if( Pos == End )
						break;

					if( End - Pos < 2 )
						appErrorf( "Invalid LZ lighting distance" );
					INT Distance = Packed[Pos] | ((INT)Packed[Pos + 1] << 8);
					Pos += 2;
					if( Distance <= 0 || Distance > Out )
						appErrorf( "Invalid LZ lighting back-reference" );

					INT MatchLength = (Token & 15) + 4;
					if( (Token & 15) == 15 )
					{
						BYTE Extension;
						do
						{
							if( Pos >= End )
								appErrorf( "Invalid LZ lighting match length" );
							Extension = Packed[Pos++];
							MatchLength += Extension;
						}
						while( Extension == 255 );
					}
					if( MatchLength > Raw - Out )
						appErrorf( "Invalid LZ lighting match" );
					for( INT j = 0; j < MatchLength; ++j )
					{
						GDCLightCache[Out + j] = GDCLightCache[Out + j - Distance];
					}
					Out += MatchLength;
				}

				if( Out != Raw || Pos != End )
					appErrorf( "Invalid LZ lighting block end" );
				GDCLightCacheModel = this;
				GDCLightCacheBlock = Block;
			}
			appMemcpy( Dest, GDCLightCache + Skip, Take );
		}
		else
		{
			appErrorf( "Unknown lighting block mode" );
		}

		Offset += Take;
		Dest += Take;
		Count -= Take;
	}

	unguard;
}

BYTE* UModel::ShadowBits( INT Offset, INT Count )
{
	guard(UModel::ShadowBits);

	if( Offset == INDEX_NONE )
	{
		return NULL;
	}

	INT Size = LightBlockOffsets.Num() ? LightRawSize : LightBits.Num();
	if( Offset < 0 || Count < 0 || Offset > Size || Count > Size - Offset )
	{
		appErrorf( "Invalid shadow mask range" );
	}

	if( !Count )
	{
		return NULL;
	}

	if( !LightBlockOffsets.Num() )
	{
		return &LightBits(Offset);
	}

	BYTE* Result = New<BYTE>( GMem, Count );
	ReadLightBits( Offset, Result, Count );
	return Result;

	unguard;
}

#endif
