#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AegisGodotUniversal.h"
#include "AegisUniversalOverlay.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

extern bool g_drawAllNode3Ds;

namespace
{
    struct Rect2D
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        bool valid = false;
    };

    bool g_drawEnabled = true;
    bool g_drawBoxes = true;
    bool g_drawCornerBoxes = true;
    bool g_drawFilledBoxes = false;
    bool g_drawLines = true;
    bool g_drawLabels = true;
    bool g_hideInvisible = true;
    bool g_drawLikelyOnly = true;
    float g_boxThickness = 1.5f;
    float g_lineThickness = 1.25f;
    ImVec4 g_boxColor = ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
    ImVec4 g_lineColor = ImVec4(1.0f, 0.75f, 0.25f, 0.9f);
    ImVec4 g_fillColor = ImVec4(0.55f, 0.85f, 1.0f, 0.12f);

    AegisGodotVec3 Add(const AegisGodotVec3& a, const AegisGodotVec3& b)
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    void Resolve2DCanvasScale(const AegisGodotObjectSnapshot& object, const ImVec2& display, float& scaleX, float& scaleY)
    {
        scaleX = 1.0f;
        scaleY = 1.0f;
        if (display.x <= 1.0f || display.y <= 1.0f ||
            !std::isfinite(object.origin.x) || !std::isfinite(object.origin.y))
        {
            return;
        }

        struct CandidateCanvas
        {
            float width;
            float height;
        };

        static constexpr CandidateCanvas candidates[] = {
            { 1920.0f, 1080.0f },
            { 1600.0f, 900.0f },
            { 1366.0f, 768.0f },
            { 1280.0f, 720.0f },
            { 1024.0f, 576.0f }
        };

        const float displayAspect = display.x / display.y;
        float bestScore = 1000.0f;
        const CandidateCanvas* best = nullptr;
        for (const CandidateCanvas& candidate : candidates)
        {
            if (display.x >= candidate.width - 1.0f && display.y >= candidate.height - 1.0f)
                continue;
            if (object.origin.x < -candidate.width * 0.25f || object.origin.x > candidate.width * 1.25f ||
                object.origin.y < -candidate.height * 0.25f || object.origin.y > candidate.height * 1.25f)
            {
                continue;
            }

            const float aspectScore = std::abs(displayAspect - (candidate.width / candidate.height));
            const float centerScore =
                (std::abs(object.origin.x - (candidate.width * 0.5f)) / candidate.width) +
                (std::abs(object.origin.y - (candidate.height * 0.5f)) / candidate.height);
            const float score = aspectScore + centerScore;
            if (score < bestScore)
            {
                bestScore = score;
                best = &candidate;
            }
        }

        if (best && bestScore < 0.35f)
        {
            scaleX = display.x / best->width;
            scaleY = display.y / best->height;
        }
    }

    bool ProjectBounds(const AegisGodotObjectSnapshot& object, Rect2D& rect, std::uint32_t& projected, std::uint32_t& clipped)
    {
        if (object.flags & AegisGodotObject_Node2D)
        {
            // For 2D, origin is in Godot canvas coordinates. Stretch modes often keep that canvas
            // at 1920x1080 while the OpenGL framebuffer/ImGui display is smaller.
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            float canvasScaleX = 1.0f;
            float canvasScaleY = 1.0f;
            Resolve2DCanvasScale(object, display, canvasScaleX, canvasScaleY);

            const float originX = object.origin.x * canvasScaleX;
            const float originY = object.origin.y * canvasScaleY;
            const float boundsScale = (canvasScaleX + canvasScaleY) * 0.5f;
            float zoom = object.origin.z;
            if (zoom < 0.01f) zoom = 1.0f;
            zoom *= boundsScale;

            rect.minX = originX + object.boundsMin.x * zoom;
            rect.maxX = originX + object.boundsMax.x * zoom;
            rect.minY = originY + object.boundsMin.y * zoom;
            rect.maxY = originY + object.boundsMax.y * zoom;
            rect.valid = true;
            ++projected;
            return true;
        }

        const std::array<AegisGodotVec3, 8> corners = {
            Add(object.origin, { object.boundsMin.x, object.boundsMin.y, object.boundsMin.z }),
            Add(object.origin, { object.boundsMax.x, object.boundsMin.y, object.boundsMin.z }),
            Add(object.origin, { object.boundsMin.x, object.boundsMax.y, object.boundsMin.z }),
            Add(object.origin, { object.boundsMax.x, object.boundsMax.y, object.boundsMin.z }),
            Add(object.origin, { object.boundsMin.x, object.boundsMin.y, object.boundsMax.z }),
            Add(object.origin, { object.boundsMax.x, object.boundsMin.y, object.boundsMax.z }),
            Add(object.origin, { object.boundsMin.x, object.boundsMax.y, object.boundsMax.z }),
            Add(object.origin, { object.boundsMax.x, object.boundsMax.y, object.boundsMax.z })
        };

        rect = {};
        for (const AegisGodotVec3& corner : corners)
        {
            AegisGodotProjectedPoint point = {};
            if (!AegisGodot_ProjectWorldToScreen(&corner, &point) || point.clipped ||
                !std::isfinite(point.x) || !std::isfinite(point.y))
            {
                ++clipped;
                continue;
            }

            ++projected;
            if (!rect.valid)
            {
                rect.minX = rect.maxX = point.x;
                rect.minY = rect.maxY = point.y;
                rect.valid = true;
            }
            else
            {
                rect.minX = std::min(rect.minX, point.x);
                rect.minY = std::min(rect.minY, point.y);
                rect.maxX = std::max(rect.maxX, point.x);
                rect.maxY = std::max(rect.maxY, point.y);
            }
        }

        if (rect.valid && rect.maxX > rect.minX && rect.maxY > rect.minY)
            return true;

        // Fallback: still draw at the detected object origin if bounds projection is clipped.
        AegisGodotProjectedPoint originPoint = {};
        if (!AegisGodot_ProjectWorldToScreen(&object.origin, &originPoint) || originPoint.clipped ||
            !std::isfinite(originPoint.x) || !std::isfinite(originPoint.y))
        {
            ++clipped;
            return false;
        }

        ++projected;
        const float halfWidth = 10.0f;
        const float halfHeight = 18.0f;
        rect.minX = originPoint.x - halfWidth;
        rect.maxX = originPoint.x + halfWidth;
        rect.minY = originPoint.y - halfHeight;
        rect.maxY = originPoint.y + halfHeight;
        rect.valid = true;
        return true;
    }

    void DrawCornerBox(ImDrawList* drawList, const Rect2D& rect, ImU32 color)
    {
        const float width = rect.maxX - rect.minX;
        const float height = rect.maxY - rect.minY;
        const float x = rect.minX;
        const float y = rect.minY;
        const float w = width * 0.25f;
        const float h = height * 0.22f;
        drawList->AddLine(ImVec2(x, y), ImVec2(x + w, y), color, g_boxThickness);
        drawList->AddLine(ImVec2(x, y), ImVec2(x, y + h), color, g_boxThickness);
        drawList->AddLine(ImVec2(x + width, y), ImVec2(x + width - w, y), color, g_boxThickness);
        drawList->AddLine(ImVec2(x + width, y), ImVec2(x + width, y + h), color, g_boxThickness);
        drawList->AddLine(ImVec2(x, y + height), ImVec2(x + w, y + height), color, g_boxThickness);
        drawList->AddLine(ImVec2(x, y + height), ImVec2(x, y + height - h), color, g_boxThickness);
        drawList->AddLine(ImVec2(x + width, y + height), ImVec2(x + width - w, y + height), color, g_boxThickness);
        drawList->AddLine(ImVec2(x + width, y + height), ImVec2(x + width, y + height - h), color, g_boxThickness);
    }

    void DrawObjectOverlay()
    {
        if (!g_drawEnabled)
            return;

        const std::uint32_t count = std::min<std::uint32_t>(AegisGodot_GetObjectCount(), 512);
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(g_boxColor);
        const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(g_lineColor);
        const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(g_fillColor);

        std::uint32_t projected = 0;
        std::uint32_t clipped = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            AegisGodotObjectSnapshot object = {};
            if (!AegisGodot_GetObjectSnapshot(i, &object))
                continue;
            if (g_hideInvisible && !object.visible)
                continue;
            if (g_drawLikelyOnly && (object.flags & AegisGodotObject_LikelyTarget) == 0)
                continue;

            Rect2D rect = {};
            if (!ProjectBounds(object, rect, projected, clipped))
                continue;

            if (g_drawFilledBoxes)
                drawList->AddRectFilled(ImVec2(rect.minX, rect.minY), ImVec2(rect.maxX, rect.maxY), fillColor);
            if (g_drawBoxes)
                drawList->AddRect(ImVec2(rect.minX, rect.minY), ImVec2(rect.maxX, rect.maxY), boxColor, 0.0f, 0, g_boxThickness);
            if (g_drawCornerBoxes)
                DrawCornerBox(drawList, rect, boxColor);
            if (g_drawLines)
                drawList->AddLine(ImVec2(display.x * 0.5f, display.y), ImVec2((rect.minX + rect.maxX) * 0.5f, rect.maxY), lineColor, g_lineThickness);
            if (g_drawLabels)
                drawList->AddText(ImVec2(rect.minX, std::max(0.0f, rect.minY - 16.0f)), boxColor, object.name[0] ? object.name : object.className);
        }
    }

    void DrawObjectTable()
    {
        const std::uint32_t count = std::min<std::uint32_t>(AegisGodot_GetObjectCount(), 128);
        if (ImGui::BeginTable("godot-objects", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Class");
            ImGui::TableSetupColumn("Path");
            ImGui::TableSetupColumn("Origin");
            ImGui::TableSetupColumn("Visible");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableHeadersRow();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                AegisGodotObjectSnapshot object = {};
                if (!AegisGodot_GetObjectSnapshot(i, &object))
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(object.id));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(object.name);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(object.className);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(object.path);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f, %.2f, %.2f", object.origin.x, object.origin.y, object.origin.z);
                ImGui::TableNextColumn();
                ImGui::Text("%s", object.visible ? "yes" : "no");
                ImGui::TableNextColumn();
                ImGui::Text("0x%08x", object.flags);
            }
            ImGui::EndTable();
        }
    }

    struct LocalClass
    {
        std::string name;
        std::vector<AegisGodotMemberInfo> members;
    };
    
    std::vector<LocalClass> g_localClasses;
    bool g_classDbScanned = false;
    char g_classSearch[64] = {};
    char g_memberSearch[64] = {};
    int g_selectedClassIndex = -1;

    bool ContainsAnsi(const char* haystack, const char* needle)
    {
        if (!haystack || !needle)
            return false;
        std::string h = haystack;
        std::string n = needle;
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return h.find(n) != std::string::npos;
    }

    void UpdateLocalClassCache()
    {
        g_localClasses.clear();
        std::uint32_t count = AegisGodot_GetMemberCount();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            AegisGodotMemberInfo member = {};
            if (!AegisGodot_GetMemberInfo(i, &member))
                continue;
                
            auto it = std::find_if(g_localClasses.begin(), g_localClasses.end(), [&](const LocalClass& c) {
                return c.name == member.className;
            });
            
            if (it == g_localClasses.end())
            {
                LocalClass newClass;
                newClass.name = member.className;
                newClass.members.push_back(member);
                g_localClasses.push_back(newClass);
            }
            else
            {
                it->members.push_back(member);
            }
        }
        
        std::sort(g_localClasses.begin(), g_localClasses.end(), [](const LocalClass& a, const LocalClass& b) {
            return a.name < b.name;
        });
    }
}

