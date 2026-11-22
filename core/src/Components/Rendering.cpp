#include "StdInclude.hpp"
#include "Rendering.hpp"

#include "Mod.hpp"
#include "D3D9.hpp"

namespace IWXMVM::Components::Rendering
{
    std::atomic<Types::RenderingFlags> renderingFlags;

    void SetGreenscreenColor(uint32_t color)
    {
        // (a game without them, T6 without r_clear, has no green screen: ending a capture threw there)
        auto r_clearcolor = Mod::GetGameInterface()->GetDvar("r_clearcolor");
        auto r_clear = Mod::GetGameInterface()->GetDvar("r_clear");
        if (!r_clearcolor || !r_clear)
            return;
        r_clearcolor->value->uint32 = color;
        r_clear->value->int32 = 3; // 3 = "steady" - clear every frame with the specified color
    }

    void SetDrawGun(bool drawGun)
    {
		if (auto cg_drawgun = Mod::GetGameInterface()->GetDvar("cg_drawgun"))
		    cg_drawgun->value->int32 = drawGun;
        else
        {
            static bool told = false;
            if (!std::exchange(told, true))
                LOG_WARN("No cg_drawgun dvar: the first person gun can't be shown or hidden for a pass");
        }
	}

    Types::RenderingFlags GetRenderingFlags()
    {
        return renderingFlags;
    }

    void SetRenderingFlags(Types::RenderingFlags flags)
    {
        renderingFlags = flags;
    }

    void SetVisibleElements(VisibleElements elements)
    {
        if (elements == VisibleElements::Everything)
        {
            ResetVisibleElements();
			return;
		}

        // the players are drawn on black and get their green from the depth after the game's post-processing (see
        // GraphicsManager::DrawShaderForPassIndex), which blends a green background into them and shades it unevenly
        SetGreenscreenColor(elements == VisibleElements::OnlyPlayers && D3D9::GetDepthTexture() ? 0xFF000000
                                                                                                 : 0xFF00FF00);

        if (elements == VisibleElements::WorldAndPlayers)
        {
			SetRenderingFlags(Types::RenderingFlags_DrawEverything);
            SetDrawGun(0);
		} 
        else if (elements == VisibleElements::OnlyGun)
        {
            SetRenderingFlags(Types::RenderingFlags_DrawEverything);
            SetDrawGun(1);

            // The visuals for this will be drawn via a shader, 
            // see Graphics/Graphics.cpp
        }
        else if (elements == VisibleElements::OnlyWorld)
        {
			SetRenderingFlags(Types::RenderingFlags_DrawWorld);
            SetDrawGun(0);
		}
        else if (elements == VisibleElements::OnlyPlayers)
        {
            SetRenderingFlags(Types::RenderingFlags_DrawPlayers);
            SetDrawGun(0);
        }
        else
        {
			LOG_WARN("Unknown pass elements: {0}", magic_enum::enum_name(elements));
		}
    }

    std::atomic<std::uint32_t> greenScreenKeeps = GreenScreen_Players | GreenScreen_Actors | GreenScreen_Guns;

    void SetVisiblePass(const PassData& pass)
    {
        static std::optional<VisibleElements> last;
        if (last != pass.elements)
        {
            last = pass.elements;
            LOG_INFO("A pass's elements: {} (keeps players {}, actors {}, props {})",
                     magic_enum::enum_name(pass.elements), pass.keepPlayers, pass.keepActors, pass.keepProps);
        }
        SetVisibleElements(pass.elements);
        greenScreenKeeps = (pass.keepPlayers ? GreenScreen_Players : 0u) | (pass.keepActors ? GreenScreen_Actors : 0u) |
                           (pass.keepProps ? GreenScreen_Props : 0u) | (pass.keepGuns ? GreenScreen_Guns : 0u) |
                           (pass.keepWorld ? GreenScreen_World : 0u);
        // (a Green screen's first person gun: there with the guns, gone without)
        if (pass.elements == VisibleElements::OnlyPlayers)
            SetDrawGun(pass.keepGuns);
    }

    std::uint32_t GetGreenScreenKeeps()
    {
        return greenScreenKeeps;
    }

    void SetVisibleElementsOfScene(VisibleElements elements)
    {
        SetDrawGun(elements == VisibleElements::Everything || elements == VisibleElements::OnlyGun);
    }

    void ResetVisibleElements()
    {
        SetRenderingFlags(Types::RenderingFlags_DrawEverything);
        SetDrawGun(1);
        SetGreenscreenColor(0xFF000000);
    }

    void Initialize()
    {
        renderingFlags = Types::RenderingFlags_DrawEverything;
    }
}  // namespace IWXMVM::Components::Rendering