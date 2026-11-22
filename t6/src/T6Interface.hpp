#pragma once
#include "StdInclude.hpp"
#include "GameInterface.hpp"

#include "D3D11.hpp"
#include "Addresses.hpp"
#include "Patches.hpp"
#include "Functions.hpp"
#include "Structures.hpp"
#include "Events.hpp"
#include "TheaterProbe.hpp"
#include "Theater.hpp"
#include "Cursor.hpp"
#include "Filmtweaks.hpp"
#include "Camera.hpp"
#include "Bones.hpp"
#include "Props.hpp"

// Black Ops II multiplayer (t6mp) on the Redacted LAN client.
//
// Phase 2 binding: every method core requires is here, and none of them needs an address of the game yet. BO2 renders
// with Direct3D 11 (d3d9.dll never loads), so core draws the editor through its D3D11 module, which finds the game's
// device and window from its swap chain: enough for the overlay over the main menu, with no signatures. Everything
// else returns "nothing here" values that keep core's demo paths switched off:
// a film playing in the theater is core's demo (Theater.cpp); core's own rewinding is off (CanRewind) and the
// playback addresses point at scratch memory
// so a stray rewind can't write into the game.
//
// Each TODO(T6) names what replaces the stub, in the order of the plan's phases.
namespace IWXMVM::T6
{
    class T6Interface : public GameInterface
    {
        // why the last PlayDemo sent nothing, empty when it sent demo_play
        std::string playDemoError;

       public:
        T6Interface() : GameInterface(Types::Game::T6)
        {
        }

        // ---- Setup -------------------------------------------------------------------------------------------------

        void InitializeGameAddresses() override
        {
            char exePath[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            MODULEINFO moduleInfo{};
            GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &moduleInfo, sizeof(moduleInfo));
            LOG_DEBUG("Main module: {} (base {}, size {:X})", exePath, moduleInfo.lpBaseOfDll, moduleInfo.SizeOfImage);

            // Nothing waits for the game's window or device: core hooks D3D11's Present, which works before the game
            // has made its swap chain. The code is mapped by the time Redacted calls PreInit, so the scan can run now.
            try
            {
                GetGameAddresses();
            }
            catch (std::exception& ex)
            {
                LOG_ERROR("Failed to find required signature: {}", ex.what());
                MessageBoxA(nullptr,
                            "It seems like you are running an unsupported game version.\n\nT6iris supports Call of "
                            "Duty: Black Ops II multiplayer as the Redacted LAN client runs it (t6mpv43.exe).",
                            "Unsupported game version", MB_OK | MB_ICONWARNING);
                // Mod::Initialize stops here, before anything is hooked
                throw;
            }
        }

        void InstallHooksAndPatches() override
        {
            // the patches are made here, none applied yet (SetMouseMode applies IN_Frame's)
            Patches::GetGamePatches();
            // the editor's cameras: CG_CalcViewValues' view replaced or followed
            Camera::Install();
            Theater::InstallHooks();
            Cursor::Install();
            Filmtweaks::Install();
            Props::Install();
            LOG_INFO("T6: hooks installed (input, cameras, the party's match, the cursor, the colour grade)");
        }

        void SetupEventListeners() override
        {
            // Once, at the first frame: the dvars and the console state as T6iris reads them, which the log shows
            // match the game (cg_fov 65 and timescale 1 by default, the console closed at the menu)
            Events::RegisterListener(EventType::OnFrame, [] {
                static bool logged = false;
                if (logged)
                    return;
                logged = true;

                for (const auto name : {"cg_fov", "timescale", "com_maxfps", "cl_ingame", "fs_game"})
                {
                    const auto dvar = Functions::FindDvar(name);
                    if (!dvar)
                    {
                        LOG_INFO("T6: dvar {} not found", name);
                        continue;
                    }
                    switch (dvar->type)
                    {
                        case Structures::DVAR_TYPE_BOOL:
                            LOG_INFO("T6: dvar {} = {} (bool)", dvar->name, dvar->current.enabled);
                            break;
                        case Structures::DVAR_TYPE_FLOAT:
                            LOG_INFO("T6: dvar {} = {} (float)", dvar->name, dvar->current.value);
                            break;
                        case Structures::DVAR_TYPE_INT:
                            LOG_INFO("T6: dvar {} = {} (int)", dvar->name, dvar->current.integer);
                            break;
                        case Structures::DVAR_TYPE_STRING:
                            LOG_INFO("T6: dvar {} = '{}' (string)", dvar->name,
                                     dvar->current.string ? dvar->current.string : "");
                            break;
                        default:
                            LOG_INFO("T6: dvar {} has type {}", dvar->name, dvar->type);
                    }
                }
                LOG_INFO("T6: keyCatchers = {:#x}", Functions::GetKeyCatchers());
            });

            // diagnostic: the theater's candidate fields while a film plays (see TheaterProbe.hpp), logged once a
            // second; only with the environment variable T6IRIS_PROBE set, as the fields it was for are found
            if (GetEnvironmentVariableA("T6IRIS_PROBE", nullptr, 0) != 0)
            {
                LOG_INFO("T6: T6IRIS_PROBE is set, so the theater probe logs once a second");
                Events::RegisterListener(EventType::OnFrame, TheaterProbe::OnFrame);
            }

            // the theater's film as core's demo: its start and stop, and the pause in step with the editor's
            Events::RegisterListener(EventType::OnFrame, Theater::OnFrame);

        }

