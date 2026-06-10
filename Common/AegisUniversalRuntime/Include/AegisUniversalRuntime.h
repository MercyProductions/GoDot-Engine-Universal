#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

#if defined(AEGIS_UNIVERSAL_EXPORTS)
#define AEGIS_UNIVERSAL_API extern "C" __declspec(dllexport)
#else
#define AEGIS_UNIVERSAL_API extern "C" __declspec(dllimport)
#endif

enum AegisUniversalRuntimeFlags : std::uint32_t
{
    AegisUniversalRuntime_None = 0,
    AegisUniversalRuntime_ProcessHintMatched = 1u << 0,
    AegisUniversalRuntime_ModuleHintMatched = 1u << 1,
    AegisUniversalRuntime_ExportHintMatched = 1u << 2,
    AegisUniversalRuntime_EngineDetected = 1u << 3
};

enum AegisUniversalSignatureFlags : std::uint32_t
{
    AegisUniversalSignature_None = 0,
    AegisUniversalSignature_Process = 1u << 0,
    AegisUniversalSignature_Module = 1u << 1,
    AegisUniversalSignature_Export = 1u << 2,
    AegisUniversalSignature_Renderer = 1u << 3,
    AegisUniversalSignature_Scripting = 1u << 4,
    AegisUniversalSignature_Physics = 1u << 5,
    AegisUniversalSignature_Core = 1u << 6
};

enum AegisUniversalSdkSource : std::uint32_t
{
    AegisUniversalSdkSource_None = 0,
    AegisUniversalSdkSource_LiveExport = 1u << 0,
    AegisUniversalSdkSource_DumpedSdk = 1u << 1,
    AegisUniversalSdkSource_ReloadedSdk = 1u << 2
};

struct AegisUniversalSignature
{
    const wchar_t* processHint;
    const wchar_t* moduleHint;
    const char* exportName;
    std::uint32_t flags;
    const char* note;
};

struct AegisUniversalProfile
{
    const wchar_t* engineName;
    const wchar_t* shortName;
    const wchar_t* reportFileName;
    const wchar_t* traceFileName;
    const wchar_t* logFileName;
    const AegisUniversalSignature* signatures;
    std::size_t signatureCount;
};

struct AegisUniversalRuntimeInfo
{
    wchar_t engineName[64];
    wchar_t processName[MAX_PATH];
    wchar_t processPath[MAX_PATH];
    wchar_t detectedModule[MAX_PATH];
    std::uint32_t moduleCount;
    std::uint32_t matchedModuleCount;
    std::uint32_t matchedExportCount;
    std::uint32_t flags;
};

struct AegisUniversalModuleInfo
{
    wchar_t name[MAX_PATH];
    wchar_t path[MAX_PATH];
    std::uintptr_t baseAddress;
    std::uint32_t imageSize;
    std::uint32_t flags;
};

struct AegisUniversalExportInfo
{
    char exportName[128];
    wchar_t moduleName[MAX_PATH];
    wchar_t modulePath[MAX_PATH];
    std::uintptr_t address;
    std::uint32_t flags;
};

struct AegisUniversalSdkExportInfo
{
    char exportName[160];
    wchar_t moduleName[MAX_PATH];
    wchar_t modulePath[MAX_PATH];
    std::uintptr_t address;
    std::uintptr_t rva;
    std::uint32_t ordinal;
    std::uint32_t flags;
    std::uint32_t source;
};

struct AegisUniversalResolvedSymbol
{
    char exportName[160];
    wchar_t moduleName[MAX_PATH];
    wchar_t modulePath[MAX_PATH];
    std::uintptr_t address;
    std::uintptr_t rva;
    std::uint32_t ordinal;
    std::uint32_t flags;
    std::uint32_t source;
    std::int32_t loaded;
};

