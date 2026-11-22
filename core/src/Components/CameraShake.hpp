#pragma once
#include "StdInclude.hpp"

// A camera shake of the editor's (Features_CameraShake): smooth noise added to the view's angles and position each
// frame, by the film's time, so a recording (and each of a shutter's frames) shakes the same every time. A style picks
// how it moves (a handheld camera's slow drift, a walk's bob, a vehicle's rumble...); Intensity, Rotation and Position
// are keyed on the timeline (an Intensity key to 0 and up again makes a hit), Speed and the style aren't (a keyed speed
// would make the noise jump).
namespace IWXMVM::Components::CameraShake
{
    enum class Style
    {
        Handheld,
        Walking,
        Running,
        Vehicle,
        Earthquake,
        Count
    };
    const char* GetStyleName(Style style);

    struct Settings
    {
        bool enabled = false;
        Style style = Style::Handheld;
        float intensity = 1.0f;  // times the style's
        float speed = 1.0f;      // times the style's
        float rotation = 1.0f;   // how much it turns, times the style's
        float position = 1.0f;   // how much it moves, times the style's
    };
    Settings GetSettings();
    void SetSettings(const Settings& settings);

    // The shake at a time of the film (ms): degrees of pitch, yaw and roll, and units forward, right and up; none while
    // it's off. Intensity, Rotation and Position take their keys there. Any thread.
    struct Offset
    {
        glm::vec3 rotation{};
        glm::vec3 position{};
    };
    Offset Evaluate(double tick);
    Offset Evaluate(const Settings& settings, double tick);  // (without keys)

    void Initialize();
}  // namespace IWXMVM::Components::CameraShake
