/*=============================================================================
    UnSpan.cpp: Unreal span buffering functions
    Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

#include "RenderPrivate.h"
#include "UnDCFrameProfile.h"

// Aggregate locally: no profiler function calls in the span-walking loops.
#if defined(PLATFORM_DREAMCAST)
struct FDCSpanWork
{
	DWORD Links, Fragments, Outputs, ScreenSplits;
    const UBOOL Enabled;
    FDCSpanWork()
		: Links(0), Fragments(0), Outputs(0), ScreenSplits(0),
          Enabled(DC_FRAME_PROFILE && GDCFrameProfileEnabled && GDCFrameProfileDetailed) {}

    ~FDCSpanWork()
    {
        if( Enabled )
        {
            DCFrameCount(DCFC_SpanLinks, Links);
            DCFrameCount(DCFC_SpanFragments, Fragments);
            DCFrameCount(DCFC_SpanOutputs, Outputs);
			DCFrameCount(DCFC_SpanScreenSplits, ScreenSplits);
        }
    }
};
#define DC_SPAN_VISIT() do { if( SpanWork.Enabled && ScreenSpan ) ++SpanWork.Links; } while(0)
#define DC_SPAN_FRAGMENT() do { if( SpanWork.Enabled ) ++SpanWork.Fragments; } while(0)
#define DC_SPAN_OUTPUT() do { if( SpanWork.Enabled ) ++SpanWork.Outputs; } while(0)
#define DC_SPAN_SPLIT() do { if( SpanWork.Enabled ) ++SpanWork.ScreenSplits; } while(0)
#else
#define DC_SPAN_VISIT() do {} while(0)
#define DC_SPAN_FRAGMENT() do {} while(0)
#define DC_SPAN_OUTPUT() do {} while(0)
#define DC_SPAN_SPLIT() do {} while(0)
#endif

#define UPDATE_PREVLINK(START,END)\
{\
	TopSpan         = New<FSpan>(*Mem,1,DC_SPAN_ALIGN);\
    *PrevLink       = TopSpan;\
    TopSpan->Start  = START;\
    TopSpan->End    = END;\
    PrevLink        = &TopSpan->Next;\
    ValidLines++;\
};

#define UPDATE_PREVLINK_ALLOC(START,END)\
{\
    DC_SPAN_FRAGMENT();\
    DC_SPAN_OUTPUT();\
    NewSpan         = New<FSpan>(*Mem,1,DC_SPAN_ALIGN);\
    *PrevLink       = NewSpan;\
    NewSpan->Start  = START;\
    NewSpan->End    = END;\
    PrevLink        = &(NewSpan->Next);\
    ValidLines++;\
};

/*-----------------------------------------------------------------------------
    Allocation.
-----------------------------------------------------------------------------*/

//
// Allocate a linear span buffer in temporary memory.  Allocates zero bytes
// for the list; must call spanAllocLinear to allocate the proper amount of memory
// for it.
//
void FSpanBuffer::AllocIndex( int AllocStartY, int AllocEndY, FMemStack* MemStack )
{
    guard(FSpanBuffer::AllocIndex);

    Mem         = MemStack;
    StartY      = AllocStartY;
    EndY        = AllocEndY;
    ValidLines  = 0;
#if defined(PLATFORM_DREAMCAST)
	UpdateSpanPool = NULL;
	UpdateSpanRemaining = 0;
#endif

    if( StartY <= EndY )
        Index = New<FSpan*>(*Mem,AllocEndY-AllocStartY);
    else
        Index = NULL;

	Mark = FMemMark(*MemStack);
    unguardf(("(%i-%i)", AllocStartY, AllocEndY));
}

