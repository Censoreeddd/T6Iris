#include "StdInclude.hpp"
#include "GettingStarted.hpp"

#include "UI/Theme.hpp"

#include "Mod.hpp"
#include "Resources.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/KillFinder.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/Commands.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/Toasts.hpp"
#include "UI/UIManager.hpp"

namespace IWXMVM::UI::GettingStarted
{
    using GettingStartedLogic::Step;

    namespace
    {
        // the jumps to kills there had been when the steps were last reset
        std::uint32_t jumpsBefore = 0;

        bool IsInDemo()
        {
            return Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        }
    }  // namespace

    GettingStartedLogic::State GetState()
    {
        const auto& preferences = PreferencesConfiguration::Get();
        GettingStartedLogic::State state;
        state.hidden = preferences.gettingStartedHidden;
        return GettingStartedLogic::WithDoneNames(state, preferences.gettingStartedDone);
    }

    void SetState(const GettingStartedLogic::State& state)
    {
        auto& preferences = PreferencesConfiguration::Get();
        preferences.gettingStartedDone = GettingStartedLogic::GetDoneNames(state);
        preferences.gettingStartedHidden = state.hidden;
        Configuration::Get().Write(true);
    }

    void RegisterCommands()
    {
        Commands::Register({
            .id = "help.gettingStarted",
            .label = "Getting started",
            .category = "Help",
            .run = Show,
            .keywords = "first run tutorial steps card help",
        });
    }

    void Update()
    {
        const auto state = GetState();
        GettingStartedLogic::Seen seen;
        seen.inDemo = IsInDemo();
        seen.jumpedToKill = Components::KillFinder::GetJumps() > jumpsBefore;
        if (seen.inDemo)
        {
            auto& keyframeManager = Components::KeyframeManager::Get();
            seen.nodes = keyframeManager
                             .GetKeyframes(keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera))
                             .size();
        }
        seen.recording = Components::CaptureManager::Get().IsCapturing();
        const auto updated = GettingStartedLogic::Update(state, seen);
        if (updated == state)
            return;
        // (kept, quietly: the card is a short tour of the tool now, not steps to tick off)
        SetState(updated);
    }

    void Reset()
    {
        jumpsBefore = Components::KillFinder::GetJumps();
        SetState({});
    }

    void Show()
    {
        auto state = GetState();
        state.hidden = false;
        SetState(state);
        UIManager::Get().SelectTab(Tab::Demo, Page::Browse);
    }

    void DrawCard()
    {
        auto state = GetState();
        if (state.hidden)
            return;

        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ImVec2(fontSize * 0.6f, fontSize * 0.5f);
        const auto start = ImGui::GetCursorScreenPos();
        const auto width = ImGui::GetContentRegionAvail().x;
        auto drawList = ImGui::GetWindowDrawList();
        // (its background goes under it once its height is known)
        drawList->ChannelsSplit(2);
        drawList->ChannelsSetCurrent(1);

        ImGui::PushID("gettingStarted");
        ImGui::SetCursorScreenPos(start + padding);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - padding.x * 2);

        ImGui::PushFont(UIManager::Get().GetBoldFont());
        ImGui::TextUnformatted("Welcome to T6Iris");
        ImGui::PopFont();
        ImGui::SameLine();
        const auto closeSize = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - padding.x - closeSize, ImGui::GetCursorScreenPos().y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(ICON_FA_XMARK "##close", ImVec2(closeSize, 0)))
        {
            state.hidden = true;
            SetState(state);
            Toasts::Show("Help > Getting started shows it again");
        }
        ImGui::PopStyleColor();
        ImGuiEx::Keyframeable::Record("firstrun.close");
        ImGui::SetItemTooltip("Close it (Help > Getting started shows it again)");
        ImGui::TextDisabled("What's in the tool, in short. Click one to open its tab.");

        // the tour: each tab, what it's for in a line, a click opening it
        const auto key = [](std::string_view id) {
            const auto shortcut = Commands::GetShortcut(id);
            return shortcut.empty() ? std::string("its key") : shortcut;
        };
        struct Feature
        {
            const char* icon;
            const char* name;
            std::string line;
            std::optional<Tab> tab;
        };
        const std::array<Feature, 6> features = {{
            {ICON_FA_FILM, "Demo", "Pick a film and press Play.", Tab::Demo},
            {ICON_FA_VIDEO, "Camera",
             std::format("Free, Dolly and Bone cameras. Fly, then press {} to add campath nodes. Lens, shake, flares.",
                         key("campath.addNode")),
             Tab::Camera},
            {ICON_FA_SLIDERS, "Look", "Colour, depth of field, sky and fog, and your own game .cfg files.", Tab::Look},
            {ICON_FA_PERSON, "Scene", "Actors, killstreaks, props and effects, with animations and shots.", Tab::Scene},
            {ICON_FA_CIRCLE, "Record",
             std::format("Set the range with {} and {}, then record video or multi-pass streams.",
                         key("capture.setStart"), key("capture.setEnd")),
             Tab::Record},
            {ICON_FA_KEYBOARD, "Keys",
             std::format("{} hides the editor; ? lists every shortcut.", key("view.toggleOverlay")), std::nullopt},
        }};
        ImGui::Dummy(ImVec2(0, fontSize * 0.15f));
        for (const auto& feature : features)
        {
            ImGui::PushID(feature.name);
            const auto rowStart = ImGui::GetCursorScreenPos();
            ImGui::BeginGroup();
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", feature.icon);
            ImGui::SameLine(fontSize * 1.6f);
            ImGui::BeginGroup();
            ImGui::PushFont(UIManager::Get().GetBoldFont());
            ImGui::TextUnformatted(feature.name);
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", feature.line.c_str());
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            ImGui::EndGroup();
            if (feature.tab)
            {
                const auto rowEnd = ImGui::GetItemRectMax();
                if (ImGui::IsMouseHoveringRect(rowStart, ImVec2(start.x + width - padding.x, rowEnd.y)))
                {
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(rowStart.x - padding.x * 0.4f, rowStart.y - 2),
                        ImVec2(start.x + width - padding.x * 0.6f, rowEnd.y + 2),
                        ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.5f), fontSize * 0.3f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        UIManager::Get().SelectTab(*feature.tab);
                }
            }
            ImGui::PopID();
        }

        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();

        const auto end = ImVec2(start.x + width, ImGui::GetItemRectMax().y + padding.y);
        drawList->ChannelsSetCurrent(0);
        drawList->AddRectFilled(start, end, Theme::Card(), fontSize * 0.4f);
        drawList->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border), fontSize * 0.4f);
        drawList->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(start.x, end.y));
        ImGui::Dummy(ImVec2(width, ImGui::GetStyle().ItemSpacing.y));
    }
}  // namespace IWXMVM::UI::GettingStarted
