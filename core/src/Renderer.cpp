#include "StdInclude.hpp"
#include "Renderer.hpp"

#include "D3D9.hpp"
#include "D3D11.hpp"
#include "Mod.hpp"

namespace IWXMVM::Renderer
{
    Types::GraphicsApi GetApi()
    {
        return Mod::GetGameInterface()->GetGraphicsApi();
    }

    namespace
    {
        bool IsD3D11()
        {
            return GetApi() == Types::GraphicsApi::D3D11;
        }
    }  // namespace

    void Initialize()
    {
        if (IsD3D11())
            D3D11::Initialize();
        else
            D3D9::Initialize();
    }

    bool ReleaseForUnload(std::chrono::milliseconds timeout)
    {
        return IsD3D11() ? D3D11::ReleaseForUnload(timeout) : D3D9::ReleaseForUnload(timeout);
    }

    HWND FindWindowHandle()
    {
        return IsD3D11() ? D3D11::FindWindowHandle() : D3D9::FindWindowHandle();
    }

    bool IsReshadePresent()
    {
        // ReShade with a D3D11 game isn't supported yet
        return IsD3D11() ? false : D3D9::IsReshadePresent();
    }

    void InitImGuiBackend(IDirect3DDevice9* device)
    {
        if (IsD3D11())
        {
            LOG_DEBUG("Initializing ImGui_ImplDX11 with D3D11 Device {0:x}",
                      reinterpret_cast<std::uintptr_t>(D3D11::GetDevice()));
            ImGui_ImplDX11_Init(D3D11::GetDevice(), D3D11::GetContext());
        }
        else
        {
            LOG_DEBUG("Initializing ImGui_ImplDX9 with D3D9 Device {0:x}", reinterpret_cast<std::uintptr_t>(device));
            ImGui_ImplDX9_Init(device);
        }
    }

    void ImGuiNewFrame()
    {
        if (IsD3D11())
            ImGui_ImplDX11_NewFrame();
        else
            ImGui_ImplDX9_NewFrame();
    }

    void RenderImGui(ImDrawData* drawData)
    {
        if (IsD3D11())
            ImGui_ImplDX11_RenderDrawData(drawData);
        else
            ImGui_ImplDX9_RenderDrawData(drawData);
    }

    void InvalidateImGuiDeviceObjects()
    {
        if (IsD3D11())
            ImGui_ImplDX11_InvalidateDeviceObjects();
        else
            ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    void ShutdownImGuiBackend()
    {
        if (IsD3D11())
            ImGui_ImplDX11_Shutdown();
        else
            ImGui_ImplDX9_Shutdown();
    }

    std::optional<ImVec2> GetBackBufferSize()
    {
        if (IsD3D11())
            return D3D11::GetBackBufferSize();

        IDirect3DSurface9* backBuffer = nullptr;
        const auto device = D3D9::GetDevice();
        if (!device || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
            return std::nullopt;

        D3DSURFACE_DESC desc{};
        const auto result = backBuffer->GetDesc(&desc);
        backBuffer->Release();
        if (FAILED(result))
            return std::nullopt;
        return ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    }

    void GameViewTexture::Resize(ImVec2 size)
    {
        if (!IsD3D11())
            D3D9::CreateTexture(d3d9, size);
    }

    bool GameViewTexture::Capture()
    {
        // While a D3D11 game's theater seeks (T6 jumps back to a snapshot and plays on fast to the time), the game view
        // keeps the frame from before it, and shows the new time once it's there: a seek looks like a jump
        // (not while the timeline is dragged: then the film shows as it goes, near the time under the mouse)
        const auto game = Mod::GetGameInterface();
        if (IsD3D11() && d3d11.view && game->IsDemoSeeking() && !game->IsDemoScrubbing())
            return true;
        return IsD3D11() ? D3D11::CaptureBackBuffer(d3d11) : D3D9::CaptureBackBuffer(d3d9);
    }

    void GameViewTexture::Draw(ImVec2 size)
    {
        if (IsD3D11())
        {
            if (d3d11.view)
                D3D11::DrawOpaqueImage(d3d11.view, size);
            else
                ImGui::Dummy(size);
        }
        else
        {
            ImGui::Image(reinterpret_cast<void*>(d3d9), size);
        }
    }

    void GameViewTexture::Release()
    {
        if (d3d9)
        {
            d3d9->Release();
            d3d9 = nullptr;
        }
        D3D11::Release(d3d11);
    }

    ImTextureID CreateTextureFromMemory(const std::uint8_t* data, std::uint32_t size, ImVec2& dimensions)
    {
        if (IsD3D11())
        {
            ImTextureID texture = nullptr;
            if (!D3D11::CreateTextureFromMemory(data, size, texture, dimensions))
                throw std::runtime_error("failed to load image");
            return texture;
        }

        PDIRECT3DTEXTURE9 texture = nullptr;
        if (D3DXCreateTextureFromFileInMemory(D3D9::GetDevice(), data, size, &texture) != S_OK)
            throw std::runtime_error("failed to load image");

        D3DSURFACE_DESC imageDesc{};
        texture->GetLevelDesc(0, &imageDesc);
        dimensions = ImVec2(static_cast<float>(imageDesc.Width), static_cast<float>(imageDesc.Height));
        return texture;
    }

    void ReleaseTexture(ImTextureID texture)
    {
        if (!texture)
            return;
        if (IsD3D11())
            D3D11::ReleaseTexture(texture);
        else
            static_cast<IDirect3DTexture9*>(texture)->Release();
    }
}  // namespace IWXMVM::Renderer
