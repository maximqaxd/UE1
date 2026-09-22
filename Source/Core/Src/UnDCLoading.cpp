/*=============================================================================
	UnDCLoading.cpp: Dreamcast loading screen.
=============================================================================*/

#include "CorePrivate.h"
#include "UnDCLoading.h"

#if defined(PLATFORM_DREAMCAST)

#include <kos.h>
#include <dc/video.h>

#define DCB_MAGIC 0x31424344	/* 'DCB1' */

// Assets are cooked by Source/DCUtil/cook_loadscreen.py and sit beside the
// configuration, so appBaseDir resolves them directly.
#define DCB_BACKGROUND "loadbg.dcb"
#define DCB_BAR        "loadbar.dcb"

// The bar has to survive every repaint, so it stays resident for the duration
// of the load. At 256x36 that is 18K. The background is streamed straight to
// the framebuffer and never held.
#define DC_LOADING_MAX_BAR_BYTES (64 * 1024)

// The screen is only worth repainting at roughly display rate; the dependency
// stream calls in thousands of times.
#define DC_LOADING_REPAINT_MS 50

struct FDCLoadingBar
{
	INT      Width;
	INT      Height;		// height of one row; the file holds two
	INT      OriginX;
	INT      OriginY;
	_WORD*   Pixels;
};

static UBOOL         GDCLoadingActive = 0;
static FDCLoadingBar GDCLoadingBar = { 0, 0, 0, 0, NULL };
static INT           GDCLoadingLastFill = -1;
static DWORD         GDCLoadingLastPaint = 0;

static void DCLoadingPath( char* Out, INT Capacity, const char* Name )
{
	snprintf( Out, Capacity, "%s%s", appBaseDir(), Name );
}

//
// Read a .dcb header, leaving the handle on the pixel data.
//
static UBOOL DCLoadingReadHeader( FILE* File, INT& Width, INT& Height, INT& Scale )
{
	DWORD Header[4];
	if( appFread( Header, 1, sizeof(Header), File ) != sizeof(Header)
		|| Header[0] != DCB_MAGIC )
	{
		return 0;
	}
	Width  = (INT)Header[1];
	Height = (INT)Header[2];
	Scale  = (INT)Header[3];
	return Width > 0 && Height > 0 && (Scale == 1 || Scale == 2);
}

//
// Stream the background into the framebuffer one source row at a time, so a
// 640x480 frame never needs a buffer of its own. At scale 2 each source row is
// expanded horizontally and emitted twice.
//
static void DCLoadingDrawBackground()
{
	char Path[256];
	DCLoadingPath( Path, ARRAY_COUNT(Path), DCB_BACKGROUND );
	FILE* File = appFopen( Path, "rb" );
	if( !File )
	{
		return;
	}

	INT Width = 0, Height = 0, Scale = 1;
	if( !DCLoadingReadHeader( File, Width, Height, Scale ) )
	{
		appFclose( File );
		return;
	}

	const INT ScreenWidth  = (INT)vid_mode->width;
	const INT ScreenHeight = (INT)vid_mode->height;
	_WORD* Row = (_WORD*)appMalloc( Width * sizeof(_WORD), "DCLoadingRow" );
	if( !Row )
	{
		appFclose( File );
		return;
	}

	const INT DrawWidth  = Min( Width * Scale, ScreenWidth );
	const INT DrawHeight = Min( Height * Scale, ScreenHeight );
	const INT OffsetX    = (ScreenWidth - DrawWidth) / 2;
	const INT OffsetY    = (ScreenHeight - DrawHeight) / 2;

	for( INT y = 0; y < Height; ++y )
	{
		if( appFread( Row, 1, Width * sizeof(_WORD), File ) != Width * (INT)sizeof(_WORD) )
		{
			break;
		}
		for( INT Repeat = 0; Repeat < Scale; ++Repeat )
		{
			const INT ScreenY = OffsetY + y * Scale + Repeat;
			if( ScreenY < 0 || ScreenY >= ScreenHeight )
			{
				continue;
			}
			_WORD* Dest = vram_s + ScreenY * ScreenWidth + OffsetX;
			if( Scale == 1 )
			{
				appMemcpy( Dest, Row, DrawWidth * sizeof(_WORD) );
			}
			else
			{
				for( INT x = 0; x * 2 + 1 < DrawWidth; ++x )
				{
					Dest[x * 2 + 0] = Row[x];
					Dest[x * 2 + 1] = Row[x];
				}
			}
		}
	}

	appFree( Row );
	appFclose( File );
}

