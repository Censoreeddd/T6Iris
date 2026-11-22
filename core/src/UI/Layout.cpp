#include "StdInclude.hpp"
#include "Layout.hpp"

#include "Mod.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/CaptureManager.hpp"
#include "UI/UIManager.hpp"
#include "Renderer.hpp"
#include "UI/Components/TimelineGraph.hpp"

namespace IWXMVM::UI::Layout
{
    namespace
    {
        // the panel's area (right of the view), the card in it the tabs show on, and the icon rail at the window's
        // right edge
        ImRect view, panelArea, panel, rail, timeline;

        // a splitter being dragged: the size it had and where the mouse was when the drag started
        struct Drag
        {
            float size = 0.0f;
            float mouse = 0.0f;
        };
        std::optional<Drag> drag;

        std::vector<std::pair<std::string, ImRect>> splitters;

        // the layout's windows, behind every other one (the panel's tabs are windows of their own)
        constexpr std::array<std::string_view, 14> LAYOUT_WINDOWS = {
            "PrimaryRail",
            "##splitter.timeline",
            "##splitter.panel",
            "Timeline",
            "Demos",
            "Kills",
            "Camera",
            "Visuals",
            "Actors",
            "Players",
            "Capture",
            "PrimaryTabs",
            "GameView",
            "Background",
        };

        bool IsTimelineShown()
        {
            return Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        }

        void SetSizes(const LayoutMath::Sizes& sizes)
        {
            auto& preferences = PreferencesConfiguration::Get();
            preferences.layoutPanel = sizes.panel;
            preferences.layoutTimeline = sizes.timeline;
            preferences.layoutTimelineWithGraph = sizes.timelineWithGraph;
        }

        // A splitter on an edge between two of the layout's windows, as a window of its own in front of them (so it
        // gets the mouse rather than them): the drag sets size to what it was plus how far the mouse went along
        // (back, for a part on the far side of the edge), a double-click puts the default back. Returns whether it
        // was let go (or reset), when the sizes are saved.
        bool DrawSplitter(const char* name, ImRect rect, bool vertical, float size,
                          const std::function<void(float)>& setSize, const std::function<void()>& reset)
        {
            splitters.emplace_back(name + 11, rect);  // (without "##splitter.")
            ImGui::SetNextWindowPos(rect.Min);
            ImGui::SetNextWindowSize(rect.GetSize());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            constexpr ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
            bool released = false;
            if (ImGui::Begin(name, nullptr, flags))
            {
                ImGui::BeginDisabled(Components::CaptureManager::Get().IsCapturing());
                ImGui::InvisibleButton("##drag", rect.GetSize());
                const bool hovered = ImGui::IsItemHovered();
                const bool active = ImGui::IsItemActive();
                const auto mouse = vertical ? ImGui::GetMousePos().x : ImGui::GetMousePos().y;
                if (ImGui::IsItemActivated())
                    drag = Drag{size, mouse};
                if (active && drag)
                    setSize(drag->size - (mouse - drag->mouse));
                if (ImGui::IsItemDeactivated())
                {
                    drag.reset();
                    released = true;
                }
                // (the second click starts a drag too, which would put the size back while it is held)
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    drag.reset();
                    reset();
                    released = true;
                }
                if (hovered || active)
                {
                    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
                    const auto middle = vertical ? std::floor(rect.GetCenter().x) : std::floor(rect.GetCenter().y);
                    const auto from = vertical ? ImVec2(middle, rect.Min.y) : ImVec2(rect.Min.x, middle);
                    const auto to = vertical ? ImVec2(middle, rect.Max.y) : ImVec2(rect.Max.x, middle);
                    ImGui::GetWindowDrawList()->AddLine(
                        from, to, ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered),
                        2.0f);
                }
                if (!active && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
                    ImGui::SetTooltip("Drag to resize, double-click for the default size");
                ImGui::EndDisabled();
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            return released;
        }
    }  // namespace

    LayoutMath::Sizes GetSizes()
    {
        const auto& preferences = PreferencesConfiguration::Get();
        return {preferences.layoutPanel, preferences.layoutTimeline, preferences.layoutTimelineWithGraph};
    }