        std::string_view GetClientName() override
        {
            return "Redacted T6";
        }

        Types::GraphicsApi GetGraphicsApi() override
        {
            return Types::GraphicsApi::D3D11;
        }

        Types::Features GetSupportedFeatures() override
        {
            // (actors are the Actors tab's lights alone: see Lights.hpp)
            return static_cast<Types::Features>(Types::Features_SkyRotation | Types::Features_Actors |
                                                Types::Features_FilmtweakRanges |
                                                Types::Features_FilmtweakBrightness | Types::Features_ScreenBlur |
                                                Types::Features_LensBlur | Types::Features_Shutter |
                                                Types::Features_CameraShake | Types::Features_LensFlare |
                                                Types::Features_SkyReplace);
        }

        // ---- Window, device, input ---------------------------------------------------------------------------------

        IDirect3DDevice9* GetGameDevicePtr() const override
        {
            // a D3D11 game: core takes the device from the swap chain (D3D11::GetDevice)
            return nullptr;
        }

        uintptr_t GetWndProc() override
        {
            // TODO(T6) Phase 1: the game's MainWndProc by signature. What the window calls now can be the Steam
            // overlay's or another hook's procedure, if one was installed before ours.
            const HWND window = D3D11::FindWindowHandle();
            if (!window)
                return 0;
            // the real function, not the handle GetWindowLongPtr gives for a window of the other character set
            return IsWindowUnicode(window) ? static_cast<uintptr_t>(GetWindowLongPtrW(window, GWLP_WNDPROC))
                                           : static_cast<uintptr_t>(GetWindowLongPtrA(window, GWLP_WNDPROC));
        }

        void SetMouseMode(Types::MouseMode mode) override
        {
            // As T5: while the editor has the mouse, IN_Frame returns at once. Core shows the system cursor again on
            // every window message while the editor is up, which undoes the game hiding it.
            if (mode == Types::MouseMode::Capture)
                Patches::GetGamePatches().IN_Frame.Apply();
            else
                Patches::GetGamePatches().IN_Frame.Revert();
        }

        bool IsConsoleOpen() override
        {
            return (Functions::GetKeyCatchers() & Structures::KEYCATCH_CONSOLE) != 0;
        }

        // the console, a menu or the chat field has the keyboard, as in T5
        bool IsGameUITakingKeys() override
        {
            return (Functions::GetKeyCatchers() &
                    (Structures::KEYCATCH_CONSOLE | Structures::KEYCATCH_UI | Structures::KEYCATCH_MESSAGE)) != 0;
        }

        // the theater's key handler (0x5F3E30) takes Escape for its own menu (0x653650), and the film plays on
        bool IsDemoScrubbing() override
        {
            return Theater::IsScrubbing();
        }

        bool IsDemoSeeking() override
        {
            return Theater::IsSeeking();
        }

        bool DoesEscapeOpenDemoMenu() override
        {
            return true;
        }

        void Quit() override
        {
            Functions::Cbuf_AddText("quit");
        }

        void Vid_Restart() override
        {
            // Core asks for this when it hooks D3D9 without ReShade, so the game recreates its depth buffer through
            // core's hooks. Left empty, as T5 does, until a restart is known to be safe in T6 (Phase 5, capture
            // passes, needs the depth buffer).
        }

        // ---- Game state and demos ----------------------------------------------------------------------------------

        Types::GameState GetGameState() override
        {
            if (Theater::IsFilmPlaying())
                return Types::GameState::InDemo;

            // cl_ingame is set while the client is connected to a game, as in T5
            const auto cl_ingame = Functions::FindDvar("cl_ingame");
            if (!cl_ingame || cl_ingame->type != Structures::DVAR_TYPE_BOOL || !cl_ingame->current.enabled)
                return Types::GameState::MainMenu;
            return Types::GameState::InGame;
        }

