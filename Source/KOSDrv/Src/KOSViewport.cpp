#include <string.h>
#include <ctype.h>
#include <kos.h>
#include "GL/glkos.h"

#include "KOSDrv.h"
#include "UnRender.h"

IMPLEMENT_CLASS( UKOSViewport );

/*-----------------------------------------------------------------------------
	UKOSViewport implementation.
-----------------------------------------------------------------------------*/

enum EDreamcastJoyAction
{
	DCJA_MoveForward,
	DCJA_MoveBackward,
	DCJA_StrafeLeft,
	DCJA_StrafeRight,
	DCJA_Jump,
	DCJA_Duck,
	DCJA_ShowMenu,
	DCJA_InventoryPrevious,
	DCJA_InventoryNext,
	DCJA_AltFire,
	DCJA_Fire,
	DCJA_Translator,
	DCJA_InventoryActivate,
	DCJA_PreviousWeapon,
	DCJA_NextWeapon,
	DCJA_UIAccept,
	DCJA_UICancel,
	DCJA_UIUp,
	DCJA_UIDown,
	DCJA_UILeft,
	DCJA_UIRight,
};

const BYTE UKOSViewport::JoyActionMap[MAX_JOY_ACTIONS] =
{
	IK_Joy1,
	IK_Joy2,
	IK_Joy3,
	IK_Joy4,
	IK_Joy5,
	IK_Joy6,
	IK_Joy7,
	IK_Joy8,
	IK_Joy9,
	IK_Joy10,
	IK_Joy11,
	IK_Joy12,
	IK_Joy13,
	IK_Joy14,
	IK_Joy15,
	IK_Enter,
	IK_Escape,
	IK_Up,
	IK_Down,
	IK_Left,
	IK_Right,
};

static FLOAT ApplyDreamcastDeadZone( INT Value, FLOAT DeadZone )
{
	const FLOAT Normalized = Clamp( Value / 127.f, -1.f, 1.f );
	const FLOAT Magnitude = Abs(Normalized);
	const FLOAT ClampedDeadZone = Clamp( DeadZone, 0.f, 0.95f );
	if( Magnitude <= ClampedDeadZone )
		return 0.f;

	const FLOAT Rescaled = ( Magnitude - ClampedDeadZone ) / ( 1.f - ClampedDeadZone );
	return Normalized < 0.f ? -Rescaled : Rescaled;
}

static void UpdateDreamcastTrigger( UBOOL& Down, INT Value )
{
	const INT PressThreshold = 32;
	const INT ReleaseThreshold = 24;
	if( Down )
		Down = Value > ReleaseThreshold;
	else
		Down = Value >= PressThreshold;
}

BYTE UKOSViewport::KeyMap[MAX_KBD_KEYS];

//
// Scancode -> EInputKey translation map.
//
void UKOSViewport::InitKeyMap()
{
	#define INIT_KEY_RANGE( AStart, AEnd, BStart, BEnd ) \
		for( DWORD Key = AStart; Key <= AEnd; ++Key ) KeyMap[Key] = BStart + ( Key - AStart )

	appMemset( KeyMap, 0, sizeof( KeyMap ) );

	INIT_KEY_RANGE( 0x04, 0x1d, IK_A, IK_Z );
	INIT_KEY_RANGE( 0x1e, 0x26, IK_1, IK_9 );
	INIT_KEY_RANGE( 0x3a, 0x45, IK_F1, IK_F12 );

	KeyMap[0x27] = IK_0;
	KeyMap[0x28] = IK_Enter;
	KeyMap[0x29] = IK_Escape;
	KeyMap[0x2a] = IK_Backspace;
	KeyMap[0x2b] = IK_Tab;
	KeyMap[0x2c] = IK_Space;
	KeyMap[0x35] = IK_Tilde;
	KeyMap[0x4f] = IK_Right;
	KeyMap[0x50] = IK_Left;
	KeyMap[0x51] = IK_Down;
	KeyMap[0x52] = IK_Up;

	#undef INIT_KEY_RANGE
}

