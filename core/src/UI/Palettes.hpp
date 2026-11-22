#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The palettes of the launcher and the mod's own UI: neutral studio greys, each a set of named colors that both map
// onto ImGui the same way, so picking one recolors both. Mono, the user's pick, is the
// default. The launcher (launcher/) includes this file too, so it stays free of ImGui and the game.
namespace IWXMVM::UI::Palettes
{
    // a color as 0xRRGGBB
    struct Color
    {
        std::uint32_t rgb = 0;

        constexpr std::uint8_t R() const
        {
            return static_cast<std::uint8_t>(rgb >> 16);
        }
        constexpr std::uint8_t G() const
        {
            return static_cast<std::uint8_t>(rgb >> 8);
        }
        constexpr std::uint8_t B() const
        {
            return static_cast<std::uint8_t>(rgb);
        }
        // as ImGui's IM_COL32 packs it (A B G R from the top byte down), with an alpha of 0-255
        constexpr std::uint32_t Abgr(std::uint8_t alpha = 255) const
        {
            return static_cast<std::uint32_t>(alpha) << 24 | static_cast<std::uint32_t>(B()) << 16 |
                   static_cast<std::uint32_t>(G()) << 8 | R();
        }
        constexpr bool operator==(const Color&) const = default;
    };

    // this color moved toward another by an amount from 0 to 1, each channel rounded
    constexpr Color Mix(Color from, Color to, float amount)
    {
        auto channel = [amount](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint32_t>(a + (static_cast<float>(b) - a) * amount + 0.5f);
        };
        return {channel(from.R(), to.R()) << 16 | channel(from.G(), to.G()) << 8 | channel(from.B(), to.B())};
    }

    struct Palette
    {
        std::string_view id;    // as preferences.palette and launcher.json keep it
        std::string_view name;  // as the pickers show it
        Color bg;               // window backgrounds
        Color surface;          // panels, child windows, frames
        Color raised;           // buttons, headers, tabs
        Color border;           // borders, separators
        Color text;
        Color muted;     // disabled text and hints
        Color accent;    // the primary button, selection, sliders, check marks, the active tab
        Color onAccent;  // text on accent-colored buttons
        Color ok;
        Color warn;
        Color bad;

        // the accent under the mouse and held down
        constexpr Color AccentHover() const
        {
            return Mix(accent, {0xFFFFFF}, 0.12f);
        }
        constexpr Color AccentActive() const
        {
            return Mix(accent, {0x000000}, 0.12f);
        }
    };

