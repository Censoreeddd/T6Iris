#pragma once
#include "StdInclude.hpp"
#include "Utilities/Signatures.hpp"

// Patterns were taken from t6mpv43.exe, the executable the Redacted LAN client runs (PE timestamp 0x536E8FB4, SHA-256
// 770318175f0161aa7a1ff0f9a5530336836a99e72900d7608a63973e56004adf), and checked to match once in its image.
// Function names follow T5's and Quake 3's for the same code.
namespace IWXMVM::T6::Signatures
{
    struct T6Addresses
    {
#define Sig IWXMVM::Signatures::Signature < IWXMVM::Signatures::SignatureImpl
#define Lambda IWXMVM::Signatures::Lambdas

        using GAType = IWXMVM::Signatures::GameAddressType;

        // IN_Frame (0x433CB0): activates the mouse while the window is in front, then IN_MouseMove reads the cursor,
        // hands the movement to the client and puts the cursor back in the middle. The same shape as T5's IN_Frame:
        // a dvar's Dvar_GetBool, then PostMessage(hWnd, WM_LBUTTONDOWN, 1, 0) when it's set.
        Sig("?? ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 16 8B 0D ?? ?? ?? ?? 6A 00 6A 01 68 01 02 00 00 51 FF 15",
            GAType::Code, 0) > IN_Frame;

        // Cbuf_AddText(localClientNum, text) (0x5C6F10): T5's signature matches it byte for byte
        Sig("?? ?? ?? ?? ?? ?? ?? 8B 4C 24 0C 83 C4 04 8B D1 85 C9 74 1F 8A 01 3C 70", GAType::Code, 0) > Cbuf_AddText;
        // Dvar_FindVar (0x8C8950), name in eax: a djb2 hash of the lowercased name (5381, then hash * 33 + c), then
        // Dvar_FindMalleableVar (0x8C88E0) with the hash in esi, as T5 calls it
        Sig("?? ?? ?? ?? ?? FF 75 0A 33 F6 E8 ?? ?? ?? ?? 5F 5E C3 8A 07 BE 05 15 00 00 84 C0", GAType::Code, 0)
            > Dvar_FindVar;
        // the client's keyCatchers (0x11C7844), from Key_AddCatcher (0x4C5D00): keyCatchers |= catcher
        Sig("83 7C 24 04 FF 74 0A 8B 44 24 08 09 05 ?? ?? ?? ?? C3", GAType::Data, 13, Lambda::DereferenceAddress)
            > keyCatchers;

