#pragma once
#include "Types/ScreenTrack.hpp"

// The editor over a game that renders with Direct3D 11 (T6). Core's D3D9 module hooks the device's EndScene; a D3D11
// game has none, so this hooks the swap chain's Present and ResizeBuffers, found through a swap chain of its own made
// for a moment (whose functions in dxgi.dll the game's swap chain shares). The game's device, context and window come
// from its swap chain the first time it presents.
//
// What this covers so far: the editor's UI, the game view (the game's frame copied into a texture the editor shows),
// images and capture's reading of the frame. Not yet: the 3D drawing of gizmos (GFX), the depth buffer, ReShade.
namespace IWXMVM::D3D11
{
    // Hooks Present and ResizeBuffers. Any thread, before or after the game made its swap chain.
    void Initialize();

    // The game's window and device, null before its first frame
    HWND FindWindowHandle();
    ID3D11Device* GetDevice();
    ID3D11DeviceContext* GetContext();
    // the size of the back buffer of the frame being drawn; none outside a frame
    std::optional<ImVec2> GetBackBufferSize();

    // A copy of the game's frame for the game view, sized from the back buffer (ImGui scales it when it draws)
    struct GameViewTexture
    {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    // Render thread, inside the frame: copies the back buffer into the texture, made again when the back buffer's
    // size or format changed (multisampled back buffers are resolved)
    bool CaptureBackBuffer(GameViewTexture& target);
    void Release(GameViewTexture& target);

    // ImGui::Image without blending: the game's frame has whatever alpha it was drawn with, which the image would
    // otherwise be see-through by
    void DrawOpaqueImage(ImTextureID texture, ImVec2 size);

    // An image file (PNG and the like) in memory as a texture ImGui can draw; render thread
    bool CreateTextureFromMemory(const std::uint8_t* data, std::uint32_t size, ImTextureID& texture,
                                 ImVec2& dimensions);
    void ReleaseTexture(ImTextureID texture);

    // For a capture, render thread, inside the frame: the back buffer read back to memory (resolved when multisampled,
    // through a staging texture kept until ReleaseFrameReading), handed to use as rows of 4 bytes a pixel, rowPitch
    // apart. False when it couldn't be read, or isn't 8 bits a channel.
    bool ReadFrame(const std::function<void(const std::uint8_t* pixels, std::uint32_t rowPitch, std::uint32_t width,
                                            std::uint32_t height)>& use);
    void ReleaseFrameReading();
    // the channels' order in what ReadFrame hands over, as ffmpeg names it ("rgba" or "bgra"); the frame's back buffer's
    std::string_view GetFramePixelFormat();

    // A blur of the mod's over the game's whole frame (Features_ScreenBlur), drawn into the back buffer before the
    // frame is captured or the editor drawn (and before ReShade's effects), so recordings have it and the editor's UI
    // doesn't. Any thread.
    struct ScreenBlur
    {
        enum Type
        {
            NONE = 0,
            DIRECTIONAL = 1,
            RADIAL = 2,
            LENS = 3,  // by depth, as a camera lens: needs depth
        };
        Type type = NONE;
        float strength = 0.0f;     // how far it smears, as a part of the frame's height
        float angle = 0.0f;        // directional: degrees, 0 to the right, 90 up
        float centerX = 0.5f;      // radial: where it streaks out from, 0 to 1 across and down
        float centerY = 0.5f;
        float clearRadius = 0.0f;  // radial: how far out stays sharp, as a part of the frame's height

        // lens: what's sharp, in the depth's units; the focal length (mm, full frame) and f-number; the bokeh's blades
        // (under 3 round) and how much bright spots stand out; the picture tinted by focus instead (not while a
        // capture runs)
        float focusDistance = 300.0f;
        float focalLength = 50.0f;
        float aperture = 2.8f;
        float blades = 0.0f;
        float highlights = 1.0f;
        float amount = 1.0f;     // times the lens's blur
        float maxSize = 0.04f;   // the largest blur radius, as a part of the frame's height
        // the pin (0 to 1 across and down), where the depth is read back (GetPointDepth); with followPoint the focus
        // is the depth there each frame instead of focusDistance
        float pointX = 0.5f;
        float pointY = 0.5f;
        bool followPoint = false;
        float smoothing = 0.0f;  // followPoint: seconds of the film the focus takes to follow (0 at once)
        bool showFocus = false;
        // lens: the focus on what Components::Tracking's focus track follows: its distance along the view each frame,
        // the pin on it
        bool trackFocus = false;
        // where the game keeps the view of its linear depth (each pixel's distance from the camera, in its units),
        // read each frame (the game makes it again with its renderer)
        ID3D11ShaderResourceView* const* depth = nullptr;
        // and of its depth buffer, read when the linear depth has nothing (as zNear / (1 - z), or zNear / z when
        // reversed, found from where most of the picture is)
        ID3D11ShaderResourceView* const* hardwareDepth = nullptr;
        float zNear = 4.0f;
    };
    void SetScreenBlur(const ScreenBlur& blur);
    // The Blur section's directional or radial blur (its type NONE, DIRECTIONAL or RADIAL), drawn after the screen
    // blur (the depth of field's Lens) and the flare, so it smears both
    void SetPictureBlur(const ScreenBlur& blur);
    // the depth at the pin while the lens blur is on, a few frames old; none otherwise
    std::optional<float> GetPointDepth();
    // where the lens's focus follows something, the pin on the picture it's at (none while nothing or not in view)
    std::optional<glm::vec2> GetTrackedFocus();
    // The depth (the game's units, along the view) at a spot of the picture, read from the depth buffer the lens flare
    // has (Components::LensFlare::GetDepthBuffer): asked for, then there a few frames later (none until then, or
    // where nothing is drawn). Any thread.
    void RequestDepthAt(glm::vec2 uv);
    std::optional<float> GetDepthAt(glm::vec2 uv);

    // As D3D9::ReleaseForUnload: the render thread lets go of the UI at its next Present, after which the hooks only
    // pass through
    bool ReleaseForUnload(std::chrono::milliseconds timeout);
}  // namespace IWXMVM::D3D11