//
// Static init.
//
void UKOSViewport::InternalClassInitializer( UClass* Class )
{
	guard(UKOSViewport::InternalClassInitializer);

	InitKeyMap();

	unguard;
}

//
// Constructor.
//
UKOSViewport::UKOSViewport( ULevel* InLevel, UKOSClient* InClient )
:	UViewport( InLevel, InClient )
,	Client( InClient )
{
	guard(UKOSViewport::UKOSViewport);

	ColorBytes = 2;
	Caps = 0;
	appMemset( KeyState, 0, sizeof(KeyState) );
	appMemset( KeyStatePrev, 0, sizeof(KeyStatePrev) );
	JoyActionState = 0;
#if defined(PLATFORM_DREAMCAST)
	ProfilePreviousButtons = 0;
	ProfileChordActive = false;
#endif
	LeftTriggerDown = false;
	RightTriggerDown = false;
	MenuStartArmed = true;
	InputUpdateTime = appSeconds();
	SavedX = 0;
	SavedY = 0;

	// Init input.
	if( GIsEditor )
		Input->Init( this, GSystem );

	Destroyed = false;
	QuitRequested = false;

	unguard;
}

// UObject interface.
void UKOSViewport::Destroy()
{
	guard(UKOSViewport::Destroy);

	if( Client->FullscreenViewport == this )
	{
		Client->FullscreenViewport = NULL;
	}
	UViewport::Destroy();

	unguard;
}

//
// Set the mouse cursor according to Unreal or UnrealEd's mode, or to
// an hourglass if a slow task is active. Not implemented.
//
void UKOSViewport::SetModeCursor()
{
	guard(UKOSViewport::SetModeCursor);
	unguard;
}

//
// Update user viewport interface.
//
void UKOSViewport::UpdateWindow()
{
	guard(UKOSViewport::UpdateViewportWindow);

	unguard;
}

//
// Open a viewport window.
//
void UKOSViewport::OpenWindow( void* InParentWindow, UBOOL Temporary, INT NewX, INT NewY, INT OpenX, INT OpenY )
{
	guard(UKOSViewport::OpenWindow);
	check(Actor);
	check(!OnHold);
	UBOOL DoRepaint=0, DoSetActive=0;
	UBOOL NoHard=ParseParam( appCmdLine(), "nohard" );
	NewX = Align(NewX,4);

	// User window of launcher if no parent window was specified.
	if( !InParentWindow )
	{
		QWORD ParentPtr;
		Parse( appCmdLine(), "HWND=", ParentPtr );
		InParentWindow = (void*)ParentPtr;
	}

	if( Temporary )
	{
		// Create in-memory data.
		ColorBytes = 2;
		ScreenPointer = (BYTE*)appMalloc( 2 * NewX * NewY, "TemporaryViewportData" );	
		debugf( NAME_Log, "Opened temporary viewport" );
	}
	else
	{
		ColorBytes = 2;
		if( NewX <= 320 )
		{
			vid_set_mode( DM_320x240, PM_RGB565 );
			NewX = 320;
			NewY = 240;
		}
		else if( NewX <= 640 )
		{
			vid_set_mode( DM_640x480, PM_RGB565 );
			NewX = 640;
			NewY = 480;
		}
		else
		{
			vid_set_mode( DM_768x480, PM_RGB565 );
			NewX = 768;
			NewY = 480;
		}
	}

	SizeX = NewX;
	SizeY = NewY;

	if( !RenDev && Temporary )
		Client->TryRenderDevice( this, "SoftDrv.SoftwareRenderDevice", 0 );
	if( !RenDev && !GIsEditor && !NoHard )
		Client->TryRenderDevice( this, "ini:Engine.Engine.GameRenderDevice", Client->StartupFullscreen );
	if( !RenDev )
		Client->TryRenderDevice( this, "ini:Engine.Engine.WindowedRenderDevice", 0 );
	check(RenDev);

	if( !Temporary )
		UpdateWindow();
	if( DoRepaint )
		Repaint();

	unguard;
}

