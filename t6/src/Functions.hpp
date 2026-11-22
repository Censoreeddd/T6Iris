#pragma once
#include "StdInclude.hpp"
#include "Structures.hpp"

namespace IWXMVM::T6::Functions
{
    // the game's dvar of that name (case doesn't matter), null when there's none. Any thread.
    Structures::dvar_t* FindDvar(std::string_view name);

    // runs a console command on the next frame (a newline is added)
    void Cbuf_AddText(std::string command);

    std::int32_t GetKeyCatchers();

    // A bool, int or float dvar's value as a number (true 1), whichever of them it is; none for another type or no
    // dvar of that name. A string dvar ("0", "1") is read and set as its number too, set only to what it already
    // holds (it's left as it is otherwise, its string being the game's).
    std::optional<float> GetDvarNumber(std::string_view name);
    // its reset (default) value, the same way
    std::optional<float> GetDvarDefault(std::string_view name);
    bool SetDvarNumber(std::string_view name, float value);
    // a vec3, vec4 or color dvar's first three values (a color's 0 to 255 as 0 to 1), current or reset; and set
    std::optional<glm::vec3> GetDvarVec3(std::string_view name, bool reset = false);
    bool SetDvarVec3(std::string_view name, glm::vec3 value);
    // a vec4 dvar's four values (the vision colour's vc_ dvars)
    bool SetDvarVec4(std::string_view name, glm::vec4 value);

    // Where a line from start to end first hits the map (what a shot hits, not the players: MASK_SHOT_NO_CHARACTERS);
    // none when it hits nothing. On the game's thread.
    std::optional<glm::vec3> TraceMap(glm::vec3 start, glm::vec3 end);
    // and the surface it hits there (the game's surface type, bits 20-24 of the trace's surface flags)
    struct MapHit
    {
        glm::vec3 point;
        std::uint32_t surface = 0;
    };
    std::optional<MapHit> TraceMapSurface(glm::vec3 start, glm::vec3 end);
    // where something put in the scene goes: on what's along the direction from the origin (as far as reach), else
    // that far ahead
    glm::vec3 FindPlace(glm::vec3 origin, glm::vec3 direction, float ahead, float reach = 8192.0f);
}  // namespace IWXMVM::T6::Functions
