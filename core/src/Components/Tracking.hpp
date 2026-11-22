#pragma once
#include "StdInclude.hpp"
#include "Types/ScreenTrack.hpp"
#include "nlohmann/json.hpp"

// Where the things screen effects follow (Types::ScreenTrack) are on the picture: the game gives the view it wrote
// last (Features_Tracking), from which a bone's or a point's place in the world is projected, and a picked spot of the
// picture with its depth taken back into the world.
namespace IWXMVM::Components::Tracking
{
    struct View
    {
        glm::vec3 position{0.0f};
        glm::mat3 axis{1.0f};  // forward, left, up
        float tanHalfFovX = 1.0f;
        float tanHalfFovY = 1.0f;
    };
    // From the game, as it writes the view (each write, the last of a frame counting). Any thread.
    void SetView(const View& view);
    std::optional<View> GetView();

    // where it is in the world now: none for nothing tracked, or a target not there or without the bone
    std::optional<glm::vec3> WorldPosition(const Types::ScreenTrack& track);

    // a place in the world on the picture: 0 to 1 across and down, and its depth (along the view, the game's units);
    // none behind the camera
    struct Projected
    {
        glm::vec2 uv{0.5f};
        float depth = 0.0f;
    };
    std::optional<Projected> Project(const glm::vec3& world, const View& view);
    // where something tracked is on the picture, by the view the game wrote last
    std::optional<Projected> Project(const Types::ScreenTrack& track);
    // a spot of the picture at a depth back in the world
    glm::vec3 Unproject(glm::vec2 uv, float depth, const View& view);

    // What the lens's Track focus follows (saved with the keyframes): the UI's, read by the lens each frame. Any thread.
    Types::ScreenTrack GetFocusTrack();
    void SetFocusTrack(const Types::ScreenTrack& track);

    // a track as the keyframe file has it, and back (none for a node that isn't one)
    nlohmann::json ToJson(const Types::ScreenTrack& track);
    Types::ScreenTrack FromJson(const nlohmann::json& node);

    // what can be followed: the demo's players and the scene's actors, as the bone camera's targets
    std::vector<Types::BoneCameraTarget> GetTargets();
    std::string DescribeTarget(const Types::BoneCameraTarget& target);
}  // namespace IWXMVM::Components::Tracking
