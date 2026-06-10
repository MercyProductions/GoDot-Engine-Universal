#include "GodotSceneBackend.h"
#include "AegisUniversalRuntime.h"
#include "GodotResolverTrace.h"

#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

extern bool g_drawAllNode3Ds;

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

    bool IsLikelyActorName(const std::string& className, const std::string& nodeName)
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

    bool IsIgnoredTargetNode(const std::string& className, const std::string& nodeName)
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

    constexpr std::uint32_t kGdnativeCoreApiType = 1;

    using Godot3StringNewUtf8Fn = void (*)(void* r_dest, const char* contents);
    using Godot3StringDestroyFn = void (*)(void* self);
    using Godot3GlobalGetSingletonFn = void* (*)(const void* name);
    using Godot3MethodBindGetFn = void* (*)(const char* className, const char* methodName);
    using Godot3MethodBindPtrcallFn = void (*)(void* methodBind, void* instance, const void** args, void* ret);

    struct Godot3Api
    {
        Godot3StringNewUtf8Fn string_new_utf8 = nullptr;
        Godot3StringDestroyFn string_destroy = nullptr;
        Godot3GlobalGetSingletonFn global_get_singleton = nullptr;
        Godot3MethodBindGetFn method_bind_get = nullptr;
        Godot3MethodBindPtrcallFn method_bind_ptrcall = nullptr;
        void* core_struct = nullptr;
    };

    Godot3Api g_godot3Api;
    bool g_godot3_resolution_done = false;
    bool g_godot3_resolution_ok = false;
    DWORD g_godot3_retry_after_tick = 0;

    bool IsExecutablePointer(const void* ptr, uint8_t* mainStart, uint8_t* mainEnd)
    {
        const auto address = reinterpret_cast<uint8_t*>(const_cast<void*>(ptr));
        return address >= mainStart && address < mainEnd;
    }

    bool TryStringNewUtf8(void* candidate, void* outBuf)
    {
        if (!candidate || !outBuf)
            return false;

        __try
        {
            std::memset(outBuf, 0, 16);
            reinterpret_cast<Godot3StringNewUtf8Fn>(candidate)(outBuf, "Aegis");
            const auto* bytes = static_cast<const std::uint8_t*>(outBuf);
            for (size_t index = 0; index < 16; ++index)
            {
                if (bytes[index] != 0)
                    return true;
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryGlobalGetSingleton(void* candidate, const void* sceneTreeName)
    {
        if (!candidate || !sceneTreeName)
            return false;

        __try
        {
            void* singleton = reinterpret_cast<Godot3GlobalGetSingletonFn>(candidate)(sceneTreeName);
            return singleton != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryMethodBindGet(void* candidate)
    {
        if (!candidate)
            return false;

        __try
        {
            void* bind = reinterpret_cast<Godot3MethodBindGetFn>(candidate)("Node", "get_child_count");
            return bind != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryMethodBindPtrcall(void* candidate, void* methodBind, void* instance)
    {
        if (!candidate || !methodBind || !instance)
            return false;

        __try
        {
            int64_t childCount = -1;
            int8_t includeInternal = 0;
            const void* args[] = { &includeInternal };
            reinterpret_cast<Godot3MethodBindPtrcallFn>(candidate)(methodBind, instance, args, &childCount);
            return childCount >= 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool DiscoverGodot3ApiFromStruct(uint8_t* structBase, uint8_t* mainStart, uint8_t* mainEnd, Godot3Api& outApi)
    {
        if (!structBase)
            return false;

        const auto* header = reinterpret_cast<const std::uint32_t*>(structBase);
        if (header[0] != kGdnativeCoreApiType || header[1] != 3)
            return false;

        void** slots = reinterpret_cast<void**>(structBase + 20);
        constexpr int kMaxSlots = 128;

        alignas(8) char sceneTreeName[16] = {};
        for (int index = 0; index < kMaxSlots; ++index)
        {
            void* candidate = slots[index];
            if (!IsExecutablePointer(candidate, mainStart, mainEnd))
                continue;

            if (!outApi.string_new_utf8 && TryStringNewUtf8(candidate, sceneTreeName))
                outApi.string_new_utf8 = reinterpret_cast<Godot3StringNewUtf8Fn>(candidate);
        }

        if (!outApi.string_new_utf8)
            return false;

        std::memset(sceneTreeName, 0, sizeof(sceneTreeName));
        outApi.string_new_utf8(sceneTreeName, "SceneTree");

        for (int index = 0; index < kMaxSlots; ++index)
        {
            void* candidate = slots[index];
            if (!IsExecutablePointer(candidate, mainStart, mainEnd))
                continue;
            if (candidate == reinterpret_cast<void*>(outApi.string_new_utf8))
                continue;

            if (!outApi.global_get_singleton && TryGlobalGetSingleton(candidate, sceneTreeName))
                outApi.global_get_singleton = reinterpret_cast<Godot3GlobalGetSingletonFn>(candidate);
        }

        if (!outApi.global_get_singleton)
            return false;

        void* sceneTree = outApi.global_get_singleton(sceneTreeName);
        if (!sceneTree)
            return false;

        void* rootBind = nullptr;
        for (int index = 0; index < kMaxSlots; ++index)
        {
            void* candidate = slots[index];
            if (!IsExecutablePointer(candidate, mainStart, mainEnd))
                continue;
            if (!TryMethodBindGet(candidate))
                continue;
            outApi.method_bind_get = reinterpret_cast<Godot3MethodBindGetFn>(candidate);
            rootBind = outApi.method_bind_get("SceneTree", "get_root");
            if (rootBind)
                break;
            outApi.method_bind_get = nullptr;
        }

        if (!outApi.method_bind_get || !rootBind)
            return false;

        void* root = nullptr;
        for (int index = 0; index < kMaxSlots; ++index)
        {
            void* candidate = slots[index];
            if (!IsExecutablePointer(candidate, mainStart, mainEnd))
                continue;
            if (!TryMethodBindPtrcall(candidate, rootBind, sceneTree))
                continue;
            outApi.method_bind_ptrcall = reinterpret_cast<Godot3MethodBindPtrcallFn>(candidate);
            outApi.method_bind_ptrcall(rootBind, sceneTree, nullptr, &root);
            if (root)
                break;
            outApi.method_bind_ptrcall = nullptr;
        }

        if (!outApi.method_bind_ptrcall || !root)
            return false;

        for (int index = 0; index < kMaxSlots; ++index)
        {
            void* candidate = slots[index];
            if (!IsExecutablePointer(candidate, mainStart, mainEnd))
                continue;
            if (candidate == reinterpret_cast<void*>(outApi.string_new_utf8))
                continue;

            alignas(8) char probe[16] = {};
            outApi.string_new_utf8(probe, "x");
            __try
            {
                reinterpret_cast<Godot3StringDestroyFn>(candidate)(probe);
                outApi.string_destroy = reinterpret_cast<Godot3StringDestroyFn>(candidate);
                break;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        outApi.core_struct = structBase;
        return true;
    }

    bool ResolveGodot3Api()
    {
        if (g_godot3_resolution_done)
            return g_godot3_resolution_ok;

        const DWORD now = ::GetTickCount();
        if (g_godot3_retry_after_tick != 0 && now < g_godot3_retry_after_tick)
            return false;

        HMODULE hMain = GetModuleHandleW(nullptr);
        MODULEINFO mainInfo = {};
        GetModuleInformation(GetCurrentProcess(), hMain, &mainInfo, sizeof(mainInfo));
        uint8_t* mainStart = reinterpret_cast<uint8_t*>(mainInfo.lpBaseOfDll);
        uint8_t* mainEnd = mainStart + mainInfo.SizeOfImage;

        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mainStart);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mainStart + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        Godot3Api discovered = {};
        IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections; ++sectionIndex, ++section)
        {
            if (!(section->Characteristics & IMAGE_SCN_MEM_READ))
                continue;

            uint8_t* secStart = mainStart + section->VirtualAddress;
            uint8_t* secEnd = secStart + section->Misc.VirtualSize;
            for (uint8_t* cursor = secStart; cursor < secEnd - 32; cursor += 4)
            {
                const auto* header = reinterpret_cast<const std::uint32_t*>(cursor);
                if (header[0] != kGdnativeCoreApiType || header[1] != 3)
                    continue;

                Godot3Api candidate = {};
                if (!DiscoverGodot3ApiFromStruct(cursor, mainStart, mainEnd, candidate))
                    continue;

                discovered = candidate;
                break;
            }
            if (discovered.global_get_singleton)
                break;
        }

        g_godot3_resolution_done = true;
        if (discovered.global_get_singleton)
        {
            g_godot3Api = discovered;
            g_godot3_resolution_ok = true;
            LogMsg("[AegisGodot3] Resolved GDNative core API at %p (string_new=%p singleton=%p method_bind=%p ptrcall=%p)\n",
                g_godot3Api.core_struct,
                reinterpret_cast<void*>(g_godot3Api.string_new_utf8),
                reinterpret_cast<void*>(g_godot3Api.global_get_singleton),
                reinterpret_cast<void*>(g_godot3Api.method_bind_get),
                reinterpret_cast<void*>(g_godot3Api.method_bind_ptrcall));
            GodotResolver_Record("gdnative_core_struct", "live-struct-scan", g_godot3Api.core_struct);
            GodotResolver_Record("string_new_utf8", "live-struct-scan", reinterpret_cast<void*>(g_godot3Api.string_new_utf8));
            GodotResolver_Record("global_get_singleton", "live-struct-scan", reinterpret_cast<void*>(g_godot3Api.global_get_singleton));
            GodotResolver_Record("method_bind_get", "live-struct-scan", reinterpret_cast<void*>(g_godot3Api.method_bind_get));
            GodotResolver_Record("method_bind_ptrcall", "live-struct-scan", reinterpret_cast<void*>(g_godot3Api.method_bind_ptrcall));
            wchar_t tempPath[MAX_PATH] = {};
            if (::GetTempPathW(MAX_PATH, tempPath) != 0)
            {
                std::wstring report = tempPath;
                if (!report.empty() && report.back() != L'\\')
                    report.push_back(L'\\');
                report += L"Godot_Universal_Resolver.json";
                GodotResolver_WriteReport(report.c_str());
            }
        }
        else
        {
            g_godot3_resolution_ok = false;
            g_godot3_retry_after_tick = now + 30000;
            LogMsg("[AegisGodot3] Failed to resolve GDNative core API\n");
        }

        return g_godot3_resolution_ok;
    }

    struct Godot3AutoScanState
    {
        void* sceneTree = nullptr;
        void* activeCamera = nullptr;
        bool initialized = false;

        void* mb_SceneTree_get_root = nullptr;
        void* mb_Node_get_child_count = nullptr;
        void* mb_Node_get_child = nullptr;
        void* mb_Node_get_class = nullptr;
        void* mb_Node_get_name = nullptr;
        void* mb_Spatial_get_translation = nullptr;
        void* mb_Spatial_is_visible_in_tree = nullptr;
        void* mb_Camera_get_camera_transform = nullptr;
        void* mb_Camera_get_projection = nullptr;
        void* mb_Object_is_class = nullptr;
        void* mb_CanvasItem_get_global_transform_with_canvas = nullptr;
        void* mb_CanvasItem_get_screen_transform = nullptr;
        void* mb_CanvasItem_is_visible_in_tree = nullptr;

        char str_Spatial[16] = {};
        char str_Camera[16] = {};
        char str_KinematicBody[16] = {};
        char str_Node2D[16] = {};
        char str_Camera2D[16] = {};

        bool Init()
        {
            if (initialized)
                return sceneTree != nullptr;

            if (!ResolveGodot3Api())
                return false;

            alignas(8) char sceneTreeName[16] = {};
            g_godot3Api.string_new_utf8(sceneTreeName, "SceneTree");
            sceneTree = g_godot3Api.global_get_singleton(sceneTreeName);
            if (g_godot3Api.string_destroy)
                g_godot3Api.string_destroy(sceneTreeName);

            if (!sceneTree)
                return false;

            mb_SceneTree_get_root = g_godot3Api.method_bind_get("SceneTree", "get_root");
            mb_Node_get_child_count = g_godot3Api.method_bind_get("Node", "get_child_count");
            mb_Node_get_child = g_godot3Api.method_bind_get("Node", "get_child");
            mb_Node_get_class = g_godot3Api.method_bind_get("Object", "get_class");
            mb_Node_get_name = g_godot3Api.method_bind_get("Node", "get_name");
            mb_Spatial_get_translation = g_godot3Api.method_bind_get("Spatial", "get_translation");
            if (!mb_Spatial_get_translation)
                mb_Spatial_get_translation = g_godot3Api.method_bind_get("Spatial", "get_global_transform");
            mb_Spatial_is_visible_in_tree = g_godot3Api.method_bind_get("Spatial", "is_visible_in_tree");
            mb_Camera_get_camera_transform = g_godot3Api.method_bind_get("Camera", "get_camera_transform");
            mb_Camera_get_projection = g_godot3Api.method_bind_get("Camera", "get_projection");
            mb_Object_is_class = g_godot3Api.method_bind_get("Object", "is_class");
            mb_CanvasItem_get_global_transform_with_canvas =
                g_godot3Api.method_bind_get("CanvasItem", "get_global_transform_with_canvas");
            mb_CanvasItem_get_screen_transform =
                g_godot3Api.method_bind_get("CanvasItem", "get_screen_transform");
            mb_CanvasItem_is_visible_in_tree = g_godot3Api.method_bind_get("CanvasItem", "is_visible_in_tree");

            g_godot3Api.string_new_utf8(str_Spatial, "Spatial");
            g_godot3Api.string_new_utf8(str_Camera, "Camera");
            g_godot3Api.string_new_utf8(str_KinematicBody, "KinematicBody");
            g_godot3Api.string_new_utf8(str_Node2D, "Node2D");
            g_godot3Api.string_new_utf8(str_Camera2D, "Camera2D");

            initialized = mb_SceneTree_get_root != nullptr &&
                mb_Node_get_child_count != nullptr &&
                mb_Object_is_class != nullptr;

            LogMsg("[AegisGodot3] Init sceneTree=%p get_root=%p spatial_pos=%p camera=%p\n",
                sceneTree, mb_SceneTree_get_root, mb_Spatial_get_translation, mb_Camera_get_camera_transform);
            return initialized;
        }

        bool IsType(void* object, const void* className)
        {
            if (!mb_Object_is_class || !object)
                return false;

            uint8_t result = 0;
            const void* args[] = { className };
            g_godot3Api.method_bind_ptrcall(mb_Object_is_class, object, args, &result);
            return result != 0;
        }

        std::string ReadGodotString(void* stringBuf)
        {
            if (!stringBuf)
                return "";

            void* strData = *reinterpret_cast<void**>(stringBuf);
            if (!strData)
                return "";

            const auto* lengthPtr = reinterpret_cast<const int32_t*>(static_cast<const char*>(strData) - sizeof(int32_t));
            int32_t length = *lengthPtr;
            if (length <= 0)
                return "";

            const char32_t* chars = static_cast<const char32_t*>(strData);
            std::string result;
            result.reserve(static_cast<size_t>(length));
            for (int32_t index = 0; index < length; ++index)
            {
                const char32_t codepoint = chars[index];
                if (codepoint < 0x80)
                    result.push_back(static_cast<char>(codepoint));
            }
            return result;
        }

        std::string GetNodeClass(void* node)
        {
            alignas(8) char ret[16] = {};
            g_godot3Api.method_bind_ptrcall(mb_Node_get_class, node, nullptr, ret);
            std::string value = ReadGodotString(ret);
            if (g_godot3Api.string_destroy)
                g_godot3Api.string_destroy(ret);
            return value;
        }

        std::string GetNodeName(void* node)
        {
            alignas(8) char ret[16] = {};
            g_godot3Api.method_bind_ptrcall(mb_Node_get_name, node, nullptr, ret);
            std::string value = ReadGodotString(ret);
            if (g_godot3Api.string_destroy)
                g_godot3Api.string_destroy(ret);
            return value;
        }

        void WalkNode(void* node, std::vector<AegisGodotObjectSnapshot>& out, int depth)
        {
            if (!node || depth > 20 || out.size() >= 512)
                return;

            const bool isSpatial = IsType(node, str_Spatial);
            const bool isNode2D = IsType(node, str_Node2D);

            if (isSpatial && mb_Spatial_get_translation)
            {
                AegisGodotObjectSnapshot snap = {};
                const std::string className = GetNodeClass(node);
                const std::string nodeName = GetNodeName(node);
                strncpy_s(snap.className, className.c_str(), _TRUNCATE);
                strncpy_s(snap.name, nodeName.c_str(), _TRUNCATE);

                struct Vec3F { float x, y, z; } pos = {};
                g_godot3Api.method_bind_ptrcall(mb_Spatial_get_translation, node, nullptr, &pos);
                snap.origin = { pos.x, pos.y, pos.z };

                if (mb_Spatial_is_visible_in_tree)
                {
                    uint8_t visible = 0;
                    g_godot3Api.method_bind_ptrcall(mb_Spatial_is_visible_in_tree, node, nullptr, &visible);
                    snap.visible = visible ? 1 : 0;
                }
                else
                {
                    snap.visible = 1;
                }

                snap.boundsMin = { -0.4f, -0.1f, -0.4f };
                snap.boundsMax = { 0.4f, 1.8f, 0.4f };
                snap.flags = AegisGodotObject_Node3D;

                if (IsType(node, str_Camera))
                {
                    activeCamera = node;
                    strncpy_s(snap.name, "Camera", _TRUNCATE);
                    snap.flags |= AegisGodotObject_VisibleOnScreen;
                }
                else
                {
                    bool isLikely = IsType(node, str_KinematicBody) || IsLikelyActorName(className, nodeName);
                    if (isLikely && IsIgnoredTargetNode(className, nodeName))
                        isLikely = false;
                    if (isLikely || g_drawAllNode3Ds)
                    {
                        if (isLikely)
                            snap.flags |= AegisGodotObject_LikelyTarget;
                        out.push_back(snap);
                    }
                }
            }
            else if (isNode2D && (mb_CanvasItem_get_screen_transform || mb_CanvasItem_get_global_transform_with_canvas))
            {
                AegisGodotObjectSnapshot snap = {};
                const std::string className = GetNodeClass(node);
                const std::string nodeName = GetNodeName(node);
                strncpy_s(snap.className, className.c_str(), _TRUNCATE);
                strncpy_s(snap.name, nodeName.c_str(), _TRUNCATE);

                float transform2d[6] = {};
                if (mb_CanvasItem_get_global_transform_with_canvas)
                    g_godot3Api.method_bind_ptrcall(mb_CanvasItem_get_global_transform_with_canvas, node, nullptr, transform2d);
                else if (mb_CanvasItem_get_screen_transform)
                    g_godot3Api.method_bind_ptrcall(mb_CanvasItem_get_screen_transform, node, nullptr, transform2d);
                const float scaleX = std::sqrt((transform2d[0] * transform2d[0]) + (transform2d[1] * transform2d[1]));
                const float scaleY = std::sqrt((transform2d[2] * transform2d[2]) + (transform2d[3] * transform2d[3]));
                float zoom = (scaleX > 0.001f && scaleY > 0.001f) ? ((scaleX + scaleY) * 0.5f) : 1.0f;
                if (zoom < 0.001f) zoom = 1.0f;
                snap.origin = { transform2d[4], transform2d[5], zoom };
                snap.flags = AegisGodotObject_Node2D;
                snap.visible = 1;

                if (IsType(node, str_Camera2D))
                    strncpy_s(snap.name, "Camera2D", _TRUNCATE);
                else
                {
                    bool isLikely = IsLikelyActorName(className, nodeName);
                    if (isLikely && IsIgnoredTargetNode(className, nodeName))
                        isLikely = false;
                    if (isLikely || g_drawAllNode3Ds)
                    {
                        if (isLikely)
                            snap.flags |= AegisGodotObject_LikelyTarget;
                        out.push_back(snap);
                    }
                }
            }

            if (mb_Node_get_child_count && mb_Node_get_child)
            {
                int64_t childCount = 0;
                int8_t includeInternal = 0;
                const void* args[] = { &includeInternal };
                g_godot3Api.method_bind_ptrcall(mb_Node_get_child_count, node, args, &childCount);

                const int64_t maxChildren = childCount < 256 ? childCount : 256;
                for (int64_t childIndex = 0; childIndex < maxChildren; ++childIndex)
                {
                    void* child = nullptr;
                    int64_t index = childIndex;
                    int8_t internal = 0;
                    const void* childArgs[] = { &index, &internal };
                    g_godot3Api.method_bind_ptrcall(mb_Node_get_child, node, childArgs, &child);
                    if (child)
                        WalkNode(child, out, depth + 1);
                }
            }
        }
    };

    Godot3AutoScanState g_godot3Scan;

    bool Godot3Init()
    {
        return g_godot3Scan.Init();
    }

    void* Godot3GetActiveCamera()
    {
        return g_godot3Scan.activeCamera;
    }

    std::uint32_t AEGIS_GODOT_CALL Godot3CollectObjects(
        AegisGodotObjectSnapshot* outObjects,
        std::uint32_t capacity,
        void*)
    {
        if (!g_godot3Scan.Init() || !g_godot3Scan.sceneTree || !g_godot3Scan.mb_SceneTree_get_root)
            return 0;

        g_godot3Scan.activeCamera = nullptr;
        void* root = nullptr;
        g_godot3Api.method_bind_ptrcall(g_godot3Scan.mb_SceneTree_get_root, g_godot3Scan.sceneTree, nullptr, &root);
        if (!root)
            return 0;

        std::vector<AegisGodotObjectSnapshot> collected;
        g_godot3Scan.WalkNode(root, collected, 0);
        LogMsg("[AegisGodot3] Collected %d objects from SceneTree root %p\n", static_cast<int>(collected.size()), root);

        const std::uint32_t collectedCount = static_cast<std::uint32_t>(collected.size());
        const std::uint32_t count = capacity < collectedCount ? capacity : collectedCount;
        for (std::uint32_t index = 0; index < count; ++index)
            outObjects[index] = collected[index];
        return count;
    }

    int AEGIS_GODOT_CALL Godot3GetViewProjection(AegisGodotMatrix4x4* outMatrix, void*)
    {
        if (!outMatrix || !g_godot3Scan.activeCamera)
            return 0;
        if (!g_godot3Scan.mb_Camera_get_camera_transform || !g_godot3Scan.mb_Camera_get_projection)
            return 0;

        float cameraTransform[12] = {};
        float projection[16] = {};
        g_godot3Api.method_bind_ptrcall(
            g_godot3Scan.mb_Camera_get_camera_transform,
            g_godot3Scan.activeCamera,
            nullptr,
            cameraTransform);
        g_godot3Api.method_bind_ptrcall(
            g_godot3Scan.mb_Camera_get_projection,
            g_godot3Scan.activeCamera,
            nullptr,
            projection);

        outMatrix->flags = AegisGodotMatrix_ColumnMajor | AegisGodotMatrix_OpenGLDepth;
        for (int index = 0; index < 16; ++index)
            outMatrix->m[index] = (index < 12) ? cameraTransform[index] : 0.0f;
        outMatrix->m[15] = 1.0f;
        (void)projection;
        return 1;
    }

    const GodotAutoScanBackend kGodot3Backend =
    {
        &Godot3Init,
        &Godot3GetActiveCamera,
        &Godot3CollectObjects,
        &Godot3GetViewProjection,
        "Godot 3 GDNative"
    };
}

const GodotAutoScanBackend& GetGodot3AutoScanBackend()
{
    return kGodot3Backend;
}
