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

static UBOOL IsScreenSpaceDreamcastProcedural( UTexture* Texture )
{
	const char* Path = Texture->GetPathName();
	return !appStricmp(Path, "UnrealI.MenuGfx.menu2")
		|| !appStricmp(Path, "UnrealI.MenuGfx.MenuBarrier")
		|| !appStricmp(Path, "MenuGr.menu2")
		|| !appStricmp(Path, "MenuGr.MenuBarrier");
}

static UBOOL IsDreamcastStarfield( UTexture* Texture )
{
	const char* Path = Texture->GetPathName();
	return !appStricmp(Path, "GenFluid.Sky.NghSky3")
		|| !appStricmp(Path, "GenFluid.Sky.Ntskyt");
}

enum { DCProceduralFrames = 8, DCProceduralFPS = 8 };

static UBOOL IsAnimatedDreamcastProcedural( UTexture* Texture )
{
	return Texture->Format == TEXF_P8
		&& Texture->Palette
		&& Texture->Mips.Num()
		&& (Texture->TextureFlags & TF_Parametric)
		&& Texture->GetClass() != UTexture::StaticClass
		&& !Texture->IsA(UFont::StaticClass);
}

static void NullCookedProceduralInputs( UTexture* Texture )
{
	for( TFieldIterator<UProperty> It(Texture->GetClass()); It; ++It )
	{
		UObjectProperty* Property = Cast<UObjectProperty>( *It );
		if( !Property )
		{
			continue;
		}
		if( appStricmp(Property->GetName(), "SourceTexture")
		&& appStricmp(Property->GetName(), "GlassTexture") )
		{
			continue;
		}
		if( Property->Offset + (INT)sizeof(UObject*) <= Texture->GetClass()->GetPropertiesSize() )
		{
			*(UObject**)((BYTE*)Texture + Property->Offset) = NULL;
		}
	}
}

static void BakeDreamcastProcedural( UTexture* Texture )
{
	// Advance past the mostly empty initial state before sampling a compact,
	// looping Dreamcast animation.
	for( INT Frame=0; Frame<32; ++Frame )
	{
		Texture->Tick( 1.f / 30.f );
	}
}

