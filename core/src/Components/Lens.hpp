#pragma once
#include "StdInclude.hpp"

// The camera's lens (Features_LensFlare): a focal length (on a full frame, 36 mm across) that sets the field of view
// of the editor's cameras, the way it bends the picture (a rectilinear lens's barrel or pincushion, or a fisheye's,
// from a wider view the game draws) and its colour fringes and darkened corners. The bend, fringes and corners are
// drawn last over the frame (D3D11), after the depth of field, the flare and the dirt, so they bend with it.
namespace IWXMVM::Components::Lens
{
    enum class Projection
    {
        Rectilinear,
        Fisheye,  // equisolid
    };

    struct Settings
    {
        bool enabled = false;
        Projection projection = Projection::Rectilinear;
        float focalLength = 35.0f;  // mm (Rectilinear)
        bool setsFov = true;        // Rectilinear: the field of view from the focal length (else the camera's)
        float fisheyeFov = 130.0f;  // degrees across the picture (or the circle, Circular): to 130 it fills the corners
        bool circular = false;      // Fisheye: the whole circle in the picture's height, black round it
        float distortion = 0.0f;    // Rectilinear: -1 pincushion to 1 barrel
        float chromatic = 0.0f;     // 0 to 1: colour fringes toward the edges
        float vignette = 0.0f;      // 0 to 1: corners darker
        bool depthOfField = true;   // the depth of field's focal length this lens's
    };
    Settings GetSettings();
    void SetSettings(const Settings& settings);

    struct Preset
    {
        const char* name;
        Projection projection;
        float focalLength;
        float distortion;
        float chromatic;
        float vignette;
    };
    std::span<const Preset> GetPresets();
    Settings ApplyPreset(Settings settings, std::size_t preset);

    // the horizontal field of view (degrees) of a rectilinear lens of this focal length
    float FovOf(float focalLength);
    // the focal length (mm) the depth of field takes for these settings (a fisheye's by its field of view)
    float EquivalentFocalLength(const Settings& settings);

    // What the game draws for the lens, tan(half the view across), for a picture aspect (its height / width): the
    // focal length's, or what a fisheye needs to cover the picture (at most 160 degrees); none for the camera's own
    std::optional<float> RenderTanHalfX(float aspect);
    // The fisheye's angle (radians) at a point of the picture, in units of half its width (y in the same units), or
    // none outside what it shows (beyond the circle)
    std::optional<float> FisheyeAngle(const Settings& settings, glm::vec2 point, float aspect);

    // A point of the picture as shown (bent by the lens) to where it is in the picture the game drew (what the depth,
    // the view's projections and the effects before the lens use), as the lens's shader reads it; unchanged without a
    // bend
    glm::vec2 PictureToRender(glm::vec2 uv);
}  // namespace IWXMVM::Components::Lens