        bool IsPlayingDemo() override
        {
            return Theater::IsFilmPlaying();
        }

        // The timeline runs from the film's start (0) to its end, in ms of film time. Called every frame; the file is
        // read once per film.
        // The view the frame on the screen was drawn from: the backend's first view of the frame it draws (its
        // GfxViewParms: the projection at +0x40, its first value 1 / tan(fov x / 2); the origin at +0x100, the axes,
        // forward, left and up, from +0x110). The game draws a frame from the camera of a frame or two before, so
        // what's drawn over the view (the gizmo, the rings) goes by this, not the camera as it is now.
        std::optional<Types::AnimationPose> GetAnimationPoseByName(std::string_view animation, std::string_view model,
                                                                   float time) override
        {
            return Props::GetAnimationPose(animation, model, time);
        }

        void LoadContentPack(const std::string& name) override
        {
            Props::LoadPack(name);
        }

        std::optional<Types::DrawnView> GetDrawnView() override
        {
            if (GetGameState() != Types::GameState::InDemo)
                return std::nullopt;
            // the buffer the backend draws: not the one the frontend fills (its views stay after it's drawn)
            const auto& addresses = GetGameAddresses();
            const auto filling = *reinterpret_cast<const std::int32_t*>(addresses.smpFrame()) & 1;
            const auto drawn = 1 - filling;
            constexpr std::size_t VIEW_INFOS_SIZE = 0x3140, FRAME_DATA_SIZE = 0x48BF00, FRAME_NUMBER = 0x462D70;
            const auto viewsOf = [&](std::int32_t buffer) {
                return reinterpret_cast<const std::uint8_t*>(addresses.frameViewInfos() + buffer * VIEW_INFOS_SIZE);
            };
            const auto frameOf = [&](std::int32_t buffer) {
                return *reinterpret_cast<const std::int32_t*>(addresses.frameDatas() + buffer * FRAME_DATA_SIZE +
                                                              FRAME_NUMBER);
            };
            const auto views = viewsOf(drawn);
            const auto count = frameOf(drawn);
            const auto floats = reinterpret_cast<const float*>(views);
            const auto at = [&](std::size_t offset) {
                return glm::make_vec3(reinterpret_cast<const float*>(views + offset));
            };
            const auto inverseTan = floats[0x40 / 4];
            const auto origin = at(0x100), forward = at(0x110), up = at(0x128);
            const bool sane = std::isfinite(inverseTan) && inverseTan > 0.01f && std::isfinite(origin.x) &&
                              std::isfinite(origin.y) && std::isfinite(origin.z) &&
                              std::abs(glm::length(forward) - 1.0f) < 0.01f && std::abs(glm::length(up) - 1.0f) < 0.01f;
            if (!sane)
            {
                static std::uint64_t loggedAt = 0;
                if (const auto now = GetTickCount64(); now - loggedAt > 3000)
                {
                    loggedAt = now;
                    LOG_DEBUG("T6: the drawn view doesn't look like one (1/tan {}, origin ({}, {}, {}), forward {}, up {})",
                              inverseTan, origin.x, origin.y, origin.z, glm::length(forward), glm::length(up));
                }
                return std::nullopt;
            }
            // (another view than the camera's, as a scene in a menu: the camera's own then)
            if (const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                camera && glm::distance(camera->GetPosition(), origin) > 2000.0f)
            {
                static std::uint64_t loggedAt = 0;
                if (const auto now = GetTickCount64(); now - loggedAt > 3000)
                {
                    loggedAt = now;
                    LOG_DEBUG("T6: the drawn view (buffer {}) at ({:.0f}, {:.0f}, {:.0f}) is far from the camera", drawn,
                              origin.x, origin.y, origin.z);
                }
                return std::nullopt;
            }
            const auto inverseTanY = floats[0x54 / 4];
            Types::DrawnView view{origin, forward, up, glm::degrees(std::atan(1.0f / inverseTan) * 2.0f)};
            if (std::isfinite(inverseTanY) && inverseTanY > 0.01f)
                view.tanHalfFovY = 1.0f / inverseTanY;
            // (now and then, to compare it with the camera's)
            static std::uint64_t loggedAt = 0;
            if (const auto now = GetTickCount64(); now - loggedAt > 3000)
            {
                loggedAt = now;
                const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                const auto at = camera ? camera->GetPosition() : glm::vec3(0.0f);
                const auto other = glm::make_vec3(reinterpret_cast<const float*>(viewsOf(filling) + 0x100));
                LOG_DEBUG("T6: drawn view (buffer {}, frame {}) at ({:.1f}, {:.1f}, {:.1f}); the other (frame {}) at "
                          "({:.1f}, {:.1f}, {:.1f}); the camera at ({:.1f}, {:.1f}, {:.1f}); tan x {:.4f} y {:.4f}, the "
                          "window {:.0f}x{:.0f}",
                          drawn, count, origin.x, origin.y, origin.z, frameOf(filling), other.x, other.y, other.z, at.x,
                          at.y, at.z, 1.0f / inverseTan, view.tanHalfFovY, ImGui::GetMainViewport()->Size.x,
                          ImGui::GetMainViewport()->Size.y);
            }
            return view;
        }

