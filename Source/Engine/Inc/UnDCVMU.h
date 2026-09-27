#pragma once
#if defined(PLATFORM_DREAMCAST)
ENGINE_API void DCVMUStartup();
ENGINE_API void DCVMURefreshMenus();
ENGINE_API UBOOL DCVMUCanSave(char* Error);
ENGINE_API UBOOL DCVMUSave(INT Slot, const char* File, const char* Map, const TArray<BYTE>& State,
                           char* Error, UBOOL Canonical = 0);
ENGINE_API UBOOL DCVMULoad(INT Slot, char* Map, char* File, TArray<BYTE>& State, char* Error);
#endif