//
// Allocate a linear span buffer and initialize it to represent
// the yet-undrawn region of a viewport.
//
void FSpanBuffer::AllocIndexForScreen( INT SXR, INT SYR, FMemStack* MemStack )
{
    guard(FSpanBuffer::AllocIndexForScreen);
#if defined(PLATFORM_DREAMCAST) && DC_COMPACT_SPANS
    if(SXR<0 || SXR>32767 || SYR<0 || SYR>32767)
        appErrorf("Compact span viewport out of range: %dx%d",SXR,SYR);
#endif
    int  i;

    Mem     = MemStack;
    StartY  = 0;
    EndY    = ValidLines = SYR;
#if defined(PLATFORM_DREAMCAST)
	UpdateSpanPool = NULL;
	UpdateSpanRemaining = 0;
#endif

    Index       = New<FSpan*>(*Mem,SYR,4);
    FSpan *List = New<FSpan>(*Mem,SYR,DC_SPAN_POOL_ALIGN);
    for( i=0; i<SYR; i++ )
    {
        Index[i]        = &List[i];
        List [i].Start  = 0;
        List [i].End    = SXR;
        List [i].Next   = NULL;
    }
    unguard;
}

//
// Free a linear span buffer in temporary rendering pool memory.
// Works whether actually saved or not.
//
void FSpanBuffer::Release()
{
    guard(FSpanBuffer::Release);
    Mark.Pop();
#if defined(PLATFORM_DREAMCAST)
	UpdateSpanPool = NULL;
	UpdateSpanRemaining = 0;
#endif
    unguard;
}

//
// Compute's a span buffer's valid range StartY-EndY range.
// Sets to 0,0 if the span is entirely empty.  You can also detect
// this condition by comparing ValidLines to 0.
//
void FSpanBuffer::GetValidRange( SWORD* ValidStartY, SWORD* ValidEndY )
{
    if( ValidLines )
    {
        FSpan **TempIndex;
        int NewStartY,NewEndY;

        NewStartY = StartY;
        TempIndex = &Index [0];
        while( *TempIndex==NULL )
		{
			TempIndex++;
			NewStartY++;
		}

        NewEndY   = EndY;
        TempIndex = &Index [EndY-StartY-1];
        while( *TempIndex==NULL )
		{
			TempIndex--;
			NewEndY--;
		}

        *ValidStartY = NewStartY;
        *ValidEndY   = NewEndY;
	}
    else *ValidStartY = *ValidEndY = 0;
}

/*-----------------------------------------------------------------------------
    Span occlusion.
-----------------------------------------------------------------------------*/

//
// See if a rectangle is visible.  Returns 1 if all or partially visible,
// 0 if totally occluded.
//
// Status: Performance critical.
//
INT FSpanBuffer::BoxIsVisible( INT X1, INT Y1, INT X2, INT Y2 )
{
    guard(FSpanBuffer::BoxIsVisible);

    FSpan **ScreenIndex, *Span;
    if( Y1 >= EndY )
    {
        return 0;
    }
    if( Y2 <= StartY )
    {
        return 0;
    }
    if (Y1 < StartY)    Y1 = StartY;
    if (Y2 > EndY)      Y2 = EndY;

    // Check box occlusion with span buffer.
    ScreenIndex = &Index [Y1-StartY];
    int Count   = Y2-Y1;

    // Start checking last line, then first and the rest.
    Span = *(ScreenIndex + Count - 1 );
    while( --Count >= 0 )
    {
        while( Span && X2>Span->Start )
		{
    		if( X1 < Span->End )
    		{
    			return 1;
    		}
    		Span = Span->Next;
        }
        Span = *ScreenIndex++;
    }
    return 0;
    unguard;
}

/*-----------------------------------------------------------------------------
    Span grabbing and updating.
-----------------------------------------------------------------------------*/

