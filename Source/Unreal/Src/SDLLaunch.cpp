#ifdef PLATFORM_SDL
#include "SDL2/SDL.h"
#endif

#ifdef PLATFORM_WIN32
#include <windows.h>
#endif

#ifdef PLATFORM_DREAMCAST
#include <kos.h>
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <kos/thread.h>
#define MAIN_STACK_SIZE (32 * 1024)  
#ifdef DREAMCAST_USE_FATFS
extern "C" {
#include <fatfs.h>
}
#endif
KOS_INIT_FLAGS( INIT_DEFAULT | INIT_CDROM | INIT_CONTROLLER | INIT_KEYBOARD | INIT_MOUSE | INIT_VMU | INIT_NET );
#endif

#include <stdio.h>

#include "Engine.h"
#if defined(PLATFORM_DREAMCAST)
#include "UnDCStream.h"
extern CORE_API void appDCCloseDat();
#endif

extern CORE_API FGlobalPlatform GTempPlatform;
extern DLL_IMPORT UBOOL GTickDue;
extern "C" {HINSTANCE hInstance;}
extern "C" {char GCC_HIDDEN THIS_PACKAGE[64]="Launch";}

// FExecHook.
class FExecHook : public FExec
{
	UBOOL Exec( const char* Cmd, FOutputDevice* Out )
	{
		return 0;
	}
};

FExecHook GLocalHook;
DLL_EXPORT FExec* GThisExecHook = &GLocalHook;

#ifdef PLATFORM_DREAMCAST
// fix thread stack underrun
static void init_thread_stack(void) {
    kthread_t *current = thd_get_current();
    if (current) {
        void *new_stack = malloc(MAIN_STACK_SIZE);
        if (new_stack) {
            current->stack = new_stack;
            current->stack_size = MAIN_STACK_SIZE;
            current->flags |= THD_OWNS_STACK;
        }
    }
}

//
// Report through the active serial/dcload device and lock up.
//
void FatalError( const char* Fmt, ... ) __attribute__((noreturn));
void FatalError( const char* Fmt, ... )
{
	char Msg[2048];

	va_list Args;
	va_start( Args, Fmt );
	vsnprintf( Msg, sizeof( Msg ), Fmt, Args );
	va_end( Args );

	printf( "%s\n\n", Msg );

	arch_stk_trace( 2 );

	while (true)
		thd_sleep( 100 );
}

//
// Handle assertion failure.
//
void HandleAssertFail( const char* File, int Line, const char* Expr, const char* Msg, const char* Func )
{
	FatalError( "ASSERTION FAILED:\nLoc: %s:%d (%s)\nExpr: %s\n%s", File, Line, Func, Expr, Msg);
}

void HandleIrqException( irq_t Code, irq_context_t* Context, void* Data )
{
	printf( "UNHANDLED EXCEPTION 0x%08x\n", Code );
	printf( "PC: %p PR: %p\n", (void*)Context->pc, (void*)Context->pr );
	printf( "SR: %p R0: %p\n", (void*)Context->sr, (void*)Context->r[0] );

	// The exception may have followed memory corruption, so the interrupted
	// frame pointer cannot be trusted. Walking it can replace the original
	// report with a double fault before the serial log is flushed.
	volatile INT Dummy = 1;
	while (Dummy);
}

#endif

//
// Handle an error.
//
void HandleError( const char* Exception )
{
	GIsGuarded=0;
	GIsCriticalError=1;
	debugf( NAME_Exit, "Shutting down after catching exception" );
	GObj.ShutdownAfterError();
	debugf( NAME_Exit, "Exiting due to exception" );
	GErrorHist[ARRAY_COUNT(GErrorHist)-1]=0;
#ifdef PLATFORM_SDL
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, LocalizeError("Critical"), GErrorHist, SDL_GetKeyboardFocus() );
#elif defined(PLATFORM_DREAMCAST)
	if( Exception )
		FatalError( "FATAL ERROR:\n%s\n\n%s", Exception, GErrorHist );
	else
		FatalError( "FATAL ERROR:\n%s", GErrorHist );
#endif
}

