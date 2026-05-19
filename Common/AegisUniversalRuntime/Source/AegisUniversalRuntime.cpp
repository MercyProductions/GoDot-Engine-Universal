#include "AegisUniversalRuntimeInternal.h"
#include "AegisEngineProfile.generated.h"

#include <TlHelp32.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    const char* kAegisAsciiArt =
        "    ___    ______ ______ ____ _____\n"
        "   /   |  / ____// ____//  _// ___/\n"
        "  / /| | / __/  / / __  / /  \\__ \\\n"
        " / ___ |/ /___ / /_/ /_/ /  ___/ /\n"
        "/_/  |_/_____/ \\____//___/ /____/\n";

    struct ModuleRecord
    {
        std::wstring name;
        std::wstring path;
        HMODULE handle = nullptr;
        std::uintptr_t baseAddress = 0;
        std::uint32_t imageSize = 0;
        std::uint32_t flags = AegisUniversalSignature_None;
    };

    struct ExportRecord
    {
        std::string exportName;
        std::wstring moduleName;
        std::wstring modulePath;
        std::uintptr_t address = 0;
        std::uint32_t flags = AegisUniversalSignature_None;
    };

    std::mutex g_mutex;
    bool g_initialized = false;
    std::wstring g_processName;
    std::wstring g_processPath;
    std::wstring g_detectedModule;
    std::vector<ModuleRecord> g_modules;
    std::vector<ExportRecord> g_exports;
    std::uint32_t g_runtimeFlags = AegisUniversalRuntime_None;

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
    }

    bool ContainsInsensitive(const std::wstring& haystack, const wchar_t* needle)
    {
        return needle && needle[0] && ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
    }

    std::wstring FileNameFromPath(const std::wstring& path)
    {
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

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

    std::wstring WidenAscii(const char* value)
    {
        std::wstring result;
        if (!value)
            return result;

        while (*value)
        {
            result.push_back(static_cast<unsigned char>(*value));
            ++value;
        }

        return result;
    }

    std::wstring CsvEscape(const std::wstring& value)
    {
        const bool needsQuotes = value.find_first_of(L",\"\r\n") != std::wstring::npos;
        if (!needsQuotes)
            return value;

        std::wstring escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back(L'"');
        for (wchar_t ch : value)
        {
            if (ch == L'"')
                escaped.push_back(L'"');
            escaped.push_back(ch);
        }
        escaped.push_back(L'"');
        return escaped;
    }

    std::wstring CsvEscapeAscii(const std::string& value)
    {
        return CsvEscape(WidenAscii(value.c_str()));
    }

    std::wstring CurrentProcessPath()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        return length == 0 ? std::wstring() : std::wstring(path, length);
    }

    HMODULE CurrentRuntimeModule()
    {
        HMODULE module = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CurrentRuntimeModule),
            &module);
        return module;
    }

    std::wstring CurrentRuntimeModulePath(HMODULE module)
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = ::GetModuleFileNameW(module, path, MAX_PATH);
        return length == 0 ? std::wstring() : std::wstring(path, length);
    }

    bool SamePathInsensitive(const std::wstring& left, const std::wstring& right)
    {
        return !left.empty() && !right.empty() && ToLower(left) == ToLower(right);
    }

    std::wstring TempFilePath(const wchar_t* fileName)
    {
        wchar_t tempPath[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, tempPath) == 0)
            return fileName ? std::wstring(fileName) : std::wstring();

        std::wstring path = tempPath;
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');
        if (fileName)
            path += fileName;
        return path;
    }

    void AppendTrace(const std::wstring& message)
    {
        std::wofstream trace(TempFilePath(AegisUniversal_GetProfile().traceFileName), std::ios::app);
        if (!trace)
            return;

        SYSTEMTIME time = {};
        ::GetLocalTime(&time);
        trace << L"[" << time.wYear << L"-" << time.wMonth << L"-" << time.wDay
              << L" " << time.wHour << L":" << time.wMinute << L":" << time.wSecond
              << L"] " << message << L"\n";
    }

    std::vector<ModuleRecord> EnumerateModules()
    {
        std::vector<ModuleRecord> modules;
        const HMODULE runtimeModule = CurrentRuntimeModule();
        const std::wstring runtimeModulePath = CurrentRuntimeModulePath(runtimeModule);
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ::GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            return modules;

        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (::Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (entry.hModule == runtimeModule || SamePathInsensitive(entry.szExePath, runtimeModulePath))
                {
                    entry.dwSize = sizeof(entry);
                    continue;
                }

                ModuleRecord record;
                record.name = entry.szModule;
                record.path = entry.szExePath;
                record.handle = entry.hModule;
                record.baseAddress = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                record.imageSize = entry.modBaseSize;
                modules.push_back(std::move(record));
                entry.dwSize = sizeof(entry);
            } while (::Module32NextW(snapshot, &entry));
        }

        ::CloseHandle(snapshot);
        return modules;
    }

    bool ProcessMatchesSignature(const AegisUniversalSignature& signature)
    {
        return signature.processHint && signature.processHint[0] &&
            (ContainsInsensitive(g_processName, signature.processHint) ||
             ContainsInsensitive(g_processPath, signature.processHint));
    }

    bool ModuleMatchesSignature(const ModuleRecord& module, const AegisUniversalSignature& signature)
    {
        return signature.moduleHint && signature.moduleHint[0] &&
            (ContainsInsensitive(module.name, signature.moduleHint) ||
             ContainsInsensitive(module.path, signature.moduleHint));
    }

    void RebuildLocked()
    {
        const AegisUniversalProfile& profile = AegisUniversal_GetProfile();
        g_processPath = CurrentProcessPath();
        g_processName = FileNameFromPath(g_processPath);
        g_modules = EnumerateModules();
        g_exports.clear();
        g_detectedModule.clear();
        g_runtimeFlags = AegisUniversalRuntime_None;

        for (std::size_t index = 0; index < profile.signatureCount; ++index)
        {
            if (ProcessMatchesSignature(profile.signatures[index]))
                g_runtimeFlags |= AegisUniversalRuntime_ProcessHintMatched;
        }

        for (ModuleRecord& module : g_modules)
        {
            for (std::size_t index = 0; index < profile.signatureCount; ++index)
            {
                const AegisUniversalSignature& signature = profile.signatures[index];
                if (ModuleMatchesSignature(module, signature))
                {
                    module.flags |= signature.flags | AegisUniversalSignature_Module;
                    g_runtimeFlags |= AegisUniversalRuntime_ModuleHintMatched;
                    if (g_detectedModule.empty())
                        g_detectedModule = module.name;
                }

                if (signature.exportName && signature.exportName[0] && module.handle)
                {
                    if (FARPROC address = ::GetProcAddress(module.handle, signature.exportName))
                    {
                        ExportRecord record;
                        record.exportName = signature.exportName;
                        record.moduleName = module.name;
                        record.modulePath = module.path;
                        record.address = reinterpret_cast<std::uintptr_t>(address);
                        record.flags = signature.flags | AegisUniversalSignature_Export;
                        g_exports.push_back(std::move(record));
                        module.flags |= AegisUniversalSignature_Export;
                        g_runtimeFlags |= AegisUniversalRuntime_ExportHintMatched;
                        if (g_detectedModule.empty())
                            g_detectedModule = module.name;
                    }
                }
            }
        }

        if ((g_runtimeFlags & (AegisUniversalRuntime_ProcessHintMatched | AegisUniversalRuntime_ModuleHintMatched | AegisUniversalRuntime_ExportHintMatched)) != 0)
            g_runtimeFlags |= AegisUniversalRuntime_EngineDetected;

        g_initialized = true;
    }
}

