#include "CorePrivate.h"
#include "UnLinker.h"
#include "UnDCStream.h"
#include "UnDCLoading.h"
#include <string.h>

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)

const char* appDCResolveCampaignMap( const char* Map )
{
	static const char* const Aliases[][2] = {
		{"Dig","Dig1"}, {"DasaCellars","DasaCellars1"}, {"Ruins","Ruins1"},
		{"Chizra","Chizra1"}, {"Terraniux","Terraniux1"},
		{"IsvKran32","IsvKran32A"}, {"SkyTown","SkyTown1"}
	};
	const char* Base=Map;
	for( const char* P=Map; *P; ++P ) if( *P=='/' || *P=='\\' ) Base=P+1;
	char Name[128];
	appStrncpy(Name,Base,ARRAY_COUNT(Name));
	if( char* Extension=appStrchr(Name,'.') )
	{
		if( appStricmp(Extension,".unr") ) return Map;
		*Extension=0;
	}
	for( INT i=0; i<ARRAY_COUNT(Aliases); ++i )
		if( !appStricmp(Name,Aliases[i][0]) )
		{
			char Path[160];
			appSprintf(Path,"../Maps/%s.dcs",Aliases[i][1]);
			return appFSize(Path)>0 ? Aliases[i][1] : Map;
		}
	return Map;
}

struct FDCStreamFile
{
	char Name[64];
	DWORD Size;
};

struct FDCStreamLinkerIndices
{
	DWORD NameCount;
	DWORD ImportCount;
	DWORD ExportCount;
	TArray<_WORD> Names;
	TArray<_WORD> Imports;
	TArray<_WORD> Exports;
};

static INT DCStreamLiveStores = 0;

struct FDCStreamStore
{
	INT References;
	DWORD Size;
	char Path[256];
	FILE* Resources;

	FDCStreamStore() : References(1), Size(0), Resources(NULL)
	{
		++DCStreamLiveStores;
	}
	~FDCStreamStore()
	{
		if( Resources )
		{
			appFclose( Resources );
		}
		--DCStreamLiveStores;
	}
};

static void DCStreamRelease( FDCStreamStore* Store )
{
	if( Store && --Store->References == 0 )
	{
		delete Store;
	}
}

FDCStreamSlice::FDCStreamSlice() : Store(NULL), Physical(0), Length(0) {}

FDCStreamSlice::FDCStreamSlice( const FDCStreamSlice& Other )
	: Store(Other.Store), Physical(Other.Physical), Length(Other.Length)
{
	if( Store )
	{
		++Store->References;
	}
}

FDCStreamSlice& FDCStreamSlice::operator=( const FDCStreamSlice& Other )
{
	if( this != &Other )
	{
		if( Other.Store )
		{
			++Other.Store->References;
		}
		DCStreamRelease( Store );
		Store = Other.Store;
		Physical = Other.Physical;
		Length = Other.Length;
	}
	return *this;
}

FDCStreamSlice::~FDCStreamSlice()
{
	DCStreamRelease( Store );
}

void FDCStreamSlice::Read( void* Destination ) const
{
	ReadRange( 0, Destination, Length );
}

void FDCStreamSlice::ReadRange( INT Offset, void* Destination, INT Count ) const
{
	if( !Store || Offset < 0 || Count <= 0 || Offset > Length
		|| Count > Length - Offset || !Destination || Physical > Store->Size
		|| (DWORD)Offset > Store->Size - Physical
		|| (DWORD)Count > Store->Size - Physical - Offset )
	{
		appErrorf( "Invalid DAT resource slice" );
	}
	if( !Store->Resources )
	{
		Store->Resources = appFopen( Store->Path, "rb" );
		if( !Store->Resources )
		{
			appErrorf( "Cannot reopen DAT resources: %s", Store->Path );
		}
	}
	// Independent from the forward-only load cursor; never seek the package or
	// the active load stream. The backing DAT remains owned until the last slice.
	if( appFseek( Store->Resources, Physical + Offset, USEEK_SET )
		|| appFread( Destination, 1, Count, Store->Resources ) != Count )
	{
		appErrorf( "DAT resource read failed: %s offset=%u size=%d",
			Store->Path, Physical + Offset, Count );
	}
}

