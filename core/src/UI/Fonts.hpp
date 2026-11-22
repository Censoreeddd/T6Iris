#pragma once

#include <array>
#include <string_view>

// The fonts the editor can use besides its own (Rubik): fonts every Windows has, by their files in its Fonts folder
// (regular, bold). Preferences keep the name; a .ttf / .otf file of the user's is kept by its path instead.
namespace IWXMVM::UI::Fonts
{
    struct Font
    {
        std::string_view name;
        std::string_view regular;
        std::string_view bold;
    };

    inline constexpr std::array<Font, 9> ALL = {{
        {"Segoe UI", "segoeui.ttf", "segoeuib.ttf"},
        {"Bahnschrift", "bahnschrift.ttf", "bahnschrift.ttf"},
        {"Arial", "arial.ttf", "arialbd.ttf"},
        {"Verdana", "verdana.ttf", "verdanab.ttf"},
        {"Tahoma", "tahoma.ttf", "tahomabd.ttf"},
        {"Calibri", "calibri.ttf", "calibrib.ttf"},
        {"Trebuchet MS", "trebuc.ttf", "trebucbd.ttf"},
        {"Consolas", "consola.ttf", "consolab.ttf"},
        {"Cascadia Code", "CascadiaCode.ttf", "CascadiaCode.ttf"},
    }};
}  // namespace IWXMVM::UI::Fonts