//
// Close a viewport window.  Assumes that the viewport has been opened with
// OpenViewportWindow.  Does not affect the viewport's object, only the
// platform-specific information associated with it.
//
void UKOSViewport::CloseWindow()
{
	guard(UKOSViewport::CloseWindow);


	unguard;
}

//
// Lock the viewport window and set the approprite Screen and RealScreen fields
// of Viewport.  Returns 1 if locked successfully, 0 if failed.  Note that a
// lock failing is not a critical error; it's a sign that a DirectDraw mode
// has ended or the user has closed a viewport window.
//
UBOOL UKOSViewport::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize )
{
	guard(UKOSViewport::LockWindow);
	uclock(Client->DrawCycles);

	// Success.
	uunclock(Client->DrawCycles);

	return UViewport::Lock( FlashScale, FlashFog, ScreenClear, RenderLockFlags, HitData, HitSize );

	unguard;
}

//
// Unlock the viewport window.  If Blit=1, blits the viewport's frame buffer.
//
void UKOSViewport::Unlock( UBOOL Blit )
{
	guard(UKOSViewport::Unlock);

	Client->DrawCycles=0;
	uclock(Client->DrawCycles);

	// Unlock base.
	UViewport::Unlock( Blit );

	uunclock(Client->DrawCycles);

	unguard;
}

//
// Make this viewport the current one.
// If Viewport=0, makes no viewport the current one.
//
void UKOSViewport::MakeCurrent()
{
	guard(UKOSViewport::MakeCurrent);
	Current = 1;
	for( INT i=0; i<Client->Viewports.Num(); i++ )
	{
		UViewport* OldViewport = Client->Viewports(i);
		if( OldViewport->Current && OldViewport != this )
		{
			OldViewport->Current = 0;
			OldViewport->UpdateWindow();
		}
	}
	UpdateWindow();
	unguard;
}

//
// Repaint the viewport.
//
void UKOSViewport::Repaint()
{
	guard(UKOSViewport::Repaint);
	if( !OnHold && RenDev && SizeX && SizeY )
		Client->Engine->Draw( this, 0 );
	unguard;
}

//
// Set the client size (viewport view size) of a viewport.
//
void UKOSViewport::SetClientSize( INT NewX, INT NewY, UBOOL UpdateProfile )
{
	guard(UKOSViewport::SetClientSize);


	SizeX = NewX;
	SizeY = NewY;

	// Optionally save this size in the profile.
	if( UpdateProfile )
	{
		Client->ViewportX = NewX;
		Client->ViewportY = NewY;
		Client->SaveConfig();
	}

	unguard;
}

//
// Return the viewport's window.
//
void* UKOSViewport::GetWindow()
{
	return (void*)-1;
}

//
// Try to make this viewport fullscreen, matching the fullscreen
// mode of the nearest x-size to the current window. If already in
// fullscreen, returns to non-fullscreen.
//
void UKOSViewport::MakeFullscreen( INT NewX, INT NewY, UBOOL UpdateProfile )
{
	guard(UKOSViewport::MakeFullscreen);

	// If someone else is fullscreen, stop them.
	if( Client->FullscreenViewport )
		Client->EndFullscreen();

	// Save this window.
	SavedX = SizeX;
	SavedY = SizeY;

	// Fullscreen rendering. For now no borderless.
	Client->FullscreenViewport = this;
	SetClientSize( NewX, NewY, false );

	if( UpdateProfile )
	{
		Client->ViewportX = NewX;
		Client->ViewportY = NewY;
		Client->SaveConfig();
	}

	unguard;
}

//
//
//
void UKOSViewport::EndFullscreen()
{
	guard(UKOSViewport::EndFullscreen);

	SetClientSize( SavedX, SavedY, false );

	unguard;
}

