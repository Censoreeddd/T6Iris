#pragma once
#include "Camera.hpp"
#include "CaptureWatch.hpp"
#include "RestartLogic.hpp"
#include "Types/RenderingFlags.hpp"

namespace IWXMVM::Components
{
    enum class OutputFormat
    {
        Video,
        CameraData,
        ImageSequence,

        Count
    };

    enum class PassType
    {
        Default,
        Depth,
        Normal,
        Count
    };

    enum class VisibleElements
    {
        Everything,
        WorldAndPlayers,
        OnlyGun,
        OnlyWorld,
        OnlyPlayers,
        Count
    };

    struct PassData
    {
        PassType type;
        VisibleElements elements;
        FILE* pipe = nullptr;
        bool useReshade = true;
        // a Green screen's: what stays over the green (the demo's players, the scene's actors, its props)
        bool keepPlayers = true;
        bool keepActors = true;
        bool keepProps = false;
        bool keepGuns = true;    // the guns the players and actors hold (and the first person gun)
        bool keepWorld = false;  // the map itself, behind them (the green then only where the sky is)
    };

    // The streams a multi-pass capture offers, each a pass's kind and what it draws, by name
    struct PassPreset
    {
        const char* name;
        const char* description;
        PassType type;
        VisibleElements elements;
    };
    inline constexpr PassPreset PASS_PRESETS[] = {
        {"Full frame", "Everything as the game draws it", PassType::Default, VisibleElements::Everything},
        {"Green screen", "Players and props on a green background, for keying", PassType::Default,
         VisibleElements::OnlyPlayers},
        {"World only", "The map without players or props", PassType::Default, VisibleElements::OnlyWorld},
        {"No gun", "Everything but the first person gun", PassType::Default, VisibleElements::WorldAndPlayers},
        {"Gun only", "The first person gun alone", PassType::Default, VisibleElements::OnlyGun},
        {"Depth", "White near, fading to black far away: for fog, focus and depth effects", PassType::Depth,
         VisibleElements::Everything},
        {"Normals", "Which way surfaces face, as colours: for relighting", PassType::Normal,
         VisibleElements::Everything},
    };
    // a pass's name: its stream's, or its kind's and what it draws for one that's none
    inline std::string GetPassName(const PassData& pass)
    {
        for (const auto& preset : PASS_PRESETS)
            if (preset.type == pass.type && preset.elements == pass.elements)
                return preset.name;
        const char* kind = pass.type == PassType::Depth ? "Depth" : pass.type == PassType::Normal ? "Normals" : "Frame";
        for (const auto& preset : PASS_PRESETS)
            if (preset.type == PassType::Default && preset.elements == pass.elements)
                return std::string(kind) + " (" + preset.name + ")";
        return kind;
    }

    struct Resolution
    {
        int32_t width, height;

        bool operator==(const Resolution& other) const
        {
            return width == other.width && height == other.height;
        }

        std::string ToString() const
        {
            return std::format("{0}x{1}", width, height);
        }
    };

    enum class VideoCodec
    {
        Prores4444XQ,
        Prores4444,
        Prores422HQ,
        Prores422,
        Prores422LT,

        Count
    };

    // A camera's shutter (Features_Shutter): each frame recorded is the average of samples frames the game renders
    // across the time the shutter is open, so what moves smears as in a photo; longer than a frame, frames before it
    // are blended in too (up to MAX_SHUTTER_FRAMES). preview: an estimate while the film plays in the editor.
    struct ShutterSettings
    {
        bool enabled = false;
        float seconds = 1.0f / 60.0f;
        std::int32_t samples = 16;
        bool preview = true;
    };
    constexpr std::int32_t MAX_SHUTTER_FRAMES = 16;

    struct CaptureSettings
    {
        uint32_t startTick, endTick;
        
        OutputFormat outputFormat;
        std::optional<VideoCodec> videoCodec;

        Resolution resolution;
        int32_t framerate;

