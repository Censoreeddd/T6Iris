#include "StdInclude.hpp"
#include "Renderer.hpp"
#include "CaptureMenu.hpp"

#include "Resources.hpp"
#include "Graphics/Graphics.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "Events.hpp"
#include "Mod.hpp"
#include "UI/UIManager.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/CameraManager.hpp"
#include "Utilities/PathUtils.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/TaskbarProgress.hpp"

namespace IWXMVM::UI
{
    enum class MenuMode
    {
        SinglePass,
        MultiPass
    };

    constexpr auto fieldLayoutPercentage = 0.4f;
    std::optional<int32_t> displayPassIndex = std::nullopt;

    std::optional<int32_t> CaptureMenu::GetDisplayPassIndex() const
    {
        return displayPassIndex;
    }

    void CaptureMenu::ResetPassPreview()
    {
        displayPassIndex = std::nullopt;
        Components::Rendering::ResetVisibleElements();
    }

    void CaptureMenu::Initialize()
    {
    }

    void CaptureMenu::DrawStreamsSection(Components::CaptureSettings& captureSettings)
    {
        using namespace Components;

        // the streams this game can draw (a D3D11 game's gun alone needs the D3D9 games' shader)
        const bool d3d11 = Renderer::GetApi() == Types::GraphicsApi::D3D11;
        const auto offered = [&](const PassPreset& preset) {
            return !(d3d11 && preset.elements == VisibleElements::OnlyGun);
        };

        // (a game that tells its people apart: BO2's, see its Props)
        const bool greenKeeps = d3d11;

        ImGui::Dummy(ImVec2(0, 5));
        ImGui::Indent();

        if (captureSettings.passes.empty())
        {
            ImGui::TextWrapped("Each stream is recorded as its own video of the same shot. Add the ones you want to "
                               "combine in your editor.");
            ImGui::Dummy(ImVec2(0, 3));
        }

        const float buttons = ImGui::GetStyle().ItemSpacing.x * 2 + ImGui::GetFrameHeight() * 2;
        const float fieldWidth = ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                 ImGui::GetStyle().WindowPadding.x;

        for (auto it = captureSettings.passes.begin(); it != captureSettings.passes.end();)
        {
            auto i = std::distance(captureSettings.passes.begin(), it);
            ImGui::PushID(std::format("##pass{}", i).c_str());

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 0.1f);
            ImGui::AlignTextToFramePadding();
            ImGui::Text(ICON_FA_LAYER_GROUP "  Stream %d", static_cast<int>(i + 1));

            // its stream, by name
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
            ImGui::SetNextItemWidth(fieldWidth - buttons);
            const auto name = GetPassName(*it);
            if (ImGui::BeginCombo("##passStreamCombo", name.c_str()))
            {
                for (const auto& preset : PASS_PRESETS)
                {
                    if (!offered(preset))
                        continue;
                    const bool isSelected = it->type == preset.type && it->elements == preset.elements;
                    if (ImGui::Selectable(preset.name, isSelected))
                    {
                        it->type = preset.type;
                        it->elements = preset.elements;
                    }
                    ImGui::SetItemTooltip("%s", preset.description);
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // its eye: shows it in the game view (another's then off), again to stop; its bin deletes it, whether one
            // is looked at or not
            const bool shown = displayPassIndex.has_value() && displayPassIndex.value() == i;
            ImGui::SameLine();
            if (ImGui::Button(shown ? ICON_FA_EYE_SLASH : ICON_FA_EYE, ImVec2(ImGui::GetFrameHeight(), 0)))
            {
                if (shown)
                    ResetPassPreview();
                else
                {
                    displayPassIndex = static_cast<std::int32_t>(i);
                    Components::Rendering::SetVisiblePass(*it);
                }
            }
            else if (shown)
            {
                Components::Rendering::SetVisiblePass(*it);
            }
            ImGui::SetItemTooltip(shown ? "Stop previewing this stream" : "Preview this stream in the game view");

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(ImGui::GetFrameHeight(), 0)))
            {
                // (the one looked at goes off with it; one after it is looked at where it moved to)
                if (shown)
                    ResetPassPreview();
                else if (displayPassIndex.has_value() && displayPassIndex.value() > i)
                    displayPassIndex = displayPassIndex.value() - 1;
                captureSettings.passes.erase(it);
                ImGui::PopID();
                break;
            }
            ImGui::SetItemTooltip("Remove this stream");

            // (a Green screen's: what stays over the green, each on or off)
            if (it->type == PassType::Default && it->elements == VisibleElements::OnlyPlayers && greenKeeps)
            {
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
                ImGui::Checkbox("Players##keepPlayers", &it->keepPlayers);
                ImGui::SetItemTooltip("The demo's players stay over the green");
                ImGui::SameLine();
                ImGui::Checkbox("Actors##keepActors", &it->keepActors);
                ImGui::SetItemTooltip("The actors you added stay over the green");
                ImGui::SameLine();
                ImGui::Checkbox("Props##keepProps", &it->keepProps);
                ImGui::SetItemTooltip("The props you added stay over the green");
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
                ImGui::Checkbox("Guns##keepGuns", &it->keepGuns);
                ImGui::SetItemTooltip("The guns the players and actors hold, and the first person gun, stay over the green");
                ImGui::SameLine();
                ImGui::Checkbox("World##keepWorld", &it->keepWorld);
                ImGui::SetItemTooltip("The map stays too (the green only where the sky is)");
            }

            // (a D3D11 game's Depth: how far is black, under the stream itself)
            if (d3d11 && it->type == PassType::Depth)
            {
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
                ImGui::SetNextItemWidth(fieldWidth - buttons);
                ImGui::DragFloat("##captureDepthDistance", &captureSettings.depthDistance, 10.0f, 50.0f, 100000.0f,
                                 "Fades out at %.0f units");
                ImGui::SetItemTooltip("How far away the depth reaches black: drag, or double-click to type");
            }

            if (D3D9::IsReshadePresent())
            {
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
                if (it->type == PassType::Default)
                {
                    ImGui::Checkbox("ReShade", &it->useReshade);
                    ImGui::SetItemTooltip("ReShade must be on");
                }
                else
                {
                    it->useReshade = false;
                }
            }

            ImGui::Dummy(ImVec2(0, 3));

            ImGui::PopID();
            it++;
        }

