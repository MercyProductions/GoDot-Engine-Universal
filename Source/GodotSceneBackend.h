#pragma once

#include "AegisGodotUniversal.h"

enum class GodotRuntimeBackendKind : int
{
    Unknown = 0,
    Godot3 = 3,
    Godot4 = 4
};

struct GodotAutoScanBackend
{
    bool (*Init)() = nullptr;
    void* (*GetActiveCamera)() = nullptr;
    std::uint32_t(AEGIS_GODOT_CALL *CollectObjects)(AegisGodotObjectSnapshot*, std::uint32_t, void*) = nullptr;
    int(AEGIS_GODOT_CALL *GetViewProjection)(AegisGodotMatrix4x4*, void*) = nullptr;
    const char* label = "unknown";
};

const GodotAutoScanBackend& GetGodot3AutoScanBackend();
const GodotAutoScanBackend& GetGodot4AutoScanBackend();
const GodotAutoScanBackend& GetGodotAutoScanBackend();
