#include <stdlib.h>
#include <stdio.h>
#if defined(DC_RESOURCE_COOKER) && defined(__GLIBC__)
#include <malloc.h>
#endif

#include "DCUtilPrivate.h"
#include "UnRender.h"
#include "UnDCStream.h"

extern CORE_API FGlobalPlatform GTempPlatform;
extern DLL_IMPORT UBOOL GTickDue;
extern "C" {HINSTANCE hInstance;}
extern "C" {char GCC_HIDDEN THIS_PACKAGE[64]="DCUtil";}

#if defined(DC_RESOURCE_COOKER)
void CompactBspVerts( UModel* Model )
{
	if( !Model->Nodes || !Model->Verts )
		return;
	const INT OldCount = Model->Verts->Num();
	TArray<INT> Remap;
	Remap.Add( OldCount );
	for( INT i = 0; i < OldCount; ++i )
		Remap(i) = INDEX_NONE;
	for( INT i = 0; i < Model->Nodes->Num(); ++i )
	{
		const FBspNode& Node = Model->Nodes->Element(i);
		if( !Node.NumVertices )
			continue;
		if( Node.iVertPool < 0 || Node.iVertPool > OldCount - Node.NumVertices )
			appErrorf( "Invalid BSP vertex range in %s node=%d", Model->GetFullName(), i );
		for( INT j = 0; j < Node.NumVertices; ++j )
			Remap(Node.iVertPool + j) = 0;
	}
	TArray<FVert> Packed;
	for( INT i = 0; i < OldCount; ++i )
	{
		if( Remap(i) != INDEX_NONE )
			Remap(i) = Packed.AddItem( Model->Verts->Element(i) );
	}
	for( INT i = 0; i < Model->Nodes->Num(); ++i )
	{
		FBspNode& Node = Model->Nodes->Element(i);
		if( !Node.NumVertices )
		{
			Node.iVertPool = INDEX_NONE;
			continue;
		}
		const INT Start = Remap(Node.iVertPool);
		for( INT j = 0; j < Node.NumVertices; ++j )
		{
			const FVert& Before = Model->Verts->Element(Node.iVertPool + j);
			const FVert& After = Packed(Start + j);
			if( Remap(Node.iVertPool + j) != Start + j
				|| Before.pVertex != After.pVertex || Before.iSide != After.iSide )
				appErrorf( "BSP compaction changed polygon connectivity" );
		}
		Node.iVertPool = Start;
	}
	Model->Verts->SetNum( Packed.Num() );
	Model->Verts->SetMax( Packed.Num() );
	Model->Verts->Realloc();
	for( INT i = 0; i < Packed.Num(); ++i )
		Model->Verts->Element(i) = Packed(i);
	// Shared-side IDs are a separate namespace and deliberately stay unchanged.
	debugf( "DCBSP compact model=%s old=%d packed=%d", Model->GetFullName(), OldCount, Packed.Num() );
}
#endif

class FDatCookSession
{
public:
	enum EMode
	{
		MODE_Record,
		MODE_Verify
	};

	FDatCookSession()
	: Dat(NULL), Manifest(NULL), Indices(NULL), Mode(MODE_Record), DatOffset(0), ReadCount(0), Crc(0xffffffffu)
	, PendingOffset(0), PendingSourceOffset(0), PendingLength(0)
	{
		PendingFilename[0] = 0;
	}

	void Start( const char* DatPath, EMode InMode )
	{
		guard(FDatCookSession::Start);
		check(!Active);
		Mode = InMode;
		Dat = appFopen( DatPath, Mode==MODE_Record ? "w+b" : "rb" );
		if( !Dat )
			appErrorf( "Unable to open DAT '%s'", DatPath );
		if( Mode==MODE_Record )
		{
			char ManifestPath[1024];
			appSprintf( ManifestPath, "%s.manifest.tsv", DatPath );
			Manifest = appFopen( ManifestPath, "w+b" );
			if( !Manifest )
				appErrorf( "Unable to open DAT manifest '%s'", ManifestPath );
			fprintf( Manifest, "# utdat-manifest-v1\n# dat_offset\tlength\tsource_offset\tsource_file\n" );
			appSprintf( ManifestPath, "%s.indices.tsv", DatPath );
			Indices = appFopen( ManifestPath, "w+b" );
			if( !Indices )
				appErrorf( "Unable to open DAT index manifest '%s'", ManifestPath );
			fprintf( Indices, "# utdc-linker-indices-v1\n# source_file\tkind\tindex\n" );
#if defined(DC_RESOURCE_COOKER)
			if( appDCStreamDeferredMips() )
			{
				fprintf( Manifest, "# mip_policy\tdeferred\n" );
			}
#endif
		}
		DatOffset = 0;
		ReadCount = 0;
		Crc = 0xffffffffu;
		PendingLength = 0;
		Active = this;
		GArchiveFileReadCallback = ObserveRead;
		GDCLinkerIndexCallback = ObserveIndex;
		unguard;
	}

	void Finish()
	{
		guard(FDatCookSession::Finish);
		GArchiveFileReadCallback = NULL;
		GDCLinkerIndexCallback = NULL;
		Active = NULL;
		FlushManifestRange();
		if( Mode==MODE_Verify )
		{
			BYTE Extra;
			if( appFread(&Extra,1,1,Dat) == 1 )
				appErrorf( "DAT has trailing data after offset %i", DatOffset );
		}
		else
		{
			fprintf( Manifest, "# size\t%i\n# reads\t%i\n# crc32\t%08x\n", DatOffset, ReadCount, Crc ^ 0xffffffffu );
			appFclose( Manifest );
			Manifest = NULL;
			appFclose( Indices );
			Indices = NULL;
		}
		appFclose( Dat );
		Dat = NULL;
		unguard;
	}

	INT GetSize() const { return DatOffset; }
	INT GetReadCount() const { return ReadCount; }
	DWORD GetCrc() const { return Crc ^ 0xffffffffu; }

private:
	static FDatCookSession* Active;

	static void ObserveRead( const char* Filename, INT Offset, const void* Data, INT Length )
	{
		check(Active);
		Active->OnRead( Filename, Offset, Data, Length );
	}

	static void ObserveIndex( const char* Filename, INT Kind, INT Index )
	{
		check(Active);
		if( Active->Mode == MODE_Record )
			fprintf( Active->Indices, "%s\t%i\t%i\n", Filename, Kind, Index );
	}

