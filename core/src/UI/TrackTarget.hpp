#pragma once
#include "StdInclude.hpp"
#include "Types/ScreenTrack.hpp"

namespace IWXMVM::UI
{
    // The rows choosing what a screen effect follows: a player or an actor and its bone, or a point picked on the
    // picture (the caller's pick, shown as such). Says whether it changed.
    bool DrawTrackTarget(const char* id, Types::ScreenTrack& track);

    // A click on the picture made into what to follow: the player or actor whose bone (or middle) is nearest it, at
    // once, or else the spot of the map there, once its depth is read back (a few frames later)
    class TrackPicker
    {
       public:
        // (spotsOnly: never a player or an actor, the spot of the map there)
        void Begin(glm::vec2 uv, const std::string& bone, bool spotsOnly = false);
        // the pick, once there (then not again); none while waiting or with nothing picked
        std::optional<Types::ScreenTrack> Poll();
        bool IsWaiting() const
        {
            return waiting.has_value();
        }

       private:
        std::optional<Types::ScreenTrack> result;
        std::optional<glm::vec2> waiting;
        std::uint64_t startedAt = 0;
    };
}  // namespace IWXMVM::UI
