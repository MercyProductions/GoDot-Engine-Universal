#include "../../Include/AegisGodotUniversal.h"

#include <Windows.h>

#include <cstdio>
#include <cwchar>
#include <cstring>

namespace
{
    std::uint32_t AEGIS_GODOT_CALL SampleObjects(AegisGodotObjectSnapshot* outObjects, std::uint32_t capacity, void*)
    {
        if (!outObjects || capacity < 4)
            return 0;

        AegisGodotObjectSnapshot samples[4] = {};
        samples[0].id = 101;
        strcpy_s(samples[0].name, "Player");
        strcpy_s(samples[0].className, "CharacterBody3D");
        strcpy_s(samples[0].path, "/root/Game/Player");
        samples[0].origin = { 0.0f, 0.0f, 4.0f };
        samples[0].boundsMin = { -0.5f, -0.5f, 0.0f };
        samples[0].boundsMax = { 0.5f, 0.5f, 1.8f };
        samples[0].group = 1;
        samples[0].visible = 1;
        samples[0].flags = AegisGodotObject_Node3D | AegisGodotObject_PhysicsBody | AegisGodotObject_VisibleOnScreen;

        samples[1].id = 102;
        strcpy_s(samples[1].name, "TrainingNpc");
        strcpy_s(samples[1].className, "CharacterBody3D");
        strcpy_s(samples[1].path, "/root/Game/Npcs/TrainingNpc");
        samples[1].origin = { 1.4f, 0.0f, 6.0f };
        samples[1].boundsMin = { -0.45f, -0.45f, 0.0f };
        samples[1].boundsMax = { 0.45f, 0.45f, 1.7f };
        samples[1].group = 2;
        samples[1].visible = 1;
        samples[1].flags = AegisGodotObject_Node3D | AegisGodotObject_PhysicsBody | AegisGodotObject_VisibleOnScreen;

        samples[2].id = 103;
        strcpy_s(samples[2].name, "Collectible");
        strcpy_s(samples[2].className, "Area3D");
        strcpy_s(samples[2].path, "/root/Game/Pickups/Collectible");
        samples[2].origin = { -1.4f, 0.0f, 5.0f };
        samples[2].boundsMin = { -0.25f, -0.25f, -0.25f };
        samples[2].boundsMax = { 0.25f, 0.25f, 0.25f };
        samples[2].group = 0;
        samples[2].visible = 1;
        samples[2].flags = AegisGodotObject_Node3D | AegisGodotObject_VisibleOnScreen;

        samples[3].id = 104;
        strcpy_s(samples[3].name, "HudMarker");
        strcpy_s(samples[3].className, "Node2D");
        strcpy_s(samples[3].path, "/root/Game/UI/HudMarker");
        samples[3].origin = { 0.0f, 0.8f, 4.0f };
        samples[3].boundsMin = { -0.1f, -0.1f, 0.0f };
        samples[3].boundsMax = { 0.1f, 0.1f, 0.0f };
        samples[3].group = 3;
        samples[3].visible = 1;
        samples[3].flags = AegisGodotObject_Node2D | AegisGodotObject_VisibleOnScreen;

        std::memcpy(outObjects, samples, sizeof(samples));
        return 4;
    }

    std::int32_t AEGIS_GODOT_CALL SampleViewport(AegisGodotViewport* outViewport, void*)
    {
        if (!outViewport)
            return 0;
        *outViewport = { 0.0f, 0.0f, 1280.0f, 720.0f };
        return 1;
    }

    std::int32_t AEGIS_GODOT_CALL SampleMatrix(AegisGodotMatrix4x4* outMatrix, void*)
    {
        if (!outMatrix)
            return 0;

        *outMatrix = {};
        outMatrix->flags = AegisGodotMatrix_RowMajor | AegisGodotMatrix_OpenGLDepth;
        outMatrix->m[0] = 1.0f;
        outMatrix->m[5] = 1.0f;
        outMatrix->m[10] = 1.0f;
        outMatrix->m[14] = 1.0f;
        return 1;
    }

    template <typename T>
    bool LoadFunction(HMODULE module, const char* name, T& outFunction)
    {
        outFunction = reinterpret_cast<T>(::GetProcAddress(module, name));
        if (!outFunction)
            std::printf("missing export: %s\n", name);
        return outFunction != nullptr;
    }
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* dllPath = argc > 1 ? argv[1] : L"..\\..\\build\\x64\\Release\\AegisGodotUniversal.dll";
    HMODULE dll = ::LoadLibraryW(dllPath);
    if (!dll)
    {
        ::wprintf(L"LoadLibrary failed: %ls (%lu)\n", dllPath, ::GetLastError());
        return 1;
    }

    using RegisterObjectsFn = void(__cdecl*)(AegisGodotObjectProvider, void*);
    using RegisterMatrixFn = void(__cdecl*)(AegisGodotViewProjectionProvider, void*);
    using RegisterViewportFn = void(__cdecl*)(AegisGodotViewportProvider, void*);
    using UpdateFn = int(__cdecl*)();
    using CapabilityFn = int(__cdecl*)(AegisGodotCapabilityInfo*);
    using CountFn = std::uint32_t(__cdecl*)();
    using WriteJsonFn = int(__cdecl*)(const wchar_t*);
    using PrintFn = void(__cdecl*)();

    RegisterObjectsFn registerObjects = nullptr;
    RegisterMatrixFn registerMatrix = nullptr;
    RegisterViewportFn registerViewport = nullptr;
    UpdateFn update = nullptr;
    CapabilityFn getCapability = nullptr;
    CountFn getCount = nullptr;
    WriteJsonFn writeJson = nullptr;
    PrintFn printObjects = nullptr;

    if (!LoadFunction(dll, "AegisGodot_RegisterObjectProvider", registerObjects) ||
        !LoadFunction(dll, "AegisGodot_RegisterViewProjectionProvider", registerMatrix) ||
        !LoadFunction(dll, "AegisGodot_RegisterViewportProvider", registerViewport) ||
        !LoadFunction(dll, "AegisGodot_UpdateProviders", update) ||
        !LoadFunction(dll, "AegisGodot_GetCapabilityInfo", getCapability) ||
        !LoadFunction(dll, "AegisGodot_GetObjectCount", getCount) ||
        !LoadFunction(dll, "AegisGodot_WriteSnapshotJson", writeJson) ||
        !LoadFunction(dll, "AegisGodot_PrintCurrentObjects", printObjects))
    {
        return 2;
    }

    registerObjects(SampleObjects, nullptr);
    registerMatrix(SampleMatrix, nullptr);
    registerViewport(SampleViewport, nullptr);
    update();

    AegisGodotCapabilityInfo capability = {};
    getCapability(&capability);
    ::wprintf(L"backend=%ls details=%ls\n", capability.rendererBackend, capability.details);
    std::printf("objects=%u\n", getCount());
    printObjects();
    writeJson(L"GodotProviderHarness.snapshot.json");
    return 0;
}
