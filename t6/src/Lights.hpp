#pragma once
#include "StdInclude.hpp"

// The scene's lights (the Actors tab's lights, Components::LightLogic), added to the game's scene each frame as its
// effects add theirs: R_AddOmniLightToScene (0x726F00; origin, radius, r, g, b in sRGB 0 to 1, intensity), which takes
// up to 32 a frame into the scene's list (count 0x3553BFC, 0x160 bytes each from 0x3553C00). The renderer draws as many
// of them as r_dlightLimit (at most 4) per surface.
//
// Spots through R_AddSpotLightToScene (0x727110), their cone from Cone and their soft edge from Softness. Without a
// trace of the map, a new light goes 120 units in front of the camera instead of on the surface it looks at.
namespace IWXMVM::T6::Lights
{
    // As a film starts: the Actors tab gets its (empty) catalog, so it can add lights
    void OnFilmStarted();
    // On the client's thread, once a frame of a film, after the view is built
    void AddToScene();
}  // namespace IWXMVM::T6::Lights
