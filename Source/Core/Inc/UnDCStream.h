#pragma once

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
struct FDCStreamStore;
// Resolve legacy campaign names before both startup stream selection and Browse.
// Returns Map unchanged when no installed split replacement exists.
CORE_API const char* appDCResolveCampaignMap( const char* Map );

// A bounded, reference-counted blob location in the DAT that supplied it.
// Copies retain the store; unloading a level must not invalidate Entry textures.
class CORE_API FDCStreamSlice
{
public:
	FDCStreamSlice();
	FDCStreamSlice( const FDCStreamSlice& Other );
	FDCStreamSlice& operator=( const FDCStreamSlice& Other );
	~FDCStreamSlice();
	INT Size() const { return Length; }
	void Read( void* Destination ) const;
	void ReadRange( INT Offset, void* Destination, INT Count ) const;

private:
	FDCStreamStore* Store;
	DWORD Physical;
	INT Length;
	friend void appDCStreamCapture( const char*, INT, INT, FDCStreamSlice& );
};

CORE_API void appDCStreamCapture( const char* Filename, INT Offset, INT Length, FDCStreamSlice& Slice );
CORE_API UBOOL appDCReadDependencyFile( const char* Filename, INT Offset, void* Data, INT Length );
CORE_API UBOOL appDCCaptureDependencyFile( const char* Filename, INT Offset, INT Length, FDCStreamSlice& Slice );
// Checked dependency-order replay. This is not the indexed DCD1 reader.
CORE_API void appDCStreamOpen( const char* Path );
// Save restoration loads dependencies in object-reference order, not startup order.
CORE_API void appDCStreamUseIndexedReads();
CORE_API void appDCStreamFinish();
CORE_API void appDCStreamClose();
// Close() only unmounts; slices may retain resource handles across mounts.
// Shutdown() is for the session boundary, AFTER all slice owners are destroyed.
CORE_API UBOOL appDCStreamCanResetSession();
CORE_API void appDCStreamShutdown();
CORE_API UBOOL appDCStreamActive();
// DCS2 flag 1 records mip bodies at first use, not inside FMipmap serialization.
CORE_API UBOOL appDCStreamDeferredMips();
// DCS3 carries the exact linker indices touched by the recorded session.
// Export entries remain addressed by their original package index on disk,
// but only used entries receive resident FObjectExport slots.
CORE_API INT appDCStreamCompactExportCount( const char* Filename, INT OriginalCount );
CORE_API INT appDCStreamMapExport( const char* Filename, INT OriginalIndex );
CORE_API INT appDCStreamOriginalExport( const char* Filename, INT CompactIndex );
CORE_API UBOOL appDCStreamUsesImport( const char* Filename, INT OriginalCount, INT OriginalIndex );
#if defined(DC_RESOURCE_COOKER)
CORE_API extern UBOOL GDCStreamCookDeferredMips;
#endif
CORE_API INT appDCStreamFileSize( const char* Filename );
CORE_API UBOOL appDCStreamResolve( const char* Name, char* Out );
CORE_API void appDCStreamRead( const char* Filename, INT Offset, void* Data, INT Length );
#endif
