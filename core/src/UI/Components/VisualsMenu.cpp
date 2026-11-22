#include "StdInclude.hpp"
#include "UI/ImGuiEx/ImGuiExtensions.hpp"
#include "VisualsMenu.hpp"
#include "Mod.hpp"
#include "D3D11.hpp"
#include "UI/TrackTarget.hpp"
#include "Components/SkyReplace.hpp"
#include "Utilities/PathUtils.hpp"
#include <sstream>
#include "Components/KeyframeSerializer.hpp"
#include "Components/Tracking.hpp"
#include "Components/Lens.hpp"
#include "Utilities/MathUtils.hpp"

#include "UI/UIManager.hpp"
#include "UI/Components/GameView.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/Toasts.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/Playback.hpp"
#include "Components/UndoHistory.hpp"
#include "Components/KeyframeLogic.hpp"
#include "Components/ReShadeKeys.hpp"
#include "Components/RuntimeProperties.hpp"
#include "Components/SurfaceDepth.hpp"
#include "Components/SurfaceLogic.hpp"
#include "Components/TexturePack.hpp"
#include "UI/Theme.hpp"
#include "Events.hpp"
#include "Resources.hpp"
#include "Utilities/PathUtils.hpp"
#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

namespace IWXMVM::UI
{
    constexpr std::size_t MAX_RECENT_PRESETS = 10;
    // where the tab's controls start, a fraction of its width: wide enough for Shadow distance
    constexpr float LOOK_COLUMN = 0.45f;

    using Settings = Components::VisualConfiguration::Settings;
    using Property = Types::KeyframeablePropertyType;

    namespace
    {
        void SetMapValues(Settings& settings, const Types::MapVisuals& map)
        {
            settings.dof = map.dof;
            settings.sunColor = map.sun.color;
            settings.sunDirection = map.sun.direction;
            settings.sunBrightness = map.sun.brightness;
            settings.skyRotation = map.sun.skyRotation;
            settings.filmtweaks = map.filmtweaks;
        }
    }  // namespace

    void VisualsMenu::Initialize()
    {
        for (const auto& path : PreferencesConfiguration::Get().recentVisualPresets)
        {
            // (config.json can have one more than once: AddPresetToRecent took out only the first)
            if (std::filesystem::exists(path) &&
                std::ranges::find(recentPresets, path, &Preset::path) == recentPresets.end())
                recentPresets.push_back({path.filename().string(), path});
        }

        Events::RegisterListener(EventType::OnRenderGameView, [this]() {
            DrawFocusPin();
            DrawSunAim();
        });

        quality = Components::QualityLogic::FromJson(PreferencesConfiguration::Get().quality);
        Components::TexturePack::SetOn(quality.texturePack);

        // the map effects hidden on each level, as config.json keeps them
        for (const auto& [level, hidden] : PreferencesConfiguration::Get().hiddenMapEffects.items())
            if (hidden.is_array())
                Mod::GetGameInterface()->SetHiddenMapEffects(level, hidden.get<std::set<std::string>>());

        static bool justLoadedDemo = false;
        // The scene's Shadow quality restarts the renderer, whose loading screen keeps the window's picture around the
        // game view: some frames into the demo, so the picture has the timeline, not the game under it
        static constexpr std::int32_t SCENE_SHADOWS_FRAMES = 10;
        static std::int32_t framesToSceneShadows = 0;

        Events::RegisterListener(EventType::PostDemoLoad, [&]() {
            justLoadedDemo = true;
        });

        Events::RegisterListener(EventType::OnFrame, [&]() {
            const bool inDemo = IWXMVM::Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
            if (inDemo && framesToSceneShadows > 0 && --framesToSceneShadows == 0)
            {
                // (some frames in: right after a demo's command the game can still say it's in the last demo)
                ApplySceneShadows();
                TakeMapDefaults();
            }

            if (inDemo && justLoadedDemo)
            {
                justLoadedDemo = false;
                framesToSceneShadows = SCENE_SHADOWS_FRAMES;
                keptReShadeEffects.clear();

                if (visualsInitialized)
                    return;

                auto sun = Mod::GetGameInterface()->GetSun();
                auto dof = Mod::GetGameInterface()->GetDof();
                auto filmtweaks = Mod::GetGameInterface()->GetFilmtweaks();
                auto hudInfo = Mod::GetGameInterface()->GetHudInfo();

                visuals = {dof, sun.color, sun.direction, sun.brightness, filmtweaks, hudInfo};
                visuals.skyRotation = sun.skyRotation;
                // (the demo's own reach, as the map is now)
                const auto& game = Mod::GetGameInterface();
                const auto size = game->GetSunShadowMapSize();
                if (const auto dvar = game->GetDvar("sm_sunSampleSizeNear");
                    dvar && size && game->GetSupportedFeatures() & Types::Features_ShadowQuality)
                    visuals.shadowDistance =
                        Components::QualityLogic::GetShadowDistance(dvar->value->floating_point, *size);

                // We do this once to force r_dof_enable and r_dof_tweak
                // into sync with each other
                UpdateDof();

                defaultVisuals = visuals;
                if (const auto map = Mod::GetGameInterface()->GetMapVisuals())
                    SetMapValues(defaultVisuals, *map);
                defaultPreset = {"Default"};
                currentPreset = defaultPreset;

                visualsInitialized = true;
            }
		});

        // keyed visuals follow the timeline, those the tab doesn't show too
        Events::RegisterListener(EventType::OnFrame, [&]() {
            // (an edit in a control ends when no control is active, drawn or not)
            ImGuiEx::Keyframeable::EndEditWhenIdle();
            EndEditWhenIdle();
            Components::ReShadeKeys::EndPresetEditWhenIdle(ImGui::IsAnyItemActive() ||
                                                           ImGui::IsMouseDown(ImGuiMouseButton_Left));
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;

            // a project's depth of field, once the tab has its map's settings
            if (pendingDepthOfField && visualsInitialized)
            {
                for (const auto& [dvar, values] : pendingDepthOfField->items())
                {
                    const auto setting = Components::VisualsLogic::FindSetting(dvar);
                    if (!setting || !values.is_array() || values.size() < setting->valueCount)
                        continue;
                    std::array<float, 4> read{};
                    for (std::size_t i = 0; i < setting->valueCount && i < read.size(); ++i)
                        read[i] = values[i].is_number() ? values[i].get<float>() : 0.0f;
                    setting->read(visuals, read.data());
                }
                pendingDepthOfField.reset();
                UpdateDof();
            }

            ApplyKeyed();
            UpdateShadows();
            UpdateSurfaces();
            // as RenderHud does: the game brings parts of the HUD back now and then
            if (UIManager::Get().GetSelectedTab() != Tab::Look && !UIManager::Get().IsOverlayHidden())
                UpdateHudInfo();
        });

        // a key added on the timeline to a property without keys takes the value it is set to here
        Components::KeyframeManager::Get().AddUnkeyedValues(
            [this](Types::KeyframeablePropertyType type) -> std::optional<Types::KeyframeValue> {
                if (Components::RuntimeProperties::IsActorProperty(type))
                    return std::nullopt;
                if (Components::RuntimeProperties::IsRuntime(type))
                    return Components::ReShadeKeys::GetPresetKey(type);
                if (!visualsInitialized)
                    return std::nullopt;
                return Components::VisualsLogic::GetValue(visuals, type);
            });
    }

