#include "StdInclude.hpp"
#include "LensDirt.hpp"

namespace IWXMVM::Components::LensDirt
{
    namespace
    {
        std::mutex mutex;
        Settings current;
    }  // namespace

    const char* GetStyleName(Style style)
    {
        switch (style)
        {
            case Style::Dust:
                return "Dust";
            case Style::Smudges:
                return "Smudges";
            case Style::Grime:
                return "Dust and Smudges";
            case Style::WaterDrops:
                return "Water Drops";
            case Style::Image:
                return "An Image";
            default:
                return "?";
        }
    }

    const char* GetBlendName(Blend blend)
    {
        switch (blend)
        {
            case Blend::Add:
                return "Add";
            case Blend::Screen:
                return "Screen";
            case Blend::Lighten:
                return "Lighten";
            case Blend::Overlay:
                return "Overlay";
            case Blend::SoftLight:
                return "Soft Light";
            case Blend::ColorDodge:
                return "Color Dodge";
            case Blend::Multiply:
                return "Multiply";
            default:
                return "?";
        }
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
}  // namespace IWXMVM::Components::LensDirt
