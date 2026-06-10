# Godot Dumpbin Findings

Generated from:

```text
C:\Program Files (x86)\Steam\steamapps\common\Unnamed Space Idle
C:\Program Files (x86)\Steam\steamapps\common\Chambers The Outlaw
C:\Program Files (x86)\Steam\steamapps\common\Kerker
C:\Program Files (x86)\Steam\steamapps\common\Wheelchair Wizards
```

## Unnamed Space Idle

```text
SpaceIdle.exe
  Export section: godot.windows.template_release.x86_64.exe
  Exports:
    AmdPowerXpressRequestHighPerformance
    NoHotPatch
    NvOptimusEnablement
  Sections:
    pck
  Imports:
    dxgi.dll
    steam_api64.dll
```

Notes:

```text
The export section name identifies the Godot Windows template. The `pck` section confirms an embedded Godot pack.
```

## Chambers The Outlaw

```text
Chambers The Outlaw.exe
  Machine: x64
  Sections:
    pck

godot-jolt_windows-x64_editor.dll
  godot_jolt_main

godotsteam.debug.x86_64.dll
  Export section: libgodotsteam.windows.template_debug.x86_64.dll
  godotsteam_init

libterrain.windows.debug.x86_64.dll
  terrain_3d_init
```

Notes:

```text
The main executable is a monolithic Godot export with an embedded pack. Extension DLLs provide strong Godot module/export evidence.
```

## Kerker

```text
kerker.exe
  Machine: x64
  Sections:
    pck
```

Notes:

```text
The executable has a Godot-style embedded pack section. Its appended pack data makes full export dumping unreliable, so the profile uses process plus section evidence for this target.
```

## Wheelchair Wizards

```text
Wizards.exe
  Machine: x64
  Export section: godot.windows.template_release.x86_64.exe
  Exports:
    AmdPowerXpressRequestHighPerformance
    NoHotPatch
    NvOptimusEnablement
  Sections:
    .text
    .rdata
    .pdata
    pck

steam_api64.dll
  Exports:
    SteamAPI_*
```

Notes:

```text
The main executable is a monolithic Godot export with an embedded pack section.
No game-local Godot extension DLL exports were present; runtime resolution must come from the main executable's GDExtension registration code and live memory scan.
Crash RVAs from the resolver trace map to x64 unwind entries:
  0x2CAE7B0 is a function start.
  0x2CAE7B6 is inside 0x2CAE7B0-0x2CAE861 and must not be called.
  0x2CAEE66 is inside 0x2CAEE60-0x2CAEFEF and must not be called.
```
