#pragma once
#include "StdInclude.hpp"

// The editor's cameras in T6, as t5/src/Hooks/Camera.cpp does it for T5: after CG_CalcViewValues has built the view
// (and after the theater's camera and its free camera, which write it again),
// the editor's camera replaces it when the editor flies it (free, orbit), and takes the game's view when the game has
// it (first person), so switching to a free camera starts where the view was.
//
// The dolly camera is the campath at the timeline's tick. While the editor flies the camera, the theater's camera is
// put in its free mode (and back afterwards), so the game draws the view from outside: no gun or first-person hud.
//
// Not yet: the bone camera, the third-person camera mode, and anything else the game writes into the refdef after
// CG_CalcViewValues (T5 hooks CG_ApplyScriptedCamera and CG_SetViewFromViewAngles for that).
namespace IWXMVM::T6::Camera
{
    void Install();
}  // namespace IWXMVM::T6::Camera
