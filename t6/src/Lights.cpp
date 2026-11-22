#include "StdInclude.hpp"
#include "Lights.hpp"

#include "Addresses.hpp"
#include "Functions.hpp"
#include "Components/Actors.hpp"
#include "Components/LightLogic.hpp"
#include "Components/Lights.hpp"
#include "Components/Playback.hpp"
#include "Mod.hpp"

namespace IWXMVM::T6::Lights
{
    namespace
    {
        namespace LightLogic = Components::LightLogic;

        constexpr float PLACE_AHEAD = 120.0f;

        std::optional<std::uint32_t> sceneGeneration;
        std::vector<Types::Actor> scene;
        std::map<std::uint32_t, LightLogic::Placed> followed;
        bool loggedFirst = false;

        // the linear colour back to the sRGB the game's call takes (it makes it linear again, as LightLogic::ToLinear)
        float ToGamma(float linear)
        {
            linear = std::clamp(linear, 0.0f, 1.0f);
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }

        void AddOmni(const LightLogic::State& state, glm::vec3 position)
        {
            typedef void(__cdecl * R_AddOmniLightToScene_t)(const float* origin, float radius, float r, float g, float b,
                                                            float intensity);
            const auto R_AddOmniLightToScene =
                reinterpret_cast<R_AddOmniLightToScene_t>(GetGameAddresses().R_AddOmniLightToScene());
            // its colour at its brightest channel 1, that channel as the intensity
            const auto brightest = std::max({state.color.r, state.color.g, state.color.b});
            if (brightest <= 0.0f || state.radius <= 0.0f)
                return;
            const auto color = state.color / brightest;
            R_AddOmniLightToScene(&position.x, state.radius, ToGamma(color.r), ToGamma(color.g), ToGamma(color.b),
                                  brightest);
        }
        void AddSpot(const LightLogic::State& state, const LightLogic::Placed& placed)
        {
            typedef void(__cdecl * R_AddSpotLightToScene_t)(const float* origin, const float* axis, float radius,
                                                            float innerFraction, float startRadius, float endRadius,
                                                            float r, float g, float b, float intensity,
                                                            const void* def);
            const auto R_AddSpotLightToScene =
                reinterpret_cast<R_AddSpotLightToScene_t>(GetGameAddresses().R_AddSpotLightToScene());
            const auto brightest = std::max({state.color.r, state.color.g, state.color.b});
            if (brightest <= 0.0f || state.radius <= 0.0f || glm::length(placed.direction) < 0.001f)
                return;
            const auto color = state.color / brightest;

            // its axes: forward along its aim, then its side and up
            const auto forward = glm::normalize(placed.direction);
            auto side = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), forward);
            if (glm::length(side) < 0.001f)
                side = glm::vec3(0.0f, 1.0f, 0.0f);
            side = glm::normalize(side);
            const auto up = glm::cross(forward, side);
            const float axis[9] = {forward.x, forward.y, forward.z, side.x, side.y, side.z, up.x, up.y, up.z};

            const auto halfAngle = glm::radians(std::clamp(state.cone, 2.0f, 170.0f) * 0.5f);
            const auto endRadius = state.radius * std::tan(halfAngle);
            const auto innerFraction = std::clamp(1.0f - state.softness, 0.0f, 0.99f);
            R_AddSpotLightToScene(&placed.position.x, axis, state.radius, innerFraction, 0.0f, endRadius,
                                  ToGamma(color.r), ToGamma(color.g), ToGamma(color.b), brightest, nullptr);
        }
    }  // namespace

    void OnFilmStarted()
    {
        // (the catalog is the props', Props::OnFilmStarted)
        followed.clear();
    }

    void AddToScene()
    {
        // what's put in the scene goes where the line from the camera (or down from it, to drop it to the ground)
        // first hits the map; a light in front of the camera, in the air (core moves it there: see
        // Components::Actors::Place)
        for (const auto& request : Components::Actors::TakePlacementRequests())
        {
            const auto actor = Components::Actors::GetActor(request.id);
            const bool light = actor && actor->light;
            Components::Actors::Place(request, light ? request.origin + glm::normalize(request.direction) * PLACE_AHEAD
                                                     : Functions::FindPlace(request.origin, request.direction,
                                                                            PLACE_AHEAD));
        }

        if (const auto generation = Components::Actors::GetGeneration(); sceneGeneration != generation)
        {
            sceneGeneration = generation;
            scene = Components::Actors::GetActors();
        }
        if (std::ranges::none_of(scene, [](const Types::Actor& actor) { return actor.light.has_value(); }))
            return;

        const auto tick = Components::Playback::GetActorTick();
        const auto timelineTick = Components::Playback::GetTimelineTick();
        const auto keys = Components::Lights::GetKeys();
        for (const auto& sceneActor : scene)
        {
            if (!sceneActor.light || sceneActor.hidden)
                continue;
            const auto found = keys ? keys->find(sceneActor.light->id) : decltype(keys->end()){};
            const auto state = LightLogic::GetState(
                sceneActor, keys && found != keys->end() ? found->second : LightLogic::Keys{}, tick, timelineTick);
            if (!state)
                continue;

            const auto actor = Components::Lights::WithKeyedPlacement(sceneActor, timelineTick);
            auto placed = LightLogic::Place(actor);
            if (const auto& follow = actor.light->follow)
            {
                if (const auto bone = Mod::GetGameInterface()->GetTargetBone(follow->target, follow->bone))
                    placed = followed[actor.light->id] = LightLogic::Place(actor, bone->position, bone->axes);
                else if (const auto last = followed.find(actor.light->id); last != followed.end())
                    placed = last->second;
            }
            if (!loggedFirst)
            {
                loggedFirst = true;
                LOG_DEBUG("T6: adding light {} at ({:.0f}, {:.0f}, {:.0f}), radius {:.0f}", actor.name,
                          placed.position.x, placed.position.y, placed.position.z, state->radius);
            }
            if (state->kind == Types::LightKind::Spot)
                AddSpot(*state, placed);
            else
                AddOmni(*state, placed.position);
        }
    }
}  // namespace IWXMVM::T6::Lights
