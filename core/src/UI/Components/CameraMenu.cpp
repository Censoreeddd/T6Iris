#include "StdInclude.hpp"
#include "CameraMenu.hpp"

#include "Mod.hpp"
#include "Components/Actors.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "UI/UIManager.hpp"
#include "UI/ImGuiEx/ImGuiExtensions.hpp"
#include "Components/CameraManager.hpp"
#include "Components/CameraShake.hpp"
#include "Components/LensFlare.hpp"
#include "Components/LensDirt.hpp"
#include "Components/Lens.hpp"
#include "Utilities/PathUtils.hpp"
#include "Components/Tracking.hpp"
#include "UI/TrackTarget.hpp"
#include "Components/KeyframeManager.hpp"
#include "UI/Components/GameView.hpp"
#include "Events.hpp"
#include "Components/KillFinder.hpp"
#include "Components/Playback.hpp"
#include "Components/UndoHistory.hpp"
#include "UI/Components/Timeline.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/TimelineMath.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Commands.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Input.hpp"
#include "Events.hpp"
#include "Utilities/MathUtils.hpp"
#include "Resources.hpp"

namespace IWXMVM::UI
{
    float fov = 90;

    namespace
    {
        // the Lens Flare section: drawn this frame (its pin shows over the picture then), and a pick waited for
        int flareRowsShownAt = -100;
        bool pickingFlare = false;
        TrackPicker flarePicker;  // the flare's Follow pick (a player, or a spot of the map)

