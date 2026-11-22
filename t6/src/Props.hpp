#pragma once
#include "StdInclude.hpp"
#include "Types/AnimationPose.hpp"

// The Actors tab's props (models of the map on their own, Types::ActorPlacedProp) and its catalog of the models loaded.
//
// A prop is drawn as the client draws its own models: a DObj of the model (Com_ClientDObjCreate, 0x565FF0) on a handle
// of the client's entities from the top of cg_entities (1,792 of 0x380 bytes, the pose at the start: its origin at
// +0x2C and angles at +0x38), handed to the renderer each frame with R_AddDObjToScene (0x7269C0) as the client's
// general entities are (0x7A1DD0). The models are found in the assets' hash table (0x131D740, 0x9600 chains of
// entries of 16 bytes from 0x14342E0, as T5's).
namespace IWXMVM::T6::Props
{
    // As a film starts: the catalog of the models loaded (the map's too, by its clip map's zone), and the props drawn
    // before forgotten (the game freed their DObjs with the last map). On the game's thread.
    void OnFilmStarted();
    // Once a frame of a film, as the lights are added: the props in their bars, and the prop browser's preview
    void AddToScene();
    // the props' DObjs freed, as the mod unloads (main thread)
    void ReleaseAll();
    // DB_LoadXAssets hooked, for the packs to load with the films' maps
    void Install();
    // A pack of more characters and animations (a zone of the catalog's packs, empty for none) to load with the films
    // from now on, with their maps (any thread); Update takes it, on the game's thread each frame of a film, and the
    // catalog says whether this film has it
    void LoadPack(const std::string& zone);
    void Update();
    // A loaded model in a loaded animation at a time from 0 to 1, decoded from the animation's data, for the animation
    // browser's preview (GameInterface::GetAnimationPoseByName); none while a pack loads. Any thread.
    std::optional<Types::AnimationPose> GetAnimationPose(std::string_view animation, std::string_view model, float time);
}  // namespace IWXMVM::T6::Props
