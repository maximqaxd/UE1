#include "Sound.h"
#include <stdlib.h>

extern CORE_API void appCreateTempFilename( const char* Path, char* Result256 );
extern CORE_API INT appUnlink( const char* Filename );

static FString CreateTempFile( const char* Ext )
{
	char Temp[256] = {0};
	appCreateTempFilename( ".", Temp );
	FString Path = Temp;
	Path += Ext;
	return Path;
}

static FString SaveSoundToTempWav( USound* Sound )
{
	if( !Sound || Sound->Data.Num() == 0 )
		return "";

	FString TempPath = CreateTempFile( ".wav" );
	FILE* Out = appFopen( *TempPath, "wb" );
	if( !Out )
		return "";

	fwrite( &Sound->Data(0), 1, Sound->Data.Num(), Out );
	appFclose( Out );
	return TempPath;
}

static FString RunFfmpegAdpcm( const FString& InPath )
{
	FString OutPath = CreateTempFile( ".adpcm.wav" );
	char Cmd[1024];
	appSprintf( Cmd, "ffmpeg -y -i \"%s\" -ac 1 -ar 11025 -f wav -acodec adpcm_yamaha \"%s\"", *InPath, *OutPath );
	if( system( Cmd ) != 0 )
		return "";
	return OutPath;
}

static UBOOL LoadAdpcmBack( USound* Sound, const FString& AdpcmPath )
{
	FILE* In = appFopen( *AdpcmPath, "rb" );
	if( !In )
		return 0;
	fseek( In, 0, SEEK_END );
	INT Size = ftell( In );
	fseek( In, 0, SEEK_SET );
	TArray<BYTE> Data;
	Data.Add( Size );
	if( fread( &Data(0), 1, Size, In ) != (size_t)Size )
	{
		appFclose( In );
		return 0;
	}
	appFclose( In );

	Sound->Data = Data;
	Sound->FileType = FName("WAV");
	Sound->OriginalSize = Sound->Data.Num();
	Sound->Handle = nullptr;
	return 1;
}

UBOOL FSoundCompressor::CompressUSound( USound* Sound )
{
	if( !Sound || Sound->Data.Num() == 0 )
		return 0;

	const FString Wav = SaveSoundToTempWav( Sound );
	if( !appStrlen( *Wav ) )
		return 0;
	const FString Adpcm = RunFfmpegAdpcm( Wav );
	if( !appStrlen( *Adpcm ) )
		return 0;
	const UBOOL Result = LoadAdpcmBack( Sound, Adpcm );
	if( Result )
	{
		Sound->OriginalSize = Sound->Data.Num();
	}
	appUnlink( *Wav );
	appUnlink( *Adpcm );
	return Result;
}

