#include "StdInclude.hpp"
#include "Lens.hpp"
#include "Tracking.hpp"

namespace IWXMVM::Components::Lens
{
    namespace
    {
        std::mutex mutex;
        Settings current;
        constexpr float MAX_RENDER_HALF = 1.3962634f;  // 80 degrees

        constexpr std::array<Preset, 9> PRESETS = {{
            {"Fisheye", Projection::Fisheye, 8.0f, 0.0f, 0.35f, 0.3f},
            {"14 mm", Projection::Rectilinear, 14.0f, 0.25f, 0.25f, 0.3f},
            {"18 mm", Projection::Rectilinear, 18.0f, 0.15f, 0.2f, 0.25f},
            {"24 mm", Projection::Rectilinear, 24.0f, 0.08f, 0.15f, 0.2f},
            {"35 mm", Projection::Rectilinear, 35.0f, 0.03f, 0.1f, 0.15f},
            {"50 mm", Projection::Rectilinear, 50.0f, 0.0f, 0.05f, 0.12f},
            {"75 mm", Projection::Rectilinear, 75.0f, -0.03f, 0.05f, 0.1f},
            {"85 mm", Projection::Rectilinear, 85.0f, -0.04f, 0.05f, 0.1f},
            {"135 mm", Projection::Rectilinear, 135.0f, -0.06f, 0.03f, 0.08f},
        }};
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

    std::span<const Preset> GetPresets()
    {
        return PRESETS;
    }

    Settings ApplyPreset(Settings settings, std::size_t preset)
    {
        const auto& p = PRESETS[std::min(preset, PRESETS.size() - 1)];
        settings.projection = p.projection;
        if (p.projection == Projection::Rectilinear)
            settings.focalLength = p.focalLength;
        settings.distortion = p.distortion;
        settings.chromatic = p.chromatic;
        settings.vignette = p.vignette;
        return settings;
    }

    float FovOf(float focalLength)
    {
        return glm::degrees(2.0f * std::atan(18.0f / std::max(focalLength, 1.0f)));
    }

    float EquivalentFocalLength(const Settings& settings)
    {
        if (settings.projection == Projection::Rectilinear)
            return settings.focalLength;
        // an equisolid fisheye's: r = 2 f sin(theta / 2), half the frame (18 mm) at half its view
        return 18.0f / (2.0f * std::sin(glm::radians(std::clamp(settings.fisheyeFov, 60.0f, 220.0f)) / 4.0f));
    }

    std::optional<float> FisheyeAngle(const Settings& settings, glm::vec2 point, float aspect)
    {
        const float unit = settings.circular ? aspect : 1.0f;
        const float half = glm::radians(std::clamp(settings.fisheyeFov, 60.0f, 220.0f)) * 0.5f;
        const float s = glm::length(point) / unit * std::sin(half * 0.5f);
        if (settings.circular && glm::length(point) > unit)
            return std::nullopt;
        if (s >= 1.0f)
            return std::nullopt;
        return 2.0f * std::asin(s);
    }

    std::optional<float> RenderTanHalfX(float aspect)
    {
        const auto settings = GetSettings();
        if (!settings.enabled)
            return std::nullopt;
        if (settings.projection == Projection::Rectilinear)
        {
            if (!settings.setsFov)
                return std::nullopt;
            return 18.0f / std::max(settings.focalLength, 1.0f);
        }
        // the fisheye: wide enough for every point of the picture it shows (to 80 degrees off the middle)
        float needX = 0.01f, needY = 0.01f;
        constexpr int STEPS = 48;
        for (int j = 0; j <= STEPS; ++j)
            for (int i = 0; i <= STEPS; ++i)
            {
                const glm::vec2 point((i * 2.0f / STEPS - 1.0f), (j * 2.0f / STEPS - 1.0f) * aspect);
                const auto angle = FisheyeAngle(settings, point, aspect);
                if (!angle || glm::length(point) <= 0.0f)
                    continue;
                const float t = std::tan(std::min(*angle, MAX_RENDER_HALF));
                const auto direction = point / glm::length(point);
                needX = std::max(needX, t * std::abs(direction.x));
                needY = std::max(needY, t * std::abs(direction.y));
            }
        return std::min(std::max(needX, needY / std::max(aspect, 0.01f)), std::tan(MAX_RENDER_HALF));
    }

    glm::vec2 PictureToRender(glm::vec2 uv)
    {
        const auto settings = GetSettings();
        const bool fisheye = settings.projection == Projection::Fisheye;
        if (!settings.enabled || (!fisheye && std::abs(settings.distortion) < 0.001f))
            return uv;
        const auto view = Tracking::GetView();
        const float aspect = view && view->tanHalfFovX > 0.0f ? view->tanHalfFovY / view->tanHalfFovX : 9.0f / 16.0f;
        const glm::vec2 p((uv.x * 2.0f - 1.0f), (uv.y * 2.0f - 1.0f) * aspect);
        glm::vec2 q;
        if (!fisheye)
        {
            const float bend = std::clamp(settings.distortion, -1.0f, 1.0f) * 0.25f;
            const float a2 = aspect * aspect;
            const float fit = std::max({1.0f + bend, 1.0f + bend * a2, 1.0f + bend * (1.0f + a2), 0.05f});
            q = p * (1.0f + bend * glm::dot(p, p)) / fit;
        }
        else
        {
            const auto angle = FisheyeAngle(settings, p, aspect);
            const float r = glm::length(p);
            if (!angle || r <= 0.0f || !view)
                return uv;
            q = p / r * std::tan(std::min(*angle, 1.5533f)) / std::max(view->tanHalfFovX, 0.01f);
        }
        return {0.5f + 0.5f * q.x, 0.5f + 0.5f * q.y / aspect};
    }
}  // namespace IWXMVM::Components::Lens