        // a stream to add, picked from the list (one already there isn't offered twice)
        ImGui::Dummy(ImVec2(0, 3));
        if (ImGui::Button(ICON_FA_PLUS " Add stream"))
            ImGui::OpenPopup("##addStream");
        if (ImGui::BeginPopup("##addStream"))
        {
            for (const auto& preset : PASS_PRESETS)
            {
                if (!offered(preset))
                    continue;
                const bool there = std::ranges::any_of(captureSettings.passes, [&](const PassData& pass) {
                    return pass.type == preset.type && pass.elements == preset.elements;
                });
                ImGui::BeginDisabled(there);
                if (ImGui::Selectable(preset.name))
                    captureSettings.passes.push_back({preset.type, preset.elements});
                ImGui::EndDisabled();
                ImGui::SameLine(ImGui::GetFontSize() * 7.5f);
                ImGui::TextDisabled("%s", there ? "already added" : preset.description);
            }
            ImGui::EndPopup();
        }

        ImGui::Unindent();
    }

    // Motion blur (Features_Shutter): a camera's shutter speed, the frames rendered for each one recorded, and the
    // estimate in the editor while the film plays
    void CaptureMenu::DrawShutter(Components::CaptureSettings& captureSettings)
    {
        using namespace Components;
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_Shutter))
            return;
        auto& shutter = captureSettings.shutter;
        auto& captureManager = CaptureManager::Get();
        const auto fieldWidth =
            ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) - ImGui::GetStyle().WindowPadding.x;
        const auto row = [&](const char* label) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", label);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
            ImGui::SetNextItemWidth(fieldWidth);
        };

        ImGui::Dummy(ImVec2(0, 10));
        row("Motion Blur");
        ImGui::Checkbox("##shutterEnabled", &shutter.enabled);
        ImGui::SetItemTooltip("Blurs what moves as a camera's shutter would: each recorded frame blends several the "
                              "game renders while the shutter is open");
        if (!shutter.enabled)
            return;

        // the shutter speeds, as a camera shows them, with the 180 degree rule's (half a frame) named
        constexpr std::array SPEEDS = {1000, 500, 250, 125, 100, 60, 50, 30, 25, 15, 8, 4, 2};
        const auto name = [&](float seconds) {
            const auto denominator = static_cast<int>(std::lround(1.0f / seconds));
            const bool halfFrame = captureSettings.framerate > 0 && denominator == captureSettings.framerate * 2;
            return std::format("1/{} s{}", denominator, halfFrame ? "  (180\xC2\xB0)" : "");
        };
        row("Shutter Speed");
        if (ImGui::BeginCombo("##shutterSpeed", name(shutter.seconds).c_str()))
        {
            for (const auto speed : SPEEDS)
            {
                const float seconds = 1.0f / static_cast<float>(speed);
                if (ImGui::Selectable(name(seconds).c_str(), std::abs(shutter.seconds - seconds) < 0.00001f))
                    shutter.seconds = seconds;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Slower (1/30, 1/8...) smears more. Longer than a frame, the frames before it blend in "
                              "too, up to %d frames' time.",
                              MAX_SHUTTER_FRAMES);

        row("Samples");
        ImGui::SliderInt("##shutterSamples", &shutter.samples, 2, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Frames rendered for each one recorded: more is smoother and records slower");

        row("Preview");
        ImGui::Checkbox("##shutterPreview", &shutter.preview);
        ImGui::SetItemTooltip("An estimate in the editor while the film plays (the recording renders it properly)");

        const auto plan = captureManager.GetShutterPlan();
        const auto frameTime = 1000 / std::max(captureSettings.framerate, 1);
        ImGui::TextDisabled("Renders %d frames for each one recorded%s", plan.count,
                            plan.count < shutter.samples ? " (the shutter's time in milliseconds allows no more)" : "");
        if (shutter.seconds * 1000.0f > static_cast<float>(frameTime * MAX_SHUTTER_FRAMES))
            ImGui::TextDisabled("(at most %d frames' time at this framerate)", MAX_SHUTTER_FRAMES);
        if (captureManager.MultiPassEnabled())
            ImGui::TextDisabled("(not with multi-pass streams)");
    }

    void CaptureMenu::DrawSettings(Components::CaptureSettings& captureSettings)
    {
        using namespace Components;

        auto& captureManager = CaptureManager::Get();

        auto halfWidth = ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) - ImGui::GetStyle().WindowPadding.x;
        halfWidth /= 2;
        halfWidth -= ImGui::GetStyle().ItemSpacing.x / 2;
        auto endTick = Mod::GetGameInterface()->GetDemoInfo().endTick;

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Timeframe");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
        ImGui::SetNextItemWidth(halfWidth);
        ImGui::DragInt("##startTickInput", (int32_t*)&captureSettings.startTick, 10, 0, captureSettings.endTick);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(halfWidth);
        ImGui::DragInt("##endTickInput", (int32_t*)&captureSettings.endTick, 10, captureSettings.startTick, endTick);

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Output Format");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                ImGui::GetStyle().WindowPadding.x);
        if (ImGui::BeginCombo("##captureMenuOutputFormatCombo",
                              captureManager.GetOutputFormatLabel(captureSettings.outputFormat).data()))
        {
            for (auto outputFormat = 0; outputFormat < (int)OutputFormat::Count; outputFormat++)
            {
                bool isSelected = captureSettings.outputFormat == (OutputFormat)outputFormat;
                if (ImGui::Selectable(captureManager.GetOutputFormatLabel((OutputFormat)outputFormat).data(),
                        captureSettings.outputFormat == (OutputFormat)outputFormat))
                {
                    captureSettings.outputFormat = (OutputFormat)outputFormat;
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (captureSettings.outputFormat == OutputFormat::Video)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Video Codec");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                                    ImGui::GetStyle().WindowPadding.x);
            if (ImGui::BeginCombo("##captureMenuVideoCodecCombo",
                                    captureManager.GetVideoCodecLabel(captureSettings.videoCodec.value())
                                        .data()))
            {
                for (auto videoCodec = 0; videoCodec < (int)VideoCodec::Count; videoCodec++)
                {
                    bool isSelected = captureSettings.videoCodec == (VideoCodec)videoCodec;
                    if (ImGui::Selectable(captureManager.GetVideoCodecLabel((VideoCodec)videoCodec).data(),
                            captureSettings.videoCodec == (VideoCodec)videoCodec))
                    {
                        captureSettings.videoCodec = (VideoCodec)videoCodec;
                    }

                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
        
        // rendering larger than the window gives a sharper capture, which an output below it supersamples
        if (const auto renderResolutions = captureManager.GetRenderResolutions(); !renderResolutions.empty())
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Render at");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                    ImGui::GetStyle().WindowPadding.x);
            const auto windowLabel =
                std::format("Window ({})", captureManager.GetGameResolution().ToString());
            const auto currentLabel = captureSettings.renderResolution
                                          ? captureSettings.renderResolution->ToString()
                                          : windowLabel;
            if (ImGui::BeginCombo("##captureMenuRenderResolutionCombo", currentLabel.c_str()))
            {
                if (ImGui::Selectable(windowLabel.c_str(), !captureSettings.renderResolution.has_value()))
                {
                    captureSettings.renderResolution.reset();
                    captureSettings.resolution = captureManager.GetGameResolution();
                }
                for (const auto& resolution : renderResolutions)
                {
                    if (ImGui::Selectable(resolution.ToString().c_str(),
                                          captureSettings.renderResolution == resolution))
                    {
                        captureSettings.renderResolution = resolution;
                        captureSettings.resolution = resolution;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip(
                "The game switches to this size while it captures, also above your screen's resolution.\n"
                "Pick a smaller output resolution below to supersample it.");
        }

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Resolution");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                ImGui::GetStyle().WindowPadding.x);
        if (ImGui::BeginCombo("##captureMenuResolutionCombo", captureSettings.resolution.ToString().c_str()))
        {
            for (auto resolution : captureManager.GetSupportedResolutions())
            {
                bool isSelected = captureSettings.resolution == resolution;
                if (ImGui::Selectable(resolution.ToString().c_str(),
                                      captureSettings.resolution == resolution))
                {
                    captureSettings.resolution = resolution;
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Framerate");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                ImGui::GetStyle().WindowPadding.x);
        if (ImGui::BeginCombo("##captureMenuFramerateCombo", std::to_string(captureSettings.framerate).c_str()))
        {
            for (auto framerate : captureManager.GetSupportedFramerates())
            {
                bool isSelected = captureSettings.framerate == framerate;
                if (ImGui::Selectable(std::to_string(framerate).c_str(),
                                      captureSettings.framerate == framerate))
                {
                    captureSettings.framerate = framerate;
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        DrawShutter(captureSettings);

        ImGui::Dummy(ImVec2(0, 10));

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Record");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * fieldLayoutPercentage);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1 - fieldLayoutPercentage) -
                                ImGui::GetStyle().WindowPadding.x);

        ImGui::BeginDisabled(displayPassIndex.has_value());
        static MenuMode menuMode;
        const auto modeName = [](MenuMode mode) { return mode == MenuMode::SinglePass ? "One video" : "Multi-pass streams"; };
        if (ImGui::BeginCombo("##captureMenuModeCombo", modeName(menuMode)))
        {
            for (const auto mode : {MenuMode::SinglePass, MenuMode::MultiPass})
            {
                // (streams start with the full frame and the green screen, the usual pair)
                if (ImGui::Selectable(modeName(mode), menuMode == mode) && std::exchange(menuMode, mode) != mode &&
                    mode == MenuMode::MultiPass && captureSettings.passes.empty())
                {
                    captureSettings.passes.push_back({PassType::Default, VisibleElements::Everything});
                    captureSettings.passes.push_back({PassType::Default, VisibleElements::OnlyPlayers});
                }
            }

            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        if (menuMode == MenuMode::MultiPass)
        {
            DrawStreamsSection(captureSettings);
        }
        else
        {
            captureSettings.passes.clear();
        }
    }

    void CaptureMenu::Render()
    {
        using namespace Components;

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("Capture", NULL, flags))
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                UI::DrawInaccessibleTabWarning();
                ImGui::End();
                return;
            }

            auto& captureManager = CaptureManager::Get();
            auto& captureSettings = captureManager.GetCaptureSettings();

            ImGui::BeginDisabled(captureManager.IsCapturing());

            if (ImGuiEx::Keyframeable::PanelSection(ICON_FA_VIDEO "  Capture", "capture"))
                DrawSettings(captureSettings);

            ImGui::EndDisabled();

            if (ImGuiEx::Keyframeable::PanelSection(ICON_FA_FOLDER_OPEN "  Output", "output"))
            {
                // the folder, then Browse at the rows' right edge
                const auto browseLabel = "Browse";
                const auto browseWidth =
                    ImGui::CalcTextSize(browseLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                const auto right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                const auto& outputDirectory = PreferencesConfiguration::Get().captureOutputDirectory;
                ImGui::AlignTextToFramePadding();
                ImGui::PushTextWrapPos(right - browseWidth - ImGui::GetStyle().ItemSpacing.x);
                ImGui::TextWrapped("%s", outputDirectory.string().c_str());
                ImGui::PopTextWrapPos();
                ImGui::SameLine(right - browseWidth);
                ImGui::BeginDisabled(captureManager.IsCapturing());
                if (ImGui::Button(browseLabel, ImVec2(browseWidth, 0)))
                {
                    auto folder = PathUtils::OpenFolderBrowseDialog();
                    if (folder.has_value())
                    {
                        LOG_INFO("Set recording output directory to: {}", folder.value().string());
                        PreferencesConfiguration::Get().captureOutputDirectory = folder.value();
                        Configuration::Get().Write(true);
                    }
                }
                ImGui::EndDisabled();
            }

            ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
            auto label = captureManager.IsRestarting() ? ICON_FA_HOURGLASS " Switching"
                         : captureManager.IsCapturing() ? ICON_FA_STOP " Stop"
                                                        : ICON_FA_CIRCLE " Capture";
            ImGui::BeginDisabled(captureManager.IsRestarting());
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const bool capturePressed =
                ImGui::Button(label, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 2));
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (capturePressed)
            {
                if (captureManager.IsCapturing())
                    captureManager.StopCapture();
                else
                    captureManager.StartCapture();
            }

            if (!captureManager.IsFFmpegPresent())
            {
                ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 4));
                ImGui::PushStyleColor(ImGuiCol_Text, {249.0f / 255.0f, 22.0f / 255.0f, 22.0f / 255.0f, 1.0f});
                ImGui::PushFont(UIManager::Get().GetBoldFont());
                ImGui::TextWrapped("FFmpeg not found. Start from Iris Studio.");
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }

            if (captureManager.IsCapturing())
            {
                ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 4));
                ImGui::PushFont(UIManager::Get().GetBoldFont());
                ImGui::Text("Progress");
                ImGui::PopFont();
                ImGui::Text("Captured %d frames", captureManager.GetCapturedFrameCount());

                auto totalFrames = (captureSettings.endTick - captureSettings.startTick) * (captureSettings.framerate / 1000.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::GetColorU32(ImGuiCol_Button));

                if (CaptureManager::Get().MultiPassEnabled())
                {
                    ImGui::ProgressBar((float)(captureManager.GetCapturedFrameCount() / captureSettings.passes.size()) / totalFrames, ImVec2(-1, 0), "");
                }
                else
                {
                    ImGui::ProgressBar((float)captureManager.GetCapturedFrameCount() / totalFrames, ImVec2(-1, 0), "");
                }
                ImGui::PopStyleColor();

                // (the frames of all passes, as the bar above)
                const auto passes = CaptureManager::Get().MultiPassEnabled() ? captureSettings.passes.size() : 1;
                TaskbarProgress::SetProgressValue(static_cast<int>(captureManager.GetCapturedFrameCount() / passes),
                                                  static_cast<unsigned long long>(totalFrames));
                TaskbarProgress::SetProgressState(TBPF_NORMAL);
            }
            else
            {
                TaskbarProgress::SetProgressState(TBPF_NOPROGRESS);
            }
        }
        ImGui::End();
    }

    void CaptureMenu::Release()
    {
    }
}  // namespace IWXMVM::UI