//
// Grind this polygon through the span buffer and:
// - See if the poly is totally occluded.
// - Update the span buffer by adding this poly to it.
// - Build a new, temporary span buffer for raster and span clipping the poly.
//
// Returns 1 if poly is all or partially visible, 0 if completely obscured.
// If 0 was returned, no screen span buffer memory was allocated and the resulting
// span index can be safely freed.
//
// Requires that StartY <= Raster.StartY, EndY >= Raster.EndY;
//
// If the destination FSpanBuffer and the screen's FSpanBuffer are using the same memory
// pool, the newly-allocated screen spans will be intermixed with the destination
// screen spans.  Freeing the destination in this case will overwrite the screen span buffer
// with garbage.
//
// Status: Extremely performance critical.
//
INT FSpanBuffer::CopyFromRasterUpdate( FSpanBuffer& Screen, INT RasterStartY, INT RasterEndY, FRasterSpan* Raster )
{
    guard(FSpanBuffer::CopyFromRasterUpdate);
	DC_FRAME_SCOPE_NAMED(SpanCopyScope, DCFS_SpanCopy);
#if defined(PLATFORM_DREAMCAST)
    DCFrameCount(DCFC_Spans, Max(0, RasterEndY - RasterStartY));
#endif
    DC_FRAME_SCOPE(DCFS_Span);

    FRasterSpan *Line;
    FSpan       **ScreenIndex,*NewScreenSpan,*NewSpan,*ScreenSpan,**PrevScreenLink;
    FSpan       **TempIndex,**PrevLink;
	int			i,OurStart,OurEnd,Accept=0;
#if defined(PLATFORM_DREAMCAST)
    FDCSpanWork SpanWork;
#endif

    if( StartY>RasterStartY || EndY<RasterEndY )
	{
		debugf( NAME_Warning, "Illegal span range <%i,%i> <%i,%i>", StartY, EndY, RasterStartY, RasterEndY );
		return 0;
       //appErrorf( "Illegal span range <%i,%i> <%i,%i>", StartY, EndY, RasterStartY, RasterEndY );
	}

    OurStart  = Max( RasterStartY, Screen.StartY );
    OurEnd    = Min( RasterEndY,   Screen.EndY   );
 	TempIndex = &Index[ 0 ];

    // Extra check for OurStart>OurEnd = screen and rasterpoly don't overlap, so all-null output.
    if( OurStart>=OurEnd )
    {
    	for( i=StartY; i<EndY; i++ )
			*(TempIndex++) = NULL;
        return 0;
    }

    for( i=StartY; i<OurStart; i++ )
		*(TempIndex++) = NULL;

    Line        = Raster + OurStart - RasterStartY;
    ScreenIndex = Screen.Index + OurStart - Screen.StartY;

	for( i=OurStart; i<OurEnd; i++ )
    {
        PrevScreenLink  = ScreenIndex;
        ScreenSpan      = *(ScreenIndex++);
        DC_SPAN_VISIT();
        PrevLink        = TempIndex++;

        // Skip if this screen span is already full, or if the raster is empty.
        if( (!ScreenSpan) || (Line->X[1] <= Line->X[0]) )
			goto NextLine;

        // Skip past all spans that occur before the raster.
        while( ScreenSpan->End <= Line->X[0] )
        {
            PrevScreenLink  = &(ScreenSpan->Next);
            ScreenSpan      = ScreenSpan->Next;
            DC_SPAN_VISIT();
            if( ScreenSpan == NULL )
				goto NextLine; // This line is full.
        }

        // ASSERT: ScreenSpan->End.X > Line->Start.X.

        // See if this span straddles the raster's starting point.
        if( ScreenSpan->Start < Line->X[0] )
        {
            // Add partial chunk to span buffer.
            Accept = 1;
            UPDATE_PREVLINK_ALLOC(Line->X[0],Min(Line->X[1], (INT)ScreenSpan->End));

            // See if span entirely encloses raster; if so, break span
            // up into two pieces and we're done.
            if( ScreenSpan->End > Line->X[1] )
            {
                // Get memory for the new span.  Note that this may be drawing from
                // the same memory pool as the destination.
                DC_SPAN_FRAGMENT();
                DC_SPAN_SPLIT();
                NewScreenSpan        = New<FSpan>(*Screen.Mem,1,DC_SPAN_ALIGN);
                NewScreenSpan->Start = Line->X[1];
                NewScreenSpan->End   = ScreenSpan->End;
                NewScreenSpan->Next  = ScreenSpan->Next;

                ScreenSpan->Next     = NewScreenSpan;
                ScreenSpan->End      = Line->X[0];

                Screen.ValidLines++;

                goto NextLine; // Done (everything is clean).
            }
            else
            {
                // Remove partial chunk from the span buffer.
                ScreenSpan->End = Line->X[0];

                PrevScreenLink  = &(ScreenSpan->Next);
                ScreenSpan      = ScreenSpan->Next;
                DC_SPAN_VISIT();
                if (ScreenSpan == NULL) goto NextLine; // Done (everything is clean).
            }
        }

        // ASSERT: Span->Start >= Line->Start.X
        // if (ScreenSpan->Start < Line->Start.X) appError ("Span2");

        // Process all screen spans that are entirely within the raster.
        while( ScreenSpan->End <= Line->X[1] )
        {
            // Add entire chunk to temporary span buffer.
            Accept = 1;
            UPDATE_PREVLINK_ALLOC(ScreenSpan->Start,ScreenSpan->End);

            // Delete this span from the span buffer.
            *PrevScreenLink = ScreenSpan->Next;
            ScreenSpan      = ScreenSpan->Next;
            DC_SPAN_VISIT();
            Screen.ValidLines--;
            if( ScreenSpan==NULL )
				goto NextLine; // Done (everything is clean).
        }

        // ASSERT: Span->End > Line->End.X
        // if (ScreenSpan->End <= Line->End.X) appError ("Span3");

        // If span overlaps raster's end point, process the partial chunk:
        if( ScreenSpan->Start < Line->X[1] )
        {
            // Add chunk from Span->Start to Line->End.X to temp span buffer.
            Accept = 1;
            UPDATE_PREVLINK_ALLOC(ScreenSpan->Start,Line->X[1]);

            // Shorten this span line by removing the raster.
            ScreenSpan->Start = Line->X[1];
        }
        NextLine:
        *PrevLink = NULL;
        Line ++;
    }
    for( i=OurEnd; i<EndY; i++ )
		*(TempIndex++) = NULL;

#if CHECK_ALL
    AssertGoodEnough("CopyFromRasterUpdate - Output spanbuffer");
    Screen.AssertValid("CopyFromRasterUpdate - Screen spanbuffer");
#endif
    return Accept;
    unguard;
}