    // as the mockups have them (the first five; then colors of their own, and one light); warn, which the launcher
    // doesn't draw, a muted amber
    inline constexpr std::array<Palette, 13> ALL = {{
        {"graphite", "Graphite", {0x1E1E1E}, {0x252525}, {0x303030}, {0x3A3A3A}, {0xD6D6D6}, {0x8F8F8F}, {0x2F80D1},
         {0xFFFFFF}, {0x5AAE7A}, {0xD4AA55}, {0xD9534F}},
        {"workshop", "Workshop", {0x2B2B2B}, {0x333333}, {0x3F3F3F}, {0x222222}, {0xE2E2E2}, {0x9C9C9C}, {0xE08A33},
         {0x1C1206}, {0x6DB37F}, {0xD9B45A}, {0xE06055}},
        {"steel", "Steel", {0x23262A}, {0x2B2F34}, {0x363B41}, {0x41474E}, {0xD9DDE2}, {0x939AA3}, {0x6E98C4},
         {0x0E1620}, {0x6DB38A}, {0xD4AA55}, {0xD9625A}},
        {"mono", "Mono", {0x1C1C1C}, {0x242424}, {0x2E2E2E}, {0x383838}, {0xD6D6D6}, {0x8C8C8C}, {0xCFCFCF},
         {0x161616}, {0x7FB58F}, {0xD4AA55}, {0xD46A62}},
        {"sage", "Sage", {0x202221}, {0x282A29}, {0x323534}, {0x3D413F}, {0xD7DBD8}, {0x909893}, {0x7FA58A},
         {0x0F1712}, {0x7FA58A}, {0xD4AA55}, {0xD46A62}},
        {"midnight", "Midnight", {0x14161F}, {0x1B1E2A}, {0x252938}, {0x2E3345}, {0xD8DCEB}, {0x868CA3}, {0x7A8CFF},
         {0x0B0E1A}, {0x6FBF8E}, {0xD9B45A}, {0xE0666A}},
        {"iris", "Iris", {0x1B1820}, {0x221E29}, {0x2C2735}, {0x373042}, {0xE0DBE8}, {0x958DA3}, {0xA67CF0},
         {0x140B22}, {0x7FBF95}, {0xD9B45A}, {0xE0666A}},
        {"ocean", "Ocean", {0x161E22}, {0x1C262B}, {0x253238}, {0x2F3E45}, {0xD6E4E8}, {0x86999F}, {0x2FB5C4},
         {0x061518}, {0x6FBF8E}, {0xD9B45A}, {0xE0666A}},
        {"forest", "Forest", {0x171D18}, {0x1E251F}, {0x283129}, {0x323D33}, {0xD9E3DA}, {0x8C9A8D}, {0x5FBF6A},
         {0x07140A}, {0x5FBF6A}, {0xD9B45A}, {0xE0666A}},
        {"ember", "Ember", {0x1E1A18}, {0x26211E}, {0x322B27}, {0x3D3530}, {0xE6DED8}, {0x9A8F87}, {0xE8663C},
         {0x1A0B05}, {0x7FB58F}, {0xD9B45A}, {0xE0666A}},
        {"crimson", "Crimson", {0x1C1819}, {0x241F20}, {0x2F2829}, {0x3A3132}, {0xE4DCDC}, {0x9A8E8F}, {0xD9434F},
         {0xFFFFFF}, {0x7FB58F}, {0xD9B45A}, {0xF08A4B}},
        {"neon", "Neon", {0x121214}, {0x19191D}, {0x222228}, {0x2C2C34}, {0xE8E8F0}, {0x8E8E9C}, {0xFF3DAA},
         {0x1A0412}, {0x5FE0A0}, {0xF0C850}, {0xFF5A5A}},
        {"daylight", "Daylight", {0xE9EAEC}, {0xF5F5F6}, {0xDADCDF}, {0xC4C7CC}, {0x1E2024}, {0x6E737B}, {0x2F80D1},
         {0xFFFFFF}, {0x2E8B57}, {0xB8860B}, {0xC0392B}},
    }};

    // the user's own palette (preferences.palette "custom"): from a background, a text and an accent color, the rest
    // mixed from them
    inline constexpr std::string_view CUSTOM_ID = "custom";
    constexpr Palette MakeCustom(Color bg, Color text, Color accent)
    {
        const bool lightAccent = accent.R() * 0.299f + accent.G() * 0.587f + accent.B() * 0.114f > 150.0f;
        return {CUSTOM_ID,
                "Custom",
                bg,
                Mix(bg, text, 0.04f),
                Mix(bg, text, 0.09f),
                Mix(bg, text, 0.14f),
                text,
                Mix(text, bg, 0.42f),
                accent,
                lightAccent ? Color{0x111111} : Color{0xFFFFFF},
                {0x7FB58F},
                {0xD9B45A},
                {0xE0666A}};
    }

    inline constexpr std::string_view DEFAULT_ID = "mono";

    // the palette of an id; none for an id that isn't one
    constexpr std::optional<std::size_t> Find(std::string_view id)
    {
        for (std::size_t i = 0; i < ALL.size(); ++i)
            if (ALL[i].id == id)
                return i;
        return std::nullopt;
    }

    // the palette of an id, the default for one that isn't one
    constexpr const Palette& Get(std::string_view id)
    {
        return ALL[Find(id).value_or(*Find(DEFAULT_ID))];
    }
}  // namespace IWXMVM::UI::Palettes
