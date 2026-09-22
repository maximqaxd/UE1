#include "DCUtilPrivate.h"

#if defined(DC_RESOURCE_COOKER)

static void ResourceFile( char* Path, INT Capacity, const char* Directory, UObject* Object, const char* Extension )
{
	INT Length = snprintf( Path, Capacity, "%s/%s.%s", Directory, Object->GetPathName(), Extension );
	if( Length < 0 || Length >= Capacity )
	{
		appErrorf( "Resource path too long" );
	}
}

static void WriteResource( const char* Path, const TArray<BYTE>& Data )
{
	FILE* File = appFopen( Path, "wb" );
	if( !File || (Data.Num() && appFwrite( &Data(0), 1, Data.Num(), File ) != Data.Num()) )
	{
		appErrorf( "Cannot write resource %s", Path );
	}
	appFclose( File );
}

static void ReadResource( const char* Path, TArray<BYTE>& Data )
{
	FILE* File = appFopen( Path, "rb" );
	if( !File )
	{
		appErrorf( "Missing cooked resource %s", Path );
	}
	appFseek( File, 0, USEEK_END );
	INT Size = appFtell( File );
	appFseek( File, 0, USEEK_SET );
	if( Size <= 0 || Size > 64 * 1024 * 1024 )
	{
		appErrorf( "Invalid cooked resource size: %s", Path );
	}
	Data.SetNum( Size );
	if( appFread( &Data(0), 1, Size, File ) != Size )
	{
		appErrorf( "Truncated cooked resource %s", Path );
	}
	appFclose( File );
}

static UBOOL TextureNeedsMipmaps( UTexture* Texture, UPackage* Package )
{
	if( Texture->Mips.Num() <= 1 )
	{
		return 0;
	}

	// MenuGr is imported into Unreal.MenuGfx. Icons contains the HUD and
	// crosshair atlas. These are drawn in screen space and must stay base-only
	// even when a retail package happens to carry a generated mip chain.
	const char* Group = Texture->GetParent() ? Texture->GetParent()->GetName() : "";
	if( !appStricmp(Package->GetName(), "MenuGr")
		|| !appStricmp(Group, "MenuGfx")
		|| !appStricmp(Group, "Icons") )
	{
		return 0;
	}
	return 1;
}

static void PatchDreamcastPlayerFallback( UPackage* Package )
{
	// The retail file is UnrealI.u, while its script classes use the logical
	// Unreal package name.
	if( appStricmp(Package->GetName(), "UnrealI") )
	{
		return;
	}

	UClass* GameInfo = FindObject<UClass>( ANY_PACKAGE, "UnrealGameInfo" );
	UClass* FemaleOne = FindObject<UClass>( ANY_PACKAGE, "FemaleOne" );
	UClass* MaleOne = FindObject<UClass>( ANY_PACKAGE, "MaleOne" );
	if( !GameInfo || !FemaleOne || !MaleOne )
	{
		appErrorf( "Cannot patch Dreamcast player fallback" );
	}
	UObjectProperty* DefaultPlayerProperty = NULL;
	for( TFieldIterator<UProperty> It(GameInfo); It; ++It )
	{
		if( It->GetFName() == FName("DefaultPlayerClass") )
		{
			DefaultPlayerProperty = Cast<UObjectProperty>( *It );
			break;
		}
	}
	if( !DefaultPlayerProperty )
	{
		appErrorf( "Missing DefaultPlayerClass property" );
	}

	INT PatchedClasses = 0;
	for( TObjectIterator<UClass> It; It; ++It )
	{
		UClass* Class = *It;
		if( !Class->IsChildOf(GameInfo)
		|| DefaultPlayerProperty->Offset + (INT)sizeof(UObject*) > Class->Defaults.Num() )
		{
			continue;
		}
		UObject** Value = (UObject**)(&Class->Defaults(DefaultPlayerProperty->Offset));
		if( *Value == MaleOne )
		{
			*Value = FemaleOne;
			++PatchedClasses;
		}
	}
	printf( "DCDEPENDENCY player_fallback=%s patched_classes=%d\n",
		FemaleOne->GetPathName(), PatchedClasses );
}

