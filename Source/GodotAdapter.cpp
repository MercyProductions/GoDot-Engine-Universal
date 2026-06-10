#include "AegisGodotUniversal.h"
#include "GodotResolverTrace.h"
#include "GodotSafeProbe.h"
#include "GodotSceneBackend.h"

AEGIS_UNIVERSAL_API int AegisGodot_InitAutoProviders();

#include <Windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>

bool g_drawAllNode3Ds = false;

namespace
{
    void LogMsg(const char* format, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        AegisUniversal_LogA(buffer);
    }

    constexpr std::uint32_t kMaxObjects = 4096;

    struct ProviderState
    {
        AegisGodotObjectProvider objectProvider = nullptr;
        void* objectUserData = nullptr;
        AegisGodotViewProjectionProvider matrixProvider = nullptr;
        void* matrixUserData = nullptr;
        AegisGodotViewportProvider viewportProvider = nullptr;
        void* viewportUserData = nullptr;
    };

    std::mutex g_adapterMutex;
    ProviderState g_providers;
    std::vector<AegisGodotObjectSnapshot> g_objects;
    AegisGodotMatrix4x4 g_viewProjection = {};
    AegisGodotViewport g_viewport = {};
    AegisGodotAdapterTiming g_timing = {};
    bool g_hasMatrix = false;
    bool g_hasViewport = false;

    template <std::size_t N>
    void CopyWide(wchar_t (&dest)[N], const wchar_t* value)
    {
        wcsncpy_s(dest, value ? value : L"", _TRUNCATE);
    }

    template <std::size_t N>
    void CopyAnsi(char (&dest)[N], const std::string& value)
    {
        strncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    bool IsFinite(float value)
    {
        return std::isfinite(value);
    }

    bool IsValidViewport(const AegisGodotViewport& viewport)
    {
        return IsFinite(viewport.x) && IsFinite(viewport.y) &&
            IsFinite(viewport.width) && IsFinite(viewport.height) &&
            viewport.width > 1.0f && viewport.height > 1.0f;
    }

    bool IsValidMatrix(const AegisGodotMatrix4x4& matrix)
    {
        bool anyNonZero = false;
        for (float value : matrix.m)
        {
            if (!IsFinite(value))
                return false;
            anyNonZero = anyNonZero || std::fabs(value) > 0.000001f;
        }
        return anyNonZero;
    }

    LARGE_INTEGER NowCounter()
    {
        LARGE_INTEGER value = {};
        ::QueryPerformanceCounter(&value);
        return value;
    }

    double ElapsedMs(LARGE_INTEGER start, LARGE_INTEGER end)
    {
        LARGE_INTEGER frequency = {};
        ::QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart == 0)
            return 0.0;
        return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) /
            static_cast<double>(frequency.QuadPart);
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
    }

    bool Contains(const std::wstring& value, const wchar_t* needle)
    {
        return needle && Lower(value).find(Lower(needle)) != std::wstring::npos;
    }

    bool ContainsAnsi(const char* value, const char* needle)
    {
        if (!value || !needle)
            return false;

        std::string haystack = value;
        std::string target = needle;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        std::transform(target.begin(), target.end(), target.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return haystack.find(target) != std::string::npos;
    }

    bool EqualsAnsi(const char* value, const char* expected)
    {
        if (!value || !expected)
            return false;

        std::string left = value;
        std::string right = expected;
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return left == right;
    }

    bool MainModuleHasPckSection()
    {
        const auto base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
        if (!base)
            return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section)
        {
            char name[9] = {};
            std::memcpy(name, section->Name, 8);
            if (_stricmp(name, "pck") == 0)
                return true;
        }
        return false;
    }

    struct GodotRuntimeProfile
    {
        bool probed = false;
        bool is64 = true;
        bool godot3 = false;
        bool godot4 = false;
        bool gdExtensionStrings = false;
        bool autoScanSupported = false;
        GodotRuntimeBackendKind backend = GodotRuntimeBackendKind::Unknown;
        char exportDllName[96] = {};
        const char* blockReason = nullptr;
    };

    GodotRuntimeProfile g_runtimeProfile;

    bool ImageContainsAsciiString(const std::uint8_t* start, const std::uint8_t* end, const char* needle)
    {
        if (!start || !end || !needle || end <= start)
            return false;

        const size_t needleLen = std::strlen(needle);
        if (needleLen == 0 || static_cast<size_t>(end - start) < needleLen)
            return false;

        for (const std::uint8_t* cursor = start; cursor < end - needleLen; ++cursor)
        {
            if (std::memcmp(cursor, needle, needleLen) == 0)
                return true;
        }
        return false;
    }

    const GodotRuntimeProfile& ProbeGodotRuntimeProfile()
    {
        if (g_runtimeProfile.probed)
            return g_runtimeProfile;

        g_runtimeProfile.probed = true;

        const auto base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
        if (!base)
        {
            g_runtimeProfile.blockReason = "main module unavailable";
            return g_runtimeProfile;
        }

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            g_runtimeProfile.blockReason = "invalid PE image";
            return g_runtimeProfile;
        }

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            g_runtimeProfile.blockReason = "invalid PE image";
            return g_runtimeProfile;
        }

        g_runtimeProfile.is64 = (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64);
        if (!g_runtimeProfile.is64)
        {
            g_runtimeProfile.blockReason = "32-bit Godot executable (x64 DLL only)";
            return g_runtimeProfile;
        }

        const DWORD exportRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (exportRva != 0)
        {
            const auto* exportDir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exportRva);
            if (exportDir->Name != 0)
            {
                const char* exportName = reinterpret_cast<const char*>(base + exportDir->Name);
                strncpy_s(g_runtimeProfile.exportDllName, exportName, _TRUNCATE);

                if (ContainsAnsi(exportName, ".opt.") || ContainsAnsi(exportName, "opt.64"))
                    g_runtimeProfile.godot3 = true;
                if (ContainsAnsi(exportName, "template_") || ContainsAnsi(exportName, "gdextension"))
                    g_runtimeProfile.godot4 = true;
            }
        }

        const std::uint8_t* imageStart = base;
        const std::uint8_t* imageEnd = base + nt->OptionalHeader.SizeOfImage;
        g_runtimeProfile.gdExtensionStrings =
            ImageContainsAsciiString(imageStart, imageEnd, "global_get_singleton") ||
            ImageContainsAsciiString(imageStart, imageEnd, "classdb_get_method_bind") ||
            ImageContainsAsciiString(imageStart, imageEnd, "Attempt to get non-existent interface function");

        const bool godot3Markers =
            ImageContainsAsciiString(imageStart, imageEnd, "godot_nativescript") &&
            ImageContainsAsciiString(imageStart, imageEnd, "SceneTree") &&
            !g_runtimeProfile.gdExtensionStrings;
        if (godot3Markers)
            g_runtimeProfile.godot3 = true;

        if (!g_runtimeProfile.godot4 && g_runtimeProfile.gdExtensionStrings)
            g_runtimeProfile.godot4 = true;

        if (!g_runtimeProfile.godot3 && !g_runtimeProfile.godot4 && MainModuleHasPckSection())
            g_runtimeProfile.godot4 = true;

        if (g_runtimeProfile.godot4)
            g_runtimeProfile.backend = GodotRuntimeBackendKind::Godot4;
        else if (g_runtimeProfile.godot3)
            g_runtimeProfile.backend = GodotRuntimeBackendKind::Godot3;

        if (g_runtimeProfile.backend == GodotRuntimeBackendKind::Godot4 ||
            g_runtimeProfile.backend == GodotRuntimeBackendKind::Godot3)
        {
            g_runtimeProfile.autoScanSupported = true;
        }
        else
        {
            g_runtimeProfile.blockReason = "unrecognized Godot runtime profile (Godot 1/2 not supported)";
        }

        return g_runtimeProfile;
    }

    bool ProjectWithConvention(
        const AegisGodotMatrix4x4& matrix,
        const AegisGodotViewport& viewport,
        const AegisGodotVec3& world,
        bool columnMajor,
        bool openGlDepth,
        bool yFlip,
        AegisGodotProjectedPoint& outPoint)
    {
        const float* m = matrix.m;
        float clipX = 0.0f;
        float clipY = 0.0f;
        float clipZ = 0.0f;
        float clipW = 0.0f;

        if (columnMajor)
        {
            clipX = world.x * m[0] + world.y * m[4] + world.z * m[8] + m[12];
            clipY = world.x * m[1] + world.y * m[5] + world.z * m[9] + m[13];
            clipZ = world.x * m[2] + world.y * m[6] + world.z * m[10] + m[14];
            clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];
        }
        else
        {
            clipX = world.x * m[0] + world.y * m[1] + world.z * m[2] + m[3];
            clipY = world.x * m[4] + world.y * m[5] + world.z * m[6] + m[7];
            clipZ = world.x * m[8] + world.y * m[9] + world.z * m[10] + m[11];
            clipW = world.x * m[12] + world.y * m[13] + world.z * m[14] + m[15];
        }

        if (!IsFinite(clipX) || !IsFinite(clipY) || !IsFinite(clipZ) || !IsFinite(clipW) || std::fabs(clipW) < 0.00001f)
            return false;

        const float ndcX = clipX / clipW;
        const float ndcY = clipY / clipW;
        const float ndcZ = clipZ / clipW;
        if (!IsFinite(ndcX) || !IsFinite(ndcY) || !IsFinite(ndcZ))
            return false;

        outPoint.x = viewport.x + ((ndcX * 0.5f) + 0.5f) * viewport.width;
        outPoint.y = viewport.y + (yFlip ? ((ndcY * 0.5f) + 0.5f) : (0.5f - (ndcY * 0.5f))) * viewport.height;
        outPoint.depth = ndcZ;

        const bool depthClipped = openGlDepth ? (ndcZ < -1.0f || ndcZ > 1.0f) : (ndcZ < 0.0f || ndcZ > 1.0f);
        const bool xyClipped = ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f;
        outPoint.clipped = (xyClipped || depthClipped || clipW < 0.0f) ? 1 : 0;
        return IsFinite(outPoint.x) && IsFinite(outPoint.y);
    }

    bool ProjectCurrentLocked(const AegisGodotVec3& world, AegisGodotProjectedPoint& outPoint)
    {
        if (!g_hasMatrix || !g_hasViewport || !IsValidMatrix(g_viewProjection) || !IsValidViewport(g_viewport))
            return false;

        const std::uint32_t flags = g_viewProjection.flags;
        const bool wantsColumn = (flags & AegisGodotMatrix_ColumnMajor) != 0;
        const bool wantsRow = (flags & AegisGodotMatrix_RowMajor) != 0;
        const bool openGlDepth = (flags & AegisGodotMatrix_OpenGLDepth) != 0 || (flags & AegisGodotMatrix_D3DDepth) == 0;
        const bool yFlip = (flags & AegisGodotMatrix_YFlip) != 0;

        if (wantsColumn || wantsRow)
            return ProjectWithConvention(g_viewProjection, g_viewport, world, wantsColumn, openGlDepth, yFlip, outPoint);

        AegisGodotProjectedPoint row = {};
        if (ProjectWithConvention(g_viewProjection, g_viewport, world, false, openGlDepth, yFlip, row) && !row.clipped)
        {
            outPoint = row;
            return true;
        }

        AegisGodotProjectedPoint column = {};
        if (ProjectWithConvention(g_viewProjection, g_viewport, world, true, openGlDepth, yFlip, column))
        {
            outPoint = column;
            return true;
        }

        if (ProjectWithConvention(g_viewProjection, g_viewport, world, false, openGlDepth, yFlip, row))
        {
            outPoint = row;
            return true;
        }
        return false;
    }

    void RebuildProjectionStatsLocked()
    {
        g_timing.objectCount = static_cast<std::uint32_t>(g_objects.size());
        g_timing.projectedCount = 0;
        g_timing.clippedCount = 0;

        for (const AegisGodotObjectSnapshot& object : g_objects)
        {
            if (object.flags & AegisGodotObject_Node2D)
            {
                ++g_timing.projectedCount;
                continue;
            }

            AegisGodotProjectedPoint point = {};
            if (!ProjectCurrentLocked(object.origin, point))
                continue;
            if (point.clipped)
                ++g_timing.clippedCount;
            else
                ++g_timing.projectedCount;
        }
    }

    std::string JsonEscape(const char* value)
    {
        std::string escaped;
        if (!value)
            return escaped;

        for (const char* cursor = value; *cursor; ++cursor)
        {
            switch (*cursor)
            {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(*cursor);
                break;
            }
        }
        return escaped;
    }

    std::string ExtractJsonString(const std::string& object, const char* key)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return {};
        pos = object.find(':', pos);
        pos = object.find('"', pos);
        if (pos == std::string::npos)
            return {};
        ++pos;

        std::string result;
        for (; pos < object.size(); ++pos)
        {
            const char ch = object[pos];
            if (ch == '\\' && pos + 1 < object.size())
            {
                result.push_back(object[pos + 1]);
                ++pos;
                continue;
            }
            if (ch == '"')
                break;
            result.push_back(ch);
        }
        return result;
    }

    bool ExtractJsonNumber(const std::string& object, const char* key, float& outValue)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return false;
        pos = object.find(':', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        const float value = std::strtof(object.c_str() + pos + 1, &end);
        if (end == object.c_str() + pos + 1 || !IsFinite(value))
            return false;
        outValue = value;
        return true;
    }

    bool ExtractJsonUInt64(const std::string& object, const char* key, std::uint64_t& outValue)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return false;
        pos = object.find(':', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        const unsigned long long value = std::strtoull(object.c_str() + pos + 1, &end, 10);
        if (end == object.c_str() + pos + 1)
            return false;
        outValue = static_cast<std::uint64_t>(value);
        return true;
    }

    bool ExtractJsonInt(const std::string& object, const char* key, std::int32_t& outValue)
    {
        float value = 0.0f;
        if (!ExtractJsonNumber(object, key, value))
            return false;
        outValue = static_cast<std::int32_t>(value);
        return true;
    }

    bool ExtractJsonVec3(const std::string& object, const char* key, AegisGodotVec3& outValue)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return false;
        pos = object.find('[', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        outValue.x = std::strtof(object.c_str() + pos + 1, &end);
        if (!end || *end == '\0')
            return false;
        outValue.y = std::strtof(end + 1, &end);
        if (!end || *end == '\0')
            return false;
        outValue.z = std::strtof(end + 1, &end);
        return IsFinite(outValue.x) && IsFinite(outValue.y) && IsFinite(outValue.z);
    }

    bool ExtractJsonMatrix(const std::string& json, AegisGodotMatrix4x4& outMatrix)
    {
        std::size_t pos = json.find("\"m\"");
        if (pos == std::string::npos)
            return false;
        pos = json.find('[', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        const char* cursor = json.c_str() + pos + 1;
        for (float& value : outMatrix.m)
        {
            value = std::strtof(cursor, &end);
            if (end == cursor || !IsFinite(value))
                return false;
            cursor = end + 1;
        }

        float flags = 0.0f;
        if (ExtractJsonNumber(json, "matrixFlags", flags))
            outMatrix.flags = static_cast<std::uint32_t>(flags);
        return true;
    }

    std::wstring DetectRendererBackend()
    {
        if (!AegisUniversal_IsInitialized())
            AegisUniversal_Initialize();

        bool hasVulkan = false;
        bool hasOpenGl = false;
        bool hasD3D11 = false;
        bool hasD3D12 = false;
        const std::uint32_t count = AegisUniversal_GetModuleCount();
        for (std::uint32_t index = 0; index < count; ++index)
        {
            AegisUniversalModuleInfo module = {};
            if (!AegisUniversal_GetModuleInfo(index, &module))
                continue;
            const std::wstring name = module.name;
            hasVulkan = hasVulkan || Contains(name, L"vulkan");
            hasOpenGl = hasOpenGl || Contains(name, L"opengl32");
            hasD3D11 = hasD3D11 || Contains(name, L"d3d11") || Contains(name, L"dxgi");
            hasD3D12 = hasD3D12 || Contains(name, L"d3d12");
        }

        if (hasVulkan)
            return L"Vulkan";
        if (hasD3D12)
            return L"Direct3D12";
        if (hasD3D11)
            return L"Direct3D11";
        if (hasOpenGl)
            return L"OpenGL";
        return L"Unknown";
    }

    void MaybeInitAutoProvidersOnGameThread()
    {
        static DWORD s_firstTick = 0;
        if (s_firstTick == 0)
            s_firstTick = ::GetTickCount();
        if (::GetTickCount() - s_firstTick < 8000)
            return;

        AegisGodot_InitAutoProviders();
    }
}

AEGIS_UNIVERSAL_API void AegisGodot_RegisterObjectProvider(AegisGodotObjectProvider provider, void* userData)
{
    std::lock_guard lock(g_adapterMutex);
    g_providers.objectProvider = provider;
    g_providers.objectUserData = userData;
}