#if defined(PLATFORM_DREAMCAST)
INT FSpanBuffer::TestRaster( INT RasterStartY, INT RasterEndY, FRasterSpan* Raster )
{
	DC_FRAME_SCOPE_NAMED(SpanTestScope, DCFS_SpanTest);
	DC_FRAME_SCOPE(DCFS_Span);
	FDCSpanWork SpanWork;
	DCFrameCount(DCFC_Spans, Max(0, RasterEndY - RasterStartY));
	const INT FirstY = Max(RasterStartY, StartY);
	const INT LastY = Min(RasterEndY, EndY);
	for( INT Y = FirstY; Y < LastY; ++Y )
	{
		const FRasterSpan& Line = Raster[Y - RasterStartY];
		if( Line.X[0] >= Line.X[1] )
			continue;
		FSpan* Span = Index[Y - StartY];
		while( Span && Span->End <= Line.X[0] )
		{
			if( SpanWork.Enabled ) ++SpanWork.Links;
			Span = Span->Next;
		}
		if( Span )
		{
			if( SpanWork.Enabled ) ++SpanWork.Links;
			if( Span->Start < Line.X[1] )
				return 1;
		}
	}
	return 0;
}

#if defined(PLATFORM_DREAMCAST)
#define DC_SPAN_KERNEL SHZ_NO_INLINE __attribute__((noclone))
#define DC_SPAN_COLD SHZ_NO_INLINE __attribute__((cold))
#else
#define DC_SPAN_KERNEL
#define DC_SPAN_COLD
#endif
static DC_SPAN_COLD void DCRefillUpdateSpans(FSpanBuffer& Buffer)
{
	Buffer.UpdateSpanPool = New<FSpan>(*Buffer.Mem, 32, DC_SPAN_POOL_ALIGN);
	Buffer.UpdateSpanRemaining = 32;
}

