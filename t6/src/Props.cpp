#include "StdInclude.hpp"
#include "Props.hpp"

#include <shared_mutex>

#include "Addresses.hpp"
#include "Functions.hpp"
#include "Components/Actors.hpp"
#include "Components/Playback.hpp"
#include "Components/Props.hpp"
#include "Components/Loadouts.hpp"
#include "Components/ActorLogic.hpp"
#include "Components/PoseLogic.hpp"
#include "Components/Rendering.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/VehicleLogic.hpp"
#include "Components/CharacterLogic.hpp"
#include "Components/CameraManager.hpp"
#include "Utilities/MathUtils.hpp"
#include "Mod.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T6::Props
{
    namespace
    {
        // the assets (T6's XAssetType: xmodelpieces, physpreset, physconstraints, destructibledef, xanim, xmodel, ...
        // col_map_mp at 12, from the names at 0xD4A8F0)
        constexpr std::int32_t ASSET_TYPE_XMODEL = 5;
        constexpr std::int32_t ASSET_TYPE_CLIPMAP_MP = 12;
        constexpr std::size_t DB_HASH_TABLE_SIZE = 0x9600;
        struct XAssetEntry
        {
            std::int32_t type;
            const void* header;
            std::uint8_t zoneIndex;
            std::uint8_t inuse;
            std::uint16_t nextHash;
            std::uint16_t nextOverride;
            std::uint16_t usageFrame;
        };
        static_assert(sizeof(XAssetEntry) == 16);

        // an XModel: its name first, its bones' count at +4 (XModelNumBones, 0x4C2B40)
        constexpr std::size_t XMODEL_NUM_BONES = 4;
        // DObjCreate (0x8DB490) drops out of the film (Com_Error, ERR_DROP) with a DObj whose models have more in all
        constexpr std::uint32_t MAX_DOBJ_BONES = 160;

        // DObjModel_s: the model, the bone it hangs on and its model, whether it collides, an offset and angles
        struct DObjModel
        {
            const void* model = nullptr;
            std::uint16_t boneName = 0;
            std::uint16_t parentModel = 0;
            bool ignoreCollision = false;
            float position[3]{};
            float angles[3]{};
        };
        static_assert(sizeof(DObjModel) == 0x24);

        // the client's entities: 1,792 of them, each with its pose at the start
        constexpr std::size_t ENTITY_SIZE = 0x380;
        constexpr std::size_t POSE_ORIGIN = 0x2C;
        constexpr std::size_t POSE_ANGLES = 0x38;
        constexpr std::size_t ENTITY_NUMBER = 0x1DC;
        // the handles props take, from the top of the client's entities (those the game gives its own client
        // entities start well under them)
        constexpr std::int32_t TOP_HANDLE = 0x6FF;
        constexpr std::int32_t MAX_HANDLES = 64;

        // a prop's place on the ground when it's put in the scene: in front of the camera, without a trace of the map
        constexpr float PLACE_AHEAD = 120.0f;

        // the preview's key among the drawn models
        constexpr std::uint32_t PREVIEW_KEY = 0xFFFFFFFF;

        struct Drawn
        {
            std::vector<std::string> models;
            std::int32_t handle = 0;
            void* obj = nullptr;
            bool drawn = false;
            // a person's animations: the tree's entries after its root, their parts, the tree, and the weights given
            // to its entries the frame before
            std::vector<std::string> animations;
            std::uint8_t* anims = nullptr;
            void* tree = nullptr;
            std::vector<float> lastWeights;
            // its guns' camos: a WeaponCamo, its set, and the DObj's models (bits) it's on
            struct Camo
            {
                const std::uint8_t* camo;
                std::int32_t set;
                std::uint32_t mask;
            };
            std::vector<Camo> camos;
        };
        std::map<std::uint32_t, Drawn> drawnModels;  // by actor id
        std::map<std::uint32_t, std::vector<std::string>> failedModels;  // by actor id: what wasn't found

        // the animations loaded (XAnimParts: numframes at +0xE, loop at +0x10, framerate at +0x30, as T5's), by name
        constexpr std::int32_t ASSET_TYPE_XANIMPARTS = 4;
        constexpr std::size_t PARTS_NUM_FRAMES = 0xE, PARTS_LOOP = 0x10, PARTS_DELTA = 0x11, PARTS_FRAMERATE = 0x30;
        constexpr std::size_t PARTS_SIZE = 0x68;  // the whole XAnimParts
        // WeaponCamo: the images its materials have where a camo goes, and its sets of a camo's images (20 bytes:
        // solid, pattern, the pattern's offset and scale); GfxImage's name
        constexpr std::size_t CAMO_SOLID_BASE = 4, CAMO_PATTERN_BASE = 8, CAMO_SETS = 12, CAMO_SET_COUNT = 16,
                              CAMO_SET_SIZE = 20, IMAGE_NAME = 72;
        constexpr std::uint32_t MAX_ROOT_MOTION_STEPS = 64;
        std::map<std::string, const std::uint8_t*, std::less<>> animationParts;
        // the models loaded, by name; with animationParts, what the browser's previews read on the render thread:
        // changed under catalogMutex alone (and not read while a pack loads)
        std::map<std::string, const std::uint8_t*, std::less<>> modelPointers;
        std::shared_mutex catalogMutex;
        std::atomic<bool> packBusy = false;

        float GetDuration(const std::uint8_t* parts)
        {
            const auto frames = *reinterpret_cast<const std::uint16_t*>(parts + PARTS_NUM_FRAMES);
            const auto rate = *reinterpret_cast<const float*>(parts + PARTS_FRAMERATE);
            return rate > 0.0f && std::isfinite(rate) ? static_cast<float>(frames) / rate : 0.0f;
        }

        // the anim trees: XAnim_s (T6's, XAnimCreateAnims 0x5CDFE0: the debug name, its size, ... the debug names of
        // its entries at +0x10, its entries of 16 bytes from +0x14), made and freed with these
        constexpr std::size_t XANIM_ENTRIES = 0x14, XANIM_ENTRY_SIZE = 0x10, XANIM_ENTRY_PARTS = 0xC,
                              XANIM_DEBUG_NAMES = 0x10;
        constexpr std::uint32_t ROOT_INDEX = 0, FIRST_CLIP_INDEX = 1;
        constexpr std::size_t MAX_ANIMATIONS = 48;
        // XAnimSetGoalWeight takes a weight under 0.001 for 0: weights are given a hundred times as they are, the same
        // to the game, which blends by their ratio (as T5's actors)
        constexpr float WEIGHT_SCALE = 100.0f;

        void* __cdecl Alloc(int size)
        {
            return std::calloc(1, static_cast<std::size_t>(size));
        }
        void __cdecl Free(void* pointer, int)
        {
            std::free(pointer);
        }
        typedef void*(__cdecl* Alloc_t)(int size);
        typedef void(__cdecl* Free_t)(void* pointer, int size);

        std::map<std::uint32_t, std::string> lastShown;  // what each actor showed, for the log
        // the animation browser's preview, and when it started
        std::optional<std::string> previewedAnimation;
        std::uint64_t previewStart = 0;

        // ---- The animation browser's previews: an animation's data decoded (laid out as T5's and IW3's, see T5's
        // AnimationPose.cpp; XAnimParts: its streams' counts at +4, the bones' counts by part type at +0x18,
        // randomDataShortCount +0x28, indexCount +0x2C, the bones' names +0x40, the streams from +0x44, the indices
        // +0x5C) and posed on a model's bones at rest (XModel: its bones' names at +8, parents +0xC, rotations +0x10
        // and translations +0x14 of the bones after its roots, with a stride worked out from the model)
        enum PartType
        {
            PART_TYPE_NO_QUAT,
            PART_TYPE_HALF_QUAT,
            PART_TYPE_FULL_QUAT,
            PART_TYPE_HALF_QUAT_NO_SIZE,
            PART_TYPE_FULL_QUAT_NO_SIZE,
            PART_TYPE_SMALL_TRANS,
            PART_TYPE_TRANS,
            PART_TYPE_TRANS_NO_SIZE,
            PART_TYPE_NO_TRANS,
            PART_TYPE_ALL,
        };

        struct Track
        {
            std::vector<std::uint16_t> frames;
            std::vector<glm::vec4> values;
        };
        struct DecodedPose
        {
            std::uint16_t numFrames = 0;
            float duration = 0.0f;
            std::vector<std::optional<Track>> rotations, translations;  // by bone of the body
        };
        std::map<std::pair<std::string, std::string>, std::optional<DecodedPose>> decodedPoses;  // under poseMutex
        std::mutex poseMutex;

        template <typename T>
        struct Stream
        {
            const T* data;
            std::size_t count;
            std::size_t position = 0;
            bool Take(std::size_t n, const T*& out)
            {
                if (!data || position + n > count)
                    return false;
                out = data + position;
                position += n;
                return true;
            }
        };

        template <typename T>
        T Read(const std::uint8_t* base, std::size_t offset)
        {
            T value;
            std::memcpy(&value, base + offset, sizeof(T));
            return value;
        }

        struct ModelView
        {
            std::uint32_t numBones = 0, numRootBones = 0;
            const std::uint16_t* boneNames = nullptr;
            const std::uint8_t* parentList = nullptr;
            const std::int16_t* quats = nullptr;  // 4 each
            const float* trans = nullptr;
            std::size_t transStride = 3;  // floats
        };

        glm::quat ToQuat(const glm::vec4& q)
        {
            return glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
        }

        std::vector<PoseLogic::LocalBone> GetRestBones(const ModelView& model)
        {
            std::vector<PoseLogic::LocalBone> bones(model.numBones);
            for (std::uint32_t b = model.numRootBones; b < model.numBones; ++b)
            {
                const auto i = b - model.numRootBones;
                const auto q = model.quats + i * 4;
                const auto t = model.trans + i * model.transStride;
                bones[b] = {.rotation = ToQuat(glm::vec4(q[0], q[1], q[2], q[3])),
                            .translation = glm::vec3(t[0], t[1], t[2]),
                            .parent = static_cast<std::int32_t>(b) - model.parentList[i]};
            }
            return bones;
        }

        // the model's fields, its translations' stride the one that stands it up as a body stands (the highest bone
        // 40 to 90 units above the lowest, everything within 200 units), else 3
        std::optional<ModelView> ViewModel(const std::uint8_t* model)
        {
            ModelView view{.numBones = model[4],
                           .numRootBones = model[5],
                           .boneNames = Read<const std::uint16_t*>(model, 8),
                           .parentList = Read<const std::uint8_t*>(model, 0xC),
                           .quats = Read<const std::int16_t*>(model, 0x10),
                           .trans = Read<const float*>(model, 0x14)};
            if (!view.boneNames || view.numBones == 0 || view.numRootBones > view.numBones ||
                (view.numBones > view.numRootBones && (!view.parentList || !view.quats || !view.trans)))
                return std::nullopt;
            for (const std::size_t stride : {std::size_t{3}, std::size_t{4}})
            {
                view.transStride = stride;
                Types::AnimationPose pose{};
                PoseLogic::Compose(GetRestBones(view), pose);
                float low = FLT_MAX, high = -FLT_MAX, reach = 0.0f;
                bool finite = true;
                for (const auto& position : pose.positions)
                {
                    finite &= std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
                    low = std::min(low, position.z);
                    high = std::max(high, position.z);
                    reach = std::max(reach, glm::length(position));
                }
                if (finite && high - low > 40.0f && high - low < 90.0f && reach < 200.0f)
                    return view;
            }
            view.transStride = 3;
            return view;
        }

        std::optional<DecodedPose> Decode(const std::uint8_t* parts, const ModelView& body)
        {
            const auto numFrames = Read<std::uint16_t>(parts, PARTS_NUM_FRAMES);
            const auto framerate = Read<float>(parts, PARTS_FRAMERATE);
            if (numFrames == 0 || framerate <= 0.0f)
                return std::nullopt;
            std::array<std::uint8_t, 10> boneCount{};
            std::memcpy(boneCount.data(), parts + 0x18, boneCount.size());
            const auto names = Read<const std::uint16_t*>(parts, 0x40);
            Stream<std::uint8_t> dataByte{Read<const std::uint8_t*>(parts, 0x44), Read<std::uint16_t>(parts, 4)};
            Stream<std::int16_t> dataShort{Read<const std::int16_t*>(parts, 0x48), Read<std::uint16_t>(parts, 6)};
            Stream<float> dataInt{Read<const float*>(parts, 0x4C), Read<std::uint16_t>(parts, 8)};
            Stream<std::int16_t> randomDataShort{Read<const std::int16_t*>(parts, 0x50), Read<std::uint32_t>(parts, 0x28)};
            Stream<std::uint8_t> randomDataByte{Read<const std::uint8_t*>(parts, 0x54), Read<std::uint16_t>(parts, 0xA)};
            const bool longFrames = numFrames >= 256;
            Stream<std::uint16_t> indices{longFrames ? Read<const std::uint16_t*>(parts, 0x5C) : nullptr,
                                          longFrames ? Read<std::uint32_t>(parts, 0x2C) : 0};
            if (!names)
                return std::nullopt;

            DecodedPose decoded{.numFrames = numFrames,
                                .duration = numFrames / framerate,
                                .rotations = std::vector<std::optional<Track>>(body.numBones),
                                .translations = std::vector<std::optional<Track>>(body.numBones)};
            const auto count = boneCount[PART_TYPE_ALL];
            std::vector<std::int32_t> bodyBones(count, -1);
            for (std::uint32_t i = 0; i < count; ++i)
                for (std::uint32_t b = 0; b < body.numBones; ++b)
                    if (body.boneNames[b] == names[i])
                        bodyBones[i] = static_cast<std::int32_t>(b);

            const auto readKeys = [&](Track& track) {
                const std::int16_t* keyCount;
                if (!dataShort.Take(1, keyCount))
                    return false;
                const std::size_t last = static_cast<std::uint16_t>(*keyCount);
                if (!longFrames)
                {
                    const std::uint8_t* frames;
                    if (!dataByte.Take(last + 1, frames))
                        return false;
                    track.frames.assign(frames, frames + last + 1);
                    return true;
                }
                const std::uint16_t* frames;
                if (last >= 64)
                {
                    const std::int16_t* table;
                    if (!dataShort.Take(((last - 1) >> 8) + 2, table) || !indices.Take(last + 1, frames))
                        return false;
                }
                else
                {
                    const std::int16_t* shorts;
                    if (!dataShort.Take(last + 1, shorts))
                        return false;
                    frames = reinterpret_cast<const std::uint16_t*>(shorts);
                }
                track.frames.assign(frames, frames + last + 1);
                return true;
            };
            const auto store = [&](bool rotation, std::uint32_t animBone, Track&& track) {
                if (animBone < bodyBones.size() && bodyBones[animBone] >= 0)
                    (rotation ? decoded.rotations : decoded.translations)[bodyBones[animBone]] = std::move(track);
            };

            std::uint32_t bone = boneCount[PART_TYPE_NO_QUAT];
            for (const auto type : {PART_TYPE_HALF_QUAT, PART_TYPE_FULL_QUAT})
            {
                const std::size_t components = type == PART_TYPE_HALF_QUAT ? 2 : 4;
                for (std::uint32_t i = 0; i < boneCount[type]; ++i, ++bone)
                {
                    Track track;
                    if (!readKeys(track))
                        return std::nullopt;
                    for (std::size_t k = 0; k < track.frames.size(); ++k)
                    {
                        const std::int16_t* q;
                        if (!randomDataShort.Take(components, q))
                            return std::nullopt;
                        track.values.push_back(components == 2 ? glm::vec4(0, 0, q[0], q[1]) / 32767.0f
                                                               : glm::vec4(q[0], q[1], q[2], q[3]) / 32767.0f);
                    }
                    store(true, bone, std::move(track));
                }
            }
            for (const auto type : {PART_TYPE_HALF_QUAT_NO_SIZE, PART_TYPE_FULL_QUAT_NO_SIZE})
            {
                const std::size_t components = type == PART_TYPE_HALF_QUAT_NO_SIZE ? 2 : 4;
                for (std::uint32_t i = 0; i < boneCount[type]; ++i, ++bone)
                {
                    const std::int16_t* q;
                    if (!dataShort.Take(components, q))
                        return std::nullopt;
                    Track track{.frames = {0}};
                    track.values.push_back(components == 2 ? glm::vec4(0, 0, q[0], q[1]) / 32767.0f
                                                           : glm::vec4(q[0], q[1], q[2], q[3]) / 32767.0f);
                    store(true, bone, std::move(track));
                }
            }
            for (const auto type : {PART_TYPE_SMALL_TRANS, PART_TYPE_TRANS})
            {
                for (std::uint32_t i = 0; i < boneCount[type]; ++i)
                {
                    const std::uint8_t* animBone;
                    Track track;
                    const float* minsAndSize;
                    if (!dataByte.Take(1, animBone) || !readKeys(track) || !dataInt.Take(6, minsAndSize))
                        return std::nullopt;
                    const glm::vec3 mins(minsAndSize[0], minsAndSize[1], minsAndSize[2]);
                    const glm::vec3 size(minsAndSize[3], minsAndSize[4], minsAndSize[5]);
                    for (std::size_t k = 0; k < track.frames.size(); ++k)
                    {
                        glm::vec3 steps;
                        if (type == PART_TYPE_SMALL_TRANS)
                        {
                            const std::uint8_t* v;
                            if (!randomDataByte.Take(3, v))
                                return std::nullopt;
                            steps = {v[0], v[1], v[2]};
                        }
                        else
                        {
                            const std::int16_t* v;
                            if (!randomDataShort.Take(3, v))
                                return std::nullopt;
                            steps = {static_cast<std::uint16_t>(v[0]), static_cast<std::uint16_t>(v[1]),
                                     static_cast<std::uint16_t>(v[2])};
                        }
                        track.values.emplace_back(mins + size * steps, 0.0f);
                    }
                    store(false, *animBone, std::move(track));
                }
            }
            for (std::uint32_t i = 0; i < boneCount[PART_TYPE_TRANS_NO_SIZE]; ++i)
            {
                const std::uint8_t* animBone;
                const float* t;
                if (!dataByte.Take(1, animBone) || !dataInt.Take(3, t))
                    return std::nullopt;
                store(false, *animBone, Track{.frames = {0}, .values = {glm::vec4(t[0], t[1], t[2], 0)}});
            }
            return decoded;
        }

        glm::vec4 Sample(const Track& track, float frame, bool rotation)
        {
            const auto next = std::upper_bound(track.frames.begin(), track.frames.end(), frame);
            if (next == track.frames.begin())
                return track.values.front();
            if (next == track.frames.end())
                return track.values.back();
            const auto i = static_cast<std::size_t>(next - track.frames.begin());
            const float t = (frame - track.frames[i - 1]) / static_cast<float>(track.frames[i] - track.frames[i - 1]);
            auto a = track.values[i - 1];
            auto b = track.values[i];
            if (rotation && glm::dot(a, b) < 0.0f)
                b = -b;
            return a + (b - a) * t;
        }

        // the body's bones at a frame of a decoded animation, composed into the model's space
        Types::AnimationPose PoseAt(const DecodedPose& decoded, const ModelView& body, float frame,
                                    const DecodedPose* under = nullptr)
        {
            auto bones = GetRestBones(body);
            // (an upper body one over the first frame of a stance's, which gives the bones it hasn't)
            for (std::uint32_t b = 0; under && b < body.numBones; ++b)
            {
                if (decoded.rotations[b] || decoded.translations[b])
                    continue;
                if (under->rotations[b])
                    bones[b].rotation = ToQuat(Sample(*under->rotations[b], 0.0f, true));
                if (under->translations[b])
                    bones[b].translation += glm::vec3(Sample(*under->translations[b], 0.0f, false));
            }
            for (std::uint32_t b = 0; b < body.numBones; ++b)
            {
                if (decoded.rotations[b])
                    bones[b].rotation = ToQuat(Sample(*decoded.rotations[b], frame, true));
                if (decoded.translations[b])
                    bones[b].translation += glm::vec3(Sample(*decoded.translations[b], frame, false));
            }
            Types::AnimationPose pose{.names = std::vector<std::string>(body.numBones), .duration = decoded.duration};
            PoseLogic::Compose(bones, pose);
            return pose;
        }

        // Why an animation can't be one a person plays (none for one that can): its data doesn't decode, it moves
        // few of a body's bones (another skeleton's, as the vehicles' and turrets'), or the body it poses is broken
        // up or thrown far off (an animation of another rig, or one the game only blends into others)
        std::optional<std::string_view> WhyUnusable(const std::uint8_t* parts, const ModelView& body)
        {
            const auto decoded = Decode(parts, body);
            if (!decoded)
                return "its data doesn't decode";
            const auto moved = std::ranges::count_if(decoded->rotations, [](const auto& track) { return track.has_value(); });
            if (moved < 20)
                return "it moves few of a body's bones";
            for (const float time : {0.0f, 0.5f, 1.0f})
            {
                const auto pose = PoseAt(*decoded, body, time * decoded->numFrames);
                float low = FLT_MAX, high = -FLT_MAX, reach = 0.0f;
                for (const auto& position : pose.positions)
                {
                    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                        return "it poses the body with bones nowhere";
                    low = std::min(low, position.z);
                    high = std::max(high, position.z);
                }
                // (the spread of the body around its own middle: a person's is within a few feet of it)
                glm::vec3 middle{};
                for (const auto& position : pose.positions)
                    middle += position;
                middle /= static_cast<float>(std::max<std::size_t>(pose.positions.size(), 1));
                for (const auto& position : pose.positions)
                    reach = std::max(reach, glm::length(position - middle));
                if (high - low < 3.0f || high - low > 160.0f || reach > 160.0f)
                    return "it poses the body broken up";
            }
            return std::nullopt;
        }

        std::optional<std::uint32_t> sceneGeneration;
        // each actor's models with its guns' (AddWeapon's), as of the scene's generation and the catalog
        struct ActorModels
        {
            std::optional<std::uint32_t> generation;
            const void* catalog = nullptr;
            bool gunless = false;  // left without its guns (a Green screen's without them)
            std::vector<std::string> models;
        };
        std::map<std::uint32_t, ActorModels> actorModels;
        std::vector<Types::Actor> scene;
        bool loggedFirst = false;

        void* GetClientDObj(std::int32_t handle)
        {
            typedef void*(__cdecl * Com_GetClientDObj_t)(int handle, int localClientNum);
            return reinterpret_cast<Com_GetClientDObj_t>(GetGameAddresses().Com_GetClientDObj())(handle, 0);
        }

        std::uint8_t* GetEntity(std::int32_t handle)
        {
            const auto entities = *reinterpret_cast<std::uint8_t* const*>(GetGameAddresses().cgEntities());
            return entities ? entities + static_cast<std::size_t>(handle) * ENTITY_SIZE : nullptr;
        }

        template <typename Visit>
        void ForEachAsset(Visit visit)
        {
            const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
            const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
            if (!hashTable || !entries)
                return;
            for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
            {
                // (a chain longer than the pool is one being changed as it's read)
                std::size_t steps = 0;
                for (auto index = hashTable[hash]; index != 0 && steps < 4096; index = entries[index].nextHash, ++steps)
                    if (entries[index].header)
                        visit(entries[index]);
            }
        }

        const char* GetAssetName(const XAssetEntry& entry)
        {
            const auto name = *static_cast<const char* const*>(entry.header);
            return name && !IsBadReadPtr(name, 1) ? name : nullptr;
        }

        // A character's model: c_..., not the pieces a body falls apart into, view models or arms (as T5's)
        bool IsCharacterModel(std::string_view name)
        {
            constexpr std::array SKIP{"_g_", "gib", "viewmodel", "viewhands", "_vm_", "_arms", "_viewarms"};
            return name.starts_with("c_") && std::ranges::none_of(SKIP, [&](const char* part) {
                       return name.find(part) != std::string_view::npos;
                   });
        }

        Types::ActorCatalog::ModelKind Classify(std::string_view name)
        {
            using Kind = Types::ActorCatalog::ModelKind;
            constexpr std::array GEAR{"headgear", "headwear", "hat",    "helmet", "beret", "cap",  "gasmask",
                                      "goggle",   "hood",     "bandana", "glasses", "gear", "mask", "beanie"};
            if (std::ranges::any_of(GEAR, [&](const char* part) { return name.find(part) != std::string_view::npos; }))
                return Kind::Gear;
            if (name.find("head") != std::string_view::npos)
                return Kind::Head;
            if (name.ends_with("_fb") || name.find("_fb_") != std::string_view::npos ||
                name.find("fullbody") != std::string_view::npos)
                return Kind::FullBody;
            return Kind::Body;
        }

        const void* FindModel(const std::string& name)
        {
            const void* found = nullptr;
            ForEachAsset([&](const XAssetEntry& entry) {
                if (found || entry.type != ASSET_TYPE_XMODEL)
                    return;
                if (const auto assetName = GetAssetName(entry); assetName && name == assetName)
                    found = entry.header;
            });
            return found;
        }

        std::uint32_t CountBones(const void* model)
        {
            return *(static_cast<const std::uint8_t*>(model) + XMODEL_NUM_BONES);
        }

        void Release(const Drawn& drawn)
        {
            typedef void(__cdecl * Com_SafeClientDObjFree_t)(int handle, int localClientNum);
            typedef void(__cdecl * XAnimFreeTree_t)(void* tree, Free_t free);
            const bool ours = drawn.obj && GetClientDObj(drawn.handle) == drawn.obj;
            if (ours)
                reinterpret_cast<Com_SafeClientDObjFree_t>(GetGameAddresses().Com_SafeClientDObjFree())(drawn.handle, 0);
            // (the tree and its animations once the DObj is gone; with a DObj the game freed with its map they're
            // left, as the game may have freed what they hold)
            if (ours && drawn.tree)
                reinterpret_cast<XAnimFreeTree_t>(GetGameAddresses().XAnimFreeTree())(drawn.tree, Free);
            if (ours && drawn.anims)
                std::free(drawn.anims);
        }

        // DObjs no longer drawn, freed a few frames on (one freed while the renderer may still have it from the frame
        // before, as a gun or a look changed, could hang the game with several actors): each with the frames since
        std::vector<std::pair<Drawn, int>> retired;
        constexpr int RETIRE_FRAMES = 4;

        void Retire(const Drawn& drawn)
        {
            // (one the game freed itself, with its map, has nothing to wait for)
            if (drawn.obj && GetClientDObj(drawn.handle) == drawn.obj)
                retired.emplace_back(drawn, 0);
            else
                Release(drawn);
        }

        void FreeRetired(bool all)
        {
            std::erase_if(retired, [&](auto& entry) {
                if (!all && ++entry.second < RETIRE_FRAMES)
                    return false;
                LOG_DEBUG("T6: freeing {} on handle {:#x}, retired {} frames ago",
                          entry.first.models.empty() ? "a DObj" : entry.first.models.front(), entry.first.handle,
                          entry.second);
                Release(entry.first);
                LOG_DEBUG("T6: freed handle {:#x}", entry.first.handle);
                return true;
            });
        }

        // The players' upper body pt_ animations are XAnimParts' asset type 1 (+0x23), but their frames are whole
        // poses of the upper body (the first of pt_reload_stand_rifle is pb_stand_alert's, bone for bone), not what
        // they add to one: under an additive node (0x10) they added the stance's pose to itself, the body bent over.
        // They're played as the other upper body clips are, blended over the legs, which give the bones they lack.
        // (What an additive node would hold, last in a tree under it, is kept for animations that are deltas.)
        constexpr std::size_t PARTS_ASSET_TYPE = 0x23;
        constexpr std::uint8_t ASSET_TYPE_UPPER_BODY = 1;
        constexpr std::uint32_t BLEND_ADDITIVE = 0x10;
        bool IsAdditive(const std::string&)
        {
            return false;
        }
        bool IsUpperBodyLayer(const std::string& name)
        {
            const auto parts = animationParts.find(name);
            return parts != animationParts.end() && *(parts->second + PARTS_ASSET_TYPE) == ASSET_TYPE_UPPER_BODY;
        }
        // the legs an upper body pt_ one is shown over, by its name: its stance (prone, crouched, else standing) with
        // the gun it's for where the game has that stance's (a pistol's, a launcher's), as the players hold them
        std::string StanceUnder(std::string_view name)
        {
            const std::string base = name.find("prone") != std::string_view::npos    ? "pb_prone_aim"
                                     : name.find("crouch") != std::string_view::npos ? "pb_crouch_alert"
                                                                                     : "pb_stand_alert";
            for (const auto gun : {"pistol", "rpg"})
                if (name.find(gun) != std::string_view::npos)
                    if (const auto withGun = base + "_" + gun; animationParts.contains(withGun))
                        return withGun;
            return base;
        }
        // how many of a tree's animations, the additive ones last, aren't additive
        std::size_t CountNormal(const std::vector<std::string>& names)
        {
            return static_cast<std::size_t>(
                std::ranges::find_if(names, [](const std::string& name) { return IsAdditive(name); }) -
                names.begin());
        }
        // the entry of the tree's animation at a position: the others after the root, then the additive node and
        // the additive ones under it
        std::uint32_t EntryOf(std::size_t position, std::size_t normal)
        {
            return FIRST_CLIP_INDEX + static_cast<std::uint32_t>(position < normal ? position : position + 1);
        }

        // The animations of a person's tree: a root blending them, each entry given its parts (as XAnimCreate does
        // with the parts it finds by name: the entry's first u16 1, the next 0, the parts at +0xC); with additive ones
        // (after the others), an additive node over them as the root's last child
        std::uint8_t* CreateAnimations(const std::vector<std::string>& names, std::optional<std::size_t> normalCount =
                                                                                  std::nullopt)
        {
            typedef std::uint8_t*(__cdecl * XAnimCreateAnims_t)(const char* debugName, std::uint32_t size,
                                                               std::uint32_t, Alloc_t alloc);
            typedef void(__cdecl * XAnimBlend_t)(void* anims, std::uint32_t index, const char* name,
                                                 std::uint32_t firstChild, std::uint32_t numChildren,
                                                 std::uint32_t flags);
            const auto& addresses = GetGameAddresses();
            const auto count = static_cast<std::uint32_t>(names.size());
            const auto normal = static_cast<std::uint32_t>(normalCount.value_or(names.size()));
            const auto additive = count - normal;
            const auto entries = count + 1 + (additive > 0 ? 1 : 0);
            const auto anims =
                reinterpret_cast<XAnimCreateAnims_t>(addresses.XAnimCreateAnims())("actor", entries, 0, Alloc);
            if (!anims)
                return nullptr;
            const auto XAnimBlend = reinterpret_cast<XAnimBlend_t>(addresses.XAnimBlend());
            XAnimBlend(anims, ROOT_INDEX, "root", FIRST_CLIP_INDEX, normal + (additive > 0 ? 1 : 0), 0);
            if (additive > 0)
                XAnimBlend(anims, FIRST_CLIP_INDEX + normal, "additive", FIRST_CLIP_INDEX + normal + 1, additive,
                           BLEND_ADDITIVE);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto index = EntryOf(i, normal);
                const auto entry = anims + XANIM_ENTRIES + index * XANIM_ENTRY_SIZE;
                const std::uint16_t animation = 1, children = 0;
                std::memcpy(entry, &animation, sizeof(animation));
                std::memcpy(entry + 2, &children, sizeof(children));
                const auto parts = animationParts.at(names[i]);
                std::memcpy(entry + XANIM_ENTRY_PARTS, &parts, sizeof(parts));
                if (const auto debugNames = *reinterpret_cast<const char***>(anims + XANIM_DEBUG_NAMES))
                    debugNames[index] = *reinterpret_cast<const char* const*>(parts);
            }
            return anims;
        }

        void GetRelDelta(std::uint8_t* anims, std::uint32_t index, float (&rotation)[2], float (&translation)[3],
                         float from, float to)
        {
            typedef void(__cdecl * XAnimGetRelDelta_t)(void* anims, std::uint32_t index, float* rotation,
                                                       float* translation, float startTime, float endTime);
            reinterpret_cast<XAnimGetRelDelta_t>(GetGameAddresses().XAnimGetRelDelta())(anims, index, rotation,
                                                                                         translation, from, to);
        }

        // How far each animation moves the root in one play (x forward, y to the left), which paths are walked at
        // and the legs' animations played as fast as: from the game's delta, measured in a tree's animations made of
        // them for it (as T5's actors measure them)
        void MeasureMotions(const std::vector<std::string>& names)
        {
            if (names.empty())
                return;
            const auto anims = CreateAnimations(names);
            if (!anims)
                return;
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                float rotation[2] = {}, translation[3] = {};
                if (*(animationParts.at(names[i]) + PARTS_DELTA))
                    GetRelDelta(anims, FIRST_CLIP_INDEX + static_cast<std::uint32_t>(i), rotation, translation, 0.0f,
                                1.0f);
                Components::Actors::SetAnimationMotion(names[i], {translation[0], translation[1]});
            }
            std::free(anims);
        }

        // and how an animation moves the root through a play, for clips that move the actor by it
        std::shared_ptr<const ActorLogic::RootMotion> MeasureRootMotion(std::uint8_t* anims, std::uint32_t index,
                                                                        const std::uint8_t* parts)
        {
            auto motion = std::make_shared<ActorLogic::RootMotion>();
            const auto frames = *reinterpret_cast<const std::uint16_t*>(parts + PARTS_NUM_FRAMES);
            const auto steps = std::clamp<std::uint32_t>(frames, 1, MAX_ROOT_MOTION_STEPS);
            float turned = 0.0f;
            for (std::uint32_t step = 0; step <= steps; ++step)
            {
                float rotation[2] = {}, translation[3] = {};
                GetRelDelta(anims, index, rotation, translation, 0.0f, static_cast<float>(step) / steps);
                // the turn is a quaternion about z (the sine and cosine of half of it), unwrapped from step to step
                const auto yaw = glm::degrees(2.0f * std::atan2(rotation[0], rotation[1]));
                turned += std::remainder(yaw - turned, 360.0f);
                motion->samples.emplace_back(translation[0], translation[1], turned);
            }
            return motion;
        }

        // where an animation is at a time in seconds, from 0 to 1: looping, or held at its last frame
        float GetAnimationTime(const std::uint8_t* parts, float time, bool loop)
        {
            const auto duration = GetDuration(parts);
            if (duration <= 0.0f)
                return 0.0f;
            if (!loop)
                return std::clamp(time / duration, 0.0f, 1.0f);
            const auto wrapped = std::fmod(time, duration);
            return (wrapped < 0.0f ? wrapped + duration : wrapped) / duration;
        }

        // the entries a tree has nodes (XAnimInfo) for, those the game made as they were weighed
        std::set<std::uint32_t> TreeNodes(void* tree)
        {
            std::set<std::uint32_t> entries;
            if (!tree)
                return entries;
            const auto infos = reinterpret_cast<const std::uint8_t*>(GetGameAddresses().xanimInfos());
            const auto u16 = [&](std::uint16_t index, std::size_t offset) {
                return *reinterpret_cast<const std::uint16_t*>(infos + static_cast<std::size_t>(index) * 0x40 + offset);
            };
            std::vector<std::uint16_t> pending{*reinterpret_cast<const std::uint16_t*>(static_cast<std::uint8_t*>(tree) + 4)};
            for (std::size_t guard = 0; !pending.empty() && guard < 4096; ++guard)
            {
                auto index = pending.back();
                pending.pop_back();
                for (std::size_t siblings = 0; index && siblings < 256; ++siblings)
                {
                    entries.insert(u16(index, 0x10));
                    if (u16(index, 0x12) == 0)
                        if (const auto child = u16(index, 0xC))
                            pending.push_back(child);
                    index = u16(index, 0xA);
                }
            }
            return entries;
        }

        // Weighs the tree's entries by what the actor shows now and sets their times (as T5's actors: entries play
        // at rate 0, so only the times set here move them; one shown before and not now is set to 0)
        void Animate(Drawn& drawn, const ActorLogic::TreeWeights& shown)
        {
            typedef void(__cdecl * XAnimSetGoalWeight_t)(void* obj, std::uint32_t index, float weight,
                                                         float blendTime, float rate, std::uint32_t notifyName,
                                                         std::uint32_t notifyType, std::int32_t restart,
                                                         std::int32_t);
            typedef void(__cdecl * XAnimSetTime_t)(void* tree, std::uint32_t index, float time, std::int32_t);
            const auto& addresses = GetGameAddresses();
            const auto XAnimSetGoalWeight = reinterpret_cast<XAnimSetGoalWeight_t>(addresses.XAnimSetGoalWeight());
            const auto XAnimSetTime = reinterpret_cast<XAnimSetTime_t>(addresses.XAnimSetTime());

            const auto count = drawn.animations.size();
            const auto normal = CountNormal(drawn.animations);
            std::vector<float> weights(count, 0.0f), times(count, 0.0f);
            for (const auto& entry : shown.animations)
            {
                const auto& name = entry.clip ? entry.clip->animation : entry.animation;
                const auto at = std::ranges::find(drawn.animations, name);
                if (at == drawn.animations.end())
                    continue;
                const auto position = static_cast<std::size_t>(at - drawn.animations.begin());
                const auto parts = animationParts.at(name);
                const bool loop = entry.clip ? entry.clip->loop : true;
                times[position] = GetAnimationTime(parts, entry.time, loop);
                // (an additive one by as much as it's shown: 1 adds all it does)
                weights[position] += entry.weight * (position < normal ? WEIGHT_SCALE : 1.0f);
            }
            // Each frame, every one that shows given its weight with the plain variant, which sets it without
            // touching the others. Only one the tree has no node for yet goes through the knob variant first, which
            // makes the node (the plain one does nothing without it) but clears its siblings: each of them knobbed
            // each frame left only the last one set (a blend of two clips snapped from one to the other)
            const auto XAnimSetGoalWeightOnly =
                reinterpret_cast<XAnimSetGoalWeight_t>(addresses.XAnimSetGoalWeightOnly());
            drawn.lastWeights.resize(count, 0.0f);
            const bool anyAdditive = std::any_of(weights.begin() + static_cast<std::ptrdiff_t>(normal), weights.end(),
                                                 [](float weight) { return weight > 0.0f; });
            const auto additiveNode = FIRST_CLIP_INDEX + static_cast<std::uint32_t>(normal);
            const auto nodes = TreeNodes(drawn.tree);
            for (std::size_t position = 0; position < count; ++position)
                if (const auto index = EntryOf(position, normal); weights[position] > 0.0f && !nodes.contains(index))
                    XAnimSetGoalWeight(drawn.obj, index, weights[position], 0.0f, 0.0f, 0, 0, 0, -1);
            if (anyAdditive && !nodes.contains(additiveNode))
                XAnimSetGoalWeight(drawn.obj, additiveNode, 1.0f, 0.0f, 0.0f, 0, 0, 0, -1);
            XAnimSetGoalWeightOnly(drawn.obj, ROOT_INDEX, 1.0f, 0.0f, 0.0f, 0, 0, 0, -1);
            for (std::size_t position = 0; position < count; ++position)
            {
                const auto index = EntryOf(position, normal);
                if (weights[position] > 0.0f || drawn.lastWeights[position] > 0.0f)
                    XAnimSetGoalWeightOnly(drawn.obj, index, weights[position], 0.0f, 0.0f, 0, 0, 0, -1);
                if (weights[position] > 0.0f)
                    XAnimSetTime(drawn.tree, index, times[position], -1);
            }
            if (normal < count)
                XAnimSetGoalWeightOnly(drawn.obj, additiveNode, anyAdditive ? 1.0f : 0.0f, 0.0f, 0.0f, 0, 0, 0, -1);
            drawn.lastWeights = std::move(weights);
        }

        // a handle no DObj of the game's has, and no prop's
        std::optional<std::int32_t> FindFreeHandle()
        {
            for (std::int32_t handle = TOP_HANDLE; handle > TOP_HANDLE - MAX_HANDLES; --handle)
            {
                const bool taken = std::ranges::any_of(drawnModels, [&](const auto& entry) {
                    return entry.second.handle == handle;
                });
                if (!taken && !GetClientDObj(handle) && GetEntity(handle))
                    return handle;
            }
            return std::nullopt;
        }

        // Draws models (the first the body, the others sharing its bones) at a place, their DObj made the first time
        // (and again once the game has freed it, as it frees the client's DObjs with a map)
        void Draw(std::uint32_t key, const std::vector<std::string>& names, glm::vec3 origin, glm::vec3 angles,
                  float scale, glm::vec3 lightingOffset = glm::vec3(0.0f, 0.0f, 32.0f),
                  const ActorLogic::TreeWeights* shown = nullptr, const std::vector<std::string>* upcoming = nullptr)
        {
            // the animations it shows: those its tree has, else the tree made again with them too
            std::vector<std::string> animations;
            if (shown)
            {
                for (const auto& entry : shown->animations)
                {
                    const auto& name = entry.clip ? entry.clip->animation : entry.animation;
                    if (animationParts.contains(name) && std::ranges::find(animations, name) == animations.end())
                        animations.push_back(name);
                }
            }
            auto found = drawnModels.find(key);
            const auto lacks = [&](const Drawn& drawn) {
                return std::ranges::any_of(animations, [&](const std::string& name) {
                    return std::ranges::find(drawn.animations, name) == drawn.animations.end();
                });
            };
            if (found != drawnModels.end() && shown && lacks(found->second))
            {
                // (those it had as well, so the tree isn't made again each time the actor goes between two; of those
                // not its clips', as the browser's previews, the latest few only: a tree of every one hovered grew to
                // the most, and each made again slower)
                constexpr std::size_t MAX_KEPT_OTHERS = 6;
                std::size_t others = 0;
                for (const auto& name : found->second.animations)
                {
                    if (animations.size() >= MAX_ANIMATIONS || std::ranges::find(animations, name) != animations.end())
                        continue;
                    const bool clips = upcoming && std::ranges::find(*upcoming, name) != upcoming->end();
                    if (!clips && ++others > MAX_KEPT_OTHERS)
                        continue;
                    animations.push_back(name);
                }
            }
            if (found != drawnModels.end() &&
                (found->second.models != names || GetClientDObj(found->second.handle) != found->second.obj ||
                 (shown && lacks(found->second)) || (!shown && found->second.tree)))
            {
                Retire(found->second);
                drawnModels.erase(found);
                found = drawnModels.end();
            }
            if (found == drawnModels.end())
            {
                if (names.empty() || names.size() > 48)
                    return;
                // (one of another map's stays out: the catalog says, without going through the assets each frame)
                const auto catalog = Components::Actors::GetCatalog();
                if (!catalog)
                    return;
                // (models not loaded, tried before: not looked for each frame)
                if (const auto failed = failedModels.find(key); failed != failedModels.end() && failed->second == names)
                    return;
                failedModels[key] = names;
                std::array<DObjModel, 16> models{};
                std::uint32_t bones = 0;
                std::uint16_t count = 0;
                // the DObj's model of each entry (none for one left off), and the tags to hide after
                std::vector<std::int32_t> made(names.size(), -1);
                std::vector<std::uint32_t> firstBone(models.size(), 0);
                std::vector<std::pair<std::uint16_t, std::size_t>> hides;
                struct CamoEntry
                {
                    const std::uint8_t* camo;
                    std::int32_t set;
                    std::size_t entry;
                };
                std::vector<CamoEntry> camos;
                typedef std::uint32_t(__cdecl * SL_GetString_t)(const char* text);
                const auto SL_GetString = reinterpret_cast<SL_GetString_t>(GetGameAddresses().SL_GetString());
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    // ("~camo^set^entry": a camo's set on an entry's model, the camo its WeaponCamo)
                    if (names[i].starts_with('~'))
                    {
                        std::vector<std::string> fields;
                        for (std::size_t from = 1;;)
                        {
                            const auto caret = names[i].find('^', from);
                            fields.push_back(names[i].substr(from, caret == std::string::npos ? caret : caret - from));
                            if (caret == std::string::npos)
                                break;
                            from = caret + 1;
                        }
                        if (fields.size() == 3)
                            camos.push_back({reinterpret_cast<const std::uint8_t*>(std::stoull(fields[0])),
                                             std::stoi(fields[1]), std::stoul(fields[2])});
                        continue;
                    }
                    // ("!tag^entry": a tag of an entry's model hidden, the tag's script string)
                    if (names[i].starts_with('!'))
                    {
                        const auto caret = names[i].find('^');
                        if (caret != std::string::npos)
                            hides.emplace_back(static_cast<std::uint16_t>(std::stoul(names[i].substr(1, caret - 1))),
                                               std::stoul(names[i].substr(caret + 1)));
                        continue;
                    }
                    if (count >= models.size())
                        break;
                    // ("model@bone": one held on a bone of the first, as a gun in the right hand; "model@bone^entry
                    // ^x^y^z^pitch^yaw^roll" on a bone of another entry's model, moved and turned from it)
                    const auto at = names[i].find('@');
                    const auto modelName = names[i].substr(0, at);
                    if (!std::ranges::binary_search(catalog->props, modelName))
                    {
                        if (i == 0)
                            return;
                        continue;
                    }
                    const auto model = FindModel(modelName);
                    if (!model)
                    {
                        if (i == 0)
                            return;
                        continue;
                    }
                    // (the game counts every model's bones, shared or not: those past its most are left off)
                    if (bones + CountBones(model) > MAX_DOBJ_BONES)
                    {
                        if (i == 0)
                            return;
                        LOG_WARN("T6: {} left off {}: {} bones would be more than the game's {}", names[i],
                                 names.front(), bones + CountBones(model), MAX_DOBJ_BONES);
                        continue;
                    }
                    models[count] = {};
                    models[count].model = model;
                    if (at != std::string::npos && i > 0)
                    {
                        std::vector<std::string> fields;
                        for (std::size_t from = at + 1;;)
                        {
                            const auto caret = names[i].find('^', from);
                            fields.push_back(names[i].substr(from, caret == std::string::npos ? caret : caret - from));
                            if (caret == std::string::npos)
                                break;
                            from = caret + 1;
                        }
                        models[count].boneName = static_cast<std::uint16_t>(SL_GetString(fields[0].c_str()) & 0xFFFF);
                        if (fields.size() == 8)
                        {
                            const auto parent = std::stoul(fields[1]);
                            if (parent >= made.size() || made[parent] < 0)
                                continue;
                            models[count].parentModel = static_cast<std::uint16_t>(made[parent]);
                            for (std::size_t axis = 0; axis < 3; ++axis)
                            {
                                models[count].position[axis] = std::stof(fields[2 + axis]);
                                models[count].angles[axis] = std::stof(fields[5 + axis]);
                            }
                        }
                    }
                    made[i] = count;
                    firstBone[count] = bones;
                    ++count;
                    bones += CountBones(model);
                }
                const auto handle = FindFreeHandle();
                if (!handle)
                    return;
                failedModels.erase(key);

                // (a person's tree of its animations: XAnimCreateTree, 0x5F3530)
                typedef void*(__cdecl * XAnimCreateTree_t)(void* anims, Alloc_t alloc);
                typedef void(__cdecl * XAnimFreeTree_t)(void* tree, Free_t free);
                // (and those of its other clips, so a clip starting doesn't make the tree again then: an event at
                // its start, as a death's hit, had no bones to go off at)
                for (std::size_t i = 0; upcoming && i < upcoming->size() && animations.size() < MAX_ANIMATIONS; ++i)
                    if (animationParts.contains((*upcoming)[i]) &&
                        std::ranges::find(animations, (*upcoming)[i]) == animations.end())
                        animations.push_back((*upcoming)[i]);
                if (animations.size() > MAX_ANIMATIONS)
                    animations.resize(MAX_ANIMATIONS);
                // (the additive ones last, under their node)
                std::ranges::stable_partition(animations, [](const std::string& name) { return !IsAdditive(name); });
                std::uint8_t* anims = nullptr;
                void* tree = nullptr;
                if (shown && !animations.empty())
                {
                    anims = CreateAnimations(animations, CountNormal(animations));
                    tree = anims ? reinterpret_cast<XAnimCreateTree_t>(GetGameAddresses().XAnimCreateTree())(anims, Alloc)
                                 : nullptr;
                    if (!tree)
                    {
                        std::free(anims);
                        anims = nullptr;
                        animations.clear();
                    }
                }
                else
                    animations.clear();

                typedef void*(__cdecl * Com_ClientDObjCreate_t)(DObjModel * models, std::uint16_t numModels, void* tree,
                                                               int handle, int localClientNum);
                const auto obj = reinterpret_cast<Com_ClientDObjCreate_t>(GetGameAddresses().Com_ClientDObjCreate())(
                    models.data(), count, tree, *handle, 0);
                if (!obj)
                {
                    if (tree)
                        reinterpret_cast<XAnimFreeTree_t>(GetGameAddresses().XAnimFreeTree())(tree, Free);
                    std::free(anims);
                    return;
                }
                const auto entity = GetEntity(*handle);
                std::memset(entity, 0, ENTITY_SIZE);
                *reinterpret_cast<std::int32_t*>(entity + ENTITY_NUMBER) = *handle;
                // (the tags hidden: bits of the DObj's hidePartBits, 0x5C, by the bones of its models one after another)
                for (const auto& [tag, entry] : hides)
                {
                    if (entry >= made.size() || made[entry] < 0)
                        continue;
                    const auto modelIndex = static_cast<std::size_t>(made[entry]);
                    const auto model = static_cast<const std::uint8_t*>(models[modelIndex].model);
                    const auto boneNames = *reinterpret_cast<const std::uint16_t* const*>(model + 8);
                    const auto modelBones = CountBones(model);
                    for (std::uint32_t b = 0; boneNames && b < modelBones; ++b)
                    {
                        const auto bone = firstBone[modelIndex] + b;
                        if (boneNames[b] == tag && bone < 160)
                            reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(obj) + 0x5C)[bone / 32] |=
                                0x80000000u >> (bone % 32);
                    }
                }
                found = drawnModels.emplace(key, Drawn{names, *handle, obj}).first;
                // (the camos, a mask of the DObj's models each)
                for (const auto& camo : camos)
                {
                    if (camo.entry >= made.size() || made[camo.entry] < 0)
                        continue;
                    const auto mask = 1u << made[camo.entry];
                    const auto same = std::ranges::find_if(found->second.camos, [&](const Drawn::Camo& c) {
                        return c.camo == camo.camo && c.set == camo.set;
                    });
                    if (same != found->second.camos.end())
                        same->mask |= mask;
                    else
                        found->second.camos.push_back({camo.camo, camo.set, mask});
                }
                found->second.animations = animations;
                // (what bones its animations move, the first time each is in a tree: for working out the upper body
                // ones' legs)
                for (const auto& name : animations)
                {
                    static std::set<std::string> described;
                    if (!described.insert(name).second)
                        continue;
                    const auto parts = animationParts.at(name);
                    const auto boneNames = *reinterpret_cast<const std::uint16_t* const*>(parts + 0x40);
                    const auto count = *(parts + 0x18 + 9);
                    typedef std::uint32_t(__cdecl * SL_FindString_t)(const char* text);
                    const auto SL_FindString = reinterpret_cast<SL_FindString_t>(GetGameAddresses().SL_FindString());
                    std::string has;
                    for (const auto bone : {"j_mainroot", "pelvis", "torso_stabilizer", "j_spinelower", "j_hip_le",
                                            "j_knee_le", "j_ankle_le", "j_neck", "j_wrist_ri", "tag_origin"})
                    {
                        const auto id = static_cast<std::uint16_t>(SL_FindString(bone) & 0xFFFF);
                        const bool found = boneNames && id && std::find(boneNames, boneNames + count, id) !=
                                                                  boneNames + count;
                        has += std::format(" {}{}", found ? "+" : "-", bone);
                    }
                    std::array<std::uint8_t, 10> counts{};
                    std::memcpy(counts.data(), parts + 0x18, counts.size());
                    LOG_DEBUG("T6: {} moves {} bones ({} {} {} {} {} {} {} {} {}),{}; {} frames, asset type {}", name,
                              count, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6],
                              counts[7], counts[8], has, *reinterpret_cast<const std::uint16_t*>(parts + 0xE),
                              *(parts + 0x23));
                }
                found->second.anims = anims;
                found->second.tree = tree;
                // (how the moving ones move the root through a play, the first time one is in a tree)
                for (std::size_t i = 0; anims && i < animations.size(); ++i)
                {
                    const auto parts = animationParts.at(animations[i]);
                    if (*(parts + PARTS_DELTA) && !IsAdditive(animations[i]) &&
                        !Components::Actors::GetAnimationRootMotion(animations[i]))
                        Components::Actors::SetAnimationRootMotion(
                            animations[i], MeasureRootMotion(anims, EntryOf(i, CountNormal(animations)), parts));
                }
                LOG_DEBUG("T6: made {} ({} bones in all) on handle {:#x}, {} animations", names.front(), bones,
                          *handle, animations.size());
            }

            auto& drawn = found->second;
            const auto entity = GetEntity(drawn.handle);
            if (!entity)
                return;
            std::memcpy(entity + POSE_ORIGIN, &origin.x, sizeof(float) * 3);
            std::memcpy(entity + POSE_ANGLES, &angles.x, sizeof(float) * 3);
            if (shown && drawn.tree)
                Animate(drawn, *shown);

            // as the client's general entities are (0x7A1DD0): lit from a little above where it stands
            typedef void(__cdecl * R_AddDObjToScene_t)(void* obj, void* pose, int entnum, std::uint32_t renderFlags,
                                                       const float* lightingOrigin, const float* a, const float* b,
                                                       int c, int d, void* e, float f, float scale, int g);
            const glm::vec3 lighting = origin + lightingOffset;
            const float zeros[2] = {};
            // (its camos, for this frame: the images where a camo goes replaced by the set's, and the pattern's offset
            // and scale, as the client does a player's gun's)
            std::int32_t overrides = -1;
            for (const auto& camo : drawn.camos)
            {
                const auto sets = *reinterpret_cast<const std::uint8_t* const*>(camo.camo + CAMO_SETS);
                const auto setCount = *reinterpret_cast<const std::uint32_t*>(camo.camo + CAMO_SET_COUNT);
                if (!sets || camo.set < 0 || static_cast<std::uint32_t>(camo.set) >= setCount)
                    continue;
                const auto set = sets + camo.set * CAMO_SET_SIZE;
                typedef bool(__cdecl * R_AddImageOverride_t)(std::uint32_t mask, const void* from, const void* to,
                                                            std::int32_t* slot);
                typedef bool(__cdecl * R_AddConstOverride_t)(std::uint32_t mask, std::uint32_t code, float x, float y,
                                                            float z, float w, std::int32_t* slot);
                const auto imageOverride = reinterpret_cast<R_AddImageOverride_t>(GetGameAddresses().R_AddImageOverride());
                const auto constOverride = reinterpret_cast<R_AddConstOverride_t>(GetGameAddresses().R_AddConstOverride());
                const auto image = [](const std::uint8_t* at) {
                    return *reinterpret_cast<const std::uint8_t* const*>(at);
                };
                // (a camo's images are streamed from the disk as the game uses them: one not in memory yet, its
                // texture none, isn't put on, as the renderer would wait for it and hung the game)
                const auto resident = [](const std::uint8_t* gfxImage) {
                    return gfxImage && *reinterpret_cast<const void* const*>(gfxImage) != nullptr;
                };
                const auto solid = image(set), pattern = image(set + 4);
                const bool ready = (!solid || resident(solid)) && (!pattern || resident(pattern));
                static std::set<std::pair<const std::uint8_t*, std::int32_t>> logged;
                if (logged.insert({camo.camo, camo.set}).second)
                    LOG_DEBUG("T6: camo set {} on models {:#x}: solid {} ({}), pattern {} ({}){}", camo.set, camo.mask,
                              solid ? *reinterpret_cast<const char* const*>(solid + IMAGE_NAME) : "none",
                              resident(solid) ? "in memory" : "not in memory",
                              pattern ? *reinterpret_cast<const char* const*>(pattern + IMAGE_NAME) : "none",
                              resident(pattern) ? "in memory" : "not in memory", ready ? "" : "; left off");
                if (!ready)
                    continue;
                if (solid && image(camo.camo + CAMO_SOLID_BASE))
                    imageOverride(camo.mask, image(camo.camo + CAMO_SOLID_BASE), solid, &overrides);
                if (pattern && image(camo.camo + CAMO_PATTERN_BASE))
                    imageOverride(camo.mask, image(camo.camo + CAMO_PATTERN_BASE), pattern, &overrides);
                const auto values = reinterpret_cast<const float*>(set + 8);
                constOverride(camo.mask, 0xB5, values[0], values[1], values[2], 1.0f, &overrides);
            }
            reinterpret_cast<R_AddDObjToScene_t>(GetGameAddresses().R_AddDObjToScene())(
                drawn.obj, entity, drawn.handle, 0, &lighting.x, &zeros[0], &zeros[1], -1, overrides, nullptr, 0.0f,
                std::max(scale, 0.01f), 0);
            drawn.drawn = true;

            if (!loggedFirst)
            {
                loggedFirst = true;
                LOG_DEBUG("T6: drawing {} at ({:.0f}, {:.0f}, {:.0f})", names.front(), origin.x, origin.y, origin.z);
            }
        }
    }  // namespace

    namespace
    {
        // The packs: zones of the game's loaded for more characters and their animations (multiplayer's other factions,
        // the campaign's common animations), queued for the game's database thread (QueueZone) as the film plays on.
        // Nothing is freed: a pack loaded stays, with the others, until the game closes.
        struct PackInfo
        {
            const char* zone;
            const char* name;
            const char* group;
        };
        constexpr std::array<PackInfo, 7> PACKS = {{
            {"faction_seals_mp", "Navy SEALs", "Multiplayer"},
            {"faction_pmc_mp", "Mercenaries", "Multiplayer"},
            {"faction_pla_mp", "PLA", "Multiplayer"},
            {"faction_isa_mp", "ISA", "Multiplayer"},
            {"faction_cd_mp", "Cordis Die", "Multiplayer"},
            {"faction_fbi_mp", "FBI", "Multiplayer"},
            {"faction_multiteam_mp", "Multi-team", "Multiplayer"},
        }};
        std::mutex packMutex;
        bool packedNamed = false;  // the campaign animations' bones' script strings made for this film
        std::string requestedPack = "\x01";  // under packMutex; \x01 for none asked for
        std::string loadedPack, loadingPack, packError;
        int loadingFrames = 0;
        std::size_t countBefore = 0;
        // the assets registered as a pack loads: it's loaded once they've grown and stopped growing, or not after
        // PACK_WAIT_FRAMES (a big zone loads over many frames)
        std::size_t assetsBefore = 0, assetsSeen = 0;
        int steadyChecks = 0;
        constexpr int PACK_CHECK_FRAMES = 15, PACK_WAIT_FRAMES = 1800;

        std::size_t CountAssets();

        void BuildCatalog();

        // whether the game has a zone of that name loaded (or one it's loading), by its list of them
        bool IsZoneLoaded(std::string_view zone)
        {
            const auto count = reinterpret_cast<const std::int32_t*>(GetGameAddresses().g_zoneCount());
            const auto flags = reinterpret_cast<const std::uint8_t*>(GetGameAddresses().g_zoneFlags());
            if (!count || !flags || *count <= 0 || *count > 64)
                return false;
            for (std::int32_t i = 0; i < *count; ++i)
            {
                const auto name = reinterpret_cast<const char*>(flags + i * 0x48 + 4);
                if (std::string_view(name, strnlen(name, 64)) == zone)
                    return true;
            }
            return false;
        }

        struct XZoneInfo
        {
            const char* name;
            std::int32_t allocFlags;
            std::int32_t freeFlags;
        };
        typedef void(__cdecl * DB_LoadXAssets_t)(XZoneInfo * zoneInfo, std::uint32_t count, std::int32_t sync,
                                                 std::int32_t);
        DB_LoadXAssets_t DB_LoadXAssets_Trampoline = nullptr;
        typedef void(__cdecl * DB_QueueZones_t)(XZoneInfo * zoneInfo, std::uint32_t count);
        DB_QueueZones_t DB_QueueZones_Trampoline = nullptr;

        // The pack loaded with each film: the game loads it with the film's map, as it loads the map's zones (a
        // zone loaded as a film plays hung the game, or stopped the film), and frees it with the map. Kept in a
        // file in the game's folder, for the films after a restart too.
        std::mutex filmPackMutex;
        std::string filmPack;  // under filmPackMutex
    }  // namespace

    namespace
    {
        // A pack whose load never finished hung the game (the campaign's did, as a film played): its name is written
        // to a file in the game's folder as it starts loading, and the file goes once it's loaded. Found there as
        // the game starts again, that pack is left out from then on, kept in a second file.
        std::filesystem::path GameFolder()
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            return std::filesystem::path(path).parent_path();
        }
        std::filesystem::path PendingFile()
        {
            return GameFolder() / "T6iris-pack-loading-with-film.txt";
        }
        std::filesystem::path FrozeFile()
        {
            return GameFolder() / "T6iris-packs-that-broke-a-film.txt";
        }

        std::vector<std::string> ReadLines(const std::filesystem::path& path)
        {
            std::vector<std::string> lines;
            std::ifstream file(path);
            for (std::string line; std::getline(file, line);)
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (!line.empty())
                    lines.push_back(line);
            }
            return lines;
        }

        // the packs that froze the game, read once (and the one that was loading as it last closed added)
        const std::vector<std::string>& FrozenPacks()
        {
            static const auto packs = [] {
                std::error_code error;
                auto frozen = ReadLines(FrozeFile());
                for (const auto& pack : ReadLines(PendingFile()))
                {
                    LOG_WARN("T6: the pack {} was loading with a film as the game last closed; it's left out from now "
                             "on (delete {} to try it again)",
                             pack, FrozeFile().string());
                    if (std::ranges::find(frozen, pack) == frozen.end())
                    {
                        frozen.push_back(pack);
                        std::ofstream(FrozeFile(), std::ios::app) << pack << "\n";
                    }
                }
                std::filesystem::remove(PendingFile(), error);
                return frozen;
            }();
            return packs;
        }

        std::filesystem::path FilmPackFile()
        {
            return GameFolder() / "T6iris-pack.txt";
        }

        void SaveFilmPack(const std::string& pack)
        {
            std::error_code error;
            if (pack.empty())
                std::filesystem::remove(FilmPackFile(), error);
            else
                std::ofstream(FilmPackFile(), std::ios::trunc) << pack << "\n";
        }

        void MarkLoading(const std::string& pack)
        {
            std::error_code error;
            if (pack.empty())
                std::filesystem::remove(PendingFile(), error);
            else
                std::ofstream(PendingFile(), std::ios::trunc) << pack << "\n";
        }
    }  // namespace

    namespace
    {
        // The zones the database is handed (DB_QueueZones, hooked): with the film's map (the zone of an mp_ map, not
        // its loading screen's or patch's) comes the film's pack, allocated as the map is so the game frees it with
        // the map
        void __cdecl DB_QueueZones_Hook(XZoneInfo* zoneInfo, std::uint32_t count)
        {
            const XZoneInfo* level = nullptr;
            std::string names;
            for (std::uint32_t i = 0; zoneInfo && i < count && i < 32; ++i)
            {
                const std::string_view name = zoneInfo[i].name ? zoneInfo[i].name : "";
                names += std::format(" {}({:#x})", name.empty() ? "-" : name, zoneInfo[i].allocFlags);
                if (name.starts_with("mp_") && name.find("_load") == std::string_view::npos &&
                    name.find("_patch") == std::string_view::npos)
                    level = &zoneInfo[i];
            }
            LOG_DEBUG("T6: the database is handed zones{}", names);
            std::string pack;
            {
                std::lock_guard lock(filmPackMutex);
                pack = filmPack;
            }
            if (level && !pack.empty() && std::ranges::find(FrozenPacks(), pack) == FrozenPacks().end() &&
                !IsZoneLoaded(pack) && count < 16)
            {
                std::vector<XZoneInfo> zones(zoneInfo, zoneInfo + count);
                zones.push_back({pack.c_str(), level->allocFlags, 0});
                LOG_INFO("T6: the pack {} loads with the map {} (allocated {:#x})", pack, level->name,
                         level->allocFlags);
                MarkLoading(pack);
                // (the names are copied as it's handed them)
                DB_QueueZones_Trampoline(zones.data(), static_cast<std::uint32_t>(zones.size()));
                return;
            }
            DB_QueueZones_Trampoline(zoneInfo, count);
        }

        void __cdecl DB_LoadXAssets_Hook(XZoneInfo* zoneInfo, std::uint32_t count, std::int32_t sync, std::int32_t unknown)
        {
            std::string names;
            for (std::uint32_t i = 0; zoneInfo && i < count && i < 32; ++i)
                names += std::format(" {}({:#x}, free {:#x})", zoneInfo[i].name ? zoneInfo[i].name : "-",
                                     zoneInfo[i].allocFlags, zoneInfo[i].freeFlags);
            LOG_DEBUG("T6: the game loads zones{}{}", names, sync ? " (synced)" : "");
            DB_LoadXAssets_Trampoline(zoneInfo, count, sync, unknown);
        }
    }  // namespace

    namespace
    {
        // A multi-pass capture's elements (Components::Rendering's flags, as the pass recorded or looked at sets
        // them): what's drawn of the DObjs, the players (the clients' entities, 0 to 17, and the actors) or the rest
        // (the props, the map's script models and vehicles), and the world's geometry and effects, by their dvars
        enum class Drawing
        {
            Game,
            Person,
            Prop,
        };
        Drawing drawing = Drawing::Game;
        constexpr int MAX_CLIENTS = 18;
        typedef void(__cdecl * R_AddDObjToSceneHooked_t)(void* obj, void* pose, int entnum, std::uint32_t renderFlags,
                                                          const float* lightingOrigin, const float* a, const float* b,
                                                          int c, int d, void* e, float f, float scale, int g);
        R_AddDObjToSceneHooked_t R_AddDObjToScene_Trampoline = nullptr;
        void __cdecl R_AddDObjToScene_Hook(void* obj, void* pose, int entnum, std::uint32_t renderFlags,
                                           const float* lightingOrigin, const float* a, const float* b, int c, int d,
                                           void* e, float f, float scale, int g)
        {
            const auto flags = Components::Rendering::GetRenderingFlags();
            // (for finding why a pass shows what it shouldn't: what it draws and leaves out, by entity, every 2 s)
            static std::map<int, std::pair<int, int>> seen;  // entnum: drawn, left out
            static ULONGLONG lastReport = 0;
            bool left = false;
            const auto report = [&] {
                auto& counts = seen[drawing == Drawing::Game ? entnum : (drawing == Drawing::Person ? -100 : -200)];
                (left ? counts.second : counts.first)++;
                if (const auto now = GetTickCount64(); now - lastReport > 2000)
                {
                    lastReport = now;
                    std::string text;
                    for (const auto& [number, count] : seen)
                        text += std::format(" {}:{}/{}", number == -100 ? std::string("actors")
                                                         : number == -200 ? std::string("props")
                                                                          : std::to_string(number),
                                            count.first, count.second);
                    LOG_DEBUG("T6: the pass (flags {:#x}, keeps {:#x}) drew/left out of the DObjs, by entity:{}",
                              static_cast<int>(flags), Components::Rendering::GetGreenScreenKeeps(), text);
                    seen.clear();
                }
            };
            if (flags != Types::RenderingFlags_DrawEverything)
            {
                const bool player = drawing == Drawing::Game && entnum >= 0 && entnum < MAX_CLIENTS;
                const bool person = drawing == Drawing::Person || player;
                // (a Green screen's: the players, actors and props it keeps, each its own)
                if (flags == Types::RenderingFlags_DrawPlayers)
                {
                    const auto keeps = Components::Rendering::GetGreenScreenKeeps();
                    const auto kind = player                        ? Components::Rendering::GreenScreen_Players
                                      : drawing == Drawing::Person ? Components::Rendering::GreenScreen_Actors
                                      : drawing == Drawing::Prop   ? Components::Rendering::GreenScreen_Props
                                                                   : 0u;
                    // (a player's other DObj than its own body is the gun it holds)
                    const bool gun = player && obj != GetClientDObj(entnum);
                    if (!(keeps & kind) || (gun && !(keeps & Components::Rendering::GreenScreen_Guns)))
                    {
                        left = true;
                        report();
                        return;
                    }
                }
                else if (!(flags & (person ? Types::RenderingFlags_DrawPlayers : Types::RenderingFlags_DrawWorld)))
                {
                    left = true;
                    report();
                    return;
                }
                report();
            }
            R_AddDObjToScene_Trampoline(obj, pose, entnum, renderFlags, lightingOrigin, a, b, c, d, e, f, scale, g);
        }

        // A call (5 bytes, E8) skipped by a short jump over it (EB 03), or put back: written as one 16-bit store, so a
        // render worker running past it sees one or the other. Only where the bytes are the game's (t6mpv43).
        void SkipCall(std::uintptr_t at, const std::array<std::uint8_t, 5>& original, bool skip)
        {
            auto* code = reinterpret_cast<std::uint8_t*>(at);
            const bool skipped = code[0] == 0xEB && code[1] == 0x03;
            if ((skipped ? std::memcmp(code + 2, original.data() + 2, 3) : std::memcmp(code, original.data(), 5)) != 0)
            {
                LOG_WARN("T6: the call at {:#x} isn't the game's as known, so it's left as it is", at);
                return;
            }
            if (skipped == skip)
                return;
            DWORD protection = 0;
            if (!VirtualProtect(code, 2, PAGE_EXECUTE_READWRITE, &protection))
                return;
            const auto value = static_cast<SHORT>(skip ? 0x03EB : (original[0] | (original[1] << 8)));
            InterlockedExchange16(reinterpret_cast<volatile SHORT*>(code), value);
            VirtualProtect(code, 2, protection, &protection);
            FlushInstructionCache(GetCurrentProcess(), code, 2);
        }

        // The map's geometry and static models left out of the scene, as the release game has no dvars for it: the
        // render jobs r_add_bsp (0x753360) and r_add_staticmodel (0x7534b0) skip the calls that add their surfaces
        // (0x76E3F0, 0x74DEC0), still closing their (empty) draw lists. Out of the depth too, so a Green screen's green
        // (from the depth) takes their place.
        void LeaveOutWorld(bool off)
        {
            SkipCall(0x7533A3, {0xE8, 0x48, 0xB0, 0x01, 0x00}, off);
            SkipCall(0x7534F1, {0xE8, 0xCA, 0xA9, 0xFF, 0xFF}, off);
        }

        // the world's geometry and effects off while a pass shows the players alone, as they were after
        void ApplyPassElements()
        {
            static bool worldOff = false;
            static std::map<std::string, float> saved;
            const auto flags = Components::Rendering::GetRenderingFlags();
            // (a Green screen that keeps the world: drawn)
            const bool off = flags != Types::RenderingFlags_DrawEverything && !(flags & Types::RenderingFlags_DrawWorld) &&
                             !(Components::Rendering::GetGreenScreenKeeps() & Components::Rendering::GreenScreen_World);
            if (off == worldOff)
                return;
            worldOff = off;
            // (the release game hasn't the draw_lit ones or fx_draw: what adds the map's geometry and static models to
            // the scene, r_add_*, leaves them out instead)
            for (const auto name : {"r_draw_lit_bsp", "r_draw_lit_static_model", "r_add_bsp", "r_add_staticmodel",
                                    "r_drawWater", "fx_draw", "fx_marks_draw"})
            {
                if (off)
                {
                    const auto value = Functions::GetDvarNumber(name);
                    if (!value)
                    {
                        LOG_DEBUG("T6: no {} dvar for a players' pass", name);
                        continue;
                    }
                    LOG_INFO("T6: {} off for a players' pass (was {})", name, *value);
                    saved[name] = *value;
                    Functions::SetDvarNumber(name, 0.0f);
                }
                else if (const auto was = saved.find(name); was != saved.end())
                {
                    Functions::SetDvarNumber(name, was->second);
                }
            }
            if (!off)
                saved.clear();
            LeaveOutWorld(off);
            LOG_INFO("T6: the world's {} for the pass", off ? "left out" : "drawn again");
        }
    }  // namespace

    void Install()
    {
        // (the killstreaks are Black Ops 2's: their models, speeds and looks)
        Components::KillstreakLogic::UseBlackOps2();
        HookManager::CreateHook(GetGameAddresses().R_AddDObjToScene(), reinterpret_cast<std::uintptr_t>(R_AddDObjToScene_Hook),
                                reinterpret_cast<std::uintptr_t*>(&R_AddDObjToScene_Trampoline));
        {
            std::lock_guard lock(filmPackMutex);
            const auto saved = ReadLines(FilmPackFile());
            filmPack = saved.empty() ? std::string() : saved.front();
            // (one left out since, as the campaign's zone: its animations come from the animation pack now)
            if (std::ranges::none_of(PACKS, [&](const auto& pack) { return pack.zone == filmPack; }))
                filmPack.clear();
            // (no pack loads with films for now: More characters... is left out)
            filmPack.clear();
        }
        LOG_INFO("T6: the pack loaded with films: {}", filmPack.empty() ? "none" : filmPack);
        HookManager::CreateHook(GetGameAddresses().DB_LoadXAssets(), reinterpret_cast<std::uintptr_t>(DB_LoadXAssets_Hook),
                                reinterpret_cast<std::uintptr_t*>(&DB_LoadXAssets_Trampoline));
        HookManager::CreateHook(GetGameAddresses().DB_QueueZones(), reinterpret_cast<std::uintptr_t>(DB_QueueZones_Hook),
                                reinterpret_cast<std::uintptr_t*>(&DB_QueueZones_Trampoline));
    }

    void LoadPack(const std::string& zone)
    {
        LOG_INFO("T6: the pack {} is asked for, but packs are left out for now", zone);
    }

    void Update()
    {
        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            return;
        // a pack asked for: the one the next film loads with its map (this film's, if it's loaded already)
        std::string wanted;
        bool asked = false;
        {
            std::lock_guard lock(packMutex);
            if (requestedPack != "\x01")
            {
                wanted = std::exchange(requestedPack, "\x01");
                asked = true;
            }
        }
        if (!asked)
            return;
        {
            std::lock_guard lock(filmPackMutex);
            if (wanted == filmPack && (wanted.empty() || wanted == loadedPack))
                return;
            filmPack = wanted;
        }
        SaveFilmPack(wanted);
        if (wanted.empty())
        {
            LOG_INFO("T6: no pack with films from now on");
            packError.clear();
        }
        else if (std::ranges::find(FrozenPacks(), wanted) != FrozenPacks().end())
        {
            LOG_WARN("T6: the pack {} broke a film before, so it's not loaded", wanted);
            packError = std::format("{} broke a film before, so it's left out", wanted);
        }
        else if (IsZoneLoaded(wanted))
        {
            LOG_INFO("T6: the pack {} is loaded already", wanted);
            loadedPack = wanted;
            packError.clear();
        }
        else
        {
            LOG_INFO("T6: the pack {} loads with the next film", wanted);
            packError = "Load the film again to load these animations";
        }
        BuildCatalog();
    }

    namespace
    {
        void ForgetSceneEffects();
        void ForgetKillstreakEffects();
    }

    void OnFilmStarted()
    {
        ForgetSceneEffects();
        // (the effects went with the film before)
        ForgetKillstreakEffects();
        // (the game freed the DObjs with the last map)
        drawnModels.clear();
        retired.clear();
        failedModels.clear();
        sceneGeneration.reset();
        actorModels.clear();
        loggedFirst = false;
        loadingPack.clear();
        packBusy = false;
        // (a pack that was loading as the game last closed: found now; this film's loaded, if it did)
        FrozenPacks();
        MarkLoading({});
        packedNamed = false;
        {
            const auto load = reinterpret_cast<const std::uint8_t*>(GetGameAddresses().DB_LoadXAssets());
            const auto queue = reinterpret_cast<const std::uint8_t*>(GetGameAddresses().DB_QueueZones());
            LOG_DEBUG("T6: DB_LoadXAssets starts {:02x} {:02x} {:02x} {:02x} {:02x}, DB_QueueZones {:02x} {:02x} {:02x} "
                      "{:02x} {:02x}",
                      load[0], load[1], load[2], load[3], load[4], queue[0], queue[1], queue[2], queue[3], queue[4]);
        }
        std::string pack;
        {
            std::lock_guard lock(filmPackMutex);
            pack = filmPack;
        }
        loadedPack.clear();
        packError.clear();
        if (!pack.empty() && IsZoneLoaded(pack))
        {
            loadedPack = pack;
            LOG_INFO("T6: the pack {} loaded with the film", pack);
        }
        else if (!pack.empty() && std::ranges::find(FrozenPacks(), pack) == FrozenPacks().end())
        {
            packError = std::format("{} didn't load with the film", pack);
            LOG_WARN("T6: the pack {} didn't load with the film", pack);
        }
        BuildCatalog();
    }

    namespace
    {
        // The campaign's animations, which the multiplayer game can't load from their zones (common.ff hung a film's
        // loading): written from the zones on disk to an animation pack by t6/tools/animations (see its
        // t6xanimpack.cpp for the format), read here once, and rebuilt in memory as the game's own XAnimParts (0x68
        // bytes: the layout at the top of this file) for actors to play as they play those the game has loaded.
        struct PackedAnimation
        {
            std::string name;
            std::uint8_t* parts = nullptr;
            std::vector<std::string> bones;  // the names' script strings, made again for each film
            std::vector<std::unique_ptr<std::uint8_t[]>> memory;
        };
        std::vector<PackedAnimation> packedAnimations;
        bool packedRead = false;

        std::filesystem::path PackedAnimationsFile()
        {
            return GameFolder() / "T6iris-campaign-animations.bin";
        }

        struct PackReader
        {
            const std::uint8_t* position;
            const std::uint8_t* end;
            bool ok = true;

            const std::uint8_t* Take(std::size_t size)
            {
                if (!ok || static_cast<std::size_t>(end - position) < size)
                {
                    ok = false;
                    return nullptr;
                }
                const auto taken = position;
                position += size;
                return taken;
            }
            template <typename T>
            T Get()
            {
                T value{};
                if (const auto bytes = Take(sizeof(T)))
                    std::memcpy(&value, bytes, sizeof(T));
                return value;
            }
            std::string String()
            {
                const auto size = Get<std::uint16_t>();
                const auto bytes = Take(size);
                return bytes ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string();
            }
        };

        // memory of the animation's, zeroed (the source's bytes copied in, if any)
        std::uint8_t* Allocate(PackedAnimation& animation, std::size_t size, const std::uint8_t* source = nullptr,
                               std::size_t copied = 0)
        {
            auto block = std::make_unique<std::uint8_t[]>(std::max<std::size_t>(size, 4));
            std::memset(block.get(), 0, std::max<std::size_t>(size, 4));
            if (source && copied)
                std::memcpy(block.get(), source, std::min(size, copied));
            const auto pointer = block.get();
            animation.memory.push_back(std::move(block));
            return pointer;
        }

        template <typename T>
        void Store(std::uint8_t* at, std::size_t offset, T value)
        {
            std::memcpy(at + offset, &value, sizeof(T));
        }

        // an array of the file's in the animation's memory, none for none
        std::uint8_t* TakeArray(PackReader& reader, PackedAnimation& animation, std::size_t size)
        {
            const auto bytes = reader.Take(size);
            return bytes && size ? Allocate(animation, size, bytes, size) : nullptr;
        }

        // the delta part (0x64): XAnimDeltaPart {trans, quat2, quat}, each {u16 size, (trans: u8 smallTrans), at +4
        // frame0 when its size is 0, else (trans: mins, size) the frames' pointer and the indices after it}
        std::uint8_t* ReadDelta(PackReader& reader, PackedAnimation& animation, bool byteIndices)
        {
            const auto delta = Allocate(animation, 12);
            const std::size_t indexSize = byteIndices ? 1 : 2;
            if (reader.Get<std::uint8_t>())
            {
                const auto size = reader.Get<std::uint16_t>();
                const auto smallTrans = reader.Get<std::uint8_t>();
                std::uint8_t* trans;
                if (size == 0)
                {
                    trans = Allocate(animation, 16);
                    if (const auto frame = reader.Take(12))
                        std::memcpy(trans + 4, frame, 12);
                }
                else
                {
                    const std::size_t count = size + 1u;
                    trans = Allocate(animation, 32 + count * indexSize);
                    if (const auto bounds = reader.Take(24))
                        std::memcpy(trans + 4, bounds, 24);
                    if (const auto indices = reader.Take(count * indexSize))
                        std::memcpy(trans + 32, indices, count * indexSize);
                    Store(trans, 28, TakeArray(reader, animation, count * 3 * (smallTrans ? 1 : 2)));
                }
                Store(trans, 0, size);
                Store(trans, 2, smallTrans);
                Store(delta, 0, trans);
            }
            for (const std::size_t components : {std::size_t{2}, std::size_t{4}})
            {
                if (!reader.Get<std::uint8_t>())
                    continue;
                const auto size = reader.Get<std::uint16_t>();
                std::uint8_t* quat;
                if (size == 0)
                {
                    quat = Allocate(animation, 4 + components * 2);
                    if (const auto frame = reader.Take(components * 2))
                        std::memcpy(quat + 4, frame, components * 2);
                }
                else
                {
                    const std::size_t count = size + 1u;
                    quat = Allocate(animation, 8 + count * indexSize);
                    if (const auto indices = reader.Take(count * indexSize))
                        std::memcpy(quat + 8, indices, count * indexSize);
                    Store(quat, 4, TakeArray(reader, animation, count * components * 2));
                }
                Store(quat, 0, size);
                Store(delta, components == 2 ? 4 : 8, quat);
            }
            return delta;
        }

        void ReadPackedAnimations()
        {
            if (std::exchange(packedRead, true))
                return;
            std::ifstream file(PackedAnimationsFile(), std::ios::binary);
            if (!file)
            {
                LOG_INFO("T6: no campaign animations ({} isn't there)", PackedAnimationsFile().string());
                return;
            }
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            PackReader reader{bytes.data(), bytes.data() + bytes.size()};
            const auto magic = reader.Take(8);
            if (!magic || std::memcmp(magic, "IRISXA01", 8) != 0)
            {
                LOG_WARN("T6: {} isn't an animation pack", PackedAnimationsFile().string());
                return;
            }
            const auto count = reader.Get<std::uint32_t>();
            for (std::uint32_t i = 0; i < count && reader.ok; ++i)
            {
                PackedAnimation animation;
                animation.name = reader.String();
                const auto parts = Allocate(animation, PARTS_SIZE);
                const auto name = Allocate(animation, animation.name.size() + 1,
                                           reinterpret_cast<const std::uint8_t*>(animation.name.c_str()),
                                           animation.name.size());
                Store(parts, 0, name);
                const auto dataByteCount = reader.Get<std::uint16_t>();
                const auto dataShortCount = reader.Get<std::uint16_t>();
                const auto dataIntCount = reader.Get<std::uint16_t>();
                const auto randomDataByteCount = reader.Get<std::uint16_t>();
                const auto randomDataIntCount = reader.Get<std::uint16_t>();
                const auto numFrames = reader.Get<std::uint16_t>();
                for (const auto& [offset, value] : {std::pair{4, dataByteCount}, {6, dataShortCount}, {8, dataIntCount},
                                                   {0xA, randomDataByteCount}, {0xC, randomDataIntCount},
                                                   {0xE, numFrames}})
                    Store(parts, offset, value);
                for (std::size_t flag = 0; flag < 4; ++flag)
                    Store(parts, 0x10 + flag, reader.Get<std::uint8_t>());
                reader.Get<std::uint32_t>();  // (streamed from the disk: not here, the data is in memory)
                std::array<std::uint8_t, 10> boneCount{};
                for (auto& bones : boneCount)
                    bones = reader.Get<std::uint8_t>();
                std::memcpy(parts + 0x18, boneCount.data(), boneCount.size());
                const auto notifyCount = reader.Get<std::uint8_t>();
                Store(parts, 0x23, reader.Get<std::uint8_t>());  // asset type
                reader.Get<std::uint8_t>();                        // (a default's)
                const auto randomDataShortCount = reader.Get<std::uint32_t>();
                const auto indexCount = reader.Get<std::uint32_t>();
                Store(parts, 0x28, randomDataShortCount);
                Store(parts, 0x2C, indexCount);
                for (std::size_t value = 0; value < 4; ++value)
                    Store(parts, 0x30 + value * 4, reader.Get<float>());
                // (the bones' names as text, their script strings made for each film; the notetracks left out)
                for (std::uint32_t bone = 0; bone < boneCount[PART_TYPE_ALL]; ++bone)
                    animation.bones.push_back(reader.String());
                Store(parts, 0x40, Allocate(animation, animation.bones.size() * 2));
                for (std::uint32_t note = 0; note < notifyCount; ++note)
                {
                    reader.String();
                    reader.Get<float>();
                }
                Store(parts, 0x44, TakeArray(reader, animation, dataByteCount));
                Store(parts, 0x48, TakeArray(reader, animation, dataShortCount * 2u));
                Store(parts, 0x4C, TakeArray(reader, animation, dataIntCount * 4u));
                Store(parts, 0x50, TakeArray(reader, animation, randomDataShortCount * 2u));
                Store(parts, 0x54, TakeArray(reader, animation, randomDataByteCount));
                Store(parts, 0x58, TakeArray(reader, animation, randomDataIntCount * 4u));
                Store(parts, 0x5C, TakeArray(reader, animation, indexCount * (numFrames < 256 ? 1u : 2u)));
                if (reader.Get<std::uint8_t>())
                    Store(parts, 0x64, ReadDelta(reader, animation, numFrames < 256));
                animation.parts = parts;
                if (reader.ok)
                    packedAnimations.push_back(std::move(animation));
            }
            if (!reader.ok)
                LOG_WARN("T6: {} ends early: {} of its {} animations read", PackedAnimationsFile().string(),
                         packedAnimations.size(), count);
            LOG_INFO("T6: {} campaign animations read from {}", packedAnimations.size(),
                     PackedAnimationsFile().string());
        }

        // the bones' script strings, made once a film (the game's are made again with each map)
        void NamePackedAnimations()
        {
            if (std::exchange(packedNamed, true) || packedAnimations.empty())
                return;
            typedef std::uint32_t(__cdecl * SL_GetString_t)(const char* text);
            const auto SL_GetString = reinterpret_cast<SL_GetString_t>(GetGameAddresses().SL_GetString());
            std::map<std::string, std::uint16_t, std::less<>> made;
            for (auto& animation : packedAnimations)
            {
                const auto names = *reinterpret_cast<std::uint16_t**>(animation.parts + 0x40);
                for (std::size_t i = 0; i < animation.bones.size(); ++i)
                {
                    // (the gun's bones the campaign's AI animations key, as its bipod's, which folded the legs of an
                    // actor's gun out at odd angles: given a name no model has, so the gun keeps its own pose)
                    const std::string_view bone = animation.bones[i];
                    const bool gunsBone = bone == "tag_bipod" || bone.starts_with("tag_bipod_") ||
                                          bone == "tag_clip" || bone == "tag_scope";
                    const std::string name = gunsBone ? "iris_unused_" + std::string(bone) : std::string(bone);
                    auto found = made.find(name);
                    if (found == made.end())
                        found = made.emplace(name, static_cast<std::uint16_t>(SL_GetString(name.c_str()) & 0xFFFF))
                                    .first;
                    names[i] = found->second;
                }
            }
            LOG_INFO("T6: the campaign animations' {} bone names made for this film", made.size());
        }
    }  // namespace

    namespace
    {
        // The weapons' parts, from the game's weapon assets (WeaponVariantDef, 25; WeaponAttachment, 29;
        // WeaponAttachmentUnique, 30; offsets as OpenAssetTools' T6 structures lay them out on 32 bits): a gun's world
        // model shows every part of its attachments (both grips of a gun with one, say) until the tags its variant
        // hides are hidden, and an attachment brings a model of its own and hides or shows tags of the gun's
        constexpr std::int32_t ASSET_TYPE_WEAPON = 25;
        constexpr std::size_t VARIANT_WEAPDEF = 8, VARIANT_ATTACHMENTS = 24, VARIANT_UNIQUES = 28,
                              VARIANT_HIDE_TAGS = 36, VARIANT_ATTACH_WORLD_MODEL = 44, VARIANT_ATTACH_WORLD_TAG = 52,
                              VARIANT_ATTACH_WORLD_OFFSETS = 152, VARIANT_ATTACH_WORLD_ROTATIONS = 344,
                              WEAPDEF_WORLD_MODEL = 916;
        constexpr std::size_t UNIQUE_CAMO = 164, UNIQUE_DISABLE_BASE_ATTACHMENT = 168, UNIQUE_DISABLE_BASE_CLIP = 169;
        constexpr std::size_t UNIQUE_HIDE_TAGS = 36, UNIQUE_WORLD_MODEL = 52, UNIQUE_WORLD_MODEL_ADDITIONAL = 56,
                              UNIQUE_WORLD_MODEL_TAG = 64, UNIQUE_OFFSETS = 80, UNIQUE_ROTATIONS = 104,
                              UNIQUE_ADD_OFFSETS = 128, UNIQUE_ADD_ROTATIONS = 152;
        constexpr std::size_t ATTACHMENT_POINT = 12;
        constexpr std::size_t MAX_HIDE_TAGS = 32, MAX_ATTACHMENTS = 63;

        struct WeaponAttachmentInfo
        {
            std::string name;  // acog
            std::int32_t point = 0;
            const std::uint8_t* unique = nullptr;  // the gun's own of it
        };
        // a model of its own a gun has on it, as a sniper's scope or a magazine
        struct WeaponPart
        {
            std::string model, tag;
            std::array<float, 3> offsets{}, rotations{};
            bool clip = false;
        };
        struct WeaponInfo
        {
            std::vector<std::uint16_t> hideTags;  // what it hides with no attachment
            std::vector<WeaponAttachmentInfo> attachments;
            std::vector<WeaponPart> parts;
            const std::uint8_t* camo = nullptr;  // its WeaponCamo
            const std::uint8_t* def = nullptr;   // its WeaponDef
            int rank = INT_MAX;                  // of the variant it's read from (see VariantRank)
        };
        std::map<std::string, WeaponInfo, std::less<>> weaponInfo;  // by world model, on the game's thread
        // the effects by name, from the game's FX assets (33), for actors' events
        constexpr std::int32_t ASSET_TYPE_FX = 33;
        std::map<std::string, const void*, std::less<>> effectDefs;
        // the catalog's camos' sets (its camo 0 none, -1)
        std::vector<std::int32_t> camoSetIndex;

        std::vector<std::uint16_t> ReadTags(const std::uint8_t* at)
        {
            std::vector<std::uint16_t> tags;
            const auto list = at ? *reinterpret_cast<const std::uint16_t* const*>(at) : nullptr;
            for (std::size_t i = 0; list && i < MAX_HIDE_TAGS; ++i)
                if (list[i])
                    tags.push_back(list[i]);
            return tags;
        }

        // a weapon's world flash effect, the sound it fires and its casing's effect
        constexpr std::size_t WEAPDEF_WORLD_FLASH = 0x70, WEAPDEF_FIRE_SOUND = 0xB0, WEAPDEF_WORLD_SHELL = 0x1D4;
        // every weapon's definition by its variant's name (the killstreaks' too, which have no world model)
        std::map<std::string, const std::uint8_t*, std::less<>> weaponDefs;

        void ReadWeapons()
        {
            weaponInfo.clear();
            weaponDefs.clear();
            ForEachAsset([&](const XAssetEntry& entry) {
                if (entry.type != ASSET_TYPE_WEAPON)
                    return;
                const auto variant = static_cast<const std::uint8_t*>(entry.header);
                const auto name = GetAssetName(entry);
                if (!name)
                    return;
                const auto def = *reinterpret_cast<const std::uint8_t* const*>(variant + VARIANT_WEAPDEF);
                if (def)
                    weaponDefs.emplace(name, def);
                const auto attachments = *reinterpret_cast<const std::uint8_t* const* const*>(variant + VARIANT_ATTACHMENTS);
                const auto uniques = *reinterpret_cast<const std::uint8_t* const* const*>(variant + VARIANT_UNIQUES);
                if (!def || !attachments || !uniques)
                    return;
                const auto worldModels = *reinterpret_cast<const std::uint8_t* const* const*>(def + WEAPDEF_WORLD_MODEL);
                const auto worldModel = worldModels ? worldModels[0] : nullptr;
                const auto modelName = worldModel ? *reinterpret_cast<const char* const*>(worldModel) : nullptr;
                if (!modelName)
                    return;
                // (the gun's own multiplayer variant, dsr50_mp: not one of its alternate modes' (sf_, dualoptic_,
                // gl_), nor the campaign's or another mode's from a pack, whose parts and hidden tags differ (they
                // put a stray scope or sight on it), whichever the game happens to have first)
                const std::string_view variantName(name);
                const bool mp = variantName.ends_with("_mp");
                const bool alternate = variantName.find('+') != std::string_view::npos ||
                                       std::ranges::any_of(std::array<std::string_view, 8>{"sf_", "dualoptic_", "gl_",
                                                                                           "ft_", "mk_", "is_", "dw_",
                                                                                           "upgraded_"},
                                                           [&](std::string_view prefix) {
                                                               return variantName.starts_with(prefix);
                                                           });
                const int rank = mp ? (alternate ? 1 : 0) : (alternate ? 3 : 2);
                auto& info = weaponInfo[modelName];
                if (!info.attachments.empty() && info.rank <= rank)
                    return;
                info = WeaponInfo{};
                info.rank = rank;
                info.hideTags = ReadTags(variant + VARIANT_HIDE_TAGS);
                info.def = def;
                // (and what it hides with no attachment, as a SCAR-H its sights when off)
                if (const auto none = uniques[0])
                {
                    for (const auto tag : ReadTags(none + UNIQUE_HIDE_TAGS))
                        if (std::ranges::find(info.hideTags, tag) == info.hideTags.end())
                            info.hideTags.push_back(tag);
                    info.camo = *reinterpret_cast<const std::uint8_t* const*>(none + UNIQUE_CAMO);
                }
                // its models of its own (scopes, magazines)
                const auto partModels = *reinterpret_cast<const std::uint8_t* const* const*>(variant + VARIANT_ATTACH_WORLD_MODEL);
                const auto partTags = *reinterpret_cast<const char* const* const*>(variant + VARIANT_ATTACH_WORLD_TAG);
                for (std::size_t i = 0; partModels && i < 8; ++i)
                {
                    const auto partName = partModels[i] ? *reinterpret_cast<const char* const*>(partModels[i]) : nullptr;
                    if (!partName)
                        continue;
                    WeaponPart part{.model = partName,
                                    .tag = partTags && partTags[i] && *partTags[i] ? partTags[i] : "tag_weapon"};
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        part.offsets[axis] = reinterpret_cast<const float*>(variant + VARIANT_ATTACH_WORLD_OFFSETS)[i * 3 + axis];
                        part.rotations[axis] = reinterpret_cast<const float*>(variant + VARIANT_ATTACH_WORLD_ROTATIONS)[i * 3 + axis];
                    }
                    part.clip = part.model.find("_mag_") != std::string::npos || part.model.find("clip") != std::string::npos;
                    info.parts.push_back(std::move(part));
                }
                for (std::size_t type = 1; type < MAX_ATTACHMENTS; ++type)
                {
                    const auto attachment = attachments[type];
                    const auto unique = uniques[type];
                    if (!attachment || !unique)
                        continue;
                    const auto attachmentName = *reinterpret_cast<const char* const*>(attachment);
                    if (!attachmentName || *attachmentName == ',')
                        continue;
                    info.attachments.push_back({.name = attachmentName,
                                                .point = *reinterpret_cast<const std::int32_t*>(attachment + ATTACHMENT_POINT),
                                                .unique = unique});
                }
            });
            std::erase_if(weaponInfo, [](const auto& entry) { return entry.second.attachments.empty(); });
            LOG_INFO("T6: {} guns' attachments read", weaponInfo.size());
        }

        constexpr std::array<std::pair<std::string_view, std::string_view>, 30> ATTACHMENT_NAMES{{
            {"acog", "ACOG"}, {"acog_sniper", "ACOG"}, {"reflex", "Reflex"}, {"holo", "EOTech"},
            {"dualoptic", "Hybrid Optic"}, {"mms", "Millimeter Scanner"}, {"rangefinder", "Target Finder"},
            {"ir", "Dual Band"}, {"vzoom", "Variable Zoom"}, {"is", "Iron Sights"},
            {"silencer", "Suppressor"}, {"silencer_smg", "Suppressor"}, {"extbarrel", "Long Barrel"},
            {"grip", "Foregrip"}, {"gl", "Grenade Launcher"}, {"tacknife", "Tactical Knife"},
            {"extclip", "Extended Clip"}, {"extclip_sniper", "Extended Clip"}, {"dualclip", "Fast Mag"},
            {"fmj", "FMJ"}, {"steadyaim", "Laser Sight"}, {"steadyaim_smg", "Laser Sight"},
            {"steadyaim_sniper", "Laser Sight"}, {"fastads", "Quickdraw"}, {"fastads_smg", "Quickdraw"},
            {"stalker", "Stock"}, {"stalker_smg", "Stock"}, {"sf", "Select Fire"}, {"rf", "Rapid Fire"},
            {"swayreduc", "Ballistics CPU"},
        }};

        std::string AttachmentDisplayName(std::string_view name)
        {
            const auto named = std::ranges::find(ATTACHMENT_NAMES, name, &std::pair<std::string_view, std::string_view>::first);
            if (named != ATTACHMENT_NAMES.end())
                return std::string(named->second);
            std::string display(name);
            for (auto& c : display)
                c = c == '_' ? ' ' : c;
            if (!display.empty())
                display[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(display[0])));
            return display;
        }

        // The entries Draw takes for a gun on a tag of the body (models[0]) with its attachments: "model@tag", its
        // attachments' "model@tag^parent^x^y^z^pitch^yaw^roll" on it, and "!tag^model" for each of its tags hidden
        void AddWeapon(std::vector<std::string>& models, const std::string& gun, const std::vector<std::string>& chosen,
                       std::int32_t camo, const char* tag)
        {
            const auto gunIndex = models.size();
            models.push_back(gun + "@" + tag);
            // (the camo's set of the gun's WeaponCamo, by the catalog's camo)
            const std::int32_t set =
                camo > 0 && static_cast<std::size_t>(camo) < camoSetIndex.size() ? camoSetIndex[camo] : -1;
            const auto info = weaponInfo.find(gun);
            if (info == weaponInfo.end())
                return;
            // (its camo on it, and on the parts of its attachments that have the gun's camo)
            if (set >= 0 && info->second.camo)
                models.push_back(std::format("~{}^{}^{}", reinterpret_cast<std::uintptr_t>(info->second.camo), set, gunIndex));
            // what's hidden: its variant's, with what its attachments hide too and less what they show
            std::set<std::uint16_t> base(info->second.hideTags.begin(), info->second.hideTags.end()), hidden = base,
                shown;
            bool withoutBase = false, withoutClip = false;
            for (const auto& name : chosen)
            {
                const auto attachment =
                    std::ranges::find(info->second.attachments, name, &WeaponAttachmentInfo::name);
                if (attachment == info->second.attachments.end())
                    continue;
                const auto unique = attachment->unique;
                withoutBase |= unique[UNIQUE_DISABLE_BASE_ATTACHMENT] != 0;
                withoutClip |= unique[UNIQUE_DISABLE_BASE_CLIP] != 0;
                const bool gunsCamo = *reinterpret_cast<const std::uint8_t* const*>(unique + UNIQUE_CAMO) == info->second.camo;
                const auto tags = ReadTags(unique + UNIQUE_HIDE_TAGS);
                const std::set<std::uint16_t> own(tags.begin(), tags.end());
                for (const auto t : own)
                    hidden.insert(t);
                for (const auto t : base)
                    if (!own.contains(t))
                        shown.insert(t);
                // its models, on the gun's tag_weapon (or the tag it names) where it says
                const auto modelTag = *reinterpret_cast<const char* const*>(unique + UNIQUE_WORLD_MODEL_TAG);
                const std::string on = modelTag && *modelTag ? modelTag : "tag_weapon";
                for (const auto [model, offsets, rotations] :
                     {std::tuple{UNIQUE_WORLD_MODEL, UNIQUE_OFFSETS, UNIQUE_ROTATIONS},
                      std::tuple{UNIQUE_WORLD_MODEL_ADDITIONAL, UNIQUE_ADD_OFFSETS, UNIQUE_ADD_ROTATIONS}})
                {
                    const auto xmodel = *reinterpret_cast<const std::uint8_t* const*>(unique + model);
                    const auto modelName = xmodel ? *reinterpret_cast<const char* const*>(xmodel) : nullptr;
                    if (!modelName)
                        continue;
                    const auto o = reinterpret_cast<const float*>(unique + offsets);
                    const auto r = reinterpret_cast<const float*>(unique + rotations);
                    models.push_back(std::format("{}@{}^{}^{}^{}^{}^{}^{}^{}", modelName, on, gunIndex, o[0], o[1], o[2],
                                                 r[0], r[1], r[2]));
                    if (set >= 0 && gunsCamo && info->second.camo)
                        models.push_back(std::format("~{}^{}^{}", reinterpret_cast<std::uintptr_t>(info->second.camo),
                                                     set, models.size() - 1));
                }
            }
            // its own models (a sniper's scope, unless an optic takes its place; its magazine, unless one does)
            for (const auto& part : info->second.parts)
            {
                if (part.clip ? withoutClip : withoutBase)
                    continue;
                models.push_back(std::format("{}@{}^{}^{}^{}^{}^{}^{}^{}", part.model, part.tag, gunIndex, part.offsets[0],
                                             part.offsets[1], part.offsets[2], part.rotations[0], part.rotations[1],
                                             part.rotations[2]));
                if (set >= 0 && info->second.camo)
                    models.push_back(std::format("~{}^{}^{}", reinterpret_cast<std::uintptr_t>(info->second.camo), set,
                                                 models.size() - 1));
            }
            for (const auto t : hidden)
                if (!shown.contains(t))
                    models.push_back(std::format("!{}^{}", t, gunIndex));
        }
    }  // namespace

    namespace
    {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 20> CAMO_NAMES{{
            {"devgru", "DEVGRU"}, {"atacs", "A-TACS AU"}, {"erdl", "ERDL"}, {"siberia", "Siberia"},
            {"choco", "Choco"}, {"tiger_blue", "Blue Tiger"}, {"bloodshot", "Bloodshot"},
            {"ghostex_delta6", "Ghostex: Delta 6"}, {"kryptek_typhon", "Kryptek: Typhon"}, {"blossom", "Cherry Blossom"},
            {"artofwar", "Art of War"}, {"ronin", "Ronin"}, {"skulls", "Skulls"}, {"bo2collectors", "CE Digital"},
            {"elite", "Elite Member"},
            {"sahara", "Sahara"}, {"urban_russia", "Urban Russia"}, {"flecktarn", "Flecktarn"}, {"flora", "Flora"},
            {"tiger_jungle", "Jungle Tiger"},
        }};

        std::string CamoDisplayName(const std::string& name)
        {
            const auto named = std::ranges::find(CAMO_NAMES, name, &std::pair<std::string_view, std::string_view>::first);
            if (named != CAMO_NAMES.end())
                return std::string(named->second);
            std::string display = name;
            for (auto& c : display)
                c = c == '_' ? ' ' : c;
            if (!display.empty())
                display[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(display[0])));
            return display;
        }

        // a swatch's colour, near the camo's main one
        std::uint32_t CamoColor(const std::string& display)
        {
            constexpr std::array<std::pair<std::string_view, std::uint32_t>, 14> COLORS{{
                {"DEVGRU", 0x6B6B5A}, {"A-TACS", 0x9A8A6A}, {"ERDL", 0x5E6B3C}, {"Siberia", 0xC8CCD0},
                {"Choco", 0x8A6A44}, {"Blue Tiger", 0x2E4A7A}, {"Bloodshot", 0x8A1E1E}, {"Ghostex", 0x6E7266},
                {"Kryptek", 0x5A5F5A}, {"Tiger", 0x6A7A3A}, {"Nevada", 0xB49A6E}, {"Sahara", 0xC8A878},
                {"Urban", 0x7A7A80}, {"Flecktarn", 0x4E5A3A},
            }};
            for (const auto& [name, color] : COLORS)
                if (display.find(name) != std::string::npos)
                    return color;
            return 0x7A7A7A;
        }

        // The guns actors hold: the world models of the weapons the game has loaded (t6_wpn_<kind>_<gun>_world, not
        // their attachments' or scopes' pieces), as a loadout catalog of weapons named by those models, without
        // attachments or camos
        std::shared_ptr<const Types::LoadoutCatalog> MakeWeaponCatalog(const std::set<std::string>& models)
        {
            struct Kind
            {
                std::string_view prefix, category;
                bool primary;
            };
            constexpr std::array<Kind, 9> KINDS{{
                {"ar_", "Assault Rifles", true},
                {"smg_", "SMGs", true},
                {"lmg_", "LMGs", true},
                {"sniper_", "Sniper Rifles", true},
                {"shotty_", "Shotguns", true},
                {"pistol_", "Pistols", false},
                {"launch_", "Launchers", false},
                {"knife_", "Specials", false},
                {"", "Specials", false},
            }};
            constexpr std::array<std::pair<std::string_view, std::string_view>, 52> NAMES{{
                {"ar_an94", "AN-94"}, {"ar_hk416", "M27"}, {"ar_sa58", "FAL OSW"}, {"ar_saritch", "SMR"},
                {"ar_scarh", "SCAR-H"}, {"ar_sig556", "SWAT-556"}, {"ar_type95", "Type 25"}, {"ar_x95l", "MTAR"},
                {"ar_xm8", "M8A1"}, {"ar_galil", "Galil"}, {"ar_m16", "M16"}, {"ar_fal", "FAL"}, {"ar_peacekeeper", "Peacekeeper"},
                {"smg_chicom", "Chicom CQB"}, {"smg_mp7", "MP7"}, {"smg_msmc", "MSMC"}, {"smg_pdw57", "PDW-57"},
                {"smg_scorpion", "Skorpion EVO"}, {"smg_vector", "Vector K10"}, {"smg_ak74u", "AK-74u"},
                {"lmg_hamr", "HAMR"}, {"lmg_lsat", "LSAT"}, {"lmg_mk48", "Mk 48"}, {"lmg_type95", "QBB LSW"},
                {"lmg_hk21", "HK21"}, {"sniper_ballista", "Ballista"}, {"sniper_dsr50", "DSR 50"},
                {"sniper_svu", "SVU-AS"}, {"sniper_xpr50", "XPR-50"}, {"sniper_as50", "AS50"},
                {"shotty_870mcs", "R870 MCS"}, {"shotty_ksg", "KSG"}, {"shotty_saiga", "S12"},
                {"shotty_srm1216", "M1216"}, {"pistol_b2023r", "B23R"}, {"pistol_fiveseven", "Five-seven"},
                {"pistol_fnp45", "Tac-45"}, {"pistol_judge", "Executioner"}, {"pistol_kard", "KAP-40"},
                {"launch_fhj18", "FHJ-18 AA"}, {"launch_m32", "War Machine"}, {"launch_smaw", "SMAW"},
                {"launch_usrpg", "RPG"}, {"knife_base", "Knife"}, {"ballistic_knife", "Ballistic Knife"},
                {"crossbow", "Crossbow"}, {"minigun", "Death Machine"}, {"shield_carry", "Assault Shield"},
                {"hatchet", "Combat Axe"}, {"tablet", "Tablet"}, {"pda", "PDA"}, {"uav_radio", "Radio"},
            }};
            auto catalog = std::make_shared<Types::LoadoutCatalog>();
            for (const auto& model : models)
            {
                std::string_view name(model);
                if (!name.starts_with("t6_wpn_") || !name.ends_with("_world"))
                    continue;
                const auto gun = name.substr(7, name.size() - 7 - 6);
                // (not the pieces: scopes, grips, blades; nor what's thrown or placed)
                if (gun.find("_scope") != std::string_view::npos || gun.find("_grip") != std::string_view::npos ||
                    gun.find("_blade") != std::string_view::npos || gun.starts_with("grenade_") ||
                    gun.find("mine") != std::string_view::npos || gun == "c4" || gun == "claymore" ||
                    gun == "bouncing_betty" || gun == "tac_insert" || gun.starts_with("trophy") ||
                    gun.starts_with("motion_sensor") || gun.starts_with("turret") || gun == "briefcase_bomb" ||
                    gun == "shield_pickup" || gun == "shield_stow")
                    continue;
                const auto kind = std::ranges::find_if(KINDS, [&](const Kind& k) { return gun.starts_with(k.prefix); });
                const auto named = std::ranges::find(NAMES, gun, &std::pair<std::string_view, std::string_view>::first);
                std::string display;
                if (named != NAMES.end())
                    display = named->second;
                else
                {
                    // (one of a DLC's: its name as the model has it, upper case)
                    const auto start = kind->prefix.empty() ? std::size_t{0} : kind->prefix.size();
                    for (const auto c : gun.substr(start))
                        display += c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                catalog->weapons.push_back({.name = model,
                                            .displayName = display,
                                            .category = std::string(kind->category),
                                            .primary = kind->primary});
                catalog->variants.push_back({.name = model, .weapon = model});
                // (its attachments, one or two at a time, not two on the same place of the gun)
                const auto info = weaponInfo.find(model);
                if (info == weaponInfo.end())
                    continue;
                const auto& attachments = info->second.attachments;
                for (std::size_t a = 0; a < attachments.size(); ++a)
                {
                    if (std::ranges::none_of(catalog->attachments,
                                             [&](const auto& known) { return known.name == attachments[a].name; }))
                        catalog->attachments.push_back(
                            {.name = attachments[a].name, .displayName = AttachmentDisplayName(attachments[a].name)});
                    catalog->variants.push_back(
                        {.name = model + "+" + attachments[a].name, .weapon = model, .attachments = {attachments[a].name}});
                    for (std::size_t b = a + 1; b < attachments.size(); ++b)
                    {
                        if (attachments[a].point == attachments[b].point && attachments[a].point != 0)
                            continue;
                        std::vector<std::string> pair{attachments[a].name, attachments[b].name};
                        std::ranges::sort(pair);
                        catalog->variants.push_back(
                            {.name = model + "+" + pair[0] + "+" + pair[1], .weapon = model, .attachments = pair});
                    }
                }
            }
            // the camos: the sets of a gun's WeaponCamo (every gun's have them in the same order), by their solid
            // image's name (t6_camo_<name>_solid), those without images left out; camo 0 none
            camoSetIndex = {-1};
            catalog->camos.push_back({.displayName = "None", .color = 0x5A5A5A});
            if (const auto first = std::ranges::find_if(weaponInfo, [](const auto& entry) { return entry.second.camo; });
                first != weaponInfo.end())
            {
                const auto camo = first->second.camo;
                const auto sets = *reinterpret_cast<const std::uint8_t* const*>(camo + CAMO_SETS);
                const auto count = *reinterpret_cast<const std::uint32_t*>(camo + CAMO_SET_COUNT);
                for (std::uint32_t set = 0; sets && set < count && set < 64; ++set)
                {
                    const auto solid = *reinterpret_cast<const std::uint8_t* const*>(sets + set * CAMO_SET_SIZE);
                    const auto pattern = *reinterpret_cast<const std::uint8_t* const*>(sets + set * CAMO_SET_SIZE + 4);
                    const auto named = pattern ? pattern : solid;
                    const auto imageName = named ? *reinterpret_cast<const char* const*>(named + IMAGE_NAME) : nullptr;
                    if (!imageName)
                        continue;
                    std::string display(imageName);
                    for (const auto part : {"t6_camo_", "_pattern", "_solid"})
                        if (const auto at = display.find(part); at != std::string::npos)
                            display.erase(at, std::string_view(part).size());
                    display = CamoDisplayName(display);
                    camoSetIndex.push_back(static_cast<std::int32_t>(set));
                    // (GfxImage's texture at +0, streamed: null until the game loads it)
                    catalog->camos.push_back({.displayName = display,
                                              .color = CamoColor(display),
                                              .texture = reinterpret_cast<void* const*>(named)});
                }
            }
            // by category in the kinds' order, then by name
            const auto order = [&](const Types::LoadoutCatalog::Weapon& weapon) {
                return std::ranges::find(KINDS, weapon.category, [](const Kind& k) { return std::string(k.category); }) -
                       KINDS.begin();
            };
            std::ranges::sort(catalog->weapons, [&](const auto& a, const auto& b) {
                return std::pair(order(a), a.displayName) < std::pair(order(b), b.displayName);
            });
            return catalog;
        }
    }  // namespace

    namespace
    {
    std::size_t CountAssets()
    {
        std::size_t count = 0;
        ForEachAsset([&](const XAssetEntry&) { ++count; });
        return count;
    }

    namespace
    {
        // What of the killstreaks and dogs the game has loaded, once a film: their models (and which of the tags
        // Black Ops' killstreaks are posed and lit by they have), effects and animations, by the names they go by, for
        // giving Black Ops 2's their own wheels, guns and effects
        void LogKillstreakAssets(const std::set<std::string>& names,
                                 const decltype(modelPointers)& pointers)
        {
            static std::set<std::string> logged;
            const auto matches = [](std::string_view name, std::span<const char* const> parts) {
                return std::ranges::any_of(parts,
                                           [&](const char* part) { return name.find(part) != std::string_view::npos; });
            };
            constexpr std::array MODEL_PARTS{"drone", "turret", "supply_drop", "shepherd", "rcxd", "rcbomb", "ai_tank",
                                             "killstreak", "veh_t6"};
            constexpr std::array FX_PARTS{"rcbomb", "rc_car", "rcxd", "drone", "turret", "supply", "sentry", "microwave",
                                          "ai_tank", "agr", "killstreak", "dog", "guardian", "carepackage", "care_package",
                                          "heli", "vtol", "a10", "f38", "fa38", "jet", "plane", "uav", "afterburner",
                                          "exhaust", "contrail", "rotor", "pegasus", "overwatch", "qr_", "warthog",
                                          "lodestar", "straferun", "vehicle/light", "vehicle/props", "geotrail",
                                          "muzzleflashes/fx_heavy", "muzzleflashes/fx_muz_lg", "chopper", "osprey"};
            constexpr std::array ANIM_PARTS{"shepherd", "dog", "drone", "turret", "tank", "rcxd", "rcbomb", "sentry",
                                            "supply", "crate", "microwave", "guardian", "veh_"};
            constexpr std::array TAGS{
                "tag_origin",        "tag_body",          "tag_turret",          "tag_barrel",        "tag_flash",
                "tag_aim",           "tag_aim_pitch",     "tag_spin",            "tag_player",        "tag_driver",
                "tag_gunner",        "tag_fx",            "tag_light",           "tag_rotor",         "tag_rotor_fl",
                "tag_rotor_fr",      "tag_rotor_rl",      "tag_rotor_rr",        "tag_wheel_front_left",
                "tag_wheel_front_right", "tag_wheel_back_left", "tag_wheel_back_right", "left_wheel_01_jnt",
                "left_wheel_02_jnt", "right_wheel_01_jnt", "right_wheel_02_jnt", "tag_flash_gunner1", "tag_rocket1",
                "tag_rocket2",       "tag_missile_left",  "tag_missile_right",   "tag_turret_pitch",  "tag_turret_yaw",
                "tag_barrel_spin",   "tag_lid",           "tag_cargo",           "tag_engine_left",   "tag_engine_right",
                "tag_headlight_left", "tag_headlight_right", "tag_light_front",  "tag_light_back",    "tag_treads_left",
                "tag_treads_right",  "tag_antenna",       "tag_camera",          "tag_dish",          "tag_ads"};
            typedef std::uint32_t(__cdecl * SL_FindString_t)(const char* text);
            const auto SL_FindString = reinterpret_cast<SL_FindString_t>(GetGameAddresses().SL_FindString());
            for (const auto& name : names)
            {
                if (!matches(name, MODEL_PARTS) || !logged.insert("model " + name).second)
                    continue;
                std::string has;
                if (const auto found = pointers.find(name); found != pointers.end())
                {
                    const auto model = found->second;
                    const auto boneNames = *reinterpret_cast<const std::uint16_t* const*>(model + 8);
                    const auto count = CountBones(model);
                    for (const auto tag : TAGS)
                    {
                        const auto id = static_cast<std::uint16_t>(SL_FindString(tag) & 0xFFFF);
                        if (id && boneNames && std::find(boneNames, boneNames + count, id) != boneNames + count)
                            has += std::format(" {}", tag);
                    }
                    has = std::format("{} bones;{}", count, has.empty() ? " none of the tags" : has);
                    // (every bone by name, of the killstreaks', from the script strings' text)
                    const auto buffer = *reinterpret_cast<const std::uint8_t* const*>(GetGameAddresses().scrStrings());
                    if (buffer && boneNames && name.starts_with("veh_t6_") && name.find("dead") == std::string::npos &&
                        name.find("civ_") == std::string::npos)
                    {
                        std::string all;
                        for (std::uint32_t b = 0; b < count; ++b)
                        {
                            const auto text = reinterpret_cast<const char*>(buffer + 24u * boneNames[b] + 4);
                            if (!IsBadReadPtr(text, 1))
                                all += std::format(" {}", std::string_view(text, strnlen(text, 64)));
                        }
                        LOG_INFO("T6: killstreak model {} bones:{}", name, all);
                    }
                }
                LOG_INFO("T6: killstreak model {}: {}", name, has);
            }
            ForEachAsset([&](const XAssetEntry& entry) {
                const bool fx = entry.type == ASSET_TYPE_FX, anim = entry.type == ASSET_TYPE_XANIMPARTS;
                if (!fx && !anim)
                    return;
                const auto name = GetAssetName(entry);
                if (!name || !*name || *name == ',' || !matches(name, fx ? std::span<const char* const>(FX_PARTS)
                                                                         : std::span<const char* const>(ANIM_PARTS)))
                    return;
                if (logged.insert(std::format("{} {}", fx ? "fx" : "anim", name)).second)
                    LOG_INFO("T6: killstreak {} {}", fx ? "effect" : "animation", name);
            });
            // (the killstreaks' weapons, and the effect each flashes and the sound it fires)
            constexpr std::array WEAPON_PARTS{"turret", "drone", "heli", "chopper", "qr", "tank", "straferun", "guard",
                                              "gunner", "killstreak", "warthog", "a10", "vtol", "pegasus", "cobra",
                                              "missile", "remote", "planemortar", "rcbomb", "minigun"};
            for (const auto& [name, def] : weaponDefs)
            {
                if (!matches(name, WEAPON_PARTS) || !logged.insert("weapon " + name).second)
                    continue;
                const auto flash = *reinterpret_cast<const std::uint8_t* const*>(def + WEAPDEF_WORLD_FLASH);
                const auto flashName = flash ? *reinterpret_cast<const char* const*>(flash) : nullptr;
                const auto sound = *reinterpret_cast<const char* const*>(def + WEAPDEF_FIRE_SOUND);
                LOG_INFO("T6: killstreak weapon {}: flash {}, sound {}", name, flashName ? flashName : "none",
                         sound && *sound ? sound : "none");
            }
        }
    }  // namespace

    void BuildCatalog()
    {
        std::unique_lock lock(catalogMutex);
        auto catalog = std::make_shared<Types::ActorCatalog>();
        catalog->listsOnlyLoaded = true;
        // (no packs offered in BO2 for now: More characters... and its messages left out, as no pack loads in a film)
        // (nothing of the zones while one loads)
        if (!loadingPack.empty())
        {
            Components::Actors::SetCatalog(std::move(catalog));
            return;
        }
        std::optional<std::uint8_t> mapZone;
        ForEachAsset([&](const XAssetEntry& entry) {
            if (entry.type == ASSET_TYPE_CLIPMAP_MP && !mapZone)
                mapZone = entry.zoneIndex;
        });
        std::set<std::string> names, mapNames;
        std::map<std::string, std::uint32_t> bonesOf;
        modelPointers.clear();
        decodedPoses.clear();
        ForEachAsset([&](const XAssetEntry& entry) {
            if (entry.type != ASSET_TYPE_XMODEL)
                return;
            const auto name = GetAssetName(entry);
            // (a model with too many bones for a DObj can't be one; ',' starts the names of the defaults' copies)
            if (!name || !*name || *name == ',' || CountBones(entry.header) > MAX_DOBJ_BONES)
                return;
            names.emplace(name);
            bonesOf[name] = CountBones(entry.header);
            modelPointers[name] = static_cast<const std::uint8_t*>(entry.header);
            if (mapZone && entry.zoneIndex == *mapZone)
                mapNames.emplace(name);
        });
        catalog->props.assign(names.begin(), names.end());
        // the effects, for actors' events
        effectDefs.clear();
        ForEachAsset([&](const XAssetEntry& entry) {
            if (entry.type != ASSET_TYPE_FX)
                return;
            if (const auto name = GetAssetName(entry); name && *name && *name != ',')
                effectDefs.emplace(name, entry.header);
        });
        for (const auto& [name, def] : effectDefs)
        {
            catalog->effects.push_back(name);
            // how it plays by itself, for the scene's effects: FxEffectDef's looping life (ms, 0x7FFFFFFF till it is
            // stopped) at +0x14, and how many of its elements loop and play once, the u16s at +0x8 and +0xA
            const auto bytes = static_cast<const std::uint8_t*>(def);
            catalog->effectInfo[name] = {
                static_cast<std::uint32_t>(std::max(*reinterpret_cast<const std::int32_t*>(bytes + 0x14), 0)),
                static_cast<std::uint32_t>(std::max<std::int16_t>(*reinterpret_cast<const std::int16_t*>(bytes + 0x8), 0)),
                static_cast<std::uint32_t>(std::max<std::int16_t>(*reinterpret_cast<const std::int16_t*>(bytes + 0xA), 0))};
        }
        ReadWeapons();
        Components::Loadouts::SetCatalog(MakeWeaponCatalog(names));
        // the characters' models, and a character of each body (a full body on its own, a body with a head of the
        // same set: the names before "_body" or "_fb" alike)
        for (const auto& name : names)
            if (IsCharacterModel(name))
                catalog->models.push_back({.name = name, .kind = Classify(name), .source = "Multiplayer",
                                           .bones = bonesOf[name]});
        const auto setOf = [](std::string_view name) {
            for (const auto marker : {"_body", "_fb", "_head"})
                if (const auto at = name.find(marker); at != std::string_view::npos)
                    return std::string(name.substr(0, at));
            return std::string(name);
        };
        for (const auto& model : catalog->models)
        {
            using Kind = Types::ActorCatalog::ModelKind;
            if (model.kind != Kind::Body && model.kind != Kind::FullBody)
                continue;
            Types::Character character{.id = "map/" + model.name, .name = model.name, .body = model.name};
            if (model.kind == Kind::Body)
            {
                const auto set = setOf(model.name);
                const auto head = std::ranges::find_if(catalog->models, [&](const auto& other) {
                    return other.kind == Kind::Head && setOf(other.name) == set;
                });
                const auto anyHead = std::ranges::find(catalog->models, Kind::Head, &Types::ActorCatalog::Model::kind);
                if (head != catalog->models.end())
                    character.head = head->name;
                else if (anyHead != catalog->models.end())
                    character.head = anyHead->name;
            }
            character.bodies = {character.body};
            if (!character.head.empty())
                character.heads = {character.head};
            catalog->characters.push_back(std::move(character));
        }
        // the killstreaks and dogs it has loaded: their models, and a character of each (its looks its bodies)
        {
            using Kind = Types::ActorCatalog::ModelKind;
            const auto addModel = [&](const std::string& name) {
                if (!catalog->FindModel(name))
                    catalog->models.push_back(
                        {.name = name, .kind = Kind::Body, .source = "Multiplayer", .bones = bonesOf[name]});
            };
            for (const auto& info : Components::KillstreakLogic::GetAll())
            {
                if (!names.contains(std::string(info.models.front())))
                    continue;
                Types::Character character;
                character.id = "map/" + std::string(info.models.front());
                character.name = std::string(info.name);
                character.kind = Types::Character::Kind::Killstreak;
                character.body = std::string(info.models.front());
                for (const auto model : info.models)
                    if (names.contains(std::string(model)))
                    {
                        addModel(std::string(model));
                        character.bodies.emplace_back(model);
                    }
                catalog->characters.push_back(std::move(character));
            }
            for (const auto dog : Components::CharacterLogic::MAP_DOGS)
            {
                if (!names.contains(std::string(dog)))
                    continue;
                addModel(std::string(dog));
                Types::Character character;
                character.id = "map/" + std::string(dog);
                character.name = dog.ends_with("_black") ? "German shepherd (black vest)" : "German shepherd";
                character.kind = Types::Character::Kind::Dog;
                character.body = std::string(dog);
                character.bodies = {character.body};
                catalog->characters.push_back(std::move(character));
            }
            LogKillstreakAssets(names, modelPointers);
        }
        catalog->mapProps.assign(mapNames.begin(), mapNames.end());

        // the animations: how long, whether they loop, and what they're for by their names (the players' pb_ of the
        // whole body, pt_ of the torso; faces; dogs')
        animationParts.clear();
        std::size_t streamedLeftOut = 0;
        ForEachAsset([&](const XAssetEntry& entry) {
            if (entry.type != ASSET_TYPE_XANIMPARTS)
                return;
            const auto name = GetAssetName(entry);
            if (!name || !*name || *name == ',')
                return;
            const auto parts = static_cast<const std::uint8_t*>(entry.header);
            // (a pose of a frame or none, as the mounted guns' aims: the game only blends those into its own, and on
            // their own they do nothing)
            if (GetDuration(parts) < 0.05f)
                return;
            // (nor the mounted guns' gunners', which only the game's turrets blend into theirs; of the additive ones,
            // asset type 1 at +0x23, only the players' upper body pt_ ones, their guns' firing, reloading, raising and
            // putting away, which add to what the legs show under an additive node (see CreateAnimations))
            if (std::string_view(name).find("gunner") != std::string_view::npos ||
                (*(parts + 0x23) == 1 && !std::string_view(name).starts_with("pt_")))
                return;
            // (nor asset type 0's: the vehicles' riders' and the zipline's, 80 frames of placeholder each)
            if (*(parts + 0x23) == 0)
                return;
            // (nor the first person arms' vm_ ones, nor the zombies')
            {
                std::string lower(name);
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.starts_with("vm_") || lower.starts_with("viewmodel") || lower.find("zombie") != std::string::npos ||
                    lower.starts_with("zm_") || lower.find("_zm_") != std::string::npos)
                    return;
            }
            // (nor one streamed from the disk that isn't in memory, as the campaign's long scenes: its streams'
            // pointers, from +0x44, empty with streamedFileSize at +0x14)
            const auto streamed = *reinterpret_cast<const std::uint32_t*>(parts + 0x14);
            const auto dataShort = *reinterpret_cast<const void* const*>(parts + 0x48);
            const auto randomShort = *reinterpret_cast<const void* const*>(parts + 0x50);
            if (streamed != 0 && !dataShort && !randomShort)
            {
                ++streamedLeftOut;
                return;
            }
            if (!animationParts.emplace(name, parts).second)
                return;
            const std::string_view view(name);
            auto kind = Types::AnimationKind::Other;
            auto family = Types::AnimationFamily::Other;
            if (view.find("face") != std::string_view::npos || view.starts_with("pf_"))
                kind = Types::AnimationKind::Face, family = Types::AnimationFamily::Human;
            else if (view.starts_with("pb_"))
                kind = Types::AnimationKind::FullBody, family = Types::AnimationFamily::MpPlayer;
            else if (view.starts_with("pt_"))
                kind = Types::AnimationKind::Torso, family = Types::AnimationFamily::MpPlayer;
            else if (view.find("dog") != std::string_view::npos || view.starts_with("german_shepherd"))
                kind = Types::AnimationKind::FullBody, family = Types::AnimationFamily::Dog;
            else if (view.starts_with("ai_") || view.starts_with("zombie") || view.starts_with("ch_"))
                kind = Types::AnimationKind::FullBody, family = Types::AnimationFamily::SpHuman;
            // (only people's and the dogs' (multiplayer's german_shepherd_*): not faces, props' or vehicles')
            if (family != Types::AnimationFamily::MpPlayer && family != Types::AnimationFamily::SpHuman &&
                family != Types::AnimationFamily::Dog)
                return;
            if (kind == Types::AnimationKind::Face)
                return;
            catalog->animations.push_back({.name = name,
                                           .duration = GetDuration(parts),
                                           .loop = *(parts + PARTS_LOOP) != 0,
                                           .kind = kind,
                                           .family = family});
        });
        // the campaign's, from the animation pack (those the game has loaded itself as they are)
        ReadPackedAnimations();
        NamePackedAnimations();
        for (const auto& packed : packedAnimations)
        {
            if (!animationParts.emplace(packed.name, packed.parts).second)
                continue;
            catalog->animations.push_back({.name = packed.name,
                                           .duration = GetDuration(packed.parts),
                                           .loop = *(packed.parts + PARTS_LOOP) != 0,
                                           .kind = Types::AnimationKind::FullBody,
                                           .family = Types::AnimationFamily::SpHuman});
        }
        std::ranges::sort(catalog->animations, {}, &Types::ActorCatalog::Animation::name);
        LOG_INFO("T6: {} animations left out as streamed and not in memory", streamedLeftOut);
        // (those a person can't play, tried on a character's body: left out of the browser)
        {
            const auto reference = std::ranges::find_if(catalog->models, [](const auto& model) {
                return model.kind == Types::ActorCatalog::ModelKind::FullBody ||
                       model.kind == Types::ActorCatalog::ModelKind::Body;
            });
            const auto pointer = reference != catalog->models.end() ? modelPointers.find(reference->name)
                                                                     : modelPointers.end();
            const auto body = pointer != modelPointers.end() ? ViewModel(pointer->second) : std::nullopt;
            if (body)
            {
                std::map<std::string, std::string_view> unusable;
                std::size_t tried = 0;
                for (const auto& animation : catalog->animations)
                {
                    if (animation.family != Types::AnimationFamily::MpPlayer &&
                        animation.family != Types::AnimationFamily::SpHuman)
                        continue;
                    ++tried;
                    // (an upper body pt_ one poses only the upper body: the legs are the clips' under it)
                    if (IsUpperBodyLayer(animation.name))
                        continue;
                    if (const auto why = WhyUnusable(animationParts.at(animation.name), *body))
                        unusable.emplace(animation.name, *why);
                }
                // (most of them failing would be the check's fault, not theirs: then all are kept)
                if (unusable.size() * 2 > tried)
                    LOG_WARN("T6: {} of {} animations looked unplayable on {}, so none are left out", unusable.size(),
                             tried, reference->name);
                else
                {
                    for (const auto& [name, why] : unusable)
                    {
                        LOG_DEBUG("T6: animation {} left out: {}", name, why);
                        animationParts.erase(name);
                    }
                    std::erase_if(catalog->animations,
                                  [&](const auto& animation) { return unusable.contains(animation.name); });
                    LOG_INFO("T6: {} of {} animations left out as ones a person can't play (tried on {})",
                             unusable.size(), tried, reference->name);
                }
            }
            else
                LOG_WARN("T6: no character's body to try the animations on");
        }
        // (how far each of the people's moves in a play, measured in trees of 48 at a time)
        {
            std::vector<std::string> batch;
            std::size_t measured = 0;
            for (const auto& animation : catalog->animations)
            {
                if (animation.family != Types::AnimationFamily::MpPlayer &&
                    animation.family != Types::AnimationFamily::SpHuman && animation.family != Types::AnimationFamily::Dog)
                    continue;
                if (Components::Actors::GetAnimationMotion(animation.name))
                    continue;
                batch.push_back(animation.name);
                if (batch.size() == MAX_ANIMATIONS)
                {
                    MeasureMotions(batch);
                    measured += batch.size();
                    batch.clear();
                }
            }
            MeasureMotions(batch);
            measured += batch.size();
            if (const auto run = Components::Actors::GetAnimationMotion("pb_combatrun_forward_loop"))
                LOG_INFO("T6: measured how {} animations move; pb_combatrun_forward_loop {:.1f} forward, {:.1f} left",
                         measured, run->x, run->y);
            else
                LOG_INFO("T6: measured how {} animations move", measured);
        }
        LOG_INFO("T6: {} models loaded for props, {} of them the map's (zone {}); {} character models, {} characters, "
                 "{} animations",
                 catalog->props.size(), catalog->mapProps.size(), mapZone ? std::to_string(*mapZone) : "not found",
                 catalog->models.size(), catalog->characters.size(), catalog->animations.size());
        for (const auto& animation : catalog->animations)
            if (animation.family == Types::AnimationFamily::MpPlayer ||
                animation.family == Types::AnimationFamily::SpHuman)
                LOG_DEBUG("T6: animation {} ({:.2f} s{}, type {})", animation.name, animation.duration,
                          animation.loop ? ", loops" : "", *(animationParts.at(animation.name) + 0x23));
        for (const auto& model : catalog->models)
            LOG_DEBUG("T6: character model {} ({} bones)", model.name, model.bones);
        Components::Actors::SetCatalog(std::move(catalog));
    }
    }  // namespace

    namespace
    {
        // Actors' events as the timeline plays past them (a jump, or more than a second at once, plays none): a shot's
        // muzzle flash, sound and casing from its gun (an attachment's own, as a suppressor's), and a hit's or an
        // effect's effect on a bone; effects by name, from the game's FX assets (33)
        constexpr std::uint32_t MAX_EVENT_STEP = 1000;
        constexpr auto DEFAULT_HIT_EFFECT = "impacts/fx_flesh_hit";
        constexpr auto DEFAULT_HIT_TAG = "j_spine4";
        constexpr auto DEFAULT_EFFECT_TAG = "tag_origin";
        constexpr std::size_t UNIQUE_SOUND_OVERRIDES = 0xFC, UNIQUE_FIRE_SOUND = 0x100, UNIQUE_WORLD_FLASH = 0x140;
        std::optional<std::uint32_t> eventTick;

        std::uint32_t ScriptString(const char* text)
        {
            typedef std::uint32_t(__cdecl * SL_GetString_t)(const char* text);
            return reinterpret_cast<SL_GetString_t>(GetGameAddresses().SL_GetString())(text) & 0xFFFF;
        }

        // a bone of a drawn actor in the world: its place and axes; none for a bone it hasn't
        bool GetTag(const Drawn& drawn, const char* tag, float (&axis)[3][3], float (&origin)[3])
        {
            typedef int(__cdecl * CG_DObjGetWorldTagMatrix_t)(void* pose, void* obj, std::uint32_t tagName,
                                                               float (*axis)[3], float* origin);
            const auto entity = GetEntity(drawn.handle);
            return entity && drawn.obj &&
                   reinterpret_cast<CG_DObjGetWorldTagMatrix_t>(GetGameAddresses().CG_DObjGetWorldTagMatrix())(
                       entity, drawn.obj, ScriptString(tag), axis, origin) != 0;
        }

        void PlayEffect(const void* def, const float (&origin)[3], const float (&axis)[3][3])
        {
            if (!def)
                return;
            typedef std::int32_t(__cdecl * FX_PlayOrientedEffect_t)(std::int32_t localClientNum, const void* def,
                                                                    std::int32_t startTime, const float* origin,
                                                                    const float (*axis)[3], float scale, std::int32_t);
            const auto cg = *reinterpret_cast<const std::uint8_t* const*>(GetGameAddresses().cgArray());
            if (!cg)
                return;
            const auto time = *reinterpret_cast<const std::int32_t*>(cg + 0x4808C);
            reinterpret_cast<FX_PlayOrientedEffect_t>(GetGameAddresses().FX_PlayOrientedEffect())(0, def, time, origin,
                                                                                                 axis, 1.0f, 0);
        }

        void PlaySound(const char* alias, const float (&origin)[3])
        {
            if (!alias || !*alias)
                return;
            typedef std::int32_t(__cdecl * CG_PlaySoundAlias_t)(std::int32_t localClientNum, std::int32_t entnum,
                                                                const float* origin, std::int32_t, std::int32_t,
                                                                float volume, std::uint32_t aliasHash);
            typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
            const auto& addresses = GetGameAddresses();
            reinterpret_cast<CG_PlaySoundAlias_t>(addresses.CG_PlaySoundAlias())(
                0, 0x3FE, origin, 0, 0, 1.0f, reinterpret_cast<SND_HashName_t>(addresses.SND_HashName())(alias));
        }

        // where an actor that shoots at another aims at a tick: that one's chest, as it stands then (none for one that
        // shoots where it faces, or whose target is gone)
        std::optional<glm::vec3> GetShootTarget(const Types::Actor& actor, std::uint32_t tick)
        {
            if (actor.shootAt == 0 || actor.shootAt == actor.id)
                return std::nullopt;
            const auto other = std::ranges::find(scene, actor.shootAt, &Types::Actor::id);
            if (other == scene.end() || !Components::Actors::IsVisible(*other, tick))
                return std::nullopt;
            const bool killstreak = Components::KillstreakLogic::FindByModels(other->models).has_value();
            return Components::Actors::EvaluatePlacement(*other, tick).position +
                   glm::vec3(0.0f, 0.0f, killstreak ? 20.0f : 46.0f);
        }

        // axes whose forward (x) is a direction, the game's way (left y, up z)
        void AimAxis(float (&axis)[3][3], glm::vec3 direction)
        {
            if (glm::length(direction) < 0.001f)
                return;
            const auto forward = glm::normalize(direction);
            auto left = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), forward);
            left = glm::length(left) < 0.001f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(left);
            const auto up = glm::cross(forward, left);
            for (int i = 0; i < 3; ++i)
            {
                axis[0][i] = forward[i];
                axis[1][i] = left[i];
                axis[2][i] = up[i];
            }
        }

        // A shot at an actor's target hitting it: the flesh hit on its chest bone (else where it aims), facing back
        // at the muzzle. Returns where it hit, none for an actor that shoots where it faces.
        std::optional<glm::vec3> HitShootTarget(const Types::Actor& actor, const float (&muzzle)[3])
        {
            const auto tick = Components::Playback::GetActorTick();
            auto point = GetShootTarget(actor, tick);
            if (!point)
                return std::nullopt;
            float axis[3][3]{}, origin[3]{};
            if (const auto drawn = drawnModels.find(actor.shootAt);
                drawn != drawnModels.end() && drawn->second.drawn &&
                (GetTag(drawn->second, "j_spine4", axis, origin) || GetTag(drawn->second, "tag_body", axis, origin)))
                point = glm::vec3(origin[0], origin[1], origin[2]);
            const glm::vec3 from(muzzle[0], muzzle[1], muzzle[2]);
            const float at[3] = {point->x, point->y, point->z};
            float facing[3][3]{};
            AimAxis(facing, from - *point);
            if (const auto def = effectDefs.find(DEFAULT_HIT_EFFECT); def != effectDefs.end())
                PlayEffect(def->second, at, facing);
            return point;
        }

        // a shot of the actor's gun: its flash and sound at its muzzle, its casing from its ejection port
        void Fire(const Types::Actor& actor, const Drawn& drawn)
        {
            const auto info = weaponInfo.find(actor.weapon);
            if (info == weaponInfo.end() || !info->second.def)
                return;
            const auto def = info->second.def;
            const void* flash = *reinterpret_cast<const void* const*>(def + WEAPDEF_WORLD_FLASH);
            const char* sound = *reinterpret_cast<const char* const*>(def + WEAPDEF_FIRE_SOUND);
            for (const auto& name : actor.attachments)
            {
                const auto attachment = std::ranges::find(info->second.attachments, name, &WeaponAttachmentInfo::name);
                if (attachment == info->second.attachments.end())
                    continue;
                if (const auto own = *reinterpret_cast<const void* const*>(attachment->unique + UNIQUE_WORLD_FLASH))
                    flash = own;
                const auto ownSound = *reinterpret_cast<const char* const*>(attachment->unique + UNIQUE_FIRE_SOUND);
                if (*reinterpret_cast<const std::int32_t*>(attachment->unique + UNIQUE_SOUND_OVERRIDES) && ownSound &&
                    *ownSound)
                    sound = ownSound;
            }
            float axis[3][3]{}, origin[3]{};
            if (GetTag(drawn, "tag_flash", axis, origin))
            {
                if (const auto hit = HitShootTarget(actor, origin))
                    AimAxis(axis, *hit - glm::vec3(origin[0], origin[1], origin[2]));
                PlayEffect(flash, origin, axis);
                PlaySound(sound, origin);
            }
            if (GetTag(drawn, "tag_brass", axis, origin))
                PlayEffect(*reinterpret_cast<const void* const*>(def + WEAPDEF_WORLD_SHELL), origin, axis);
        }

        // A shot of a killstreak's guns: each one's muzzle flash and its weapon's sound, at its muzzle (its weapon's
        // world flash, else a heavy machine gun's; the guns of a killstreak whose weapon the game hasn't loaded fire as
        // an LMG does)
        void FireKillstreak(const Components::KillstreakLogic::Info& info, const Drawn& drawn)
        {
            const std::uint8_t* def = nullptr;
            for (const std::string_view name : {info.weapon, std::string_view("lsat_mp"), std::string_view("mk48_mp"),
                                                std::string_view("hamr_mp")})
                if (const auto found = weaponDefs.find(name); !name.empty() && found != weaponDefs.end())
                {
                    def = found->second;
                    break;
                }
            static std::set<std::string_view> told;
            if (told.insert(info.name).second)
                LOG_DEBUG("T6: the {} fires as {}", info.name, def ? "its weapon (or an LMG)" : "nothing loaded");
            const void* flash = def ? *reinterpret_cast<const void* const*>(def + WEAPDEF_WORLD_FLASH) : nullptr;
            const char* sound = def ? *reinterpret_cast<const char* const*>(def + WEAPDEF_FIRE_SOUND) : nullptr;
            // (most killstreaks' weapons fire without a sound of their own, the game playing a loop instead: an LMG's)
            if (!sound || !*sound)
                for (const auto name : {"lsat_mp", "mk48_mp", "hamr_mp"})
                    if (const auto found = weaponDefs.find(std::string_view(name)); found != weaponDefs.end())
                        if (const auto own = *reinterpret_cast<const char* const*>(found->second + WEAPDEF_FIRE_SOUND);
                            own && *own)
                        {
                            sound = own;
                            break;
                        }
            for (const auto& gun : info.guns)
            {
                const void* own = flash;
                if (!own && !gun.flashEffect.empty())
                    if (const auto found = effectDefs.find(gun.flashEffect); found != effectDefs.end())
                        own = found->second;
                const std::string tag(gun.flash.empty() ? std::string_view("tag_flash") : gun.flash);
                float axis[3][3]{}, origin[3]{};
                if (!GetTag(drawn, tag.c_str(), axis, origin))
                    continue;
                PlayEffect(own, origin, axis);
                PlaySound(sound, origin);
            }
        }

        void PlayEvent(const Types::Actor& actor, const Drawn& drawn, const Types::ActorEvent& event)
        {
            switch (event.type)
            {
                case Types::ActorEventType::Fire:
                    if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models))
                        FireKillstreak(Components::KillstreakLogic::Get(*killstreak), drawn);
                    else
                        Fire(actor, drawn);
                    break;
                case Types::ActorEventType::Hit:
                case Types::ActorEventType::Effect:
                {
                    const bool hit = event.type == Types::ActorEventType::Hit;
                    const auto name = event.effect.empty() ? (hit ? std::string(DEFAULT_HIT_EFFECT) : std::string())
                                                           : event.effect;
                    // (another game's name, as the death's blood a Hit dies with: BO2's without its _mp, else its
                    // blood)
                    auto def = effectDefs.find(name);
                    if (def == effectDefs.end() && name.ends_with("_mp"))
                        def = effectDefs.find(std::string_view(name).substr(0, name.size() - 3));
                    // (Black Ops' killstreak effects BO2 hasn't: its nearest)
                    if (def == effectDefs.end())
                    {
                        static const std::map<std::string, std::string, std::less<>> NEAREST{
                            {"maps/mp_maps/fx_mp_exp_rc_bomb", "weapon/qr_drone/fx_exp_qr_drone"},
                            {"explosions/fx_grenadeexp_default", "weapon/qr_drone/fx_exp_qr_drone"},
                            {"weapon/grenade/fx_spark_disabled_weapon_lg", "weapon/grenade/fx_spark_disabled_rc_car"},
                        };
                        if (const auto nearest = NEAREST.find(name); nearest != NEAREST.end())
                            def = effectDefs.find(nearest->second);
                    }
                    if (def == effectDefs.end() && hit)
                        def = effectDefs.find(DEFAULT_HIT_EFFECT);
                    if (def == effectDefs.end())
                    {
                        LOG_DEBUG("T6: actor {}'s event has the effect {}, which the game hasn't", actor.id, name);
                        break;
                    }
                    const auto tag = event.tag.empty() ? (hit ? DEFAULT_HIT_TAG : DEFAULT_EFFECT_TAG) : event.tag.c_str();
                    float axis[3][3]{}, origin[3]{};
                    if (!GetTag(drawn, tag, axis, origin))
                    {
                        // (a bone not found, as on a DObj made this frame: at the actor's chest, facing up)
                        const auto entity = GetEntity(drawn.handle);
                        if (!entity)
                            break;
                        std::memcpy(origin, entity + POSE_ORIGIN, sizeof(origin));
                        origin[2] += hit ? 48.0f : 0.0f;
                        axis[0][2] = 1.0f;
                        axis[1][1] = 1.0f;
                        axis[2][0] = -1.0f;
                        LOG_DEBUG("T6: actor {}'s bone {} not found for its event; played at its place", actor.id, tag);
                    }
                    PlayEffect(def->second, origin, axis);
                    break;
                }
                default:
                    break;
            }
        }
        // The scene's effects (Add Effect..., on their own, not an actor's): each play of its bar spawned as the
        // timeline passes its begin, from as far back as the playhead is into it; one that loops kept (FX_SpawnEffect)
        // and killed at its play's end, one that plays once let go to end by itself. What was spawned for one ends at
        // once when it changes, is hidden or deleted, and when the playhead jumps (then what is on is spawned again).
        struct SceneEffect
        {
            Types::Actor actor;                                                             // as its plays were spawned
            std::map<std::uint32_t, std::pair<std::int32_t, ActorLogic::EffectPlay>> kept;  // by begin
            std::set<std::uint32_t> begun;  // the begins of the plays spawned since the last jump
        };
        std::map<std::uint32_t, SceneEffect> sceneEffects;
        std::optional<std::uint32_t> sceneEffectsTick;

        void KillEffect(std::int32_t handle, bool immediately)
        {
            typedef void(__cdecl * FX_KillEffect_t)(std::int32_t localClientNum, std::int32_t handle,
                                                    std::int32_t immediately);
            if (handle)
                reinterpret_cast<FX_KillEffect_t>(GetGameAddresses().FX_KillEffect())(0, handle, immediately ? 1 : 0);
        }

        void EndSceneEffect(SceneEffect& effect, bool cut)
        {
            for (const auto& [begin, kept] : effect.kept)
                KillEffect(kept.first, cut);
            effect.kept.clear();
            effect.begun.clear();
        }

        // an effect in the world, from msecBegin; kept: its handle, to kill (else let go, 0)
        std::int32_t SpawnWorldEffect(const void* def, const float (&origin)[3], const float (&axis)[3][3],
                                      std::int32_t msecBegin, bool keep)
        {
            if (!def)
                return 0;
            if (!keep)
            {
                typedef std::int32_t(__cdecl * FX_PlayOrientedEffect_t)(std::int32_t localClientNum, const void* def,
                                                                        std::int32_t startTime, const float* origin,
                                                                        const float(*axis)[3], float scale,
                                                                        std::int32_t);
                reinterpret_cast<FX_PlayOrientedEffect_t>(GetGameAddresses().FX_PlayOrientedEffect())(
                    0, def, msecBegin, origin, axis, 1.0f, 0);
                return 0;
            }
            const auto system = *reinterpret_cast<void* const*>(GetGameAddresses().fx_system());
            if (!system)
                return 0;
            typedef std::int32_t(__cdecl * FX_SpawnEffect_t)(
                void* system, const void* def, std::int32_t msecBegin, const float* origin, const float(*axis)[3],
                std::uint32_t dobjHandle, std::uint32_t bone, std::uint32_t sortOrder, std::uint32_t owner,
                std::uint32_t markEntnum, const float* offset, std::int32_t, std::int32_t, std::int32_t, float scale,
                std::int32_t);
            // (no DObj, bone, sort order or owner: one of the world's, as the game's playfx spawns it)
            static constexpr float NO_OFFSET[3] = {};
            return reinterpret_cast<FX_SpawnEffect_t>(GetGameAddresses().FX_SpawnEffect())(
                system, def, msecBegin, origin, axis, 0x7FF, 0x1FF, 0xFF, 0, 0x3FF, NO_OFFSET, 0, 0, -1, 1.0f, 0);
        }

        void PlaySceneEffects(std::uint32_t tick)
        {
            const auto catalog = Components::Actors::GetCatalog();
            const auto cg = *reinterpret_cast<const std::uint8_t* const*>(GetGameAddresses().cgArray());
            if (!catalog || !cg)
                return;
            const auto time = *reinterpret_cast<const std::int32_t*>(cg + 0x4808C);
            const bool jumped =
                !sceneEffectsTick || tick < *sceneEffectsTick || tick - *sceneEffectsTick > MAX_EVENT_STEP;
            sceneEffectsTick = tick;

            // gone, hidden or changed: what it spawned ends at once; after a jump all of it does
            for (auto it = sceneEffects.begin(); it != sceneEffects.end();)
            {
                const auto actor = std::ranges::find(scene, it->first, &Types::Actor::id);
                const auto& was = it->second.actor;
                if (actor == scene.end() || actor->hidden || actor->effect != was.effect ||
                    actor->position != was.position || actor->yaw != was.yaw)
                {
                    EndSceneEffect(it->second, true);
                    it = sceneEffects.erase(it);
                    continue;
                }
                if (jumped)
                    EndSceneEffect(it->second, true);
                ++it;
            }

            for (const auto& actor : scene)
            {
                if (!actor.effect || actor.hidden)
                    continue;
                const auto def = effectDefs.find(actor.effect->name);
                const auto info = catalog->effectInfo.find(actor.effect->name);
                if (def == effectDefs.end() || info == catalog->effectInfo.end())
                    continue;
                auto& state = sceneEffects[actor.id];
                state.actor = actor;

                // its looping plays that are over end, their particles let finish (or cut at once at the bar's end)
                for (auto it = state.kept.begin(); it != state.kept.end();)
                {
                    const auto& [handle, play] = it->second;
                    if (tick < play.end && tick >= play.begin)
                    {
                        ++it;
                        continue;
                    }
                    KillEffect(handle, play.cut);
                    it = state.kept.erase(it);
                }

                // facing its way: its x axis where it fires, as the game's angles turn it
                const auto turned =
                    MathUtils::AnglesToAxis(glm::vec3(actor.effect->pitch, actor.yaw, actor.effect->roll));
                float axis[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        axis[i][j] = turned[i][j];
                const float origin[3] = {actor.position.x, actor.position.y, actor.position.z};
                for (const auto& play : ActorLogic::GetEffectPlays(actor, info->second, tick))
                {
                    if (!state.begun.insert(play.begin).second)
                        continue;
                    const auto msecBegin =
                        time - static_cast<std::int32_t>(std::min(tick - play.begin, ActorLogic::MAX_EFFECT_CATCH_UP));
                    const auto handle = SpawnWorldEffect(def->second, origin, axis, msecBegin, play.kept);
                    if (play.kept && handle)
                        state.kept[play.begin] = {handle, play};
                }
            }
        }

        // (with a map: the game's effects went with it)
        void ForgetSceneEffects()
        {
            sceneEffects.clear();
            sceneEffectsTick.reset();
        }

        void StopSceneEffects()
        {
            for (auto& [id, effect] : sceneEffects)
                EndSceneEffect(effect, true);
            sceneEffects.clear();
            sceneEffectsTick.reset();
        }
    }  // namespace

    namespace
    {
        // A killstreak's moving parts: its body leaning over its wheels, its front wheels steering and all of them
        // rolling (VehicleLogic::GetJoints), its guns aimed by its Look events or scanning, its barrels spinning as it
        // fires. T6 has no bone controllers to call (T5's CG_DoControllers), so each killstreak's are the pose of an
        // animation made for it, of those bones alone (each a still full quaternion, PART_TYPE_FULL_QUAT_NO_SIZE),
        // which its DObj's tree plays at full weight; the rotations written each frame before it is drawn. Never
        // freed: a tree going may still have one for a few frames.
        struct PoseAnimation
        {
            std::string name;
            std::vector<std::string> bones;
            std::vector<glm::quat> rests;  // each bone's own rotation, relative to its parent
            std::vector<std::uint16_t> names;
            std::vector<std::int16_t> quats;
            // (the game reads which of its bones have no translation from its bytes, one each after the moving
            // ones' (the skeleton's 0x8E2C13): all of them, by their indices)
            std::vector<std::uint8_t> noTrans;
            std::vector<std::uint8_t> parts = std::vector<std::uint8_t>(PARTS_SIZE);
        };
        std::map<std::uint32_t, std::unique_ptr<PoseAnimation>> poseAnimations;  // by actor id
        std::vector<std::unique_ptr<PoseAnimation>> retiredPoses;

        PoseAnimation* GetPoseAnimation(std::uint32_t actorId, const std::string& model,
                                        const std::vector<std::string>& bones)
        {
            auto& slot = poseAnimations[actorId];
            if (slot && slot->bones != bones)
                retiredPoses.push_back(std::move(slot));
            if (!slot)
            {
                static std::uint32_t made = 0;
                auto pose = std::make_unique<PoseAnimation>();
                pose->name = std::format("iris_pose_{}_{}", actorId, made++);
                pose->bones = bones;
                pose->rests.assign(bones.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                pose->names.assign(bones.size(), 0);
                pose->quats.assign(bones.size() * 4, 0);
                for (std::size_t i = 0; i < bones.size(); ++i)
                    pose->quats[i * 4 + 3] = 32767;
                for (std::size_t i = 0; i < bones.size(); ++i)
                    pose->noTrans.push_back(static_cast<std::uint8_t>(i));
                // (its bones' own rotations, from the model)
                if (const auto found = modelPointers.find(model); found != modelPointers.end())
                    if (const auto view = ViewModel(found->second))
                    {
                        const auto rest = GetRestBones(*view);
                        for (std::size_t i = 0; i < bones.size(); ++i)
                        {
                            const auto id = static_cast<std::uint16_t>(ScriptString(bones[i].c_str()));
                            for (std::uint32_t b = 0; b < view->numBones; ++b)
                                if (view->boneNames[b] == id)
                                    pose->rests[i] = rest[b].rotation;
                        }
                    }
                const auto n = static_cast<std::uint8_t>(bones.size());
                auto* p = pose->parts.data();
                Store(p, 0, pose->name.c_str());
                Store(p, 4, static_cast<std::uint16_t>(bones.size()));      // dataByteCount
                Store(p, 6, static_cast<std::uint16_t>(bones.size() * 4));  // dataShortCount
                Store(p, PARTS_NUM_FRAMES, static_cast<std::uint16_t>(1));
                p[PARTS_LOOP] = 1;
                p[0x18 + PART_TYPE_FULL_QUAT_NO_SIZE] = n;
                p[0x18 + PART_TYPE_NO_TRANS] = n;
                p[0x18 + PART_TYPE_ALL] = n;
                p[0x23] = 2;  // asset type: as the people's
                Store(p, PARTS_FRAMERATE, 30.0f);
                Store(p, PARTS_FRAMERATE + 4, 30.0f);  // frequency
                Store(p, 0x40, pose->names.data());
                Store(p, 0x44, pose->noTrans.data());
                Store(p, 0x48, pose->quats.data());
                LOG_DEBUG("T6: made the pose of actor {}'s moving parts: {} bones", actorId, bones.size());
                slot = std::move(pose);
            }
            // (one of the catalog's animations, its bones' names this map's script strings: again once the catalog
            // is made again)
            if (!animationParts.contains(slot->name))
            {
                for (std::size_t i = 0; i < slot->bones.size(); ++i)
                    slot->names[i] = static_cast<std::uint16_t>(ScriptString(slot->bones[i].c_str()));
                std::unique_lock lock(catalogMutex);
                animationParts.emplace(slot->name, slot->parts.data());
            }
            return slot.get();
        }

        // where a Look event looks: the camera, a point, another actor (a little above where it stands)
        std::optional<glm::vec3> FindLookTarget(const Types::ActorEvent& event)
        {
            switch (event.lookTarget)
            {
                case Types::ActorLookTarget::Camera:
                    return Components::CameraManager::Get().GetActiveCamera()->GetPosition();
                case Types::ActorLookTarget::Point:
                    return event.lookPoint;
                case Types::ActorLookTarget::Actor:
                {
                    const auto other = std::ranges::find(scene, event.lookActor, &Types::Actor::id);
                    if (other == scene.end())
                        return std::nullopt;
                    return Components::Actors::EvaluatePlacement(*other, Components::Playback::GetActorTick())
                               .position +
                           glm::vec3(0.0f, 0.0f, 40.0f);
                }
                default:
                    return std::nullopt;
            }
        }

        // A killstreak's moving parts at a tick, where it stands and faces: its pose animation shown at full weight
        void PoseKillstreak(const Types::Actor& actor, const Components::KillstreakLogic::Info& info,
                            std::uint32_t tick, glm::vec3 position, float yaw,
                            const std::optional<Components::VehicleLogic::Body>& body, ActorLogic::TreeWeights& shown)
        {
            namespace Vehicles = Components::VehicleLogic;
            // each bone, turned from its own rotation: in its parent's axes (the wheels', so the left and right ones
            // roll alike) or its own (a gun's)
            struct Turn
            {
                std::string bone;
                glm::quat by{1.0f, 0.0f, 0.0f, 0.0f};
                bool inParent = false;
            };
            std::vector<Turn> turns;
            const auto toQuat = [](glm::vec3 angles) { return glm::quat_cast(MathUtils::AnglesToAxis(angles)); };
            for (const auto& joint : Vehicles::GetJoints(info, body.value_or(Vehicles::Body{})))
                turns.push_back({std::string(joint.bone), body ? toQuat(joint.angles) : glm::quat(1, 0, 0, 0), true});
            // its guns: at what its Look events look at (from a little above where it stands, the way it faces),
            // else scanning, else at rest
            const auto look = Components::Actors::GetLook(actor, tick);
            const bool aiming = look.from || look.to;
            const auto from = position + glm::vec3(0.0f, 0.0f, 30.0f);
            const auto unturn = glm::mat3(MathUtils::AnglesToAxis(glm::vec3(0.0f, yaw, 0.0f)));
            for (const auto& gun : info.guns)
            {
                const auto scan = Vehicles::GetIdleAim(gun, actor, tick);
                const auto aimAt = [&](const Types::ActorEvent* event) {
                    const auto target = event ? FindLookTarget(*event) : std::nullopt;
                    return target ? Vehicles::AimAt(gun, glm::transpose(unturn) * (*target - from))
                                  : scan.value_or(Vehicles::Aim{});
                };
                const auto aim = aiming ? Vehicles::Blend(aimAt(look.from), aimAt(look.to), look.turned)
                                        : scan.value_or(Vehicles::Aim{});
                turns.push_back({std::string(gun.yaw), toQuat(glm::vec3(0.0f, aim.yaw, 0.0f))});
                if (!gun.pitch.empty())
                    turns.push_back({std::string(gun.pitch), toQuat(glm::vec3(aim.pitch, 0.0f, 0.0f))});
            }
            if (!info.spin.empty() && !info.guns.empty())
                turns.push_back({std::string(info.spin),
                                 toQuat(glm::vec3(0.0f, 0.0f,
                                                  Vehicles::GetSpin(actor.events, tick, info.guns.front().fireTime)))});
            // its rotors, spinning at their rates (about the bone's up, a tail rotor's side) by the actor's time
            for (const auto& rotor : info.rotors)
            {
                const auto turned = std::fmod(static_cast<float>(tick) / 1000.0f * rotor.rate, 360.0f);
                turns.push_back({std::string(rotor.bone),
                                 toQuat(rotor.tail ? glm::vec3(turned, 0.0f, 0.0f) : glm::vec3(0.0f, turned, 0.0f))});
            }
            if (turns.empty() || actor.models.empty())
                return;
            std::vector<std::string> bones;
            for (const auto& turn : turns)
                bones.push_back(turn.bone);
            const auto pose = GetPoseAnimation(actor.id, actor.models.front(), bones);
            for (std::size_t i = 0; i < turns.size(); ++i)
            {
                const auto& rest = pose->rests[i];
                const auto q = glm::normalize(turns[i].inParent ? turns[i].by * rest : rest * turns[i].by);
                const std::array<float, 4> values{q.x, q.y, q.z, q.w};
                for (std::size_t c = 0; c < 4; ++c)
                    pose->quats[i * 4 + c] =
                        static_cast<std::int16_t>(std::lround(std::clamp(values[c], -1.0f, 1.0f) * 32767.0f));
            }
            shown.animations.push_back({.animation = pose->name, .time = 0.0f, .weight = 1.0f});
        }

        // A killstreak's effects that run while it is there (its lights), each bolted to its bone (FX_SpawnEffect
        // with its DObj's handle and the bone's index, as the game's playFxOnTag): spawned once, killed as its row
        // is turned off, its look changes, it is hidden or gone, or its DObj is made again
        struct RunningEffect
        {
            std::string effect;
            std::string tag;
            std::int32_t handle = 0;
            void* obj = nullptr;
        };
        std::map<std::uint32_t, std::vector<RunningEffect>> killstreakEffects;  // by actor id

        std::int32_t SpawnBoltedEffect(const void* def, const Drawn& drawn, const char* tag)
        {
            float axis[3][3]{}, origin[3]{};
            if (!def || !GetTag(drawn, tag, axis, origin) || drawn.models.empty())
                return 0;
            // (the bone's index in the DObj: of its first model, the killstreak's)
            const auto model = modelPointers.find(drawn.models.front());
            if (model == modelPointers.end())
                return 0;
            const auto boneNames = *reinterpret_cast<const std::uint16_t* const*>(model->second + 8);
            const auto count = CountBones(model->second);
            const auto id = static_cast<std::uint16_t>(ScriptString(tag));
            std::uint32_t bone = 0;
            for (std::uint32_t b = 0; boneNames && b < count; ++b)
                if (boneNames[b] == id)
                    bone = b;
            const auto system = *reinterpret_cast<void* const*>(GetGameAddresses().fx_system());
            const auto cg = *reinterpret_cast<const std::uint8_t* const*>(GetGameAddresses().cgArray());
            if (!system || !cg)
                return 0;
            const auto time = *reinterpret_cast<const std::int32_t*>(cg + 0x4808C);
            typedef std::int32_t(__cdecl * FX_SpawnEffect_t)(
                void* system, const void* def, std::int32_t msecBegin, const float* origin, const float(*axis)[3],
                std::uint32_t dobjHandle, std::uint32_t bone, std::uint32_t sortOrder, std::uint32_t owner,
                std::uint32_t markEntnum, const float* offset, std::int32_t, std::int32_t, std::int32_t, float scale,
                std::int32_t);
            static constexpr float NO_OFFSET[3] = {};
            return reinterpret_cast<FX_SpawnEffect_t>(GetGameAddresses().FX_SpawnEffect())(
                system, def, time, origin, axis, static_cast<std::uint32_t>(drawn.handle), bone, 0xFF, 0, 0x3FF,
                NO_OFFSET, 0, 0, -1, 1.0f, 0);
        }

        // The bones of a drawn model a tag stands for: itself, or for "*a|b|c" those whose names have the first of
        // a, b, c any has (a jet's engines, named as the game's artists named them), else its origin
        std::vector<std::string> ResolveTags(const Drawn& drawn, std::string_view tag)
        {
            if (!tag.starts_with('*'))
                return {std::string(tag)};
            std::vector<std::string> names;
            const auto model = drawn.models.empty() ? modelPointers.end() : modelPointers.find(drawn.models.front());
            const auto buffer = *reinterpret_cast<const std::uint8_t* const*>(GetGameAddresses().scrStrings());
            if (model != modelPointers.end() && buffer)
            {
                const auto boneNames = *reinterpret_cast<const std::uint16_t* const*>(model->second + 8);
                for (std::uint32_t b = 0; boneNames && b < CountBones(model->second); ++b)
                {
                    const auto text = reinterpret_cast<const char*>(buffer + 24u * boneNames[b] + 4);
                    if (!IsBadReadPtr(text, 1))
                        names.emplace_back(text, strnlen(text, 64));
                }
            }
            for (std::size_t from = 1; from <= tag.size();)
            {
                const auto bar = std::min(tag.find('|', from), tag.size());
                const auto part = tag.substr(from, bar - from);
                std::vector<std::string> found;
                for (const auto& name : names)
                    if (!part.empty() && name.find(part) != std::string::npos)
                        found.push_back(name);
                if (!found.empty())
                    return found;
                from = bar + 1;
            }
            return {"tag_origin"};
        }

        void RunKillstreakEffects(const Types::Actor& actor, const Components::KillstreakLogic::Info& info,
                                  const Drawn& drawn)
        {
            namespace Killstreaks = Components::KillstreakLogic;
            auto& running = killstreakEffects[actor.id];
            // (each on the bones its tag stands for: kept for the DObj, as they don't change with it)
            static std::map<std::pair<const void*, std::string>, std::vector<std::string>, std::less<>> resolved;
            std::vector<std::string> tagNames;  // owns the tags wanted's point into
            tagNames.reserve(64);
            std::vector<Killstreaks::Effect> wanted;
            for (const auto& effect : Killstreaks::GetEffects(info, Killstreaks::GetLook(info, actor.models.front())))
                if (Killstreaks::IsEffectRowOn(actor, effect.row))
                {
                    auto& tags = resolved[{drawn.obj, std::string(effect.tag)}];
                    if (tags.empty())
                    {
                        tags = ResolveTags(drawn, effect.tag);
                        if (effect.tag.starts_with('*'))
                            for (const auto& tag : tags)
                                LOG_DEBUG("T6: actor {}'s {} goes on {} ({})", actor.id, effect.effect, tag, effect.tag);
                    }
                    for (const auto& tag : tags)
                        if (tagNames.size() < tagNames.capacity())
                        {
                            tagNames.push_back(tag);
                            auto on = effect;
                            on.tag = tagNames.back();
                            wanted.push_back(on);
                        }
                }
            // (a helicopter's engine smoke as damaged as it is set to be)
            if (Killstreaks::CanBeDamaged(info))
                if (const auto smoke = Killstreaks::GetDamageEffect(actor.damage); !smoke.empty())
                    wanted.push_back({.effect = smoke, .tag = Killstreaks::DAMAGE_TAG});
            std::erase_if(running, [&](const RunningEffect& effect) {
                const bool keep = effect.obj == drawn.obj && std::ranges::any_of(wanted, [&](const auto& want) {
                                      return want.effect == effect.effect && want.tag == effect.tag;
                                  });
                if (!keep)
                    KillEffect(effect.handle, true);
                return !keep;
            });
            for (const auto& want : wanted)
            {
                if (std::ranges::any_of(running, [&](const RunningEffect& effect) {
                        return want.effect == effect.effect && want.tag == effect.tag;
                    }))
                    continue;
                const auto def = effectDefs.find(want.effect);
                if (def == effectDefs.end())
                    continue;
                const std::string tag(want.tag);
                const auto handle = SpawnBoltedEffect(def->second, drawn, tag.c_str());
                if (!handle)
                    continue;
                running.push_back({std::string(want.effect), tag, handle, drawn.obj});
                LOG_DEBUG("T6: actor {}'s {} on {} (effect {:#x})", actor.id, want.effect, tag, handle);
            }
        }

        void ForgetKillstreakEffects()
        {
            killstreakEffects.clear();
        }

        // those of actors not drawn this frame end
        void EndKillstreakEffects(const std::set<std::uint32_t>& drawnNow)
        {
            for (auto it = killstreakEffects.begin(); it != killstreakEffects.end();)
            {
                if (drawnNow.contains(it->first))
                {
                    ++it;
                    continue;
                }
                for (const auto& effect : it->second)
                    KillEffect(effect.handle, true);
                it = killstreakEffects.erase(it);
            }
        }

        // The RC-XD's treads (_rcbomb.csc): every TREAD_STEP it drives, the effects of its speed by the ground under
        // it (dust on earth, concrete on hard ground, snow), at its origin
        void PlayTreads(const Types::Actor& actor, const Components::KillstreakLogic::Info& info, const Drawn& drawn,
                        std::uint32_t from, std::uint32_t to)
        {
            namespace Killstreaks = Components::KillstreakLogic;
            namespace Vehicles = Components::VehicleLogic;
            if (info.vehicleClass != Types::VehicleClass::Car ||
                !Killstreaks::IsEffectRowOn(actor, Killstreaks::EffectRow::Treads))
                return;
            for (auto tick = (from / Vehicles::TREAD_STEP + 1) * Vehicles::TREAD_STEP; tick <= to;
                 tick += Vehicles::TREAD_STEP)
            {
                const auto body = Components::Actors::GetVehicleBody(actor, tick);
                if (!body)
                    continue;
                const auto at = Components::Actors::EvaluatePlacement(actor, tick).position;
                const auto ground =
                    Functions::TraceMapSurface(at + glm::vec3(0.0f, 0.0f, 8.0f), at - glm::vec3(0.0f, 0.0f, 40.0f));
                auto surface = ground ? Killstreaks::GetTreadSurface(ground->surface) : std::string_view();
                static std::set<std::uint32_t> loggedSurfaces;
                if (ground && loggedSurfaces.insert(ground->surface).second)
                    LOG_DEBUG("T6: the RC-XD's treads on surface {} ({}): {}", ground->surface,
                              Killstreaks::GetSurfaceName(ground->surface), surface.empty() ? "none" : surface);
                if (surface.empty())
                    continue;
                float axis[3][3]{}, origin[3]{};
                if (!GetTag(drawn, "tag_origin", axis, origin))
                    continue;
                for (const auto tread : Vehicles::GetTreads(info, *body))
                    if (const auto def = effectDefs.find(Vehicles::GetTreadEffect(surface, tread));
                        def != effectDefs.end())
                        PlayEffect(def->second, origin, axis);
            }
        }

    }  // namespace

    namespace
    {
        // A car lying on the ground under its four wheels (as T5's, traced from a little above to below each)
        void LayOnGround(const Components::KillstreakLogic::Wheels& wheels, glm::vec3& origin, glm::vec3& angles)
        {
            const auto yaw = glm::radians(angles.y);
            const glm::vec2 forward(std::cos(yaw), std::sin(yaw));
            const glm::vec2 left(-forward.y, forward.x);
            const auto ground = [&](float ahead, float aside) {
                const auto at = glm::vec2(origin) + forward * ahead + left * aside;
                const auto hit = Functions::TraceMap(glm::vec3(at, origin.z + 32.0f), glm::vec3(at, origin.z - 96.0f));
                return hit ? hit->z : origin.z;
            };
            const auto frontLeft = ground(wheels.front, wheels.track);
            const auto frontRight = ground(wheels.front, -wheels.track);
            const auto backLeft = ground(-wheels.back, wheels.track);
            const auto backRight = ground(-wheels.back, -wheels.track);
            angles.x = glm::degrees(
                std::atan2((backLeft + backRight - frontLeft - frontRight) / 2.0f, wheels.front + wheels.back));
            angles.z = glm::degrees(
                std::atan2((frontLeft + backLeft - frontRight - backRight) / 2.0f, 2.0f * std::max(wheels.track, 1.0f)));
            origin.z = (frontLeft + frontRight + backLeft + backRight) / 4.0f;
        }
    }  // namespace

    void AddToScene()
    {
        // (a frame much slower than the ones before, for the log: how long since the last, and how long the actors
        // took of the one before)
        const auto startedAt = std::chrono::steady_clock::now();
        static std::optional<std::chrono::steady_clock::time_point> lastStarted;
        static std::chrono::steady_clock::time_point lastLogged;
        static double lastTook = 0.0;
        if (lastStarted)
        {
            const auto gap = std::chrono::duration<double, std::milli>(startedAt - *lastStarted).count();
            if (gap > 100.0 && gap < 5000.0 && startedAt - lastLogged > std::chrono::seconds(2))
            {
                lastLogged = startedAt;
                LOG_DEBUG("T6: a slow frame: {:.0f} ms since the one before, whose actors took {:.1f} ms", gap,
                          lastTook);
            }
        }
        lastStarted = startedAt;
        struct Took
        {
            std::chrono::steady_clock::time_point from;
            ~Took()
            {
                lastTook = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
            }
        } took{startedAt};

        // (what a multi-pass capture's pass leaves out of the world)
        ApplyPassElements();
        // (none while a pack loads: the models and animations of the one before go)
        if (!loadingPack.empty())
            return;
        // (those retired long enough, before this frame's are drawn)
        FreeRetired(false);
        if (const auto generation = Components::Actors::GetGeneration(); sceneGeneration != generation)
        {
            sceneGeneration = generation;
            scene = Components::Actors::GetActors();
        }

        const auto tick = Components::Playback::GetActorTick();
        const auto keysTick = Components::Playback::GetTimelineTick();
        // (the events passed since the frame before, as the timeline plays on)
        const bool playEvents = eventTick && tick > *eventTick && tick - *eventTick <= MAX_EVENT_STEP;
        const auto eventsFrom = eventTick.value_or(tick);
        eventTick = tick;
        for (const auto& actor : scene)
        {
            if (!actor.placedProp || actor.hidden || tick < actor.placedProp->start || tick >= actor.placedProp->end)
                continue;
            const auto keyed = Components::Props::WithKeyedPlacement(actor, keysTick);
            const auto& prop = *keyed.placedProp;
            drawing = Drawing::Prop;
            Draw(actor.id, {prop.model}, keyed.position, glm::vec3(prop.pitch, keyed.yaw, prop.roll), prop.scale);
            drawing = Drawing::Game;
        }

        // the people (and the others with models): at their place, playing what their clips and legs show now
        const auto previewing = Components::Props::GetAnimationPreview();
        const auto catalogNow = Components::Actors::GetCatalog();
        std::erase_if(actorModels, [&](const auto& entry) {
            return std::ranges::find(scene, entry.first, &Types::Actor::id) == scene.end();
        });
        if (previewing != previewedAnimation)
        {
            previewedAnimation = previewing;
            previewStart = GetTickCount64();
        }
        std::set<std::uint32_t> lit;  // the killstreaks drawn, whose effects run on
        for (const auto& actor : scene)
        {
            if (actor.placedProp || actor.light || actor.effect || actor.mortar || actor.models.empty() ||
                !Components::Actors::IsVisible(actor, tick))
                continue;
            const auto placement = Components::Actors::EvaluatePlacement(actor, tick);
            const auto layers = Components::Actors::GetLayers(actor, tick, Components::Actors::GetBody(actor));
            auto shown = ActorLogic::WeighLayers(
                layers, [](const std::string&) { return PoseLogic::SpineKind::AtRest; });
            // (the animation browser's preview on the selected actor: the animation alone, playing from when it was
            // first hovered, over and over)
            // (once hovered a moment: going down the list doesn't make the actor's tree again for each one passed)
            constexpr std::uint64_t PREVIEW_DELAY = 250;
            if (previewing && Components::Actors::GetSelected() == actor.id && animationParts.contains(*previewing) &&
                GetTickCount64() - previewStart >= PREVIEW_DELAY)
            {
                const auto elapsed = static_cast<float>(GetTickCount64() - previewStart - PREVIEW_DELAY) / 1000.0f;
                shown.animations = {{.animation = *previewing, .time = elapsed, .weight = 1.0f}};
                // (an upper body one over the legs' idle, as a clip of it plays)
                // (a pt_ one over its own stance's legs: crouched, prone or standing, with its gun's)
                if (previewing->starts_with("pt_"))
                    if (const auto legs = StanceUnder(*previewing); animationParts.contains(legs))
                        shown.animations.push_back(
                            {.animation = legs, .time = elapsed, .weight = ActorLogic::LEGS_WEIGHT});
            }
            // (with no clip showing, a person stands in the players' idle, not its model's T of a rest pose)
            if (shown.animations.empty() && Components::Actors::GetActorKind(actor) == Types::ActorKind::Human)
                for (const auto idle : ActorLogic::IDLE_ANIMATIONS)
                    if (animationParts.contains(idle))
                    {
                        shown.animations.push_back({.animation = std::string(idle), .time = 0.0f, .weight = 1.0f});
                        break;
                    }
            // (what it shows, as that changes: for the log)
            {
                std::string names;
                for (const auto& entry : shown.animations)
                    names += std::format("{}{} {:.2f}", names.empty() ? "" : ", ",
                                         entry.clip ? entry.clip->animation : entry.animation, entry.weight);
                // (and its path's points)
                if (actor.movement == Types::ActorMovement::Path)
                {
                    names += " | path";
                    for (const auto& point : actor.path)
                        names += std::format(" ({:.0f} {:.0f} {:.0f})", point.x, point.y, point.z);
                }
                if (auto& last = lastShown[actor.id]; last != names)
                {
                    last = names;
                    LOG_DEBUG("T6: actor {} ({} clips) shows: {}", actor.id, actor.clips.size(),
                              names.empty() ? "nothing" : names);
                }
            }
            // (its gun in its right hand and the one on its back, by their world models: worked out again only as
            // the actors or the catalog change, not each frame)
            auto& own = actorModels[actor.id];
            const bool gunless = Components::Rendering::GetRenderingFlags() == Types::RenderingFlags_DrawPlayers &&
                                 !(Components::Rendering::GetGreenScreenKeeps() & Components::Rendering::GreenScreen_Guns);
            if (own.generation != sceneGeneration || own.catalog != catalogNow.get() || own.gunless != gunless)
            {
                own.generation = sceneGeneration;
                own.catalog = catalogNow.get();
                own.gunless = gunless;
                own.models = actor.models;
                if (!gunless && !actor.weapon.empty())
                    AddWeapon(own.models, actor.weapon, actor.attachments, actor.camo, "tag_weapon_right");
                if (!gunless && !actor.stowedWeapon.empty())
                    AddWeapon(own.models, actor.stowedWeapon, actor.stowedAttachments, actor.stowedCamo,
                              "tag_stowed_back");
            }
            // (its clips' animations, those starting soonest first)
            std::vector<std::string> upcoming;
            {
                std::vector<std::size_t> order(actor.clips.size());
                std::iota(order.begin(), order.end(), std::size_t{0});
                std::ranges::stable_sort(order, {}, [&](std::size_t i) {
                    const auto& clip = actor.clips[i];
                    return clip.start + clip.length < tick ? std::numeric_limits<std::uint32_t>::max() : clip.start;
                });
                for (const auto i : order)
                    if (std::ranges::find(upcoming, actor.clips[i].animation) == upcoming.end())
                        upcoming.push_back(actor.clips[i].animation);
            }
            // (a killstreak as the game's vehicle code moves it: a car on the ground under its wheels, leaning; a
            // helicopter tilted and drifting where it hovers; the care package falling from its drone)
            auto position = placement.position;
            glm::vec3 angles(0.0f, placement.yaw, 0.0f);
            // (one that shoots at another actor faces it)
            if (const auto target = GetShootTarget(actor, tick))
            {
                const auto toward = *target - position;
                if (glm::length(glm::vec2(toward)) > 1.0f)
                    angles.y = glm::degrees(std::atan2(toward.y, toward.x));
            }
            if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models))
            {
                const auto& info = Components::KillstreakLogic::Get(*killstreak);
                const auto body = Components::Actors::GetVehicleBody(actor, tick);
                if (body)
                {
                    if (info.vehicleClass == Types::VehicleClass::Car)
                        LayOnGround(info.wheels, position, angles);
                    else
                    {
                        position += body->drift;
                        angles.x = body->pitch;
                        angles.z = body->roll;
                    }
                }
                position.z += Components::VehicleLogic::GetDropHeight(actor, tick);
                PoseKillstreak(actor, info, tick, position, angles.y, body, shown);
            }
            drawing = Drawing::Person;
            Draw(actor.id, own.models, position, angles, 1.0f, glm::vec3(0.0f, 0.0f, 40.0f), &shown, &upcoming);
            drawing = Drawing::Game;
            // (its lights, and the RC-XD's treads as it drives)
            if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models))
                if (const auto drawn = drawnModels.find(actor.id); drawn != drawnModels.end() && drawn->second.drawn)
                {
                    const auto& info = Components::KillstreakLogic::Get(*killstreak);
                    RunKillstreakEffects(actor, info, drawn->second);
                    lit.insert(actor.id);
                    if (playEvents)
                        PlayTreads(actor, info, drawn->second, eventsFrom, tick);
                }
            if (playEvents)
                if (const auto drawn = drawnModels.find(actor.id); drawn != drawnModels.end())
                    for (const auto& fired : Components::Actors::GetFiredEvents(actor, eventsFrom, tick))
                        PlayEvent(actor, drawn->second, *fired.event);
        }

        // the scene's effects, on their own
        PlaySceneEffects(tick);

        // the browser's preview, in front of the camera as a prop put in the scene goes
        if (const auto preview = Components::Props::GetPreview())
        {
            drawing = Drawing::Prop;
            Draw(PREVIEW_KEY, {preview->model}, Functions::FindPlace(preview->origin, preview->direction, PLACE_AHEAD),
                 glm::vec3(0.0f, preview->yaw, 0.0f), 1.0f);
            drawing = Drawing::Game;
        }

        EndKillstreakEffects(lit);
        // those not drawn this frame go
        std::erase_if(drawnModels, [](auto& entry) {
            auto& drawn = entry.second;
            const bool gone = !drawn.drawn;
            drawn.drawn = false;
            if (gone)
                Retire(drawn);
            return gone;
        });
    }

    void ReleaseAll()
    {
        StopSceneEffects();
        for (const auto& [key, drawn] : drawnModels)
            Release(drawn);
        drawnModels.clear();
        FreeRetired(true);
    }

    std::optional<Types::AnimationPose> GetAnimationPose(std::string_view animation, std::string_view model, float time)
    {
        // (none while a pack loads: the assets go and come)
        if (packBusy)
            return std::nullopt;
        std::shared_lock lock(catalogMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return std::nullopt;
        const auto parts = animationParts.find(animation);
        const auto found = modelPointers.find(model);
        if (parts == animationParts.end() || found == modelPointers.end())
            return std::nullopt;
        // (an upper body pt_ one over its stance's legs, as it plays over them: prone, crouched or standing)
        const bool upperBody = *(parts->second + PARTS_ASSET_TYPE) == ASSET_TYPE_UPPER_BODY;
        const auto stance = upperBody ? StanceUnder(animation) : std::string();
        const auto underParts = upperBody ? animationParts.find(stance) : animationParts.end();
        const auto body = ViewModel(found->second);
        if (!body)
            return std::nullopt;

        std::lock_guard poses(poseMutex);
        const auto key = std::pair(std::string(animation), std::string(model));
        auto it = decodedPoses.find(key);
        if (it == decodedPoses.end())
        {
            if (decodedPoses.size() > 64)
                decodedPoses.clear();
            it = decodedPoses.emplace(key, Decode(parts->second, *body)).first;
        }
        if (!it->second)
            return std::nullopt;
        const auto& decoded = *it->second;
        const DecodedPose* under = nullptr;
        if (underParts != animationParts.end())
        {
            const auto underKey = std::pair(stance, std::string(model));
            auto below = decodedPoses.find(underKey);
            if (below == decodedPoses.end())
                below = decodedPoses.emplace(underKey, Decode(underParts->second, *body)).first;
            if (below->second)
                under = &*below->second;
        }

        const float frame = std::clamp(time, 0.0f, 1.0f) * decoded.numFrames;
        auto pose = PoseAt(decoded, *body, frame, under);
        // (tags far off a bone with nothing below them would draw as long lines out of the body: left out)
        constexpr float HELPER_TAG_DISTANCE = 25.0f;
        std::vector<bool> hasChildren(body->numBones);
        for (const auto parent : pose.parents)
            if (parent >= 0)
                hasChildren[parent] = true;
        for (std::uint32_t b = body->numRootBones; b < body->numBones; ++b)
        {
            const auto t = body->trans + (b - body->numRootBones) * body->transStride;
            if (!hasChildren[b] && glm::length(glm::vec3(t[0], t[1], t[2])) > HELPER_TAG_DISTANCE)
                pose.parents[b] = -1;
        }
        return pose;
    }
}  // namespace IWXMVM::T6::Props