    void VisualsMenu::Render()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("Visuals", NULL, flags))
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                UI::DrawInaccessibleTabWarning();
                ImGui::End();
                return;
            }

            // (an older project's directional or radial blur, kept as the depth of field's type, is the Blur
            // section's now, when it isn't keyed there: the game's depth of field was off with it, as it stays)
            auto& keyframeManager = Components::KeyframeManager::Get();
            if (visuals.dof.HasOlderBlurType() &&
                keyframeManager
                    .GetKeyframes(keyframeManager.GetProperty(Types::KeyframeablePropertyType::DepthOfFieldBlurType))
                    .empty())
            {
                visuals.dof.screenBlur = static_cast<float>(visuals.dof.GetScreenBlur());
                if (visuals.dof.screenBlur != 0.0f)
                    visuals.dof.enabled = false;
                visuals.dof.blurType = static_cast<float>(Types::DoF::BLUR_DEPTH);
                UpdateDof();
            }

            // what the controls change is an edit, a step to undo
            const auto frameStart = visuals;
            const auto presetBefore = currentPreset;

            const ImGuiEx::Keyframeable::LabelColumn labels(LOOK_COLUMN);
            ShowRequestedRow();
            RenderConfigSection();
            RenderHud();
            RenderMapEffects();
            RenderSun();
            RenderQuality();
            RenderFilmtweaks();
            // (no ReShade section: not used here, RenderReShade kept for another build)
            RenderDOF();
            RenderBlur();
            RenderSky();

            TrackEdit(frameStart, presetBefore);
            EndEditWhenIdle();

            ImGui::End();
        }
    }

    bool VisualsMenu::DrawSection(Components::VisualsLogic::Section section, const char* label,
                                  const char* resetTooltip, bool* reset)
    {
        namespace VisualsLogic = Components::VisualsLogic;
        const auto name = std::string(VisualsLogic::GetSectionName(section));
        auto& names = PreferencesConfiguration::Get().lookSectionsOpen;
        auto sections = VisualsLogic::ReadOpenSections(names);
        const bool wasOpen = sections.contains(section);

        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        sectionTop = ImGui::GetCursorPosY();
        ImGui::SetNextItemOpen(wasOpen);
        const bool open = ImGuiEx::CollapsingHeader(std::format("{}##lookSection{}", label, name).c_str(),
                                                  ImGuiTreeNodeFlags_AllowOverlap);
        ImGuiEx::Keyframeable::Record("visuals.section." + name);
        // (late: a band is where the mouse stays after opening one, over the rows)
        if (ImGui::IsItemHovered() && ImGui::GetCurrentContext()->HoveredIdTimer > 1.0f)
            ImGui::SetTooltip("Alt+click: every section");
        const auto headerMin = ImGui::GetItemRectMin(), headerMax = ImGui::GetItemRectMax();
        // Alt: every section's, as Advanced on the Actors page (a header takes no click with a modifier held)
        const bool alt = ImGui::GetIO().KeyAlt;
        const bool altClicked = alt && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (open != wasOpen || altClicked)
        {
            if (alt)
                sections = wasOpen ? std::set<VisualsLogic::Section>{}
                                   : std::set(VisualsLogic::SECTIONS.begin(), VisualsLogic::SECTIONS.end());
            else if (open)
                sections.insert(section);
            else
                sections.erase(section);
            names = VisualsLogic::WriteOpenSections(sections);
            Configuration::Get().Write(true);
            if (open)
                sectionToShow = section;
        }

        // at the header's right: Reset, and left of it the closed section's marks
        const auto after = ImGui::GetCursorScreenPos();
        const auto& style = ImGui::GetStyle();
        auto right = headerMax.x - style.FramePadding.x;
        const auto middle = (headerMin.y + headerMax.y) / 2.0f;
        if (resetTooltip)
        {
            const char* resetLabel = ICON_FA_REPEAT " Reset";
            const auto height = ImGui::GetFontSize() + 2.0f;
            const auto width = ImGui::CalcTextSize(resetLabel).x + style.FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(right - width, middle - height / 2.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 1.0f));
            ImGui::PushID(name.c_str());
            if (ImGui::Button(resetLabel) && reset)
                *reset = true;
            ImGui::PopID();
            ImGui::PopStyleVar();
            ImGuiEx::Keyframeable::Record(std::format("visuals.reset.{}", name));
            ImGui::SetItemTooltip("%s", resetTooltip);
            right -= width + style.ItemSpacing.x;
        }
        if (!open)
        {
            const auto marks = GetSectionMarks(section);
            const auto size = ImGui::GetFontSize() * 0.55f;
            auto* drawList = ImGui::GetWindowDrawList();
            const auto mark = [&](bool shown, const char* markName, const char* tooltip, auto draw) {
                if (!shown)
                    return;
                const auto center = ImVec2(right - size / 2.0f, middle);
                ImGui::SetCursorScreenPos(ImVec2(right - size, middle - size / 2.0f));
                ImGui::Dummy(ImVec2(size, size));
                ImGuiEx::Keyframeable::Record(std::format("visuals.section.{}.{}", name, markName));
                ImGui::SetItemTooltip("%s", tooltip);
                draw(center);
                right -= size + style.ItemSpacing.x;
            };
            mark(marks.changed, "changed", "Changed", [&](ImVec2 center) {
                drawList->AddCircleFilled(center, size * 0.45f, ImGui::GetColorU32(Theme::Note()));
            });
            mark(marks.keyed, "keyed", "Has keys", [&](ImVec2 center) {
                const auto half = size * 0.6f;
                drawList->AddQuadFilled(ImVec2(center.x, center.y - half), ImVec2(center.x + half, center.y),
                                        ImVec2(center.x, center.y + half), ImVec2(center.x - half, center.y),
                                        ImGui::GetColorU32(ImGuiCol_CheckMark));
            });
        }
        ImGui::SetCursorScreenPos(after);
        return open;
    }

    void VisualsMenu::EndSection(Components::VisualsLogic::Section section)
    {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (sectionToShow != section)
            return;
        sectionToShow.reset();
        // just opened: its rows in view, without scrolling by hand
        const auto bottom = ImGui::GetCursorPosY();
        const auto height = ImGui::GetWindowHeight();
        if (bottom - ImGui::GetScrollY() > height)
            ImGui::SetScrollY(std::min(sectionTop, bottom - height));
    }

    void VisualsMenu::ShowRow(Types::KeyframeablePropertyType type)
    {
        requestedRow = type;
    }

    void VisualsMenu::ShowRequestedRow()
    {
        namespace VisualsLogic = Components::VisualsLogic;
        const auto row = std::exchange(requestedRow, std::nullopt);
        const auto section = row ? VisualsLogic::GetSection(*row) : std::nullopt;
        if (!section)
            return;
        SetSectionOpen(*section, true);
        ImGuiEx::Keyframeable::FlashRow(*row);
        if (*section == VisualsLogic::Section::ReShade)
            if (const auto property = Components::RuntimeProperties::Find(*row))
                reShadeRowToShow = property->id;
    }

    void VisualsMenu::SetSectionOpen(Components::VisualsLogic::Section section, bool open)
    {
        namespace VisualsLogic = Components::VisualsLogic;
        auto& names = PreferencesConfiguration::Get().lookSectionsOpen;
        auto sections = VisualsLogic::ReadOpenSections(names);
        if (open == sections.contains(section))
            return;
        if (open)
            sections.insert(section);
        else
            sections.erase(section);
        names = VisualsLogic::WriteOpenSections(sections);
        Configuration::Get().Write(true);
    }

    void VisualsMenu::RequestPreset(std::optional<std::filesystem::path> path)
    {
        requestedPreset = std::move(path);
    }

    void VisualsMenu::RenderConfigSection()
    {
        // (asked for by the harness, as if chosen here)
        if (const auto requested = std::exchange(requestedPreset, std::nullopt))
        {
            if (requested->has_value())
                LoadPreset({requested->value().filename().string(), requested->value()});
            else
                LoadDefaultPreset();
        }

        const auto saveLabel = ICON_FA_FLOPPY_DISK " Save";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(saveLabel).x -
                                ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::BeginCombo("##configCombo", currentPreset.name.c_str()))
        {
            if (ImGui::Selectable(ICON_FA_CHESS_KING " Default Preset", currentPreset.name == "Default"))
            {
                LoadDefaultPreset();
            }

            ImGui::Separator();


            for (auto& preset : recentPresets)
            {
                auto label = std::string(ICON_FA_ARROW_ROTATE_RIGHT) + " " + preset.name + "##" + preset.path.string();
                if (ImGui::Selectable(label.c_str(), currentPreset.name == preset.name))
                {
                    LoadPreset(preset);
                }
            }

            ImGui::Separator();
            if (ImGui::Selectable(ICON_FA_FOLDER_OPEN " Load from file", false))
            {
                auto path =
                    PathUtils::OpenFileDialog(false, OFN_EXPLORER | OFN_FILEMUSTEXIST, "Config\0*.cfg\0All Files\0*.*\0", "cfg");
                if (path.has_value())
                {
                    Preset preset;
                    preset.name = path.value().filename().string();
                    preset.path = path.value();
                    LoadPreset(preset);
                }
            }

            ImGui::EndCombo();
        }

        ImGui::SameLine();

        if (ImGui::Button(saveLabel))
        {
            auto path = PathUtils::OpenFileDialog(
                true, OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT, "Config\0*.cfg\0", "cfg");
            if (path.has_value())
            {
                Components::VisualConfiguration::Save(path.value(), visuals);
                Preset preset = {path.value().filename().string(), path.value()};
                AddPresetToRecent(preset);
                currentPreset = preset;
            }
        }
        ImGui::SetItemTooltip("Save this tab as a preset");

        RenderGameConfigs();

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Drag to change \xC2\xB7 double-click to type \xC2\xB7 right-click to reset");
        ImGui::PopStyleColor();
    }

    namespace
    {
        // Game .cfgs run from the Visuals tab: the ones picked before (kept in recent_cfgs.txt), the one running now,
        // and the values the dvars they set had before any of them ran, which Default puts back. Picking another
        // puts those back first, so two don't pile up.
        struct GameConfigs
        {
            std::vector<std::filesystem::path> recent;
            std::optional<std::filesystem::path> running;
            std::map<std::string, std::string> before;  // dvar -> its value before
            bool loaded = false;
        };
        GameConfigs gameConfigs;

        std::filesystem::path RecentConfigsFile()
        {
            return PathUtils::GetModPath() / "recent_cfgs.txt";
        }

        void LoadRecentConfigs()
        {
            if (std::exchange(gameConfigs.loaded, true))
                return;
            std::ifstream file(RecentConfigsFile());
            for (std::string line; std::getline(file, line);)
                if (!line.empty() && std::filesystem::exists(std::filesystem::path(line)))
                    gameConfigs.recent.push_back(std::filesystem::path(line));
        }

        void SaveRecentConfigs()
        {
            std::ofstream file(RecentConfigsFile(), std::ios::trunc);
            for (const auto& path : gameConfigs.recent)
                file << path.string() << '\n';
        }

        // the dvars a .cfg sets: each line's first word (or the one after set, seta, sets, setu, setdvar) that is one
        std::vector<std::string> ConfigDvars(std::string_view text)
        {
            std::vector<std::string> dvars;
            for (std::size_t from = 0; from < text.size();)
            {
                auto end = text.find('\n', from);
                if (end == std::string_view::npos)
                    end = text.size();
                std::istringstream line{std::string(text.substr(from, end - from))};
                from = end + 1;
                std::string word;
                if (!(line >> word) || word.starts_with("//") || word.starts_with('#'))
                    continue;
                std::string lower = word;
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == "set" || lower == "seta" || lower == "sets" || lower == "setu" || lower == "setdvar")
                    if (!(line >> word))
                        continue;
                if (word.starts_with('"'))
                    continue;
                if (Mod::GetGameInterface()->GetDvarText(word) && std::ranges::find(dvars, word) == dvars.end())
                    dvars.push_back(word);
            }
            return dvars;
        }

        // the dvars back as they were before any .cfg ran
        void RestoreDefaultConfig()
        {
            std::string text;
            for (const auto& [dvar, value] : gameConfigs.before)
            {
                std::string quoted;
                for (const auto c : value)
                    if (c != '"')
                        quoted += c;
                text += std::format("set {} \"{}\"\n", dvar, quoted);
            }
            if (!text.empty())
                Mod::GetGameInterface()->RunConfig(text);
            LOG_INFO("Game .cfg: {} dvars back to their values before", gameConfigs.before.size());
            gameConfigs.before.clear();
            gameConfigs.running.reset();
        }

        void RunGameConfig(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text.empty())
            {
                Toasts::Show(std::format("Couldn't read {}", path.filename().string()));
                return;
            }
            // (the one running before undone first)
            if (gameConfigs.running)
                RestoreDefaultConfig();
            for (const auto& dvar : ConfigDvars(text))
                if (!gameConfigs.before.contains(dvar))
                    if (const auto value = Mod::GetGameInterface()->GetDvarText(dvar))
                        gameConfigs.before.emplace(dvar, *value);
            if (!Mod::GetGameInterface()->RunConfig(text))
            {
                Toasts::Show("This game can't run a .cfg from here");
                return;
            }
            gameConfigs.running = path;
            std::erase(gameConfigs.recent, path);
            gameConfigs.recent.insert(gameConfigs.recent.begin(), path);
            if (gameConfigs.recent.size() > 10)
                gameConfigs.recent.resize(10);
            SaveRecentConfigs();
            Toasts::Show(std::format("Ran {} ({} dvars it sets can go back with Default)", path.filename().string(),
                                     gameConfigs.before.size()));
        }
    }  // namespace

    // A game .cfg (its dvars and commands) run in the console as exec would: picked from those run before, or a
    // file; Default puts the dvars it set back
    void VisualsMenu::RenderGameConfigs()
    {
        LoadRecentConfigs();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_TERMINAL " Game .cfg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto shown = gameConfigs.running ? gameConfigs.running->filename().string() : std::string("Default");
        if (ImGui::BeginCombo("##gameConfig", shown.c_str()))
        {
            if (ImGui::Selectable(ICON_FA_ARROW_ROTATE_LEFT " Default", !gameConfigs.running))
                RestoreDefaultConfig();
            ImGui::SetItemTooltip("Puts back the dvars the .cfgs set, as they were before");
            if (!gameConfigs.recent.empty())
                ImGui::Separator();
            for (const auto& path : std::vector(gameConfigs.recent))
            {
                const auto label = std::format("{}##{}", path.filename().string(), path.string());
                if (ImGui::Selectable(label.c_str(), gameConfigs.running == path))
                    RunGameConfig(path);
                ImGui::SetItemTooltip("%s", path.string().c_str());
            }
            ImGui::Separator();
            if (ImGui::Selectable(ICON_FA_FOLDER_OPEN " Run from file..."))
                if (const auto path = PathUtils::OpenFileDialog(false, OFN_EXPLORER | OFN_FILEMUSTEXIST,
                                                                "Config\0*.cfg\0All Files\0*.*\0", "cfg"))
                    RunGameConfig(*path);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Runs a game .cfg's commands and dvars now, as exec would; Default puts its dvars back");
    }

    void VisualsMenu::RenderHud()
    {
        using Components::VisualsLogic::Section;
        bool reset = false;
        const bool open = DrawSection(Section::Hud, ICON_FA_BURGER "  HUD", "Back to the game's", &reset);
        if (reset)
        {
            visuals.hudInfo = defaultVisuals.hudInfo;
            NameChange("reset the HUD", "The HUD back to the game's");
        }
        if (open)
        {
            const auto x = ImGui::GetWindowWidth() * LOOK_COLUMN;
            const auto check = [&](const char* label, const char* id, bool* value) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                ImGui::SetCursorPosX(x);
                ImGui::Checkbox(id, value);
                ImGuiEx::Keyframeable::Record(std::format("visuals.hud.{}", id + 2));
            };

            check("2D Elements", "##show2DElementsCheckbox", &visuals.hudInfo.show2DElements);
            if (visuals.hudInfo.show2DElements)
            {
                check("Player HUD", "##showPlayerHUDCheckbox", &visuals.hudInfo.showPlayerHUD);
                check("Shellshock/Flashbang", "##showFlashbangCheckbox", &visuals.hudInfo.showShellshock);
                check("Crosshair", "##showCrosshairCheckbox", &visuals.hudInfo.showCrosshair);
                check("Score", "##showScoreCheckbox", &visuals.hudInfo.showScore);
                check("Icons and Text", "##showOtherTextCheckbox", &visuals.hudInfo.showIconsAndText);
                check("Blood Overlay", "##showBloodOverlayCheckbox", &visuals.hudInfo.showBloodOverlay);

                ImGui::Dummy(ImVec2(0.0f, 8.0f));

                check("Killfeed", "##showKillfeedCheckbox", &visuals.hudInfo.showKillfeed);
                const auto width = ImGui::GetWindowWidth() * (1.0f - LOOK_COLUMN) - ImGui::GetStyle().WindowPadding.x;
                const auto color = [&](const char* label, const char* id, glm::vec3& value) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(x);
                    ImGui::SetNextItemWidth(width);
                    ImGui::ColorEdit3(id, glm::value_ptr(value));
                };
                if (Mod::GetGameInterface()->HasKillfeedColors())
                {
                    color("Team 1 Color", "##killfeedTeam1Color", visuals.hudInfo.killfeedTeam1Color);
                    color("Team 2 Color", "##killfeedTeam2Color", visuals.hudInfo.killfeedTeam2Color);
                }
            }
            EndSection(Section::Hud);
        }

        // Since the hud occasionally re-appears more or less randomly,
        // we update the hud settings every frame, regardless of whether they were modified
        UpdateHudInfo();
    }

    void VisualsMenu::SetHiddenMapEffects(const std::string& level, const std::set<std::string>& hidden)
    {
        Mod::GetGameInterface()->SetHiddenMapEffects(level, hidden);
        auto& kept = PreferencesConfiguration::Get().hiddenMapEffects;
        if (hidden.empty())
            kept.erase(level);
        else
            kept[level] = hidden;
        Configuration::Get().Write(true);
    }

    void VisualsMenu::RenderMapEffects()
    {
        using Components::VisualsLogic::Section;
        const auto effects = Mod::GetGameInterface()->GetMapEffects();
        if (!effects || effects->kinds.empty())
            return;

        bool reset = false;
        const bool open = DrawSection(Section::MapEffects, ICON_FA_FIRE "  Map Effects", "Back to the game's", &reset);
        std::set<std::string> hidden;
        std::uint32_t placed = 0, playing = 0;
        for (const auto& kind : effects->kinds)
        {
            if (!kind.shown)
                hidden.insert(kind.definition);
            placed += kind.placed;
            playing += kind.playing;
        }
        auto changed = hidden;
        std::string label;
        if (reset)
        {
            changed.clear();
            label = "reset Map Effects";
        }

        if (open)
        {
            // the switch of all of them in the rows' column, each effect's further right: names are long
            const auto x = ImGui::GetWindowWidth() * LOOK_COLUMN;
            const auto checkboxColumn = ImGui::GetWindowWidth() * 0.6f;

            // all of them, on while any is
            bool all = hidden.size() < effects->kinds.size();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Show Map Effects");
            ImGui::SameLine();
            ImGui::SetCursorPosX(x);
            if (ImGui::Checkbox("##mapEffects", &all))
            {
                changed.clear();
                if (!all)
                    for (const auto& kind : effects->kinds)
                        changed.insert(kind.definition);
                label = all ? "show Map Effects" : "hide Map Effects";
            }
            ImGuiEx::Keyframeable::Record("visuals.mapeffects");
            ImGui::SetItemTooltip("The level's own fire, smoke, dust and lights");
            ImGui::TextDisabled("%u placed, %u playing", placed, playing);

            // each definition: its name and how many the level places, in a fold (a level has up to 53)
            const bool listOpen =
                ImGui::TreeNodeEx(std::format("Each effect ({})##mapEffectsList", effects->kinds.size()).c_str());
            ImGuiEx::Keyframeable::Record("visuals.mapeffects.list");
            for (const auto& kind : listOpen ? effects->kinds : std::vector<Types::MapEffectKind>{})
            {
                bool shown = kind.shown;
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", kind.name.c_str());
                ImGui::SetItemTooltip("%s: %u placed, %u playing", kind.definition.c_str(), kind.placed,
                                      kind.playing);
                ImGui::SameLine();
                ImGui::TextDisabled("%u", kind.placed);
                ImGui::SameLine();
                ImGui::SetCursorPosX(std::max(checkboxColumn, ImGui::GetCursorPosX()));
                if (ImGui::Checkbox(std::format("##mapEffect{}", kind.definition).c_str(), &shown))
                {
                    if (shown)
                        changed.erase(kind.definition);
                    else
                        changed.insert(kind.definition);
                    label = std::format("{} {}", shown ? "show" : "hide", kind.name);
                }
                ImGuiEx::Keyframeable::Record("visuals.mapeffects." + kind.name);
                ImGui::SetItemTooltip("%s: %u placed, %u playing", kind.definition.c_str(), kind.placed,
                                      kind.playing);
            }
            if (listOpen)
                ImGui::TreePop();
            EndSection(Section::MapEffects);
        }

        if (changed == hidden)
            return;
        SetHiddenMapEffects(effects->level, changed);
        Components::UndoHistory::Get().Push(
            label, [this, level = effects->level, hidden] { SetHiddenMapEffects(level, hidden); },
            [this, level = effects->level, changed] { SetHiddenMapEffects(level, changed); });
    }

    void VisualsMenu::RenderFilmtweaks()
    {
        using Components::VisualsLogic::Section;
        bool modified = false;

        const bool open =
            DrawSection(Section::Filmtweaks, ICON_FA_IMAGE "  Filmtweaks", "Back to the game's", &modified);
        if (modified)
        {
            visuals.filmtweaks = defaultVisuals.filmtweaks;
            NameChange("reset Filmtweaks", "Filmtweaks back to the game's");
        }
        if (!open)
        {
            if (modified)
                UpdateFilmtweaks();
            return;
        }

        auto& filmtweaks = visuals.filmtweaks;
        const auto& defaults = defaultVisuals.filmtweaks;

        // the rows are named like the ones of other sections
        ImGui::PushID("Filmtweaks");

        modified =
            ImGuiEx::Keyframeable::Checkbox("Enable", &filmtweaks.enabled, Property::FilmtweakEnabled, "Filmtweaks") ||
            modified;

        // the game ignores the other settings while filmtweaks are off
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (filmtweaks.enabled ? 1.0f : 0.5f));

        const auto features = Mod::GetGameInterface()->GetSupportedFeatures();
        if (features & Types::Features_FilmtweakRanges)
        {
            // (Brightness and Invert over all ranges, for a game that offers them)
            if (features & Types::Features_FilmtweakBrightness)
            {
                modified = ImGuiEx::Keyframeable::SliderFloat("Brightness", &filmtweaks.brightness, -1, 1,
                                                              Types::KeyframeablePropertyType::FilmtweakBrightness,
                                                              "%.2f", {.resetValue = defaults.brightness}) ||
                           modified;
                modified = ImGuiEx::Keyframeable::Checkbox("Invert", &filmtweaks.invert, Property::FilmtweakInvert,
                                                           "Invert") ||
                           modified;
            }
            modified = RenderFilmtweakRanges() || modified;
        }
        else
        {
            modified = ImGuiEx::Keyframeable::SliderFloat("Brightness", &filmtweaks.brightness, -1, 1,
                                                          Types::KeyframeablePropertyType::FilmtweakBrightness, "%.2f",
                                                          {.resetValue = defaults.brightness}) ||
                       modified;
            modified = ImGuiEx::Keyframeable::SliderFloat("Contrast", &filmtweaks.contrast, 0, 4,
                                                          Types::KeyframeablePropertyType::FilmtweakContrast, "%.2f",
                                                          {.origin = 1.0f, .resetValue = defaults.contrast}) ||
                       modified;
            modified = ImGuiEx::Keyframeable::SliderFloat("Desaturation", &filmtweaks.desaturation, 0, 4,
                                                          Types::KeyframeablePropertyType::FilmtweakDesaturation,
                                                          "%.2f", {.resetValue = defaults.desaturation}) ||
                       modified;
            modified = ImGuiEx::Keyframeable::ColorEdit3("Tint Light", glm::value_ptr(filmtweaks.tintLight),
                                                         Types::KeyframeablePropertyType::FilmtweakTintLight,
                                                         defaults.tintLight) ||
                       modified;
            modified = ImGuiEx::Keyframeable::ColorEdit3("Tint Dark", glm::value_ptr(filmtweaks.tintDark),
                                                         Types::KeyframeablePropertyType::FilmtweakTintDark,
                                                         defaults.tintDark) ||
                       modified;
            modified =
                ImGuiEx::Keyframeable::Checkbox("Invert", &filmtweaks.invert, Property::FilmtweakInvert, "Invert") ||
                modified;
        }

        ImGui::PopStyleVar();
        ImGui::PopID();

        if (modified)
        {
            UpdateFilmtweaks();
        }

        EndSection(Section::Filmtweaks);
    }

    // Black Ops grades three ranges of the image separately, kept in x, y and z of the range values
    constexpr std::size_t DARK_RANGE = 0;
    constexpr std::size_t MID_RANGE = 1;
    constexpr std::size_t LIGHT_RANGE = 2;

    struct FilmtweakRangeProperty
    {
        const char* label;
        glm::vec3 Types::Filmtweaks::*value;
        Property property;  // the keyframeable property of all three ranges
        float min;
        float max;
        const char* format;
        float neutral;  // the bar fills from here
    };

    // the limits are the ones the game registers the dvars with
    const std::array FILMTWEAK_RANGE_PROPERTIES = {
        FilmtweakRangeProperty{"Contrast", &Types::Filmtweaks::contrastRanges, Property::FilmtweakContrastRanges, 0, 2,
                               "%.2f", 1},
        FilmtweakRangeProperty{"Saturation", &Types::Filmtweaks::saturationRanges, Property::FilmtweakSaturationRanges,
                               0, 2, "%.2f", 1},
        FilmtweakRangeProperty{"Hue", &Types::Filmtweaks::hueRanges, Property::FilmtweakHueRanges, -180, 180,
                               "%.0f\xC2\xB0", 0},
        FilmtweakRangeProperty{"Temperature", &Types::Filmtweaks::colorTemperatureRanges,
                               Property::FilmtweakTemperatureRanges, 1650, 25000, "%.0f K", 6500},
        FilmtweakRangeProperty{"Bleach", &Types::Filmtweaks::bleachRanges, Property::FilmtweakBleachRanges, 0, 1,
                               "%.2f", 0},
    };

    // the tints of the dark, mid and light range
    constexpr std::array TINT_PROPERTIES = {Property::FilmtweakTintDark, Property::FilmtweakTintMid,
                                            Property::FilmtweakTintLight};

    // The "All" tab edits the three ranges together: it shows their average and moves all three by the same amount,
    // so the differences between them stay. Its reset puts back all three.
    bool DragAllRanges(const char* id, glm::vec3& values, const glm::vec3& defaults,
                       const FilmtweakRangeProperty& property)
    {
        const float average = (values.x + values.y + values.z) / 3.0f;
        const bool mixed = values.x != values.y || values.y != values.z;
        const auto format = std::string(property.format) + (mixed ? " (avg)" : "");

        float edited = average;
        bool changed = ImGuiEx::DragSlider(id, &edited, property.min, property.max, format.c_str(),
                                           {.origin = property.neutral, .flags = ImGuiSliderFlags_AlwaysClamp});
        if (changed)
        {
            values = glm::clamp(values + (edited - average), property.min, property.max);
        }

        if (ImGuiEx::ResetMenu("the game's") && values != defaults)
        {
            values = defaults;
            changed = true;
        }

        return changed;
    }

    // A black to white bar with lines where the mid range starts and ends. The selected range is underlined.
    void DrawToneRanges(const Types::Filmtweaks& filmtweaks, std::optional<std::size_t> range)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFontSize() * 0.6f;
        const float underlineHeight = 3.0f;
        ImGui::Dummy(ImVec2(width, height + underlineHeight + 2.0f));

        auto drawList = ImGui::GetWindowDrawList();
        const auto accent = ImGui::GetColorU32(ImGuiCol_SliderGrab);
        const std::array bounds = {0.0f, std::clamp(filmtweaks.midStart, 0.0f, 1.0f),
                                   std::clamp(filmtweaks.midEnd, 0.0f, 1.0f), 1.0f};
        // the shadows, midtones and highlights as three flat greys (no gradient)
        const std::array greys = {ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.08f, 1)),
                                  ImGui::GetColorU32(ImVec4(0.44f, 0.44f, 0.44f, 1)),
                                  ImGui::GetColorU32(ImVec4(0.86f, 0.86f, 0.86f, 1))};
        for (std::size_t i = 0; i < greys.size(); ++i)
            drawList->AddRectFilled(ImVec2(min.x + bounds[i] * width, min.y),
                                    ImVec2(min.x + bounds[i + 1] * width, min.y + height), greys[i]);
        for (auto bound : {bounds[1], bounds[2]})
        {
            const float x = min.x + bound * width;
            drawList->AddLine(ImVec2(x, min.y), ImVec2(x, min.y + height), accent, 2.0f);
        }

        if (range.has_value())
        {
            const float top = min.y + height + 2.0f;
            drawList->AddRectFilled(ImVec2(min.x + bounds[range.value()] * width, top),
                                    ImVec2(min.x + bounds[range.value() + 1] * width, top + underlineHeight), accent);
        }
    }

    // Black Ops' film system: the image is split into dark, mid and light ranges that are graded separately. It
    // registers r_filmTweakBrightness, r_filmTweakDesaturation and r_filmTweakInvert but never reads them (the film's
    // constants are made from the dvars below and Enable), so the other games' Brightness, Desaturation and Invert
    // aren't offered (VisualsLogic::IsUsed).
    bool VisualsMenu::RenderFilmtweakRanges()
    {
        bool modified = false;

        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        if (ImGui::BeginTabBar("##filmtweakRanges"))
        {
            // the tab for all ranges, then one per range (the harness clicks them as "visuals.range.<name>")
            constexpr std::array TABS = {"All", "Shadows", "Midtones", "Highlights"};
            constexpr std::array TAB_NAMES = {"all", "shadows", "midtones", "highlights"};
            for (std::size_t tab = 0; tab < TABS.size(); tab++)
            {
                const bool open = ImGui::BeginTabItem(TABS[tab]);
                ImGuiEx::Keyframeable::Record(std::format("visuals.range.{}", TAB_NAMES[tab]));
                if (open)
                {
                    // rows such as Feather are in more than one tab
                    ImGui::PushID(static_cast<int>(tab));
                    modified = RenderFilmtweakRange(tab == 0 ? std::nullopt : std::optional(tab - 1)) || modified;
                    ImGui::PopID();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        return modified;
    }

    bool VisualsMenu::RenderFilmtweakRange(std::optional<std::size_t> range)
    {
        auto& filmtweaks = visuals.filmtweaks;
        const auto& defaults = defaultVisuals.filmtweaks;
        bool modified = false;

        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        DrawToneRanges(filmtweaks, range);

        // each row keys all three ranges of its property, whichever tab it is in
        for (const auto& property : FILMTWEAK_RANGE_PROPERTIES)
        {
            auto& values = filmtweaks.*property.value;
            const auto& defaultValues = defaults.*property.value;

            Types::KeyframeValue value(values);
            modified =
                ImGuiEx::Keyframeable::Row(property.label, property.property, value,
                                           [&](const char* id, Types::KeyframeValue& edited) {
                                               if (!range.has_value())
                                                   return DragAllRanges(id, edited.vector3, defaultValues, property);

                                               return ImGuiEx::DragSlider(id, &edited.vector3[range.value()],
                                                                          property.min, property.max, property.format,
                                                                          {.origin = property.neutral,
                                                                           .resetValue = defaultValues[range.value()],
                                                                           .flags = ImGuiSliderFlags_AlwaysClamp});
                                           }) ||
                modified;
            values = value.vector3;
        }

        // the game allows tints up to 6, so the color edit goes above 1
        const std::array tints = {&filmtweaks.tintDark, &filmtweaks.tintMid, &filmtweaks.tintLight};
        const std::array defaultTints = {&defaults.tintDark, &defaults.tintMid, &defaults.tintLight};
        constexpr auto tintFlags = ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoOptions;
        if (range.has_value())
        {
            auto& tint = *tints[range.value()];
            const auto& defaultTint = *defaultTints[range.value()];
            Types::KeyframeValue value(tint);
            modified = ImGuiEx::Keyframeable::Row(
                           "Tint", TINT_PROPERTIES[range.value()], value,
                           [&](const char* id, Types::KeyframeValue& edited) {
                               bool changed = ImGui::ColorEdit3(id, glm::value_ptr(edited.vector3), tintFlags);
                               if (ImGuiEx::ResetMenu(std::format("{:.2f}, {:.2f}, {:.2f}", defaultTint.x,
                                                                  defaultTint.y, defaultTint.z)
                                                          .c_str(),
                                                      "##tintReset") &&
                                   edited.vector3 != defaultTint)
                               {
                                   edited.vector3 = defaultTint;
                                   changed = true;
                               }
                               return changed;
                           }) ||
                       modified;
            tint = value.vector3;
        }
        else
        {
            // like DragAllRanges: the average color, and changes move all three tints (keying those with keys)
            std::array<Types::KeyframeValue, 3> values = {
                Types::KeyframeValue(*tints[0]), Types::KeyframeValue(*tints[1]), Types::KeyframeValue(*tints[2])};
            modified = ImGuiEx::Keyframeable::GroupRow(
                           "Tint", TINT_PROPERTIES, values,
                           [&](const char* id) {
                               const auto average = (values[0].vector3 + values[1].vector3 + values[2].vector3) / 3.0f;
                               auto edited = average;
                               bool changed = ImGui::ColorEdit3(id, glm::value_ptr(edited), tintFlags);
                               if (changed)
                               {
                                   for (auto& value : values)
                                       value.vector3 = glm::clamp(value.vector3 + (edited - average), 0.0f, 6.0f);
                               }
                               if (ImGuiEx::ResetMenu("the game's", "##tintReset"))
                               {
                                   for (std::size_t i = 0; i < values.size(); i++)
                                       values[i].vector3 = *defaultTints[i];
                                   changed = true;
                               }
                               return changed;
                           }) ||
                       modified;
            for (std::size_t i = 0; i < tints.size(); i++)
                *tints[i] = values[i].vector3;
        }

        // where the ranges meet, from 0 (black) to 1 (white); each range tab shows its own edges. Says whether the
        // user changed it (keys may change it too).
        auto edgeRow = [&](const char* label, const char* name, Property property, float& setting, float defaultValue,
                           const char* tooltip) {
            bool changed = false;
            Types::KeyframeValue value(setting);
            modified = ImGuiEx::Keyframeable::Row(
                           label, property, value,
                           [&](const char* id, Types::KeyframeValue& edited) {
                               changed = ImGuiEx::DragSlider(
                                   id, &edited.floatingPoint, 0, 1, "%.2f",
                                   {.resetValue = defaultValue, .flags = ImGuiSliderFlags_AlwaysClamp});
                               return changed;
                           },
                           name) ||
                       modified;
            ImGui::SetItemTooltip("%s", tooltip);
            setting = value.floatingPoint;
            return changed;
        };

        bool midStartChanged = false;
        bool midEndChanged = false;
        // (only a game that can move where the ranges meet shows their edges)
        const bool edges = Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_FilmtweakRangeEdges;
        if (!edges)
        {
            // (the map's own edges)
        }
        else if (range == DARK_RANGE)
        {
            midStartChanged =
                edgeRow("Ends At", "Mid Start", Property::FilmtweakMidStart, filmtweaks.midStart, defaults.midStart,
                        "Where shadows end");
            edgeRow("Feather", "Dark Feather", Property::FilmtweakDarkFeather, filmtweaks.darkFeather,
                    defaults.darkFeather, "How soft that edge is");
        }
        else if (range == MID_RANGE)
        {
            midStartChanged =
                edgeRow("Starts At", "Mid Start", Property::FilmtweakMidStart, filmtweaks.midStart, defaults.midStart,
                        "Where shadows end");
            midEndChanged = edgeRow("Ends At", "Mid End", Property::FilmtweakMidEnd, filmtweaks.midEnd, defaults.midEnd,
                                    "Where highlights start");
        }
        else if (range == LIGHT_RANGE)
        {
            midEndChanged =
                edgeRow("Starts At", "Mid End", Property::FilmtweakMidEnd, filmtweaks.midEnd, defaults.midEnd,
                        "Where highlights start");
            edgeRow("Feather", "Light Feather", Property::FilmtweakLightFeather, filmtweaks.lightFeather,
                    defaults.lightFeather, "How soft that edge is");
        }

        // The mid range can't end before it starts, so the edge being dragged pushes the other one, keying it at the
        // playhead too when it has keys (in the same step)
        auto& keyframeManager = Components::KeyframeManager::Get();
        if (midStartChanged && filmtweaks.midEnd < filmtweaks.midStart)
        {
            filmtweaks.midEnd = filmtweaks.midStart;
            ImGuiEx::Keyframeable::KeyValueHere(keyframeManager.GetProperty(Property::FilmtweakMidEnd),
                                                Types::KeyframeValue(filmtweaks.midEnd), "Mid Start");
        }
        if (midEndChanged && filmtweaks.midStart > filmtweaks.midEnd)
        {
            filmtweaks.midStart = filmtweaks.midEnd;
            ImGuiEx::Keyframeable::KeyValueHere(keyframeManager.GetProperty(Property::FilmtweakMidStart),
                                                Types::KeyframeValue(filmtweaks.midStart), "Mid End");
        }

        return modified || midStartChanged || midEndChanged;
    }

    // The pin over the game view while the Lens rows are shown, and a pick there while Pick Focus waits for one
    glm::vec3 VisualsMenu::ToSun() const
    {
        auto toSun = MathUtils::AnglesToAxis(visuals.sunDirection)[0];
        return toSun.z < 0.0f ? -toSun : toSun;
    }

    glm::vec3 VisualsMenu::SunAngles(glm::vec3 toSun) const
    {
        // (the map's angles give the light's way, down, when their forward is down: kept so)
        const bool lightsWay = MathUtils::AnglesToAxis(defaultVisuals.sunDirection)[0].z < 0.0f;
        const auto forward = lightsWay ? -toSun : toSun;
        const float pitch = -glm::degrees(std::asin(std::clamp(forward.z, -1.0f, 1.0f)));
        const float yaw = glm::degrees(std::atan2(forward.y, forward.x));
        return {pitch, yaw, visuals.sunDirection.z};
    }

    void VisualsMenu::DrawSunAim()
    {
        const bool shown = ImGui::GetFrameCount() - sunRowsShownAt <= 2;
        const auto view = Components::Tracking::GetView();
        if (!shown || !view || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
        {
            aimingSun = draggingSun = false;
            return;
        }
        const auto gameView = UIManager::Get().GetUIComponent<GameView>(Component::Component::GameView);
        const auto position = gameView->GetViewportPosition();
        const auto size = gameView->GetViewportSize();
        if (size.x <= 0 || size.y <= 0)
            return;
        auto* drawList = ImGui::GetWindowDrawList();
        const auto mouse = ImGui::GetIO().MousePos;

        // aiming: the sun follows the mouse while it's held on the picture, a key (or a step) on letting go
        if (aimingSun)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                aimingSun = draggingSun = false;
                return;
            }
            const bool over = mouse.x >= position.x && mouse.y >= position.y && mouse.x < position.x + size.x &&
                              mouse.y < position.y + size.y && !gameView->IsMouseOverOverlay();
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                draggingSun = true;
            if (draggingSun)
            {
                const glm::vec2 uv = Components::Lens::PictureToRender(
                    glm::vec2(std::clamp((mouse.x - position.x) / size.x, 0.0f, 1.0f),
                              std::clamp((mouse.y - position.y) / size.y, 0.0f, 1.0f)));
                const auto point = Components::Tracking::Unproject(uv, 1000.0f, *view);
                visuals.sunDirection = SunAngles(glm::normalize(point - view->position));
                UpdateSun();
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    draggingSun = false;
                    aimingSun = false;
                    ImGuiEx::Keyframeable::KeyValueHere(
                        Components::KeyframeManager::Get().GetProperty(Property::SunLightDirection),
                        Types::KeyframeValue(visuals.sunDirection), "Aim the Sun");
                    NameChange("aim the sun", "Sun aimed");
                }
            }
            if (over || draggingSun)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                const auto color = IM_COL32(255, 230, 150, 230);
                drawList->AddLine(mouse - ImVec2(14, 0), mouse + ImVec2(15, 0), color, 1.5f);
                drawList->AddLine(mouse - ImVec2(0, 14), mouse + ImVec2(0, 15), color, 1.5f);
                drawList->AddText(mouse + ImVec2(12, 10), color, draggingSun ? "let go to place it" : "drag the sun");
            }
        }

        // where the sun is: its disc and rays, or an arrow at the edge toward it (behind or off the picture)
        const auto toSun = ToSun();
        const auto color = ImGui::GetColorU32(ImVec4(std::min(visuals.sunColor.x, 1.0f) * 0.5f + 0.5f,
                                                     std::min(visuals.sunColor.y, 1.0f) * 0.5f + 0.5f,
                                                     std::min(visuals.sunColor.z, 1.0f) * 0.5f + 0.5f, 0.95f));
        const auto shadow = IM_COL32(0, 0, 0, 140);
        const auto projected = Components::Tracking::Project(view->position + toSun * 10000.0f, *view);
        const ImVec2 middle = position + size * 0.5f;
        if (projected && projected->uv.x >= 0.0f && projected->uv.x <= 1.0f && projected->uv.y >= 0.0f &&
            projected->uv.y <= 1.0f)
        {
            const ImVec2 at = position + ImVec2(projected->uv.x * size.x, projected->uv.y * size.y);
            drawList->AddLine(middle, at, IM_COL32(255, 255, 255, 50), 1.0f);
            drawList->AddCircle(at, 9.0f, shadow, 24, 4.0f);
            drawList->AddCircle(at, 9.0f, color, 24, 2.0f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = i * 0.785398f;
                const ImVec2 d(std::cos(a), std::sin(a));
                drawList->AddLine(at + d * 13.0f, at + d * 19.0f, color, 2.0f);
            }
        }
        else
        {
            // toward it on the picture's plane (its direction in the view's left and up)
            const glm::vec2 way(-glm::dot(toSun, view->axis[1]), -glm::dot(toSun, view->axis[2]));
            if (glm::length(way) < 0.0001f)
                return;
            const auto unit = glm::normalize(way);
            const float reach = std::min(std::abs(size.x * 0.5f / std::max(std::abs(unit.x), 0.0001f)),
                                         std::abs(size.y * 0.5f / std::max(std::abs(unit.y), 0.0001f))) -
                                24.0f;
            const ImVec2 tip = middle + ImVec2(unit.x, unit.y) * reach;
            const ImVec2 side(-unit.y * 8.0f, unit.x * 8.0f);
            const ImVec2 back = tip - ImVec2(unit.x, unit.y) * 14.0f;
            drawList->AddTriangleFilled(tip, back + side, back - side, color);
            drawList->AddText(back - ImVec2(unit.x, unit.y) * 18.0f - ImVec2(14.0f, 7.0f), color, "sun");
        }
    }

    void VisualsMenu::DrawFocusPin()
    {
        auto& dof = visuals.dof;
        const bool lensOn = dof.enabled && static_cast<int>(std::round(dof.blurType)) == Types::DoF::BLUR_LENS;
        const bool shown = ImGui::GetFrameCount() - lensRowsShownAt <= 2;
        if (!lensOn || !shown || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
        {
            pickingFocus = false;
            return;
        }
        const auto gameView = UIManager::Get().GetUIComponent<GameView>(Component::Component::GameView);
        const auto position = gameView->GetViewportPosition();
        const auto size = gameView->GetViewportSize();
        if (size.x <= 0 || size.y <= 0)
            return;
        auto* drawList = ImGui::GetWindowDrawList();

        if (pickingFocus)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                pickingFocus = false;
                return;
            }
            const auto mouse = ImGui::GetIO().MousePos;
            const bool over = mouse.x >= position.x && mouse.y >= position.y && mouse.x < position.x + size.x &&
                              mouse.y < position.y + size.y && !gameView->IsMouseOverOverlay();
            if (over)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                const auto color = IM_COL32(255, 255, 255, 230);
                drawList->AddLine(mouse - ImVec2(14, 0), mouse + ImVec2(15, 0), color, 1.5f);
                drawList->AddLine(mouse - ImVec2(0, 14), mouse + ImVec2(0, 15), color, 1.5f);
                drawList->AddText(mouse + ImVec2(12, 10), color, "click where it should be sharp");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    static_cast<int>(dof.focusMode) == Types::DoF::FOCUS_TRACK)
                {
                    // Track: what's clicked followed (RenderLens takes the pick)
                    pickingFocus = false;
                    focusPicker.Begin(glm::vec2(std::clamp((mouse.x - position.x) / size.x, 0.0f, 1.0f),
                                                std::clamp((mouse.y - position.y) / size.y, 0.0f, 1.0f)),
                                      Components::Tracking::GetFocusTrack().bone);
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    pickingFocus = false;
                    // (where it is in the picture the game drew, under the lens's bend)
                    const auto picked = glm::clamp(
                        Components::Lens::PictureToRender(glm::vec2((mouse.x - position.x) / size.x,
                                                                    (mouse.y - position.y) / size.y)),
                        glm::vec2(0.0f), glm::vec2(1.0f));
                    dof.focusPointX = picked.x;
                    dof.focusPointY = picked.y;
                    auto& keyframes = Components::KeyframeManager::Get();
                    ImGuiEx::Keyframeable::KeyValueHere(keyframes.GetProperty(Property::DepthOfFieldFocusPointX),
                                                        Types::KeyframeValue(dof.focusPointX), "Pick Focus");
                    ImGuiEx::Keyframeable::KeyValueHere(keyframes.GetProperty(Property::DepthOfFieldFocusPointY),
                                                        Types::KeyframeValue(dof.focusPointY), "Pick Focus");
                    applyPickedDistance = static_cast<int>(dof.focusMode) == Types::DoF::FOCUS_DISTANCE;
                    pickedAtFrame = ImGui::GetFrameCount();
                    NameChange("pick the focus", "Focus picked");
                    UpdateDof();
                }
                return;
            }
        }

        // the pin: a ring with a dot (on what Track follows, where it is now)
        auto pinAt = glm::vec2(dof.focusPointX, dof.focusPointY);
        if (static_cast<int>(dof.focusMode) == Types::DoF::FOCUS_TRACK)
        {
            const auto tracked = D3D11::GetTrackedFocus();
            if (!tracked)
                return;
            pinAt = *tracked;
        }
        const ImVec2 pin(position.x + pinAt.x * size.x, position.y + pinAt.y * size.y);
        drawList->AddCircle(pin, 9.0f, IM_COL32(0, 0, 0, 160), 0, 3.5f);
        drawList->AddCircle(pin, 9.0f, IM_COL32(255, 210, 60, 255), 0, 2.0f);
        drawList->AddCircleFilled(pin, 2.0f, IM_COL32(255, 210, 60, 255));
    }

    // The Lens blur's rows: where it's sharp (with the distance at the picture's centre to focus on), the lens, and
    // the bokeh. Says whether a setting changed.
    bool VisualsMenu::RenderLens()
    {
        auto& dof = visuals.dof;
        const auto& defaults = defaultVisuals.dof;
        bool modified = false;

        lensRowsShownAt = ImGui::GetFrameCount();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        // where it focuses: a distance, or whatever is under the pin each frame
        {
            constexpr std::array MODES = {"Distance", "Follow Pin", "Track"};
            Types::KeyframeValue value(dof.focusMode);
            modified = ImGuiEx::Keyframeable::Row(
                           "Focus", Property::DepthOfFieldFocusMode, value,
                           [&](const char* id, Types::KeyframeValue& edited) {
                               int mode = std::clamp(static_cast<int>(std::round(edited.floatingPoint)), 0, 2);
                               const bool changed =
                                   ImGui::Combo(id, &mode, MODES.data(), static_cast<int>(MODES.size()));
                               if (changed)
                                   edited.floatingPoint = static_cast<float>(mode);
                               return changed;
                           },
                           "Focus Mode") ||
                       modified;
            dof.focusMode = std::clamp(std::round(value.floatingPoint), 0.0f, 2.0f);
            ImGui::SetItemTooltip("Distance: sharp at the Focus Distance. Follow Pin: sharp at whatever is under the "
                                  "pin, as it moves. Track: sharp on a player (or a spot of the map) wherever it goes, "
                                  "without keys.");
        }
        const bool followPin = static_cast<int>(dof.focusMode) == Types::DoF::FOCUS_FOLLOW_POINT;
        const bool track = static_cast<int>(dof.focusMode) == Types::DoF::FOCUS_TRACK;

        // Track: what it follows, chosen or clicked on the picture
        auto focusTrack = Components::Tracking::GetFocusTrack();
        if (const auto picked = focusPicker.Poll())
        {
            focusTrack = *picked;
            Components::Tracking::SetFocusTrack(focusTrack);
            Components::KeyframeSerializer::WriteRecentSoon();
            modified = true;
        }
        if (track)
        {
            if (DrawTrackTarget("focusTrack", focusTrack))
            {
                Components::Tracking::SetFocusTrack(focusTrack);
                Components::KeyframeSerializer::WriteRecentSoon();
                modified = true;
            }
            if (ImGui::Button(pickingFocus             ? ICON_FA_CROSSHAIRS " Click the Picture..."
                              : focusPicker.IsWaiting() ? ICON_FA_CROSSHAIRS " Reading the depth..."
                                                        : ICON_FA_CROSSHAIRS " Pick on Picture"))
                pickingFocus = !pickingFocus;
            ImGui::SetItemTooltip("Then click a player to follow them, or a spot of the map to stay on it (Escape "
                                  "cancels)");
            if (focusTrack.kind != Types::TrackKind::None && !D3D11::GetTrackedFocus())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(not in view: its last distance kept)");
            }
        }

        if (track)
        {
        }
        else if (followPin)
        {
            modified = ImGuiEx::Keyframeable::SliderFloat("Smoothing", &dof.focusSmoothing, 0, 2,
                                                          Property::DepthOfFieldFocusSmoothing, "%.2f s",
                                                          {.resetValue = defaults.focusSmoothing}) ||
                       modified;
            ImGui::SetItemTooltip("About how long the focus takes to get to something new under the pin, in the "
                                  "film's time (0 at once)");
        }
        else
        {
            modified = ImGuiEx::Keyframeable::SliderFloat("Focus Distance", &dof.focusDistance, 0, 5000,
                                                          Property::DepthOfFieldFocusDistance, "%.0f",
                                                          {.resetValue = defaults.focusDistance}) ||
                       modified;
            ImGui::SetItemTooltip("How far away is sharp, in the game's units (about an inch each)");
        }

        // the pin: picked on the picture, or moved with its rows
        if (!track && ImGui::Button(pickingFocus ? ICON_FA_CROSSHAIRS " Click the Picture..."
                                                 : ICON_FA_CROSSHAIRS " Pick Focus"))
            pickingFocus = !pickingFocus;
        if (!track)
            ImGui::SetItemTooltip("Then click on the picture where it should be sharp (Escape cancels)");
        if (const auto distance = D3D11::GetPointDepth(); distance && !track)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("at the pin: %.0f", *distance);
            if (!followPin)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Focus Here"))
                {
                    dof.focusDistance = std::clamp(*distance, 0.0f, 100000.0f);
                    ImGuiEx::Keyframeable::KeyValueHere(
                        Components::KeyframeManager::Get().GetProperty(Property::DepthOfFieldFocusDistance),
                        Types::KeyframeValue(dof.focusDistance), "Focus Distance");
                    modified = true;
                }
            }
        }
        if (!track)
        {
            modified = ImGuiEx::Keyframeable::SliderFloat("Pin X", &dof.focusPointX, 0, 1,
                                                          Property::DepthOfFieldFocusPointX, "%.3f",
                                                          {.resetValue = defaults.focusPointX}) ||
                       modified;
            modified = ImGuiEx::Keyframeable::SliderFloat("Pin Y", &dof.focusPointY, 0, 1,
                                                          Property::DepthOfFieldFocusPointY, "%.3f",
                                                          {.resetValue = defaults.focusPointY}) ||
                       modified;
        }

        // a pick in Distance mode sets the distance once the depth there is read (a few frames later)
        if (applyPickedDistance && !followPin)
        {
            if (const auto distance = D3D11::GetPointDepth();
                distance && ImGui::GetFrameCount() - pickedAtFrame > 6)
            {
                applyPickedDistance = false;
                dof.focusDistance = std::clamp(*distance, 0.0f, 100000.0f);
                ImGuiEx::Keyframeable::KeyValueHere(
                    Components::KeyframeManager::Get().GetProperty(Property::DepthOfFieldFocusDistance),
                    Types::KeyframeValue(dof.focusDistance), "Focus Distance");
                modified = true;
            }
            else if (ImGui::GetFrameCount() - pickedAtFrame > 120)
            {
                applyPickedDistance = false;
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        modified = ImGuiEx::Keyframeable::SliderFloat("Focal Length", &dof.focalLength, 10, 300,
                                                      Property::DepthOfFieldFocalLength, "%.0f mm",
                                                      {.resetValue = defaults.focalLength}) ||
                   modified;
        ImGui::SetItemTooltip("A longer lens blurs more away from the focus");
        modified = ImGuiEx::Keyframeable::SliderFloat("Aperture", &dof.aperture, 1, 22, Property::DepthOfFieldAperture,
                                                      "f/%.1f", {.resetValue = defaults.aperture}) ||
                   modified;
        ImGui::SetItemTooltip("A lower f-number blurs more");

        modified = ImGuiEx::Keyframeable::SliderFloat("Blur Amount", &dof.blurAmount, 0, 4,
                                                      Property::DepthOfFieldBlurAmount, "%.2fx",
                                                      {.origin = 1.0f, .resetValue = defaults.blurAmount}) ||
                   modified;
        ImGui::SetItemTooltip("How much it blurs, times what the lens would");

        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        // the bokeh's shape, kept as the number of the aperture's blades (0 round)
        {
            constexpr std::array SHAPES = {"Round", "Triangle", "Square", "Pentagon", "Hexagon", "Heptagon", "Octagon",
                                           "Nonagon"};
            constexpr std::array BLADES = {0, 3, 4, 5, 6, 7, 8, 9};
            Types::KeyframeValue value(dof.bokehBlades);
            modified = ImGuiEx::Keyframeable::Row(
                           "Bokeh Shape", Property::DepthOfFieldBokehBlades, value,
                           [&](const char* id, Types::KeyframeValue& edited) {
                               const auto blades = static_cast<int>(std::round(edited.floatingPoint));
                               int shape = 0;
                               for (std::size_t i = 0; i < BLADES.size(); ++i)
                                   if (BLADES[i] == blades)
                                       shape = static_cast<int>(i);
                               const bool changed =
                                   ImGui::Combo(id, &shape, SHAPES.data(), static_cast<int>(SHAPES.size()));
                               if (changed)
                                   edited.floatingPoint = static_cast<float>(BLADES[static_cast<std::size_t>(shape)]);
                               return changed;
                           },
                           "Bokeh Shape") ||
                       modified;
            dof.bokehBlades = std::round(value.floatingPoint);
        }
        modified = ImGuiEx::Keyframeable::SliderFloat("Bokeh Size", &dof.bokehSize, 0.01f, 0.1f,
                                                      Property::DepthOfFieldBokehSize, "%.3f",
                                                      {.resetValue = defaults.bokehSize}) ||
                   modified;
        ImGui::SetItemTooltip("How big the blur and its discs may grow, as a part of the picture's height");
        modified = ImGuiEx::Keyframeable::SliderFloat("Highlights", &dof.bokehHighlights, 0, 4,
                                                      Property::DepthOfFieldBokehHighlights, "%.2f",
                                                      {.resetValue = defaults.bokehHighlights}) ||
                   modified;
        ImGui::SetItemTooltip("How much bright spots stand out as discs");

        ImGui::Dummy(ImVec2(0.0f, 5.0f));
        if (ImGui::Checkbox("Show Focus", &dof.showFocus))
            UpdateDof();
        ImGui::SetItemTooltip("Tints the picture while this is on: green is sharp, blue nearer, red farther. Not "
                              "recorded.");
        return modified;
    }

    void VisualsMenu::RenderDOF()
    {
        using Components::VisualsLogic::Section;
        bool modified = false;

        const bool open =
            DrawSection(Section::DepthOfField, ICON_FA_CAMERA "  Depth of Field", "Back to the game's", &modified);
        if (modified)
        {
            // (the Blur section's settings are its own to reset)
            const auto kept = visuals.dof;
            visuals.dof = defaultVisuals.dof;
            visuals.dof.screenBlur = static_cast<float>(kept.GetScreenBlur());
            visuals.dof.blurStrength = kept.blurStrength;
            visuals.dof.blurAngle = kept.blurAngle;
            visuals.dof.blurCenterX = kept.blurCenterX;
            visuals.dof.blurCenterY = kept.blurCenterY;
            visuals.dof.blurClearRadius = kept.blurClearRadius;
            NameChange("reset Depth of Field", "Depth of Field back to the game's");
        }
        if (!open)
        {
            if (modified)
                UpdateDof();
            return;
        }

        auto& dof = visuals.dof;
        const auto& defaults = defaultVisuals.dof;

        ImGui::PushID("DepthOfField");
        modified =
            ImGuiEx::Keyframeable::Checkbox("Enable", &dof.enabled, Property::DepthOfFieldEnabled, "Depth of Field") ||
            modified;
        ImGui::PopID();

        // the depth of field's kind: the game's, or the mod's Lens for a game that gives its depth
        const auto features = Mod::GetGameInterface()->GetSupportedFeatures();
        if ((features & Types::Features_ScreenBlur) && (features & Types::Features_LensBlur))
        {
            constexpr std::array DOF_TYPES = {"Depth of Field", "Lens (Bokeh)"};
            Types::KeyframeValue value(dof.blurType);
            modified = ImGuiEx::Keyframeable::Row(
                           "Type", Property::DepthOfFieldBlurType, value,
                           [&](const char* id, Types::KeyframeValue& edited) {
                               int type = static_cast<int>(std::round(edited.floatingPoint)) == Types::DoF::BLUR_LENS;
                               const bool changed = ImGui::Combo(id, &type, DOF_TYPES.data(), 2);
                               if (changed)
                                   edited.floatingPoint =
                                       static_cast<float>(type ? Types::DoF::BLUR_LENS : Types::DoF::BLUR_DEPTH);
                               return changed;
                           },
                           "Depth of Field Type") ||
                       modified;
            // (an older project's directional or radial type stays until another is picked: the Blur section shows it)
            const auto picked = static_cast<int>(std::round(value.floatingPoint));
            dof.blurType = static_cast<float>(std::clamp(picked, 0, 3));

            if (dof.GetDepthBlur() == Types::DoF::BLUR_LENS)
            {
                modified = RenderLens() || modified;
                if (modified)
                    UpdateDof();
                EndSection(Section::DepthOfField);
                return;
            }
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }

        modified = ImGuiEx::Keyframeable::SliderFloat("Far Blur", &dof.farBlur, 0, 10,
                                                      Types::KeyframeablePropertyType::DepthOfFieldFarBlur, "%.2f",
                                                      {.resetValue = defaults.farBlur}) ||
                   modified;
        modified = ImGuiEx::Keyframeable::SliderFloat("Far Start", &dof.farStart, 0, 5000,
                                                      Types::KeyframeablePropertyType::DepthOfFieldFarStart, "%.0f",
                                                      {.resetValue = defaults.farStart}) ||
                   modified;
        modified = ImGuiEx::Keyframeable::SliderFloat("Far End", &dof.farEnd, 0, 10000,
                                                      Types::KeyframeablePropertyType::DepthOfFieldFarEnd, "%.0f",
                                                      {.resetValue = defaults.farEnd}) ||
                   modified;

        ImGui::Dummy(ImVec2(0.0f, 20.0f));  // Spacing

        modified = ImGuiEx::Keyframeable::SliderFloat("Near Blur", &dof.nearBlur, 0, 10,
                                                      Types::KeyframeablePropertyType::DepthOfFieldNearBlur, "%.2f",
                                                      {.resetValue = defaults.nearBlur}) ||
                   modified;
        modified = ImGuiEx::Keyframeable::SliderFloat("Near Start", &dof.nearStart, 0, 5000,
                                                      Types::KeyframeablePropertyType::DepthOfFieldNearStart, "%.0f",
                                                      {.resetValue = defaults.nearStart}) ||
                   modified;
        modified = ImGuiEx::Keyframeable::SliderFloat("Near End", &dof.nearEnd, 0, 5000,
                                                      Types::KeyframeablePropertyType::DepthOfFieldNearEnd, "%.0f",
                                                      {.resetValue = defaults.nearEnd}) ||
                   modified;

        ImGui::Dummy(ImVec2(0.0f, 20.0f));  // Spacing

        modified = ImGuiEx::Keyframeable::SliderFloat("Bias", &dof.bias, 0.1f, 10,
                                                      Types::KeyframeablePropertyType::DepthOfFieldBias, "%.2f",
                                                      {.resetValue = defaults.bias}) ||
                   modified;

        if (modified)
        {
            UpdateDof();
        }

        EndSection(Section::DepthOfField);
    }

    // The Sky section (Features_SkyReplace): the mod's sky in place of the map's, from an image or a gradient
    void VisualsMenu::RenderBlur()
    {
        using Components::VisualsLogic::Section;
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_ScreenBlur))
            return;
        bool modified = false;

        const bool open = DrawSection(Section::Blur, ICON_FA_WIND "  Blur", "Back to none", &modified);
        auto& dof = visuals.dof;
        const auto& defaults = defaultVisuals.dof;
        if (modified)
        {
            dof.screenBlur = defaults.screenBlur;
            if (dof.HasOlderBlurType())
                dof.blurType = static_cast<float>(Types::DoF::BLUR_DEPTH);
            dof.blurStrength = defaults.blurStrength;
            dof.blurAngle = defaults.blurAngle;
            dof.blurCenterX = defaults.blurCenterX;
            dof.blurCenterY = defaults.blurCenterY;
            dof.blurClearRadius = defaults.blurClearRadius;
            NameChange("reset Blur", "Blur back to none");
        }
        if (!open)
        {
            if (modified)
                UpdateDof();
            return;
        }

        constexpr std::array BLUR_TYPES = {"None", "Directional", "Radial"};
        Types::KeyframeValue value(static_cast<float>(dof.GetScreenBlur()));
        modified = ImGuiEx::Keyframeable::Row(
                       "Type", Property::ScreenBlurType, value,
                       [&](const char* id, Types::KeyframeValue& edited) {
                           int type = std::clamp(static_cast<int>(std::round(edited.floatingPoint)), 0, 2);
                           const bool changed = ImGui::Combo(id, &type, BLUR_TYPES.data(), 3);
                           if (changed)
                               edited.floatingPoint = static_cast<float>(type);
                           return changed;
                       },
                       "Blur Type") ||
                   modified;
        dof.screenBlur = std::clamp(std::round(value.floatingPoint), 0.0f, 2.0f);
        ImGui::SetItemTooltip("Directional smears the picture along a direction, as if moving fast; Radial streaks "
                              "it out from a point, as if zooming. Over the depth of field and the flare.");
        const auto type = static_cast<int>(dof.screenBlur);

        if (type != 0)
        {
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
            modified = ImGuiEx::Keyframeable::SliderFloat("Strength", &dof.blurStrength, 0, 0.3f,
                                                          Property::DepthOfFieldBlurStrength, "%.3f",
                                                          {.resetValue = defaults.blurStrength}) ||
                       modified;
            if (type == Types::DoF::BLUR_DIRECTIONAL)
            {
                modified = ImGuiEx::Keyframeable::SliderFloat("Angle", &dof.blurAngle, -180, 180,
                                                              Property::DepthOfFieldBlurAngle, "%.0f\xC2\xB0",
                                                              {.resetValue = defaults.blurAngle}) ||
                           modified;
            }
            else
            {
                modified = ImGuiEx::Keyframeable::SliderFloat("Center X", &dof.blurCenterX, 0, 1,
                                                              Property::DepthOfFieldBlurCenterX, "%.2f",
                                                              {.resetValue = defaults.blurCenterX}) ||
                           modified;
                modified = ImGuiEx::Keyframeable::SliderFloat("Center Y", &dof.blurCenterY, 0, 1,
                                                              Property::DepthOfFieldBlurCenterY, "%.2f",
                                                              {.resetValue = defaults.blurCenterY}) ||
                           modified;
                modified = ImGuiEx::Keyframeable::SliderFloat("Clear Radius", &dof.blurClearRadius, 0, 1,
                                                              Property::DepthOfFieldBlurClearRadius, "%.2f",
                                                              {.resetValue = defaults.blurClearRadius}) ||
                           modified;
                ImGui::SetItemTooltip("How far out from the centre stays sharp");
            }
        }

        if (modified)
            UpdateDof();
        EndSection(Section::Blur);
    }

    void VisualsMenu::RenderSky()
    {
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_SkyReplace))
            return;
        using Components::VisualsLogic::Section;
        namespace Sky = Components::SkyReplace;
        auto settings = Sky::GetSettings();
        bool reset = false;
        const bool open = DrawSection(Section::Sky, ICON_FA_CLOUD "  Sky", "Back to the map's sky", &reset);
        if (reset)
        {
            settings = Sky::Settings{};
            Sky::SetSettings(settings);
        }
        if (!open)
            return;

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

        ImGui::Checkbox("Replace the Sky##sky", &settings.enabled);
        ImGui::SetItemTooltip("Draws this sky where the map's is (wherever nothing is in front)");
        ImGui::BeginDisabled(!settings.enabled);

        label("Sky");
        constexpr std::array KINDS = {"An image", "A gradient"};
        int kind = static_cast<int>(settings.kind);
        if (ImGui::Combo("##skyKind", &kind, KINDS.data(), static_cast<int>(KINDS.size())))
            settings.kind = static_cast<Sky::Kind>(kind);

        if (settings.kind == Sky::Kind::Image)
        {
            label("Image");
            const auto name = settings.imagePath.empty()
                                  ? std::string("Choose...")
                                  : std::filesystem::path(settings.imagePath).filename().string();
            if (ImGui::Button(std::format("{}##skyImage", name).c_str(), ImVec2(-FLT_MIN, 0)))
            {
                if (const auto path = PathUtils::OpenFileDialog(
                        false, OFN_EXPLORER | OFN_FILEMUSTEXIST,
                        "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.dds;*.hdr;*.exr\0HDRIs\0*.hdr;*.exr\0All Files\0*.*\0",
                        "png"))
                    settings.imagePath = path->string();
            }
            ImGui::SetItemTooltip("%s", settings.imagePath.empty() ? "A PNG, JPEG, BMP, TIFF or DDS, or an HDRI (.hdr, .exr)"
                                                                   : settings.imagePath.c_str());
            // an HDRI's exposure, before it's tone mapped to the picture
            const auto extension = std::filesystem::path(settings.imagePath).extension().string();
            if (extension == ".hdr" || extension == ".exr" || extension == ".HDR" || extension == ".EXR")
            {
                slider("Exposure", "##skyExposure", &settings.exposure, -8, 8, "%+.1f stops");
                ImGui::SetItemTooltip("An HDRI is brighter than a screen shows: this sets how bright, before it's "
                                      "tone mapped");
            }
            label("Kind of Image");
            constexpr std::array MAPPINGS = {"360\xC2\xB0 panorama", "Photo"};
            int mapping = static_cast<int>(settings.mapping);
            if (ImGui::Combo("##skyMapping", &mapping, MAPPINGS.data(), static_cast<int>(MAPPINGS.size())))
                settings.mapping = static_cast<Sky::Mapping>(mapping);
            ImGui::SetItemTooltip("A panorama covers the whole sky (2:1, equirectangular); a photo stands on the "
                                  "horizon, mirrored round");
            if (settings.mapping == Sky::Mapping::Photo)
                slider("Photo Width", "##skyPhotoWidth", &settings.photoWidth, 20, 360, "%.0f\xC2\xB0");
        }
        else
        {
            label("Start from");
            if (ImGui::BeginCombo("##skyGradient", "..."))
            {
                const auto names = Sky::GetGradientNames();
                for (std::size_t i = 0; i < names.size(); ++i)
                    if (ImGui::Selectable(names[i]))
                        settings = Sky::ApplyGradient(settings, i);
                ImGui::EndCombo();
            }
            label("Top");
            ImGui::ColorEdit3("##skyTop", glm::value_ptr(settings.top), ImGuiColorEditFlags_Float);
            label("Horizon");
            ImGui::ColorEdit3("##skyHorizon", glm::value_ptr(settings.horizonColor), ImGuiColorEditFlags_Float);
            label("Below");
            ImGui::ColorEdit3("##skyBottom", glm::value_ptr(settings.bottom), ImGuiColorEditFlags_Float);
            slider("Softness", "##skySoftness", &settings.horizonSoftness, 0.02f, 1.0f);
        }

        ImGui::Dummy(ImVec2(0, 4));
        slider("Rotation", "##skyRotation", &settings.rotation, -180, 180, "%.0f\xC2\xB0");
        slider("Drift", "##skyDrift", &settings.drift, -10, 10, "%.2f\xC2\xB0/s");
        ImGui::SetItemTooltip("Keeps turning it as the film plays, as clouds move");
        slider("Horizon Height", "##skyHorizonHeight", &settings.horizon, -30, 30, "%.1f\xC2\xB0");

        ImGui::Dummy(ImVec2(0, 4));
        slider("Brightness", "##skyBrightness", &settings.brightness, 0, 3);
        slider("Saturation", "##skySaturation", &settings.saturation, 0, 2);
        label("Tint");
        ImGui::ColorEdit3("##skyTint", glm::value_ptr(settings.tint), ImGuiColorEditFlags_Float);
        slider("Opacity", "##skyOpacity", &settings.opacity, 0, 1);
        ImGui::SetItemTooltip("0 the map's sky, 1 this one");
        slider("Edge Softness", "##skyFeather", &settings.feather, 0, 4, "%.1f px");
        ImGui::SetItemTooltip("How softly it meets what's in front of it");

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Checkbox("Match the Map's Lighting##skyMatch", &settings.matchLighting);
        ImGui::SetItemTooltip("Lights the map to go with this sky: the colour of its light, its brightness, and haze "
                              "toward the distance");
        if (settings.matchLighting)
        {
            slider("Light Color", "##skyLightColor", &settings.lightColor, 0, 1);
            ImGui::SetItemTooltip("How much the map takes on the colour of the sky's light (warm at sunset, blue at "
                                  "night)");
            slider("Light Brightness", "##skyLightBrightness", &settings.lightBrightness, 0, 1);
            ImGui::SetItemTooltip("How much the map takes on the sky's brightness: darker under a night or overcast "
                                  "sky, brighter under a bright one");
            slider("Haze", "##skyHaze", &settings.haze, 0, 1);
            ImGui::SetItemTooltip("Far things fade into the sky's colour, as with air in the distance");
            if (settings.haze > 0.0f)
            {
                slider("Haze Start", "##skyHazeStart", &settings.hazeStart, 0, 5000, "%.0f");
                ImGui::SetItemTooltip("Nothing nearer than this is hazed (game units)");
                slider("Haze Distance", "##skyHazeDistance", &settings.hazeDistance, 200, 30000, "%.0f");
                ImGui::SetItemTooltip("How far past the start most of the haze is (game units)");
            }
        }

        ImGui::EndDisabled();
        Sky::SetSettings(settings);
        EndSection(Section::Sky);
    }

    void VisualsMenu::RenderSun()
    {
        using Components::VisualsLogic::Section;
        bool modified = false;

        const bool open = DrawSection(Section::Sun, ICON_FA_SUN "  Sun", "Back to the map's", &modified);
        if (modified)
        {
            visuals.sunBrightness = defaultVisuals.sunBrightness;
            visuals.sunColor = defaultVisuals.sunColor;
            visuals.sunDirection = defaultVisuals.sunDirection;
            visuals.skyRotation = defaultVisuals.skyRotation;
            NameChange("reset the Sun", "The Sun back to the map's");
        }
        if (!open)
        {
            if (modified)
                UpdateSun();
            return;
        }

        sunRowsShownAt = ImGui::GetFrameCount();
        modified = ImGuiEx::Keyframeable::ColorEdit3("Color", glm::value_ptr(visuals.sunColor),
                                                     Types::KeyframeablePropertyType::SunLightColor,
                                                     defaultVisuals.sunColor, "Sun Color") ||
                   modified;

        // the color from a temperature, as Blender's blackbody (warm low, blue high)
        {
            Types::KeyframeValue value(visuals.sunColor);
            if (ImGuiEx::Keyframeable::Row("Temperature", Types::KeyframeablePropertyType::SunLightColor, value,
                                           [&](const char* id, Types::KeyframeValue& edited) {
                                               if (!ImGuiEx::DragSlider(id, &sunTemperature, 1000.0f, 12000.0f,
                                                                        "%.0f K", {.resetValue = 5500.0f}))
                                                   return false;
                                               edited.vector3 = Components::VisualsLogic::Blackbody(sunTemperature);
                                               return true;
                                           },
                                           "Sun Color"))
            {
                visuals.sunColor = value.vector3;
                modified = true;
            }
            ImGui::SetItemTooltip("Sets the Color from a temperature: 1900 K candle, 3000 K sunrise, 5500 K midday, "
                                  "6500 K overcast, 10000 K blue sky");
        }

        // (a map's sun is 13 or 14: the bar goes to twice the map's)
        const auto brightest = Components::VisualsLogic::GetSunBrightnessMax(defaultVisuals.sunBrightness);
        modified = ImGuiEx::Keyframeable::SliderFloat("Strength", &visuals.sunBrightness, 0, brightest,
                                                      Types::KeyframeablePropertyType::SunLightBrightness, "%.2f",
                                                      {.resetValue = defaultVisuals.sunBrightness}, "Sun Strength") ||
                   modified;

        // the direction as elevation (above the horizon) and rotation (round it), as Blender's sun is aimed
        {
            const auto toSun = ToSun();
            float elevation = glm::degrees(std::asin(std::clamp(toSun.z, -1.0f, 1.0f)));
            float rotation = glm::degrees(std::atan2(toSun.y, toSun.x));
            if (rotation < 0.0f)
                rotation += 360.0f;
            const auto directionOf = [&](float e, float r) {
                const float pitch = glm::radians(e), yaw = glm::radians(r);
                return SunAngles({std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)});
            };
            Types::KeyframeValue value(visuals.sunDirection);
            bool turned = ImGuiEx::Keyframeable::Row("Elevation", Types::KeyframeablePropertyType::SunLightDirection,
                                                     value,
                                                     [&](const char* id, Types::KeyframeValue& edited) {
                                                         if (!ImGuiEx::DragSlider(id, &elevation, -10.0f, 90.0f,
                                                                                  "%.1f\xC2\xB0"))
                                                             return false;
                                                         edited.vector3 = directionOf(elevation, rotation);
                                                         return true;
                                                     },
                                                     "Sun Direction");
            ImGui::SetItemTooltip("How high the sun is: 0 on the horizon (long shadows), 90 straight above");
            turned = ImGuiEx::Keyframeable::Row("Rotation", Types::KeyframeablePropertyType::SunLightDirection, value,
                                                [&](const char* id, Types::KeyframeValue& edited) {
                                                    if (!ImGuiEx::DragSlider(id, &rotation, 0.0f, 360.0f,
                                                                             "%.1f\xC2\xB0"))
                                                        return false;
                                                    edited.vector3 = directionOf(elevation, rotation);
                                                    return true;
                                                },
                                                "Sun Direction") ||
                     turned;
            ImGui::SetItemTooltip("Which way the sun is, round the horizon");
            if (turned)
            {
                visuals.sunDirection = value.vector3;
                modified = true;
            }

            // aimed from the view
            const auto view = Components::Tracking::GetView();
            const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
            if (ImGui::Button(aimingSun ? ICON_FA_CROSSHAIRS " Drag on Picture..." : ICON_FA_CROSSHAIRS " Aim on Picture",
                              ImVec2(third, 0)))
                aimingSun = !aimingSun;
            ImGui::SetItemTooltip("Then drag on the picture to where the sun should be (Escape to stop)");
            ImGui::SameLine();
            ImGui::BeginDisabled(!view);
            const auto aim = [&](glm::vec3 direction, const char* step) {
                visuals.sunDirection = SunAngles(glm::normalize(direction));
                ImGuiEx::Keyframeable::KeyValueHere(
                    Components::KeyframeManager::Get().GetProperty(Types::KeyframeablePropertyType::SunLightDirection),
                    Types::KeyframeValue(visuals.sunDirection), step);
                modified = true;
            };
            if (ImGui::Button("From Camera", ImVec2(third, 0)) && view)
                aim(-view->axis[0], "Aim the Sun");
            ImGui::SetItemTooltip("The sun behind the camera: it lights what you see head on");
            ImGui::SameLine();
            if (ImGui::Button("Into Camera", ImVec2(third, 0)) && view)
                aim(view->axis[0], "Aim the Sun");
            ImGui::SetItemTooltip("The sun in the middle of the view: backlight, and the lens flare's sun there");
            ImGui::EndDisabled();
        }

        // (a game whose sky turns: the sky with the sun, as its picture of the sun's place)
        if (Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_SkyRotation)
            modified = ImGuiEx::Keyframeable::SliderFloat("Sky Rotation", &visuals.skyRotation, 0.0f, 360.0f,
                                                          Types::KeyframeablePropertyType::SunSkyRotation, "%.1f",
                                                          {.resetValue = defaultVisuals.skyRotation}, "Sky Rotation") ||
                       modified;

        if (modified)
        {
            UpdateSun();
        }

        EndSection(Section::Sun);
    }

    namespace
    {
        // the shadow qualities side by side in a column, the chosen one selected; the one clicked
        std::optional<Components::QualityLogic::ShadowQuality> DrawShadowQualities(
            const char* id, Components::QualityLogic::ShadowQuality chosen, float width)
        {
            using namespace Components::QualityLogic;
            std::optional<ShadowQuality> clicked;
            const auto each = (width - 2.0f) / static_cast<float>(SHADOW_QUALITIES.size());
            ImGui::PushID(id);
            // (narrow: "Game" fits a third of a column)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, ImGui::GetStyle().FramePadding.y));
            for (const auto shadows : SHADOW_QUALITIES)
            {
                if (shadows != SHADOW_QUALITIES.front())
                    ImGui::SameLine(0, 1.0f);
                const bool selected = shadows == chosen;
                if (selected)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                }
                const auto name = std::string(GetName(shadows));
                if (ImGui::Button(name.c_str(), ImVec2(each, 0)))
                    clicked = shadows;
                if (selected)
                    ImGui::PopStyleColor(2);
                auto lower = name;
                std::ranges::transform(lower, lower.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
                ImGuiEx::Keyframeable::Record(std::format("visuals.quality.{}.{}", id, lower));
                const auto size = GetShadowMapSize(shadows);
                ImGui::SetItemTooltip("%s", std::format("{} × {}{}", size, size,
                                                        shadows == ShadowQuality::Game ? " (the game's)" : "")
                                                .c_str());
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
            return clicked;
        }
    }  // namespace

    void VisualsMenu::RenderTextureRow(float x, float width)
    {
        // Textures: Game / Pack, this PC's like Shadow quality, the same while Record
        // runs; Pack greyed when the map has no pack
        namespace TextureLogic = Components::TextureLogic;
        const auto status = Components::TexturePack::GetStatus();
        const bool noPack = !status.map.empty() && !status.mapHasPack;
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Textures");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x);
        auto changed = quality;
        const auto each = (width - 1.0f) / 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, ImGui::GetStyle().FramePadding.y));
        // (a map without a pack shows Game, the setting kept for maps that have one)
        const bool drawsPack = quality.texturePack && !noPack;
        for (const bool pack : {false, true})
        {
            if (pack)
                ImGui::SameLine(0, 1.0f);
            const bool selected = drawsPack == pack;
            if (selected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            }
            ImGui::BeginDisabled(pack && noPack);
            if (ImGui::Button(pack ? "Pack##textures" : "Game##textures", ImVec2(each, 0)))
                changed.texturePack = pack;
            ImGui::EndDisabled();
            if (selected)
                ImGui::PopStyleColor(2);
            ImGuiEx::Keyframeable::Record(pack ? "visuals.quality.textures.pack" : "visuals.quality.textures.game");
            ImGui::SetItemTooltip("%s", pack ? "Upscaled by Iris Studio" : "The game's own");
        }
        ImGui::PopStyleVar();
        if (changed != quality)
            SetQuality(changed);

        // what the pack has for the map, under the label: its name brighter, or the map without one in the note's
        // colour (none until a demo's map is in)
        if (status.map.empty())
            return;
        const auto indent = ImGui::GetFontSize() * 1.5f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::Indent(indent);
        auto line = TextureLogic::GetStatusLine(status);
        const auto lead = std::min(noPack ? line.find('.') + 1 : TextureLogic::GetStatusLead(status), line.size());
        // (the repair's note, in the note's colour, after the rest)
        const bool repair = status.repair && status.mapHasPack && line.ends_with(TextureLogic::REPAIR_NOTE);
        if (repair)
            line.resize(line.size() - TextureLogic::REPAIR_NOTE.size());
        // the lead in its colour, then as many words as fit after it, the rest wrapped from the line's start
        auto rest = line.substr(lead);
        if (lead > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, noPack ? Theme::Note() : Theme::Done());
            ImGui::TextUnformatted(line.substr(0, lead).c_str());
            ImGui::PopStyleColor();
            const auto room = ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x;
            std::size_t fits = 0;
            for (auto space = rest.find(' ', 1); ; space = rest.find(' ', space + 1))
            {
                const auto end = space == std::string::npos ? rest.size() : space;
                if (ImGui::CalcTextSize(rest.substr(0, end).c_str()).x > room)
                    break;
                fits = end;
                if (space == std::string::npos)
                    break;
            }
            if (fits > 0)
            {
                ImGui::SameLine(0, 0);
                ImGui::TextDisabled("%s", rest.substr(0, fits).c_str());
                rest = rest.substr(std::min(fits + 1, rest.size()));
            }
        }
        if (!rest.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", rest.c_str());
            ImGui::PopTextWrapPos();
        }
        if (repair)
        {
            const std::string note(TextureLogic::REPAIR_NOTE);
            const auto after = ImGui::GetItemRectMax().x + ImGui::CalcTextSize(note.c_str()).x;
            if (after <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
                ImGui::SameLine(0, 0);
            ImGui::TextColored(Theme::Note(), "%s", note.c_str());
        }
        ImGui::Unindent(indent);
    }

    void VisualsMenu::RenderSurfaceRows(float x, float width)
    {
        namespace SurfaceLogic = Components::SurfaceLogic;

        // Surface depth: Off or On (no key: its Depth and Bump strength are keyed)
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Surface depth");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x);
        ImGui::PushID("surface");
        const auto each = (width - 1.0f) / 2.0f;
        for (const bool on : {false, true})
        {
            if (on)
                ImGui::SameLine(0, 1.0f);
            const bool selected = visuals.surfaceDepth == on;
            if (selected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            }
            if (ImGui::Button(on ? "On" : "Off", ImVec2(each, 0)))
                visuals.surfaceDepth = on;
            if (selected)
                ImGui::PopStyleColor(2);
            ImGuiEx::Keyframeable::Record(on ? "visuals.quality.surface.on" : "visuals.quality.surface.off");
            ImGui::SetItemTooltip(on ? "Relief from the normal maps" : "The game's surfaces");
        }
        ImGui::PopID();

        // Depth: dimmed while Off; changing it turns Surface depth on
        if (!visuals.surfaceDepth)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const auto depthBefore = visuals.surfaceDepthAmount;
        ImGuiEx::Keyframeable::SliderFloat("Depth", &visuals.surfaceDepthAmount, SurfaceLogic::MIN_DEPTH,
                                           SurfaceLogic::MAX_DEPTH, Types::KeyframeablePropertyType::SurfaceDepth,
                                           "%.1f in", {.resetValue = SurfaceLogic::DEFAULT_DEPTH}, "Surface Depth");
        if (!visuals.surfaceDepth)
            ImGui::PopStyleColor();
        if (visuals.surfaceDepthAmount != depthBefore && !visuals.surfaceDepth)
            visuals.surfaceDepth = true;

        // Bump strength: works On or Off, 1 the game's (T1)
        ImGuiEx::Keyframeable::SliderFloat(
            "Bump strength", &visuals.bumpStrength, SurfaceLogic::MIN_BUMP_STRENGTH, SurfaceLogic::MAX_BUMP_STRENGTH,
            Types::KeyframeablePropertyType::BumpStrength, visuals.bumpStrength == 1.0f ? "%.1f (game)" : "%.1f",
            {.origin = 1.0f, .resetValue = 1.0f}, "Bump Strength");
    }

    void VisualsMenu::SetSurface(bool on, float depth, float bumpStrength)
    {
        visuals.surfaceDepth = on;
        visuals.surfaceDepthAmount = depth;
        visuals.bumpStrength = bumpStrength;
    }

    void VisualsMenu::UpdateSurfaces()
    {
        if (!visualsInitialized || !(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_SurfaceDepth))
            return;
        Components::SurfaceDepth::SetOn(visuals.surfaceDepth);
        Components::SurfaceDepth::SetDepth(visuals.surfaceDepthAmount);
        Components::SurfaceDepth::SetBumpStrength(visuals.bumpStrength);
    }

    void VisualsMenu::RenderQuality()
    {
        using namespace Components::QualityLogic;
        const auto& game = Mod::GetGameInterface();
        if (!(game->GetSupportedFeatures() & Types::Features_ShadowQuality))
            return;

        using Components::VisualsLogic::Section;
        bool reset = false;
        const bool open = DrawSection(Section::Quality, ICON_FA_SLIDERS "  Quality", "Back to the game's", &reset);
        if (reset)
        {
            visuals.shadowDistance = defaultVisuals.shadowDistance;
            visuals.surfaceDepth = defaultVisuals.surfaceDepth;
            visuals.surfaceDepthAmount = defaultVisuals.surfaceDepthAmount;
            visuals.bumpStrength = defaultVisuals.bumpStrength;
            SetQuality({});
            NameChange("reset the Quality", "The Quality back to the game's");
        }

        if (!open)
            return;

        // the rows' controls from x to the right
        const auto x = ImGui::GetWindowWidth() * LOOK_COLUMN;
        const auto width = ImGui::GetWindowWidth() * (1.0f - LOOK_COLUMN) - ImGui::GetStyle().WindowPadding.x;

        ImGuiEx::Keyframeable::SliderFloat("Shadow distance", &visuals.shadowDistance, MIN_SHADOW_DISTANCE,
                                           MAX_SHADOW_DISTANCE, Types::KeyframeablePropertyType::ShadowDistance, "%.0f",
                                           {.resetValue = defaultVisuals.shadowDistance}, "Shadow Distance");

        // (where the rows' diamonds are, none: this PC's values aren't keyed)
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shadow quality");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x);
        auto changed = quality;
        if (const auto clicked = DrawShadowQualities("scene", quality.sceneShadows, width))
            changed.sceneShadows = *clicked;
        if (changed != quality)
            SetQuality(changed);

        if (game->GetSupportedFeatures() & Types::Features_TexturePack)
            RenderTextureRow(x, width);
        if (game->GetSupportedFeatures() & Types::Features_SurfaceDepth)
            RenderSurfaceRows(x, width);

        // the value comes in with a restart of the renderer, with the game view's loading screen; none while a
        // capture runs or the renderer restarts
        const auto& capture = Components::CaptureManager::Get();
        const auto size = game->GetSunShadowMapSize();
        if (size && *size != GetShadowMapSize(quality.sceneShadows) && !capture.IsCapturing() &&
            !capture.IsRestarting())
        {
            if (ImGui::Button(std::format("Apply {}", GetName(quality.sceneShadows)).c_str(),
                              ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                ApplySceneShadows();
            ImGuiEx::Keyframeable::Record("visuals.quality.apply");
            ImGui::SetItemTooltip("Restarts the renderer");
        }

        EndSection(Section::Quality);
    }

    namespace
    {
        constexpr auto RESHADE_HEADER = ICON_FA_WAND_MAGIC_SPARKLES "  ReShade";

        // the property of a ReShade id, among those keys can be added to
        const Types::KeyframeableProperty* FindReShadeProperty(const std::string& id)
        {
            const auto type = Components::ReShadeKeys::FindType(id);
            auto& keyframeManager = Components::KeyframeManager::Get();
            return type && keyframeManager.HasProperty(*type) ? &keyframeManager.GetProperty(*type) : nullptr;
        }

        // ReShade's ids with keys, those of effects not loaded too
        std::vector<std::string> GetKeyedReShadeIds()
        {
            auto& keyframeManager = Components::KeyframeManager::Get();
            std::vector<std::string> ids;
            for (const auto type : Components::RuntimeProperties::GetAll())
            {
                if (!Components::RuntimeProperties::IsActorProperty(type) && keyframeManager.HasProperty(type) &&
                    !keyframeManager.GetKeyframes(keyframeManager.GetProperty(type)).empty())
                    ids.push_back(Components::RuntimeProperties::Find(type)->id);
            }
            return ids;
        }

        // A value's format as ReShade's menu shows it: whole numbers for ints and lists, three decimals otherwise,
        // its units, and an arrow when it is past its range (ReShade's Ctrl+click allows it)
        std::string GetReShadeFormat(const ReShade::Uniform& uniform, float value)
        {
            std::string format = uniform.integer ? "%.0f" : "%.3f";
            for (const auto c : uniform.units)
                format += c == '%' ? std::string("%%") : std::string(1, c);
            if (const auto out = Components::ReShadeLogic::OutOfRange(uniform, value))
                format += out > 0 ? " " ICON_FA_ARROW_UP_LONG : " " ICON_FA_ARROW_DOWN_LONG;
            return format;
        }

        // A value of an effect as a row: its diamond, label and control, keyed as the other rows are; changed
        // without keys, the preset's own value changes, as in ReShade's menu (D1)
        void DrawReShadeValue(const ReShade::Uniform& uniform, const Types::KeyframeableProperty& property,
                              const ReShade::Value& preset, const std::string& effectName)
        {
            using namespace Components;
            auto& keyframeManager = KeyframeManager::Get();
            const bool keyed = !keyframeManager.GetKeyframes(property).empty();
            const auto shown = keyed ? ReShadeLogic::FromKey(
                                           uniform, keyframeManager.Interpolate(property, Playback::GetTimelineTick()),
                                           preset)
                                     : preset;
            const auto name = std::format("{} {}", effectName, uniform.label);
            const auto label = uniform.label.c_str();
            auto value = preset;

            switch (uniform.kind)
            {
                case ReShade::Kind::Slider:
                case ReShade::Kind::Drag:
                {
                    const auto [low, high] = ReShadeLogic::GetSliderRange(uniform, shown[0], preset[0]);
                    const auto format = GetReShadeFormat(uniform, shown[0]);
                    ImGuiEx::Keyframeable::SliderFloat(label, &value[0], low, high, property.type, format.c_str(),
                                                       {.resetValue = preset[0]}, name.c_str());
                    break;
                }
                case ReShade::Kind::Bool:
                {
                    bool on = value[0] >= 0.5f;
                    ImGuiEx::Keyframeable::Checkbox(label, &on, property.type, name.c_str());
                    value[0] = on ? 1.0f : 0.0f;
                    break;
                }
                case ReShade::Kind::List:
                {
                    Types::KeyframeValue key(value[0]);
                    ImGuiEx::Keyframeable::Row(
                        label, property.type, key,
                        [&](const char* id, Types::KeyframeValue& edited) {
                            const auto last = static_cast<long>(uniform.items.size()) - 1;
                            const auto item =
                                static_cast<std::size_t>(std::clamp(std::lround(edited.floatingPoint), 0l, last));
                            bool changed = false;
                            if (ImGui::BeginCombo(id, uniform.items[item].c_str()))
                            {
                                for (std::size_t i = 0; i < uniform.items.size(); ++i)
                                {
                                    if (ImGui::Selectable(uniform.items[i].c_str(), i == item) && i != item)
                                    {
                                        edited.floatingPoint = static_cast<float>(i);
                                        changed = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            return changed;
                        },
                        name.c_str());
                    value[0] = key.floatingPoint;
                    break;
                }
                case ReShade::Kind::Color:
                    ImGuiEx::Keyframeable::ColorEdit3(label, value.data(), property.type,
                                                      glm::vec3(preset[0], preset[1], preset[2]), name.c_str());
                    break;
                case ReShade::Kind::Vector:
                {
                    auto key = ReShadeLogic::ToKey(uniform, value);
                    const auto [low, high] =
                        ReShadeLogic::GetSliderRange(uniform, std::max({shown[0], shown[1], shown[2]}),
                                                     std::max({preset[0], preset[1], preset[2]}));
                    const auto format = GetReShadeFormat(uniform, 0.0f);
                    ImGuiEx::Keyframeable::Row(
                        label, property.type, key,
                        [&](const char* id, Types::KeyframeValue& edited) {
                            bool changed = false;
                            const auto count = static_cast<int>(std::min(uniform.components, 3u));
                            ImGui::PushID(id);
                            ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
                            for (int i = 0; i < count; ++i)
                            {
                                if (i > 0)
                                    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                                ImGui::PushID(i);
                                changed = ImGuiEx::DragSlider("##component", &edited.vector3[i], low, high,
                                                              format.c_str(), {.resetValue = preset[i]}) ||
                                          changed;
                                ImGui::PopID();
                                ImGui::PopItemWidth();
                            }
                            ImGui::PopID();
                            return changed;
                        },
                        name.c_str());
                    value = ReShadeLogic::FromKey(uniform, key, preset);
                    break;
                }
            }
            if (!uniform.tooltip.empty())
                ImGui::SetItemTooltip("%s", uniform.tooltip.c_str());

            // (with keys the row keyed it at the playhead)
            if (!keyed && value != preset)
                ReShadeKeys::ChangePreset(uniform.id, false, preset, value, "change " + name);
        }
    }  // namespace

    void VisualsMenu::SetReShadeFoldOpen(const std::string& techniqueId, bool open)
    {
        if (open)
            openReShadeFolds.insert(techniqueId);
        else
            openReShadeFolds.erase(techniqueId);
    }

    void VisualsMenu::RenderReShade()
    {
        using namespace Components;
        using VisualsLogic::Section;
        const auto status = ReShade::GetStatus();
        const auto effects = ReShade::GetEffects();
        const auto values = ReShade::GetValues();
        if (status != ReShade::Status::Ready || !effects || values.generation != effects->generation)
        {
            if (!DrawSection(Section::ReShade, RESHADE_HEADER))
                return;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", status == ReShade::Status::NotFound ? "ReShade not found"
                                      : status == ReShade::Status::PerformanceMode
                                          ? "Turn off ReShade's Performance Mode (Home tab) to edit here"
                                          : "Waiting for ReShade's first frame");
            ImGui::PopTextWrapPos();
            ImGuiEx::Keyframeable::Record("visuals.reshade.state");
            EndSection(Section::ReShade);
            return;
        }

        auto& keyframeManager = KeyframeManager::Get();
        const auto keyedIds = GetKeyedReShadeIds();
        bool reset = false;
        const bool open = DrawSection(Section::ReShade, RESHADE_HEADER, "Back to the preset's", &reset);
        if (reset)
        {
            // (the keys of effects that aren't loaded are kept: they have their own fold)
            UndoHistory::Get().BeginGroup("reset ReShade");
            for (const auto& id : keyedIds)
            {
                if (const auto property = FindReShadeProperty(id))
                {
                    keyframeManager.ClearKeyframes(*property);
                    keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(*property));
                }
            }
            UndoHistory::Get().EndGroup();
            Toasts::Show("ReShade back to the preset's values", true);
        }
        if (!open)
            return;

        std::vector<bool> on(effects->techniques.size());
        std::size_t onCount = 0;
        for (std::size_t i = 0; i < on.size(); ++i)
        {
            const bool presetOn = i < values.presetOn.size() && values.presetOn[i];
            on[i] = presetOn || (i < values.drawnOn.size() && values.drawnOn[i]);
            onCount += presetOn ? 1 : 0;
        }
        const std::set<std::string> keyed(keyedIds.begin(), keyedIds.end());
        const auto folds = ReShadeLogic::GetFolds(*effects, on, keyed, keptReShadeEffects);
        const auto presetName =
            std::filesystem::path(std::u8string(effects->presetPath.begin(), effects->presetPath.end())).filename();
        const auto keyedCount = folds.listed.size() - folds.kept - std::min(onCount, folds.listed.size() - folds.kept);
        ImGui::TextDisabled("%s \xC2\xB7 %zu on, %zu keyed",
                            presetName.string().c_str(), onCount, keyedCount);
        ImGui::SetItemTooltip("Its clock follows the demo");

        // (a key picked on the timeline: the fold of its row)
        if (const auto row = std::exchange(reShadeRowToShow, std::nullopt))
        {
            for (const auto* list : {&folds.listed, &folds.others})
                for (const auto& fold : *list)
                    if (effects->techniques[fold.technique].id == *row ||
                        std::ranges::any_of(fold.uniforms,
                                            [&](std::size_t u) { return effects->uniforms[u].id == *row; }))
                        SetReShadeFoldOpen(effects->techniques[fold.technique].id, true);
        }

        for (const auto& fold : folds.listed)
            RenderReShadeFold(*effects, values, fold);
        for (const auto& missing : ReShadeLogic::GetMissing(*effects, keyedIds))
            RenderReShadeMissing(missing);

        const bool allOpen = ImGui::TreeNodeEx(std::format("All effects ({})##reshadeAll", folds.others.size()).c_str(),
                                               ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGuiEx::Keyframeable::Record("visuals.reshade.all");
        if (allOpen)
        {
            ImGui::TreePop();
            for (const auto& fold : folds.others)
                RenderReShadeFold(*effects, values, fold);
        }

        EndSection(Section::ReShade);
    }

    Components::VisualsLogic::SectionMarks VisualsMenu::GetSectionMarks(Components::VisualsLogic::Section section)
    {
        using namespace Components;
        using VisualsLogic::Section;
        auto& keyframeManager = KeyframeManager::Get();
        const auto keyed = [&](Types::KeyframeablePropertyType type) {
            return !keyframeManager.GetKeyframes(keyframeManager.GetProperty(type)).empty();
        };
        auto marks = VisualsLogic::GetMarks(section, visuals, defaultVisuals, keyed);
        // (what isn't a setting of the tab: this PC's Quality, ReShade's keys, the level's effects)
        if (section == Section::Quality)
            marks.changed = marks.changed || quality != QualityLogic::Settings{};
        else if (section == Section::ReShade)
            marks.keyed = !GetKeyedReShadeIds().empty();
        else if (section == Section::MapEffects)
            if (const auto effects = Mod::GetGameInterface()->GetMapEffects())
                marks.changed = std::ranges::any_of(effects->kinds, [](const auto& kind) { return !kind.shown; });
        return marks;
    }

    void VisualsMenu::RenderReShadeFold(const ReShade::Effects& effects, const ReShade::Values& values,
                                        const Components::ReShadeLogic::Fold& fold)
    {
        using namespace Components;
        const auto& technique = effects.techniques[fold.technique];
        const auto property = FindReShadeProperty(technique.id);
        if (!property)
            return;
        auto& keyframeManager = KeyframeManager::Get();
        const bool keyed = !keyframeManager.GetKeyframes(*property).empty();
        const bool presetOn = fold.technique < values.presetOn.size() && values.presetOn[fold.technique];
        const bool on =
            keyed ? KeyframeLogic::GetStepValue(
                        keyframeManager.Interpolate(*property, Playback::GetTimelineTick()).floatingPoint) != 0.0f
                  : presetOn;
        const bool hasValues = !fold.uniforms.empty();
        const bool open = hasValues && openReShadeFolds.contains(technique.id);

        ImGui::PushID(technique.id.c_str());
        // the fold's line, a click on it opens or closes it: its arrow, on/off, diamond, name and file
        const auto start = ImGui::GetCursorPos();
        if (ImGui::Selectable("##fold", false, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0.0f, ImGui::GetFrameHeight())) &&
            hasValues)
            SetReShadeFoldOpen(technique.id, !open);
        ImGuiEx::Keyframeable::Record("visuals.reshade.fold." + technique.id);
        if (!technique.tooltip.empty())
            ImGui::SetItemTooltip("%s", technique.tooltip.c_str());
        ImGui::SetCursorPos(start);
        ImGui::AlignTextToFramePadding();
        const auto arrowWidth = ImGui::GetFontSize() * 1.1f;
        if (hasValues)
            ImGui::TextDisabled("%s", open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
        else
            ImGui::Dummy(ImVec2(arrowWidth, ImGui::GetFontSize()));
        ImGui::SameLine(start.x + arrowWidth + ImGui::GetStyle().ItemSpacing.x);

        const auto onName = technique.label + " on";
        bool checked = on;
        if (ImGui::Checkbox("##on", &checked))
        {
            const auto step = std::format("turn {} {}", technique.label, checked ? "on" : "off");
            if (keyed)
                ImGuiEx::Keyframeable::KeyValueHere(*property, Types::KeyframeValue(checked ? 1.0f : 0.0f),
                                                    onName.c_str(), step);
            else
                ReShadeKeys::ChangePreset(technique.id, true, {presetOn ? 1.0f : 0.0f}, {checked ? 1.0f : 0.0f}, step);
        }
        ImGuiEx::Keyframeable::Record("visuals.reshade.on." + technique.id);
        ImGui::SetItemTooltip("%s", keyed ? "Keyed at the playhead" : "On or off in the preset");
        ImGui::SameLine();
        ImGuiEx::Keyframeable::Diamond(*property, Types::KeyframeValue(on ? 1.0f : 0.0f), onName.c_str(),
                                       ImGui::GetFontSize() * 1.5f);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        // the name, then the file at the right when both fit; a name longer than the line ends with "..." and shows
        // whole in a tooltip
        const auto& style = ImGui::GetStyle();
        const auto right = ImGui::GetWindowWidth() - style.WindowPadding.x;
        const auto nameSize = ImGui::CalcTextSize(technique.label.c_str());
        const auto fileWidth = ImGui::CalcTextSize(technique.file.c_str()).x;
        const auto nameX = ImGui::GetCursorPosX();
        const bool withFile = nameX + nameSize.x + 20.0f + fileWidth <= right;
        const auto room = right - nameX;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(on ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        if (nameSize.x > room && room > 0.0f)
        {
            const auto at = ImGui::GetCursorScreenPos() + ImVec2(0.0f, style.FramePadding.y);
            ImGui::Dummy(ImVec2(room, nameSize.y));
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, at + ImVec2(room, nameSize.y), at.x + room,
                                      at.x + room, technique.label.c_str(), nullptr, &nameSize);
            ImGui::SetItemTooltip("%s (%s)", technique.label.c_str(), technique.file.c_str());
        }
        else
        {
            ImGui::TextUnformatted(technique.label.c_str());
        }
        ImGui::PopStyleColor();
        if (withFile)
        {
            ImGui::SameLine(right - fileWidth);
            ImGui::TextDisabled("%s", technique.file.c_str());
        }
        ImGui::PopID();

        if (!open)
            return;
        // (ReShade's labels are longer than the Look tab's others)
        const ImGuiEx::Keyframeable::LabelColumn labels(0.55f);
        const auto indent = ImGui::GetFontSize() * 0.6f;
        ImGui::Indent(indent);
        std::string category;
        for (const auto index : fold.uniforms)
        {
            const auto& uniform = effects.uniforms[index];
            const auto valueProperty = FindReShadeProperty(uniform.id);
            if (!valueProperty || index >= values.preset.size())
                continue;
            if (uniform.category != category && !uniform.category.empty())
                ImGui::TextDisabled("%s", uniform.category.c_str());
            category = uniform.category;
            DrawReShadeValue(uniform, *valueProperty, values.preset[index], technique.label);
        }
        ImGui::Unindent(indent);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    void VisualsMenu::RenderReShadeMissing(const Components::ReShadeLogic::Missing& missing)
    {
        using namespace Components;
        auto& keyframeManager = KeyframeManager::Get();
        std::size_t keys = 0;
        for (const auto& id : missing.ids)
        {
            if (const auto type = RuntimeProperties::FindById(id); type && keyframeManager.HasProperty(*type))
                keys += keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type)).size();
        }
        const auto name =
            missing.file.ends_with(".fx") ? missing.file.substr(0, missing.file.size() - 3) : missing.file;

        ImGui::PushID(("missing" + missing.file).c_str());
        const auto start = ImGui::GetCursorPos();
        ImGui::Selectable("##fold", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetFrameHeight()));
        ImGuiEx::Keyframeable::Record("visuals.reshade.missing." + missing.file);
        ImGui::SetItemTooltip("%s isn't loaded; its keys are kept. Right-click to delete them.",
                              missing.file.c_str());
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##missingMenu");
        if (ImGui::BeginPopup("##missingMenu"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete their keys"))
            {
                UndoHistory::Get().BeginGroup(std::format("delete the keys of {}", name));
                for (const auto& id : missing.ids)
                {
                    if (const auto type = RuntimeProperties::FindById(id); type && keyframeManager.HasProperty(*type))
                    {
                        const auto& property = keyframeManager.GetProperty(*type);
                        keyframeManager.ClearKeyframes(property);
                        keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
                    }
                }
                UndoHistory::Get().EndGroup();
                Toasts::Show(std::format("Deleted the keys of {}", name), true);
            }
            ImGui::EndPopup();
        }
        ImGui::SetCursorPos(start);
        ImGui::AlignTextToFramePadding();
        // (where the arrow and the on/off are on the others' lines: the diamond of keys)
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.1f + ImGui::GetFrameHeight(), ImGui::GetFontSize()));
        ImGui::SameLine();
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), ICON_FA_DIAMOND);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", name.c_str());
        const auto note = std::format("missing \xC2\xB7 {} kept", keys == 1 ? std::string("1 key")
                                                                            : std::format("{} keys", keys));
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                        ImGui::CalcTextSize(note.c_str()).x);
        ImGui::TextDisabled("%s", note.c_str());
        ImGui::PopID();
    }

    void VisualsMenu::UpdateShadows()
    {
        const auto& game = Mod::GetGameInterface();
        if (!visualsInitialized || !(game->GetSupportedFeatures() & Types::Features_ShadowQuality))
            return;
        const auto size = game->GetSunShadowMapSize();
        const auto dvar = game->GetDvar("sm_sunSampleSizeNear");
        if (!size || !dvar)
            return;
        const auto value = Components::QualityLogic::GetSampleSize(visuals.shadowDistance, *size);
        if (dvar->value->floating_point != value)
            dvar->value->floating_point = value;
    }

    void VisualsMenu::ApplySceneShadows()
    {
        const auto& game = Mod::GetGameInterface();
        auto& capture = Components::CaptureManager::Get();
        const auto size = game->GetSunShadowMapSize();
        const auto wanted = Components::QualityLogic::GetShadowMapSize(quality.sceneShadows);
        if (size && *size != wanted && !capture.IsCapturing() && !capture.IsRestarting())
            capture.RestartForShadows(wanted, Components::QualityLogic::GetName(quality.sceneShadows));
    }

    void VisualsMenu::SetQuality(const Components::QualityLogic::Settings& settings)
    {
        quality = settings;
        Components::TexturePack::SetOn(quality.texturePack);
        PreferencesConfiguration::Get().quality = Components::QualityLogic::ToJson(quality);
        Configuration::Get().Write(true);
    }

    void VisualsMenu::ApplyKeyed()
    {
        if (!visualsInitialized)
            return;

        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto tick = Components::Playback::GetTimelineTick();
        bool sun = false, filmtweaks = false, dof = false;
        for (const auto& [property, keys] : keyframeManager.GetKeyframes())
        {
            // (the campath sets none of them)
            if (keys.empty() || !Components::VisualsLogic::SetValue(visuals, property.type,
                                                                    keyframeManager.Interpolate(property, tick)))
                continue;
            keyedSettings.insert(property.type);
            const auto name = magic_enum::enum_name(property.type);
            sun = sun || name.starts_with("Sun");
            filmtweaks = filmtweaks || name.starts_with("Filmtweak");
            dof = dof || name.starts_with("DepthOfField");
        }
        if (sun)
            UpdateSun();
        if (filmtweaks)
            UpdateFilmtweaks();
        if (dof)
            UpdateDof();
    }

    void VisualsMenu::TakeMapDefaults()
    {
        const auto map = Mod::GetGameInterface()->GetMapVisuals();
        if (!visualsInitialized || !map)
            return;

        auto defaults = defaultVisuals;
        SetMapValues(defaults, *map);
        visuals = Components::VisualsLogic::FollowMap(
            visuals, defaultVisuals, defaults,
            [this](Types::KeyframeablePropertyType type) { return keyedSettings.contains(type); });
        defaultVisuals = defaults;
        keyedSettings.clear();
        UpdateAll();
    }

    void VisualsMenu::NameChange(std::string label, std::string message)
    {
        namedChange = std::move(label);
        namedMessage = std::move(message);
    }

    void VisualsMenu::TrackEdit(const Settings& frameStart, const Preset& presetBefore)
    {
        // (what the frame said about itself goes with it)
        const auto row = ImGuiEx::Keyframeable::TakeChanged();
        auto label = std::exchange(namedChange, std::nullopt);
        auto message = std::exchange(namedMessage, std::nullopt);
        if (!visualsInitialized)
            return;

        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto keyed = [&](Types::KeyframeablePropertyType type) {
            return !keyframeManager.GetKeyframes(keyframeManager.GetProperty(type)).empty();
        };
        const auto changed = Components::VisualsLogic::GetChanged(frameStart, visuals, keyed);
        if (changed.empty())
            return;

        // named as the tab said, else after the row changed ("change Temperature", as its keys' steps are), but a
        // switch, which is turned on or off ("turn Invert on")
        if (!label)
        {
            const auto& settings = Components::VisualsLogic::GetSettings();
            const auto kind = settings[changed.front()].kind;
            const bool aSwitch = changed.size() == 1 && (kind == Components::VisualsLogic::Kind::Switch ||
                                                         kind == Components::VisualsLogic::Kind::Shown);
            label = row && !aSwitch ? "change " + *row : Components::VisualsLogic::DescribeChange(changed, visuals);
        }
        if (edit.Track(frameStart, visuals, keyed, label))
        {
            // (a group, so that the steps the keys of the same control push meanwhile are one with it)
            Components::UndoHistory::Get().BeginGroup(*label);
            editPresets.reset();
            editMessage = message;
        }
        else if (message && !editMessage)
        {
            editMessage = message;
        }
        if (presetBefore.name != currentPreset.name || presetBefore.path != currentPreset.path)
            editPresets = std::pair(editPresets ? editPresets->first : presetBefore, currentPreset);
    }

    void VisualsMenu::EndEditWhenIdle()
    {
        if (!edit.IsOpen() || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left))
            return;

        const auto presets = std::exchange(editPresets, std::nullopt);
        const auto message = std::exchange(editMessage, std::nullopt);
        if (const auto step = edit.End(visuals))
        {
            // puts the settings it changed back as they were before or after it (and the preset it chose)
            const auto restore = [this](const Settings& from, const std::vector<std::size_t>& settings,
                                        const std::optional<Preset>& preset) {
                Components::VisualsLogic::Copy(from, visuals, settings);
                if (preset)
                    currentPreset = *preset;
                UpdateAll();
            };
            Components::UndoHistory::Get().Push(
                step->label,
                [restore, step = *step, presets] {
                    restore(step.before, step.settings, presets ? std::optional(presets->first) : std::nullopt);
                },
                [restore, step = *step, presets] {
                    restore(step.after, step.settings, presets ? std::optional(presets->second) : std::nullopt);
                });
            if (message)
                Toasts::Show(*message, true);
        }
        Components::UndoHistory::Get().EndGroup();
    }

    void VisualsMenu::UpdateDof()
    {
        Mod::GetGameInterface()->SetDof(visuals.dof);
    }

    void VisualsMenu::UpdateSun()
    {
        IWXMVM::Types::Sun sunSettings = {glm::make_vec3(visuals.sunColor), glm::make_vec3(visuals.sunDirection),
                                          visuals.sunBrightness, visuals.skyRotation};
        Mod::GetGameInterface()->SetSun(sunSettings);
    }

    void VisualsMenu::UpdateFilmtweaks()
    {
        Mod::GetGameInterface()->SetFilmtweaks(visuals.filmtweaks);
    }

    void VisualsMenu::UpdateHudInfo()
    {
        // Do not update HUD settings when using any free-floating camera
        // We do not want to overwrite cg_draw2D for example
        auto& camera = Components::CameraManager::Get().GetActiveCamera();
        if (camera->IsModControlledCameraMode() && !Mod::GetGameInterface()->AppliesHudInModCameras())
            return;

        Mod::GetGameInterface()->SetHudInfo(visuals.hudInfo);
    }

    void VisualsMenu::UpdateAll()
    {
        UpdateDof();
        UpdateSun();
        UpdateFilmtweaks();
        UpdateHudInfo();
    }

    nlohmann::json VisualsMenu::WriteDepthOfField() const
    {
        auto saved = nlohmann::json::object();
        auto& keyframeManager = Components::KeyframeManager::Get();
        for (const auto& setting : Components::VisualsLogic::GetSettings())
        {
            if (!setting.dvar.starts_with("r_dof"))
                continue;
            // (one with keys is the keys' as the film plays: not saved here, or it would count as changed each
            // moment and the project be written again and again)
            if (setting.property && keyframeManager.HasProperty(*setting.property) &&
                !keyframeManager.GetKeyframes(keyframeManager.GetProperty(*setting.property)).empty())
                continue;
            std::array<float, 4> values{};
            setting.write(visuals, values.data());
            saved[std::string(setting.dvar)] = std::vector<float>(values.begin(), values.begin() + setting.valueCount);
        }
        return saved;
    }

    void VisualsMenu::ReadDepthOfField(const nlohmann::json& saved)
    {
        if (saved.is_object())
            pendingDepthOfField = saved;
    }

    void VisualsMenu::LoadPreset(Preset preset)
    {
        Components::VisualConfiguration::Settings newVisuals = defaultVisuals;

        if (Components::VisualConfiguration::Load(preset.path, newVisuals))
        {
            visuals = newVisuals;
            UpdateAll();

            AddPresetToRecent(preset);
            currentPreset = preset;
            NameChange(std::format("load preset {}", preset.name), std::format("Loaded preset {}", preset.name));
        }
    }

    void VisualsMenu::LoadDefaultPreset()
    {
        visuals = defaultVisuals;
        currentPreset = defaultPreset;
        UpdateAll();
        NameChange("load the Default preset", "Loaded the Default preset");
    }

    void VisualsMenu::AddPresetToRecent(Preset newPreset)
    {
        for (size_t i = 0; i < recentPresets.size(); ++i)
        {
            Preset preset = recentPresets[i];
            if (preset.path == newPreset.path)
            {
                recentPresets.erase(recentPresets.begin() + i);
                break;
            }
        }
        recentPresets.push_back(newPreset);

        while (recentPresets.size() > MAX_RECENT_PRESETS)
            recentPresets.erase(recentPresets.begin());

        // the list is kept in the config, so presets saved or loaded before are still there when the mod starts again
        auto& savedPresets = PreferencesConfiguration::Get().recentVisualPresets;
        savedPresets.clear();
        for (const auto& preset : recentPresets)
            savedPresets.push_back(preset.path);
        Configuration::Get().Write(true);
    }

    void VisualsMenu::Release()
    {
    }
}  // namespace IWXMVM::UI