	void OnRead( const char* Filename, INT Offset, const void* Data, INT Length )
	{
		guard(FDatCookSession::OnRead);
		if( Mode==MODE_Record )
		{
			if( appFwrite( Data, 1, Length, Dat ) != Length )
				appErrorf( "Failed writing DAT at offset %i", DatOffset );
			RecordManifestRange( Filename, Offset, Length );
		}
		else
		{
			BYTE Buffer[4096];
			const BYTE* Source = (const BYTE*)Data;
			INT Remaining = Length;
			while( Remaining )
			{
				INT Count = Min( Remaining, (INT)ARRAY_COUNT(Buffer) );
				if( appFread( Buffer, 1, Count, Dat ) != Count )
					appErrorf( "DAT ended at offset %i while reading '%s'", DatOffset, Filename );
				for( INT i=0; i<Count; i++ )
					if( Buffer[i] != Source[i] )
						appErrorf( "DAT mismatch at offset %i for '%s' source offset %i", DatOffset+i, Filename, Offset+(Length-Remaining)+i );
				UpdateCrc( Source, Count );
				Source += Count;
				Remaining -= Count;
				DatOffset += Count;
			}
			ReadCount++;
			return;
		}
		UpdateCrc( (const BYTE*)Data, Length );
		DatOffset += Length;
		ReadCount++;
		unguard;
	}

	void UpdateCrc( const BYTE* Data, INT Length )
	{
		for( INT i=0; i<Length; i++ )
		{
			Crc ^= Data[i];
			for( INT Bit=0; Bit<8; Bit++ )
				Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
		}
	}

	void RecordManifestRange( const char* Filename, INT SourceOffset, INT Length )
	{
		if
		( PendingLength
		&& !appStrcmp(PendingFilename,Filename)
		&& PendingOffset + PendingLength == DatOffset
		&& PendingSourceOffset + PendingLength == SourceOffset )
		{
			PendingLength += Length;
			return;
		}
		FlushManifestRange();
		appStrncpy( PendingFilename, Filename, ARRAY_COUNT(PendingFilename) );
		PendingOffset = DatOffset;
		PendingSourceOffset = SourceOffset;
		PendingLength = Length;
	}

	void FlushManifestRange()
	{
		if( PendingLength )
		{
			fprintf( Manifest, "%i\t%i\t%i\t%s\n", PendingOffset, PendingLength, PendingSourceOffset, PendingFilename );
			PendingLength = 0;
		}
	}

	FILE* Dat;
	FILE* Manifest;
	FILE* Indices;
	EMode Mode;
	INT DatOffset;
	INT ReadCount;
	DWORD Crc;
	char PendingFilename[1024];
	INT PendingOffset;
	INT PendingSourceOffset;
	INT PendingLength;
};

FDatCookSession* FDatCookSession::Active = NULL;

#if defined(DC_RESOURCE_COOKER)
static void CheckDeferredStreamPostLoad()
{
	if( !appDCStreamDeferredMips() )
	{
		return;
	}
	DWORD Bytes = 0;
	INT Count = 0;
	// Never supply a missing read here: first use belongs to UTexture::PostLoad.
	// This assertion catches serializers/subclasses which omit that phase.
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		for( INT i = 0; i < It->Mips.Num(); ++i )
		{
			FMipmap& Mip = It->Mips(i);
			if( Mip.DCDataSize > 0 && !Mip.DataArray.Num() && !Mip.StreamData.Size() )
			{
				appErrorf( "PostLoad did not resolve deferred mip: %s mip=%d", It->GetFullName(), i );
			}
			if( Mip.DCDataSize > 0 )
			{
				Bytes += Mip.DCDataSize;
				++Count;
			}
		}
	}
	debugf( "DCDEFERRED postload_verified_mips=%d bytes=%u", Count, Bytes );
}

struct FStreamResourceTest
{
	FDCStreamSlice Slice;
	DWORD Crc;
};

static void CaptureStreamResourceTests( TArray<FStreamResourceTest>& Tests )
{
	DWORD Released = 0;
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		for( INT i = 0; i < It->Mips.Num(); ++i )
		{
			FMipmap& Mip = It->Mips(i);
			if( Mip.StreamData.Size() && Mip.DataArray.Num() )
			{
				if( Mip.StreamData.Size() != Mip.DataArray.Num() )
				{
					appErrorf( "DAT mip reference size mismatch" );
				}
				FStreamResourceTest* Test = new(Tests)FStreamResourceTest;
				Test->Slice = Mip.StreamData;
				Test->Crc = appMemCrc( &Mip.DataArray(0), Mip.DataArray.Num() );
				if( It->Format != TEXF_P8 && It->GetClass() == UTexture::StaticClass
					&& !(It->TextureFlags & (TF_Parametric | TF_Realtime | TF_RealtimePalette)) )
				{
					Released += Mip.DataArray.Num();
					Mip.DataArray.Empty();
					Mip.DataPtr = NULL;
				}
			}
		}
	}
	for( TObjectIterator<UModel> It; It; ++It )
	{
		if( It->LightStreamData.Size() && It->LightBits.Num() )
		{
			if( It->LightStreamData.Size() != It->LightBits.Num() )
			{
				appErrorf( "DAT lighting reference size mismatch" );
			}
			FStreamResourceTest* Test = new(Tests)FStreamResourceTest;
			Test->Slice = It->LightStreamData;
			Test->Crc = appMemCrc( &It->LightBits(0), It->LightBits.Num() );
			Released += It->LightBits.Num();
			It->LightBits.Empty();
		}
	}
	for( TObjectIterator<UMesh> It; It; ++It )
	{
		if( It->DCFrameStreamData.Size() && It->DCFrameWords.Num() )
		{
			INT Bytes = It->DCFrameWords.Num() * sizeof(_WORD);
			if( It->DCFrameStreamData.Size() != Bytes )
			{
				appErrorf( "DAT mesh-frame reference size mismatch" );
			}
			FStreamResourceTest* Test = new(Tests)FStreamResourceTest;
			Test->Slice = It->DCFrameStreamData;
			Test->Crc = appMemCrc( (const BYTE*)&It->DCFrameWords(0), Bytes );
			Released += Bytes;
			It->DCFrameWords.Empty();
		}
	}
	debugf( "DCRESOURCE captured=%d released_cpu=%u", Tests.Num(), Released );
}

static void VerifyStreamResources( TArray<FStreamResourceTest>& Tests )
{
	// Two reverse-order passes simulate eviction/reload and exercise seeks on
	// the independent resource handles after the active load stream is closed.
	for( INT Pass = 0; Pass < 2; ++Pass )
	{
		for( INT i = Tests.Num() - 1; i >= 0; --i )
		{
			TArray<BYTE> Pixels;
			Pixels.SetNum( Tests(i).Slice.Size() );
			Tests(i).Slice.Read( &Pixels(0) );
			if( appMemCrc( &Pixels(0), Pixels.Num() ) != Tests(i).Crc )
			{
				appErrorf( "DAT resource CRC mismatch index=%d pass=%d", i, Pass );
			}
		}
	}
	debugf( "DCRESOURCE VERIFIED slices=%d passes=2 loose_texture_opens=0", Tests.Num() );
}

