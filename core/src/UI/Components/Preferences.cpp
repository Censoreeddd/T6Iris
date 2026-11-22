#include "StdInclude.hpp"
#include "Preferences.hpp"

#include "UI/Theme.hpp"
#include "UI/Fonts.hpp"
#include "Utilities/PathUtils.hpp"

#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/UIManager.hpp"
#include "UI/Layout.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        // whether a control changed a preference this frame, to save them once (not every frame of a drag)
        bool changed = false;
    }  // namespace

    void Preferences::Initialize()
    {
    }

    // a color of the preferences (0xRRGGBB) with ImGui's picker; whether it changed
    bool EditColor(const char* label, std::uint32_t& rgb)
    {
        float color[3] = {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                          static_cast<float>(rgb & 0xFF) / 255.0f};
        if (!ImGui::ColorEdit3(label, color, ImGuiColorEditFlags_NoInputs))
            return false;
        const auto channel = [](float value) {
            return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        rgb = channel(color[0]) << 16 | channel(color[1]) << 8 | channel(color[2]);
        return true;
    }

    void DrawMiscSection()
    {
        auto& preferences = PreferencesConfiguration::Get();

        // the palette, shared with the launcher: a dot of each one's accent (and the user's own last), the one in use
        // ringed, seven a row
        ImGui::TextDisabled("Colors");
        const auto& current = Theme::Current();
        const float dot = ImGui::GetFrameHeight() * 1.1f;
        auto* drawList = ImGui::GetWindowDrawList();
        const auto custom = Palettes::MakeCustom({preferences.customBackground}, {preferences.customText},
                                                 {preferences.customAccent});
        std::vector<const Palettes::Palette*> palettes;
        for (const auto& palette : Palettes::ALL)
            palettes.push_back(&palette);
        palettes.push_back(&custom);
        for (std::size_t i = 0; i < palettes.size(); ++i)
        {
            const auto& palette = *palettes[i];
            ImGui::PushID(palette.id.data());
            if (i % 7 != 0)
                ImGui::SameLine(0.0f, dot * 0.35f);
            if (ImGui::InvisibleButton("##palette", ImVec2(dot, dot)))
            {
                preferences.palette = palette.id;
                UIManager::Get().ApplyPalette();
                changed = true;
            }
            const auto center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
            if (palette.id == current.id)
                drawList->AddCircle(center, dot * 0.5f - 1.0f, Theme::U32(current.text), 24, 2.0f);
            else if (ImGui::IsItemHovered())
                drawList->AddCircle(center, dot * 0.5f - 1.0f, Theme::U32(current.muted), 24, 1.5f);
            // (the background's half and the accent's)
            drawList->AddCircleFilled(center, dot * 0.5f - 4.0f, Theme::U32(palette.bg), 24);
            drawList->PathArcTo(center, dot * 0.5f - 4.0f, -1.5708f, 1.5708f, 16);
            drawList->PathFillConvex(Theme::U32(palette.accent));
            drawList->AddCircle(center, dot * 0.5f - 4.0f, Theme::U32(palette.raised), 24, 1.0f);
            ImGui::SetItemTooltip("%s", std::string(palette.name).c_str());
            ImGui::PopID();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", std::string(current.name).c_str());
        ImGui::SetItemTooltip("The editor's colors (and the launcher's, but for Custom)");

        // the user's own: its three colors
        if (preferences.palette == Palettes::CUSTOM_ID)
        {
            bool edited = EditColor("Background##customBackground", preferences.customBackground);
            ImGui::SameLine();
            edited |= EditColor("Text##customText", preferences.customText);
            ImGui::SameLine();
            edited |= EditColor("Accent##customAccent", preferences.customAccent);
            if (edited)
            {
                UIManager::Get().ApplyPalette();
                changed = true;
            }
        }

        // the shape: corners, spacing and borders
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextDisabled("Style");
        const auto row = [](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", text);
            ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        };
        bool styled = false;
        row("Corners");
        styled |= ImGui::SliderFloat("##uiRounding", &preferences.uiRounding, 0.0f, 1.0f,
                                     preferences.uiRounding < 0.01f ? "Square" : "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Square to round corners on buttons, fields, panels and tabs");
        row("Spacing");
        constexpr std::array SPACINGS = {"Compact", "Normal", "Roomy"};
        styled |= ImGui::Combo("##uiSpacing", &preferences.uiSpacing, SPACINGS.data(), static_cast<int>(SPACINGS.size()));
        styled |= ImGui::Checkbox("Borders round fields and buttons", &preferences.uiBorders);
        styled |= ImGui::Checkbox("Glass panels", &preferences.uiGlass);
        ImGui::SetItemTooltip("See-through, frosted panels over a wallpaper");
        if (preferences.uiGlass)
        {
            row("Wallpaper");
            const std::string wallpaper = preferences.uiWallpaper.empty()
                                              ? "The palette's own"
                                              : std::filesystem::path(preferences.uiWallpaper).filename().string();
            if (ImGui::BeginCombo("##uiWallpaper", wallpaper.c_str()))
            {
                if (ImGui::Selectable("The palette's own", preferences.uiWallpaper.empty()))
                {
                    preferences.uiWallpaper.clear();
                    changed = true;
                }
                if (ImGui::Selectable("An image file..."))
                {
                    if (const auto path = PathUtils::OpenFileDialog(
                            false, OFN_EXPLORER | OFN_FILEMUSTEXIST,
                            "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.dds\0All Files\0*.*\0", "png"))
                    {
                        preferences.uiWallpaper = path->string();
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("The picture behind the glass panels (PNG, JPG, BMP or DDS)");
            // the liquid gradient's colours, which the editor's own colours come from while it's glass
            row("Gradient");
            bool tinted = EditColor("Glow##uiGlassColor", preferences.uiGlassColor);
            ImGui::SameLine();
            tinted |= EditColor("Deep##uiGlassBase", preferences.uiGlassBase);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##uiGlassReset"))
            {
                preferences.uiGlassColor = 0x000000;
                preferences.uiGlassBase = 0x000000;
                tinted = true;
            }
            if (tinted)
            {
                UIManager::Get().ApplyPalette();
                changed = true;
            }
        }
        if (styled)
        {
            UIManager::Get().ApplyPalette();
            changed = true;
        }

        // the font: Rubik, a Windows one, or a file
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextDisabled("Font");
        row("Font");
        const std::string shown = preferences.uiFont.empty() ? "Alte Haas Grotesk (T6Iris)"
                                  : std::filesystem::path(preferences.uiFont).has_extension()
                                      ? std::filesystem::path(preferences.uiFont).filename().string()
                                      : preferences.uiFont;
        bool fontChanged = false;
        if (ImGui::BeginCombo("##uiFont", shown.c_str()))
        {
            if (ImGui::Selectable("Alte Haas Grotesk (T6Iris)", preferences.uiFont.empty()))
            {
                preferences.uiFont.clear();
                fontChanged = true;
            }
            for (const auto& font : Fonts::ALL)
            {
                const std::string name(font.name);
                if (ImGui::Selectable(name.c_str(), preferences.uiFont == name))
                {
                    preferences.uiFont = name;
                    fontChanged = true;
                }
            }
            if (ImGui::Selectable("A font file..."))
            {
                if (const auto path = PathUtils::OpenFileDialog(false, OFN_EXPLORER | OFN_FILEMUSTEXIST,
                                                                "Fonts\0*.ttf;*.otf\0All Files\0*.*\0", "ttf"))
                {
                    preferences.uiFont = path->string();
                    fontChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The editor's font. Its size is the UI scale's (View menu)");
        if (fontChanged)
        {
            UIManager::Get().ReloadFonts();
            changed = true;
        }

        ImGui::Dummy(ImVec2(0, 4));
        changed |= ImGui::Checkbox("Developer tools", &preferences.developerTools);
        ImGui::SetItemTooltip("The ImGui demo and the debug panel, in the Help menu");
    }

    void Preferences::Render()
    {
        if (!visible)
            return;

        changed = false;
        if (ImGui::Begin("Preferences", &visible, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            DrawMiscSection();
        }
        ImGui::End();

        // once an edit is done (a drag let go, a box ticked), rather than every second while the window is open
        if (changed)
            Configuration::Get().Write(true);
    }

    void Preferences::Release()
    {
    }
}  // namespace IWXMVM::UI