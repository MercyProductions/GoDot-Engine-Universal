#include "AegisUniversalRuntimeInternal.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct SdkExportRecord
    {
        std::string exportName;
        std::wstring moduleName;
        std::wstring modulePath;
        std::uintptr_t address = 0;
        std::uintptr_t rva = 0;
        std::uint32_t ordinal = 0;
        std::uint32_t flags = AegisUniversalSignature_None;
        std::uint32_t source = AegisUniversalSdkSource_None;
    };

    std::mutex g_sdkMutex;
    std::vector<SdkExportRecord> g_loadedSdkExports;

    template <std::size_t N>
    void CopyWide(wchar_t (&dest)[N], const std::wstring& value)
    {
        wcsncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    template <std::size_t N>
    void CopyAnsi(char (&dest)[N], const std::string& value)
    {
        strncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::wstring WidenAscii(const std::string& value)
    {
        std::wstring result;
        result.reserve(value.size());
        for (unsigned char ch : value)
            result.push_back(static_cast<wchar_t>(ch));
        return result;
    }

    std::string NarrowWide(const std::wstring& value)
    {
        std::string result;
        result.reserve(value.size());
        for (wchar_t ch : value)
            result.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
        return result;
    }

    std::string JsonEscape(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (char ch : value)
        {
            switch (ch)
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
                escaped.push_back(ch);
                break;
            }
        }
        return escaped;
    }

    std::string JsonEscapeWide(const std::wstring& value)
    {
        return JsonEscape(NarrowWide(value));
    }

    std::string CsvEscape(const std::string& value)
    {
        const bool needsQuotes = value.find_first_of(",\"\r\n") != std::string::npos;
        if (!needsQuotes)
            return value;

        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');
        for (const char ch : value)
        {
            if (ch == '"')
                escaped.push_back('"');
            escaped.push_back(ch);
        }
        escaped.push_back('"');
        return escaped;
    }

    std::string CsvEscapeWide(const std::wstring& value)
    {
        return CsvEscape(NarrowWide(value));
    }

    bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
    {
        return !needle.empty() && ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
    }

    bool EqualsInsensitive(const std::wstring& left, const std::wstring& right)
    {
        return !left.empty() && !right.empty() && ToLower(left) == ToLower(right);
    }

    bool EqualsInsensitiveAscii(const std::string& left, const std::string& right)
    {
        return !left.empty() && !right.empty() && ToLowerAscii(left) == ToLowerAscii(right);
    }

    bool MatchModule(const AegisUniversalModuleInfo& module, const wchar_t* moduleName)
    {
        if (!moduleName || !moduleName[0])
            return true;
        const std::wstring needle = moduleName;
        return EqualsInsensitive(module.name, needle) ||
            EqualsInsensitive(module.path, needle) ||
            ContainsInsensitive(module.name, needle) ||
            ContainsInsensitive(module.path, needle);
    }

    std::string FlagsHex(std::uint32_t flags)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << flags;
        return stream.str();
    }

    std::string PointerHex(std::uintptr_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << value;
        return stream.str();
    }

    bool IsRuntimeInitialized()
    {
        if (AegisUniversal_IsInitialized())
            return true;
        AegisUniversal_Initialize();
        return AegisUniversal_IsInitialized() != 0;
    }

    std::vector<AegisUniversalModuleInfo> CurrentModules()
    {
        std::vector<AegisUniversalModuleInfo> modules;
        if (!IsRuntimeInitialized())
            return modules;

        const std::uint32_t count = AegisUniversal_GetModuleCount();
        modules.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            AegisUniversalModuleInfo module = {};
            if (AegisUniversal_GetModuleInfo(index, &module))
                modules.push_back(module);
        }
        return modules;
    }

    bool ShouldDumpModuleExports(const AegisUniversalModuleInfo& module, const AegisUniversalRuntimeInfo& runtime)
    {
        if (module.flags != AegisUniversalSignature_None)
            return true;
        if (EqualsInsensitive(module.path, runtime.processPath))
            return true;

        try
        {
            const std::filesystem::path moduleDir = std::filesystem::path(module.path).parent_path();
            const std::filesystem::path processDir = std::filesystem::path(runtime.processPath).parent_path();
            if (!moduleDir.empty() && !processDir.empty() &&
                EqualsInsensitive(moduleDir.wstring(), processDir.wstring()))
            {
                return true;
            }
        }
        catch (...)
        {
        }

        const AegisUniversalProfile& profile = AegisUniversal_GetProfile();
        if (profile.shortName && profile.shortName[0] && ContainsInsensitive(module.name, profile.shortName))
            return true;
        if (profile.engineName && profile.engineName[0] && ContainsInsensitive(module.name, profile.engineName))
            return true;
        return false;
    }

    std::vector<SdkExportRecord> EnumeratePeExports(const AegisUniversalModuleInfo& module)
    {
        std::vector<SdkExportRecord> exports;
        if (module.baseAddress == 0)
            return exports;

        const auto base = reinterpret_cast<const std::uint8_t*>(module.baseAddress);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return exports;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
            return exports;

        const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0 || directory.Size == 0)
            return exports;

        const auto* exportDirectory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        if (!exportDirectory || exportDirectory->NumberOfFunctions == 0)
            return exports;

        const auto* names = reinterpret_cast<const DWORD*>(base + exportDirectory->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(base + exportDirectory->AddressOfNameOrdinals);
        const auto* functions = reinterpret_cast<const DWORD*>(base + exportDirectory->AddressOfFunctions);

        exports.reserve(exportDirectory->NumberOfNames);
        for (DWORD index = 0; index < exportDirectory->NumberOfNames; ++index)
        {
            const char* name = reinterpret_cast<const char*>(base + names[index]);
            if (!name || !name[0])
                continue;

            const WORD ordinalIndex = ordinals[index];
            if (ordinalIndex >= exportDirectory->NumberOfFunctions)
                continue;

            const DWORD rva = functions[ordinalIndex];
            if (rva == 0)
                continue;

            SdkExportRecord record;
            record.exportName = name;
            record.moduleName = module.name;
            record.modulePath = module.path;
            record.address = module.baseAddress + rva;
            record.rva = rva;
            record.ordinal = exportDirectory->Base + ordinalIndex;
            record.flags = module.flags;
            record.source = AegisUniversalSdkSource_DumpedSdk;
            exports.push_back(std::move(record));
        }
        return exports;
    }

    std::vector<SdkExportRecord> BuildSdkExports()
    {
        std::vector<SdkExportRecord> records;
        AegisUniversalRuntimeInfo runtime = {};
        AegisUniversal_GetRuntimeInfo(&runtime);

        for (const AegisUniversalModuleInfo& module : CurrentModules())
        {
            if (!ShouldDumpModuleExports(module, runtime))
                continue;
            std::vector<SdkExportRecord> moduleExports = EnumeratePeExports(module);
            records.insert(records.end(), moduleExports.begin(), moduleExports.end());
        }

        const std::uint32_t matchedCount = AegisUniversal_GetMatchedExportCount();
        for (std::uint32_t index = 0; index < matchedCount; ++index)
        {
            AegisUniversalExportInfo exportInfo = {};
            if (!AegisUniversal_GetMatchedExportInfo(index, &exportInfo))
                continue;

            const std::uintptr_t moduleBase = [&]() -> std::uintptr_t {
                for (const AegisUniversalModuleInfo& module : CurrentModules())
                {
                    if (EqualsInsensitive(module.name, exportInfo.moduleName) || EqualsInsensitive(module.path, exportInfo.modulePath))
                        return module.baseAddress;
                }
                return 0;
            }();

            const std::uintptr_t rva = moduleBase && exportInfo.address >= moduleBase ? exportInfo.address - moduleBase : 0;
            const auto duplicate = std::find_if(records.begin(), records.end(), [&](const SdkExportRecord& record) {
                return EqualsInsensitive(record.moduleName, exportInfo.moduleName) &&
                    EqualsInsensitiveAscii(record.exportName, exportInfo.exportName);
            });
            if (duplicate != records.end())
            {
                duplicate->flags |= exportInfo.flags;
                duplicate->source |= AegisUniversalSdkSource_LiveExport;
                continue;
            }

            SdkExportRecord record;
            record.exportName = exportInfo.exportName;
            record.moduleName = exportInfo.moduleName;
            record.modulePath = exportInfo.modulePath;
            record.address = exportInfo.address;
            record.rva = rva;
            record.flags = exportInfo.flags;
            record.source = AegisUniversalSdkSource_LiveExport | AegisUniversalSdkSource_DumpedSdk;
            records.push_back(std::move(record));
        }

        std::sort(records.begin(), records.end(), [](const SdkExportRecord& left, const SdkExportRecord& right) {
            const std::wstring leftModule = ToLower(left.moduleName);
            const std::wstring rightModule = ToLower(right.moduleName);
            if (leftModule != rightModule)
                return leftModule < rightModule;
            return ToLowerAscii(left.exportName) < ToLowerAscii(right.exportName);
        });
        return records;
    }

    std::string ProviderSchemaJson()
    {
        const std::wstring engine = ToLower(AegisUniversal_GetEngineName() ? AegisUniversal_GetEngineName() : L"");
        if (engine.find(L"id tech") != std::wstring::npos || engine.find(L"idtech") != std::wstring::npos)
        {
            return R"({
      "snapshot": "AegisIdTechEntitySnapshot",
      "provider": "AegisIdTech_RegisterEntityProvider",
      "matrixProvider": "AegisIdTech_RegisterViewProjectionProvider",
      "viewportProvider": "AegisIdTech_RegisterViewportProvider",
      "fields": ["id", "name", "className", "origin", "boundsMin", "boundsMax", "team", "visible", "flags"]
    })";
        }
        if (engine.find(L"godot") != std::wstring::npos)
        {
            return R"({
      "snapshot": "AegisGodotObjectSnapshot",
      "provider": "AegisGodot_RegisterObjectProvider",
      "matrixProvider": "AegisGodot_RegisterViewProjectionProvider",
      "viewportProvider": "AegisGodot_RegisterViewportProvider",
      "fields": ["id", "name", "className", "path", "origin", "boundsMin", "boundsMax", "group", "visible", "flags"]
    })";
        }
        if (engine.find(L"gamemaker") != std::wstring::npos || engine.find(L"game maker") != std::wstring::npos)
        {
            return R"({
      "snapshot": "AegisGameMakerInstanceSnapshot",
      "provider": "AegisGameMaker_RegisterInstanceProvider",
      "matrixProvider": "AegisGameMaker_RegisterViewProjectionProvider",
      "viewportProvider": "AegisGameMaker_RegisterViewportProvider",
      "fields": ["id", "objectName", "instanceName", "roomName", "position", "boundsMin", "boundsMax", "layerId", "depth", "visible", "active", "flags"]
    })";
        }
        return R"({
      "snapshot": "unknown",
      "provider": "unknown",
      "matrixProvider": "unknown",
      "viewportProvider": "unknown",
      "fields": []
    })";
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
                const char next = object[pos + 1];
                switch (next)
                {
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                default:
                    result.push_back(next);
                    break;
                }
                ++pos;
                continue;
            }
            if (ch == '"')
                break;
            result.push_back(ch);
        }
        return result;
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
        ++pos;
        while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos])))
            ++pos;
        if (pos < object.size() && object[pos] == '"')
            ++pos;

        char* end = nullptr;
        const unsigned long long value = std::strtoull(object.c_str() + pos, &end, 0);
        if (end == object.c_str() + pos)
            return false;
        outValue = static_cast<std::uint64_t>(value);
        return true;
    }

    std::size_t FindMatchingArrayEnd(const std::string& json, std::size_t arrayStart)
    {
        int depth = 0;
        bool inString = false;
        bool escape = false;
        for (std::size_t pos = arrayStart; pos < json.size(); ++pos)
        {
            const char ch = json[pos];
            if (escape)
            {
                escape = false;
                continue;
            }
            if (ch == '\\' && inString)
            {
                escape = true;
                continue;
            }
            if (ch == '"')
            {
                inString = !inString;
                continue;
            }
            if (inString)
                continue;
            if (ch == '[')
                ++depth;
            else if (ch == ']')
            {
                --depth;
                if (depth == 0)
                    return pos;
            }
        }
        return std::string::npos;
    }

    void FillSdkExportInfo(const SdkExportRecord& record, AegisUniversalSdkExportInfo& outInfo)
    {
        outInfo = {};
        CopyAnsi(outInfo.exportName, record.exportName);
        CopyWide(outInfo.moduleName, record.moduleName);
        CopyWide(outInfo.modulePath, record.modulePath);
        outInfo.address = record.address;
        outInfo.rva = record.rva;
        outInfo.ordinal = record.ordinal;
        outInfo.flags = record.flags;
        outInfo.source = record.source;
    }

    void FillResolvedSymbol(const SdkExportRecord& record, bool loaded, AegisUniversalResolvedSymbol& outSymbol)
    {
        outSymbol = {};
        CopyAnsi(outSymbol.exportName, record.exportName);
        CopyWide(outSymbol.moduleName, record.moduleName);
        CopyWide(outSymbol.modulePath, record.modulePath);
        outSymbol.address = record.address;
        outSymbol.rva = record.rva;
        outSymbol.ordinal = record.ordinal;
        outSymbol.flags = record.flags;
        outSymbol.source = record.source;
        outSymbol.loaded = loaded ? 1 : 0;
    }

    bool TryLiveResolve(const wchar_t* moduleName, const char* exportName, AegisUniversalResolvedSymbol& outSymbol)
    {
        if (!exportName || !exportName[0])
            return false;

        for (const AegisUniversalModuleInfo& module : CurrentModules())
        {
            if (!MatchModule(module, moduleName))
                continue;

            HMODULE handle = reinterpret_cast<HMODULE>(module.baseAddress);
            FARPROC address = handle ? ::GetProcAddress(handle, exportName) : nullptr;
            if (!address)
                continue;

            SdkExportRecord record;
            record.exportName = exportName;
            record.moduleName = module.name;
            record.modulePath = module.path;
            record.address = reinterpret_cast<std::uintptr_t>(address);
            record.rva = record.address >= module.baseAddress ? record.address - module.baseAddress : 0;
            record.flags = module.flags;
            record.source = AegisUniversalSdkSource_LiveExport;
            FillResolvedSymbol(record, true, outSymbol);
            return true;
        }
        return false;
    }

    bool TryGetModuleForSdkRecord(const SdkExportRecord& record, AegisUniversalModuleInfo& outModule)
    {
        for (const AegisUniversalModuleInfo& module : CurrentModules())
        {
            if (EqualsInsensitive(module.name, record.moduleName) || EqualsInsensitive(module.path, record.modulePath))
            {
                outModule = module;
                return true;
            }
        }
        return false;
    }

    bool FindModuleForSdkRecord(const std::vector<AegisUniversalModuleInfo>& modules, const SdkExportRecord& record, AegisUniversalModuleInfo& outModule)
    {
        for (const AegisUniversalModuleInfo& module : modules)
        {
            if (EqualsInsensitive(module.name, record.moduleName) || EqualsInsensitive(module.path, record.modulePath))
            {
                outModule = module;
                return true;
            }
        }
        return false;
    }

    bool LooksLikeSelfReference(const SdkExportRecord& record)
    {
        const std::wstring combined = ToLower(record.moduleName + L" " + record.modulePath);
        return combined.find(L"aegis") != std::wstring::npos &&
            combined.find(L"universal") != std::wstring::npos;
    }

    std::vector<SdkExportRecord> SnapshotSdkExports()
    {
        std::lock_guard lock(g_sdkMutex);
        if (!g_loadedSdkExports.empty())
            return g_loadedSdkExports;
        return {};
    }

    AegisUniversalSdkValidationInfo BuildValidationInfo()
    {
        AegisUniversalSdkValidationInfo info = {};
        info.size = sizeof(info);

        std::vector<SdkExportRecord> loaded;
        {
            std::lock_guard lock(g_sdkMutex);
            loaded = g_loadedSdkExports;
        }

        info.sdkLoaded = loaded.empty() ? 0 : 1;
        info.loadedExportCount = static_cast<std::uint32_t>(loaded.size());

        if (loaded.empty())
        {
            CopyWide(info.details, L"No SDK JSON has been loaded into resolver memory yet.");
            return info;
        }

        const std::vector<AegisUniversalModuleInfo> modules = CurrentModules();
        for (const SdkExportRecord& record : loaded)
        {
            if (LooksLikeSelfReference(record))
                ++info.selfReferenceCount;

            AegisUniversalModuleInfo module = {};
            if (!FindModuleForSdkRecord(modules, record, module))
            {
                ++info.sdkOnlyCount;
                continue;
            }

            ++info.moduleMatchedCount;
            if (record.rva != 0 && record.rva < module.imageSize)
            {
                ++info.liveResolvedCount;
                continue;
            }

            if (record.rva >= module.imageSize)
                ++info.staleRvaCount;
            else
            {
                AegisUniversalResolvedSymbol symbol = {};
                if (TryLiveResolve(record.moduleName.c_str(), record.exportName.c_str(), symbol))
                    ++info.liveResolvedCount;
            }
        }

        std::wstringstream details;
        details << L"Loaded " << info.loadedExportCount
                << L" SDK exports; " << info.liveResolvedCount
                << L" resolve against currently loaded modules, " << info.sdkOnlyCount
                << L" are SDK-only, " << info.staleRvaCount
                << L" have stale RVAs.";
        if (info.selfReferenceCount)
            details << L" Self references found: " << info.selfReferenceCount << L".";
        CopyWide(info.details, details.str());
        return info;
    }
}