static void VerifyStreamLighting()
{
	BYTE Block[4096];
	DWORD Crc = 0;
	DWORD Bytes = 0;
	INT Models = 0;
	for( TObjectIterator<UModel> It; It; ++It )
	{
		if( !It->LightStreamData.Size() )
		{
			continue;
		}
		for( INT Offset = 0; Offset < It->LightRawSize; Offset += sizeof(Block) )
		{
			INT Count = Min( It->LightRawSize - Offset, (INT)sizeof(Block) );
			It->ReadLightBits( Offset, Block, Count );
			Crc = ((Crc << 5) | (Crc >> 27)) ^ appMemCrc( Block, Count ) ^ Count;
			Bytes += Count;
		}
		++Models;
	}
	debugf( "DCLIGHTSTREAM VERIFIED models=%d decoded_bytes=%u crc=%08x", Models, Bytes, Crc );
}
#endif

// FObjListItem.
struct FObjListItem
{
	UObject* Obj;
	INT Size;
};

static INT Compare( const FObjListItem& A, const FObjListItem& B )
{
	return A.Size - B.Size;
}

// FExecHook.
class FExecHook : public FExec
{
	UBOOL Exec( const char* Cmd, FOutputDevice* Out )
	{
		return 0;
	}
};

FExecHook GLocalHook;
DLL_EXPORT FExec* GThisExecHook = &GLocalHook;

//
// Handle an error.
//
void FDCUtil::HandleError( const char* Exception )
{
	GIsGuarded=0;
	GIsCriticalError=1;

	debugf( NAME_Exit, "Shutting down after catching exception" );
	GObj.ShutdownAfterError();

	debugf( NAME_Exit, "Exiting due to exception" );
	GErrorHist[ARRAY_COUNT(GErrorHist)-1]=0;

	if( Exception )
		fprintf( stderr, "Fatal error: %s\n", Exception );

	abort();
}

//
// Initialize.
//
void FDCUtil::InitEngine()
{
	guard(InitEngine);

	// Platform init.
	appInit();
	GDynMem.Init( 65536 );

	// Init subsystems.
	GSceneMem.Init( 32768 );

#if defined(DC_RESOURCE_COOKER)
	UBOOL CookSession = ParseParam( appCmdLine(), "COOKSESSION" );
	UBOOL VerifySession = ParseParam( appCmdLine(), "VERIFYSESSION" );
	FDatCookSession Session;
	char SessionPath[2048] = { 0 };
	if( CookSession && VerifySession )
		appErrorf( "COOKSESSION and VERIFYSESSION are mutually exclusive" );
	if( CookSession )
	{
		if( !Parse(appCmdLine(),"OUT=",SessionPath,ARRAY_COUNT(SessionPath)) )
			appErrorf( "COOKSESSION requires OUT=<raw.dat>" );
		GDCStreamCookDeferredMips = ParseParam( appCmdLine(), "DEFERMIPS" );
		Session.Start( SessionPath, FDatCookSession::MODE_Record );
	}
	else if( VerifySession )
	{
		if( !Parse(appCmdLine(),"STREAM=",SessionPath,ARRAY_COUNT(SessionPath)) )
			appErrorf( "VERIFYSESSION requires STREAM=<map.dcs>" );
		appDCStreamOpen( SessionPath );
	}
#endif

	// DAT cooking only needs the object and serialization subsystems. Avoid
	// constructing platform audio and video drivers from the game ini.
	char DatArg[2048];
	if
	( Parse(appCmdLine(),"COOKLIGHT=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"EXPORTDC=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"IMPORTDC=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"CHECKLIGHT=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"AUDITBSP=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"COOKDAT=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"VERIFYDAT=",DatArg,ARRAY_COUNT(DatArg)) )
		return;

	// Create the requested engine.
	UClass* EngineClass;
#if defined(DC_RESOURCE_COOKER)
	if( CookSession || VerifySession )
		EngineClass = GObj.LoadClass( UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	else
#endif
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );

	// Init engine.
	Engine = ConstructClassObject<UEngine>( EngineClass );
	Engine->Init();

#if defined(DC_RESOURCE_COOKER)
	if( CookSession || VerifySession )
	{
		if( !Engine->Render || !Engine->Render->Exec("DCCACHECHECK", GSystem) )
			appErrorf( "Session renderer cache check unavailable" );
	}
#endif

#if defined(DC_RESOURCE_COOKER)
	if( CookSession )
	{
		CheckDeferredStreamPostLoad();
		Session.Finish();
		debugf( "DCSESSION recipe_recorded bytes=%d reads=%d crc=%08x",
			Session.GetSize(), Session.GetReadCount(), Session.GetCrc() );
	}
	else if( VerifySession )
	{
		CheckDeferredStreamPostLoad();
		appDCStreamFinish();
		TArray<FStreamResourceTest> Tests;
		CaptureStreamResourceTests( Tests );
		appDCStreamClose();
		VerifyStreamResources( Tests );
		VerifyStreamLighting();
		debugf( "DCSESSION recipe_verified stream=%s", SessionPath );
	}
#endif

	unguard;
}

//
// Exit the engine.
//
void FDCUtil::ExitEngine()
{
	guard(ExitEngine);
	char DatArg[2048];
	UBOOL IsDatCommand
	= Parse(appCmdLine(),"COOKLIGHT=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"EXPORTDC=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"IMPORTDC=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"CHECKLIGHT=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"AUDITBSP=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"COOKDAT=",DatArg,ARRAY_COUNT(DatArg))
	|| Parse(appCmdLine(),"VERIFYDAT=",DatArg,ARRAY_COUNT(DatArg));
	// Every DAT traversal runs in a fresh process. Some original packages have
	// unsafe editor-time destruction paths, so leave process teardown to the OS.
	if( IsDatCommand )
		return;

	GObj.Exit();
	GMem.Exit();
	GDynMem.Exit();
	GSceneMem.Exit();
	GCache.Exit(1);

	unguard;
}

void FDCUtil::LoadPackages( const char *Dir )
{
	guard(LoadPackages);

	char Path[2048];
	appStrcpy( Path, Dir );

	if( char* Glob = appStrchr( Path, '*' ) )
	{
		char Temp[2048];
		TArray<FString> Files = appFindFiles( Path );
		*Glob = 0;
		for( INT i = 0; i < Files.Num(); ++i )
		{
			snprintf( Temp, sizeof(Temp), "%s%s", Path, *Files(i) );
			UPackage* Pkg = Cast<UPackage>( GObj.LoadPackage( nullptr, Temp, LOAD_KeepImports ) );
			if( !Pkg )
				appErrorf(  "Package '%s' does not exist", Pkg );
			LoadedPackages.Add( Temp, Pkg );
			if( ULinkerLoad* Linker = GObj.GetPackageLinker( Pkg, nullptr, LOAD_KeepImports, nullptr, nullptr ) )
				PackageGuids.Add( Pkg, Linker->Heritage(0) );
		}
	}
	else
	{
		UPackage* Pkg = Cast<UPackage>( GObj.LoadPackage( nullptr, Path, LOAD_KeepImports ) );
		if( !Pkg )
			appErrorf(  "Package '%s' does not exist", Pkg );
		LoadedPackages.Add( Path, Pkg );
		if( ULinkerLoad* Linker = GObj.GetPackageLinker( Pkg, nullptr, LOAD_KeepImports, nullptr, nullptr ) )
			PackageGuids.Add( Pkg, Linker->Heritage(0) );
	}

	unguard;
}

void FDCUtil::ParsePackageArg( const char* Arg, const char* Glob )
{
	if( !appStrcmp( Arg, "*" ) )
	{
		// Go through ALL .u and specific packages
		if( Glob )
			LoadPackages( Glob );
		LoadPackages( "../System/*.u" );
	}
	else
	{
		if( !appStrchr( Arg, '.' ) )
			appErrorf( "Specify filename with extension." );
		LoadPackages( Arg );
	}
}

void FDCUtil::ConvertTexturePkg( const FString& PkgPath, UPackage* Pkg )
{
	guard(ConvertTexturePkg);

	printf( "Converting textures in '%s'\n", Pkg->GetName() );

	// first, collect all palettes to see which ones get orphaned after we're done
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		if( !It->IsIn( Pkg ) )
			continue;
		if( It->Palette && It->Palette->IsIn( Pkg ) )
		{
			if( !( It->TextureFlags & (TF_Realtime|TF_RealtimePalette|TF_Parametric) ) )
				UnrefPalettes.AddUniqueItem( It->Palette );
		}
	}

	UBOOL Changed = false;
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		if( It->IsIn( Pkg ) )
		{
			const BYTE OldFmt = It->Format;
			const INT OldSize = It->MemUsage();
			TotalPrevSize += OldSize;
			if( FTextureConverter::AutoConvertTexture( *It ) )
			{
				const DWORD NewSize = It->MemUsage();
				Changed = true;
				printf( "- Converted '%s' from %d to %d (%d -> %d bytes)\n", It->GetName(), OldFmt, It->Format, OldSize, NewSize );
				TotalNewSize += NewSize;
			}
			if( It->Palette )
				UnrefPalettes.RemoveItem( It->Palette );
		}
	}

	if( Changed )
		ChangedPackages.Add( PkgPath, Pkg );

	unguard;
}