static UBOOL DCLoadingLoadBar()
{
	char Path[256];
	DCLoadingPath( Path, ARRAY_COUNT(Path), DCB_BAR );
	FILE* File = appFopen( Path, "rb" );
	if( !File )
	{
		return 0;
	}

	INT Width = 0, Height = 0, Scale = 1;
	if( !DCLoadingReadHeader( File, Width, Height, Scale ) || (Height & 1) )
	{
		appFclose( File );
		return 0;
	}

	const INT Bytes = Width * Height * (INT)sizeof(_WORD);
	if( Bytes <= 0 || Bytes > DC_LOADING_MAX_BAR_BYTES )
	{
		appFclose( File );
		return 0;
	}

	GDCLoadingBar.Pixels = (_WORD*)appMalloc( Bytes, "DCLoadingBar" );
	if( !GDCLoadingBar.Pixels )
	{
		appFclose( File );
		return 0;
	}
	if( appFread( GDCLoadingBar.Pixels, 1, Bytes, File ) != Bytes )
	{
		appFree( GDCLoadingBar.Pixels );
		GDCLoadingBar.Pixels = NULL;
		appFclose( File );
		return 0;
	}
	appFclose( File );

	// The file stacks the empty well over the filled state.
	GDCLoadingBar.Width   = Width;
	GDCLoadingBar.Height  = Height / 2;
	GDCLoadingBar.OriginX = ((INT)vid_mode->width - Width) / 2;
	GDCLoadingBar.OriginY = (INT)vid_mode->height - 62;
	return 1;
}

//
// Blit the filled row up to the cursor and the empty row after it.
//
static void DCLoadingDrawBar( INT Fill )
{
	if( !GDCLoadingBar.Pixels )
	{
		return;
	}

	const INT ScreenWidth  = (INT)vid_mode->width;
	const INT ScreenHeight = (INT)vid_mode->height;
	const INT BarWidth     = GDCLoadingBar.Width;
	const INT BarHeight    = GDCLoadingBar.Height;

	for( INT y = 0; y < BarHeight; ++y )
	{
		const INT ScreenY = GDCLoadingBar.OriginY + y;
		if( ScreenY < 0 || ScreenY >= ScreenHeight )
		{
			continue;
		}
		const _WORD* Empty  = GDCLoadingBar.Pixels + y * BarWidth;
		const _WORD* Filled = GDCLoadingBar.Pixels + (BarHeight + y) * BarWidth;
		_WORD* Dest = vram_s + ScreenY * ScreenWidth + GDCLoadingBar.OriginX;
		for( INT x = 0; x < BarWidth; ++x )
		{
			const INT ScreenX = GDCLoadingBar.OriginX + x;
			if( ScreenX < 0 || ScreenX >= ScreenWidth )
			{
				continue;
			}
			Dest[x] = ( x < Fill ) ? Filled[x] : Empty[x];
		}
	}
}

void appDCLoadingBegin()
{
	if( GDCLoadingActive )
	{
		return;
	}
	if( !vid_mode || vid_mode->pm != PM_RGB565 )
	{
		// Every other path assumes 16bpp; drawing into a different packing
		// would be worse than leaving the screen alone.
		return;
	}
	if( !DCLoadingLoadBar() )
	{
		return;
	}

	// PVRDrv keeps the PVR alive across travel, so on anything but the first
	// load it still owns the display. Point the scanout back at framebuffer
	// zero; the PVR reclaims it on its next presented frame.
	vid_set_start( vid_get_start(0) );

	DCLoadingDrawBackground();
	GDCLoadingActive = 1;
	GDCLoadingLastFill = -1;
	GDCLoadingLastPaint = 0;
	appDCLoadingProgress( 0.f );
}

void appDCLoadingProgress( FLOAT Fraction )
{
	if( !GDCLoadingActive )
	{
		return;
	}

	Fraction = Clamp( Fraction, 0.f, 1.f );
	const INT Fill = (INT)( Fraction * GDCLoadingBar.Width + 0.5f );
	if( Fill == GDCLoadingLastFill )
	{
		return;
	}

	// Rate limit repaints: the stream reports thousands of records and each
	// blit is otherwise pure overhead against the load itself.
	const DWORD Now = timer_ms_gettime64();
	if( GDCLoadingLastFill >= 0 && Fill < GDCLoadingBar.Width
		&& Now - GDCLoadingLastPaint < DC_LOADING_REPAINT_MS )
	{
		return;
	}

	GDCLoadingLastFill = Fill;
	GDCLoadingLastPaint = Now;
	DCLoadingDrawBar( Fill );
}

void appDCLoadingEnd()
{
	if( !GDCLoadingActive )
	{
		return;
	}
	GDCLoadingActive = 0;
	GDCLoadingLastFill = -1;
	if( GDCLoadingBar.Pixels )
	{
		appFree( GDCLoadingBar.Pixels );
		GDCLoadingBar.Pixels = NULL;
	}
}

#endif