static FILE* DCStream = NULL;
static DWORD DCStreamFlags = 0;
#if defined(DC_RESOURCE_COOKER)
UBOOL GDCStreamCookDeferredMips = false;
#endif
static FDCStreamStore* DCStreamBacking = NULL;
static TArray<FDCStreamFile> DCStreamFiles;
static TArray<FDCStreamLinkerIndices> DCStreamLinkerIndices;
static DWORD DCStreamRecords = 0;
static DWORD DCStreamRecordCount = 0;
static DWORD DCStreamBytes = 0;
static DWORD DCStreamSize = 0;
static DWORD DCStreamPosition = 0;
static DWORD DCStreamCurrent[3];
static DWORD DCStreamRemaining = 0;

struct FDCIndexedRead
{
	DWORD File, Start, Count, Physical;
};
static TArray<FDCIndexedRead> DCIndexedReads;
static UBOOL DCIndexed = 0;

static INT CDECL DCCompareReads(const void* A, const void* B)
{
	const FDCIndexedRead& X = *(const FDCIndexedRead*)A;
	const FDCIndexedRead& Y = *(const FDCIndexedRead*)B;
	if (X.File != Y.File) return X.File < Y.File ? -1 : 1;
	if (X.Start != Y.Start) return X.Start < Y.Start ? -1 : 1;
	return X.Count < Y.Count ? -1 : X.Count > Y.Count ? 1 : 0;
}

static const FDCIndexedRead& DCFindRead(INT File, DWORD Offset, DWORD Count)
{
	INT Low=0, High=DCIndexedReads.Num();
	while (Low<High)
	{
		INT Mid=Low+(High-Low)/2;
		const FDCIndexedRead& R=DCIndexedReads(Mid);
		if (R.File<(DWORD)File || (R.File==(DWORD)File && R.Start<=Offset)) Low=Mid+1;
		else High=Mid;
	}
	for (INT i=Low-1; i>=0 && DCIndexedReads(i).File==(DWORD)File; --i)
	{
		const FDCIndexedRead& R=DCIndexedReads(i);
		if (Offset>=R.Start && Offset-R.Start<R.Count && Count<=R.Count-(Offset-R.Start)) return R;
	}
	appErrorf("Saved world needs uncooked dependency: file=%d offset=%u count=%u", File, Offset, Count);
	return DCIndexedReads(0);
}

static const char* DCStreamBaseName( const char* Path )
{
	const char* Name = Path;
	for( const char* Cursor = Path; *Cursor; ++Cursor )
	{
		if( *Cursor == '/' || *Cursor == '\\' )
		{
			Name = Cursor + 1;
		}
	}
	return Name;
}

static INT DCStreamFind( const char* Filename )
{
	const char* Name = DCStreamBaseName( Filename );
	for( INT i = 0; i < DCStreamFiles.Num(); ++i )
	{
		if( !appStricmp( Name, DCStreamFiles(i).Name ) )
		{
			return i;
		}
	}
	return INDEX_NONE;
}

UBOOL appDCStreamActive()
{
	return DCStream != NULL;
}

UBOOL appDCStreamDeferredMips()
{
	if( DCStream )
	{
		return (DCStreamFlags & 1) != 0;
	}
#if defined(DC_RESOURCE_COOKER)
	return GDCStreamCookDeferredMips;
#else
	return false;
#endif
}

static INT DCStreamFindIndex( const TArray<_WORD>& Indices, INT OriginalIndex )
{
	INT Low = 0;
	INT High = Indices.Num();
	while( Low < High )
	{
		INT Middle = Low + (High - Low) / 2;
		INT Value = Indices(Middle);
		if( Value < OriginalIndex )
			Low = Middle + 1;
		else
			High = Middle;
	}
	return Low < Indices.Num() && Indices(Low) == OriginalIndex ? Low : INDEX_NONE;
}

INT appDCStreamCompactExportCount( const char* Filename, INT OriginalCount )
{
	INT File = DCStreamFind( Filename );
	if( File == INDEX_NONE || !DCStreamLinkerIndices.IsValidIndex(File) )
		return INDEX_NONE;
	const FDCStreamLinkerIndices& Indices = DCStreamLinkerIndices(File);
	if( Indices.ExportCount != (DWORD)OriginalCount )
		appErrorf( "DAT export manifest mismatch: %s package=%d DAT=%u", Filename, OriginalCount, Indices.ExportCount );
	return Indices.Exports.Num();
}

INT appDCStreamMapExport( const char* Filename, INT OriginalIndex )
{
	INT File = DCStreamFind( Filename );
	if( File == INDEX_NONE || !DCStreamLinkerIndices.IsValidIndex(File) )
		return OriginalIndex;
	return DCStreamFindIndex( DCStreamLinkerIndices(File).Exports, OriginalIndex );
}