AEGIS_UNIVERSAL_API void AegisGodot_RegisterViewProjectionProvider(AegisGodotViewProjectionProvider provider, void* userData)
{
    std::lock_guard lock(g_adapterMutex);
    g_providers.matrixProvider = provider;
    g_providers.matrixUserData = userData;
}

AEGIS_UNIVERSAL_API void AegisGodot_RegisterViewportProvider(AegisGodotViewportProvider provider, void* userData)
{
    std::lock_guard lock(g_adapterMutex);
    g_providers.viewportProvider = provider;
    g_providers.viewportUserData = userData;
}

AEGIS_UNIVERSAL_API int AegisGodot_UpdateProviders()
{
    MaybeInitAutoProvidersOnGameThread();

    ProviderState providers = {};
    {
        std::lock_guard lock(g_adapterMutex);
        providers = g_providers;
    }

    std::vector<AegisGodotObjectSnapshot> objects(kMaxObjects);
    std::uint32_t objectCount = 0;
    double objectMs = 0.0;
    double matrixMs = 0.0;
    double viewportMs = 0.0;
    bool hasMatrix = false;
    bool hasViewport = false;
    AegisGodotMatrix4x4 matrix = {};
    AegisGodotViewport viewport = {};

    if (providers.viewportProvider)
    {
        const LARGE_INTEGER start = NowCounter();
        hasViewport = providers.viewportProvider(&viewport, providers.viewportUserData) != 0 && IsValidViewport(viewport);
        viewportMs = ElapsedMs(start, NowCounter());
    }

    if (providers.objectProvider)
    {
        const LARGE_INTEGER start = NowCounter();
        objectCount = providers.objectProvider(objects.data(), kMaxObjects, providers.objectUserData);
        objectMs = ElapsedMs(start, NowCounter());
        objectCount = std::min<std::uint32_t>(objectCount, kMaxObjects);
        objects.resize(objectCount);
    }
    else
    {
        objects.clear();
    }

    if (providers.matrixProvider)
    {
        const LARGE_INTEGER start = NowCounter();
        hasMatrix = providers.matrixProvider(&matrix, providers.matrixUserData) != 0 && IsValidMatrix(matrix);
        matrixMs = ElapsedMs(start, NowCounter());
    }

    std::lock_guard lock(g_adapterMutex);
    if (providers.objectProvider)
        g_objects = std::move(objects);
    if (providers.matrixProvider)
    {
        g_viewProjection = matrix;
        g_hasMatrix = hasMatrix;
    }
    if (providers.viewportProvider)
    {
        g_viewport = viewport;
        g_hasViewport = hasViewport;
    }
    g_timing.objectProviderMs = objectMs;
    g_timing.matrixProviderMs = matrixMs;
    g_timing.viewportProviderMs = viewportMs;
    ++g_timing.frameId;
    RebuildProjectionStatsLocked();
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_SubmitObjectSnapshots(const AegisGodotObjectSnapshot* objects, std::uint32_t count)
{
    if (!objects && count != 0)
        return 0;

    const std::uint32_t clampedCount = std::min<std::uint32_t>(count, kMaxObjects);
    std::lock_guard lock(g_adapterMutex);
    g_objects.assign(objects, objects + clampedCount);
    ++g_timing.frameId;
    RebuildProjectionStatsLocked();
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_SubmitViewProjection(const AegisGodotMatrix4x4* matrix)
{
    if (!matrix || !IsValidMatrix(*matrix))
        return 0;

    std::lock_guard lock(g_adapterMutex);
    g_viewProjection = *matrix;
    g_hasMatrix = true;
    RebuildProjectionStatsLocked();
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_SubmitViewport(const AegisGodotViewport* viewport)
{
    if (!viewport || !IsValidViewport(*viewport))
        return 0;

    std::lock_guard lock(g_adapterMutex);
    g_viewport = *viewport;
    g_hasViewport = true;
    RebuildProjectionStatsLocked();
    return 1;
}

AEGIS_UNIVERSAL_API std::uint32_t AegisGodot_GetObjectCount()
{
    std::lock_guard lock(g_adapterMutex);
    return static_cast<std::uint32_t>(g_objects.size());
}

AEGIS_UNIVERSAL_API int AegisGodot_GetObjectSnapshot(std::uint32_t index, AegisGodotObjectSnapshot* outObject)
{
    if (!outObject)
        return 0;

    std::lock_guard lock(g_adapterMutex);
    if (index >= g_objects.size())
        return 0;
    *outObject = g_objects[index];
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_GetAdapterTiming(AegisGodotAdapterTiming* outTiming)
{
    if (!outTiming)
        return 0;
    std::lock_guard lock(g_adapterMutex);
    *outTiming = g_timing;
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_GetCapabilityInfo(AegisGodotCapabilityInfo* outInfo)
{
    if (!outInfo)
        return 0;

    if (!AegisUniversal_IsInitialized())
        AegisUniversal_Initialize();

    AegisUniversalRuntimeInfo runtime = {};
    AegisUniversal_GetRuntimeInfo(&runtime);

    bool gdNative = false;
    bool gdExtension = false;
    bool steam = false;
    bool jolt = false;
    bool terrain = false;
    const std::uint32_t exportCount = AegisUniversal_GetMatchedExportCount();
    for (std::uint32_t index = 0; index < exportCount; ++index)
    {
        AegisUniversalExportInfo exportInfo = {};
        if (!AegisUniversal_GetMatchedExportInfo(index, &exportInfo))
            continue;

        gdNative = gdNative || ContainsAnsi(exportInfo.exportName, "godot_gdnative") || ContainsAnsi(exportInfo.exportName, "godot_nativescript");
        gdExtension = gdExtension || ContainsAnsi(exportInfo.exportName, "gdextension_library_init");
        steam = steam || ContainsAnsi(exportInfo.exportName, "godotsteam_init");
        jolt = jolt || ContainsAnsi(exportInfo.exportName, "godot_jolt_main");
        terrain = terrain || ContainsAnsi(exportInfo.exportName, "terrain_3d_init");
    }

    const std::uint32_t moduleCount = AegisUniversal_GetModuleCount();
    for (std::uint32_t index = 0; index < moduleCount; ++index)
    {
        AegisUniversalModuleInfo module = {};
        if (!AegisUniversal_GetModuleInfo(index, &module))
            continue;
        steam = steam || Contains(module.name, L"godotsteam");
        jolt = jolt || Contains(module.name, L"godot-jolt");
        terrain = terrain || Contains(module.name, L"terrain");
    }

    std::lock_guard lock(g_adapterMutex);
    *outInfo = {};
    outInfo->godotDetected = (runtime.flags & AegisUniversalRuntime_EngineDetected) || MainModuleHasPckSection() ? 1 : 0;
    outInfo->embeddedPackSectionFound = MainModuleHasPckSection() ? 1 : 0;
    outInfo->gdNativeExportFound = gdNative ? 1 : 0;
    outInfo->gdExtensionExportFound = gdExtension ? 1 : 0;
    outInfo->godotSteamFound = steam ? 1 : 0;
    outInfo->joltExtensionFound = jolt ? 1 : 0;
    outInfo->terrainExtensionFound = terrain ? 1 : 0;
    outInfo->objectProviderRegistered = g_providers.objectProvider ? 1 : 0;
    outInfo->viewProjectionProviderRegistered = g_providers.matrixProvider ? 1 : 0;
    outInfo->viewportProviderRegistered = g_providers.viewportProvider ? 1 : 0;
    outInfo->viewportValid = (g_hasViewport && IsValidViewport(g_viewport)) ? 1 : 0;
    outInfo->matrixValid = (g_hasMatrix && IsValidMatrix(g_viewProjection)) ? 1 : 0;
    outInfo->snapshotReady = !g_objects.empty() ? 1 : 0;

    AegisGodotProjectedPoint point = {};
    const AegisGodotVec3 origin = { 0.0f, 0.0f, 0.0f };
    outInfo->w2sProjectionWorking = ProjectCurrentLocked(origin, point) ? 1 : 0;
    CopyWide(outInfo->rendererBackend, DetectRendererBackend().c_str());

    const GodotRuntimeProfile& profile = ProbeGodotRuntimeProfile();

    std::wstringstream details;
    details << L"profile "
            << (profile.backend == GodotRuntimeBackendKind::Godot4 ? L"Godot4" :
                profile.backend == GodotRuntimeBackendKind::Godot3 ? L"Godot3" : L"unknown")
            << (profile.is64 ? L"/x64" : L"/x86")
            << L", backend " << GetGodotAutoScanBackend().label;
    if (profile.exportDllName[0] != '\0')
    {
        wchar_t exportWide[96] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, profile.exportDllName, -1, exportWide, static_cast<int>(std::size(exportWide)));
        details << L", export " << exportWide;
    }
    details << L", resolver live-scan only (no static offsets)"
            << L", auto-scan " << (profile.autoScanSupported ? L"supported" : L"blocked")
            << L", modules " << runtime.moduleCount
            << L", matched exports " << runtime.matchedExportCount
            << L", pck " << (outInfo->embeddedPackSectionFound ? L"yes" : L"no")
            << L", objects " << g_objects.size();
    if (profile.blockReason)
        details << L", note " << profile.blockReason;
    CopyWide(outInfo->details, details.str().c_str());
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_ProjectWorldToScreen(const AegisGodotVec3* world, AegisGodotProjectedPoint* outPoint)
{
    if (!world || !outPoint)
        return 0;

    std::lock_guard lock(g_adapterMutex);
    *outPoint = {};
    return ProjectCurrentLocked(*world, *outPoint) ? 1 : 0;
}

AEGIS_UNIVERSAL_API int AegisGodot_WriteSnapshotJson(const wchar_t* path)
{
    if (!path || !path[0])
        return 0;

    std::lock_guard lock(g_adapterMutex);
    std::wofstream out(std::filesystem::path(path), std::ios::trunc);
    if (!out)
        return 0;

    out << L"{\n";
    out << L"  \"engine\": \"Godot\",\n";
    out << L"  \"frameId\": " << g_timing.frameId << L",\n";
    out << L"  \"viewport\": { \"x\": " << g_viewport.x << L", \"y\": " << g_viewport.y
        << L", \"width\": " << g_viewport.width << L", \"height\": " << g_viewport.height << L" },\n";
    out << L"  \"matrixFlags\": " << g_viewProjection.flags << L",\n";
    out << L"  \"m\": [";
    for (std::size_t index = 0; index < 16; ++index)
    {
        if (index)
            out << L", ";
        out << g_viewProjection.m[index];
    }
    out << L"],\n";
    out << L"  \"timing\": { \"objectProviderMs\": " << g_timing.objectProviderMs
        << L", \"matrixProviderMs\": " << g_timing.matrixProviderMs
        << L", \"viewportProviderMs\": " << g_timing.viewportProviderMs
        << L", \"projected\": " << g_timing.projectedCount
        << L", \"clipped\": " << g_timing.clippedCount << L" },\n";
    out << L"  \"objects\": [\n";
    for (std::size_t index = 0; index < g_objects.size(); ++index)
    {
        const AegisGodotObjectSnapshot& object = g_objects[index];
        out << L"    { \"id\": " << object.id
            << L", \"name\": \"" << JsonEscape(object.name).c_str()
            << L"\", \"className\": \"" << JsonEscape(object.className).c_str()
            << L"\", \"path\": \"" << JsonEscape(object.path).c_str()
            << L"\", \"origin\": [" << object.origin.x << L", " << object.origin.y << L", " << object.origin.z
            << L"], \"boundsMin\": [" << object.boundsMin.x << L", " << object.boundsMin.y << L", " << object.boundsMin.z
            << L"], \"boundsMax\": [" << object.boundsMax.x << L", " << object.boundsMax.y << L", " << object.boundsMax.z
            << L"], \"group\": " << object.group
            << L", \"visible\": " << object.visible
            << L", \"flags\": " << object.flags << L" }";
        if (index + 1 < g_objects.size())
            out << L",";
        out << L"\n";
    }
    out << L"  ]\n";
    out << L"}\n";
    return 1;
}

AEGIS_UNIVERSAL_API int AegisGodot_LoadSnapshotJson(const wchar_t* path)
{
    if (!path || !path[0])
        return 0;

    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in)
        return 0;

    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    AegisGodotViewport viewport = {};
    ExtractJsonNumber(json, "x", viewport.x);
    ExtractJsonNumber(json, "y", viewport.y);
    ExtractJsonNumber(json, "width", viewport.width);
    ExtractJsonNumber(json, "height", viewport.height);

    AegisGodotMatrix4x4 matrix = {};
    const bool hasMatrix = ExtractJsonMatrix(json, matrix);

    std::vector<AegisGodotObjectSnapshot> objects;
    std::size_t arrayPos = json.find("\"objects\"");
    if (arrayPos != std::string::npos)
    {
        std::size_t pos = json.find('{', arrayPos);
        while (pos != std::string::npos)
        {
            const std::size_t end = json.find('}', pos);
            if (end == std::string::npos)
                break;
            const std::string objectJson = json.substr(pos, end - pos + 1);

            AegisGodotObjectSnapshot object = {};
            ExtractJsonUInt64(objectJson, "id", object.id);
            CopyAnsi(object.name, ExtractJsonString(objectJson, "name"));
            CopyAnsi(object.className, ExtractJsonString(objectJson, "className"));
            CopyAnsi(object.path, ExtractJsonString(objectJson, "path"));
            ExtractJsonVec3(objectJson, "origin", object.origin);
            ExtractJsonVec3(objectJson, "boundsMin", object.boundsMin);
            ExtractJsonVec3(objectJson, "boundsMax", object.boundsMax);
            ExtractJsonInt(objectJson, "group", object.group);
            ExtractJsonInt(objectJson, "visible", object.visible);
            std::int32_t flags = 0;
            ExtractJsonInt(objectJson, "flags", flags);
            object.flags = static_cast<std::uint32_t>(flags);
            objects.push_back(object);

            pos = json.find('{', end + 1);
        }
    }

    std::lock_guard lock(g_adapterMutex);
    if (IsValidViewport(viewport))
    {
        g_viewport = viewport;
        g_hasViewport = true;
    }
    if (hasMatrix && IsValidMatrix(matrix))
    {
        g_viewProjection = matrix;
        g_hasMatrix = true;
    }
    g_objects = std::move(objects);
    ++g_timing.frameId;
    RebuildProjectionStatsLocked();
    return 1;
}

AEGIS_UNIVERSAL_API void AegisGodot_PrintCurrentObjects()
{
    std::vector<AegisGodotObjectSnapshot> snapshot;
    {
        std::lock_guard lock(g_adapterMutex);
        snapshot = g_objects;
    }

    HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    for (const AegisGodotObjectSnapshot& object : snapshot)
    {
        std::ostringstream line;
        line << "[GodotUniversal] Object id=" << object.id
             << " name=\"" << object.name
             << "\" class=\"" << object.className
             << "\" path=\"" << object.path
             << "\" origin=(" << object.origin.x << ", " << object.origin.y << ", " << object.origin.z << ")\n";
        const std::string text = line.str();
        ::OutputDebugStringA(text.c_str());
        if (output && output != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            if (!::WriteConsoleA(output, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr))
                ::WriteFile(output, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
    }
}

namespace
{
    std::vector<AegisGodotMemberInfo> g_scannedMembers;
    std::mutex g_scannedMembersMutex;

    using GDExtensionInterfaceGetProcAddress = void*(__stdcall*)(const char* p_name);
    using GDExtensionInterfaceClassdbGetMethodBind = void*(__stdcall*)(const void* p_class_name, const void* p_method_name, int64_t p_hash);
    using GDExtensionInterfaceObjectCallMethodBind = void(__stdcall*)(void* p_method_bind, void* p_instance, const void** p_args, int64_t p_arg_count, void* r_ret, void* r_error);
    using GDExtensionInterfaceVariantGetPtrBuiltInMethod = void*(__stdcall*)(int32_t p_type, const char* p_method, int64_t p_hash);
    using GDExtensionInterfaceVariantGetPtrConstructor = void*(__stdcall*)(int32_t p_type, int32_t p_constructor);
    using GDExtensionInterfaceVariantGetPtrDestructor = void*(__stdcall*)(int32_t p_type);
    using GDExtensionInterfaceGetVariantFromTypeConstructor = void*(__stdcall*)(int32_t p_type);
    using GDExtensionInterfaceStringNameNewWithUtf8Chars = void(__stdcall*)(void* r_dest, const char* p_contents);
    using GDExtensionInterfaceStringNewWithUtf8Chars = void(__stdcall*)(void* r_dest, const char* p_contents);
    using GDExtensionInterfaceStringNameOperatorString = void(__stdcall*)(const void* p_self, void* r_dest);

    using GDExtensionPtrConstructor = void(__stdcall*)(void* p_self, const void** p_args);
    using GDExtensionPtrDestructor = void(__stdcall*)(void* p_self);
    using GDExtensionVariantFromTypeConstructorFunc = void(__stdcall*)(void* r_variant, void* p_value);
    using GDExtGlobalGetSingletonFn = void*(__stdcall*)(const void* p_name);

    constexpr int32_t kGodotVariantTypeString = 4;
    constexpr int32_t kGodotVariantTypeStringName = 21;

    std::mutex g_gdextensionResolveMutex;

    GDExtensionInterfaceGetProcAddress g_get_proc_address = nullptr;
    void* g_get_interface_function_impl = nullptr;
    bool g_gdextension_resolution_done = false;
    bool g_gdextension_resolution_ok = false;
    DWORD g_gdextension_retry_after_tick = 0;
    GDExtensionInterfaceClassdbGetMethodBind g_classdb_get_method_bind = nullptr;
    GDExtensionInterfaceObjectCallMethodBind g_object_call_method_bind = nullptr;
    GDExtensionInterfaceVariantGetPtrBuiltInMethod g_variant_get_ptr_builtin_method = nullptr;
    GDExtensionInterfaceVariantGetPtrConstructor g_variant_get_ptr_constructor = nullptr;
    GDExtensionInterfaceVariantGetPtrDestructor g_variant_get_ptr_destructor = nullptr;
    GDExtensionInterfaceGetVariantFromTypeConstructor g_get_variant_from_type_constructor = nullptr;
    GDExtensionInterfaceStringNameNewWithUtf8Chars g_string_name_new_with_utf8_chars = nullptr;
    GDExtensionInterfaceStringNewWithUtf8Chars g_string_new_with_utf8_chars = nullptr;
    GDExtensionInterfaceStringNameOperatorString g_string_name_operator_string = nullptr;
    GDExtGlobalGetSingletonFn g_global_get_singleton = nullptr;

    bool BuildStringNameFromUtf8(const char* utf8, char (&outStringName)[64]);

    struct AutoStringName
    {
        char buf[64] = {};
        AutoStringName(const char* utf8)
        {
            if (g_string_name_new_with_utf8_chars)
                g_string_name_new_with_utf8_chars(buf, utf8);
            else
                BuildStringNameFromUtf8(utf8, buf);
        }
        ~AutoStringName()
        {
            if (g_variant_get_ptr_destructor)
            {
                auto dtor = reinterpret_cast<GDExtensionPtrDestructor>(g_variant_get_ptr_destructor(21));
                if (dtor) dtor(buf);
            }
        }
        AutoStringName(const AutoStringName&) = delete;
        AutoStringName& operator=(const AutoStringName&) = delete;
    };

    void* GetClassdbMethodBind(const char* className, const char* methodName, int64_t hash = 0)
    {
        if (!g_classdb_get_method_bind)
            return nullptr;
        AutoStringName cls(className);
        AutoStringName method(methodName);
        return g_classdb_get_method_bind(cls.buf, method.buf, hash);
    }

    bool IsRuntimeFunctionStart(uint8_t* ptr, uint8_t* mainStart)
    {
        const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION))
            return false;

        const auto rva = static_cast<DWORD>(ptr - mainStart);
        const auto functions = reinterpret_cast<const RUNTIME_FUNCTION*>(mainStart + dir.VirtualAddress);
        const auto count = dir.Size / sizeof(RUNTIME_FUNCTION);
        std::size_t lo = 0;
        std::size_t hi = count;

        while (lo < hi)
        {
            const std::size_t mid = lo + ((hi - lo) / 2);
            const auto& fn = functions[mid];

            if (rva < fn.BeginAddress)
            {
                hi = mid;
            }
            else if (rva >= fn.EndAddress)
            {
                lo = mid + 1;
            }
            else
            {
                return rva == fn.BeginAddress;
            }
        }

        return false;
    }

    bool IsLikelyFunctionPrologue(uint8_t* ptr, uint8_t* mainStart)
    {
        // On x64, the .pdata unwind table is the best cheap guard against
        // jumping into the middle of a function. The previous byte-pattern scan
        // accepted instruction fragments like "sub rsp" as fresh starts.
        if (!IsRuntimeFunctionStart(ptr, mainStart))
            return false;

        if (ptr[0] == 0x41 && ptr[1] == 0x57 && ptr[2] == 0x41 && ptr[3] == 0x56)
            return true; // push r15; push r14
        if (ptr[0] == 0x40 && ptr[1] == 0x53)
            return true; // push rbx
        if (ptr[0] == 0x48 && ptr[1] == 0x89 && ptr[2] == 0x5C)
            return true; // mov [rsp+X], rbx
        if (ptr[0] == 0x48 && ptr[1] == 0x83 && ptr[2] == 0xEC)
            return true; // sub rsp, X
        if (ptr[0] == 0x48 && ptr[1] == 0x8B && ptr[2] == 0xC4)
            return true; // mov rax, rsp
        if (ptr[0] == 0x55)
            return true; // push rbp
        return false;
    }

    bool IsValidInterfacePointer(void* ptr, uint8_t* mainStart, uint8_t* mainEnd)
    {
        return ptr >= mainStart && ptr < mainEnd;
    }

    bool ValidateStringNewFunction(GDExtensionInterfaceStringNewWithUtf8Chars candidate)
    {
        if (!candidate)
            return false;

        alignas(8) char stringBuf[64] = {};
        __try
        {
            candidate(stringBuf, "Aegis");
            const void* stringData = *reinterpret_cast<void* const*>(stringBuf);
            return stringData != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ValidateStringNameNewFunction(GDExtensionInterfaceStringNameNewWithUtf8Chars candidate)
    {
        if (!candidate || !g_get_interface_function_impl)
            return false;

        alignas(8) char stringNameBuf[64] = {};
        __try
        {
            candidate(stringNameBuf, "classdb_get_method_bind");
            using GetInterfaceFunctionFn = void*(__stdcall*)(const void*);
            void* methodBind = reinterpret_cast<GetInterfaceFunctionFn>(g_get_interface_function_impl)(stringNameBuf);
            return methodBind != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogMsg("[AegisGodot] ValidateStringNameNewFunction: SEH exception 0x%08X\n", ::GetExceptionCode());
            return false;
        }
    }

    bool BuildApiStringName(const char* utf8, char (&outStringName)[64])
    {
        if (g_string_name_new_with_utf8_chars)
        {
            g_string_name_new_with_utf8_chars(outStringName, utf8);
            return true;
        }
        return BuildStringNameFromUtf8(utf8, outStringName);
    }

    int64_t LookupGodot4MethodHash(const char* className, const char* methodName)
    {
        struct MethodHash
        {
            const char* className;
            const char* methodName;
            int64_t hash;
        };

        static constexpr MethodHash kHashes[] = {
            { "Engine", "get_main_loop", 1016888095 },
            { "SceneTree", "get_root", 1757182445 },
            { "Node", "get_child_count", 894402480 },
            { "Node", "get_child", 541253412 },
            { "Node", "get_name", 2002593661 },
            { "Object", "get_class", 201670096 },
            { "Object", "is_class", 3927539163 },
            { "Node3D", "get_global_position", 3360562783 },
            { "Node3D", "is_visible_in_tree", 36873697 },
            { "Camera3D", "get_camera_projection", 2910717950 },
            { "Camera3D", "get_camera_transform", 3229777777 },
            { "CanvasItem", "get_global_transform_with_canvas", 3814499831 },
            { "CanvasItem", "get_screen_transform", 3814499831 },
            { "CanvasItem", "is_visible_in_tree", 36873697 },
        };

        if (!className || !methodName)
            return 0;

        for (const MethodHash& entry : kHashes)
        {
            if (std::strcmp(entry.className, className) == 0 &&
                std::strcmp(entry.methodName, methodName) == 0)
            {
                return entry.hash;
            }
        }

        return 0;
    }

    bool ValidateResolvedGodot4Api(uint8_t* mainStart, uint8_t* mainEnd)
    {
        if (!g_global_get_singleton || !g_classdb_get_method_bind)
            return false;

        if (!IsValidInterfacePointer(reinterpret_cast<void*>(g_global_get_singleton), mainStart, mainEnd))
            return false;
        if (!IsValidInterfacePointer(reinterpret_cast<void*>(g_classdb_get_method_bind), mainStart, mainEnd))
            return false;

        alignas(8) char sceneTreeName[64] = {};
        alignas(8) char getRootName[64] = {};
        alignas(8) char sceneTreeClass[64] = {};
        __try
        {
            LogMsg("[AegisGodot] ValidateResolvedGodot4Api: building SceneTree StringName...\n");
            if (!BuildApiStringName("SceneTree", sceneTreeName))
            {
                LogMsg("[AegisGodot] ValidateResolvedGodot4Api: BuildApiStringName(SceneTree) failed\n");
                return false;
            }

            LogMsg("[AegisGodot] ValidateResolvedGodot4Api: probing global_get_singleton(SceneTree)...\n");
            void* sceneTree = g_global_get_singleton(sceneTreeName);
            if (!sceneTree)
            {
                LogMsg("[AegisGodot] ValidateResolvedGodot4Api: SceneTree singleton is null\n");
                return false;
            }

            LogMsg("[AegisGodot] ValidateResolvedGodot4Api: building get_root/StringName probes...\n");
            if (!BuildApiStringName("get_root", getRootName) ||
                !BuildApiStringName("SceneTree", sceneTreeClass))
            {
                LogMsg("[AegisGodot] ValidateResolvedGodot4Api: method/class StringName build failed\n");
                return false;
            }

            LogMsg("[AegisGodot] ValidateResolvedGodot4Api: probing classdb_get_method_bind(SceneTree.get_root)...\n");
            void* getRootBind = g_classdb_get_method_bind(
                sceneTreeClass,
                getRootName,
                LookupGodot4MethodHash("SceneTree", "get_root"));
            LogMsg("[AegisGodot] ValidateResolvedGodot4Api: get_root bind = %p\n", getRootBind);
            return getRootBind != nullptr &&
                IsValidInterfacePointer(getRootBind, mainStart, mainEnd);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogMsg("[AegisGodot] ValidateResolvedGodot4Api: SEH exception 0x%08X\n", ::GetExceptionCode());
            return false;
        }
    }

    std::wstring DefaultResolverReportPath()
    {
        wchar_t tempPath[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, tempPath) == 0)
            return L"Godot_Universal_Resolver.json";

        std::wstring path = tempPath;
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');
        path += L"Godot_Universal_Resolver.json";
        return path;
    }

    void WriteDefaultResolverReport()
    {
        const std::wstring path = DefaultResolverReportPath();
        GodotResolver_WriteReport(path.c_str());
    }

    void RecordGodot4ResolverState(const char* sourceLabel)
    {
        GodotResolver_Record("get_proc_address", sourceLabel, reinterpret_cast<void*>(g_get_proc_address));
        GodotResolver_Record("get_interface_function", sourceLabel, g_get_interface_function_impl);
        GodotResolver_Record("classdb_get_method_bind", sourceLabel, reinterpret_cast<void*>(g_classdb_get_method_bind));
        GodotResolver_Record("object_method_bind_call", sourceLabel, reinterpret_cast<void*>(g_object_call_method_bind));
        GodotResolver_Record("global_get_singleton", sourceLabel, reinterpret_cast<void*>(g_global_get_singleton));
        GodotResolver_Record("string_new_with_utf8_chars", sourceLabel, reinterpret_cast<void*>(g_string_new_with_utf8_chars));
        GodotResolver_Record("string_name_new_with_utf8_chars", sourceLabel, reinterpret_cast<void*>(g_string_name_new_with_utf8_chars));
        GodotResolver_Record("string_name_operator_string", sourceLabel, reinterpret_cast<void*>(g_string_name_operator_string));
        GodotResolver_Record("variant_get_ptr_constructor", sourceLabel, reinterpret_cast<void*>(g_variant_get_ptr_constructor));
        GodotResolver_Record("variant_get_ptr_destructor", sourceLabel, reinterpret_cast<void*>(g_variant_get_ptr_destructor));
    }

    void ResetGodot4ApiResolution()
    {
        GodotResolver_Clear();
        g_gdextension_resolution_done = false;
        g_gdextension_resolution_ok = false;
        g_get_proc_address = nullptr;
        g_get_interface_function_impl = nullptr;
        g_classdb_get_method_bind = nullptr;
        g_object_call_method_bind = nullptr;
        g_variant_get_ptr_builtin_method = nullptr;
        g_variant_get_ptr_constructor = nullptr;
        g_variant_get_ptr_destructor = nullptr;
        g_get_variant_from_type_constructor = nullptr;
        g_string_name_new_with_utf8_chars = nullptr;
        g_string_new_with_utf8_chars = nullptr;
        g_string_name_operator_string = nullptr;
        g_global_get_singleton = nullptr;
    }

    bool BuildStringNameFromUtf8(const char* utf8, char (&outStringName)[64])
    {
        if (!utf8 || !utf8[0] || !g_string_new_with_utf8_chars || !g_variant_get_ptr_constructor)
            return false;

        alignas(8) char stringBuf[64] = {};
        __try
        {
            g_string_new_with_utf8_chars(stringBuf, utf8);
            if (!*reinterpret_cast<void* const*>(stringBuf))
                return false;

            for (int32_t ctorIndex = 0; ctorIndex < 4; ++ctorIndex)
            {
                auto ctor = reinterpret_cast<GDExtensionPtrConstructor>(
                    g_variant_get_ptr_constructor(kGodotVariantTypeStringName, ctorIndex));
                if (!ctor)
                    continue;

                memset(outStringName, 0, sizeof(outStringName));
                const void* args[1] = { stringBuf };
                ctor(outStringName, args);

                if (outStringName[0] || outStringName[1] || outStringName[sizeof(outStringName) - 1])
                    return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogMsg("[AegisGodot] BuildStringNameFromUtf8: SEH exception 0x%08X for '%s'\n",
                ::GetExceptionCode(),
                utf8 ? utf8 : "(null)");
            return false;
        }

        return false;
    }

    bool TryBuildStringNameFromUtf8(
        GDExtensionInterfaceStringNewWithUtf8Chars stringNew,
        GDExtensionInterfaceVariantGetPtrConstructor variantCtor,
        const char* utf8,
        char (&outStringName)[64])
    {
        return GodotSafe_BuildStringName(
            reinterpret_cast<void*>(stringNew),
            reinterpret_cast<void*>(variantCtor),
            kGodotVariantTypeStringName,
            utf8,
            outStringName,
            static_cast<int>(sizeof(outStringName))) != 0;
    }

    void* LookupInterfaceFunctionByName(const char* interfaceName)
    {
        if (!g_get_interface_function_impl || !interfaceName || !interfaceName[0])
            return nullptr;

        alignas(8) char stringName[64] = {};
        if (!BuildStringNameFromUtf8(interfaceName, stringName))
            return nullptr;

        __try
        {
            using GetInterfaceFunctionFn = void*(__stdcall*)(const void*);
            return reinterpret_cast<GetInterfaceFunctionFn>(g_get_interface_function_impl)(stringName);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ValidateGetProcAddressCandidate(
        GDExtensionInterfaceGetProcAddress candidate,
        uint8_t* mainStart,
        uint8_t* mainEnd)
    {
        if (!candidate)
            return false;

        void* methodBind = nullptr;
        void* ptrcall = nullptr;
        void* globalGet = nullptr;
        return GodotSafe_ProbeGetProcAddress(
            reinterpret_cast<void*>(candidate),
            reinterpret_cast<std::uintptr_t>(mainStart),
            reinterpret_cast<std::uintptr_t>(mainEnd),
            &methodBind,
            &ptrcall,
            &globalGet) != 0;
    }

    void* __stdcall ShimGetProcAddress(const char* name)
    {
        if (!name || !name[0])
            return nullptr;

        if (g_string_name_new_with_utf8_chars)
        {
            alignas(8) char stringName[64] = {};
            __try
            {
                g_string_name_new_with_utf8_chars(stringName, name);
                using GetInterfaceFunctionFn = void*(__stdcall*)(const void*);
                return reinterpret_cast<GetInterfaceFunctionFn>(g_get_interface_function_impl)(stringName);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        return LookupInterfaceFunctionByName(name);
    }

    struct StrippedBootstrapExports
    {
        GDExtensionInterfaceStringNewWithUtf8Chars stringNew = nullptr;
        GDExtensionInterfaceVariantGetPtrConstructor variantCtor = nullptr;
        GDExtensionInterfaceVariantGetPtrDestructor variantDtor = nullptr;
        alignas(8) char probeMethodBind[64] = {};
        alignas(8) char probeGlobalGet[64] = {};
        bool hasProbes = false;
    };

    bool FindStrippedBootstrapExports(uint8_t* mainStart, uint8_t* mainEnd, StrippedBootstrapExports& out);
    bool BootstrapGetProcAddressShim(uint8_t* mainStart, uint8_t* mainEnd, const StrippedBootstrapExports* prefoundStripped = nullptr);
    bool IsRipRelativeLea(const uint8_t* insn);
    uint8_t* DecodeRipRelativeLeaTarget(const uint8_t* insn);

    bool TryResolveGetProcAddressCandidate(
        GDExtensionInterfaceGetProcAddress candidate,
        uint8_t* mainStart,
        uint8_t* mainEnd,
        const char* sourceLabel,
        std::uintptr_t rva)
    {
        if (!ValidateGetProcAddressCandidate(candidate, mainStart, mainEnd))
            return false;

        LogMsg("[AegisGodot] ResolveGetProcAddress: Resolved via %s at RVA 0x%llX\n",
            sourceLabel,
            static_cast<unsigned long long>(rva));
        g_get_proc_address = candidate;
        GodotResolver_Record("get_proc_address", sourceLabel, reinterpret_cast<void*>(candidate));
        return true;
    }

    bool TryResolveJmpThunksTo(
        uint8_t* target,
        uint8_t* secStart,
        uint8_t* secEnd,
        uint8_t* mainStart,
        uint8_t* mainEnd)
    {
        for (uint8_t* p = secStart; p < secEnd - 5; ++p)
        {
            if (p[0] != 0xE9)
                continue;

            int32_t disp = *reinterpret_cast<int32_t*>(p + 1);
            uint8_t* jmpTarget = p + 5 + disp;
            if (jmpTarget != target)
                continue;

            if (TryResolveGetProcAddressCandidate(
                    reinterpret_cast<GDExtensionInterfaceGetProcAddress>(p),
                    mainStart,
                    mainEnd,
                    "jmp thunk to get_interface_function",
                    static_cast<std::uintptr_t>(p - mainStart)))
            {
                return true;
            }
        }
        return false;
    }

    bool ResolveGetProcAddress(uint8_t* mainStart, uint8_t* mainEnd, bool allowFullScan)
    {
        if (g_get_proc_address)
            return true;

        if (g_get_interface_function_impl)
        {
            if (BootstrapGetProcAddressShim(mainStart, mainEnd))
                return true;

            IMAGE_DOS_HEADER* mainDos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
            if (mainDos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                IMAGE_NT_HEADERS* mainNt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + mainDos->e_lfanew);
                if (mainNt->Signature == IMAGE_NT_SIGNATURE)
                {
                    IMAGE_SECTION_HEADER* codeSection = IMAGE_FIRST_SECTION(mainNt);
                    for (WORD codeIdx = 0; codeIdx < mainNt->FileHeader.NumberOfSections; ++codeIdx, ++codeSection)
                    {
                        if (!(codeSection->Characteristics & IMAGE_SCN_CNT_CODE))
                            continue;

                        uint8_t* codeStart = mainStart + codeSection->VirtualAddress;
                        uint8_t* codeEnd = codeStart + codeSection->Misc.VirtualSize;
                        if (TryResolveJmpThunksTo(
                                reinterpret_cast<uint8_t*>(g_get_interface_function_impl),
                                codeStart,
                                codeEnd,
                                mainStart,
                                mainEnd))
                        {
                            return true;
                        }
                    }
                }
            }
        }

        if (!allowFullScan)
            return false;

        LogMsg("[AegisGodot] ResolveGetProcAddress: Starting one-time GDExtension get_proc_address resolution...\n");

        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded))
        {
            for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
            {
                if (GetProcAddress(hMods[i], "gdextension_library_init"))
                {
                    LogMsg("[AegisGodot] ResolveGetProcAddress: Found GDExtension module at %p\n", hMods[i]);
                    MODULEINFO modInfo = {};
                    GetModuleInformation(GetCurrentProcess(), hMods[i], &modInfo, sizeof(modInfo));
                    
                    HMODULE hMain = GetModuleHandleW(nullptr);
                    MODULEINFO mainInfo = {};
                    GetModuleInformation(GetCurrentProcess(), hMain, &mainInfo, sizeof(mainInfo));
                    
                    uint8_t* start = (uint8_t*)modInfo.lpBaseOfDll;
                    uint8_t* exeStart = (uint8_t*)mainInfo.lpBaseOfDll;
                    uint8_t* exeEnd = exeStart + mainInfo.SizeOfImage;
                    
                    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)start;
                    if (dos->e_magic == IMAGE_DOS_SIGNATURE)
                    {
                        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(start + dos->e_lfanew);
                        if (nt->Signature == IMAGE_NT_SIGNATURE)
                        {
                            IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
                            for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, section++)
                            {
                                if (section->Characteristics & IMAGE_SCN_MEM_WRITE)
                                {
                                    uint8_t* secStart = start + section->VirtualAddress;
                                    uint8_t* secEnd = secStart + section->Misc.VirtualSize;
                                    
                                    for (uint8_t* p = secStart; p < secEnd - 8; p += 8)
                                    {
                                        void* val = *(void**)p;
                                        if (val >= exeStart && val < exeEnd)
                                        {
                                            __try
                                            {
                                                GDExtensionInterfaceGetProcAddress test_func =
                                                    reinterpret_cast<GDExtensionInterfaceGetProcAddress>(val);
                                                if (TryResolveGetProcAddressCandidate(
                                                        test_func,
                                                        mainStart,
                                                        mainEnd,
                                                        "GDExtension module pointer",
                                                        static_cast<std::uintptr_t>(
                                                            reinterpret_cast<uint8_t*>(val) - exeStart)))
                                                {
                                                    return true;
                                                }
                                            }
                                            __except (EXCEPTION_EXECUTE_HANDLER)
                                            {
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        LogMsg("[AegisGodot] ResolveGetProcAddress: Falling back to classdb_construct_object string scan...\n");
        const char* targetStr = "classdb_construct_object";
        size_t targetLen = strlen(targetStr);
        uint8_t* strAddr = nullptr;
        
        for (uint8_t* p = mainStart; p < mainEnd - targetLen; p++)
        {
            if (memcmp(p, targetStr, targetLen + 1) == 0)
            {
                strAddr = p;
                break;
            }
        }
        
        if (strAddr)
        {
            LogMsg("[AegisGodot] ResolveGetProcAddress: Found classdb_construct_object string at RVA 0x%X (skipping prologue probe; use error-string resolver)\n",
                (uint32_t)(strAddr - mainStart));
        }

        // Fallback 3: Search for "Attempt to get non-existent interface function" error string reference
        LogMsg("[AegisGodot] ResolveGetProcAddress: Falling back to error string scan...\n");
        const char* errStr = "Attempt to get non-existent interface function";
        size_t errStrLen = strlen(errStr);
        uint8_t* errStrAddr = nullptr;
        for (uint8_t* p = mainStart; p < mainEnd - errStrLen; p++)
        {
            if (memcmp(p, errStr, errStrLen) == 0)
            {
                errStrAddr = p;
                break;
            }
        }

        if (errStrAddr)
        {
            LogMsg("[AegisGodot] ResolveGetProcAddress: Found error string at RVA 0x%X\n", (uint32_t)(errStrAddr - mainStart));
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mainStart;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mainStart + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                {
                    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
                    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, section++)
                    {
                        if (section->Characteristics & IMAGE_SCN_CNT_CODE)
                        {
                            uint8_t* secStart = mainStart + section->VirtualAddress;
                            uint8_t* secEnd = secStart + section->Misc.VirtualSize;
                            
                            for (uint8_t* p = secStart; p < secEnd - 7; p++)
                            {
                                if (!IsRipRelativeLea(p))
                                    continue;
                                if (DecodeRipRelativeLeaTarget(p) != errStrAddr)
                                    continue;

                                LogMsg("[AegisGodot] ResolveGetProcAddress: Found LEA reference at RVA 0x%X, scanning backwards for get_interface_function prologue...\n", (uint32_t)(p - mainStart));
                                        uint8_t* prologueCandidates[32] = {};
                                        int prologueCandidateCount = 0;
                                        for (int offset = 0; offset < 2048; offset++)
                                        {
                                            uint8_t* ptr = p - offset;
                                            if (ptr < secStart)
                                                break;

                                            if (IsLikelyFunctionPrologue(ptr, mainStart) &&
                                                prologueCandidateCount < static_cast<int>(sizeof(prologueCandidates) / sizeof(prologueCandidates[0])))
                                            {
                                                prologueCandidates[prologueCandidateCount++] = ptr;
                                            }
                                        }

                                        LogMsg("[AegisGodot] ResolveGetProcAddress: Collected %d unwind-filtered candidate(s)\n",
                                            prologueCandidateCount);

                                        if (prologueCandidateCount > 0)
                                        {
                                            uint8_t* ownerFunc = prologueCandidates[0];
                                            LogMsg("[AegisGodot] ResolveGetProcAddress: Trying error-string owner as get_proc_address at RVA 0x%X\n",
                                                (uint32_t)(ownerFunc - mainStart));
                                            if (TryResolveGetProcAddressCandidate(
                                                    reinterpret_cast<GDExtensionInterfaceGetProcAddress>(ownerFunc),
                                                    mainStart,
                                                    mainEnd,
                                                    "error-string owner",
                                                    static_cast<std::uintptr_t>(ownerFunc - mainStart)))
                                            {
                                                return true;
                                            }

                                            LogMsg("[AegisGodot] ResolveGetProcAddress: Error-string owner did not validate as get_proc_address; skipping guessed get_interface_function calls\n");
                                        }

                                        LogMsg("[AegisGodot] ResolveGetProcAddress: Skipping thunk scan for rejected guessed candidates\n");
                            }
                        }
                    }
                }
            }
        }
        
        LogMsg("[AegisGodot] ResolveGetProcAddress: FAILED to resolve GDExtension get_proc_address\n");
        return false;
    }

    bool IsExecutableAddress(uint8_t* addr, uint8_t* mainStart)
    {
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
        {
            if (!(section->Characteristics & IMAGE_SCN_CNT_CODE))
                continue;

            uint8_t* secStart = mainStart + section->VirtualAddress;
            uint8_t* secEnd = secStart + section->Misc.VirtualSize;
            if (addr >= secStart && addr < secEnd)
                return true;
        }
        return false;
    }

    uint8_t* FindCStringInImage(const char* text, uint8_t* mainStart, uint8_t* mainEnd)
    {
        if (!text || !text[0])
            return nullptr;

        const size_t len = strlen(text);
        for (uint8_t* p = mainStart; p < mainEnd - len; ++p)
        {
            if (memcmp(p, text, len + 1) == 0)
                return p;
        }
        return nullptr;
    }

    uint8_t* FindNextCStringInImage(const char* text, uint8_t* searchStart, uint8_t* mainEnd)
    {
        if (!text || !text[0] || !searchStart || !mainEnd || searchStart >= mainEnd)
            return nullptr;

        const size_t len = strlen(text);
        for (uint8_t* p = searchStart; p < mainEnd - len; ++p)
        {
            if (memcmp(p, text, len + 1) == 0)
                return p;
        }
        return nullptr;
    }

    bool IsRipRelativeLea(const uint8_t* insn)
    {
        if ((insn[0] != 0x48 && insn[0] != 0x4C) || insn[1] != 0x8D)
            return false;
        return (insn[2] & 0xC0) == 0 && (insn[2] & 0x07) == 0x05;
    }

    uint8_t* DecodeRipRelativeLeaTarget(const uint8_t* insn)
    {
        if (!IsRipRelativeLea(insn))
            return nullptr;
        const int32_t disp = *reinterpret_cast<const int32_t*>(insn + 3);
        return const_cast<uint8_t*>(insn) + 7 + disp;
    }

    bool LeaRipTargetMatches(const uint8_t* insn, uint8_t* target)
    {
        return DecodeRipRelativeLeaTarget(insn) == target;
    }

    void* TryExtractCodePointerFromInsn(const uint8_t* insn, uint8_t* mainStart, uint8_t* mainEnd)
    {
        auto acceptCodePointer = [&](uint8_t* target) -> void*
        {
            if (target >= mainStart && target < mainEnd && IsExecutableAddress(target, mainStart))
                return target;
            return nullptr;
        };

        if (IsRipRelativeLea(insn))
            return acceptCodePointer(DecodeRipRelativeLeaTarget(insn));

        if (insn[0] == 0x48 && insn[1] == 0xB8)
            return acceptCodePointer(reinterpret_cast<uint8_t*>(*reinterpret_cast<void* const*>(insn + 2)));

        if (insn[0] == 0x48 && insn[1] == 0xBA)
            return acceptCodePointer(reinterpret_cast<uint8_t*>(*reinterpret_cast<void* const*>(insn + 2)));

        return nullptr;
    }

    void* TryExtractRegisteredFunctionNearLea(uint8_t* leaInsn, uint8_t* mainStart, uint8_t* mainEnd)
    {
        // Godot's generated registration code constructs the interface name,
        // then loads the callback pointer shortly after the string reference.
        // Prefer forward candidates so duplicate sequential registrations do
        // not accidentally bind a name to the previous entry's callback.
        for (int i = 7; i < 96; ++i)
        {
            const uint8_t* p = leaInsn + i;
            void* fn = TryExtractCodePointerFromInsn(p, mainStart, mainEnd);
            if (fn)
                return fn;
        }

        for (int i = -64; i < 0; ++i)
        {
            const uint8_t* p = leaInsn + i;
            void* fn = TryExtractCodePointerFromInsn(p, mainStart, mainEnd);
            if (fn)
                return fn;
        }

        return nullptr;
    }

    void* FindRegisteredInterfaceFunction(const char* interfaceName, uint8_t* mainStart, uint8_t* mainEnd)
    {
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;

        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return nullptr;

        int occurrence = 0;
        for (uint8_t* nameAddr = FindNextCStringInImage(interfaceName, mainStart, mainEnd);
             nameAddr != nullptr;
             nameAddr = FindNextCStringInImage(interfaceName, nameAddr + 1, mainEnd))
        {
            ++occurrence;
            IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
            {
                if (!(section->Characteristics & IMAGE_SCN_CNT_CODE))
                    continue;

                uint8_t* secStart = mainStart + section->VirtualAddress;
                uint8_t* secEnd = secStart + section->Misc.VirtualSize;
                for (uint8_t* p = secStart; p < secEnd - 7; ++p)
                {
                    if (!LeaRipTargetMatches(p, nameAddr))
                        continue;

                    void* fn = TryExtractRegisteredFunctionNearLea(p, mainStart, mainEnd);
                    if (fn)
                    {
                        LogMsg("[AegisGodot] FindRegisteredInterfaceFunction: Resolved '%s' occurrence %d at RVA 0x%llX\n",
                            interfaceName,
                            occurrence,
                            static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(fn) - mainStart));
                        return fn;
                    }
                }
            }
        }
        return nullptr;
    }

    bool ValidateVariantGetPtrConstructor(GDExtensionInterfaceVariantGetPtrConstructor candidate)
    {
        if (!candidate || !g_string_new_with_utf8_chars)
            return false;

        alignas(8) char probeStringName[64] = {};
        return TryBuildStringNameFromUtf8(
            g_string_new_with_utf8_chars,
            candidate,
            "classdb_get_method_bind",
            probeStringName);
    }

    void* FindValidatedRegisteredInterfaceFunction(
        const char* interfaceName,
        uint8_t* mainStart,
        uint8_t* mainEnd,
        bool (*validator)(void*))
    {
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;

        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return nullptr;

        int occurrence = 0;
        for (uint8_t* nameAddr = FindNextCStringInImage(interfaceName, mainStart, mainEnd);
             nameAddr != nullptr;
             nameAddr = FindNextCStringInImage(interfaceName, nameAddr + 1, mainEnd))
        {
            ++occurrence;
            IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
            {
                if (!(section->Characteristics & IMAGE_SCN_CNT_CODE))
                    continue;

                uint8_t* secStart = mainStart + section->VirtualAddress;
                uint8_t* secEnd = secStart + section->Misc.VirtualSize;
                for (uint8_t* p = secStart; p < secEnd - 7; ++p)
                {
                    if (!LeaRipTargetMatches(p, nameAddr))
                        continue;

                    void* fn = TryExtractRegisteredFunctionNearLea(p, mainStart, mainEnd);
                    if (!fn)
                        continue;

                    bool accepted = !validator;
                    if (validator)
                    {
                        __try
                        {
                            accepted = validator(fn);
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER)
                        {
                            LogMsg("[AegisGodot] FindValidatedRegisteredInterfaceFunction: validator SEH 0x%08X for '%s' occurrence %d candidate RVA 0x%llX\n",
                                ::GetExceptionCode(),
                                interfaceName,
                                occurrence,
                                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(fn) - mainStart));
                            accepted = false;
                        }
                    }

                    if (accepted)
                    {
                        LogMsg("[AegisGodot] FindValidatedRegisteredInterfaceFunction: Resolved '%s' occurrence %d at RVA 0x%llX\n",
                            interfaceName,
                            occurrence,
                            static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(fn) - mainStart));
                        return fn;
                    }
                }
            }
        }
        return nullptr;
    }

    bool ForEachRegisteredInterfaceCandidate(
        const char* interfaceName,
        uint8_t* mainStart,
        uint8_t* mainEnd,
        bool (*visitor)(void* candidate, void* context),
        void* context)
    {
        if (!visitor)
            return false;

        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        int occurrence = 0;
        for (uint8_t* nameAddr = FindNextCStringInImage(interfaceName, mainStart, mainEnd);
             nameAddr != nullptr;
             nameAddr = FindNextCStringInImage(interfaceName, nameAddr + 1, mainEnd))
        {
            ++occurrence;
            IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
            {
                if (!(section->Characteristics & IMAGE_SCN_CNT_CODE))
                    continue;

                uint8_t* secStart = mainStart + section->VirtualAddress;
                uint8_t* secEnd = secStart + section->Misc.VirtualSize;
                for (uint8_t* p = secStart; p < secEnd - 7; ++p)
                {
                    if (!LeaRipTargetMatches(p, nameAddr))
                        continue;

                    void* fn = TryExtractRegisteredFunctionNearLea(p, mainStart, mainEnd);
                    if (!fn)
                        continue;

                    bool stop = false;
                    __try
                    {
                        stop = visitor(fn, context);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        LogMsg("[AegisGodot] ForEachRegisteredInterfaceCandidate: visitor SEH 0x%08X for '%s' occurrence %d candidate RVA 0x%llX\n",
                            ::GetExceptionCode(),
                            interfaceName,
                            occurrence,
                            static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(fn) - mainStart));
                    }

                    if (stop)
                        return true;
                }
            }
        }

        return false;
    }

    struct StrippedBootstrapSearchContext
    {
        uint8_t* mainStart = nullptr;
        uint8_t* mainEnd = nullptr;
        StrippedBootstrapExports* out = nullptr;
    };

    bool TryAcceptStrippedBootstrapPair(void* stringNewCandidate, void* context)
    {
        auto* search = reinterpret_cast<StrippedBootstrapSearchContext*>(context);
        if (!search || !search->out || !search->mainStart || !search->mainEnd)
            return false;

        const auto stringNew = reinterpret_cast<GDExtensionInterfaceStringNewWithUtf8Chars>(stringNewCandidate);
        if (!ValidateStringNewFunction(stringNew))
            return false;

        struct CtorSearchContext
        {
            GDExtensionInterfaceStringNewWithUtf8Chars stringNew = nullptr;
            StrippedBootstrapExports* out = nullptr;
            uint8_t* mainStart = nullptr;
            uint8_t* mainEnd = nullptr;
        } ctorSearch;
        ctorSearch.stringNew = stringNew;
        ctorSearch.out = search->out;
        ctorSearch.mainStart = search->mainStart;
        ctorSearch.mainEnd = search->mainEnd;

        const auto visitCtor = +[](void* ctorCandidate, void* ctorContext) -> bool
        {
            auto* ctx = reinterpret_cast<CtorSearchContext*>(ctorContext);
            if (!ctx || !ctx->out)
                return false;

            const auto variantCtor = reinterpret_cast<GDExtensionInterfaceVariantGetPtrConstructor>(ctorCandidate);
            if (!TryBuildStringNameFromUtf8(ctx->stringNew, variantCtor, "classdb_get_method_bind", ctx->out->probeMethodBind))
                return false;
            if (!TryBuildStringNameFromUtf8(ctx->stringNew, variantCtor, "global_get_singleton", ctx->out->probeGlobalGet))
                return false;

            ctx->out->stringNew = ctx->stringNew;
            ctx->out->variantCtor = variantCtor;
            ctx->out->hasProbes = true;

            LogMsg("[AegisGodot] FindStrippedBootstrapExports: paired string_new RVA 0x%llX with variant_ctor RVA 0x%llX\n",
                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(ctx->out->stringNew) - ctx->mainStart),
                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(ctx->out->variantCtor) - ctx->mainStart));
            GodotResolver_Record("string_new_with_utf8_chars", "live-cstring-lea-pair", ctx->out->stringNew);
            GodotResolver_Record("variant_get_ptr_constructor", "live-cstring-lea-pair", ctx->out->variantCtor);
            return true;
        };

        return ForEachRegisteredInterfaceCandidate(
            "variant_get_ptr_constructor",
            search->mainStart,
            search->mainEnd,
            visitCtor,
            &ctorSearch);
    }

    bool FindStrippedBootstrapExports(uint8_t* mainStart, uint8_t* mainEnd, StrippedBootstrapExports& out)
    {
        out = {};

        StrippedBootstrapSearchContext search;
        search.mainStart = mainStart;
        search.mainEnd = mainEnd;
        search.out = &out;

        if (!ForEachRegisteredInterfaceCandidate(
                "string_new_with_utf8_chars",
                mainStart,
                mainEnd,
                TryAcceptStrippedBootstrapPair,
                &search))
        {
            LogMsg("[AegisGodot] FindStrippedBootstrapExports: no paired string_new/variant_ctor exports found\n");
            return false;
        }

        return out.stringNew != nullptr && out.variantCtor != nullptr;
    }

    bool ValidateGetInterfaceFunctionImpl(
        void* impl,
        uint8_t* mainStart,
        uint8_t* mainEnd,
        const StrippedBootstrapExports& bootstrap)
    {
        if (!impl || !bootstrap.hasProbes || !IsExecutableAddress(reinterpret_cast<uint8_t*>(impl), mainStart))
            return false;

        void* methodBind = nullptr;
        void* globalGet = nullptr;
        const std::uintptr_t imageStart = reinterpret_cast<std::uintptr_t>(mainStart);
        const std::uintptr_t imageEnd = reinterpret_cast<std::uintptr_t>(mainEnd);

        if (!GodotSafe_CallGetInterfaceFunction(impl, bootstrap.probeMethodBind, imageStart, imageEnd, &methodBind))
        {
            LogMsg("[AegisGodot] ValidateGetInterfaceFunctionImpl: classdb_get_method_bind probe failed for RVA 0x%llX\n",
                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(impl) - mainStart));
            return false;
        }

        if (!GodotSafe_CallGetInterfaceFunction(impl, bootstrap.probeGlobalGet, imageStart, imageEnd, &globalGet))
        {
            LogMsg("[AegisGodot] ValidateGetInterfaceFunctionImpl: global_get_singleton probe failed for RVA 0x%llX\n",
                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(impl) - mainStart));
            return false;
        }

        return methodBind != nullptr && globalGet != nullptr;
    }

    bool BootstrapGetProcAddressShim(uint8_t* mainStart, uint8_t* mainEnd, const StrippedBootstrapExports* prefoundStripped)
    {
        if (!g_get_interface_function_impl)
            return false;

        LogMsg("[AegisGodot] BootstrapGetProcAddressShim: enter (impl=%p RVA=0x%llX)\n",
            g_get_interface_function_impl,
            static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_get_interface_function_impl) - mainStart));

        StrippedBootstrapExports stripped = {};
        if (prefoundStripped && prefoundStripped->hasProbes)
            stripped = *prefoundStripped;
        else if (!FindStrippedBootstrapExports(mainStart, mainEnd, stripped))
        {
            LogMsg("[AegisGodot] BootstrapGetProcAddressShim: Failed to resolve stripped bootstrap exports\n");
            return false;
        }

        LogMsg("[AegisGodot] BootstrapGetProcAddressShim: validating get_interface_function...\n");
        if (!ValidateGetInterfaceFunctionImpl(g_get_interface_function_impl, mainStart, mainEnd, stripped))
        {
            LogMsg("[AegisGodot] BootstrapGetProcAddressShim: Rejecting invalid get_interface_function at RVA 0x%llX\n",
                static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_get_interface_function_impl) - mainStart));
            return false;
        }

        g_string_new_with_utf8_chars = stripped.stringNew;
        g_variant_get_ptr_constructor = stripped.variantCtor;
        g_variant_get_ptr_destructor = reinterpret_cast<GDExtensionInterfaceVariantGetPtrDestructor>(
            FindRegisteredInterfaceFunction("variant_get_ptr_destructor", mainStart, mainEnd));

        g_get_proc_address = ShimGetProcAddress;
        LogMsg("[AegisGodot] BootstrapGetProcAddressShim: Installed stripped-export shim (string_new) over get_interface_function at %p\n",
            g_get_interface_function_impl);
        const bool shimOk = ValidateGetProcAddressCandidate(g_get_proc_address, mainStart, mainEnd);
        if (shimOk)
        {
            GodotResolver_Record("get_interface_function", "live-prologue-scan", g_get_interface_function_impl);
            RecordGodot4ResolverState("shim-stripped");
            WriteDefaultResolverReport();
        }
        else
        {
            g_get_proc_address = nullptr;
        }
        return shimOk;
    }

    template <typename T>
    T ResolveInterfaceFunction(const char* name, uint8_t* mainStart, uint8_t* mainEnd)
    {
        if (g_get_proc_address)
        {
            void* ptr = g_get_proc_address(name);
            if (ptr && IsValidInterfacePointer(ptr, mainStart, mainEnd))
                return reinterpret_cast<T>(ptr);
        }

        void* direct = LookupInterfaceFunctionByName(name);
        if (direct && IsValidInterfacePointer(direct, mainStart, mainEnd))
            return reinterpret_cast<T>(direct);

        void* registeredFn = FindRegisteredInterfaceFunction(name, mainStart, mainEnd);
        if (registeredFn && IsValidInterfacePointer(registeredFn, mainStart, mainEnd))
            return reinterpret_cast<T>(registeredFn);

        return nullptr;
    }

    bool ResolveGDExtensionFunctions()
    {
        std::lock_guard<std::mutex> resolveLock(g_gdextensionResolveMutex);

        if (g_gdextension_resolution_done)
            return g_gdextension_resolution_ok;

        const DWORD now = ::GetTickCount();
        if (g_gdextension_retry_after_tick != 0 && now < g_gdextension_retry_after_tick)
            return false;

        HMODULE hMain = GetModuleHandleW(nullptr);
        MODULEINFO mainInfo = {};
        GetModuleInformation(GetCurrentProcess(), hMain, &mainInfo, sizeof(mainInfo));
        uint8_t* mainStart = reinterpret_cast<uint8_t*>(mainInfo.lpBaseOfDll);
        uint8_t* mainEnd = mainStart + mainInfo.SizeOfImage;

        const bool allowFullScan = (g_get_interface_function_impl == nullptr);
        if (!ResolveGetProcAddress(mainStart, mainEnd, allowFullScan) ||
            !g_get_proc_address ||
            !ValidateGetProcAddressCandidate(g_get_proc_address, mainStart, mainEnd))
        {
            g_get_proc_address = nullptr;
            g_get_interface_function_impl = nullptr;
            LogMsg("[AegisGodot] ResolveGDExtensionFunctions: get_proc_address unavailable; using static registered-interface scan\n");
        }

        g_classdb_get_method_bind = ResolveInterfaceFunction<GDExtensionInterfaceClassdbGetMethodBind>(
            "classdb_get_method_bind", mainStart, mainEnd);
        g_object_call_method_bind = ResolveInterfaceFunction<GDExtensionInterfaceObjectCallMethodBind>(
            "object_method_bind_call", mainStart, mainEnd);
        if (!g_object_call_method_bind)
        {
            g_object_call_method_bind = ResolveInterfaceFunction<GDExtensionInterfaceObjectCallMethodBind>(
                "object_call_method_bind", mainStart, mainEnd);
        }
        g_variant_get_ptr_builtin_method = ResolveInterfaceFunction<GDExtensionInterfaceVariantGetPtrBuiltInMethod>(
            "variant_get_ptr_builtin_method", mainStart, mainEnd);
        if (!g_variant_get_ptr_constructor)
        {
            g_variant_get_ptr_constructor = ResolveInterfaceFunction<GDExtensionInterfaceVariantGetPtrConstructor>(
                "variant_get_ptr_constructor", mainStart, mainEnd);
        }
        if (!g_variant_get_ptr_destructor)
        {
            g_variant_get_ptr_destructor = ResolveInterfaceFunction<GDExtensionInterfaceVariantGetPtrDestructor>(
                "variant_get_ptr_destructor", mainStart, mainEnd);
        }
        g_get_variant_from_type_constructor = ResolveInterfaceFunction<GDExtensionInterfaceGetVariantFromTypeConstructor>(
            "get_variant_from_type_constructor", mainStart, mainEnd);
        g_string_name_new_with_utf8_chars = ResolveInterfaceFunction<GDExtensionInterfaceStringNameNewWithUtf8Chars>(
            "string_name_new_with_utf8_chars", mainStart, mainEnd);
        g_string_name_operator_string = ResolveInterfaceFunction<GDExtensionInterfaceStringNameOperatorString>(
            "string_name_operator_string", mainStart, mainEnd);
        if (!g_string_new_with_utf8_chars)
        {
            g_string_new_with_utf8_chars = ResolveInterfaceFunction<GDExtensionInterfaceStringNewWithUtf8Chars>(
                "string_new_with_utf8_chars", mainStart, mainEnd);
        }
        g_global_get_singleton = ResolveInterfaceFunction<GDExtGlobalGetSingletonFn>(
            "global_get_singleton", mainStart, mainEnd);

        LogMsg("[AegisGodot] ResolveGDExtensionFunctions: classdb_get_method_bind=%p object_method_bind_call=%p global_get_singleton=%p string_new=%p variant_ctor=%p get_proc_address=%p\n",
            g_classdb_get_method_bind,
            g_object_call_method_bind,
            g_global_get_singleton,
            g_string_new_with_utf8_chars,
            g_variant_get_ptr_constructor,
            reinterpret_cast<void*>(g_get_proc_address));

        const bool pointersResolved =
            g_classdb_get_method_bind != nullptr &&
            g_object_call_method_bind != nullptr &&
            g_global_get_singleton != nullptr &&
            g_string_new_with_utf8_chars != nullptr &&
            g_variant_get_ptr_constructor != nullptr;

        const bool apiValidated = pointersResolved &&
            ValidateResolvedGodot4Api(mainStart, mainEnd);
        if (pointersResolved && !apiValidated)
        {
            LogMsg("[AegisGodot] ResolveGDExtensionFunctions: live API smoke validation failed; keeping resolved pointers for Init diagnostics\n");
        }

        g_gdextension_resolution_done = true;
        g_gdextension_resolution_ok = pointersResolved;

        if (!g_gdextension_resolution_ok)
            g_gdextension_retry_after_tick = now + 30000;
        else
            RecordGodot4ResolverState(apiValidated ? "live-api-validated" : "static-api-resolved");

        WriteDefaultResolverReport();
        return g_gdextension_resolution_ok;
    }

    std::string GetStringVal(const void* string_ptr)
    {
        if (!string_ptr) return "";
        void* str_data = *(void**)string_ptr;
        if (!str_data) return "";
        int32_t len = ((int32_t*)str_data)[-1];
        if (len <= 0) return "";
        const char32_t* chars = (const char32_t*)str_data;
        std::string result;
        result.reserve(len);
        for (int i = 0; i < len; ++i)
        {
            char32_t c = chars[i];
            if (c < 128) result.push_back((char)c);
            else result.push_back('?');
        }
        return result;
    }

    std::string GetStringNameVal(const void* string_name_ptr)
    {
        if (!string_name_ptr || !g_string_name_operator_string)
            return "";

        char stringBuf[64] = {};
        g_string_name_operator_string(string_name_ptr, stringBuf);
        std::string result = GetStringVal(stringBuf);

        if (g_variant_get_ptr_destructor)
        {
            auto dtor = reinterpret_cast<GDExtensionPtrDestructor>(g_variant_get_ptr_destructor(kGodotVariantTypeString));
            if (dtor)
                dtor(stringBuf);
        }

        return result;
    }

    void ParseProperties(const std::string& className, const std::string& json, std::vector<AegisGodotMemberInfo>& outMembers)
    {
        size_t pos = 0;
        while (true)
        {
            pos = json.find("\"name\"", pos);
            if (pos == std::string::npos)
                break;
                
            size_t colon = json.find(":", pos);
            if (colon == std::string::npos)
                break;
                
            size_t quoteStart = json.find("\"", colon);
            if (quoteStart == std::string::npos)
                break;
                
            size_t quoteEnd = json.find("\"", quoteStart + 1);
            if (quoteEnd == std::string::npos)
                break;
                
            std::string propName = json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            
            std::string typeName = "Variant";
            size_t typePos = json.find("\"type\"", quoteEnd);
            if (typePos != std::string::npos && typePos < json.find("\"name\"", quoteEnd))
            {
                size_t typeColon = json.find(":", typePos);
                if (typeColon != std::string::npos)
                {
                    char* end = nullptr;
                    long typeVal = strtol(json.c_str() + typeColon + 1, &end, 10);
                    if (end && end != json.c_str() + typeColon + 1)
                    {
                        switch (typeVal)
                        {
                        case 1: typeName = "bool"; break;
                        case 2: typeName = "int"; break;
                        case 3: typeName = "float"; break;
                        case 4: typeName = "String"; break;
                        case 5: typeName = "Vector2"; break;
                        case 9: typeName = "Vector3"; break;
                        case 20: typeName = "Color"; break;
                        case 21: typeName = "StringName"; break;
                        case 24: typeName = "Object"; break;
                        case 28: typeName = "Array"; break;
                        case 27: typeName = "Dictionary"; break;
                        default: typeName = "Variant"; break;
                        }
                    }
                }
            }
            
            AegisGodotMemberInfo member = {};
            strcpy_s(member.className, className.c_str());
            strcpy_s(member.memberName, propName.c_str());
            strcpy_s(member.typeName, typeName.c_str());
            member.offset = 0;
            member.isMethod = 0;
            
            outMembers.push_back(member);
            pos = quoteEnd + 1;
        }
    }

    void ParseMethods(const std::string& className, const std::string& json, std::vector<AegisGodotMemberInfo>& outMembers)
    {
        size_t pos = 0;
        while (true)
        {
            pos = json.find("\"name\"", pos);
            if (pos == std::string::npos)
                break;
                
            size_t colon = json.find(":", pos);
            if (colon == std::string::npos)
                break;
                
            size_t quoteStart = json.find("\"", colon);
            if (quoteStart == std::string::npos)
                break;
                
            size_t quoteEnd = json.find("\"", quoteStart + 1);
            if (quoteEnd == std::string::npos)
                break;
                
            std::string methodName = json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            
            AegisGodotMemberInfo member = {};
            strcpy_s(member.className, className.c_str());
            strcpy_s(member.memberName, methodName.c_str());
            strcpy_s(member.typeName, "void");
            member.offset = 0;
            member.isMethod = 1;
            
            outMembers.push_back(member);
            pos = quoteEnd + 1;
        }
    }

    void ScanGodot4ClassesInternal(std::vector<AegisGodotMemberInfo>& outMembers)
    {
        if (!ResolveGDExtensionFunctions())
            return;
            
        void* get_class_list_mb = GetClassdbMethodBind("ClassDB", "get_class_list", 0);
        if (!get_class_list_mb)
            return;
            
        // Get ClassDB singleton instance
        AutoStringName classdbSN("ClassDB");
        void* classdbInstance = g_global_get_singleton ? g_global_get_singleton(classdbSN.buf) : nullptr;
        if (!classdbInstance)
            return;
            
        char retBuffer[64] = {};
        GDExtensionPtrConstructor packed_str_array_ctor = g_variant_get_ptr_constructor ? (GDExtensionPtrConstructor)g_variant_get_ptr_constructor(34, 0) : nullptr;
        if (packed_str_array_ctor)
            packed_str_array_ctor(retBuffer, nullptr);
            
        g_object_call_method_bind(get_class_list_mb, classdbInstance, nullptr, 0, retBuffer, nullptr);
        
        void* data = *(void**)retBuffer;
        if (!data)
            return;
            
        int32_t classCount = ((int32_t*)data)[-1];
        std::vector<std::string> classNames;
        for (int i = 0; i < classCount; i++)
        {
            void* string_ptr = ((void**)data) + i;
            std::string className = GetStringVal(string_ptr);
            if (!className.empty())
            {
                classNames.push_back(className);
            }
        }
        
        GDExtensionPtrDestructor packed_str_array_dtor = g_variant_get_ptr_destructor ? (GDExtensionPtrDestructor)g_variant_get_ptr_destructor(34) : nullptr;
        if (packed_str_array_dtor)
            packed_str_array_dtor(retBuffer);
            
        void* get_prop_list_mb = GetClassdbMethodBind("ClassDB", "class_get_property_list", 0);
        void* get_method_list_mb = GetClassdbMethodBind("ClassDB", "class_get_method_list", 0);
        void* stringify_mb = GetClassdbMethodBind("JSON", "stringify", 0);
        
        if (!get_prop_list_mb || !get_method_list_mb || !stringify_mb)
            return;
            
        // Get JSON singleton instance
        AutoStringName jsonSN("JSON");
        void* jsonInstance = g_global_get_singleton ? g_global_get_singleton(jsonSN.buf) : nullptr;
        if (!jsonInstance)
            return;
            
        GDExtensionVariantFromTypeConstructorFunc wrap_array = g_get_variant_from_type_constructor ?
            (GDExtensionVariantFromTypeConstructorFunc)g_get_variant_from_type_constructor(28) : nullptr;
        
        auto string_name_ctor_utf8 = g_string_name_new_with_utf8_chars;
        GDExtensionPtrDestructor string_name_dtor = g_variant_get_ptr_destructor ? (GDExtensionPtrDestructor)g_variant_get_ptr_destructor(21) : nullptr;
        GDExtensionPtrDestructor array_dtor = g_variant_get_ptr_destructor ? (GDExtensionPtrDestructor)g_variant_get_ptr_destructor(28) : nullptr;
        GDExtensionPtrDestructor string_dtor = g_variant_get_ptr_destructor ? (GDExtensionPtrDestructor)g_variant_get_ptr_destructor(4) : nullptr;
        
        for (const std::string& className : classNames)
        {
            char classNameSN[64] = {};
            if (string_name_ctor_utf8)
                string_name_ctor_utf8(classNameSN, className.c_str());
                
            {
                char retArray[64] = {};
                GDExtensionPtrConstructor array_ctor = g_variant_get_ptr_constructor ? (GDExtensionPtrConstructor)g_variant_get_ptr_constructor(28, 0) : nullptr;
                if (array_ctor)
                    array_ctor(retArray, nullptr);
                    
                const void* args[1] = { classNameSN };
                g_object_call_method_bind(get_prop_list_mb, classdbInstance, args, 1, retArray, nullptr);
                
                char variantArray[64] = {};
                if (wrap_array)
                    wrap_array(variantArray, retArray);
                    
                char retString[64] = {};
                GDExtensionPtrConstructor string_ctor = g_variant_get_ptr_constructor ? (GDExtensionPtrConstructor)g_variant_get_ptr_constructor(4, 0) : nullptr;
                if (string_ctor)
                    string_ctor(retString, nullptr);
                    
                const void* jsonArgs[1] = { variantArray };
                g_object_call_method_bind(stringify_mb, jsonInstance, jsonArgs, 1, retString, nullptr);
                
                std::string jsonStr = GetStringVal(retString);
                if (!jsonStr.empty())
                {
                    ParseProperties(className, jsonStr, outMembers);
                }
                
                if (string_dtor)
                    string_dtor(retString);
                if (array_dtor)
                    array_dtor(retArray);
            }
            
            {
                char retArray[64] = {};
                GDExtensionPtrConstructor array_ctor = g_variant_get_ptr_constructor ? (GDExtensionPtrConstructor)g_variant_get_ptr_constructor(28, 0) : nullptr;
                if (array_ctor)
                    array_ctor(retArray, nullptr);
                    
                const void* args[1] = { classNameSN };
                g_object_call_method_bind(get_method_list_mb, classdbInstance, args, 1, retArray, nullptr);
                
                char variantArray[64] = {};
                if (wrap_array)
                    wrap_array(variantArray, retArray);
                    
                char retString[64] = {};
                GDExtensionPtrConstructor string_ctor = g_variant_get_ptr_constructor ? (GDExtensionPtrConstructor)g_variant_get_ptr_constructor(4, 0) : nullptr;
                if (string_ctor)
                    string_ctor(retString, nullptr);
                    
                const void* jsonArgs[1] = { variantArray };
                g_object_call_method_bind(stringify_mb, jsonInstance, jsonArgs, 1, retString, nullptr);
                
                std::string jsonStr = GetStringVal(retString);
                if (!jsonStr.empty())
                {
                    ParseMethods(className, jsonStr, outMembers);
                }
                
                if (string_dtor)
                    string_dtor(retString);
                if (array_dtor)
                    array_dtor(retArray);
            }
            
            if (string_name_dtor)
                string_name_dtor(classNameSN);
        }
    }

    int ScanGodot4ClassesSafe(std::vector<AegisGodotMemberInfo>* outMembers)
    {
        __try
        {
            ScanGodot4ClassesInternal(*outMembers);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void PrepopulateCoreClasses(std::vector<AegisGodotMemberInfo>& outMembers)
    {
        struct CoreMember {
            const char* className;
            const char* memberName;
            const char* typeName;
            int isMethod;
        };
        
        CoreMember coreList[] = {
            { "Node", "get_name", "String", 1 },
            { "Node", "get_parent", "Node", 1 },
            { "Node", "get_child", "Node", 1 },
            { "Node", "get_child_count", "int", 1 },
            { "Node", "add_child", "void", 1 },
            { "Node", "remove_child", "void", 1 },
            { "Node", "queue_free", "void", 1 },
            { "Node", "name", "String", 0 },
            
            { "Node2D", "get_position", "Vector2", 1 },
            { "Node2D", "set_position", "void", 1 },
            { "Node2D", "get_global_position", "Vector2", 1 },
            { "Node2D", "set_global_position", "void", 1 },
            { "Node2D", "get_rotation", "float", 1 },
            { "Node2D", "position", "Vector2", 0 },
            { "Node2D", "global_position", "Vector2", 0 },
            
            { "Node3D", "get_position", "Vector3", 1 },
            { "Node3D", "set_position", "void", 1 },
            { "Node3D", "get_global_position", "Vector3", 1 },
            { "Node3D", "set_global_position", "void", 1 },
            { "Node3D", "get_transform", "Transform3D", 1 },
            { "Node3D", "position", "Vector3", 0 },
            { "Node3D", "global_position", "Vector3", 0 },
            
            { "Spatial", "get_translation", "Vector3", 1 },
            { "Spatial", "set_translation", "void", 1 },
            { "Spatial", "get_global_transform", "Transform", 1 },
            { "Spatial", "translation", "Vector3", 0 },
            
            { "Camera3D", "project_ray_origin", "Vector3", 1 },
            { "Camera3D", "project_ray_normal", "Vector3", 1 },
            { "Camera3D", "project_position", "Vector3", 1 },
            { "Camera3D", "project_local_ray_normal", "Vector3", 1 },
            { "Camera3D", "fov", "float", 0 },
            
            { "Camera", "project_ray_origin", "Vector3", 1 },
            { "Camera", "project_position", "Vector3", 1 },
            { "Camera", "fov", "float", 0 },
            
            { "Viewport", "get_camera_3d", "Camera3D", 1 },
            { "Viewport", "get_mouse_position", "Vector2", 1 },
            { "Viewport", "size", "Vector2i", 0 }
        };
        
        for (const auto& item : coreList)
        {
            AegisGodotMemberInfo member = {};
            strcpy_s(member.className, item.className);
            strcpy_s(member.memberName, item.memberName);
            strcpy_s(member.typeName, item.typeName);
            member.offset = 0;
            member.isMethod = item.isMethod;
            outMembers.push_back(member);
        }
    }
}

AEGIS_UNIVERSAL_API int AegisGodot_ScanClasses()
{
    std::lock_guard lock(g_scannedMembersMutex);
    g_scannedMembers.clear();
    PrepopulateCoreClasses(g_scannedMembers);
    
    std::vector<AegisGodotMemberInfo> dynamicMembers;
    if (ScanGodot4ClassesSafe(&dynamicMembers))
    {
        g_scannedMembers.insert(g_scannedMembers.end(), dynamicMembers.begin(), dynamicMembers.end());
    }
    
    return 1;
}

AEGIS_UNIVERSAL_API std::uint32_t AegisGodot_GetMemberCount()
{
    std::lock_guard lock(g_scannedMembersMutex);
    return static_cast<std::uint32_t>(g_scannedMembers.size());
}

AEGIS_UNIVERSAL_API int AegisGodot_GetMemberInfo(std::uint32_t index, AegisGodotMemberInfo* outInfo)
{
    if (!outInfo)
        return 0;
    std::lock_guard lock(g_scannedMembersMutex);
    if (index >= g_scannedMembers.size())
        return 0;
    *outInfo = g_scannedMembers[index];
    return 1;
}

// ============================================================
// Automatic Scene Tree Scanner & Provider System
// ============================================================

namespace
{
    // Additional GDExtension function types for the auto-scanner
    using GDExtGlobalGetSingletonFn = void*(__stdcall*)(const void* p_name);
    using GDExtPtrcallFn = void(__stdcall*)(void* p_method_bind, void* p_instance, const void** p_args, void* r_ret);
    using GDExtGetMethodBindFn = void*(__stdcall*)(const void* p_classname, const void* p_methodname, int64_t p_hash);
    using GDExtGetClassTagFn = void*(__stdcall*)(const void* p_classname);
    using GDExtObjectCastToFn = void*(__stdcall*)(const void* p_object, void* p_class_tag);
    using GDExtObjectGetInstanceIdFn = uint64_t(__stdcall*)(const void* p_object);

    // Auto-scanner state
    struct AutoScanState
    {
        // Additional function pointers
        GDExtGlobalGetSingletonFn fn_global_get_singleton = nullptr;
        GDExtPtrcallFn fn_ptrcall = nullptr;
        GDExtGetMethodBindFn fn_get_method_bind = nullptr;
        GDExtGetClassTagFn fn_get_class_tag = nullptr;
        GDExtObjectCastToFn fn_object_cast_to = nullptr;
        GDExtObjectGetInstanceIdFn fn_object_get_instance_id = nullptr;

        // Cached String representation buffers
        char str_Node3D[64] = {};
        char str_Camera3D[64] = {};
        char str_CharacterBody3D[64] = {};
        char str_RigidBody3D[64] = {};
        char str_Node2D[64] = {};
        char str_Camera2D[64] = {};
        char str_CharacterBody2D[64] = {};
        char str_RigidBody2D[64] = {};

        // Method binds
        void* mb_SceneTree_get_root = nullptr;
        void* mb_Node_get_child_count = nullptr;
        void* mb_Node_get_child = nullptr;
        void* mb_Node_get_class = nullptr;
        void* mb_Node_get_name = nullptr;
        void* mb_Node3D_get_global_position = nullptr;
        void* mb_Node3D_is_visible_in_tree = nullptr;
        void* mb_Camera3D_get_camera_projection = nullptr;
        void* mb_Camera3D_get_camera_transform = nullptr;
        void* mb_Object_is_class = nullptr;
        void* mb_CanvasItem_get_global_transform_with_canvas = nullptr;
        void* mb_CanvasItem_get_screen_transform = nullptr;
        void* mb_CanvasItem_is_visible_in_tree = nullptr;

        // Singleton
        void* sceneTree = nullptr;

        // Runtime
        void* activeCamera = nullptr;
        bool functionsResolved = false;
        bool methodBindsResolved = false;
        bool classStringsInitialized = false;

        struct WalkStats
        {
            std::uint32_t visited = 0;
            std::uint32_t node3D = 0;
            std::uint32_t node2D = 0;
            std::uint32_t cameras3D = 0;
            std::uint32_t likely = 0;
            std::uint32_t generic = 0;
            std::uint32_t ignored = 0;
            bool truncated = false;
        };

        ~AutoScanState()
        {
            if (!classStringsInitialized || !g_variant_get_ptr_destructor)
                return;

            auto dtor = reinterpret_cast<GDExtensionPtrDestructor>(g_variant_get_ptr_destructor(kGodotVariantTypeString));
            if (!dtor)
                return;

            dtor(str_Node3D);
            dtor(str_Camera3D);
            dtor(str_CharacterBody3D);
            dtor(str_RigidBody3D);
            dtor(str_Node2D);
            dtor(str_Camera2D);
            dtor(str_CharacterBody2D);
            dtor(str_RigidBody2D);
        }

        void* GetMethodBind(const char* className, const char* methodName)
        {
            if (!fn_get_method_bind)
                return nullptr;
            AutoStringName cls(className);
            AutoStringName method(methodName);
            const int64_t hash = LookupGodot4MethodHash(className, methodName);
            void* bind = fn_get_method_bind(cls.buf, method.buf, hash);
            LogMsg("[AegisGodot] GetMethodBind: %s.%s hash=%lld -> %p\n",
                className ? className : "(null)",
                methodName ? methodName : "(null)",
                static_cast<long long>(hash),
                bind);
            return bind;
        }

        void* GetClassTag(const char* className)
        {
            if (!fn_get_class_tag)
                return nullptr;
            AutoStringName cls(className);
            return fn_get_class_tag(cls.buf);
        }

        bool ResolveFunctions()
        {
            if (functionsResolved)
                return fn_ptrcall != nullptr;

            LogMsg("[AegisGodot] ResolveFunctions: Resolving GDExtension interface functions...\n");
            if (!ResolveGDExtensionFunctions())
            {
                LogMsg("[AegisGodot] ResolveFunctions: ResolveGDExtensionFunctions() failed\n");
                return false;
            }

            HMODULE hMain = GetModuleHandleW(nullptr);
            MODULEINFO mainInfo = {};
            GetModuleInformation(GetCurrentProcess(), hMain, &mainInfo, sizeof(mainInfo));
            uint8_t* mainStart = reinterpret_cast<uint8_t*>(mainInfo.lpBaseOfDll);
            uint8_t* mainEnd = mainStart + mainInfo.SizeOfImage;

            fn_global_get_singleton = g_global_get_singleton;
            fn_ptrcall = ResolveInterfaceFunction<GDExtPtrcallFn>(
                "object_method_bind_ptrcall", mainStart, mainEnd);
            fn_get_method_bind = ResolveInterfaceFunction<GDExtGetMethodBindFn>(
                "classdb_get_method_bind", mainStart, mainEnd);
            fn_get_class_tag = ResolveInterfaceFunction<GDExtGetClassTagFn>(
                "classdb_get_class_tag", mainStart, mainEnd);
            fn_object_cast_to = ResolveInterfaceFunction<GDExtObjectCastToFn>(
                "object_cast_to", mainStart, mainEnd);
            fn_object_get_instance_id = ResolveInterfaceFunction<GDExtObjectGetInstanceIdFn>(
                "object_get_instance_id", mainStart, mainEnd);

            LogMsg("[AegisGodot] ResolveFunctions: global_get_singleton = %p, ptrcall = %p, get_method_bind = %p, get_class_tag = %p, object_cast_to = %p, object_get_instance_id = %p\n",
                fn_global_get_singleton, fn_ptrcall, fn_get_method_bind, fn_get_class_tag, fn_object_cast_to, fn_object_get_instance_id);

            functionsResolved = true;
            return fn_ptrcall != nullptr && fn_get_method_bind != nullptr && fn_global_get_singleton != nullptr;
        }

        void ResetForRetry()
        {
            sceneTree = nullptr;
            activeCamera = nullptr;
            functionsResolved = false;
            methodBindsResolved = false;
            classStringsInitialized = false;
            fn_global_get_singleton = nullptr;
            fn_ptrcall = nullptr;
            fn_get_method_bind = nullptr;
            fn_get_class_tag = nullptr;
            fn_object_cast_to = nullptr;
            fn_object_get_instance_id = nullptr;
            mb_SceneTree_get_root = nullptr;
            mb_Node_get_child_count = nullptr;
            mb_Node_get_child = nullptr;
            mb_Node_get_class = nullptr;
            mb_Node_get_name = nullptr;
            mb_Node3D_get_global_position = nullptr;
            mb_Node3D_is_visible_in_tree = nullptr;
            mb_Camera3D_get_camera_projection = nullptr;
            mb_Camera3D_get_camera_transform = nullptr;
            mb_Object_is_class = nullptr;
            mb_CanvasItem_get_global_transform_with_canvas = nullptr;
            mb_CanvasItem_get_screen_transform = nullptr;
            mb_CanvasItem_is_visible_in_tree = nullptr;
            std::memset(str_Node3D, 0, sizeof(str_Node3D));
            std::memset(str_Camera3D, 0, sizeof(str_Camera3D));
            std::memset(str_CharacterBody3D, 0, sizeof(str_CharacterBody3D));
            std::memset(str_RigidBody3D, 0, sizeof(str_RigidBody3D));
            std::memset(str_Node2D, 0, sizeof(str_Node2D));
            std::memset(str_Camera2D, 0, sizeof(str_Camera2D));
            std::memset(str_CharacterBody2D, 0, sizeof(str_CharacterBody2D));
            std::memset(str_RigidBody2D, 0, sizeof(str_RigidBody2D));
        }

        bool Init()
        {
            if (sceneTree && methodBindsResolved)
                return true;

            LogMsg("[AegisGodot] Init: Initializing auto-scanner state...\n");
            if (!ResolveFunctions())
            {
                LogMsg("[AegisGodot] Init: ResolveFunctions() failed\n");
                return false;
            }

            // Try to get SceneTree singleton
            if (!sceneTree)
            {
                LogMsg("[AegisGodot] Init: calling global_get_singleton(\"SceneTree\")...\n");
                AutoStringName stName("SceneTree");
                sceneTree = fn_global_get_singleton(stName.buf);
                LogMsg("[AegisGodot] Init: sceneTree pointer obtained = %p\n", sceneTree);
                if (!sceneTree)
                {
                    LogMsg("[AegisGodot] Init: SceneTree singleton unavailable, trying Engine.get_main_loop fallback...\n");
                    AutoStringName engineName("Engine");
                    void* engine = fn_global_get_singleton(engineName.buf);
                    LogMsg("[AegisGodot] Init: Engine singleton pointer obtained = %p\n", engine);
                    if (!engine)
                        return false;

                    void* getMainLoop = GetMethodBind("Engine", "get_main_loop");
                    LogMsg("[AegisGodot] Init: Engine.get_main_loop bind = %p\n", getMainLoop);
                    if (!getMainLoop || !fn_ptrcall)
                        return false;

                    void* mainLoop = nullptr;
                    fn_ptrcall(getMainLoop, engine, nullptr, &mainLoop);
                    LogMsg("[AegisGodot] Init: Engine.get_main_loop returned = %p\n", mainLoop);
                    sceneTree = mainLoop;
                    if (!sceneTree)
                        return false;
                }
            }

            // Resolve method binds (only once)
            if (!methodBindsResolved)
            {
                LogMsg("[AegisGodot] Init: resolving SceneTree/Node method binds...\n");
                mb_SceneTree_get_root = GetMethodBind("SceneTree", "get_root");
                mb_Node_get_child_count = GetMethodBind("Node", "get_child_count");
                mb_Node_get_child = GetMethodBind("Node", "get_child");
                mb_Node_get_class = GetMethodBind("Object", "get_class");
                mb_Node_get_name = GetMethodBind("Node", "get_name");
                mb_Node3D_get_global_position = GetMethodBind("Node3D", "get_global_position");
                mb_Node3D_is_visible_in_tree = GetMethodBind("Node3D", "is_visible_in_tree");
                mb_Camera3D_get_camera_projection = GetMethodBind("Camera3D", "get_camera_projection");
                mb_Camera3D_get_camera_transform = GetMethodBind("Camera3D", "get_camera_transform");
                mb_Object_is_class = GetMethodBind("Object", "is_class");
                mb_CanvasItem_get_global_transform_with_canvas = GetMethodBind("CanvasItem", "get_global_transform_with_canvas");
                mb_CanvasItem_get_screen_transform = GetMethodBind("CanvasItem", "get_screen_transform");
                mb_CanvasItem_is_visible_in_tree = GetMethodBind("CanvasItem", "is_visible_in_tree");

                LogMsg("[AegisGodot] Init: Resolved method binds (get_root=%p, child_count=%p, child=%p, class=%p, name=%p, pos=%p, visible=%p, camera_proj=%p, camera_trans=%p, is_class=%p, canvas_tform=%p, screen_tform=%p)\n",
                    mb_SceneTree_get_root, mb_Node_get_child_count, mb_Node_get_child, mb_Node_get_class, mb_Node_get_name,
                    mb_Node3D_get_global_position, mb_Node3D_is_visible_in_tree, mb_Camera3D_get_camera_projection, mb_Camera3D_get_camera_transform,
                    mb_Object_is_class, mb_CanvasItem_get_global_transform_with_canvas, mb_CanvasItem_get_screen_transform);

                // Object::is_class expects a String argument in Godot 4.
                if (g_string_new_with_utf8_chars && !classStringsInitialized)
                {
                    LogMsg("[AegisGodot] Init: caching class String values for Object.is_class...\n");
                    g_string_new_with_utf8_chars(str_Node3D, "Node3D");
                    g_string_new_with_utf8_chars(str_Camera3D, "Camera3D");
                    g_string_new_with_utf8_chars(str_CharacterBody3D, "CharacterBody3D");
                    g_string_new_with_utf8_chars(str_RigidBody3D, "RigidBody3D");
                    g_string_new_with_utf8_chars(str_Node2D, "Node2D");
                    g_string_new_with_utf8_chars(str_Camera2D, "Camera2D");
                    g_string_new_with_utf8_chars(str_CharacterBody2D, "CharacterBody2D");
                    g_string_new_with_utf8_chars(str_RigidBody2D, "RigidBody2D");
                    classStringsInitialized = true;
                }

                methodBindsResolved = true;
            }

            return mb_SceneTree_get_root != nullptr && mb_Node_get_child_count != nullptr && mb_Object_is_class != nullptr;
        }

        bool IsType(void* object, const void* classStringPtr)
        {
            if (!mb_Object_is_class || !fn_ptrcall || !object)
                return false;
            uint8_t ret = 0;
            const void* args[] = { classStringPtr };
            fn_ptrcall(mb_Object_is_class, object, args, &ret);
            return ret != 0;
        }

        std::string GetNodeClass(void* node)
        {
            if (!mb_Node_get_class || !fn_ptrcall)
                return "";

            char retStr[64] = {};
            fn_ptrcall(mb_Node_get_class, node, nullptr, retStr);
            std::string result = GetStringVal(retStr);

            // Destroy the returned String
            if (g_variant_get_ptr_destructor)
            {
                auto dtor = reinterpret_cast<GDExtensionPtrDestructor>(g_variant_get_ptr_destructor(4));
                if (dtor) dtor(retStr);
            }
            return result;
        }

        std::string GetNodeName(void* node)
        {
            if (!mb_Node_get_name || !fn_ptrcall)
                return "";

            char retStringName[64] = {};
            fn_ptrcall(mb_Node_get_name, node, nullptr, retStringName);
            std::string result = GetStringNameVal(retStringName);

            // Destroy the returned StringName
            if (g_variant_get_ptr_destructor)
            {
                auto dtor = reinterpret_cast<GDExtensionPtrDestructor>(g_variant_get_ptr_destructor(kGodotVariantTypeStringName));
                if (dtor) dtor(retStringName);
            }
            return result;
        }

        bool IsIgnoredTargetNode(const std::string& className, const std::string& nodeName) const
        {
            const char* cls = className.c_str();
            const char* name = nodeName.c_str();
            return ContainsAnsi(cls, "audio") || ContainsAnsi(name, "audio") ||
                ContainsAnsi(cls, "sound") || ContainsAnsi(name, "sound") ||
                ContainsAnsi(cls, "listener") || ContainsAnsi(name, "listener") ||
                ContainsAnsi(cls, "music") || ContainsAnsi(name, "music") ||
                ContainsAnsi(cls, "sfx") || ContainsAnsi(name, "sfx") ||
                EqualsAnsi(name, "characters") ||
                EqualsAnsi(name, "character") ||
                ContainsAnsi(name, "characterselect") ||
                ContainsAnsi(name, "character_select") ||
                ContainsAnsi(name, "characterlist") ||
                ContainsAnsi(name, "character_list") ||
                ContainsAnsi(name, "container") ||
                ContainsAnsi(name, "manager") ||
                ContainsAnsi(name, "spawner") ||
                ContainsAnsi(name, "spawnpoint") ||
                ContainsAnsi(name, "spawn_point") ||
                ContainsAnsi(name, "canvas") ||
                ContainsAnsi(name, "hud") ||
                ContainsAnsi(name, "ui") ||
                ContainsAnsi(cls, "meshinstance") ||
                ContainsAnsi(cls, "multimesh") ||
                ContainsAnsi(cls, "label") ||
                ContainsAnsi(cls, "control") ||
                ContainsAnsi(cls, "light") ||
                ContainsAnsi(cls, "collision") ||
                ContainsAnsi(cls, "area") ||
                ContainsAnsi(cls, "raycast") ||
                ContainsAnsi(cls, "marker") ||
                ContainsAnsi(cls, "navigation") ||
                ContainsAnsi(cls, "animation") ||
                ContainsAnsi(cls, "particles") ||
                ContainsAnsi(cls, "skeleton") ||
                ContainsAnsi(cls, "bone");
        }

        bool IsLikelyActorName(const std::string& className, const std::string& nodeName) const
        {
            const char* cls = className.c_str();
            const char* name = nodeName.c_str();
            return ContainsAnsi(name, "player") ||
                ContainsAnsi(name, "localplayer") ||
                ContainsAnsi(name, "mage") ||
                ContainsAnsi(name, "wizard") ||
                ContainsAnsi(name, "enemy") ||
                ContainsAnsi(name, "monster") ||
                ContainsAnsi(name, "npc") ||
                ContainsAnsi(name, "mob") ||
                ContainsAnsi(name, "boss") ||
                ContainsAnsi(cls, "player") ||
                ContainsAnsi(cls, "enemy") ||
                ContainsAnsi(cls, "monster") ||
                ContainsAnsi(cls, "npc") ||
                ContainsAnsi(cls, "mob") ||
                ContainsAnsi(cls, "boss") ||
                ContainsAnsi(cls, "wizard");
        }

        void WalkNode(
            void* node,
            std::vector<AegisGodotObjectSnapshot>& priority,
            std::vector<AegisGodotObjectSnapshot>& generic,
            WalkStats& stats,
            int depth)
        {
            if (!node || depth > 32)
                return;
            if (++stats.visited > 8192)
            {
                stats.truncated = true;
                return;
            }

            // Check if this node is a Node3D or Node2D using is_class
            bool isNode3D = IsType(node, str_Node3D);
            bool isNode2D = IsType(node, str_Node2D);

            if (isNode3D && mb_Node3D_get_global_position)
            {
                ++stats.node3D;
                AegisGodotObjectSnapshot snap = {};

                // Get instance ID
                if (fn_object_get_instance_id)
                    snap.id = fn_object_get_instance_id(node);

                // Get class name
                std::string className = GetNodeClass(node);
                strncpy_s(snap.className, className.c_str(), _TRUNCATE);

                // Get node name
                std::string nodeName = GetNodeName(node);
                strncpy_s(snap.name, nodeName.c_str(), _TRUNCATE);

                // Get global position
                struct Vec3F { float x, y, z; } pos = {};
                fn_ptrcall(mb_Node3D_get_global_position, node, nullptr, &pos);
                snap.origin = { pos.x, pos.y, pos.z };

                // Get visibility
                if (mb_Node3D_is_visible_in_tree)
                {
                    uint8_t visible = 0;
                    fn_ptrcall(mb_Node3D_is_visible_in_tree, node, nullptr, &visible);
                    snap.visible = visible ? 1 : 0;
                }
                else
                {
                    snap.visible = 1;
                }

                // Default bounds (small box)
                snap.boundsMin = { -0.4f, -0.1f, -0.4f };
                snap.boundsMax = { 0.4f, 1.8f, 0.4f };
                snap.flags = AegisGodotObject_Node3D;

                // Check for Camera3D using is_class
                bool isCamera = IsType(node, str_Camera3D);
                if (isCamera)
                {
                    activeCamera = node;
                    ++stats.cameras3D;
                    strncpy_s(snap.name, "Camera3D", _TRUNCATE);
                    snap.flags |= AegisGodotObject_VisibleOnScreen;
                }
                else
                {
                    // Check for character/physics bodies using is_class
                    bool isCharBody = IsType(node, str_CharacterBody3D);
                    bool isRigidBody = IsType(node, str_RigidBody3D);

                    // Check if node/class name implies an actor without promoting generic parent nodes.
                    bool isLikelyPlayerOrNpc = isCharBody || IsLikelyActorName(className, nodeName);

                    if (isCharBody)
                    {
                        snap.group = 1; // Character group
                        snap.boundsMin = { -0.4f, -0.1f, -0.4f };
                        snap.boundsMax = { 0.4f, 2.0f, 0.4f };
                        snap.flags |= AegisGodotObject_PhysicsBody;
                        if (snap.name[0] == '\0')
                            strncpy_s(snap.name, "Character", _TRUNCATE);
                    }
                    else if (isRigidBody)
                    {
                        snap.group = 2; // Physics object group
                        snap.flags |= AegisGodotObject_PhysicsBody;
                        if (snap.name[0] == '\0')
                            strncpy_s(snap.name, "RigidBody", _TRUNCATE);
                    }
                    else
                    {
                        snap.group = 0;
                        if (snap.name[0] == '\0')
                            strncpy_s(snap.name, className.c_str(), _TRUNCATE);
                    }

                    if (isLikelyPlayerOrNpc)
                    {
                        if (IsIgnoredTargetNode(className, nodeName))
                        {
                            ++stats.ignored;
                        }
                        else
                        {
                            snap.flags |= AegisGodotObject_LikelyTarget;
                            ++stats.likely;
                            if (priority.size() < 512)
                                priority.push_back(snap);
                        }
                    }
                    else
                    {
                        ++stats.generic;
                        if (g_drawAllNode3Ds && generic.size() < 512)
                            generic.push_back(snap);
                    }
                }
            }
            else if (isNode2D && (mb_CanvasItem_get_screen_transform || mb_CanvasItem_get_global_transform_with_canvas))
            {
                ++stats.node2D;
                AegisGodotObjectSnapshot snap = {};

                // Get instance ID
                if (fn_object_get_instance_id)
                    snap.id = fn_object_get_instance_id(node);

                // Get class name
                std::string className = GetNodeClass(node);
                strncpy_s(snap.className, className.c_str(), _TRUNCATE);

                // Get node name
                std::string nodeName = GetNodeName(node);
                strncpy_s(snap.name, nodeName.c_str(), _TRUNCATE);

                // Canvas transform gives the actor position after Camera2D/canvas transforms.
                // Screen transform can collapse followed players to the viewport center in stretched 2D games.
                // Transform2D is 24 bytes (3x Vector2 = float columns[3][2]).
                float t2d[6] = {};
                if (mb_CanvasItem_get_global_transform_with_canvas)
                    fn_ptrcall(mb_CanvasItem_get_global_transform_with_canvas, node, nullptr, t2d);
                else if (mb_CanvasItem_get_screen_transform)
                    fn_ptrcall(mb_CanvasItem_get_screen_transform, node, nullptr, t2d);

                const float scaleX = std::sqrt((t2d[0] * t2d[0]) + (t2d[1] * t2d[1]));
                const float scaleY = std::sqrt((t2d[2] * t2d[2]) + (t2d[3] * t2d[3]));
                float zoom = (scaleX > 0.001f && scaleY > 0.001f) ? ((scaleX + scaleY) * 0.5f) : 1.0f;
                if (zoom < 0.001f) zoom = 1.0f;

                // Store 2D canvas/screen coordinates directly in snap.origin.
                snap.origin = { t2d[4], t2d[5], zoom };

                // Get visibility
                if (mb_CanvasItem_is_visible_in_tree)
                {
                    uint8_t visible = 0;
                    fn_ptrcall(mb_CanvasItem_is_visible_in_tree, node, nullptr, &visible);
                    snap.visible = visible ? 1 : 0;
                }
                else
                {
                    snap.visible = 1;
                }

                // Default bounds (we will use this to draw a scaled box around origin)
                snap.boundsMin = { -24.0f, -32.0f, 0.0f };
                snap.boundsMax = { 24.0f, 32.0f, 0.0f };
                snap.flags = AegisGodotObject_Node2D;

                // Check for Camera2D using is_class
                bool isCamera = IsType(node, str_Camera2D);
                if (isCamera)
                {
                    strncpy_s(snap.name, "Camera2D", _TRUNCATE);
                    snap.flags |= AegisGodotObject_VisibleOnScreen;
                }
                else
                {
                    // Check for character/physics bodies using is_class
                    bool isCharBody = IsType(node, str_CharacterBody2D);
                    bool isRigidBody = IsType(node, str_RigidBody2D);

                    // Check if node/class name implies an actor without promoting generic parent nodes.
                    bool isLikelyPlayerOrNpc = isCharBody || IsLikelyActorName(className, nodeName);

                    if (isCharBody)
                    {
                        snap.group = 1; // Character group
                        snap.flags |= AegisGodotObject_PhysicsBody;
                        if (snap.name[0] == '\0')
                            strncpy_s(snap.name, "Character2D", _TRUNCATE);
                    }
                    else if (isRigidBody)
                    {
                        snap.group = 2; // Physics object group
                        snap.flags |= AegisGodotObject_PhysicsBody;
                        if (snap.name[0] == '\0')
                            strncpy_s(snap.name, "RigidBody2D", _TRUNCATE);
                    }
                    else
                    {
                        snap.group = 0;
                        if (snap.name[0] == '\0')
                            strncpy_s(snap.name, className.c_str(), _TRUNCATE);
                    }

                    if (isLikelyPlayerOrNpc)
                    {
                        if (IsIgnoredTargetNode(className, nodeName))
                        {
                            ++stats.ignored;
                        }
                        else
                        {
                            snap.flags |= AegisGodotObject_LikelyTarget;
                            ++stats.likely;
                            if (priority.size() < 512)
                                priority.push_back(snap);
                        }
                    }
                    else
                    {
                        ++stats.generic;
                        if (g_drawAllNode3Ds && generic.size() < 512)
                            generic.push_back(snap);
                    }
                }
            }

            // Recurse into children
            if (mb_Node_get_child_count && mb_Node_get_child && fn_ptrcall)
            {
                int64_t childCount = 0;
                int8_t includeInternal = 0;
                const void* countArgs[] = { &includeInternal };
                fn_ptrcall(mb_Node_get_child_count, node, countArgs, &childCount);

                int64_t maxChildren = (childCount < 256) ? childCount : 256;
                for (int64_t i = 0; i < maxChildren; ++i)
                {
                    void* child = nullptr;
                    int64_t idx = i;
                    int8_t incInternal = 0;
                    const void* childArgs[] = { &idx, &incInternal };
                    fn_ptrcall(mb_Node_get_child, node, childArgs, &child);
                    if (child)
                        WalkNode(child, priority, generic, stats, depth + 1);
                }
            }
        }
    };

    AutoScanState g_autoScan;

    // --- Provider Callbacks ---

    std::uint32_t AEGIS_GODOT_CALL AutoObjectProvider(
        AegisGodotObjectSnapshot* outObjects, std::uint32_t capacity, void*)
    {
        if (!g_autoScan.Init())
            return 0;
        if (!g_autoScan.sceneTree || !g_autoScan.mb_SceneTree_get_root || !g_autoScan.fn_ptrcall)
            return 0;

        g_autoScan.activeCamera = nullptr;

        void* root = nullptr;
        g_autoScan.fn_ptrcall(g_autoScan.mb_SceneTree_get_root, g_autoScan.sceneTree, nullptr, &root);
        if (!root)
            return 0;

        std::vector<AegisGodotObjectSnapshot> priority;
        std::vector<AegisGodotObjectSnapshot> generic;
        AutoScanState::WalkStats stats = {};
        g_autoScan.WalkNode(root, priority, generic, stats, 0);

        std::vector<AegisGodotObjectSnapshot> collected;
        collected.reserve(512);
        for (const auto& snap : priority)
        {
            if (collected.size() >= 512)
                break;
            collected.push_back(snap);
        }
        if (g_drawAllNode3Ds)
        {
            for (const auto& snap : generic)
            {
                if (collected.size() >= 512)
                    break;
                collected.push_back(snap);
            }
        }

        static DWORD lastLogTick = 0;
        const DWORD now = ::GetTickCount();
        if (now - lastLogTick > 1000)
        {
            lastLogTick = now;
            const AegisGodotObjectSnapshot* first = collected.empty() ? nullptr : &collected.front();
            LogMsg("[AegisGodot] AutoObjectProvider: root=%p visited=%u node3d=%u node2d=%u cameras=%u likely=%u ignored=%u generic=%u emitted=%d draw_all=%d truncated=%d active_camera=%p first='%s' class='%s' flags=0x%X pos=(%.1f,%.1f,%.2f)\n",
                root,
                stats.visited,
                stats.node3D,
                stats.node2D,
                stats.cameras3D,
                stats.likely,
                stats.ignored,
                stats.generic,
                static_cast<int>(collected.size()),
                g_drawAllNode3Ds ? 1 : 0,
                stats.truncated ? 1 : 0,
                g_autoScan.activeCamera,
                first ? first->name : "",
                first ? first->className : "",
                first ? first->flags : 0,
                first ? first->origin.x : 0.0f,
                first ? first->origin.y : 0.0f,
                first ? first->origin.z : 0.0f);
        }

        std::uint32_t count = std::min<std::uint32_t>(static_cast<std::uint32_t>(collected.size()), capacity);
        for (std::uint32_t i = 0; i < count; ++i)
            outObjects[i] = collected[i];

        return count;
    }

    int AutoObjectProviderSafe(AegisGodotObjectSnapshot* outObjects, std::uint32_t capacity, void* ud)
    {
        __try
        {
            return static_cast<int>(AutoObjectProvider(outObjects, capacity, ud));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::uint32_t AEGIS_GODOT_CALL AutoObjectProviderWrapper(
        AegisGodotObjectSnapshot* outObjects, std::uint32_t capacity, void* userData)
    {
        int result = AutoObjectProviderSafe(outObjects, capacity, userData);
        return static_cast<std::uint32_t>(result >= 0 ? result : 0);
    }

    std::int32_t AEGIS_GODOT_CALL AutoViewProjectionProvider(AegisGodotMatrix4x4* outMatrix, void*)
    {
        if (!g_autoScan.activeCamera || !g_autoScan.fn_ptrcall)
            return 0;
        if (!g_autoScan.mb_Camera3D_get_camera_projection || !g_autoScan.mb_Camera3D_get_camera_transform)
            return 0;

        __try
        {
            // Get camera projection matrix (Projection = 4x Vector4 = 16 floats, column-major)
            float projection[16] = {};
            g_autoScan.fn_ptrcall(g_autoScan.mb_Camera3D_get_camera_projection, g_autoScan.activeCamera, nullptr, projection);

            // Get camera transform (Transform3D = Basis(3x Vector3) + Vector3 origin = 12 floats)
            // Layout: basis.rows[0](xyz), basis.rows[1](xyz), basis.rows[2](xyz), origin(xyz)
            float transform[12] = {};
            g_autoScan.fn_ptrcall(g_autoScan.mb_Camera3D_get_camera_transform, g_autoScan.activeCamera, nullptr, transform);

            const float* r0 = &transform[0]; // basis row 0
            const float* r1 = &transform[3]; // basis row 1
            const float* r2 = &transform[6]; // basis row 2
            const float* origin = &transform[9];

            // Build view matrix (inverse of camera world transform)
            // For orthogonal rotation: V = [R^T | -R^T * t]
            // Column-major storage
            float view[16];
            view[0]  = r0[0]; view[4]  = r1[0]; view[8]  = r2[0]; view[12] = -(r0[0]*origin[0] + r1[0]*origin[1] + r2[0]*origin[2]);
            view[1]  = r0[1]; view[5]  = r1[1]; view[9]  = r2[1]; view[13] = -(r0[1]*origin[0] + r1[1]*origin[1] + r2[1]*origin[2]);
            view[2]  = r0[2]; view[6]  = r1[2]; view[10] = r2[2]; view[14] = -(r0[2]*origin[0] + r1[2]*origin[1] + r2[2]*origin[2]);
            view[3]  = 0.0f;  view[7]  = 0.0f;  view[11] = 0.0f;  view[15] = 1.0f;

            // Multiply VP = Projection * View (column-major)
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        sum += projection[k * 4 + row] * view[col * 4 + k];
                    outMatrix->m[col * 4 + row] = sum;
                }
            }

            outMatrix->flags = AegisGodotMatrix_ColumnMajor | AegisGodotMatrix_OpenGLDepth;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::int32_t AEGIS_GODOT_CALL AutoViewportProvider(AegisGodotViewport* outViewport, void*)
    {
        // Find the game window and use its client rect
        DWORD pid = ::GetCurrentProcessId();
        struct EnumData { DWORD pid; HWND best; int bestArea; } data = { pid, nullptr, 0 };

        ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* d = reinterpret_cast<EnumData*>(lParam);
            DWORD wPid = 0;
            ::GetWindowThreadProcessId(hwnd, &wPid);
            if (wPid != d->pid || !::IsWindowVisible(hwnd))
                return TRUE;

            wchar_t title[256] = {};
            ::GetWindowTextW(hwnd, title, 256);
            if (wcsstr(title, L"Aegis") != nullptr)
                return TRUE;

            wchar_t className[256] = {};
            ::GetClassNameW(hwnd, className, 256);
            if (wcscmp(className, L"ConsoleWindowClass") == 0)
                return TRUE;
            if (wcscmp(className, L"AegisOverlayClass") == 0)
                return TRUE;

            RECT rect = {};
            ::GetClientRect(hwnd, &rect);
            int area = (rect.right - rect.left) * (rect.bottom - rect.top);
            if (area > d->bestArea)
            {
                d->bestArea = area;
                d->best = hwnd;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&data));

        if (!data.best)
            return 0;

        RECT rect = {};
        ::GetClientRect(data.best, &rect);
        outViewport->x = 0.0f;
        outViewport->y = 0.0f;
        outViewport->width = static_cast<float>(rect.right - rect.left);
        outViewport->height = static_cast<float>(rect.bottom - rect.top);
        return (outViewport->width > 1.0f && outViewport->height > 1.0f) ? 1 : 0;
    }

    bool Godot4BackendInit()
    {
        return g_autoScan.Init();
    }

    void* Godot4BackendGetActiveCamera()
    {
        return g_autoScan.activeCamera;
    }

    const GodotAutoScanBackend kGodot4Backend =
    {
        &Godot4BackendInit,
        &Godot4BackendGetActiveCamera,
        &AutoObjectProviderWrapper,
        &AutoViewProjectionProvider,
        "Godot 4 GDExtension"
    };
}

const GodotAutoScanBackend& GetGodot4AutoScanBackend()
{
    return kGodot4Backend;
}

static GodotRuntimeBackendKind DetectGodotRuntimeBackendKind()
{
    return ProbeGodotRuntimeProfile().backend;
}

const GodotAutoScanBackend& GetGodotAutoScanBackend()
{
    switch (DetectGodotRuntimeBackendKind())
    {
    case GodotRuntimeBackendKind::Godot3:
        return GetGodot3AutoScanBackend();
    case GodotRuntimeBackendKind::Godot4:
    default:
        return GetGodot4AutoScanBackend();
    }
}

AEGIS_UNIVERSAL_API int AegisGodot_WriteResolverReport(const wchar_t* path)
{
    if (!path || !path[0])
        return GodotResolver_WriteReport(DefaultResolverReportPath().c_str());
    return GodotResolver_WriteReport(path);
}

AEGIS_UNIVERSAL_API int AegisGodot_InitAutoProviders()
{
    static bool succeeded = false;
    static DWORD s_nextInitAttemptTick = 0;
    static bool loggedAttempt = false;
    if (succeeded)
        return 1;

    const DWORD now = ::GetTickCount();
    if (s_nextInitAttemptTick != 0 && now < s_nextInitAttemptTick)
        return 0;

    const GodotRuntimeProfile& profile = ProbeGodotRuntimeProfile();
    if (!profile.autoScanSupported)
    {
        static bool loggedUnsupported = false;
        if (!loggedUnsupported)
        {
            LogMsg("[AegisGodot] AegisGodot_InitAutoProviders: Auto-scan blocked for this runtime (%s)\n",
                profile.blockReason ? profile.blockReason : "unsupported profile");
            loggedUnsupported = true;
        }
        return 0;
    }

    const GodotAutoScanBackend& backend = GetGodotAutoScanBackend();

    if (profile.backend == GodotRuntimeBackendKind::Godot4 &&
        g_gdextension_resolution_done && !g_gdextension_resolution_ok)
    {
        if (g_gdextension_retry_after_tick != 0 && now < g_gdextension_retry_after_tick)
            return 0;
        ResetGodot4ApiResolution();
    }

    if (!loggedAttempt)
    {
        LogMsg("[AegisGodot] AegisGodot_InitAutoProviders: Attempting auto-provider initialization (%s)...\n", backend.label);
        loggedAttempt = true;
    }

    bool initOk = false;
    __try
    {
        initOk = backend.Init ? backend.Init() : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogMsg("[AegisGodot] AegisGodot_InitAutoProviders: Auto-scan Init crashed (SEH 0x%08X), resetting and backing off for 30s\n",
            ::GetExceptionCode());
        if (profile.backend == GodotRuntimeBackendKind::Godot4)
        {
            ResetGodot4ApiResolution();
            g_autoScan.ResetForRetry();
            g_gdextension_retry_after_tick = now + 30000;
        }
        s_nextInitAttemptTick = now + 30000;
        loggedAttempt = false;
        return 0;
    }

    if (!initOk)
    {
        LogMsg("[AegisGodot] AegisGodot_InitAutoProviders: Auto-scan Init failed, backing off for 30s\n");
        if (profile.backend == GodotRuntimeBackendKind::Godot4)
        {
            ResetGodot4ApiResolution();
            g_autoScan.ResetForRetry();
            g_gdextension_retry_after_tick = now + 30000;
        }
        s_nextInitAttemptTick = now + 30000;
        loggedAttempt = false;
        return 0;
    }

    // Log success
    LogMsg("[AegisGodot] Auto-scan initialized via %s: SceneTree found, providers registered\n", backend.label);
    loggedAttempt = false;

    // Register providers
    AegisGodot_RegisterObjectProvider(backend.CollectObjects, nullptr);
    AegisGodot_RegisterViewProjectionProvider(backend.GetViewProjection, nullptr);
    AegisGodot_RegisterViewportProvider(AutoViewportProvider, nullptr);

    succeeded = true;
    return 1;
}
