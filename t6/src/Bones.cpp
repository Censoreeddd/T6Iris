#include "StdInclude.hpp"
#include "Bones.hpp"

#include "Addresses.hpp"
#include "Mod.hpp"

namespace IWXMVM::T6::Bones
{
    namespace
    {
        constexpr std::size_t ENTITY_SIZE = 0x380;
        constexpr std::size_t ENTITY_NUMBER = 0x1DC;
        constexpr std::size_t ENTITY_TYPE = 0x2B4;
        constexpr std::int16_t ET_PLAYER = 1;
        // what was asked for is looked up for this many updates after it was last asked for
        constexpr int KEEP_UPDATES = 120;

        std::atomic<DWORD> clientThread = 0;

        std::mutex mutex;
        using BoneKey = std::pair<std::int32_t, std::string>;
        std::map<BoneKey, int> watched;            // client thread
        std::set<BoneKey> requests;                // under mutex
        std::map<BoneKey, Types::BoneData> found;  // under mutex
        std::array<std::string, MAX_CLIENTS> names;  // under mutex, empty for one not there
        std::uint64_t namesReadAt = 0;              // client thread

        void ReadNames()
        {
            const auto now = GetTickCount64();
            if (now - namesReadAt < 500)
                return;
            namesReadAt = now;

            typedef int(__cdecl * CG_GetPlayerName_t)(int localClientNum, int clientNum, char* out, int size,
                                                      bool withClan);
            const auto CG_GetPlayerName = reinterpret_cast<CG_GetPlayerName_t>(GetGameAddresses().CG_GetPlayerName());
            std::array<std::string, MAX_CLIENTS> read;
            for (std::int32_t i = 0; i < MAX_CLIENTS; ++i)
            {
                char name[64]{};
                if (CG_GetPlayerName(0, i, name, sizeof(name) - 1, true))
                    read[i] = std::string(name, strnlen(name, sizeof(name)));
            }
            std::lock_guard lock(mutex);
            names = std::move(read);
        }

        std::uintptr_t GetEntity(std::int32_t id)
        {
            if (id < 0 || id >= MAX_ENTITIES)
                return 0;
            const auto entities = *reinterpret_cast<const std::uintptr_t*>(GetGameAddresses().cgEntities());
            return entities ? entities + static_cast<std::uintptr_t>(id) * ENTITY_SIZE : 0;
        }

        void* GetDObj(std::uintptr_t entity)
        {
            typedef void*(__cdecl * Com_GetClientDObj_t)(int handle, int localClientNum);
            const auto Com_GetClientDObj = reinterpret_cast<Com_GetClientDObj_t>(GetGameAddresses().Com_GetClientDObj());
            return Com_GetClientDObj(*reinterpret_cast<const std::int32_t*>(entity + ENTITY_NUMBER), 0);
        }

        Types::BoneData LookUp(std::int32_t entityId, const std::string& name)
        {
            const auto entity = GetEntity(entityId);
            if (!entity || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return {.id = -1};
            const auto obj = GetDObj(entity);
            if (!obj)
                return {.id = -1};

            typedef std::uint32_t(__cdecl * SL_FindString_t)(const char* text);
            const auto SL_FindString = reinterpret_cast<SL_FindString_t>(GetGameAddresses().SL_FindString());
            const auto tag = SL_FindString(name.c_str()) & 0xFFFF;
            if (!tag)
                return {.id = -1};

            typedef int(__cdecl * CG_DObjGetWorldTagMatrix_t)(void* pose, void* obj, std::uint32_t tagName,
                                                               float (*axis)[3], float* origin);
            const auto CG_DObjGetWorldTagMatrix =
                reinterpret_cast<CG_DObjGetWorldTagMatrix_t>(GetGameAddresses().CG_DObjGetWorldTagMatrix());
            float axis[3][3]{};
            float origin[3]{};
            if (!CG_DObjGetWorldTagMatrix(reinterpret_cast<void*>(entity), obj, tag, axis, origin))
                return {.id = -1};
            return {.id = static_cast<std::int32_t>(tag), .position = glm::make_vec3(origin),
                    .rotation = glm::make_mat3(&axis[0][0])};
        }
    }  // namespace

    std::vector<Types::Entity> GetEntities()
    {
        std::vector<Types::Entity> entities;
        entities.reserve(MAX_ENTITIES);
        const bool onClient = GetCurrentThreadId() == clientThread.load();
        for (std::int32_t i = 0; i < MAX_ENTITIES; ++i)
        {
            const auto entity = GetEntity(i);
            if (!entity)
            {
                entities.push_back({.id = i, .type = Types::EntityType::Unsupported, .clientNum = -1});
                continue;
            }
            const auto type = *reinterpret_cast<const std::int16_t*>(entity + ENTITY_TYPE);
            const bool player = type == ET_PLAYER && i < MAX_CLIENTS;
            // (whether it has a model is the client thread's to ask; elsewhere a player is taken as there)
            const bool valid = player && (!onClient || GetDObj(entity) != nullptr);
            entities.push_back({.id = i,
                                .type = player ? Types::EntityType::Player : Types::EntityType::Unsupported,
                                .clientNum = player ? i : -1,
                                .isValid = valid});
        }
        return entities;
    }

    Types::BoneData GetBoneData(std::int32_t entityId, const std::string& name)
    {
        if (GetCurrentThreadId() == clientThread.load())
            return LookUp(entityId, name);

        std::lock_guard lock(mutex);
        BoneKey key{entityId, name};
        requests.insert(key);
        const auto result = found.find(key);
        return result != found.end() ? result->second : Types::BoneData{.id = -1};
    }

    std::optional<std::string> GetPlayerName(std::int32_t clientNum)
    {
        if (clientNum < 0 || clientNum >= MAX_CLIENTS)
            return std::nullopt;
        std::lock_guard lock(mutex);
        return names[clientNum].empty() ? std::nullopt : std::optional<std::string>(names[clientNum]);
    }

    void Update()
    {
        clientThread = GetCurrentThreadId();
        ReadNames();

        std::set<BoneKey> asked;
        {
            std::lock_guard lock(mutex);
            asked.swap(requests);
        }
        for (auto it = watched.begin(); it != watched.end();)
            it = ++it->second > KEEP_UPDATES ? watched.erase(it) : std::next(it);
        for (const auto& key : asked)
            watched[key] = 0;

        std::map<BoneKey, Types::BoneData> results;
        for (const auto& [key, age] : watched)
        {
            results[key] = LookUp(key.first, key.second);
            // (once a bone, the first time it's asked for: whether it was found)
            static std::set<BoneKey> logged;
            if (logged.insert(key).second)
            {
                const auto& bone = results[key];
                if (bone.id == -1)
                    LOG_DEBUG("T6: entity {} has no bone {} now", key.first, key.second);
                else
                    LOG_DEBUG("T6: entity {}'s {} is at ({:.1f}, {:.1f}, {:.1f})", key.first, key.second,
                              bone.position.x, bone.position.y, bone.position.z);
            }
        }

        std::lock_guard lock(mutex);
        found.swap(results);
    }
}  // namespace IWXMVM::T6::Bones