void FDCUtil::ProcessResources( const char* PackagePath, const char* ResourceDir, const char* OutPath )
{
	guard(FDCUtil::ProcessResources);

	// Full package loading preserves script exports and resource namespaces.
	GIsEditor = true;
	UPackage* Package = Cast<UPackage>( GObj.LoadPackage( NULL, PackagePath, LOAD_KeepImports | LOAD_NoFail ) );
	if( !Package )
	{
		appErrorf( "Cannot load package %s", PackagePath );
	}

	UBOOL Import = OutPath && OutPath[0];
	INT Sounds = 0;
	INT Textures = 0;
	INT Music = 0;
	INT Meshes = 0;
	INT Models = 0;
	char Path[2048];

	for( TObjectIterator<USound> It; It; ++It )
	{
		if( !It->IsIn( Package ) || !It->Data.Num() )
		{
			continue;
		}
		ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, Import ? "dca.wav" : "wav" );
		if( Import )
		{
			ReadResource( Path, It->Data );
			FWaveModInfo Wave;
			if( !Wave.ReadWaveInfo( It->Data ) || *Wave.pBitsPerSample != 4 || *Wave.pChannels != 1 )
			{
				appErrorf( "Cooked audio must be mono Yamaha ADPCM: %s", Path );
			}
			It->FileType = FName("dca");
		}
		else
		{
			WriteResource( Path, It->Data );
		}
		++Sounds;
	}

	for( TObjectIterator<UTexture> It; It; ++It )
	{
		if( !It->IsIn( Package ) || It->GetClass() != UTexture::StaticClass
			|| It->Format != TEXF_P8 || !It->Palette || !It->Mips.Num()
			|| FTextureConverter::IsBlacklisted( *It )
			|| (It->TextureFlags & (TF_Realtime | TF_RealtimePalette | TF_Parametric)) )
		{
			continue;
		}

		ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, Import ? "dt" : "png" );
		if( Import )
		{
			// Keep the DT header with the payload; the renderer validates its mode
			// and uploads the complete hardware mip chain directly.
			TArray<BYTE> Data;
			ReadResource( Path, Data );
			if( Data.Num() < 32 || appMemcmp( &Data(0), "DcTx", 4 ) )
			{
				appErrorf( "Invalid DT texture %s", Path );
			}
			if( It->Mips.Num() > 1 )
			{
				It->Mips.Remove( 1, It->Mips.Num() - 1 );
			}
			It->Mips(0).DataArray = Data;
			It->Mips(0).DataPtr = &It->Mips(0).DataArray(0);
			It->Format = TEXF_EXT_DCTEX;
		}
		else
		{
			FTextureConverter::ExportDCTexture( *It, Path );

			// Preserve the source package's authoring decision and screen-space
			// interface groups. A sidecar lets the external encoder distinguish
			// base-only textures without relying on individual object names.
			ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, "png.nomip" );
			if( !TextureNeedsMipmaps(*It, Package) )
			{
				TArray<BYTE> Marker;
				WriteResource( Path, Marker );
			}
			else
			{
				appUnlink( Path );
			}
		}
		++Textures;
	}

	if( Import )
	{
		INT PackagePathLength = appStrlen(PackagePath);
		UBOOL CompactMapBsp = PackagePathLength >= 4
			&& !appStricmp(PackagePath + PackagePathLength - 4, ".unr");

		// EntryGameInfo inherits UnrealGameInfo's MaleOne fallback. The shipped
		// Dreamcast campaign selects FemaleOne explicitly, so make the global
		// fallback match it and avoid pulling both player mesh families into Entry.
		PatchDreamcastPlayerFallback( Package );

		for( TObjectIterator<UMesh> It; It; ++It )
		{
			if( It->IsIn( Package ) && It->FrameVerts > 0 )
			{
				It->CookDCMesh();
				++Meshes;
			}
		}

		for( TObjectIterator<UModel> It; It; ++It )
		{
			if( It->IsIn( Package ) )
			{
				UBOOL Touched = 0;
				if( CompactMapBsp && It->Nodes && It->Verts )
				{
					CompactBspVerts( *It );
					Touched = 1;
				}
				if( It->LightBits.Num() || It->Lights.Num() )
				{
					It->CompactLightLists();
					It->CompressLightBits();
					Touched = 1;
				}
				if( Touched )
					++Models;
			}
		}

		for( TObjectIterator<UMusic> It; It; ++It )
		{
			if( It->IsIn( Package ) )
			{
				It->Data.Empty();
				++Music;
			}
		}

		if( !Sounds && !Textures && !Music && !Meshes && !Models )
		{
			// Native/code-only packages need no rewrite. Preserve their original
			// export tables and GUIDs byte for byte.
			TArray<BYTE> Original;
			ReadResource( PackagePath, Original );
			WriteResource( OutPath, Original );
			printf( "RESOURCES OK unchanged package=%s\n", Package->GetName() );
			return;
		}

		// Preserve original exports, not native-only fields introduced by this
		// engine build whose parent class has no export in the source package.
		for( FObjectIterator It; It; ++It )
		{
			if( It->IsIn( Package ) )
			{
				It->ClearFlags( RF_Standalone );
			}
		}
		ULinkerLoad* Source = GObj.GetPackageLinker( Package, PackagePath, LOAD_NoFail, NULL, NULL );
		for( INT i = 0; i < Source->ExportMap.Num(); ++i )
		{
			if( Source->ExportMap(i)._Object )
			{
				Source->ExportMap(i)._Object->SetFlags( RF_Standalone );
			}
		}

		if( !GObj.SavePackage( Package, NULL, RF_Standalone, OutPath ) )
		{
			appErrorf( "Cannot save cooked package %s", OutPath );
		}
	}

	printf( "RESOURCES OK import=%i sounds=%i textures=%i music=%i package=%s\n",
		Import, Sounds, Textures, Music, Package->GetName() );
	unguard;
}
#endif