void FDCUtil::ConvertSoundPkg( const FString& PkgPath, UPackage* Pkg )
{
	guard(ConvertSoundPkg);

	printf( "Nuking sounds in '%s'\n", Pkg->GetName() );

	UBOOL Changed = false;
	for( TObjectIterator<USound> It; It; ++It )
	{
		// just nuke for now
		if( It->IsIn( Pkg ) && It->Data.Num() )
		{
			const DWORD Size = It->MemUsage();
			printf( "- Nuking '%s' (%u bytes)\n", It->GetName(), Size );
			TotalPrevSize += Size;
			TotalNewSize += Size - It->Data.Num();
			It->Data.Empty();
			Changed = true;
		}
	}

	if( Changed )
		ChangedPackages.Add( PkgPath, Pkg );

	unguard;
}

void FDCUtil::ConvertMusicPkg(const FString &PkgPath, UPackage *Pkg)
{
	guard(ConvertMusicPkg);

	printf( "Nuking music in '%s'\n", Pkg->GetName() );

	UBOOL Changed = false;
	for( TObjectIterator<UMusic> It; It; ++It )
	{
		// just nuke for now
		if( It->IsIn( Pkg ) && It->Data.Num() )
		{
			const DWORD Size = It->MemUsage();
			printf( "- Nuking '%s' (%u bytes)\n", It->GetName(), Size );
			TotalPrevSize += Size;
			TotalNewSize += Size - It->Data.Num();
			It->Data.Empty();
			Changed = true;
		}
	}

	if( Changed )
		ChangedPackages.Add( PkgPath, Pkg );

	unguard;
}

void FDCUtil::CommitChanges()
{
	if( UnrefPalettes.Num() )
	{
		// double check if any other packages are referencing these
		for( TObjectIterator<UTexture> It; It; ++It )
			if( It->Palette )
				UnrefPalettes.RemoveItem( It->Palette );
		// then delete remaining
		printf( "Cleaning up %d orphaned palettes\n", UnrefPalettes.Num() );
		for( INT i = 0; i < UnrefPalettes.Num(); ++i )
		{
			UnrefPalettes(i)->Colors.Empty();
			UnrefPalettes(i)->ConditionalDestroy();
			UnrefPalettes(i) = nullptr;
		}
		UnrefPalettes.Empty();
	}

	GObj.CollectGarbage( GSystem, RF_Intrinsic | RF_Standalone );

	if( ChangedPackages.Size() )
	{
		printf( "Saving %d changed packages\n", ChangedPackages.Size() );
		for( INT i = 0; i < ChangedPackages.Size(); ++i )
		{
			FString PkgName;
			UPackage* Pkg;
			FGuid* OldGuid;
			ChangedPackages.GetPair( i, PkgName, Pkg );
			// Try to keep the previous version in heritage list to maintain backwards compatibility
			OldGuid = PackageGuids.Find( Pkg );
			GObj.SavePackage( Pkg, nullptr, RF_Standalone, *PkgName, false, OldGuid );
		}
	}

	LoadedPackages.Empty();

	printf( "Total size change: %u -> %u\n", TotalPrevSize, TotalNewSize );
}

