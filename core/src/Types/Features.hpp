#pragma once

namespace IWXMVM::Types
{
    enum Features : uint32_t
    {
        Features_None = 0,

        Features_ChangeAnimations = 1 << 0,

        // The filmtweaks panel shows the per-range controls of Black Ops' film system
        Features_FilmtweakRanges = 1 << 1,

        // GetDemoEvents finds the kills of a demo for the timeline
        Features_DemoEvents = 1 << 2,

        // SetRenderSize renders above the window's resolution, for captures
        Features_RenderSize = 1 << 3,

        // The game reports what players can look like (Components::Loadouts::SetCatalog) and applies the choices
        Features_ChangeLoadouts = 1 << 4,

        // The game draws actors (Components::Actors), reports what they can be made of (SetCatalog) and loads content
        // packs (LoadContentPack)
        Features_Actors = 1 << 5,

        // GetSunShadowMapSize and SetSunShadowMapSize change the sun's shadow map (the Look tab's Shadow quality), and
        // sm_sunSampleSizeNear sets how far its shadows reach (Shadow distance)
        Features_ShadowQuality = 1 << 6,

        // GetMapNormalMaps gives the normal maps of the map's surfaces and props, for Surface depth and Bump strength
        // (Components::SurfaceDepth)
        Features_SurfaceDepth = 1 << 7,

        // GetPackImages gives the images the game has registered with the groups of what uses them, for texture packs
        // (Components::TexturePack and the Look tab's Textures)
        Features_TexturePack = 1 << 8,

        // the sun's settings take the sky's rotation too (Types::Sun::skyRotation), shown on the Look tab's Sun
        Features_SkyRotation = 1 << 9,

        // With Features_FilmtweakRanges: where the ranges meet can be moved (Mid Start, Mid End and the feathers)
        Features_FilmtweakRangeEdges = 1 << 10,

        // With Features_FilmtweakRanges: Brightness and Invert are offered too, over all ranges
        Features_FilmtweakBrightness = 1 << 11,

        // The Look tab's Blur section: the mod's directional and radial blur (Types::DoF::screenBlur), drawn
        // over the game's frame before it's captured (D3D11::SetScreenBlur)
        Features_ScreenBlur = 1 << 12,

        // With Features_ScreenBlur: the Lens blur too, which needs the game's depth (focus distance, focal length,
        // aperture and bokeh)
        Features_LensBlur = 1 << 13,

        // The Record tab's Motion Blur: a capture renders several frames across the shutter's time for each frame it
        // records and blends them (CaptureManager::ShutterSettings), on a D3D11 game with a render thread
        Features_Shutter = 1 << 14,

        // The Camera tab's Camera Shake: the game adds Components::CameraShake's offset to the view
        Features_CameraShake = 1 << 15,

        // The Camera tab's Lens Flare: the game says where the sun is on the picture and gives its depth buffer
        // (Components::LensFlare), the mod draws the flare over the frame (D3D11)
        Features_LensFlare = 1 << 16,

        // The Look tab's Sky: the mod's sky in place of the map's where the depth buffer has nothing, turned with the
        // view the game reports (Components::SkyReplace, Components::Tracking)
        Features_SkyReplace = 1 << 17,

        // The Actors tab's lights can cast shadows (one spot at a time) and have their highlights (specular) set: the
        // game's renderer is told both (T5's Lights.cpp)
        Features_LightShadows = 1 << 18,
        // An actor's Style (whose walks and turns it goes with), Stance and Face paint: the AI's movement sets and
        // the players' face paints BO1 has
        Features_ActorStyles = 1 << 19,
    };
}  // namespace IWXMVM::Types