        Types::DemoInfo GetDemoInfo() override
        {
            Types::DemoInfo info{};
            if (!Theater::IsFilmPlaying())
                return info;

            info.name = Theater::GetFilmName();
            info.path = Theater::GetFilmPath().string();

            // while seeking, where it's going (see Theater::GetSeekTarget)
            const auto time = Theater::GetSeekTarget().value_or(Theater::GetFilmTime());
            if (const auto times = Theater::GetFilmTimes())
            {
                info.endTick = static_cast<std::uint32_t>(times->end - times->start);
                info.gameTick = static_cast<std::uint32_t>(std::clamp(time - times->start, 0, times->end - times->start));
            }
            else
            {
                // without the file, the furthest the film has been
                static std::int32_t furthest = 0;
                furthest = std::max(furthest, time);
                info.endTick = static_cast<std::uint32_t>(std::max(furthest, 1));
                info.gameTick = static_cast<std::uint32_t>(std::max(time, 0));
            }
            return info;
        }

        // core's rewinding isn't for T6: SeekDemo hands every seek to the theater
        bool CanRewind() override
        {
            return false;
        }

        bool SeekDemo(std::uint32_t worldTick) override
        {
            if (!Theater::IsFilmPlaying())
                return false;
            // the timeline's ticks are ms from the film's start
            const auto times = Theater::GetFilmTimes();
            const std::int32_t start = times ? times->start : 0;
            Theater::SeekTo(start + static_cast<std::int32_t>(worldTick));
            return true;
        }

        std::string_view GetDemoExtension() override
        {
            // Redacted's films: Plugins\OpenNetStorage\Cloud\FILM_MP\<name>.demo
            return ".demo";
        }

        // From the Theater's lobby, as its Start Film does (Theater::PlayFilmFromLobby): demo_play by itself, without
        // the lobby's players, crashed the game ("Unhandled exception" right after it), so elsewhere nothing is sent
        // and the Demo tab says to open the Theater first.
        void PlayDemo(std::filesystem::path demoPath) override
        {
            playDemoError = Theater::PlayFilmFromLobby(demoPath);
            if (!playDemoError.empty())
                LOG_WARN("T6: not playing {}: {}", demoPath.filename().string(), playDemoError);
        }

        std::string GetPlayDemoError() override
        {
            return playDemoError;
        }

        void Disconnect() override
        {
            Functions::Cbuf_AddText("disconnect");
        }

        void ExecuteNewServerCommands() override
        {
            // (core's rewinding isn't used: CanRewind is false, the theater seeks itself, SeekDemo)
        }

        // ---- Rewinding ---------------------------------------------------------------------------------------------

        void CL_FirstSnapshot() override
        {
            // (core's rewinding isn't used: the theater seeks itself, SeekDemo)
        }

        void ResetClientData(int serverTime) override
        {
        }

        Types::PlaybackData GetPlaybackDataAddresses() const override
        {
            // Scratch memory, never the game's: core's rewinding and skipping write through these addresses, which T6
            // doesn't use (CanRewind is false; the theater seeks itself, SeekDemo)
            static std::int32_t scratch[16]{};
            const auto at = [](std::size_t i) { return reinterpret_cast<uintptr_t>(&scratch[i]); };

            Types::PlaybackData data{};
            data.cl = {at(0), at(1), at(2), at(3)};
            data.clc = {at(4), at(5), {at(6), 0}, 0};
            data.cgs = {at(7)};
            data.cls = {at(8)};
            data.s_compassActors = {at(9), 0};
            data.teamChatMsgs = {at(10), 0};
            data.cg_entities = {at(11), 0};
            data.clientInfo = {at(12), 0};
            data.gameState = {at(13), 0};
            data.killfeed = 0;
            return data;
        }

        // ---- Visuals -----------------------------------------------------------------------------------------------

