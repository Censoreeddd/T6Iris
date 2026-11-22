#pragma once
#include "Components/CaptureManager.hpp"

namespace IWXMVM::Components::Rendering
{
    Types::RenderingFlags GetRenderingFlags();

    void SetVisibleElements(VisibleElements elements);
    // a pass's: what it draws, and what of the people stays over a Green screen's green
    void SetVisiblePass(const PassData& pass);
    enum GreenScreenKeeps : std::uint32_t
    {
        GreenScreen_Players = 1,
        GreenScreen_Actors = 2,
        GreenScreen_Props = 4,
        GreenScreen_Guns = 8,
        GreenScreen_World = 16,
    };
    std::uint32_t GetGreenScreenKeeps();
    // only what the game reads when it builds a frame (the first person gun), see CaptureManager::OnFrameHandedOver
    void SetVisibleElementsOfScene(VisibleElements elements);
    void ResetVisibleElements();

    void Initialize();
}  // namespace IWXMVM::Components::Rendering