void FDCUtil::CookDat( const char* MapPath, const char* DatPath, UBOOL Verify )
{
	guard(FDCUtil::CookDat);

	GIsEditor = false;
	GIsClient = true;
	GIsServer = true;

#if defined(DC_RESOURCE_COOKER)
	GDCStreamCookDeferredMips = !Verify && ParseParam( appCmdLine(), "DEFERMIPS" );
	TArray<FStreamResourceTest> ResourceTests;
	char Warmup[2048] = { 0 };
	if( !Verify && Parse( appCmdLine(), "WARMUP=", Warmup, ARRAY_COUNT(Warmup) ) )
	{
		ULevel* WarmLevel = LoadObject<ULevel>( NULL, "MyLevel", Warmup, LOAD_KeepImports | LOAD_NoFail, NULL );
		check(WarmLevel);
		CheckDeferredStreamPostLoad();
	}
	char WarmStream[2048] = { 0 };
	char WarmMap[256] = { 0 };
	if( Verify && Parse( appCmdLine(), "WARMSTREAM=", WarmStream, ARRAY_COUNT(WarmStream) ) )
	{
		if( !Parse( appCmdLine(), "WARMMAP=", WarmMap, ARRAY_COUNT(WarmMap) ) )
		{
			appErrorf( "WARMSTREAM requires WARMMAP" );
		}
		appDCStreamOpen( WarmStream );
		ULevel* WarmLevel = LoadObject<ULevel>( NULL, "MyLevel", WarmMap, LOAD_KeepImports | LOAD_NoFail, NULL );
		check(WarmLevel);
		CheckDeferredStreamPostLoad();
		appDCStreamFinish();
		CaptureStreamResourceTests( ResourceTests );
		appDCStreamClose();
	}
#endif

	FDatCookSession Session;
	Session.Start( DatPath, Verify ? FDatCookSession::MODE_Verify : FDatCookSession::MODE_Record );
#if defined(DC_RESOURCE_COOKER)
	char IndexedPath[2048] = { 0 };
	char StreamPath[2048] = { 0 };
	if( Verify && Parse( appCmdLine(), "STREAM=", StreamPath, ARRAY_COUNT(StreamPath) ) )
	{
		appDCStreamOpen( StreamPath );
	}
	if( Verify && Parse( appCmdLine(), "INDEXED=", IndexedPath, ARRAY_COUNT(IndexedPath) ) )
	{
		appDCOpenDat( IndexedPath );
	}
#endif
	ULevel* Level = LoadObject<ULevel>( NULL, "MyLevel", MapPath, LOAD_KeepImports | LOAD_NoFail, NULL );
	check(Level);
#if defined(DC_RESOURCE_COOKER)
	CheckDeferredStreamPostLoad();
	appDCValidateNativeClassRegistry();
	if( ParseParam( appCmdLine(), "TESTNATIVERESTARTAFTERLOAD" ) )
	{
		// Negative test: this must reject resident map/script state.
		GObj.RestartNativeCore();
	}
#endif
	Session.Finish();
#if defined(DC_RESOURCE_COOKER)
	if( StreamPath[0] )
	{
		appDCStreamFinish();
		CaptureStreamResourceTests( ResourceTests );
		appDCStreamClose();
		VerifyStreamResources( ResourceTests );
		VerifyStreamLighting();
		if( ResourceTests.Num() && appDCStreamCanResetSession() )
		{
			appErrorf( "DAT reset guard missed retained resource slices" );
		}
		ResourceTests.Empty();
		if( ParseParam( appCmdLine(), "TESTMAPTEARDOWN" ) )
		{
			UBOOL WasRunning = GIsRunning;
			GIsRunning = 0;
			GObj.RestartNativeCore( 1 );
			GIsRunning = WasRunning;
		}
		else
		{
			// Legacy ownership-only test. Loaded teardown must release locators
			// through real object destruction, never through this shortcut.
			for( TObjectIterator<UTexture> It; It; ++It )
			{
				for( INT i = 0; i < It->Mips.Num(); ++i )
				{
					It->Mips(i).StreamData = FDCStreamSlice();
				}
			}
			for( TObjectIterator<UModel> It; It; ++It )
			{
				It->LightStreamData = FDCStreamSlice();
			}
			for( TObjectIterator<UMesh> It; It; ++It )
			{
				It->DCFrameStreamData = FDCStreamSlice();
			}
		}
		appDCStreamShutdown();
		appDCStreamShutdown(); // Session cleanup must also be idempotent.
	}
	if( IndexedPath[0] )
	{
		appDCCloseDat();
	}
#endif

	printf
	(
		"%s '%s': %i bytes, %i reads, crc32=%08x\n",
		Verify ? "Verified" : "Cooked",
		DatPath,
		Session.GetSize(),
		Session.GetReadCount(),
		Session.GetCrc()
	);

	unguard;
}

static void AuditIndex( INT Value, INT Limit, INT& Maximum, INT& BadCount )
{
	if( Value == INDEX_NONE )
		return;
	if( Value < 0 || Value >= Limit )
		BadCount++;
	if( Value > Maximum )
		Maximum = Value;
}

static void AuditOffset( INT Value, INT Limit, INT& Maximum, INT& BadCount )
{
	if( Value == INDEX_NONE )
		return;
	if( Value < 0 || Value > Limit )
		BadCount++;
	if( Value > Maximum )
		Maximum = Value;
}

static void AuditLightList( UModel* Model, INT Root, TArray<BYTE>& Used, INT& Roots, INT& BadCount )
{
	if( Root == INDEX_NONE )
		return;
	++Roots;
	if( Root < 0 || Root >= Model->Lights.Num() )
	{
		++BadCount;
		return;
	}

	for( INT Index = Root; Index < Model->Lights.Num(); ++Index )
	{
		Used(Index) = 1;
		if( !Model->Lights(Index) )
			return;
	}
	++BadCount;
}

