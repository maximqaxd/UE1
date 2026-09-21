/*=============================================================================
	UnModel.h: Unreal UModel definition.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

/*-----------------------------------------------------------------------------
	UModel.
-----------------------------------------------------------------------------*/

//
// Model objects are used for brushes and for the level itself.
//
class ENGINE_API UModel : public UPrimitive
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT(UModel,UPrimitive,0)

	// References.
	UVectors*				Vectors;
	UVectors*				Points;
	UBspNodes*				Nodes;
	UBspSurfs*				Surfs;
	UVerts*					Verts;
	UPolys*					Polys;
	TArray<FLightMapIndex>	LightMap;
	TArray<BYTE>			LightBits;
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	// Port-specific lossless 4 KiB shadow-mask blocks. Empty offsets mean legacy bytes.
	TArray<INT>             LightBlockOffsets;
	FDCStreamSlice          LightStreamData;
	INT                     LightRawSize;
	DWORD                   LightDecodeCalls;
	DWORD                   LightDecodeBytes;
#endif
	TArray<FBox>			Bounds;
	TArray<INT>				LeafHulls;
	TArray<FLeaf>			Leaves;
	TArray<AActor*>			Lights;
	UBitArray*				LeafZone;
	UBitMatrix*				LeafLeaf;

	// Used by brush models, not level models.
	DWORD					MoverLink;
	UBOOL					RootOutside;
	UBOOL					Linked;

	// Constructors.
	UModel()
	: RootOutside(1)
	{}
	UModel( ABrush* Owner, UBOOL InRootOutside=1 );

	// UObject interface.
	void Serialize( FArchive& Ar );
	void Export( FOutputDevice& Out, const char* FileType, int Indent );

	// UPrimitive interface.
	UBOOL PointCheck
	(
		FCheckResult	&Result,
		AActor			*Owner,
		FVector			Location,
		FVector			Extent,
		DWORD           ExtraNodeFlags
	);
	UBOOL LineCheck
	(
		FCheckResult	&Result,
		AActor			*Owner,
		FVector			End,
		FVector			Start,
		FVector			Extent,
		DWORD           ExtraNodeFlags
	);
	FBox GetCollisionBoundingBox( const AActor *Owner ) const;
	FBox GetRenderBoundingBox( const AActor* Owner, UBOOL Exact ) const;

	// UModel interface.
	void AllocDatabases( UBOOL AllocPolys );
	void Modify();
	void BuildBound();
	void Transform( ABrush* Owner );
	void EmptyModel( INT EmptySurfInfo, INT EmptyPolys );
	void ShrinkModel();
#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
	void SerializeLightBits( FArchive& Ar );
#if defined(DC_RESOURCE_COOKER)
	void CompressLightBits();
	void CompactLightLists();
#endif
	void ReadLightBits( INT Offset, BYTE* Dest, INT Count );
	BYTE* ShadowBits( INT Offset, INT Count );
#endif
	UBOOL PotentiallyVisible( INT iLeaf1, INT iLeaf2 );

	// UModel collision functions.
	typedef void (*PLANE_FILTER_CALLBACK )(UModel *Model, INT iNode, int Param);
	typedef void (*SPHERE_FILTER_CALLBACK)(UModel *Model, INT iNode, int IsBack, int Outside, int Param);
	FPointRegion PointRegion( AZoneInfo* Zone, FVector Location ) const;
	FLOAT FindNearestVertex
	(
		const FVector	&SourcePoint,
		FVector			&DestPoint,
		FLOAT			MinRadius,
		INT				&pVertex
	) const;
	void PrecomputeSphereFilter
	(
		const FPlane	&Sphere
	);
	FLightMapIndex* GetLightMapIndex( INT iSurf )
	{
		guard(UModel::GetLightMapIndex);
		if( iSurf == INDEX_NONE ) return NULL;
		FBspSurf& Surf = Surfs->Element(iSurf);
		if( Surf.iLightMap==INDEX_NONE || !LightMap.Num() ) return NULL;
		return &LightMap(Surf.iLightMap);
		unguard;
	}
};

/*----------------------------------------------------------------------------
	The End.
----------------------------------------------------------------------------*/
