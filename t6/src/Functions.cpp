#include "StdInclude.hpp"
#include "Functions.hpp"

#include "Addresses.hpp"

namespace IWXMVM::T6::Functions
{
    Structures::dvar_t* FindDvar(std::string_view name)
    {
        const std::string nameString{name};
        const char* namePointer = nameString.c_str();
        const auto Dvar_FindVar = GetGameAddresses().Dvar_FindVar();
        Structures::dvar_t* dvar = nullptr;

        // Dvar_FindVar takes the name in eax; it saves esi and edi itself
#if defined(_MSC_VER)
        __asm
        {
            mov eax, namePointer
            call Dvar_FindVar
            mov dvar, eax
        }
#else
        asm volatile("call *%1" : "=a"(dvar) : "r"(Dvar_FindVar), "a"(namePointer) : "ecx", "edx", "memory", "cc");
#endif
        return dvar;
    }

    void Cbuf_AddText(std::string command)
    {
        LOG_DEBUG("Executing command \"{0}\"", command);
        command.append("\n");

        typedef void(__cdecl * Cbuf_AddText_t)(int localClientNum, const char* text);
        const auto Cbuf_AddText = reinterpret_cast<Cbuf_AddText_t>(GetGameAddresses().Cbuf_AddText());
        Cbuf_AddText(0, command.c_str());
    }

    std::int32_t GetKeyCatchers()
    {
        return *reinterpret_cast<const volatile std::int32_t*>(GetGameAddresses().keyCatchers());
    }

    namespace
    {
        std::optional<float> ReadNumber(const Structures::dvar_t& dvar, const Structures::DvarValue& value)
        {
            switch (dvar.type)
            {
                case Structures::DVAR_TYPE_BOOL:
                    return value.enabled ? 1.0f : 0.0f;
                case Structures::DVAR_TYPE_INT:
                case Structures::DVAR_TYPE_ENUM:
                    return static_cast<float>(value.integer);
                case Structures::DVAR_TYPE_FLOAT:
                    return value.value;
                case Structures::DVAR_TYPE_STRING:
                    if (value.string && (value.string[0] == '0' || value.string[0] == '1') && !value.string[1])
                        return value.string[0] == '1' ? 1.0f : 0.0f;
                    return std::nullopt;
                default:
                    return std::nullopt;
            }
        }
    }  // namespace

    std::optional<float> GetDvarNumber(std::string_view name)
    {
        const auto dvar = FindDvar(name);
        return dvar ? ReadNumber(*dvar, dvar->current) : std::nullopt;
    }

    std::optional<float> GetDvarDefault(std::string_view name)
    {
        const auto dvar = FindDvar(name);
        return dvar ? ReadNumber(*dvar, dvar->reset) : std::nullopt;
    }

    bool SetDvarNumber(std::string_view name, float value)
    {
        const auto dvar = FindDvar(name);
        if (!dvar)
            return false;
        switch (dvar->type)
        {
            case Structures::DVAR_TYPE_BOOL:
                if (dvar->current.enabled != (value != 0.0f))
                {
                    dvar->current.enabled = value != 0.0f;
                    dvar->modified = true;
                }
                return true;
            case Structures::DVAR_TYPE_INT:
            case Structures::DVAR_TYPE_ENUM:
                if (dvar->current.integer != static_cast<std::int32_t>(value))
                {
                    dvar->current.integer = static_cast<std::int32_t>(value);
                    dvar->modified = true;
                }
                return true;
            case Structures::DVAR_TYPE_FLOAT:
                if (dvar->current.value != value)
                {
                    dvar->current.value = value;
                    dvar->modified = true;
                }
                return true;
            case Structures::DVAR_TYPE_STRING:
            {
                // through the console, which makes the game's own copy of the string
                // (once until the game has run it: it's asked for every frame)
                static std::map<std::string, std::pair<bool, std::uint64_t>> queued;
                const auto now = ReadNumber(*dvar, dvar->current);
                const bool wanted = value != 0.0f;
                if (now && (*now != 0.0f) != wanted)
                {
                    auto& last = queued[std::string(name)];
                    if (last.first != wanted || GetTickCount64() - last.second > 1000)
                    {
                        last = {wanted, GetTickCount64()};
                        Cbuf_AddText(std::format("set {} {}", dvar->name, wanted ? 1 : 0));
                    }
                }
                return now.has_value();
            }
            default:
                return false;
        }
    }

    namespace
    {
        // sRGB and linear light, both ways (above 1 too: a map's sun can be brighter than white)
        float ToLinear(float gamma)
        {
            gamma = std::max(gamma, 0.0f);
            return gamma <= 0.04045f ? gamma / 12.92f : std::pow((gamma + 0.055f) / 1.055f, 2.4f);
        }

        float ToGamma(float linear)
        {
            linear = std::max(linear, 0.0f);
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }
    }  // namespace