AEGIS_UNIVERSAL_API int AegisUniversal_DumpSdkJson(const wchar_t* sdkPath)
{
    if (!sdkPath || !sdkPath[0])
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    AegisUniversalRuntimeInfo runtime = {};
    AegisUniversal_GetRuntimeInfo(&runtime);
    const std::vector<AegisUniversalModuleInfo> modules = CurrentModules();
    const std::vector<SdkExportRecord> exports = BuildSdkExports();
    const AegisUniversalProfile& profile = AegisUniversal_GetProfile();

    std::ofstream out(std::filesystem::path(sdkPath), std::ios::binary | std::ios::trunc);
    if (!out)
        return 0;

    out << "{\n";
    out << "  \"format\": \"AegisUniversalSdk\",\n";
    out << "  \"version\": 1,\n";
    out << "  \"engine\": \"" << JsonEscapeWide(runtime.engineName) << "\",\n";
    out << "  \"processName\": \"" << JsonEscapeWide(runtime.processName) << "\",\n";
    out << "  \"processPath\": \"" << JsonEscapeWide(runtime.processPath) << "\",\n";
    out << "  \"runtimeFlags\": " << runtime.flags << ",\n";
    out << "  \"selfModuleFiltered\": true,\n";
    out << "  \"providerSchema\": " << ProviderSchemaJson() << ",\n";
    out << "  \"signatures\": [\n";
    for (std::size_t index = 0; index < profile.signatureCount; ++index)
    {
        const AegisUniversalSignature& signature = profile.signatures[index];
        out << "    { \"processHint\": \"" << JsonEscapeWide(signature.processHint ? signature.processHint : L"")
            << "\", \"moduleHint\": \"" << JsonEscapeWide(signature.moduleHint ? signature.moduleHint : L"")
            << "\", \"exportName\": \"" << JsonEscape(signature.exportName ? signature.exportName : "")
            << "\", \"flags\": " << signature.flags
            << ", \"note\": \"" << JsonEscape(signature.note ? signature.note : "") << "\" }";
        if (index + 1 < profile.signatureCount)
            out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"modules\": [\n";
    for (std::size_t index = 0; index < modules.size(); ++index)
    {
        const AegisUniversalModuleInfo& module = modules[index];
        out << "    { \"name\": \"" << JsonEscapeWide(module.name)
            << "\", \"path\": \"" << JsonEscapeWide(module.path)
            << "\", \"baseAddress\": " << module.baseAddress
            << ", \"baseAddressHex\": \"" << PointerHex(module.baseAddress)
            << "\", \"imageSize\": " << module.imageSize
            << ", \"flags\": " << module.flags
            << ", \"flagsHex\": \"" << FlagsHex(module.flags) << "\" }";
        if (index + 1 < modules.size())
            out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"exports\": [\n";
    for (std::size_t index = 0; index < exports.size(); ++index)
    {
        const SdkExportRecord& record = exports[index];
        out << "    { \"moduleName\": \"" << JsonEscapeWide(record.moduleName)
            << "\", \"modulePath\": \"" << JsonEscapeWide(record.modulePath)
            << "\", \"exportName\": \"" << JsonEscape(record.exportName)
            << "\", \"address\": " << record.address
            << ", \"addressHex\": \"" << PointerHex(record.address)
            << "\", \"rva\": " << record.rva
            << ", \"rvaHex\": \"" << PointerHex(record.rva)
            << "\", \"ordinal\": " << record.ordinal
            << ", \"flags\": " << record.flags
            << ", \"source\": " << record.source << " }";
        if (index + 1 < exports.size())
            out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_WriteSdkHeader(const wchar_t* headerPath)
{
    if (!headerPath || !headerPath[0])
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    AegisUniversalRuntimeInfo runtime = {};
    AegisUniversal_GetRuntimeInfo(&runtime);
    const std::vector<SdkExportRecord> exports = BuildSdkExports();

    std::ofstream out(std::filesystem::path(headerPath), std::ios::binary | std::ios::trunc);
    if (!out)
        return 0;

    out << "#pragma once\n\n";
    out << "// Generated by Aegis Universal SDK dumper.\n";
    out << "// Engine: " << JsonEscapeWide(runtime.engineName) << "\n";
    out << "// Process: " << JsonEscapeWide(runtime.processName) << "\n";
    out << "// The universal DLL filters itself out of engine detection before this SDK is generated.\n\n";
    out << "#include <stdint.h>\n\n";
    out << "typedef struct AegisGeneratedSdkExport\n";
    out << "{\n";
    out << "    const char* moduleName;\n";
    out << "    const char* exportName;\n";
    out << "    uint64_t rva;\n";
    out << "    uint32_t ordinal;\n";
    out << "    uint32_t flags;\n";
    out << "} AegisGeneratedSdkExport;\n\n";
    out << "static const AegisGeneratedSdkExport kAegisGeneratedSdkExports[] = {\n";
    for (const SdkExportRecord& record : exports)
    {
        out << "    { \"" << JsonEscapeWide(record.moduleName)
            << "\", \"" << JsonEscape(record.exportName)
            << "\", UINT64_C(" << record.rva << "), "
            << record.ordinal << "u, " << record.flags << "u },\n";
    }
    out << "};\n\n";
    out << "static const unsigned int kAegisGeneratedSdkExportCount = "
        << static_cast<unsigned int>(exports.size()) << "u;\n";
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_WriteSdkMapCsv(const wchar_t* csvPath)
{
    if (!csvPath || !csvPath[0])
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    std::vector<SdkExportRecord> records = SnapshotSdkExports();
    if (records.empty())
        records = BuildSdkExports();

    const std::vector<AegisUniversalModuleInfo> modules = CurrentModules();
    std::ofstream out(std::filesystem::path(csvPath), std::ios::binary | std::ios::trunc);
    if (!out)
        return 0;

    out << "Module,Path,ExportName,Ordinal,RVA,Address,ResolvedAddress,Flags,Source,Loaded\n";
    for (const SdkExportRecord& record : records)
    {
        AegisUniversalModuleInfo module = {};
        const bool loaded = FindModuleForSdkRecord(modules, record, module);
        const std::uintptr_t resolvedAddress =
            loaded && record.rva != 0 && record.rva < module.imageSize ? module.baseAddress + record.rva : 0;

        out << CsvEscapeWide(record.moduleName) << ","
            << CsvEscapeWide(record.modulePath) << ","
            << CsvEscape(record.exportName) << ","
            << record.ordinal << ","
            << PointerHex(record.rva) << ","
            << PointerHex(record.address) << ","
            << PointerHex(resolvedAddress) << ","
            << FlagsHex(record.flags) << ","
            << FlagsHex(record.source) << ","
            << (loaded ? "true" : "false") << "\n";
    }
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_LoadSdkJson(const wchar_t* sdkPath)
{
    if (!sdkPath || !sdkPath[0])
        return 0;

    std::ifstream in(std::filesystem::path(sdkPath), std::ios::binary);
    if (!in)
        return 0;

    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t exportsKey = json.find("\"exports\"");
    if (exportsKey == std::string::npos)
        return 0;
    const std::size_t arrayStart = json.find('[', exportsKey);
    if (arrayStart == std::string::npos)
        return 0;
    const std::size_t arrayEnd = FindMatchingArrayEnd(json, arrayStart);
    if (arrayEnd == std::string::npos)
        return 0;

    std::vector<SdkExportRecord> loaded;
    std::size_t pos = json.find('{', arrayStart);
    while (pos != std::string::npos && pos < arrayEnd)
    {
        const std::size_t end = json.find('}', pos);
        if (end == std::string::npos || end > arrayEnd)
            break;

        const std::string object = json.substr(pos, end - pos + 1);
        SdkExportRecord record;
        record.moduleName = WidenAscii(ExtractJsonString(object, "moduleName"));
        record.modulePath = WidenAscii(ExtractJsonString(object, "modulePath"));
        record.exportName = ExtractJsonString(object, "exportName");
        std::uint64_t value = 0;
        if (ExtractJsonUInt64(object, "rva", value))
            record.rva = static_cast<std::uintptr_t>(value);
        if (ExtractJsonUInt64(object, "address", value))
            record.address = static_cast<std::uintptr_t>(value);
        if (ExtractJsonUInt64(object, "ordinal", value))
            record.ordinal = static_cast<std::uint32_t>(value);
        if (ExtractJsonUInt64(object, "flags", value))
            record.flags = static_cast<std::uint32_t>(value);
        record.source = AegisUniversalSdkSource_ReloadedSdk;

        if (!record.moduleName.empty() && !record.exportName.empty())
            loaded.push_back(std::move(record));
        pos = json.find('{', end + 1);
    }

    std::lock_guard lock(g_sdkMutex);
    g_loadedSdkExports = std::move(loaded);
    return 1;
}

AEGIS_UNIVERSAL_API void AegisUniversal_ClearLoadedSdk()
{
    std::lock_guard lock(g_sdkMutex);
    g_loadedSdkExports.clear();
}

AEGIS_UNIVERSAL_API std::uint32_t AegisUniversal_GetLoadedSdkExportCount()
{
    std::lock_guard lock(g_sdkMutex);
    return static_cast<std::uint32_t>(g_loadedSdkExports.size());
}

AEGIS_UNIVERSAL_API int AegisUniversal_GetLoadedSdkExportInfo(std::uint32_t index, AegisUniversalSdkExportInfo* outInfo)
{
    if (!outInfo)
        return 0;
    std::lock_guard lock(g_sdkMutex);
    if (index >= g_loadedSdkExports.size())
        return 0;
    FillSdkExportInfo(g_loadedSdkExports[index], *outInfo);
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_ResolveExport(const wchar_t* moduleName, const char* exportName, AegisUniversalResolvedSymbol* outSymbol)
{
    if (!outSymbol || !exportName || !exportName[0])
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    AegisUniversalResolvedSymbol live = {};
    if (TryLiveResolve(moduleName, exportName, live))
    {
        *outSymbol = live;
        return 1;
    }

    std::lock_guard lock(g_sdkMutex);
    for (SdkExportRecord record : g_loadedSdkExports)
    {
        if (!EqualsInsensitiveAscii(record.exportName, exportName))
            continue;
        if (moduleName && moduleName[0] && !ContainsInsensitive(record.moduleName, moduleName) && !ContainsInsensitive(record.modulePath, moduleName))
            continue;

        AegisUniversalModuleInfo module = {};
        const bool loaded = TryGetModuleForSdkRecord(record, module);
        if (loaded && record.rva != 0 && record.rva < module.imageSize)
            record.address = module.baseAddress + record.rva;
        record.source |= AegisUniversalSdkSource_ReloadedSdk;
        FillResolvedSymbol(record, loaded, *outSymbol);
        return 1;
    }
    return 0;
}

AEGIS_UNIVERSAL_API int AegisUniversal_ValidateLoadedSdk(AegisUniversalSdkValidationInfo* outInfo)
{
    if (!outInfo)
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    *outInfo = BuildValidationInfo();
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_WriteSdkValidationJson(const wchar_t* jsonPath)
{
    if (!jsonPath || !jsonPath[0])
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    const AegisUniversalSdkValidationInfo info = BuildValidationInfo();
    std::ofstream out(std::filesystem::path(jsonPath), std::ios::binary | std::ios::trunc);
    if (!out)
        return 0;

    out << "{\n";
    out << "  \"format\": \"AegisUniversalSdkValidation\",\n";
    out << "  \"version\": 1,\n";
    out << "  \"engine\": \"" << JsonEscapeWide(AegisUniversal_GetEngineName() ? AegisUniversal_GetEngineName() : L"") << "\",\n";
    out << "  \"sdkLoaded\": " << (info.sdkLoaded ? "true" : "false") << ",\n";
    out << "  \"loadedExportCount\": " << info.loadedExportCount << ",\n";
    out << "  \"liveResolvedCount\": " << info.liveResolvedCount << ",\n";
    out << "  \"sdkOnlyCount\": " << info.sdkOnlyCount << ",\n";
    out << "  \"staleRvaCount\": " << info.staleRvaCount << ",\n";
    out << "  \"selfReferenceCount\": " << info.selfReferenceCount << ",\n";
    out << "  \"moduleMatchedCount\": " << info.moduleMatchedCount << ",\n";
    out << "  \"details\": \"" << JsonEscapeWide(info.details) << "\"\n";
    out << "}\n";
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_ResolveRva(const wchar_t* moduleName, std::uintptr_t rva, AegisUniversalResolvedSymbol* outSymbol)
{
    if (!outSymbol || !moduleName || !moduleName[0] || rva == 0)
        return 0;
    if (!IsRuntimeInitialized())
        return 0;

    for (const AegisUniversalModuleInfo& module : CurrentModules())
    {
        if (!MatchModule(module, moduleName))
            continue;
        SdkExportRecord record;
        record.moduleName = module.name;
        record.modulePath = module.path;
        record.address = rva < module.imageSize ? module.baseAddress + rva : 0;
        record.rva = rva;
        record.flags = module.flags;
        record.source = AegisUniversalSdkSource_LiveExport;
        FillResolvedSymbol(record, record.address != 0, *outSymbol);
        return 1;
    }

    std::lock_guard lock(g_sdkMutex);
    for (SdkExportRecord record : g_loadedSdkExports)
    {
        if (record.rva != rva)
            continue;
        if (!ContainsInsensitive(record.moduleName, moduleName) && !ContainsInsensitive(record.modulePath, moduleName))
            continue;

        AegisUniversalModuleInfo module = {};
        const bool loaded = TryGetModuleForSdkRecord(record, module);
        if (loaded && record.rva < module.imageSize)
            record.address = module.baseAddress + record.rva;
        record.source |= AegisUniversalSdkSource_ReloadedSdk;
        FillResolvedSymbol(record, loaded, *outSymbol);
        return 1;
    }
    return 0;
}
