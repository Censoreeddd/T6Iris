#pragma once
#include <cmath>
#include "Types/ScreenTrack.hpp"

namespace IWXMVM::Types
{
    struct DoF
    {
        bool enabled;
        float farBlur;
        float farStart;
        float farEnd;
        float nearBlur;
        float nearStart;
        float nearEnd;
        float bias;

        // The kind of blur, for games that offer the others (Features_ScreenBlur): the game's depth of field, or the
        // mod's own over the whole picture. A number so it keys like the rest (held from key to key).
        enum BlurType
        {
            BLUR_DEPTH = 0,
            BLUR_DIRECTIONAL = 1,  // smeared along a direction, as if moving fast
            BLUR_RADIAL = 2,       // streaked out from a point, as if zooming
            BLUR_LENS = 3,         // a camera lens's: sharp at the focus distance, bokeh elsewhere (Features_LensBlur)
        };
        float blurType = static_cast<float>(BLUR_DEPTH);
        // The Blur section's: none, or the directional or radial (BLUR_DIRECTIONAL, BLUR_RADIAL) over the whole
        // picture, on whether the depth of field is or not, and drawn over it
        float screenBlur = 0.0f;

        // the depth of field's kind, BLUR_DEPTH or BLUR_LENS (a project's older directional or radial blurType is the
        // Blur section's now, the game's depth of field off with it as it was)
        int GetDepthBlur() const
        {
            const auto type = static_cast<int>(std::round(blurType));
            return type == BLUR_LENS ? BLUR_LENS : BLUR_DEPTH;
        }
        // the Blur section's kind, 0 none: its own, else an older project's directional or radial blurType (while the
        // depth of field is on, as it was then)
        int GetScreenBlur() const
        {
            const auto own = static_cast<int>(std::round(screenBlur));
            if (own == BLUR_DIRECTIONAL || own == BLUR_RADIAL)
                return own;
            const auto type = static_cast<int>(std::round(blurType));
            return enabled && (type == BLUR_DIRECTIONAL || type == BLUR_RADIAL) ? type : 0;
        }
        // whether the depth of field's blurType is an older project's directional or radial one
        bool HasOlderBlurType() const
        {
            const auto type = static_cast<int>(std::round(blurType));
            return type == BLUR_DIRECTIONAL || type == BLUR_RADIAL;
        }
        float blurStrength = 0.05f;    // how far it smears, as a part of the picture's height
        float blurAngle = 0.0f;        // directional: degrees, 0 to the right, 90 up
        float blurCenterX = 0.5f;      // radial: where it streaks out from, 0 to 1 across
        float blurCenterY = 0.5f;      // and down
        float blurClearRadius = 0.1f;  // radial: how far out from the centre stays sharp, as a part of the height

        // the lens: what's sharp, in the game's units; the lens's focal length in mm (on a full frame camera) and
        // its aperture's f-number, which say how fast the blur grows away from the focus; the bokeh's shape (0 round,
        // else the number of aperture blades) and how much bright spots stand out in it
        float focusDistance = 300.0f;
        float focalLength = 50.0f;
        float aperture = 2.8f;
        float bokehBlades = 0.0f;
        float bokehHighlights = 1.0f;
        // how much it blurs (times the lens's own) and how big the blur and its discs may grow, as a part of the
        // picture's height
        float blurAmount = 1.0f;
        float bokehSize = 0.04f;
        // the pin on the picture (0 to 1 across and down): with Follow Point the focus is whatever is under it each
        // frame, else a pick there sets the Focus Distance once
        enum FocusMode
        {
            FOCUS_DISTANCE = 0,
            FOCUS_FOLLOW_POINT = 1,
            FOCUS_TRACK = 2,  // on what Components::Tracking's focus track follows, as far away as it is
        };
        float focusMode = static_cast<float>(FOCUS_DISTANCE);
        float focusPointX = 0.5f;
        float focusPointY = 0.5f;
        // Follow Pin: about how many seconds of the film the focus takes to get to what's under the pin (0 at once)
        float focusSmoothing = 0.3f;
        // (not a setting: the picture shows what's in focus while the section is open with it on)
        bool showFocus = false;
    };
}  // namespace IWXMVM::Types