//
// Update input for viewport.
//
void UKOSViewport::UpdateInput( UBOOL Reset )
{
	guard(UKOSViewport::UpdateInput);

	if( Reset )
	{
		appMemset( KeyState, 0, sizeof(KeyState) );
		appMemset( KeyStatePrev, 0, sizeof(KeyStatePrev) );
		SetJoyActionState( 0 );
		LeftTriggerDown = false;
		RightTriggerDown = false;
		MenuStartArmed = true;
#if defined(PLATFORM_DREAMCAST)
		ProfilePreviousButtons = 0;
		ProfileChordActive = false;
#endif

		maple_device_t* Keyboard = maple_enum_type( 0, MAPLE_FUNC_KEYBOARD );
		if( Keyboard )
		{
			kbd_state_t* State = (kbd_state_t*)maple_dev_status( Keyboard );
			if( State )
			{
				appMemcpy( KeyState, State->matrix, sizeof(KeyState) );
				appMemcpy( KeyStatePrev, State->matrix, sizeof(KeyStatePrev) );
			}
		}

		maple_device_t* Controller = maple_enum_type( 0, MAPLE_FUNC_CONTROLLER );
		if( Controller )
		{
			cont_state_t* State = (cont_state_t*)maple_dev_status( Controller );
			if( State )
			{
				UpdateDreamcastTrigger( LeftTriggerDown, State->ltrig );
				UpdateDreamcastTrigger( RightTriggerDown, State->rtrig );
			}
		}
		InputUpdateTime = appSeconds();
	}
	unguard;
}

//
// If the cursor is currently being captured, stop capturing, clipping, and 
// hiding it, and move its position back to where it was when it was initially
// captured.
//
void UKOSViewport::SetMouseCapture( UBOOL Capture, UBOOL Clip, UBOOL OnlyFocus )
{
	guard(UKOSViewport::SetMouseCapture);

	unguard;
}

UBOOL UKOSViewport::CauseInputEvent( INT iKey, EInputAction Action, FLOAT Delta )
{
	guard(UWindowsViewport::CauseInputEvent);

	// Route to engine if a valid key
	if( iKey > 0 )
		return Client->Engine->InputEvent( this, (EInputKey)iKey, Action, Delta );
	else
		return 0;

	unguard;
}

void UKOSViewport::SetJoyActionState( DWORD NewActionState )
{
	const DWORD ChangedActions = NewActionState ^ JoyActionState;
	for( DWORD Action = 0; Action < MAX_JOY_ACTIONS; ++Action )
	{
		const DWORD Mask = 1U << Action;
		if( ChangedActions & Mask )
		{
			const EInputAction Event = ( NewActionState & Mask ) ? IST_Press : IST_Release;
			CauseInputEvent( JoyActionMap[Action], Event );
		}
	}
	JoyActionState = NewActionState;
}

