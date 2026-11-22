#include "StdInclude.hpp"
#include "D3D11.hpp"

#include "MinHook.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include "Mod.hpp"
#include "ReShade.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/Playback.hpp"
#include "Components/LensFlare.hpp"
#include "Components/Tracking.hpp"
#include "Components/SkyReplace.hpp"
#include "Components/LensDirt.hpp"
#include "Components/Lens.hpp"
#include "Components/Rendering.hpp"
#include "Utilities/HdrImage.hpp"
#include "UI/UIManager.hpp"
#include "UI/Components/CaptureMenu.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::D3D11
{
    namespace
    {
        // IDXGISwapChain's vtable: IUnknown (3), IDXGIObject (4), IDXGIDeviceSubObject (1), then Present and on
        constexpr std::size_t PRESENT_INDEX = 8;
        constexpr std::size_t RESIZE_BUFFERS_INDEX = 13;

        typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
        Present_t Present = nullptr;
        typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain* swapChain, UINT bufferCount, UINT width,
                                                     UINT height, DXGI_FORMAT format, UINT flags);
        ResizeBuffers_t ResizeBuffers = nullptr;

        // The game's, all touched on its render thread only (Present, ResizeBuffers), once the first frame came
        IDXGISwapChain* gameSwapChain = nullptr;  // not held: the game owns it
        std::atomic<HWND> gameWindow = nullptr;
        ID3D11Device* device = nullptr;          // held
        ID3D11DeviceContext* context = nullptr;  // held
        ID3D11RenderTargetView* backBufferView = nullptr;
        ID3D11BlendState* opaqueBlendState = nullptr;
        // the game view's picture, scaled with a Catmull-Rom filter (sharper than ImGui's bilinear one, as the
        // picture is shown smaller than the game draws it); none if it couldn't be made, and ImGui's is used
        ID3D11PixelShader* gameViewShader = nullptr;
        bool gameViewShaderTried = false;
        // what a capture reads the frame back through
        ID3D11Texture2D* readStaging = nullptr;
        ID3D11Texture2D* readResolved = nullptr;
        D3D11_TEXTURE2D_DESC readDesc{};
        // the swap chain of the frame being drawn, null outside one
        IDXGISwapChain* frameSwapChain = nullptr;

        // The screen blur (SetScreenBlur): its settings, and what draws it, the frame copied into a texture it reads
        std::mutex screenBlurMutex;
        ScreenBlur screenBlur{};
        ScreenBlur pictureBlur{};  // the Blur section's (SetPictureBlur)
        ID3D11VertexShader* blurVertexShader = nullptr;
        ID3D11PixelShader* blurPixelShader = nullptr;
        // the lens's three passes (half size: its colour and blur, then the gather; then the full size), and their
        // two half size textures
        ID3D11PixelShader* lensPixelShader = nullptr;  // the composite
        // a multi-pass capture's Depth and players' passes (DrawStreamPass)
        ID3D11PixelShader* depthPassShader = nullptr;
        ID3D11PixelShader* greenPassShader = nullptr;
        ID3D11PixelShader* normalPassShader = nullptr;
        bool passShadersTried = false;
        ID3D11PixelShader* lensPrepareShader = nullptr;
        ID3D11PixelShader* lensGatherShader = nullptr;
        ID3D11PixelShader* lensTileMaxShader = nullptr;
        ID3D11PixelShader* lensTileDilateShader = nullptr;
        ID3D11Texture2D* lensHalfTextures[2]{};
        ID3D11RenderTargetView* lensHalfTargets[2]{};
        ID3D11ShaderResourceView* lensHalfViews[2]{};
        UINT lensHalfWidth = 0, lensHalfHeight = 0;
        // the most blur of each 8 x 8 tile of the half size pixels, and of what reaches each
        ID3D11Texture2D* lensTileTextures[2]{};
        ID3D11RenderTargetView* lensTileTargets[2]{};
        ID3D11ShaderResourceView* lensTileViews[2]{};
        UINT lensTileWidth = 0, lensTileHeight = 0;
        // Follow Pin's smoothed focus: two 1 x 1 textures, each frame's step drawn from one into the other
        ID3D11PixelShader* focusStepShader = nullptr;
        ID3D11Texture2D* focusTextures[2]{};
        ID3D11RenderTargetView* focusTargets[2]{};
        ID3D11ShaderResourceView* focusViews[2]{};
        int focusCurrent = 0;
        bool focusValid = false;
        std::uint32_t focusLastTick = 0;
        std::uint64_t focusLastTime = 0;
        // the depth at the pin (GetPointDepth), the centre and a grid, drawn by the probe shader into an 18 x 1 target and
        // read back a few frames late; which depth the lens reads (BlurConstants::depthMode), decided from it
        constexpr UINT DEPTH_SAMPLES = 18;
        ID3D11PixelShader* probeShader = nullptr;
        ID3D11Texture2D* probeTexture = nullptr;
        ID3D11RenderTargetView* probeTarget = nullptr;
        ID3D11Texture2D* probeStaging = nullptr;
        bool probePending = false;
        int depthMode = -1;  // none decided
        std::uint64_t depthLoggedAt = 0;
        std::atomic<float> centerDepth = 0.0f;
        std::atomic<std::uint64_t> centerDepthAt = 0;
        // the spot the probe drawn last read (pending), and the one of the depth read back (probedAt)
        glm::vec2 probingAt{-1.0f};
        std::mutex probeMutex;
        glm::vec2 probedAt{-1.0f};
        // a spot asked for (RequestDepthAt), none while nothing is
        std::optional<glm::vec2> depthRequest;
        std::uint64_t lastProbeSent = 0;  // when a probe was drawn last (the lens's, a pick's or a look at the depth)
        // the lens's focus following something: where it is on the picture, and its last depth (kept while it's gone)
        std::optional<glm::vec2> trackedFocus;
        float trackedFocusDepth = 0.0f;
        ID3D11SamplerState* pointSampler = nullptr;

        // The sky (Components::SkyReplace): its shader, and the image loaded (again when its path changes)
        bool skyTried = false;
        ID3D11VertexShader* skyVertexShader = nullptr;
        ID3D11PixelShader* skyPixelShader = nullptr;
        ID3D11Buffer* skyConstants = nullptr;
        ID3D11SamplerState* skyPanoramaSampler = nullptr;  // wrapped across, clamped up and down
        ID3D11SamplerState* skyPhotoSampler = nullptr;     // mirrored across, clamped up and down
        ID3D11Texture2D* skyImage = nullptr;
        ID3D11ShaderResourceView* skyImageView = nullptr;
        std::string skyImagePath;  // the one loaded (or tried)
        UINT skyImageWidth = 0, skyImageHeight = 0;
        bool skyImageHdr = false;  // linear light (an HDRI): exposed and tone mapped

        // The lens (Components::Lens): its shader, drawn last over the frame
        bool lensTried = false;
        ID3D11PixelShader* lensBendShader = nullptr;
        ID3D11Buffer* lensBendConstants = nullptr;

        // The lens dirt (Components::LensDirt): its shader, the frame copied with its mips (blurred, for the light on
        // it) and the image loaded (again when its path changes)
        bool dirtTried = false;
        ID3D11PixelShader* dirtPixelShader = nullptr;
        ID3D11Buffer* dirtConstants = nullptr;
        ID3D11Texture2D* dirtGlow = nullptr;
        ID3D11ShaderResourceView* dirtGlowView = nullptr;
        D3D11_TEXTURE2D_DESC dirtGlowDesc{};
        bool dirtGlowFailed = false;  // the frame's format can't have its mips made: lit by the flare alone
        ID3D11ShaderResourceView* dirtImageView = nullptr;
        std::string dirtImagePath;
        UINT dirtImageWidth = 0, dirtImageHeight = 0;

        // The lens flare (Components::LensFlare): drawn added to the frame
        bool flareTried = false;
        ID3D11VertexShader* flareVertexShader = nullptr;
        ID3D11PixelShader* flarePixelShader = nullptr;
        ID3D11Buffer* flareConstants = nullptr;
        ID3D11BlendState* flareBlend = nullptr;  // added
        // Auto's search for bright lights: the frame in a grid of cells, then the brightest apart
        ID3D11PixelShader* flareDetectShader = nullptr;
        ID3D11PixelShader* flarePickShader = nullptr;
        ID3D11Texture2D* flareGridTexture = nullptr;
        ID3D11RenderTargetView* flareGridTarget = nullptr;
        ID3D11ShaderResourceView* flareGridView = nullptr;
        ID3D11Texture2D* flareLightsTexture = nullptr;
        ID3D11RenderTargetView* flareLightsTarget = nullptr;
        ID3D11ShaderResourceView* flareLightsView = nullptr;
        // the lights followed, two in turn: the last frame's read while this frame's is drawn
        ID3D11PixelShader* flareTrackShader = nullptr;
        ID3D11Texture2D* flareTrackedTextures[2]{};
        ID3D11RenderTargetView* flareTrackedTargets[2]{};
        ID3D11ShaderResourceView* flareTrackedViews[2]{};
        int flareTrackedCurrent = 0;
        bool flareTrackedValid = false;
        std::uint32_t flareTrackedTick = 0;
        std::uint64_t flareTrackedTime = 0;

        // The shutter (CaptureManager::ShutterSettings): the frames of a recorded frame averaged in linear light into
        // the accumulation (each blended in by 1 / its number), and for a shutter longer than a frame the recorded
        // frames' averages kept in a ring, the last ones blended for the frame recorded; in the editor an estimate
        bool shutterTried = false;
        ID3D11VertexShader* shutterVertexShader = nullptr;
        ID3D11PixelShader* shutterInShader = nullptr;
        ID3D11PixelShader* shutterCopyShader = nullptr;
        ID3D11PixelShader* shutterOutShader = nullptr;
        ID3D11Buffer* shutterConstants = nullptr;
        ID3D11BlendState* shutterBlend = nullptr;  // by the blend factor
        struct ShutterTarget
        {
            ID3D11Texture2D* texture = nullptr;
            ID3D11RenderTargetView* target = nullptr;
            ID3D11ShaderResourceView* view = nullptr;
        };
        ShutterTarget shutterAccumulation;
        std::array<ShutterTarget, Components::MAX_SHUTTER_FRAMES> shutterRing;
        UINT shutterWidth = 0, shutterHeight = 0;
        std::int32_t shutterRingNext = 0, shutterRingCount = 0;
        std::uint32_t shutterPreviewTick = 0;
        bool shutterPreviewValid = false;
        ID3D11Buffer* blurConstants = nullptr;
        ID3D11SamplerState* blurSampler = nullptr;
        ID3D11RasterizerState* blurRasterizer = nullptr;
        ID3D11DepthStencilState* blurDepthState = nullptr;
        bool blurShadersTried = false;
        ID3D11Texture2D* blurFrame = nullptr;
        ID3D11ShaderResourceView* blurFrameView = nullptr;
        D3D11_TEXTURE2D_DESC blurFrameDesc{};

        std::atomic<bool> unloadRequested = false;
        std::atomic<bool> releasedForUnload = false;
        // ReShade, when it's in the game (its DLL exports ReShadeRegisterAddon); looked for at the first frame
        bool reshadeChecked = false;
        std::atomic<bool> reshadePresent = false;

        template <typename T>
        void SafeRelease(T*& object)
        {
            if (object)
            {
                object->Release();
                object = nullptr;
            }
        }

        // The format a view of a back buffer of this format reads it in: a typeless one has none of its own
        DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
        {
            switch (format)
            {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case DXGI_FORMAT_B8G8R8X8_TYPELESS:
                    return DXGI_FORMAT_B8G8R8X8_UNORM;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                default:
                    return format;
            }
        }

        void ReleaseBackBufferView()
        {
            SafeRelease(backBufferView);
        }

        bool CreateBackBufferView(IDXGISwapChain* swapChain)
        {
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return false;

            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);

            D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.Format = ViewFormat(desc.Format);
            viewDesc.ViewDimension =
                desc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
            const auto result = device->CreateRenderTargetView(backBuffer, &viewDesc, &backBufferView);
            backBuffer->Release();
            if (FAILED(result))
            {
                LOG_ERROR("Couldn't make a render target of the back buffer ({:#x})", static_cast<std::uint32_t>(result));
                return false;
            }
            return true;
        }

        void ReleaseScreenBlur()
        {
            SafeRelease(blurVertexShader);
            SafeRelease(blurPixelShader);
            SafeRelease(lensPixelShader);
            SafeRelease(lensPrepareShader);
            SafeRelease(lensGatherShader);
            SafeRelease(lensTileMaxShader);
            SafeRelease(lensTileDilateShader);
            for (int i = 0; i < 2; ++i)
            {
                SafeRelease(lensHalfViews[i]);
                SafeRelease(lensHalfTargets[i]);
                SafeRelease(lensHalfTextures[i]);
            }
            lensHalfWidth = lensHalfHeight = 0;
            for (int i = 0; i < 2; ++i)
            {
                SafeRelease(lensTileViews[i]);
                SafeRelease(lensTileTargets[i]);
                SafeRelease(lensTileTextures[i]);
            }
            lensTileWidth = lensTileHeight = 0;
            SafeRelease(focusStepShader);
            for (int i = 0; i < 2; ++i)
            {
                SafeRelease(focusViews[i]);
                SafeRelease(focusTargets[i]);
                SafeRelease(focusTextures[i]);
            }
            focusValid = false;
            SafeRelease(probeShader);
            SafeRelease(depthPassShader);
            SafeRelease(greenPassShader);
            SafeRelease(normalPassShader);
            passShadersTried = false;
            SafeRelease(probeTexture);
            SafeRelease(probeTarget);
            SafeRelease(probeStaging);
            SafeRelease(pointSampler);
            probePending = false;
            depthMode = -1;
            SafeRelease(blurConstants);
            SafeRelease(blurSampler);
            SafeRelease(blurRasterizer);
            SafeRelease(blurDepthState);
            SafeRelease(blurFrameView);
            SafeRelease(blurFrame);
            blurFrameDesc = {};
            blurShadersTried = false;
        }

        void ReleaseShutterTarget(ShutterTarget& target)
        {
            SafeRelease(target.view);
            SafeRelease(target.target);
            SafeRelease(target.texture);
        }

        void ReleaseShutterTargets()
        {
            ReleaseShutterTarget(shutterAccumulation);
            for (auto& target : shutterRing)
                ReleaseShutterTarget(target);
            shutterWidth = shutterHeight = 0;
            shutterRingNext = shutterRingCount = 0;
            shutterPreviewValid = false;
        }

        void ReleaseShutter()
        {
            ReleaseShutterTargets();
            SafeRelease(shutterVertexShader);
            SafeRelease(shutterInShader);
            SafeRelease(shutterCopyShader);
            SafeRelease(shutterOutShader);
            SafeRelease(shutterConstants);
            SafeRelease(shutterBlend);
            shutterTried = false;
        }

        void ReleaseSky()
        {
            SafeRelease(skyVertexShader);
            SafeRelease(skyPixelShader);
            SafeRelease(skyConstants);
            SafeRelease(skyPanoramaSampler);
            SafeRelease(skyPhotoSampler);
            SafeRelease(skyImageView);
            SafeRelease(skyImage);
            skyImagePath.clear();
            skyImageWidth = skyImageHeight = 0;
            skyTried = false;
        }

        void ReleaseLens()
        {
            SafeRelease(lensBendShader);
            SafeRelease(lensBendConstants);
            lensTried = false;
        }

        void ReleaseDirt()
        {
            SafeRelease(dirtPixelShader);
            SafeRelease(dirtConstants);
            SafeRelease(dirtGlowView);
            SafeRelease(dirtGlow);
            dirtGlowDesc = {};
            dirtGlowFailed = false;
            SafeRelease(dirtImageView);
            dirtImagePath.clear();
            dirtImageWidth = dirtImageHeight = 0;
            dirtTried = false;
        }

        void ReleaseFlare()
        {
            SafeRelease(flareVertexShader);
            SafeRelease(flarePixelShader);
            SafeRelease(flareConstants);
            SafeRelease(flareBlend);
            SafeRelease(flareDetectShader);
            SafeRelease(flarePickShader);
            SafeRelease(flareGridView);
            SafeRelease(flareGridTarget);
            SafeRelease(flareGridTexture);
            SafeRelease(flareLightsView);
            SafeRelease(flareLightsTarget);
            SafeRelease(flareLightsTexture);
            SafeRelease(flareTrackShader);
            for (int i = 0; i < 2; ++i)
            {
                SafeRelease(flareTrackedViews[i]);
                SafeRelease(flareTrackedTargets[i]);
                SafeRelease(flareTrackedTextures[i]);
            }
            flareTrackedValid = false;
            flareTried = false;
        }

        void ReleaseDevice()
        {
            ReleaseBackBufferView();
            ReleaseScreenBlur();
            ReleaseShutter();
            ReleaseFlare();
            ReleaseSky();
            ReleaseDirt();
            ReleaseLens();
            SafeRelease(opaqueBlendState);
            SafeRelease(gameViewShader);
            gameViewShaderTried = false;
            SafeRelease(readStaging);
            SafeRelease(readResolved);
            SafeRelease(context);
            SafeRelease(device);
        }

        // Whether this swap chain is the game's: the first one to present is, and later one for the same window, which
        // the game makes when it restarts its renderer. Any other (an overlay's own) passes through.
        bool Adopt(IDXGISwapChain* swapChain)
        {
            if (swapChain == gameSwapChain)
                return true;

            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(swapChain->GetDesc(&desc)))
                return false;
            if (gameSwapChain && desc.OutputWindow != gameWindow.load())
                return false;

            ID3D11Device* swapChainDevice = nullptr;
            if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&swapChainDevice))))
                return false;  // a D3D10 or D3D12 swap chain, not the game's

            if (gameSwapChain)
            {
                LOG_INFO("The game made its swap chain again");
                ReleaseBackBufferView();
                if (swapChainDevice != device)
                {
                    LOG_INFO("...with a new device: the UI starts again on it");
                    if (UI::UIManager::Get().IsInitialized())
                        UI::UIManager::Get().ShutdownImGui();
                    ReleaseDevice();
                }
            }

            if (!device)
            {
                device = swapChainDevice;  // keeps GetDevice's reference
                device->GetImmediateContext(&context);
            }
            else
            {
                swapChainDevice->Release();
            }

            gameSwapChain = swapChain;
            gameWindow = desc.OutputWindow;

            char className[256]{};
            GetClassNameA(desc.OutputWindow, className, static_cast<int>(std::size(className)));
            LOG_INFO("Found the game's D3D11 swap chain: {}x{}, format {}, {} sample(s), window class '{}', {}, "
                     "refresh {}/{}, swap effect {}, {} buffer(s)",
                     desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format),
                     desc.SampleDesc.Count, className, desc.Windowed ? "windowed" : "fullscreen",
                     desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator,
                     static_cast<int>(desc.SwapEffect), desc.BufferCount);
            return true;
        }

        bool ReleaseIfUnloading()
        {
            if (!unloadRequested)
                return false;
            if (!releasedForUnload)
            {
                // (ReShade lets go of the mod's events)
                if (reshadePresent)
                    ReShade::ShutdownForPresent();
                if (UI::UIManager::Get().IsInitialized())
                    UI::UIManager::Get().ShutdownImGui();
                ReleaseDevice();
                gameSwapChain = nullptr;
                releasedForUnload = true;
            }
            return true;
        }

        // when ReShade last presented a frame through its events (OnReShadeStage), which then do the frame's work
        // instead of Present_Hook; and whether this frame was captured already
        std::atomic<std::uint64_t> reshadeFrameAt = 0;
        bool capturedThisFrame = false;
        // when ReShade was found, and how long its events are waited for before Present_Hook does the work instead
        std::uint64_t reshadeFoundAt = 0;
        constexpr std::uint64_t RESHADE_EVENTS_WAIT_MS = 10000;
        bool reshadeEventsMissed = false;

        bool FindReShade()
        {
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
                return false;
            const auto count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
            for (std::size_t i = 0; i < count; ++i)
            {
                if (GetProcAddress(modules[i], "ReShadeRegisterAddon"))
                {
                    char path[MAX_PATH]{};
                    GetModuleFileNameA(modules[i], path, MAX_PATH);
                    LOG_INFO("Found ReShade: {}", path);
                    return true;
                }
            }
            return false;
        }

        void OnReShadeStage(ReShade::PresentStage stage, void* swapChain);

        void CheckForReShade()
        {
            if (std::exchange(reshadeChecked, true))
                return;
            if (!FindReShade())
                return;
            reshadePresent = true;
            reshadeFoundAt = GetTickCount64();
            // (only its present: the game's frame has its effects, the editor is drawn after them; its own settings
            // stay in its menu, with no section or keys in the editor)
            if (!ReShade::InitializeForPresent(&OnReShadeStage))
                reshadePresent = false;
        }

        // The screen blur: one triangle over the frame, taking many taps along a line through each pixel (centred on it
        // for the directional blur, towards the centre for the radial one), each pixel's taps moved a little by a
        // noise so few taps don't show as copies
        constexpr char SCREEN_BLUR_SHADER[] = R"(
cbuffer Blur : register(b0)
{
    float2 direction;  // directional: the smear, in uv
    float2 center;     // radial: in uv
    float strength;
    float clearRadius;
    float type;        // 1 directional, 2 radial
    float aspect;      // width / height
    float focus;       // lens: the focus distance (with followPoint, the depth at the point instead)
    float cocScale;    // pixels a millimetre of blur on the 24 mm tall sensor makes, times the amount
    float maxRadius;   // in pixels
    float blades;      // under 3 round
    float highlights;
    float showFocus;
    float2 pixel;      // a pixel's size in uv
    float2 pin;        // the pin, in uv
    float followPoint;
    float focalLength; // mm
    float fNumber;
    float follow;      // the smoothed focus's step towards what's under the pin, 0 to 1 (1 at once)
    float depthMode;   // 0 the game's linear depth, 1 its depth buffer (1 far), 2 its depth buffer reversed (0 far)
    float zNear;       // the near plane, for the depth buffer's
};
Texture2D frame : register(t0);
Texture2D<float> depth : register(t1);          // linear: each pixel's distance from the camera
Texture2D<float> smoothedFocus : register(t2);  // Follow Pin's focus, 1 x 1
Texture2D<float> hardwareDepth : register(t3);  // the depth buffer
SamplerState clampSampler : register(s0);
SamplerState pointSampler : register(s1);