INT appDCStreamOriginalExport( const char* Filename, INT CompactIndex )
{
	INT File = DCStreamFind( Filename );
	if( File == INDEX_NONE || !DCStreamLinkerIndices.IsValidIndex(File) )
		return CompactIndex;
	const TArray<_WORD>& Exports = DCStreamLinkerIndices(File).Exports;
	return Exports.IsValidIndex(CompactIndex) ? Exports(CompactIndex) : INDEX_NONE;
}

UBOOL appDCStreamUsesImport( const char* Filename, INT OriginalCount, INT OriginalIndex )
{
	INT File = DCStreamFind( Filename );
	if( File == INDEX_NONE || !DCStreamLinkerIndices.IsValidIndex(File) )
		return 1;
	const FDCStreamLinkerIndices& Indices = DCStreamLinkerIndices(File);
	if( Indices.ImportCount != (DWORD)OriginalCount )
		appErrorf( "DAT import manifest mismatch: %s package=%d DAT=%u", Filename, OriginalCount, Indices.ImportCount );
	return DCStreamFindIndex( Indices.Imports, OriginalIndex ) != INDEX_NONE;
}

INT appDCStreamFileSize( const char* Filename )
{
	INT Index = DCStreamFind( Filename );
	return Index == INDEX_NONE ? INDEX_NONE : (INT)DCStreamFiles(Index).Size;
}

UBOOL appDCStreamResolve( const char* Name, char* Out )
{
	const char* Base = DCStreamBaseName( Name );
	for( INT i = 0; i < DCStreamFiles.Num(); ++i )
	{
		if( !appStricmp( Base, DCStreamFiles(i).Name ) )
		{
			appStrcpy( Out, DCStreamFiles(i).Name );
			return true;
		}
	}

	INT Match = INDEX_NONE;
	for( INT i = 0; i < DCStreamFiles.Num(); ++i )
	{
		char Stem[64];
		appStrcpy( Stem, DCStreamFiles(i).Name );
		char* Extension = appStrchr( Stem, '.' );
		if( !Extension )
		{
			continue;
		}

		const char* PackageExtension = Extension + 1;
		if
		(
			appStricmp( PackageExtension, "u"   ) &&
			appStricmp( PackageExtension, "unr" ) &&
			appStricmp( PackageExtension, "utx" ) &&
			appStricmp( PackageExtension, "uax" ) &&
			appStricmp( PackageExtension, "umx" )
		)
		{
			continue;
		}

		*Extension = 0;
		if( !appStricmp( Base, Stem ) )
		{
			if( Match != INDEX_NONE )
			{
				appErrorf( "Ambiguous stream package: %s", Name );
			}
			Match = i;
		}
	}
	if( Match == INDEX_NONE )
	{
		return false;
	}
	appStrcpy( Out, DCStreamFiles(Match).Name );
	return true;
}

void appDCStreamClose()
{
#if defined(PLATFORM_DREAMCAST)
	appDCLoadingEnd();
#endif
	if( DCStream )
	{
		appFclose( DCStream );
		DCStream = NULL;
	}
	DCStreamFiles.Empty();
	DCIndexedReads.Empty();
	DCIndexed = 0;
	DCStreamLinkerIndices.Empty();
	DCStreamRemaining = 0;
	DCStreamFlags = 0;
	DCStreamRelease( DCStreamBacking );
	DCStreamBacking = NULL;
}

UBOOL appDCStreamCanResetSession()
{
	return !DCStream && DCStreamLiveStores == 0;
}

void appDCStreamShutdown()
{
	appDCStreamClose();
	if( !appDCStreamCanResetSession() )
	{
		appErrorf( "DAT session reset blocked: %d backing stores still owned", DCStreamLiveStores );
	}
	debugf( "DCSTREAM session_released stores=0 load_handles=0 resource_handles=0" );
}

