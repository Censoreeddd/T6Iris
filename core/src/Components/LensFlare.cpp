#include "StdInclude.hpp"
#include "LensFlare.hpp"

#include "KeyframeManager.hpp"
#include "Tracking.hpp"

namespace IWXMVM::Components::LensFlare
{
    namespace
    {
        std::mutex mutex;
        Settings current;
        std::optional<glm::vec2> sunPosition;
        ID3D11ShaderResourceView* const* depthBuffer = nullptr;
        float depthNear = 4.0f;

        float Keyed(Types::KeyframeablePropertyType type, float value, double tick)
        {
            auto& keyframeManager = KeyframeManager::Get();
            const auto& property = keyframeManager.GetProperty(type);
            const auto keys = keyframeManager.GetKeyframes(property);
            if (keys.empty())
                return value;
            return keyframeManager.Interpolate(property, keys, static_cast<float>(tick)).floatingPoint;
        }

        constexpr std::array<const char*, 10> PRESETS = {"Classic",     "Sun Star",     "Anamorphic",   "Rainbow Halo",
                                                         "Hexagon Ghosts", "Soft Glow",  "Golden Hour",  "Vintage Rings",
                                                         "Arcs",        "Prism"};
    }  // namespace

    Settings GetSettings()
    {
        std::lock_guard lock(mutex);
        return current;
    }

    void SetSettings(const Settings& settings)
    {
        std::lock_guard lock(mutex);
        current = settings;
    }

    std::span<const char* const> GetPresetNames()
    {
        return PRESETS;
    }

    Settings ApplyPreset(Settings settings, std::size_t preset)
    {
        Settings look;
        look.enabled = settings.enabled;
        look.source = settings.source;
        look.track = settings.track;
        look.autoLights = settings.autoLights;
        look.autoThreshold = settings.autoThreshold;
        look.autoSeparation = settings.autoSeparation;
        look.maxDistance = settings.maxDistance;
        look.sizeByDistance = settings.sizeByDistance;
        look.blurredByDepthOfField = settings.blurredByDepthOfField;
        // (the rotation, spin and flicker are how it moves, not its look)
        look.rotation = settings.rotation;
        look.spin = settings.spin;
        look.flicker = settings.flicker;
        look.flickerSpeed = settings.flickerSpeed;
        look.hideBehind = settings.hideBehind;
        switch (preset)
        {
            case 0:  // Classic: a warm glow, a few rays and a line of ghosts
                break;
            case 1:  // Sun Star: a bright star of thin rays
                look.color = {1.0f, 0.95f, 0.85f};
                look.glow = 0.8f;
                look.glowSize = 0.08f;
                look.rays = 8;
                look.rayAmount = 1.0f;
                look.rayLength = 0.6f;
                look.raySharpness = 0.85f;
                look.ghosts = 3;
                look.ghostAmount = 0.15f;
                break;
            case 2:  // Anamorphic: a long blue streak across, oval ghosts
                look.color = {0.55f, 0.7f, 1.0f};
                look.glow = 0.5f;
                look.glowSize = 0.08f;
                look.rays = 0;
                look.streak = 1.0f;
                look.streakLength = 1.2f;
                look.ghosts = 5;
                look.ghostAmount = 0.25f;
                look.ghostShape = GhostShape::Round;
                look.ghostRainbow = 0.1f;
                break;
            case 3:  // Rainbow Halo: a soft warm glow and a rainbow ring
                look.color = {1.0f, 0.75f, 0.45f};
                look.glow = 0.9f;
                look.glowSize = 0.2f;
                look.rays = 0;
                look.ghosts = 2;
                look.ghostAmount = 0.2f;
                look.halo = 0.4f;
                look.haloRadius = 0.3f;
                look.haloCenter = 0.6f;
                look.haloRainbow = 1.0f;
                break;
            case 4:  // Hexagon Ghosts: a row of coloured hexagons
                look.color = {0.85f, 0.8f, 1.0f};
                look.glow = 0.5f;
                look.rays = 6;
                look.rayAmount = 0.4f;
                look.ghosts = 8;
                look.ghostAmount = 0.3f;
                look.ghostSize = 0.07f;
                look.ghostSpacing = 0.22f;
                look.ghostShape = GhostShape::Hexagon;
                look.ghostRainbow = 0.9f;
                break;
            case 5:  // Soft Glow: only a wide glow
                look.glow = 1.0f;
                look.glowSize = 0.3f;
                look.rays = 0;
                look.ghosts = 0;
                break;
            case 6:  // Golden Hour: a warm sun, a red ghost and a green-gold ring around the picture's middle
                look.color = {1.0f, 0.72f, 0.38f};
                look.glow = 1.0f;
                look.glowSize = 0.14f;
                look.rays = 0;
                look.ghosts = 4;
                look.ghostAmount = 0.45f;
                look.ghostSize = 0.05f;
                look.ghostSpacing = 0.35f;
                look.ghostShape = GhostShape::Round;
                look.ghostRainbow = 0.6f;
                look.halo = 0.45f;
                look.haloRadius = 0.18f;
                look.haloCenter = 1.0f;
                look.haloRainbow = 0.6f;
                break;
            case 7:  // Vintage Rings: a reddish light in a ring, rings and discs along the line
                look.color = {1.0f, 0.55f, 0.55f};
                look.glow = 0.9f;
                look.glowSize = 0.18f;
                look.rays = 0;
                look.ghosts = 7;
                look.ghostAmount = 0.35f;
                look.ghostSize = 0.09f;
                look.ghostSpacing = 0.28f;
                look.ghostShape = GhostShape::Round;
                look.ghostRainbow = 0.7f;
                look.ghostRing = 0.5f;
                look.halo = 0.6f;
                look.haloRadius = 0.07f;
                look.haloCenter = 0.0f;
                look.haloRainbow = 0.2f;
                break;
            case 8:  // Arcs: a starburst and crescents beyond the centre
                look.color = {0.95f, 0.88f, 1.0f};
                look.glow = 0.8f;
                look.glowSize = 0.1f;
                look.rays = 12;
                look.rayAmount = 0.5f;
                look.rayLength = 0.45f;
                look.raySharpness = 0.45f;
                look.ghosts = 5;
                look.ghostAmount = 0.2f;
                look.ghostShape = GhostShape::Round;
                look.ghostRainbow = 0.3f;
                look.arcs = 3;
                look.arcAmount = 0.6f;
                look.arcSize = 0.3f;
                break;
            default:  // Prism: a rainbow ring around the light and rainbow rays
                look.color = {1.0f, 0.82f, 0.6f};
                look.glow = 0.9f;
                look.glowSize = 0.12f;
                look.rays = 14;
                look.rayAmount = 0.45f;
                look.rayLength = 0.9f;
                look.raySharpness = 0.55f;
                look.rayRainbow = 0.8f;
                look.ghosts = 0;
                look.halo = 0.7f;
                look.haloRadius = 0.22f;
                look.haloCenter = 0.0f;
                look.haloRainbow = 1.0f;
                break;
        }
        return look;
    }

