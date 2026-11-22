#pragma once

#include "Components/Camera.hpp"
#include "Types/Keyframe.hpp"

namespace IWXMVM::MathUtils
{
    glm::vec3 ForwardVectorFromAngles(glm::vec3 eulerAngles);
    glm::vec3 AnglesFromForwardVector(glm::vec3 forward);

    glm::mat3x3 AnglesToAxis(glm::vec3 angles);
    glm::vec3 AxisToAngles(glm::mat3x3 axis);
    // The angles (pitch, yaw, roll, degrees) of a turn as glm::eulerAngleZYX(yaw, pitch, roll) makes it, of the two
    // ways of writing it (pitch within 90 degrees, or past it with yaw and roll half a turn round) and whole turns
    // added, the one nearest the angles it was turned from: a turn past straight up or down, or round past 180, goes
    // on smoothly, and a campath through it doesn't spin round between nodes
    glm::vec3 NearestAngles(const glm::mat3& turn, glm::vec3 previous);

    std::optional<ImVec2> WorldToScreenPoint(glm::vec3 point, Components::Camera& camera);
}  // namespace IWXMVM::MathUtils