void appDCStreamOpen( const char* Path )
{
	if( DCStream )
	{
		// Replacing a mount must not hide an incomplete startup recipe.
		appDCStreamFinish();
	}
	appDCStreamClose();
	DCStream = appFopen( Path, "rb" );
	if( !DCStream )
	{
		appErrorf( "Cannot open dependency stream: %s", Path );
	}
	appFseek( DCStream, 0, USEEK_END );
	INT Size = appFtell( DCStream );
	appFseek( DCStream, 0, USEEK_SET );
	DWORD Header[8];
	if( Size < 32 || appFread( Header, 1, sizeof(Header), DCStream ) != sizeof(Header)
		|| Header[0] != 0x32534344 || Header[1] != 3 || !Header[2] || Header[2] > 1024
		|| !Header[3] || Header[4] != 32 || Header[5] < 32 + Header[2] * 68
		|| Header[5] > (DWORD)Size || Header[6] != (DWORD)Size || (Header[7] & ~3u) != 0
		|| !(Header[7] & 2) )
	{
		appErrorf( "Invalid DCS3 dependency stream: %s", Path );
	}
	static_assert( sizeof(FDCStreamFile) == 68, "DCS3 file layout" );
	DCStreamFiles.SetNum( Header[2] );
	if( appFread( &DCStreamFiles(0), 68, Header[2], DCStream ) != Header[2] )
	{
		appErrorf( "Truncated dependency stream directory" );
	}
	for( INT i = 0; i < DCStreamFiles.Num(); ++i )
	{
		const FDCStreamFile& File = DCStreamFiles(i);
		if( !File.Name[0] || !memchr( File.Name, 0, 64 ) || File.Size > 0x7fffffffu
			|| strchr( File.Name, '/' ) || strchr( File.Name, '\\' ) )
		{
			appErrorf( "Invalid dependency stream package" );
		}
		for( INT j = 0; j < i; ++j )
		{
			if( !appStricmp( File.Name, DCStreamFiles(j).Name ) )
			{
				appErrorf( "Duplicate dependency stream package" );
			}
		}
	}
	DWORD IndexHeader[2];
	if( appFread(IndexHeader,1,sizeof(IndexHeader),DCStream) != sizeof(IndexHeader)
		|| IndexHeader[0] != 0x58494344 || IndexHeader[1] != Header[2] )
	{
		appErrorf( "Invalid DCS3 linker manifest" );
	}
	DCStreamLinkerIndices.Empty();
	for( DWORD i = 0; i < Header[2]; ++i )
	{
		// UE1 TArray::SetNum only reallocates raw storage. These entries contain
		// nested TArrays, so each element must be constructed before use.
		new(DCStreamLinkerIndices) FDCStreamLinkerIndices;
	}
	for( INT i = 0; i < DCStreamLinkerIndices.Num(); ++i )
	{
		DWORD Counts[6];
		if( appFread(Counts,1,sizeof(Counts),DCStream) != sizeof(Counts) )
			appErrorf( "Truncated DCS3 linker manifest" );
		FDCStreamLinkerIndices& Indices = DCStreamLinkerIndices(i);
		Indices.NameCount = Counts[0];
		Indices.ImportCount = Counts[1];
		Indices.ExportCount = Counts[2];
		if( Counts[3] > Counts[0] || Counts[4] > Counts[1] || Counts[5] > Counts[2]
			|| Counts[0] > 65535 || Counts[1] > 65535 || Counts[2] > 65535 )
		{
			appErrorf( "Invalid DCS3 linker index counts" );
		}
		TArray<_WORD>* Lists[3] = { &Indices.Names, &Indices.Imports, &Indices.Exports };
		DWORD Limits[3] = { Counts[0], Counts[1], Counts[2] };
		for( INT Kind = 0; Kind < 3; ++Kind )
		{
			TArray<_WORD>& List = *Lists[Kind];
			List.SetNum( Counts[Kind+3] );
			if( List.Num() && appFread(&List(0),sizeof(_WORD),List.Num(),DCStream) != List.Num() )
				appErrorf( "Truncated DCS3 linker indices" );
			for( INT j = 0; j < List.Num(); ++j )
				if( List(j) >= Limits[Kind] || (j && List(j-1) >= List(j)) )
					appErrorf( "Invalid DCS3 linker index list" );
		}
	}
	if( (DWORD)appFtell(DCStream) != Header[5] )
		appErrorf( "Invalid DCS3 linker manifest size" );
	DCStreamRecords = 0;
	DCStreamFlags = Header[7];
	DCStreamRecordCount = Header[3];
	DCStreamBytes = 0;
	DCStreamSize = Header[6];
	DCStreamPosition = Header[5];
	if( appStrlen( Path ) >= 256 )
	{
		appErrorf( "DAT resource path too long" );
	}
	DCStreamBacking = new FDCStreamStore;
	DCStreamBacking->Size = DCStreamSize;
	appStrcpy( DCStreamBacking->Path, Path );
	debugf( "DCSTREAM open files=%d records=%u directory=%u compact_exports=1 strict=1",
		DCStreamFiles.Num(), DCStreamRecordCount, Header[5] );
#if defined(PLATFORM_DREAMCAST)
	// The stream is a recorded replay of this exact load, so consumed records
	// over total is a genuinely linear measure rather than an estimate.
	appDCLoadingBegin();
#endif
}