        std::optional<Types::Dvar> GetDvar(const std::string_view name) override
        {
            const auto gameDvar = Functions::FindDvar(name);
            if (!gameDvar)
                return std::nullopt;

            Types::Dvar dvar;
            dvar.name = gameDvar->name;
            dvar.value = reinterpret_cast<Types::Dvar::Value*>(&gameDvar->current);
            return dvar;
        }

        void SetFov(float fov) override
        {
            // as T5 does; only written when it changes, since core sets it every frame in first person
            const auto cg_fov = Functions::FindDvar("cg_fov");
            if (cg_fov && cg_fov->type == Structures::DVAR_TYPE_FLOAT && cg_fov->current.value != fov)
            {
                cg_fov->current.value = fov;
                cg_fov->modified = true;
            }
        }

        // The sun as its tweak dvars have it (r_lightTweakSunColor, Direction and Light), and the sky's rotation
        // (r_skyRotation). The renderer takes them up when they're marked modified (0x712460, 0x74E910); each map's
        // own are their reset values.
        Types::Sun ReadSun(bool reset)
        {
            Types::Sun sun{};
            sun.color = Functions::GetDvarVec3("r_lightTweakSunColor", reset).value_or(glm::vec3(1.0f));
            sun.direction = Functions::GetDvarVec3("r_lightTweakSunDirection", reset).value_or(glm::vec3(0.0f));
            const auto number = [&](std::string_view name, float otherwise) {
                const auto value = reset ? Functions::GetDvarDefault(name) : Functions::GetDvarNumber(name);
                return value.value_or(otherwise);
            };
            sun.brightness = number("r_lightTweakSunLight", 1.0f);
            sun.skyRotation = number("r_skyRotation", 0.0f);
            return sun;
        }

        Types::Sun GetSun() override
        {
            return ReadSun(false);
        }

        std::optional<Types::MapVisuals> GetMapVisuals() override
        {
            return Types::MapVisuals{ReadSun(true), ReadDof(true), GetFilmtweaks()};
        }

        // Depth of field from the r_dof_ dvars: with r_dof_tweak the renderer takes the frame's DoF from them
        // (0x729D90: view model start and end, near start and end, far start and end, near and far blur) whatever
        // r_dof_enable says; without it from the refdef (the game's own, aiming down sights) when r_dof_enable is on.
        // r_dof_bias is read by the renderer itself (0x76C1A2). The defaults are the dvars' reset values.
        Types::DoF ReadDof(bool reset)
        {
            const auto number = [&](std::string_view name, float otherwise) {
                const auto value = reset ? Functions::GetDvarDefault(name) : Functions::GetDvarNumber(name);
                return value.value_or(otherwise);
            };
            Types::DoF dof{};
            dof.enabled = number("r_dof_tweak", 0.0f) != 0.0f;
            dof.farBlur = number("r_dof_farBlur", 1.8f);
            dof.farStart = number("r_dof_farStart", 1000.0f);
            dof.farEnd = number("r_dof_farEnd", 7000.0f);
            dof.nearBlur = number("r_dof_nearBlur", 6.0f);
            dof.nearStart = number("r_dof_nearStart", 0.0f);
            dof.nearEnd = number("r_dof_nearEnd", 60.0f);
            dof.bias = number("r_dof_bias", 0.5f);
            return dof;
        }

        Types::DoF GetDof() override
        {
            return ReadDof(false);
        }

        // Filmtweaks on the map's own colour grade (Filmtweaks.cpp); the defaults change nothing
        Types::Filmtweaks GetFilmtweaks() override
        {
            Types::Filmtweaks filmtweaks{};
            filmtweaks.enabled = false;
            filmtweaks.brightness = 0.0f;
            filmtweaks.contrast = 1.0f;
            filmtweaks.desaturation = 0.0f;
            filmtweaks.tintLight = glm::vec3(1.0f);
            filmtweaks.tintDark = glm::vec3(1.0f);
            filmtweaks.invert = false;
            return filmtweaks;
        }

        // The HUD: 2D Elements is the theater's own HUD (the killfeed, score and the rest, F2 in the theater), hidden by
        // default while the editor is up or records (Theater::SetHudShown); ui_hud_hardcore and the names over players
        // for the player's HUD, cg_drawCrosshair, cg_drawShellshock and ui_hud_obituaries (the killfeed). The score,
        // icons and blood have no switch of their own here, so they stay shown.
        Types::HudInfo GetHudInfo() override
        {
            const auto shown = [](std::string_view name, bool inverted = false) {
                const auto value = Functions::GetDvarNumber(name);
                return !value ? true : ((*value != 0.0f) != inverted);
            };
            Types::HudInfo hud{};
            hud.show2DElements = false;
            hud.showPlayerHUD = shown("ui_hud_hardcore", true);
            hud.showShellshock = shown("cg_drawShellshock");
            hud.showCrosshair = shown("cg_drawCrosshair");
            hud.showScore = true;
            hud.showIconsAndText = true;
            hud.showBloodOverlay = true;
            hud.showKillfeed = shown("ui_hud_obituaries");
            hud.killfeedTeam1Color = glm::vec3(0.6f, 0.8f, 1.0f);
            hud.killfeedTeam2Color = glm::vec3(1.0f, 0.5f, 0.4f);
            return hud;
        }