// the distance from the camera at uv, in the game's units; far (a million) where nothing's drawn in the depth buffer,
// 0 where the linear depth has nothing
float sceneDepth(float2 uv)
{
    if (depthMode < 0.5f)
        return depth.SampleLevel(pointSampler, uv, 0);
    float z = hardwareDepth.SampleLevel(pointSampler, uv, 0);
    if (depthMode < 1.5f)
        return z >= 1.0f ? 1000000.0f : zNear / max(1.0f - z, 0.0000001f);
    return z <= 0.0f ? 1000000.0f : zNear / max(z, 0.0000001f);
}
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUTPUT vs(uint id : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
float noise(float2 position)
{
    return frac(52.9829189f * frac(dot(position, float2(0.06711056f, 0.00583715f))));
}
float4 ps(VS_OUTPUT input) : SV_Target
{
    static const int TAPS = 48;
    float2 smear;
    float start;
    if (type < 1.5f)
    {
        smear = direction;
        start = -0.5f;
    }
    else
    {
        // out from the centre: nothing inside the clear radius, growing to the full strength beyond it
        float2 offset = input.uv - center;
        float distance = length(offset * float2(aspect, 1.0f));
        float amount = smoothstep(clearRadius, clearRadius + 0.25f, distance);
        smear = -offset * strength * 2.0f * amount;
        start = 0.0f;
    }
    float jitter = noise(input.pos.xy) - 0.5f;
    float3 color = 0.0f;
    [loop] for (int i = 0; i < TAPS; ++i)
    {
        float t = start + (i + 0.5f + jitter) / TAPS;
        color += frame.SampleLevel(clampSampler, input.uv + smear * t, 0).rgb;
    }
    return float4(color / TAPS, 1.0f);
}

// The lens: each pixel's blur from its depth (a circle of confusion), gathered from rings of taps shaped as the
// aperture; a tap counts when its own blur reaches this pixel or it's farther, so a sharp thing in front doesn't
// smear over what's behind it. Highlights spread into discs.
// the radius in pixels of a thin lens's circle of confusion at depth d focused at s: f^2 / (N (S - f)) on the sensor
// (S in mm, the game's units taken as inches) times |d - s| / d
float lensScale(float s)
{
    float focusMm = max(s * 25.4f, focalLength + 1.0f);
    return cocScale * focalLength * focalLength / (fNumber * (focusMm - focalLength));
}
float coc(float d, float s, float scale)
{
    d = max(d, 0.001f);
    return min(scale * abs(d - s) / d, maxRadius);
}
float apertureShape(float angle)
{
    if (blades < 2.5f)
        return 1.0f;
    float side = 6.28318531f / blades;
    float a = fmod(angle + 6.28318531f, side) - side * 0.5f;
    return cos(side * 0.5f) / cos(a);
}
// Follow Pin's focus, a step towards the depth under the pin each frame (in 1 / depth, as a lens's focus moves); none
// before (0) or a step of 1 takes it at once
float focusStep(VS_OUTPUT input) : SV_Target
{
    // (point sampled: a blend across an edge is neither thing; nothing there, 0, keeps the focus)
    float target = sceneDepth(pin);
    float previous = smoothedFocus.Load(int3(0, 0, 0));
    if (target <= 0.0f)
        return previous > 0.0f ? previous : 100000.0f;
    if (previous <= 0.0f || follow >= 1.0f)
        return target;
    return 1.0f / lerp(1.0f / previous, 1.0f / target, follow);
}

// The depth read at the pin (pixel 0), the centre (1) and a 4 x 4 grid (2 to 17), for the editor and the log: the
// linear depth's raw value, the depth buffer's, and the distance as the lens takes it
float4 probe(VS_OUTPUT input) : SV_Target
{
    int index = (int)input.pos.x;
    float2 uv = index == 0 ? pin : (index == 1 ? float2(0.5f, 0.5f)
                                               : float2(0.125f + 0.25f * ((index - 2) % 4), 0.125f + 0.25f * ((index - 2) / 4)));
    float distance = sceneDepth(uv);
    // the pin's: the nearest thing within a few pixels of it (a click on a thin thing or its edge takes it, not what's
    // behind it)
    if (index == 0)
    {
        float width, height;
        hardwareDepth.GetDimensions(width, height);
        float2 step = 1.5f / max(float2(width, height), 1.0f);
        [unroll] for (int y = -2; y <= 2; ++y)
            [unroll] for (int x = -2; x <= 2; ++x)
            {
                float around = sceneDepth(saturate(uv + float2(x, y) * step));
                if (around > 0.0f)
                    distance = distance > 0.0f ? min(distance, around) : around;
            }
    }
    return float4(depth.SampleLevel(pointSampler, uv, 0), hardwareDepth.SampleLevel(pointSampler, uv, 0), distance, 0.0f);
}

// The lens in five passes, the blur at half the size (a quarter of the pixels): each half size pixel's colour and
// signed blur (nearer than the focus negative); the most blur in each 8 x 8 tile of them, then in reach of each tile
// (how far a blur can come from); the gather; and each full size pixel between itself and the blur by how blurred it
// is and how much of a nearer blur is over it, the blur read from the half size pixels at its own depth (no halo of
// what's sharp round it)
Texture2D<float4> halfSize : register(t4);     // the gather's: the prepared; the composite's: the gathered
Texture2D<float4> preparedHalf : register(t5); // the composite's: the prepared (its blurs)
Texture2D<float> tiles : register(t6);         // the most blur of each tile (the dilate's: before reach)

float focusNow()
{
    return followPoint > 0.5f ? max(smoothedFocus.Load(int3(0, 0, 0)), 0.001f) : focus;
}

// the blur's radius (full size pixels), negative nearer than the focus: grows with the depth, so farther is more
float signedCoc(float d, float s, float scale)
{
    d = max(d, 0.001f);
    return clamp(scale * (d - s) / d, -maxRadius, maxRadius);
}

// Highlights: a small bright spot (a light, a glint: brighter than everything a few pixels round it) made brighter for
// the blur, as the scene's light was before the picture clipped it, so it spreads into a bright disc. Only such spots:
// a bright sky or wall is left as it is, so it doesn't swallow the edges of what's in front of it.
float luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }
float highlightGain(float2 uv, float3 color)
{
    float l = luma(color);
    float bright = saturate((l - 0.5f) / 0.5f);
    if (highlights <= 0.0f || bright <= 0.0f)
        return 1.0f;
    float around = 0.0f;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float a = i * 0.785398f;
        around = max(around, luma(frame.SampleLevel(pointSampler, uv + float2(cos(a), sin(a)) * 6.0f * pixel, 0).rgb));
    }
    float spot = saturate((l - around) / 0.3f);
    return 1.0f + highlights * 6.0f * spot * bright * bright;
}

float4 lensPrepare(VS_OUTPUT input) : SV_Target
{
    float s = focusNow();
    float scale = lensScale(s);
    // the four pixels' nearest blur (a near thing's edge keeps it), and the colour of those of them at that blur (a
    // near thing's colour isn't mixed with what's behind it)
    float2 half_ = pixel * 0.5f;
    float2 offsets[4] = {float2(-half_.x, -half_.y), float2(half_.x, -half_.y), float2(-half_.x, half_.y),
                         float2(half_.x, half_.y)};
    float cocs[4];
    float3 colors[4];
    float nearest = 1000000.0f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        cocs[i] = signedCoc(sceneDepth(input.uv + offsets[i]), s, scale);
        colors[i] = frame.SampleLevel(pointSampler, input.uv + offsets[i], 0).rgb;
        nearest = min(nearest, cocs[i]);
    }
    float3 color = 0.0f;
    float total = 0.0f;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        float w = abs(cocs[j] - nearest) <= 1.0f + 0.15f * abs(nearest) ? 1.0f : 0.0f;
        color += colors[j] * w;
        total += w;
    }
    color /= max(total, 1.0f);
    return float4(color * highlightGain(input.uv, color), nearest);
}

// the most blur (either way) of a tile's 8 x 8 half size pixels
float lensTileMax(VS_OUTPUT input) : SV_Target
{
    int2 first = int2(input.pos.xy) * 8;
    float most = 0.0f;
    [loop] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x)
            most = max(most, abs(halfSize.Load(int3(first + int2(x, y), 0)).w));
    return most;
}

// the most blur that reaches a tile: of the tiles about it, those whose blur comes as far (a tile 16 full size pixels)
float lensTileDilate(VS_OUTPUT input) : SV_Target
{
    int2 tile = int2(input.pos.xy);
    int reach = (int)min(ceil(maxRadius / 16.0f) + 1.0f, 16.0f);
    float most = 0.0f;
    [loop] for (int y = -reach; y <= reach; ++y)
        [loop] for (int x = -reach; x <= reach; ++x)
        {
            float blur = tiles.Load(int3(tile + int2(x, y), 0));
            float away = max(max(abs(x), abs(y)) - 1, 0) * 16.0f;
            most = blur > away ? max(most, blur) : most;
        }
    return most;
}

// The gather: rings of taps shaped as the aperture, out to the most blur that can reach this pixel; a tap counts as
// much as its blur covers this pixel (softly, at its edge), a farther one's no more than twice this pixel's own (what's
// behind doesn't spread over a sharper thing in front) or, under a nearer blur, as far as that blur (the near thing's
// edge shows what's behind it); one that doesn't count adds the colour so far. (Highlights: brightened spots, above.) Its alpha: how much a nearer blur is over this pixel.
float4 lensGather(VS_OUTPUT input) : SV_Target
{
    float4 centre = halfSize.Load(int3(input.pos.xy, 0));
    float own = abs(centre.w);
    float search = max(own, tiles.Load(int3(int2(input.pos.xy) / 8, 0)));
    if (search < 1.5f)
        return float4(centre.rgb, 0.0f);

    // (rings about 2.5 half size pixels apart, at most 8: 288 taps, on a quarter of the pixels)
    int rings = (int)clamp(ceil(search / 5.0f), 2.0f, 8.0f);
    float jitter = noise(input.pos.xy) * 6.28318531f;
    float shift = noise(input.pos.yx + 17.0f) - 0.5f;
    float3 color = centre.rgb;
    float total = 1.0f;
    float nearer = 0.0f;
    float taps = 0.0f;
    [loop] for (int ring = 1; ring <= rings; ++ring)
    {
        int count = ring * 8;
        float ringRadius = search * (ring + shift * 0.5f) / rings;
        [loop] for (int i = 0; i < count; ++i)
        {
            float angle = (i + 0.5f * (ring & 1)) * 6.28318531f / count + jitter;
            float distance = ringRadius * apertureShape(angle);
            float2 uv = input.uv + float2(cos(angle), sin(angle)) * distance * pixel;
            // (its colour blended between the pixels about it, so a small light isn't missed between taps; its blur
            // the one pixel's: a blend of two blurs is neither)
            float4 tap = float4(halfSize.SampleLevel(clampSampler, uv, 0).rgb, halfSize.SampleLevel(pointSampler, uv, 0).w);
            float size = abs(tap.w);
            // (a farther thing shows through a nearer blur as far as that blur's own, and spreads over a sharper
            // thing no more than twice its blur)
            if (tap.w > centre.w)
                size = centre.w < -1.0f ? own : min(size, own * 2.0f);
            // (against the ring's radius, not the tap's distance: the aperture's corners are its own)
            float covers = saturate((size - ringRadius) * 0.5f + 0.5f);
            color += tap.rgb * covers + (color / total) * (1.0f - covers);
            total += 1.0f;
            nearer += (tap.w < centre.w - 1.0f && tap.w < -1.0f) ? covers : 0.0f;
            taps += 1.0f;
        }
    }
    return float4(color / total, saturate(nearer / max(taps, 1.0f) * 2.0f));
}

float4 lensComposite(VS_OUTPUT input) : SV_Target
{
    float3 sharp = frame.SampleLevel(clampSampler, input.uv, 0).rgb;
    float s = focusNow();
    float centreDepth = sceneDepth(input.uv);
    float radius = abs(signedCoc(centreDepth, s, lensScale(s)));
    if (showFocus > 0.5f)
    {
        // green sharp, blue nearer, red farther, by how blurred
        float amount = saturate(radius / 6.0f);
        float3 tint = amount < 0.15f ? float3(0.2f, 1.0f, 0.2f)
                                     : (centreDepth < s ? float3(0.25f, 0.45f, 1.0f) : float3(1.0f, 0.3f, 0.2f));
        float grey = dot(sharp, float3(0.2126f, 0.7152f, 0.0722f));
        return float4(lerp(grey.xxx, tint * (0.35f + grey), 0.65f), 1.0f);
    }
    // the four half size pixels about this one, each as much as its blur is this pixel's (bilinear otherwise)
    float signedRadius = signedCoc(centreDepth, s, lensScale(s));
    float width_, height_;
    halfSize.GetDimensions(width_, height_);
    float2 at = input.uv * float2(width_, height_) - 0.5f;
    int2 base = int2(floor(at));
    float2 f = at - base;
    float3 color = 0.0f;
    float total = 0.0f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        int2 o = int2(i & 1, i >> 1);
        int2 texel = clamp(base + o, int2(0, 0), int2(width_, height_) - 1);
        float bilinear = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y);
        float blur = preparedHalf.Load(int3(texel, 0)).w;
        float w = bilinear / (1.0f + abs(blur - signedRadius) * 0.5f) + 0.0001f;
        color += halfSize.Load(int3(texel, 0)).rgb * w;
        total += w;
    }
    float nearer = halfSize.SampleLevel(clampSampler, input.uv, 0).a;
    float amount = max(saturate((radius - 0.5f) / 1.5f), nearer);
    return float4(lerp(sharp, color / total, amount), 1.0f);
}
// A multi-pass capture's Depth pass: white near, black at focus (the pass's distance) and beyond, black where nothing is
float4 depthPass(VS_OUTPUT input) : SV_Target
{
    float d = sceneDepth(input.uv);
    float g = d >= 900000.0f ? 0.0f : 1.0f - saturate(d / max(focus, 1.0f));
    return float4(g, g, g, 1.0f);
}
// Its players' pass: the frame where something is drawn in the depth buffer (the world isn't, see the game's), green
// where nothing is
float4 greenPass(VS_OUTPUT input) : SV_Target
{
    float z = hardwareDepth.SampleLevel(pointSampler, input.uv, 0);
    bool nothing = depthMode < 1.5f ? z >= 1.0f : z <= 0.0f;
    return nothing ? float4(0.0f, 1.0f, 0.0f, 1.0f) : float4(frame.SampleLevel(pointSampler, input.uv, 0).rgb, 1.0f);
}
)"
                                                R"(// Its Normal pass: which way what's drawn faces the camera, from the depth buffer (X right red, Y up green, towards