void appDCStreamUseIndexedReads()
{
	if (!DCStream || DCStreamRecords || DCStreamRemaining || DCIndexed)
		appErrorf("Indexed stream mode must be selected before reading dependencies");
	if (DCStreamRecordCount > DCStreamSize/12) appErrorf("Invalid indexed stream record count");
	DCIndexedReads.SetNum(DCStreamRecordCount);
	DWORD Position=DCStreamPosition;
	for (INT i=0; i<DCIndexedReads.Num(); ++i)
	{
		FDCIndexedRead& R=DCIndexedReads(i);
		if (Position>DCStreamSize || DCStreamSize-Position<12 ||
			appFseek(DCStream, Position, USEEK_SET) || appFread(&R, 1, 12, DCStream)!=12)
			appErrorf("Truncated indexed dependency directory");
		R.Physical=Position+12;
		if (R.File>=(DWORD)DCStreamFiles.Num() || !R.Count || R.Count>DCStreamSize-R.Physical ||
			R.Start>DCStreamFiles(R.File).Size || R.Count>DCStreamFiles(R.File).Size-R.Start)
			appErrorf("Invalid indexed dependency extent");
		Position=R.Physical+R.Count;
		appDCLoadingProgress(0.1f * (FLOAT)(i+1) / (FLOAT)DCIndexedReads.Num());
	}
	if (Position!=DCStreamSize) appErrorf("Indexed dependency length mismatch");
	appQsort(&DCIndexedReads(0), DCIndexedReads.Num(), sizeof(FDCIndexedRead), DCCompareReads);
	DCIndexed=1;
	debugf("DCSTREAM indexed_restore records=%d bytes=%d", DCIndexedReads.Num(),
		DCIndexedReads.Num()*(INT)sizeof(FDCIndexedRead));
}

static void DCStreamPrepareRecord( const char* Filename, INT Offset )
{
	if( DCStreamRemaining )
	{
		return;
	}
	if( DCStreamRecords == DCStreamRecordCount || DCStreamSize - DCStreamPosition < 12
		|| appFread( DCStreamCurrent, 1, 12, DCStream ) != 12 )
	{
		appErrorf( "Dependency stream exhausted: %s offset=%d", Filename, Offset );
	}
	DCStreamPosition += 12;
	++DCStreamRecords;
#if defined(PLATFORM_DREAMCAST)
	if( DCStreamRecordCount )
		appDCLoadingProgress( (FLOAT)DCStreamRecords / (FLOAT)DCStreamRecordCount );
#endif
	DWORD Source = DCStreamCurrent[0];
	DWORD Start = DCStreamCurrent[1];
	DWORD Count = DCStreamCurrent[2];
	if( Source >= (DWORD)DCStreamFiles.Num() || !Count
		|| Start > DCStreamFiles(Source).Size || Count > DCStreamFiles(Source).Size - Start
		|| Count > DCStreamSize - DCStreamPosition )
	{
		appErrorf( "Invalid dependency stream record %u", DCStreamRecords );
	}
	DCStreamRemaining = Count;
}

void appDCStreamCapture( const char* Filename, INT Offset, INT Length, FDCStreamSlice& Slice )
{
	if( !DCStream || Length <= 0 )
	{
		appErrorf( "Cannot capture inactive or empty DAT resource" );
	}
	if (DCIndexed)
	{
		const FDCIndexedRead& R=DCFindRead(DCStreamFind(Filename), Offset, Length);
		FDCStreamSlice NewSlice;
		NewSlice.Store=DCStreamBacking;
		++DCStreamBacking->References;
		NewSlice.Physical=R.Physical+Offset-R.Start;
		NewSlice.Length=Length;
		Slice=NewSlice;
		return;
	}
	DCStreamPrepareRecord( Filename, Offset );
	INT File = DCStreamFind( Filename );
	if( File == INDEX_NONE || DCStreamCurrent[0] != (DWORD)File
		|| DCStreamCurrent[1] != (DWORD)Offset || (DWORD)Length > DCStreamRemaining )
	{
		const char* Actual = DCStreamCurrent[0] < (DWORD)DCStreamFiles.Num()
			? DCStreamFiles(DCStreamCurrent[0]).Name : "<invalid>";
		appErrorf(
			"Dependency stream divergence record=%u expected=%s@%u+%u actual=%s@%d+%d",
			DCStreamRecords,
			Actual,
			DCStreamCurrent[1],
			DCStreamRemaining,
			Filename,
			Offset,
			Length );
	}
	FDCStreamSlice NewSlice;
	NewSlice.Store = DCStreamBacking;
	++DCStreamBacking->References;
	NewSlice.Physical = DCStreamPosition;
	NewSlice.Length = Length;
	Slice = NewSlice;
}