struct AegisUniversalSdkValidationInfo
{
    std::uint32_t size;
    std::int32_t sdkLoaded;
    std::uint32_t loadedExportCount;
    std::uint32_t liveResolvedCount;
    std::uint32_t sdkOnlyCount;
    std::uint32_t staleRvaCount;
    std::uint32_t selfReferenceCount;
    std::uint32_t moduleMatchedCount;
    wchar_t details[256];
};

AEGIS_UNIVERSAL_API int AegisUniversal_Initialize();
AEGIS_UNIVERSAL_API int AegisUniversal_Refresh();
AEGIS_UNIVERSAL_API void AegisUniversal_Shutdown();
AEGIS_UNIVERSAL_API int AegisUniversal_IsInitialized();
AEGIS_UNIVERSAL_API int AegisUniversal_IsEngineDetected();
AEGIS_UNIVERSAL_API int AegisUniversal_GetRuntimeInfo(AegisUniversalRuntimeInfo* outInfo);
AEGIS_UNIVERSAL_API std::uint32_t AegisUniversal_GetModuleCount();
AEGIS_UNIVERSAL_API int AegisUniversal_GetModuleInfo(std::uint32_t index, AegisUniversalModuleInfo* outInfo);
AEGIS_UNIVERSAL_API std::uint32_t AegisUniversal_GetMatchedExportCount();
AEGIS_UNIVERSAL_API int AegisUniversal_GetMatchedExportInfo(std::uint32_t index, AegisUniversalExportInfo* outInfo);
AEGIS_UNIVERSAL_API void* AegisUniversal_GetExport(const wchar_t* moduleName, const char* exportName);
AEGIS_UNIVERSAL_API int AegisUniversal_WriteRuntimeReport(const wchar_t* reportPath);
AEGIS_UNIVERSAL_API int AegisUniversal_WriteModuleCsv(const wchar_t* csvPath);
AEGIS_UNIVERSAL_API int AegisUniversal_WriteMatchedExportsCsv(const wchar_t* csvPath);
AEGIS_UNIVERSAL_API int AegisUniversal_DumpSdkJson(const wchar_t* sdkPath);
AEGIS_UNIVERSAL_API int AegisUniversal_WriteSdkHeader(const wchar_t* headerPath);
AEGIS_UNIVERSAL_API int AegisUniversal_WriteSdkMapCsv(const wchar_t* csvPath);
AEGIS_UNIVERSAL_API int AegisUniversal_LoadSdkJson(const wchar_t* sdkPath);
AEGIS_UNIVERSAL_API void AegisUniversal_ClearLoadedSdk();
AEGIS_UNIVERSAL_API std::uint32_t AegisUniversal_GetLoadedSdkExportCount();
AEGIS_UNIVERSAL_API int AegisUniversal_GetLoadedSdkExportInfo(std::uint32_t index, AegisUniversalSdkExportInfo* outInfo);
AEGIS_UNIVERSAL_API int AegisUniversal_ResolveExport(const wchar_t* moduleName, const char* exportName, AegisUniversalResolvedSymbol* outSymbol);
AEGIS_UNIVERSAL_API int AegisUniversal_ResolveRva(const wchar_t* moduleName, std::uintptr_t rva, AegisUniversalResolvedSymbol* outSymbol);
AEGIS_UNIVERSAL_API int AegisUniversal_ValidateLoadedSdk(AegisUniversalSdkValidationInfo* outInfo);
AEGIS_UNIVERSAL_API int AegisUniversal_WriteSdkValidationJson(const wchar_t* jsonPath);
AEGIS_UNIVERSAL_API const char* AegisUniversal_GetBrandAsciiArt();
AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetEngineName();
AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetReportFileName();
AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetTraceFileName();
AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetLogFileName();
AEGIS_UNIVERSAL_API void AegisUniversal_LogA(const char* message);
AEGIS_UNIVERSAL_API void AegisUniversal_LogPrintfA(const char* format, ...);
AEGIS_UNIVERSAL_API void AegisUniversal_LogW(const wchar_t* message);