        // (the 2D elements' colours left out)
        bool HasKillfeedColors() override
        {
            return false;
        }

        bool AppliesHudInModCameras() override
        {
            return true;
        }

        void SetSun(Types::Sun sun) override
        {
            Functions::SetDvarVec3("r_lightTweakSunColor", sun.color);
            Functions::SetDvarVec3("r_lightTweakSunDirection", sun.direction);
            Functions::SetDvarNumber("r_lightTweakSunLight", sun.brightness);
            Functions::SetDvarNumber("r_skyRotation", sun.skyRotation);
        }

        // Where the game keeps the shader resource view of a render target image (at its +0), checked by the image's
        // name: $floatz (2), its linear depth, which it fills only for its own effects that need it, and $scene_depth
        // (11), the depth buffer as a texture (0x75D880 makes its view)
        static ID3D11ShaderResourceView* const* FindImageView(std::uintptr_t index, const char* expected)
        {
            constexpr std::uintptr_t IMAGE_SIZE = 0x50;
            constexpr std::uintptr_t IMAGE_NAME = 0x48;
            const auto image = GetGameAddresses().renderTargetImages() + index * IMAGE_SIZE;
            const auto name = *reinterpret_cast<const char* const*>(image + IMAGE_NAME);
            const bool found = name && !IsBadReadPtr(name, 16) && std::strcmp(name, expected) == 0;
            static std::set<std::uintptr_t> logged;
            if (logged.insert(index).second)
                LOG_DEBUG("T6: the image at {:#x}: {}", image, found ? expected : "not the one looked for");
            return found ? reinterpret_cast<ID3D11ShaderResourceView* const*>(image) : nullptr;
        }

        // r_dof_tweak on with the editor's values, off for the game's own DoF (r_dof_enable, the player's setting, is
        // left as it is). The Lens and the Blur section's directional and radial blurs are the mod's own
        // (D3D11::SetScreenBlur, SetPictureBlur), the Blur section's on whether the depth of field is or not.
        // (BO2's renderer draws a frame a little after the game built it: each pass drawn twice before it's recorded,
        // so what it hides is gone from the frame recorded)
        // (handed to the console's buffer a few lines at a time, under its size; its comments and blank lines left
        // out)
        bool RunConfig(std::string_view text) override
        {
            std::string chunk;
            std::size_t lines = 0;
            for (std::size_t from = 0; from <= text.size();)
            {
                auto end = text.find('\n', from);
                if (end == std::string_view::npos)
                    end = text.size();
                auto line = text.substr(from, end - from);
                from = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.remove_suffix(1);
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                    line.remove_prefix(1);
                if (line.starts_with("\xEF\xBB\xBF"))
                    line.remove_prefix(3);
                if (line.empty() || line.starts_with("//") || line.starts_with('#'))
                    continue;
                if (chunk.size() + line.size() + 1 > 4000)
                {
                    Functions::Cbuf_AddText(chunk);
                    chunk.clear();
                }
                if (!chunk.empty())
                    chunk += '\n';
                chunk += line;
                ++lines;
            }
            if (!chunk.empty())
                Functions::Cbuf_AddText(chunk);
            LOG_INFO("Ran a .cfg of {} lines", lines);
            return true;
        }

        std::optional<std::string> GetDvarText(std::string_view name) override
        {
            const auto dvar = Functions::FindDvar(name);
            if (!dvar)
                return std::nullopt;
            const auto& v = dvar->current;
            switch (dvar->type)
            {
                case Structures::DVAR_TYPE_BOOL:
                    return v.enabled ? "1" : "0";
                case Structures::DVAR_TYPE_FLOAT:
                    return std::format("{}", v.value);
                case Structures::DVAR_TYPE_FLOAT_2:
                    return std::format("{} {}", v.vector[0], v.vector[1]);
                case Structures::DVAR_TYPE_FLOAT_3:
                case Structures::DVAR_TYPE_LINEAR_COLOR_RGB:
                    return std::format("{} {} {}", v.vector[0], v.vector[1], v.vector[2]);
                case Structures::DVAR_TYPE_FLOAT_4:
                    return std::format("{} {} {} {}", v.vector[0], v.vector[1], v.vector[2], v.vector[3]);
                case Structures::DVAR_TYPE_INT:
                case Structures::DVAR_TYPE_ENUM:
                    return std::to_string(v.integer);
                case Structures::DVAR_TYPE_STRING:
                    return v.string && !IsBadReadPtr(v.string, 1) ? std::string(v.string) : std::string();
                case Structures::DVAR_TYPE_COLOR:
                    return std::format("{} {} {} {}", v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f,
                                       v.color[3] / 255.0f);
                default:
                    return std::nullopt;
            }
        }