void FDCUtil::AuditBsp( const char* MapPath, const char* OutPath )
{
	guard(FDCUtil::AuditBsp);

	GIsEditor = false;
	GIsClient = true;
	GIsServer = true;

	ULevel* Level = LoadObject<ULevel>( NULL, "MyLevel", MapPath, LOAD_KeepImports | LOAD_NoFail, NULL );
	check(Level && Level->Model);
	UModel* Model = Level->Model;
	check(Model->Points && Model->Vectors && Model->Nodes && Model->Surfs && Model->Verts);

	INT MaxNodeVertPool=-1, MaxNodeVertEnd=-1, MaxNodeSurf=-1, MaxNodeChild=-1;
	INT MaxCollisionBound=-1, MaxRenderBound=-1, MaxNodeLeaf=-1;
	INT MaxVertPoint=-1, MaxVertSide=-1;
	INT MaxSurfBase=-1, MaxSurfNormal=-1, MaxSurfU=-1, MaxSurfV=-1;
	INT MaxSurfLightMap=-1, MaxSurfBrushPoly=-1;
	INT MaxLightDataOffset=-1, MaxLightActors=-1, MaxUClamp=-1, MaxVClamp=-1;
	INT MaxLeafZone=-1, MaxLeafPermeating=-1, MaxLeafVolumetric=-1;
	INT BadNode=0, BadVert=0, BadSurf=0, BadLight=0, BadLeaf=0;
	INT UsedVerts=0;
	INT LightRoots=0, UsedLights=0, BadLightLists=0;
	TArray<BYTE> VertUsed;
	VertUsed.AddZeroed( Model->Verts->Num() );
	TArray<BYTE> LightUsed;
	LightUsed.AddZeroed( Model->Lights.Num() );

	for( INT i=0; i<Model->Nodes->Num(); i++ )
	{
		const FBspNode& Node = Model->Nodes->Element(i);
		AuditIndex( Node.iVertPool, Model->Verts->Num(), MaxNodeVertPool, BadNode );
		if( Node.iVertPool != INDEX_NONE )
		{
			INT End = Node.iVertPool + Node.NumVertices;
			if( End > MaxNodeVertEnd ) MaxNodeVertEnd = End;
			if( End > Model->Verts->Num() ) BadNode++;
			else for( INT j=Node.iVertPool; j<End; j++ )
				if( !VertUsed(j) )
				{
					VertUsed(j) = 1;
					UsedVerts++;
				}
		}
		AuditIndex( Node.iSurf, Model->Surfs->Num(), MaxNodeSurf, BadNode );
		for( INT Child=0; Child<3; Child++ )
			AuditIndex( Node.iChild[Child], Model->Nodes->Num(), MaxNodeChild, BadNode );
		AuditIndex( Node.iCollisionBound, Model->LeafHulls.Num(), MaxCollisionBound, BadNode );
		AuditIndex( Node.iRenderBound, Model->Bounds.Num(), MaxRenderBound, BadNode );
		AuditIndex( Node.iLeaf[0], Model->Leaves.Num(), MaxNodeLeaf, BadNode );
		AuditIndex( Node.iLeaf[1], Model->Leaves.Num(), MaxNodeLeaf, BadNode );
	}

	for( INT i=0; i<Model->Verts->Num(); i++ )
	{
		if( !VertUsed(i) )
			continue;
		const FVert& Vert = Model->Verts->Element(i);
		AuditIndex( Vert.pVertex, Model->Points->Num(), MaxVertPoint, BadVert );
		AuditIndex( Vert.iSide, Model->Verts->NumSharedSides, MaxVertSide, BadVert );
	}

	for( INT i=0; i<Model->Surfs->Num(); i++ )
	{
		const FBspSurf& Surf = Model->Surfs->Element(i);
		AuditIndex( Surf.pBase, Model->Points->Num(), MaxSurfBase, BadSurf );
		AuditIndex( Surf.vNormal, Model->Vectors->Num(), MaxSurfNormal, BadSurf );
		AuditIndex( Surf.vTextureU, Model->Vectors->Num(), MaxSurfU, BadSurf );
		AuditIndex( Surf.vTextureV, Model->Vectors->Num(), MaxSurfV, BadSurf );
		AuditIndex( Surf.iLightMap, Model->LightMap.Num(), MaxSurfLightMap, BadSurf );
		if( Surf.iBrushPoly > MaxSurfBrushPoly ) MaxSurfBrushPoly = Surf.iBrushPoly;
	}

	for( INT i=0; i<Model->LightMap.Num(); i++ )
	{
		const FLightMapIndex& Light = Model->LightMap(i);
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
		INT LightSize = Model->LightBlockOffsets.Num() ? Model->LightRawSize : Model->LightBits.Num();
		AuditOffset( Light.DataOffset, LightSize, MaxLightDataOffset, BadLight );
#else
		AuditOffset( Light.DataOffset, Model->LightBits.Num(), MaxLightDataOffset, BadLight );
#endif
		AuditIndex( Light.iLightActors, Model->Lights.Num(), MaxLightActors, BadLight );
		AuditLightList( Model, Light.iLightActors, LightUsed, LightRoots, BadLightLists );
		if( Light.UClamp > MaxUClamp ) MaxUClamp = Light.UClamp;
		if( Light.VClamp > MaxVClamp ) MaxVClamp = Light.VClamp;
	}

	for( INT i=0; i<Model->Leaves.Num(); i++ )
	{
		const FLeaf& Leaf = Model->Leaves(i);
		AuditIndex( Leaf.iZone, 64, MaxLeafZone, BadLeaf );
		AuditIndex( Leaf.iPermeating, Model->Lights.Num(), MaxLeafPermeating, BadLeaf );
		AuditIndex( Leaf.iVolumetric, Model->Lights.Num(), MaxLeafVolumetric, BadLeaf );
		AuditLightList( Model, Leaf.iPermeating, LightUsed, LightRoots, BadLightLists );
		AuditLightList( Model, Leaf.iVolumetric, LightUsed, LightRoots, BadLightLists );
	}
	for( INT i=0; i<LightUsed.Num(); ++i )
		UsedLights += LightUsed(i) != 0;

	FILE* Out = appFopen( OutPath, "w+b" );
	if( !Out )
		appErrorf( "Unable to open BSP audit output '%s'", OutPath );
	fprintf
	(
		Out,
		"BSPAUDIT\tmap=%s\tnodes=%i\tsurfs=%i\tverts=%i\tused_verts=%i\tpoints=%i\tvectors=%i"
		"\tlightmaps=%i\tlightbits=%i\tbounds=%i\tleafhulls=%i\tleaves=%i\tlights=%i\tsharedsides=%i"
		"\tlight_roots=%i\tused_lights=%i\tbad_light_lists=%i"
		"\tmax_node_vertpool=%i\tmax_node_vertend=%i\tmax_node_surf=%i\tmax_node_child=%i"
		"\tmax_collision_bound=%i\tmax_render_bound=%i\tmax_node_leaf=%i"
		"\tmax_vert_point=%i\tmax_vert_side=%i"
		"\tmax_surf_base=%i\tmax_surf_normal=%i\tmax_surf_u=%i\tmax_surf_v=%i"
		"\tmax_surf_lightmap=%i\tmax_surf_brushpoly=%i"
		"\tmax_light_dataoffset=%i\tmax_light_actors=%i\tmax_uclamp=%i\tmax_vclamp=%i"
		"\tmax_leaf_zone=%i\tmax_leaf_permeating=%i\tmax_leaf_volumetric=%i"
		"\tbad_node=%i\tbad_vert=%i\tbad_surf=%i\tbad_light=%i\tbad_leaf=%i\n",
		MapPath, Model->Nodes->Num(), Model->Surfs->Num(), Model->Verts->Num(), UsedVerts,
		Model->Points->Num(), Model->Vectors->Num(), Model->LightMap.Num(), Model->LightBits.Num(),
		Model->Bounds.Num(), Model->LeafHulls.Num(), Model->Leaves.Num(), Model->Lights.Num(),
		Model->Verts->NumSharedSides, LightRoots, UsedLights, BadLightLists,
		MaxNodeVertPool, MaxNodeVertEnd, MaxNodeSurf, MaxNodeChild,
		MaxCollisionBound, MaxRenderBound, MaxNodeLeaf, MaxVertPoint, MaxVertSide,
		MaxSurfBase, MaxSurfNormal, MaxSurfU, MaxSurfV, MaxSurfLightMap, MaxSurfBrushPoly,
		MaxLightDataOffset, MaxLightActors, MaxUClamp, MaxVClamp,
		MaxLeafZone, MaxLeafPermeating, MaxLeafVolumetric,
		BadNode, BadVert, BadSurf, BadLight, BadLeaf
	);
	appFclose( Out );

	unguard;
}