        std::vector<PassData> passes;
        // a Depth pass's distance (the game's units) at which it's black, where a D3D11 game draws it (D3D11.cpp)
        float depthDistance = 4000.0f;

        // the size the game renders at while capturing, when it isn't the window's; see Features_RenderSize
        std::optional<Resolution> renderResolution;

        ShutterSettings shutter;
    };

    // the FFmpeg captures are written with (Setup::FFmpegPlaces)
    std::filesystem::path GetFFmpegPath();

    class CaptureManager
    {
       public:
        static CaptureManager& Get()
        {
            static CaptureManager instance;
            return instance;
        }

        CaptureManager(CaptureManager const&) = delete;
        void operator=(CaptureManager const&) = delete;

        void Initialize();
        void ToggleCapture();
        void StartCapture();
        void StopCapture();
        void CaptureFrame();
        void PrepareFrame();

        std::string_view GetOutputFormatLabel(OutputFormat outputFormat);
        std::string_view GetVideoCodecLabel(VideoCodec codec);
        
        CaptureSettings& GetCaptureSettings()
        {
            return captureSettings;
        }

        std::array<Components::Camera::Mode, 3> GetRecordableCameras() 
        {
            return 
            {
                Components::Camera::Mode::FirstPerson, 
                Components::Camera::Mode::Dolly,
                Components::Camera::Mode::Bone
            };
        }

        // the output sizes: the render size and fractions of it, which supersample it; two thirds is the step between
        // (2,560 x 1,440 of 3,840 x 2,160)
        std::array<Resolution, 5> GetSupportedResolutions() const
        {
            const auto base = captureSettings.renderResolution.value_or(gameResolution);
            constexpr std::array<std::pair<std::int32_t, std::int32_t>, 5> fractions = {
                {{1, 1}, {2, 3}, {1, 2}, {1, 3}, {1, 4}}};
            std::array<Resolution, 5> resolutions;
            for (std::size_t i = 0; i < fractions.size(); i++)
            {
                const auto [numerator, denominator] = fractions[i];
                resolutions[i] = {base.width * numerator / denominator, base.height * numerator / denominator};
            }
            return resolutions;
        }

        // the sizes the game can render at for a capture, larger than its window
        std::vector<Resolution> GetRenderResolutions() const;

        Resolution GetGameResolution() const
        {
            return gameResolution;
        }

        // The restarts of the game's renderer the editor waits for, with the game view's loading screen
        // (RestartLogic): a capture's render size and the switch back after it, and
        // the Look tab's Shadow quality. Each lasts until ReShade has loaded its effects again and Pack and Surface depth
        // are back, when they're on. IsSwitchingRenderSize: a capture's, also while switching back after it
        bool IsSwitchingRenderSize() const
        {
            return switchingRenderSize || switchingBack;
        }
        bool IsRestarting() const
        {
            return IsSwitchingRenderSize() || switchingShadows;
        }
        // what the loading screen says, and what is under it for what is waited for now
        std::string GetRestartLabel() const;
        RestartLogic::Screen GetRestartScreen() const;
        // (the game view's) the loading screen was drawn this frame
        void OnRestartScreenDrawn();
        // the Look tab's Shadow quality: the sun's shadow map at size, through a restart (name: "High")
        void RestartForShadows(std::int32_t size, std::string_view name);

        // the restarts that ended, the last one last (up to 8), for the log and the harness
        struct RestartRecord
        {
            RestartLogic::Kind kind;
            std::string label;
            RestartLogic::Step end;  // Done, or a timeout
            std::uint32_t heldMs = 0;  // held back while ReShade loaded
            std::uint32_t gameMs = 0;
            std::uint32_t reshadeMs = 0;
            std::uint32_t packMs = 0;
            std::uint32_t surfaceMs = 0;
            bool waitedForReShade = false;
            bool waitedForPack = false;
            bool waitedForSurface = false;
            bool reshadeTimedOut = false;
            bool packTimedOut = false;
            bool surfaceTimedOut = false;
            std::uint32_t screenFrames = 0;
        };
        std::vector<RestartRecord> GetRestarts() const;
        // (the harness's) how long ReShade is waited for, and a restart held back for it (none: RestartLogic's)
        void SetRestartTimeouts(std::optional<std::uint64_t> reshadeMs, std::optional<std::uint64_t> holdMs);

