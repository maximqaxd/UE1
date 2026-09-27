/*=============================================================================
	UnSpan.h: Span buffering functions and structures
	Copyright 1995 Epic MegaGames, Inc.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

/*------------------------------------------------------------------------------------
	General span buffer related classes.
------------------------------------------------------------------------------------*/

//
// A span buffer linked-list entry representing a free (undrawn) 
// portion of a scanline. 
//
#if defined(PLATFORM_DREAMCAST) && DC_COMPACT_SPANS
#include <sh4zam/shz_cdefs.h>
#define DC_SPAN_ALIGN 8
#define DC_SPAN_POOL_ALIGN 32
class SHZ_ALIGNAS(8) FSpan
#else
#define DC_SPAN_ALIGN 4
#define DC_SPAN_POOL_ALIGN 4
class FSpan
#endif
{
public:
	// Variables.
#if defined(PLATFORM_DREAMCAST) && DC_COMPACT_SPANS
	SWORD Start, End;
#else
	INT Start, End;
#endif
	FSpan* Next;

	// Constructors.
	FSpan()
	{}
	FSpan( INT InStart, INT InEnd )
	:	Start		(InStart)
	,	End			(InEnd)
	{}
};
#if defined(PLATFORM_DREAMCAST) && DC_COMPACT_SPANS
static_assert(sizeof(FSpan)==8,"Compact spans must fit four per cache line");
#endif

//
// A raster span.
//
struct FRasterSpan
{
	INT X[2];
};

//
// A raster polygon.
//
class FRasterPoly
{
public:
	INT	StartY;
	INT EndY;
	FRasterSpan Lines[];
};

//
// A span buffer, which represents all free (undrawn) scanlines on
// the screen.
//
class RENDER_API FSpanBuffer
{
public:
	INT			StartY;		// Starting Y value.
	INT			EndY;		// Last Y value + 1.
	INT			ValidLines;	// Number of lines at beginning (for screen).
	FSpan**		Index;		// Contains (EndY-StartY) units pointing to first span or NULL.
	FMemStack*	Mem;		// Memory pool everything is stored in.
	FMemMark	Mark;		// Top of memory pool marker.
#if defined(PLATFORM_DREAMCAST)
	FSpan*		UpdateSpanPool;
	INT			UpdateSpanRemaining;
#endif

	// Constructors.
	FSpanBuffer()
	#if defined(PLATFORM_DREAMCAST)
	: UpdateSpanPool(NULL), UpdateSpanRemaining(0)
	#endif
	{}
	FSpanBuffer( const FSpanBuffer& Source, FMemStack& InMem )
	:	StartY		(Source.StartY)
	,	EndY		(Source.EndY)
	,	ValidLines	(Source.ValidLines)
	,	Index		(new(InMem,EndY-StartY)FSpan*)
	,	Mem			(&InMem)
	,	Mark		(InMem)
	#if defined(PLATFORM_DREAMCAST)
	,	UpdateSpanPool(NULL)
	,	UpdateSpanRemaining(0)
	#endif
	{
		for( int i=0; i<EndY-StartY; i++ )
		{
			FSpan** PrevLink = &Index[i];
			for( FSpan* Other=Source.Index[i]; Other; Other=Other->Next )
			{
				*PrevLink = new( *Mem, 1, DC_SPAN_ALIGN )FSpan( Other->Start, Other->End );
				PrevLink  = &(*PrevLink)->Next;
			}
			*PrevLink = NULL;
		}
	}

	// Allocation.
	void AllocIndex( INT AllocStartY, INT AllocEndY, FMemStack* Mem );
	void AllocIndexForScreen( INT SXR, INT SYR, FMemStack* Mem );
	void Release();
	void GetValidRange( SWORD* ValidStartY, SWORD* ValidEndY );

	// Merge/copy/alter operations.
	void CopyIndexFrom( const FSpanBuffer& Source, FMemStack* Mem );
	void MergeWith( const FSpanBuffer& Other );

	// Grabbing and updating from rasterizations.
	INT CopyFromRaster( FSpanBuffer& ScreenSpanBuffer, INT RasterStartY, INT RasterEndY, FRasterSpan* Raster );
	INT CopyFromRasterUpdate( FSpanBuffer& ScreenSpanBuffer, INT RasterStartY, INT RasterEndY, FRasterSpan* Raster );
#if defined(PLATFORM_DREAMCAST)
	// Hardware BSP rendering does not consume the visible fragment list for
	// ordinary polygons. These variants preserve visibility and screen updates
	// without allocating a throwaway output span buffer.
	INT TestRaster( INT RasterStartY, INT RasterEndY, FRasterSpan* Raster );
	INT TestRasterUpdate( INT RasterStartY, INT RasterEndY, FRasterSpan* Raster );
#endif

	// Occlusion.
	INT BoxIsVisible( INT X1, INT Y1, INT X2, INT Y2 );

	// Debugging.
	void AssertEmpty( char* Name );
	void AssertNotEmpty( char* Name );
	void AssertValid( char* Name );
	void AssertGoodEnough( char* Name );
};


/*------------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------------*/