// the camera blue, each -1 to 1 as 0 to 1), black where nothing is; direction holds the view's half angle tangents
float3 viewPosition(float2 uv)
{
    float d = sceneDepth(uv);
    return float3((uv.x * 2.0f - 1.0f) * direction.x * d, (1.0f - uv.y * 2.0f) * direction.y * d, d);
}
float4 normalPass(VS_OUTPUT input) : SV_Target
{
    float d = sceneDepth(input.uv);
    if (d >= 900000.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float3 p = viewPosition(input.uv);
    // (each way, the nearer neighbour's side: an edge's other side would bend it)
    float3 right = viewPosition(input.uv + float2(pixel.x, 0.0f)) - p;
    float3 left = p - viewPosition(input.uv - float2(pixel.x, 0.0f));
    float3 down = viewPosition(input.uv + float2(0.0f, pixel.y)) - p;
    float3 up = p - viewPosition(input.uv - float2(0.0f, pixel.y));
    float3 dx = abs(right.z) < abs(left.z) ? right : left;
    float3 dy = abs(down.z) < abs(up.z) ? down : up;
    float3 n = cross(dx, dy);
    float len = length(n);
    n = len > 0.0f ? n / len : float3(0.0f, 0.0f, -1.0f);
    return float4(n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, -n.z * 0.5f + 0.5f, 1.0f);
}
)";

        struct BlurConstants
        {
            float direction[2];
            float center[2];
            float strength;
            float clearRadius;
            float type;
            float aspect;
            float focus;
            float cocScale;
            float maxRadius;
            float blades;
            float highlights;
            float showFocus;
            float pixel[2];
            float point[2];
            float followPoint;
            float focalLength;
            float fNumber;
            float follow;
            float depthMode;
            float zNear;
        };

        bool CompileBlurShader(const char* entry, const char* target, ID3DBlob*& code)
        {
            ID3DBlob* errors = nullptr;
            const auto result = D3DCompile(SCREEN_BLUR_SHADER, sizeof(SCREEN_BLUR_SHADER) - 1, "ScreenBlur", nullptr,
                                           nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(result))
                LOG_ERROR("Couldn't compile the screen blur's {} ({})", entry,
                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
            SafeRelease(errors);
            return SUCCEEDED(result);
        }

        bool MakeScreenBlur()
        {
            if (std::exchange(blurShadersTried, true))
                return blurPixelShader != nullptr;
            ID3DBlob* vertexCode = nullptr;
            ID3DBlob* pixelCode = nullptr;
            ID3DBlob* lensCode = nullptr;
            bool made = CompileBlurShader("vs", "vs_4_0", vertexCode) && CompileBlurShader("ps", "ps_4_0", pixelCode);
            // (the lens without its own: the others still work)
            ID3DBlob* prepareCode = nullptr;
            ID3DBlob* gatherCode = nullptr;
            ID3DBlob* tileMaxCode = nullptr;
            ID3DBlob* tileDilateCode = nullptr;
            if (made)
            {
                const bool compiled = CompileBlurShader("lensComposite", "ps_4_0", lensCode) &&
                                      CompileBlurShader("lensPrepare", "ps_4_0", prepareCode) &&
                                      CompileBlurShader("lensGather", "ps_4_0", gatherCode) &&
                                      CompileBlurShader("lensTileMax", "ps_4_0", tileMaxCode) &&
                                      CompileBlurShader("lensTileDilate", "ps_4_0", tileDilateCode);
                if (!compiled ||
                    FAILED(device->CreatePixelShader(lensCode->GetBufferPointer(), lensCode->GetBufferSize(), nullptr,
                                                     &lensPixelShader)) ||
                    FAILED(device->CreatePixelShader(prepareCode->GetBufferPointer(), prepareCode->GetBufferSize(),
                                                     nullptr, &lensPrepareShader)) ||
                    FAILED(device->CreatePixelShader(gatherCode->GetBufferPointer(), gatherCode->GetBufferSize(),
                                                     nullptr, &lensGatherShader)) ||
                    FAILED(device->CreatePixelShader(tileMaxCode->GetBufferPointer(), tileMaxCode->GetBufferSize(),
                                                     nullptr, &lensTileMaxShader)) ||
                    FAILED(device->CreatePixelShader(tileDilateCode->GetBufferPointer(),
                                                     tileDilateCode->GetBufferSize(), nullptr, &lensTileDilateShader)))
                {
                    LOG_ERROR("Couldn't make the lens blur's pixel shaders");
                    SafeRelease(lensPixelShader);
                }
            }
            SafeRelease(lensCode);
            SafeRelease(prepareCode);
            SafeRelease(gatherCode);
            SafeRelease(tileMaxCode);
            SafeRelease(tileDilateCode);
            // (Follow Pin's smoothing: without it the lens focuses on its Focus Distance only)
            ID3DBlob* stepCode = nullptr;
            if (made && CompileBlurShader("focusStep", "ps_4_0", stepCode))
            {
                bool focusMade = SUCCEEDED(device->CreatePixelShader(
                    stepCode->GetBufferPointer(), stepCode->GetBufferSize(), nullptr, &focusStepShader));
                D3D11_TEXTURE2D_DESC focusDesc{};
                focusDesc.Width = focusDesc.Height = 1;
                focusDesc.MipLevels = focusDesc.ArraySize = 1;
                focusDesc.Format = DXGI_FORMAT_R32_FLOAT;
                focusDesc.SampleDesc.Count = 1;
                focusDesc.Usage = D3D11_USAGE_DEFAULT;
                focusDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                for (int i = 0; i < 2 && focusMade; ++i)
                    focusMade = SUCCEEDED(device->CreateTexture2D(&focusDesc, nullptr, &focusTextures[i])) &&
                                SUCCEEDED(device->CreateRenderTargetView(focusTextures[i], nullptr, &focusTargets[i])) &&
                                SUCCEEDED(device->CreateShaderResourceView(focusTextures[i], nullptr, &focusViews[i]));
                if (!focusMade)
                {
                    LOG_ERROR("Couldn't make Follow Pin's focus; the lens focuses at its distance");
                    SafeRelease(focusStepShader);
                }
            }
            SafeRelease(stepCode);
            // the probe of the depth, and a sampler for depth (a blend of two depths is neither)
            ID3DBlob* probeCode = nullptr;
            if (made && CompileBlurShader("probe", "ps_4_0", probeCode))
            {
                D3D11_TEXTURE2D_DESC probeDesc{};
                probeDesc.Width = DEPTH_SAMPLES;
                probeDesc.Height = 1;
                probeDesc.MipLevels = probeDesc.ArraySize = 1;
                probeDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                probeDesc.SampleDesc.Count = 1;
                probeDesc.Usage = D3D11_USAGE_DEFAULT;
                probeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                D3D11_TEXTURE2D_DESC stagingDesc = probeDesc;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.BindFlags = 0;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(device->CreatePixelShader(probeCode->GetBufferPointer(), probeCode->GetBufferSize(), nullptr,
                                                     &probeShader)) ||
                    FAILED(device->CreateTexture2D(&probeDesc, nullptr, &probeTexture)) ||
                    FAILED(device->CreateRenderTargetView(probeTexture, nullptr, &probeTarget)) ||
                    FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &probeStaging)))
                {
                    LOG_ERROR("Couldn't make the lens's depth probe");
                    SafeRelease(probeShader);
                }
            }
            SafeRelease(probeCode);
            D3D11_SAMPLER_DESC pointDesc{};
            pointDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            pointDesc.AddressU = pointDesc.AddressV = pointDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            pointDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            pointDesc.MaxLOD = D3D11_FLOAT32_MAX;
            made = made && SUCCEEDED(device->CreateSamplerState(&pointDesc, &pointSampler));
            made = made && SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                                vertexCode->GetBufferSize(), nullptr,
                                                                &blurVertexShader));
            made = made && SUCCEEDED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                                                               nullptr, &blurPixelShader));
            SafeRelease(vertexCode);
            SafeRelease(pixelCode);

            D3D11_BUFFER_DESC constantsDesc{};
            constantsDesc.ByteWidth = sizeof(BlurConstants);
            constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            made = made && SUCCEEDED(device->CreateBuffer(&constantsDesc, nullptr, &blurConstants));

            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            made = made && SUCCEEDED(device->CreateSamplerState(&samplerDesc, &blurSampler));

            D3D11_RASTERIZER_DESC rasterizerDesc{};
            rasterizerDesc.FillMode = D3D11_FILL_SOLID;
            rasterizerDesc.CullMode = D3D11_CULL_NONE;
            rasterizerDesc.DepthClipEnable = TRUE;
            made = made && SUCCEEDED(device->CreateRasterizerState(&rasterizerDesc, &blurRasterizer));

            D3D11_DEPTH_STENCIL_DESC depthDesc{};
            depthDesc.DepthEnable = FALSE;
            depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            made = made && SUCCEEDED(device->CreateDepthStencilState(&depthDesc, &blurDepthState));

            if (!made)
            {
                LOG_ERROR("Couldn't make the screen blur; it isn't drawn");
                ReleaseScreenBlur();
                blurShadersTried = true;
            }
            return made;
        }

        // the lens's half size textures, made again when the frame's size changed
        bool MakeLensHalfSize(UINT width, UINT height)
        {
            const UINT halfWidth = std::max(width / 2, 1u), halfHeight = std::max(height / 2, 1u);
            if (lensHalfViews[0] && lensHalfWidth == halfWidth && lensHalfHeight == halfHeight)
                return true;
            for (int i = 0; i < 2; ++i)
            {
                SafeRelease(lensHalfViews[i]);
                SafeRelease(lensHalfTargets[i]);
                SafeRelease(lensHalfTextures[i]);
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = halfWidth;
            desc.Height = halfHeight;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            for (int i = 0; i < 2; ++i)
            {
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &lensHalfTextures[i])) ||
                    FAILED(device->CreateRenderTargetView(lensHalfTextures[i], nullptr, &lensHalfTargets[i])) ||
                    FAILED(device->CreateShaderResourceView(lensHalfTextures[i], nullptr, &lensHalfViews[i])))
                {
                    LOG_ERROR("Couldn't make the lens blur's {}x{} textures", halfWidth, halfHeight);
                    SafeRelease(lensHalfViews[0]);
                    return false;
                }
            }
            const UINT tileWidth = (halfWidth + 7) / 8, tileHeight = (halfHeight + 7) / 8;
            for (int i = 0; i < 2; ++i)
            {
                SafeRelease(lensTileViews[i]);
                SafeRelease(lensTileTargets[i]);
                SafeRelease(lensTileTextures[i]);
            }
            D3D11_TEXTURE2D_DESC tileDesc = desc;
            tileDesc.Width = tileWidth;
            tileDesc.Height = tileHeight;
            tileDesc.Format = DXGI_FORMAT_R16_FLOAT;
            for (int i = 0; i < 2; ++i)
            {
                if (FAILED(device->CreateTexture2D(&tileDesc, nullptr, &lensTileTextures[i])) ||
                    FAILED(device->CreateRenderTargetView(lensTileTextures[i], nullptr, &lensTileTargets[i])) ||
                    FAILED(device->CreateShaderResourceView(lensTileTextures[i], nullptr, &lensTileViews[i])))
                {
                    LOG_ERROR("Couldn't make the lens blur's {}x{} tiles", tileWidth, tileHeight);
                    SafeRelease(lensHalfViews[0]);
                    return false;
                }
            }
            lensTileWidth = tileWidth;
            lensTileHeight = tileHeight;
            lensHalfWidth = halfWidth;
            lensHalfHeight = halfHeight;
            return true;
        }

        // the frame's copy the blur reads, made again when the back buffer's size or format changed
        bool CopyFrameForBlur(ID3D11Texture2D* backBuffer)
        {
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            const auto format = ViewFormat(desc.Format);
            if (!blurFrame || blurFrameDesc.Width != desc.Width || blurFrameDesc.Height != desc.Height ||
                blurFrameDesc.Format != format)
            {
                SafeRelease(blurFrameView);
                SafeRelease(blurFrame);
                D3D11_TEXTURE2D_DESC copyDesc{};
                copyDesc.Width = desc.Width;
                copyDesc.Height = desc.Height;
                copyDesc.MipLevels = 1;
                copyDesc.ArraySize = 1;
                copyDesc.Format = format;
                copyDesc.SampleDesc.Count = 1;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &blurFrame)) ||
                    FAILED(device->CreateShaderResourceView(blurFrame, nullptr, &blurFrameView)))
                {
                    SafeRelease(blurFrame);
                    blurFrameDesc = {};
                    return false;
                }
                blurFrameDesc = copyDesc;
            }
            if (desc.SampleDesc.Count > 1)
                context->ResolveSubresource(blurFrame, 0, backBuffer, 0, format);
            else
                context->CopyResource(blurFrame, backBuffer);
            return true;
        }

        // What the blur sets of the context, as the game had it, put back after (as ImGui's backend does)
        struct ContextState
        {
            UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            ID3D11RasterizerState* rasterizer = nullptr;
            ID3D11BlendState* blend = nullptr;
            FLOAT blendFactor[4]{};
            UINT sampleMask = 0;
            ID3D11DepthStencilState* depth = nullptr;
            UINT stencilRef = 0;
            ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* depthView = nullptr;
            ID3D11ShaderResourceView* resources[16]{};
            ID3D11SamplerState* samplers[2]{};
            ID3D11PixelShader* pixelShader = nullptr;
            ID3D11VertexShader* vertexShader = nullptr;
            ID3D11GeometryShader* geometryShader = nullptr;
            ID3D11HullShader* hullShader = nullptr;
            ID3D11DomainShader* domainShader = nullptr;
            ID3D11ClassInstance* pixelInstances[256]{};
            ID3D11ClassInstance* vertexInstances[256]{};
            ID3D11ClassInstance* geometryInstances[256]{};
            UINT pixelInstanceCount = 256, vertexInstanceCount = 256, geometryInstanceCount = 256;
            ID3D11Buffer* pixelConstants = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            ID3D11InputLayout* layout = nullptr;

            void Save()
            {
                context->RSGetScissorRects(&scissorCount, scissors);
                context->RSGetViewports(&viewportCount, viewports);
                context->RSGetState(&rasterizer);
                context->OMGetBlendState(&blend, blendFactor, &sampleMask);
                context->OMGetDepthStencilState(&depth, &stencilRef);
                context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthView);
                context->PSGetShaderResources(0, 16, resources);
                context->PSGetSamplers(0, 2, samplers);
                context->PSGetShader(&pixelShader, pixelInstances, &pixelInstanceCount);
                context->VSGetShader(&vertexShader, vertexInstances, &vertexInstanceCount);
                context->GSGetShader(&geometryShader, geometryInstances, &geometryInstanceCount);
                context->HSGetShader(&hullShader, nullptr, nullptr);
                context->DSGetShader(&domainShader, nullptr, nullptr);
                context->PSGetConstantBuffers(0, 1, &pixelConstants);
                context->IAGetPrimitiveTopology(&topology);
                context->IAGetInputLayout(&layout);
            }

            void Restore()
            {
                context->RSSetScissorRects(scissorCount, scissors);
                context->RSSetViewports(viewportCount, viewports);
                context->RSSetState(rasterizer);
                context->OMSetBlendState(blend, blendFactor, sampleMask);
                context->OMSetDepthStencilState(depth, stencilRef);
                context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depthView);
                context->PSSetShaderResources(0, 16, resources);
                context->PSSetSamplers(0, 2, samplers);
                context->PSSetShader(pixelShader, pixelInstances, pixelInstanceCount);
                context->VSSetShader(vertexShader, vertexInstances, vertexInstanceCount);
                context->GSSetShader(geometryShader, geometryInstances, geometryInstanceCount);
                context->HSSetShader(hullShader, nullptr, 0);
                context->DSSetShader(domainShader, nullptr, 0);
                context->PSSetConstantBuffers(0, 1, &pixelConstants);
                context->IASetPrimitiveTopology(topology);
                context->IASetInputLayout(layout);

                SafeRelease(rasterizer);
                SafeRelease(blend);
                SafeRelease(depth);
                for (auto& target : targets)
                    SafeRelease(target);
                SafeRelease(depthView);
                for (auto& resource : resources)
                    SafeRelease(resource);
                for (auto& sampler : samplers)
                    SafeRelease(sampler);
                SafeRelease(pixelShader);
                SafeRelease(vertexShader);
                SafeRelease(geometryShader);
                SafeRelease(hullShader);
                SafeRelease(domainShader);
                for (UINT i = 0; i < pixelInstanceCount; ++i)
                    SafeRelease(pixelInstances[i]);
                for (UINT i = 0; i < vertexInstanceCount; ++i)
                    SafeRelease(vertexInstances[i]);
                for (UINT i = 0; i < geometryInstanceCount; ++i)
                    SafeRelease(geometryInstances[i]);
                SafeRelease(pixelConstants);
                SafeRelease(layout);
            }
        };

        void SetOpaqueBlending(const ImDrawList*, const ImDrawCmd*);

        // The probe read back (without waiting): the pin's distance for the editor, which depth the lens reads (the
        // linear depth while it has anything, else the depth buffer, reversed or not by where most of the picture is),
        // and the log every 2 seconds
        void ReadProbe(bool hasLinear, bool hasHardware, float pointX, float pointY)
        {
            if (!probePending)
                return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(probeStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
                return;  // (not there yet)
            std::array<std::array<float, 4>, DEPTH_SAMPLES> samples{};
            std::memcpy(samples.data(), mapped.pData, sizeof(samples));
            context->Unmap(probeStaging, 0);
            probePending = false;

            const auto previousMode = depthMode;
            // the depth buffer whenever there is one: the game fills its linear depth for some things only (those its
            // own effects need, such as what a light lights), and what it leaves empty would read as nearest
            if (!hasHardware)
            {
                depthMode = hasLinear ? 0 : depthMode;
            }
            else
            {
                // reversed or not by what's drawn: a depth buffer's values are near 1 for all but what's a few units
                // from the camera (1 - zNear / d), a reversed one's near 0 (zNear / d), so the middle of the samples
                // that aren't the sky says which; with only the sky in them, by the sky (exactly 1, or exactly 0
                // reversed); else as it was
                int ones = 0, zeros = 0;
                std::vector<float> drawn;
                for (std::size_t i = 0; i < DEPTH_SAMPLES; ++i)
                {
                    const float z = samples[i][1];
                    if (z >= 1.0f)
                        ++ones;
                    else if (z <= 0.0f)
                        ++zeros;
                    else
                        drawn.push_back(z);
                }
                if (!drawn.empty())
                {
                    std::ranges::nth_element(drawn, drawn.begin() + drawn.size() / 2);
                    depthMode = drawn[drawn.size() / 2] > 0.5f ? 1 : 2;
                }
                else if (ones != zeros)
                {
                    depthMode = ones > zeros ? 1 : 2;
                }
            }
            if (depthMode != previousMode && previousMode != -1)
                LOG_INFO("The lens blur now reads {}", depthMode == 0   ? "the game's linear depth"
                                                       : depthMode == 1 ? "the depth buffer"
                                                                        : "the depth buffer (reversed)");

            // (the pin's distance once the mode it was read with is the one decided)
            const float pinDistance = samples[0][2];
            if (depthMode == previousMode && pinDistance > 0.0f && pinDistance < 900000.0f && std::isfinite(pinDistance))
            {
                centerDepth = pinDistance;
                centerDepthAt = GetTickCount64();
                std::lock_guard lock(probeMutex);
                probedAt = probingAt;
            }

            if (GetTickCount64() - depthLoggedAt >= 2000)
            {
                depthLoggedAt = GetTickCount64();
                std::string grid;
                for (UINT i = 2; i < DEPTH_SAMPLES; ++i)
                    grid += std::format(" {:.0f}", samples[i][2]);
                LOG_DEBUG("Lens depth (mode {}): at the pin ({:.2f} {:.2f}) {:.1f} (linear {:.1f}, buffer {:.6f}), centre "
                          "{:.1f} (linear {:.1f}, buffer {:.6f}), grid{}",
                          depthMode, pointX, pointY, samples[0][2], samples[0][0], samples[0][1], samples[1][2],
                          samples[1][0], samples[1][1], grid);
            }
        }

        // The game's depth view for the lens, when it's one the blur can read: a 2D texture's on this device (each kind
        // logged once)
        ID3D11ShaderResourceView* UsableDepth(ID3D11ShaderResourceView* const* where, const char* kind)
        {
            if (!where || !*where)
                return nullptr;
            ID3D11ShaderResourceView* view = *where;
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            view->GetDesc(&desc);
            ID3D11Device* viewDevice = nullptr;
            view->GetDevice(&viewDevice);
            const bool sameDevice = viewDevice == device;
            SafeRelease(viewDevice);
            const bool usable = sameDevice && desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
            static std::set<std::string> logged;
            if (logged.insert(kind).second)
                LOG_INFO("The game's {} for the lens blur: format {}, dimension {}, {}", kind,
                         static_cast<int>(desc.Format), static_cast<int>(desc.ViewDimension),
                         usable ? "used" : (sameDevice ? "not a 2D texture: not used" : "another device's: not used"));
            return usable ? view : nullptr;
        }

        // A spot asked for (RequestDepthAt) read by the probe from the lens flare's depth buffer, while the probe isn't
        // busy (with the lens's pin): the result read back as the lens's are (ReadProbe)
        void ProbeRequestedDepth()
        {
            std::optional<glm::vec2> request;
            {
                std::lock_guard lock(probeMutex);
                request = depthRequest;
            }
            // with nothing asked for, a look at the depth now and then (when no other probe looked lately), so
            // what reads it without the lens (the flare's distances, a pick) knows which kind it is
            const bool modeOnly = !request;
            if (modeOnly && GetTickCount64() - lastProbeSent < 1000)
                return;
            const auto [where, zNear] = Components::LensFlare::GetDepthBuffer();
            ID3D11ShaderResourceView* hardwareView = UsableDepth(where, "depth buffer");
            if (!hardwareView || !MakeScreenBlur() || !probeShader)
                return;
            ReadProbe(false, true, request ? request->x : 0.5f, request ? request->y : 0.5f);
            if (probePending)
                return;
            if (!request)
                request = glm::vec2(0.5f);
            if (depthMode == -1)
                depthMode = 1;

            BlurConstants constants{};
            constants.point[0] = std::clamp(request->x, 0.0f, 1.0f);
            constants.point[1] = std::clamp(request->y, 0.0f, 1.0f);
            constants.depthMode = static_cast<float>(std::max(depthMode, 1));
            constants.zNear = std::max(zNear, 0.001f);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(blurConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(blurConstants, 0);

            ContextState state;
            state.Save();
            D3D11_VIEWPORT probeViewport{0.0f, 0.0f, static_cast<float>(DEPTH_SAMPLES), 1.0f, 0.0f, 1.0f};
            context->RSSetViewports(1, &probeViewport);
            context->RSSetState(blurRasterizer);
            const float factor[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            context->OMSetBlendState(opaqueBlendState, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->OMSetRenderTargets(1, &probeTarget, nullptr);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(blurVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            ID3D11SamplerState* samplers[2] = {blurSampler, pointSampler};
            context->PSSetSamplers(0, 2, samplers);
            context->PSSetConstantBuffers(0, 1, &blurConstants);
            ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, nullptr, hardwareView};
            context->PSSetShaderResources(0, 4, views);
            context->PSSetShader(probeShader, nullptr, 0);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[4] = {};
            context->PSSetShaderResources(0, 4, none);
            context->CopyResource(probeStaging, probeTexture);
            probePending = true;
            // (a look at the depth's kind is no spot's distance)
            probingAt = modeOnly ? glm::vec2(-1.0f) : *request;
            lastProbeSent = GetTickCount64();
            state.Restore();
        }

        // A multi-pass capture's pass drawn over the frame before it's recorded: Depth (white near, black from the
        // pass distance on) from the depth buffer, or the players' green behind what's drawn (the game leaves the
        // world out of that pass's frames, see Components::Rendering's flags); also while a players' pass is looked
        // at in the Record tab
        void DrawStreamPass(IDXGISwapChain* swapChain)
        {
            auto& capture = Components::CaptureManager::Get();
            const bool passes = capture.IsCapturing() && capture.MultiPassEnabled();
            // (or the pass looked at with its eye in the Record tab, while not recording)
            std::optional<Components::PassType> previewed;
            if (!capture.IsCapturing())
            {
                const auto menu =
                    UI::UIManager::Get().GetUIComponent<UI::CaptureMenu>(UI::Component::Component::CaptureMenu);
                const auto& settings = capture.GetCaptureSettings();
                if (const auto index = menu ? menu->GetDisplayPassIndex() : std::nullopt;
                    index && *index >= 0 && static_cast<std::size_t>(*index) < settings.passes.size())
                    previewed = settings.passes[*index].type;
            }
            const bool depthPass = (passes && capture.GetCurrentPass().type == Components::PassType::Depth) ||
                                   previewed == Components::PassType::Depth;
            const bool normalPass = !depthPass &&
                                    ((passes && capture.GetCurrentPass().type == Components::PassType::Normal) ||
                                     previewed == Components::PassType::Normal);
            const bool greenPass = !depthPass && !normalPass &&
                                   Components::Rendering::GetRenderingFlags() == Types::RenderingFlags_DrawPlayers;
            if (!depthPass && !greenPass && !normalPass)
                return;
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur())
                return;
            if (!std::exchange(passShadersTried, true))
            {
                ID3DBlob* depthCode = nullptr;
                ID3DBlob* greenCode = nullptr;
                ID3DBlob* normalCode = nullptr;
                if (CompileBlurShader("normalPass", "ps_4_0", normalCode) &&
                    FAILED(device->CreatePixelShader(normalCode->GetBufferPointer(), normalCode->GetBufferSize(),
                                                     nullptr, &normalPassShader)))
                    normalPassShader = nullptr;
                SafeRelease(normalCode);
                if (!CompileBlurShader("depthPass", "ps_4_0", depthCode) ||
                    !CompileBlurShader("greenPass", "ps_4_0", greenCode) ||
                    FAILED(device->CreatePixelShader(depthCode->GetBufferPointer(), depthCode->GetBufferSize(), nullptr,
                                                     &depthPassShader)) ||
                    FAILED(device->CreatePixelShader(greenCode->GetBufferPointer(), greenCode->GetBufferSize(), nullptr,
                                                     &greenPassShader)))
                {
                    LOG_ERROR("Couldn't make the multi-pass capture's Depth and players' shaders");
                    SafeRelease(depthPassShader);
                    SafeRelease(greenPassShader);
                }
                SafeRelease(depthCode);
                SafeRelease(greenCode);
            }
            const auto shader = depthPass ? depthPassShader : normalPass ? normalPassShader : greenPassShader;
            if (!shader)
                return;
            // (the Normal pass's view: its half angles)
            const auto view = normalPass ? Components::Tracking::GetView() : std::nullopt;
            if (normalPass && !view)
            {
                static bool told = false;
                if (!std::exchange(told, true))
                    LOG_WARN("A multi-pass capture's Normal pass has no view to work from; left as drawn");
                return;
            }
            const auto [where, zNear] = Components::LensFlare::GetDepthBuffer();
            const auto hardwareView = UsableDepth(where, "depth buffer");
            if (!hardwareView)
            {
                static bool told = false;
                if (!std::exchange(told, true))
                    LOG_WARN("A multi-pass capture's Depth or players' pass has no depth buffer to read; left as drawn");
                return;
            }

            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            const bool copied = CopyFrameForBlur(backBuffer);
            backBuffer->Release();
            if (!copied)
                return;

            const float width = static_cast<float>(blurFrameDesc.Width);
            const float height = static_cast<float>(blurFrameDesc.Height);
            BlurConstants constants{};
            constants.focus = std::max(capture.GetCaptureSettings().depthDistance, 1.0f);
            constants.pixel[0] = 1.0f / std::max(width, 1.0f);
            constants.pixel[1] = 1.0f / std::max(height, 1.0f);
            // (the depth buffer's way round as the lens found it; BO2's is reversed, 0 far)
            constants.depthMode = depthMode == 1 ? 1.0f : 2.0f;
            constants.zNear = std::max(zNear, 0.001f);
            if (view)
            {
                constants.direction[0] = std::max(view->tanHalfFovX, 0.001f);
                constants.direction[1] = std::max(view->tanHalfFovY, 0.001f);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(blurConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(blurConstants, 0);

            ContextState state;
            state.Save();
            D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(blurRasterizer);
            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            const float blendFactor[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            context->OMSetBlendState(opaqueBlendState, blendFactor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(blurVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            ID3D11SamplerState* samplers[2] = {blurSampler, pointSampler};
            context->PSSetSamplers(0, 2, samplers);
            context->PSSetConstantBuffers(0, 1, &blurConstants);
            ID3D11ShaderResourceView* views[4] = {blurFrameView, nullptr, nullptr, hardwareView};
            context->PSSetShaderResources(0, 4, views);
            context->PSSetShader(shader, nullptr, 0);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[4] = {};
            context->PSSetShaderResources(0, 4, none);
            state.Restore();
        }

        // The screen blur drawn into the frame's back buffer, when it's on
        void DrawScreenBlur(IDXGISwapChain* swapChain, bool picture = false)
        {
            ScreenBlur blur;
            {
                std::lock_guard lock(screenBlurMutex);
                blur = picture ? pictureBlur : screenBlur;
            }
            // (the Blur section's is never the Lens, which reads back the depth)
            if (picture && blur.type == ScreenBlur::LENS)
                return;
            if (blur.type == ScreenBlur::NONE || (blur.type != ScreenBlur::LENS && blur.strength <= 0.0f))
                return;
            // (only over a demo: not the menus after one)
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur())
                return;
            ID3D11ShaderResourceView* depthView = nullptr;
            ID3D11ShaderResourceView* hardwareView = nullptr;
            if (blur.type == ScreenBlur::LENS)
            {
                depthView = UsableDepth(blur.depth, "linear depth");
                hardwareView = UsableDepth(blur.hardwareDepth, "depth buffer");
                if ((!depthView && !hardwareView) || !lensPixelShader)
                    return;
                // the focus on what it follows: its distance along the view, the pin on it (its last distance while
                // it's out of view)
                const auto focusTrack =
                    blur.trackFocus ? Components::Tracking::GetFocusTrack() : Types::ScreenTrack{};
                if (focusTrack.kind != Types::TrackKind::None)
                {
                    const auto projected = Components::Tracking::Project(focusTrack);
                    if (projected)
                    {
                        trackedFocusDepth = projected->depth;
                        blur.pointX = std::clamp(projected->uv.x, 0.0f, 1.0f);
                        blur.pointY = std::clamp(projected->uv.y, 0.0f, 1.0f);
                    }
                    {
                        std::lock_guard lock(probeMutex);
                        trackedFocus = projected ? std::optional(projected->uv) : std::nullopt;
                    }
                    if (trackedFocusDepth > 0.0f)
                        blur.focusDistance = trackedFocusDepth;
                    blur.followPoint = false;
                }
                else
                {
                    std::lock_guard lock(probeMutex);
                    trackedFocus.reset();
                }
                ReadProbe(depthView != nullptr, hardwareView != nullptr, blur.pointX, blur.pointY);
                if (depthMode == -1)
                    depthMode = hardwareView ? 1 : 0;
                // (a recording never has the focus tint)
                if (Components::CaptureManager::Get().IsCapturing())
                    blur.showFocus = false;
            }

            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            const bool copied = CopyFrameForBlur(backBuffer);
            backBuffer->Release();
            if (!copied)
                return;
            if (blur.type == ScreenBlur::LENS && !MakeLensHalfSize(blurFrameDesc.Width, blurFrameDesc.Height))
                return;

            const float width = static_cast<float>(blurFrameDesc.Width);
            const float height = static_cast<float>(blurFrameDesc.Height);
            const float aspect = width / std::max(height, 1.0f);
            BlurConstants constants{};
            // (the smear's length as a part of the height, in uv: across is shorter by the aspect)
            const float angle = blur.angle * 3.14159265f / 180.0f;
            constants.direction[0] = std::cos(angle) * blur.strength / aspect;
            constants.direction[1] = -std::sin(angle) * blur.strength;
            constants.center[0] = blur.centerX;
            constants.center[1] = blur.centerY;
            constants.strength = blur.strength;
            constants.clearRadius = blur.clearRadius;
            constants.type = static_cast<float>(blur.type);
            constants.aspect = aspect;
            // the lens's circle of confusion (the shader's lensScale and coc), as a radius in pixels of a 24 mm tall
            // full frame
            {
                // (the lens's, when it's on and the depth of field takes it)
                if (const auto lens = Components::Lens::GetSettings(); lens.enabled && lens.depthOfField)
                    blur.focalLength = Components::Lens::EquivalentFocalLength(lens);
                constants.focalLength = std::clamp(blur.focalLength, 1.0f, 2000.0f);
                constants.fNumber = std::max(blur.aperture, 0.5f);
                constants.focus = std::max(blur.focusDistance, 0.001f);
                constants.cocScale = 0.5f / 24.0f * height * std::max(blur.amount, 0.0f);
                constants.point[0] = std::clamp(blur.pointX, 0.0f, 1.0f);
                constants.point[1] = std::clamp(blur.pointY, 0.0f, 1.0f);
                constants.followPoint = blur.followPoint && focusStepShader ? 1.0f : 0.0f;
                // the step this frame: 1 - e^(-dt / smoothing) for the film's time since the last (the wall's while it
                // stands still, so a moved pin still gets there); at once after a jump in the film or the first time
                const auto tick = Components::Playback::GetTimelineTick();
                const auto now = GetTickCount64();
                float seconds = 0.0f;
                bool jumped = !focusValid || !blur.followPoint;
                if (tick != focusLastTick)
                {
                    const auto delta = static_cast<std::int64_t>(tick) - static_cast<std::int64_t>(focusLastTick);
                    jumped = jumped || delta < 0 || delta > 500;
                    seconds = static_cast<float>(delta) / 1000.0f;
                }
                else
                {
                    seconds = static_cast<float>(std::min<std::uint64_t>(now - focusLastTime, 100)) / 1000.0f;
                }
                focusLastTick = tick;
                focusLastTime = now;
                constants.follow = jumped || blur.smoothing <= 0.0f
                                       ? 1.0f
                                       : 1.0f - std::exp(-std::max(seconds, 0.0f) / blur.smoothing);
                constants.maxRadius = height * std::clamp(blur.maxSize, 0.001f, 0.2f);
                constants.blades = std::round(blur.blades);
                constants.highlights = std::max(blur.highlights, 0.0f);
                constants.showFocus = blur.showFocus ? 1.0f : 0.0f;
                constants.pixel[0] = 1.0f / width;
                constants.pixel[1] = 1.0f / std::max(height, 1.0f);
                constants.depthMode = static_cast<float>(std::max(depthMode, 0));
                constants.zNear = std::max(blur.zNear, 0.001f);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(blurConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(blurConstants, 0);

            ContextState state;
            state.Save();

            D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(blurRasterizer);
            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            const float blendFactor[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            context->OMSetBlendState(opaqueBlendState, blendFactor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(blurVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            ID3D11SamplerState* samplers[2] = {blurSampler, pointSampler};
            context->PSSetSamplers(0, 2, samplers);
            context->PSSetConstantBuffers(0, 1, &blurConstants);
            // Follow Pin's focus stepped first, into the other of its two textures
            ID3D11ShaderResourceView* focusView = nullptr;
            if (blur.type == ScreenBlur::LENS && constants.followPoint > 0.5f)
            {
                const int next = 1 - focusCurrent;
                D3D11_VIEWPORT focusViewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
                context->RSSetViewports(1, &focusViewport);
                context->OMSetRenderTargets(1, &focusTargets[next], nullptr);
                ID3D11ShaderResourceView* stepViews[4] = {nullptr, depthView, focusViews[focusCurrent], hardwareView};
                context->PSSetShaderResources(0, 4, stepViews);
                context->PSSetShader(focusStepShader, nullptr, 0);
                context->Draw(3, 0);
                ID3D11RenderTargetView* noTarget = nullptr;
                context->OMSetRenderTargets(1, &noTarget, nullptr);
                focusCurrent = next;
                focusValid = true;
                focusView = focusViews[focusCurrent];
                context->RSSetViewports(1, &viewport);
                context->OMSetRenderTargets(1, &backBufferView, nullptr);
            }
            else if (blur.type == ScreenBlur::LENS)
            {
                focusValid = false;
            }

            // the depth probe: the pin's, the centre's and a grid's, read back a few frames later (ReadProbe)
            if (blur.type == ScreenBlur::LENS && probeShader && !probePending)
            {
                D3D11_VIEWPORT probeViewport{0.0f, 0.0f, static_cast<float>(DEPTH_SAMPLES), 1.0f, 0.0f, 1.0f};
                context->RSSetViewports(1, &probeViewport);
                context->OMSetRenderTargets(1, &probeTarget, nullptr);
                ID3D11ShaderResourceView* probeViews[4] = {nullptr, depthView, nullptr, hardwareView};
                context->PSSetShaderResources(0, 4, probeViews);
                context->PSSetShader(probeShader, nullptr, 0);
                context->Draw(3, 0);
                ID3D11RenderTargetView* noTarget = nullptr;
                context->OMSetRenderTargets(1, &noTarget, nullptr);
                context->CopyResource(probeStaging, probeTexture);
                probePending = true;
                probingAt = glm::vec2(blur.pointX, blur.pointY);
                lastProbeSent = GetTickCount64();
                context->RSSetViewports(1, &viewport);
                context->OMSetRenderTargets(1, &backBufferView, nullptr);
            }

            ID3D11ShaderResourceView* none[7] = {};
            ID3D11ShaderResourceView* views[7] = {blurFrameView, depthView, focusView, hardwareView, nullptr, nullptr,
                                                  nullptr};
            // the lens: its colour and blur at half the size, the most blur of each tile and what reaches it, then the
            // gather (not for Show Focus's tint)
            if (blur.type == ScreenBlur::LENS && !blur.showFocus)
            {
                D3D11_VIEWPORT halfViewport{0.0f, 0.0f, static_cast<float>(lensHalfWidth),
                                            static_cast<float>(lensHalfHeight), 0.0f, 1.0f};
                D3D11_VIEWPORT tileViewport{0.0f, 0.0f, static_cast<float>(lensTileWidth),
                                            static_cast<float>(lensTileHeight), 0.0f, 1.0f};
                ID3D11RenderTargetView* noTarget = nullptr;
                context->RSSetViewports(1, &halfViewport);
                context->OMSetRenderTargets(1, &lensHalfTargets[0], nullptr);
                context->PSSetShaderResources(0, 7, views);
                context->PSSetShader(lensPrepareShader, nullptr, 0);
                context->Draw(3, 0);

                context->OMSetRenderTargets(1, &noTarget, nullptr);
                views[4] = lensHalfViews[0];
                context->RSSetViewports(1, &tileViewport);
                context->OMSetRenderTargets(1, &lensTileTargets[0], nullptr);
                context->PSSetShaderResources(0, 7, views);
                context->PSSetShader(lensTileMaxShader, nullptr, 0);
                context->Draw(3, 0);

                context->OMSetRenderTargets(1, &noTarget, nullptr);
                views[6] = lensTileViews[0];
                context->OMSetRenderTargets(1, &lensTileTargets[1], nullptr);
                context->PSSetShaderResources(0, 7, views);
                context->PSSetShader(lensTileDilateShader, nullptr, 0);
                context->Draw(3, 0);

                context->OMSetRenderTargets(1, &noTarget, nullptr);
                views[6] = lensTileViews[1];
                context->RSSetViewports(1, &halfViewport);
                context->OMSetRenderTargets(1, &lensHalfTargets[1], nullptr);
                context->PSSetShaderResources(0, 7, views);
                context->PSSetShader(lensGatherShader, nullptr, 0);
                context->Draw(3, 0);
                context->PSSetShaderResources(0, 7, none);

                context->RSSetViewports(1, &viewport);
                context->OMSetRenderTargets(1, &backBufferView, nullptr);
                views[4] = lensHalfViews[1];
                views[5] = lensHalfViews[0];
                views[6] = nullptr;
            }

            context->PSSetShader(blur.type == ScreenBlur::LENS ? lensPixelShader : blurPixelShader, nullptr, 0);
            context->PSSetShaderResources(0, 7, views);
            context->Draw(3, 0);

            // (the copy isn't left bound, which the game's next frame could otherwise find)
            context->PSSetShaderResources(0, 7, none);
            state.Restore();
        }

        // The sky: each pixel's direction from the view, the image's place for it (a panorama's, or a photo's from the
        // horizon up) or the gradient's colour, where the depth buffer has nothing; softly at the edges (the part of a
        // few taps around that are the sky)
        constexpr char SKY_SHADER[] = R"(
cbuffer Sky : register(b0)
{
    float3 forward; float tanX;
    float3 left; float tanY;
    float3 up; float kind;          // 0 the image, 1 the gradient
    float mapping; float rotation; float horizon; float photoWidth;   // 0 panorama, 1 photo; radians
    float3 tint; float brightness;
    float saturation; float opacity; float feather; float imageAspect;  // feather in pixels; width / height
    float2 pixel; float lod; float softness;
    float3 top; float hasImage;
    float3 horizonColor; float hdr;     // the image is linear light (an HDRI)
    float3 bottom; float exposure;      // its exposure (stops)
    float lightColor; float lightBrightness; float haze; float hazeDistance;  // the map lit to match it
    float hazeStart; float zNear; float depthMode; float ambientLod;        // depthMode: 1 the buffer, 2 reversed
};
Texture2D frame : register(t0);
Texture2D<float> depthBuffer : register(t1);
Texture2D image : register(t2);
SamplerState pointSampler : register(s0);
SamplerState panoramaSampler : register(s1);
SamplerState photoSampler : register(s2);
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUTPUT vs(uint id : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
float isSky(float2 uv)
{
    float z = depthBuffer.SampleLevel(pointSampler, saturate(uv), 0);
    return (z >= 1.0f || z <= 0.0f) ? 1.0f : 0.0f;
}
// the sky's colour (graded as it's drawn) toward a direction, from the image's mip lod
float3 skyColor(float3 direction, float mip)
{
    float yaw = atan2(direction.y, direction.x) + rotation;
    float pitch = asin(clamp(direction.z, -1.0f, 1.0f)) - horizon;
    float3 color;
    if (kind > 0.5f || hasImage < 0.5f)
    {
        // the gradient: the horizon's colour, to the top's above and the bottom's below
        float rise = pitch / 1.5707963f;
        float blendUp = pow(saturate(rise / max(softness, 0.01f) * 0.35f), 0.6f);
        float blendDown = saturate(-rise / max(softness, 0.01f));
        color = rise >= 0.0f ? lerp(horizonColor, top, blendUp) : lerp(horizonColor, bottom, blendDown);
    }
    else if (mapping < 0.5f)
    {
        float2 uv = float2(frac(0.5f - yaw / 6.28318531f), 0.5f - pitch / 3.14159265f);
        color = image.SampleLevel(panoramaSampler, uv, mip).rgb;
    }
    else
    {
        float across = yaw / max(photoWidth, 0.01f) + 0.5f;
        float upward = pitch / max(photoWidth / max(imageAspect, 0.01f), 0.01f);
        color = image.SampleLevel(photoSampler, float2(across, 1.0f - upward), mip).rgb;
    }
    // an HDRI: exposed, tone mapped (ACES, fitted) and to the picture's gamma
    if (hdr > 0.5f && kind < 0.5f)
    {
        float3 x = max(color, 0.0f) * exp2(exposure);
        x = saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
        color = pow(x, 1.0f / 2.2f);
    }
    float grey = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    return lerp(grey.xxx, color, saturation) * tint * brightness;
}
float luma(float3 color) { return dot(color, float3(0.2126f, 0.7152f, 0.0722f)); }
// the light the sky gives: its upper half's colour, averaged (a ring of it up high, a ring low, and the top)
float3 skyLight()
{
    float3 light = skyColor(float3(0.0f, 0.0f, 1.0f), ambientLod) * 2.0f;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float a = i * 0.785398f;
        light += skyColor(float3(cos(a) * 0.82f, sin(a) * 0.82f, 0.57f), ambientLod);
        light += skyColor(float3(cos(a + 0.39f) * 0.97f, sin(a + 0.39f) * 0.97f, 0.24f), ambientLod);
    }
    return light / 18.0f;
}
float4 sky(VS_OUTPUT input) : SV_Target
{
    float3 original = frame.SampleLevel(pointSampler, input.uv, 0).rgb;
    float mask = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            mask += isSky(input.uv + float2(x, y) * pixel * feather);
    mask /= 9.0f;
    float3 direction = normalize(forward - (input.uv.x * 2.0f - 1.0f) * tanX * left + (1.0f - input.uv.y * 2.0f) * tanY * up);

    // the map lit to match: the colour of the sky's light (its hue, the brightness kept), its brightness (to a
    // daylight sky's), and haze of the sky behind toward what's far
    float3 lit = original;
    if (mask < 1.0f && (lightColor > 0.0f || lightBrightness > 0.0f || haze > 0.0f))
    {
        float3 light = max(skyLight(), 0.0001f);
        float level = max(luma(light), 0.0001f);
        float3 hue = light / level;
        hue *= min(1.6f / max(hue.r, max(hue.g, hue.b)), 1.0f);  // (not too strong a colour)
        lit *= lerp(1.0f.xxx, hue, lightColor * opacity);
        lit *= lerp(1.0f, clamp(level / 0.45f, 0.15f, 1.6f), lightBrightness * opacity);
        if (haze > 0.0f)
        {
            float z = depthBuffer.SampleLevel(pointSampler, input.uv, 0);
            float distance = depthMode > 1.5f ? zNear / max(z, 0.000001f) : zNear / max(1.0f - z, 0.000001f);
            float amount = 1.0f - exp(-max(distance - hazeStart, 0.0f) / max(hazeDistance, 1.0f));
            float3 behind = skyColor(direction, ambientLod - 2.0f);
            lit = lerp(lit, behind, saturate(amount) * haze * opacity);
        }
    }
    if (mask <= 0.0f)
        return float4(lit, 1.0f);
    float3 color = skyColor(direction, lod);
    return float4(lerp(lit, color, mask * opacity), 1.0f);
}
)";

        struct SkyConstants
        {
            float forward[3];
            float tanX;
            float left[3];
            float tanY;
            float up[3];
            float kind;
            float mapping;
            float rotation;
            float horizon;
            float photoWidth;
            float tint[3];
            float brightness;
            float saturation;
            float opacity;
            float feather;
            float imageAspect;
            float pixel[2];
            float lod;
            float softness;
            float top[3];
            float hasImage;
            float horizonColor[3];
            float hdr;
            float bottom[3];
            float exposure;
            float lightColor;
            float lightBrightness;
            float haze;
            float hazeDistance;
            float hazeStart;
            float zNear;
            float depthMode;
            float ambientLod;
        };
        static_assert(sizeof(SkyConstants) % 16 == 0);

        bool MakeSky()
        {
            if (std::exchange(skyTried, true))
                return skyPixelShader != nullptr;
            const auto compile = [](const char* entry, const char* target, ID3DBlob*& code) {
                ID3DBlob* errors = nullptr;
                const auto result = D3DCompile(SKY_SHADER, sizeof(SKY_SHADER) - 1, "Sky", nullptr, nullptr, entry,
                                               target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
                if (FAILED(result))
                    LOG_ERROR("Couldn't compile the sky's {} ({})", entry,
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
                SafeRelease(errors);
                return SUCCEEDED(result);
            };
            ID3DBlob* vertexCode = nullptr;
            ID3DBlob* pixelCode = nullptr;
            bool made = compile("vs", "vs_4_0", vertexCode) && compile("sky", "ps_4_0", pixelCode);
            made = made && SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                                vertexCode->GetBufferSize(), nullptr,
                                                                &skyVertexShader));
            made = made && SUCCEEDED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                                                               nullptr, &skyPixelShader));
            SafeRelease(vertexCode);
            SafeRelease(pixelCode);
            D3D11_BUFFER_DESC constantsDesc{};
            constantsDesc.ByteWidth = sizeof(SkyConstants);
            constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            made = made && SUCCEEDED(device->CreateBuffer(&constantsDesc, nullptr, &skyConstants));
            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            made = made && SUCCEEDED(device->CreateSamplerState(&samplerDesc, &skyPanoramaSampler));
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR;
            made = made && SUCCEEDED(device->CreateSamplerState(&samplerDesc, &skyPhotoSampler));
            if (!made)
            {
                LOG_ERROR("Couldn't make the sky; it isn't drawn");
                ReleaseSky();
                skyTried = true;
            }
            return made;
        }

        // The sky's image read with D3DX11 (PNG, JPEG, BMP, TIFF, DDS...) into a texture with its mips, again when its
        // path changes; none when it can't be
        void LoadSkyImage(const std::string& path)
        {
            if (path == skyImagePath)
                return;
            skyImagePath = path;
            SafeRelease(skyImageView);
            SafeRelease(skyImage);
            skyImageWidth = skyImageHeight = 0;
            skyImageHdr = false;
            if (path.empty())
                return;
            // an HDRI (.hdr, .exr): read here into a 16 bit float texture, at most 8192 across, with its mips
            if (HdrImage::IsHdr(path))
            {
                std::string error;
                auto image = HdrImage::Load(std::filesystem::path(path), error);
                if (!image)
                {
                    LOG_ERROR("Couldn't read the sky's image {}: {}", path, error);
                    return;
                }
                const auto read = std::make_pair(image->width, image->height);
                HdrImage::ShrinkTo(*image, 8192);
                std::vector<std::uint16_t> halves(image->rgba.size());
                for (std::size_t i = 0; i < halves.size(); ++i)
                    halves[i] = HdrImage::ToHalf(image->rgba[i]);
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = image->width;
                desc.Height = image->height;
                desc.MipLevels = 0;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &skyImage)) ||
                    FAILED(device->CreateShaderResourceView(skyImage, nullptr, &skyImageView)))
                {
                    LOG_ERROR("Couldn't make a {}x{} texture for the sky", image->width, image->height);
                    SafeRelease(skyImageView);
                    SafeRelease(skyImage);
                    return;
                }
                context->UpdateSubresource(skyImage, 0, nullptr, halves.data(), image->width * 8, 0);
                context->GenerateMips(skyImageView);
                skyImageWidth = image->width;
                skyImageHeight = image->height;
                skyImageHdr = true;
                LOG_INFO("Loaded the sky's HDR image {} ({}x{}, used at {}x{})", path, read.first, read.second,
                         skyImageWidth, skyImageHeight);
                return;
            }
            // (the file's bytes to D3DX, as the texture pack's are: its defaults make every mip)
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            HRESULT result = S_OK;
            if (bytes.empty() ||
                FAILED(D3DX11CreateShaderResourceViewFromMemory(device, bytes.data(), bytes.size(), nullptr, nullptr,
                                                                &skyImageView, &result)) ||
                !skyImageView)
            {
                LOG_ERROR("Couldn't read the sky's image {} ({:#x})", path, static_cast<std::uint32_t>(result));
                SafeRelease(skyImageView);
                return;
            }
            ID3D11Resource* resource = nullptr;
            skyImageView->GetResource(&resource);
            if (resource)
                resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&skyImage));
            SafeRelease(resource);
            if (skyImage)
            {
                D3D11_TEXTURE2D_DESC desc{};
                skyImage->GetDesc(&desc);
                skyImageWidth = desc.Width;
                skyImageHeight = desc.Height;
            }
            LOG_INFO("Loaded the sky's image {} ({}x{})", path, skyImageWidth, skyImageHeight);
        }

        ID3D11ShaderResourceView* UsableDepth(ID3D11ShaderResourceView* const* where, const char* kind);

        // The sky drawn into the frame's back buffer, while it's on and the view and the depth buffer are known
        void DrawSky(IDXGISwapChain* swapChain)
        {
            if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_SkyReplace) ||
                Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            const auto s = Components::SkyReplace::GetSettings();
            if (!s.enabled || s.opacity <= 0.0f)
                return;
            const auto view = Components::Tracking::GetView();
            const auto [depthWhere, zNear] = Components::LensFlare::GetDepthBuffer();
            ID3D11ShaderResourceView* depthView = UsableDepth(depthWhere, "depth buffer");
            if (!view || !depthView)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur() || !MakeSky())
                return;
            LoadSkyImage(s.kind == Components::SkyReplace::Kind::Image ? s.imagePath : std::string());
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            const bool copied = CopyFrameForBlur(backBuffer);
            backBuffer->Release();
            if (!copied)
                return;
            const float width = static_cast<float>(blurFrameDesc.Width);
            const float height = static_cast<float>(std::max(blurFrameDesc.Height, 1u));

            SkyConstants constants{};
            for (int i = 0; i < 3; ++i)
            {
                constants.forward[i] = view->axis[0][i];
                constants.left[i] = view->axis[1][i];
                constants.up[i] = view->axis[2][i];
                constants.tint[i] = s.tint[i];
                constants.top[i] = s.top[i];
                constants.horizonColor[i] = s.horizonColor[i];
                constants.bottom[i] = s.bottom[i];
            }
            constants.tanX = view->tanHalfFovX;
            constants.tanY = view->tanHalfFovY;
            constants.kind = s.kind == Components::SkyReplace::Kind::Gradient ? 1.0f : 0.0f;
            constants.mapping = s.mapping == Components::SkyReplace::Mapping::Photo ? 1.0f : 0.0f;
            const float seconds = static_cast<float>(Components::Playback::GetTimelineTick()) / 1000.0f;
            constants.rotation = (s.rotation + s.drift * seconds) * 3.14159265f / 180.0f;
            constants.horizon = s.horizon * 3.14159265f / 180.0f;
            constants.photoWidth = std::max(s.photoWidth, 1.0f) * 3.14159265f / 180.0f;
            constants.brightness = std::max(s.brightness, 0.0f);
            constants.saturation = std::max(s.saturation, 0.0f);
            constants.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
            constants.feather = std::max(s.feather, 0.0f);
            constants.hasImage = skyImageView ? 1.0f : 0.0f;
            constants.imageAspect =
                skyImageHeight ? static_cast<float>(skyImageWidth) / static_cast<float>(skyImageHeight) : 1.0f;
            constants.pixel[0] = 1.0f / width;
            constants.pixel[1] = 1.0f / height;
            constants.softness = std::max(s.horizonSoftness, 0.01f);
            constants.hdr = skyImageHdr ? 1.0f : 0.0f;
            constants.exposure = s.exposure;
            if (s.matchLighting)
            {
                constants.lightColor = std::clamp(s.lightColor, 0.0f, 1.0f);
                constants.lightBrightness = std::clamp(s.lightBrightness, 0.0f, 1.0f);
                constants.haze = std::clamp(s.haze, 0.0f, 1.0f);
            }
            constants.hazeDistance = std::max(s.hazeDistance, 1.0f);
            constants.hazeStart = std::max(s.hazeStart, 0.0f);
            constants.zNear = std::max(zNear, 0.001f);
            constants.depthMode = depthMode == 2 ? 2.0f : 1.0f;
            // the sky's light from a mip of it some 16 texels across (the whole of a gradient's worked out)
            constants.ambientLod =
                skyImageWidth ? std::max(std::log2(static_cast<float>(skyImageWidth) / 16.0f), 0.0f) : 0.0f;
            // the image's mip for a screen pixel's share of it (across the view)
            if (skyImageWidth)
            {
                const float viewAngle = 2.0f * std::atan(view->tanHalfFovX);
                const float imageAngle = s.mapping == Components::SkyReplace::Mapping::Photo
                                             ? constants.photoWidth
                                             : 6.28318531f;
                const float texelsPerPixel = static_cast<float>(skyImageWidth) * viewAngle / imageAngle / width;
                constants.lod = std::max(std::log2(std::max(texelsPerPixel, 1e-6f)), 0.0f);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(skyConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(skyConstants, 0);

            ContextState state;
            state.Save();
            D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(blurRasterizer);
            const float factor[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            context->OMSetBlendState(opaqueBlendState, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(skyVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(skyPixelShader, nullptr, 0);
            ID3D11SamplerState* samplers[3] = {pointSampler, skyPanoramaSampler, skyPhotoSampler};
            context->PSSetSamplers(0, 3, samplers);
            context->PSSetConstantBuffers(0, 1, &skyConstants);
            ID3D11ShaderResourceView* views[3] = {blurFrameView, depthView, skyImageView};
            context->PSSetShaderResources(0, 3, views);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[3] = {};
            context->PSSetShaderResources(0, 3, none);
            state.Restore();
        }

        // The lens dirt: the dirt (made here, from a few layers of scattered soft specks and of noise, or the image's),
        // times the light on it (the blurred frame's bright parts, and the flare's light around where it is unless
        // something's in front of it), added to the frame
        constexpr char DIRT_SHADER[] = R"(
cbuffer Dirt : register(b0)
{
    float aspect; float amount; float threshold; float glowLod;
    float3 tint; float style;           // 0 dust, 1 smudges, 2 both, 3 water drops, 4 the image
    float density; float size; float seed; float hasImage;
    float2 light; float flareOn; float flareStrength;  // light: uv
    float3 flareColor; float flareSpread;
    float hide; float lightDepth; float zNear; float depthMode;
    float imageAspect; float softness; float hasGlow; float hasDepth;
    float blendMode; float imageRotation; float imageScale; float unused0;  // the image's: radians, times
};
Texture2D frame : register(t0);
Texture2D glow : register(t1);
Texture2D image : register(t2);
Texture2D<float> depthBuffer : register(t3);
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float hash(float2 p) { return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f); }
float2 hash2(float2 p) { return frac(sin(float2(dot(p, float2(127.1f, 311.7f)), dot(p, float2(269.5f, 183.3f)))) * 43758.5453f); }
float noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
float fbm(float2 p)
{
    float sum = 0.0f, weight = 0.5f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        sum += noise(p) * weight;
        p = p * 2.03f + 17.0f;
        weight *= 0.5f;
    }
    return sum;
}
// soft round specks scattered on a grid of cells (one at most in each, some cells empty): their brightness, with a
// brighter rim (rim 0 to 1)
float specks(float2 p, float cells, float radius, float present, float rim, float layer)
{
    p *= cells;
    float2 cell = floor(p);
    float result = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 c = cell + float2(x, y);
            float2 key = c + seed * 7.31f + layer * 31.7f;
            float2 center = c + hash2(key);
            float r = radius * lerp(0.35f, 1.0f, hash(key + 9.7f));
            float d = length(p - center) / r;
            float edge = lerp(0.04f, 0.5f, softness);
            float disc = 1.0f - smoothstep(1.0f - edge, 1.0f, d);
            float ring = disc * smoothstep(0.3f, 1.0f, d);
            float shown = hash(key + 3.1f) <= present ? 1.0f : 0.0f;
            result += shown * lerp(disc, ring * 1.6f, rim) * lerp(0.25f, 1.0f, hash(key + 5.3f));
        }
    return result;
}
float3 dirt(float2 uv)
{
    if (style > 3.5f)
    {
        if (hasImage < 0.5f)
            return 0.0f.xxx;
        // the image covering the picture (cropped, not stretched), turned and scaled about its centre; black beyond it
        float2 a = (uv - 0.5f) * float2(aspect, 1.0f);
        float c = cos(imageRotation), s = sin(imageRotation);
        a = float2(a.x * c - a.y * s, a.x * s + a.y * c) / max(imageScale, 0.05f);
        float2 cover = imageAspect > aspect ? float2(imageAspect, 1.0f) : float2(aspect, aspect / imageAspect);
        float2 at = a / cover + 0.5f;
        float inside = all(at >= 0.0f) && all(at <= 1.0f) ? 1.0f : 0.0f;
        return image.SampleLevel(linearSampler, at, softness * 3.0f).rgb * inside;
    }
    float2 p = (uv - 0.5f) * float2(aspect, 1.0f) + seed * 0.137f;
    float scale = 1.0f / max(size, 0.05f);
    float result = 0.0f;
    if (style < 0.5f || (style > 1.5f && style < 2.5f))
    {
        float present = lerp(0.08f, 0.6f, density);
        result += specks(p, 5.0f * scale, 0.45f, present * 0.6f, 0.25f, 0.0f) * 0.9f;
        result += specks(p, 11.0f * scale, 0.4f, present, 0.15f, 1.0f) * 0.6f;
        result += specks(p, 26.0f * scale, 0.35f, present, 0.0f, 2.0f) * 0.35f;
    }
    if (style > 0.5f && style < 2.5f)
    {
        // smudges: patches of noise, and streaks of it (as a wiped lens's), thinned by the density
        float patches = smoothstep(lerp(0.75f, 0.4f, density), 0.85f, fbm(p * 2.2f + 4.0f));
        float2 wipe = float2(p.x * 0.6f + p.y * 0.8f, p.y * 0.6f - p.x * 0.8f);
        float streaks = smoothstep(lerp(0.7f, 0.45f, density), 0.9f, fbm(wipe * float2(1.5f, 9.0f) + 11.0f));
        float detail = lerp(0.6f, 1.0f, noise(p * 40.0f));
        result += (patches * 0.8f + streaks * 0.5f) * detail * (style > 1.5f ? 0.7f : 1.0f);
    }
    if (style > 2.5f)
    {
        // drops: their rims brighter, the light bent round them
        float present = lerp(0.1f, 0.65f, density);
        result += specks(p, 4.0f * scale, 0.42f, present * 0.7f, 0.75f, 3.0f) * 0.9f;
        result += specks(p, 9.0f * scale, 0.38f, present, 0.6f, 4.0f) * 0.6f;
        result += specks(p, 20.0f * scale, 0.3f, present, 0.3f, 5.0f) * 0.3f;
    }
    return result.xxx;
}
float isSky(float z) { return (z >= 1.0f || z <= 0.0f) ? 1.0f : 0.0f; }
// how much of the flare's light is seen (a few taps of the depth buffer about it)
float flareSeen()
{
    if (hide < 0.5f || hasDepth < 0.5f)
        return 1.0f;
    float seen = 0.0f;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        float2 offset = i == 0 ? 0.0f.xx : float2(cos(i * 1.5708f), sin(i * 1.5708f)) * 0.004f;
        float z = depthBuffer.SampleLevel(pointSampler, saturate(light + offset), 0);
        float distance = depthMode > 1.5f ? zNear / max(z, 0.000001f) : zNear / max(1.0f - z, 0.000001f);
        seen += lightDepth > 900000.0f ? isSky(z) : (isSky(z) > 0.5f || distance >= lightDepth * 0.9f ? 1.0f : 0.0f);
    }
    return seen / 5.0f;
}
float4 lensDirt(VS_OUTPUT input) : SV_Target
{
    float3 original = frame.SampleLevel(pointSampler, input.uv, 0).rgb;
    float3 lit = 0.0f.xxx;
    if (hasGlow > 0.5f)
    {
        // the bright parts, blurred twice as wide again, and the whole picture's a little
        float3 near = glow.SampleLevel(linearSampler, input.uv, glowLod).rgb;
        float3 wide = glow.SampleLevel(linearSampler, input.uv, glowLod + 1.5f).rgb;
        float3 all = glow.SampleLevel(linearSampler, float2(0.5f, 0.5f), 16.0f).rgb;
        float3 bright = near * 0.5f + wide * 0.4f + all * 0.25f;
        lit += max(bright - threshold, 0.0f) / max(1.0f - threshold, 0.05f);
    }
    if (flareOn > 0.5f)
    {
        float2 d = (input.uv - light) * float2(aspect, 1.0f);
        float falloff = exp(-dot(d, d) / max(flareSpread * flareSpread, 0.0001f));
        lit += flareColor * flareStrength * falloff * flareSeen();
    }
    float3 layer = max(dirt(input.uv) * lit * tint * amount, 0.0f);
    float3 o = saturate(original);
    float3 l = saturate(layer);
    float3 color;
    if (blendMode < 0.5f)
        color = original + layer;                                   // Add
    else if (blendMode < 1.5f)
        color = 1.0f - (1.0f - o) * (1.0f - l);                     // Screen
    else if (blendMode < 2.5f)
        color = max(original, layer);                               // Lighten
    else if (blendMode < 3.5f)
    {
        // Overlay: the dirt lightening the picture's contrast (none where it isn't lit)
        float3 b = 0.5f + 0.5f * l;
        color = o < 0.5f ? 2.0f * o * b : 1.0f - 2.0f * (1.0f - o) * (1.0f - b);
    }
    else if (blendMode < 4.5f)
    {
        // Soft Light (as Photoshop's), the same
        float3 b = 0.5f + 0.5f * l;
        color = b < 0.5f ? o - (1.0f - 2.0f * b) * o * (1.0f - o)
                         : o + (2.0f * b - 1.0f) * ((o < 0.25f ? ((16.0f * o - 12.0f) * o + 4.0f) * o : sqrt(o)) - o);
    }
    else if (blendMode < 5.5f)
        color = o / max(1.0f - l * 0.95f, 0.05f);                   // Color Dodge
    else
        color = original * (1.0f - l);                              // Multiply: the dirt dark where it's lit
    return float4(color, 1.0f);
}
)";

        struct DirtConstants
        {
            float aspect;
            float amount;
            float threshold;
            float glowLod;
            float tint[3];
            float style;
            float density;
            float size;
            float seed;
            float hasImage;
            float light[2];
            float flareOn;
            float flareStrength;
            float flareColor[3];
            float flareSpread;
            float hide;
            float lightDepth;
            float zNear;
            float depthMode;
            float imageAspect;
            float softness;
            float hasGlow;
            float hasDepth;
            float blendMode;
            float imageRotation;
            float imageScale;
            float unused0;
        };
        static_assert(sizeof(DirtConstants) % 16 == 0);

        bool MakeDirt()
        {
            if (std::exchange(dirtTried, true))
                return dirtPixelShader != nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            bool made = SUCCEEDED(D3DCompile(DIRT_SHADER, sizeof(DIRT_SHADER) - 1, "LensDirt", nullptr, nullptr,
                                             "lensDirt", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
            if (!made)
                LOG_ERROR("Couldn't compile the lens dirt ({})",
                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
            SafeRelease(errors);
            made = made && SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                               &dirtPixelShader));
            SafeRelease(code);
            D3D11_BUFFER_DESC constantsDesc{};
            constantsDesc.ByteWidth = sizeof(DirtConstants);
            constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            made = made && SUCCEEDED(device->CreateBuffer(&constantsDesc, nullptr, &dirtConstants));
            if (!made)
            {
                LOG_ERROR("Couldn't make the lens dirt; it isn't drawn");
                ReleaseDirt();
                dirtTried = true;
            }
            return made;
        }

        // The dirt's image read with D3DX11 (with its mips, for its softness), again when its path changes
        void LoadDirtImage(const std::string& path)
        {
            if (path == dirtImagePath)
                return;
            dirtImagePath = path;
            SafeRelease(dirtImageView);
            dirtImageWidth = dirtImageHeight = 0;
            if (path.empty())
                return;
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            HRESULT result = S_OK;
            if (bytes.empty() ||
                FAILED(D3DX11CreateShaderResourceViewFromMemory(device, bytes.data(), bytes.size(), nullptr, nullptr,
                                                                &dirtImageView, &result)) ||
                !dirtImageView)
            {
                LOG_ERROR("Couldn't read the lens dirt's image {} ({:#x})", path, static_cast<std::uint32_t>(result));
                SafeRelease(dirtImageView);
                return;
            }
            ID3D11Resource* resource = nullptr;
            dirtImageView->GetResource(&resource);
            ID3D11Texture2D* texture = nullptr;
            if (resource)
                resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
            SafeRelease(resource);
            if (texture)
            {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                dirtImageWidth = desc.Width;
                dirtImageHeight = desc.Height;
                texture->Release();
            }
            LOG_INFO("Loaded the lens dirt's image {} ({}x{})", path, dirtImageWidth, dirtImageHeight);
        }

        // The frame (already copied for the blur) copied again into a texture with its mips made: the blurred picture
        // the dirt's light is read from
        bool CopyDirtGlow()
        {
            if (dirtGlowFailed)
                return false;
            if (!dirtGlow || dirtGlowDesc.Width != blurFrameDesc.Width || dirtGlowDesc.Height != blurFrameDesc.Height ||
                dirtGlowDesc.Format != blurFrameDesc.Format)
            {
                SafeRelease(dirtGlowView);
                SafeRelease(dirtGlow);
                UINT support = 0;
                if (FAILED(device->CheckFormatSupport(blurFrameDesc.Format, &support)) ||
                    !(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
                {
                    LOG_INFO("The frame's format ({}) can't have mips made: the lens dirt is lit by the flare alone",
                             static_cast<int>(blurFrameDesc.Format));
                    dirtGlowFailed = true;
                    return false;
                }
                D3D11_TEXTURE2D_DESC desc = blurFrameDesc;
                desc.MipLevels = 0;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &dirtGlow)) ||
                    FAILED(device->CreateShaderResourceView(dirtGlow, nullptr, &dirtGlowView)))
                {
                    LOG_ERROR("Couldn't make the lens dirt's blurred frame: it's lit by the flare alone");
                    SafeRelease(dirtGlowView);
                    SafeRelease(dirtGlow);
                    dirtGlowFailed = true;
                    return false;
                }
                dirtGlowDesc = blurFrameDesc;
            }
            context->CopySubresourceRegion(dirtGlow, 0, 0, 0, 0, blurFrame, 0, nullptr);
            context->GenerateMips(dirtGlowView);
            return true;
        }

        // The lens dirt drawn into the frame's back buffer, while it's on
        void DrawLensDirt(IDXGISwapChain* swapChain)
        {
            if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare) ||
                Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            const auto s = Components::LensDirt::GetSettings();
            if (!s.enabled || s.amount <= 0.0f)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur() || !MakeDirt())
                return;
            LoadDirtImage(s.style == Components::LensDirt::Style::Image ? s.imagePath : std::string());
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            const bool copied = CopyFrameForBlur(backBuffer);
            backBuffer->Release();
            if (!copied)
                return;
            const bool hasGlow = CopyDirtGlow();
            const float width = static_cast<float>(blurFrameDesc.Width);
            const float height = static_cast<float>(std::max(blurFrameDesc.Height, 1u));

            DirtConstants constants{};
            constants.aspect = width / height;
            constants.amount = std::max(s.amount, 0.0f);
            constants.threshold = std::clamp(s.threshold, 0.0f, 0.98f);
            // the light read from a mip some 6 (spread 1) to 48 (spread 0) texels high
            constants.glowLod = std::max(std::log2(height / (48.0f * std::pow(0.125f, std::clamp(s.spread, 0.0f, 1.0f)))), 0.0f);
            for (int i = 0; i < 3; ++i)
                constants.tint[i] = s.tint[i];
            constants.style = static_cast<float>(static_cast<int>(s.style));
            constants.density = std::clamp(s.density, 0.0f, 1.0f);
            constants.size = std::max(s.size, 0.05f);
            constants.seed = static_cast<float>(s.seed % 1000);
            constants.hasImage = dirtImageView ? 1.0f : 0.0f;
            constants.imageAspect =
                dirtImageHeight ? static_cast<float>(dirtImageWidth) / static_cast<float>(dirtImageHeight) : 1.0f;
            constants.softness = std::clamp(s.softness, 0.0f, 1.0f);
            constants.hasGlow = hasGlow ? 1.0f : 0.0f;
            constants.blendMode = static_cast<float>(static_cast<int>(s.blend));
            constants.imageRotation = s.imageRotation * 3.14159265f / 180.0f;
            constants.imageScale = std::max(s.imageScale, 0.05f);

            // the lens flare's light, while there's a flare
            ID3D11ShaderResourceView* depthView = nullptr;
            if (s.flareLight > 0.0f)
                if (const auto flare = Components::LensFlare::GetFrame(Components::Playback::GetTimelineTick());
                    flare && flare->light && flare->settings.source != Components::LensFlare::Source::Auto)
                {
                    // (Auto's lights are found on the picture: the bright parts light the dirt about them already)
                    constants.flareOn = 1.0f;
                    constants.light[0] = flare->light->x;
                    constants.light[1] = flare->light->y;
                    constants.flareStrength = s.flareLight * std::max(flare->settings.intensity, 0.0f);
                    for (int i = 0; i < 3; ++i)
                        constants.flareColor[i] = flare->settings.color[i];
                    constants.flareSpread = std::max(s.flareSpread, 0.02f);
                    constants.hide = flare->settings.hideBehind ? 1.0f : 0.0f;
                    constants.lightDepth = flare->lightDepth;
                    depthView = UsableDepth(flare->depth, "depth buffer");
                }
            constants.hasDepth = depthView ? 1.0f : 0.0f;
            constants.zNear = std::max(Components::LensFlare::GetDepthBuffer().second, 0.001f);
            constants.depthMode = depthMode == 2 ? 2.0f : 1.0f;
            if (!hasGlow && constants.flareOn < 0.5f)
                return;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(dirtConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(dirtConstants, 0);

            ContextState state;
            state.Save();
            D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(blurRasterizer);
            const float factor[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            context->OMSetBlendState(opaqueBlendState, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(blurVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(dirtPixelShader, nullptr, 0);
            ID3D11SamplerState* samplers[2] = {pointSampler, blurSampler};
            context->PSSetSamplers(0, 2, samplers);
            context->PSSetConstantBuffers(0, 1, &dirtConstants);
            ID3D11ShaderResourceView* views[4] = {blurFrameView, hasGlow ? dirtGlowView : nullptr, dirtImageView,
                                                  depthView};
            context->PSSetShaderResources(0, 4, views);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[4] = {};
            context->PSSetShaderResources(0, 4, none);
            state.Restore();
        }

        // The lens: each pixel's place in the frame as the game drew it (rectilinear, at the view the lens set): a
        // rectilinear lens's bend (r (1 + k r^2), scaled so the picture stays filled), or a fisheye's (equisolid: the
        // angle off the middle from the distance, then where that angle is in the game's view); red and blue a little
        // apart toward the edges, and the corners darker
        constexpr char LENS_SHADER[] = R"(
cbuffer Lens : register(b0)
{
    float aspect;      // height / width
    float projection;  // 0 rectilinear, 1 fisheye
    float bend;        // rectilinear: k
    float fit;         // rectilinear: what the bend is divided by, so the picture stays filled
    float halfView;    // fisheye: half its view across (or the circle's), radians
    float renderTan;   // fisheye: tan(half the view across) the game drew
    float circular;
    float chromatic;
    float vignette;
    float2 pixel;
    float unused0;
};
Texture2D frame : register(t0);
SamplerState linearSampler : register(s0);
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// where to read for a point (half widths from the middle, y the same units), and how much of it is there (0 beyond)
float3 source(float2 p)
{
    if (projection < 0.5f)
    {
        float2 q = p * (1.0f + bend * dot(p, p)) / fit;
        return float3(0.5f + 0.5f * q.x, 0.5f + 0.5f * q.y / aspect, 1.0f);
    }
    float unit = circular > 0.5f ? aspect : 1.0f;
    float r = length(p);
    float s = r / unit * sin(halfView * 0.5f);
    float shown = (circular > 0.5f ? 1.0f - smoothstep(unit - 1.5f * pixel.x * 2.0f, unit, r) : 1.0f) *
                  (1.0f - smoothstep(0.985f, 1.0f, s));
    float theta = 2.0f * asin(min(s, 0.9999f));
    float2 direction = r > 0.0f ? p / r : float2(0.0f, 0.0f);
    float2 q = direction * tan(min(theta, 1.5533f)) / renderTan;
    // (the game's view ends at its edges: what's beyond them fades out)
    float2 edge = abs(q) / float2(1.0f, aspect);
    shown *= 1.0f - smoothstep(0.995f, 1.0f, max(edge.x, edge.y));
    return float3(0.5f + 0.5f * q.x, 0.5f + 0.5f * q.y / aspect, shown);
}
float4 lensBend(VS_OUTPUT input) : SV_Target
{
    float2 p = (input.uv * 2.0f - 1.0f) * float2(1.0f, aspect);
    float spread = chromatic * 0.012f;
    float3 r = source(p * (1.0f + spread));
    float3 g = source(p);
    float3 b = source(p * (1.0f - spread));
    float3 color = float3(frame.SampleLevel(linearSampler, r.xy, 0).r * r.z, frame.SampleLevel(linearSampler, g.xy, 0).g * g.z,
                          frame.SampleLevel(linearSampler, b.xy, 0).b * b.z);
    // the corners darker (as cos^4 of a wide lens's angle, eased)
    float corner = dot(p, p) / (1.0f + aspect * aspect);
    color *= lerp(1.0f, saturate(1.0f - corner * corner * 1.2f) * (1.0f - 0.35f * corner), vignette);
    return float4(color, 1.0f);
}
)";

        struct LensConstants
        {
            float aspect;
            float projection;
            float bend;
            float fit;
            float halfView;
            float renderTan;
            float circular;
            float chromatic;
            float vignette;
            float pixel[2];
            float unused0;
        };
        static_assert(sizeof(LensConstants) % 16 == 0);

        bool MakeLens()
        {
            if (std::exchange(lensTried, true))
                return lensBendShader != nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            bool made = SUCCEEDED(D3DCompile(LENS_SHADER, sizeof(LENS_SHADER) - 1, "Lens", nullptr, nullptr, "lensBend",
                                             "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
            if (!made)
                LOG_ERROR("Couldn't compile the lens ({})",
                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
            SafeRelease(errors);
            made = made && SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                               &lensBendShader));
            SafeRelease(code);
            D3D11_BUFFER_DESC constantsDesc{};
            constantsDesc.ByteWidth = sizeof(LensConstants);
            constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            made = made && SUCCEEDED(device->CreateBuffer(&constantsDesc, nullptr, &lensBendConstants));
            if (!made)
            {
                LOG_ERROR("Couldn't make the lens; it isn't drawn");
                ReleaseLens();
                lensTried = true;
            }
            return made;
        }

        // The lens drawn over the frame's back buffer, while it's on and bends or marks the picture
        void DrawLens(IDXGISwapChain* swapChain)
        {
            if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare) ||
                Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            const auto s = Components::Lens::GetSettings();
            if (!s.enabled)
                return;
            const bool fisheye = s.projection == Components::Lens::Projection::Fisheye;
            if (!fisheye && std::abs(s.distortion) < 0.001f && s.chromatic <= 0.0f && s.vignette <= 0.0f)
                return;
            // (the fisheye's bend needs the view the game drew for it)
            const auto view = Components::Tracking::GetView();
            if (fisheye && !view)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur() || !MakeLens())
                return;
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            const bool copied = CopyFrameForBlur(backBuffer);
            backBuffer->Release();
            if (!copied)
                return;
            const float width = static_cast<float>(blurFrameDesc.Width);
            const float height = static_cast<float>(std::max(blurFrameDesc.Height, 1u));

            LensConstants constants{};
            constants.aspect = height / width;
            constants.projection = fisheye ? 1.0f : 0.0f;
            // (a quarter of the bend at the slider's end: k up to 0.25 barrel, down to -0.25 pincushion)
            constants.bend = std::clamp(s.distortion, -1.0f, 1.0f) * 0.25f;
            const float a2 = constants.aspect * constants.aspect;
            constants.fit = std::max({1.0f + constants.bend, 1.0f + constants.bend * a2,
                                      1.0f + constants.bend * (1.0f + a2), 0.05f});
            constants.halfView = glm::radians(std::clamp(s.fisheyeFov, 60.0f, 220.0f)) * 0.5f;
            constants.renderTan = view ? std::max(view->tanHalfFovX, 0.01f) : 1.0f;
            constants.circular = s.circular ? 1.0f : 0.0f;
            constants.chromatic = std::clamp(s.chromatic, 0.0f, 1.0f);
            constants.vignette = std::clamp(s.vignette, 0.0f, 1.0f);
            constants.pixel[0] = 1.0f / width;
            constants.pixel[1] = 1.0f / height;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(lensBendConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(lensBendConstants, 0);

            ContextState state;
            state.Save();
            D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(blurRasterizer);
            const float factor[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            context->OMSetBlendState(opaqueBlendState, factor, 0xFFFFFFFF);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(blurVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(lensBendShader, nullptr, 0);
            context->PSSetSamplers(0, 1, &blurSampler);
            context->PSSetConstantBuffers(0, 1, &lensBendConstants);
            context->PSSetShaderResources(0, 1, &blurFrameView);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            context->PSSetShaderResources(0, 1, &none);
            state.Restore();
        }

        // The lens flare, everything of it worked out for each pixel from where the light is: in the picture's height
        // units, from its centre (x across times the aspect). (Two literals: MSVC takes at most 16 KB in one.)
        constexpr char FLARE_SHADER[] = R"(
cbuffer Flare : register(b0)
{
    float2 light;  // uv
    float aspect;
    float intensity;
    float3 color;
    float glow;
    float glowSize;
    float rays;
    float rayAmount;
    float rayLength;
    float rayRotation;  // radians
    float raySharpness;
    float streak;
    float streakLength;
    float3 streakColor;
    float ghosts;
    float ghostAmount;
    float ghostSize;
    float ghostSpacing;
    float ghostShape;  // 0 round, else its sides
    float ghostRainbow;
    float halo;
    float haloRadius;
    float haloCenter;
    float haloRainbow;
    float hideBehind;
    float2 pixel;
    float autoLights;  // Auto: how many lights at most (0: the one light)
    float threshold;   // how bright a light is (0 to 1)
    float2 grid;       // the search's cells across and down
    float separation;  // how far apart two lights are at least (the picture's height)
    float follow;      // the lights followed: how far their brightness goes to the ones found this frame (1 at once)
    float matchRadius; // how far a light can have moved to be the same one
    float unused;
    float lightDepth;      // the one light's distance (the sun's far)
    float depthMode;       // the depth buffer: 2 reversed, else not
    float zNear;
    float maxDistance;     // lights beyond it fade out (0: none do)
    float sizeByDistance;  // 0 to 1: how much farther lights' flares are smaller
    float rotation;        // radians: the whole flare turned about its light (with the spin by now)
    float flicker;         // 0 to 1: how much it flickers
    float flickerSpeed;    // Hz
    float time;            // the film's, seconds
    float rayRainbow;      // 0 to 1: each ray its own hue
    float ghostRing;       // 0 a ghost filled, 1 only its rim
    float arcs;            // how many arcs (crescents beyond the centre)
    float arcAmount;
    float arcSize;
    float2 unused3;
};
Texture2D<float> depthBuffer : register(t0);
Texture2D<float4> lights : register(t1);     // Auto: the lights found (brightness, uv)
Texture2D frame : register(t2);              // Auto's search: the frame
Texture2D<float4> gridCells : register(t3);  // and its cells
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUTPUT vs(uint id : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
float3 hue(float h)
{
    return saturate(abs(frac(h + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
}
float hash(float n)
{
    return frac(sin(n * 12.9898f) * 43758.5453f);
}
// how much of the light shows: the part of a few taps around it where the depth buffer has nothing (the sky: its far
// value, 1, or 0 when reversed); off the picture, all of it
float visibility()
{
    if (hideBehind < 0.5f)
        return 1.0f;
    float seen = 0.0f;
    [unroll] for (int y = -2; y <= 2; ++y)
    {
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            float2 uv = light + float2(x, y) * pixel * 4.0f;
            if (any(uv < 0.0f) || any(uv > 1.0f))
            {
                seen += 1.0f;
                continue;
            }
            float z = depthBuffer.SampleLevel(pointSampler, uv, 0);
            bool sky = z >= 1.0f || z <= 0.0f;
            // a spot of the map (not the sun): seen where nothing is nearer than it (a little give, for the surface
            // it's on)
            float distance = depthMode > 1.5f ? zNear / max(z, 0.0000001f) : zNear / max(1.0f - z, 0.0000001f);
            bool behind = lightDepth < 900000.0f && distance >= lightDepth * 0.95f - 4.0f;
            seen += (sky || behind) ? 1.0f : 0.0f;
        }
    }
    return seen / 25.0f;
}
// how far from a shape's centre, as a part of its size (round, or a polygon of so many sides)
float shapeDistance(float2 q, float sides)
{
    float r = length(q);
    if (sides < 2.5f)
        return r;
    float side = 6.28318531f / sides;
    float a = fmod(atan2(q.y, q.x) + 6.28318531f + side * 0.25f, side) - side * 0.5f;
    return r * cos(a) / cos(side * 0.5f);
}
// the flare of a light at lightUV, at the pixel at uv
float2 turn(float2 v, float angle)
{
    float c = cos(angle), s = sin(angle);
    return float2(v.x * c - v.y * s, v.x * s + v.y * c);
}

float3 flareAt(float2 uv, float2 lightUV, float k)
{
    float2 scale = float2(aspect, 1.0f);
    float2 p = (uv - 0.5f) * scale;
    float2 l = (lightUV - 0.5f) * scale;
    float2 d = p - l;
    float r = length(d);
    float3 c = 0.0f;
    // (the line the ghosts, halo and arcs are on: from the light through the picture's centre, turned by the rotation)
    float2 toCentre = turn(-l, rotation);

    // the glow: a soft one and a hot core
    float size = max(glowSize * k, 0.001f);
    c += color * glow * (exp(-r * r / (size * size) * 2.0f) + 0.5f * exp(-r / (size * 0.12f)));

    // the rays: a star of so many, thinner as sharper
    if (rays >= 1.0f)
    {
        float a = atan2(d.y, d.x) + rayRotation + rotation;
        float sharp = lerp(4.0f, 400.0f, raySharpness * raySharpness);
        float ray = pow(abs(cos(a * rays * 0.5f)), sharp);
        // (each ray its own length, and with Ray Rainbow its own hue)
        float which = floor((a / 6.28318531f + 1.0f) * rays + 0.5f);
        float reach = rayLength * k * (0.6f + 0.4f * hash(which));
        float3 rayColor = lerp(color, lerp(1.0f, hue(frac(which * 0.137f + a * 0.08f)), 0.8f), rayRainbow);
        c += rayColor * rayAmount * ray * exp(-r / max(reach * 0.35f, 0.001f));
    }

    // the anamorphic streak, across
    if (streak > 0.0f)
    {
        float2 across = turn(d, -rotation);
        float along = exp(-abs(across.x) / max(streakLength * k * 0.4f, 0.001f));
        c += streakColor * streak * along * (exp(-abs(across.y) / 0.003f) + 0.25f * exp(-abs(across.y) / 0.02f));
    }

    // the ghosts, along the line from the light through the picture's centre
    [loop] for (int i = 0; i < 16; ++i)
    {
        if (i >= (int)ghosts)
            break;
        float along = 0.4f + i * ghostSpacing;
        float2 g = l + toCentre * along;
        float ghostRadius = max(ghostSize * k * (0.4f + 1.2f * hash(i + 3.0f)), 0.001f);
        float dist = shapeDistance(p - g, ghostShape) / ghostRadius;
        float filled = smoothstep(1.0f, 0.85f, dist) * (0.65f + 0.35f * smoothstep(0.5f, 1.0f, dist));
        // (Ghost Rings: only the rim, as a thin ring)
        float rim = exp(-pow((dist - 0.93f) / 0.06f, 2.0f));
        float disc = lerp(filled, rim * 1.3f, ghostRing);
        float3 tint = lerp(color, lerp(1.0f, hue(frac(i * 0.17f + 0.05f)), 0.65f), ghostRainbow);
        c += tint * ghostAmount * disc * (0.4f + 0.6f * hash(i + 11.0f));
    }

    // the halo: a ring, rainbow coloured across its width
    if (halo > 0.0f)
    {
        float2 h = l + toCentre * haloCenter;
        float offset = (length(p - h) - haloRadius * k) / (0.03f * k);
        float ring = exp(-offset * offset);
        float3 rainbow = hue(saturate(offset * 0.35f + 0.5f) * 0.8f);
        c += lerp(color, rainbow, haloRainbow) * halo * ring;
    }

    // the arcs: crescents of big rings beyond the picture's centre, the side away from the light lit
    [loop] for (int j = 0; j < 6; ++j)
    {
        if (j >= (int)arcs)
            break;
        float2 centre = l + toCentre * (1.05f + 0.22f * j);
        float radius = arcSize * k * (0.8f + 0.45f * hash(j + 31.0f));
        float2 fromCentre = p - centre;
        float offset = (length(fromCentre) - radius) / (0.012f * k);
        float side = dot(normalize(fromCentre + 0.0001f), normalize(toCentre + 0.0001f));
        float crescent = exp(-offset * offset) * smoothstep(0.1f, 0.8f, side);
        float3 tint = lerp(color, lerp(1.0f, hue(frac(j * 0.23f + 0.6f)), 0.5f), 0.5f);
        c += tint * arcAmount * crescent * (0.6f + 0.4f * hash(j + 41.0f));
    }

    return c;
}

)"
                                     R"(// how much a light shows by the flicker now: smooth noise of the film's time (each light its own)
float flickerNow(float seed)
{
    if (flicker <= 0.0f)
        return 1.0f;
    float t = time * flickerSpeed + seed * 17.31f;
    float i = floor(t);
    float f = t - i;
    float n = lerp(hash(i + seed * 3.7f), hash(i + 1.0f + seed * 3.7f), f * f * (3.0f - 2.0f * f));
    float fast = hash(floor(t * 3.0f) + seed * 5.1f);
    return 1.0f - flicker * saturate(0.7f * n + 0.3f * fast);
}

// The distance of what's at uv, in the game's units: from the depth buffer by its near plane (reversed or not, as
// the lens's probe found); far (a million) where nothing is drawn there, the sky
float lightDistance(float2 uv)
{
    float z = depthBuffer.SampleLevel(pointSampler, saturate(uv), 0);
    if (depthMode > 1.5f)
        return z <= 0.0f ? 1000000.0f : zNear / max(z, 0.0000001f);
    return z >= 1.0f ? 1000000.0f : zNear / max(1.0f - z, 0.0000001f);
}

// what a light's distance does to its flare: x how much shows (faded out beyond Max Distance), y how big it is
// (smaller farther, as much as Size by Distance says, 500 units the flare's own size)
float2 byDistance(float distance)
{
    float fade = maxDistance > 0.0f ? 1.0f - smoothstep(maxDistance * 0.7f, maxDistance, distance) : 1.0f;
    float size = lerp(1.0f, clamp(500.0f / max(distance, 1.0f), 0.25f, 2.0f), sizeByDistance);
    return float2(fade, size);
}

// how much a light at uv shows by where it is: faded as it leaves the picture
float onPicture(float2 uv)
{
    float2 edge = abs(uv - 0.5f);
    return saturate((0.75f - max(edge.x, edge.y)) / 0.25f);
}

float4 flare(VS_OUTPUT input) : SV_Target
{
    // Auto: a flare on each of the bright lights found (lights: their brightness over the threshold and where)
    if (autoLights > 0.5f)
    {
        float3 c = 0.0f;
        [loop] for (int i = 0; i < (int)autoLights; ++i)
        {
            float4 found = lights.Load(int3(i, 0, 0));
            if (found.x <= 0.0f)
                break;
            float2 depthEffect = byDistance(lightDistance(found.yz));
            c += flareAt(input.uv, found.yz, depthEffect.y) * found.x * onPicture(found.yz) * depthEffect.x *
                 flickerNow(i + 1.0f);
        }
        return float4(c * intensity, 0.0f);
    }

    float strength = intensity * onPicture(light);
    if (strength <= 0.0001f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    strength *= visibility();
    if (strength <= 0.0001f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 depthEffect = byDistance(lightDepth);
    return float4(flareAt(input.uv, light, depthEffect.y) * strength * depthEffect.x * flickerNow(0.0f), 0.0f);
}

// Auto's search, first: the picture in a grid of cells, each with how far over the threshold its brightest of 36
// taps is (0 to 1), and where its light is: the taps over the threshold, weighed by how much (steadier than the
// brightest tap, which jumps from tap to tap as the picture moves)
float4 detect(VS_OUTPUT input) : SV_Target
{
    float2 cell = floor(input.pos.xy);
    float brightest = 0.0f;
    float weight = 0.0f;
    float2 centre = 0.0f;
    [unroll] for (int y = 0; y < 6; ++y)
    {
        [unroll] for (int x = 0; x < 6; ++x)
        {
            float2 uv = (cell + (float2(x, y) + 0.5f) / 6.0f) / grid;
            float3 color = frame.SampleLevel(pointSampler, uv, 0).rgb;
            float bright = max(color.r, max(color.g, color.b)) * 0.7f + dot(color, float3(0.2126f, 0.7152f, 0.0722f)) * 0.3f;
            float over = saturate((bright - threshold) / max(1.0f - threshold, 0.02f));
            brightest = max(brightest, over);
            weight += over;
            centre += uv * over;
        }
    }
    return weight > 0.0f ? float4(brightest, centre / weight, weight) : 0.0f;
}

// A light's place, exactly: the brightness over the threshold in a box of 2 cells around where it was found, its
// weighted centre, twice (the second box on the first's centre), from the frame's own pixels; so a flare moves as its
// light does, not in the grid's steps
float2 refine(float2 uv)
{
    float2 box = 2.0f / grid;
    [loop] for (int iteration = 0; iteration < 2; ++iteration)
    {
        float weight = 0.0f;
        float2 centre = 0.0f;
        [loop] for (int y = 0; y < 12; ++y)
        {
            [loop] for (int x = 0; x < 12; ++x)
            {
                float2 at = uv + ((float2(x, y) + 0.5f) / 12.0f - 0.5f) * box;
                float3 color = frame.SampleLevel(linearSampler, at, 0).rgb;
                float bright =
                    max(color.r, max(color.g, color.b)) * 0.7f + dot(color, float3(0.2126f, 0.7152f, 0.0722f)) * 0.3f;
                float over = saturate((bright - threshold) / max(1.0f - threshold, 0.02f));
                weight += over * over;
                centre += at * over * over;
            }
        }
        if (weight <= 0.0f)
            break;
        uv = centre / weight;
    }
    return uv;
}

// then the lights: pixel k the k-th brightest cell, apart from the ones before it (so one light isn't found twice)
float4 pickLights(VS_OUTPUT input) : SV_Target
{
    int k = (int)input.pos.x;
    float2 found[8];
    float4 result = 0.0f;
    [loop] for (int j = 0; j <= k; ++j)
    {
        float4 best = 0.0f;
        int2 bestCell = 0;
        [loop] for (int y = 0; y < (int)grid.y; ++y)
        {
            [loop] for (int x = 0; x < (int)grid.x; ++x)
            {
                float4 cellValue = gridCells.Load(int3(x, y, 0));
                if (cellValue.x <= best.x)
                    continue;
                bool tooClose = false;
                [loop] for (int m = 0; m < j; ++m)
                    tooClose = tooClose || length((cellValue.yz - found[m]) * float2(aspect, 1.0f)) < separation;
                if (!tooClose)
                {
                    best = cellValue;
                    bestCell = int2(x, y);
                }
            }
        }
        if (best.x <= 0.0f)
        {
            result = 0.0f;
            break;
        }
        // where the light is: its cells and the ones around, by how much of each is over the threshold (a light
        // across two cells isn't pulled to one)
        float weight = 0.0f;
        float2 centre = 0.0f;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                int2 at = clamp(bestCell + int2(dx, dy), int2(0, 0), int2(grid) - 1);
                float4 around = gridCells.Load(int3(at, 0));
                weight += around.w;
                centre += around.yz * around.w;
            }
        }
        best.yz = weight > 0.0f ? centre / weight : best.yz;
        best.yz = refine(best.yz);
        found[j] = best.yz;
        result = float4(best.x, best.yz, 0.0f);
    }
    return result;
}

// Last, the lights followed from frame to frame (pixel k the k-th): each light of the last frame takes the nearest
// light found now, if one is near, and is where it is (its brightness eased a little, so it doesn't flicker); one
// with none fades out; a light found that none took fades in where a light has gone. Every pixel works all of it out
// alike and keeps its own.
// (the lights found, at t3 where the search's cells are while it searches)
float4 trackLights(VS_OUTPUT input) : SV_Target
{
    int k = (int)input.pos.x;
    int count = (int)autoLights;
    float4 tracked[8];
    bool taken[8];
    [loop] for (int i = 0; i < 8; ++i)
    {
        tracked[i] = i < count ? lights.Load(int3(i, 0, 0)) : 0.0f;
        taken[i] = false;
    }
    // the lights there were: each to its nearest light now
    [loop] for (int i = 0; i < count; ++i)
    {
        if (tracked[i].x <= 0.001f)
            continue;
        int match = -1;
        float nearest = matchRadius;
        [loop] for (int j = 0; j < count; ++j)
        {
            float4 candidate = gridCells.Load(int3(j, 0, 0));
            if (taken[j] || candidate.x <= 0.0f)
                continue;
            float distance = length((candidate.yz - tracked[i].yz) * float2(aspect, 1.0f));
            if (distance < nearest)
            {
                nearest = distance;
                match = j;
            }
        }
        if (match >= 0)
        {
            taken[match] = true;
            float4 candidate = gridCells.Load(int3(match, 0, 0));
            tracked[i].yz = candidate.yz;
            tracked[i].x = lerp(tracked[i].x, candidate.x, follow);
        }
        else
        {
            tracked[i].x = lerp(tracked[i].x, 0.0f, follow);
        }
    }
    // the lights now no light took: where a light has gone, from nothing
    [loop] for (int j = 0; j < count; ++j)
    {
        float4 candidate = gridCells.Load(int3(j, 0, 0));
        if (taken[j] || candidate.x <= 0.0f)
            continue;
        [loop] for (int i = 0; i < count; ++i)
        {
            if (tracked[i].x > 0.001f)
                continue;
            tracked[i] = float4(candidate.x * follow, candidate.yz, 0.0f);
            taken[j] = true;
            break;
        }
    }
    return k < count ? tracked[k] : 0.0f;
}
)";

        struct FlareConstants
        {
            float light[2];
            float aspect;
            float intensity;
            float color[3];
            float glow;
            float glowSize;
            float rays;
            float rayAmount;
            float rayLength;
            float rayRotation;
            float raySharpness;
            float streak;
            float streakLength;
            float streakColor[3];
            float ghosts;
            float ghostAmount;
            float ghostSize;
            float ghostSpacing;
            float ghostShape;
            float ghostRainbow;
            float halo;
            float haloRadius;
            float haloCenter;
            float haloRainbow;
            float hideBehind;
            float pixel[2];
            float autoLights;
            float threshold;
            float grid[2];
            float separation;
            float follow;
            float matchRadius;
            float unused;
            float lightDepth;
            float depthMode;
            float zNear;
            float maxDistance;
            float sizeByDistance;
            float rotation;
            float flicker;
            float flickerSpeed;
            float time;
            float rayRainbow;
            float ghostRing;
            float arcs;
            float arcAmount;
            float arcSize;
            float unused3[2];
        };
        static_assert(sizeof(FlareConstants) % 16 == 0);
        constexpr UINT FLARE_GRID_WIDTH = 64, FLARE_GRID_HEIGHT = 36, FLARE_MAX_LIGHTS = 8;

        bool MakeFlare()
        {
            if (std::exchange(flareTried, true))
                return flarePixelShader != nullptr;
            const auto compile = [](const char* entry, const char* target, ID3DBlob*& code) {
                ID3DBlob* errors = nullptr;
                const auto result = D3DCompile(FLARE_SHADER, sizeof(FLARE_SHADER) - 1, "LensFlare", nullptr, nullptr,
                                               entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
                if (FAILED(result))
                    LOG_ERROR("Couldn't compile the lens flare's {} ({})", entry,
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
                SafeRelease(errors);
                return SUCCEEDED(result);
            };
            ID3DBlob* vertexCode = nullptr;
            ID3DBlob* pixelCode = nullptr;
            bool made = compile("vs", "vs_4_0", vertexCode) && compile("flare", "ps_4_0", pixelCode);
            made = made && SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                                vertexCode->GetBufferSize(), nullptr,
                                                                &flareVertexShader));
            made = made && SUCCEEDED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                                                               nullptr, &flarePixelShader));
            SafeRelease(vertexCode);
            SafeRelease(pixelCode);
            D3D11_BUFFER_DESC constantsDesc{};
            constantsDesc.ByteWidth = sizeof(FlareConstants);
            constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            made = made && SUCCEEDED(device->CreateBuffer(&constantsDesc, nullptr, &flareConstants));
            D3D11_BLEND_DESC blendDesc{};
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask =
                D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
            made = made && SUCCEEDED(device->CreateBlendState(&blendDesc, &flareBlend));

            // (Auto's search: without it, Auto draws nothing and the others still work)
            ID3DBlob* detectCode = nullptr;
            ID3DBlob* pickCode = nullptr;
            ID3DBlob* trackCode = nullptr;
            if (made && compile("detect", "ps_4_0", detectCode) && compile("pickLights", "ps_4_0", pickCode) &&
                compile("trackLights", "ps_4_0", trackCode))
            {
                const auto target = [&](UINT width, UINT height, ID3D11Texture2D*& texture,
                                        ID3D11RenderTargetView*& view, ID3D11ShaderResourceView*& resource) {
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = width;
                    desc.Height = height;
                    desc.MipLevels = desc.ArraySize = 1;
                    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
                           SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &view)) &&
                           SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &resource));
                };
                if (FAILED(device->CreatePixelShader(detectCode->GetBufferPointer(), detectCode->GetBufferSize(),
                                                     nullptr, &flareDetectShader)) ||
                    FAILED(device->CreatePixelShader(pickCode->GetBufferPointer(), pickCode->GetBufferSize(), nullptr,
                                                     &flarePickShader)) ||
                    !target(FLARE_GRID_WIDTH, FLARE_GRID_HEIGHT, flareGridTexture, flareGridTarget, flareGridView) ||
                    !target(FLARE_MAX_LIGHTS, 1, flareLightsTexture, flareLightsTarget, flareLightsView) ||
                    FAILED(device->CreatePixelShader(trackCode->GetBufferPointer(), trackCode->GetBufferSize(), nullptr,
                                                     &flareTrackShader)) ||
                    !target(FLARE_MAX_LIGHTS, 1, flareTrackedTextures[0], flareTrackedTargets[0],
                            flareTrackedViews[0]) ||
                    !target(FLARE_MAX_LIGHTS, 1, flareTrackedTextures[1], flareTrackedTargets[1],
                            flareTrackedViews[1]))
                {
                    LOG_ERROR("Couldn't make the lens flare's search for lights");
                    SafeRelease(flareDetectShader);
                }
            }
            SafeRelease(detectCode);
            SafeRelease(pickCode);
            SafeRelease(trackCode);

            if (!made)
            {
                LOG_ERROR("Couldn't make the lens flare; it isn't drawn");
                ReleaseFlare();
                flareTried = true;
            }
            return made;
        }

        ID3D11ShaderResourceView* UsableDepth(ID3D11ShaderResourceView* const* where, const char* kind);

        // The lens flare added to the frame's back buffer, while it's on and its light is known
        void DrawLensFlare(IDXGISwapChain* swapChain)
        {
            if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare) ||
                Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            const auto frame = Components::LensFlare::GetFrame(Components::Playback::GetTimelineTick());
            if (!frame)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur() || !MakeFlare())
                return;
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(swapChain->GetDesc(&desc)))
                return;
            const float width = static_cast<float>(desc.BufferDesc.Width);
            const float height = static_cast<float>(std::max(desc.BufferDesc.Height, 1u));
            const auto& s = frame->settings;
            ID3D11ShaderResourceView* depthView = UsableDepth(frame->depth, "depth buffer");

            FlareConstants constants{};
            constants.light[0] = frame->light->x;
            constants.light[1] = frame->light->y;
            constants.aspect = width / height;
            constants.intensity = std::max(s.intensity, 0.0f);
            for (int i = 0; i < 3; ++i)
            {
                constants.color[i] = s.color[i];
                constants.streakColor[i] = s.streakColor[i];
            }
            constants.glow = s.glow;
            constants.glowSize = s.glowSize;
            constants.rays = static_cast<float>(std::clamp(s.rays, 0, 32));
            constants.rayAmount = s.rayAmount;
            constants.rayLength = s.rayLength;
            constants.rayRotation = s.rayRotation * 3.14159265f / 180.0f;
            constants.raySharpness = std::clamp(s.raySharpness, 0.0f, 1.0f);
            constants.streak = s.streak;
            constants.streakLength = s.streakLength;
            constants.ghosts = static_cast<float>(std::clamp(s.ghosts, 0, 16));
            constants.ghostAmount = s.ghostAmount;
            constants.ghostSize = s.ghostSize;
            constants.ghostSpacing = s.ghostSpacing;
            constants.ghostShape = s.ghostShape == Components::LensFlare::GhostShape::Hexagon   ? 6.0f
                                   : s.ghostShape == Components::LensFlare::GhostShape::Octagon ? 8.0f
                                                                                                : 0.0f;
            constants.ghostRainbow = s.ghostRainbow;
            constants.halo = s.halo;
            constants.haloRadius = s.haloRadius;
            constants.haloCenter = s.haloCenter;
            constants.haloRainbow = s.haloRainbow;
            constants.hideBehind = depthView && s.hideBehind ? 1.0f : 0.0f;
            constants.pixel[0] = 1.0f / width;
            constants.pixel[1] = 1.0f / height;
            const bool autoLights = s.source == Components::LensFlare::Source::Auto;
            if (autoLights && !flareDetectShader)
                return;
            constants.autoLights =
                autoLights ? static_cast<float>(std::clamp(s.autoLights, 1, static_cast<int>(FLARE_MAX_LIGHTS))) : 0.0f;
            constants.threshold = std::clamp(s.autoThreshold, 0.0f, 0.99f);
            constants.grid[0] = static_cast<float>(FLARE_GRID_WIDTH);
            constants.grid[1] = static_cast<float>(FLARE_GRID_HEIGHT);
            constants.separation = std::max(s.autoSeparation, 0.0f);
            // by distance: the one light's (the sun far), the depth buffer's for the lights found
            constants.lightDepth = frame->lightDepth;
            constants.depthMode = depthMode == 2 ? 2.0f : 1.0f;
            constants.zNear = std::max(Components::LensFlare::GetDepthBuffer().second, 0.001f);
            constants.maxDistance = std::max(s.maxDistance, 0.0f);
            constants.sizeByDistance = std::clamp(s.sizeByDistance, 0.0f, 1.0f);
            // the look's newer parts: the rotation (with the spin by the film's time), flicker, rainbow rays, rings
            // and arcs
            constants.time = static_cast<float>(Components::Playback::GetTimelineTick()) / 1000.0f;
            constants.rotation = (s.rotation + s.spin * constants.time) * 3.14159265f / 180.0f;
            constants.flicker = std::clamp(s.flicker, 0.0f, 1.0f);
            constants.flickerSpeed = std::max(s.flickerSpeed, 0.0f);
            constants.rayRainbow = std::clamp(s.rayRainbow, 0.0f, 1.0f);
            constants.ghostRing = std::clamp(s.ghostRing, 0.0f, 1.0f);
            constants.arcs = static_cast<float>(std::clamp(s.arcs, 0, 6));
            constants.arcAmount = s.arcAmount;
            constants.arcSize = s.arcSize;
            // the lights followed: their brightness eased by 1 - e^(-dt / 60 ms) for the film's time since the last frame
            // (the wall's while it stands still); at once after a jump or the first time
            if (autoLights)
            {
                const auto tick = Components::Playback::GetTimelineTick();
                const auto now = GetTickCount64();
                float seconds;
                bool jumped = !flareTrackedValid;
                if (tick != flareTrackedTick)
                {
                    const auto delta = static_cast<std::int64_t>(tick) - static_cast<std::int64_t>(flareTrackedTick);
                    jumped = jumped || delta < 0 || delta > 500;
                    seconds = static_cast<float>(delta) / 1000.0f;
                }
                else
                {
                    seconds = static_cast<float>(std::min<std::uint64_t>(now - flareTrackedTime, 100)) / 1000.0f;
                }
                flareTrackedTick = tick;
                flareTrackedTime = now;
                // (only a light's brightness is eased, over about 60 ms: where it is follows it at once)
                constexpr float FADE_SECONDS = 0.06f;
                constants.follow = jumped ? 1.0f : 1.0f - std::exp(-std::max(seconds, 0.0f) / FADE_SECONDS);
                constants.matchRadius = std::max(0.15f, s.autoSeparation);
            }
            else
            {
                flareTrackedValid = false;
            }
            // (Auto's search reads the frame as it is, before the flares)
            if (autoLights)
            {
                ID3D11Texture2D* backBuffer = nullptr;
                if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                    return;
                const bool copied = CopyFrameForBlur(backBuffer);
                backBuffer->Release();
                if (!copied)
                    return;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(flareConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(flareConstants, 0);

            ContextState state;
            state.Save();
            context->RSSetState(blurRasterizer);
            context->OMSetDepthStencilState(blurDepthState, 0);
            const float factor[4]{};
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(flareVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            ID3D11SamplerState* flareSamplers[2] = {pointSampler, blurSampler};
            context->PSSetSamplers(0, 2, flareSamplers);
            context->PSSetConstantBuffers(0, 1, &flareConstants);
            std::array<ID3D11ShaderResourceView*, 4> unbound{};

            // Auto: the bright cells, then the lights among them
            if (autoLights)
            {
                SetOpaqueBlending(nullptr, nullptr);
                context->OMSetBlendState(opaqueBlendState, factor, 0xFFFFFFFF);
                D3D11_VIEWPORT gridViewport{0.0f, 0.0f, static_cast<float>(FLARE_GRID_WIDTH),
                                            static_cast<float>(FLARE_GRID_HEIGHT), 0.0f, 1.0f};
                context->RSSetViewports(1, &gridViewport);
                context->OMSetRenderTargets(1, &flareGridTarget, nullptr);
                ID3D11ShaderResourceView* detectViews[3] = {nullptr, nullptr, blurFrameView};
                context->PSSetShaderResources(0, 3, detectViews);
                context->PSSetShader(flareDetectShader, nullptr, 0);
                context->Draw(3, 0);
                context->PSSetShaderResources(0, 4, unbound.data());

                D3D11_VIEWPORT lightsViewport{0.0f, 0.0f, constants.autoLights, 1.0f, 0.0f, 1.0f};
                context->RSSetViewports(1, &lightsViewport);
                context->OMSetRenderTargets(1, &flareLightsTarget, nullptr);
                ID3D11ShaderResourceView* pickViews[4] = {nullptr, nullptr, blurFrameView, flareGridView};
                context->PSSetShaderResources(0, 4, pickViews);
                context->PSSetShader(flarePickShader, nullptr, 0);
                context->Draw(3, 0);
                context->PSSetShaderResources(0, 4, unbound.data());

                // the lights followed: the last frame's (none the first time) eased towards those found
                const int next = 1 - flareTrackedCurrent;
                if (!flareTrackedValid)
                {
                    const float zero[4]{};
                    context->ClearRenderTargetView(flareTrackedTargets[flareTrackedCurrent], zero);
                }
                context->OMSetRenderTargets(1, &flareTrackedTargets[next], nullptr);
                ID3D11ShaderResourceView* trackViews[4] = {nullptr, flareTrackedViews[flareTrackedCurrent], nullptr,
                                                           flareLightsView};
                context->PSSetShaderResources(0, 4, trackViews);
                context->PSSetShader(flareTrackShader, nullptr, 0);
                context->Draw(3, 0);
                context->PSSetShaderResources(0, 4, unbound.data());
                flareTrackedCurrent = next;
                flareTrackedValid = true;
            }

            D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->OMSetBlendState(flareBlend, factor, 0xFFFFFFFF);
            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(flareVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(flarePixelShader, nullptr, 0);
            ID3D11ShaderResourceView* flareViews[2] = {depthView,
                                                       autoLights ? flareTrackedViews[flareTrackedCurrent] : nullptr};
            context->PSSetShaderResources(0, 2, flareViews);
            context->Draw(3, 0);
            context->PSSetShaderResources(0, 4, unbound.data());
            state.Restore();
        }

        constexpr char SHUTTER_SHADER[] = R"(
cbuffer Shutter : register(b0)
{
    float4 weights[4];  // out: the ring's frames' weights, the newest first (summing to 1)
};
Texture2D frames[16] : register(t0);
SamplerState pointSampler : register(s0);
struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUTPUT vs(uint id : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
// the frame in linear light, to blend in
float4 shutterIn(VS_OUTPUT input) : SV_Target
{
    return float4(pow(max(frames[0].SampleLevel(pointSampler, input.uv, 0).rgb, 0.0f), 2.2f), 1.0f);
}
// a copy (the accumulation into the ring)
float4 shutterCopy(VS_OUTPUT input) : SV_Target
{
    return frames[0].SampleLevel(pointSampler, input.uv, 0);
}
// the weighted frames, back from linear light
float4 shutterOut(VS_OUTPUT input) : SV_Target
{
    float3 color = 0.0f;
    [unroll] for (int i = 0; i < 16; ++i)
    {
        float weight = weights[i / 4][i % 4];
        if (weight > 0.0f)
            color += frames[i].SampleLevel(pointSampler, input.uv, 0).rgb * weight;
    }
    return float4(pow(max(color, 0.0f), 1.0f / 2.2f), 1.0f);
}
)";

        bool MakeShutter()
        {
            if (std::exchange(shutterTried, true))
                return shutterOutShader != nullptr;
            const auto compile = [](const char* entry, const char* target, ID3DBlob*& code) {
                ID3DBlob* errors = nullptr;
                const auto result = D3DCompile(SHUTTER_SHADER, sizeof(SHUTTER_SHADER) - 1, "Shutter", nullptr, nullptr,
                                               entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
                if (FAILED(result))
                    LOG_ERROR("Couldn't compile the shutter's {} ({})", entry,
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
                SafeRelease(errors);
                return SUCCEEDED(result);
            };
            ID3DBlob* vertexCode = nullptr;
            ID3DBlob* inCode = nullptr;
            ID3DBlob* copyCode = nullptr;
            ID3DBlob* outCode = nullptr;
            bool made = compile("vs", "vs_4_0", vertexCode) && compile("shutterIn", "ps_4_0", inCode) &&
                        compile("shutterCopy", "ps_4_0", copyCode) && compile("shutterOut", "ps_4_0", outCode);
            made = made && SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                                vertexCode->GetBufferSize(), nullptr,
                                                                &shutterVertexShader));
            made = made && SUCCEEDED(device->CreatePixelShader(inCode->GetBufferPointer(), inCode->GetBufferSize(),
                                                               nullptr, &shutterInShader));
            made = made && SUCCEEDED(device->CreatePixelShader(copyCode->GetBufferPointer(), copyCode->GetBufferSize(),
                                                               nullptr, &shutterCopyShader));
            made = made && SUCCEEDED(device->CreatePixelShader(outCode->GetBufferPointer(), outCode->GetBufferSize(),
                                                               nullptr, &shutterOutShader));
            SafeRelease(vertexCode);
            SafeRelease(inCode);
            SafeRelease(copyCode);
            SafeRelease(outCode);

            D3D11_BUFFER_DESC constantsDesc{};
            constantsDesc.ByteWidth = sizeof(float) * 16;
            constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
            constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            made = made && SUCCEEDED(device->CreateBuffer(&constantsDesc, nullptr, &shutterConstants));

            D3D11_BLEND_DESC blendDesc{};
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            made = made && SUCCEEDED(device->CreateBlendState(&blendDesc, &shutterBlend));

            if (!made)
            {
                LOG_ERROR("Couldn't make the shutter; motion blur isn't drawn");
                ReleaseShutter();
                shutterTried = true;
            }
            return made;
        }

        bool MakeShutterTarget(ShutterTarget& target, DXGI_FORMAT format)
        {
            if (target.view)
                return true;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = shutterWidth;
            desc.Height = shutterHeight;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.texture)) ||
                FAILED(device->CreateRenderTargetView(target.texture, nullptr, &target.target)) ||
                FAILED(device->CreateShaderResourceView(target.texture, nullptr, &target.view)))
            {
                ReleaseShutterTarget(target);
                LOG_ERROR("Couldn't make a {}x{} texture for the shutter", shutterWidth, shutterHeight);
                return false;
            }
            return true;
        }

        // The shutter drawn into the frame's back buffer: while a capture renders its frames, each is blended into the
        // accumulation, and the accumulation (with the frames before it, for a shutter longer than a frame) is what
        // the back buffer shows; in the editor, while the film plays, each frame blended in by how much of the
        // shutter's time it moved on
        void DrawShutter(IDXGISwapChain* swapChain)
        {
            auto& captureManager = Components::CaptureManager::Get();
            const auto& shutter = captureManager.GetCaptureSettings().shutter;
            const auto role = captureManager.GetShutterRole();
            const bool preview = !captureManager.IsCapturing() && shutter.enabled && shutter.preview &&
                                 (Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_Shutter);
            if (!role && !preview)
            {
                shutterPreviewValid = false;
                return;
            }
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            if (!MakeScreenBlur() || !MakeShutter())
                return;

            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
                return;
            const bool copied = CopyFrameForBlur(backBuffer);
            backBuffer->Release();
            if (!copied)
                return;
            if (shutterWidth != blurFrameDesc.Width || shutterHeight != blurFrameDesc.Height)
            {
                ReleaseShutterTargets();
                shutterWidth = blurFrameDesc.Width;
                shutterHeight = blurFrameDesc.Height;
            }
            if (!MakeShutterTarget(shutterAccumulation, DXGI_FORMAT_R16G16B16A16_FLOAT))
                return;

            // how much of this frame goes into the accumulation, and the frames blended for the picture
            float blend = 1.0f;
            std::array<float, 16> weights{};
            weights[0] = 1.0f;
            std::array<ID3D11ShaderResourceView*, 16> sources{};
            sources[0] = shutterAccumulation.view;
            bool toRing = false;
            if (role)
            {
                // (a capture's first frame starts the ring again)
                if (role->recordedFrame == 0 && role->subframe == 0)
                    shutterRingNext = shutterRingCount = 0;
                blend = 1.0f / static_cast<float>(role->subframe + 1);
                toRing = role->last && role->exposureFrames > 1.0f;
            }
            else
            {
                // the film's time since the last frame, as a part of the shutter's (none while it stands still or
                // jumps: the frame as it is)
                const auto tick = Components::Playback::GetTimelineTick();
                const auto delta = static_cast<std::int64_t>(tick) - static_cast<std::int64_t>(shutterPreviewTick);
                const float exposure = std::max(shutter.seconds * 1000.0f, 1.0f);
                blend = !shutterPreviewValid || delta <= 0 || delta > 500
                            ? 1.0f
                            : std::clamp(static_cast<float>(delta) / exposure, 0.05f, 1.0f);
                shutterPreviewTick = tick;
                shutterPreviewValid = true;
            }

            ContextState state;
            state.Save();
            D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(shutterWidth), static_cast<float>(shutterHeight),
                                    0.0f, 1.0f};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(blurRasterizer);
            context->OMSetDepthStencilState(blurDepthState, 0);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(shutterVertexShader, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->PSSetSamplers(0, 1, &pointSampler);
            std::array<ID3D11ShaderResourceView*, 16> none{};

            // the frame blended into the accumulation
            const float factor[4] = {blend, blend, blend, blend};
            context->OMSetBlendState(shutterBlend, factor, 0xFFFFFFFF);
            context->OMSetRenderTargets(1, &shutterAccumulation.target, nullptr);
            context->PSSetShaderResources(0, 1, &blurFrameView);
            context->PSSetShader(shutterInShader, nullptr, 0);
            context->Draw(3, 0);
            context->PSSetShaderResources(0, 16, none.data());

            const float opaque[4]{};
            SetOpaqueBlending(nullptr, nullptr);
            // a recorded frame's average kept in the ring, and the picture blended from the last ones: the newest
            // whole, those before as far as the shutter reaches back, the oldest by the part of it that's left
            if (toRing && MakeShutterTarget(shutterRing[static_cast<std::size_t>(shutterRingNext)],
                                            DXGI_FORMAT_R11G11B10_FLOAT))
            {
                auto& slot = shutterRing[static_cast<std::size_t>(shutterRingNext)];
                context->OMSetBlendState(opaqueBlendState, opaque, 0xFFFFFFFF);
                context->OMSetRenderTargets(1, &slot.target, nullptr);
                context->PSSetShaderResources(0, 1, &shutterAccumulation.view);
                context->PSSetShader(shutterCopyShader, nullptr, 0);
                context->Draw(3, 0);
                context->PSSetShaderResources(0, 16, none.data());
                shutterRingNext = (shutterRingNext + 1) % Components::MAX_SHUTTER_FRAMES;
                shutterRingCount = std::min(shutterRingCount + 1, Components::MAX_SHUTTER_FRAMES);

                const float reach = std::min(role->exposureFrames, static_cast<float>(shutterRingCount));
                float total = 0.0f;
                weights.fill(0.0f);
                for (std::int32_t i = 0; i < shutterRingCount && static_cast<float>(i) < reach; ++i)
                {
                    const auto index = (shutterRingNext - 1 - i + Components::MAX_SHUTTER_FRAMES) %
                                       Components::MAX_SHUTTER_FRAMES;
                    sources[static_cast<std::size_t>(i)] = shutterRing[static_cast<std::size_t>(index)].view;
                    weights[static_cast<std::size_t>(i)] = std::min(reach - static_cast<float>(i), 1.0f);
                    total += weights[static_cast<std::size_t>(i)];
                }
                for (auto& weight : weights)
                    weight /= std::max(total, 0.0001f);
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(shutterConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                std::memcpy(mapped.pData, weights.data(), sizeof(float) * weights.size());
                context->Unmap(shutterConstants, 0);
                context->OMSetBlendState(opaqueBlendState, opaque, 0xFFFFFFFF);
                context->OMSetRenderTargets(1, &backBufferView, nullptr);
                context->PSSetConstantBuffers(0, 1, &shutterConstants);
                context->PSSetShaderResources(0, 16, sources.data());
                context->PSSetShader(shutterOutShader, nullptr, 0);
                context->Draw(3, 0);
                context->PSSetShaderResources(0, 16, none.data());
            }
            state.Restore();
        }

        // The frame's work, in turn: the UI made the first time, the game told of the frame, the frame captured (before
        // the UI is drawn over it, as D3D9's EndScene does: the frame prepared last time is the one in the back buffer
        // now), and the UI drawn
        void BeginFrame(IDXGISwapChain* swapChain)
        {
            frameSwapChain = swapChain;
            if (!UI::UIManager::Get().IsInitialized())
                UI::UIManager::Get().Initialize(nullptr, gameWindow.load());
            capturedThisFrame = false;
            try
            {
                if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
                    ProbeRequestedDepth();
                DrawSky(swapChain);
                // (the flare first when the depth of field blurs it too)
                const bool flareBlurred = Components::LensFlare::GetSettings().blurredByDepthOfField;
                if (flareBlurred)
                    DrawLensFlare(swapChain);
                DrawScreenBlur(swapChain);
                if (!flareBlurred)
                    DrawLensFlare(swapChain);
                DrawScreenBlur(swapChain, true);
                DrawLensDirt(swapChain);
                DrawLens(swapChain);
                DrawShutter(swapChain);
                DrawStreamPass(swapChain);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Drawing the screen blur or the shutter failed: {}", e.what());
            }
        }

        void CaptureThisFrame()
        {
            if (std::exchange(capturedThisFrame, true))
                return;
            auto& capture = Components::CaptureManager::Get();
            if (capture.IsCapturing())
            {
                if (capture.IsFramePrepared())
                    capture.CaptureFrame();
                capture.PrepareFrame();
            }
        }

        void DrawUI(IDXGISwapChain* swapChain)
        {
            if (!backBufferView && !CreateBackBufferView(swapChain))
                return;
            // the game's targets back as they were after the UI, which ImGui's backend doesn't do itself
            ID3D11RenderTargetView* gameTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* gameDepth = nullptr;
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gameTargets, &gameDepth);

            context->OMSetRenderTargets(1, &backBufferView, nullptr);
            UI::UIManager::Get().RunImGuiFrame();

            context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gameTargets, gameDepth);
            for (auto& target : gameTargets)
                SafeRelease(target);
            SafeRelease(gameDepth);
        }

        // With ReShade the frame's work goes in its present: the frame taken before its effects; captured after them
        // (or after its overlay when they didn't draw: off or loading), so a recording has them; the UI after its
        // overlay, over everything and without them
        void OnReShadeStage(ReShade::PresentStage stage, void* swapChain)
        {
            if (unloadRequested)
                return;
            try
            {
                switch (stage)
                {
                    case ReShade::PresentStage::BeforeEffects:
                    {
                        const auto chain = static_cast<IDXGISwapChain*>(swapChain);
                        if (!chain || !Adopt(chain))
                        {
                            frameSwapChain = nullptr;
                            return;
                        }
                        reshadeFrameAt = GetTickCount64();
                        BeginFrame(chain);
                        break;
                    }
                    case ReShade::PresentStage::AfterEffects:
                        if (frameSwapChain)
                            CaptureThisFrame();
                        break;
                    case ReShade::PresentStage::AfterOverlay:
                        if (!frameSwapChain)
                            return;
                        CaptureThisFrame();
                        DrawUI(frameSwapChain);
                        frameSwapChain = nullptr;
                        break;
                }
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Drawing the UI over the D3D11 frame in ReShade's present failed: {}", e.what());
                frameSwapChain = nullptr;
            }
        }

        HRESULT __stdcall Present_Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
        {
            if (ReleaseIfUnloading() || (flags & DXGI_PRESENT_TEST))
                return Present(swapChain, syncInterval, flags);
            // With ReShade its present does the frame's work (OnReShadeStage), always: the swap chain this sees is
            // ReShade's, with ReShade's device, and taking turns with the game's own in ReShade's events made the UI
            // start again on the other device each time (and the game froze). Only when its events never come does
            // this do it.
            CheckForReShade();
            if (reshadePresent)
            {
                if (reshadeFrameAt.load() || GetTickCount64() - reshadeFoundAt < RESHADE_EVENTS_WAIT_MS)
                    return Present(swapChain, syncInterval, flags);
                if (!std::exchange(reshadeEventsMissed, true))
                    LOG_WARN("ReShade's present events didn't come: the UI is drawn in Present, under its effects");
            }
            if (!Adopt(swapChain))
                return Present(swapChain, syncInterval, flags);

            try
            {
                BeginFrame(swapChain);
                CaptureThisFrame();
                DrawUI(swapChain);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Drawing the UI over the D3D11 frame failed: {}", e.what());
            }
            frameSwapChain = nullptr;

            return Present(swapChain, syncInterval, flags);
        }

        // The back buffers can't be resized while anything holds them
        HRESULT __stdcall ResizeBuffers_Hook(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                             DXGI_FORMAT format, UINT flags)
        {
            // (whichever it's called on: with ReShade the game's is ReShade's, and this may be either)
            ReleaseBackBufferView();
            return ResizeBuffers(swapChain, bufferCount, width, height, format, flags);
        }

        void SetOpaqueBlending(const ImDrawList*, const ImDrawCmd*)
        {
            if (!opaqueBlendState)
            {
                D3D11_BLEND_DESC desc{};
                desc.RenderTarget[0].BlendEnable = FALSE;
                desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                if (FAILED(device->CreateBlendState(&desc, &opaqueBlendState)))
                    return;
            }
            context->OMSetBlendState(opaqueBlendState, nullptr, 0xFFFFFFFF);
        }

        // Catmull-Rom in five bilinear taps (the corners' weights are left out), as ImGui's vertex shader passes it
        // the position, colour and uv; opaque
        constexpr char GAME_VIEW_SHADER[] = R"(
