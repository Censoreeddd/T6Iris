#include "StdInclude.hpp"
#include "CameraShake.hpp"

#include "KeyframeManager.hpp"

namespace IWXMVM::Components::CameraShake
{
    namespace
    {
        std::mutex mutex;
        Settings current;

        struct StyleShape
        {
            const char* name;
            glm::vec3 rotation;  // degrees: pitch, yaw, roll
            glm::vec3 position;  // units: forward, right, up
            float frequency;     // Hz
            float bob;           // a walk's step: up and down, and a roll, at the frequency
        };
        constexpr std::array<StyleShape, static_cast<std::size_t>(Style::Count)> STYLES = {{
            {"Handheld", {0.7f, 0.7f, 0.4f}, {0.3f, 0.4f, 0.3f}, 0.8f, 0.0f},
            {"Walking", {0.5f, 0.4f, 0.3f}, {0.2f, 0.4f, 0.3f}, 1.8f, 1.0f},
            {"Running", {1.1f, 0.8f, 0.6f}, {0.4f, 0.8f, 0.6f}, 2.8f, 2.2f},
            {"Vehicle", {0.45f, 0.35f, 0.5f}, {0.3f, 0.3f, 0.7f}, 7.0f, 0.0f},
            {"Earthquake", {1.6f, 1.6f, 2.0f}, {2.5f, 2.5f, 2.5f}, 5.0f, 0.0f},
        }};

        // smooth noise: a gradient per whole number (hashed from it and the seed), eased between, about -1 to 1
        float Gradient(std::int32_t i, std::uint32_t seed)
        {
            std::uint32_t h = (static_cast<std::uint32_t>(i) * 0x9E3779B1u) ^ (seed * 0x85EBCA77u);
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            h *= 0x297A2D39u;
            h ^= h >> 15;
            return static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f;
        }

        float Noise(double t, std::uint32_t seed)
        {
            const auto i = static_cast<std::int32_t>(std::floor(t));
            const auto f = static_cast<float>(t - std::floor(t));
            const float a = Gradient(i, seed) * f;
            const float b = Gradient(i + 1, seed) * (f - 1.0f);
            const float ease = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
            return (a + (b - a) * ease) * 2.0f;
        }

        // three octaves, the finer ones weaker, about -1 to 1
        float Fractal(double t, std::uint32_t seed)
        {
            return (Noise(t, seed) + 0.5f * Noise(t * 2.13, seed + 101) + 0.25f * Noise(t * 4.37, seed + 211)) / 0.875f;
        }

        float Keyed(Types::KeyframeablePropertyType type, float value, double tick)
        {
            auto& keyframeManager = KeyframeManager::Get();
            const auto& property = keyframeManager.GetProperty(type);
            const auto keys = keyframeManager.GetKeyframes(property);
            if (keys.empty())
                return value;
            return keyframeManager.Interpolate(property, keys, static_cast<float>(tick)).floatingPoint;
        }
    }  // namespace

    const char* GetStyleName(Style style)
    {
        const auto index = static_cast<std::size_t>(style);
        return index < STYLES.size() ? STYLES[index].name : "";
    }

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

    Offset Evaluate(const Settings& settings, double tick)
    {
        Offset offset;
        const auto index = static_cast<std::size_t>(settings.style);
        if (!settings.enabled || index >= STYLES.size() || settings.intensity <= 0.0f)
            return offset;
        const auto& style = STYLES[index];
        const double t = tick / 1000.0 * style.frequency * std::max(settings.speed, 0.0f);
        for (int axis = 0; axis < 3; ++axis)
        {
            offset.rotation[axis] = Fractal(t, 17u + static_cast<std::uint32_t>(axis)) * style.rotation[axis];
            offset.position[axis] = Fractal(t, 59u + static_cast<std::uint32_t>(axis)) * style.position[axis];
        }
        // a step's bob: down at each footfall, the roll swaying from one foot to the other
        if (style.bob > 0.0f)
        {
            const double step = t * 3.14159265358979;
            offset.position.z += style.bob * (static_cast<float>(std::abs(std::sin(step))) - 0.64f);
            offset.rotation.z += style.bob * 0.35f * static_cast<float>(std::sin(step * 0.5));
            offset.rotation.x += style.bob * 0.25f * (static_cast<float>(std::abs(std::sin(step))) - 0.64f);
        }
        offset.rotation *= settings.intensity * std::max(settings.rotation, 0.0f);
        offset.position *= settings.intensity * std::max(settings.position, 0.0f);
        return offset;
    }

    Offset Evaluate(double tick)
    {
        auto settings = GetSettings();
        if (!settings.enabled)
            return {};
        using Type = Types::KeyframeablePropertyType;
        settings.intensity = Keyed(Type::CameraShakeIntensity, settings.intensity, tick);
        settings.rotation = Keyed(Type::CameraShakeRotation, settings.rotation, tick);
        settings.position = Keyed(Type::CameraShakePosition, settings.position, tick);
        return Evaluate(settings, tick);
    }

    void Initialize()
    {
        // a key added to a row without keys takes the row's value
        KeyframeManager::Get().AddUnkeyedValues(
            [](Types::KeyframeablePropertyType type) -> std::optional<Types::KeyframeValue> {
                const auto settings = GetSettings();
                switch (type)
                {
                    case Types::KeyframeablePropertyType::CameraShakeIntensity:
                        return Types::KeyframeValue(settings.intensity);
                    case Types::KeyframeablePropertyType::CameraShakeRotation:
                        return Types::KeyframeValue(settings.rotation);
                    case Types::KeyframeablePropertyType::CameraShakePosition:
                        return Types::KeyframeValue(settings.position);
                    default:
                        return std::nullopt;
                }
            });
    }
}  // namespace IWXMVM::Components::CameraShake