    void Update()
    {
        const auto display = ImGui::GetIO().DisplaySize;
        const auto top = UIManager::Get().GetUIComponent(Component::MenuBar)->GetSize().y;
        const auto fontSize = ImGui::GetFontSize();
        const auto railWidth = GetRailWidth();
        auto layout = LayoutMath::Fit(std::max(display.x - railWidth, 0.0f), std::max(display.y - top, 0.0f), fontSize,
                                      GetSizes(), IsTimelineShown(), TimelineGraph::IsOpen());
        // In a demo, the view as the game's frame is shaped (its back buffer, else the window), so the picture fills
        // it without bars: a view too tall gives what's over to the timeline under it, one too wide to the panel
        // beside it. (Under the view's bar, GameView::DrawTopBar, which is the combo's height and its margins.)
        if (IsTimelineShown())
        {
            auto aspect = display.y > 0 ? display.x / display.y : 0.0f;
            if (const auto backBuffer = Renderer::GetBackBufferSize(); backBuffer && backBuffer->x > 0 && backBuffer->y > 0)
                aspect = backBuffer->x / backBuffer->y;
            const auto bar = ImGui::GetFrameHeight() + 20.0f;
            const auto pictureHeight = layout.viewHeight - bar;
            if (aspect > 0 && layout.viewWidth > 0 && pictureHeight > 0)
            {
                if (layout.viewWidth / pictureHeight < aspect)
                {
                    const auto height = std::floor(layout.viewWidth / aspect + bar);
                    layout.timelineHeight += layout.viewHeight - height;
                    layout.viewHeight = height;
                }
                else
                {
                    const auto width = std::floor(pictureHeight * aspect);
                    layout.panelWidth += layout.viewWidth - width;
                    layout.viewWidth = width;
                }
            }
        }
        view = ImRect(0, top, layout.viewWidth, top + layout.viewHeight);
        panelArea = ImRect(layout.viewWidth, top, display.x - railWidth, top + layout.viewHeight);
        rail = ImRect(display.x - railWidth, top, display.x, top + layout.viewHeight);
        // the card: the panel's area with a margin all round (less on the rail's side, which has its own)
        const auto gap = std::floor(fontSize * 0.45f);
        panel = ImRect(panelArea.Min.x + gap, panelArea.Min.y + gap, panelArea.Max.x - std::floor(gap * 0.5f),
                       std::max(panelArea.Max.y - gap, panelArea.Min.y + gap + 1.0f));
        timeline = ImRect(0, view.Max.y, display.x, view.Max.y + layout.timelineHeight);
    }

    ImRect GetView()
    {
        return view;
    }

    ImRect GetPanel()
    {
        return panel;
    }

    ImRect GetPanelArea()
    {
        return panelArea;
    }

    ImRect GetRail()
    {
        return rail;
    }

    float GetRailWidth()
    {
        return std::floor(ImGui::GetFontSize() * 3.0f);
    }

    ImRect GetTimeline()
    {
        return timeline;
    }

    void DrawSplitters()
    {
        splitters.clear();
        const auto display = ImGui::GetIO().DisplaySize;
        const auto fontSize = ImGui::GetFontSize();
        const auto half = std::max(std::floor(fontSize * 0.2f), 3.0f);
        const auto save = [] { Configuration::Get().Write(true); };

        const auto panelWidth = panelArea.GetWidth();
        const auto railWidth = rail.GetWidth();
        if (DrawSplitter(
                "##splitter.panel", ImRect(view.Max.x - half, view.Min.y, view.Max.x + half, view.Max.y), true,
                panelWidth,
                [&](float pixels) {
                    SetSizes(LayoutMath::SetPanel(GetSizes(), pixels, std::max(display.x - railWidth, 0.0f), fontSize));
                },
                [] {
                    auto sizes = GetSizes();
                    sizes.panel = LayoutMath::DEFAULT_PANEL;
                    SetSizes(sizes);
                }))
            save();

        if (IsTimelineShown())
        {
            const bool graph = TimelineGraph::IsOpen();
            const auto height = display.y - view.Min.y;
            if (DrawSplitter(
                    "##splitter.timeline", ImRect(0, timeline.Min.y - half, display.x, timeline.Min.y + half), false,
                    timeline.GetHeight(),
                    [&](float pixels) {
                        SetSizes(LayoutMath::SetTimeline(GetSizes(), pixels, height, fontSize, graph));
                    },
                    [graph] {
                        auto sizes = GetSizes();
                        (graph ? sizes.timelineWithGraph : sizes.timeline) =
                            graph ? LayoutMath::DEFAULT_TIMELINE_WITH_GRAPH : LayoutMath::DEFAULT_TIMELINE;
                        SetSizes(sizes);
                    }))
                save();
        }

        // ImGui puts a window made with ImGuiWindowFlags_NoBringToFrontOnFocus behind all the others, so the
        // background, which covers the screen, would end up in front of those made before it, and the splitters
        // behind the windows they lie on: from the front, the splitters, the layout's windows, then the background
        for (const auto name : LAYOUT_WINDOWS)
        {
            if (const auto window = ImGui::FindWindowByName(name.data()))
                ImGui::BringWindowToDisplayBack(window);
        }
    }

    void Apply(Preset preset)
    {
        switch (preset)
        {
            case Preset::Default:
                SetSizes({});
                break;
            case Preset::Editing:
                SetSizes({32.0f, 22.0f, 30.0f});
                break;
            case Preset::Watching:
                SetSizes({LayoutMath::MIN_PANEL, LayoutMath::MIN_TIMELINE, LayoutMath::DEFAULT_TIMELINE_WITH_GRAPH});
                break;
        }
        Configuration::Get().Write(true);
    }

    void SetScale(float scale)
    {
        PreferencesConfiguration::Get().uiScale = LayoutMath::ValidScale(scale);
        Configuration::Get().Write(true);
    }

    std::optional<ImRect> GetSplitter(std::string_view name)
    {
        for (const auto& [splitter, rect] : splitters)
        {
            if (splitter == name)
                return rect;
        }
        return std::nullopt;
    }
}  // namespace IWXMVM::UI::Layout