const AegisUniversalProfile& AegisUniversal_GetProfile()
{
    return kAegisUniversalProfile;
}

AEGIS_UNIVERSAL_API int AegisUniversal_Initialize()
{
    std::lock_guard lock(g_mutex);
    RebuildLocked();
    AppendTrace(L"[AegisUniversal] Initialized " + std::wstring(AegisUniversal_GetProfile().engineName));
    return (g_runtimeFlags & AegisUniversalRuntime_EngineDetected) != 0 ? 1 : 0;
}

AEGIS_UNIVERSAL_API int AegisUniversal_Refresh()
{
    return AegisUniversal_Initialize();
}

AEGIS_UNIVERSAL_API void AegisUniversal_Shutdown()
{
    std::lock_guard lock(g_mutex);
    g_modules.clear();
    g_exports.clear();
    g_initialized = false;
    g_runtimeFlags = AegisUniversalRuntime_None;
}

AEGIS_UNIVERSAL_API int AegisUniversal_IsInitialized()
{
    std::lock_guard lock(g_mutex);
    return g_initialized ? 1 : 0;
}

AEGIS_UNIVERSAL_API int AegisUniversal_IsEngineDetected()
{
    std::lock_guard lock(g_mutex);
    return (g_runtimeFlags & AegisUniversalRuntime_EngineDetected) != 0 ? 1 : 0;
}

