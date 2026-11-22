#include "StdInclude.hpp"
#include "Filmtweaks.hpp"

#include "Addresses.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T6::Filmtweaks
{
    namespace
    {
        // refdef + 0x90: the grade, as the frame has it at +0x24 (0x72B7B0's rep movsd of 0x44 dwords)
        constexpr std::uintptr_t REFDEF_FILM = 0x90;
        constexpr std::uintptr_t FILM_ENABLE = 0x00;
        constexpr std::uintptr_t FILM_VECTORS = 0x04;
        enum Vector : std::size_t
        {
            RANGE_START = 0,
            RANGE_END = 1,
            SHADOWS = 2,     // three rows: red, green, blue
            MIDTONES = 5,
            HIGHLIGHTS = 8,
            FINAL_GAIN = 11,
            FINAL_SATURATION = 12,
            FINAL_BLEND = 13,
            VECTOR_COUNT = 14,
        };

        std::mutex mutex;
        Types::Filmtweaks settings{};
        bool loggedMap = false;

        glm::vec4* Vectors(std::uint8_t* film)
        {
            return reinterpret_cast<glm::vec4*>(film + FILM_VECTORS);
        }

        using Rows = std::array<glm::vec4, 3>;  // three rows of (weights, offset): out[row] = dot(w, c) + offset

        // the luma the map's grade uses (its final saturation's weights)
        constexpr glm::vec3 LUMA(0.2126f, 0.7152f, 0.0722f);

        Rows Identity()
        {
            Rows rows{};
            for (int row = 0; row < 3; ++row)
                rows[row][row] = 1.0f;
            return rows;
        }

        // a then b: out = b(a(c))
        Rows Then(const Rows& a, const Rows& b)
        {
            Rows result{};
            for (int j = 0; j < 3; ++j)
            {
                glm::vec4 row(0.0f, 0.0f, 0.0f, b[j].w);
                for (int i = 0; i < 3; ++i)
                    row += b[j][i] * a[i];
                result[j] = row;
            }
            return result;
        }

        // towards the luma (0) or away from it (above 1)
        Rows Saturation(float saturation)
        {
            Rows rows{};
            for (int row = 0; row < 3; ++row)
            {
                glm::vec3 weights = LUMA * (1.0f - saturation);
                weights[row] += saturation;
                rows[row] = glm::vec4(weights, 0.0f);
            }
            return rows;
        }

        // the colours turned about the grey axis, keeping their luma
        Rows Hue(float degrees)
        {
            const float c = std::cos(glm::radians(degrees));
            const float s = std::sin(glm::radians(degrees));
            const float r = LUMA.r, g = LUMA.g, b = LUMA.b;
            return {
                glm::vec4(r + c * (1 - r) - s * r, g - c * g - s * g, b - c * b + s * (1 - b), 0.0f),
                glm::vec4(r - c * r + s * 0.143f, g + c * (1 - g) + s * 0.140f, b - c * b - s * 0.283f, 0.0f),
                glm::vec4(r - c * r - s * (1 - r), g - c * g + s * g, b + c * (1 - b) + s * b, 0.0f),
            };
        }

        // a black body's colour (Tanner Helland's fit), 0 to 1 per channel
        glm::vec3 Kelvin(float kelvin)
        {
            const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
            glm::vec3 color;
            color.r = t <= 66 ? 255.0f : 329.698727446f * std::pow(t - 60, -0.1332047592f);
            color.g = t <= 66 ? 99.4708025861f * std::log(t) - 161.1195681661f
                              : 288.1221695283f * std::pow(t - 60, -0.0755148492f);
            color.b = t >= 66 ? 255.0f : (t <= 19 ? 0.0f : 138.5177312231f * std::log(t - 10) - 305.0447927307f);
            return glm::clamp(color / 255.0f, 0.0f, 1.0f);
        }

        // white balanced for 6500 K: lower is warmer, higher cooler; the luma kept
        Rows Temperature(float kelvin)
        {
            glm::vec3 gain = Kelvin(kelvin) / glm::max(Kelvin(6500.0f), glm::vec3(0.001f));
            gain /= std::max(glm::dot(gain, LUMA), 0.001f);
            Rows rows{};
            for (int row = 0; row < 3; ++row)
                rows[row][row] = gain[row];
            return rows;
        }

        // about the middle grey
        Rows Contrast(float contrast)
        {
            Rows rows{};
            for (int row = 0; row < 3; ++row)
            {
                rows[row][row] = contrast;
                rows[row].w = 0.5f * (1.0f - contrast);
            }
            return rows;
        }

        Rows Gain(glm::vec3 gain)
        {
            Rows rows{};
            for (int row = 0; row < 3; ++row)
                rows[row][row] = gain[row];
            return rows;
        }

        // the editor's grade of a range (0 shadows, 1 midtones, 2 highlights): saturation (with the bleach's, a bleach
        // bypass: less colour, more contrast), hue, temperature, contrast, the range's tint, then the brightness and
        // invert of all ranges
        Rows EditorMatrix(const Types::Filmtweaks& f, int range)
        {
            const std::array tints = {f.tintDark, f.tintMid, f.tintLight};
            const float bleach = std::clamp(f.bleachRanges[range], 0.0f, 1.0f);
            Rows rows = Saturation(f.saturationRanges[range] * (1.0f - 0.7f * bleach));
            rows = Then(rows, Hue(f.hueRanges[range]));
            rows = Then(rows, Temperature(f.colorTemperatureRanges[range]));
            rows = Then(rows, Contrast(f.contrastRanges[range] * (1.0f + 0.5f * bleach)));
            rows = Then(rows, Gain(tints[range]));
            for (auto& row : rows)
                row.w += f.brightness;
            if (f.invert)
            {
                for (auto& row : rows)
                {
                    row = -row;
                    row.w += 1.0f;
                }
            }
            return rows;
        }

        // ours after the map's
        void Compose(glm::vec4* mapRows, const Rows& editor)
        {
            const Rows result = Then({mapRows[0], mapRows[1], mapRows[2]}, editor);
            for (int j = 0; j < 3; ++j)
                mapRows[j] = result[j];
        }

        void Apply(std::uint8_t* film)
        {
            Types::Filmtweaks f;
            {
                std::lock_guard lock(mutex);
                f = settings;
            }
            if (!f.enabled)
                return;

            auto vectors = Vectors(film);
            const bool mapGrades = film[FILM_ENABLE] != 0;
            if (!loggedMap)
            {
                loggedMap = true;
                std::string values;
                for (std::size_t i = 0; i < VECTOR_COUNT; ++i)
                    values += std::format(" ({:.2f} {:.2f} {:.2f} {:.2f})", vectors[i].x, vectors[i].y, vectors[i].z,
                                          vectors[i].w);
                LOG_DEBUG("T6: the map's grade: enable {}, {}", film[FILM_ENABLE], values);
            }
            // A map without its own grade (enable 0): its grade turned on as neutral. The values it has are kept when
            // it has them (the maps logged have a neutral grade there: each range's matrix the identity, the final
            // gain and blend (1 1 1 0)), its final saturation made whole (w 1). Only a map with none gets values of
            // the same shape. (A friend's map without a grade went black with the vc_ dvars' defaults put in: their
            // final blend and gain were 0 where a map has 1.)
            if (!mapGrades)
            {
                bool hasValues = false;
                for (std::size_t i = SHADOWS; i < FINAL_GAIN; ++i)
                    hasValues = hasValues || vectors[i] != glm::vec4(0.0f);
                if (!hasValues)
                {
                    vectors[RANGE_START] = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
                    vectors[RANGE_END] = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);
                    for (const auto range : {SHADOWS, MIDTONES, HIGHLIGHTS})
                        for (int row = 0; row < 3; ++row)
                        {
                            glm::vec4 identity(0.0f);
                            identity[row] = 1.0f;
                            vectors[range + row] = identity;
                        }
                    vectors[FINAL_GAIN] = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
                    vectors[FINAL_SATURATION] = glm::vec4(0.2126f, 0.7152f, 0.0722f, 1.0f);
                    vectors[FINAL_BLEND] = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
                }
                vectors[FINAL_SATURATION].w = 1.0f;
                film[FILM_ENABLE] = 1;
            }
            // each range graded on its own; where the ranges meet stays the map's
            Compose(&vectors[SHADOWS], EditorMatrix(f, 0));
            Compose(&vectors[MIDTONES], EditorMatrix(f, 1));
            Compose(&vectors[HIGHLIGHTS], EditorMatrix(f, 2));
        }

        typedef void(__cdecl* SetupFilm_t)(void* frame, std::uint8_t* refdef, void* other);
        SetupFilm_t SetupFilm_Trampoline = nullptr;
        // the grade copied (0x44 dwords): changed for the call, put back after it, so it's never changed twice
        constexpr std::size_t FILM_SIZE = 0x44 * 4;
        void __cdecl SetupFilm_Hook(void* frame, std::uint8_t* refdef, void* other)
        {
            if (!refdef)
            {
                SetupFilm_Trampoline(frame, refdef, other);
                return;
            }
            std::array<std::uint8_t, FILM_SIZE> original;
            std::memcpy(original.data(), refdef + REFDEF_FILM, FILM_SIZE);
            Apply(refdef + REFDEF_FILM);
            SetupFilm_Trampoline(frame, refdef, other);
            std::memcpy(refdef + REFDEF_FILM, original.data(), FILM_SIZE);
        }
    }  // namespace

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().setupFilm(), reinterpret_cast<std::uintptr_t>(SetupFilm_Hook),
                                reinterpret_cast<std::uintptr_t*>(&SetupFilm_Trampoline));
    }

    void Set(const Types::Filmtweaks& filmtweaks)
    {
        std::lock_guard lock(mutex);
        if (filmtweaks.enabled && !settings.enabled)
            loggedMap = false;
        settings = filmtweaks;
    }
}  // namespace IWXMVM::T6::Filmtweaks