//
// Actual main function.
//
void FDCUtil::Main( )
{
	guard(Main);

	GIsRunning = 1;

	char Temp[2048] = { 0 };
	const char* Cmd = appCmdLine();
	FString PkgPath;
	UPackage* Pkg = nullptr;
	if( ParseParam(Cmd,"COOKSESSION") || ParseParam(Cmd,"VERIFYSESSION") )
	{
#if defined(DC_RESOURCE_COOKER)
		// Exercise the same loaded-engine teardown used by Dreamcast travel.
		GIsRunning = 0;
		GObj.RestartNativeCore( 1 );
		Engine = NULL;
		debugf( "DCSESSION recipe_teardown_verified" );
#else
		appErrorf( "Session recipes require a DC_RESOURCE_COOKER build" );
#endif
	}
	else if( Parse( Cmd, "EXPORTDC=", Temp, sizeof(Temp) - 1 )
		|| Parse( Cmd, "IMPORTDC=", Temp, sizeof(Temp) - 1 ) )
	{
#if defined(DC_RESOURCE_COOKER)
		char ResourceDir[2048] = { 0 };
		char OutPath[2048] = { 0 };
		if( !Parse( Cmd, "RES=", ResourceDir, sizeof(ResourceDir) - 1 ) )
		{
			appErrorf( "Resource commands require RES=<directory>" );
		}
		if( Parse( Cmd, "IMPORTDC=", Temp, sizeof(Temp) - 1 )
			&& !Parse( Cmd, "OUT=", OutPath, sizeof(OutPath) - 1 ) )
		{
			appErrorf( "IMPORTDC requires OUT=<staged package>" );
		}
		ProcessResources( Temp, ResourceDir, OutPath );
#else
		appErrorf( "Resource commands require DC_RESOURCE_COOKER" );
#endif
	}
	else if( Parse( Cmd, "CHECKLIGHT=", Temp, sizeof( Temp ) - 1 ) )
	{
#if defined(DC_RESOURCE_COOKER)
		GIsEditor = false;
		GIsClient = true;
		GIsServer = true;

		ULevel* Level = LoadObject<ULevel>( NULL, "MyLevel", Temp, LOAD_KeepImports | LOAD_NoFail, NULL );
		for( TObjectIterator<UModel> It; It; ++It )
		{
			if( !It->IsIn( Level->GetParent() ) || !It->LightBits.Num() )
			{
				continue;
			}

			INT Size = It->LightBlockOffsets.Num() ? It->LightRawSize : It->LightBits.Num();
			TArray<BYTE> Decoded;
			Decoded.SetNum( Size );
			It->ReadLightBits( 0, &Decoded(0), Size );

			// Exercise the same unaligned, cross-block reads used by the renderer.
			for( INT i = 0; i < It->LightMap.Num(); ++i )
			{
				const FLightMapIndex& Index = It->LightMap(i);
				if( Index.iLightActors == INDEX_NONE )
				{
					continue;
				}

				INT Count = ((Index.UClamp + 7) / 8) * Index.VClamp;
				INT Offset = Index.DataOffset;
				TArray<BYTE> Mask;
				Mask.SetNum( Count );

				for( INT Light = Index.iLightActors; ; ++Light )
				{
					if( Light < 0 || Light >= It->Lights.Num() )
					{
						appErrorf( "Unterminated light list" );
					}

					if( !It->Lights(Light) )
					{
						break;
					}

					if( Count )
					{
						It->ReadLightBits( Offset, &Mask(0), Count );
						if( appMemcmp( &Mask(0), &Decoded(Offset), Count ) )
						{
							appErrorf( "Shadow mask random-access mismatch" );
						}
					}

					Offset += Count;
				}
			}

			printf( "LIGHTCHECK model=%s bytes=%i crc32=%08x\n", It->GetPathName(), Size,
				(DWORD)appMemCrc( &Decoded(0), Size ) );
		}

		printf( "CHECKLIGHT OK\n" );
#else
		appErrorf( "CHECKLIGHT requires a DC_RESOURCE_COOKER build" );
#endif
	}
	else if( Parse( Cmd, "COOKLIGHT=", Temp, sizeof( Temp ) - 1 ) )
	{
#if defined(DC_RESOURCE_COOKER)
		char OutPath[2048] = { 0 };
		if( !Parse( Cmd, "OUT=", OutPath, sizeof(OutPath) - 1 ) || !appStricmp( Temp, OutPath ) )
		{
			appErrorf( "COOKLIGHT requires a separate OUT=<map.unr>" );
		}

		GIsEditor = false;
		GIsClient = true;
		GIsServer = true;

		ULevel* Level = LoadObject<ULevel>( NULL, "MyLevel", Temp, LOAD_KeepImports | LOAD_NoFail, NULL );
		INT Before = 0;
		INT After = 0;
		INT Models = 0;

		for( TObjectIterator<UModel> It; It; ++It )
		{
			if( !It->IsIn( Level->GetParent() ) )
			{
				continue;
			}

			Before += It->LightBits.Num();
			if( ParseParam(Cmd, "COMPACTBSP") )
				CompactBspVerts( *It );
			It->CompactLightLists();
			It->CompressLightBits();
			After += It->LightBits.Num() + 4 * It->LightBlockOffsets.Num();
			++Models;
		}

		if( !GObj.SavePackage( Level->GetParent(), Level, 0, OutPath ) )
		{
			appErrorf( "COOKLIGHT save failed" );
		}

		printf( "COOKLIGHT OK models=%i raw=%i resident=%i output=%s\n", Models, Before, After, OutPath );
#else
		appErrorf( "COOKLIGHT requires a DC_RESOURCE_COOKER build" );
#endif
	}
	else if( Parse( Cmd, "AUDITBSP=", Temp, sizeof( Temp ) - 1 ) )
	{
		char OutPath[2048] = { 0 };
		if( !Parse( Cmd, "OUT=", OutPath, sizeof( OutPath ) - 1 ) )
			appErrorf( "AUDITBSP requires OUT=<report.tsv>" );
		AuditBsp( Temp, OutPath );
	}
	else if( Parse( Cmd, "COOKDAT=", Temp, sizeof( Temp ) - 1 ) )
	{
		char DatPath[2048] = { 0 };
		if( !Parse( Cmd, "OUT=", DatPath, sizeof( DatPath ) - 1 ) )
			appErrorf( "COOKDAT requires OUT=<file.dat>" );
		CookDat( Temp, DatPath, false );
	}
	else if( Parse( Cmd, "VERIFYDAT=", Temp, sizeof( Temp ) - 1 ) )
	{
		char DatPath[2048] = { 0 };
		if( !Parse( Cmd, "DAT=", DatPath, sizeof( DatPath ) - 1 ) )
			appErrorf( "VERIFYDAT requires DAT=<file.dat>" );
#if defined(DC_RESOURCE_COOKER)
		INT MapCycles = 1;
		Parse( Cmd, "TESTMAPCYCLES=", MapCycles );
		if( MapCycles < 1 || MapCycles > 100
			|| (MapCycles > 1 && !ParseParam(Cmd, "TESTMAPTEARDOWN")) )
		{
			appErrorf( "TESTMAPCYCLES requires 1..100 cycles and TESTMAPTEARDOWN" );
		}
		for( INT Cycle = 0; Cycle < MapCycles; ++Cycle )
		{
			CookDat( Temp, DatPath, true );
			if( ParseParam(Cmd, "TESTMAPTEARDOWN") && getenv("UE1_RESTART_ALLOCS") )
			{
				debugf( "DCNATIVE allocations_begin cycle=%d", Cycle + 1 );
				appDumpAllocs( GSystem );
				debugf( "DCNATIVE allocations_end cycle=%d", Cycle + 1 );
			}
#if defined(__GLIBC__)
			if( ParseParam(Cmd, "TESTMAPTEARDOWN") )
			{
				struct mallinfo Heap = mallinfo();
				debugf( "DCNATIVE map_cycle=%d host_heap_inuse=%d host_heap_arena=%d",
					Cycle + 1, Heap.uordblks, Heap.arena );
			}
#endif
		}
		debugf( "DCNATIVE map_cycles_complete cycles=%d", MapCycles );
#else
		CookDat( Temp, DatPath, true );
#endif
	}
	else if( Parse( Cmd, "CVTUTX=", Temp, sizeof( Temp ) - 1 ) )
	{
		ParsePackageArg( Temp, "../Textures/*.utx" );
		for( INT i = 0; i < LoadedPackages.Size(); ++i )
		{
			LoadedPackages.GetPair( i, PkgPath, Pkg );
			ConvertTexturePkg( PkgPath, Pkg );
		}
		CommitChanges();
	}
	else if( Parse( Cmd, "CVTUAX=", Temp, sizeof( Temp ) - 1 ) )
	{
		ParsePackageArg( Temp, "../Sounds/*.uax" );
		for( INT i = 0; i < LoadedPackages.Size(); ++i )
		{
			LoadedPackages.GetPair( i, PkgPath, Pkg );
			ConvertSoundPkg( PkgPath, Pkg );
		}
		CommitChanges();
	}
	else if( Parse( Cmd, "CVTUMX=", Temp, sizeof( Temp ) - 1 ) )
	{
		ParsePackageArg( Temp, "../Music/*.umx" );
		for( INT i = 0; i < LoadedPackages.Size(); ++i )
		{
			LoadedPackages.GetPair( i, PkgPath, Pkg );
			ConvertMusicPkg( PkgPath, Pkg );
		}
		CommitChanges();
	}
	else if( Parse( Cmd, "CVTALL=", Temp, sizeof( Temp ) - 1 ) )
	{
		if( Temp[0] == '*' && Temp[0] == 0 )
		{
			LoadPackages( "../Textures/*.utx" );
			LoadPackages( "../Sounds/*.uax" );
			LoadPackages( "../Music/*.umx" );
			LoadPackages( "../System/*.u" );
		}
		else
		{
			if( !appStrchr( Temp, '.' ) )
				appErrorf( "Specify filename with extension." );
			LoadPackages( Temp );
		}
		for( INT i = 0; i < LoadedPackages.Size(); ++i )
		{
			LoadedPackages.GetPair( i, PkgPath, Pkg );
			ConvertTexturePkg( PkgPath, Pkg );
			ConvertSoundPkg( PkgPath, Pkg );
			ConvertMusicPkg( PkgPath, Pkg );
		}
		CommitChanges();
	}
	else if( Parse( Cmd, "LIST=", Temp, sizeof( Temp ) - 1 ) )
	{
		Pkg = Cast<UPackage>( GObj.LoadPackage( Pkg, Temp, LOAD_KeepImports ) );
		if( !Pkg )
			appThrowf( "Package '%s' does not exist", Temp );

		TArray<FObjListItem> Items;
		for( TObjectIterator<UObject> It; It; ++It )
		{
			if( It->IsIn( Pkg ) )
				Items.AddItem( { *It, It->MemUsage() } );
		}

		appSort( &Items(0), Items.Num() );

		printf( "%d objects in '%s':\n", Items.Num(), Pkg->GetName() );
		for( INT i = 0; i < Items.Num(); ++i )
			printf( "- %s: %s (%d bytes)\n", Items(i).Obj->GetPathName(), Items(i).Obj->GetClassName(), Items(i).Size );
	}
	else
	{
		printf( "Usage: DCUtil AUDITBSP=<MAP.UNR> OUT=<REPORT.TSV>\n" );
		printf( "       DCUtil COOKDAT=<MAP.UNR> OUT=<MAP.DAT>\n" );
		printf( "       DCUtil VERIFYDAT=<MAP.UNR> DAT=<MAP.DAT>\n" );
		printf( "       DCUtil <MAP.UNR> -COOKSESSION OUT=<RAW.DAT> -DEFERMIPS\n" );
		printf( "       DCUtil <MAP.UNR> -VERIFYSESSION STREAM=<MAP.DCS>\n" );
		printf( "       DCUtil CVTUTX=<TEXPKG>\n" );
	}

	GIsRunning = 0;

	unguard;
}

