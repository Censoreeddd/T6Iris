#pragma once
#include "StdInclude.hpp"
#include "Addresses.hpp"
#include "Utilities/Patches.hpp"

namespace IWXMVM::T6::Patches
{
    using namespace IWXMVM::Patches;

    struct T6Patches
    {
        // Returns at once while the editor has the mouse, so the game neither turns the view nor puts the cursor
        // back in the middle of the window (SetMouseMode)
        ReturnPatch IN_Frame{GetGameAddresses().IN_Frame(), PatchApplySetting::Deferred};
    };

    inline T6Patches& GetGamePatches()
    {
        static T6Patches patches;
        return patches;
    }
}  // namespace IWXMVM::T6::Patches
