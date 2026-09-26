#pragma once

#include "Engine.h"
#include "Texture.h"

#if defined(DC_RESOURCE_COOKER)
void CompactBspVerts( UModel* Model );
#endif

class FDCUtil
{
public:
	FDCUtil() { }
	void InitEngine();
	void Main();
	void HandleError( const char* Exception );
	void ExitEngine();

private:
	void LoadPackages( const char* Dir );
	void ParsePackageArg( const char* Arg, const char* Glob );
	void ConvertTexturePkg( const FString& PkgPath, UPackage* Pkg );
	void ConvertSoundPkg( const FString& PkgPath, UPackage* Pkg );
	void ConvertMusicPkg( const FString& PkgPath, UPackage* Pkg );
	void CommitChanges();
	void CookDat( const char* MapPath, const char* DatPath, UBOOL Verify );
	void AuditBsp( const char* MapPath, const char* OutPath );
	void AuditSplit( const char* MapPath, const char* OutPath );
	void FixSplitTravel( const char* MapPath, const char* OutPath );
	void TestSplitBsp( const char* MapPath, const char* OutPath, const char* Zones, const char* Role );
#if defined(DC_RESOURCE_COOKER)
	void ProcessResources( const char* PackagePath, const char* ResourceDir, const char* OutPath );
#endif

private:
	UEngine* Engine = nullptr;
	TMap<FString, UPackage*> LoadedPackages;
	TMap<FString, UPackage*> ChangedPackages;
	TMap<UPackage*, FGuid> PackageGuids;
	TArray<UPalette*> UnrefPalettes;
	DWORD TotalPrevSize = 0;
	DWORD TotalNewSize = 0;
};
