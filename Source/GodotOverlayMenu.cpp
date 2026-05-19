#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AegisGodotUniversal.h"
#include "AegisUniversalOverlay.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>

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
    float g_boxThickness = 1.5f;
    float g_lineThickness = 1.25f;
    ImVec4 g_boxColor = ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
    ImVec4 g_lineColor = ImVec4(1.0f, 0.75f, 0.25f, 0.9f);
    ImVec4 g_fillColor = ImVec4(0.55f, 0.85f, 1.0f, 0.12f);

    AegisGodotVec3 Add(const AegisGodotVec3& a, const AegisGodotVec3& b)
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    bool ProjectBounds(const AegisGodotObjectSnapshot& object, Rect2D& rect, std::uint32_t& projected, std::uint32_t& clipped)
    {
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
        return rect.valid && rect.maxX > rect.minX && rect.maxY > rect.minY;
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
}