void UKOSViewport::TickJoystick( maple_device_t* Dev, const FLOAT DeltaTime )
{
	cont_state_t* State = (cont_state_t*)maple_dev_status( Dev );
	if( !State )
	{
		SetJoyActionState( 0 );
		LeftTriggerDown = false;
		RightTriggerDown = false;
		MenuStartArmed = true;
#if defined(PLATFORM_DREAMCAST)
		ProfilePreviousButtons = 0;
		ProfileChordActive = false;
#endif
		return;
	}

	UpdateDreamcastTrigger( LeftTriggerDown, State->ltrig );
	UpdateDreamcastTrigger( RightTriggerDown, State->rtrig );

#if defined(PLATFORM_DREAMCAST)
	// Right trigger + D-pad controls the profiler. Right trigger alone still
	// fires, so simultaneous input is needed to avoid a shot before the chord.
	// Consume the release tail so releasing the D-pad first cannot resume fire.
	const DWORD ProfilePressed = State->buttons & ~ProfilePreviousButtons;
	ProfilePreviousButtons = State->buttons;
	const DWORD ProfileDirections = CONT_DPAD_UP | CONT_DPAD_DOWN
		| CONT_DPAD_LEFT | CONT_DPAD_RIGHT;
	const UBOOL ProfileModifier = RightTriggerDown;
	if( ProfileModifier && (State->buttons & ProfileDirections) )
		ProfileChordActive = true;
	if( ProfileChordActive )
	{
		SetJoyActionState(0);
		MenuStartArmed = false;
		if( ProfileModifier && RenDev )
		{
			// One command per press; a diagonal cannot toggle two settings.
			if( ProfilePressed & CONT_DPAD_RIGHT ) RenDev->Exec("DCPPAGE", GSystem);
			else if( ProfilePressed & CONT_DPAD_UP ) RenDev->Exec("DCPDUMP", GSystem);
			else if( ProfilePressed & CONT_DPAD_DOWN ) RenDev->Exec("DCPOVERLAY", GSystem);
			else if( ProfilePressed & CONT_DPAD_LEFT ) RenDev->Exec("DCPDETAIL", GSystem);
		}
		if( !State->buttons && !LeftTriggerDown && !RightTriggerDown )
		{
			ProfileChordActive = false;
			MenuStartArmed = true;
		}
		return;
	}
#endif

	const UBOOL InMenu = Console
		&& ((UObject*)Console)->GetMainFrame()
		&& ((UObject*)Console)->GetMainFrame()->StateNode
		&& ((UObject*)Console)->GetMainFrame()->StateNode->GetFName() == "Menuing";

	DWORD NewActionState = 0;
	#define DC_ACTION(Action, Condition) \
		do { if( Condition ) NewActionState |= 1U << (Action); } while( 0 )

	if( InMenu )
	{
		DC_ACTION( DCJA_UIAccept, State->buttons & CONT_A );
		DC_ACTION( DCJA_UICancel, ( State->buttons & CONT_B )
			|| ( MenuStartArmed && ( State->buttons & CONT_START ) ) );
		DC_ACTION( DCJA_UIUp, State->buttons & CONT_DPAD_UP );
		DC_ACTION( DCJA_UIDown, State->buttons & CONT_DPAD_DOWN );
		DC_ACTION( DCJA_UILeft, State->buttons & CONT_DPAD_LEFT );
		DC_ACTION( DCJA_UIRight, State->buttons & CONT_DPAD_RIGHT );
		if( !( State->buttons & CONT_START ) )
			MenuStartArmed = true;
	}
	else
	{
		DC_ACTION( DCJA_ShowMenu, State->buttons & CONT_START );
		if( State->buttons & CONT_START )
			MenuStartArmed = false;
		DC_ACTION( DCJA_Translator, State->buttons & CONT_DPAD_UP );
		DC_ACTION( DCJA_InventoryActivate, State->buttons & CONT_DPAD_DOWN );
		DC_ACTION( DCJA_PreviousWeapon, State->buttons & CONT_DPAD_LEFT );
		DC_ACTION( DCJA_NextWeapon, State->buttons & CONT_DPAD_RIGHT );

		if( LeftTriggerDown )
		{
			DC_ACTION( DCJA_Jump, State->buttons & CONT_Y );
			DC_ACTION( DCJA_Duck, State->buttons & CONT_A );
			DC_ACTION( DCJA_InventoryPrevious, State->buttons & CONT_X );
			DC_ACTION( DCJA_InventoryNext, State->buttons & CONT_B );
			DC_ACTION( DCJA_AltFire, RightTriggerDown );
		}
		else
		{
			DC_ACTION( DCJA_MoveForward, State->buttons & CONT_Y );
			DC_ACTION( DCJA_MoveBackward, State->buttons & CONT_A );
			DC_ACTION( DCJA_StrafeLeft, State->buttons & CONT_X );
			DC_ACTION( DCJA_StrafeRight, State->buttons & CONT_B );
			DC_ACTION( DCJA_Fire, RightTriggerDown );
		}
	}

	#undef DC_ACTION

	SetJoyActionState( NewActionState );

	// Reset sequence
	if( ( State->buttons & CONT_RESET_BUTTONS ) == CONT_RESET_BUTTONS )
		QuitRequested = true;

	if( !InMenu )
	{
		const FLOAT CameraX = ApplyDreamcastDeadZone( State->joyx, Client->DeadZoneRUV );
		const FLOAT CameraY = ApplyDreamcastDeadZone( State->joyy, Client->DeadZoneRUV );
		const FLOAT AxisDeltaTime = Clamp( DeltaTime, 0.f, 0.1f );
		const FLOAT AxisScale = Client->ScaleRUV * 60.f * AxisDeltaTime;
		if( CameraX )
			CauseInputEvent( IK_JoyU, IST_Axis, CameraX * AxisScale );
		if( CameraY )
		{
			const FLOAT VerticalScale = Client->InvertV ? -AxisScale : AxisScale;
			CauseInputEvent( IK_JoyV, IST_Axis, CameraY * VerticalScale );
		}
	}
}

