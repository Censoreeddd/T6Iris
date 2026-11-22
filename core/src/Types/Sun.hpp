#pragma once
#include "glm/vec3.hpp"

namespace IWXMVM::Types
{
    struct Sun
    {
        glm::vec3 color;
        glm::vec3 direction;
        float brightness;
        // the sky's turn about the up axis, degrees (Features_SkyRotation; T6's r_skyRotation)
        float skyRotation = 0.0f;
    };
}  // namespace IWXMVM::Types