AEGIS_UNIVERSAL_API int AegisUniversal_GetRuntimeInfo(AegisUniversalRuntimeInfo* outInfo)
{
    if (!outInfo)
        return 0;

    std::lock_guard lock(g_mutex);
    *outInfo = {};
    CopyWide(outInfo->engineName, AegisUniversal_GetProfile().engineName ? std::wstring(AegisUniversal_GetProfile().engineName) : std::wstring());
    CopyWide(outInfo->processName, g_processName);
    CopyWide(outInfo->processPath, g_processPath);
    CopyWide(outInfo->detectedModule, g_detectedModule);
    outInfo->moduleCount = static_cast<std::uint32_t>(g_modules.size());
    outInfo->matchedExportCount = static_cast<std::uint32_t>(g_exports.size());
    outInfo->flags = g_runtimeFlags;
    for (const ModuleRecord& module : g_modules)
    {
        if (module.flags != AegisUniversalSignature_None)
            ++outInfo->matchedModuleCount;
    }
    return 1;
}

AEGIS_UNIVERSAL_API std::uint32_t AegisUniversal_GetModuleCount()
{
    std::lock_guard lock(g_mutex);
    return static_cast<std::uint32_t>(g_modules.size());
}

AEGIS_UNIVERSAL_API int AegisUniversal_GetModuleInfo(std::uint32_t index, AegisUniversalModuleInfo* outInfo)
{
    if (!outInfo)
        return 0;

    std::lock_guard lock(g_mutex);
    if (index >= g_modules.size())
        return 0;

    const ModuleRecord& module = g_modules[index];
    *outInfo = {};
    CopyWide(outInfo->name, module.name);
    CopyWide(outInfo->path, module.path);
    outInfo->baseAddress = module.baseAddress;
    outInfo->imageSize = module.imageSize;
    outInfo->flags = module.flags;
    return 1;
}

AEGIS_UNIVERSAL_API std::uint32_t AegisUniversal_GetMatchedExportCount()
{
    std::lock_guard lock(g_mutex);
    return static_cast<std::uint32_t>(g_exports.size());
}

AEGIS_UNIVERSAL_API int AegisUniversal_GetMatchedExportInfo(std::uint32_t index, AegisUniversalExportInfo* outInfo)
{
    if (!outInfo)
        return 0;

    std::lock_guard lock(g_mutex);
    if (index >= g_exports.size())
        return 0;

    const ExportRecord& record = g_exports[index];
    *outInfo = {};
    CopyAnsi(outInfo->exportName, record.exportName);
    CopyWide(outInfo->moduleName, record.moduleName);
    CopyWide(outInfo->modulePath, record.modulePath);
    outInfo->address = record.address;
    outInfo->flags = record.flags;
    return 1;
}

AEGIS_UNIVERSAL_API void* AegisUniversal_GetExport(const wchar_t* moduleName, const char* exportName)
{
    if (!exportName || !exportName[0])
        return nullptr;

    std::lock_guard lock(g_mutex);
    for (const ModuleRecord& module : g_modules)
    {
        if (moduleName && moduleName[0] &&
            !ContainsInsensitive(module.name, moduleName) &&
            !ContainsInsensitive(module.path, moduleName))
        {
            continue;
        }

        if (module.handle)
        {
            if (FARPROC address = ::GetProcAddress(module.handle, exportName))
                return reinterpret_cast<void*>(address);
        }
    }
    return nullptr;
}