void UKOSViewport::TickKeyboard( maple_device_t* Dev, const FLOAT DeltaTime )
{
	kbd_state_t* State = (kbd_state_t*)maple_dev_status( Dev );
	if( !State )
		return;

	// Emit key events for the regular input system
	appMemcpy( KeyStatePrev, KeyState, sizeof( KeyState ) );
	appMemcpy( KeyState, State->matrix, sizeof( KeyState ) );
	for( INT i = 0; i < MAX_KBD_KEYS; ++i )
	{
		if( KeyMap[i] )
		{
			if( KeyState[i] && !KeyStatePrev[i] )
				CauseInputEvent( KeyMap[i], IST_Press );
			else if( !KeyState[i] && KeyStatePrev[i ])
				CauseInputEvent( KeyMap[i], IST_Release );
		}
	}

	// Emit text input
	const INT Chr = kbd_queue_pop( Dev, true );
	if( Chr > 0 ) {
		if( isprint( Chr ) || Chr == '\r' )
			Client->Engine->Key( this, (EInputKey)Chr );
	}
}

UBOOL UKOSViewport::TickInput()
{
	guard(UKOSViewport::TickInput);

	maple_device_t* Dev;
	const FLOAT CurTime = appSeconds();
	const FLOAT DeltaTime = CurTime - InputUpdateTime;

	// Check keyboard
	Dev = maple_enum_type( 0, MAPLE_FUNC_KEYBOARD );
	if ( Dev )
		TickKeyboard( Dev, DeltaTime );

	// Check joystick.
	Dev = Client->UseJoystick ? maple_enum_type( 0, MAPLE_FUNC_CONTROLLER ) : NULL;
	if( Dev )
		TickJoystick( Dev, DeltaTime );
	else
	{
		SetJoyActionState( 0 );
		LeftTriggerDown = false;
		RightTriggerDown = false;
		MenuStartArmed = true;
#if defined(PLATFORM_DREAMCAST)
		ProfilePreviousButtons = 0;
		ProfileChordActive = false;
#endif
	}

	InputUpdateTime = CurTime;

	return QuitRequested;

	unguard;
}

/*-----------------------------------------------------------------------------
	Command line.
-----------------------------------------------------------------------------*/

UBOOL UKOSViewport::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UKOSViewport::Exec);
	if( UViewport::Exec( Cmd, Out ) )
	{
		return 1;
	}
	else if( ParseCommand(&Cmd, "ToggleFullscreen") )
	{
		// Toggle fullscreen.
		if( Client->FullscreenViewport )
			Client->EndFullscreen();
		else if( !(Actor->ShowFlags & SHOW_ChildWindow) )
			Client->TryRenderDevice( this, "ini:Engine.Engine.GameRenderDevice", 1 );
		return 1;
	}
	else if( ParseCommand(&Cmd, "GetCurrentRes") )
	{
		Out->Logf( "%ix%i", SizeX, SizeY );
		return 1;
	}
	else if( ParseCommand(&Cmd, "SetRes") )
	{
		INT X=appAtoi(Cmd), Y=appAtoi(appStrchr(Cmd,'x') ? appStrchr(Cmd,'x')+1 : appStrchr(Cmd,'X') ? appStrchr(Cmd,'X')+1 : "");
		if( X && Y )
		{
			if( Client->FullscreenViewport )
				MakeFullscreen( X, Y, 1 );
			else
				SetClientSize( X, Y, 1 );
		}
		return 1;
	}
	else if( ParseCommand(&Cmd, "Preferences") )
	{
		if( Client->FullscreenViewport )
			Client->EndFullscreen();
		return 1;
	}
	else return 0;
	unguard;
}