// Keep the row loop out of OccludeBsp's large register/stack working set.
static DC_SPAN_KERNEL INT DCUpdateSpanKernel(FSpanBuffer& Buffer, INT FirstY,
	INT LastY, INT RasterStartY, FRasterSpan* Raster, FDCSpanWork& SpanWork)
{
	if(FirstY>=LastY)return 0; // Do not form pointers outside empty intersections.
	FSpan** Row = Buffer.Index + (FirstY-Buffer.StartY);
	FRasterSpan* Line = Raster + (FirstY-RasterStartY);
	INT Remaining = LastY-FirstY;
	INT ValidDelta = 0;
	INT Visible = 0;
	for(; Remaining; --Remaining, ++Row, ++Line)
	{
		const INT RasterStart = Line->X[0];
		const INT RasterEnd = Line->X[1];
		if( RasterStart >= RasterEnd )
			continue;

		FSpan** PreviousLink = Row;
		FSpan* Span = *PreviousLink;
		while( Span && Span->End <= RasterStart )
		{
#if DC_FRAME_PROFILE
			if( SpanWork.Enabled ) ++SpanWork.Links;
#endif
			PreviousLink = &Span->Next;
			Span = Span->Next;
		}
		if( !Span )
			continue;
#if DC_FRAME_PROFILE
		if( SpanWork.Enabled ) ++SpanWork.Links;
#endif

		if( Span->Start < RasterStart )
		{
			Visible = 1;
			if( Span->End > RasterEnd )
			{
				// Split nodes survive until buffer release; allocate them in batches.
				if( !Buffer.UpdateSpanRemaining )DCRefillUpdateSpans(Buffer);
				FSpan* Right = Buffer.UpdateSpanPool++;
				--Buffer.UpdateSpanRemaining;
				Right->Start = RasterEnd;
				Right->End = Span->End;
				Right->Next = Span->Next;
				Span->Next = Right;
				Span->End = RasterStart;
				++ValidDelta;
#if DC_FRAME_PROFILE
				if( SpanWork.Enabled ) ++SpanWork.Fragments;
				DC_SPAN_SPLIT();
#endif
				continue;
			}
			Span->End = RasterStart;
			PreviousLink = &Span->Next;
			Span = Span->Next;
		}

		FSpan* FirstRemoved = Span;
		while( Span && Span->End <= RasterEnd )
		{
#if DC_FRAME_PROFILE
			if( SpanWork.Enabled ) ++SpanWork.Links;
#endif
			Visible = 1;
			Span = Span->Next;
			--ValidDelta;
		}
		// No observer runs inside this walk. Publish the surviving suffix once.
		if( Span != FirstRemoved )
			*PreviousLink = Span;
		if( Span && Span->Start < RasterEnd )
		{
#if DC_FRAME_PROFILE
			if( SpanWork.Enabled ) ++SpanWork.Links;
#endif
			Visible = 1;
			Span->Start = RasterEnd;
		}
	}
	Buffer.ValidLines += ValidDelta;
	return Visible;
}

INT FSpanBuffer::TestRasterUpdate( INT RasterStartY, INT RasterEndY, FRasterSpan* Raster )
{
	DC_FRAME_SCOPE_NAMED(SpanUpdateScope, DCFS_SpanUpdate);
	DC_FRAME_SCOPE(DCFS_Span);
	FDCSpanWork SpanWork;
	DCFrameCount(DCFC_Spans, Max(0, RasterEndY - RasterStartY));
	const INT FirstY = Max(RasterStartY, StartY);
	const INT LastY = Min(RasterEndY, EndY);
	return DCUpdateSpanKernel(*this, FirstY, LastY, RasterStartY, Raster, SpanWork);
}
#endif