        // The flare's pin over the game view while the section is open with the Pin source, and a pick there
        void DrawFlarePin()
        {
            auto settings = Components::LensFlare::GetSettings();
            const bool shown = ImGui::GetFrameCount() - flareRowsShownAt <= 2;
            if (!settings.enabled || settings.source != Components::LensFlare::Source::Tracked || !shown ||
                Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                pickingFlare = false;
                return;
            }
            const auto gameView = UIManager::Get().GetUIComponent<GameView>(Component::Component::GameView);
            const auto position = gameView->GetViewportPosition();
            const auto size = gameView->GetViewportSize();
            if (size.x <= 0 || size.y <= 0)
                return;
            auto* drawList = ImGui::GetWindowDrawList();
            if (pickingFlare)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    pickingFlare = false;
                    return;
                }
                const auto mouse = ImGui::GetIO().MousePos;
                if (mouse.x >= position.x && mouse.y >= position.y && mouse.x < position.x + size.x &&
                    mouse.y < position.y + size.y && !gameView->IsMouseOverOverlay())
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                    const auto color = IM_COL32(255, 255, 255, 230);
                    drawList->AddLine(mouse - ImVec2(14, 0), mouse + ImVec2(15, 0), color, 1.5f);
                    drawList->AddLine(mouse - ImVec2(0, 14), mouse + ImVec2(0, 15), color, 1.5f);
                    drawList->AddText(mouse + ImVec2(12, 10), color, "click where the light is");
                    // the spot of the map clicked followed (DrawLensFlare takes the pick)
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        pickingFlare = false;
                        flarePicker.Begin(glm::vec2(std::clamp((mouse.x - position.x) / size.x, 0.0f, 1.0f),
                                                    std::clamp((mouse.y - position.y) / size.y, 0.0f, 1.0f)),
                                          settings.track.bone, true);
                    }
                    return;
                }
            }
            // the spot on the picture, where it is now
            const auto projected = Components::Tracking::Project(settings.track);
            if (!projected)
                return;
            const auto pinAt = projected->uv;
            const ImVec2 pin(position.x + pinAt.x * size.x, position.y + pinAt.y * size.y);
            drawList->AddCircle(pin, 9.0f, IM_COL32(0, 0, 0, 160), 0, 3.5f);
            drawList->AddCircle(pin, 9.0f, IM_COL32(120, 200, 255, 255), 0, 2.0f);
            drawList->AddCircleFilled(pin, 2.0f, IM_COL32(120, 200, 255, 255));
        }
    }  // namespace

    void CameraMenu::Initialize()
    {
        Events::RegisterListener(EventType::OnRenderGameView, []() { DrawFlarePin(); });
    }

    void DrawFirstPersonSettings()
    {
        auto columnPercent = 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Field of View");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::SliderFloat("##fovSlider", &fov, 45, 160, "%.1f"))
        {
            Mod::GetGameInterface()->SetFov(fov);
        }
    }

    // a row's label in the first column, its control filling the rest
    void BeginRow(const char* label)
    {
        constexpr auto COLUMN_PERCENT = 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * COLUMN_PERCENT);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - COLUMN_PERCENT) - ImGui::GetStyle().WindowPadding.x);
    }

    // a row of the bone camera's that has no keys: nothing where the keyed rows' diamond is, so that the labels line
    // up (as the Look tab's rows without keys)
    void BeginBoneRow(const char* label)
    {
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        BeginRow(label);
    }

    // What the bone camera can follow: the demo's players and the scene's actors
    std::vector<Types::BoneCameraTarget> GetBoneCameraTargets()
    {
        std::vector<Types::BoneCameraTarget> targets;
        if (const auto events = Components::KillFinder::GetEvents(); events && !events->players.empty())
        {
            for (const auto& [client, player] : events->players)
                targets.push_back({Types::BoneCameraTargetKind::Player, client});
        }
        else
        {
            // a game whose demos aren't read for their events (T6): the players there now
            for (const auto& entity : Mod::GetGameInterface()->GetEntities())
            {
                if (entity.type == Types::EntityType::Player && entity.isValid && entity.clientNum >= 0)
                    targets.push_back({Types::BoneCameraTargetKind::Player, entity.clientNum});
            }
        }
        for (const auto& actor : Components::Actors::GetActors())
            targets.push_back({Types::BoneCameraTargetKind::Actor, static_cast<std::int32_t>(actor.id)});
        return targets;
    }

    std::string DescribeBoneCameraTarget(const Types::BoneCameraTarget& target)
    {
        const auto name = Components::BoneCamera::DescribeTarget(target);
        return target.kind == Types::BoneCameraTargetKind::Player ? name + " (player)" : name;
    }

    std::string DescribeBoneCameraBone(const std::string& bone)
    {
        const auto label = Components::BoneCameraLogic::DescribeBone(bone);
        return label == bone ? bone : std::format("{}  ({})", label, bone);
    }

    std::string ToLower(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    void DrawBoneCameraSettings()
    {
        auto& boneCamera = static_cast<Components::BoneCamera&>(*Components::CameraManager::Get().GetActiveCamera());
        auto& settings = boneCamera.GetSettings();

        BeginBoneRow("Follows");
        if (ImGui::BeginCombo("##boneCameraTarget", DescribeBoneCameraTarget(settings.target).c_str()))
        {
            const auto targets = GetBoneCameraTargets();
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                if (i > 0 && targets[i].kind != targets[i - 1].kind)
                    ImGui::Separator();
                const bool selected = targets[i] == settings.target;
                if (ImGui::Selectable(
                        std::format("{}##{}", DescribeBoneCameraTarget(targets[i]), static_cast<int>(i)).c_str(),
                        selected))
                    settings.target = targets[i];
                ImGuiEx::Keyframeable::Record(std::format(
                    "camera.bone.target.{}{}",
                    targets[i].kind == Types::BoneCameraTargetKind::Actor ? "actor" : "player", targets[i].id));
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            if (targets.empty())
                ImGui::TextDisabled("No players or actors yet");
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("camera.bone.target");
        ImGui::SetItemTooltip("A player of the demo (through its body once it died) or an actor");

        // the target's bones, the useful ones first by what they are, the others below with a filter
        const auto bones = Mod::GetGameInterface()->GetTargetBones(settings.target);
        const auto [ordered, named] = Components::BoneCameraLogic::OrderBones(bones);
        BeginBoneRow("Bone");
        static char filter[64] = "";
        if (ImGui::BeginCombo("##boneCameraBone", DescribeBoneCameraBone(settings.bone).c_str(),
                              ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing())
            {
                filter[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##boneCameraBoneFilter", "Filter", filter, sizeof(filter));
            const auto lowerFilter = ToLower(filter);
            for (std::size_t i = 0; i < ordered.size(); ++i)
            {
                const auto label = DescribeBoneCameraBone(ordered[i]);
                if (!lowerFilter.empty() && ToLower(label).find(lowerFilter) == std::string::npos)
                    continue;
                if (i == named && named > 0 && lowerFilter.empty())
                    ImGui::Separator();
                const bool selected = ordered[i] == settings.bone;
                if (ImGui::Selectable(label.c_str(), selected))
                    settings.bone = ordered[i];
                ImGuiEx::Keyframeable::Record("camera.bone.bone." + ordered[i]);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            if (ordered.empty())
                ImGui::TextDisabled("Its bones show once it is drawn");
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("camera.bone.bone");

        if (!Mod::GetGameInterface()->GetTargetBone(settings.target, settings.bone))
        {
            const bool lacksBone = !bones.empty() && std::ranges::find(bones, settings.bone) == bones.end();
            ImGui::TextDisabled(ICON_FA_TRIANGLE_EXCLAMATION " %s",
                                lacksBone ? "It has no such bone" : "It isn't in the demo now");
        }

        // the offsets and FOV are keyed on the timeline: with keys a row shows theirs at
        // the playhead and a change keys it there
        ImGui::Dummy(ImVec2(0, 4));
        Types::KeyframeValue offset(settings.positionOffset);
        ImGuiEx::Keyframeable::Row(
            "Offset", Types::KeyframeablePropertyType::BoneCameraOffset, offset,
            [](const char* id, Types::KeyframeValue& value) {
                const bool changed = ImGui::DragFloat3(id, &value.vector3.x, 0.25f, -10000, 10000, "%.1f");
                ImGuiEx::Keyframeable::Record("camera.bone.offset");
                ImGui::SetItemTooltip("Forward, left and up from the bone (from where it faces, with Turn with: "
                                      "Where it faces)");
                return changed;
            },
            "Bone Camera Offset");
        settings.positionOffset = offset.vector3;
        Types::KeyframeValue rotation(settings.rotationOffset);
        ImGuiEx::Keyframeable::Row(
            "Rotation", Types::KeyframeablePropertyType::BoneCameraRotation, rotation,
            [](const char* id, Types::KeyframeValue& value) {
                const bool changed = ImGui::DragFloat3(id, &value.vector3.x, 0.5f, -180, 180, "%.1f");
                ImGuiEx::Keyframeable::Record("camera.bone.rotation");
                ImGui::SetItemTooltip("Pitch, yaw and roll from the bone's own axes");
                return changed;
            },
            "Bone Camera Rotation");
        settings.rotationOffset = rotation.vector3;
        Types::KeyframeValue fov(settings.fov);
        ImGuiEx::Keyframeable::Row(
            "Field of View", Types::KeyframeablePropertyType::BoneCameraFov, fov,
            [](const char* id, Types::KeyframeValue& value) {
                const bool changed = ImGui::DragFloat(id, &value.floatingPoint, 0.5f, 1, 160, "%.0f",
                                                      ImGuiSliderFlags_AlwaysClamp);
                ImGuiEx::Keyframeable::Record("camera.bone.fov");
                return changed;
            },
            "Bone Camera FOV");
        settings.fov = fov.floatingPoint;

        BeginBoneRow("Steady");
        ImGui::SliderFloat("##boneCameraSteady", &settings.steady, 0.0f, 0.5f,
                           settings.steady > 0.0f ? "%.2f s" : "Off", ImGuiSliderFlags_AlwaysClamp);
        ImGuiEx::Keyframeable::Record("camera.bone.steady");
        ImGui::SetItemTooltip("Smooths the bone's sway (a head's bob) over this long, in its target's own space, so "
                              "the camera doesn't fall behind as it moves. A recording starts from the bone as it is.");

        static constexpr std::array<std::pair<Types::BoneCameraTurn, const char*>, 3> TURNS = {{
            {Types::BoneCameraTurn::Bone, "The bone"},
            {Types::BoneCameraTurn::Level, "The bone, level"},
            {Types::BoneCameraTurn::Facing, "Where it faces"},
        }};
        BeginBoneRow("Turn with");
        const auto turn = std::ranges::find_if(TURNS, [&](const auto& entry) { return entry.first == settings.turn; });
        if (ImGui::BeginCombo("##boneCameraTurn", turn != TURNS.end() ? turn->second : ""))
        {
            for (const auto& [value, label] : TURNS)
            {
                if (ImGui::Selectable(label, value == settings.turn))
                    settings.turn = value;
                ImGuiEx::Keyframeable::Record(std::format("camera.bone.turn.{}", static_cast<int>(value)));
            }
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("camera.bone.turn");
        ImGui::SetItemTooltip("The bone: as the bone turns. The bone, level: so, with the horizon level. Where it "
                              "faces: only as its target turns the way it goes, never with the bone's sway.");

        BeginBoneRow("Show Bone");
        ImGui::Checkbox("##boneCameraShowBone", &settings.showBone);
        ImGuiEx::Keyframeable::Record("camera.bone.showbone");

        // (rows with keys keyed so at the playhead)
        if (ImGui::Button("Reset offsets"))
            boneCamera.ResetOffsets();
        ImGuiEx::Keyframeable::Record("camera.bone.reset");
        ImGui::TextDisabled("Fly to move it on the bone.");
    }

    void DrawThirdPersonSettings()
    {
        ImGui::TextDisabled("Follows the player from behind.");
    }

    void DrawDollycamSettings()
    {
        ImGui::TextWrapped("Plays the campath.");
    }

    // a speed the camera tab keeps in config.json, saved once a drag or typing ends
    void PreferenceRow(const char* label, const char* id, float* value, float speed, float min, float max,
                       const char* format)
    {
        const auto columnPercent = 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x);
        ImGui::DragFloat(id, value, speed, min, max, format, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit())
            Configuration::Get().Write(true);
    }

    void DrawFreeCameraSettings()
    {
        auto& preferences = PreferencesConfiguration::Get();
        PreferenceRow("Speed", "##freeCameraSpeed", &preferences.freecamSpeed, 1.0f, 1.0f, 500.0f, "%.0f");
        ImGui::SetItemTooltip("How fast it flies; Shift and Ctrl held go faster and slower");
        PreferenceRow("Mouse sensitivity", "##freeCameraMouse", &preferences.freecamMouseSpeed, 0.01f, 0.01f, 5.0f,
                      "%.2f");
        ImGui::TextDisabled("Fly: hold the right mouse button in the view, or press %s.",
                            Commands::GetShortcut("fly.toggle").c_str());
    }

    void DrawOrbitCameraSettings()
    {
        auto& preferences = PreferencesConfiguration::Get();
        PreferenceRow("Rotation speed", "##orbitRotation", &preferences.orbitRotationSpeed, 0.01f, 0.01f, 5.0f, "%.2f");
        PreferenceRow("Movement speed", "##orbitMove", &preferences.orbitMoveSpeed, 0.01f, 0.01f, 5.0f, "%.2f");
        PreferenceRow("Zoom speed", "##orbitZoom", &preferences.orbitZoomSpeed, 0.01f, 0.01f, 5.0f, "%.2f");
        ImGui::TextDisabled("Right drag orbits, Shift+right drag pans, the wheel zooms.");
    }

    // the camera modes as buttons, three a row, with their keys
    void DrawModes()
    {
        struct Mode
        {
            Components::Camera::Mode mode;
            const char* label;
            std::string_view command;
            const char* name;  // for the harness
        };
        // (Orbit left out)
        static constexpr std::array<Mode, 5> MODES = {{
            {Components::Camera::Mode::FirstPerson, "First person", "camera.firstPerson", "first"},
            {Components::Camera::Mode::ThirdPerson, "Third person", "camera.thirdPerson", "third"},
            {Components::Camera::Mode::Free, "Free", "camera.free", "free"},
            {Components::Camera::Mode::Dolly, "Dolly", "camera.dolly", "dolly"},
            {Components::Camera::Mode::Bone, "Bone", "camera.bone", "bone"},
        }};
        const auto current = Components::CameraManager::Get().GetActiveCamera()->GetMode();
        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto width = std::floor((ImGui::GetContentRegionAvail().x - spacing * 2) / 3);
        for (std::size_t i = 0; i < MODES.size(); i++)
        {
            const auto& mode = MODES[i];
            if (i % 3 != 0)
                ImGui::SameLine();
            const bool selected = mode.mode == current;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(mode.label, ImVec2(width, 0)))
                Commands::Run(mode.command);
            if (selected)
                ImGui::PopStyleColor();
            ImGuiEx::Keyframeable::Record(std::format("camera.mode.{}", mode.name));
            const auto key = Commands::GetShortcut(mode.command);
            if (!key.empty())
                ImGui::SetItemTooltip("%s (%s)", Commands::Find(mode.command)->label.data(), key.c_str());
        }
    }

    // A button that runs a command, with its key; greyed out with why when it can't run now
    void CommandButton(const char* label, std::string_view id, const char* name, ImVec2 size = ImVec2(0, 0))
    {
        const auto reason = Commands::GetUnavailableReason(id);
        ImGui::BeginDisabled(reason.has_value());
        if (ImGui::Button(label, size))
            Commands::Run(id);
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record(name);
        const auto key = Commands::GetShortcut(id);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s%s%s", Commands::Find(id)->label.data(),
                              key.empty() ? "" : std::format(" ({})", key).c_str(),
                              reason ? std::format("\n{}", *reason).c_str() : "");
    }

    // The campath's nodes, one a row: a click selects one and goes there; its buttons go there, set it to the camera
    // and delete it (each one step with a message, as on the timeline)
    // A node's ease in or out in the node list: how much, marked Auto when it follows Auto Ease,
    // or a dash for none
    void DrawNodeEase(const Types::Keyframe& node, std::size_t index, bool in)
    {
        const auto amount = in ? node.easeIn : node.easeOut;
        const bool follows = in ? node.autoEaseIn : node.autoEaseOut;
        ImGui::AlignTextToFramePadding();
        if (amount <= 0.0f)
        {
            ImGui::TextDisabled("-");
            ImGui::SetItemTooltip("No ease %s", in ? "in" : "out");
            return;
        }
        ImGui::BeginGroup();
        ImGui::Text("%.0f%%", amount * 100.0f);
        if (follows)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Auto");
            ImGuiEx::Keyframeable::Record(std::format("node.{}.ease{}.auto", index, in ? "in" : "out"));
        }
        ImGui::EndGroup();
        ImGui::SetItemTooltip("Ease %s %.0f%%: %s", in ? "in" : "out", amount * 100.0f,
                              follows ? "Auto Ease's" : "the node's own");
    }

    void DrawNodes(const std::vector<Types::Keyframe>& nodes)
    {
        const auto& property =
            Components::KeyframeManager::Get().GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        const auto select = [&](const Types::Keyframe& node) {
            const auto item = TimelineSelection::KeyItem(property.type, node.id);
            TimelineSelection::Set(std::span(&item, 1));
            TimelineSelection::SetAnchor(item);
        };
        const auto tick = Components::Playback::GetTimelineTick();
        const auto buttonWidth = ImGui::GetFrameHeight();
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuterH;
        // (a row more for the headers)
        const auto rows = std::min<std::size_t>(nodes.size(), 8) + 1;
        const auto height = (ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2) * rows + 2;
        if (!ImGui::BeginTable("##campathNodes", 5, flags, ImVec2(0, height)))
            return;
        const auto easeWidth = ImGui::CalcTextSize("100% Auto").x;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Ease in", ImGuiTableColumnFlags_WidthFixed, easeWidth);
        ImGui::TableSetupColumn("Ease out", ImGuiTableColumnFlags_WidthFixed, easeWidth);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                buttonWidth * 3 + ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::TableHeadersRow();

        std::optional<Types::Keyframe> toDelete;
        for (std::size_t i = 0; i < nodes.size(); i++)
        {
            const auto& node = nodes[i];
            ImGui::PushID(node.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = TimelineSelection::IsSelected(TimelineSelection::KeyItem(property.type, node.id));
            const auto label = std::format("{}Node {}", node.tick == tick ? ICON_FA_CARET_RIGHT " " : "", i + 1);
            if (ImGui::Selectable(label.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, ImGui::GetFrameHeight())))
            {
                select(node);
                Seek(node.tick, false);
            }
            ImGuiEx::Keyframeable::Record(std::format("node.{}", i));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TimelineMath::FormatTime(node.tick).c_str());
            ImGui::TableNextColumn();
            DrawNodeEase(node, i, true);
            ImGui::TableNextColumn();
            DrawNodeEase(node, i, false);
            ImGui::TableNextColumn();
            if (ImGui::Button(ICON_FA_ARROW_RIGHT, ImVec2(buttonWidth, 0)))
                Seek(node.tick, false);
            ImGuiEx::Keyframeable::Record(std::format("node.{}.goto", i));
            ImGui::SetItemTooltip("Go to it");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CAMERA_ROTATE, ImVec2(buttonWidth, 0)))
            {
                select(node);
                Commands::Run("campath.updateNode");
            }
            ImGuiEx::Keyframeable::Record(std::format("node.{}.update", i));
            ImGui::SetItemTooltip("Set it to where the camera is (%s with it selected)",
                                  Commands::GetShortcut("campath.updateNode").c_str());
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(buttonWidth, 0)))
                toDelete = node;
            ImGuiEx::Keyframeable::Record(std::format("node.{}.delete", i));
            ImGui::SetItemTooltip("Delete it");
            ImGui::PopID();
        }
        ImGui::EndTable();

        // (after the table: deleting changes the nodes it went through)
        if (toDelete)
        {
            select(*toDelete);
            TimelineSelection::Delete();
        }
    }

    // The campath, under every camera's settings: the node at the playhead with its diamond, its position, angles and
    // field of view (changing them keys them there, as on the Visuals tab), its interpolation and eases, and how fast
    // the camera goes along the path
    void DrawCampath()
    {
        const auto columnPercent = 0.4f;
        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        auto& nodes = keyframeManager.GetKeyframes(property);
        const auto tick = Components::Playback::GetTimelineTick();

        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_VIDEO "  Campath", "campath"))
            return;

        // a node made here from the view (as K does), or the path's values here
        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        Types::KeyframeValue value = nodes.empty()
                                         ? Types::KeyframeValue(Types::CameraData{
                                               camera->GetPosition(), camera->GetRotation(), camera->GetFov()})
                                         : keyframeManager.Interpolate(property, tick);
        // the node here, found again after what can add one (the diamond, + Node, a value changed): an added node can
        // move the nodes, and the first one is where nothing was, so one found before is no node at all
        const auto findHere = [&] { return std::ranges::find(nodes, tick, &Types::Keyframe::tick); };

        ImGui::PushID("campath");
        ImGuiEx::Keyframeable::Diamond(property, value, "The campath", ImGui::GetFontSize() * 1.5f);
        auto here = findHere();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (nodes.empty())
            ImGui::TextUnformatted("No nodes yet");
        else if (here != nodes.end())
            ImGui::Text("Node %d of %zu, at %s", static_cast<int>(here - nodes.begin()) + 1, nodes.size(),
                        TimelineMath::FormatTime(tick).c_str());
        else
            ImGui::Text("Between nodes, at %s", TimelineMath::FormatTime(tick).c_str());
        if (!nodes.empty())
        {
            // the nodes before and after
            ImGui::SameLine();
            const auto before = std::ranges::find_if(nodes.rbegin(), nodes.rend(),
                                                     [&](const Types::Keyframe& node) { return node.tick < tick; });
            const auto after =
                std::ranges::find_if(nodes, [&](const Types::Keyframe& node) { return node.tick > tick; });
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                                 ImGui::GetFrameHeight() * 2 - ImGui::GetStyle().ItemSpacing.x);
            ImGui::BeginDisabled(before == nodes.rend());
            if (ImGui::ArrowButton("##before", ImGuiDir_Left))
                Seek(before->tick, false);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Go to the node before");
            ImGui::SameLine();
            ImGui::BeginDisabled(after == nodes.end());
            if (ImGui::ArrowButton("##after", ImGuiDir_Right))
                Seek(after->tick, false);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Go to the node after");
        }

        // nodes added, set to the camera, played
        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto third = std::floor((ImGui::GetContentRegionAvail().x - spacing * 2) / 3);
        CommandButton(ICON_FA_PLUS " Node", "campath.addNode", "campath.add", ImVec2(third, 0));
        ImGui::SameLine();
        CommandButton(ICON_FA_CAMERA_ROTATE " Update", "campath.updateNode", "campath.update", ImVec2(third, 0));
        ImGui::SameLine();
        CommandButton(ICON_FA_PLAY " Play", "campath.play", "campath.play", ImVec2(third, 0));

        if (nodes.empty())
        {
            ImGui::TextDisabled("Fly and press %s.", Commands::GetShortcut("campath.addNode").c_str());
            ImGui::PopID();
            return;
        }

        // its values: changing one keys it here
        const auto row = [&](const char* label, const char* id, float* values, int count, float speed) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) -
                                    ImGui::GetStyle().WindowPadding.x);
            const bool changed = count == 3 ? ImGui::DragFloat3(id, values, speed, 0.0f, 0.0f, "%.1f")
                                            : ImGui::DragFloat(id, values, speed, 1.0f, 179.0f, "%.1f");
            ImGuiEx::Keyframeable::Record(std::format("control.CampathCamera.{}", id + 2));
            if (changed)
                ImGuiEx::Keyframeable::KeyValueHere(property, value, "the campath");
        };
        row("Position", "##position", glm::value_ptr(value.cameraData.position), 3, 1.0f);
        row("Angles", "##angles", glm::value_ptr(value.cameraData.rotation), 3, 0.25f);
        row("Field of View", "##fov", &value.cameraData.fov, 1, 0.25f);
        ImGuiEx::Keyframeable::EndEditWhenIdle();

        // how the camera goes along the path: every node's, Mixed when they differ (one node's is set from its
        // diamond's menu on the timeline), with how many have which under it
        {
            const auto shared = KeyframeLogic::GetSharedInterpolation(nodes);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Interpolation");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) -
                                    ImGui::GetStyle().WindowPadding.x);
            constexpr std::array ORDER = {Types::KeyInterpolation::Auto, Types::KeyInterpolation::Smooth,
                                          Types::KeyInterpolation::Linear, Types::KeyInterpolation::Hold};
            const auto preview = shared ? std::string(magic_enum::enum_name(*shared)) : std::string("Mixed");
            if (ImGui::BeginCombo("##interpolation", preview.c_str()))
            {
                for (const auto interpolation : ORDER)
                {
                    const auto name = magic_enum::enum_name(interpolation);
                    if (ImGui::Selectable(std::string(name).c_str(), shared == interpolation) &&
                        shared != interpolation)
                    {
                        auto changed = nodes;
                        for (auto& node : changed)
                            node.interpolation = interpolation;
                        keyframeManager.ChangeKeyframes(property, std::move(changed),
                                                        std::format("make the campath {}", name));
                    }
                    ImGuiEx::Keyframeable::Record(
                        std::format("control.interpolation.{}", ToLower(std::string(name))));
                }
                ImGui::EndCombo();
            }
            ImGuiEx::Keyframeable::Record("control.interpolation");
            ImGui::SetItemTooltip(
                "Every node's: Auto (one curve through all nodes), Smooth (a curve through each node from the "
                "nodes beside it), Linear, or Hold (stays until the next node). A node's own: its diamond's menu "
                "on the timeline");
            std::string counts;
            if (shared)
                counts = nodes.size() == 1 ? "1 node" : nodes.size() == 2 ? "Both nodes"
                                                                         : std::format("All {} nodes", nodes.size());
            for (const auto interpolation : ORDER)
            {
                const auto count = std::ranges::count(nodes, interpolation, &Types::Keyframe::interpolation);
                if (!shared && count > 0)
                    counts += std::format("{}{} {}", counts.empty() ? "" : ", ", count,
                                          magic_enum::enum_name(interpolation));
            }
            ImGui::TextDisabled("%s", counts.c_str());
            ImGuiEx::Keyframeable::Record("control.interpolation.counts");
        }

        // the node's eases
        here = findHere();
        if (here != nodes.end())
        {
            const auto change = [&](const std::function<void(Types::Keyframe&)>& edit, std::string label) {
                auto changed = *here;
                edit(changed);
                keyframeManager.ChangeKeyframes(property, {changed}, std::move(label));
            };

            // how much, a drag of the amount one step; after it, Auto when it is Auto Ease's, or on a side Auto Ease
            // eases (an end of the path; with All Nodes every side between two nodes) whose ease is the node's own, a
            // button that gives it back
            const auto autoEase = keyframeManager.GetAutoEase();
            const bool allNodes = autoEase.nodes == Types::AutoEaseNodes::AllNodes;
            const auto markWidth = ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2;
            const auto index = static_cast<std::size_t>(here - nodes.begin());
            for (const bool in : {true, false})
            {
                const auto end = in ? "in" : "out";
                // the tick where the labels are, the slider where the controls are
                ImGuiEx::Keyframeable::Ease(
                    in ? "Ease in" : "Ease out", in ? here->easeIn : here->easeOut,
                    ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x - markWidth -
                        ImGui::GetStyle().ItemSpacing.x,
                    ImGui::GetWindowWidth() * columnPercent, std::format("change the ease {} of the node", end),
                    [&](float amount, bool) {
                        change([&](Types::Keyframe& node) { (in ? node.easeIn : node.easeOut) = amount; },
                               amount > 0.0f ? std::format("ease {} the node", end)
                                             : std::format("no ease {} on the node", end));
                    },
                    in ? "Slow down to rest arriving at the node" : "Start from rest leaving the node",
                    std::format("control.ease{}", end), ImGuiEx::Keyframeable::Record);
                const bool follows = in ? here->autoEaseIn : here->autoEaseOut;
                const bool pathEnd = in ? index + 1 == nodes.size() : index == 0;
                const bool between = in ? index > 0 : index + 1 < nodes.size();
                const bool eased = pathEnd || (allNodes && between);
                if (!autoEase.on || (follows && (in ? here->easeIn : here->easeOut) <= 0.0f) || (!follows && !eased))
                    continue;
                ImGui::SameLine();
                if (follows)
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Auto");
                    ImGuiEx::Keyframeable::Record(std::format("control.ease{}.auto", end));
                    ImGui::SetItemTooltip(
                        "Auto Ease's (at the bottom of the tab); a change here makes it the node's "
                        "own");
                    continue;
                }
                if (ImGui::Button(std::format("Auto##ease{}", end).c_str(), ImVec2(markWidth, 0)))
                    change([&](Types::Keyframe& node) { (in ? node.autoEaseIn : node.autoEaseOut) = true; },
                           std::format("give the ease {} back to Auto Ease", end));
                ImGuiEx::Keyframeable::Record(std::format("control.ease{}.giveback", end));
                ImGui::SetItemTooltip("Give it back to Auto Ease: %.0f%%",
                                      (in ? autoEase.end : autoEase.start) * 100.0f);
            }
        }

        // the whole path
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Constant speed");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        bool constant = keyframeManager.IsConstantSpeed();
        if (ImGui::Checkbox("##constantSpeed", &constant))
            Commands::Run("campath.constantSpeed");
        ImGuiEx::Keyframeable::Record("control.constantSpeed");
        ImGui::SetItemTooltip(
            "The camera goes along the path at one speed from the first node to the last, "
            "whenever the nodes between are (the first node's ease out and the last's ease in "
            "still slow its ends)");
        if (nodes.size() >= 2)
        {
            const auto seconds = (nodes.back().tick - nodes.front().tick) / 1000.0;
            const auto length = keyframeManager.GetCampathLength();
            ImGui::TextDisabled("%zu nodes, %.2f s, %.0f units long%s", nodes.size(), seconds, length,
                                constant && seconds > 0 ? std::format(", {:.0f} u/s", length / seconds).c_str() : "");
        }
        else
        {
            ImGui::TextDisabled("One node: add another for a path.");
        }

        ImGui::Spacing();
        DrawNodes(nodes);
        ImGui::PopID();
    }

    // Auto Ease, at the bottom of the tab: new campaths ease out of their first node and
    // into their last by the amounts set, or out of and into every node (6.5); on or off, which nodes and the amounts
    // are preferences, and the campath's nodes that follow it follow them (KeyframeManager::SetAutoEase). A change is
    // one step, a slider's drag too.
    void DrawAutoEase()
    {
        static bool dragging = false;
        auto& keyframeManager = Components::KeyframeManager::Get();
        auto autoEase = keyframeManager.GetAutoEase();
        const auto columnPercent = 0.4f;

        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_WAND_MAGIC_SPARKLES "  Auto Ease", "autoease"))
            return;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("On");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        if (ImGui::Checkbox("##autoEase", &autoEase.on))
            keyframeManager.SetAutoEase(autoEase, autoEase.on ? "Auto Ease on" : "Auto Ease off");
        ImGuiEx::Keyframeable::Record("control.autoease");
        const bool allNodes = autoEase.nodes == Types::AutoEaseNodes::AllNodes;
        ImGui::SetItemTooltip(allNodes ? "Campaths ease out of every node and into every node; off, new nodes get none "
                                         "and eases already on nodes stay"
                                       : "New campaths ease out of their first node and into their last; off, they get "
                                         "none and eases already on nodes stay");

        ImGui::BeginDisabled(!autoEase.on);
        // which nodes: the first and last, or all of them
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Nodes");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            const auto spacing = ImGui::GetStyle().ItemSpacing.x;
            const auto width = std::floor(
                (ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x - spacing) / 2);
            struct Choice
            {
                Types::AutoEaseNodes nodes;
                const char* label;
                const char* tooltip;
                const char* record;
                const char* step;
            };
            static constexpr std::array<Choice, 2> CHOICES{{
                {Types::AutoEaseNodes::FirstAndLast, "First and Last",
                 "Eases out of the campath's first node and into its last", "control.autoease.firstandlast",
                 "Auto Ease on the first and last nodes"},
                {Types::AutoEaseNodes::AllNodes, "All Nodes",
                 "Eases out of every node and into every node, nodes added later too; an ease changed on a node stays "
                 "its own",
                 "control.autoease.allnodes", "Auto Ease on all nodes"},
            }};
            for (std::size_t i = 0; i < CHOICES.size(); ++i)
            {
                const auto& choice = CHOICES[i];
                if (i > 0)
                    ImGui::SameLine();
                const bool selected = autoEase.nodes == choice.nodes;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(choice.label, ImVec2(width, 0)) && !selected)
                {
                    autoEase.nodes = choice.nodes;
                    keyframeManager.SetAutoEase(autoEase, choice.step);
                }
                if (selected)
                    ImGui::PopStyleColor();
                ImGuiEx::Keyframeable::Record(choice.record);
                ImGui::SetItemTooltip("%s", choice.tooltip);
            }
        }
        for (const bool start : {true, false})
        {
            ImGui::PushID(start ? "autoEaseStart" : "autoEaseEnd");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(start ? "Ease start" : "Ease end");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) -
                                    ImGui::GetStyle().WindowPadding.x);
            auto& amount = start ? autoEase.start : autoEase.end;
            float percent = amount * 100.0f;
            const bool changed =
                ImGui::SliderFloat("##amount", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            ImGuiEx::Keyframeable::Record(start ? "control.autoease.start" : "control.autoease.end");
            if (allNodes)
                ImGui::SetItemTooltip(start ? "How much the camera starts from rest leaving each node"
                                            : "How much the camera slows to rest arriving at each node");
            else
                ImGui::SetItemTooltip(start ? "How much the camera starts from rest leaving the first node"
                                            : "How much the camera slows to rest arriving at the last node");
            const auto label = std::format("Auto Ease {}", start ? "start" : "end");
            if (changed)
            {
                // (a drag is one step)
                if (!dragging)
                    Components::UndoHistory::Get().BeginGroup(label);
                dragging = true;
                amount = std::clamp(percent / 100.0f, 0.0f, 1.0f);
                keyframeManager.SetAutoEase(autoEase, label, false);
            }
            if (ImGui::IsItemDeactivated() && dragging)
            {
                keyframeManager.SetAutoEase(autoEase, label);
                Components::UndoHistory::Get().EndGroup();
                dragging = false;
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", !autoEase.on ? "Off: new campaths get no ease."
                                  : allNodes   ? "Campaths ease out of every node by Ease start and into every node by "
                                                 "Ease end, new nodes too."
                                               : "New campaths ease out of their first node and into their last.");
        ImGui::PopTextWrapPos();
    }

    // The Lens section (with Features_LensFlare): a lens to start from (a fisheye, 14 to 135 mm), its focal length and
    // how it bends the picture, its fringes and corners
    void DrawLens()
    {
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare))
            return;
        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_CIRCLE_DOT "  Lens", "lens"))
            return;
        namespace Lens = Components::Lens;
        auto settings = Lens::GetSettings();

        const auto label = [](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", text);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        const auto slider = [&](const char* text, const char* id, float* value, float min, float max,
                                const char* format = "%.2f", ImGuiSliderFlags flags = 0) {
            label(text);
            ImGui::SliderFloat(id, value, min, max, format, ImGuiSliderFlags_AlwaysClamp | flags);
        };

        ImGui::Checkbox("Enable##lens", &settings.enabled);
        ImGui::SetItemTooltip("The editor's cameras see through this lens (not the game's own view)");
        ImGui::BeginDisabled(!settings.enabled);

        // the lenses as buttons, two rows
        const auto presets = Lens::GetPresets();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int perRow = 5;
        const float buttonWidth = (ImGui::GetContentRegionAvail().x - spacing * (perRow - 1)) / perRow;
        for (std::size_t i = 0; i < presets.size(); ++i)
        {
            if (i % perRow != 0)
                ImGui::SameLine();
            const bool chosen = presets[i].projection == settings.projection &&
                                (presets[i].projection == Lens::Projection::Fisheye ||
                                 std::abs(presets[i].focalLength - settings.focalLength) < 0.5f);
            if (chosen)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(std::format("{}##lensPreset{}", presets[i].name, i).c_str(), ImVec2(buttonWidth, 0)))
                settings = Lens::ApplyPreset(settings, i);
            if (chosen)
                ImGui::PopStyleColor();
        }

        label("Kind");
        constexpr std::array PROJECTIONS = {"Rectilinear", "Fisheye"};
        int projection = static_cast<int>(settings.projection);
        if (ImGui::Combo("##lensProjection", &projection, PROJECTIONS.data(), static_cast<int>(PROJECTIONS.size())))
            settings.projection = static_cast<Lens::Projection>(projection);
        ImGui::SetItemTooltip("Rectilinear keeps straight lines straight (most lenses); a fisheye bends the whole "
                              "picture round, for a very wide view");

        if (settings.projection == Lens::Projection::Rectilinear)
        {
            slider("Focal Length", "##lensFocal", &settings.focalLength, 8, 300,
                   std::format("%.0f mm ({:.0f}\xC2\xB0)", Lens::FovOf(settings.focalLength)).c_str(),
                   ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("On a full frame camera: shorter is wider, longer zooms in");
            ImGui::Checkbox("Sets the Field of View##lensSetsFov", &settings.setsFov);
            ImGui::SetItemTooltip("The field of view from the focal length, in place of the camera's own (its keys); "
                                  "off, the lens only bends and marks the picture");
            slider("Distortion", "##lensDistortion", &settings.distortion, -1, 1);
            ImGui::SetItemTooltip("Above 0 barrel (wide lenses: the middle bulges), below 0 pincushion (long lenses)");
        }
        else
        {
            slider("Field of View", "##lensFisheyeFov", &settings.fisheyeFov, 100, 220, "%.0f\xC2\xB0");
            ImGui::SetItemTooltip("How much the fisheye sees across the picture (or the circle). The game draws at "
                                  "most 160\xC2\xB0, so past that the edges go black");
            ImGui::Checkbox("Circular##lensCircular", &settings.circular);
            ImGui::SetItemTooltip("The whole circle of the fisheye in the picture, black round it");
        }

        slider("Color Fringing", "##lensChromatic", &settings.chromatic, 0, 1);
        ImGui::SetItemTooltip("Red and blue edges toward the sides of the picture (chromatic aberration)");
        slider("Vignette", "##lensVignette", &settings.vignette, 0, 1);
        ImGui::SetItemTooltip("Darker corners");
        ImGui::Checkbox("Depth of Field uses this lens##lensDof", &settings.depthOfField);
        ImGui::SetItemTooltip("The Depth of Field's focal length is this lens's, so the blur matches it");

        ImGui::EndDisabled();
        Lens::SetSettings(settings);
    }

    // The Camera Shake section (Features_CameraShake): its style, how strong and fast, and how much it turns and moves;
    // Intensity, Rotation and Position are keyed on the timeline
    void DrawCameraShake()
    {
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_CameraShake))
            return;
        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_WAVE_SQUARE "  Camera Shake", "camerashake"))
            return;
        using namespace Components;
        auto settings = CameraShake::GetSettings();

        ImGui::Checkbox("Enable##cameraShake", &settings.enabled);
        ImGui::SetItemTooltip("Shakes whichever camera is used, by the film's time: a recording shakes the same each "
                              "time");
        ImGui::BeginDisabled(!settings.enabled);

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Style");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##cameraShakeStyle", CameraShake::GetStyleName(settings.style)))
        {
            for (int style = 0; style < static_cast<int>(CameraShake::Style::Count); ++style)
            {
                const auto value = static_cast<CameraShake::Style>(style);
                if (ImGui::Selectable(CameraShake::GetStyleName(value), value == settings.style))
                    settings.style = value;
            }
            ImGui::EndCombo();
        }

        ImGuiEx::Keyframeable::SliderFloat("Intensity", &settings.intensity, 0, 3,
                                           Types::KeyframeablePropertyType::CameraShakeIntensity, "%.2f",
                                           {.resetValue = 1.0f});
        ImGui::SetItemTooltip("Key it from 0 up and back for a hit");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Speed");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##cameraShakeSpeed", &settings.speed, 0.1f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How fast it shakes, times the style's");
        ImGuiEx::Keyframeable::SliderFloat("Rotation", &settings.rotation, 0, 3,
                                           Types::KeyframeablePropertyType::CameraShakeRotation, "%.2f",
                                           {.resetValue = 1.0f});
        ImGui::SetItemTooltip("How much it turns, times the style's");
        ImGuiEx::Keyframeable::SliderFloat("Position", &settings.position, 0, 3,
                                           Types::KeyframeablePropertyType::CameraShakePosition, "%.2f",
                                           {.resetValue = 1.0f});
        ImGui::SetItemTooltip("How much it moves, times the style's");

        ImGui::EndDisabled();
        CameraShake::SetSettings(settings);
    }

    // The Lens Dirt section (with Features_LensFlare): its kind (made, or an image), how much and where it's lit, and
    // how it looks
    void DrawLensDirt()
    {
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare))
            return;
        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_DROPLET "  Lens Dirt", "lensdirt"))
            return;
        namespace Dirt = Components::LensDirt;
        auto settings = Dirt::GetSettings();

        const auto label = [](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", text);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        const auto slider = [&](const char* text, const char* id, float* value, float min, float max,
                                const char* format = "%.2f") {
            label(text);
            ImGui::SliderFloat(id, value, min, max, format, ImGuiSliderFlags_AlwaysClamp);
        };
        const auto heading = [](const char* text) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("%s", text);
        };

        ImGui::Checkbox("Enable##lensDirt", &settings.enabled);
        ImGui::SetItemTooltip("Dirt on the lens, seen where bright light catches it");
        ImGui::BeginDisabled(!settings.enabled);

        label("Dirt");
        if (ImGui::BeginCombo("##lensDirtStyle", Dirt::GetStyleName(settings.style)))
        {
            for (int style = 0; style < static_cast<int>(Dirt::Style::Count); ++style)
            {
                const auto value = static_cast<Dirt::Style>(style);
                if (ImGui::Selectable(Dirt::GetStyleName(value), value == settings.style))
                    settings.style = value;
            }
            ImGui::EndCombo();
        }
        if (settings.style == Dirt::Style::Image)
        {
            label("Image");
            const auto name = settings.imagePath.empty()
                                  ? std::string("Choose...")
                                  : std::filesystem::path(settings.imagePath).filename().string();
            if (ImGui::Button(std::format("{}##lensDirtImage", name).c_str(), ImVec2(-FLT_MIN, 0)))
            {
                if (const auto path = PathUtils::OpenFileDialog(
                        false, OFN_EXPLORER | OFN_FILEMUSTEXIST,
                        "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.dds\0All Files\0*.*\0", "png"))
                    settings.imagePath = path->string();
            }
            ImGui::SetItemTooltip("%s", settings.imagePath.empty()
                                            ? "A lens dirt texture: the dirt light on black (it fills the picture)"
                                            : settings.imagePath.c_str());
            slider("Rotation", "##lensDirtRotation", &settings.imageRotation, -180, 180, "%.0f\xC2\xB0");
            slider("Scale", "##lensDirtScale", &settings.imageScale, 0.25f, 4, "%.2fx");
            ImGui::SetItemTooltip("1 fills the picture; smaller leaves black (no dirt) round it");
        }

        label("Blend Mode");
        if (ImGui::BeginCombo("##lensDirtBlend", Dirt::GetBlendName(settings.blend)))
        {
            for (int blend = 0; blend < static_cast<int>(Dirt::Blend::Count); ++blend)
            {
                const auto value = static_cast<Dirt::Blend>(blend);
                if (ImGui::Selectable(Dirt::GetBlendName(value), value == settings.blend))
                    settings.blend = value;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("How the dirt goes over the picture: Add and Screen brighten (Screen more gently), "
                              "Lighten only where the dirt is brighter, Overlay and Soft Light add contrast, Color Dodge "
                              "burns bright, Multiply darkens (grime)");

        slider("Amount", "##lensDirtAmount", &settings.amount, 0, 4);
        ImGui::SetItemTooltip("How bright the dirt is where it's lit");
        slider("Threshold", "##lensDirtThreshold", &settings.threshold, 0, 0.98f);
        ImGui::SetItemTooltip("How bright the picture is before it lights the dirt: lower, and more lights it");
        slider("Spread", "##lensDirtSpread", &settings.spread, 0, 1);
        ImGui::SetItemTooltip("How far from a bright spot the dirt is lit");
        label("Tint");
        ImGui::ColorEdit3("##lensDirtTint", glm::value_ptr(settings.tint), ImGuiColorEditFlags_Float);

        if (settings.style != Dirt::Style::Image)
        {
            heading("Look");
            slider("Density", "##lensDirtDensity", &settings.density, 0, 1);
            ImGui::SetItemTooltip("How much dirt there is");
            slider("Size", "##lensDirtSize", &settings.size, 0.3f, 3, "%.2fx");
            slider("Softness", "##lensDirtSoftness", &settings.softness, 0, 1);
            ImGui::SetItemTooltip("How blurred the dirt is (it's out of focus, on the lens)");
            label("Pattern");
            ImGui::SliderInt("##lensDirtSeed", &settings.seed, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Another scattering of the same dirt");
        }
        else
        {
            slider("Softness", "##lensDirtSoftness", &settings.softness, 0, 1);
            ImGui::SetItemTooltip("How blurred the image is");
        }

        heading("Lens Flare");
        slider("Flare Light", "##lensDirtFlare", &settings.flareLight, 0, 3);
        ImGui::SetItemTooltip("How much the Lens Flare's light lights the dirt around it (none while the flare's off "
                              "or hidden; Bright Lights' flares light it as bright parts do)");
        slider("Flare Spread", "##lensDirtFlareSpread", &settings.flareSpread, 0.05f, 1.5f);
        ImGui::SetItemTooltip("How far from the flare's light the dirt is lit");

        ImGui::EndDisabled();
        Dirt::SetSettings(settings);
    }

    // The Lens Flare section (Features_LensFlare): where its light is (the sun, or a pin picked on the picture), a
    // look to start from, and each part's amount, size and colour; Intensity and the pin are keyed on the timeline
    void DrawLensFlare()
    {
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare))
            return;
        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_SUN "  Lens Flare", "lensflare"))
            return;
        using namespace Components;
        using Type = Types::KeyframeablePropertyType;
        flareRowsShownAt = ImGui::GetFrameCount();
        auto settings = LensFlare::GetSettings();

        const auto label = [](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", text);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        const auto slider = [&](const char* text, const char* id, float* value, float min, float max,
                                const char* format = "%.2f", ImGuiSliderFlags flags = 0) {
            label(text);
            ImGui::SliderFloat(id, value, min, max, format, ImGuiSliderFlags_AlwaysClamp | flags);
        };
        const auto heading = [](const char* text) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("%s", text);
        };

        ImGui::Checkbox("Enable##lensFlare", &settings.enabled);
        ImGui::BeginDisabled(!settings.enabled);

        label("Light");
        constexpr std::array SOURCES = {"The sun", "A spot of the map", "Bright lights (auto)"};
        int source = static_cast<int>(settings.source);
        if (ImGui::Combo("##lensFlareSource", &source, SOURCES.data(), static_cast<int>(SOURCES.size())))
            settings.source = static_cast<LensFlare::Source>(source);
        if (const auto picked = flarePicker.Poll())
            settings.track = *picked;
        if (settings.source == LensFlare::Source::Auto)
        {
            label("Lights");
            ImGui::SliderInt("##flareAutoLights", &settings.autoLights, 1, 8, "up to %d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Each frame the brightest lights on the picture get a flare: the sun, lamps, "
                                  "windows, muzzle flashes...");
            slider("Brightness", "##flareAutoThreshold", &settings.autoThreshold, 0.5f, 0.99f);
            ImGui::SetItemTooltip("How bright something must be to get a flare: higher, only the brightest lights");
            slider("Spread", "##flareAutoSeparation", &settings.autoSeparation, 0.02f, 0.5f);
            ImGui::SetItemTooltip("How far apart two lights are at least (so one big light gets one flare)");
        }
        else if (settings.source == LensFlare::Source::Tracked)
        {
            // (a spot of the map only: a flare doesn't follow players)
            if (settings.track.kind == Types::TrackKind::Target)
                settings.track = {};
            if (ImGui::Button(pickingFlare             ? ICON_FA_CROSSHAIRS " Click the Picture..."
                              : flarePicker.IsWaiting() ? ICON_FA_CROSSHAIRS " Reading the depth..."
                                                        : ICON_FA_CROSSHAIRS " Pick on Picture##flareTrack"))
                pickingFlare = !pickingFlare;
            ImGui::SetItemTooltip("Then click a light of the map: the flare stays on it as the camera moves (Escape "
                                  "cancels)");
            ImGui::SameLine();
            if (settings.track.kind == Types::TrackKind::None)
                ImGui::TextDisabled("(none picked yet)");
            else if (!Components::Tracking::Project(settings.track))
                ImGui::TextDisabled("(not in view)");
            ImGui::Checkbox("Hide behind things##lensFlare", &settings.hideBehind);
            ImGui::SetItemTooltip("Fades out as walls, players and the like come in front of the spot");
        }
        else
        {
            ImGui::Checkbox("Hide behind things##lensFlare", &settings.hideBehind);
            ImGui::SetItemTooltip("Fades out as walls, players and the like come in front of the sun");
        }

        label("Look");
        if (ImGui::BeginCombo("##lensFlarePreset", "Start from..."))
        {
            const auto presets = LensFlare::GetPresetNames();
            for (std::size_t i = 0; i < presets.size(); ++i)
                if (ImGui::Selectable(presets[i]))
                    settings = LensFlare::ApplyPreset(settings, i);
            ImGui::EndCombo();
        }

        ImGuiEx::Keyframeable::SliderFloat("Intensity", &settings.intensity, 0, 3, Type::LensFlareIntensity, "%.2f",
                                           {.resetValue = 1.0f});
        label("Color");
        ImGui::ColorEdit3("##lensFlareColor", glm::value_ptr(settings.color), ImGuiColorEditFlags_Float);

        heading("Motion");
        slider("Rotation", "##flareRotation", &settings.rotation, -180, 180, "%.0f\xC2\xB0");
        ImGui::SetItemTooltip("Turns the whole flare about its light: the rays, the streak and the line of ghosts");
        slider("Spin", "##flareSpin", &settings.spin, -90, 90, "%.0f\xC2\xB0/s");
        ImGui::SetItemTooltip("Keeps turning it as the film plays (degrees a second)");
        slider("Flicker", "##flareFlicker", &settings.flicker, 0, 1);
        ImGui::SetItemTooltip("How much its brightness flickers (each auto light its own)");
        slider("Flicker Speed", "##flareFlickerSpeed", &settings.flickerSpeed, 0.5f, 30, "%.1f Hz");

        heading("Depth");
        // how far the light is now, so what these do can be seen (they change the flare as the camera moves closer
        // or away)
        if (settings.source == LensFlare::Source::Tracked)
        {
            if (const auto frame = LensFlare::GetFrame(Playback::GetTimelineTick()); frame && frame->light)
                ImGui::TextDisabled("The spot is %.0f units away (a flare's own size at 500)", frame->lightDepth);
        }
        else if (settings.source == LensFlare::Source::Sun)
        {
            ImGui::TextDisabled("The sun is as far as it gets: a Max Distance hides it");
        }
        else
        {
            ImGui::TextDisabled("Each light's distance is read from the picture");
        }
        slider("Max Distance", "##flareMaxDistance", &settings.maxDistance, 0, 10000,
               settings.maxDistance <= 0.0f ? "No limit" : "%.0f units", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Lights farther than this fade out (from 70% of it), so a flare fades as the camera "
                              "moves away from its light");
        slider("Size by Distance", "##flareSizeByDistance", &settings.sizeByDistance, 0, 1);
        ImGui::SetItemTooltip("Lights nearer than 500 units get bigger flares, farther ones smaller (to 2x and a "
                              "quarter): the flare grows as the camera comes closer");
        ImGui::Checkbox("Blurred by Depth of Field##lensFlare", &settings.blurredByDepthOfField);
        ImGui::SetItemTooltip("The Look tab's depth of field blurs the flare as it blurs what's around the light");

        heading("Glow");
        slider("Amount", "##flareGlow", &settings.glow, 0, 2);
        slider("Size", "##flareGlowSize", &settings.glowSize, 0.01f, 0.6f, "%.3f");

        heading("Rays");
        label("Count");
        ImGui::SliderInt("##flareRays", &settings.rays, 0, 16, settings.rays == 0 ? "None" : "%d",
                         ImGuiSliderFlags_AlwaysClamp);
        slider("Amount", "##flareRayAmount", &settings.rayAmount, 0, 2);
        slider("Length", "##flareRayLength", &settings.rayLength, 0.05f, 1.5f);
        slider("Thinness", "##flareRaySharp", &settings.raySharpness, 0, 1);
        slider("Rotation", "##flareRayRotation", &settings.rayRotation, -180, 180, "%.0f\xC2\xB0");
        slider("Rainbow", "##flareRayRainbow", &settings.rayRainbow, 0, 1);
        ImGui::SetItemTooltip("Each ray its own colour");

        heading("Streak");
        slider("Amount", "##flareStreak", &settings.streak, 0, 2);
        slider("Length", "##flareStreakLength", &settings.streakLength, 0.1f, 2.5f);
        label("Color");
        ImGui::ColorEdit3("##flareStreakColor", glm::value_ptr(settings.streakColor), ImGuiColorEditFlags_Float);

        heading("Ghosts");
        label("Count");
        ImGui::SliderInt("##flareGhosts", &settings.ghosts, 0, 16, settings.ghosts == 0 ? "None" : "%d",
                         ImGuiSliderFlags_AlwaysClamp);
        label("Shape");
        constexpr std::array SHAPES = {"Round", "Hexagon", "Octagon"};
        int shape = static_cast<int>(settings.ghostShape);
        if (ImGui::Combo("##flareGhostShape", &shape, SHAPES.data(), static_cast<int>(SHAPES.size())))
            settings.ghostShape = static_cast<LensFlare::GhostShape>(shape);
        slider("Amount", "##flareGhostAmount", &settings.ghostAmount, 0, 1.5f);
        slider("Size", "##flareGhostSize", &settings.ghostSize, 0.01f, 0.25f, "%.3f");
        slider("Spacing", "##flareGhostSpacing", &settings.ghostSpacing, 0.05f, 0.6f);
        slider("Rainbow", "##flareGhostRainbow", &settings.ghostRainbow, 0, 1);
        slider("Rings", "##flareGhostRing", &settings.ghostRing, 0, 1);
        ImGui::SetItemTooltip("0 filled discs, 1 only their rims (rings)");

        heading("Arcs");
        label("Count");
        ImGui::SliderInt("##flareArcs", &settings.arcs, 0, 6, settings.arcs == 0 ? "None" : "%d",
                         ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Crescents of big rings beyond the picture's centre, as an old lens's");
        slider("Amount", "##flareArcAmount", &settings.arcAmount, 0, 1.5f);
        slider("Size", "##flareArcSize", &settings.arcSize, 0.1f, 0.8f);

        heading("Halo");
        slider("Amount", "##flareHalo", &settings.halo, 0, 1.5f);
        slider("Radius", "##flareHaloRadius", &settings.haloRadius, 0.05f, 0.8f);
        slider("Centre", "##flareHaloCentre", &settings.haloCenter, 0, 1);
        ImGui::SetItemTooltip("Where the ring is centred: 0 on the light, 1 on the picture's centre");
        slider("Rainbow", "##flareHaloRainbow", &settings.haloRainbow, 0, 1);

        ImGui::EndDisabled();
        LensFlare::SetSettings(settings);
    }

    void CameraMenu::Render()
    {
        // (scrolls: the node list can make it longer than the panel)
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Controls...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Camera", NULL, flags);

        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
        {
            UI::DrawInaccessibleTabWarning();
            ImGui::End();
            return;
        }

        auto& cameraManager = Components::CameraManager::Get();

        if (ImGuiEx::Keyframeable::PanelSection(ICON_FA_CAMERA "  Camera", "camera"))
        {
            DrawModes();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));

            switch (cameraManager.GetActiveCamera()->GetMode())
            {
                case Components::Camera::Mode::FirstPerson:
                    DrawFirstPersonSettings();
                    break;
                case Components::Camera::Mode::ThirdPerson:
                    DrawThirdPersonSettings();
                    break;
                case Components::Camera::Mode::Free:
                    DrawFreeCameraSettings();
                    break;
                case Components::Camera::Mode::Orbit:
                    DrawOrbitCameraSettings();
                    break;
                case Components::Camera::Mode::Bone:
                    DrawBoneCameraSettings();
                    break;
                case Components::Camera::Mode::Dolly:
                    DrawDollycamSettings();
                    break;
                default:
                    break;
            }
        }

        DrawLens();
        DrawCampath();
        DrawAutoEase();
        DrawCameraShake();
        DrawLensFlare();
        DrawLensDirt();

        ImGui::End();
    }

    void CameraMenu::Release()
    {
    }
}  // namespace IWXMVM::UI