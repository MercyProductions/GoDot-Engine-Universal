#include "GodotResolverTrace.h"
#include "AegisUniversalRuntime.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    struct ResolverRecord
    {
        std::string symbolName;
        std::string sourceLabel;
        std::wstring moduleName;
        std::uintptr_t address = 0;
        std::uintptr_t rva = 0;
    };

    std::mutex g_mutex;
    std::vector<ResolverRecord> g_records;

    std::string JsonEscape(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (char ch : value)
        {
            switch (ch)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    std::string JsonEscapeWide(const std::wstring& value)
    {
        std::string escaped;
        for (wchar_t ch : value)
        {
            if (ch >= 0 && ch <= 0x7f)
                escaped.push_back(static_cast<char>(ch));
            else
                escaped.push_back('?');
        }
        return JsonEscape(escaped);
    }

    void LookupModuleForAddress(std::uintptr_t address, std::wstring& outModule, std::uintptr_t& outRva)
    {
        outModule.clear();
        outRva = 0;
        if (!address)
            return;

        if (!AegisUniversal_IsInitialized())
            AegisUniversal_Initialize();

        const std::uint32_t moduleCount = AegisUniversal_GetModuleCount();
        for (std::uint32_t index = 0; index < moduleCount; ++index)
        {
            AegisUniversalModuleInfo module = {};
            if (!AegisUniversal_GetModuleInfo(index, &module))
                continue;

            const std::uintptr_t base = module.baseAddress;
            const std::uintptr_t end = base + module.imageSize;
            if (address < base || address >= end)
                continue;

            outModule = module.name;
            outRva = address - base;
            return;
        }
    }
}

void GodotResolver_Clear()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_records.clear();
}

void GodotResolver_Record(const char* symbolName, const char* sourceLabel, void* address)
{
    if (!symbolName || !symbolName[0])
        return;

    ResolverRecord record;
    record.symbolName = symbolName;
    record.sourceLabel = sourceLabel ? sourceLabel : "unknown";
    record.address = reinterpret_cast<std::uintptr_t>(address);
    LookupModuleForAddress(record.address, record.moduleName, record.rva);

    std::lock_guard<std::mutex> lock(g_mutex);
    for (ResolverRecord& existing : g_records)
    {
        if (existing.symbolName == record.symbolName)
        {
            existing = record;
            return;
        }
    }
    g_records.push_back(std::move(record));
}

int GodotResolver_WriteReport(const wchar_t* path)
{
    if (!path || !path[0])
        return 0;

    std::vector<ResolverRecord> records;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        records = g_records;
    }

    std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path())
        std::filesystem::create_directories(outputPath.parent_path());

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file)
        return 0;

    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"format\": \"AegisGodotResolver\",\n");
    std::fprintf(file, "  \"version\": 1,\n");
    std::fprintf(file, "  \"resolutionPolicy\": \"live-scan-only\",\n");
    std::fprintf(file, "  \"staticOffsetsBundled\": false,\n");
    std::fprintf(file, "  \"symbolCount\": %zu,\n", records.size());
    std::fprintf(file, "  \"symbols\": [\n");

    for (std::size_t index = 0; index < records.size(); ++index)
    {
        const ResolverRecord& record = records[index];
        std::fprintf(file,
            "    { \"name\": \"%s\", \"source\": \"%s\", \"module\": \"%s\", \"address\": %llu, \"addressHex\": \"0x%llX\", \"rva\": %llu, \"rvaHex\": \"0x%llX\" }",
            JsonEscape(record.symbolName).c_str(),
            JsonEscape(record.sourceLabel).c_str(),
            JsonEscapeWide(record.moduleName).c_str(),
            static_cast<unsigned long long>(record.address),
            static_cast<unsigned long long>(record.address),
            static_cast<unsigned long long>(record.rva),
            static_cast<unsigned long long>(record.rva));
        if (index + 1 < records.size())
            std::fprintf(file, ",");
        std::fprintf(file, "\n");
    }

    std::fprintf(file, "  ]\n");
    std::fprintf(file, "}\n");
    std::fclose(file);
    return 1;
}
