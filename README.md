# Godot Universal

Aegis universal runtime project for **Godot**. This follows the same diagnostic-SDK concept as the CryEngine and Id Tech universals: identify runtime capabilities, expose provider-based object/camera/viewport submission, validate world-to-screen projection, and write reports plus replayable JSON snapshots from debug data.

It does not add game-specific memory scanners, node offset scraping, anti-cheat bypasses, stealth behavior, or hidden injection behavior.

## Dumpbin Findings

The profile was refreshed from these local targets:

```text
C:\Program Files (x86)\Steam\steamapps\common\Unnamed Space Idle
C:\Program Files (x86)\Steam\steamapps\common\Chambers The Outlaw
C:\Program Files (x86)\Steam\steamapps\common\Kerker
```

Confirmed signals:

```text
Unnamed Space Idle\SpaceIdle.exe
  Export section name: godot.windows.template_release.x86_64.exe
  Exports: AmdPowerXpressRequestHighPerformance, NoHotPatch, NvOptimusEnablement
  Sections: pck

Kerker\kerker.exe
  Machine: x64
  Sections: pck

Chambers The Outlaw\Chambers The Outlaw.exe
  Machine: x64
  Sections: pck

Chambers The Outlaw\godot-jolt_windows-x64_editor.dll
  Export: godot_jolt_main

Chambers The Outlaw\godotsteam.debug.x86_64.dll
  Export section name: libgodotsteam.windows.template_debug.x86_64.dll
  Export: godotsteam_init

Chambers The Outlaw\libterrain.windows.debug.x86_64.dll
  Export: terrain_3d_init
```

## Runtime Flow

1. Load the DLL into a process you are authorized to inspect.
2. The bootstrap thread initializes the shared Aegis runtime.
3. Loaded modules are enumerated and matched against Godot process/module/export hints.
4. The Godot adapter can receive object snapshots, viewport size, and view-projection matrices from a debug/test provider.
5. The adapter validates matrix/viewport health, checks for embedded `pck` sections, projects submitted world points to screen coordinates, records JSON snapshots, and reports backend/capability status.

## Adapter API

The Godot-specific API is declared in:

```text
Include\AegisGodotUniversal.h
```

Core provider functions:

```cpp
AegisGodot_RegisterObjectProvider(...);
AegisGodot_RegisterViewProjectionProvider(...);
AegisGodot_RegisterViewportProvider(...);
AegisGodot_UpdateProviders();
```

Direct submission functions:

```cpp
AegisGodot_SubmitObjectSnapshots(...);
AegisGodot_SubmitViewProjection(...);
AegisGodot_SubmitViewport(...);
```

Diagnostics:

```cpp
AegisGodot_GetCapabilityInfo(...);
AegisGodot_ProjectWorldToScreen(...);
AegisGodot_WriteSnapshotJson(...);
AegisGodot_LoadSnapshotJson(...);
AegisGodot_PrintCurrentObjects();
```

## Object Snapshot

Each submitted object snapshot contains:

```text
id
name
className
path
origin
boundsMin / boundsMax
group
visible
flags
```

This lets a Godot debug adapter submit `Node2D`, `Node3D`, `CharacterBody3D`, `Area3D`, NPCs, pickups, markers, or other project objects without hardcoding title-specific offsets into the universal DLL.

## Renderer Status

`AegisGodot_GetCapabilityInfo` reports the detected backend as one of:

```text
Vulkan
Direct3D12
Direct3D11
OpenGL
Unknown
```

The backend is inferred from loaded modules such as `vulkan-1.dll`, `d3d12.dll`, `d3d11.dll`, `dxgi.dll`, and `opengl32.dll`.

## Internal ImGui Overlay

The DLL now owns an in-process ImGui diagnostics menu. On load it starts an internal render bridge and attempts backend setup in this order:

```text
Direct3D11 Present
Direct3D9 EndScene
OpenGL SwapBuffers
```

Press `F4` to show or hide the menu. The menu includes runtime status, renderer status, adapter capability checks, object/provider timing, and overlay toggles for boxes, corner boxes, filled boxes, lines, and labels. Vulkan and D3D12 are still detected and reported clearly, but this build does not render ImGui through those backends yet.

## Build

This repository vendors the small shared Aegis runtime under:

```text
Common\AegisUniversalRuntime
```

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" ".\AegisGodotUniversal.sln" /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Output:

```text
build\x64\Release\AegisGodotUniversal.dll
```

## Sample Harness

A local provider harness is included at:

```text
Samples\GodotProviderHarness\GodotProviderHarness.cpp
```

It loads the DLL, registers known test objects, submits a viewport and matrix, prints object names/paths, and writes `GodotProviderHarness.snapshot.json` so W2S and replay can be validated without relying on a specific game.
