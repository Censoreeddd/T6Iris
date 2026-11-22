#pragma once
#include "StdInclude.hpp"

// Dirt on the lens (Features_LensFlare): smudges, specks of dust or drops of water, or an image of them, seen only where
// light catches them, as a real lens's are: by the bright parts of the picture (from a blurred copy of it, so a light
// lights the dirt around it) and by the lens flare's light (none while it's hidden behind something). Added to the
// frame after the depth of field and the lens flare, before the shutter.
namespace IWXMVM::Components::LensDirt
{
    enum class Style
    {
        Dust,
        Smudges,
        Grime,       // dust and smudges together
        WaterDrops,
        Image,
        Count,
    };
    const char* GetStyleName(Style style);

    // how it's put over the picture
    enum class Blend
    {
        Add,
        Screen,
        Lighten,
        Overlay,
        SoftLight,
        ColorDodge,
        Multiply,  // dark where it's lit
        Count,
    };
    const char* GetBlendName(Blend blend);

    struct Settings
    {
        bool enabled = false;
        Style style = Style::Grime;
        std::string imagePath;  // Image: dirt, light on black (as dirt textures are)
        float imageRotation = 0.0f;  // Image: degrees, about the picture's centre
        float imageScale = 1.0f;     // Image: times the size that fills the picture
        Blend blend = Blend::Add;
        float amount = 1.0f;     // how bright the dirt is where it's lit
        float threshold = 0.6f;  // 0 to 1: how bright the picture is before it lights the dirt
        float spread = 0.5f;     // 0 to 1: how far from a bright spot the dirt is lit
        float density = 0.5f;    // 0 to 1: how much dirt
        float size = 1.0f;       // the specks' and drops' size
        float softness = 0.5f;   // 0 to 1: how blurred the dirt is (out of focus)
        std::int32_t seed = 1;   // another scattering of the same dirt
        glm::vec3 tint{1.0f};
        float flareLight = 1.0f;   // how much the lens flare's light lights it (0: not)
        float flareSpread = 0.5f;  // how far from the flare's light (a part of the picture's height)
    };
    Settings GetSettings();
    void SetSettings(const Settings& settings);
}  // namespace IWXMVM::Components::LensDirt
