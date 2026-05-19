#pragma once

#include "AegisUniversalRuntime.h"

#include <cstdint>

enum AegisUniversalOverlayBackend : std::uint32_t
{
    AegisUniversalOverlayBackend_None = 0,
    AegisUniversalOverlayBackend_Direct3D9 = 1,
    AegisUniversalOverlayBackend_Direct3D11 = 2,
    AegisUniversalOverlayBackend_Direct3D12 = 3,
    AegisUniversalOverlayBackend_OpenGL = 4,
    AegisUniversalOverlayBackend_Vulkan = 5
};

struct AegisUniversalOverlayBridgeInfo
{
    std::uint32_t size;
    std::int32_t running;
    std::int32_t hooksInstalled;
    std::int32_t swapchainCaptured;
    std::int32_t hwndFound;
    std::int32_t imguiInitialized;
    std::int32_t renderTargetReady;
    std::int32_t menuVisible;
    std::uint64_t presentCount;
    std::uint64_t resizeCount;
    void* hwnd;
    std::uint32_t backbufferWidth;
    std::uint32_t backbufferHeight;
    std::uint32_t backbufferFormat;
    wchar_t selectedBackend[32];
    wchar_t detectedBackends[128];
    wchar_t status[256];
};

AEGIS_UNIVERSAL_API int AegisUniversalOverlay_Start();
AEGIS_UNIVERSAL_API void AegisUniversalOverlay_Stop();
AEGIS_UNIVERSAL_API int AegisUniversalOverlay_IsRunning();
AEGIS_UNIVERSAL_API int AegisUniversalOverlay_IsInstalled();
AEGIS_UNIVERSAL_API int AegisUniversalOverlay_IsMenuVisible();
AEGIS_UNIVERSAL_API void AegisUniversalOverlay_SetMenuVisible(int visible);
AEGIS_UNIVERSAL_API void AegisUniversalOverlay_ToggleMenu();
AEGIS_UNIVERSAL_API const wchar_t* AegisUniversalOverlay_GetStatus();
AEGIS_UNIVERSAL_API int AegisUniversalOverlay_GetInfo(AegisUniversalOverlayBridgeInfo* outInfo);
