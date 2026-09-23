/*=============================================================================
	UnDCVRAM.cpp: Dreamcast VRAM side-heap for cold CPU data.

	The Dreamcast has 8MB of video RAM and this build uses roughly 3.4MB of it:
	2,714,624 for the framebuffer and TA command buffers, plus a measured
	721,920 texture high-water (pvr_mem_stats, base a4296c00). The remaining
	~4.9MB sits idle while main RAM is the binding constraint, so data that is
	resident but cold can live there instead.

	VRAM is uncached and shares a bus with the PVR, so a read costs far more
	than a cached main-RAM read. Only data that is NOT touched per frame
	belongs here.

	Layout. KOS grows its texture heap upward from pvr_mem_base and
	pvr_int_sbrk refuses to pass PVR_RAM_INT_TOP, so the reserve is taken off
	the top. PVR would have to allocate several times its measured peak before
	reaching it. That is a margin, not a guarantee, so a sentinel at the base
	of the region is checked on every allocation: if the PVR ever does grow
	that far the result is a loud error rather than silent corruption.

	Window. Allocations are in the 64-bit window (PVR_RAM_INT_BASE), the same
	one pvr_mem_sbrk uses, so byte offsets are directly comparable and the
	collision argument above is actually sound. The 32-bit window at
	PVR_RAM_BASE addresses the same 8MB with a different bank interleave, so
	an offset there is a different physical byte -- do not mix the two. These
	pointers are CPU-only and must never be handed to a pvr_* call.
=============================================================================*/

#include "CorePrivate.h"

#if defined(PLATFORM_DREAMCAST)

#include <dc/pvr.h>
#include <string.h>

/*-----------------------------------------------------------------------------
	Region.
-----------------------------------------------------------------------------*/

// Keep the PVR roughly five times its measured texture high-water.
#define DCVRAM_RESERVE      ( 2 * 1024 * 1024 )
#define DCVRAM_ALIGN        32
#define DCVRAM_SENTINEL     0xD0C0FFEEu

// Payload alignment is 32 bytes, so the header is padded to match: every
// returned pointer is then 32-aligned like appMalloc's.
struct FDCVRAMBlock
{
	DWORD          Size;      // payload bytes, already aligned
	DWORD          Free;
	FDCVRAMBlock*  Next;
	FDCVRAMBlock*  Prev;
	DWORD          Pad[4];
};

static BYTE*         GVRAMBase     = NULL;   // first byte of the reserve
static FDCVRAMBlock* GVRAMHead     = NULL;
static DWORD         GVRAMBytes    = 0;      // payload bytes currently handed out
static DWORD         GVRAMPeak     = 0;
static INT           GVRAMBlocks   = 0;
static UBOOL         GVRAMFailed   = 0;      // sticky: fall back to main RAM

// Scope depth: while nonzero, appMalloc prefers VRAM.
INT GDCVRAMPreferDepth = 0;

static inline DWORD DCVRAMAlign( DWORD n )
{
	return ( n + (DCVRAM_ALIGN-1) ) & ~(DWORD)(DCVRAM_ALIGN-1);
}

//
// Lay the reserve out as one free block. Lazy so there is no startup ordering
// to get wrong: nothing touches the top of VRAM before the first call.
//
static void DCVRAMInit()
{
	const DWORD Top     = (DWORD)PVR_RAM_INT_BASE + (DWORD)PVR_RAM_SIZE;
	const DWORD Reserve = DCVRAM_RESERVE;

	GVRAMBase = (BYTE*)( Top - Reserve );

	// Sentinel sits at the very base, below the first block header, and is the
	// first thing the PVR would overwrite if its heap ever grew this far.
	*(volatile DWORD*)GVRAMBase = DCVRAM_SENTINEL;

	GVRAMHead        = (FDCVRAMBlock*)( GVRAMBase + DCVRAM_ALIGN );
	GVRAMHead->Size  = Reserve - DCVRAM_ALIGN - sizeof(FDCVRAMBlock);
	GVRAMHead->Free  = 1;
	GVRAMHead->Next  = NULL;
	GVRAMHead->Prev  = NULL;

	debugf( "DCVRAM init base=%08x size=%u pvr_top=%08x",
		(DWORD)GVRAMBase, Reserve, Top );
}