static UBOOL TextureNeedsMipmaps( UTexture* Texture, UPackage* Package,
	const TArray<UTexture*>& MeshTextures )
{
	// Sparse one-pixel stars are a worst case for both 2x2 VQ blocks and mip
	// averaging. Preserve the authored base image exactly for the sky dome.
	if( IsDreamcastStarfield(Texture) )
	{
		return 0;
	}

	if( IsScreenSpaceDreamcastProcedural(Texture) )
	{
		return 0;
	}
	// Keep animated world/procedural frames mipmapped while their visual
	// quality is being evaluated; an actor may also use one as a mesh skin.
	if( IsAnimatedDreamcastProcedural(Texture) )
		return 1;
	// Mesh skins and actor/prop skins are sampled at their authored UV scale;
	// they do not benefit enough from a hardware mip chain to justify its VRAM.
	INT MeshIndex;
	if( MeshTextures.FindItem(Texture, MeshIndex) )
		return 0;
	const char* Group = Texture->GetParent() ? Texture->GetParent()->GetName() : "";
	if( !appStricmp(Group, "Skins") )
		return 0;

	// Font glyph atlases are always sampled in screen space. Their character
	// metrics remain in UFont; only the immutable bitmap is cooked to DT.
	if( Texture->IsA(UFont::StaticClass) )
	{
		return 0;
	}

	// MenuGr is imported into Unreal.MenuGfx. Icons contains the HUD and
	// crosshair atlas. These are drawn in screen space and must stay base-only
	// even when a retail package happens to carry a generated mip chain.
	if( !appStricmp(Package->GetName(), "MenuGr")
		|| !appStricmp(Group, "MenuGfx")
		|| !appStricmp(Group, "Icons") )
	{
		return 0;
	}

	// Procedural world textures normally contain only their writable base mip.
	// The Dreamcast encoder can still build the complete hardware mip chain
	// from each cooked animation frame. Leaving these base-only causes severe
	// aliasing on receding water, fire and smoke surfaces.
	if( Texture->Mips.Num() <= 1 )
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
	printf( "DCCOOK loading %s\n", PackagePath );
	fflush( stdout );
	UPackage* Package = Cast<UPackage>( GObj.LoadPackage( NULL, PackagePath, LOAD_KeepImports | LOAD_NoFail ) );
	if( !Package )
	{
		appErrorf( "Cannot load package %s", PackagePath );
	}

	UBOOL Import = OutPath && OutPath[0];
	printf( "DCCOOK loaded %s; %s resources\n", Package->GetName(),
		Import ? "importing" : "exporting" );
	fflush( stdout );
	INT Sounds = 0;
	INT Textures = 0;
	INT Music = 0;
	INT Meshes = 0;
	INT Models = 0;
	char Path[2048];
	TArray<UTexture*> MeshTextures;
	if( !Import )
	{
		for( TObjectIterator<UMesh> Mesh; Mesh; ++Mesh )
			for( INT i = 0; i < Mesh->Textures.Num(); ++i )
				if( Mesh->Textures(i) )
					MeshTextures.AddUniqueItem( Mesh->Textures(i) );
		for( TObjectIterator<AActor> Actor; Actor; ++Actor )
			if( Actor->Mesh && Actor->Skin )
				MeshTextures.AddUniqueItem( Actor->Skin );
	}

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
		if( !(Sounds % 32) )
		{
			printf( "DCCOOK %s sounds=%d\n", Package->GetName(), Sounds );
			fflush( stdout );
		}
	}
	printf( "DCCOOK %s sounds complete=%d\n", Package->GetName(), Sounds );
	fflush( stdout );

	for( TObjectIterator<UTexture> It; It; ++It )
	{
		const UBOOL AnimatedProcedural = IsAnimatedDreamcastProcedural( *It );
		const UBOOL ConvertibleClass = It->GetClass() == UTexture::StaticClass
			|| It->IsA(UFont::StaticClass)
			|| AnimatedProcedural;
		if( !It->IsIn(Package) )
		{
			continue;
		}
		if( !ConvertibleClass || It->Format != TEXF_P8 || !It->Palette || !It->Mips.Num()
			|| (!AnimatedProcedural
				&& (It->TextureFlags & (TF_Realtime | TF_RealtimePalette | TF_Parametric))) )
		{
			continue;
		}

		if( AnimatedProcedural )
		{
			if( Import )
			{
				const INT USize = It->USize;
				const INT VSize = It->VSize;
				const BYTE UBits = It->UBits;
				const BYTE VBits = It->VBits;
				It->Mips.Empty();
				for( INT Frame = 0; Frame < DCProceduralFrames; ++Frame )
				{
					char Extension[32];
					snprintf( Extension, sizeof(Extension), "frame%02d.dt", Frame );
					ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, Extension );
					FMipmap& Mip = *new(It->Mips) FMipmap;
					ReadResource( Path, Mip.DataArray );
					if( Mip.DataArray.Num() < 32 || appMemcmp(&Mip.DataArray(0), "DcTx", 4) )
					{
						appErrorf( "Invalid animated DT frame %s", Path );
					}
					Mip.DataPtr = &Mip.DataArray(0);
					Mip.USize = USize;
					Mip.VSize = VSize;
					Mip.UBits = UBits;
					Mip.VBits = VBits;
				}
				UBOOL AllFramesIdentical = 1;
				for( INT Frame = 1; Frame < It->Mips.Num(); ++Frame )
				{
					AllFramesIdentical = It->Mips(Frame).DataArray.Num()
						== It->Mips(0).DataArray.Num()
						&& !appMemcmp(
							&It->Mips(Frame).DataArray(0),
							&It->Mips(0).DataArray(0),
							It->Mips(0).DataArray.Num() );
					if( !AllFramesIdentical )
					{
						break;
					}
				}
				if( AllFramesIdentical )
				{
					It->Mips.Remove( 1, It->Mips.Num() - 1 );
					It->Format = TEXF_EXT_DCTEX;
				}
				else
				{
					It->Format = TEXF_EXT_DCANIM;
				}
				It->Palette = NULL;
				It->PrimeCount = 0;
				It->PrimeCurrent = 0;
				It->MinFrameRate = DCProceduralFPS;
				It->MaxFrameRate = DCProceduralFPS;
				It->TextureFlags &= ~(TF_Parametric | TF_RealtimeChanged | TF_RealtimePalette | TF_Realtime);
				if( !AllFramesIdentical )
				{
					It->TextureFlags |= TF_Realtime;
				}
				NullCookedProceduralInputs( *It );
			}
			else
			{
				BakeDreamcastProcedural( *It );
				for( INT Frame = 0; Frame < DCProceduralFrames; ++Frame )
				{
					It->Tick( 1.f / DCProceduralFPS );
					char Extension[32];
					snprintf( Extension, sizeof(Extension), "frame%02d.png", Frame );
					ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, Extension );
					FTextureConverter::ExportDCTexture( *It, Path );
					char MarkerExtension[40];
					snprintf( MarkerExtension, sizeof(MarkerExtension),
						"frame%02d.png.nomip", Frame );
					ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, MarkerExtension );
					if( !TextureNeedsMipmaps(*It, Package, MeshTextures) )
					{
						TArray<BYTE> Marker;
						WriteResource( Path, Marker );
					}
					else
					{
						appUnlink( Path );
					}
				}
			}
			++Textures;
			if( !(Textures % 32) )
			{
				printf( "DCCOOK %s textures=%d\n", Package->GetName(), Textures );
				fflush( stdout );
			}
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
			if( !TextureNeedsMipmaps(*It, Package, MeshTextures) )
			{
				TArray<BYTE> Marker;
				WriteResource( Path, Marker );
			}
			else
			{
				appUnlink( Path );
			}

			ResourceFile( Path, ARRAY_COUNT(Path), ResourceDir, *It, "png.novq" );
			if( IsDreamcastStarfield(*It) )
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
		if( !(Textures % 32) )
		{
			printf( "DCCOOK %s textures=%d\n", Package->GetName(), Textures );
			fflush( stdout );
		}
	}
	printf( "DCCOOK %s textures complete=%d\n", Package->GetName(), Textures );
	fflush( stdout );

	if( Import )
	{
		INT PackagePathLength = appStrlen(PackagePath);
		UBOOL CompactMapBsp = PackagePathLength >= 4
			&& !appStricmp(PackagePath + PackagePathLength - 4, ".unr");
		printf( "DCCOOK %s cooking meshes and BSP\n", Package->GetName() );
		fflush( stdout );

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
				if( !(Meshes % 16) )
				{
					printf( "DCCOOK %s meshes=%d\n", Package->GetName(), Meshes );
					fflush( stdout );
				}
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
		printf( "DCCOOK %s meshes complete=%d models=%d\n", Package->GetName(), Meshes, Models );
		fflush( stdout );

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
		// Bitmap.InternalTime is serialized even though PostLoad replaces it.
		// A process-clock value here made otherwise identical imports differ.
		for( TObjectIterator<UTexture> It; It; ++It )
		{
			if( It->IsIn( Package ) )
			{
				It->LastUpdateTime = 0.0;
			}
		}

		if( !Source->Heritage.Num() )
		{
			appErrorf( "Source package has no GUID: %s", PackagePath );
		}
		const FGuid SourceGuid = Source->Heritage(Source->Heritage.Num() - 1);
		printf( "DCCOOK saving %s meshes=%d models=%d\n", Package->GetName(), Meshes, Models );
		fflush( stdout );
		if( !GObj.SavePackage( Package, NULL, RF_Standalone, OutPath, 0, &SourceGuid ) )
		{
			appErrorf( "Cannot save cooked package %s", OutPath );
		}
	}

	printf( "RESOURCES OK import=%i sounds=%i textures=%i music=%i package=%s\n",
		Import, Sounds, Textures, Music, Package->GetName() );
	unguard;
}
#endif
