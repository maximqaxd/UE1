/*=============================================================================
	UnDCLoading.h: Dreamcast loading screen.

	Level travel on this platform restarts the whole engine, so during a load
	there is no viewport, no render device and no level for PaintProgress to
	draw through. This owns the framebuffer directly instead and is driven by
	the dependency stream, which knows its record count up front.
=============================================================================*/

#ifndef _INC_UNDCLOADING
#define _INC_UNDCLOADING

#if defined(PLATFORM_DREAMCAST)

// Show the loading screen and claim the display. Safe to call when the assets
// are missing: the screen is then simply not drawn.
CORE_API void appDCLoadingBegin();

// Fraction in [0,1]. Repaints are rate limited, so calling this per stream
// record is fine.
CORE_API void appDCLoadingProgress( FLOAT Fraction );

// Release the loading screen. The PVR takes the display back on its next
// presented frame.
CORE_API void appDCLoadingEnd();

#else

inline void appDCLoadingBegin() {}
inline void appDCLoadingProgress( FLOAT ) {}
inline void appDCLoadingEnd() {}

#endif

#endif
