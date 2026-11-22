#include "StdInclude.hpp"
#include "Background.hpp"

#include "Mod.hpp"
#include "imgui_internal.h"
#include "UI/Layout.hpp"
#include "UI/Theme.hpp"
#include "Renderer.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

#include <fstream>

namespace IWXMVM::UI
{
    namespace
    {
        // the wallpaper of the glass look, loaded once per file (and let go when it changes)
        struct Wallpaper
        {
            std::string path;
            ImTextureID texture = nullptr;
            ImVec2 size{};
        } wallpaper;

        ImTextureID GetWallpaper(const std::string& path, ImVec2& size)
        {
            if (path != wallpaper.path)
            {
                if (wallpaper.texture)
                    Renderer::ReleaseTexture(wallpaper.texture);
                wallpaper = {path};
                std::ifstream file(path, std::ios::binary);
                const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),
                                                     std::istreambuf_iterator<char>());
                if (!data.empty())
                    wallpaper.texture = Renderer::CreateTextureFromMemory(
                        data.data(), static_cast<std::uint32_t>(data.size()), wallpaper.size);
                if (!wallpaper.texture)
                    LOG_WARN("The wallpaper {} couldn't be loaded", path);
            }
            size = wallpaper.size;
            return wallpaper.texture;
        }

        // the picture to cover a rect, cropped to its shape (as a wallpaper does), as the UVs to draw it with
        std::pair<ImVec2, ImVec2> CoverUVs(ImVec2 picture, ImVec2 area)
        {
            if (picture.x <= 0 || picture.y <= 0 || area.x <= 0 || area.y <= 0)
                return {{0, 0}, {1, 1}};
            const auto pictureAspect = picture.x / picture.y, areaAspect = area.x / area.y;
            if (pictureAspect > areaAspect)
            {
                const auto shown = areaAspect / pictureAspect;
                return {{(1 - shown) / 2, 0}, {(1 + shown) / 2, 1}};
            }
            const auto shown = pictureAspect / areaAspect;
            return {{0, (1 - shown) / 2}, {1, (1 + shown) / 2}};
        }

        // The backdrop the glass sits on: the wallpaper, or the palette's own soft glow
        void DrawBackdrop(ImDrawList* draw, ImVec2 display, const Palettes::Palette& palette, ImTextureID texture,
                          ImVec2 pictureSize, float blur)
        {
            if (texture)
            {
                const auto [uv0, uv1] = CoverUVs(pictureSize, display);
                // a blur: the picture again at offsets round a ring, each averaged in (the nth at 1/n)
                draw->AddImage(texture, ImVec2(0, 0), display, uv0, uv1);
                if (blur > 0)
                {
                    int n = 1;
                    for (const float ring : {0.5f, 1.0f})
                    {
                        for (int k = 0; k < 8; ++k, ++n)
                        {
                            const auto angle = (static_cast<float>(k) + ring) * 3.14159265f / 4.0f;
                            const ImVec2 offset(std::cos(angle) * blur * ring, std::sin(angle) * blur * ring);
                            draw->AddImage(texture, offset, ImVec2(display.x + offset.x, display.y + offset.y), uv0,
                                           uv1, IM_COL32(255, 255, 255, static_cast<int>(255.0f / (n + 1) + 0.5f)));
                        }
                    }
                }
                return;
            }
            // (no wallpaper: the liquid gradient: the deep colour, darker at the foot, and the glow's colour pooling in
            // soft lights that drift slowly, as liquid does; blurred, the lights spread wider and softer)
            const auto& preferences = PreferencesConfiguration::Get();
            const Palettes::Color deep{preferences.uiGlassBase & 0xFFFFFF}, glow{preferences.uiGlassColor & 0xFFFFFF};
            const auto top = Theme::U32(Palettes::Mix(deep, glow, 0.10f));
            const auto bottom = Theme::U32(Palettes::Mix(deep, {0x000000}, 0.35f));
            draw->AddRectFilledMultiColor(ImVec2(0, 0), display, top, top, bottom, bottom);
            const auto time = static_cast<float>(ImGui::GetTime());
            const auto size = std::max(display.x, display.y);
            const auto spread = 1.0f + blur * 0.01f;
            struct Light
            {
                float x, y, radius, alpha, speed;
                Palettes::Color color;
            };
            const std::array<Light, 5> lights = {{
                {0.18f, 0.10f, 0.55f, 0.55f, 0.11f, glow},
                {0.62f, 0.02f, 0.45f, 0.35f, 0.07f, Palettes::Mix(glow, {0xFFFFFF}, 0.25f)},
                {0.95f, 0.85f, 0.60f, 0.45f, 0.09f, Palettes::Mix(glow, {0xFF4FD8}, 0.35f)},
                {0.40f, 0.95f, 0.50f, 0.30f, 0.05f, Palettes::Mix(glow, {0x3FA8FF}, 0.35f)},
                {0.50f, 0.45f, 0.35f, 0.18f, 0.13f, glow},
            }};
            for (std::size_t i = 0; i < lights.size(); ++i)
            {
                const auto& light = lights[i];
                const auto phase = time * light.speed + static_cast<float>(i) * 1.7f;
                const ImVec2 center(display.x * light.x + std::cos(phase) * size * 0.04f,
                                    display.y * light.y + std::sin(phase * 1.3f) * size * 0.03f);
                Theme::DrawGlow(draw, center, size * light.radius * spread, light.color, light.alpha, 0.8f);
            }
        }
    }  // namespace

    void Background::Initialize()
    {
        SetPosition(0, 0);
        SetSize(ImGui::GetIO().DisplaySize);
    }

    void Background::Render()
    {
        const auto display = ImGui::GetIO().DisplaySize;
        SetSize(display);
        ImGui::SetNextWindowPos(GetPosition());
        ImGui::SetNextWindowSize(GetSize());

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::Begin("Background", NULL,
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        auto* draw = window->DrawList;
        draw->PushClipRectFullScreen();
        const auto& palette = Theme::Current();
        const auto& preferences = PreferencesConfiguration::Get();
        const bool glass = preferences.uiGlass;
        const auto fontSize = ImGui::GetFontSize();

        ImTextureID texture = nullptr;
        ImVec2 pictureSize{};
        if (glass)
        {
            if (!preferences.uiWallpaper.empty())
                texture = GetWallpaper(preferences.uiWallpaper, pictureSize);
            DrawBackdrop(draw, display, palette, texture, pictureSize, 0.0f);
        }
        else
        {
            draw->AddRectFilled(ImVec2(0, 0), display, ImGui::GetColorU32(ImVec4(0, 0, 0, 1)));
        }

        // the side panel: its area and the icon rail in the background's colour, and the card the tabs show on,
        // raised a little with a soft shadow under it (glass: the wallpaper frosted under a see-through card, its
        // edge lit)
        const auto area = Layout::GetPanelArea();
        const auto rail = Layout::GetRail();
        const auto card = Layout::GetPanel();
        if (area.GetWidth() > 0 && area.GetHeight() > 0)
        {
            const auto rounding = ImGui::GetStyle().WindowRounding;
            if (!glass)
                draw->AddRectFilled(area.Min, ImVec2(rail.Max.x, area.Max.y), Theme::U32(palette.bg));
            for (int i = 3; i >= 1; --i)
            {
                const auto spread = fontSize * 0.12f * static_cast<float>(i);
                draw->AddRectFilled(ImVec2(card.Min.x - spread, card.Min.y - spread * 0.5f),
                                    ImVec2(card.Max.x + spread, card.Max.y + spread * 1.5f),
                                    Theme::U32({0x000000}, glass ? 0.05f : 0.07f), rounding + spread);
            }
            if (glass)
            {
                // the frost: the backdrop again, blurred, inside the card
                draw->PushClipRect(card.Min, card.Max, true);
                DrawBackdrop(draw, display, palette, texture, pictureSize, texture ? fontSize * 1.1f : 60.0f);
                draw->PopClipRect();
                draw->AddRectFilled(card.Min, card.Max, Theme::U32(palette.bg, 0.40f), rounding);
                // a sheen down from the top, and the lit edge: brighter along the top, as light catches glass
                draw->AddRectFilledMultiColor(card.Min, ImVec2(card.Max.x, card.Min.y + fontSize * 4.0f),
                                              Theme::U32({0xFFFFFF}, 0.06f), Theme::U32({0xFFFFFF}, 0.06f),
                                              Theme::U32({0xFFFFFF}, 0.0f), Theme::U32({0xFFFFFF}, 0.0f));
                draw->AddRect(card.Min, card.Max, Theme::U32({0xFFFFFF}, 0.10f), rounding);
                draw->PushClipRect(card.Min, ImVec2(card.Max.x, card.Min.y + fontSize * 2.0f), true);
                draw->AddRect(card.Min, card.Max, Theme::U32({0xFFFFFF}, 0.16f), rounding);
                draw->PopClipRect();
                // the rail a glass strip of its own
                const ImRect strip(ImVec2(rail.Min.x + fontSize * 0.2f, card.Min.y),
                                   ImVec2(rail.Max.x - fontSize * 0.25f, card.Max.y));
                draw->AddRectFilled(strip.Min, strip.Max, Theme::U32(palette.surface, 0.4f), rounding);
                draw->AddRect(strip.Min, strip.Max, Theme::U32({0xFFFFFF}, 0.10f), rounding);
            }
            else
            {
                draw->AddRectFilled(card.Min, card.Max, Theme::U32(palette.surface), rounding);
                draw->AddRect(card.Min, card.Max, Theme::U32(Palettes::Mix(palette.surface, palette.text, 0.06f)),
                              rounding);
            }
        }

        draw->PopClipRect();
        ImGui::End();
        ImGui::PopStyleColor();
    }

    void Background::Release()
    {
        if (wallpaper.texture)
            Renderer::ReleaseTexture(wallpaper.texture);
        wallpaper = {};
    }
}  // namespace IWXMVM::UI
