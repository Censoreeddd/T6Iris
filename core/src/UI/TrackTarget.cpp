#include "StdInclude.hpp"
#include "TrackTarget.hpp"

#include "Mod.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/Tracking.hpp"
#include "Components/Lens.hpp"
#include "D3D11.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        void Label(const char* text)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", text);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
            ImGui::SetNextItemWidth(-FLT_MIN);
        }

        std::string Describe(const Types::ScreenTrack& track)
        {
            switch (track.kind)
            {
                case Types::TrackKind::Point:
                    return "The point picked";
                case Types::TrackKind::Target:
                    return Components::Tracking::DescribeTarget(track.target);
                default:
                    return "Choose...";
            }
        }
    }  // namespace

    bool DrawTrackTarget(const char* id, Types::ScreenTrack& track)
    {
        bool changed = false;
        ImGui::PushID(id);
        Label("Follows");
        if (ImGui::BeginCombo("##trackTarget", Describe(track).c_str()))
        {
            if (ImGui::Selectable("A point picked on the picture", track.kind == Types::TrackKind::Point))
            {
                track.kind = Types::TrackKind::Point;
                changed = true;
            }
            ImGui::SetItemTooltip("Then Pick on the picture: it stays on that spot of the map as the camera moves");
            const auto targets = Components::Tracking::GetTargets();
            if (!targets.empty())
                ImGui::Separator();
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                const bool selected = track.kind == Types::TrackKind::Target && track.target == targets[i];
                if (ImGui::Selectable(
                        std::format("{}##{}", Components::Tracking::DescribeTarget(targets[i]), static_cast<int>(i))
                            .c_str(),
                        selected))
                {
                    track.kind = Types::TrackKind::Target;
                    track.target = targets[i];
                    changed = true;
                }
            }
            if (targets.empty())
                ImGui::TextDisabled("No players or actors yet");
            ImGui::EndCombo();
        }

        if (track.kind == Types::TrackKind::Target)
        {
            // the target's named bones (head, chest, weapon...) first
            const auto bones = Mod::GetGameInterface()->GetTargetBones(track.target);
            const auto [ordered, named] = Components::BoneCameraLogic::OrderBones(bones);
            Label("Bone");
            const auto describe = [](const std::string& bone) {
                const auto label = Components::BoneCameraLogic::DescribeBone(bone);
                return label == bone ? bone : std::format("{}  ({})", label, bone);
            };
            if (ImGui::BeginCombo("##trackBone", describe(track.bone).c_str(), ImGuiComboFlags_HeightLarge))
            {
                for (std::size_t i = 0; i < ordered.size(); ++i)
                {
                    if (i == named && named > 0)
                        ImGui::Separator();
                    if (ImGui::Selectable(describe(ordered[i]).c_str(), ordered[i] == track.bone))
                    {
                        track.bone = ordered[i];
                        changed = true;
                    }
                }
                if (ordered.empty())
                    ImGui::TextDisabled("(not there now)");
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
        return changed;
    }

    void TrackPicker::Begin(glm::vec2 uv, const std::string& bone, bool spotsOnly)
    {
        // (where the click is in the picture the game drew, under the lens's bend)
        uv = Components::Lens::PictureToRender(uv);
        result.reset();
        waiting.reset();
        // the nearest player or actor: by the bone followed, or its middle (within a little of the click)
        float nearest = 0.05f;
        for (const auto& target : spotsOnly ? std::vector<Types::BoneCameraTarget>{} : Components::Tracking::GetTargets())
        {
            for (const auto& name : {bone, std::string("j_spine4"), std::string("j_mainroot")})
            {
                const Types::ScreenTrack candidate{Types::TrackKind::Target, target, name};
                if (const auto projected = Components::Tracking::Project(candidate))
                {
                    const float distance = glm::distance(projected->uv, uv);
                    if (distance < nearest)
                    {
                        nearest = distance;
                        result = Types::ScreenTrack{Types::TrackKind::Target, target, bone};
                    }
                }
            }
        }
        if (result)
            return;
        waiting = uv;
        startedAt = GetTickCount64();
        D3D11::RequestDepthAt(uv);
    }

    std::optional<Types::ScreenTrack> TrackPicker::Poll()
    {
        if (result)
            return std::exchange(result, std::nullopt);
        if (!waiting)
            return std::nullopt;
        if (const auto depth = D3D11::GetDepthAt(*waiting))
        {
            const auto view = Components::Tracking::GetView();
            const auto uv = *waiting;
            waiting.reset();
            if (!view)
                return std::nullopt;
            Types::ScreenTrack track;
            track.kind = Types::TrackKind::Point;
            track.point = Components::Tracking::Unproject(uv, *depth, *view);
            return track;
        }
        // (nothing drawn there, the sky, or no depth: given up)
        if (GetTickCount64() - startedAt > 2000)
        {
            LOG_DEBUG("A pick on the picture found nothing to follow at ({:.3f} {:.3f})", waiting->x, waiting->y);
            waiting.reset();
        }
        else
        {
            D3D11::RequestDepthAt(*waiting);
        }
        return std::nullopt;
    }
}  // namespace IWXMVM::UI
