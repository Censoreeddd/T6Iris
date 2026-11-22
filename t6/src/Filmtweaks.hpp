#pragma once
#include "StdInclude.hpp"
#include "Types/Filmtweaks.hpp"

// The Look tab's filmtweaks on the map's own colour grading. The frame's grade is set up by 0x72B7B0(frame, refdef,
// ...): with r_filmUseTweaks from the vc_ dvars, otherwise copied from the refdef's (refdef + 0x90: an enable, then
// 14 vec4s: vc_RS, vc_RE, the shadow, midtone and highlight colour matrices' rows, vc_FGM, vc_FSM, vc_FBM, then more).
// r_filmUseTweaks made the picture black (it leaves the rest of the grade unset), so instead the refdef's grade is
// changed just before it's copied (and put back after): each range's matrix is the editor's (desaturation, contrast, brightness, the range's
// tint, invert) applied after the map's. With the editor's at their defaults the picture is the map's.
namespace IWXMVM::T6::Filmtweaks
{
    void Install();
    void Set(const Types::Filmtweaks& filmtweaks);
}  // namespace IWXMVM::T6::Filmtweaks