        std::int32_t GetPassSettleFrames() override
        {
            return 2;
        }

        void SetDof(Types::DoF dof) override
        {
            // the Blur section's
            const auto screenBlur = dof.GetScreenBlur();
            D3D11::ScreenBlur picture{};
            if (screenBlur == Types::DoF::BLUR_DIRECTIONAL || screenBlur == Types::DoF::BLUR_RADIAL)
            {
                picture.type = screenBlur == Types::DoF::BLUR_DIRECTIONAL ? D3D11::ScreenBlur::DIRECTIONAL
                                                                          : D3D11::ScreenBlur::RADIAL;
                picture.strength = dof.blurStrength;
                picture.angle = dof.blurAngle;
                picture.centerX = dof.blurCenterX;
                picture.centerY = dof.blurCenterY;
                picture.clearRadius = dof.blurClearRadius;
            }
            D3D11::SetPictureBlur(picture);
            // (an older project's directional or radial blur type was instead of the game's depth of field)
            if (dof.HasOlderBlurType())
                dof.enabled = false;

            const auto type = dof.GetDepthBlur();
            D3D11::ScreenBlur blur{};
            if (dof.enabled && type == Types::DoF::BLUR_LENS)
            {
                blur.type = D3D11::ScreenBlur::LENS;
                blur.focusDistance = dof.focusDistance;
                blur.focalLength = dof.focalLength;
                blur.aperture = dof.aperture;
                blur.blades = dof.bokehBlades;
                blur.highlights = dof.bokehHighlights;
                blur.amount = dof.blurAmount;
                blur.maxSize = dof.bokehSize;
                blur.pointX = dof.focusPointX;
                blur.pointY = dof.focusPointY;
                blur.followPoint = std::round(dof.focusMode) == static_cast<float>(Types::DoF::FOCUS_FOLLOW_POINT);
                blur.smoothing = dof.focusSmoothing;
                blur.trackFocus = std::round(dof.focusMode) == static_cast<float>(Types::DoF::FOCUS_TRACK);
                blur.showFocus = dof.showFocus;
                blur.depth = FindImageView(2, "$floatz");
                blur.hardwareDepth = FindImageView(11, "$scene_depth");
                blur.zNear = Functions::GetDvarNumber("r_znear").value_or(4.0f);
                dof.enabled = false;  // (the game's depth of field off)
            }
            D3D11::SetScreenBlur(blur);

            Functions::SetDvarNumber("r_dof_farBlur", dof.farBlur);
            Functions::SetDvarNumber("r_dof_farStart", dof.farStart);
            Functions::SetDvarNumber("r_dof_farEnd", dof.farEnd);
            Functions::SetDvarNumber("r_dof_nearBlur", dof.nearBlur);
            Functions::SetDvarNumber("r_dof_nearStart", dof.nearStart);
            Functions::SetDvarNumber("r_dof_nearEnd", dof.nearEnd);
            Functions::SetDvarNumber("r_dof_bias", dof.bias);
            Functions::SetDvarNumber("r_dof_tweak", dof.enabled ? 1.0f : 0.0f);

            // (a diagnostic: the settings each time they change, at most once a second)
            static std::string last;
            static std::uint64_t lastAt = 0;
            const auto now = std::format(
                "game's {}, far blur {:.2f} from {:.0f} to {:.0f}, near blur {:.2f} from {:.0f} to {:.0f}, bias {:.2f}; "
                "screen blur {} strength {:.3f} angle {:.0f} centre ({:.2f} {:.2f}) clear {:.2f}; lens focus {:.0f} "
                "{:.0f} mm f/{:.1f} blades {:.0f} highlights {:.2f} amount {:.2f} size {:.3f} pin ({:.2f} {:.2f}){}",
                dof.enabled, dof.farBlur, dof.farStart, dof.farEnd, dof.nearBlur, dof.nearStart, dof.nearEnd, dof.bias,
                static_cast<int>(picture.type), picture.strength, picture.angle, picture.centerX, picture.centerY,
                picture.clearRadius,
                blur.focusDistance, blur.focalLength, blur.aperture, blur.blades, blur.highlights, blur.amount,
                blur.maxSize, blur.pointX, blur.pointY, blur.followPoint ? " followed" : "");
            if (now != last && GetTickCount64() - lastAt >= 1000)
            {
                last = now;
                lastAt = GetTickCount64();
                LOG_DEBUG("T6: depth of field {}", now);
            }
        }

