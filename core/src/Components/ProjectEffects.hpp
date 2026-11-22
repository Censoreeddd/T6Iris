#pragma once
#include "StdInclude.hpp"
#include "nlohmann/json.hpp"

// The effects' settings as a project (the keyframe file) keeps them: the Lens, Camera Shake, Lens Flare (but what it
// follows, kept with the tracking), Lens Dirt, Sky and the Look tab's Depth of Field. Older files without them leave
// the settings as they are; settings a later build adds are left as they are when a file doesn't have them.
namespace IWXMVM::Components::ProjectEffects
{
    nlohmann::json Write();
    void Read(const nlohmann::json& effects);
}  // namespace IWXMVM::Components::ProjectEffects