    void SetSunPosition(std::optional<glm::vec2> position)
    {
        std::lock_guard lock(mutex);
        sunPosition = position;
    }

    void SetDepthBuffer(ID3D11ShaderResourceView* const* depth, float zNear)
    {
        std::lock_guard lock(mutex);
        depthBuffer = depth;
        depthNear = zNear;
    }

    std::pair<ID3D11ShaderResourceView* const*, float> GetDepthBuffer()
    {
        std::lock_guard lock(mutex);
        return {depthBuffer, depthNear};
    }

    std::optional<Frame> GetFrame(double tick)
    {
        Frame frame;
        {
            std::lock_guard lock(mutex);
            frame.settings = current;
            frame.light = sunPosition;
            frame.depth = depthBuffer;
        }
        if (!frame.settings.enabled)
            return std::nullopt;
        using Type = Types::KeyframeablePropertyType;
        frame.settings.intensity = Keyed(Type::LensFlareIntensity, frame.settings.intensity, tick);
        // (the lights found are on the picture: nothing hides them; a spot picked is hidden by what's in front of it)
        if (frame.settings.source == Source::Auto)
            frame.settings.hideBehind = false;
        // (Auto's lights are found on the picture as it's drawn)
        if (frame.settings.source == Source::Auto)
            frame.light = glm::vec2(0.5f);
        if (frame.settings.source == Source::Tracked)
        {
            // (a spot of the map only: a flare doesn't follow players)
            if (frame.settings.track.kind != Types::TrackKind::Point)
                return std::nullopt;
            const auto projected = Tracking::Project(frame.settings.track);
            if (!projected)
                return std::nullopt;
            frame.light = projected->uv;
            frame.lightDepth = projected->depth;
        }
        if (!frame.light || frame.settings.intensity <= 0.0f)
            return std::nullopt;
        return frame;
    }

    void Initialize()
    {
        KeyframeManager::Get().AddUnkeyedValues(
            [](Types::KeyframeablePropertyType type) -> std::optional<Types::KeyframeValue> {
                const auto settings = GetSettings();
                switch (type)
                {
                    case Types::KeyframeablePropertyType::LensFlareIntensity:
                        return Types::KeyframeValue(settings.intensity);
                    default:
                        return std::nullopt;
                }
            });
    }
}  // namespace IWXMVM::Components::LensFlare