//
// Grind this polygon through the span buffer and:
// - See if the poly is totally occluded
// - Build a new, temporary span buffer for raster and span clipping the poly
//
// Doesn't affect the span buffer no matter what.
// Returns 1 if poly is all or partially visible, 0 if completely obscured.
//
INT FSpanBuffer::CopyFromRaster( FSpanBuffer& Screen, INT RasterStartY, INT RasterEndY, FRasterSpan* Raster )
{
    guard(FSpanBuffer::CopyFromRaster);
	DC_FRAME_SCOPE_NAMED(SpanCopyScope, DCFS_SpanCopy);
#if defined(PLATFORM_DREAMCAST)
    DCFrameCount(DCFC_Spans, Max(0, RasterEndY - RasterStartY));
#endif
    DC_FRAME_SCOPE(DCFS_Span);

    FRasterSpan *Line;
    FSpan       **ScreenIndex,*ScreenSpan;
    FSpan       **TempIndex,**PrevLink,*NewSpan;
	int			i,OurStart,OurEnd,Accept=0;
#if defined(PLATFORM_DREAMCAST)
    FDCSpanWork SpanWork;
#endif

    OurStart = Max(RasterStartY,Screen.StartY);
    OurEnd   = Min(RasterEndY,Screen.EndY);

 	TempIndex = &Index [0];

    // Extra check for OurStart>OurEnd = screen and rasterpoly don't overlap, so all-null output.
    if( OurStart>=OurEnd )
    {
    	for( i=StartY; i<EndY; i++ )
			*(TempIndex++) = NULL;
        return 0;
    }

    for( i=StartY; i<OurStart; i++ )
		*(TempIndex++) = NULL;

    Line        = Raster + OurStart - RasterStartY;
    ScreenIndex = Screen.Index + OurStart - Screen.StartY;

    for( i=OurStart; i<OurEnd; i++ )
    {
        ScreenSpan      = *(ScreenIndex++);
        DC_SPAN_VISIT();
        PrevLink        = TempIndex++;

        if( !ScreenSpan || Line->X[1] <= Line->X[0] )
			// This span is already full, or raster is empty.
			goto NextLine; 

        // Skip past all spans that occur before the raster.
        while( ScreenSpan->End <= Line->X[0] )
        {
            ScreenSpan = ScreenSpan->Next;
            DC_SPAN_VISIT();
            if( !ScreenSpan )
				// This line is full.
				goto NextLine;
        }

        debug(ScreenSpan->End > Line->X[0]);

        // See if this span straddles the raster's starting point.
        if( ScreenSpan->Start < Line->X[0] )
        {
            Accept = 1;

            // Add partial chunk to temporary span buffer.
            UPDATE_PREVLINK_ALLOC(Line->X[0],Min(Line->X[1], (INT)ScreenSpan->End));
            ScreenSpan = ScreenSpan->Next;
            DC_SPAN_VISIT();
            if( !ScreenSpan )
				goto NextLine;
        }

        debug(ScreenSpan->Start >= Line->X[0]);

        // Process all spans that are entirely within the raster.
        while( ScreenSpan->End <= Line->X[1] )
        {
            Accept = 1;

            // Add entire chunk to temporary span buffer.
            UPDATE_PREVLINK_ALLOC(ScreenSpan->Start,ScreenSpan->End);
            ScreenSpan = ScreenSpan->Next;
            DC_SPAN_VISIT();
            if( !ScreenSpan )
				goto NextLine;
        }

        debug(ScreenSpan->End > Line->X[1]);

        // If span overlaps raster's end point, process the partial chunk.
        if( ScreenSpan->Start < Line->X[1] )
        {
            // Add chunk from Span->Start to Line->End.X to temp span buffer.
            Accept = 1;
            UPDATE_PREVLINK_ALLOC(ScreenSpan->Start,Line->X[1]);
        }
        NextLine:
        *PrevLink = NULL;
        Line++;
    }
    for( i=OurEnd; i<EndY; i++ )
		*(TempIndex++) = NULL;

#if CHECK_ALL
    AssertGoodEnough("CopyFromRaster");
#endif
    return Accept;
    unguard;
}

/*-----------------------------------------------------------------------------
    Merging.
-----------------------------------------------------------------------------*/

//
// Macro for copying a span.
//
#define COPY_SPAN(SRC_INDEX)\
{\
    PrevLink         = DestIndex++;\
    Span             = *(SRC_INDEX++);\
    while( Span )\
    {\
        UPDATE_PREVLINK(Span->Start,Span->End);\
        Span = Span->Next;\
    }\
    *PrevLink = NULL;\
}

