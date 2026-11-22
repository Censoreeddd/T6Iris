#include "StdInclude.hpp"
#include "Theme.hpp"

#include "Configuration/PreferencesConfiguration.hpp"
#include "imgui_internal.h"

namespace IWXMVM::UI::Theme
{
    namespace
    {
        using Palettes::Color;
        using Palettes::Mix;
    }  // namespace

    const Palettes::Palette& Current()
    {
        const auto& preferences = PreferencesConfiguration::Get();
        // glass: the colours from the liquid gradient's (its deep colour the background, its glow the accent)
        if (preferences.uiGlass)
        {
            static Palettes::Palette glass;
            const Color base{preferences.uiGlassBase & 0xFFFFFF}, glow{preferences.uiGlassColor & 0xFFFFFF};
            glass = Palettes::MakeCustom(base, {0xECEAF6}, Mix(glow, {0xFFFFFF}, 0.18f));
            glass.surface = Mix(base, glow, 0.14f);
            glass.raised = Mix(base, glow, 0.28f);
            glass.border = Mix(Mix(base, glow, 0.3f), {0xFFFFFF}, 0.15f);
            glass.onAccent = {0xFFFFFF};
            return glass;
        }
        if (preferences.palette == Palettes::CUSTOM_ID)
        {
            static Palettes::Palette custom;
            custom = Palettes::MakeCustom({preferences.customBackground & 0xFFFFFF}, {preferences.customText & 0xFFFFFF},
                                          {preferences.customAccent & 0xFFFFFF});
            return custom;
        }
        return Palettes::Get(preferences.palette);
    }

    ImVec4 Vec4(Color color, float alpha)
    {
        return {color.R() / 255.0f, color.G() / 255.0f, color.B() / 255.0f, alpha};
    }

