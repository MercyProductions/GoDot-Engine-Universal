#include "AegisGodotUniversal.h"

#include <Windows.h>

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

namespace
{
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

    if (providers.matrixProvider)
    {
        const LARGE_INTEGER start = NowCounter();
        hasMatrix = providers.matrixProvider(&matrix, providers.matrixUserData) != 0 && IsValidMatrix(matrix);
        matrixMs = ElapsedMs(start, NowCounter());
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

    std::wstringstream details;
    details << L"modules " << runtime.moduleCount
            << L", matched exports " << runtime.matchedExportCount
            << L", pck section " << (outInfo->embeddedPackSectionFound ? L"yes" : L"no")
            << L", objects " << g_objects.size()
            << L", projected " << g_timing.projectedCount
            << L", clipped " << g_timing.clippedCount;
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