//
// The PVR heap grows toward us. If it ever arrives, say so rather than
// corrupting whatever is living here.
//
static UBOOL DCVRAMSentinelOk()
{
	return *(volatile DWORD*)GVRAMBase == DCVRAM_SENTINEL;
}

/*-----------------------------------------------------------------------------
	Allocator. First fit with coalescing; the working set is a few dozen live
	blocks (three per open package), so nothing cleverer earns its keep.
-----------------------------------------------------------------------------*/

CORE_API UBOOL appDCVRAMOwns( void* Ptr )
{
	if( !GVRAMBase || !Ptr )
		return 0;
	return (BYTE*)Ptr >= GVRAMBase
		&& (BYTE*)Ptr <  GVRAMBase + DCVRAM_RESERVE;
}

CORE_API void* appDCVRAMMalloc( INT Size, const char* Tag )
{
	guard(appDCVRAMMalloc);
	if( Size <= 0 || GVRAMFailed )
		return NULL;

	if( !GVRAMBase )
		DCVRAMInit();

	if( !DCVRAMSentinelOk() )
	{
		// Loud, once, then stop using VRAM for the rest of the run.
		GVRAMFailed = 1;
		debugf( NAME_Warning, "DCVRAM sentinel clobbered -- PVR heap reached %08x;"
			" falling back to main RAM", (DWORD)GVRAMBase );
		return NULL;
	}

	const DWORD Want = DCVRAMAlign( (DWORD)Size );
	for( FDCVRAMBlock* B = GVRAMHead; B; B = B->Next )
	{
		if( !B->Free || B->Size < Want )
			continue;

		// Split when the remainder can still hold a header plus a useful
		// payload; otherwise hand over the whole block.
		if( B->Size >= Want + sizeof(FDCVRAMBlock) + DCVRAM_ALIGN )
		{
			FDCVRAMBlock* Rest = (FDCVRAMBlock*)( (BYTE*)(B+1) + Want );
			Rest->Size = B->Size - Want - sizeof(FDCVRAMBlock);
			Rest->Free = 1;
			Rest->Next = B->Next;
			Rest->Prev = B;
			if( Rest->Next )
				Rest->Next->Prev = Rest;
			B->Next = Rest;
			B->Size = Want;
		}
		B->Free = 0;
		GVRAMBytes += B->Size;
		GVRAMPeak   = Max( GVRAMPeak, GVRAMBytes );
		++GVRAMBlocks;
		return (void*)( B + 1 );
	}
	return NULL;   // caller falls back to main RAM
	unguard;
}

CORE_API void appDCVRAMFree( void* Ptr )
{
	guard(appDCVRAMFree);
	if( !appDCVRAMOwns( Ptr ) )
		return;

	FDCVRAMBlock* B = ((FDCVRAMBlock*)Ptr) - 1;
	if( B->Free )
		return;                      // double free; ignore rather than corrupt
	B->Free = 1;
	GVRAMBytes -= B->Size;
	--GVRAMBlocks;

	// Coalesce forward then backward.
	if( B->Next && B->Next->Free )
	{
		FDCVRAMBlock* N = B->Next;
		B->Size += sizeof(FDCVRAMBlock) + N->Size;
		B->Next  = N->Next;
		if( B->Next )
			B->Next->Prev = B;
	}
	if( B->Prev && B->Prev->Free )
	{
		FDCVRAMBlock* P = B->Prev;
		P->Size += sizeof(FDCVRAMBlock) + B->Size;
		P->Next  = B->Next;
		if( P->Next )
			P->Next->Prev = P;
	}
	unguard;
}

//
// Size of the payload behind a VRAM pointer, so appRealloc knows how much to
// copy when it has to move an allocation out to main RAM.
//
CORE_API INT appDCVRAMSize( void* Ptr )
{
	if( !appDCVRAMOwns( Ptr ) )
		return 0;
	return (INT)( (((FDCVRAMBlock*)Ptr) - 1)->Size );
}

CORE_API void appDCVRAMStats( DWORD& OutUsed, DWORD& OutPeak, INT& OutBlocks )
{
	OutUsed   = GVRAMBytes;
	OutPeak   = GVRAMPeak;
	OutBlocks = GVRAMBlocks;
}

#endif // PLATFORM_DREAMCAST

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