    ImU32 U32(Color color, float alpha)
    {
        return color.Abgr(static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
    }

    void Apply()
    {
        const auto& p = Current();
        // a step up from raised for a hover, the accent mixed in for what is selected or held
        const auto hover = Mix(p.raised, p.text, 0.08f);
        const auto selected = Mix(p.raised, p.accent, 0.22f);
        const auto held = Mix(p.raised, p.accent, 0.40f);

        auto& c = ImGui::GetStyle().Colors;
        c[ImGuiCol_Text] = Vec4(p.text);
        c[ImGuiCol_TextDisabled] = Vec4(p.muted);
        // glass: the windows see-through, so the wallpaper (UI::Background) shows under them, frosted
        const bool glass = PreferencesConfiguration::Get().uiGlass;
        c[ImGuiCol_WindowBg] = Vec4(p.bg, glass ? 0.55f : 1.0f);
        c[ImGuiCol_ChildBg] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_PopupBg] = Vec4(Mix(p.surface, p.raised, 0.35f), glass ? 0.94f : 0.98f);
        // (borders and rules softened toward the background: lines that separate without boxing things in)
        c[ImGuiCol_Border] = Vec4(Mix(p.border, p.bg, 0.25f));
        c[ImGuiCol_BorderShadow] = Vec4(p.bg, 0.0f);
        // (frames a step up from the side panel's card, which is surface, so fields stand out on it)
        const auto frame = Mix(p.surface, p.raised, 0.55f);
        c[ImGuiCol_FrameBg] = Vec4(frame);
        c[ImGuiCol_FrameBgHovered] = Vec4(Mix(frame, p.text, 0.06f));
        c[ImGuiCol_FrameBgActive] = Vec4(Mix(frame, p.text, 0.10f));
        c[ImGuiCol_TitleBg] = Vec4(p.surface);
        c[ImGuiCol_TitleBgActive] = Vec4(p.raised);
        c[ImGuiCol_TitleBgCollapsed] = Vec4(p.surface);
        c[ImGuiCol_MenuBarBg] = Vec4(p.surface, glass ? 0.5f : 1.0f);
        c[ImGuiCol_ScrollbarBg] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_ScrollbarGrab] = Vec4(Mix(p.raised, p.text, 0.06f));
        c[ImGuiCol_ScrollbarGrabHovered] = Vec4(hover);
        c[ImGuiCol_ScrollbarGrabActive] = Vec4(held);
        c[ImGuiCol_CheckMark] = Vec4(p.accent);
        c[ImGuiCol_SliderGrab] = Vec4(p.accent);
        c[ImGuiCol_SliderGrabActive] = Vec4(p.AccentHover());
        c[ImGuiCol_Button] = Vec4(p.raised);
        c[ImGuiCol_ButtonHovered] = Vec4(hover);
        c[ImGuiCol_ButtonActive] = Vec4(held);
        c[ImGuiCol_Header] = Vec4(selected);
        c[ImGuiCol_HeaderHovered] = Vec4(hover);
        c[ImGuiCol_HeaderActive] = Vec4(held);
        c[ImGuiCol_Separator] = Vec4(Mix(p.border, p.bg, 0.35f));
        c[ImGuiCol_SeparatorHovered] = Vec4(p.muted);
        c[ImGuiCol_SeparatorActive] = Vec4(p.accent);
        c[ImGuiCol_ResizeGrip] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_ResizeGripHovered] = Vec4(p.muted, 0.6f);
        c[ImGuiCol_ResizeGripActive] = Vec4(p.accent, 0.8f);
        c[ImGuiCol_Tab] = Vec4(p.surface);
        c[ImGuiCol_TabHovered] = Vec4(hover);
        c[ImGuiCol_TabActive] = Vec4(Mix(p.raised, p.accent, 0.30f));
        c[ImGuiCol_TabUnfocused] = Vec4(p.surface);
        c[ImGuiCol_TabUnfocusedActive] = Vec4(p.raised);
        c[ImGuiCol_PlotLines] = Vec4(p.muted);
        c[ImGuiCol_PlotLinesHovered] = Vec4(p.accent);
        c[ImGuiCol_PlotHistogram] = Vec4(p.raised);
        c[ImGuiCol_PlotHistogramHovered] = Vec4(hover);
        c[ImGuiCol_TableHeaderBg] = Vec4(p.raised);
        c[ImGuiCol_TableBorderStrong] = Vec4(p.border);
        c[ImGuiCol_TableBorderLight] = Vec4(Mix(p.border, p.bg, 0.4f));
        c[ImGuiCol_TableRowBg] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_TableRowBgAlt] = Vec4(p.text, 0.03f);
        c[ImGuiCol_TextSelectedBg] = Vec4(p.accent, 0.30f);
        c[ImGuiCol_DragDropTarget] = Vec4(p.accent);
        c[ImGuiCol_NavHighlight] = Vec4(p.accent);
        c[ImGuiCol_NavWindowingHighlight] = Vec4(p.text, 0.7f);
        c[ImGuiCol_NavWindowingDimBg] = Vec4({0x000000}, 0.4f);
        c[ImGuiCol_ModalWindowDimBg] = Vec4({0x000000}, 0.5f);
        // glass: fields, buttons and headers frosted panes of white over the gradient, what's selected the glow's
        if (glass)
        {
            c[ImGuiCol_FrameBg] = Vec4({0xFFFFFF}, 0.06f);
            c[ImGuiCol_FrameBgHovered] = Vec4({0xFFFFFF}, 0.10f);
            c[ImGuiCol_FrameBgActive] = Vec4({0xFFFFFF}, 0.14f);
            c[ImGuiCol_Button] = Vec4({0xFFFFFF}, 0.07f);
            c[ImGuiCol_ButtonHovered] = Vec4({0xFFFFFF}, 0.12f);
            c[ImGuiCol_ButtonActive] = Vec4(p.accent, 0.45f);
            c[ImGuiCol_Header] = Vec4(p.accent, 0.22f);
            c[ImGuiCol_HeaderHovered] = Vec4(p.accent, 0.32f);
            c[ImGuiCol_HeaderActive] = Vec4(p.accent, 0.42f);
            c[ImGuiCol_Border] = Vec4({0xFFFFFF}, 0.12f);
            c[ImGuiCol_Separator] = Vec4({0xFFFFFF}, 0.10f);
            c[ImGuiCol_PopupBg] = Vec4(p.surface, 0.94f);
            c[ImGuiCol_WindowBg] = Vec4(p.bg, 0.35f);
            c[ImGuiCol_MenuBarBg] = Vec4(p.bg, 0.35f);
            c[ImGuiCol_TableHeaderBg] = Vec4({0xFFFFFF}, 0.07f);
            c[ImGuiCol_TableRowBgAlt] = Vec4({0xFFFFFF}, 0.03f);
            c[ImGuiCol_ScrollbarGrab] = Vec4({0xFFFFFF}, 0.14f);
            c[ImGuiCol_ScrollbarGrabHovered] = Vec4({0xFFFFFF}, 0.22f);
            c[ImGuiCol_TabActive] = Vec4(p.accent, 0.35f);
        }
    }

    ImVec4 Favorite()
    {
        return Vec4(Current().warn);
    }

    ImVec4 Misfit()
    {
        return Vec4(Current().bad);
    }

    ImVec4 Note()
    {
        return Vec4(Current().warn);
    }

    ImVec4 Problem()
    {
        return Vec4(Current().bad);
    }

    ImVec4 Done()
    {
        return Vec4(Current().ok);
    }

    ImU32 Playhead()
    {
        return U32(Current().accent);
    }

    ImU32 SelectionFill()
    {
        return U32(Current().accent, 0.16f);
    }

    ImU32 SelectionEdge()
    {
        return U32(Current().accent, 0.8f);
    }

    ImU32 Card(float alpha)
    {
        return U32(Current().surface, alpha);
    }

    ImVec4 Link(bool hovered)
    {
        const auto& p = Current();
        return Vec4(hovered ? p.AccentHover() : p.accent);
    }

    ImVec4 Keys()
    {
        return Vec4(Current().accent);
    }

    bool IsGlass()
    {
        return PreferencesConfiguration::Get().uiGlass;
    }

    void DrawGlow(ImDrawList* draw, ImVec2 center, float radius, Color color, float alpha, float squash)
    {
        if (radius <= 0 || alpha <= 0)
            return;
        constexpr int SEGMENTS = 48;
        const auto inner = U32(color, alpha), outer = U32(color, 0.0f);
        const auto uv = draw->_Data->TexUvWhitePixel;
        // a middle ring at half the radius, at a quarter of the alpha, so it falls off softly rather than as a cone
        const auto middle = U32(color, alpha * 0.3f);
        draw->PrimReserve(SEGMENTS * 9, 1 + SEGMENTS * 2);
        const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
        draw->PrimWriteVtx(center, uv, inner);
        for (int i = 0; i < SEGMENTS; ++i)
        {
            const auto angle = static_cast<float>(i) / SEGMENTS * 6.2831853f;
            const ImVec2 direction(std::cos(angle), std::sin(angle) * squash);
            draw->PrimWriteVtx(ImVec2(center.x + direction.x * radius * 0.45f, center.y + direction.y * radius * 0.45f),
                               uv, middle);
            draw->PrimWriteVtx(ImVec2(center.x + direction.x * radius, center.y + direction.y * radius), uv, outer);
        }
        for (int i = 0; i < SEGMENTS; ++i)
        {
            const int next = (i + 1) % SEGMENTS;
            const auto m0 = static_cast<ImDrawIdx>(base + 1 + i * 2), o0 = static_cast<ImDrawIdx>(m0 + 1);
            const auto m1 = static_cast<ImDrawIdx>(base + 1 + next * 2), o1 = static_cast<ImDrawIdx>(m1 + 1);
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(m0);
            draw->PrimWriteIdx(m1);
            draw->PrimWriteIdx(m0);
            draw->PrimWriteIdx(o0);
            draw->PrimWriteIdx(o1);
            draw->PrimWriteIdx(m0);
            draw->PrimWriteIdx(o1);
            draw->PrimWriteIdx(m1);
        }
    }
}  // namespace IWXMVM::UI::Theme