//
// Merge this existing span buffer with another span buffer.  Overwrites the appropriate
// parts of this span buffer.  If this span buffer's index isn't large enough
// to hold everything, reallocates the index.
//
// This is meant to be called with this span buffer using GDynMem and the other span
// buffer using GMem.
//
// Status: This is currently unused and doesn't need to be optimized.
//
void FSpanBuffer::MergeWith( const FSpanBuffer& Other )
{
    guard(FSpanBuffer::MergeWith);
	DC_FRAME_SCOPE_NAMED(SpanMergeScope, DCFS_SpanMerge);
    DC_FRAME_SCOPE(DCFS_Span);

    // See if the existing span's index is large enough to hold the merged result.
    if( Other.StartY<StartY || Other.EndY>EndY )
    {
		// Must reallocate and copy index.
        int NewStartY = Min(StartY,Other.StartY);
        int NewEndY   = Max(EndY,  Other.EndY);
        int NewNum    = NewEndY - NewStartY;
        FSpan **NewIndex = New<FSpan*>(*Mem,NewNum);

        appMemset(&NewIndex[0                    ],0,    (StartY-NewStartY)*sizeof(FSpan *));
        appMemcpy(&NewIndex[StartY-NewStartY     ],Index,(EndY     -StartY)*sizeof(FSpan *));
        appMemset(&NewIndex[NewNum-(NewEndY-EndY)],0,    (NewEndY  -EndY  )*sizeof(FSpan *));

        StartY = NewStartY;
        EndY   = NewEndY;
        Index  = NewIndex;
    }

    // Now merge other span into this one.
    FSpan **ThisIndex  = &Index       [Other.StartY - StartY];
    FSpan **OtherIndex = &Other.Index [0];
    FSpan *ThisSpan,*OtherSpan,*TempSpan,**PrevLink;
    for( int i=Other.StartY; i<Other.EndY; i++ )
    {
        PrevLink    = ThisIndex;
        ThisSpan    = *(ThisIndex++);
        OtherSpan   = *(OtherIndex++);

        // Do everything relative to ThisSpan.
        while( ThisSpan && OtherSpan )
        {
            if( OtherSpan->End < ThisSpan->Start )
            {
				// Link OtherSpan in completely before ThisSpan.
                *PrevLink = TempSpan= New<FSpan>(*Mem,1,DC_SPAN_ALIGN);
                TempSpan->Start     = OtherSpan->Start;
                TempSpan->End       = OtherSpan->End;
                TempSpan->Next      = ThisSpan;
                PrevLink            = &TempSpan->Next;

                OtherSpan           = OtherSpan->Next;

                ValidLines++;
            }
            else if (OtherSpan->Start <= ThisSpan->End)
            {
				// Merge OtherSpan into ThisSpan.
                *PrevLink           = ThisSpan;
                ThisSpan->Start     = Min(ThisSpan->Start,OtherSpan->Start);
                ThisSpan->End       = Max(ThisSpan->End,  OtherSpan->End);
                TempSpan            = ThisSpan; // For maintaining End and Next.

                PrevLink            = &ThisSpan->Next;
                ThisSpan            = ThisSpan->Next;
                OtherSpan           = OtherSpan->Next;

                for(;;)
                {
                    if( ThisSpan&&(ThisSpan->Start <= TempSpan->End) )
                    {
                        TempSpan->End = Max(ThisSpan->End,TempSpan->End);
                        ThisSpan      = ThisSpan->Next;
                        ValidLines--;
                    }
                    else if( OtherSpan&&(OtherSpan->Start <= TempSpan->End) )
                    {
                        TempSpan->End = Max(TempSpan->End,OtherSpan->End);
                        OtherSpan     = OtherSpan->Next;
                    }
                    else break;
                }
            }
            else
            {
				// This span is entirely before the other span; keep it.
                *PrevLink           = ThisSpan;
                PrevLink            = &ThisSpan->Next;
                ThisSpan            = ThisSpan->Next;
            }
        }
        while( OtherSpan )
        {
			// Just append spans from OtherSpan.
            *PrevLink = TempSpan    = New<FSpan>(*Mem,1,DC_SPAN_ALIGN);
            TempSpan->Start         = OtherSpan->Start;
            TempSpan->End           = OtherSpan->End;
            PrevLink                = &TempSpan->Next;

            OtherSpan               = OtherSpan->Next;

            ValidLines++;
        }
        *PrevLink = ThisSpan;
    }

#if CHECK_ALL
    AssertGoodEnough("MergeWith");
#endif

    unguard;
}

/*-----------------------------------------------------------------------------
    Duplicating.
-----------------------------------------------------------------------------*/

//
// Copy the index from one span buffer to another span buffer.
//
// Status: Seldom called, no need to optimize.
//
void FSpanBuffer::CopyIndexFrom( const FSpanBuffer& Source, FMemStack* Mem )
{
    guard(FSpanBuffer::CopyIndexFrom);

    StartY   = Source.StartY;
    EndY     = Source.EndY;
 
    Index = New<FSpan*>(*Mem,Source.EndY-Source.StartY);
    appMemcpy( &Index[0], &Source.Index[0], (Source.EndY-Source.StartY) * sizeof(FSpan *) );

	unguard;
}

