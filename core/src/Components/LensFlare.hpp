#pragma once
#include "StdInclude.hpp"
#include "Types/ScreenTrack.hpp"

// A lens flare of the editor's (Features_LensFlare): drawn over the game's frame from a light, the sun (the game says
// where it is on the picture each frame), a spot of the map picked on the picture or the brightest lights found, made of a glow, rays (a starburst), an
// anamorphic streak, ghosts along the line through the picture's centre and a halo, each its own size, amount and
// colour. Intensity is keyed on the timeline. Hidden behind what's in front of the sun (the depth
// buffer).
namespace IWXMVM::Components::LensFlare
{
    enum class Source
    {
        Sun,
        Tracked,  // a spot of the map picked on the picture (Types::ScreenTrack's point)
        Auto,     // the brightest lights of each frame, found on the picture (as many as autoLights)
    };

    enum class GhostShape
    {
        Round,
        Hexagon,
        Octagon,
    };

    struct Settings
    {
        bool enabled = false;
        Source source = Source::Sun;
        Types::ScreenTrack track;
        // Auto: how many lights at most, how bright a light is (0 to 1), how far apart two are (the picture's height)
        std::int32_t autoLights = 3;
        float autoThreshold = 0.85f;
        float autoSeparation = 0.12f;
        float intensity = 1.0f;
        glm::vec3 color{1.0f, 0.85f, 0.65f};

        float glow = 0.6f;       // amount
        float glowSize = 0.12f;  // as a part of the picture's height

        std::int32_t rays = 8;  // 0 none
        float rayAmount = 0.6f;
        float rayLength = 0.35f;
        float rayRotation = 0.0f;  // degrees
        float raySharpness = 0.6f;  // 0 to 1: thin

        float streak = 0.0f;  // the anamorphic streak, across
        float streakLength = 0.6f;
        glm::vec3 streakColor{0.45f, 0.65f, 1.0f};

        std::int32_t ghosts = 6;  // 0 none
        float ghostAmount = 0.35f;
        float ghostSize = 0.06f;
        float ghostSpacing = 0.3f;
        GhostShape ghostShape = GhostShape::Hexagon;
        float ghostRainbow = 0.5f;  // 0 the flare's colour, 1 each its own hue

        float ghostRing = 0.0f;  // 0 a ghost filled, 1 only its rim (a ring)

        // arcs: crescents of big rings beyond the picture's centre (as an old lens's)
        std::int32_t arcs = 0;
        float arcAmount = 0.4f;
        float arcSize = 0.35f;

        float rayRainbow = 0.0f;  // 0 to 1: each ray its own hue

        // the whole flare turned about its light (degrees), and turning on as the film plays (degrees a second)
        float rotation = 0.0f;
        float spin = 0.0f;
        // a flicker: how much (0 to 1) and how fast (Hz), by the film's time (each auto light its own)
        float flicker = 0.0f;
        float flickerSpeed = 8.0f;

        float halo = 0.0f;
        float haloRadius = 0.35f;
        float haloCenter = 0.5f;  // where its centre is: 0 the light, 1 the picture's centre
        float haloRainbow = 0.7f;

        bool hideBehind = true;  // hidden behind what's in front of the light (the sun)

        // by the light's distance (the depth buffer; the sun and the sky far): lights beyond maxDistance fade out (0:
        // none do), and farther lights' flares are smaller as much as sizeByDistance says (0 to 1)
        float maxDistance = 0.0f;
        float sizeByDistance = 0.0f;
        // drawn before the depth of field, so it blurs the flare as it does what's around the light
        bool blurredByDepthOfField = false;
    };
    Settings GetSettings();
    void SetSettings(const Settings& settings);
    // the settings of a look ("Anamorphic", ...), keeping whether it's on, its source and what it follows
    std::span<const char* const> GetPresetNames();
    Settings ApplyPreset(Settings settings, std::size_t preset);

    // From the game, each frame as it writes the view: where the sun is on the picture (0 to 1 across and down), none
    // while it's behind the camera; and the depth buffer's view (for what's in front of the sun)
    void SetSunPosition(std::optional<glm::vec2> position);
    void SetDepthBuffer(ID3D11ShaderResourceView* const* depth, float zNear);
    // the depth buffer's view and the near plane, for a pick's depth (D3D11::RequestDepthAt)
    std::pair<ID3D11ShaderResourceView* const*, float> GetDepthBuffer();

    // What the frame draws: the settings with their keys at the tick, where the light is (none: no flare) and the
    // depth buffer's view
    struct Frame
    {
        Settings settings;
        std::optional<glm::vec2> light;
        float lightDepth = 1000000.0f;  // the light's distance: a spot's, the sun's far
        ID3D11ShaderResourceView* const* depth = nullptr;
    };
    std::optional<Frame> GetFrame(double tick);

    void Initialize();
}  // namespace IWXMVM::Components::LensFlare
