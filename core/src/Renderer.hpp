#pragma once
#include "Types/GraphicsApi.hpp"
#include "D3D11.hpp"

// What the editor's UI needs from the graphics API, for a D3D9 game (D3D9) and a D3D11 one (D3D11) alike. The D3D9
// side calls exactly what the UI called before, so the D3D9 games are unchanged.
namespace IWXMVM::Renderer
{
    Types::GraphicsApi GetApi();

    // Hooks the game's rendering (D3D9::Initialize or D3D11::Initialize)
    void Initialize();
    bool ReleaseForUnload(std::chrono::milliseconds timeout);

    HWND FindWindowHandle();
    bool IsReshadePresent();

    // ImGui's renderer backend. device is a D3D9 game's; a D3D11 game's comes from its swap chain.
    void InitImGuiBackend(IDirect3DDevice9* device);
    void ImGuiNewFrame();
    void RenderImGui(ImDrawData* drawData);
    void InvalidateImGuiDeviceObjects();
    void ShutdownImGuiBackend();
    // the size of the back buffer being drawn to
    std::optional<ImVec2> GetBackBufferSize();

    // The game's frame as the game view shows it
    class GameViewTexture
    {
       public:
        // for the size it's shown at (D3D9 copies the frame to that size; D3D11 copies it whole and scales as it draws)
        void Resize(ImVec2 size);
        bool Capture();
        void Draw(ImVec2 size);
        void Release();

       private:
        IDirect3DTexture9* d3d9 = nullptr;
        D3D11::GameViewTexture d3d11{};
    };

    // an image file in memory, as a texture ImGui can draw
    ImTextureID CreateTextureFromMemory(const std::uint8_t* data, std::uint32_t size, ImVec2& dimensions);
    void ReleaseTexture(ImTextureID texture);
}  // namespace IWXMVM::Renderer