UBOOL appDCReadDependencyFile( const char* Filename, INT Offset, void* Data, INT Length )
{
	if( !Filename || Offset < 0 || !Data || Length <= 0 )
	{
		return 0;
	}
	if( appDCStreamActive() )
	{
		appDCStreamRead( Filename, Offset, Data, Length );
		return 1;
	}

	FArchiveFileLoad File( Filename );
	if( Offset > File.Eof || Length > File.Eof - Offset )
	{
		return 0;
	}
	File.Seek( Offset );
	File.Serialize( Data, Length );
	return 1;
}

UBOOL appDCCaptureDependencyFile( const char* Filename, INT Offset, INT Length, FDCStreamSlice& Slice )
{
	if( !Filename || Offset < 0 || Length <= 0 )
	{
		return 0;
	}
	if( appDCStreamActive() )
	{
		appDCStreamCapture( Filename, Offset, Length, Slice );
		BYTE Scratch[2048];
		for( INT Position = 0; Position < Length; )
		{
			INT Count = Min( Length - Position, (INT)sizeof(Scratch) );
			appDCStreamRead( Filename, Offset + Position, Scratch, Count );
			Position += Count;
		}
		return 1;
	}

	FArchiveFileLoad File( Filename );
	if( Offset > File.Eof || Length > File.Eof - Offset )
	{
		return 0;
	}
	File.Seek( Offset );
	BYTE* Scratch = (BYTE*)appMalloc( Length, "DCDeferredDependency" );
	File.Serialize( Scratch, Length );
	appFree( Scratch );
	return 1;
}

void appDCStreamRead( const char* Filename, INT Offset, void* Data, INT Length )
{
	INT File = DCStreamFind( Filename );
	if( !DCStream || File == INDEX_NONE || Offset < 0 || Length < 0
		|| (DWORD)Offset > DCStreamFiles(File).Size
		|| (DWORD)Length > DCStreamFiles(File).Size - Offset )
	{
		appErrorf( "Read outside dependency stream: %s offset=%d length=%d", Filename, Offset, Length );
	}
	while( Length )
	{
		if (DCIndexed)
		{
			const FDCIndexedRead& R=DCFindRead(File, Offset, 1);
			INT Count=Min(Length, (INT)(R.Count-((DWORD)Offset-R.Start)));
			if (appFseek(DCStream, R.Physical+Offset-R.Start, USEEK_SET) ||
				appFread(Data, 1, Count, DCStream)!=(DWORD)Count)
				appErrorf("Indexed dependency read failed");
			Offset+=Count; Length-=Count; Data=(BYTE*)Data+Count;
			continue;
		}
		DCStreamPrepareRecord( Filename, Offset );
		if( DCStreamCurrent[0] != (DWORD)File || DCStreamCurrent[1] != (DWORD)Offset )
		{
			appErrorf( "Dependency stream divergence record=%u expected=%s@%u actual=%s@%d",
				DCStreamRecords, DCStreamFiles(DCStreamCurrent[0]).Name,
				DCStreamCurrent[1], Filename, Offset );
		}
		INT Count = Min( Length, (INT)DCStreamRemaining );
		if( appFread( Data, 1, Count, DCStream ) != Count )
		{
			appErrorf( "Truncated dependency stream payload" );
		}
		DCStreamCurrent[1] += Count;
		DCStreamRemaining -= Count;
		DCStreamPosition += Count;
		DCStreamBytes += Count;
		Offset += Count;
		Length -= Count;
		Data = (BYTE*)Data + Count;
	}
}

void appDCStreamFinish()
{
	if (DCIndexed) return;
	if( !DCStream || DCStreamRemaining || DCStreamRecords != DCStreamRecordCount
		|| DCStreamPosition != DCStreamSize )
	{
		appErrorf( "Dependency stream not fully consumed: records=%u/%u position=%u/%u",
			DCStreamRecords, DCStreamRecordCount, DCStreamPosition, DCStreamSize );
	}
	// Keep strict mode active until the caller explicitly closes the stream.
}
#endif
