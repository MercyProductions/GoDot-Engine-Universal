#pragma once

#include "AegisUniversalRuntime.h"

inline constexpr AegisUniversalSignature kAegisUniversalSignatures[] = {
    { L"Chambers The Outlaw.exe", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Dumpbin target: monolithic x64 Godot executable with embedded pck section" },
    { L"Chambers The Outlaw.console.exe", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Dumpbin target: Godot console companion executable" },
    { L"kerker.exe", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Dumpbin target: monolithic x64 Godot executable with embedded pck section" },
    { L"SpaceIdle.exe", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Dumpbin target: godot.windows.template_release.x86_64.exe export section and embedded pck section" },
    { L"godot", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Process hint" },
    { L"godot.windows", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Process hint" },
    { L"godot_console", nullptr, nullptr, AegisUniversalSignature_Process | AegisUniversalSignature_Core, "Process hint" },
    { nullptr, L"godot-jolt_windows-x64_editor.dll", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core | AegisUniversalSignature_Physics, "Godot Jolt extension module" },
    { nullptr, L"godot", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core, "Module hint" },
    { nullptr, L"godot.windows", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core, "Module hint" },
    { nullptr, L"libgodot", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core, "Module hint" },
    { nullptr, L"godot-cpp", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core, "Module hint" },
    { nullptr, L"godotsteam", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core, "Module hint" },
    { nullptr, L"godotsteam.debug.x86_64.dll", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core | AegisUniversalSignature_Scripting, "GodotSteam extension module" },
    { nullptr, L"libterrain.windows.debug.x86_64.dll", nullptr, AegisUniversalSignature_Module | AegisUniversalSignature_Core, "Godot Terrain3D extension module" },
    { nullptr, nullptr, "godot_jolt_main", AegisUniversalSignature_Export | AegisUniversalSignature_Core | AegisUniversalSignature_Physics, "Godot Jolt extension entry point" },
    { nullptr, nullptr, "godotsteam_init", AegisUniversalSignature_Export | AegisUniversalSignature_Core | AegisUniversalSignature_Scripting, "GodotSteam extension entry point" },
    { nullptr, nullptr, "godot_gdnative_init", AegisUniversalSignature_Export | AegisUniversalSignature_Core, "Export hint" },
    { nullptr, nullptr, "godot_gdnative_terminate", AegisUniversalSignature_Export | AegisUniversalSignature_Core, "Export hint" },
    { nullptr, nullptr, "godot_nativescript_init", AegisUniversalSignature_Export | AegisUniversalSignature_Core, "Export hint" },
    { nullptr, nullptr, "gdextension_library_init", AegisUniversalSignature_Export | AegisUniversalSignature_Core, "Export hint" },
    { nullptr, nullptr, "terrain_3d_init", AegisUniversalSignature_Export | AegisUniversalSignature_Core, "Terrain3D extension entry point" },
};

inline constexpr AegisUniversalProfile kAegisUniversalProfile = {
    L"Godot",
    L"Godot",
    L"Godot_Universal_Report.txt",
    L"Godot_Universal_Trace.txt",
    kAegisUniversalSignatures,
    sizeof(kAegisUniversalSignatures) / sizeof(kAegisUniversalSignatures[0])
};