//
// Initialize.
//
UEngine* InitEngine( UBOOL InitializePlatform=1 )
{
	guard(InitEngine);

	// Platform init.
	if( InitializePlatform )
	{
		appInit();
	}
#if defined(PLATFORM_DREAMCAST)
	else
	{
		// RestartNativeCore has rebuilt objects, names and GSys. The process
		// platform/config/log state deliberately survives across sessions.
		GMem.Init( 32768 );
	}
#endif

#if defined(PLATFORM_DREAMCAST)
	// Exercise libc/libm through engine wrappers, with non-constant inputs.
	volatile DOUBLE Numerator = 17.0;
	volatile DOUBLE Denominator = 5.0;
	DOUBLE Remainder = appFmod( Numerator, Denominator );
	FLOAT Parsed = appAtof( "1.25" );
	if( Remainder != 2.0 || Parsed != 1.25f )
	{
		appErrorf( "Dreamcast floating-point ABI self-test failed" );
	}
#endif

#if defined(PLATFORM_DREAMCAST)
	// The initial DCS2 stream includes Entry and the destination map. Restarted
	// sessions use direct streams and load only their destination map, avoiding
	// Entry's world and dependencies at the next map's peak. Missing streams
	// retain the DCD1 compatibility path.
	char StartupURL[1024] = "";
	const char* PendingURL = appDCGetSessionTravelURL();
	if( PendingURL )
	{
		appStrncpy( StartupURL, PendingURL, ARRAY_COUNT(StartupURL) );
	}
	else
	{
		const char* Cmd = appCmdLine();
		if( !ParseToken(Cmd,StartupURL,ARRAY_COUNT(StartupURL),0) || StartupURL[0]=='-' )
			GetConfigString( "URL", "LocalMap", StartupURL, ARRAY_COUNT(StartupURL) );
	}
	for( char* Delimiter = StartupURL; *Delimiter; ++Delimiter )
	{
		if( *Delimiter=='?' || *Delimiter=='#' )
		{
			*Delimiter = 0;
			break;
		}
	}
	const char* MapName = StartupURL;
	for( const char* Cursor = StartupURL; *Cursor; ++Cursor )
		if( *Cursor=='/' || *Cursor=='\\' )
			MapName = Cursor + 1;
	char StreamMap[128];
	appStrncpy( StreamMap, MapName, ARRAY_COUNT(StreamMap) );
	if( char* Extension = appStrchr(StreamMap,'.') )
		*Extension = 0;
	char StreamPath[256];
	appSprintf( StreamPath, "../Maps/%s.dcs", StreamMap );
	if( StreamMap[0] && appFSize(StreamPath)>0 )
	{
		appDCStreamOpen( StreamPath );
		debugf( "DCSESSION startup_stream=%s", StreamPath );
	}
	else
	{
		debugf( "DCSESSION startup_stream_missing map=%s fallback=DCD1", StreamMap );
	}
#endif

	// Init subsystems.
#ifdef PLATFORM_LOW_MEMORY
	GDynMem.Init( 32768 );
	GSceneMem.Init( 32768 );
#else
	GDynMem.Init( 65536 );
	GSceneMem.Init( 32768 );
#endif

	// First-run menu.
	UBOOL FirstRun=0;
	GetConfigBool( "FirstRun", "FirstRun", FirstRun );

	// Create the global engine object.
	UClass* EngineClass;
	if( !GIsEditor )
	{
		// Create game engine.
		EngineClass = GObj.LoadClass( UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	}
	else if( ParseParam( appCmdLine(),"MAKE" ) )
	{
		// Create editor engine.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_DisallowFiles | LOAD_KeepImports, NULL );
	}
	else
	{
		// Editor.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	}

	// Init engine.
	UEngine* Engine = ConstructClassObject<UEngine>( EngineClass );
	Engine->Init();

#ifdef PLATFORM_DREAMCAST
	malloc_stats();
#endif

	return Engine;

	unguard;
}

//
// Unreal's main message loop.  All windows in Unreal receive messages
// somewhere below this function on the stack.
//
void MainLoop( UEngine* Engine )
{
	guard(MainLoop);

	GIsRunning = 1;
	DOUBLE OldTime = appSeconds();
	while( GIsRunning && !GIsRequestingExit )
	{
		// Update the world.
		DOUBLE NewTime = appSeconds();
		Engine->Tick( NewTime - OldTime );
		OldTime = NewTime;

		// Enforce optional maximum tick rate.
		INT MaxTickRate = Engine->GetMaxTickRate();
		if( MaxTickRate )
		{
			DOUBLE Delta = (1.0/MaxTickRate) - (appSeconds()-OldTime);
			if( Delta > 0.0 )
				appSleep( Delta );
		}
	}
	GIsRunning = 0;
	unguard;
}

//
// Exit the engine.
//
void ExitEngine( UEngine* Engine )
{
	guard(ExitEngine);

	GObj.Exit();
#if defined(PLATFORM_DREAMCAST)
	// Release physical DAT ownership while the allocator is still valid.
	// Outstanding texture slices must prevent a future session arena reset.
	appDCCloseDat();
	appDCStreamShutdown();
#endif
	GMem.Exit();
	GDynMem.Exit();
	GSceneMem.Exit();
	GCache.Exit(1);
	appDumpAllocs( &GTempPlatform );

	unguard;
}

#if defined(PLATFORM_DREAMCAST)
void RestartEngineSession()
{
	guard(RestartEngineSession);
	debugf( "DCSESSION teardown_begin" );
	DCProfileMemory( "session_before_teardown" );
	GObj.RestartNativeCore( 1 );
	GMem.Exit();
	GDynMem.Exit();
	GSceneMem.Exit();
	GCache.Exit( 1 );
	DCProfileMemory( "session_after_teardown" );
	debugf( "DCSESSION teardown_complete" );
	unguard;
}
#endif

#ifdef PLATFORM_WIN32
INT WINAPI WinMain( HINSTANCE hInInstance, HINSTANCE hPrevInstance, char* InCmdLine, INT nCmdShow )
#else
int main( int argc, const char** argv )
#endif
{
#ifdef PLATFORM_WIN32
	hInstance = hInInstance;
#else
	hInstance = NULL;
	// Remember arguments since we don't have GetCommandLine().
	appSetCmdLine( argc, argv );
#endif

#ifdef PLATFORM_DREAMCAST
	// fix thread stack underrun
	init_thread_stack();
	// Keep dcload logging when present; standalone boots use serial only.
	const char* DebugDevice = dbgio_dev_get();
	if( !DebugDevice || !appStrstr( DebugDevice, "dcl" ) )
	{
		if( dbgio_dev_select( "scif" ) < 0 )
		{
			dbgio_dev_select( "null" );
		}
	}
	assert_set_handler( HandleAssertFail );
	irq_set_handler( EXC_UNHANDLED_EXC, HandleIrqException, nullptr );
#ifdef DREAMCAST_USE_FATFS
	if( fs_fat_mount_sd() == 0 )
	{
		printf( "SD card found, will try to load data from there\n" );
	}
	else
	{
		// failed
		printf( "SD card not found, will default to CD\n" );
		sd_shutdown();
		fs_fat_shutdown();
	}
#endif
#endif

	GIsStarted = 1;

	// Set package name.
	appStrcpy( THIS_PACKAGE, appPackage() );

	// Init mode.
	GIsServer = 1;
	GIsClient = !ParseParam(appCmdLine(),"SERVER") && !ParseParam(appCmdLine(),"MAKE");
	GIsEditor = ParseParam(appCmdLine(),"EDITOR") || ParseParam(appCmdLine(),"MAKE");

	// Init windowing.
	printf( "base directory: %s\n", appBaseDir() );
	appChdir( appBaseDir() );

	// Init log.
	// TODO: GLog
	GExecHook = GThisExecHook;

	// Begin.
#ifndef _DEBUG
	try
	{
#endif
		// Start main loop.
		GIsGuarded=1;
		GSystem = &GTempPlatform;
#if defined(PLATFORM_DREAMCAST)
		UBOOL InitializePlatform = 1;
		while( !GIsRequestingExit )
		{
			UEngine* Engine = InitEngine( InitializePlatform );
			InitializePlatform = 0;
			if( !GIsRequestingExit )
				MainLoop( Engine );
			if( !GIsRequestingExit && appDCHasSessionTravel() )
			{
				RestartEngineSession();
				continue;
			}
			ExitEngine( Engine );
			break;
		}
#else
		UEngine* Engine = InitEngine();
		if( !GIsRequestingExit )
			MainLoop( Engine );
		ExitEngine( Engine );
#endif
		GIsGuarded=0;
#ifndef _DEBUG
	}
	catch( const char* Error )
	{
		// Fatal error.
		try { HandleError( Error ); } catch( ... ) { }
	}
	catch( ... )
	{
		// Crashed.
		try { HandleError( nullptr ); } catch( ... ) { }
	}
#endif

	// Shut down.
	GExecHook=NULL;
	appExit();
	GIsStarted = 0;
	return 0;
}