        // what the last capture's first frame had, for the harness: Pack's textures swapped in and those still drawn
        // without theirs, and the surfaces that drew with Surface depth's shaders and what it had left
        struct FirstFrame
        {
            std::uint32_t packSwaps = 0;
            std::uint32_t packLeft = 0;
            std::uint32_t surfaceDraws = 0;
            std::uint32_t surfaceLeft = 0;
        };
        FirstFrame GetFirstFrame() const
        {
            return firstFrame;
        }

        // the frames of the last capture drawn without ReShade's effects while it was expected (ReShade loading)
        std::uint32_t GetFramesWithoutReShade() const
        {
            return framesWithoutReShade;
        }

        std::array<int32_t, 6> GetSupportedFramerates()
        {
            return { 50, 100, 125, 250, 500, 1000 };
        }

        bool IsCapturing() const
        {
            return isCapturing;
        }

        bool IsFramePrepared() const
        {
            return framePrepared;
        }

        bool IsFFmpegPresent() const
        {
            return !ffmpegNotFound;
        }

        std::int32_t GetCapturedFrameCount() const
		{
			return capturedFrameCount;
		}

        // For a game holding its film's time itself (T6's fixedtime): whether the frame it builds now is the one in a
        // multi-pass capture's set of frames that moves the film on (its first pass's first); true outside one
        bool PassTimeAdvances() const;
        // the frames of a multi-pass set (the passes, each with its settle frames)
        std::int32_t GetPassSetFrames() const;

        bool MultiPassEnabled() const
        {
            return !captureSettings.passes.empty();
        }

        const PassData& GetCurrentPass() const
        {
            return captureSettings.passes[GetCurrentPassIndex()];
        }

        int32_t OnGameFrame();

        // The shutter's plan for the capture's settings: the game frames rendered for each frame recorded, the time
        // (ms) the first of them moves on and the others, and how many recorded frames' time the shutter is open
        struct ShutterPlan
        {
            std::int32_t count = 1;
            std::int32_t firstStep = 0;
            std::int32_t step = 0;
            float exposureFrames = 1.0f;
        };
        ShutterPlan GetShutterPlan() const;
        // whether the capture running renders the shutter's frames (it's on, one pass, a game with a render thread)
        bool IsShutterCapturing() const;
        // On the render thread, for the frame drawn now while the shutter captures: which of its recorded frame's
        // frames it is (the last one is recorded, the others only blended in)
        struct ShutterRole
        {
            std::int32_t subframe = 0;
            std::int32_t count = 1;
            bool last = true;
            std::int32_t recordedFrame = 0;
            float exposureFrames = 1.0f;
        };
        std::optional<ShutterRole> GetShutterRole() const;

        // For games that draw on a render thread of their own, a frame or two behind the game: called on the game's
        // thread when the render thread waits for the frame the game built, and on the render thread once it drew
        // it (in EndScene). The frames built while capturing are numbered on the game's thread, the number follows
        // the frame to the render thread, and the pass of a frame follows from it: whether time moves on and the gun
        // is drawn as the game builds it, what the render thread skips as it draws it, and which file it goes to.
        // Reading the number of captured frames on the game's thread instead raced the render thread, so passes
        // came a frame apart for stretches of a capture.
        void OnFrameHandedOver();
        void OnFrameDrawn();
        // The timeline's tick of the frame drawn last, which the frame's ReShade values are keyed at:
        // the frame's tick as the game built it, followed to the render thread as its number is; the timeline's
        // tick for games without a render thread of their own
        std::uint32_t GetDrawnTick() const;