extern "C" const char* AegisUniversalOverlay_GetEngineOverlayName()
{
    return "Godot";
}

extern "C" void AegisUniversalOverlay_PollEngineProviders()
{
    AegisGodot_UpdateProviders();
}

extern "C" void AegisUniversalOverlay_DrawEngineOverlay()
{
    DrawObjectOverlay();
}

extern "C" void AegisUniversalOverlay_DrawEngineMenu()
{
    if (ImGui::BeginTabItem("Adapter"))
    {
        AegisGodotCapabilityInfo capability = {};
        AegisGodot_GetCapabilityInfo(&capability);
        AegisGodotAdapterTiming timing = {};
        AegisGodot_GetAdapterTiming(&timing);

        ImGui::Text("Renderer backend: %ls", capability.rendererBackend);
        ImGui::TextWrapped("Details: %ls", capability.details);
        ImGui::Separator();
        ImGui::Text("Godot detected: %s", capability.godotDetected ? "pass" : "warn");
        ImGui::Text("PCK: %s | GDExtension: %s | GDNative: %s",
            capability.embeddedPackSectionFound ? "pass" : "warn",
            capability.gdExtensionExportFound ? "pass" : "warn",
            capability.gdNativeExportFound ? "pass" : "warn");
        ImGui::Text("Object provider: %s | Matrix provider: %s | Viewport provider: %s",
            capability.objectProviderRegistered ? "pass" : "warn",
            capability.viewProjectionProviderRegistered ? "pass" : "warn",
            capability.viewportProviderRegistered ? "pass" : "warn");
        ImGui::Text("Viewport valid: %s | Matrix valid: %s | W2S: %s",
            capability.viewportValid ? "pass" : "warn",
            capability.matrixValid ? "pass" : "warn",
            capability.w2sProjectionWorking ? "pass" : "warn");
        ImGui::Text("Frame %llu | objects %u | projected %u | clipped %u",
            static_cast<unsigned long long>(timing.frameId),
            timing.objectCount,
            timing.projectedCount,
            timing.clippedCount);
        ImGui::Text("Provider timing: objects %.3f ms | matrix %.3f ms | viewport %.3f ms",
            timing.objectProviderMs,
            timing.matrixProviderMs,
            timing.viewportProviderMs);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Overlay"))
    {
        ImGui::Checkbox("Draw provider overlay", &g_drawEnabled);
        ImGui::Checkbox("Hide invisible objects", &g_hideInvisible);
        ImGui::Checkbox("Draw likely player targets only", &g_drawLikelyOnly);
        ImGui::Checkbox("Draw all SceneTree nodes (cluttered)", &g_drawAllNode3Ds);
        ImGui::Checkbox("Boxes", &g_drawBoxes);
        ImGui::Checkbox("Corner boxes", &g_drawCornerBoxes);
        ImGui::Checkbox("Filled boxes", &g_drawFilledBoxes);
        ImGui::Checkbox("Lines", &g_drawLines);
        ImGui::Checkbox("Labels", &g_drawLabels);
        ImGui::SliderFloat("Box thickness", &g_boxThickness, 0.5f, 6.0f, "%.1f");
        ImGui::SliderFloat("Line thickness", &g_lineThickness, 0.5f, 6.0f, "%.1f");
        ImGui::ColorEdit4("Box color", &g_boxColor.x);
        ImGui::ColorEdit4("Line color", &g_lineColor.x);
        ImGui::ColorEdit4("Fill color", &g_fillColor.x);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Objects"))
    {
        ImGui::Text("Object count: %u", AegisGodot_GetObjectCount());
        if (ImGui::Button("Print current objects to console"))
            AegisGodot_PrintCurrentObjects();
        DrawObjectTable();
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Inspector"))
    {
        if (ImGui::Button("Scan Godot ClassDB"))
        {
            AegisGodot_ScanClasses();
            UpdateLocalClassCache();
            g_classDbScanned = true;
            g_selectedClassIndex = -1;
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Export SDK Header") && g_classDbScanned && !g_localClasses.empty())
        {
            std::wofstream out(L"AegisGodotSDK_Generated.h", std::ios::trunc);
            if (out)
            {
                out << L"#pragma once\n\n";
                out << L"// Generated Godot SDK Header\n";
                out << L"#include <cstdint>\n\n";
                out << L"namespace GodotSDK\n{\n";
                for (const auto& c : g_localClasses)
                {
                    out << L"    // Class: " << c.name.c_str() << L"\n";
                    out << L"    namespace " << c.name.c_str() << L"\n    {\n";
                    for (const auto& m : c.members)
                    {
                        if (m.isMethod)
                        {
                            out << L"        // Method: " << m.memberName << L"\n";
                            out << L"        inline const char* Method_" << m.memberName << L"() { return \"" << m.memberName << L"\"; }\n";
                        }
                        else
                        {
                            out << L"        // Property: " << m.memberName << L" (" << m.typeName << L")\n";
                            out << L"        inline const char* Property_" << m.memberName << L"() { return \"" << m.memberName << L"\"; }\n";
                        }
                    }
                    out << L"    }\n\n";
                }
                out << L"}\n";
            }
        }
        
        ImGui::Separator();
        
        ImGui::Columns(2, "inspector-columns", true);
        
        ImGui::Text("Classes");
        ImGui::InputText("Search Class", g_classSearch, sizeof(g_classSearch));
        
        std::vector<int> filteredClassIndices;
        for (size_t i = 0; i < g_localClasses.size(); ++i)
        {
            if (g_classSearch[0] == '\0' || ContainsAnsi(g_localClasses[i].name.c_str(), g_classSearch))
            {
                filteredClassIndices.push_back(static_cast<int>(i));
            }
        }
        
        if (ImGui::BeginChild("classes-child", ImVec2(0, 300), true))
        {
            for (int idx : filteredClassIndices)
            {
                const bool isSelected = (g_selectedClassIndex == idx);
                if (ImGui::Selectable(g_localClasses[idx].name.c_str(), isSelected))
                {
                    g_selectedClassIndex = idx;
                }
            }
            ImGui::EndChild();
        }
        
        ImGui::NextColumn();
        
        ImGui::Text("Properties & Methods");
        ImGui::InputText("Search Member", g_memberSearch, sizeof(g_memberSearch));
        
        if (g_selectedClassIndex >= 0 && g_selectedClassIndex < static_cast<int>(g_localClasses.size()))
        {
            const auto& selectedClass = g_localClasses[g_selectedClassIndex];
            std::vector<AegisGodotMemberInfo> filteredMembers;
            for (const auto& m : selectedClass.members)
            {
                if (g_memberSearch[0] == '\0' || ContainsAnsi(m.memberName, g_memberSearch))
                {
                    filteredMembers.push_back(m);
                }
            }
            
            if (ImGui::BeginChild("members-child", ImVec2(0, 300), true))
            {
                if (ImGui::BeginTable("members-table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Type");
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Kind");
                    ImGui::TableHeadersRow();
                    
                    for (const auto& m : filteredMembers)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(m.typeName);
                        ImGui::TableNextColumn();
                        
                        if (ImGui::Selectable(m.memberName))
                        {
                            ImGui::SetClipboardText(m.memberName);
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("Click to copy name to clipboard");
                        }
                        
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(m.isMethod ? "Method" : "Property");
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
            }
        }
        else
        {
            ImGui::Text("Select a class to view its members.");
        }
        
        ImGui::Columns(1);
        ImGui::EndTabItem();
    }
}
