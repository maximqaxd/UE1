#pragma once

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
ENGINE_API UBOOL DCCookStateBaseline(ULevel* Level, const char* File, char* Error);
ENGINE_API UBOOL DCWriteWorldState(ULevel* Level, const char* Baseline, const char* File, char* Error);
ENGINE_API UBOOL DCReadWorldState(ULevel* Level, const char* Baseline, const char* File, char* Error);
ENGINE_API UBOOL DCCheckWorldState(const char* Map, const char* Baseline, const char* File, char* Error);
ENGINE_API UBOOL DCTestWorldState(ULevel* Level, const char* Baseline, char* Error);
#endif