        void SetFilmtweaks(Types::Filmtweaks filmtweaks) override
        {
            // (a diagnostic: the settings each time they change, at most once a second)
            {
                static std::string last;
                static std::uint64_t lastAt = 0;
                const auto Ranges = [](glm::vec3 v) { return std::format("{:.2f}/{:.2f}/{:.2f}", v.x, v.y, v.z); };
                const auto now = std::format(
                    "enabled {}, brightness {:.2f}, contrast {:.2f}, desaturation {:.2f}, tint dark ({:.2f} {:.2f} "
                    "{:.2f}), tint light ({:.2f} {:.2f} {:.2f}), invert {}; shadows/mids/highlights: contrast {}, "
                    "saturation {}, hue {}, temperature {}, bleach {}, tint mid ({:.2f} {:.2f} {:.2f})",
                    filmtweaks.enabled, filmtweaks.brightness, filmtweaks.contrast, filmtweaks.desaturation,
                    filmtweaks.tintDark.r, filmtweaks.tintDark.g, filmtweaks.tintDark.b, filmtweaks.tintLight.r,
                    filmtweaks.tintLight.g, filmtweaks.tintLight.b, filmtweaks.invert,
                    Ranges(filmtweaks.contrastRanges), Ranges(filmtweaks.saturationRanges),
                    Ranges(filmtweaks.hueRanges), Ranges(filmtweaks.colorTemperatureRanges),
                    Ranges(filmtweaks.bleachRanges), filmtweaks.tintMid.r, filmtweaks.tintMid.g,
                    filmtweaks.tintMid.b);
                if (now != last && GetTickCount64() - lastAt >= 1000)
                {
                    last = now;
                    lastAt = GetTickCount64();
                    LOG_DEBUG("T6: filmtweaks {}", now);
                }
            }
            // (the dvars' grade is off: it made the picture black)
            Functions::SetDvarNumber("r_filmUseTweaks", 0.0f);
            Filmtweaks::Set(filmtweaks);
        }

        void SetHudInfo(Types::HudInfo hud) override
        {
            Theater::SetHudShown(hud.show2DElements);
            Functions::SetDvarNumber("ui_hud_hardcore", hud.showPlayerHUD ? 0.0f : 1.0f);
            // the names, ranks and icons over players, as T5 does: their defaults back, or 0
            for (const auto name : {"cg_overheadNamesSize", "cg_overheadRankSize", "cg_overheadIconSize"})
            {
                if (const auto standard = Functions::GetDvarDefault(name))
                    Functions::SetDvarNumber(name, hud.showPlayerHUD ? *standard : 0.0f);
            }
            Functions::SetDvarNumber("cg_drawShellshock", hud.showShellshock ? 1.0f : 0.0f);
            Functions::SetDvarNumber("cg_drawCrosshair", hud.showCrosshair ? 1.0f : 0.0f);
            Functions::SetDvarNumber("ui_hud_obituaries", hud.showKillfeed ? 1.0f : 0.0f);
        }

        // ---- Entities and bones (bone camera) ----------------------------------------------------------------------

        std::vector<Types::Entity> GetEntities() override
        {
            return Bones::GetEntities();
        }

        Types::BoneData GetBoneData(int32_t entityId, const std::string& name) override
        {
            return Bones::GetBoneData(entityId, name);
        }

        std::optional<std::string> GetPlayerName(std::int32_t clientNum) override
        {
            return Bones::GetPlayerName(clientNum);
        }

        constexpr std::vector<std::string> GetSupportedBoneNames() override
        {
            // T5's, with the eye (BO2's player models have them all)
            return {
                "tag_eye",       "j_head",          "tag_weapon",       "tag_weapon_left", "tag_weapon_right",
                "tag_flash",     "tag_clip",        "tag_brass",        "j_mainroot",      "j_wrist_le",
                "j_wrist_ri",    "j_shoulder_le",   "j_shoulder_ri",    "j_ankle_le",      "j_ankle_ri",
                "tag_origin",
            };
        }
    };
}  // namespace IWXMVM::T6