/*-----------------------------------------------------------------------------
    Debugging.
-----------------------------------------------------------------------------*/

//
// These debugging functions are available while writing span buffer code.
// They perform various checks to make sure that span buffers don't become
// corrupted.  They don't need optimizing, of course.
//

//
// Make sure that a span buffer is completely empty.
//
void FSpanBuffer::AssertEmpty( char* Name )
{
    guard(FSpanBuffer::AssertEmpty);
    FSpan **TempIndex,*Span;
    int i;

    TempIndex = Index;
    for( i=StartY; i<EndY; i++ )
    {
        Span = *(TempIndex++);
        while (Span!=NULL)
        {
            appErrorf("%s not empty, line=%i<%i>%i, start=%i, end=%i",Name,StartY,i,EndY,Span->Start,Span->End);
            Span=Span->Next;
        }
    }
    unguard;
}

//
// Assure that a span buffer isn't empty.
//
void FSpanBuffer::AssertNotEmpty( char* Name )
{
    guard(FSpanBuffer::AssertNotEmpty);
    FSpan **TempIndex,*Span;
    int i,NotEmpty=0;

    TempIndex = Index;
    for( i=StartY; i<EndY; i++ )
    {
        Span = *(TempIndex++);
        while (Span!=NULL)
        {
            if( Span->Start>=Span->End )
				appErrorf("%s contains %i-length span",Name,Span->End-Span->Start);
            NotEmpty=1;
            Span=Span->Next;
        }
    }
    if( !NotEmpty )
		appErrorf ("%s is empty",Name);
    unguard;
}

//
// Make sure that a span buffer is valid.  Performs the following checks:
// - Make sure there are no zero-length spans
// - Make sure there are no negative-length spans
// - Make sure there are no overlapping spans
// - Make sure all span pointers are valid (otherwise GPF's)
//
void FSpanBuffer::AssertValid( char* Name )
{
    guard(FSpanBuffer::AssertValid);

    FSpan **TempIndex,*Span;
    int i,PrevEnd,c=0;

    TempIndex = Index;
    for( i=StartY; i<EndY; i++ )
    {
        PrevEnd = -1000;
        Span = *(TempIndex++);
        while( Span )
        {
            if ((i==StartY)||(i==(EndY-1)))
            {
                if ((PrevEnd!=-1000) && (PrevEnd >= Span->Start)) appErrorf("%s contains %i-length overlap, line %i/%i",Name,PrevEnd-Span->Start,i-StartY,EndY-StartY);
                if (Span->Start>=Span->End) appErrorf("%s contains %i-length span, line %i/%i",Name,Span->End-Span->Start,i-StartY,EndY-StartY);
                PrevEnd = Span->End;
            }
            Span=Span->Next;
            c++;
        }
    }
    if( c!=ValidLines )
		appErrorf ("%s bad ValidLines: claimed=%i, correct=%i",Name,ValidLines,c);
    unguard;
}

//
// Like AssertValid, but 'ValidLines' checked for zero/nonzero only.
//
void FSpanBuffer::AssertGoodEnough( char* Name )
{
    guard(FSpanBuffer::AssertGoodEnough);
    FSpan **TempIndex,*Span;
    int i,PrevEnd,c=0;

    TempIndex = Index;
    for( i=StartY; i<EndY; i++ )
    {
        PrevEnd = -1000;
        Span = *(TempIndex++);
        while (Span)
        {
            if( (i==StartY)||(i==(EndY-1)) )
            {
                if ((PrevEnd!=-1000) && (PrevEnd >= Span->Start)) appErrorf("%s contains %i-length overlap, line %i/%i",Name,PrevEnd-Span->Start,i-StartY,EndY-StartY);
                if (Span->Start>=Span->End) appErrorf("%s contains %i-length span, line %i/%i",Name,Span->End-Span->Start,i-StartY,EndY-StartY);
                PrevEnd = Span->End;
            }
            Span=Span->Next;
            c++;
        }
    }
    if( (c==0) != (ValidLines==0) )
		appErrorf ("%s bad ValidLines: claimed=%i, correct=%i",Name,ValidLines,c);
    unguard;
}

/*-----------------------------------------------------------------------------
    The End.
-----------------------------------------------------------------------------*/