        // On the game's thread as the demo reads its end (and rewinds a second): a capture stops there, see
        // CaptureWatch
        void OnDemoEnd();

       private:
        CaptureManager()
        {
        }

        // StartCapture once the game renders at the capture's size
        void BeginCapture();

        void OnRenderFrame();
        // a frame of the restart waited for, and its end
        void AdvanceRestart();
        void StartRestart(RestartLogic::Kind kind);
        // asks the game for it (RestartLogic::Step::Ask); false if it can't
        bool AskForRestart(RestartLogic::Kind kind);
        void CancelRestart();
        void RecordRestart(const RestartLogic::Wait& wait, RestartLogic::Step end);

        // the pass of the frame captured next
        std::size_t GetCurrentPassIndex() const;

        // a capture that can't reach its end tick stops, saying why (CaptureWatch)
        void StopEarly(CaptureWatch::Stop stop);

        bool initialized = false;
        Resolution gameResolution = Resolution(0, 0);
        CaptureSettings captureSettings;

        // a capture that renders at another size waits for the game to switch to it, and switches back when it ends;
        // Shadow quality waits likewise (see IsRestarting)
        std::atomic_bool switchingRenderSize = false;
        std::atomic_bool switchingBack = false;
        std::atomic_bool switchingShadows = false;
        // a capture's frames begin at the start tick, and time holds there until what draws them is back (RestartLogic)
        std::atomic_bool holdingStart = false;
        Resolution switchTarget = Resolution(0, 0);
        bool renderSizeChanged = false;
        std::int32_t shadowSize = 0;
        std::string shadowName;
        std::optional<RestartLogic::Wait> restart;  // the render thread's
        std::optional<std::uint64_t> reshadeTimeout;
        std::optional<std::uint64_t> holdTimeout;
        mutable std::mutex restartsMutex;
        std::vector<RestartRecord> restarts;
        RestartLogic::Screen restartScreen;
        // ReShade's effects expected in the capture's frames (it drew them as the capture began, or was waited for),
        // and the frames captured without them
        bool expectsReShade = false;
        std::atomic<std::uint32_t> framesWithoutReShade = 0;
        FirstFrame firstFrame;

        IDirect3DSurface9* depthSurface = nullptr;
        IDirect3DPixelShader9* depthShader = nullptr;

        // internal capture state
        Resolution screenDimensions = Resolution(0, 0);
        IDirect3DSurface9* backBuffer = nullptr;
        IDirect3DSurface9* downsampledRenderTarget = nullptr;
        IDirect3DSurface9* tempSurface = nullptr;
        std::atomic_bool isCapturing = false;
        std::int32_t capturedFrameCount = 0;
        // a multi-pass capture's frames with the game's settle frames (GameInterface::GetPassSettleFrames): each
        // frame drawn since the capture began, recorded or not, and how many each pass settles
        std::atomic<std::int64_t> passSlot = 0;
        std::int32_t passSettle = 0;
        CaptureWatch::Watch watch;
        std::atomic_bool demoEnded = false;

        // see OnFrameHandedOver; -1 for frames that aren't captured
        bool reportsFrameHandOver = false;
        std::atomic<std::int32_t> captureGeneration = 0;
        std::int32_t builtFrameGeneration = 0;  // the game's thread
        std::int32_t nextBuiltFrame = 0;        // the game's thread
        std::int32_t builtFrame = -1;           // the game's thread
        std::atomic<std::int32_t> handedOverFrame = -1;
        std::int32_t drawnFrame = -1;           // the render thread
        std::atomic<std::uint32_t> handedOverTick = 0;
        std::atomic<std::uint32_t> drawnTick = 0;
        bool ffmpegNotFound = false;
        bool framePrepared = false;
        FILE* pipe = nullptr;
    };
}  // namespace IWXMVM::Components