    std::optional<glm::vec3> GetDvarVec3(std::string_view name, bool reset)
    {
        const auto dvar = FindDvar(name);
        if (!dvar)
            return std::nullopt;
        const auto& value = reset ? dvar->reset : dvar->current;
        switch (dvar->type)
        {
            case Structures::DVAR_TYPE_FLOAT_3:
            case Structures::DVAR_TYPE_FLOAT_4:
                return glm::vec3(value.vector[0], value.vector[1], value.vector[2]);
            case Structures::DVAR_TYPE_COLOR:
                return glm::vec3(value.color[0], value.color[1], value.color[2]) / 255.0f;
            case Structures::DVAR_TYPE_LINEAR_COLOR_RGB:
                // (as a colour row has it, sRGB)
                return glm::vec3(ToGamma(value.vector[0]), ToGamma(value.vector[1]), ToGamma(value.vector[2]));
            default:
                return std::nullopt;
        }
    }

    bool SetDvarVec3(std::string_view name, glm::vec3 value)
    {
        const auto dvar = FindDvar(name);
        if (!dvar)
            return false;
        bool changed = false;
        switch (dvar->type)
        {
            case Structures::DVAR_TYPE_FLOAT_3:
            case Structures::DVAR_TYPE_FLOAT_4:
                for (int i = 0; i < 3; ++i)
                {
                    changed |= dvar->current.vector[i] != value[i];
                    dvar->current.vector[i] = value[i];
                }
                break;
            case Structures::DVAR_TYPE_COLOR:
                for (int i = 0; i < 3; ++i)
                {
                    const auto byte = static_cast<std::uint8_t>(std::clamp(value[i], 0.0f, 1.0f) * 255.0f + 0.5f);
                    changed |= dvar->current.color[i] != byte;
                    dvar->current.color[i] = byte;
                }
                break;
            case Structures::DVAR_TYPE_LINEAR_COLOR_RGB:
                for (int i = 0; i < 3; ++i)
                {
                    const auto linear = ToLinear(value[i]);
                    changed |= dvar->current.vector[i] != linear;
                    dvar->current.vector[i] = linear;
                }
                break;
            default:
                return false;
        }
        // (the renderer takes a changed sun from its dvars when they're marked modified, 0x712460)
        if (changed)
            dvar->modified = true;
        return true;
    }
    bool SetDvarVec4(std::string_view name, glm::vec4 value)
    {
        const auto dvar = FindDvar(name);
        if (!dvar || dvar->type != Structures::DVAR_TYPE_FLOAT_4)
            return false;
        bool changed = false;
        for (int i = 0; i < 4; ++i)
        {
            changed |= dvar->current.vector[i] != value[i];
            dvar->current.vector[i] = value[i];
        }
        if (changed)
            dvar->modified = true;
        return true;
    }
    std::optional<MapHit> TraceMapSurface(glm::vec3 start, glm::vec3 end)
    {
        constexpr std::int32_t ENTITYNUM_NONE = 0x3FF;
        constexpr std::int32_t MASK_SHOT_NO_CHARACTERS = 0x806833;
        constexpr std::size_t TRACE_FRACTION = 0x10, TRACE_SURFACE_FLAGS = 0x14;
        typedef void(__cdecl * CG_LocationalTrace_t)(void* results, const float* start, const float* end,
                                                     std::int32_t passEntityNum, std::int32_t contentMask, int, int);
        alignas(16) std::uint8_t results[0x100]{};
        reinterpret_cast<CG_LocationalTrace_t>(GetGameAddresses().CG_LocationalTrace())(
            results, &start.x, &end.x, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS, 0, 0);
        const auto fraction = *reinterpret_cast<const float*>(results + TRACE_FRACTION);
        if (!std::isfinite(fraction) || fraction >= 1.0f || fraction < 0.0f)
            return std::nullopt;
        const auto flags = *reinterpret_cast<const std::uint32_t*>(results + TRACE_SURFACE_FLAGS);
        return MapHit{start + (end - start) * fraction, (flags >> 20) & 0x1F};
    }

    std::optional<glm::vec3> TraceMap(glm::vec3 start, glm::vec3 end)
    {
        constexpr std::int32_t ENTITYNUM_NONE = 0x3FF;
        constexpr std::int32_t MASK_SHOT_NO_CHARACTERS = 0x806833;
        constexpr std::size_t TRACE_FRACTION = 0x10;
        typedef void(__cdecl * CG_LocationalTrace_t)(void* results, const float* start, const float* end,
                                                     std::int32_t passEntityNum, std::int32_t contentMask, int, int);
        // (room to spare: T6's trace_t is a little longer than T5's 0x38 bytes)
        alignas(16) std::uint8_t results[0x100]{};
        reinterpret_cast<CG_LocationalTrace_t>(GetGameAddresses().CG_LocationalTrace())(
            results, &start.x, &end.x, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS, 0, 0);
        const auto fraction = *reinterpret_cast<const float*>(results + TRACE_FRACTION);
        if (!std::isfinite(fraction) || fraction >= 1.0f || fraction < 0.0f)
            return std::nullopt;
        return start + (end - start) * fraction;
    }

    glm::vec3 FindPlace(glm::vec3 origin, glm::vec3 direction, float ahead, float reach)
    {
        const auto along = glm::length(direction) > 0.0f ? glm::normalize(direction) : glm::vec3(1.0f, 0.0f, 0.0f);
        if (const auto hit = TraceMap(origin, origin + along * reach))
            return *hit;
        return origin + along * ahead;
    }

}  // namespace IWXMVM::T6::Functions
