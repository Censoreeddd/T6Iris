#include "StdInclude.hpp"
#include "SkyReplace.hpp"

namespace IWXMVM::Components::SkyReplace
{
    namespace
    {
        std::mutex mutex;
        Settings current;
        constexpr std::array<const char*, 5> GRADIENTS = {"Clear Day", "Sunset", "Dusk", "Night", "Overcast"};
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

    std::span<const char* const> GetGradientNames()
    {
        return GRADIENTS;
    }

    Settings ApplyGradient(Settings settings, std::size_t gradient)
    {
        settings.kind = Kind::Gradient;
        switch (gradient)
        {
            case 0:
                settings.top = {0.16f, 0.38f, 0.78f};
                settings.horizonColor = {0.72f, 0.84f, 0.96f};
                settings.bottom = {0.45f, 0.5f, 0.55f};
                settings.horizonSoftness = 0.35f;
                break;
            case 1:
                settings.top = {0.12f, 0.2f, 0.45f};
                settings.horizonColor = {1.0f, 0.55f, 0.25f};
                settings.bottom = {0.35f, 0.18f, 0.12f};
                settings.horizonSoftness = 0.18f;
                break;
            case 2:
                settings.top = {0.05f, 0.06f, 0.18f};
                settings.horizonColor = {0.62f, 0.32f, 0.45f};
                settings.bottom = {0.12f, 0.08f, 0.12f};
                settings.horizonSoftness = 0.22f;
                break;
            case 3:
                settings.top = {0.01f, 0.015f, 0.04f};
                settings.horizonColor = {0.06f, 0.08f, 0.16f};
                settings.bottom = {0.02f, 0.02f, 0.03f};
                settings.horizonSoftness = 0.3f;
                break;
            default:
                settings.top = {0.55f, 0.58f, 0.62f};
                settings.horizonColor = {0.75f, 0.76f, 0.78f};
                settings.bottom = {0.4f, 0.41f, 0.43f};
                settings.horizonSoftness = 0.5f;
                break;
        }
        return settings;
    }
}  // namespace IWXMVM::Components::SkyReplace