        // theater (see RE-NOTES.md)
        // the demo state (0x30D3404, 2 while a film plays), as demo_forward and demo_back check it
        Sig("83 3D ?? ?? ?? ?? 02 75 ?? 83 3D ?? ?? ?? ?? 04 7C", GAType::Data, 2, Lambda::DereferenceAddress) > demoState;
        // the pointer to the theater's playback structure (0x3183A00), from Demo_Forward (0x675D10)
        Sig("80 3D ?? ?? ?? ?? 00 74 17 80 3D ?? ?? ?? ?? 00 75 0E A1 ?? ?? ?? ?? 80 B8", GAType::Data, 19,
            Lambda::DereferenceAddress) > theater;
        // the pointer to cg (0x11C7D74), whose time (+0x58) is the film's
        Sig("23 05 ?? ?? ?? ?? F3 0F 11 80 F4 2B 00 00 C3", GAType::Data, 2, Lambda::DereferenceAddress) > cgPointer;
        // CG_CalcViewValues(localClientNum) (0x7D7120): builds the view in cg's refdef (see Structures.hpp)
        Sig("?? ?? ?? ?? ?? 08 53 8B 5C 24 10 56 8B 35 ?? ?? ?? ?? 8B 86 8C 80 04 00 F3 0F 11 86 00 D9 04 00",
            GAType::Code, 0) > CG_CalcViewValues;
        // the theater's camera (0x6DB530, localClientNum): run by the frame (0x418540) right after
        // CG_CalcViewValues, it writes the refdef's origin and axis and the view angles again when the theater's
        // camera isn't first person
        Sig("?? ?? ?? ?? ?? ?? 81 EC 98 00 00 00 56 8B 35 ?? ?? ?? ?? 83 BE 28 99 06 00 00 57 75 0D", GAType::Code, 0)
            > CG_TheaterCamera;
        // the theater's free camera's view (0x430BC0, localClientNum): the frame (0x51DCD0) runs it after the theater's
        // camera when the camera is free (demoisanyfreemovecamera), and it writes the refdef's origin and axis again
        Sig("?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 19 E8 ?? ?? ?? ?? 84 C0 75 10 A1 ?? ?? ?? ?? 0F 57 C0 F3 0F 11 80 "
            "84 BC 0E 00",
            GAType::Code, 0) > CG_TheaterFreeCamera;
        // another view the frame runs after the theater's camera (0x6D8EF0, localClientNum), when cg + 0x6496C has bit
        // 1; it writes the refdef's origin and axis too
        Sig("?? ?? ?? ?? ?? 5C 24 20 56 8B 35 ?? ?? ?? ?? F6 86 6C 49 06 00 04 57 74", GAType::Code, 0) > CG_OtherView;
        // the pointer to the client game's cg (0x113F18C), the one with the refdef, read at the start of
        // CG_CalcViewValues. Not the structure at 0x11C7D74 (cgPointer), which has the film's time.
        Sig("?? ?? ?? ?? ?? 08 53 8B 5C 24 10 56 8B 35 ?? ?? ?? ?? 8B 86 8C 80 04 00 F3 0F 11 86 00 D9 04 00",
            GAType::Data, 14, Lambda::DereferenceAddress) > cgViewPointer;
        // the theater's Demo_SetCameraMode(localClientNum, mode) (0x405DE0): what the theater's change-camera key
        // (0x48E770) calls with the next mode. Sets the mode at theater + 0xD4C00 (Structures.hpp) and, going into or
        // out of the free camera, starts or ends it (from the third person it starts where that view was)
        Sig("?? ?? ?? ?? ?? 53 55 8B 6C 24 0C 33 DB 3B 2D ?? ?? ?? ?? 56 8B 74 24 14 0F 9D C3 57 8B B8 00 4C 0D 00",
            GAType::Code, 0) > Demo_SetCameraMode;
        // bone camera (see RE-NOTES.md)
        // Com_GetClientDObj(handle, localClientNum) (0x4DA190): an entity's model and skeleton, null when it has none
        Sig("?? ?? ?? ?? ?? 44 24 04 69 C9 01 07 00 00 03 C1 0F B7 04 45 ?? ?? ?? ?? 66 85 C0 74", GAType::Code, 0)
            > Com_GetClientDObj;
        // CG_DObjGetWorldTagMatrix(pose, obj, tagName, axis[3][3], origin[3]) (0x4FC740): a bone's place in the world,
        // 0 when the model hasn't it; T5's signature without its first bytes. Its twin at 0x4A1210 gives the origin only.
        Sig("?? ?? ?? ?? ?? 24 20 56 57 8B 7C 24 24 6A FF 8D 44 24 0F 50 51 57 C6 44 24 1B FE E8", GAType::Code, 0)
            > CG_DObjGetWorldTagMatrix;
        // SL_FindString(text) (0x6B8EB0): a script string's number, 0 when there's none (it makes none)
        Sig("?? ?? ?? ?? ?? C2 56 8D 70 01 8D 9B 00 00 00 00 8A 08 40 84 C9 75 F9 2B C6 40 50 52 E8", GAType::Code, 0)
            > SL_FindString;
        // SL_GetString(text) (0x40B250): a script string's number, made if there's none (for user 1, through
        // SL_GetStringOfSize, 0x627FE0)
        Sig("?? ?? ?? ?? ?? C2 56 8D 70 01 8D 9B 00 00 00 00 8A 08 40 84 C9 75 F9 2B C6 6A 06 40 50 6A 01 52 E8",
            GAType::Code, 0)
            > SL_GetString;
        // the name a player has in the snapshot (0x5B0F50; localClientNum, clientNum, out, size, bool withClan): looks
        // the client up in the snapshot's client states (0x120 bytes each) and copies its name; 0 if it isn't there.
        // LUI's GetFullPlayerName calls it. Its twin 0x451130 differs after the loop.
        Sig("?? ?? ?? ?? ?? 4C 24 04 C6 00 00 33 C0 3B 0D ?? ?? ?? ?? 0F 9D C0 48 23 05 ?? ?? ?? ?? 83 78 50 00 75 03 33 C0 "
            "C3 8B 88 7C 2A 00 00 53 55 56 33 F6 57 89 4C 24 14 85 C9 7E 2F 8B 78 1C 8B 58 18 8B 88 8C 2A 00 00 8B 6C 24 "
            "18 8D 9B 00 00 00 00 8B C1 99 F7 FF 8D 04 D2 C1 E0 05 03 C3 39 28 74 0F 46 41 3B 74 24 14 7C E7 5F 5E 5D 33 "
            "C0 5B C3 0F B6 4C 24 24",
            GAType::Code, 0) > CG_GetPlayerName;
        // the pointers to each local client's cg_entities (0x1140878), set where the array is allocated (0x46B630,
        // "cg_entitiesArray", 0x188000 bytes: 1,792 of 0x380)
        Sig("68 00 80 18 00 53 E8 ?? ?? ?? ?? 89 04 BD ?? ?? ?? ?? 33 C0 89 06", GAType::Data, 14,
            Lambda::DereferenceAddress)
            > cgEntities;
        // the backend's frame data (0x341D404, backEndData: the frame being drawn and shown; the frontend's is the
        // pointer before it, 0x341D400, swapped at 0x7108E0 between two of 0x48BF00 bytes from 0x3A8E800). Its views at
        // +0x474210 (GfxViewInfo, 0xC50 bytes each, its GfxViewParms first: R_SetupViewParms, 0x730C30), their count at
        // +0x474208
        Sig("A1 ?? ?? ?? ?? 81 40 18 00 04 00 00 F3 0F 10 44 24 2C 8B 44 24 28", GAType::Data, 1,
            Lambda::DereferenceAddress)
            > backEndData;
        // which of the two the frontend fills (0x3A8E790, smpFrame, toggled at 0x7108E0: the backend draws the other),
        // the two's views (0x341D440, 0x3140 bytes, four GfxViewInfo, each buffer's), and the two buffers (0x3A8E800)
        Sig("49 83 E1 01 40 25 01 00 00 80 89 0D ?? ?? ?? ?? 79", GAType::Data, 12, Lambda::DereferenceAddress) > smpFrame;
        Sig("69 C0 40 31 00 00 05 ?? ?? ?? ?? 89", GAType::Data, 7, Lambda::DereferenceAddress) > frameViewInfos;
        Sig("69 C0 00 BF 48 00 05 ?? ?? ?? ?? A3", GAType::Data, 7, Lambda::DereferenceAddress) > frameDatas;
        // CG_LocationalTrace(results, start, end, entity to skip, contents mask, 0, 0) (0x50B870): the client's trace,
        // as the client script's bullettrace (0x7CAF70) calls it; the results' fraction at +0x10
        Sig("?? ?? ?? ?? ?? 74 24 40 57 56 8D 4C 24 0C E8 ?? ?? ?? ?? 8B 44 24 4C", GAType::Code, 0) > CG_LocationalTrace;
        // actors' animations (Props.cpp): XAnimCreateAnims(debugName, size, 0, Alloc) (0x5CDFE0) and XAnimBlend(anims,
        // index, name, firstChild, numChildren, flags) (0x64F3C0), named by the strings they keep; XAnimCreateTree(anims,
        // Alloc) (0x5F3530) as the client script's useanimtree (0x7C5890) calls it, and XAnimFreeTree(tree, Free(pointer,
        // size)) (0x45B670); XAnimSetCompleteGoalWeight and XAnimSetGoalWeight(obj, index, weight, blendTime, rate,
        // notifyName, notifyType, restart, -1) (0x5B8C00, 0x5ABDB0) as its setanim calls them (0x7C5D30), and
        // XAnimSetTime(tree, index, time from 0 to 1, -1) (0x6D0C90) as its setanimtime does (0x7C70A0)
        Sig("?? ?? ?? ?? ?? 55 56 57 8B 7C 24 18 8D 44 9B 05 C1 E7 04", GAType::Code, 0) > XAnimCreateAnims;
        Sig("?? ?? ?? ?? ?? 8B 54 24 14 53 8B 5C 24 0C 55 8B 6C 24 0C", GAType::Code, 0) > XAnimBlend;
        Sig("68 ?? ?? ?? ?? 53 ?? ?? ?? ?? ?? 50 57 89 86 EC 02 00 00 E8", GAType::Code, 6, Lambda::FollowCall)
            > XAnimCreateTree;
        Sig("?? ?? ?? ?? ?? 0F B7 46 04 66 85 C0 74 0A 50 56 E8 ?? ?? ?? ?? 83 C4 08 8B 44 24 0C", GAType::Code, 0)
            > XAnimFreeTree;
        Sig("8B 4C 24 20 51 83 EC 0C F3 0F 11 4C 24 08 F3 0F 10 4C 24 38 F3 0F ?? ?? ?? ?? ?? ?? ?? ?? 24 52 50 ?? ?? ?? "
            "?? ?? EB 4B",
            GAType::Code, 33, Lambda::FollowCall) > XAnimSetCompleteGoalWeight;
        Sig("8B 4C 24 20 51 83 EC 0C F3 0F 11 4C 24 08 F3 0F 10 4C 24 38 F3 0F ?? ?? ?? ?? ?? ?? ?? ?? 24 52 50 ?? ?? ?? "
            "?? ?? 83 C4 24",
            GAType::Code, 33, Lambda::FollowCall) > XAnimSetGoalWeight;
        // the XAnimSetGoalWeight above clears its siblings (0x8DE070: each of the same "separate" group, 0x40, given
        // goal weight 0), as the script's setanimknob; this one (0x535690), the script's setanim, only sets the goal
        // weight of a node the tree already has (and does nothing to one it hasn't)
        Sig("?? ?? ?? ?? ?? ?? ?? ?? F3 0F 10 4C 24 0C 0F 2F C1 0F 57 C0 76 09 0F 28 C8 F3 0F 11 4C 24 0C 53 8B 5C 24 28 55",
            GAType::Code, 0)
            > XAnimSetGoalWeightOnly;
        Sig("F3 0F 10 44 24 0C 6A FF 51 ?? ?? ?? ?? 24 57 56 ?? ?? ?? ?? ?? 83 C4 10", GAType::Code, 16,
            Lambda::FollowCall) > XAnimSetTime;
        // the trees' XAnimInfo nodes (0x2B6EE40, 0x40 bytes each: next sibling +0xA, first child +0xC, entry +0x10, a
        // leaf's bones' map +0x12 (0 for a blend node)), as GetAnimInfo (0x486CB0) indexes them; a tree's root node
        // at +4
        Sig("8B 44 24 04 C1 E0 06 05 ?? ?? ?? ?? C3", GAType::Data, 8, Lambda::DereferenceAddress) > xanimInfos;
        // XAnimGetRelDelta(anims, index, rotation[2], translation[3], startTime, endTime) (0x5081D0): how an animation
        // moves the root between two times, as the script's getmovedelta (0x851060) calls it
        Sig("6A 00 E8 ?? ?? ?? ?? 83 C4 08 50 ?? ?? ?? ?? ?? 8D 54 24 28", GAType::Code, 11, Lambda::FollowCall)
            > XAnimGetRelDelta;
        // props (Props.cpp)
        // Com_ClientDObjCreate(models, numModels, tree, handle, localClientNum) (0x565FF0): a DObj of the models on
        // a client handle (one of 0x701 a local client has, 0x262D1C0), through DObjCreate (0x8DB490, 160 bones at
        // most)
        Sig("?? ?? ?? ?? ?? ?? 53 8B 5C 24 18 55 56 57 C6 44 24 10 00", GAType::Code, 0) > Com_ClientDObjCreate;
        // Com_SafeClientDObjFree(handle, localClientNum) (0x5D26B0): frees that DObj, under the DObjs' lock (0x16)
        Sig("?? ?? ?? ?? ?? C0 01 07 00 00 56 8B 74 24 08 6A 16", GAType::Code, 0) > Com_SafeClientDObjFree;
        // R_AddDObjToScene(obj, pose, entnum, renderFlags, lightingOrigin, float*, float*, -1, -1, 0, 0.0f, scale, 0)
        // (0x7269C0): a DObj drawn this frame, once an entity number (0x35A9114 the scene's index of each)
        Sig("?? ?? ?? ?? ?? ?? 83 EC 08 53 8B 5C 24 18 ?? ?? ?? ?? 00 66 39 04 59", GAType::Code, 0)
            > R_AddDObjToScene;
        // the assets' hash table (0x131D740, 0x9600 chains) and their entries (0x14342E0, 16 bytes each), as a zone's
        // assets are freed (0x7FE4D0)
        Sig("BD ?? ?? ?? ?? 83 C4 04 C7 86 ?? ?? ?? ?? FF FF FF FF", GAType::Data, 1, Lambda::DereferenceAddress)
            > db_hashTable;
        Sig("C1 E6 04 81 C6 ?? ?? ?? ?? 0F B6 4E 08 3B 4C 24 1C", GAType::Data, 5, Lambda::DereferenceAddress)
            > g_assetEntryPool;
        // DB_LoadXAssets(zoneInfo {name, allocFlags, freeFlags}[], count, sync, 0) (0x4174B0): the actors' packs load
        // with it as the loadzone command does (0x7FDEC0, 0x621B00), but not synced (the command waits for the zone
        // with the renderer handed to a loading screen, and the game hung there in a film); the zones loaded, as it
        // looks through them: their count (0x13DE624) and the flags of the first (0x170830C, 0x48 bytes each, the
        // zone's name 64 bytes after its flags)
        Sig("?? ?? ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 85 87 00 00 00 C6 05", GAType::Code, 0) > DB_LoadXAssets;
        Sig("8B 3D ?? ?? ?? ?? 4F 8B 03 89 44 24 20 0F 88", GAType::Data, 2, Lambda::DereferenceAddress) > g_zoneCount;
        // DB_QueueZones(zoneInfo[], count) (0x7FDDD0): what DB_LoadXAssets hands the zones it loads to, copied for the
        // database thread; the film's map comes through it even when another plugin (Redacted's ZonePlugin) calls
        // DB_LoadXAssets past a hook at its start
        Sig("?? ?? ?? ?? ?? ?? 40 53 55 8B 6C 24 10 75 0F 68", GAType::Code, 0) > DB_QueueZones;
        // A weapon's camo as the client draws a player's (0x481090, through 0x437760): for this frame, an image of the
        // models of a mask (bits of a DObj's models) replaced by another, R_AddImageOverride(mask, from, to, &slot)
        // (0x726660), and a constant of their materials set, R_AddConstOverride(mask, code, x, y, z, w, &slot)
        // (0x726830; 0xB5 the camo pattern's offset and scale); the slot, -1 to start a list, goes to
        // R_AddDObjToScene as its ninth argument
        Sig("?? ?? ?? ?? ?? 4C 24 0C 8B 54 24 08 50 8B 44 24 08 51 52 6A 00 50 E8 ?? ?? ?? ?? 83 C4 14 C3 CC 56 57 8B 7C 24 10",
            GAType::Code, 0)
            > R_AddImageOverride;
        Sig("?? ?? ?? ?? ?? 0F 10 44 24 18 8B 4C 24 08 8B 54 24 04 50 83 EC 10", GAType::Code, 0) > R_AddConstOverride;
        // actors' events: FX_PlayOrientedEffect(localClientNum, def, startTime, origin, axis[3][3], 1.0f, 0)
        // (0x67F2D0) as the client script's playfx plays one (0x6C0FB0), with cg's time (*0x113F18C, +0x4808C);
        // CG_PlaySoundAlias(localClientNum, entnum, origin, 0, 0, volume, aliasHash) (0x58E920) as its playsound
        // does (0x7BEDB0), the alias's name hashed by SND_HashName (0x58A9D0)
        Sig("?? ?? ?? ?? ?? 0F 10 44 24 18 56 57 50 51 8B 4C 24 24 F3 0F 11 04 24", GAType::Code, 0)
            > FX_PlayOrientedEffect;
        // the scene's effects that loop, kept till their bar ends: FX_SpawnEffect(system, def, msecBegin, origin,
        // axis, 0x7FF, 0x1FF, 0xFF, 0, 0x3FF, offset, 0, 0, -1, scale, 0) (0x5C1C70) as FX_PlayOrientedEffect calls it
        // before letting its reference go; FX_KillEffect(localClientNum, handle, immediately) (0x5FBD70), the effects
        // system (*0x21B29E4) +0x2D4 its being up
        Sig("68 FF 03 00 00 6A 00 68 FF 00 00 00 68 FF 01 00 00 68 FF 07 00 00 51 8B 4C 24 40 52 50 51 57 ?? ?? ?? ?? ?? 8B F0",
            GAType::Code, 31, Lambda::FollowCall)
            > FX_SpawnEffect;
        Sig("?? ?? ?? ?? ?? 80 B8 D4 02 00 00 00 56 8B F0 74 21 8B 4C 24 0C 51 50 E8", GAType::Code, 0) > FX_KillEffect;
        Sig("8B 44 24 1C 6A FF 8B 3D ?? ?? ?? ?? 6A 00 6A 00 68", GAType::Data, 8,
            Lambda::DereferenceAddress)
            > fx_system;
        // the script strings' text: a string's at the buffer (this pointer, 0x2C22AA4) + 24 * its number + 4, its length's
        // low byte at + 3 (SL_FindStringOfSize, 0x4C2626)
        Sig("8B 7D 04 8B 0D ?? ?? ?? ?? 0F B6 D3 8D 04 7F", GAType::Data, 5, Lambda::DereferenceAddress) > scrStrings;
        Sig("A1 ?? ?? ?? ?? 8B 98 8C 80 04 00 6A 01 6A 01 E8", GAType::Data, 1, Lambda::DereferenceAddress) > cgArray;
        Sig("8D 4C 24 28 23 C1 50 68 FE 03 00 00 56 ?? ?? ?? ?? ?? 50", GAType::Code, 13, Lambda::FollowCall)
            > CG_PlaySoundAlias;
        Sig("?? ?? ?? ?? ?? 85 ED 74 51 80 7D 00 00 74 4B 8B C5 8D 50 01", GAType::Code, 0) > SND_HashName;
        Sig("8D 2C FF 8D 2C ED ?? ?? ?? ?? 8D 64 24", GAType::Data, 6, Lambda::DereferenceAddress) > g_zoneFlags;
        // R_AddOmniLightToScene(origin, radius, r, g, b, intensity) (0x726F00): what the effects add their lights with
        Sig("?? ?? ?? ?? ?? F8 83 EC 0C 80 3D ?? ?? ?? ?? 00 56 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? "
            "0F 57 C9 F3 0F 10 45 0C 0F 2F C8",
            GAType::Code, 0) > R_AddOmniLightToScene;
        // R_AddSpotLightToScene(origin, axis[3][3], radius, innerFraction, startRadius, endRadius, r, g, b, intensity,
        // def) (0x727110): a cone from origin along axis[0], half angle atan((end - start) / radius), its inner part
        // innerFraction of that; def null takes the default light def
        Sig("?? ?? ?? ?? ?? F8 83 EC 28 80 3D ?? ?? ?? ?? 00 56 8B 75 0C F3 0F 10 06 F3 0F 11 44 24 20", GAType::Code, 0)
            > R_AddSpotLightToScene;
        // the loaded film's name (0x30D3380, 256 characters), which demo_play compares its argument with
        Sig("68 ?? ?? ?? ?? 85 C0 75 24 68 ?? ?? ?? ?? 8D 44 24 34 68 00 01 00 00", GAType::Data, 1,
            Lambda::DereferenceAddress) > filmName;
        // the film's bytes in memory (0x3205678, then its size and the read position): what the theater plays from,
        // filled by the lobby's download (downloaddemofile, 0x66DA80), set up by 0x6A45B0(size) with VirtualAlloc
        Sig("56 E8 ?? ?? ?? ?? 56 6A 00 50 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 10 89 35 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? "
            "00 00 00 00 5E C3",
            GAType::Data, 11, Lambda::DereferenceAddress) > filmBuffer;
        // xpartygo's handler (0x5CD780): starts the party's match (0x66AEE0, which loads the party's map). The lobby
        // the Demo tab opens has it sent by its start timer once it has party_minplayers (see Theater.cpp)
        Sig("?? ?? ?? ?? ?? 84 C0 74 12 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 83 F8 01 75 0F 6A 00 68 ?? ?? ?? ?? "
            "E8 ?? ?? ?? ?? 83 C4 08 C3",
            GAType::Code, 0) > xpartyGo;
        // the party's auto start (0x7F8EE0, the session in eax): counts a lobby with party_minplayers down to its
        // match (see Theater.cpp)
        Sig("?? ?? ?? ?? ?? F0 E8 ?? ?? ?? ?? 56 8B E8 E8 ?? ?? ?? ?? 56 8B F8 E8 ?? ?? ?? ?? 83 C4 08 3B F8 8B C6 0F 95 "
            "C3 E8",
            GAType::Code, 0) > partyAutoStart;
        // the session's modes (0x2562AC4; the LUI's SessionModeSet* set its bits, 2 the online game), read by the
        // test of bit 2 at 0x510930
        Sig("A0 ?? ?? ?? ?? C0 E8 02 24 01 C3", GAType::Data, 1, Lambda::DereferenceAddress) > sessionModes;
        // the frame's colour grade set up (0x72B7B0(frame, refdef, ...)): from the vc_ dvars with r_filmUseTweaks,
        // else copied from refdef + 0x90 (Filmtweaks.cpp)
        Sig("?? ?? ?? ?? ?? 53 8B 5C 24 10 55 56 8B 74 24 10 8B AE 7C 07 00 00 57 50 E8", GAType::Code, 0) > setupFilm;
        // the render targets' images (0x3A24A88, 0x50 bytes each, in the order of the names at 0x1064270: the sun's
        // and spot shadow maps, then $floatz, the scene's linear depth). An image's shader resource view is at +0 (the
        // render target's made by 0x75CF80: CreateShaderResourceView into the image), its name at +0x48.
        Sig("8D 34 80 8B 04 85 ?? ?? ?? ?? 0F B7 CA 0F B7 53 16 89 4C 24 44 66 8B CD 66 D3 EA C1 E6 04 81 C6",
            GAType::Data, 32, Lambda::DereferenceAddress) > renderTargetImages;
        // xpartyplaydemo's handler (0x92AF00): with the session mode theater (Com_SessionMode_IsMode(5), 0x6F7570, a
        // test of the mask at 0x2560890) or the party session (0x12A7000) running, sets up the session's players for
        // the film (0x675E90) and sends demo_play <ui_demoname>. Its code holds both addresses (Theater.cpp)
        Sig("?? ?? ?? ?? ?? ?? ?? 83 C4 04 84 C0 75 12 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 83 F8 01 75 0F 6A 00 68 "
            "?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 C3 6A 05",
            GAType::Code, 0) > xpartyPlayDemo;

#undef Sig
#undef Lambda
    };
}  // namespace IWXMVM::T6::Signatures
