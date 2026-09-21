#include "CorePrivate.h"
#include <string.h>

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)

struct FDCFileEntry
{
	char Name[64];
	DWORD Size;
};

struct FDCRange
{
	DWORD File;
	DWORD Offset;
	DWORD Length;
	DWORD Physical;
};

static FILE* DCDat = NULL;
static TArray<FDCFileEntry> DCFiles;
static TArray<FDCRange> DCRanges;
static DWORD DCHits = 0;
static DWORD DCMisses = 0;
static DWORD DCSeeks = 0;
static DWORD DCBytes = 0;
static INT DCPosition = 0;
static INT DCLastFile = INDEX_NONE;
static INT DCLastRange = INDEX_NONE;

void appDCDatStats()
{
	printf( "DCDAT hits=%u misses=%u seeks=%u bytes=%u directory=%i\n",
		DCHits, DCMisses, DCSeeks, DCBytes,
		DCFiles.Num() * sizeof(FDCFileEntry) + DCRanges.Num() * sizeof(FDCRange) );
}

void appDCCloseDat()
{
	if( DCDat )
	{
		appDCDatStats();
		appFclose( DCDat );
		DCDat = NULL;
	}
	DCFiles.Empty();
	DCRanges.Empty();
	DCLastFile = INDEX_NONE;
	DCLastRange = INDEX_NONE;
}

void appDCOpenDat( const char* Path )
{
	guard(appDCOpenDat);
	appDCCloseDat();
	DCDat = appFopen( Path, "rb" );
	if( !DCDat )
	{
		debugf( "DCDAT loose fallback: %s", Path );
		return;
	}
	DWORD Header[8];
	appFseek( DCDat, 0, USEEK_END );
	INT Size = appFtell( DCDat );
	appFseek( DCDat, 0, USEEK_SET );
	if( appFread( Header, sizeof(Header), 1, DCDat ) != 1
		|| Header[0] != 0x31444344 || Header[1] != 1 || Header[2] > 1024 || Header[3] > 262144
		|| Header[4] != 32 || Header[5] != 32 + Header[2] * sizeof(FDCFileEntry)
		|| Header[6] != Header[5] + Header[3] * sizeof(FDCRange)
		|| Header[6] > (DWORD)Size || Header[7] != (DWORD)Size )
	{
		appErrorf( "Invalid DCD1 directory: %s", Path );
	}
	DCFiles.SetNum( Header[2] );
	DCRanges.SetNum( Header[3] );
	if( (DCFiles.Num() && appFread( &DCFiles(0), sizeof(FDCFileEntry), DCFiles.Num(), DCDat ) != DCFiles.Num())
		|| (DCRanges.Num() && appFread( &DCRanges(0), sizeof(FDCRange), DCRanges.Num(), DCDat ) != DCRanges.Num()) )
	{
		appErrorf( "Truncated DAT directory" );
	}
	for( INT i = 0; i < DCFiles.Num(); ++i )
	{
		if( !memchr( DCFiles(i).Name, 0, sizeof(DCFiles(i).Name) ) )
		{
			appErrorf( "Invalid DAT source name" );
		}
	}
	for( INT i = 0; i < DCRanges.Num(); ++i )
	{
		const FDCRange& Range = DCRanges(i);
		if( Range.File >= (DWORD)DCFiles.Num() || !Range.Length
			|| Range.Offset > DCFiles(Range.File).Size || Range.Length > DCFiles(Range.File).Size - Range.Offset
			|| Range.Physical < Header[6] || Range.Physical > (DWORD)Size || Range.Length > (DWORD)Size - Range.Physical )
		{
			appErrorf( "Invalid DAT extent" );
		}
		if( i && (Range.File < DCRanges(i - 1).File
			|| (Range.File == DCRanges(i - 1).File && Range.Offset < DCRanges(i - 1).Offset + DCRanges(i - 1).Length)) )
		{
			appErrorf( "Unsorted or overlapping DAT extents" );
		}
	}
	DCHits = DCMisses = DCSeeks = DCBytes = 0;
	DCPosition = appFtell( DCDat );
	printf( "DCDAT opened %s files=%i ranges=%i\n", Path, DCFiles.Num(), DCRanges.Num() );
	unguard;
}

UBOOL appDCReadDat( const char* Filename, INT FileSize, INT Offset, void* Data, INT Length )
{
	guard(appDCReadDat);
	if( !DCDat || !Length )
	{
		return false;
	}
	const char* Name = Filename;
	for( const char* Cursor = Filename; *Cursor; ++Cursor )
	{
		if( *Cursor == '/' || *Cursor == '\\' )
		{
			Name = Cursor + 1;
		}
	}
	INT File = INDEX_NONE;
	if( DCLastFile != INDEX_NONE && !appStricmp( Name, DCFiles(DCLastFile).Name ) )
	{
		File = DCLastFile;
	}
	else
	{
		for( INT i = 0; i < DCFiles.Num(); ++i )
		{
			if( !appStricmp( Name, DCFiles(i).Name ) )
			{
				File = i;
				break;
			}
		}
	}
	if( File == INDEX_NONE )
	{
		++DCMisses;
		return false;
	}
	if( DCFiles(File).Size != (DWORD)FileSize || Offset < 0 || Length < 0 || Offset > FileSize || Length > FileSize - Offset )
	{
		appErrorf( "DAT/source mismatch for %s", Filename );
	}
	DCLastFile = File;
	INT Index = DCLastRange;
	// Unreal serializes many individual bytes/words inside one contiguous
	// extent. Keep that directory entry hot instead of searching per field.
	if( Index == INDEX_NONE || DCRanges(Index).File != (DWORD)File
		|| (DWORD)Offset < DCRanges(Index).Offset
		|| (DWORD)Offset >= DCRanges(Index).Offset + DCRanges(Index).Length )
	{
		INT Low = 0;
		INT High = DCRanges.Num();
		while( Low < High )
		{
			INT Middle = Low + (High - Low) / 2;
			const FDCRange& Range = DCRanges(Middle);
			if( Range.File < (DWORD)File || (Range.File == (DWORD)File && Range.Offset <= (DWORD)Offset) )
			{
				Low = Middle + 1;
			}
			else
			{
				High = Middle;
			}
		}
		Index = Low - 1;
	}
	INT Remaining = Length;
	while( Remaining && Index >= 0 && Index < DCRanges.Num() )
	{
		const FDCRange& Range = DCRanges(Index++);
		if( Range.File != (DWORD)File || Range.Offset > (DWORD)Offset || (DWORD)Offset >= Range.Offset + Range.Length )
		{
			break;
		}
		DCLastRange = Index - 1;
		INT Count = Min( Remaining, (INT)(Range.Offset + Range.Length - Offset) );
		INT Physical = Range.Physical + Offset - Range.Offset;
		if( DCPosition != Physical )
		{
			if( appFseek( DCDat, Physical, USEEK_SET ) )
			{
				appErrorf( "DAT seek failed" );
			}
			++DCSeeks;
		}
		if( appFread( Data, 1, Count, DCDat ) != Count )
		{
			appErrorf( "DAT read failed" );
		}
		DCPosition = Physical + Count;
		Data = (BYTE*)Data + Count;
		Remaining -= Count;
		Offset += Count;
		DCBytes += Count;
	}
	if( Remaining )
	{
		// The caller rereads the entire request from its original package.
		++DCMisses;
		return false;
	}
	++DCHits;
	return true;
	unguard;
}
#endif
