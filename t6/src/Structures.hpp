#pragma once
#include "StdInclude.hpp"

namespace IWXMVM::T6::Structures
{
    union DvarValue
    {
        bool enabled;
        std::int32_t integer;
        std::uint32_t unsignedInt;
        float value;
        float vector[4];
        const char* string;
        std::uint8_t color[4];
    };
    static_assert(sizeof(DvarValue) == 0x10);

    // T6 numbers them from 1, unlike T5 (bool 0). Seen in t6mpv43.exe: Dvar_RegisterBool passes 1 (in_mouse,
    // cl_ingame), Dvar_RegisterFloat 2 (cg_fov, timescale), the vec3 one 4 (r_lightTweakSunDirection), the int one 6
    // (com_maxfps), the enum one 7 (r_mode). The rest follow that order.
    enum dvarType_t : std::int32_t
    {
        DVAR_TYPE_INVALID = 0,
        DVAR_TYPE_BOOL = 1,
        DVAR_TYPE_FLOAT = 2,
        DVAR_TYPE_FLOAT_2 = 3,
        DVAR_TYPE_FLOAT_3 = 4,
        DVAR_TYPE_FLOAT_4 = 5,
        DVAR_TYPE_INT = 6,
        DVAR_TYPE_ENUM = 7,
        DVAR_TYPE_STRING = 8,
        DVAR_TYPE_COLOR = 9,
        // (10 int64); three floats of a colour in linear light, as the renderer takes it: r_lightTweakSunColor is one,
        // registered at 0x5C5050 as type 11
        DVAR_TYPE_LINEAR_COLOR_RGB = 11,
    };

    // As T5's up to the values; the hash chain's link moved (Dvar_FindMalleableVar, 0x8C88E0). Dvar_GetBool reads
    // type at 0x10 and the value at 0x18.
    struct dvar_t
    {
        const char* name;          // 0x00
        std::uint8_t _unk04[4];    // 0x04
        std::int32_t hash;         // 0x08
        std::uint32_t flags;       // 0x0C
        std::int32_t type;         // 0x10, dvarType_t
        bool modified;             // 0x14
        std::uint8_t _unk15[3];    // 0x15
        DvarValue current;         // 0x18
        DvarValue latched;         // 0x28
        DvarValue reset;           // 0x38
        std::uint8_t _unk48[0x10]; // 0x48
        dvar_t* hashNext;          // 0x58
    };
    static_assert(offsetof(dvar_t, hash) == 0x08);
    static_assert(offsetof(dvar_t, type) == 0x10);
    static_assert(offsetof(dvar_t, current) == 0x18);
    static_assert(offsetof(dvar_t, hashNext) == 0x58);

    // The view CG_CalcViewValues builds (0x7D7120), in cg ([0x113F18C]) at 0x4D8A0. Like T5's refdef_t up to the
    // fov: here it's at 0x20 (cg_fov, 65, seen in the game) and the origin follows it at 0x24. CG_CalcViewValues
    // writes the origin at 0x24-0x2C, the view angles to cg+0x648F0 and the axis from them (AnglesToAxis, 0x571A10).
    // tan(fov/2) for x and y were 0.8494 and 0.4778 on a 16:9 screen (cg_fov 65, which BO2 widens to ~80.7 degrees).
    struct refdef_t
    {
        std::int32_t x;              // 0x00
        std::int32_t y;              // 0x04
        std::int32_t width;          // 0x08
        std::int32_t height;         // 0x0C
        float tanHalfFovX;           // 0x10
        float tanHalfFovY;           // 0x14
        std::uint8_t _unk18[8];      // 0x18
        float fov;                   // 0x20
        float vieworg[3];            // 0x24
        std::uint8_t _unk30[4];      // 0x30
        float viewaxis[3][3];        // 0x34
    };
    static_assert(offsetof(refdef_t, tanHalfFovX) == 0x10);
    static_assert(offsetof(refdef_t, fov) == 0x20);
    static_assert(offsetof(refdef_t, vieworg) == 0x24);
    static_assert(offsetof(refdef_t, viewaxis) == 0x34);

    constexpr std::uintptr_t CG_REFDEF = 0x4D8A0;
    constexpr std::uintptr_t CG_REFDEF_VIEW_ANGLES = 0x648F0;

    // the theater's camera, in its playback structure (the pointer at 0x3183A00): the mode the change-camera key
    // cycles through (first person, third person, free), and the free camera's kind (2 the dolly camera), as the menus'
    // demoisanyfreemovecamera (0x6D7B20) and demoisdollycamera (0x63D6A0) read them
    constexpr std::uintptr_t THEATER_CAMERA_MODE = 0xD4C00;
    constexpr std::uintptr_t THEATER_FREE_CAMERA_KIND = 0xD4C04;
    enum TheaterCameraMode : std::int32_t
    {
        THEATER_CAMERA_FIRST_PERSON = 0,
        THEATER_CAMERA_THIRD_PERSON = 1,
        THEATER_CAMERA_FREE = 2,
    };

    // KEYCATCH_* as T5 has them; console, UI and chat are tested the same way in T6's CL_MouseEvent and key code
    constexpr std::int32_t KEYCATCH_CONSOLE = 0x1;
    constexpr std::int32_t KEYCATCH_UI = 0x10;
    constexpr std::int32_t KEYCATCH_MESSAGE = 0x20;
}  // namespace IWXMVM::T6::Structures
