#pragma once

#include "Engine.h"
#include "Texture.h"
#include "Mesh.h"
#include "Sound.h"

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
	void ConvertMeshPkg( const FString& PkgPath, UPackage* Pkg, const FMeshReducer::FOptions& Options );
	void CommitChanges();

private:
	UEngine* Engine = nullptr;
	TMap<FString, UPackage*> LoadedPackages;
	TMap<FString, UPackage*> ChangedPackages;
	TMap<UPackage*, FGuid> PackageGuids;
	TMap<UPackage*, QWORD> PackageSizeBefore;
	TArray<UPalette*> UnrefPalettes;
	DWORD TotalPrevSize = 0;
	DWORD TotalNewSize = 0;
};
