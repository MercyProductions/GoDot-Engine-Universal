#pragma once

#include "AegisUniversalRuntime.h"

#if defined(_MSC_VER)
#define AEGIS_GODOT_CALL __stdcall
#else
#define AEGIS_GODOT_CALL
#endif

enum AegisGodotMatrixFlags : std::uint32_t
{
    AegisGodotMatrix_Auto = 0,
    AegisGodotMatrix_RowMajor = 1u << 0,
    AegisGodotMatrix_ColumnMajor = 1u << 1,
    AegisGodotMatrix_D3DDepth = 1u << 2,
    AegisGodotMatrix_OpenGLDepth = 1u << 3,
    AegisGodotMatrix_YFlip = 1u << 4
};

enum AegisGodotObjectFlags : std::uint32_t
{
    AegisGodotObject_None = 0,
    AegisGodotObject_Node2D = 1u << 0,
    AegisGodotObject_Node3D = 1u << 1,
    AegisGodotObject_PhysicsBody = 1u << 2,
    AegisGodotObject_VisibleOnScreen = 1u << 3,
    AegisGodotObject_LikelyTarget = 1u << 4
};

struct AegisGodotVec3
{
    float x;
    float y;
    float z;
};

struct AegisGodotViewport
{
    float x;
    float y;
    float width;
    float height;
};

struct AegisGodotMatrix4x4
{
    float m[16];
    std::uint32_t flags;
};

struct AegisGodotObjectSnapshot
{
    std::uint64_t id;
    char name[96];
    char className[64];
    char path[160];
    AegisGodotVec3 origin;
    AegisGodotVec3 boundsMin;
    AegisGodotVec3 boundsMax;
    std::int32_t group;
    std::int32_t visible;
    std::uint32_t flags;
};

struct AegisGodotProjectedPoint
{
    float x;
    float y;
    float depth;
    std::int32_t clipped;
};

struct AegisGodotMemberInfo
{
    char className[64];
    char memberName[64];
    char typeName[64];
    std::uint32_t offset;
    std::uint32_t isMethod;
};

struct AegisGodotAdapterTiming
{
    double objectProviderMs;
    double matrixProviderMs;
    double viewportProviderMs;
    std::uint32_t objectCount;
    std::uint32_t projectedCount;
    std::uint32_t clippedCount;
    std::uint64_t frameId;
};

struct AegisGodotCapabilityInfo
{
    std::int32_t godotDetected;
    std::int32_t embeddedPackSectionFound;
    std::int32_t gdNativeExportFound;
    std::int32_t gdExtensionExportFound;
    std::int32_t godotSteamFound;
    std::int32_t joltExtensionFound;
    std::int32_t terrainExtensionFound;
    std::int32_t objectProviderRegistered;
    std::int32_t viewProjectionProviderRegistered;
    std::int32_t viewportProviderRegistered;
    std::int32_t viewportValid;
    std::int32_t matrixValid;
    std::int32_t w2sProjectionWorking;
    std::int32_t snapshotReady;
    wchar_t rendererBackend[32];
    wchar_t details[256];
};

using AegisGodotObjectProvider = std::uint32_t(AEGIS_GODOT_CALL*)(
    AegisGodotObjectSnapshot* outObjects,
    std::uint32_t capacity,
    void* userData);

using AegisGodotViewProjectionProvider = std::int32_t(AEGIS_GODOT_CALL*)(
    AegisGodotMatrix4x4* outMatrix,
    void* userData);

using AegisGodotViewportProvider = std::int32_t(AEGIS_GODOT_CALL*)(
    AegisGodotViewport* outViewport,
    void* userData);

AEGIS_UNIVERSAL_API void AegisGodot_RegisterObjectProvider(AegisGodotObjectProvider provider, void* userData);
AEGIS_UNIVERSAL_API void AegisGodot_RegisterViewProjectionProvider(AegisGodotViewProjectionProvider provider, void* userData);
AEGIS_UNIVERSAL_API void AegisGodot_RegisterViewportProvider(AegisGodotViewportProvider provider, void* userData);
AEGIS_UNIVERSAL_API int AegisGodot_UpdateProviders();
AEGIS_UNIVERSAL_API int AegisGodot_SubmitObjectSnapshots(const AegisGodotObjectSnapshot* objects, std::uint32_t count);
AEGIS_UNIVERSAL_API int AegisGodot_SubmitViewProjection(const AegisGodotMatrix4x4* matrix);
AEGIS_UNIVERSAL_API int AegisGodot_SubmitViewport(const AegisGodotViewport* viewport);
AEGIS_UNIVERSAL_API std::uint32_t AegisGodot_GetObjectCount();
AEGIS_UNIVERSAL_API int AegisGodot_GetObjectSnapshot(std::uint32_t index, AegisGodotObjectSnapshot* outObject);
AEGIS_UNIVERSAL_API int AegisGodot_GetAdapterTiming(AegisGodotAdapterTiming* outTiming);
AEGIS_UNIVERSAL_API int AegisGodot_GetCapabilityInfo(AegisGodotCapabilityInfo* outInfo);
AEGIS_UNIVERSAL_API int AegisGodot_ProjectWorldToScreen(const AegisGodotVec3* world, AegisGodotProjectedPoint* outPoint);
AEGIS_UNIVERSAL_API int AegisGodot_WriteSnapshotJson(const wchar_t* path);
AEGIS_UNIVERSAL_API int AegisGodot_LoadSnapshotJson(const wchar_t* path);
AEGIS_UNIVERSAL_API void AegisGodot_PrintCurrentObjects();
AEGIS_UNIVERSAL_API int AegisGodot_ScanClasses();
AEGIS_UNIVERSAL_API std::uint32_t AegisGodot_GetMemberCount();
AEGIS_UNIVERSAL_API int AegisGodot_GetMemberInfo(std::uint32_t index, AegisGodotMemberInfo* outInfo);
AEGIS_UNIVERSAL_API int AegisGodot_InitAutoProviders();
AEGIS_UNIVERSAL_API int AegisGodot_WriteResolverReport(const wchar_t* path);
