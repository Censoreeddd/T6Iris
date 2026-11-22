#include "StdInclude.hpp"
#include "ProjectEffects.hpp"

#include "CameraShake.hpp"
#include "Lens.hpp"
#include "LensDirt.hpp"
#include "LensFlare.hpp"
#include "SkyReplace.hpp"
#include "UI/UIManager.hpp"
#include "UI/Components/VisualsMenu.hpp"

namespace IWXMVM::Components::ProjectEffects
{
    namespace
    {
        using json = nlohmann::json;

        // One field written to or read from an object: numbers, switches and strings as they are, colours as three
        // numbers, kinds by name (one a build doesn't know read as nothing: the field kept)
        template <typename T>
        void Field(json& j, bool reading, const char* key, T& value)
        {
            if (!reading)
            {
                if constexpr (std::is_enum_v<T>)
                    j[key] = std::string(magic_enum::enum_name(value));
                else if constexpr (std::is_same_v<T, glm::vec3>)
                    j[key] = {value.x, value.y, value.z};
                else
                    j[key] = value;
                return;
            }
            if (!j.is_object() || !j.contains(key))
                return;
            const auto& node = j[key];
            try
            {
                if constexpr (std::is_enum_v<T>)
                {
                    if (const auto read = magic_enum::enum_cast<T>(node.get<std::string>()))
                        value = *read;
                }
                else if constexpr (std::is_same_v<T, glm::vec3>)
                {
                    if (node.is_array() && node.size() == 3)
                        value = {node[0].get<float>(), node[1].get<float>(), node[2].get<float>()};
                }
                else
                {
                    value = node.get<T>();
                }
            }
            catch (const std::exception&)
            {
            }
        }
#define FIELD(name) Field(j, reading, #name, s.name)

        void LensFields(json& j, bool reading, Components::Lens::Settings& s)
        {
            FIELD(enabled);
            FIELD(projection);
            FIELD(focalLength);
            FIELD(setsFov);
            FIELD(fisheyeFov);
            FIELD(circular);
            FIELD(distortion);
            FIELD(chromatic);
            FIELD(vignette);
            FIELD(depthOfField);
        }

        void ShakeFields(json& j, bool reading, CameraShake::Settings& s)
        {
            FIELD(enabled);
            FIELD(style);
            FIELD(intensity);
            FIELD(speed);
            FIELD(rotation);
            FIELD(position);
        }

        void FlareFields(json& j, bool reading, LensFlare::Settings& s)
        {
            FIELD(enabled);
            FIELD(source);
            FIELD(autoLights);
            FIELD(autoThreshold);
            FIELD(autoSeparation);
            FIELD(intensity);
            FIELD(color);
            FIELD(glow);
            FIELD(glowSize);
            FIELD(rays);
            FIELD(rayAmount);
            FIELD(rayLength);
            FIELD(rayRotation);
            FIELD(raySharpness);
            FIELD(streak);
            FIELD(streakLength);
            FIELD(streakColor);
            FIELD(ghosts);
            FIELD(ghostAmount);
            FIELD(ghostSize);
            FIELD(ghostSpacing);
            FIELD(ghostShape);
            FIELD(ghostRainbow);
            FIELD(ghostRing);
            FIELD(arcs);
            FIELD(arcAmount);
            FIELD(arcSize);
            FIELD(rayRainbow);
            FIELD(rotation);
            FIELD(spin);
            FIELD(flicker);
            FIELD(flickerSpeed);
            FIELD(halo);
            FIELD(haloRadius);
            FIELD(haloCenter);
            FIELD(haloRainbow);
            FIELD(hideBehind);
            FIELD(maxDistance);
            FIELD(sizeByDistance);
            FIELD(blurredByDepthOfField);
        }

        void DirtFields(json& j, bool reading, LensDirt::Settings& s)
        {
            FIELD(enabled);
            FIELD(style);
            FIELD(imagePath);
            FIELD(imageRotation);
            FIELD(imageScale);
            FIELD(blend);
            FIELD(amount);
            FIELD(threshold);
            FIELD(spread);
            FIELD(density);
            FIELD(size);
            FIELD(softness);
            FIELD(seed);
            FIELD(tint);
            FIELD(flareLight);
            FIELD(flareSpread);
        }

        void SkyFields(json& j, bool reading, SkyReplace::Settings& s)
        {
            FIELD(enabled);
            FIELD(kind);
            FIELD(imagePath);
            FIELD(mapping);
            FIELD(rotation);
            FIELD(drift);
            FIELD(horizon);
            FIELD(photoWidth);
            FIELD(exposure);
            FIELD(brightness);
            FIELD(saturation);
            FIELD(tint);
            FIELD(opacity);
            FIELD(feather);
            FIELD(matchLighting);
            FIELD(lightColor);
            FIELD(lightBrightness);
            FIELD(haze);
            FIELD(hazeDistance);
            FIELD(hazeStart);
            FIELD(top);
            FIELD(horizonColor);
            FIELD(bottom);
            FIELD(horizonSoftness);
        }
#undef FIELD

        UI::VisualsMenu* GetVisualsMenu()
        {
            return UI::UIManager::Get().GetUIComponent<UI::VisualsMenu>(UI::Component::VisualsMenu);
        }
    }  // namespace

    nlohmann::json Write()
    {
        json effects = json::object();
        {
            auto s = Components::Lens::GetSettings();
            LensFields(effects["lens"], false, s);
        }
        {
            auto s = CameraShake::GetSettings();
            ShakeFields(effects["cameraShake"], false, s);
        }
        {
            auto s = LensFlare::GetSettings();
            FlareFields(effects["lensFlare"], false, s);
        }
        {
            auto s = LensDirt::GetSettings();
            DirtFields(effects["lensDirt"], false, s);
        }
        {
            auto s = SkyReplace::GetSettings();
            SkyFields(effects["sky"], false, s);
        }
        if (const auto* visuals = GetVisualsMenu())
            effects["depthOfField"] = visuals->WriteDepthOfField();
        return effects;
    }

    void Read(const nlohmann::json& saved)
    {
        if (!saved.is_object())
            return;
        auto effects = saved;
        if (effects.contains("lens"))
        {
            auto s = Components::Lens::GetSettings();
            LensFields(effects["lens"], true, s);
            Components::Lens::SetSettings(s);
        }
        if (effects.contains("cameraShake"))
        {
            auto s = CameraShake::GetSettings();
            ShakeFields(effects["cameraShake"], true, s);
            CameraShake::SetSettings(s);
        }
        if (effects.contains("lensFlare"))
        {
            auto s = LensFlare::GetSettings();
            FlareFields(effects["lensFlare"], true, s);
            LensFlare::SetSettings(s);
        }
        if (effects.contains("lensDirt"))
        {
            auto s = LensDirt::GetSettings();
            DirtFields(effects["lensDirt"], true, s);
            LensDirt::SetSettings(s);
        }
        if (effects.contains("sky"))
        {
            auto s = SkyReplace::GetSettings();
            SkyFields(effects["sky"], true, s);
            SkyReplace::SetSettings(s);
        }
        if (effects.contains("depthOfField"))
            if (auto* visuals = GetVisualsMenu())
                visuals->ReadDepthOfField(effects["depthOfField"]);
    }
}  // namespace IWXMVM::Components::ProjectEffects
