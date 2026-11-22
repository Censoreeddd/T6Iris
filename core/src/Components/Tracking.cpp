#include "StdInclude.hpp"
#include "Tracking.hpp"

#include "Mod.hpp"
#include "Actors.hpp"
#include "BoneCamera.hpp"
#include "KillFinder.hpp"

namespace IWXMVM::Components::Tracking
{
    namespace
    {
        std::mutex mutex;
        std::optional<View> current;
        Types::ScreenTrack focusTrack;
    }  // namespace

    Types::ScreenTrack GetFocusTrack()
    {
        std::lock_guard lock(mutex);
        return focusTrack;
    }

    void SetFocusTrack(const Types::ScreenTrack& track)
    {
        std::lock_guard lock(mutex);
        focusTrack = track;
    }

    nlohmann::json ToJson(const Types::ScreenTrack& track)
    {
        switch (track.kind)
        {
            case Types::TrackKind::Target:
                return {{"kind", "target"},
                        {"target", track.target.kind == Types::BoneCameraTargetKind::Actor ? "actor" : "player"},
                        {"id", track.target.id},
                        {"bone", track.bone}};
            case Types::TrackKind::Point:
                return {{"kind", "point"}, {"point", {track.point.x, track.point.y, track.point.z}}};
            default:
                return {{"kind", "none"}};
        }
    }

    Types::ScreenTrack FromJson(const nlohmann::json& node)
    {
        Types::ScreenTrack track;
        if (!node.is_object())
            return track;
        const auto kind = node.value("kind", std::string());
        if (kind == "target")
        {
            track.kind = Types::TrackKind::Target;
            track.target.kind = node.value("target", std::string()) == "actor" ? Types::BoneCameraTargetKind::Actor
                                                                               : Types::BoneCameraTargetKind::Player;
            track.target.id = node.value("id", 0);
            track.bone = node.value("bone", track.bone);
        }
        else if (kind == "point")
        {
            const auto point = node.value("point", std::vector<float>{});
            if (point.size() == 3)
            {
                track.kind = Types::TrackKind::Point;
                track.point = glm::vec3(point[0], point[1], point[2]);
            }
        }
        return track;
    }

    void SetView(const View& view)
    {
        std::lock_guard lock(mutex);
        current = view;
    }

    std::optional<View> GetView()
    {
        std::lock_guard lock(mutex);
        return current;
    }

    std::optional<glm::vec3> WorldPosition(const Types::ScreenTrack& track)
    {
        switch (track.kind)
        {
            case Types::TrackKind::Point:
                return track.point;
            case Types::TrackKind::Target:
                if (const auto bone = Mod::GetGameInterface()->GetTargetBone(track.target, track.bone))
                    return bone->position;
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }

    std::optional<Projected> Project(const glm::vec3& world, const View& view)
    {
        const auto toward = world - view.position;
        const float forward = glm::dot(toward, view.axis[0]);
        if (forward <= 1.0f || view.tanHalfFovX <= 0.0f || view.tanHalfFovY <= 0.0f)
            return std::nullopt;
        const float x = -glm::dot(toward, view.axis[1]) / forward / view.tanHalfFovX;
        const float y = glm::dot(toward, view.axis[2]) / forward / view.tanHalfFovY;
        return Projected{glm::vec2(0.5f + 0.5f * x, 0.5f - 0.5f * y), forward};
    }

    std::optional<Projected> Project(const Types::ScreenTrack& track)
    {
        const auto view = GetView();
        const auto world = WorldPosition(track);
        if (!view || !world)
            return std::nullopt;
        return Project(*world, *view);
    }

    glm::vec3 Unproject(glm::vec2 uv, float depth, const View& view)
    {
        const float x = (uv.x - 0.5f) * 2.0f * view.tanHalfFovX;
        const float y = (0.5f - uv.y) * 2.0f * view.tanHalfFovY;
        return view.position + depth * (view.axis[0] - x * view.axis[1] + y * view.axis[2]);
    }

    std::vector<Types::BoneCameraTarget> GetTargets()
    {
        std::vector<Types::BoneCameraTarget> targets;
        if (const auto events = KillFinder::GetEvents(); events && !events->players.empty())
        {
            for (const auto& [client, player] : events->players)
                targets.push_back({Types::BoneCameraTargetKind::Player, client});
        }
        else
        {
            for (const auto& entity : Mod::GetGameInterface()->GetEntities())
            {
                if (entity.type == Types::EntityType::Player && entity.isValid && entity.clientNum >= 0)
                    targets.push_back({Types::BoneCameraTargetKind::Player, entity.clientNum});
            }
        }
        for (const auto& actor : Actors::GetActors())
            targets.push_back({Types::BoneCameraTargetKind::Actor, static_cast<std::int32_t>(actor.id)});
        return targets;
    }

    std::string DescribeTarget(const Types::BoneCameraTarget& target)
    {
        const auto name = BoneCamera::DescribeTarget(target);
        return target.kind == Types::BoneCameraTargetKind::Player ? name + " (player)" : name;
    }
}  // namespace IWXMVM::Components::Tracking
