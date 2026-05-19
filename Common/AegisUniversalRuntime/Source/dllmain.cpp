#include "AegisUniversalRuntime.h"

#include <Windows.h>

#include <sstream>
#include <string>

namespace
{
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

    std::wstring ProfileSiblingPath(const wchar_t* suffix)
    {
        std::wstring name = AegisUniversal_GetReportFileName();
        const std::wstring token = L"_Report.txt";
        const std::size_t pos = name.rfind(token);
        if (pos != std::wstring::npos && pos + token.size() == name.size())
            name = name.substr(0, pos) + suffix;
        else
            name += suffix;
        return TempFilePath(name.c_str());
    }

    void WriteStatusLine(const std::wstring& value)
    {
        ::OutputDebugStringW(value.c_str());
        ::OutputDebugStringW(L"\n");

        HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (!output || output == INVALID_HANDLE_VALUE)
            return;

        DWORD written = 0;
        ::WriteConsoleW(output, value.c_str(), static_cast<DWORD>(value.size()), &written, nullptr);
        ::WriteConsoleW(output, L"\r\n", 2, &written, nullptr);
    }

    DWORD WINAPI BootstrapThread(void*)
    {
        if (!::GetConsoleWindow())
            ::AllocConsole();

        ::SetConsoleTitleW(L"Aegis Universal Engine Status");
        WriteStatusLine(WidenAscii(AegisUniversal_GetBrandAsciiArt()));
        WriteStatusLine(L"[AegisUniversal] Initializing " + std::wstring(AegisUniversal_GetEngineName()));

        const bool detected = AegisUniversal_Initialize() != 0;
        AegisUniversalRuntimeInfo info = {};
        AegisUniversal_GetRuntimeInfo(&info);

        std::wstringstream line;
        line << L"[AegisUniversal] Engine detected: " << (detected ? L"yes" : L"no")
             << L" | modules " << info.moduleCount
             << L" | matched modules " << info.matchedModuleCount
             << L" | matched exports " << info.matchedExportCount
             << L" | flags 0x" << std::hex << info.flags;
        WriteStatusLine(line.str());

        if (info.detectedModule[0])
            WriteStatusLine(L"[AegisUniversal] Detected module: " + std::wstring(info.detectedModule));

        const std::uint32_t exportCount = AegisUniversal_GetMatchedExportCount();
        const std::uint32_t maxConsoleExports = exportCount < 64 ? exportCount : 64;
        for (std::uint32_t index = 0; index < maxConsoleExports; ++index)
        {
            AegisUniversalExportInfo exportInfo = {};
            if (!AegisUniversal_GetMatchedExportInfo(index, &exportInfo))
                continue;

            std::wstringstream exportLine;
            exportLine << L"[AegisUniversal] Export " << (index + 1) << L"/" << exportCount
                       << L": " << exportInfo.moduleName << L"!" << WidenAscii(exportInfo.exportName)
                       << L" = 0x" << std::hex << exportInfo.address;
            WriteStatusLine(exportLine.str());
        }
        if (exportCount > maxConsoleExports)
            WriteStatusLine(L"[AegisUniversal] Export console list truncated; full list is in the CSV.");

        const std::wstring reportPath = TempFilePath(AegisUniversal_GetReportFileName());
        AegisUniversal_WriteRuntimeReport(reportPath.c_str());
        WriteStatusLine(L"[AegisUniversal] Report: " + reportPath);

        const std::wstring modulesCsvPath = ProfileSiblingPath(L"_Modules.csv");
        AegisUniversal_WriteModuleCsv(modulesCsvPath.c_str());
        WriteStatusLine(L"[AegisUniversal] Modules CSV: " + modulesCsvPath);

        const std::wstring exportsCsvPath = ProfileSiblingPath(L"_MatchedExports.csv");
        AegisUniversal_WriteMatchedExportsCsv(exportsCsvPath.c_str());
        WriteStatusLine(L"[AegisUniversal] Matched exports CSV: " + exportsCsvPath);

        WriteStatusLine(L"[AegisUniversal] Trace: " + TempFilePath(AegisUniversal_GetTraceFileName()));
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(module);
        if (HANDLE thread = ::CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr))
            ::CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        AegisUniversal_Shutdown();
    }

    return TRUE;
}
