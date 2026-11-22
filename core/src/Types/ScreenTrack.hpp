#pragma once
#include "Types/BoneCamera.hpp"

namespace IWXMVM::Types
{
    // Something in the world a screen effect follows (the lens's focus, a lens flare): a bone of a player or an actor,
    // or a point picked on the picture (where it was in the world then)
    enum class TrackKind
    {
        None,
        Target,
        Point,
    };

    struct ScreenTrack
    {
        TrackKind kind = TrackKind::None;
        BoneCameraTarget target{};
        std::string bone = "j_head";
        glm::vec3 point{0.0f};

        bool operator==(const ScreenTrack&) const = default;
    };
}  // namespace IWXMVM::Types