Texture2D picture : register(t0);
SamplerState linearSampler : register(s0);
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
float4 main(PS_INPUT input) : SV_Target
{
    float width, height;
    picture.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 position = input.uv * size;
    float2 texel1 = floor(position - 0.5f) + 0.5f;
    float2 f = position - texel1;
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);
    float2 w12 = w1 + w2;
    float2 uv0 = (texel1 - 1.0f) / size;
    float2 uv3 = (texel1 + 2.0f) / size;
    float2 uv12 = (texel1 + w2 / w12) / size;
    float3 color = picture.SampleLevel(linearSampler, float2(uv0.x, uv12.y), 0).rgb * (w0.x * w12.y)
                 + picture.SampleLevel(linearSampler, float2(uv12.x, uv0.y), 0).rgb * (w12.x * w0.y)
                 + picture.SampleLevel(linearSampler, uv12, 0).rgb * (w12.x * w12.y)
                 + picture.SampleLevel(linearSampler, float2(uv3.x, uv12.y), 0).rgb * (w3.x * w12.y)
                 + picture.SampleLevel(linearSampler, float2(uv12.x, uv3.y), 0).rgb * (w12.x * w3.y);
    float weight = w0.x * w12.y + w12.x * w0.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return float4(saturate(color / weight), 1.0f) * float4(input.col.rgb, 1.0f);
}
)";

        void SetGameViewDrawing(const ImDrawList* drawList, const ImDrawCmd* command)
        {
            SetOpaqueBlending(drawList, command);
            if (!gameViewShaderTried)
            {
                gameViewShaderTried = true;
                ID3DBlob* code = nullptr;
                ID3DBlob* errors = nullptr;
                if (SUCCEEDED(D3DCompile(GAME_VIEW_SHADER, sizeof(GAME_VIEW_SHADER) - 1, "GameView", nullptr, nullptr,
                                         "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
                {
                    if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                         &gameViewShader)))
                        LOG_ERROR("Couldn't make the game view's pixel shader; it's drawn bilinear");
                }
                else
                {
                    LOG_ERROR("Couldn't compile the game view's pixel shader ({}); it's drawn bilinear",
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
                }
                SafeRelease(code);
                SafeRelease(errors);
            }
            if (gameViewShader)
                context->PSSetShader(gameViewShader, nullptr, 0);
        }

        // A swap chain of our own for a moment, for the addresses of IDXGISwapChain's functions in dxgi.dll
        bool FindSwapChainFunctions(void*& present, void*& resizeBuffers)
        {
            WNDCLASSEXA windowClass{sizeof(windowClass)};
            windowClass.lpfnWndProc = DefWindowProcA;
            windowClass.hInstance = GetModuleHandleA(nullptr);
            windowClass.lpszClassName = "IrisD3D11Probe";
            RegisterClassExA(&windowClass);
            const HWND window = CreateWindowExA(0, windowClass.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                                nullptr, nullptr, windowClass.hInstance, nullptr);
            if (!window)
                throw std::runtime_error("Couldn't make a window for the D3D11 probe");

            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferCount = 1;
            desc.BufferDesc.Width = 64;
            desc.BufferDesc.Height = 64;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.OutputWindow = window;
            desc.SampleDesc.Count = 1;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            IDXGISwapChain* swapChain = nullptr;
            ID3D11Device* probeDevice = nullptr;
            ID3D11DeviceContext* probeContext = nullptr;
            HRESULT result = E_FAIL;
            for (const auto driverType : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
            {
                result = D3D11CreateDeviceAndSwapChain(nullptr, driverType, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                                       &desc, &swapChain, &probeDevice, nullptr, &probeContext);
                if (SUCCEEDED(result))
                    break;
            }

            if (SUCCEEDED(result))
            {
                const auto vtable = *reinterpret_cast<void***>(swapChain);
                present = vtable[PRESENT_INDEX];
                resizeBuffers = vtable[RESIZE_BUFFERS_INDEX];
            }
            SafeRelease(swapChain);
            SafeRelease(probeContext);
            SafeRelease(probeDevice);
            DestroyWindow(window);
            UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);

            if (FAILED(result))
                LOG_ERROR("Couldn't make a D3D11 swap chain to find Present ({:#x})", static_cast<std::uint32_t>(result));
            return SUCCEEDED(result);
        }
    }  // namespace

    void Initialize()
    {
        void* present = nullptr;
        void* resizeBuffers = nullptr;
        if (!FindSwapChainFunctions(present, resizeBuffers))
            throw std::runtime_error("Failed to find the D3D11 swap chain's functions");

        // (D3D9::Hook initializes MinHook in a D3D9 game)
        if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("Failed to initialize MinHook");

        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(present), reinterpret_cast<std::uintptr_t>(Present_Hook),
                                reinterpret_cast<std::uintptr_t*>(&Present));
        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(resizeBuffers),
                                reinterpret_cast<std::uintptr_t>(ResizeBuffers_Hook),
                                reinterpret_cast<std::uintptr_t*>(&ResizeBuffers));
        LOG_DEBUG("Hooked D3D11 (Present at {}, ResizeBuffers at {})", present, resizeBuffers);
    }

    HWND FindWindowHandle()
    {
        return gameWindow.load();
    }

    ID3D11Device* GetDevice()
    {
        return device;
    }

    void SetScreenBlur(const ScreenBlur& blur)
    {
        std::lock_guard lock(screenBlurMutex);
        // (the depth read at the old pin isn't the new one's)
        if (blur.pointX != screenBlur.pointX || blur.pointY != screenBlur.pointY)
            centerDepthAt = 0;
        screenBlur = blur;
    }

    void SetPictureBlur(const ScreenBlur& blur)
    {
        std::lock_guard lock(screenBlurMutex);
        pictureBlur = blur;
    }

    std::optional<float> GetPointDepth()
    {
        glm::vec2 pin;
        bool tracking = false;
        {
            std::lock_guard lock(screenBlurMutex);
            if (screenBlur.type != ScreenBlur::LENS)
                return std::nullopt;
            pin = glm::vec2(screenBlur.pointX, screenBlur.pointY);
            tracking = screenBlur.trackFocus;
        }
        const auto at = centerDepthAt.load();
        if (!at || GetTickCount64() - at > 1000)
            return std::nullopt;
        // (a reading of another spot, one asked for, isn't the pin's)
        {
            std::lock_guard lock(probeMutex);
            if (!tracking && glm::distance(probedAt, pin) > 0.002f)
                return std::nullopt;
        }
        return centerDepth.load();
    }

    std::optional<glm::vec2> GetTrackedFocus()
    {
        std::lock_guard lock(probeMutex);
        return trackedFocus;
    }

    void RequestDepthAt(glm::vec2 uv)
    {
        std::lock_guard lock(probeMutex);
        if (depthRequest && glm::distance(*depthRequest, uv) < 0.0001f)
            return;
        depthRequest = uv;
        probedAt = glm::vec2(-1.0f);
    }

    std::optional<float> GetDepthAt(glm::vec2 uv)
    {
        std::lock_guard lock(probeMutex);
        const auto at = centerDepthAt.load();
        if (!at || GetTickCount64() - at > 1000 || glm::distance(probedAt, uv) > 0.0001f)
            return std::nullopt;
        // (got: no longer asked for)
        if (depthRequest && glm::distance(*depthRequest, uv) < 0.0001f)
            depthRequest.reset();
        return centerDepth.load();
    }

    ID3D11DeviceContext* GetContext()
    {
        return context;
    }

    std::optional<ImVec2> GetBackBufferSize()
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (!frameSwapChain || FAILED(frameSwapChain->GetDesc(&desc)))
            return std::nullopt;
        return ImVec2(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
    }

    bool CaptureBackBuffer(GameViewTexture& target)
    {
        if (!frameSwapChain || !device)
            return false;

        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(frameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
            return false;

        D3D11_TEXTURE2D_DESC backBufferDesc{};
        backBuffer->GetDesc(&backBufferDesc);

        if (!target.texture || target.width != backBufferDesc.Width || target.height != backBufferDesc.Height ||
            target.format != backBufferDesc.Format)
        {
            Release(target);

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = backBufferDesc.Width;
            desc.Height = backBufferDesc.Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = backBufferDesc.Format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = ViewFormat(backBufferDesc.Format);
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;

            if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.texture)) ||
                FAILED(device->CreateShaderResourceView(target.texture, &viewDesc, &target.view)))
            {
                LOG_ERROR("Couldn't make the game view's texture ({}x{}, format {})", desc.Width, desc.Height,
                          static_cast<int>(desc.Format));
                Release(target);
                backBuffer->Release();
                return false;
            }
            target.width = desc.Width;
            target.height = desc.Height;
            target.format = desc.Format;
        }

        if (backBufferDesc.SampleDesc.Count > 1)
            context->ResolveSubresource(target.texture, 0, backBuffer, 0, ViewFormat(backBufferDesc.Format));
        else
            context->CopyResource(target.texture, backBuffer);

        backBuffer->Release();
        return true;
    }

    void Release(GameViewTexture& target)
    {
        SafeRelease(target.view);
        SafeRelease(target.texture);
        target.width = target.height = 0;
        target.format = DXGI_FORMAT_UNKNOWN;
    }

    void DrawOpaqueImage(ImTextureID texture, ImVec2 size)
    {
        auto drawList = ImGui::GetWindowDrawList();
        drawList->AddCallback(SetGameViewDrawing, nullptr);
        ImGui::Image(texture, size);
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    bool CreateTextureFromMemory(const std::uint8_t* data, std::uint32_t size, ImTextureID& texture,
                                 ImVec2& dimensions)
    {
        if (!device)
            return false;

        ID3D11ShaderResourceView* view = nullptr;
        if (FAILED(D3DX11CreateShaderResourceViewFromMemory(device, data, size, nullptr, nullptr, &view, nullptr)))
            return false;

        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D* texture2D = nullptr;
        if (resource &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture2D))))
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture2D->GetDesc(&desc);
            dimensions = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
            texture2D->Release();
        }
        SafeRelease(resource);

        texture = view;
        return true;
    }

    void ReleaseTexture(ImTextureID texture)
    {
        if (texture)
            static_cast<ID3D11ShaderResourceView*>(texture)->Release();
    }

    bool ReadFrame(const std::function<void(const std::uint8_t* pixels, std::uint32_t rowPitch, std::uint32_t width,
                                            std::uint32_t height)>& use)
    {
        if (!frameSwapChain || !device || !context)
            return false;

        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(frameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);

        const auto format = ViewFormat(desc.Format);
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
            format != DXGI_FORMAT_B8G8R8X8_UNORM)
        {
            LOG_ERROR("Can't capture a back buffer of format {}", static_cast<int>(desc.Format));
            backBuffer->Release();
            return false;
        }

        if (!readStaging || readDesc.Width != desc.Width || readDesc.Height != desc.Height ||
            readDesc.Format != desc.Format || readDesc.SampleDesc.Count != desc.SampleDesc.Count)
        {
            ReleaseFrameReading();
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width = desc.Width;
            staging.Height = desc.Height;
            staging.MipLevels = 1;
            staging.ArraySize = 1;
            staging.Format = desc.Format;
            staging.SampleDesc.Count = 1;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&staging, nullptr, &readStaging)))
            {
                LOG_ERROR("Couldn't make the capture's staging texture ({}x{})", desc.Width, desc.Height);
                backBuffer->Release();
                return false;
            }
            if (desc.SampleDesc.Count > 1)
            {
                D3D11_TEXTURE2D_DESC resolved = staging;
                resolved.Usage = D3D11_USAGE_DEFAULT;
                resolved.CPUAccessFlags = 0;
                if (FAILED(device->CreateTexture2D(&resolved, nullptr, &readResolved)))
                {
                    LOG_ERROR("Couldn't make the capture's resolve texture");
                    ReleaseFrameReading();
                    backBuffer->Release();
                    return false;
                }
            }
            readDesc = desc;
        }

        if (readResolved)
        {
            context->ResolveSubresource(readResolved, 0, backBuffer, 0, format);
            context->CopyResource(readStaging, readResolved);
        }
        else
        {
            context->CopyResource(readStaging, backBuffer);
        }
        backBuffer->Release();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(readStaging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            LOG_ERROR("Couldn't read the frame back");
            return false;
        }
        use(static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch, desc.Width, desc.Height);
        context->Unmap(readStaging, 0);
        return true;
    }

    void ReleaseFrameReading()
    {
        SafeRelease(readStaging);
        SafeRelease(readResolved);
        readDesc = {};
    }

    std::string_view GetFramePixelFormat()
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        const auto swapChain = frameSwapChain ? frameSwapChain : gameSwapChain;
        if (!swapChain || FAILED(swapChain->GetDesc(&desc)))
            return "bgra";
        const auto format = ViewFormat(desc.BufferDesc.Format);
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? "rgba" : "bgra";
    }

    bool ReleaseForUnload(std::chrono::milliseconds timeout)
    {
        unloadRequested = true;
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (!releasedForUnload && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return releasedForUnload;
    }
}  // namespace IWXMVM::D3D11