AEGIS_UNIVERSAL_API int AegisUniversal_WriteRuntimeReport(const wchar_t* reportPath)
{
    std::lock_guard lock(g_mutex);
    const std::wstring path = reportPath && reportPath[0] ? reportPath : TempFilePath(AegisUniversal_GetProfile().reportFileName);
    std::wofstream report(path, std::ios::trunc);
    if (!report)
        return 0;

    AegisUniversalRuntimeInfo info = {};
    CopyWide(info.engineName, AegisUniversal_GetProfile().engineName ? std::wstring(AegisUniversal_GetProfile().engineName) : std::wstring());
    CopyWide(info.processName, g_processName);
    CopyWide(info.processPath, g_processPath);
    CopyWide(info.detectedModule, g_detectedModule);
    info.moduleCount = static_cast<std::uint32_t>(g_modules.size());
    info.matchedExportCount = static_cast<std::uint32_t>(g_exports.size());
    info.flags = g_runtimeFlags;
    for (const ModuleRecord& module : g_modules)
    {
        if (module.flags != AegisUniversalSignature_None)
            ++info.matchedModuleCount;
    }

    report << WidenAscii(kAegisAsciiArt) << L"\n";
    report << L"Aegis Universal Engine Report\n";
    report << L"Engine: " << info.engineName << L"\n";
    report << L"Process: " << info.processName << L"\n";
    report << L"Path: " << info.processPath << L"\n";
    report << L"Detected module: " << (g_detectedModule.empty() ? L"none" : g_detectedModule) << L"\n";
    report << L"Modules: " << info.moduleCount << L"\n";
    report << L"Matched modules: " << info.matchedModuleCount << L"\n";
    report << L"Matched exports: " << info.matchedExportCount << L"\n";
    report << L"Runtime flags: 0x" << std::hex << info.flags << std::dec << L"\n\n";

    report << L"[Matched Exports]\n";
    for (const ExportRecord& record : g_exports)
    {
        report << L"  " << record.moduleName << L"!" << WidenAscii(record.exportName.c_str())
               << L" = 0x" << std::hex << record.address << std::dec << L"\n";
    }

    report << L"\n[Modules]\n";
    for (const ModuleRecord& module : g_modules)
    {
        report << L"  " << module.name << L" | base 0x" << std::hex << module.baseAddress << std::dec
               << L" | size " << module.imageSize << L" | flags 0x" << std::hex << module.flags << std::dec << L"\n";
    }
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_WriteModuleCsv(const wchar_t* csvPath)
{
    if (!csvPath || !csvPath[0])
        return 0;

    std::lock_guard lock(g_mutex);
    std::wofstream csv(csvPath, std::ios::trunc);
    if (!csv)
        return 0;

    csv << L"Name,Path,BaseAddress,ImageSize,Flags\n";
    for (const ModuleRecord& module : g_modules)
    {
        csv << CsvEscape(module.name) << L","
            << CsvEscape(module.path) << L",0x"
            << std::hex << module.baseAddress << std::dec << L","
            << module.imageSize << L",0x"
            << std::hex << module.flags << std::dec << L"\n";
    }
    return 1;
}

AEGIS_UNIVERSAL_API int AegisUniversal_WriteMatchedExportsCsv(const wchar_t* csvPath)
{
    if (!csvPath || !csvPath[0])
        return 0;

    std::lock_guard lock(g_mutex);
    std::wofstream csv(csvPath, std::ios::trunc);
    if (!csv)
        return 0;

    csv << L"Module,Path,ExportName,Address,Flags\n";
    for (const ExportRecord& record : g_exports)
    {
        csv << CsvEscape(record.moduleName) << L","
            << CsvEscape(record.modulePath) << L","
            << CsvEscapeAscii(record.exportName) << L",0x"
            << std::hex << record.address << std::dec << L",0x"
            << std::hex << record.flags << std::dec << L"\n";
    }
    return 1;
}

AEGIS_UNIVERSAL_API const char* AegisUniversal_GetBrandAsciiArt()
{
    return kAegisAsciiArt;
}

AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetEngineName()
{
    return AegisUniversal_GetProfile().engineName;
}

AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetReportFileName()
{
    return AegisUniversal_GetProfile().reportFileName;
}

AEGIS_UNIVERSAL_API const wchar_t* AegisUniversal_GetTraceFileName()
{
    return AegisUniversal_GetProfile().traceFileName;
}
