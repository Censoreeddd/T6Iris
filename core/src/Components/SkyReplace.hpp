#pragma once
#include "StdInclude.hpp"

// A sky of the editor's in place of the map's (Features_SkyReplace): drawn over the frame where the depth buffer has
// nothing (the sky), from an image (a 360 degree panorama, or a photo wrapped round from the horizon up) or a gradient
// of colours, turned with the camera as the map's sky is (the view Components::Tracking has). Before the depth of field
// and the lens flares, so they take it as the sky.
namespace IWXMVM::Components::SkyReplace
{
    enum class Kind
    {
        Image,
        Gradient,
    };

    enum class Mapping
    {
        Panorama,  // 360 by 180 degrees (equirectangular)
        Photo,     // its bottom at the horizon, photoWidth degrees across, mirrored round
    };

    struct Settings
    {
        bool enabled = false;
        Kind kind = Kind::Image;
        std::string imagePath;
        Mapping mapping = Mapping::Panorama;
        float rotation = 0.0f;    // degrees, about the up axis
        float drift = 0.0f;       // degrees a second of the film, as clouds move
        float horizon = 0.0f;     // degrees: the image's horizon raised (or lowered)
        float photoWidth = 120.0f;  // Photo: degrees across
        float exposure = 0.0f;      // an HDRI's (.hdr, .exr), in stops: brighter up

        float brightness = 1.0f;
        float saturation = 1.0f;
        glm::vec3 tint{1.0f};
        float opacity = 1.0f;  // 0 the map's sky, 1 this one
        float feather = 1.5f;  // pixels: how softly it meets what's in front of it

        // The map lit to match it: taking the colour of its light, its brightness, and haze of it toward the distance
        bool matchLighting = false;
        float lightColor = 0.6f;       // 0 the map's own colour, 1 the sky's light's
        float lightBrightness = 0.4f;  // 0 the map's own brightness, 1 the sky's (darker under a night sky)
        float haze = 0.0f;             // how much the far is the sky's colour
        float hazeDistance = 6000.0f;  // units: how far for most of it
        float hazeStart = 500.0f;      // units: none nearer

        // Gradient: the colours at the top, at the horizon and below it, and how soft the horizon is
        glm::vec3 top{0.12f, 0.28f, 0.62f};
        glm::vec3 horizonColor{0.92f, 0.68f, 0.48f};
        glm::vec3 bottom{0.32f, 0.26f, 0.26f};
        float horizonSoftness = 0.2f;
    };
    Settings GetSettings();
    void SetSettings(const Settings& settings);
    std::span<const char* const> GetGradientNames();
    Settings ApplyGradient(Settings settings, std::size_t gradient);
}  // namespace IWXMVM::Components::SkyReplace
