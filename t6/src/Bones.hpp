#pragma once
#include "StdInclude.hpp"
#include "Types/BoneData.hpp"
#include "Types/Entity.hpp"

// The players and their bones, for the bone camera. cg_entities ([0x1140878], 0x380 bytes each): the entity's number
// at 0x1DC, its type at 0x2B4 (a word, 1 a player), its place at 0x2C; the entity is its own pose. A bone is
// CG_DObjGetWorldTagMatrix of the entity's DObj (Com_GetClientDObj) and the bone's script string (SL_FindString), as
// AimTarget_GetTagPos (0x794DA0) does.
//
// The game's functions are called on the client's thread only (from the camera's hook, see Camera.cpp): a bone asked for
// on another thread (the UI's) is the one found there last, and is looked up there from then on.
namespace IWXMVM::T6::Bones
{
    constexpr std::int32_t MAX_ENTITIES = 1024;
    constexpr std::int32_t MAX_CLIENTS = 18;

    std::vector<Types::Entity> GetEntities();
    Types::BoneData GetBoneData(std::int32_t entityId, const std::string& name);
    // a player's name with their clan tag, as the snapshot has it (looked up on the client's thread, at most twice a
    // second); none for one not in it
    std::optional<std::string> GetPlayerName(std::int32_t clientNum);

    // On the client's thread, before the view is built: marks it as that thread and looks up the bones asked for
    void Update();
}  // namespace IWXMVM::T6::Bones