int main( int argc, const char** argv )
{
	hInstance = NULL;
	appSetCmdLine( argc, argv );

	GIsStarted = 1;

	// Set package name.
	appStrcpy( THIS_PACKAGE, appPackage() );

	// Init mode.
	GIsServer = true;
	GIsClient = true;
	GIsEditor = !ParseParam(appCmdLine(),"COOKSESSION")
		&& !ParseParam(appCmdLine(),"VERIFYSESSION");

	appChdir( appBaseDir() );

	GExecHook = GThisExecHook;

	// Begin.
	FDCUtil DCUtil;
	try
	{
		// Start main loop.
		GIsGuarded=1;
		GSystem = &GTempPlatform;
		DCUtil.InitEngine();
#if defined(DC_RESOURCE_COOKER)
		INT RestartCycles = 0;
		if( Parse( appCmdLine(), "TESTNATIVERESTART=", RestartCycles ) )
		{
			if( RestartCycles < 1 || RestartCycles > 100 )
			{
				appErrorf( "TESTNATIVERESTART must be between 1 and 100" );
			}
			for( INT Cycle = 0; Cycle < RestartCycles; ++Cycle )
			{
				GObj.RestartNativeCore();
			}
			debugf( "DCNATIVE restart_test_complete cycles=%d", RestartCycles );
		}
#endif
		DCUtil.Main();
#if defined(DC_RESOURCE_COOKER)
		// These commands own a fresh process. Core's static class destructors
		// otherwise run after appExit has destroyed the global name table.
		char ResourceArg[2048];
		if( Parse( appCmdLine(), "COOKLIGHT=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| Parse( appCmdLine(), "EXPORTDC=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| Parse( appCmdLine(), "IMPORTDC=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| Parse( appCmdLine(), "CHECKLIGHT=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| Parse( appCmdLine(), "AUDITBSP=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| Parse( appCmdLine(), "COOKDAT=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| Parse( appCmdLine(), "VERIFYDAT=", ResourceArg, ARRAY_COUNT(ResourceArg) )
			|| ParseParam( appCmdLine(), "COOKSESSION" )
			|| ParseParam( appCmdLine(), "VERIFYSESSION" ) )
		{
			fflush( NULL );
			_Exit( 0 );
		}
#endif
		DCUtil.ExitEngine();
		GIsGuarded=0;
	}
	catch( const char* Error )
	{
		// Fatal error.
		try { DCUtil.HandleError( Error ); } catch( ... ) { }
	}
	catch( ... )
	{
		// Crashed.
		try { DCUtil.HandleError( nullptr ); } catch( ... ) { }
	}

	// Shut down.
	GExecHook=NULL;
	appExit();
	GIsStarted = 0;
	return 0;
}
